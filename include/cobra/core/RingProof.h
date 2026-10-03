#pragma once

#include "cobra/core/Expr.h"

#include <cstddef>
#include <cstdint>

namespace cobra {

    // How much work `ProveEqualByRingNormalForm` may do before it gives up.
    struct RingProofLimits
    {
        // Distinct comparisons the proof splits on: 2^this cases at most.
        uint32_t max_comparisons = 6;
        // Monomials a normal form may hold. A product of sums can grow past any
        // useful size; past this the question goes back to the caller.
        size_t max_terms = 4096;
        // Expression nodes normalised over all cases.
        size_t max_nodes = size_t{ 1 } << 20;
    };

    // Proves `lhs == rhs` for every input, at `bitwidth`, without a solver.
    //
    // Each side is put in a normal form: a polynomial over Z/2^bitwidth whose
    // indeterminates are *atoms* - the variables, and every bitwise node or
    // shift with its operands themselves in normal form. Equal normal forms are
    // equal functions, because every step is an identity that holds for all
    // inputs: the ring laws, `~a == -1 - a`, `a ^ c == ~(a ^ ~c)`, the
    // associativity, commutativity and cancellation of `^`, the absorbing and
    // neutral constants of `&`, `|`, `^`, and constant folding.
    //
    // A comparison is a 0/1 value the ring cannot see into, so the proof splits
    // on it: each distinct comparison is replaced by 0 and by 1, independently,
    // and the two sides must agree in every case. That covers the values the
    // comparisons really take and more, so agreement in every case is proof.
    //
    // This is what a bit-blasting solver is worst at. A product by an odd
    // constant behind a sign-conditional negation - `(c ? -y : y) * K` with the
    // `-y * -K == y * K` it relies on - is a multiplier equivalence to a SAT
    // solver and a one-line identity here.
    //
    // True only when proved. False means "not decided": the two sides may
    // still be equal (two different normal forms can denote the same
    // function), and the caller asks a solver.
    bool ProveEqualByRingNormalForm(
        const Expr &lhs, const Expr &rhs, uint32_t bitwidth, const RingProofLimits &limits = {}
    );

} // namespace cobra
