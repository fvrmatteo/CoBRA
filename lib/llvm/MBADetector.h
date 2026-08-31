#pragma once

#include "cobra/core/Expr.h"
#include "cobra/core/Simplifier.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Value.h"

namespace cobra {

    // Identity of an MBA tree as the detector sees it.
    //
    // `structure` folds every node's opcode together with its operand
    // hashes, so it moves whenever the operation graph does. Constants
    // contribute their value and leaves contribute their position, which
    // keeps it stable under renaming but sensitive to a leaf being swapped.
    // `depth` and `num_vars` are carried separately because they are what
    // decides whether a better simplification has become reachable, and
    // agreeing on all three makes a collision on a differently shaped tree
    // vanishingly unlikely.
    struct MbaFingerprint
    {
        uint64_t structure = 0;
        uint32_t depth     = 0;
        uint32_t num_vars  = 0;

        bool operator==(const MbaFingerprint &other) const = default;
    };

    struct MBACandidate
    {
        llvm::Instruction *root;
        std::vector< llvm::Value * > leaf_values;
        std::vector< std::string > var_names;
        std::vector< uint64_t > sig;
        uint32_t bitwidth;
        std::unique_ptr< Expr > expr;
        Evaluator evaluator;
        MbaFingerprint fingerprint;
    };

    // Find MBA candidates across a function.  Blocks are scanned in
    // post-order (uses before defs) with reverse instruction iteration
    // so that the outermost MBA root claims its full subtree first.
    // PHI nodes are treated as transparent when all incoming values
    // are MBA opcodes and evaluate identically; otherwise the PHI
    // becomes a leaf.
    //
    // A root already carrying a fingerprint recorded under the same
    // `options_tag` is claimed but not returned: the tree has not changed
    // since it was last examined, so neither would the outcome. The tag
    // covers the settings the outcome depends on, so changing them
    // invalidates every record without needing to rewrite the IR.
    std::vector< MBACandidate > DetectMbaCandidates(
        llvm::Function &f, uint32_t min_ast_size, uint32_t max_vars, uint64_t options_tag
    );

    // Record `fp` on `inst` so a later run can skip the tree rooted there.
    void RecordMbaFingerprint(
        llvm::Instruction *inst, const MbaFingerprint &fp, uint64_t options_tag
    );

    // Fingerprint the tree rooted at `inst` the way the detector would.
    // Returns nothing when `inst` does not root a tree it would consider.
    std::optional< MbaFingerprint > ComputeMbaFingerprint(llvm::Instruction *inst);

} // namespace cobra
