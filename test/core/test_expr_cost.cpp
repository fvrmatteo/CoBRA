#include "cobra/core/Expr.h"
#include "cobra/core/ExprCost.h"
#include <gtest/gtest.h>

using namespace cobra;

TEST(ExprCostTest, LeafVariable) {
    auto e    = Expr::Variable(0);
    auto info = ComputeCost(*e);
    EXPECT_EQ(info.cost.weighted_size, 1u);
    EXPECT_EQ(info.cost.nonlinear_mul_count, 0u);
    EXPECT_EQ(info.cost.max_depth, 1u);
    EXPECT_TRUE(info.has_var_dep);
}

TEST(ExprCostTest, LeafConstant) {
    auto e    = Expr::Constant(42);
    auto info = ComputeCost(*e);
    EXPECT_EQ(info.cost.weighted_size, 1u);
    EXPECT_EQ(info.cost.nonlinear_mul_count, 0u);
    EXPECT_EQ(info.cost.max_depth, 1u);
    EXPECT_FALSE(info.has_var_dep);
}

TEST(ExprCostTest, LinearMul) {
    // 3 * x: Mul(const, var) -> linear, weight 1 for Mul node
    auto e    = Expr::Mul(Expr::Constant(3), Expr::Variable(0));
    auto info = ComputeCost(*e);
    // const:1 + var:1 + mul:1 = 3
    EXPECT_EQ(info.cost.weighted_size, 3u);
    EXPECT_EQ(info.cost.nonlinear_mul_count, 0u);
    EXPECT_EQ(info.cost.max_depth, 2u);
    EXPECT_TRUE(info.has_var_dep);
}

TEST(ExprCostTest, NonlinearMul) {
    // x * y: Mul(var, var) -> nonlinear, weight 3 for Mul node
    auto e    = Expr::Mul(Expr::Variable(0), Expr::Variable(1));
    auto info = ComputeCost(*e);
    // var:1 + var:1 + mul:3 = 5
    EXPECT_EQ(info.cost.weighted_size, 5u);
    EXPECT_EQ(info.cost.nonlinear_mul_count, 1u);
    EXPECT_EQ(info.cost.max_depth, 2u);
}

TEST(ExprCostTest, BitwiseOverPoly) {
    // d | (c * a): Or(var, Mul(var, var))
    auto inner = Expr::Mul(Expr::Variable(1), Expr::Variable(2));
    auto e     = Expr::BitwiseOr(Expr::Variable(0), std::move(inner));
    auto info  = ComputeCost(*e);
    // var:1 + var:1 + var:1 + mul:3 + or:1 = 7
    EXPECT_EQ(info.cost.weighted_size, 7u);
    EXPECT_EQ(info.cost.nonlinear_mul_count, 1u);
    EXPECT_EQ(info.cost.max_depth, 3u);
}

TEST(ExprCostTest, DepthCalculation) {
    // (x + y) * z -> depth 3
    auto sum  = Expr::Add(Expr::Variable(0), Expr::Variable(1));
    auto e    = Expr::Mul(std::move(sum), Expr::Variable(2));
    auto info = ComputeCost(*e);
    EXPECT_EQ(info.cost.max_depth, 3u);
}

TEST(ExprCostTest, SubtractionCost) {
    // x - y = Add(x, Neg(y)) -> 4 total
    auto e    = Expr::Add(Expr::Variable(0), Expr::Negate(Expr::Variable(1)));
    auto info = ComputeCost(*e);
    // var:1 + var:1 + neg:1 + add:1 = 4
    EXPECT_EQ(info.cost.weighted_size, 4u);
}

TEST(ExprCostTest, ShrCost) {
    auto e    = Expr::LogicalShr(Expr::Variable(0), 1);
    auto info = ComputeCost(*e);
    // var:1 + shr:1 = 2
    EXPECT_EQ(info.cost.weighted_size, 2u);
}

TEST(ExprCostTest, IsBetterPrimary) {
    ExprCost a{ 5, 1, 3 };
    ExprCost b{ 7, 0, 2 };
    EXPECT_TRUE(IsBetter(a, b));
    EXPECT_FALSE(IsBetter(b, a));
}

TEST(ExprCostTest, IsBetterSecondary) {
    ExprCost a{ 5, 0, 3 };
    ExprCost b{ 5, 1, 2 };
    EXPECT_TRUE(IsBetter(a, b));
}

TEST(ExprCostTest, IsBetterTiebreaker) {
    ExprCost a{ 5, 1, 2 };
    ExprCost b{ 5, 1, 3 };
    EXPECT_TRUE(IsBetter(a, b));
}

TEST(ExprCostTest, IsBetterEqual) {
    ExprCost a{ 5, 1, 3 };
    EXPECT_FALSE(IsBetter(a, a));
}

TEST(ExprCostTest, IsCostBlowupExponentialExpansion) {
    // 100 weighted vs 10: > 2x AND > 32 absolute -> blow-up.
    ExprCost candidate{ 100, 0, 5 };
    ExprCost baseline{ 10, 0, 3 };
    EXPECT_TRUE(IsCostBlowup(candidate, baseline));
}

TEST(ExprCostTest, IsCostBlowupSparedByRatio) {
    // 30 vs 20: not more than 2x (would need > 40), so not a blow-up
    // even though it exceeds the absolute floor.
    ExprCost candidate{ 30, 0, 4 };
    ExprCost baseline{ 20, 0, 4 };
    EXPECT_FALSE(IsCostBlowup(candidate, baseline));
}

TEST(ExprCostTest, IsCostBlowupSparedBySmallAbsoluteFloor) {
    // 15 vs 7: more than 2x, but below the absolute floor (32). This is
    // the legitimate NOT-over-arith canonicalization that must be kept.
    ExprCost candidate{ 15, 0, 4 };
    ExprCost baseline{ 7, 0, 3 };
    EXPECT_FALSE(IsCostBlowup(candidate, baseline));
}

TEST(ExprCostTest, IsCostBlowupBoundary) {
    // Exactly at both thresholds is NOT a blow-up (strict greater-than);
    // one past both is.
    EXPECT_FALSE(IsCostBlowup(ExprCost{ 32, 0, 1 }, ExprCost{ 16, 0, 1 }));
    EXPECT_TRUE(IsCostBlowup(ExprCost{ 33, 0, 1 }, ExprCost{ 16, 0, 1 }));
}

TEST(ExprCostTest, IsCostBlowupKeysOnSizeNotMulOrDepth) {
    // The gate axis is weighted_size only: a candidate that is smaller in
    // size is never a blow-up regardless of mul-count or depth.
    ExprCost candidate{ 10, 50, 50 };
    ExprCost baseline{ 100, 0, 1 };
    EXPECT_FALSE(IsCostBlowup(candidate, baseline));
}
