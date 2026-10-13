#include "MBADetector.h"
#include <gtest/gtest.h>

using namespace cobra;

// A candidate reading the root whole needs exactly the bits its readers do.
TEST(CandidateDemand, WholeRootIsTheReadersMask) {
    EXPECT_EQ(CandidateDemand(0x00FF00F0, 32, SliceExtension::kNone, false), 0x00FF00F0u);
    EXPECT_EQ(CandidateDemand(UINT64_MAX, 64, SliceExtension::kNone, false), UINT64_MAX);
}

// Above a sign-extension slice every bit copies its top bit, so a reader of
// the high half reads bit 31 - which the low bits of the mask do not say.
TEST(CandidateDemand, SignSliceReadByItsHighBitsDemandsTheTopBit) {
    EXPECT_EQ(
        CandidateDemand(0xFFFFFFFF00000000ULL, 32, SliceExtension::kSign, false), 0x80000000u
    );
    EXPECT_EQ(
        CandidateDemand(0xFFFFFFFF0000FFFFULL, 32, SliceExtension::kSign, false), 0x8000FFFFu
    );
    // Nothing above the slice read: nothing to copy.
    EXPECT_EQ(CandidateDemand(0x0000FFFFULL, 32, SliceExtension::kSign, false), 0xFFFFu);
}

// Above a zero-extension slice every bit is zero whatever the slice holds.
TEST(CandidateDemand, ZeroSliceReadByItsHighBitsDemandsNothingMore) {
    EXPECT_EQ(CandidateDemand(0xFFFFFFFF00000000ULL, 32, SliceExtension::kZero, false), 0u);
}

// A sum group is added back to the rest of the sum, so a difference in an
// unread low bit carries into the bits read: it agrees on every bit up to the
// highest one read.
TEST(CandidateDemand, SumGroupDemandsEveryBitBelowTheHighestRead) {
    EXPECT_EQ(CandidateDemand(0xFF00, 64, SliceExtension::kNone, true), 0xFFFFu);
    EXPECT_EQ(
        CandidateDemand(0xFFFFFFFF00000000ULL, 32, SliceExtension::kSign, true), 0xFFFFFFFFu
    );
    EXPECT_EQ(CandidateDemand(0, 32, SliceExtension::kSign, true), 0u);
}
