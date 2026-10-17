#pragma once

// A product by a constant as Bitwuzla is best asked it.
//
// Bitwuzla bit-blasts `x * c` for a constant `c` as an adder per set bit of `c`, and a
// dense constant - `0x3FFFFFFFFFFFFFFE`, 61 bits set - makes a circuit no budget gets
// through: an identity Z3 proves in 0.06 s stays `unknown` after a minute, with or without
// the abstraction module, the preprocessing or the rewriting. The same product is the sum
// of shifts of `x` that the non-adjacent form of `c` names - `(x << 62) - (x << 1)` for
// that constant, and the identity is proved in 0.04 s. The value is the same modulo
// `2^width` for every `x`: only the circuit changes. It is written so when that form has
// fewer non-zero digits than `c` has set bits, and left a product otherwise.

#include <bitwuzla/cpp/bitwuzla.h>

#include <bit>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace cobra {

    // `x * c` at `width` bits (at most 64) as the signed sum of shifts of `x` the
    // non-adjacent form of `c` names, when it has fewer terms than `c` has set bits.
    inline std::optional< bitwuzla::Term > ConstantProduct(
        bitwuzla::TermManager &tm, const bitwuzla::Term &x, uint64_t c, uint32_t width
    ) {
        if (width == 0 || width > 64) {
            return std::nullopt;
        }
        const uint64_t mask = width == 64 ? ~uint64_t{ 0 } : ((uint64_t{ 1 } << width) - 1);
        c &= mask;
        // The digits of the non-adjacent form, low first: `k` odd takes the digit that
        // leaves `k - d` divisible by four. A digit at `width` or above is a multiple of
        // `2^width`, which is zero.
        std::vector< std::pair< uint32_t, int > > digits;
        unsigned __int128 k = c;
        for (uint32_t position = 0; k != 0 && position < width; ++position, k >>= 1) {
            if ((k & 1) == 0) {
                continue;
            }
            const int digit = (k & 3) == 1 ? 1 : -1;
            digits.emplace_back(position, digit);
            if (digit == 1) {
                k -= 1;
            } else {
                k += 1;
            }
        }
        if (digits.size() >= static_cast< size_t >(std::popcount(c))) {
            return std::nullopt;
        }
        const auto sort = tm.mk_bv_sort(width);
        std::optional< bitwuzla::Term > sum;
        for (const auto &[position, digit] : digits) {
            const bitwuzla::Term shifted = position == 0
                ? x
                : tm.mk_term(bitwuzla::Kind::BV_SHL, { x, tm.mk_bv_value_uint64(sort, position) });
            if (!sum) {
                sum = digit == 1 ? shifted : tm.mk_term(bitwuzla::Kind::BV_NEG, { shifted });
            } else {
                sum = tm.mk_term(digit == 1 ? bitwuzla::Kind::BV_ADD : bitwuzla::Kind::BV_SUB, { *sum, shifted });
            }
        }
        return sum.value_or(tm.mk_bv_zero(sort));
    }

} // namespace cobra
