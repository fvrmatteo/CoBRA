#pragma once

#include "cobra/core/Expr.h"
#include "cobra/core/Simplifier.h"
#include "cobra/llvm/CobraPass.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
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

    // Which bits of a root's value its readers look at, as a mask over its
    // width; all ones when unknown. What can be made of a tree depends on it -
    // a shift over a sum has a semilinear reading on the low bits only - so it
    // is as much a part of a tree's identity as its shape.
    using DemandedMaskFn = std::function< uint64_t(llvm::Instruction *) >;

    // `fp` for a root read under `mask`. A full mask leaves it as it is, so a
    // record written for a root that is read whole keeps matching.
    MbaFingerprint WithDemandedMask(MbaFingerprint fp, uint64_t mask, uint32_t bitwidth);

    // `ptradd`: a byte `getelementptr` with one index, or nothing. Its value is
    // the pointer it walks from plus that index, so the detector reads a chain
    // of them as the addition of the indices and leaves the base out; the
    // rewrite puts the base back with `ByteGepBase`, so what replaces the root
    // is a `getelementptr` again rather than a cast.
    const llvm::GetElementPtrInst *ByteGepOf(const llvm::Value *v);
    llvm::Value *ByteGepBase(const llvm::GetElementPtrInst *gep);

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

        // The instructions of the collected tree, and the ones among them that
        // `dying_count` counts. A rewrite that reads one of these in place of
        // building it again keeps it in the function, which is what it is
        // priced on (see `IrReuse`).
        std::vector< llvm::Instruction * > tree;
        std::vector< llvm::Instruction * > dying;

        // Whether poison in any instruction of the tree is poison in the root:
        // true unless the tree holds something that can stop it - a `select`,
        // whose other arm is not read, a `freeze`, or a phi the collection
        // looked through, which hands on one arm only when control came that
        // way.
        bool poison_reaches_root = false;

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

        // Collected with every arithmetic value under a bitwise reader kept
        // as a leaf (see `BoundaryCutMbaCandidate`). Like a re-cut it is a
        // partial view of the root, and `node_limit` says so too.
        bool boundary_cut = false;

        // The bits of the root its readers look at, as a mask over `bitwidth`.
        // A rewrite has to agree with the tree on these alone.
        uint64_t demanded_mask = UINT64_MAX;

        // Whether the collected tree holds a `freeze`. A rewrite reads the
        // leaves directly, so it no longer has the `freeze` between them and
        // the root, and a leaf that can be poison would reach a reader that the
        // original had made defined. Restoring that is `leaf_override`'s job at
        // reconstruction time; this is the flag that says it is owed.
        bool spans_freeze = false;
    };

    // The `node_limit` a boundary cut carries: past any re-cut ladder, so the
    // driver treats it as the partial view it is and never cuts it again.
    constexpr uint32_t kBoundaryCutLimit = UINT32_MAX;

    // The `node_limit`s the shared-value cuts carry (see
    // `SharedCutMbaCandidate`): the first rung holds `kSharedCutLimit` and each
    // later one the value below, so every rung is a partial view of its own and
    // all of them sit past any re-cut ladder, as the boundary cut does.
    constexpr uint32_t kSharedCutLimit = UINT32_MAX - 1;
    constexpr uint32_t kSharedCutRungs = 1U << 16;

    constexpr bool IsSharedCutLimit(uint32_t node_limit) {
        return node_limit <= kSharedCutLimit && node_limit > kSharedCutLimit - kSharedCutRungs;
    }

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
    //
    // `max_tree_nodes`, when non-zero, caps how many instructions a collection
    // may hold; past it the walk keeps what remains as leaves.
    //
    // `demanded`, when given, says which bits of a root its readers look at;
    // the candidate carries the answer and its fingerprint is taken under it.
    std::vector< MBACandidate > DetectMbaCandidates(
        llvm::Function &f, uint32_t min_ast_size, uint32_t max_vars, uint64_t options_tag,
        MbaCostModel cost_model = MbaCostModel::kTreeInstructions, uint32_t max_tree_nodes = 0,
        const DemandedMaskFn &demanded = {}
    );

    // Candidates for `inner_roots`, to be used once the tree enclosing them has
    // been rejected. Descending only on failure keeps the common case at one
    // candidate per tree while still reaching expressions an irreducible outer
    // node would otherwise hide.
    //
    // The roots are taken rather than the candidate they came from because
    // sub-expressions are shared: by the time an outer node fails, most of what
    // it encloses has usually been attempted under some other root, and building
    // a candidate costs a tree walk and a signature sweep whether or not anyone
    // ends up looking at it. Only the caller knows which ones are new.
    std::vector< MBACandidate > ExpandMbaCandidate(
        llvm::ArrayRef< llvm::Instruction * > inner_roots, uint32_t min_ast_size,
        uint32_t max_vars, uint64_t options_tag,
        MbaCostModel cost_model = MbaCostModel::kTreeInstructions, uint32_t max_tree_nodes = 0,
        const DemandedMaskFn &demanded = {}
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
    // first, or nothing once the cuts have caught up with the full tree or with
    // `max_recut_nodes`. One at a time is what keeps this honest: handing back
    // the whole ladder would go on solving cuts of a root a smaller cut has
    // already rewritten, whereas a chain that only advances on rejection stops
    // the moment one succeeds.
    //
    // The ladder is capped because the cuts that pay are small ones. An identity
    // is a statement about a handful of operators; the cut has to hold those and
    // leave their operands opaque, and once it is holding much more than that it
    // is only converging on the full collection that was already rejected — at a
    // candidate, a signature sweep and a solve per rung.
    std::vector< MBACandidate > RecutMbaCandidate(
        const MBACandidate &cand, uint32_t min_ast_size, uint32_t max_vars,
        uint32_t max_recut_nodes, uint32_t max_recut_vars, uint64_t options_tag,
        MbaCostModel cost_model = MbaCostModel::kTreeInstructions,
        const DemandedMaskFn &demanded = {}
    );

    // The same root as `cand`, re-collected with every arithmetic value that a
    // bitwise operator reads kept as a leaf.
    //
    // The semilinear reading of a tree is a sum of bitwise atoms over its
    // variables, and an arithmetic value under a bitwise operator can only be
    // a variable to it: `(a + b) ^ c` is no sum of atoms over `a` and `b`. The
    // exhaustive collection expands `a + b` anyway and hands the solver a
    // tree it can only reject, and the breadth-first re-cuts cannot get the
    // boundary right either - the sum sits deeper than the bitwise operators
    // that read it on one path and shallower than those on another. This is
    // the cut at that boundary, tried once the full tree has been rejected.
    //
    // Returns nothing for a candidate that is itself a cut, or when the cut
    // takes nothing out of the full collection.
    std::vector< MBACandidate > BoundaryCutMbaCandidate(
        const MBACandidate &cand, uint32_t min_ast_size, uint32_t max_vars,
        uint64_t options_tag, MbaCostModel cost_model = MbaCostModel::kTreeInstructions,
        uint32_t max_tree_nodes = 0, const DemandedMaskFn &demanded = {}
    );

    // The same root as `cand`, re-collected with the values its tree reads more
    // than once kept as leaves.
    //
    // An identity reads its operands several times - that is what makes it an
    // identity and not a computation - so an operand that is an expression in
    // its own right is a value the tree holds once and reads from several
    // places. `((A & B) ^ B) + (A & B)` is `B` whatever `B` is computed from,
    // and with `B` a variable the solver sees that at once and the rewrite is
    // the value the function already holds: nothing is rebuilt, and nothing
    // that computes `B` is priced, because none of it is in the tree. Expand
    // `B` and the identity is gone: a product under the bitwise operators is no
    // sum of atoms over its own operands, and an answer in those operands
    // would have to compute `B` a second time beside the first.
    //
    // Neither of the other cuts finds that reading. A breadth-first prefix
    // expands the nodes nearest the root, and a shared value read from near
    // the root is expanded long before the operators that read it from
    // further down; the boundary cut keeps a value opaque only where a bitwise
    // operator reads it, so the same value is expanded through the `add` that
    // reads it beside them.
    //
    // Which shared values are the identity's operands and which are only its
    // intermediate results - `A & B` above is read twice as well - is not
    // something the tree says, so the cuts form a ladder of their own. The
    // first keeps every shared value opaque, and each rejection expands one
    // more, nearest the root first: an intermediate result is read by the root
    // end of the identity and its operands by the far end. One rung at a time,
    // for the reason `RecutMbaCandidate` gives, and at most `max_shared_cuts`.
    //
    // Returns nothing for a re-cut or a boundary cut, for a tree that reads
    // nothing twice, and once the rungs have caught up with the full tree.
    std::vector< MBACandidate > SharedCutMbaCandidate(
        const MBACandidate &cand, uint32_t min_ast_size, uint32_t max_vars,
        uint32_t max_shared_cuts, uint64_t options_tag,
        MbaCostModel cost_model = MbaCostModel::kTreeInstructions,
        uint32_t max_tree_nodes = 0, const DemandedMaskFn &demanded = {}
    );

    // Record `fp` on `inst` so a later run can skip the tree rooted there.
    void RecordMbaFingerprint(
        llvm::Instruction *inst, const MbaFingerprint &fp, uint64_t options_tag
    );

    // Fingerprint the tree rooted at `inst` the way the detector would.
    // Returns nothing when `inst` does not root a tree it would consider.
    std::optional< MbaFingerprint >
    ComputeMbaFingerprint(llvm::Instruction *inst, uint32_t max_tree_nodes = 0);

} // namespace cobra
