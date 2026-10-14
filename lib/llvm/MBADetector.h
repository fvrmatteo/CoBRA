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

#include "llvm/ADT/SmallVector.h"
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

    // What LLVM's value tracking knows of a leaf's bits: the ones always zero
    // and the ones always one, as masks over the tree width. A leaf is whatever
    // the collection stopped at - a phi, a load, a value a cut kept opaque -
    // and the tree reads it as a free variable; but a phi of a constant and a
    // masked value has no bit outside the mask, and an identity written across
    // that fact is invisible to a solver that is not told it: `(v ^ C) + v` is
    // `C` exactly when `v` has no bit outside `C`. So every reading of the tree
    // - the expression, the probe plan and the signature it yields, the
    // fingerprint - masks the leaf by what is known, a rewrite is found, probed
    // and proved under the same facts the tree was read under, and the facts
    // are part of the tree's identity.
    struct LeafBits
    {
        uint64_t zero = 0;
        uint64_t one  = 0;
        bool Informative() const { return zero != 0 || one != 0; }
        bool operator==(const LeafBits &other) const = default;
    };

    // How a root is recovered from the low bits of it a candidate reads, when
    // the candidate reads fewer bits than the root's type holds (see
    // `SliceMbaCandidate`).
    enum class SliceExtension : uint8_t {
        // The candidate reads the root at its own width, which is every
        // candidate but a slice.
        kNone,
        // The root is the zero extension of the bits read: every bit above
        // them is zero.
        kZero,
        // The root is their sign extension: every bit above them is a copy of
        // the topmost bit read.
        kSign,
    };

    // The bits of a candidate's own reading a rewrite of it has to agree on,
    // from `root_demand`, the bits of the root its readers look at (over the
    // root's type). A candidate that reads the root whole needs no more than
    // those. Two readings need more:
    //
    // - A slice reads the low `width` bits and puts the rest back as their
    //   extension, so under `SliceExtension::kSign` every bit above is a copy
    //   of bit `width - 1`: a reader of any of them reads that bit.
    // - A sum group is added back to the rest of the root's sum, and a
    //   difference in a bit nobody reads carries into every bit above it: the
    //   group has to agree on every bit up to the highest one read.
    uint64_t
    CandidateDemand(uint64_t root_demand, uint32_t width, SliceExtension extension, bool sum_group);

    struct MBACandidate
    {
        llvm::Instruction *root;
        std::vector< llvm::Value * > leaf_values;
        // One entry per leaf; what the reconstruction may drop a mask against.
        std::vector< LeafBits > leaf_bits;
        // One entry per leaf: the leaf's own integer width when that is
        // narrower than the tree, and zero when the leaf is as wide as the tree
        // reads it (`NarrowWidth`).
        //
        // Kept beside `leaf_bits` rather than read off `leaf_values` because a
        // worker thread is handed the candidate without its IR, and the leaf's
        // width is part of how the tree reads the leaf - so a reading that needs
        // it has to find it here or answer differently on a worker than on the
        // loop, which is how a leaf identity came to depend on `COBRA_THREADS`.
        std::vector< uint32_t > leaf_widths;
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

        // Whether the collected tree holds a `select`, which reads only the arm
        // it takes: a leaf under the other one may be poison while the root is
        // not. The rewrite owes those leaves a freeze just as for `spans_freeze`.
        bool spans_select = false;

        // Whether the collected tree holds a comparison - an `icmp`, or the
        // `select` that reads one. The expression says so too, but a tree past
        // the expression budget has none, and a worker thread is handed no IR:
        // the fact is what tells both that only a proof may accept a rewrite.
        bool holds_comparison = false;

        // For a slice: how the root is recovered from the `bitwidth` bits this
        // candidate reads of it, proved before the candidate was built. A
        // rewrite replaces the root with that extension of what the narrow bits
        // were rewritten into, so the replacement has the root's own type.
        SliceExtension slice_extension = SliceExtension::kNone;

        // For a candidate that reads one group of the root's sum: the root's
        // other summands, each with the sign it stands at in that sum (see
        // `SumGroupSliceMbaCandidates`). The collection read them as zero, so
        // the candidate is the group alone; the rewrite adds them back, which
        // is what makes the root's value again.
        std::vector< std::pair< llvm::Value *, bool > > sum_residual;
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

    // The `node_limit` a slice carries: its width below the shared-cut rungs,
    // so that a slice is told apart from the root read whole and from every
    // other cut of it - `attempted` is keyed on the pair - and so that the
    // driver treats it as the partial view of the root it is, and records no
    // verdict for the whole tree from it.
    constexpr uint32_t kSliceCutLimit = kSharedCutLimit - kSharedCutRungs;
    constexpr uint32_t kMaxSliceWidth = 64;

    constexpr uint32_t SliceNodeLimit(uint32_t width) { return kSliceCutLimit - width; }

    constexpr bool IsSliceCutLimit(uint32_t node_limit) {
        return node_limit < kSliceCutLimit && node_limit >= kSliceCutLimit - kMaxSliceWidth;
    }

    // The `node_limit`s a sum group carries: a band of its own below the
    // slices, holding one rung per (group, width) pair, so that the groups of
    // one root are told apart from each other and from every other reading of
    // it - `attempted` is keyed on the pair - and so that the driver treats a
    // group as the partial view of the root it is. Width zero is the group read
    // at the root's own width, which is what the extension claim is proved
    // over.
    constexpr uint32_t kMaxSumGroups  = 8;
    constexpr uint32_t kSumGroupRungs = kMaxSumGroups * (kMaxSliceWidth + 1);
    constexpr uint32_t kSumGroupLimit = kSliceCutLimit - kMaxSliceWidth - 1;

    constexpr uint32_t SumGroupNodeLimit(uint32_t index, uint32_t width) {
        return kSumGroupLimit - (index * (kMaxSliceWidth + 1) + width);
    }

    constexpr bool IsSumGroupLimit(uint32_t node_limit) {
        return node_limit <= kSumGroupLimit && node_limit > kSumGroupLimit - kSumGroupRungs;
    }

    // Whether `node_limit` names a sum group read at the root's own width,
    // rather than a slice of one.
    // The `node_limit` the boolean cut carries: below the sum groups' band,
    // so the driver tells it apart from every other reading of the root and
    // treats it as the partial view it is.
    constexpr uint32_t kBooleanCutLimit = kSumGroupLimit - kSumGroupRungs;

    constexpr bool IsWholeSumGroupLimit(uint32_t node_limit) {
        return IsSumGroupLimit(node_limit)
            && (kSumGroupLimit - node_limit) % (kMaxSliceWidth + 1) == 0;
    }

    // Find MBA candidates across a function.  Blocks are scanned in
    // post-order (uses before defs) with reverse instruction iteration
    // so that the outermost MBA root claims its full subtree first.
    // PHI nodes are treated as transparent when every incoming value is an
    // instruction the walk reads and the same computation as the one followed,
    // and every leaf the followed arm reads is available at the root;
    // otherwise the PHI becomes a leaf.
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
    // The same root as `cand`, collected again with the comparisons of values
    // narrower than the tree taken into it (`CobraPassOptions::boolean_variables`).
    //
    // An equality or unsigned comparison of narrow values is exact on the
    // zero-extended values the tree holds, but the ordinary collection keeps
    // it a leaf: searched as a word, a comparison is opaque anyway. Read with
    // the conditions as variables it is not - its operands are what the
    // solver needs to see that it is the same condition as the sign of a
    // value computed from them, which as a leaf it cannot. It is the one
    // candidate the boolean reading searches, so it is built for a full
    // collection whether or not it takes a comparison in; empty when `cand` is
    // not a full collection.
    std::vector< MBACandidate > BooleanCutMbaCandidate(
        const MBACandidate &cand, uint32_t min_ast_size, uint32_t max_vars,
        uint64_t options_tag, MbaCostModel cost_model = MbaCostModel::kTreeInstructions,
        uint32_t max_tree_nodes = 0, const DemandedMaskFn &demanded = {}
    );

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

    // The widths at which the tree of `cand` could be read as a slice of its
    // root: the source widths of the extensions it holds, narrowest first.
    //
    // An extension is where an obfuscator's expression crosses widths, and a
    // crossing is what neither LLVM's folds nor the semilinear synthesis can
    // see through: both work inside one width, so the two sides of
    // `zext(x ^ m) ^ sext(m)` - which is `sext x` whenever `m` is `x`'s sign
    // mask - never present a common root at 64 bits, and the expression stands.
    // Read at 32 bits the same tree is `(m ^ x) ^ m`, which is `x` by
    // cancellation, and the 64-bit value is the sign extension of that.
    //
    // So the widths worth re-reading a root at are the ones its own extensions
    // name. Nothing here claims the root *is* an extension of its low bits;
    // that is a separate question, answered by a proof before a slice is built
    // (`SliceMbaCandidate`).
    llvm::SmallVector< uint32_t, 4 > SliceWidthsOf(const MBACandidate &cand);

    // The same root as `cand`, collected at `width` bits instead of the width
    // of its type, for a root the caller has *proved* to be the `extension` of
    // its low `width` bits.
    //
    // Reading a wide value at a narrower width is something the collection
    // already does wherever a reader looks at nothing else - a truncation, or
    // another node admitted the same way - because the low bits of a ring
    // operation are a function of its operands' low bits
    // (`WideNodeFitsTreeWidth`). What is new here is reading the *root* that
    // way: the bits above the slice are not dropped but recovered, by the
    // extension the caller proved, so the rewrite is the narrow answer extended
    // and the root's readers see the value they saw before.
    //
    // The two halves are proved separately and compose: the caller's proof says
    // the root is the extension of its low `width` bits, and the candidate's own
    // verification - which runs at `width`, like any other candidate's - says
    // the rewrite is those bits.
    //
    // Returns nothing for a candidate that is itself a cut, for a `ptradd`
    // root, for a width that is not narrower than the root, and when the root
    // has no reading at that width (the collection would make the root itself a
    // leaf, and the candidate would be the identity).
    std::vector< MBACandidate > SliceMbaCandidate(
        const MBACandidate &cand, uint32_t width, SliceExtension extension,
        uint32_t min_ast_size, uint32_t max_vars, uint64_t options_tag,
        MbaCostModel cost_model = MbaCostModel::kTreeInstructions, uint32_t max_tree_nodes = 0,
        const DemandedMaskFn &demanded = {}
    );

    // The groups of the root's sum that are worth reading on their own, each
    // already read at the width the caller's claim about it names.
    //
    // The slice reading above is the one thing that gets through an identity
    // written across a width boundary, and it is asked about the root: is the
    // root the extension of its own low bits? A base added into the sum is
    // enough to make the answer no. `base + 136 + sext(e)`, an address computed
    // off a base, is written by the obfuscator as
    // `base + 136 + u - 2*(u & n) - z` over `u = zext(e ^ trunc n)`,
    // `n = -(e >> 31)` and `z = e >> 31`; the part standing on `e` is a sign
    // extension and reads as `e` at 32 bits, but the whole sum is no extension
    // of anything - its high bits are the base's - and the slice is refused on
    // the root. The expression then stands as written however many of its own
    // identities would fold, and every pointer built on it reads as an
    // unrelated value.
    //
    // So the sum is split before the question is asked. The root's summands are
    // read off the `add`/`sub`/disjoint-`or` chain at the top of its tree, each
    // one is given the set of the candidate's leaves its cone reaches, and the
    // summands are grouped so that no two groups share a leaf. A group is then
    // collected as a candidate of its own: the same root, with the other
    // summands read as zero, which is exactly the group's sum because a summand
    // stands in the chain once and zero is the identity of every operation
    // above it. What the rewrite replaces the root with is that group's answer
    // plus the summands left out, read back from the IR they are already
    // computed in - so a group that folds to one instruction costs one more.
    //
    // Grouping by leaf support is what makes the zeroing exact. A summand whose
    // cone reaches a leaf of another group is in that group, so no value read as
    // zero can be reached from inside the group that is kept - and a value that
    // appeared in both would have had its term silently dropped. Constants have
    // no support and always join the residual: `136 + sext(e)` is not the
    // extension of its own low bits either, and leaving the constant out costs
    // nothing, since the rewrite adds it back as an operand.
    //
    // `is_extension` is asked of each group, read at the root's own width, and
    // answers at what width and as which extension that group may be read -
    // the same question `SliceToRead` answers about a whole root, over the
    // group's own expression. A group it declines is dropped; a group it
    // accepts is returned collected at that width, carrying both the extension
    // and the residual.
    //
    // Returns nothing for a cut, for a `ptradd` root, for a root whose sum does
    // not split, and for a split with more than `kMaxSumGroups` groups.
    using SumGroupExtensionFn = std::function<
        std::optional< std::pair< uint32_t, SliceExtension > >(const MBACandidate &) >;

    std::vector< MBACandidate > SumGroupSliceMbaCandidates(
        const MBACandidate &cand, uint32_t min_ast_size, uint32_t max_vars,
        uint64_t options_tag, const SumGroupExtensionFn &is_extension,
        MbaCostModel cost_model = MbaCostModel::kTreeInstructions, uint32_t max_tree_nodes = 0,
        const DemandedMaskFn &demanded = {}
    );

    // The leaf widths of `leaves` as a tree of width `bw` reads them: the
    // leaf's own width where that is narrower, zero where it is not.
    std::vector< uint32_t >
    LeafWidthsOf(const std::vector< llvm::Value * > &leaves, uint32_t bw);

    // The expression the tree reads leaf `index` as: the variable, masked to
    // the leaf's own width where that is narrower than the tree, and then by
    // what is known of its bits (`LeafBits`).
    //
    // Every reading of a tree agrees on this - `BuildExprFromIR` builds the
    // leaf this way inside the expression, and the probe plan hands the
    // evaluator the same masked value (`MaskToWidth`, `ApplyLeafBits`) - so a
    // candidate phrased over a leaf has to be phrased this way too. A bare
    // `Expr::Variable` is a *different function* wherever the leaf is narrower
    // than the tree, and a check that probes the one against the other fails on
    // an identity that holds: the probe hands the leaf a full-width value, the
    // tree masks it and the bare variable does not.
    //
    // Returns nothing for an index the candidate has no leaf for.
    std::unique_ptr< Expr > LeafExpr(const MBACandidate &cand, uint32_t index);

    // Record `fp` on `inst` so a later run can skip the tree rooted there.
    void RecordMbaFingerprint(
        llvm::Instruction *inst, const MbaFingerprint &fp, uint64_t options_tag
    );

    // Fingerprint the tree rooted at `inst` the way the detector would.
    // Returns nothing when `inst` does not root a tree it would consider.
    std::optional< MbaFingerprint >
    ComputeMbaFingerprint(llvm::Instruction *inst, uint32_t max_tree_nodes = 0);

    // How often a collection looked through a phi, and on what grounds: the
    // sets of transparent phis checked, those whose other arms the random
    // probes accepted, and those whose other arms are the same computation as
    // the arm followed (`COBRA_PHI_ARMS` picks which of the two decides).
    struct PhiArmCounters
    {
        uint64_t checked        = 0;
        uint64_t probes_accept  = 0;
        uint64_t exact_accept   = 0;
        uint64_t probes_only    = 0;
        // Arms that agree, but the followed one reads a leaf the root does not
        // see, so the phi was read as a leaf.
        uint64_t leaf_unavailable = 0;
    };
    PhiArmCounters PhiArmStatistics();

} // namespace cobra
