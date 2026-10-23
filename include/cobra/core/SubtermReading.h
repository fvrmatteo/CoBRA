#pragma once

// A root that is a bitwise function of one or two of its own subterms.
//
// The semilinear reading takes an arithmetic value under a bitwise operator as an
// atom, and atoms as independent: `(a - b - 32) ^ K` is the atom `a - b - 32`
// xored with a constant. An obfuscator writes the same number through pieces that
// only the carries between them tie together - halvings of sums, the carry out of
// the top bit picked by a comparison, the low bit of a sum written as the xor of
// its operands' low bits - and read as independent atoms those pieces compute
// nothing in particular: the identity that joins them is invisible to a reading
// that does not evaluate them together.
//
// This reading evaluates them together. The root and every arithmetic subterm are
// computed on the same inputs, and a subterm `A` (or a pair `A`, `B`) is the
// answer's variable when every bit of the root is a function of the same bit of
// `A` (and `B`) alone, the same function on every input - a constant, the bit
// itself, its complement, or for a pair any of the sixteen. The functions read off
// bit by bit are the bitwise expression over the subterms the root then is:
// `(A & P) ^ C` for one, a semilinear expression over two simplified by the search
// itself. The subterms are the tree's own, so the rewrite reads nothing the tree
// did not, and the probes only propose it: the caller proves it.

#include "cobra/core/Expr.h"
#include "cobra/core/Simplifier.h"

#include <cstdint>
#include <memory>

namespace cobra {

    // `expr`, over `num_vars` variables at `bitwidth`, as a bitwise function of one or two
    // of its own arithmetic subterms or a constant, when the probes say it is one; null
    // otherwise. `opts` is what a two-subterm function is simplified with. Unproved.
    std::unique_ptr< Expr > ReadAsBitwiseOfSubterms(
        const Expr &expr, uint32_t num_vars, uint32_t bitwidth, const Options &opts
    );

} // namespace cobra
