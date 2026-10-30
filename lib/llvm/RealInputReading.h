#pragma once

// A root read over the values it really reads.
//
// The tree the search is handed ends wherever its collection stops: at the
// collection's node budget, at an operation the 64-bit expression language
// cannot spell - a 128-bit product, a population count - and at the values it
// holds aside. Each end is a variable of its own, independent of the others,
// and a root over more of them than `max_vars` is turned down. They are not
// independent: every one is computed from the same few loads, phis, calls and
// arguments, and a tree written so that its terms cancel reads fewer of those
// than it has ends - often one.
//
// This reading follows the root through every integer operation the solver's
// translation of instructions reads, at any width, down to those real inputs;
// evaluates the slice on probes to find which inputs the root depends on;
// and searches over those alone, the root evaluated as a function of them.
// The probes only propose. The caller proves the answer against the
// instructions themselves with every input free, so an input the probes
// missed - one that matters on a sliver of its values - fails the proof
// rather than the rewrite.

#include "cobra/core/Expr.h"
#include "cobra/core/Simplifier.h"
#include "cobra/llvm/CobraPass.h"

#include "llvm/IR/Instruction.h"
#include "llvm/IR/Value.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cobra {

    struct RealInputSlice
    {
        llvm::Instruction *root = nullptr;
        // The operations from the inputs up to the root, each after its
        // operands; the root is the last.
        std::vector< llvm::Instruction * > nodes;
        // What they stop at, in the order the walk met them.
        std::vector< llvm::Value * > inputs;
    };

    // An integer operation a slice is followed through: one the translation of
    // instructions to the solver reads, over integer operands.
    bool IsRealInputOperation(const llvm::Instruction &inst);

    // `root`'s slice: every operand followed through the integer operations
    // `VerifyRootOverInputs` reads, to the values that are none - loads, phis,
    // calls, arguments, constants other than integers. Nothing when the root is
    // no such operation, or the slice holds more than `max_nodes` operations or
    // `max_inputs` inputs.
    std::optional< RealInputSlice > SliceToRealInputs(
        llvm::Instruction &root, uint32_t max_nodes, uint32_t max_inputs
    );

    // The value nearest the inputs that every path from `slice`'s root to them
    // passes through: the root is a function of it alone, so a fact of the root
    // that holds for every value of it holds of the root. Null when there is no
    // such value short of the root and the inputs.
    const llvm::Value *DominatingCut(const RealInputSlice &slice);

    struct RealInputReading
    {
        // The inputs the root depends on, as indices into the slice's inputs,
        // in the order of the answer's variables. Variable `i` is input
        // `support[i]` zero-extended to `bitwidth`.
        std::vector< uint32_t > support;
        // At least the root's width and every supporting input's.
        uint32_t bitwidth = 0;
        // What each of the answer's variables is: an input of the slice, or an
        // operation of it the tree over the support still ends in - a 128-bit
        // product of a supporting input - read as a variable of the search the
        // way any collection reads an operation it stops at. Variable `i` is
        // `leaves[i]` zero-extended to `bitwidth`.
        std::vector< llvm::Value * > leaves;
        // Over `leaves`, on the root's bits. Unproved.
        std::unique_ptr< Expr > expr;
        // Why there is no answer, when there is none.
        std::string reason;
    };

    // The root of `slice` as a function of the inputs it depends on, found by
    // the search `options` configures (its families and `max_vars`; the width,
    // the evaluator and the demanded bits are the reading's own). `seed` draws
    // the probes, so one slice is always read the same way.
    //
    // The search is handed the tree the root is over the support alone: the
    // slice built again with every other input at a value it was probed at,
    // where whatever does not depend on the support is the constant it computes
    // - a 128-bit product of an input outside it as much as a sum - and
    // collected as any tree is (`options_tag`, `cost_model`). Only when that
    // tree still ends in something other than the support is the root searched
    // for on its values alone.
    RealInputReading ReadOverRealInputs(
        const RealInputSlice &slice, const Options &options, uint64_t seed, uint64_t options_tag,
        MbaCostModel cost_model
    );

} // namespace cobra
