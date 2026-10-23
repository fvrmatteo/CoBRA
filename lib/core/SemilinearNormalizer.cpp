#include "cobra/core/SemilinearNormalizer.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/Result.h"
#include "cobra/core/SemilinearIR.h"
#include "cobra/core/Trace.h"
#include <algorithm>
#include <bit>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cobra {

    namespace {

        bool IsPurelyBitwise(const Expr &expr) {
            switch (expr.kind) {
                case Expr::Kind::kConstant:
                case Expr::Kind::kVariable:
                    return true;
                case Expr::Kind::kAnd:
                case Expr::Kind::kOr:
                case Expr::Kind::kXor:
                    return IsPurelyBitwise(*expr.children[0])
                        && IsPurelyBitwise(*expr.children[1]);
                case Expr::Kind::kNot:
                    return IsPurelyBitwise(*expr.children[0]);
                case Expr::Kind::kShr:
                    return IsPurelyBitwise(*expr.children[0]);
                case Expr::Kind::kAdd:
                case Expr::Kind::kMul:
                case Expr::Kind::kNeg:
                // A comparison looks at its operands as whole words, so it has
                // no per-bit reading and cannot take part in a bitwise atom.
                case Expr::Kind::kCmpEq:
                case Expr::Kind::kCmpUlt:
                case Expr::Kind::kCmpSlt:
                    return false;
            }
            return false;
        }

        bool HasVariable(const Expr &expr) {
            if (expr.kind == Expr::Kind::kVariable) { return true; }
            return std::ranges::any_of(expr.children, [](const auto &child) {
                return HasVariable(*child);
            });
        }

        bool HasConstant(const Expr &expr) {
            if (expr.kind == Expr::Kind::kConstant) { return true; }
            return std::ranges::any_of(expr.children, [](const auto &child) {
                return HasConstant(*child);
            });
        }

        // A bitwise value moved up by a power of two, with constant bits
        // below it: `(value << shift) | low`, where `value` is purely bitwise
        // and `low < 2^shift`.
        struct ShiftedUp
        {
            std::unique_ptr< Expr > value;
            uint32_t shift = 0;
            uint64_t low   = 0;
        };

        // Reads `expr` as a bitwise value shifted up, when it is one.
        //
        // A bitwise operation with a constant over a shifted value works on
        // the shifted bits and on the bits below the shift apart, and below
        // the shift there are only constants: `(v << k) ^ c` is
        // `(v ^ (c >> k)) << k` with `c`'s low `k` bits beneath it, and
        // likewise for `&`, `|` and `~`, and for two values shifted by the
        // same distance. A compiler moves a mask or a flip outward past a
        // doubling - `2 * (x ^ c)` is printed `(x << 1) ^ (c << 1)` - and the
        // rewritten shape is a bitwise operation over a product the sum
        // cannot take apart; read this way it is the semilinear term
        // `2^k * (v ^ c')` again. Exact in the ring: bits shifted past the
        // width are gone on both sides.
        std::optional< ShiftedUp > ReadShiftedUp(const Expr &expr, uint32_t bitwidth, uint64_t mask) {
            if (IsPurelyBitwise(expr) && HasVariable(expr)) {
                return ShiftedUp{ .value = CloneExpr(expr) };
            }
            switch (expr.kind) {
                case Expr::Kind::kMul: {
                    for (size_t side = 0; side < 2; ++side) {
                        const auto &factor = *expr.children[side];
                        if (HasVariable(factor)) { continue; }
                        const uint64_t kFactor = EvalConstantExpr(factor, bitwidth);
                        if (kFactor == 0 || !std::has_single_bit(kFactor)) { return std::nullopt; }
                        auto inner = ReadShiftedUp(*expr.children[1 - side], bitwidth, mask);
                        if (!inner.has_value()) { return std::nullopt; }
                        const auto kBy = static_cast< uint32_t >(std::countr_zero(kFactor));
                        if (inner->shift + kBy >= bitwidth) { return std::nullopt; }
                        inner->shift += kBy;
                        inner->low    = (inner->low << kBy) & mask;
                        return inner;
                    }
                    return std::nullopt;
                }
                case Expr::Kind::kNot: {
                    auto inner = ReadShiftedUp(*expr.children[0], bitwidth, mask);
                    if (!inner.has_value()) { return std::nullopt; }
                    inner->value = Expr::BitwiseNot(std::move(inner->value));
                    inner->low   = ~inner->low & ((uint64_t{ 1 } << inner->shift) - 1);
                    return inner;
                }
                case Expr::Kind::kAnd:
                case Expr::Kind::kOr:
                case Expr::Kind::kXor: {
                    const auto combine = [&](std::unique_ptr< Expr > lhs, std::unique_ptr< Expr > rhs) {
                        switch (expr.kind) {
                            case Expr::Kind::kAnd:
                                return Expr::BitwiseAnd(std::move(lhs), std::move(rhs));
                            case Expr::Kind::kOr:
                                return Expr::BitwiseOr(std::move(lhs), std::move(rhs));
                            default:
                                return Expr::BitwiseXor(std::move(lhs), std::move(rhs));
                        }
                    };
                    const auto combine_low = [&](uint64_t lhs, uint64_t rhs) {
                        switch (expr.kind) {
                            case Expr::Kind::kAnd:
                                return lhs & rhs;
                            case Expr::Kind::kOr:
                                return lhs | rhs;
                            default:
                                return lhs ^ rhs;
                        }
                    };
                    for (size_t side = 0; side < 2; ++side) {
                        const auto &constant = *expr.children[side];
                        if (HasVariable(constant)) { continue; }
                        auto inner = ReadShiftedUp(*expr.children[1 - side], bitwidth, mask);
                        if (!inner.has_value()) { return std::nullopt; }
                        const uint64_t kC    = EvalConstantExpr(constant, bitwidth);
                        const uint64_t kBelow = (uint64_t{ 1 } << inner->shift) - 1;
                        inner->value = combine(std::move(inner->value), Expr::Constant(kC >> inner->shift));
                        inner->low   = combine_low(inner->low, kC & kBelow);
                        return inner;
                    }
                    auto lhs = ReadShiftedUp(*expr.children[0], bitwidth, mask);
                    auto rhs = ReadShiftedUp(*expr.children[1], bitwidth, mask);
                    if (!lhs.has_value() || !rhs.has_value() || lhs->shift != rhs->shift) {
                        return std::nullopt;
                    }
                    lhs->value = combine(std::move(lhs->value), std::move(rhs->value));
                    lhs->low   = combine_low(lhs->low, rhs->low);
                    return lhs;
                }
                default:
                    return std::nullopt;
            }
        }

        OperatorFamily DetectProvenance(const Expr &expr) {
            switch (expr.kind) {
                case Expr::Kind::kConstant:
                case Expr::Kind::kVariable:
                    return OperatorFamily::kMixed;
                case Expr::Kind::kNot:
                    return OperatorFamily::kNot;
                case Expr::Kind::kAnd: {
                    auto lp = DetectProvenance(*expr.children[0]);
                    auto rp = DetectProvenance(*expr.children[1]);
                    if ((lp == OperatorFamily::kAnd || lp == OperatorFamily::kMixed)
                        && (rp == OperatorFamily::kAnd || rp == OperatorFamily::kMixed))
                    {
                        return OperatorFamily::kAnd;
                    }
                    return OperatorFamily::kMixed;
                }
                case Expr::Kind::kOr: {
                    auto lp = DetectProvenance(*expr.children[0]);
                    auto rp = DetectProvenance(*expr.children[1]);
                    if ((lp == OperatorFamily::kOr || lp == OperatorFamily::kMixed)
                        && (rp == OperatorFamily::kOr || rp == OperatorFamily::kMixed))
                    {
                        return OperatorFamily::kOr;
                    }
                    return OperatorFamily::kMixed;
                }
                case Expr::Kind::kXor: {
                    auto lp = DetectProvenance(*expr.children[0]);
                    auto rp = DetectProvenance(*expr.children[1]);
                    if ((lp == OperatorFamily::kXor || lp == OperatorFamily::kMixed)
                        && (rp == OperatorFamily::kXor || rp == OperatorFamily::kMixed))
                    {
                        return OperatorFamily::kXor;
                    }
                    return OperatorFamily::kMixed;
                }
                case Expr::Kind::kAdd:
                case Expr::Kind::kMul:
                case Expr::Kind::kNeg:
                case Expr::Kind::kShr:
                case Expr::Kind::kCmpEq:
                case Expr::Kind::kCmpUlt:
                case Expr::Kind::kCmpSlt:
                    return OperatorFamily::kMixed;
            }
            return OperatorFamily::kMixed;
        }

        struct CollectCtx
        {
            uint32_t bitwidth{};
            uint64_t mask{};
            std::unordered_map< AtomKey, AtomId > atom_map;
            std::vector< AtomInfo > atom_table;
            std::unordered_map< uint64_t, AtomId > hash_cache;
            bool encountered_nonlinear = false;
        };

        struct CollectResult
        {
            uint64_t constant;
            std::vector< WeightedAtom > terms;
        };

        AtomId RegisterAtom(CollectCtx &ctx, const Expr &expr) {
            std::vector< GlobalVarIdx > support;
            CollectVariables(expr, support);
            std::sort(support.begin(), support.end());
            support.erase(std::unique(support.begin(), support.end()), support.end());

            const uint64_t kStructHash = StructuralHash(expr);

            // Fast path: if structural hash matches an existing atom with the same
            // support, structurally identical subtrees are semantically identical —
            // skip truth-table computation entirely.
            auto hash_it = ctx.hash_cache.find(kStructHash);
            if (hash_it != ctx.hash_cache.end()) {
                const auto &existing = ctx.atom_table[hash_it->second];
                if (existing.key.support == support) { return hash_it->second; }
            }

            auto tt = ComputeAtomTruthTable(expr, support, ctx.bitwidth);
            const AtomKey kKey{ .support = std::move(support), .truth_table = std::move(tt) };

            // Truth-table dedup is only safe for pure-variable atoms.
            // kSemilinear atoms (containing constants like x & 0xFF) can have
            // identical Boolean truth tables but differ on full-width inputs
            // because variables evaluate to {0,1} while constants keep their
            // full value.
            const bool kPure = !HasConstant(expr) && !ContainsType(expr, Expr::Kind::kShr);
            if (kPure && !kKey.truth_table.empty()) {
                auto it = ctx.atom_map.find(kKey);
                if (it != ctx.atom_map.end()) { return it->second; }
            }

            auto atom_id = static_cast< AtomId >(ctx.atom_table.size());
            if (kPure && !kKey.truth_table.empty()) { ctx.atom_map.emplace(kKey, atom_id); }
            ctx.hash_cache.emplace(kStructHash, atom_id);

            AtomInfo info;
            info.atom_id          = atom_id;
            info.key              = kKey;
            info.structural_hash  = kStructHash;
            info.original_subtree = CloneExpr(expr);
            info.provenance       = DetectProvenance(expr);
            ctx.atom_table.push_back(std::move(info));
            return atom_id;
        }

        CollectResult CollectTerms(CollectCtx &ctx, const Expr &expr, uint64_t coeff) {
            coeff &= ctx.mask;
            if (coeff == 0) { return { .constant = 0, .terms = {} }; }

            // Lower XOR/OR with a constant operand to AND-basis:
            //   a ^ c  =  a + c - 2*(a & c)
            //   a | c  =  a + c - (a & c)
            if ((expr.kind == Expr::Kind::kXor || expr.kind == Expr::Kind::kOr)
                && IsPurelyBitwise(expr) && HasVariable(expr))
            {
                const bool kLhsConst = !HasVariable(*expr.children[0]);
                const bool kRhsConst = !HasVariable(*expr.children[1]);
                if (kLhsConst || kRhsConst) {
                    const auto &const_child = kLhsConst ? *expr.children[0] : *expr.children[1];
                    const auto &var_child   = kLhsConst ? *expr.children[1] : *expr.children[0];

                    const uint64_t kC = EvalConstantExpr(const_child, ctx.bitwidth);

                    // +a with current coefficient
                    auto var_result = CollectTerms(ctx, var_child, coeff);

                    // +c * coeff
                    var_result.constant = (var_result.constant + (coeff * kC)) & ctx.mask;

                    // -(n * coeff) * (a & c), where n=2 for XOR, n=1 for OR
                    // Process through CollectTerms (not RegisterAtom) so that
                    // further lowerings (e.g. (~a)&c) fire on the first pass,
                    // keeping the normalize→reconstruct→re-normalize round-trip
                    // idempotent.
                    auto and_expr = Expr::BitwiseAnd(CloneExpr(var_child), Expr::Constant(kC));
                    uint64_t and_coeff = (expr.kind == Expr::Kind::kXor)
                        ? ModNeg((2 * coeff) & ctx.mask, ctx.bitwidth)
                        : ModNeg(coeff, ctx.bitwidth);
                    auto and_result    = CollectTerms(ctx, *and_expr, and_coeff);
                    var_result.constant =
                        (var_result.constant + and_result.constant) & ctx.mask;
                    var_result.terms.insert(
                        var_result.terms.end(), and_result.terms.begin(), and_result.terms.end()
                    );

                    return var_result;
                }
            }

            // Lower (~a) & c  =  c - (a & c)  when a has variables
            if (expr.kind == Expr::Kind::kAnd && IsPurelyBitwise(expr) && HasVariable(expr)) {
                for (size_t side = 0; side < 2; ++side) {
                    const auto &lhs = *expr.children[side];
                    const auto &rhs = *expr.children[1 - side];
                    if (lhs.kind != Expr::Kind::kNot) { continue; }
                    if (!HasVariable(lhs) || HasVariable(rhs)) { continue; }
                    // lhs = ~a, rhs = c (constant)
                    const auto &inner  = *lhs.children[0];
                    const uint64_t kC  = EvalConstantExpr(rhs, ctx.bitwidth);
                    auto and_expr      = Expr::BitwiseAnd(CloneExpr(inner), Expr::Constant(kC));
                    AtomId and_aid     = RegisterAtom(ctx, *and_expr);
                    uint64_t and_coeff = ModNeg(coeff, ctx.bitwidth);
                    uint64_t const_part = (coeff * kC) & ctx.mask;
                    return { .constant = const_part,
                             .terms    = { { .coeff = and_coeff, .atom_id = and_aid } } };
                }
            }

            // Purely bitwise with variables: treat as atom
            if (IsPurelyBitwise(expr) && HasVariable(expr)) {
                AtomId aid = RegisterAtom(ctx, expr);
                return { .constant = 0, .terms = { { .coeff = coeff, .atom_id = aid } } };
            }

            // No variables: fold to constant
            if (!HasVariable(expr)) {
                const uint64_t kVal = EvalConstantExpr(expr, ctx.bitwidth);
                return { .constant = (coeff * kVal) & ctx.mask, .terms = {} };
            }

            // Lower (a * 2^k) & c  =  (a & (c >> k)) * 2^k.
            //
            // A mask over a value shifted up keeps only bits the shift brought
            // there, so it is the shift of the value masked by the same bits
            // moved down: a semilinear term with coefficient 2^k, where the
            // shape as written is a shift of a non-bitwise operand and was
            // turned down. It is the shape a compiler writes a masked shift
            // in - the mask is moved outward past the shift - so a term as
            // plain as `8 * (t & 0xFFFFFFFF)` arrives this way round. Modular:
            // bits the shift pushes past the width are gone on both sides.
            if (expr.kind == Expr::Kind::kAnd) {
                for (size_t side = 0; side < 2; ++side) {
                    const auto &product = *expr.children[side];
                    const auto &mask    = *expr.children[1 - side];
                    if (product.kind != Expr::Kind::kMul || HasVariable(mask)) { continue; }
                    const Expr *scaled = nullptr;
                    uint64_t factor    = 0;
                    for (size_t which = 0; which < 2 && scaled == nullptr; ++which) {
                        const auto &candidate_factor = *product.children[which];
                        const auto &candidate_value  = *product.children[1 - which];
                        if (HasVariable(candidate_factor) || !HasVariable(candidate_value)) { continue; }
                        const uint64_t f = EvalConstantExpr(candidate_factor, ctx.bitwidth);
                        if (f != 0 && (f & (f - 1)) == 0) {
                            scaled = &candidate_value;
                            factor = f;
                        }
                    }
                    if (scaled == nullptr) { continue; }
                    unsigned shift = 0;
                    while ((factor >> shift) != 1) { ++shift; }
                    const uint64_t kC = EvalConstantExpr(mask, ctx.bitwidth);
                    auto masked = Expr::BitwiseAnd(CloneExpr(*scaled), Expr::Constant((kC >> shift) & ctx.mask));
                    return CollectTerms(ctx, *masked, (coeff * factor) & ctx.mask);
                }
            }

            switch (expr.kind) {
                case Expr::Kind::kAdd: {
                    auto left         = CollectTerms(ctx, *expr.children[0], coeff);
                    auto right        = CollectTerms(ctx, *expr.children[1], coeff);
                    const uint64_t kC = (left.constant + right.constant) & ctx.mask;
                    auto &terms       = left.terms;
                    terms.insert(terms.end(), right.terms.begin(), right.terms.end());
                    return { .constant = kC, .terms = std::move(terms) };
                }

                case Expr::Kind::kNeg: {
                    const uint64_t kNegCoeff = ModNeg(coeff, ctx.bitwidth);
                    return CollectTerms(ctx, *expr.children[0], kNegCoeff);
                }

                case Expr::Kind::kMul: {
                    const bool kLhsConst = !HasVariable(*expr.children[0]);
                    const bool kRhsConst = !HasVariable(*expr.children[1]);

                    if (kLhsConst) {
                        const uint64_t kC =
                            EvalConstantExpr(*expr.children[0], ctx.bitwidth);
                        const uint64_t kNewCoeff = (coeff * kC) & ctx.mask;
                        return CollectTerms(ctx, *expr.children[1], kNewCoeff);
                    }
                    if (kRhsConst) {
                        const uint64_t kC =
                            EvalConstantExpr(*expr.children[1], ctx.bitwidth);
                        const uint64_t kNewCoeff = (coeff * kC) & ctx.mask;
                        return CollectTerms(ctx, *expr.children[0], kNewCoeff);
                    }
                    // Both sides have variables — nonlinear
                    ctx.encountered_nonlinear = true;
                    return { .constant = 0, .terms = {} };
                }

                case Expr::Kind::kShr: {
                    if (IsPurelyBitwise(expr) && HasVariable(expr)) {
                        AtomId aid = RegisterAtom(ctx, expr);
                        return { .constant = 0,
                                 .terms    = { { .coeff = coeff, .atom_id = aid } } };
                    }
                    if (!HasVariable(expr)) {
                        const uint64_t kVal = EvalConstantExpr(expr, ctx.bitwidth);
                        return { .constant = (coeff * kVal) & ctx.mask, .terms = {} };
                    }
                    ctx.encountered_nonlinear = true;
                    return { .constant = 0, .terms = {} };
                }

                case Expr::Kind::kConstant:
                case Expr::Kind::kVariable:
                    break;

                case Expr::Kind::kAnd:
                case Expr::Kind::kOr:
                case Expr::Kind::kXor:
                case Expr::Kind::kNot:
                    // A bitwise operation over a value shifted up is the
                    // shifted bitwise value plus the constant bits below it.
                    if (auto shifted = ReadShiftedUp(expr, ctx.bitwidth, ctx.mask);
                        shifted.has_value() && shifted->shift != 0)
                    {
                        auto result = CollectTerms(
                            ctx, *shifted->value, (coeff << shifted->shift) & ctx.mask
                        );
                        result.constant = (result.constant + (coeff * shifted->low)) & ctx.mask;
                        return result;
                    }
                    // Otherwise reached when the node has variables but is
                    // NOT purely bitwise (e.g. (x+y) & 0x457). Reject
                    // explicitly instead of silently dropping the subtree.
                    ctx.encountered_nonlinear = true;
                    return { .constant = 0, .terms = {} };
                // A comparison over variables is neither a linear term nor a
                // bitwise atom, so it gets the same explicit rejection.
                case Expr::Kind::kCmpEq:
                case Expr::Kind::kCmpUlt:
                case Expr::Kind::kCmpSlt:
                    ctx.encountered_nonlinear = true;
                    return { .constant = 0, .terms = {} };
            }

            return { .constant = 0, .terms = {} };
        }

    } // namespace

    Result< SemilinearIR > NormalizeToSemilinear(
        const Expr &expr, [[maybe_unused]] const std::vector< std::string > &vars,
        uint32_t bitwidth
    ) {
        COBRA_TRACE(
            "Semilinear", "NormalizeToSemilinear: vars={} bitwidth={}", vars.size(), bitwidth
        );
        // bitwidth=0 makes Bitmask(0) return 0, which would zero every
        // coefficient and drop every term — the pipeline silently emits
        // Constant(0) for any input. bitwidth>64 has no meaningful
        // interpretation in this 64-bit modular ring.
        if (bitwidth == 0 || bitwidth > 64) {
            return Err< SemilinearIR >(
                CobraError::kInvalidArgument,
                "bitwidth must be in [1, 64]; got " + std::to_string(bitwidth)
            );
        }
        CollectCtx ctx; // NOLINT(misc-const-correctness)
        ctx.bitwidth = bitwidth;
        ctx.mask     = Bitmask(bitwidth);

        auto result = CollectTerms(ctx, expr, 1);

        if (ctx.encountered_nonlinear) {
            return Err< SemilinearIR >(
                CobraError::kNonLinearInput,
                "expression is not semilinear (variable*variable "
                "multiplication or shift of non-bitwise operand)"
            );
        }

        // Group terms by AtomId, summing coefficients mod 2^w
        std::unordered_map< AtomId, uint64_t > coeff_map;
        for (const auto &t : result.terms) {
            coeff_map[t.atom_id] = (coeff_map[t.atom_id] + t.coeff) & ctx.mask;
        }

        SemilinearIR ir; // NOLINT(misc-const-correctness)
        ir.constant   = result.constant & ctx.mask;
        ir.bitwidth   = bitwidth;
        ir.atom_table = std::move(ctx.atom_table);

        COBRA_TRACE(
            "Semilinear", "NormalizeToSemilinear: terms={} atoms={}", result.terms.size(),
            ir.atom_table.size()
        );

        for (const auto &[atom_id, coeff] : coeff_map) {
            if (coeff != 0) { ir.terms.push_back({ .coeff = coeff, .atom_id = atom_id }); }
        }

        // Sort terms by atom_id for deterministic output
        std::sort(
            ir.terms.begin(), ir.terms.end(),
            [](const WeightedAtom &a, const WeightedAtom &b) { return a.atom_id < b.atom_id; }
        );

        return Ok(std::move(ir));
    }

} // namespace cobra
