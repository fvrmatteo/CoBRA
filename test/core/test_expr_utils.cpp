#include "cobra/core/ExprUtils.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureEval.h"
#include "cobra/core/Simplifier.h"
#include <random>
#include <stdexcept>
#include <gtest/gtest.h>

namespace cobra {

    TEST(ExprUtils, TryBuildVarSupportMapsSubset) {
        const std::vector< std::string > vars   = { "x", "y", "z" };
        const std::vector< std::string > subset = { "z", "x" };

        auto support = TryBuildVarSupport(vars, subset);

        ASSERT_TRUE(support.has_value());
        EXPECT_EQ(*support, (std::vector< uint32_t >{ 2, 0 }));
    }

    TEST(ExprUtils, TryBuildVarSupportRejectsUnknownVariable) {
        const std::vector< std::string > vars   = { "x", "y" };
        const std::vector< std::string > subset = { "y", "missing" };

        EXPECT_FALSE(TryBuildVarSupport(vars, subset).has_value());
        EXPECT_THROW((void) BuildVarSupport(vars, subset), std::out_of_range);
    }

    namespace {

        // `zext (xor i16 (add i16 (trunc (x & -16)), -128), 4951)` as a tree
        // reads it: the narrow add held zero-extended and masked to 16 bits.
        std::unique_ptr< Expr > NarrowLowHalf() {
            auto aligned   = Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(~uint64_t{ 15 }));
            auto truncated = Expr::BitwiseAnd(std::move(aligned), Expr::Constant(0xFFFF));
            auto sum       = Expr::BitwiseAnd(
                Expr::Add(std::move(truncated), Expr::Constant(0xFF80)), Expr::Constant(0xFFFF)
            );
            return Expr::BitwiseXor(std::move(sum), Expr::Constant(4951));
        }

        // The same number's high part computed at 64 bits:
        // `((x & -16) - 128) ^ K1) & -65536 | low ^ K2` is `((x & -16) - 128) ^ C`.
        std::unique_ptr< Expr > SavedStackPointer() {
            auto wide = Expr::Add(
                Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(~uint64_t{ 15 })),
                Expr::Constant(static_cast< uint64_t >(-128))
            );
            auto high = Expr::BitwiseAnd(
                Expr::BitwiseXor(std::move(wide), Expr::Constant(uint64_t{ 0x0F1E2D3C4B5A6978 })),
                Expr::Constant(~uint64_t{ 0xFFFF })
            );
            return Expr::BitwiseXor(
                Expr::BitwiseOr(std::move(high), NarrowLowHalf()),
                Expr::Constant(uint64_t{ 0x1122334455660000 })
            );
        }

    } // namespace

    TEST(ExprUtils, NormalizeLowBitMasksReadsANarrowSumAsTheWideOne) {
        bool changed    = false;
        auto normalized = NormalizeLowBitMasks(*NarrowLowHalf(), 64, changed);
        EXPECT_TRUE(changed);
        EXPECT_EQ(Render(*normalized, { "x" }), "(x & -16) + -128 & 65535 ^ 4951");
    }

    TEST(ExprUtils, NormalizeLowBitMasksComputesTheSameFunction) {
        const auto input = SavedStackPointer();
        bool changed     = false;
        const auto normalized = NormalizeLowBitMasks(*input, 64, changed);
        ASSERT_TRUE(changed);
        std::mt19937_64 random(7);
        for (int probe = 0; probe < 4096; ++probe) {
            const std::vector< uint64_t > x = { probe < 16 ? static_cast< uint64_t >(probe) * 0x1111 : random() };
            ASSERT_EQ(EvalExpr(*input, x, 64), EvalExpr(*normalized, x, 64));
        }
    }

    TEST(ExprUtils, NormalizeLowBitMasksKeepsMasksUnderAShiftRight) {
        // `(x & 0xFFFF) >> 4` reads bits 4..15 of `x` and none above, so the mask
        // under the shift is what decides the result and has to stay.
        auto input = Expr::BitwiseAnd(
            Expr::LogicalShr(Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFFFF)), 4),
            Expr::Constant(0xFF)
        );
        bool changed    = false;
        auto normalized = NormalizeLowBitMasks(*input, 64, changed);
        EXPECT_FALSE(changed);
        EXPECT_EQ(Render(*normalized, { "x" }), Render(*input, { "x" }));
    }

    TEST(ExprUtils, NormalizedNarrowHalfLetsTheSearchJoinTheTwoHalves) {
        const auto input       = SavedStackPointer();
        const std::vector< std::string > vars = { "x" };
        Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
        opts.enabled_families = TechniqueFamily::kSemilinear | TechniqueFamily::kBitwise | TechniqueFamily::kLifting;
        const auto sig = EvaluateBooleanSignature(*input, 1, 64);
        auto result    = Simplify(sig, vars, input.get(), opts);
        ASSERT_TRUE(result.has_value());
        ASSERT_EQ(result.value().kind, SimplifyOutcome::Kind::kSimplified);
        std::mt19937_64 random(11);
        for (int probe = 0; probe < 1024; ++probe) {
            const std::vector< uint64_t > x = { random() };
            ASSERT_EQ(EvalExpr(*result.value().expr, x, 64), EvalExpr(*input, x, 64));
        }
    }

} // namespace cobra

