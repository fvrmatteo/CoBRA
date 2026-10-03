#pragma once

#include "MBADetector.h"
#include "cobra/core/Expr.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Dominators.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Value.h"

#include <cstdint>
#include <vector>

namespace cobra {

    // What a reconstruction may read in place of building it again.
    //
    // A rewrite is an expression over the candidate's leaves, and the function
    // often holds parts of it already: the tree being replaced computed them,
    // or a neighbouring expression did. Building them a second time makes the
    // rewrite look as large as everything it spells out, when what it adds to
    // the function is only what is not there yet. With this in hand
    // `ReconstructIr` looks, for every operator it is about to emit, for an
    // instruction that already applies that operator to those very operands,
    // and reads it instead - one it emitted itself a moment ago included, so
    // an expression that repeats a sub-expression builds it once.
    //
    // Reading an instruction is not free just because it exists. One that was
    // going to disappear with the old root stays because of the rewrite, so the
    // caller prices `reused` as the cost model demands; all this does is report
    // what was read.
    struct IrReuse
    {
        // The instruction the rewrite is built in front of, and the dominator
        // tree of its function: only a value that dominates it can be read.
        llvm::Instruction *before             = nullptr;
        const llvm::DominatorTree *dominators = nullptr;

        // The instructions whose poison the root being replaced already
        // carries, if any are known. An instruction with a poison-generating
        // flag computes the operator the rewrite wants only where the flag
        // holds, and poison everywhere else; reading it is a refinement only
        // when the value replaced was poison there too. Any other flagged
        // instruction is left alone and the operator is built without flags.
        const llvm::SmallPtrSetImpl< const llvm::Instruction * > *poisons_root = nullptr;

        // Roots an earlier rewrite of the same run replaced. Each stays in the
        // function, unread, until the run ends, still computing the old tree
        // with its old flags, so it is never read back: its readers now read
        // the replacement, and it is not the value they see.
        const llvm::DenseSet< const llvm::Value * > *replaced = nullptr;

        // Filled in: the instructions read that the reconstruction did not
        // emit itself, each once, and the ones it did emit.
        llvm::SmallVector< llvm::Instruction *, 8 > reused;
        llvm::SmallPtrSet< const llvm::Instruction *, 16 > created;
        llvm::SmallPtrSet< const llvm::Instruction *, 8 > seen;

        // How many times a lookup found something to read - one instruction
        // read from three places counts three - which is how many instructions
        // the reconstruction would have emitted on top of the ones it did.
        unsigned found = 0;
    };

    // The leaves a rewrite reads, and which of them it has to freeze first.
    //
    // A tree collected through a `freeze` or a `select` can be defined where a
    // leaf is poison: the `freeze` makes it so, and a `select` does not read the
    // arm it does not take. The rewrite reads every leaf it names directly, so
    // such a leaf has to be frozen - but only once, and only if the rewrite
    // reads it at all: a freeze nothing reads is an instruction the rewrite is
    // charged for and gains nothing from.
    struct LeafSupply
    {
        std::vector< llvm::Value * > values;
        std::vector< bool > freeze_on_use;
    };

    // Convert a simplified Expr tree back to LLVM IR instructions.
    // var_map remaps simplified variable indices to original leaf indices
    // (needed when aux variable elimination reduced the variable set).
    // If empty, identity mapping is assumed.
    //
    // `leaves`, when given, stands in for `candidate.leaf_values` one for one,
    // freezing a leaf the first time the rewrite reads it when it is marked so
    // (see `LeafSupply`), without disturbing the candidate, which the driver
    // still needs intact if the rewrite goes on to be rejected.
    //
    // `reuse`, when given, lets the reconstruction read instructions the
    // function already holds (see `IrReuse`).
    llvm::Value *ReconstructIr(
        const Expr &expr, const MBACandidate &candidate, llvm::IRBuilder<> &builder,
        const std::vector< uint32_t > &var_map                 = {},
        LeafSupply *leaves                                     = nullptr,
        IrReuse *reuse                                         = nullptr
    );

} // namespace cobra
