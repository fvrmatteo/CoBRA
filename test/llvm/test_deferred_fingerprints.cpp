#include "DeferredFingerprints.h"
#include <gtest/gtest.h>

using namespace cobra;

// A turned-down root with nothing queued from it is written when it is done.
TEST(DeferredFingerprints, RootWithoutRungsIsWrittenAtOnce) {
    DeferredFingerprints deferred;
    deferred.Defer(0);
    EXPECT_EQ(deferred.Finish(0, false), 0u);
}

// The root waits for every rung, rungs of rungs included, and is written once
// the last one decided.
TEST(DeferredFingerprints, RootIsWrittenAfterItsLastRung) {
    DeferredFingerprints deferred;
    deferred.Defer(0);
    deferred.Queue(1, 0);
    deferred.Queue(2, 0);
    EXPECT_EQ(deferred.Finish(0, false), std::nullopt);
    deferred.Queue(3, 1);
    EXPECT_EQ(deferred.Finish(1, false), std::nullopt);
    EXPECT_EQ(deferred.Finish(2, false), std::nullopt);
    EXPECT_EQ(deferred.Finish(3, false), 0u);
}

// One rung left for a later run - a proof out of time, a rewrite over budget -
// and the root is not written, so the next run collects it again.
TEST(DeferredFingerprints, UndecidedRungKeepsTheRootOpen) {
    DeferredFingerprints deferred;
    deferred.Defer(0);
    deferred.Queue(1, 0);
    deferred.Queue(2, 1);
    EXPECT_EQ(deferred.Finish(0, false), std::nullopt);
    EXPECT_EQ(deferred.Finish(1, false), std::nullopt);
    EXPECT_EQ(deferred.Finish(2, true), std::nullopt);
}

// A root that was not turned down (it was rewritten, or left undecided itself)
// writes nothing, however its rungs end.
TEST(DeferredFingerprints, RootNotTurnedDownWritesNothing) {
    DeferredFingerprints deferred;
    deferred.Queue(1, 0);
    EXPECT_EQ(deferred.Finish(0, false), std::nullopt);
    EXPECT_EQ(deferred.Finish(1, false), std::nullopt);
}

// Two roots in flight at once keep their own accounts.
TEST(DeferredFingerprints, RootsAreAccountedApart) {
    DeferredFingerprints deferred;
    deferred.Defer(0);
    deferred.Defer(1);
    deferred.Queue(2, 0);
    deferred.Queue(3, 1);
    EXPECT_EQ(deferred.Finish(0, false), std::nullopt);
    EXPECT_EQ(deferred.Finish(1, false), std::nullopt);
    EXPECT_EQ(deferred.Finish(2, true), std::nullopt);
    EXPECT_EQ(deferred.Finish(3, false), 1u);
}
