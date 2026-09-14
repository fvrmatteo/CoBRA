#pragma once

#include "cobra/core/Expr.h"
#include <cstdint>
#include <vector>

namespace cobra {

    /// Evaluate an expression at all 2^n assignments where each variable
    /// is 0 or (1 << bit_pos). Right-shift results by bit_pos.
    /// This is the per-bit "signature row" from MSiMBA Theorem 1.1.
    std::vector< uint64_t > EvaluateSemilinearRow(
        const Expr &expr, uint32_t num_vars, uint32_t bitwidth, uint32_t bit_pos
    );

    /// Check if a syntactically semilinear expression is actually linear.
    /// Compares semi-linear signature rows: if all identical, it's linear.
    /// The full rows are compared up to eight variables; past that only the
    /// points with a single variable set are, which misses interactions.
    bool IsLinearShortcut(const Expr &expr, uint32_t num_vars, uint32_t bitwidth);

} // namespace cobra
