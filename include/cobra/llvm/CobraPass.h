#pragma once

#include "cobra/verify/Z3Verifier.h"

#include <cstdint>
#include <set>

#include "llvm/IR/PassManager.h"

namespace cobra {

    // Additional roots (beyond MBA opcodes) that CoBRA can be asked to
    // simplify. Combine with `|` in CobraPassOptions::target_contexts.
    enum MbaTargetContext : uint32_t {
        kMbaCtxBinaryOp   = 1u << 0, // MBA opcode roots (opcode-filtered).
        kMbaCtxLoadPtr    = 1u << 1, // Integer index of a load's getelementptr.
        kMbaCtxStorePtr   = 1u << 2, // Integer index of a store's getelementptr.
        kMbaCtxStoreValue = 1u << 3, // Integer value operand of a store.
        kMbaCtxReturn     = 1u << 4, // Integer operand of a return.
        kMbaCtxICmp       = 1u << 5, // Integer operands of an icmp.
        kMbaCtxIntToPtr   = 1u << 6, // Integer source of an inttoptr.
    };

    struct CobraPassOptions
    {
        uint32_t max_vars     = 16;
        uint32_t min_ast_size = 3;
        bool z3_verify        = false;
        Z3VerificationSettings z3_settings;
        // LLVM opcodes (llvm::Instruction::BinaryOps / CastOps) that CoBRA is
        // allowed to simplify. Empty means "all supported MBA opcodes".
        std::set< unsigned > target_opcodes;
        // MbaTargetContext bitmask selecting additional roots (load/store
        // pointers, store values, return operands, icmp operands).
        uint32_t target_contexts = kMbaCtxBinaryOp;
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
