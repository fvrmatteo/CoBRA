#include "cobra/core/SemilinearSignature.h"
#include "cobra/core/ExprTraversal.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/CompiledExpr.h"
#include "cobra/core/Expr.h"
#include "cobra/core/Trace.h"
#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cobra {

    namespace {

        // Bottom-up evaluation with variables taking {0, 2^bit_pos} values:
        // the work one node does once its children's rows are in hand, shared
        // by both walks below.
        std::vector< uint64_t > CombineSemilinear(
            const Expr &expr, size_t len, uint32_t bitwidth, uint32_t bit_pos,
            std::vector< uint64_t > left, std::vector< uint64_t > right
        ) {
            const uint64_t kMask   = Bitmask(bitwidth);
            const uint64_t kBitVal = (bit_pos < 64) ? (1ULL << bit_pos) : 0;

            switch (expr.kind) {
                case Expr::Kind::kConstant:
                    return std::vector< uint64_t >(len, expr.constant_val & kMask);

                case Expr::Kind::kVariable: {
                    std::vector< uint64_t > r(len);
                    const uint32_t kIdx = expr.var_index;
                    for (size_t i = 0; i < len; ++i) {
                        r[i] = ((i >> kIdx) & 1) != 0 ? kBitVal : 0;
                    }
                    return r;
                }

                case Expr::Kind::kNot: {
                    auto &child = left;
                    for (auto &v : child) { v = (~v) & kMask; }
                    return std::move(child);
                }
                case Expr::Kind::kNeg: {
                    auto &child = left;
                    for (auto &v : child) { v = (-v) & kMask; }
                    return std::move(child);
                }
                case Expr::Kind::kShr: {
                    auto &child = left;
                    const uint64_t kK = expr.constant_val;
                    if (kK >= 64) {
                        std::fill(child.begin(), child.end(), 0);
                    } else {
                        for (auto &v : child) { v = (v >> kK) & kMask; }
                    }
                    return std::move(child);
                }

                case Expr::Kind::kAdd: {
                    for (size_t i = 0; i < len; ++i) { left[i] = (left[i] + right[i]) & kMask; }
                    return std::move(left);
                }
                case Expr::Kind::kMul: {
                    for (size_t i = 0; i < len; ++i) { left[i] = (left[i] * right[i]) & kMask; }
                    return std::move(left);
                }

                case Expr::Kind::kAnd: {
                    for (size_t i = 0; i < len; ++i) { left[i] = left[i] & right[i]; }
                    return std::move(left);
                }
                case Expr::Kind::kOr: {
                    for (size_t i = 0; i < len; ++i) { left[i] = (left[i] | right[i]) & kMask; }
                    return std::move(left);
                }
                case Expr::Kind::kXor: {
                    for (size_t i = 0; i < len; ++i) { left[i] = (left[i] ^ right[i]) & kMask; }
                    return std::move(left);
                }

                // This is concrete evaluation at particular inputs rather than
                // a per-bit abstraction, so a comparison evaluates exactly.
                case Expr::Kind::kCmpEq:
                case Expr::Kind::kCmpUlt:
                case Expr::Kind::kCmpSlt: {
                    for (size_t i = 0; i < len; ++i) {
                        left[i] = expr.kind == Expr::Kind::kCmpEq
                            ? ModCmpEq(left[i], right[i], bitwidth)
                            : (expr.kind == Expr::Kind::kCmpUlt
                                   ? ModCmpUlt(left[i], right[i], bitwidth)
                                   : ModCmpSlt(left[i], right[i], bitwidth));
                    }
                    return std::move(left);
                }
            }
            return std::vector< uint64_t >(len, 0);
        }

#if COBRA_NONRECURSIVE

        std::vector< uint64_t > EvalSemilinearIterative(
            const Expr &root, size_t len, uint32_t bitwidth, uint32_t bit_pos
        ) {
            return FoldPostOrder< std::vector< uint64_t > >(
                root,
                [&](const Expr &node, std::vector< uint64_t > *kids, size_t count) {
                    std::vector< uint64_t > left;
                    std::vector< uint64_t > right;
                    if (count > 0) { left = std::move(kids[0]); }
                    if (count > 1) { right = std::move(kids[1]); }
                    return CombineSemilinear(
                        node, len, bitwidth, bit_pos, std::move(left), std::move(right)
                    );
                }
            );
        }

#else

        std::vector< uint64_t > EvalSemilinearRecursive(
            const Expr &expr, size_t len, uint32_t bitwidth, uint32_t bit_pos
        ) {
            std::vector< uint64_t > left;
            std::vector< uint64_t > right;
            if (!expr.children.empty()) {
                left = EvalSemilinearRecursive(*expr.children[0], len, bitwidth, bit_pos);
            }
            if (expr.children.size() > 1) {
                right = EvalSemilinearRecursive(*expr.children[1], len, bitwidth, bit_pos);
            }
            return CombineSemilinear(expr, len, bitwidth, bit_pos, std::move(left), std::move(right));
        }

#endif

    } // namespace

    std::vector< uint64_t > EvaluateSemilinearRow(
        const Expr &expr, uint32_t num_vars, uint32_t bitwidth, uint32_t bit_pos
    ) {
        // 1 << num_vars is UB for num_vars >= 64. Even though no
        // production code calls this evaluator today (it is reachable
        // only from tests of the per-row strategy), guard the boundary
        // so the UB cannot reach a future caller.
        assert(num_vars < 64 && "EvaluateSemilinearRow: 1 << num_vars UB for num_vars >= 64");
        const size_t kLen = size_t{ 1 } << num_vars;
#if COBRA_NONRECURSIVE
        auto result       = EvalSemilinearIterative(expr, kLen, bitwidth, bit_pos);
#else
        auto result       = EvalSemilinearRecursive(expr, kLen, bitwidth, bit_pos);
#endif

        if (bit_pos > 0 && bit_pos < 64) {
            const uint64_t kMask = Bitmask(bitwidth);
            for (auto &v : result) { v = (v >> bit_pos) & kMask; }
        }
        return result;
    }

    // Rows over more variables than this are sampled one variable at a time.
    constexpr uint32_t kMaxFullRowVars = 8;

    bool IsLinearShortcut(const Expr &expr, uint32_t num_vars, uint32_t bitwidth) {
        COBRA_TRACE(
            "SemilinearSig", "IsLinearShortcut: vars={} bitwidth={}", num_vars, bitwidth
        );
        if (bitwidth == 0 || num_vars > 20) { return false; }

        const uint64_t kMask = Bitmask(bitwidth);
        // Up to (2^vars - 1) * (bitwidth - 1) points, so the tree is compiled
        // once and each point is a pass over the program rather than a walk
        // of the tree. A variable past `num_vars` - a malformed tree - reads
        // as 0.
        const CompiledExpr program = CompileExpr(expr, bitwidth);
        std::vector< uint64_t > assignment(std::max(num_vars, program.arity), 0);
        std::vector< uint64_t > stack;
        const auto at = [&] { return EvalCompiledExpr(program, assignment, stack); };
        const uint64_t kFZero = at();

        // The whole row, when it is small enough to evaluate: every subset of
        // the variables set to bit `b` must move the result by 2^b times what
        // the same subset does at bit 0. Setting one variable at a time is not
        // enough - `a + (b & m) - (a ^ (b & m))` is `2 * (a & b & m)`, which is
        // zero whenever only one of them is set and nowhere near linear.
        if (num_vars <= kMaxFullRowVars) {
            const size_t kLen = size_t{ 1 } << num_vars;
            std::vector< uint64_t > base_row(kLen);
            const auto delta_at = [&](size_t subset, uint64_t value) {
                for (uint32_t j = 0; j < num_vars; ++j) {
                    assignment[j] = ((subset >> j) & 1) != 0 ? value : 0;
                }
                const uint64_t kDelta = (at() - kFZero) & kMask;
                std::fill(assignment.begin(), assignment.begin() + num_vars, 0);
                return kDelta;
            };
            for (size_t subset = 1; subset < kLen; ++subset) {
                base_row[subset] = delta_at(subset, 1);
            }
            for (uint32_t bit = 1; bit < bitwidth; ++bit) {
                const uint64_t kBitVal = 1ULL << bit;
                for (size_t subset = 1; subset < kLen; ++subset) {
                    const uint64_t kExpect = (base_row[subset] * kBitVal) & kMask;
                    if (delta_at(subset, kBitVal) != kExpect) {
                        COBRA_TRACE(
                            "SemilinearSig", "IsLinearShortcut: false (subset={} bit={})",
                            subset, bit
                        );
                        return false;
                    }
                }
            }
            COBRA_TRACE("SemilinearSig", "IsLinearShortcut: true");
            return true;
        }

        // Extract per-variable coefficient from bit 0.
        std::vector< uint64_t > coeff(num_vars);
        for (uint32_t j = 0; j < num_vars; ++j) {
            assignment[j] = 1;
            coeff[j]      = (at() - kFZero) & kMask;
            assignment[j] = 0;
        }

        // Verify at each bit position: delta == coeff[j] * 2^bit.
        for (uint32_t bit = 1; bit < bitwidth; ++bit) {
            const uint64_t kBitVal = 1ULL << bit;
            for (uint32_t j = 0; j < num_vars; ++j) {
                assignment[j]         = kBitVal;
                const uint64_t kDelta = (at() - kFZero) & kMask;
                const uint64_t kExpect = (coeff[j] * kBitVal) & kMask;
                if (kDelta != kExpect) {
                    COBRA_TRACE(
                        "SemilinearSig",
                        "IsLinearShortcut: false (var={} bit={} delta={} expect={})", j, bit,
                        kDelta, kExpect
                    );
                    return false;
                }
                assignment[j] = 0;
            }
        }

        COBRA_TRACE("SemilinearSig", "IsLinearShortcut: true");
        return true;
    }

} // namespace cobra
