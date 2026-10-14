#include "../RandomExpr.h"
#include "SmtStrategy.h"
#include "cobra/core/Expr.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/verify/Z3Verifier.h"
#include <gtest/gtest.h>

#include <random>

using namespace cobra;
using namespace cobra::testing;

// The strategies put one question to the solver three ways - as built, through
// LLVM's optimizer, and both at once - with or without the sweep. Whatever way
// it is asked, a definite answer has to be the truth: an equal pair is never
// called different, and a pair the evaluator tells apart is never called
// equal. An unknown is allowed either way; it is no answer.
namespace {

    // `expr` with sums and exclusive ors spelled as identities of themselves.
    std::unique_ptr< Expr > Obfuscate(const Expr &expr, std::mt19937_64 &rng) {
        if (expr.children.empty()) { return CloneExpr(expr); }
        auto copy = CloneNode(expr);
        for (const auto &child : expr.children) {
            copy->children.push_back(Obfuscate(*child, rng));
        }
        if (rng() % 2 == 0) { return copy; }
        if (copy->kind == Expr::Kind::kAdd) {
            auto &a = copy->children[0];
            auto &b = copy->children[1];
            return Expr::Add(
                Expr::BitwiseXor(CloneExpr(*a), CloneExpr(*b)),
                Expr::Mul(Expr::Constant(2), Expr::BitwiseAnd(std::move(a), std::move(b)))
            );
        }
        if (copy->kind == Expr::Kind::kXor) {
            auto &a = copy->children[0];
            auto &b = copy->children[1];
            return Expr::Add(
                Expr::BitwiseOr(CloneExpr(*a), CloneExpr(*b)),
                Expr::Negate(Expr::BitwiseAnd(std::move(a), std::move(b)))
            );
        }
        return copy;
    }

    struct Way
    {
        SmtStrategy strategy;
        bool sweep;
        const char *name;
    };

    constexpr Way kWays[] = {
        { SmtStrategy::kDirect, false,     "direct" },
        {   SmtStrategy::kLLVM, false,       "llvm" },
        {   SmtStrategy::kLLVM,  true, "llvm+sweep" },
        {   SmtStrategy::kRace, false,       "race" },
        {   SmtStrategy::kRace,  true, "race+sweep" },
    };

    Z3VerificationSettings SettingsFor(const Way &way) {
        Z3VerificationSettings settings;
        settings.timeout_ms = 5000;
        settings.strategy   = way.strategy;
        settings.sweep      = way.sweep;
        return settings;
    }

} // namespace

TEST(SmtStrategies, EqualPairsAreNeverCalledDifferent) {
    std::mt19937_64 rng(23);
    for (const uint32_t bitwidth : { 8U, 16U, 32U }) {
        for (int round = 0; round < 12; ++round) {
            const uint32_t vars = 1 + static_cast< uint32_t >(round % 3);
            RandomExpr random(1000 + round * 7 + bitwidth, vars, bitwidth);
            const auto lhs = random.Build(3);
            const auto rhs = Obfuscate(*lhs, rng);
            for (const auto &way : kWays) {
                const auto result = VerifyExprsWithStrategy(
                    *lhs, *rhs, VariableNames(vars), bitwidth, SettingsFor(way)
                );
                EXPECT_TRUE(result.equivalent || result.unknown)
                    << way.name << ", w=" << bitwidth << ": "
                    << Render(*lhs, VariableNames(vars), bitwidth) << " vs "
                    << Render(*rhs, VariableNames(vars), bitwidth);
            }
        }
    }
}

TEST(SmtStrategies, PairsTheEvaluatorTellsApartAreNeverCalledEqual) {
    std::mt19937_64 rng(29);
    for (const uint32_t bitwidth : { 8U, 16U, 32U }) {
        for (int round = 0; round < 12; ++round) {
            const uint32_t vars = 1 + static_cast< uint32_t >(round % 3);
            RandomExpr random(2000 + round * 11 + bitwidth, vars, bitwidth);
            const auto lhs = random.Build(3);
            auto rhs       = Obfuscate(*lhs, rng);
            rhs            = Expr::BitwiseXor(
                std::move(rhs), Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(1))
            );
            for (const auto &way : kWays) {
                const auto result = VerifyExprsWithStrategy(
                    *lhs, *rhs, VariableNames(vars), bitwidth, SettingsFor(way)
                );
                EXPECT_FALSE(result.equivalent && !result.unknown)
                    << way.name << ", w=" << bitwidth << ": "
                    << Render(*lhs, VariableNames(vars), bitwidth) << " vs "
                    << Render(*rhs, VariableNames(vars), bitwidth);
            }
        }
    }
}
