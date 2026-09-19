#pragma once

#include <bitwuzla/cpp/bitwuzla.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <vector>

namespace cobra {

    struct SweepSettings
    {
        // No lemma is asked after this, and one being asked is stopped here.
        std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
        // ...or once this is set: the other side of a race has answered.
        const std::atomic< bool > *stop = nullptr;
        uint64_t rewrite_level          = 2;
    };

    struct SweepCounters
    {
        uint64_t lemmas      = 0; // equivalences put to the solver
        uint64_t proved      = 0;
        uint64_t generalized = 0; // of those proved, with the subterms both sides share cut away
        uint64_t synthesized = 0; // candidates the formula did not contain
        uint64_t refuted     = 0;
    };

    // `assertions` with every equivalence between their subterms that could be
    // proved before the deadline substituted: word-level SAT sweeping.
    //
    // Every subterm is evaluated on the same random inputs (with zero, all
    // ones, one and the signed extremes among them), and subterms that agree on
    // every input become candidates: the earliest subterm with the same values,
    // a constant, or - for arithmetic over at most three variables - the bitwise
    // function of them its values describe, written as an XOR of ANDs with
    // constant masks. Candidates are proved bottom-up, each as its own small
    // question, and each proof is substituted before the next is asked, so a
    // formula that stacks the same identity several times is proved one layer at
    // a time instead of all at once. A lemma is first asked with the subterms
    // both sides share replaced by fresh variables, which is enough more often
    // than not and never costs the solver the arithmetic below them; a model
    // that refutes a candidate joins the inputs and rules out every other
    // candidate it separates.
    //
    // What comes back is equisatisfiable with what went in - every substitution
    // is backed by an UNSAT answer - and holds new terms made in `terms`, which
    // the caller must own for the duration of the call. It was written for the
    // obfuscated carry arithmetic of a protector, `((a ^ b) >> 1) - ((a - b) >> 1
    // | borrow << 63)` standing for `b & ~a`: Bitwuzla needs one to five seconds
    // to prove a flag built on two of those equal to a comparison, and the sweep
    // proves the same in about a hundred milliseconds.
    std::vector< bitwuzla::Term > SweepFormula(
        bitwuzla::TermManager &terms, const std::vector< bitwuzla::Term > &assertions,
        const SweepSettings &settings, SweepCounters *counters = nullptr
    );

} // namespace cobra
