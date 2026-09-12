#pragma once

#include "cobra/core/SemilinearIR.h"
#include <optional>

namespace cobra {

    /// Solve a semilinear sum one bit class at a time (MSiMBA, Section 5).
    ///
    /// The bit partitioner groups the bit positions on which every atom reads
    /// the same Boolean function of the variables. Restricted to one such
    /// class the whole sum is an ordinary linear MBA - the constants inside
    /// the atoms have collapsed to 0 or 1 - so its signature over the
    /// variables determines it, and the linear machinery (coefficient
    /// interpolation plus the change-of-basis builder) finds its cheapest
    /// form. Masking each class's answer back to the bits it owns and adding
    /// the classes gives an expression equal to the input on every bit,
    /// which is then normalized into a fresh IR for the structure-recovery
    /// passes to merge across classes.
    ///
    /// This is what lets a variable hidden under a constant `xor` or `and`
    /// behave as a unit inside a multi-variable identity: the term-level
    /// rewrites only ever compare atoms that share a basis, so
    /// `((y ^ c) & ~x) + ((y ^ c) & x)` had nothing to say to them, whereas
    /// per class it is `(~y & ~x) + (~y & x)` and `(y & ~x) + (y & x)`.
    ///
    /// Returns the re-normalized IR, or nothing when some atom has no
    /// bit-local reading (a shift, or a support too wide for a truth table)
    /// or when the sum reads more variables than a signature can hold.
    std::optional< SemilinearIR > SolvePartitionsLinearly(const SemilinearIR &ir);

} // namespace cobra
