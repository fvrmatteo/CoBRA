#include "cobra/core/SubtermReading.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/SignatureChecker.h"
#include <gtest/gtest.h>
#include <random>

namespace cobra {

    namespace {

        void ExpectSameFunction(const Expr &expr, const Expr &answer, uint32_t vars) {
            std::mt19937_64 random(23);
            for (int probe = 0; probe < 2048; ++probe) {
                std::vector< uint64_t > inputs(vars);
                for (auto &input : inputs) {
                    input = probe < 16 ? static_cast< uint64_t >(probe) : random();
                }
                ASSERT_EQ(EvalExpr(expr, inputs, 64), EvalExpr(answer, inputs, 64));
            }
        }

        Options Opts() { return Options{ .bitwidth = 64, .max_vars = 2, .spot_check = true }; }

    } // namespace

    // `(d & M) | (~d & ~M)` over the sum `d = a + b` is `d ^ ~M`: every bit of the
    // root is a function of the same bit of `d`.
    TEST(SubtermReading, ReadsABitwiseFunctionOfOneSubterm) {
        const uint64_t m = 0x0F0F00FF12345678ULL;
        const auto d     = [] { return Expr::Add(Expr::Variable(0), Expr::Variable(1)); };
        auto tree        = Expr::BitwiseOr(
            Expr::BitwiseAnd(d(), Expr::Constant(m)),
            Expr::BitwiseAnd(Expr::BitwiseNot(d()), Expr::Constant(~m))
        );
        auto answer = ReadAsBitwiseOfSubterms(*tree, 2, 64, Opts());
        ASSERT_NE(answer, nullptr);
        EXPECT_EQ(Render(*answer, { "a", "b" }), "a + b ^ -1085087130741659257");
        ExpectSameFunction(*tree, *answer, 2);
        EXPECT_TRUE(IsBetter(ComputeCost(*answer).cost, ComputeCost(*tree).cost));
    }

    // Two subterms: `(s & ~t) ^ (t & 0xFFFF)` over the sums `s = a + b` and
    // `t = a - b` is a function of neither alone, and of the two bit by bit.
    TEST(SubtermReading, ReadsABitwiseFunctionOfTwoSubterms) {
        const auto s = [] { return Expr::Add(Expr::Variable(0), Expr::Variable(1)); };
        const auto t = [] { return Expr::Add(Expr::Variable(0), Expr::Negate(Expr::Variable(1))); };
        auto tree    = Expr::BitwiseXor(
            Expr::BitwiseAnd(s(), Expr::BitwiseNot(t())), Expr::BitwiseAnd(t(), Expr::Constant(0xFFFF))
        );
        auto answer = ReadAsBitwiseOfSubterms(*tree, 2, 64, Opts());
        ASSERT_NE(answer, nullptr);
        ExpectSameFunction(*tree, *answer, 2);
    }

    // A root that is the same on every input is that constant.
    TEST(SubtermReading, ReadsAConstant) {
        const auto d = [] { return Expr::Add(Expr::Variable(0), Expr::Variable(1)); };
        auto tree    = Expr::BitwiseXor(Expr::BitwiseXor(d(), Expr::Constant(77)), d());
        auto answer  = ReadAsBitwiseOfSubterms(*tree, 2, 64, Opts());
        ASSERT_NE(answer, nullptr);
        ASSERT_EQ(answer->kind, Expr::Kind::kConstant);
        EXPECT_EQ(answer->constant_val, 77u);
    }

    // A carry between bits is no bitwise function of any subterm.
    TEST(SubtermReading, TurnsDownACarry) {
        auto tree = Expr::Add(
            Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1)),
            Expr::BitwiseXor(Expr::Variable(0), Expr::Mul(Expr::Variable(1), Expr::Variable(1)))
        );
        EXPECT_EQ(ReadAsBitwiseOfSubterms(*tree, 2, 64, Opts()), nullptr);
    }

} // namespace cobra
