#include "SmtStrategy.h"
#include "cobra/core/Expr.h"
#include "cobra/llvm/CobraStats.h"
#include "cobra/verify/Z3Verifier.h"
#include "verify/VerifyCache.h"
#include <gtest/gtest.h>

using namespace cobra;

namespace {

    Z3VerificationSettings Settings(Z3UnknownResultMode mode, SmtStrategy strategy) {
        Z3VerificationSettings settings;
        settings.timeout_ms          = 1000;
        settings.unknown_result_mode = mode;
        settings.strategy            = strategy;
        return settings;
    }

} // namespace

// A memo restored into a fresh process says which questions came back unknown,
// not how the run that asked them took an unknown. Served from it, an unknown
// is taken the way the asker's own policy says - as asking again would.
TEST(UnknownMemo, RestoredUnknownFollowsTheAskersPolicy) {
    auto lhs = Expr::Add(Expr::Variable(0), Expr::Constant(7));
    auto rhs = Expr::Add(Expr::Constant(7), Expr::Variable(0));
    const std::vector< std::string > vars{ "a" };
    const auto lenient = Settings(Z3UnknownResultMode::kTreatAsEquivalent, SmtStrategy::kDirect);
    const auto strict  = Settings(Z3UnknownResultMode::kFail, SmtStrategy::kDirect);

    UnknownMemo memo;
    memo.verify.push_back({ verify_detail::ExprQueryKey(*lhs, *rhs, vars, 32, lenient), true });
    memo.verify.push_back({ verify_detail::ExprQueryKey(*lhs, *rhs, vars, 32, strict), true });
    RestoreUnknownMemo(memo);

    const auto taken = Z3VerifyExprs(*lhs, *rhs, vars, 32, lenient);
    EXPECT_TRUE(taken.unknown);
    EXPECT_TRUE(taken.equivalent);
    const auto declined = Z3VerifyExprs(*lhs, *rhs, vars, 32, strict);
    EXPECT_TRUE(declined.unknown);
    EXPECT_FALSE(declined.equivalent);
}

// A question asked through a strategy is remembered like a direct one, so a
// saved memo carries its unknowns too.
TEST(UnknownMemo, StrategyAnswersAreRemembered) {
    auto lhs = Expr::Mul(Expr::Variable(0), Expr::Constant(3));
    auto rhs = Expr::Add(Expr::Variable(0), Expr::Mul(Expr::Variable(0), Expr::Constant(2)));
    const std::vector< std::string > vars{ "a" };
    const auto settings = Settings(Z3UnknownResultMode::kFail, SmtStrategy::kRace);

    UnknownMemo memo;
    memo.verify.push_back(
        { verify_detail::StrategyQueryKey(*lhs, *rhs, vars, 16, settings), true }
    );
    RestoreUnknownMemo(memo);
    const auto served = VerifyExprsWithStrategy(*lhs, *rhs, vars, 16, settings);
    EXPECT_TRUE(served.unknown);

    // Asked fresh, at another width, it is proved - and that answer is kept.
    const auto proved = VerifyExprsWithStrategy(*lhs, *rhs, vars, 24, settings);
    EXPECT_TRUE(proved.equivalent);
    EXPECT_FALSE(proved.unknown);
    const auto again = VerifyExprsWithStrategy(*lhs, *rhs, vars, 24, settings);
    EXPECT_TRUE(again.equivalent);
}

// Leaves are named afresh every time a host finds them: the names are no part
// of the question.
TEST(UnknownMemo, KeysIgnoreVariableNames) {
    auto lhs           = Expr::Add(Expr::Variable(0), Expr::Variable(1));
    auto rhs           = Expr::Add(Expr::Variable(1), Expr::Variable(0));
    const auto settings = Settings(Z3UnknownResultMode::kFail, SmtStrategy::kDirect);
    EXPECT_EQ(
        verify_detail::ExprQueryKey(*lhs, *rhs, { "a", "b" }, 64, settings),
        verify_detail::ExprQueryKey(*lhs, *rhs, { "x", "y" }, 64, settings)
    );
}
