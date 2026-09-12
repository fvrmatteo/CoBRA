#pragma once

#include "cobra/core/SemilinearIR.h"
#include <vector>

namespace cobra {

    std::vector< PartitionClass > ComputePartitions(const SemilinearIR &ir);

    /// The 1-bit truth table of a bitwise atom at one bit position: bit `i` of
    /// the result is the atom's output bit `bit_pos` under the Boolean
    /// assignment `i` of its support variables, with every constant reduced
    /// to its bit at that position and a right shift read from the source
    /// bit it moves down. This is the reading the partition classes and the
    /// partition solve are built on.
    uint64_t EvalAtomAtBit(
        const Expr &atom, const std::vector< GlobalVarIdx > &support, uint32_t bit_pos,
        uint32_t bitwidth
    );

} // namespace cobra
