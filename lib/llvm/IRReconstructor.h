#pragma once

#include "MBADetector.h"
#include "cobra/core/Expr.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Value.h"

#include <cstdint>
#include <vector>

namespace cobra {

    // Convert a simplified Expr tree back to LLVM IR instructions.
    // var_map remaps simplified variable indices to original leaf indices
    // (needed when aux variable elimination reduced the variable set).
    // If empty, identity mapping is assumed.
    //
    // `leaf_override`, when given, stands in for `candidate.leaf_values` one for
    // one. It is how a caller hands the rewrite values it has already prepared -
    // frozen leaves for a tree that was collected through a `freeze` - without
    // disturbing the candidate, which the driver still needs intact if the
    // rewrite goes on to be rejected.
    llvm::Value *ReconstructIr(
        const Expr &expr, const MBACandidate &candidate, llvm::IRBuilder<> &builder,
        const std::vector< uint32_t > &var_map                 = {},
        llvm::ArrayRef< llvm::Value * > leaf_override          = {}
    );

} // namespace cobra
