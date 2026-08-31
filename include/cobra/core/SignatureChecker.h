#pragma once

#include "cobra/core/Evaluator.h"
#include "cobra/core/Expr.h"
#include <cstdint>
#include <vector>

namespace cobra {

    // Default probe count for FullWidthCheck / FullWidthCheckEval.
    // Sufficient for shape-deterministic rewrites (e.g., AnfCleanup,
    // RefoldNegation, single-pattern matchers) where the candidate's
    // shape is fully determined by the input and only randomized
    // sampling is needed to exercise full-width modular arithmetic.
    inline constexpr uint32_t kDefaultProbeCount = 8;

    // Strengthened probe count for "post-solver verification" gates —
    // the boundary between a numeric solver (CoB, ANF→Mul shadow,
    // singleton polynomial reconstruction, lifted-substitute) and
    // candidate emission. The 64-probe count is the documented
    // residual-gate strength: it catches boolean-correct /
    // full-width-incorrect false positives that the 8-probe default
    // fails to surface against shape-non-deterministic rewrites.
    inline constexpr uint32_t kResidualGateProbeCount = 64;

    struct CheckResult
    {
        bool passed;
        std::vector< uint64_t > failing_input; // empty if passed
    };

    // Verify that the simplified Expr produces the same output as the original
    // signature vector at all {0,1} input combinations. This is an exhaustive
    // check — sufficient for linear MBAs by the signature-vector equivalence
    // theorem (Reichenwallner et al.).
    CheckResult SignatureCheck(
        const std::vector< uint64_t > &original_sig, const Expr &simplified, uint32_t num_vars,
        uint32_t bitwidth
    );

    // Verify that the simplified Expr produces the same output as the original
    // Expr at random full-width inputs. Catches polynomial MBA results that are
    // {0,1}-correct but wrong on wider bitvectors (e.g., CoB emitting x&y when
    // the original computes x*y).
    //
    // var_map maps simplified variable indices to original variable indices
    // (needed when aux variable elimination reduced the variable set).
    // If empty, a 1:1 identity mapping is assumed.
    CheckResult FullWidthCheck(
        const Expr &original, uint32_t original_num_vars, const Expr &simplified,
        const std::vector< uint32_t > &var_map, uint32_t bitwidth, uint32_t num_samples = 8
    );

    // Verify that the simplified Expr produces the same output as an evaluator
    // at random full-width inputs. When the evaluator is Expr-backed, the
    // original side runs through its compiled program; otherwise this falls
    // back to the evaluator callback.
    CheckResult FullWidthCheckEval(
        const Evaluator &eval_original, uint32_t num_vars, const Expr &simplified,
        uint32_t bitwidth, uint32_t num_samples = 8
    );

    // Opens a reuse window for FullWidthCheckEval's candidate-independent
    // probes. A search that checks many candidates against one fixed
    // evaluator otherwise re-evaluates that evaluator at the same adversarial
    // inputs once per candidate; inside this scope those values are computed
    // once and shared. Purely an optimization: results are identical either
    // way, and calls whose evaluator, arity, or bitwidth do not match the
    // scope are unaffected.
    //
    // The scope must not outlive `eval`. Scopes nest (the previous one is
    // restored on destruction) and are thread-local, so a scope on one thread
    // never serves calls on another.
    class FullWidthProbeScope
    {
      public:
        FullWidthProbeScope(const Evaluator &eval, uint32_t num_vars, uint32_t bitwidth);
        ~FullWidthProbeScope();

        FullWidthProbeScope(const FullWidthProbeScope &)            = delete;
        FullWidthProbeScope &operator=(const FullWidthProbeScope &) = delete;
        FullWidthProbeScope(FullWidthProbeScope &&)                 = delete;
        FullWidthProbeScope &operator=(FullWidthProbeScope &&)      = delete;

      private:
        void *state_;
        void *previous_;
    };

    // Evaluate an Expr tree at given variable values.
    uint64_t
    EvalExpr(const Expr &expr, const std::vector< uint64_t > &var_values, uint32_t bitwidth);

} // namespace cobra
