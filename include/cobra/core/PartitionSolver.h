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

    /// Solve a semilinear sum as one linear MBA over translated variables.
    ///
    /// Solved class by class, a sum whose constants move from bit to bit is
    /// assembled from as many masked answers as it has classes, even when
    /// every class is the same function: `(a ^ c1) & (b ^ c2)` is `a & b` on
    /// one class, `~a & b` on the next, `a & ~b` on another, and the
    /// assembly spells all of them out. What those classes share is one
    /// bit-slice function read through a different map of each variable -
    /// kept, complemented, cleared or set. When there is such a slice `g`,
    /// with `sigma_j` the maps class of bit `j` reads it through, bit `j` of
    /// the sum is `2^j * g(sigma_j(v_j))`, so the sum is the linear MBA with
    /// slice `g` applied to `(v & keep) ^ flip` for every variable, where
    /// `keep` has the bits on which the variable is read and `flip` those on
    /// which it is complemented or set. A masked operand `b & m` is the same
    /// thing, cleared above `m`.
    ///
    /// Classes are matched up to an additive constant per class, since the
    /// sum's constant is one word in which the constants lowered out of the
    /// atoms have already carried into each other; the reference slice's own
    /// constant is then the one that makes the classes add up to it. Returns the
    /// translated linear form as an IR whose atoms are registered as they
    /// stand, or nothing when the sum has one class, reads more than four
    /// variables, or its classes share no slice.
    std::optional< SemilinearIR > SolvePartitionsByTranslation(const SemilinearIR &ir);

} // namespace cobra
