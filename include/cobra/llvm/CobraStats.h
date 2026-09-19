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
        // A rewrite that fits the budget and then failed verification. The
        // proof runs after the budget check, so these are the only refutations.
        uint64_t rejected_unverified = 0;

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

        // Where a solve's time goes, for the candidates actually solved rather
        // than served from the cache: the search for a rewrite, then the two
        // ways of proving one.
        uint64_t search_nanoseconds         = 0;
        uint64_t search_found               = 0; // the search proposed a rewrite
        uint64_t search_found_nanoseconds   = 0;
        // Trees found by random inputs to compute one of their own inputs
        // (offered only under `sweep`, see ProposeRewrite).
        uint64_t leaf_identities = 0;
        uint64_t enumeration_nanoseconds    = 0;
        uint64_t solver_calls               = 0;
        uint64_t solver_nanoseconds         = 0;
        uint64_t solver_proved_nanoseconds  = 0;
        uint64_t solver_refuted_nanoseconds = 0;
        uint64_t solver_unknown_nanoseconds = 0;

        // A rewrite that was found, and then lost to the instruction budget
        // when it was built into the IR: the search bought nothing. Counted
        // apart for fresh solves and for cache hits.
        uint64_t proved_over_budget             = 0;
        uint64_t proved_over_budget_nanoseconds = 0;
        uint64_t cached_over_budget             = 0;

        // Solves of a candidate the outcome cache had held once and dropped
        // when it filled up.
        uint64_t resolved_after_eviction             = 0;
        uint64_t resolved_after_eviction_nanoseconds = 0;

        // The demanded-bits analysis: how often it was computed over the whole
        // function, and what the questions put to it cost, computing included.
        uint64_t demanded_builds      = 0;
        uint64_t demanded_nanoseconds = 0;
        // Instructions whose demanded bits were settled from their readers
        // instead; of those, the ones settled by a fixpoint over a cycle of
        // readers, the cycles, and the cycles too large to settle that way.
        uint64_t demanded_local_settled  = 0;
        uint64_t demanded_local_regions  = 0;
        uint64_t demanded_local_nodes    = 0;
        uint64_t demanded_local_declined = 0;
        // With `COBRA_DEMANDED_CHECK`: answers compared against the
        // whole-function analysis, and how many of them differed.
        uint64_t demanded_check_compared   = 0;
        uint64_t demanded_check_mismatches = 0;

        // Solving ahead of the loop on worker threads: candidates handed over,
        // solved (with the workers' time), left unproved because the rewrite
        // could not fit, answers the loop took and the time it waited for
        // them, tasks it withdrew and solved itself, and the time spent at the
        // end of each run waiting for workers to finish.
        uint64_t background_submitted         = 0;
        uint64_t background_solved            = 0;
        uint64_t background_proofs_skipped    = 0;
        uint64_t background_settled           = 0;
        uint64_t background_withdrawn         = 0;
        uint64_t background_wait_nanoseconds  = 0;
        uint64_t background_drain_nanoseconds = 0;
        uint64_t background_nanoseconds       = 0;

        // The ladder, by the kind of second look.
        uint64_t boundary_nanoseconds = 0;
        uint64_t recut_nanoseconds    = 0;
        uint64_t inner_nanoseconds    = 0;

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

    // What stood behind the rewrites the pass made, kept whether or not the
    // report above is on: these are a handful of increments, and the one that
    // matters is not a cost but a liability. A solver that runs out of time
    // answers "unknown", and a host can ask for that to be taken as a yes
    // (`Z3UnknownResultMode::kTreatAsEquivalent`). Every rewrite accepted that
    // way is a rewrite nothing proved, and a run has to be able to say how many
    // of those it is standing on.
    //
    // All but the last count solves, so a candidate served again from the
    // outcome cache is not counted again. `rewrites_on_unknown` counts what
    // reached the IR: each rewrite emitted whose only justification was an
    // unknown answer, cache hits included.
    struct VerificationCounters
    {
        // Settled by running both sides over every input: a proof, or a
        // counterexample, with no solver involved.
        uint64_t proved_by_enumeration  = 0;
        uint64_t refuted_by_enumeration = 0;
        uint64_t proved_by_solver       = 0;
        uint64_t refuted_by_solver      = 0;
        // The solver answered unknown and the mode took that for equivalence;
        // `unknown_accepted_timeouts` is how many of them were its time limit.
        uint64_t unknown_accepted          = 0;
        uint64_t unknown_accepted_timeouts = 0;
        // The solver answered unknown and the candidate was declined: the mode
        // said so, or the tree holds a comparison, where no probe vouches for
        // the rewrite and the mode is therefore not asked.
        uint64_t unknown_declined    = 0;
        uint64_t rewrites_on_unknown = 0;
    };

    VerificationCounters Verification();
    void ResetVerification();

} // namespace cobra
