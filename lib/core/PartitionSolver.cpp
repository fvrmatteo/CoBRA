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

    } // namespace

    std::optional< SemilinearIR > SolvePartitionsLinearly(const SemilinearIR &ir) {
        COBRA_TRACE("PartitionSolver", "SolvePartitionsLinearly: terms={}", ir.terms.size());
        if (ir.terms.empty() || ir.bitwidth == 0 || ir.bitwidth > 64) { return std::nullopt; }

        const uint64_t kMod = Bitmask(ir.bitwidth);

        // Each live term with its reading, and the variables they read in
        // ascending global order. Each class is solved over exactly these,
        // so a bit of the signature index stands for one of them.
        std::vector< AlignedTerm > terms;
        std::vector< GlobalVarIdx > vars;
        for (const auto &term : ir.terms) {
            const auto &info = ir.atom_table[term.atom_id];
            if (info.key.truth_table.empty() || info.key.support.size() > kMaxAtomSupport) {
                COBRA_TRACE("PartitionSolver", "atom {} has no truth table", term.atom_id);
                return std::nullopt;
            }
            auto shift = UniformShift(*info.original_subtree);
            if (!shift.has_value() || *shift >= ir.bitwidth) {
                COBRA_TRACE("PartitionSolver", "atom {} has no bit-local reading", term.atom_id);
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
            terms.push_back(
                { .coeff = (term.coeff >> *shift) & kMod, .shift = *shift, .atom = &info }
            );
            vars.insert(vars.end(), info.key.support.begin(), info.key.support.end());
        }
        std::sort(vars.begin(), vars.end());
        vars.erase(std::unique(vars.begin(), vars.end()), vars.end());
        if (vars.empty() || vars.size() > kMaxSolveVars) { return std::nullopt; }

        const auto num_vars = static_cast< uint32_t >(vars.size());
        const size_t kLen   = size_t{ 1 } << num_vars;

        // Global variable index -> position in `vars`, for restricting an
        // assignment over all the variables to an atom's own support.
        std::vector< uint32_t > local_of(static_cast< size_t >(vars.back()) + 1, 0);
        for (uint32_t i = 0; i < num_vars; ++i) { local_of[vars[i]] = i; }

        // The bit classes: positions on which every term reads the same
        // Boolean function of the variables. A term's reading at result bit
        // `j` is its atom's output bit `j - shift`, and nothing below the
        // shift, which is where a realigned shift differs from the
        // partitioner's own classes.
        std::map< std::vector< uint64_t >, uint64_t > class_of_profile;
        std::vector< uint64_t > profile(terms.size());
        for (uint32_t j = 0; j < ir.bitwidth; ++j) {
            for (size_t t = 0; t < terms.size(); ++t) {
                const auto &at = terms[t];
                profile[t]     = j >= at.shift ? EvalAtomAtBit(
                                     *at.atom->original_subtree, at.atom->key.support,
                                     j - at.shift, ir.bitwidth
                                 )
                                               : 0;
            }
            class_of_profile[profile] |= (1ULL << j);
        }

        std::unique_ptr< Expr > assembled;
        uint64_t constant = ir.constant;

        for (const auto &[profiles, mask] : class_of_profile) {
            // The bit-slice function of the sum on this class: what one bit
            // of the result contributes, as a function of that bit of every
            // variable, before it is weighted by its position.
            std::vector< uint64_t > slice(kLen, 0);
            for (size_t t = 0; t < terms.size(); ++t) {
                const uint64_t kTable = profiles[t];
                if (kTable == 0) { continue; }
                const auto &support = terms[t].atom->key.support;
                for (size_t a = 0; a < kLen; ++a) {
                    size_t restricted = 0;
                    for (size_t i = 0; i < support.size(); ++i) {
                        restricted |= ((a >> local_of[support[i]]) & 1) << i;
                    }
                    if (((kTable >> restricted) & 1) != 0) {
                        slice[a] = (slice[a] + terms[t].coeff) & kMod;
                    }
                }
            }
            if (std::ranges::all_of(slice, [](uint64_t v) { return v == 0; })) { continue; }

            // A linear MBA L with bit-slice function s has L(a) = s(a) - 2*s(0)
            // at the Boolean points a: bit 0 contributes s(a) and every other
            // bit s(0), and sum(2^j, j >= 1) is -2 in the ring. That is the
            // signature the interpolation expects.
            std::vector< uint64_t > sig(kLen);
            for (size_t a = 0; a < kLen; ++a) { sig[a] = (slice[a] - (2 * slice[0])) & kMod; }
            auto coeffs = InterpolateCoefficients(sig, num_vars, ir.bitwidth);
            auto linear = BuildCobExpr(coeffs, num_vars, ir.bitwidth);

            ClassSolution solution;
            auto lin = NormalizeToSemilinear(*linear, {}, ir.bitwidth);
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
            COBRA_TRACE(
                "PartitionSolver", "class mask={:#x}: {} -> {} term(s)", mask, terms.size(),
                solution.terms.size()
            );

            // On the bits of this class the sum equals the linear form. Its
            // constant c is the all-ones word times -c bit by bit, so it lands
            // on these bits as -c * mask; each atom keeps its coefficient and
            // is masked to the class.
            constant = (constant - (solution.constant * mask)) & kMod;
            for (auto &[coeff, atom] : solution.terms) {
                RemapVarIndices(*atom, vars);
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

} // namespace cobra
