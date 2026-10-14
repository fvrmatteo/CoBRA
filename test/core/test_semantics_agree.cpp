#include "cobra/core/BitWidth.h"
#include "cobra/core/CompiledExpr.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureEval.h"
#include "../RandomExpr.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using namespace cobra;
using namespace cobra::testing;

// Every place that evaluates an expression has to give each node the same
// meaning: the probes that find a rewrite, the compiled program that checks it
// and the constant folder that tidies it must agree, or a rewrite checked by
// one is wrong by another. Random expressions over every node kind, at widths
// where the edge cases live (1 bit, odd widths, shift amounts at and past the
// width, constants wider than the width), evaluated every way.

TEST(SemanticsAgree, EveryEvaluatorGivesEveryNodeOneMeaning) {
    uint64_t seed = 1;
    for (const uint32_t bitwidth : kWidths) {
        for (int round = 0; round < 400; ++round, ++seed) {
            const uint32_t vars = 1 + static_cast< uint32_t >(seed % 4);
            RandomExpr random(seed, vars, bitwidth);
            const auto expr     = random.Build(5);
            const auto compiled = CompileExpr(*expr, bitwidth);
            std::vector< uint64_t > stack;
            for (int point = 0; point < 8; ++point) {
                std::vector< uint64_t > values(vars);
                for (auto &value : values) { value = random.Input(); }
                const uint64_t truth = EvalExpr(*expr, values, bitwidth);
                ASSERT_EQ(truth & ~Bitmask(bitwidth), 0u)
                    << "EvalExpr left bits above the width: "
                    << Render(*expr, VariableNames(vars), bitwidth);
                EXPECT_EQ(EvalCompiledExpr(compiled, values, stack), truth)
                    << "compiled, w=" << bitwidth << ": "
                    << Render(*expr, VariableNames(vars), bitwidth);
                EXPECT_EQ(EvalConstantExpr(*Substitute(*expr, values), bitwidth), truth)
                    << "constant folder, w=" << bitwidth << ": "
                    << Render(*expr, VariableNames(vars), bitwidth);
            }
            // The Boolean signature is the same expression on 0/1 inputs.
            const auto sig = EvaluateBooleanSignature(*expr, vars, bitwidth);
            for (uint32_t index = 0; index < (1U << vars); ++index) {
                std::vector< uint64_t > values(vars);
                for (uint32_t v = 0; v < vars; ++v) { values[v] = (index >> v) & 1; }
                EXPECT_EQ(sig[index], EvalExpr(*expr, values, bitwidth))
                    << "signature, w=" << bitwidth << ": "
                    << Render(*expr, VariableNames(vars), bitwidth);
            }
        }
    }
}
