#include "cobra/core/Expr.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureSimplifier.h"
#include "cobra/core/TemplateDecomposer.h"
#include <bit>
#include <gtest/gtest.h>
#include <random>

using namespace cobra;

namespace {

    SignatureContext
    make_ctx(uint32_t nv, const Evaluator &eval, const std::vector< std::string > &vars) {
        SignatureContext ctx;
        ctx.vars = vars;
        ctx.original_indices.resize(nv);
        for (uint32_t i = 0; i < nv; ++i) {
            ctx.original_indices[i] = i;
        }
        ctx.eval = eval;
        return ctx;
    }

    Options make_opts(uint32_t bw = 64) {
        Options opts;
        opts.bitwidth   = bw;
        opts.max_vars   = 16;
        opts.spot_check = true;
        return opts;
    }

} // namespace

TEST(TemplateDecomposer, DirectAtomMatch) {
    // f(x) = x  → should match directly
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0]; };
    auto ctx  = make_ctx(1, eval, { "x" });
    auto opts = make_opts();

    auto r = TryTemplateDecomposition(ctx, opts, 1, nullptr);
    ASSERT_TRUE(r.Succeeded());
    EXPECT_TRUE(r.Payload().verification == VerificationState::kVerified);
}

TEST(TemplateDecomposer, BinaryXor) {
    // f(x, y) = x ^ y
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] ^ v[1]; };
    auto ctx  = make_ctx(2, eval, { "x", "y" });
    auto opts = make_opts();

    auto r = TryTemplateDecomposition(ctx, opts, 2, nullptr);
    ASSERT_TRUE(r.Succeeded());
    EXPECT_TRUE(r.Payload().verification == VerificationState::kVerified);
}

TEST(TemplateDecomposer, BinaryMul) {
    // f(x, y) = x * y
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] * v[1]; };
    auto ctx  = make_ctx(2, eval, { "x", "y" });
    auto opts = make_opts();

    auto r = TryTemplateDecomposition(ctx, opts, 2, nullptr);
    ASSERT_TRUE(r.Succeeded());
    EXPECT_TRUE(r.Payload().verification == VerificationState::kVerified);
}

TEST(TemplateDecomposer, MulOrNested) {
    // f(b, c) = b * c | -(b ^ c)
    // A classic QSynth_EA pattern needing Layer 2
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return (v[0] * v[1]) | (-(v[0] ^ v[1]));
    };
    auto ctx  = make_ctx(2, eval, { "b", "c" });
    auto opts = make_opts();

    auto r = TryTemplateDecomposition(ctx, opts, 2, nullptr);
    ASSERT_TRUE(r.Succeeded());
    EXPECT_TRUE(r.Payload().verification == VerificationState::kVerified);

    // Verify the result computes the same function
    auto check = FullWidthCheckEval(*ctx.eval, 2, *r.Payload().expr, 64);
    EXPECT_TRUE(check.passed);
}

TEST(TemplateDecomposer, RespectsBaselineCost) {
    // f(x, y) = x ^ y — very cheap
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] ^ v[1]; };
    auto ctx  = make_ctx(2, eval, { "x", "y" });
    auto opts = make_opts();

    // Set an impossibly low baseline cost
    ExprCost baseline{ 0, 0, 0 };
    auto r = TryTemplateDecomposition(ctx, opts, 2, &baseline);
    // Should not return anything better than cost 0
    EXPECT_FALSE(r.Succeeded());
}

TEST(TemplateDecomposer, NoEvaluator) {
    SignatureContext ctx;
    ctx.vars  = { "x" };
    auto opts = make_opts();

    auto r = TryTemplateDecomposition(ctx, opts, 1, nullptr);
    EXPECT_FALSE(r.Succeeded());
}

TEST(TemplateDecomposer, TooManyVars) {
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0]; };
    std::vector< std::string > vars(7, "x");
    auto ctx  = make_ctx(7, eval, vars);
    auto opts = make_opts();

    auto r = TryTemplateDecomposition(ctx, opts, 7, nullptr);
    EXPECT_FALSE(r.Succeeded());
}

TEST(TemplateDecomposer, NarrowBitwidth) {
    // f(x, y) = (x + y) & 0xFF at 8-bit width
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return (v[0] + v[1]) & 0xFF;
    };
    auto ctx  = make_ctx(2, eval, { "x", "y" });
    auto opts = make_opts(8);

    auto r = TryTemplateDecomposition(ctx, opts, 2, nullptr);
    ASSERT_TRUE(r.Succeeded());
    EXPECT_TRUE(r.Payload().verification == VerificationState::kVerified);
}

TEST(TemplateDecomposer, CachedMissDoesNotPoisonOtherTargets) {
    auto unsupported_eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return std::rotl(static_cast< uint32_t >(v[0]), 5)
            ^ std::rotl(static_cast< uint32_t >(v[1]), 13)
            ^ std::rotl(static_cast< uint32_t >(v[2]), 21);
    };
    auto unsupported_ctx = make_ctx(3, unsupported_eval, { "x", "y", "z" });
    auto opts            = make_opts(32);

    EXPECT_FALSE(TryTemplateDecomposition(unsupported_ctx, opts, 3, nullptr).Succeeded());
    EXPECT_FALSE(TryTemplateDecomposition(unsupported_ctx, opts, 3, nullptr).Succeeded());

    auto supported_eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return static_cast< uint32_t >(v[0] + v[1] + v[2]);
    };
    auto supported_ctx = make_ctx(3, supported_eval, { "x", "y", "z" });
    EXPECT_TRUE(TryTemplateDecomposition(supported_ctx, opts, 3, nullptr).Succeeded());
}

TEST(TemplateDecomposer, CachedSuccessIsReverified) {
    auto identity_eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return static_cast< uint32_t >(v[0]);
    };
    auto opts         = make_opts(32);
    auto identity_ctx = make_ctx(1, identity_eval, { "x" });
    ASSERT_TRUE(TryTemplateDecomposition(identity_ctx, opts, 1, nullptr).Succeeded());

    std::mt19937_64 rng(0xC0B4A);
    std::vector< uint32_t > probe_roots(16);
    for (auto &root : probe_roots) {
        root = static_cast< uint32_t >(rng());
    }
    auto colliding_eval = [probe_roots](const std::vector< uint64_t > &v) -> uint64_t {
        const auto x   = static_cast< uint32_t >(v[0]);
        uint32_t delta = 1;
        for (const auto root : probe_roots) {
            delta *= x - root;
        }
        return static_cast< uint32_t >(x + delta);
    };
    auto colliding_ctx = make_ctx(1, colliding_eval, { "x" });
    EXPECT_FALSE(TryTemplateDecomposition(colliding_ctx, opts, 1, nullptr).Succeeded());
}
