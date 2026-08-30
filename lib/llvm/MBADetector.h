#pragma once

#include "cobra/core/Expr.h"
#include "cobra/core/Simplifier.h"
#include "cobra/llvm/CobraPass.h"
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Value.h"

namespace cobra {

    struct MBACandidate
    {
        llvm::Instruction *root;
        std::vector< llvm::Value * > leaf_values;
        std::vector< std::string > var_names;
        std::vector< uint64_t > sig;
        uint32_t bitwidth;
        std::unique_ptr< Expr > expr;
        Evaluator evaluator;
    };

    // Find MBA candidates across a function.  Blocks are scanned in
    // post-order (uses before defs) with reverse instruction iteration
    // so that the outermost MBA root claims its full subtree first.
    // PHI nodes are treated as transparent when all incoming values
    // are MBA opcodes and evaluate identically; otherwise the PHI
    // becomes a leaf.
    //
    // `opcodes` restricts which MBA opcodes produce roots (empty = all
    // supported MBA opcodes). `contexts` is an MbaTargetContext bitmask
    // selecting which additional roots to collect.
    std::vector< MBACandidate >
    DetectMbaCandidates(llvm::Function &f, uint32_t min_ast_size, uint32_t max_vars,
                        const std::set< unsigned > &opcodes = {},
                        uint32_t contexts = kMbaCtxBinaryOp);

} // namespace cobra
