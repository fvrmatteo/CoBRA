#include "IRReconstructor.h"
#include "MBADetector.h"
#include "SmtStrategy.h"
#include "cobra/core/Expr.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/verify/Z3Verifier.h"
#include "../RandomExpr.h"
#include <gtest/gtest.h>

#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using namespace cobra;
using namespace cobra::testing;

// The rewrite is found and checked on CoBRA's own evaluator, proved by the
// solver from one lowering and built into the function by another. Each of
// them is checked here against the evaluator, on random expressions over every
// node kind with their variables fixed: the IR builder folds what the
// reconstruction emits, and the solver is asked whether the expression is the
// value the evaluator gives.

TEST(LoweringsAgree, ReconstructedIrComputesWhatTheEvaluatorDoes) {
    llvm::LLVMContext context;
    llvm::Module module("lowerings", context);
    uint64_t seed = 1000;
    for (const uint32_t bitwidth : kWidths) {
        auto *type = llvm::IntegerType::get(context, bitwidth);
        auto *function = llvm::Function::Create(
            llvm::FunctionType::get(type, false), llvm::Function::ExternalLinkage,
            "f" + std::to_string(bitwidth), module
        );
        auto *block = llvm::BasicBlock::Create(context, "entry", function);
        llvm::IRBuilder<> builder(block);
        for (int round = 0; round < 300; ++round, ++seed) {
            const uint32_t vars = 1 + static_cast< uint32_t >(seed % 4);
            RandomExpr random(seed, vars, bitwidth);
            const auto expr = random.Build(5);
            std::vector< uint64_t > values(vars);
            for (auto &value : values) { value = random.Input(); }
            MBACandidate candidate{};
            candidate.bitwidth = bitwidth;
            for (const uint64_t value : values) {
                candidate.leaf_values.push_back(
                    llvm::ConstantInt::get(type, value & Bitmask(bitwidth))
                );
                candidate.leaf_widths.push_back(bitwidth);
            }
            auto *built    = ReconstructIr(*expr, candidate, builder);
            auto *constant = llvm::dyn_cast< llvm::ConstantInt >(built);
            ASSERT_NE(constant, nullptr) << Render(*expr, VariableNames(vars), bitwidth);
            EXPECT_EQ(constant->getZExtValue(), EvalExpr(*expr, values, bitwidth))
                << "w=" << bitwidth << ": " << Render(*expr, VariableNames(vars), bitwidth);
        }
    }
}

TEST(LoweringsAgree, SolverLoweringsProveWhatTheEvaluatorComputes) {
    uint64_t seed = 5000;
    for (const uint32_t bitwidth : kWidths) {
        for (int round = 0; round < 25; ++round, ++seed) {
            const uint32_t vars = 1 + static_cast< uint32_t >(seed % 4);
            RandomExpr random(seed, vars, bitwidth);
            const auto expr = random.Build(4);
            std::vector< uint64_t > values(vars);
            for (auto &value : values) { value = random.Input(); }
            const auto fixed = Substitute(*expr, values);
            const auto truth = Expr::Constant(EvalExpr(*expr, values, bitwidth));
            for (const auto strategy : { SmtStrategy::kDirect, SmtStrategy::kLLVM }) {
                Z3VerificationSettings settings;
                settings.timeout_ms = 10000;
                settings.strategy   = strategy;
                const auto result =
                    VerifyExprsWithStrategy(*fixed, *truth, {}, bitwidth, settings);
                EXPECT_TRUE(result.equivalent && !result.unknown)
                    << (strategy == SmtStrategy::kDirect ? "direct" : "llvm")
                    << ", w=" << bitwidth << ": "
                    << Render(*expr, VariableNames(vars), bitwidth) << " at "
                    << Render(*fixed, {}, bitwidth) << " is " << truth->constant_val
                    << (result.counterexample.empty() ? "" : " - " + result.counterexample);
            }
        }
    }
}
