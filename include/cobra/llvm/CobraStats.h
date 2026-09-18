#pragma once

#include <cstdint>

namespace llvm {
    class raw_ostream;
}

namespace cobra {

    // Where the value under a candidate's root eventually goes.
    //
    // A tree is worth simplifying because of what reads it, and the classes are
    // not worth the same. An address decides whether a load can be folded at
    // all; a compared value decides an edge; a value nothing above it looks at
    // decides nothing, and the search that was spent on it bought nothing. The
    // pass cannot tell these apart on its own - it is handed every MBA root in
    // the function - so the only way to know how its time divides between them
    // is to classify each candidate and add the time up.
    enum class CandidateConsumer : uint8_t {
        // A GEP index, an `inttoptr`, or the pointer operand of a load or a
        // store: the value is being used as an address, or as part of one.
        kPointerOffset,
        // The value operand of a store.
        kStoredValue,
        // An operand of an `icmp`, so it decides a branch, a select or a flag.
        kCompared,
        kCallArgument,
        kReturned,
        // Read only by arithmetic that itself reaches nothing above.
        kOther,
        // Nothing reads it.
        kUnused,
        kCount
    };

    // Where a candidate came from.
    //
    // Only the first are expressions the function actually holds. The rest are
    // the driver's second looks at a root it could not rewrite: the same tree
    // cut shorter, and the roots that tree buried. They are solved exactly like
    // the others and they vastly outnumber them, so whether they earn their
    // keep is the single biggest question about what the pass costs.
    enum class CandidateOrigin : uint8_t {
        kDetected,    // a root the detector handed back
        kBoundaryCut, // the same root, read with arithmetic under a bitwise reader kept whole
        kRecut,       // the same root, collected to a shorter tree
        kInnerRoot,   // a root the rejected tree buried
        kCount
    };

    const char *NameOf(CandidateConsumer consumer);
    const char *NameOf(CandidateOrigin origin);

    // One row of the report: how many candidates fell in a class, how many of
    // them were rewritten, and what the class cost.
    struct CandidateClass
    {
        uint64_t candidates  = 0;
        uint64_t solved      = 0;
        uint64_t nanoseconds = 0;
    };

    struct CobraStatistics
    {
        uint64_t runs        = 0;
        uint64_t nanoseconds = 0; // the whole pass, detection included

        // The four things a run spends its time on. Detection is the sweep over
        // the function; the ladder is the second looks a rejected root asks for,
        // which is collection again and is worth knowing apart from it.
        uint64_t detect_nanoseconds  = 0;
        uint64_t ladder_nanoseconds  = 0;
        uint64_t solve_nanoseconds   = 0;
        uint64_t rebuild_nanoseconds = 0;

        // Candidates the detector handed back, and the ones a rejected
        // candidate put back on the queue (re-cuts, boundary cuts, inner
        // roots). Both are solved the same way; they are counted apart because
        // only the first are expressions the function actually contains.
        uint64_t detected = 0;
        uint64_t requeued = 0;

        // Queued candidates whose root an earlier rewrite had already replaced.
        uint64_t dead_roots = 0;

        // How each candidate that was looked at ended.
        uint64_t cache_hits        = 0;
        uint64_t solved            = 0;
        uint64_t rejected_unsolved = 0; // no simplification was found
        uint64_t rejected_cost     = 0; // one was, and it was not cheaper
        uint64_t rejected_budget   = 0; // the rebuilt IR was not cheaper
        uint64_t rejected_cached   = 0; // an earlier run had already rejected it

        uint64_t solved_nanoseconds   = 0;
        uint64_t rejected_nanoseconds = 0;
        uint64_t cache_nanoseconds    = 0;

        // Candidates whose value reaches a pointer offset somewhere among its
        // readers, whatever class they were counted under. These are the
        // addresses a dedicated pointer simplification is meant to fold, so
        // every one of them CoBRA pays for is work in the wrong pass.
        uint64_t reaching_pointer_offset        = 0;
        uint64_t solved_reaching_pointer_offset = 0;
        uint64_t pointer_offset_nanoseconds     = 0;

        CandidateClass by_consumer[static_cast< unsigned >(CandidateConsumer::kCount)];
        CandidateClass by_origin[static_cast< unsigned >(CandidateOrigin::kCount)];

        // By how many independent leaves the candidate holds, the last row
        // standing for that many or more.
        static constexpr unsigned kMaxVariableRow = 16;
        CandidateClass by_variables[kMaxVariableRow + 1];
    };

    // Statistics are collected only when they are asked for: classifying a
    // candidate walks the readers of its root, and a run that is not going to
    // print the numbers should not pay for them. The counters that cost
    // nothing are kept regardless, so the totals are always available.
    void EnableStatistics(bool enabled);
    bool StatisticsEnabled();

    CobraStatistics Statistics();
    void ResetStatistics();

    // The report, as a table. Prints nothing when the pass never ran.
    void PrintStatistics(llvm::raw_ostream &out);

} // namespace cobra
