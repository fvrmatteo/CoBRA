#pragma once

#include <cstdint>

namespace cobra {

    inline uint64_t Bitmask(uint32_t bitwidth) {
        if (bitwidth >= 64) { return UINT64_MAX; }
        return (1ULL << bitwidth) - 1;
    }

    // Mask with the sign bit set (1 << (bitwidth - 1)), guarded so that
    // bitwidth==0 returns 0 rather than triggering shift UB. Use for
    // negative-constant detection paired with Bitmask(bitwidth).
    inline uint64_t SignBitMask(uint32_t bitwidth) {
        if (bitwidth == 0) { return 0; }
        if (bitwidth >= 64) { return 1ULL << 63; }
        return 1ULL << (bitwidth - 1);
    }

    inline uint64_t ModAdd(uint64_t a, uint64_t b, uint32_t bitwidth) {
        return (a + b) & Bitmask(bitwidth);
    }

    inline uint64_t ModSub(uint64_t a, uint64_t b, uint32_t bitwidth) {
        return (a - b) & Bitmask(bitwidth);
    }

    inline uint64_t ModMul(uint64_t a, uint64_t b, uint32_t bitwidth) {
        return (a * b) & Bitmask(bitwidth);
    }

    inline uint64_t ModNeg(uint64_t a, uint32_t bitwidth) { return ModSub(0, a, bitwidth); }

    inline uint64_t ModNot(uint64_t a, uint32_t bitwidth) { return (~a) & Bitmask(bitwidth); }

    inline uint64_t ModShr(uint64_t a, uint64_t k, uint32_t bitwidth) {
        if (k >= 64) { return 0; }
        return (a >> k) & Bitmask(bitwidth);
    }

    // Reinterprets the low `bitwidth` bits of `a` as a signed value. Operands
    // are carried zero-extended, so the sign bit has to be re-extended before
    // a signed comparison.
    inline int64_t SignExtend(uint64_t a, uint32_t bitwidth) {
        if (bitwidth == 0 || bitwidth >= 64) { return static_cast< int64_t >(a); }
        const uint64_t kSign = 1ULL << (bitwidth - 1);
        return static_cast< int64_t >(((a & Bitmask(bitwidth)) ^ kSign) - kSign);
    }

    // The comparison primitives yield 0 or 1, matching the i1 they model.
    inline uint64_t ModCmpEq(uint64_t a, uint64_t b, uint32_t bitwidth) {
        const uint64_t kMask = Bitmask(bitwidth);
        return (a & kMask) == (b & kMask) ? 1 : 0;
    }

    inline uint64_t ModCmpUlt(uint64_t a, uint64_t b, uint32_t bitwidth) {
        const uint64_t kMask = Bitmask(bitwidth);
        return (a & kMask) < (b & kMask) ? 1 : 0;
    }

    inline uint64_t ModCmpSlt(uint64_t a, uint64_t b, uint32_t bitwidth) {
        return SignExtend(a, bitwidth) < SignExtend(b, bitwidth) ? 1 : 0;
    }

} // namespace cobra
