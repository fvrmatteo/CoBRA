#include "cobra/verify/Sweep.h"

#include <bitwuzla/cpp/bitwuzla.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>

using namespace cobra;
using bitwuzla::Kind;
using bitwuzla::Term;

namespace {

    bitwuzla::Result Solve(bitwuzla::TermManager &terms, const std::vector< Term > &assertions, uint64_t ms) {
        bitwuzla::Options options;
        options.set(bitwuzla::Option::TIME_LIMIT_PER, ms);
        bitwuzla::Bitwuzla solver(terms, options);
        for (const auto &assertion : assertions) {
            solver.assert_formula(assertion);
        }
        return solver.check_sat();
    }

    SweepSettings Budget(std::chrono::milliseconds ms) {
        return SweepSettings{ .deadline = std::chrono::steady_clock::now() + ms };
    }

    // An obfuscated `b & ~a`, as a protector writes it with a borrow:
    // ((a ^ b) >> 1) - (((a - b) >> 1) | (b > a) << 63).
    Term HiddenAndNot(bitwuzla::TermManager &t, const Term &a, const Term &b) {
        const auto sort = a.sort();
        const Term one  = t.mk_bv_one(sort);
        const Term top  = t.mk_bv_min_signed(sort);
        const Term zero = t.mk_bv_zero(sort);
        const Term half = t.mk_term(Kind::BV_SHR, { t.mk_term(Kind::BV_XOR, { a, b }), one });
        const Term diff = t.mk_term(
            Kind::BV_OR,
            { t.mk_term(Kind::BV_SHR, { t.mk_term(Kind::BV_SUB, { a, b }), one }),
              t.mk_term(Kind::ITE, { t.mk_term(Kind::BV_UGT, { b, a }), top, zero }) }
        );
        return t.mk_term(Kind::BV_SUB, { half, diff });
    }

    // (x ^ b) + x with x = b & ~a is b again: one layer of the protector's
    // identity update.
    Term Layer(bitwuzla::TermManager &t, const Term &a, const Term &b) {
        const Term hidden = HiddenAndNot(t, a, b);
        return t.mk_term(Kind::BV_ADD, { t.mk_term(Kind::BV_XOR, { hidden, b }), hidden });
    }

} // namespace

// Two layers of the identity feed a comparison that is then compared with
// itself through the plain value: UNSAT, and Bitwuzla alone needs seconds.
TEST(Sweep, ProvesStackedCarryIdentity) {
    bitwuzla::TermManager t;
    const auto sort = t.mk_bv_sort(64);
    const Term v0   = t.mk_const(sort, "v0");
    const Term v1   = t.mk_const(sort, "v1");
    const Term v2   = t.mk_const(sort, "v2");
    const Term v3   = t.mk_const(sort, "v3");
    const Term key  = t.mk_bv_value_uint64(sort, 0xFCD082FB84A92363ull);
    const Term b    = t.mk_term(Kind::BV_XOR, { v2, key });
    const Term once = Layer(t, t.mk_term(Kind::BV_NOT, { v1 }), b);
    const Term twice = Layer(t, t.mk_term(Kind::BV_NOT, { v0 }), once);
    const Term limit = t.mk_term(Kind::BV_NOT, { v3 });
    const Term question = t.mk_term(
        Kind::DISTINCT,
        { t.mk_term(Kind::BV_UGT, { twice, limit }), t.mk_term(Kind::BV_ULT, { limit, b }) }
    );

    SweepCounters counters;
    const auto swept = SweepFormula(t, { question }, Budget(std::chrono::milliseconds(5000)), &counters);
    ASSERT_EQ(swept.size(), 1u);
    EXPECT_GT(counters.proved, 0u);
    EXPECT_GT(counters.synthesized, 0u);
    EXPECT_EQ(Solve(t, swept, 1000), bitwuzla::Result::UNSAT);
}

// What the sweep hands back must be satisfiable when the question is.
TEST(Sweep, KeepsSatisfiableQuestionsSatisfiable) {
    bitwuzla::TermManager t;
    const auto sort = t.mk_bv_sort(32);
    const Term x    = t.mk_const(sort, "x");
    const Term y    = t.mk_const(sort, "y");
    // x * 3 + (y & 0) == 7 has a solution; (y & 0) agrees with zero everywhere.
    const Term sum = t.mk_term(
        Kind::BV_ADD,
        { t.mk_term(Kind::BV_MUL, { x, t.mk_bv_value_uint64(sort, 3) }),
          t.mk_term(Kind::BV_AND, { y, t.mk_bv_zero(sort) }) }
    );
    const Term question = t.mk_term(Kind::EQUAL, { sum, t.mk_bv_value_uint64(sort, 7) });
    const auto swept    = SweepFormula(t, { question }, Budget(std::chrono::milliseconds(2000)));
    EXPECT_EQ(Solve(t, swept, 1000), bitwuzla::Result::SAT);
}

// A comparison against one particular value is false on every random input,
// so it is offered `false` - and the solver must turn that down.
TEST(Sweep, RefutesCandidatesTheSamplesMisled) {
    bitwuzla::TermManager t;
    const auto sort    = t.mk_bv_sort(64);
    const Term x       = t.mk_const(sort, "x");
    const Term rare    = t.mk_term(Kind::EQUAL, { x, t.mk_bv_value_uint64(sort, 0x123456789ABCDEF1ull) });
    SweepCounters counters;
    const auto swept = SweepFormula(t, { rare }, Budget(std::chrono::milliseconds(2000)), &counters);
    EXPECT_GT(counters.refuted, 0u);
    EXPECT_EQ(Solve(t, swept, 1000), bitwuzla::Result::SAT);
}

// Once the other side of a race has answered, nothing more is asked.
TEST(Sweep, StopsWhenAsked) {
    bitwuzla::TermManager t;
    const auto sort = t.mk_bv_sort(64);
    const Term a    = t.mk_const(sort, "a");
    const Term b    = t.mk_const(sort, "b");
    const Term question =
        t.mk_term(Kind::DISTINCT, { Layer(t, t.mk_term(Kind::BV_NOT, { a }), b), b });
    const std::atomic< bool > stop{ true };
    SweepSettings settings = Budget(std::chrono::milliseconds(5000));
    settings.stop          = &stop;
    SweepCounters counters;
    const auto swept = SweepFormula(t, { question }, settings, &counters);
    EXPECT_EQ(counters.lemmas, 0u);
    ASSERT_EQ(swept.size(), 1u);
    EXPECT_EQ(swept.front().id(), question.id());
}

// A sum of partial products over an arithmetic value: `2 * (t & x) + (t ^ x)`
// is `t + x`, which the formula never spells out. With `t` two equal sums that
// differ in their constants' top bits, the question folds to an identity in `y`.
TEST(Sweep, SynthesizesLinearCombinationsOverAtoms) {
    bitwuzla::TermManager t;
    const auto sort = t.mk_bv_sort(64);
    const Term x    = t.mk_const(sort, "x");
    const Term y    = t.mk_const(sort, "y");
    const auto value = [&](uint64_t v) { return t.mk_bv_value_uint64(sort, v); };
    const auto sum = [&](uint64_t key, uint64_t offset) {
        const Term first = t.mk_term(Kind::BV_ADD, { t.mk_term(Kind::BV_NEG, { x }), value(1) });
        const Term second =
            t.mk_term(Kind::BV_XOR, { t.mk_term(Kind::BV_ADD, { y, value(0xFFFFFFFFDE6DC016ull) }), value(key) });
        const Term third = t.mk_term(
            Kind::BV_OR,
            { t.mk_term(Kind::BV_ADD, { t.mk_term(Kind::BV_MUL, { y, value(2) }), value(offset) }),
              value(0x43247FF4ull) }
        );
        return t.mk_term(Kind::BV_ADD, { t.mk_term(Kind::BV_ADD, { first, second }), third });
    };
    const Term low  = sum(0x7FFFFFFFDE6DC005ull, 0x7FFFFFFFBCDB802Cull);
    const Term high = sum(0xFFFFFFFFDE6DC005ull, 0xFFFFFFFFBCDB802Cull);
    const Term lhs  = t.mk_term(
        Kind::BV_ADD,
        { t.mk_term(Kind::BV_MUL, { t.mk_term(Kind::BV_AND, { low, x }), value(2) }),
          t.mk_term(Kind::BV_XOR, { high, x }) }
    );
    const Term question = t.mk_term(Kind::DISTINCT, { lhs, t.mk_term(Kind::BV_ADD, { y, value(16) }) });

    SweepCounters counters;
    const auto swept = SweepFormula(t, { question }, Budget(std::chrono::milliseconds(5000)), &counters);
    EXPECT_GT(counters.synthesized, 0u);
    EXPECT_EQ(Solve(t, swept, 2000), bitwuzla::Result::UNSAT);
}
