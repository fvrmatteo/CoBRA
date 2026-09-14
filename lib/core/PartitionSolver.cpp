#include "cobra/core/PartitionSolver.h"
#include "cobra/core/BitPartitioner.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/CoBExprBuilder.h"
#include "cobra/core/CoeffInterpolator.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/SemilinearIR.h"
#include "cobra/core/SemilinearNormalizer.h"
#include "cobra/core/Trace.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace cobra {

    namespace {

        // A class is solved from its 2^n signature, so this bounds the work
        // per class; the semilinear entry already declines anything over the
        // caller's variable cap, which is far below it in practice.
        constexpr size_t kMaxSolveVars = 8;

        // Truth tables are packed into one word, which holds them up to five
        // variables; the partitioner has the same limit.
        constexpr size_t kMaxAtomSupport = 5;

        // A translation matches a class against every way of reading each
        // variable - kept, complemented, cleared or set - which is 4^n maps
        // over 2^n points. Four variables keep that to 256 maps of 16 points.
        constexpr size_t kMaxTranslationVars = 4;

        // How many classes are tried as the slice the others are read through.
        // Any class reading every variable will do when the sum is a
        // translation at all; trying a few lets the cheapest constants win.
        constexpr size_t kMaxTranslationReferences = 4;

        bool HasConstantNode(const Expr &expr) {
            if (expr.kind == Expr::Kind::kConstant) { return true; }
            return std::ranges::any_of(expr.children, [](const auto &child) {
                return HasConstantNode(*child);
            });
        }

        // How far every variable in `atom` is shifted down on its way to the
        // atom's output, when that is the same distance along every path:
        // output bit `i` then reads the variables at bit `i + shift`, and the
        // atom has a bit-local reading again once its term is realigned.
        // Nothing when two paths disagree, or when the atom reads no variable.
        std::optional< uint32_t > UniformShift(const Expr &atom) {
            switch (atom.kind) {
                case Expr::Kind::kVariable:
                    return 0;
                case Expr::Kind::kConstant:
                    return std::nullopt;
                case Expr::Kind::kShr: {
                    auto inner = UniformShift(*atom.children[0]);
                    if (!inner.has_value() || atom.constant_val >= 64) { return std::nullopt; }
                    return *inner + static_cast< uint32_t >(atom.constant_val);
                }
                case Expr::Kind::kNot:
                    return UniformShift(*atom.children[0]);
                case Expr::Kind::kAnd:
                case Expr::Kind::kOr:
                case Expr::Kind::kXor: {
                    std::optional< uint32_t > agreed;
                    for (const auto &child : atom.children) {
                        if (!HasVarDep(*child)) { continue; }
                        auto shift = UniformShift(*child);
                        if (!shift.has_value()) { return std::nullopt; }
                        if (agreed.has_value() && *agreed != *shift) { return std::nullopt; }
                        agreed = shift;
                    }
                    return agreed;
                }
                case Expr::Kind::kAdd:
                case Expr::Kind::kMul:
                case Expr::Kind::kNeg:
                case Expr::Kind::kCmpEq:
                case Expr::Kind::kCmpUlt:
                case Expr::Kind::kCmpSlt:
                    return std::nullopt;
            }
            return std::nullopt;
        }

        // One term of the sum with its bit-local reading: the coefficient it
        // carries once the atom's shift has been taken out of it, and the
        // distance its output bits were moved down by.
        struct AlignedTerm
        {
            uint64_t coeff;
            uint32_t shift;
            const AtomInfo *atom;
        };

        // Every term of a sum with its reading, and the variables they read in
        // ascending global order. A class is solved over exactly these, so a
        // bit of a signature index stands for one of them.
        struct AlignedSum
        {
            std::vector< AlignedTerm > terms;
            std::vector< GlobalVarIdx > vars;

            // Global variable index -> position in `vars`, for restricting an
            // assignment over all the variables to an atom's own support.
            std::vector< uint32_t > local_of;
        };

        // Nothing when some atom has no bit-local reading, or when the sum
        // reads no variable or more than `max_vars` of them.
        std::optional< AlignedSum > AlignTerms(const SemilinearIR &ir, size_t max_vars) {
            const uint64_t kMod = Bitmask(ir.bitwidth);
            AlignedSum sum;
            for (const auto &term : ir.terms) {
                const auto &info = ir.atom_table[term.atom_id];
                if (info.key.truth_table.empty() || info.key.support.size() > kMaxAtomSupport) {
                    COBRA_TRACE("PartitionSolver", "atom {} has no truth table", term.atom_id);
                    return std::nullopt;
                }
                auto shift = UniformShift(*info.original_subtree);
                if (!shift.has_value() || *shift >= ir.bitwidth) {
                    COBRA_TRACE(
                        "PartitionSolver", "atom {} has no bit-local reading", term.atom_id
                    );
                    return std::nullopt;
                }
                // A term `c * (B >> s)` is `(c / 2^s) * (B with its low s bits
                // cleared)` when 2^s divides c, and has no bit-local reading
                // otherwise: the coefficient would carry the source bits to
                // positions other than their own.
                if (*shift != 0 && std::countr_zero(term.coeff) < static_cast< int >(*shift)) {
                    COBRA_TRACE(
                        "PartitionSolver", "atom {} shifted by {} under coefficient {:#x}",
                        term.atom_id, *shift, term.coeff
                    );
                    return std::nullopt;
                }
                sum.terms.push_back(
                    { .coeff = (term.coeff >> *shift) & kMod, .shift = *shift, .atom = &info }
                );
                sum.vars.insert(sum.vars.end(), info.key.support.begin(), info.key.support.end());
            }
            std::sort(sum.vars.begin(), sum.vars.end());
            sum.vars.erase(std::unique(sum.vars.begin(), sum.vars.end()), sum.vars.end());
            if (sum.vars.empty() || sum.vars.size() > max_vars) { return std::nullopt; }

            sum.local_of.assign(static_cast< size_t >(sum.vars.back()) + 1, 0);
            for (uint32_t i = 0; i < sum.vars.size(); ++i) { sum.local_of[sum.vars[i]] = i; }
            return sum;
        }

        // What every term reads at result bit `j`: its atom's output bit
        // `j - shift`, and nothing below the shift, which is where a realigned
        // shift differs from the partitioner's own classes.
        std::vector< uint64_t >
        ProfileAt(const AlignedSum &sum, uint32_t j, uint32_t bitwidth) {
            std::vector< uint64_t > profile(sum.terms.size());
            for (size_t t = 0; t < sum.terms.size(); ++t) {
                const auto &at = sum.terms[t];
                profile[t]     = j >= at.shift ? EvalAtomAtBit(
                                     *at.atom->original_subtree, at.atom->key.support,
                                     j - at.shift, bitwidth
                                 )
                                               : 0;
            }
            return profile;
        }

        // The bit-slice function of the sum on a class with these profiles:
        // what one bit of the result contributes, as a function of that bit of
        // every variable, before it is weighted by its position.
        std::vector< uint64_t > SliceOf(
            const AlignedSum &sum, const std::vector< uint64_t > &profiles, uint64_t modulus
        ) {
            const size_t kLen = size_t{ 1 } << sum.vars.size();
            std::vector< uint64_t > slice(kLen, 0);
            for (size_t t = 0; t < sum.terms.size(); ++t) {
                const uint64_t kTable = profiles[t];
                if (kTable == 0) { continue; }
                const auto &support = sum.terms[t].atom->key.support;
                for (size_t a = 0; a < kLen; ++a) {
                    size_t restricted = 0;
                    for (size_t i = 0; i < support.size(); ++i) {
                        restricted |= ((a >> sum.local_of[support[i]]) & 1) << i;
                    }
                    if (((kTable >> restricted) & 1) != 0) {
                        slice[a] = (slice[a] + sum.terms[t].coeff) & modulus;
                    }
                }
            }
            return slice;
        }

        // Every atom of `lin` is a Boolean function of the variables alone.
        // That is what makes its truth table its behaviour at every bit, which
        // the masking below relies on. The builder only ever emits such atoms;
        // the check guards the assumption rather than a known case.
        bool AllAtomsPure(const SemilinearIR &lin) {
            return std::ranges::all_of(lin.terms, [&](const WeightedAtom &term) {
                const auto &subtree = *lin.atom_table[term.atom_id].original_subtree;
                return !HasConstantNode(subtree) && !ContainsType(subtree, Expr::Kind::kShr);
            });
        }

        struct ClassSolution
        {
            uint64_t constant = 0; // c: the linear form is c + sum(d_k * H_k)
            std::vector< std::pair< uint64_t, std::unique_ptr< Expr > > > terms; // (d_k, H_k)
        };

        // Fall back to the AND basis the interpolation hands out directly.
        ClassSolution FromAndBasis(const std::vector< uint64_t > &coeffs) {
            ClassSolution solution;
            solution.constant = coeffs[0];
            for (size_t i = 1; i < coeffs.size(); ++i) {
                if (coeffs[i] == 0) { continue; }
                solution.terms.emplace_back(coeffs[i], BuildAndProduct(i));
            }
            return solution;
        }

        // The cheapest linear form whose bit-slice function is `slice`, over
        // local variable indices.
        //
        // A linear MBA L with bit-slice function s has L(a) = s(a) - 2*s(0) at
        // the Boolean points a: bit 0 contributes s(a) and every other bit
        // s(0), and sum(2^j, j >= 1) is -2 in the ring. That is the signature
        // the interpolation expects, and it pins s down: two slices with the
        // same signature differ by a d with d(a) = 2*d(0) everywhere, so d = 0.
        ClassSolution
        SolveSlice(const std::vector< uint64_t > &slice, uint32_t num_vars, uint32_t bitwidth) {
            const uint64_t kMod = Bitmask(bitwidth);
            std::vector< uint64_t > sig(slice.size());
            for (size_t a = 0; a < slice.size(); ++a) {
                sig[a] = (slice[a] - (2 * slice[0])) & kMod;
            }
            auto coeffs = InterpolateCoefficients(sig, num_vars, bitwidth);
            auto linear = BuildCobExpr(coeffs, num_vars, bitwidth);

            ClassSolution solution;
            auto lin = NormalizeToSemilinear(*linear, {}, bitwidth);
            if (lin.has_value() && AllAtomsPure(*lin)) {
                solution.constant = lin->constant;
                for (const auto &term : lin->terms) {
                    solution.terms.emplace_back(
                        term.coeff, CloneExpr(*lin->atom_table[term.atom_id].original_subtree)
                    );
                }
            } else {
                solution = FromAndBasis(coeffs);
            }
            return solution;
        }

        // How a class reads one variable, relative to the reference slice.
        enum : uint32_t {
            kMapKeep       = 0,
            kMapComplement = 1,
            kMapClear      = 2,
            kMapSet        = 3,
        };

        uint32_t MapOf(uint32_t tuple, uint32_t var) { return (tuple >> (2 * var)) & 3; }

        // Every assignment of a map to each of `n` variables, packed two bits
        // per variable, fewest departures from keeping the variable first: a
        // variable kept on every class needs no constant at all.
        const std::vector< uint32_t > &MapTuples(uint32_t n) {
            static const auto kAll = [] {
                std::array< std::vector< uint32_t >, kMaxTranslationVars + 1 > all;
                for (uint32_t vars = 0; vars <= kMaxTranslationVars; ++vars) {
                    auto &tuples = all[vars];
                    for (uint32_t t = 0; t < (uint32_t{ 1 } << (2 * vars)); ++t) {
                        tuples.push_back(t);
                    }
                    const auto departures = [vars](uint32_t t) {
                        uint32_t count = 0;
                        for (uint32_t i = 0; i < vars; ++i) {
                            count += MapOf(t, i) != kMapKeep ? 1 : 0;
                        }
                        return count;
                    };
                    std::stable_sort(tuples.begin(), tuples.end(), [&](uint32_t l, uint32_t r) {
                        return departures(l) < departures(r);
                    });
                }
                return all;
            }();
            return kAll[n];
        }

        size_t MapAssignment(uint32_t tuple, size_t a, uint32_t n) {
            size_t out = 0;
            for (uint32_t i = 0; i < n; ++i) {
                const size_t kBit = (a >> i) & 1;
                size_t mapped     = kBit;
                switch (MapOf(tuple, i)) {
                    case kMapComplement:
                        mapped = kBit ^ 1;
                        break;
                    case kMapClear:
                        mapped = 0;
                        break;
                    case kMapSet:
                        mapped = 1;
                        break;
                    default:
                        break;
                }
                out |= mapped << i;
            }
            return out;
        }

        // The variables a slice actually reads.
        uint32_t SliceSupport(const std::vector< uint64_t > &slice, uint32_t n) {
            uint32_t support = 0;
            for (uint32_t i = 0; i < n; ++i) {
                for (size_t a = 0; a < slice.size(); ++a) {
                    if (slice[a] != slice[a ^ (size_t{ 1 } << i)]) {
                        support |= uint32_t{ 1 } << i;
                        break;
                    }
                }
            }
            return support;
        }

        // The first map under which `slice` is `reference` up to an additive
        // constant, or nothing. A variable the slice reads cannot have been
        // cleared or set.
        std::optional< uint32_t > FindMap(
            const std::vector< uint64_t > &slice, uint32_t support,
            const std::vector< uint64_t > &reference, uint32_t n, uint64_t modulus
        ) {
            for (const uint32_t kTuple : MapTuples(n)) {
                bool fixes_a_read_variable = false;
                for (uint32_t i = 0; i < n && !fixes_a_read_variable; ++i) {
                    fixes_a_read_variable =
                        ((support >> i) & 1) != 0 && MapOf(kTuple, i) >= kMapClear;
                }
                if (fixes_a_read_variable) { continue; }
                const uint64_t kBase = reference[MapAssignment(kTuple, 0, n)];
                bool matches         = true;
                for (size_t a = 1; a < slice.size() && matches; ++a) {
                    matches = ((slice[a] - slice[0]) & modulus)
                        == ((reference[MapAssignment(kTuple, a, n)] - kBase) & modulus);
                }
                if (matches) { return kTuple; }
            }
            return std::nullopt;
        }

        // A variable read through its word constants: `(v & keep) ^ flip`,
        // without the parts that do nothing.
        std::unique_ptr< Expr >
        TranslatedVariable(GlobalVarIdx var, uint64_t keep, uint64_t flip, uint64_t modulus) {
            if (keep == 0) { return Expr::Constant(flip); }
            auto value = Expr::Variable(var);
            if (keep != modulus) {
                value = Expr::BitwiseAnd(std::move(value), Expr::Constant(keep));
            } else if (flip == modulus) {
                return Expr::BitwiseNot(std::move(value));
            }
            if (flip != 0) { value = Expr::BitwiseXor(Expr::Constant(flip), std::move(value)); }
            return value;
        }

        std::unique_ptr< Expr > SubstituteVariables(
            const Expr &expr, const std::vector< std::unique_ptr< Expr > > &replacement
        ) {
            if (expr.kind == Expr::Kind::kVariable) {
                return CloneExpr(*replacement[expr.var_index]);
            }
            auto copy          = std::make_unique< Expr >();
            copy->kind         = expr.kind;
            copy->constant_val = expr.constant_val;
            copy->var_index    = expr.var_index;
            for (const auto &child : expr.children) {
                copy->children.push_back(SubstituteVariables(*child, replacement));
            }
            return copy;
        }

    } // namespace

    std::optional< SemilinearIR > SolvePartitionsLinearly(const SemilinearIR &ir) {
        COBRA_TRACE("PartitionSolver", "SolvePartitionsLinearly: terms={}", ir.terms.size());
        if (ir.terms.empty() || ir.bitwidth == 0 || ir.bitwidth > 64) { return std::nullopt; }

        const uint64_t kMod = Bitmask(ir.bitwidth);

        auto sum = AlignTerms(ir, kMaxSolveVars);
        if (!sum.has_value()) { return std::nullopt; }
        const auto num_vars = static_cast< uint32_t >(sum->vars.size());

        // The bit classes: positions on which every term reads the same
        // Boolean function of the variables.
        std::map< std::vector< uint64_t >, uint64_t > class_of_profile;
        for (uint32_t j = 0; j < ir.bitwidth; ++j) {
            class_of_profile[ProfileAt(*sum, j, ir.bitwidth)] |= (1ULL << j);
        }

        std::unique_ptr< Expr > assembled;
        uint64_t constant = ir.constant;

        for (const auto &[profiles, mask] : class_of_profile) {
            const auto slice = SliceOf(*sum, profiles, kMod);
            if (std::ranges::all_of(slice, [](uint64_t v) { return v == 0; })) { continue; }

            auto solution = SolveSlice(slice, num_vars, ir.bitwidth);
            COBRA_TRACE(
                "PartitionSolver", "class mask={:#x}: {} -> {} term(s)", mask, sum->terms.size(),
                solution.terms.size()
            );

            // On the bits of this class the sum equals the linear form. Its
            // constant c is the all-ones word times -c bit by bit, so it lands
            // on these bits as -c * mask; each atom keeps its coefficient and
            // is masked to the class.
            constant = (constant - (solution.constant * mask)) & kMod;
            for (auto &[coeff, atom] : solution.terms) {
                RemapVarIndices(*atom, sum->vars);
                auto masked = mask == kMod
                    ? std::move(atom)
                    : Expr::BitwiseAnd(std::move(atom), Expr::Constant(mask));
                auto applied = ApplyCoefficient(std::move(masked), coeff, ir.bitwidth);
                assembled    = assembled ? Expr::Add(std::move(assembled), std::move(applied))
                                         : std::move(applied);
            }
        }

        if (!assembled) {
            assembled = Expr::Constant(constant);
        } else if (constant != 0) {
            assembled = Expr::Add(Expr::Constant(constant), std::move(assembled));
        }

        // Normalizing the assembly rather than registering its atoms by hand
        // runs the constant lowerings (`(~a) & c`, `a ^ c`, `a | c`) over the
        // masked answers, which is what puts a class solved as `~y` and one
        // solved as `y` on the same basis for XOR recovery to join.
        auto out = NormalizeToSemilinear(*assembled, {}, ir.bitwidth);
        if (!out.has_value()) { return std::nullopt; }
        COBRA_TRACE(
            "PartitionSolver", "SolvePartitionsLinearly: {} class(es), {} -> {} term(s)",
            class_of_profile.size(), ir.terms.size(), out->terms.size()
        );
        return std::move(out.value());
    }

    std::optional< SemilinearIR > SolvePartitionsByTranslation(const SemilinearIR &ir) {
        COBRA_TRACE("PartitionSolver", "SolvePartitionsByTranslation: terms={}", ir.terms.size());
        if (ir.terms.empty() || ir.bitwidth == 0 || ir.bitwidth > 64) { return std::nullopt; }

        const uint64_t kMod = Bitmask(ir.bitwidth);

        auto sum = AlignTerms(ir, kMaxTranslationVars);
        if (!sum.has_value()) { return std::nullopt; }
        const auto num_vars = static_cast< uint32_t >(sum->vars.size());

        std::map< std::vector< uint64_t >, uint64_t > class_of_profile;
        for (uint32_t j = 0; j < ir.bitwidth; ++j) {
            class_of_profile[ProfileAt(*sum, j, ir.bitwidth)] |= (1ULL << j);
        }
        // One class is one linear MBA, which the plain solve already finds.
        if (class_of_profile.size() < 2) { return std::nullopt; }

        struct SliceClass
        {
            uint64_t mask;
            std::vector< uint64_t > slice;
            uint32_t support;
        };

        std::vector< SliceClass > classes;
        uint32_t union_support = 0;
        for (const auto &[profiles, mask] : class_of_profile) {
            auto slice              = SliceOf(*sum, profiles, kMod);
            const uint32_t kSupport = SliceSupport(slice, num_vars);
            union_support |= kSupport;
            classes.push_back({ .mask = mask, .slice = std::move(slice), .support = kSupport });
        }

        // Each variable as `(v & keep) ^ flip`, for the reference slice chosen,
        // and the constant that slice carries.
        //
        // The slices above leave the sum's constant out, and they have to: it
        // is one word, in which the constants lowered out of the atoms have
        // already carried into each other, so its bits are not what any class
        // contributes. Classes are matched up to an additive constant
        // instead. With the reference slice `t_r` carrying `c_r`, the class
        // matched through `sigma` carries `c_r + delta` where
        // `delta = t_r(sigma(0)) - t(0)`, and the bits of every class together
        // must add up to the sum's constant:
        // `sum(mask * (c_r + delta)) = C`. The masks cover the word, which is
        // `-1` in the ring, so `c_r = sum(mask * delta) - C`.
        struct Translation
        {
            size_t reference = 0;
            std::vector< uint64_t > keep;
            std::vector< uint64_t > flip;
            uint64_t reference_constant = 0;
            uint32_t constants          = 0;
        };

        std::optional< Translation > best;
        size_t references = 0;
        for (size_t r = 0; r < classes.size() && references < kMaxTranslationReferences; ++r) {
            // A reference that ignores a variable cannot be mapped onto a
            // class that reads it.
            if (classes[r].support != union_support) { continue; }
            ++references;

            Translation candidate{ .reference = r,
                                   .keep      = std::vector< uint64_t >(num_vars, 0),
                                   .flip      = std::vector< uint64_t >(num_vars, 0) };
            bool translates      = true;
            uint64_t offset_bits = 0;
            for (const auto &cls : classes) {
                auto tuple = FindMap(cls.slice, cls.support, classes[r].slice, num_vars, kMod);
                if (!tuple.has_value()) {
                    translates = false;
                    break;
                }
                const uint64_t kDelta =
                    (classes[r].slice[MapAssignment(*tuple, 0, num_vars)] - cls.slice[0]) & kMod;
                offset_bits = (offset_bits + (cls.mask * kDelta)) & kMod;
                for (uint32_t i = 0; i < num_vars; ++i) {
                    const uint32_t kMap = MapOf(*tuple, i);
                    if (kMap == kMapKeep || kMap == kMapComplement) { candidate.keep[i] |= cls.mask; }
                    if (kMap == kMapComplement || kMap == kMapSet) { candidate.flip[i] |= cls.mask; }
                }
            }
            if (!translates) { continue; }
            candidate.reference_constant = (offset_bits - ir.constant) & kMod;

            for (uint32_t i = 0; i < num_vars; ++i) {
                candidate.constants += (candidate.keep[i] != kMod ? 1 : 0)
                    + (candidate.flip[i] != 0 && candidate.flip[i] != kMod ? 1 : 0);
            }
            if (!best.has_value() || candidate.constants < best->constants) {
                best = std::move(candidate);
            }
        }
        if (!best.has_value()) {
            COBRA_TRACE("PartitionSolver", "SolvePartitionsByTranslation: no common slice");
            return std::nullopt;
        }

        // Every bit j of the sum is 2^j * g(sigma_j(v_j)) for the reference
        // slice g with its constant, so the sum is the linear MBA with slice g
        // applied to the translated variables.
        auto reference = classes[best->reference].slice;
        for (auto &value : reference) { value = (value + best->reference_constant) & kMod; }
        auto solution = SolveSlice(reference, num_vars, ir.bitwidth);

        std::vector< std::unique_ptr< Expr > > replacement;
        for (uint32_t i = 0; i < num_vars; ++i) {
            replacement.push_back(
                TranslatedVariable(sum->vars[i], best->keep[i], best->flip[i], kMod)
            );
        }

        SemilinearIR out;
        out.bitwidth = ir.bitwidth;
        out.constant = solution.constant & kMod;
        for (const auto &[coeff, atom] : solution.terms) {
            if ((coeff & kMod) == 0) { continue; }
            auto translated = SubstituteVariables(*atom, replacement);
            if (!HasVarDep(*translated)) {
                out.constant =
                    (out.constant + (coeff * EvalConstantExpr(*translated, ir.bitwidth))) & kMod;
                continue;
            }
            const AtomId kId = CreateAtom(out, std::move(translated), OperatorFamily::kMixed);
            out.terms.push_back({ .coeff = coeff & kMod, .atom_id = kId });
        }
        COBRA_TRACE(
            "PartitionSolver", "SolvePartitionsByTranslation: {} class(es), {} -> {} term(s)",
            classes.size(), ir.terms.size(), out.terms.size()
        );
        return out;
    }

} // namespace cobra
