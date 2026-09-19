#pragma once

#include "cobra/core/Expr.h"
#include "cobra/verify/Z3Verifier.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cobra {

    // Whether `lhs` and `rhs` agree on every input, asked the way
    // `settings.strategy` says.
    //
    //  - kDirect is `Z3VerifyExprs`: the two trees handed to the solver as
    //    they are.
    //  - kLLVM compiles the question first. `lhs != rhs` is built as an LLVM
    //    function of the variables, optimized with `settings.llvm_passes`, and
    //    the optimized function is what the solver is asked about - the idea of
    //    SLOT (Mikek and Zhang, "SLOT: SMT-LLVM Optimizing Translation"). A
    //    function the optimizer folds to a constant needs no solver at all.
    //  - kRace asks both on two threads and takes the first definite answer,
    //    cancelling the other.
    //
    // The compiled question is the same question. Every operation of the
    // lowering is total - a shift by the width or more is written as the zero
    // it is, and nothing carries a poison-generating flag - and the variables
    // are `noundef`, so the function is defined on every input and whatever the
    // optimizer makes of it computes the same value on every input. An answer
    // about the optimized function is therefore an answer about the trees. What
    // the translation back to the solver cannot express sends the question to
    // kDirect instead.
    Z3VerifyResult VerifyExprsWithStrategy(
        const Expr &lhs, const Expr &rhs, const std::vector< std::string > &var_names,
        uint32_t bitwidth, const Z3VerificationSettings &settings
    );

    struct SmtStrategyCounters
    {
        uint64_t folded           = 0; // the optimizer answered, no solver call
        uint64_t compiled         = 0; // the optimized question went to the solver
        uint64_t fallbacks        = 0; // it could not be translated back: kDirect
        uint64_t race_direct_won  = 0;
        uint64_t race_compiled_won = 0;
        uint64_t race_both_unknown = 0;
        uint64_t swept             = 0; // compiled questions swept (`settings.sweep`)
        uint64_t sweep_lemmas      = 0;
        uint64_t sweep_proved      = 0;
        uint64_t sweep_generalized = 0;
        uint64_t sweep_synthesized = 0;
    };

    SmtStrategyCounters SmtStrategyStatistics();

} // namespace cobra
