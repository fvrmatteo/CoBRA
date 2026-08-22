#pragma once

#include "cobra/core/Expr.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cobra {

    /// Build an AND-product expression from a bitmask (v0 & v1 & ...).
    std::unique_ptr< Expr > BuildAndProduct(uint64_t mask);

    /// Apply a coefficient to an expression: 1*e → e, -1*e → -e, c*e → c*e.
    std::unique_ptr< Expr > ApplyCoefficient(
        std::unique_ptr< Expr > expr, uint64_t coeff, uint32_t bitwidth
    ); // NOLINT(readability-identifier-naming)

    /// Rewrite every kVariable node's var_index through index_map.
    void RemapVarIndices(Expr &expr, const std::vector< uint32_t > &index_map);

    /// Deep structural equality: same kind, constant, var_index, and children
    /// (in order). Unlike std::hash<Expr> this never reports false positives.
    bool ExprStructurallyEqual(const Expr &lhs, const Expr &rhs);

    /// Map a subset of variable names to their indices in the full variable list.
    std::optional< std::vector< uint32_t > > TryBuildVarSupport(
        const std::vector< std::string > &all_vars,
        const std::vector< std::string > &subset_vars
    );

    /// Map a subset of variable names to their indices in the full variable list.
    /// Throws std::out_of_range when subset_vars contains a name not present in all_vars.
    std::vector< uint32_t > BuildVarSupport(
        const std::vector< std::string > &all_vars,
        const std::vector< std::string > &subset_vars
    );

    /// Check if an Expr subtree contains only constants (no variables).
    bool IsConstantSubtree(const Expr &expr);

    /// Returns true if any node in the AST has the given kind.
    bool ContainsType(const Expr &expr, Expr::Kind kind);

    /// Append the var_index of every kVariable node (pre-order). Duplicates
    /// are preserved; callers that want a deduplicated support set must
    /// sort + unique the output themselves.
    void CollectVariables(const Expr &expr, std::vector< uint32_t > &out);

    /// Evaluate a constant-only Expr subtree (no variables allowed).
    /// Result is masked to Bitmask(bitwidth).
    /// Behavior is undefined if a Variable node is encountered (the
    /// implementation invokes std::unreachable() on the variable case).
    /// Use IsConstantSubtree() to verify before calling.
    uint64_t EvalConstantExpr(const Expr &expr, uint32_t bitwidth);

    /// Cosmetic cleanup on the final simplified expression.
    /// Chains: constant folding → negation refolding → ExtractCommonFactor
    /// → constant folding.
    ///
    /// Semantics-preserving: ExtractCommonFactor matches common factors by
    /// exact structural equality (ExprStructurallyEqual), so the distributive
    /// rewrite never fires on a non-equal factor.
    std::unique_ptr< Expr > CleanupFinalExpr(std::unique_ptr< Expr > expr, uint32_t bitwidth);

    /// Check if an Expr subtree depends on any variable.
    bool HasVarDep(const Expr &expr);

    /// Check if an Expr subtree contains a non-leaf bitwise node
    /// (And/Or/Xor/Not with variable dependence).
    bool HasNonleafBitwise(const Expr &expr);

    /// Replace AND(var-dep, var-dep) with MUL in an expression tree.
    /// The rewrite `AND(P, Q) → MUL(P, Q)` is the identity `x AND y == x * y`
    /// only when both x and y are individually in {0, 1}. At full width,
    /// the rewrite is unsound: `2 AND 2 = 2 ≠ 2 * 2 = 4`.
    ///
    /// CALLER OBLIGATION: this is a candidate-shape rewrite, not a
    /// semantics-preserving repair. The result MUST be verified at full
    /// width (e.g., FullWidthCheckEval with kResidualGateProbeCount)
    /// before being accepted as a verified candidate. Production
    /// callers (RunSignatureAnf) re-verify; tests and direct callers
    /// must do the same.
    std::unique_ptr< Expr > RepairProductShadow(std::unique_ptr< Expr > expr);

} // namespace cobra
