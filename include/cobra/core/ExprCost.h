#pragma once

#include "cobra/core/Expr.h"
#include <cstdint>

namespace cobra {

    struct ExprCost
    {
        uint32_t weighted_size       = 0;
        uint32_t nonlinear_mul_count = 0;
        uint32_t max_depth           = 0;
    };

    struct CostInfo
    {
        ExprCost cost;
        bool has_var_dep = false;
    };

    CostInfo ComputeCost(const Expr &expr);

    bool IsBetter(const ExprCost &candidate, const ExprCost &baseline);

    // Returns true when `candidate` is a pathological size blow-up relative to
    // `baseline`. A blow-up must clear two bars at once: it more than doubles
    // the baseline's weighted size AND is large in absolute terms. The ratio
    // catches change-of-basis expansions proportional to a large input (e.g.
    // sum(xi) - xor(xi) recovers an exponential AND-basis form); the absolute
    // floor spares the small ~2x expansions of legitimate canonicalization
    // (NOT-over-arith lowering ~(b*b) -> -(b*b)-1 roughly doubles a tiny
    // expression). This is deliberately keyed on weighted_size alone: it is a
    // size-explosion guard, distinct from IsBetter's full lexicographic
    // "is this an improvement" comparison.
    bool IsCostBlowup(const ExprCost &candidate, const ExprCost &baseline);

} // namespace cobra
