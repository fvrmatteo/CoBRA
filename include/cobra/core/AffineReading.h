#pragma once

// A root that is an affine function of its leaves, xored with a constant.
//
// An obfuscator spells `a + 0x10000 * b + K` - a sum of leaves with constant
// coefficients - through halvings, carries picked by comparisons, words taken
// apart and put back with shifts and disjoint ors, and constants xored in and
// out along the way; and very often the whole is xored with one more constant,
// `(a + 0x10000 * b + K) ^ C`. The semilinear reading holds each piece that
// carries or shifts as an atom of its own, and atoms as independent, so the
// sum the pieces add up to is invisible to it - while evaluated on the leaves
// the root is that sum on every input.
//
// This reading evaluates it on the leaves. The root `f` is asked whether, for a
// constant `C`, `f ^ C` is `K + sum(a_i * L_i) + sum(c_i * s_i)` on the bits its
// readers look at: `L_i` leaf `i` as the tree reads it - masked to its own width
// and by what is known of its bits - and `s_i` the top bit of a leaf narrower
// than the tree, which is what a sign extension adds to the zero extension the
// tree holds. `C` is read off one bit at a time from the bottom: bit `b` of
// `f ^ C` is affine modulo `2^(b+1)` for one choice of bit `b` of `C` and not
// the other, unless the bit is constant on every input, where either choice is
// the same function and the cheaper one is kept. The coefficients come from
// probes that move one leaf by its lowest free bit (and a narrow leaf by its
// top one), and every other probe has to agree. The probes only propose: the
// caller proves the answer.

#include "cobra/core/Expr.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace cobra {

    // `expr`, over `leaves.size()` variables at `bitwidth`, as an affine function of
    // its leaves xored with a constant, on the bits of `demanded` up to its highest;
    // or a constant. `leaves[i]` is how `expr` reads variable `i` (a variable, with
    // the masks the tree reads it under), and the answer reads it the same way.
    // Null when the probes find no such function. Unproved.
    std::unique_ptr< Expr > ReadAsAffineOfLeaves(
        const Expr &expr, const std::vector< std::unique_ptr< Expr > > &leaves, uint32_t bitwidth,
        uint64_t demanded
    );

} // namespace cobra
