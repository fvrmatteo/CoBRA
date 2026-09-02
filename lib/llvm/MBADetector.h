#pragma once

#include "cobra/core/Expr.h"
#include "cobra/core/Simplifier.h"
#include "cobra/llvm/CobraPass.h"
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

        // The MBA roots this tree covers, root excluded. A tree is claimed by
        // its outermost root, which is the right default: the bigger expression
        // is the one worth simplifying. But an irreducible outer node would
        // otherwise bury every reducible expression underneath it, so when a
        // candidate fails these are handed back for a second look.
        std::vector< llvm::Instruction * > inner_roots;

        // How many instructions replacing the root would actually remove. The
        // Expr the candidate carries is a tree, so a value the IR computes once
        // and reads several times appears in it once per reader; costing a
        // rewrite against that inflated shape is how a chain of shared adds ends
        // up "simplified" into a wider polynomial. This is the count to beat.
        uint32_t dying_count = 0;

        // How many instructions the tree was allowed to hold, or zero when it
        // was collected to exhaustion. Only a re-cut carries a limit, and it is
        // what tells the two apart: a limited tree is a deliberately partial
        // view of the root, so it must not claim to be the record of what the
        // root looks like.
        uint32_t node_limit = 0;

        // Instructions in the collected tree, sharing and all. This is the IR
        // that was read to build `expr`, so it is what a rewrite is measured
        // against under `MbaCostModel::kTreeInstructions`.
        uint32_t tree_size = 0;
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
        llvm::Function &f, uint32_t min_ast_size, uint32_t max_vars, uint64_t options_tag,
        MbaCostModel cost_model = MbaCostModel::kDyingInstructions
    );

    // Candidates for the roots inside `cand`, to be used once `cand` itself has
    // been rejected. Descending only on failure keeps the common case at one
    // candidate per tree while still reaching expressions an irreducible outer
    // node would otherwise hide.
    std::vector< MBACandidate > ExpandMbaCandidate(
        const MBACandidate &cand, uint32_t min_ast_size, uint64_t options_tag,
        MbaCostModel cost_model = MbaCostModel::kDyingInstructions
    );

    // The same root as `cand`, re-collected with the tree cut short so that
    // whole sub-expressions stay opaque and become variables in their own right.
    //
    // Collecting to exhaustion is what makes the largest expression available to
    // the solver, but it also dissolves the structure an identity is written in.
    // `(A & B) + ((A & B) ^ A)` is `A` for any A and B, and the solver finds that
    // immediately when A and B are variables; expand A and B into the arithmetic
    // that produced them and the candidate becomes a wide expression whose
    // signature says nothing — worse still if they share an origin, since it
    // then collapses to a single variable. The identity did not stop holding, it
    // stopped being visible.
    //
    // The cuts are the connected prefixes of the tree in breadth-first order,
    // which is finer than cutting at a fixed depth: an identity is not obliged
    // to have its operands at matching heights, and a depth cut cannot expand
    // one of them while leaving its sibling whole.
    //
    // Returns the next viable cut above the one `cand` already used, smallest
    // first, or nothing once the cuts have caught up with the full tree. One at
    // a time is what keeps this honest: handing back the whole ladder would go
    // on solving cuts of a root a smaller cut has already rewritten, whereas a
    // chain that only advances on rejection stops the moment one succeeds.
    std::vector< MBACandidate > RecutMbaCandidate(
        const MBACandidate &cand, uint32_t min_ast_size, uint64_t options_tag,
        MbaCostModel cost_model = MbaCostModel::kDyingInstructions
    );

    // Record `fp` on `inst` so a later run can skip the tree rooted there.
    void RecordMbaFingerprint(
        llvm::Instruction *inst, const MbaFingerprint &fp, uint64_t options_tag
    );

    // Fingerprint the tree rooted at `inst` the way the detector would.
    // Returns nothing when `inst` does not root a tree it would consider.
    std::optional< MbaFingerprint > ComputeMbaFingerprint(llvm::Instruction *inst);

} // namespace cobra
