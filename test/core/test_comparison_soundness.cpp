#include "cobra/core/BitWidth.h"
#include "cobra/core/Evaluator.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/SignatureEval.h"
#include "cobra/core/Simplifier.h"
#include <gtest/gtest.h>

using namespace cobra;

namespace {

    // A comparison is true on a vanishing fraction of its inputs, so a probe
    // set agreeing with a candidate is no evidence that the candidate computes
    // the input. Each case below names an input on which a probe-fitted answer
    // differs, and requires any answer Simplify gives either to agree there or
    // not to claim it was verified - the caller then has to prove it.
    void ExpectAgreesAt(
        const Expr &input, uint32_t num_vars, uint32_t bitwidth, uint64_t demanded_mask,
        const std::vector< std::vector< uint64_t > > &points
    ) {
        std::vector< std::string > vars;
        for (uint32_t i = 0; i < num_vars; ++i) { vars.push_back("v" + std::to_string(i)); }
        Options opts{ .bitwidth      = bitwidth,
                      .evaluator     = Evaluator::FromExpr(input, bitwidth),
                      .demanded_mask = demanded_mask };
        const auto sig = EvaluateBooleanSignature(input, num_vars, bitwidth);
        auto result    = Simplify(sig, vars, &input, opts);
        ASSERT_TRUE(result.has_value());
        if (result.value().kind != SimplifyOutcome::Kind::kSimplified) { return; }
        const auto &outcome = result.value();
        ASSERT_NE(outcome.expr, nullptr);
        if (!outcome.verified) { return; }
        // The answer may name a subset of the inputs; map it back by name.
        std::vector< uint32_t > index_of;
        for (const auto &name : outcome.real_vars) {
            index_of.push_back(static_cast< uint32_t >(std::stoul(name.substr(1))));
        }
        const auto answer = Evaluator::FromExpr(*outcome.expr, bitwidth);
        const auto truth  = Evaluator::FromExpr(input, bitwidth);
        const uint64_t mask = Bitmask(bitwidth) & demanded_mask;
        for (const auto &point : points) {
            std::vector< uint64_t > narrowed;
            for (const auto index : index_of) { narrowed.push_back(point[index]); }
            EXPECT_EQ(answer(narrowed) & mask, truth(point) & mask)
                << "answer " << Render(*outcome.expr, outcome.real_vars, bitwidth)
                << " differs from the input at v0=" << point[0];
        }
    }

} // namespace

// `x ^ ((3*x) == 0x12345678)` has the Boolean signature of `x` and agrees with
// it on every probe, but not where `3*x` meets the constant. The search may
// well answer `x` - it fits answers to probes - but must not call it verified.
TEST(ComparisonSoundness, ProbeFittedAnswerIsNotClaimedVerified) {
    auto input = Expr::BitwiseXor(
        Expr::Variable(0),
        Expr::CmpEq(Expr::Mul(Expr::Constant(3), Expr::Variable(0)), Expr::Constant(0x12345678))
    );
    ExpectAgreesAt(*input, 1, 32, UINT64_MAX, { { 0x06117228 }, { 0 }, { 1 } });
}

// `((x <u 300) + x) & 0xFF` must not be solved in the 8-bit ring: 300 does
// not survive the narrowing, so the comparison changes meaning.
TEST(ComparisonSoundness, LowBitMaskNarrowingKeepsComparisons) {
    auto input = Expr::BitwiseAnd(
        Expr::Add(Expr::CmpUlt(Expr::Variable(0), Expr::Constant(300)), Expr::Variable(0)),
        Expr::Constant(0xFF)
    );
    ExpectAgreesAt(*input, 1, 64, UINT64_MAX, { { 50 }, { 100 }, { 299 }, { 300 }, { 301 } });
}

// An answer the ring normal form proves over the comparison's two values is
// still a verified one: the comparison cancels whatever it compares.
TEST(ComparisonSoundness, RingProvedAnswerStaysVerified) {
    auto cmp   = [] { return Expr::CmpUlt(Expr::Variable(1), Expr::Variable(2)); };
    auto input = Expr::Add(
        Expr::Add(Expr::Variable(0), Expr::Mul(Expr::Constant(3), cmp())),
        Expr::Negate(Expr::Mul(Expr::Constant(3), cmp()))
    );
    std::vector< std::string > vars{ "v0", "v1", "v2" };
    Options opts{ .bitwidth = 64, .evaluator = Evaluator::FromExpr(*input, 64) };
    const auto sig = EvaluateBooleanSignature(*input, 3, 64);
    auto result    = Simplify(sig, vars, input.get(), opts);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result.value().kind, SimplifyOutcome::Kind::kSimplified);
    EXPECT_TRUE(result.value().verified)
        << Render(*result.value().expr, result.value().real_vars, 64);
}
