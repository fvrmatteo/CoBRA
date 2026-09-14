#include "cobra/core/ShiftLifting.h"

#include "cobra/core/BitWidth.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/SignatureEval.h"
#include "cobra/core/Simplifier.h"

#include <gtest/gtest.h>

#include <random>
#include <vector>

using namespace cobra;

namespace {

    std::unique_ptr< Expr > Var(uint32_t index) { return Expr::Variable(index); }
    std::unique_ptr< Expr > Const(uint64_t value) { return Expr::Constant(value); }

    // ((x + y + ((x ^ y) & 1)) >> 1) + ((x ^ y) >> 1): x | y below the top bit.
    std::unique_ptr< Expr > OrIdentity(std::unique_ptr< Expr > x, std::unique_ptr< Expr > y) {
        auto d   = Expr::BitwiseXor(CloneExpr(*x), CloneExpr(*y));
        auto c   = Expr::BitwiseAnd(CloneExpr(*d), Const(1));
        auto sum = Expr::Add(Expr::Add(std::move(x), std::move(y)), std::move(c));
        auto avg = Expr::LogicalShr(std::move(sum), 1);
        auto d1  = Expr::LogicalShr(std::move(d), 1);
        return Expr::Add(std::move(avg), std::move(d1));
    }

    // The same less (x ^ y): x & y below the top bit.
    std::unique_ptr< Expr > AndIdentity(std::unique_ptr< Expr > x, std::unique_ptr< Expr > y) {
        auto d = Expr::BitwiseXor(CloneExpr(*x), CloneExpr(*y));
        return Expr::Add(OrIdentity(std::move(x), std::move(y)), Expr::Negate(std::move(d)));
    }

    uint64_t Eval(const Expr &expr, const std::vector< uint64_t > &inputs, uint32_t bitwidth) {
        return Evaluator::FromExpr(expr, bitwidth)(inputs);
    }

    // The outcome's expression evaluated in the space of `vars`.
    uint64_t EvalOutcome(
        const SimplifyOutcome &outcome, const std::vector< std::string > &vars,
        const std::vector< uint64_t > &inputs, uint32_t bitwidth
    ) {
        auto expr = CloneExpr(*outcome.expr);
        if (!outcome.real_vars.empty()) {
            auto support = TryBuildVarSupport(vars, outcome.real_vars);
            EXPECT_TRUE(support.has_value());
            RemapVarIndices(*expr, *support);
        }
        return Eval(*expr, inputs, bitwidth);
    }

    std::vector< uint64_t > RandomInputs(std::mt19937_64 &rng, size_t count, uint32_t bitwidth) {
        std::vector< uint64_t > inputs(count);
        for (auto &value : inputs) { value = rng() & Bitmask(bitwidth); }
        return inputs;
    }

    Options MaskedOptions(uint32_t bitwidth, uint64_t mask) {
        Options opts;
        opts.bitwidth      = bitwidth;
        opts.max_vars      = 16;
        opts.spot_check    = true;
        opts.demanded_mask = mask;
        return opts;
    }

} // namespace

TEST(ShiftLiftingTest, DemandedWidthIsOnePastTheHighestBit) {
    EXPECT_EQ(DemandedWidth(0x8000, 32), 16U);
    EXPECT_EQ(DemandedWidth(0xFFFF, 32), 16U);
    EXPECT_EQ(DemandedWidth(1, 32), 1U);
    EXPECT_EQ(DemandedWidth(0, 32), 0U);
    EXPECT_EQ(DemandedWidth(UINT64_MAX, 32), 32U);
    EXPECT_EQ(DemandedWidth(UINT64_MAX, 64), 64U);
}

TEST(ShiftLiftingTest, ShiftOverAnAtomIsNotOverArithmetic) {
    auto atom = Expr::LogicalShr(Expr::BitwiseAnd(Var(0), Var(1)), 2);
    EXPECT_TRUE(IsBitwiseAtom(*atom));
    EXPECT_FALSE(HasShiftOverArithmetic(*atom));

    auto sum = Expr::LogicalShr(Expr::Add(Var(0), Var(1)), 1);
    EXPECT_FALSE(IsBitwiseAtom(*sum));
    EXPECT_TRUE(HasShiftOverArithmetic(*sum));
    EXPECT_TRUE(HasShiftOverArithmetic(*Expr::BitwiseXor(CloneExpr(*sum), Var(2))));
}

TEST(ShiftLiftingTest, LiftedExpressionIsTheScaledOriginal) {
    auto expr   = AndIdentity(Var(0), Var(1));
    auto lifted = LiftShifts(*expr, 32);
    ASSERT_TRUE(lifted.has_value());
    EXPECT_EQ(lifted->shift, 1U);
    EXPECT_FALSE(HasShiftOverArithmetic(*lifted->scaled));
    EXPECT_FALSE(ContainsType(*lifted->scaled, Expr::Kind::kShr));

    std::mt19937_64 rng(11);
    for (int sample = 0; sample < 256; ++sample) {
        const auto inputs = RandomInputs(rng, 2, 32);
        EXPECT_EQ(
            Eval(*lifted->scaled, inputs, 32), ModMul(2, Eval(*expr, inputs, 32), 32)
        );
    }
}

TEST(ShiftLiftingTest, LiftDeclinesWhatItCannotScale) {
    // Nothing to lift.
    EXPECT_FALSE(LiftShifts(*Expr::LogicalShr(Var(0), 3), 32).has_value());
    // A sum shifted by more than one: its remainder is no bitwise atom.
    EXPECT_FALSE(LiftShifts(*Expr::LogicalShr(Expr::Add(Var(0), Var(1)), 2), 32).has_value());
    // A shift over a shift over a sum.
    auto nested = Expr::LogicalShr(
        Expr::Add(Expr::LogicalShr(Expr::Add(Var(0), Var(1)), 1), Var(2)), 1
    );
    EXPECT_FALSE(LiftShifts(*nested, 32).has_value());
    // Such a shift under a bitwise operator.
    auto under = Expr::BitwiseXor(Expr::LogicalShr(Expr::Add(Var(0), Var(1)), 1), Var(2));
    EXPECT_FALSE(LiftShifts(*under, 32).has_value());
}

TEST(ShiftLiftingTest, DivideByPowerOfTwoIsStructural) {
    auto doubled = Expr::Mul(Const(2), Expr::BitwiseAnd(Var(0), Var(1)));
    auto half    = DivideByPowerOfTwo(*doubled, 1, 32);
    ASSERT_TRUE(half != nullptr);
    EXPECT_EQ(half->kind, Expr::Kind::kAnd);

    // -2 * a + 4 divides to -a + 2, as a signed quotient.
    auto sum = Expr::Add(Expr::Mul(Const(Bitmask(32) - 1), Var(0)), Const(4));
    auto q   = DivideByPowerOfTwo(*sum, 1, 32);
    ASSERT_TRUE(q != nullptr);
    std::mt19937_64 rng(5);
    for (int sample = 0; sample < 64; ++sample) {
        const auto inputs = RandomInputs(rng, 1, 32);
        EXPECT_EQ(Eval(*q, inputs, 32), ModAdd(ModNeg(inputs[0], 32), 2, 32));
    }

    // A coefficient of one carries no factor of two.
    EXPECT_TRUE(DivideByPowerOfTwo(*Expr::BitwiseAnd(Var(0), Var(1)), 1, 32) == nullptr);
    EXPECT_TRUE(DivideByPowerOfTwo(*Expr::Add(std::move(doubled), Var(0)), 1, 32) == nullptr);
}

TEST(ShiftLiftingTest, DemandedMaskRecoversTheAndIdentity) {
    const std::vector< std::string > vars = { "x", "y" };
    auto expr = AndIdentity(Var(0), Var(1));
    auto sig  = EvaluateBooleanSignature(*expr, 2, 32);

    auto result = Simplify(sig, vars, expr.get(), MaskedOptions(32, 0x7FFFFFFF));
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->kind, SimplifyOutcome::Kind::kSimplified);
    EXPECT_TRUE(result->verified);
    EXPECT_FALSE(ContainsType(*result->expr, Expr::Kind::kShr));
    EXPECT_FALSE(ContainsType(*result->expr, Expr::Kind::kAdd));

    std::mt19937_64 rng(3);
    for (int sample = 0; sample < 256; ++sample) {
        const auto inputs = RandomInputs(rng, 2, 32);
        EXPECT_EQ(
            EvalOutcome(*result, vars, inputs, 32) & 0x7FFFFFFF, (inputs[0] & inputs[1]) & 0x7FFFFFFF
        );
    }
}

TEST(ShiftLiftingTest, DemandedMaskRecoversTheOrIdentity) {
    const std::vector< std::string > vars = { "x", "y" };
    auto expr = OrIdentity(Var(0), Var(1));
    auto sig  = EvaluateBooleanSignature(*expr, 2, 32);

    auto result = Simplify(sig, vars, expr.get(), MaskedOptions(32, 0xFFFF));
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->kind, SimplifyOutcome::Kind::kSimplified);
    EXPECT_FALSE(ContainsType(*result->expr, Expr::Kind::kShr));

    std::mt19937_64 rng(9);
    for (int sample = 0; sample < 256; ++sample) {
        const auto inputs = RandomInputs(rng, 2, 32);
        EXPECT_EQ(EvalOutcome(*result, vars, inputs, 32) & 0xFFFF, (inputs[0] | inputs[1]) & 0xFFFF);
    }
}

TEST(ShiftLiftingTest, EveryBitDemandedLeavesTheTopBitAlone) {
    // At full width the identity is not x & y - the top bit carries the sum's
    // overflow - and whatever comes back has to agree with it everywhere.
    const std::vector< std::string > vars = { "x", "y" };
    auto expr = AndIdentity(Var(0), Var(1));
    auto sig  = EvaluateBooleanSignature(*expr, 2, 32);

    Options opts;
    opts.bitwidth = 32;
    auto result   = Simplify(sig, vars, expr.get(), opts);
    ASSERT_TRUE(result.has_value());
    if (result->kind == SimplifyOutcome::Kind::kSimplified) {
        std::mt19937_64 rng(17);
        for (int sample = 0; sample < 256; ++sample) {
            const auto inputs = RandomInputs(rng, 2, 32);
            EXPECT_EQ(EvalOutcome(*result, vars, inputs, 32), Eval(*expr, inputs, 32));
        }
    }
}

TEST(ShiftLiftingTest, RootMaskOverShiftsIsSolved) {
    // (identity) & 0xFFFF: the mask is written in the expression, and the
    // shifts under it are lifted.
    const std::vector< std::string > vars = { "x", "y" };
    auto expr = Expr::BitwiseAnd(AndIdentity(Var(0), Var(1)), Const(0xFFFF));
    auto sig  = EvaluateBooleanSignature(*expr, 2, 32);

    Options opts;
    opts.bitwidth = 32;
    auto result   = Simplify(sig, vars, expr.get(), opts);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->kind, SimplifyOutcome::Kind::kSimplified);
    EXPECT_FALSE(ContainsType(*result->expr, Expr::Kind::kShr));

    std::mt19937_64 rng(23);
    for (int sample = 0; sample < 256; ++sample) {
        const auto inputs = RandomInputs(rng, 2, 32);
        EXPECT_EQ(EvalOutcome(*result, vars, inputs, 32), Eval(*expr, inputs, 32));
    }
}

TEST(ShiftLiftingTest, ShiftedAtomIsReadAsItsOwnVariable) {
    // The lane a flag chain computes in: one operand of the identity reads a
    // variable through a shift, which over the Boolean points is invisible.
    const std::vector< std::string > vars = { "a", "b", "c" };
    auto x = Expr::BitwiseXor(Var(0), Const(11218));
    auto y = Expr::BitwiseXor(
        Expr::BitwiseXor(
            Expr::BitwiseXor(Expr::LogicalShr(Var(1), 16), Const(27091)),
            Expr::BitwiseAnd(Var(2), Const(0xFFFF))
        ),
        Const(25205)
    );
    auto expr = AndIdentity(std::move(x), std::move(y));
    auto sig  = EvaluateBooleanSignature(*expr, 3, 32);

    auto lifted = LiftShifts(*expr, 32);
    ASSERT_TRUE(lifted.has_value());
    auto result = Simplify(sig, vars, expr.get(), MaskedOptions(32, 0x8000));
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->kind, SimplifyOutcome::Kind::kSimplified)
        << result->diag.reason << "\nscaled: " << Render(*lifted->scaled, vars);
    EXPECT_FALSE(ContainsType(*result->expr, Expr::Kind::kAdd));

    std::mt19937_64 rng(29);
    for (int sample = 0; sample < 256; ++sample) {
        const auto inputs = RandomInputs(rng, 3, 32);
        EXPECT_EQ(
            EvalOutcome(*result, vars, inputs, 32) & 0x8000, Eval(*expr, inputs, 32) & 0x8000
        );
    }
}

TEST(PartitionTranslationTest, ConstantsMovingBetweenOperandsAreOneAnd) {
    // `(a ^ 11218) + (b ^ 25205) - (a ^ b ^ 18855)`: the add-and-xor identity
    // over decorated operands, with the xor of the two decorations already
    // folded into one constant, as an optimizer leaves it. There is no
    // `(a ^ 11218) ^ (b ^ 25205)` left to match; per bit it is still
    // `2 * (a & b)` read through the constants' bits.
    const std::vector< std::string > vars = { "a", "b" };
    auto expr = Expr::Add(
        Expr::Add(
            Expr::BitwiseXor(Var(0), Const(11218)), Expr::BitwiseXor(Var(1), Const(25205))
        ),
        Expr::Negate(Expr::BitwiseXor(Expr::BitwiseXor(Var(0), Var(1)), Const(18855)))
    );
    auto sig = EvaluateBooleanSignature(*expr, 2, 32);

    Options opts;
    opts.bitwidth = 32;
    auto result   = Simplify(sig, vars, expr.get(), opts);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->kind, SimplifyOutcome::Kind::kSimplified);
    EXPECT_FALSE(ContainsType(*result->expr, Expr::Kind::kAdd))
        << Render(*result->expr, result->real_vars);

    std::mt19937_64 rng(31);
    for (int sample = 0; sample < 256; ++sample) {
        const auto inputs = RandomInputs(rng, 2, 32);
        EXPECT_EQ(EvalOutcome(*result, vars, inputs, 32), Eval(*expr, inputs, 32));
    }
}

TEST(PartitionTranslationTest, MaskedOperandIsAClearedVariable) {
    // `a + (b & 0xFFFF) - (a ^ (b & 0xFFFF))` is `2 * (a & b & 0xFFFF)`: the
    // same slice on every bit, with `b` cleared above the mask.
    const std::vector< std::string > vars = { "a", "b" };
    auto masked = [] { return Expr::BitwiseAnd(Var(1), Const(0xFFFF)); };
    auto expr   = Expr::Add(
        Expr::Add(Var(0), masked()), Expr::Negate(Expr::BitwiseXor(Var(0), masked()))
    );
    auto sig = EvaluateBooleanSignature(*expr, 2, 32);

    Options opts;
    opts.bitwidth = 32;
    auto result   = Simplify(sig, vars, expr.get(), opts);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->kind, SimplifyOutcome::Kind::kSimplified);
    EXPECT_FALSE(ContainsType(*result->expr, Expr::Kind::kXor))
        << Render(*result->expr, result->real_vars);

    std::mt19937_64 rng(37);
    for (int sample = 0; sample < 256; ++sample) {
        const auto inputs = RandomInputs(rng, 2, 32);
        EXPECT_EQ(EvalOutcome(*result, vars, inputs, 32), Eval(*expr, inputs, 32));
    }
}

TEST(PartitionTranslationTest, LaneWithFoldedConstantsIsTheAndOfItsOperands) {
    // The lane the flag chain computes its overflow bit in, as the
    // optimizer leaves it: the carry-in and the `x ^ y` it shifts are written
    // over the variables with the two operands' constants folded together,
    // and one operand reads a variable through a shift. On the low 16 bits it
    // is `x & y`.
    const std::vector< std::string > vars = { "a", "b", "c" };
    auto x = [] { return Expr::BitwiseXor(Var(0), Const(11218)); };
    auto w = [] {
        return Expr::BitwiseXor(
            Expr::BitwiseXor(Expr::LogicalShr(Var(1), 16), Const(27091)),
            Expr::BitwiseAnd(Var(2), Const(0xFFFF))
        );
    };
    auto y     = [&] { return Expr::BitwiseXor(w(), Const(25205)); };
    auto d     = [&] { return Expr::BitwiseXor(Expr::BitwiseXor(w(), Var(0)), Const(18855)); };
    auto carry = [&] {
        return Expr::BitwiseXor(
            Expr::BitwiseAnd(
                Expr::BitwiseXor(
                    Expr::BitwiseXor(
                        Expr::BitwiseXor(Expr::LogicalShr(Var(1), 16), Const(27091)), Var(2)
                    ),
                    Var(0)
                ),
                Const(1)
            ),
            Const(1)
        );
    };
    auto sum  = Expr::Add(Expr::Add(x(), y()), carry());
    auto avg  = Expr::LogicalShr(std::move(sum), 1);
    auto expr = Expr::Add(
        std::move(avg), Expr::Add(Expr::LogicalShr(d(), 1), Expr::Negate(d()))
    );
    auto sig = EvaluateBooleanSignature(*expr, 3, 32);

    auto result = Simplify(sig, vars, expr.get(), MaskedOptions(32, 0xFFFF));
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->kind, SimplifyOutcome::Kind::kSimplified);
    EXPECT_FALSE(ContainsType(*result->expr, Expr::Kind::kAdd))
        << Render(*result->expr, result->real_vars);

    std::mt19937_64 rng(41);
    for (int sample = 0; sample < 256; ++sample) {
        const auto inputs = RandomInputs(rng, 3, 32);
        EXPECT_EQ(
            EvalOutcome(*result, vars, inputs, 32) & 0xFFFF, Eval(*expr, inputs, 32) & 0xFFFF
        );
        const uint64_t kX = (inputs[0] ^ 11218) & 0xFFFF;
        const uint64_t kY =
            ((inputs[1] >> 16) ^ 27091 ^ (inputs[2] & 0xFFFF) ^ 25205) & 0xFFFF;
        EXPECT_EQ(EvalOutcome(*result, vars, inputs, 32) & 0xFFFF, kX & kY);
    }
}
