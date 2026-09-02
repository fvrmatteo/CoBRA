#pragma once

#include "cobra/core/Simplifier.h"
#include "cobra/verify/Z3Verifier.h"

#include <cstdint>

#include "llvm/IR/PassManager.h"

namespace cobra {

    // What a rewrite has to be cheaper than before it is worth committing.
    enum class MbaCostModel : uint8_t {
        // Every instruction the tree was built from, which is the expression
        // CoBRA was handed. A shared operand is left to the other readers that
        // need it anyway, so what gets priced is the IR in against the IR out.
        kTreeInstructions,

        // Only the instructions the rewrite deletes: the root, plus everything
        // beneath it that nothing else reads. This never grows the function, but
        // it also declines a root whose operands are shared with the rest of the
        // block no matter how much simpler the root itself becomes — and a
        // simpler root is often the whole point, because it is what alias
        // analysis and the later passes actually look at.
        kDyingInstructions,
    };

    struct CobraPassOptions
    {
        uint32_t max_vars     = 16;
        uint32_t min_ast_size = 3;

        // How much tree a re-cut of a rejected root is allowed to keep. The
        // cuts that pay for themselves are small: measured over a heavily
        // obfuscated function, every rewrite a re-cut ever won came from a cut
        // of eight instructions or fewer, while the uncapped ladder went on to
        // build several thousand larger cuts that won nothing and cost a
        // signature sweep and a solve apiece.
        uint32_t max_recut_nodes = 12;

        // How many variables the collection that spawns a ladder may end in.
        // Re-cutting is worth doing when exhaustive expansion hid an identity
        // the root was written in; a collection that already resolves to many
        // independent leaves hid nothing. Measured over a heavily obfuscated
        // function, ladders started from collections of five variables or more
        // were a quarter of every cut ever built and won two rewrites out of
        // twenty-eight.
        uint32_t max_recut_vars = 6;

        bool z3_verify = false;
        Z3VerificationSettings z3_settings;
        TechniqueFamily enabled_families = TechniqueFamily::kAll;
        MbaCostModel cost_model          = MbaCostModel::kTreeInstructions;
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
