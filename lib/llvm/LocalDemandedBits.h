#pragma once

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/KnownBits.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace llvm {
    class AssumptionCache;
    class DominatorTree;
    class Instruction;
    class Use;
} // namespace llvm

namespace cobra {

    // What LLVM's `DemandedBits` says about an instruction, computed from the
    // readers that decide it rather than over the whole function.
    //
    // The pass asks which bits of a root its readers look at, and every rewrite
    // it makes changes the answer somewhere, so the whole-function analysis is
    // thrown away after each one. Rebuilding it for the next question costs a
    // walk over every instruction in the function and a known-bits query per
    // `and`, `or`, `add` and `sub` in it - measured over a VM exploration, ten
    // thousand rebuilds and more than a quarter of the pass.
    //
    // The answer for an instruction only ever depends on its readers: the
    // analysis starts from the instructions that are live whatever they
    // compute (side effects, terminators) and propagates the bits they read
    // back to their operands. So an instruction's answer is its readers'
    // contributions put together, each reader's contribution needs only that
    // reader's own answer, and the known-bits queries the contributions make
    // look at operands, which none of this changes. Two things keep the walk
    // short. A value read in full by one reader - a store, an address - is read
    // in full whatever the others do, so its remaining readers are not asked.
    // And a reader that is not an integer passes every bit of an integer
    // operand on as soon as it is live at all, so of such a reader only that
    // is asked, which stops the walk at the first address it meets instead of
    // following the loads behind it.
    //
    // Readers can form a cycle through a loop's phis, where an answer is the
    // least fixpoint rather than a sum of settled parts. The walk gives up
    // there, and the fixpoint of `DemandedBits::performAnalysis` is run over
    // the instructions the value reaches, with everything already settled held
    // fixed. When that region is too large, `Get` declines and the caller asks
    // the whole-function analysis instead.
    class LocalDemandedBits
    {
      public:
        LocalDemandedBits(
            llvm::AssumptionCache &assumptions, llvm::DominatorTree &dominators,
            size_t max_region
        )
            : assumptions_(assumptions), dominators_(dominators), max_region_(max_region) {}

        // `DemandedBits::getDemandedBits(inst)` over the function as it is
        // now, or nothing when the question can only be settled over a region
        // of more than `max_region` instructions.
        std::optional< llvm::APInt > Get(llvm::Instruction *inst);

        // The IR changed: every answer given so far may be stale.
        void Invalidate() {
            settled_.clear();
            known_.clear();
            work_this_epoch = 0;
        }

        // How many instructions were settled, and how many of those by a
        // fixpoint over a region; regions declined for their size. The work
        // counter is the instructions visited since the last `Invalidate`.
        uint64_t settled         = 0;
        uint64_t region_nodes    = 0;
        uint64_t regions         = 0;
        uint64_t declined        = 0;
        uint64_t work_this_epoch = 0;

      private:
        // An instruction whose answer is known: whether the analysis reaches
        // it at all (an instruction nothing live reads is dead, and dead
        // instructions are answered with every bit), and for an integer the
        // bits its readers look at.
        struct Settled
        {
            llvm::APInt alive;
            bool processed = false;
        };

        struct KnownOperands
        {
            llvm::KnownBits first;
            llvm::KnownBits second;
            bool computed = false;
        };

        // Settles `inst` and whatever it depends on; false when a cycle among
        // the readers was met, and nothing about the cycle has been settled.
        bool Walk(llvm::Instruction *inst);

        // `DemandedBits::performAnalysis` over the instructions `inst` reaches
        // that are not settled yet; false when there are more than
        // `max_region_` of them.
        bool Fixpoint(llvm::Instruction *inst);

        // What `use` contributes to the bits of the value it reads, given that
        // its reader is settled.
        llvm::APInt Contribution(llvm::Use &use, const Settled &reader);

        llvm::APInt Answer(llvm::Instruction *inst) const;

        llvm::AssumptionCache &assumptions_;
        llvm::DominatorTree &dominators_;
        size_t max_region_;
        llvm::DenseMap< llvm::Instruction *, Settled > settled_;
        llvm::DenseMap< llvm::Instruction *, KnownOperands > known_;
    };

} // namespace cobra
