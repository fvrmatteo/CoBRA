#pragma once

#include "cobra/core/PassContract.h"
#include "cobra/core/PolyIR.h"
#include "cobra/core/Simplifier.h"
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace cobra {

    // Weight function: evaluated at support-local coordinates.
    using WeightFn = std::function< uint64_t(std::span< const uint64_t >, uint32_t bw) >;

    struct WeightedFitResult
    {
        NormalizedPoly poly;
        uint8_t degree_used = 0; // highest total degree with nonzero coeff
    };

    // Weighted falling-factorial coefficient solve.
    //
    // Recovers h_e such that target(x) = Σ h_e [weight(x) * φ_e(x)] mod 2^bw
    // on the {0..grid_degree}^k interpolation grid, where the unknowns are
    // restricted to total degree <= max_degree.
    //
    // grid_degree and max_degree are separate: ghost-family weights are zero
    // on all {0,1}^k inputs, so grid_degree >= 2 is needed even for
    // constant (max_degree=0) quotient recovery.
    //
    // target: full-space evaluator (matches RecoverMultivarPoly contract).
    // weight: support-local evaluator.
    //
    // The returned NormalizedPoly uses original-space variable indices
    // (mapped via support_vars) with num_vars = total_num_vars.
    //
    // target sampled over the interpolation grid, in the row order the fit
    // consumes. Which points get sampled is fixed by support_vars,
    // total_num_vars and grid_degree alone — the weight never enters — so a
    // caller that fits many weights against one target can sample it once
    // and hand the same grid to every fit.
    using WeightedFitGrid = std::vector< uint64_t >;

    // Sample target over the grid RecoverWeightedPoly would use. Returns an
    // empty grid for inputs RecoverWeightedPoly would reject anyway, which
    // callers can pass through untouched: the fit re-checks its guards and
    // resamples if the grid does not fit.
    WeightedFitGrid EvaluateWeightedFitGrid(
        const Evaluator &target, const std::vector< uint32_t > &support_vars,
        uint32_t total_num_vars, uint32_t bitwidth, uint8_t grid_degree = 2
    );

    // Does NOT verify beyond the grid.
    //
    // Pass `grid` to reuse a sampling from EvaluateWeightedFitGrid taken
    // against the same target and grid parameters; it is ignored unless its
    // size matches the grid this call needs.
    SolverResult< WeightedFitResult > RecoverWeightedPoly(
        const Evaluator &target, const WeightFn &weight,
        const std::vector< uint32_t > &support_vars, uint32_t total_num_vars, uint32_t bitwidth,
        uint8_t max_degree = 2, uint8_t grid_degree = 2, const WeightedFitGrid *grid = nullptr
    );

} // namespace cobra
