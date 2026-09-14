#pragma once

// Right shifts under a demanded mask.
//
// A right shift over a sum, `(a + b) >> s`, is where the semilinear reading
// of an expression ends: the bits shifted in carry into every lower position,
// so no sum of bitwise atoms reproduces it. But `2^s * (L >> s)` is
// `L - (L mod 2^s)` exactly, in every ring, so scaling the whole expression
// by `2^s` turns each such shift into its operand less a remainder - and the
// remainder of a sum by two is its parity, a bitwise atom. The scaled
// expression is semilinear where the original was not. Its simplified form
// divided back by `2^s` agrees with the original on every bit below the top
// `s`, which is all a caller reading only the low bits ever asked for.

#include "cobra/core/Expr.h"

#include <cstdint>
#include <memory>
#include <optional>

namespace cobra {

    // A bitwise function of variables and constants, a right shift over one
    // included: what the semilinear normalizer takes as an atom.
    bool IsBitwiseAtom(const Expr &expr);

    // True when some right shift in `expr` reads an operand that is not a
    // bitwise atom.
    bool HasShiftOverArithmetic(const Expr &expr);

    // One past the highest bit a demanded mask asks for, or zero when it asks
    // for nothing.
    uint32_t DemandedWidth(uint64_t mask, uint32_t bitwidth);

    struct LiftedShifts
    {
        // Equal to `2^shift * expr` in the ring, with every shift by `shift`
        // or less - over a sum or over an atom - turned into a sum. A shift by
        // more, or one under a bitwise operator, stays the atom it is.
        std::unique_ptr< Expr > scaled;

        // The largest amount lifted. `scaled / 2^shift` agrees with `expr`
        // below the top `shift` bits only.
        uint32_t shift = 0;
    };

    // Scales `expr` by the largest shift it takes over arithmetic and lifts
    // every shift within that amount out. Declines when there is no shift over
    // arithmetic, when one sits over another, or when a sum is shifted by more
    // than one, its remainder being no bitwise atom.
    std::optional< LiftedShifts > LiftShifts(const Expr &expr, uint32_t bitwidth);

    // `expr / 2^shift` when every term carries the factor structurally -
    // constants and coefficients divisible by `2^shift`, under sums and
    // negations. Null otherwise.
    std::unique_ptr< Expr >
    DivideByPowerOfTwo(const Expr &expr, uint32_t shift, uint32_t bitwidth);

} // namespace cobra
