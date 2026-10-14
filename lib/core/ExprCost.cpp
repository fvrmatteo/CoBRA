#include "cobra/core/ExprCost.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprTraversal.h"
#include <algorithm>
#include <cstdint>
#include <tuple>
#include <vector>

namespace cobra {

    namespace {

        // The cost of one node given the costs of its children, which is what
        // both the recursive and the iterative walk actually compute.
        CostInfo CombineCost(const Expr &expr, const CostInfo *lhs, const CostInfo *rhs) {
            switch (expr.kind) {
                case Expr::Kind::kConstant:
                    return {
                        .cost = { .weighted_size = 1, .nonlinear_mul_count = 0, .max_depth = 1 },
                        .has_var_dep = false
                    };

                case Expr::Kind::kVariable:
                    return {
                        .cost = { .weighted_size = 1, .nonlinear_mul_count = 0, .max_depth = 1 },
                        .has_var_dep = true
                    };

                case Expr::Kind::kNot:
                case Expr::Kind::kNeg:
                case Expr::Kind::kShr:
                    return {
                        .cost        = { .weighted_size       = lhs->cost.weighted_size + 1,
                                        .nonlinear_mul_count = lhs->cost.nonlinear_mul_count,
                                        .max_depth           = lhs->cost.max_depth + 1 },
                        .has_var_dep = lhs->has_var_dep
                    };

                case Expr::Kind::kAdd:
                case Expr::Kind::kAnd:
                case Expr::Kind::kOr:
                case Expr::Kind::kXor:
                case Expr::Kind::kCmpEq:
                case Expr::Kind::kCmpUlt:
                case Expr::Kind::kCmpSlt:
                    return {
                        .cost = { .weighted_size =
                                      lhs->cost.weighted_size + rhs->cost.weighted_size + 1,
                                 .nonlinear_mul_count = lhs->cost.nonlinear_mul_count
                                     + rhs->cost.nonlinear_mul_count,
                                 .max_depth =
                                      std::max(lhs->cost.max_depth, rhs->cost.max_depth) + 1 },
                        .has_var_dep = lhs->has_var_dep || rhs->has_var_dep
                    };

                case Expr::Kind::kMul: {
                    const bool kNonlinear  = lhs->has_var_dep && rhs->has_var_dep;
                    const uint32_t kWeight = kNonlinear ? 3 : 1;
                    const uint32_t kNlCount = lhs->cost.nonlinear_mul_count
                        + rhs->cost.nonlinear_mul_count + (kNonlinear ? 1 : 0);
                    return {
                        .cost = { .weighted_size =
                                      lhs->cost.weighted_size + rhs->cost.weighted_size + kWeight,
                                 .nonlinear_mul_count = kNlCount,
                                 .max_depth =
                                      std::max(lhs->cost.max_depth, rhs->cost.max_depth) + 1 },
                        .has_var_dep = lhs->has_var_dep || rhs->has_var_dep
                    };
                }

                default:
                    return {
                        .cost = { .weighted_size = 1, .nonlinear_mul_count = 0, .max_depth = 1 },
                        .has_var_dep = false
                    };
            }
        }

    } // namespace

#if COBRA_NONRECURSIVE

    CostInfo ComputeCost(const Expr &expr) {
        return FoldPostOrder< CostInfo >(
            expr,
            [](const Expr &node, CostInfo *kids, size_t count) {
                return CombineCost(
                    node, count > 0 ? &kids[0] : nullptr, count > 1 ? &kids[1] : nullptr
                );
            }
        );
    }

#else

    CostInfo ComputeCost(const Expr &expr) {
        CostInfo kids[2]{};
        const size_t count = expr.children.size();
        for (size_t k = 0; k < count; ++k) { kids[k] = ComputeCost(*expr.children[k]); }
        return CombineCost(
            expr, count > 0 ? &kids[0] : nullptr, count > 1 ? &kids[1] : nullptr
        );
    }

#endif

    bool IsBetter(const ExprCost &candidate, const ExprCost &baseline) {
        return std::tie(
                   candidate.weighted_size, candidate.nonlinear_mul_count, candidate.max_depth
               )
            < std::tie(
                   baseline.weighted_size, baseline.nonlinear_mul_count, baseline.max_depth
            );
    }

    bool IsCostBlowup(const ExprCost &candidate, const ExprCost &baseline) {
        // Output must exceed this multiple of the input to count as a blow-up.
        constexpr uint32_t kBlowupRatio    = 2;
        // ...and exceed this absolute weighted size, sparing the small ~2x
        // expansions of legitimate canonicalization on tiny inputs.
        constexpr uint32_t kBlowupAbsFloor = 32;
        return candidate.weighted_size > kBlowupRatio * baseline.weighted_size
            && candidate.weighted_size > kBlowupAbsFloor;
    }

} // namespace cobra
