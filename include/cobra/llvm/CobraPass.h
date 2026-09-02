#pragma once

#include "cobra/core/Simplifier.h"
#include "cobra/verify/Z3Verifier.h"

#include <cstdint>

#include "llvm/IR/PassManager.h"

namespace cobra {

    // What a rewrite has to be cheaper than before it is worth committing.
    enum class MbaCostModel : uint8_t {
        // Only the instructions the rewrite deletes: the root, plus everything
        // beneath it that nothing else reads. This never grows the function, but
        // it also declines a root whose operands are shared with the rest of the
        // block no matter how much simpler the root itself becomes — and a
        // simpler root is often the whole point, because it is what alias
        // analysis and the later passes actually look at.
        kDyingInstructions,

        // Every instruction the tree was built from, which is the expression
        // CoBRA was handed. A shared operand is left to the other readers that
        // need it anyway, so what gets priced is the IR in against the IR out.
        kTreeInstructions,
    };

    struct CobraPassOptions
    {
        uint32_t max_vars     = 16;
        uint32_t min_ast_size = 3;
        bool z3_verify        = false;
        Z3VerificationSettings z3_settings;
        TechniqueFamily enabled_families = TechniqueFamily::kAll;
        MbaCostModel cost_model          = MbaCostModel::kDyingInstructions;
    };

    class CobraPass : public llvm::PassInfoMixin< CobraPass >
    {
      public:
        explicit CobraPass(uint32_t max_vars = 16, uint32_t min_ast_size = 3)
            : CobraPass(
                  CobraPassOptions{ .max_vars = max_vars, .min_ast_size = min_ast_size }
              ) {}

        explicit CobraPass(CobraPassOptions options) : options_(options) {}

        // NOLINTNEXTLINE(readability-identifier-naming) - LLVM PassInfoMixin requires 'run'
        llvm::PreservedAnalyses run(llvm::Function &f, llvm::FunctionAnalysisManager &am);

        static bool isRequired() {
            return false;
        } // NOLINT(readability-identifier-naming) - LLVM interface

      private:
        CobraPassOptions options_;
    };

} // namespace cobra
