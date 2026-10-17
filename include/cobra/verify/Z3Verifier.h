#pragma once

#include "cobra/core/Expr.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cobra {

    enum class Z3UnknownResultMode {
        kFail,
        kTreatAsEquivalent,
    };

    // How an equivalence query reaches the solver (see `VerifyExprsWithStrategy`
    // in the LLVM library): as built, compiled through LLVM's optimizer first,
    // or both at once on two threads, taking the first definite answer.
    enum class SmtStrategy : uint8_t { kDirect, kLLVM, kRace };

    struct Z3VerificationSettings
    {
        uint32_t timeout_ms                     = 500;
        Z3UnknownResultMode unknown_result_mode = Z3UnknownResultMode::kFail;
        // Bitwuzla's term rewriting level, 0 to 2 (its own default is 2).
        uint32_t rewrite_level = 2;
        SmtStrategy strategy   = SmtStrategy::kDirect;
        // The optimization pipeline of kLLVM and kRace, in `opt -passes=` syntax.
        std::string llvm_passes =
            "instcombine<no-verify-fixpoint>,aggressive-instcombine,reassociate,sccp,dce,adce,"
            "instsimplify,gvn";
        // Whether kLLVM and kRace sweep a question (`SweepFormula`,
        // cobra/verify/Sweep.h) that is not answered within `sweep_head_start_ms` -
        // under kRace both the compiled one and the one as built.
        bool sweep = false;
        // How long a question is asked as it is before it is swept. A sweep only
        // ever proves - it finds subterms that agree and proves them equal - so a
        // question whose answer is "these differ" is answered by the solver on the
        // question as asked or not at all: Bitwuzla finds such a counterexample in
        // tens of milliseconds (median 70 ms, at most 150 ms over 176 of them), a
        // millisecond's head start found none, and the sweep then spent the whole
        // budget on each. 150 ms finds them all.
        uint32_t sweep_head_start_ms = 150;
    };

    struct Z3VerifyResult
    {
        bool equivalent{};
        bool unknown{};
        bool timed_out{};
        std::string counterexample;
    };

    /// Formally prove equivalence of the affine reconstruction (from CoB
    /// coefficients) and the simplified expression using Z3 bitvector theory.
    /// The coefficients must be the output of cob_transform() on the reduced sig.
    Z3VerifyResult Z3Verify(
        const std::vector< uint64_t > &cob_coeffs, const Expr &simplified,
        const std::vector< std::string > &var_names, uint32_t num_vars, uint32_t bitwidth,
        Z3VerificationSettings settings = {}
    );

    /// Compare two Expr trees for equivalence over all w-bit inputs.
    /// No CoB coefficients needed — used by the semilinear pipeline.
    Z3VerifyResult Z3VerifyExprs(
        const Expr &original, const Expr &simplified,
        const std::vector< std::string > &var_names, uint32_t bitwidth,
        Z3VerificationSettings settings = {}
    );

} // namespace cobra
