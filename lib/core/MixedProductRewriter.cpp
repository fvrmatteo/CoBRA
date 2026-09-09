#include "cobra/core/MixedProductRewriter.h"
#include "cobra/core/Classification.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/ExprTraversal.h"
#include "cobra/core/Trace.h"
#include <cstdint>
#include <vector>
#include <memory>
#include <utility>

namespace cobra {

#if COBRA_NONRECURSIVE

    uint32_t NodeCount(const Expr &expr) {
        return static_cast< uint32_t >(CountNodes(expr));
    }

#else

    uint32_t NodeCount(const Expr &expr) {
        uint32_t count = 1;
        for (const auto &child : expr.children) { count += NodeCount(*child); }
        return count;
    }

#endif

    namespace {

        // Context for top-down walk: are we inside a mixed-product
        // or bitwise-over-arithmetic context?
        struct RewriteContext
        {
            bool in_mixed_product      = false;
            bool in_bitwise_over_arith = false;
        };

        bool IsInUnsupportedContext(const RewriteContext &ctx) {
            return ctx.in_mixed_product || ctx.in_bitwise_over_arith;
        }

#if COBRA_NONRECURSIVE

        // Same shape as `HasNonleafBitwise`, and quadratic for the same reason
        // when written as recursion: one bottom-up pass for the variable
        // dependency turns the test at each arithmetic node into a lookup.
        // Same shape as `HasNonleafBitwise`, and quadratic for the same reason
        // when written as recursion.
        bool HasArithVar(const Expr &expr) {
            const uint8_t kFolded = FoldPostOrder< uint8_t >(
                expr,
                [](const Expr &node, uint8_t *kids, size_t count) -> uint8_t {
                    uint8_t state = node.kind == Expr::Kind::kVariable ? 1 : 0;
                    for (size_t k = 0; k < count; ++k) { state |= kids[k]; }
                    const bool kArith = node.kind == Expr::Kind::kAdd
                        || node.kind == Expr::Kind::kMul || node.kind == Expr::Kind::kNeg;
                    if (kArith && (state & 1) != 0) { state |= 2; }
                    return state;
                }
            );
            return (kFolded & 2) != 0;
        }

#else

        bool HasArithVar(const Expr &expr) {
            if (expr.kind == Expr::Kind::kAdd || expr.kind == Expr::Kind::kMul
                || expr.kind == Expr::Kind::kNeg)
            {
                if (HasVarDep(expr)) { return true; }
            }
            for (const auto &c : expr.children) {
                if (HasArithVar(*c)) { return true; }
            }
            return false;
        }

#endif

        uint32_t CountSitesImpl(const Expr &expr, const RewriteContext &ctx) {
            uint32_t count = 0;

            RewriteContext child_ctx = ctx;

            if (expr.kind == Expr::Kind::kMul) {
                if (expr.children.size() == 2) {
                    const bool kLhsBw = HasNonleafBitwise(*expr.children[0]);
                    const bool kRhsBw = HasNonleafBitwise(*expr.children[1]);
                    const bool kLhsVd = HasVarDep(*expr.children[0]);
                    const bool kRhsVd = HasVarDep(*expr.children[1]);
                    if ((kLhsBw || kRhsBw) && kLhsVd && kRhsVd) {
                        child_ctx.in_mixed_product = true;
                    }
                }
            }

            if (expr.kind == Expr::Kind::kAnd || expr.kind == Expr::Kind::kOr
                || expr.kind == Expr::Kind::kXor)
            {
                if (expr.children.size() == 2) {
                    if (HasArithVar(*expr.children[0]) || HasArithVar(*expr.children[1])) {
                        child_ctx.in_bitwise_over_arith = true;
                    }
                }
            }

            if (expr.kind == Expr::Kind::kNot) {
                if (!expr.children.empty() && HasArithVar(*expr.children[0])) {
                    child_ctx.in_bitwise_over_arith = true;
                }
            }

            if (expr.kind == Expr::Kind::kXor && expr.children.size() == 2
                && IsInUnsupportedContext(child_ctx))
            {
                count += 1;
            }

            for (const auto &c : expr.children) { count += CountSitesImpl(*c, child_ctx); }

            return count;
        }

        // Apply XOR lowering: x ^ y -> x + y - 2*(x & y)
        // Only in unsupported contexts.
        //
        // `node_limit` is the budget `RewriteMixedProducts` will accept, and it
        // is enforced here rather than only on the finished tree. Each lowering
        // duplicates both of its operands, and the walk is bottom-up, so nested
        // XORs double at every level: an expression that ends up over budget
        // does not end up slightly over it, it ends up exponentially over it,
        // and building the whole of that before measuring it is what the
        // rewriter spends most of its memory on. The rewrite only ever adds
        // nodes, so stopping as soon as the running count passes the budget
        // rejects exactly the expressions the finished-tree check rejected,
        // while never allocating more than the budget allows. `nodes` reports
        // the size of the subtree returned; `aborted` says the budget was
        // passed and the result is to be discarded.
        // NOLINTNEXTLINE(readability-identifier-naming)
        std::unique_ptr< Expr > ApplyXorLoweringImpl(
            std::unique_ptr< Expr > expr, const RewriteContext &ctx, uint32_t bitwidth,
            uint64_t node_limit, uint64_t &nodes, bool &aborted
        ) {
            nodes = 0;
            if (aborted) { return expr; }
            RewriteContext child_ctx = ctx;

            if (expr->kind == Expr::Kind::kMul) {
                if (expr->children.size() == 2) {
                    const bool lhs_bw = HasNonleafBitwise(*expr->children[0]);
                    const bool rhs_bw = HasNonleafBitwise(*expr->children[1]);
                    const bool lhs_vd = HasVarDep(*expr->children[0]);
                    const bool rhs_vd = HasVarDep(*expr->children[1]);
                    if ((lhs_bw || rhs_bw) && lhs_vd && rhs_vd) {
                        child_ctx.in_mixed_product = true;
                    }
                }
            }

            if (expr->kind == Expr::Kind::kAnd || expr->kind == Expr::Kind::kOr
                || expr->kind == Expr::Kind::kXor)
            {
                if (expr->children.size() == 2) {
                    if (HasArithVar(*expr->children[0]) || HasArithVar(*expr->children[1])) {
                        child_ctx.in_bitwise_over_arith = true;
                    }
                }
            }

            if (expr->kind == Expr::Kind::kNot) {
                if (!expr->children.empty() && HasArithVar(*expr->children[0])) {
                    child_ctx.in_bitwise_over_arith = true;
                }
            }

            // Recurse into children first (bottom-up)
            std::vector< uint64_t > child_nodes;
            child_nodes.reserve(expr->children.size());
            uint64_t subtree = 1;
            for (auto &c : expr->children) {
                uint64_t produced = 0;
                c = ApplyXorLoweringImpl(std::move(c), child_ctx, bitwidth, node_limit, produced, aborted);
                if (aborted) { return expr; }
                child_nodes.push_back(produced);
                subtree += produced;
            }

            // Apply XOR lowering if in unsupported context
            if (expr->kind == Expr::Kind::kXor && IsInUnsupportedContext(child_ctx)
                && expr->children.size() == 2)
            {
                // The replacement holds both operands twice, plus the six nodes
                // that spell out `x + y - 2*(x & y)`.
                const uint64_t lowered = (2 * (child_nodes[0] + child_nodes[1])) + 6;
                if (lowered > node_limit) {
                    aborted = true;
                    return expr;
                }
                COBRA_TRACE("MixedRewriter", "  XOR lowering: x^y -> x+y-2*(x&y)");
                // x ^ y -> x + y - 2*(x & y)
                auto lhs  = CloneExpr(*expr->children[0]);
                auto rhs  = CloneExpr(*expr->children[1]);
                auto lhs2 = CloneExpr(*expr->children[0]);
                auto rhs2 = CloneExpr(*expr->children[1]);

                auto sum         = Expr::Add(std::move(lhs), std::move(rhs));
                auto and_term    = Expr::BitwiseAnd(std::move(lhs2), std::move(rhs2));
                auto two_and     = Expr::Mul(Expr::Constant(2), std::move(and_term));
                auto neg_two_and = Expr::Negate(std::move(two_and));
                nodes            = lowered;
                return Expr::Add(std::move(sum), std::move(neg_two_and));
            }

            if (subtree > node_limit) {
                aborted = true;
                return expr;
            }
            nodes = subtree;
            return expr;
        }

    } // namespace

    uint32_t CountRewriteableSites(const Expr &expr) {
        const RewriteContext kCtx;
        return CountSitesImpl(expr, kCtx);
    }

    RewriteResult RewriteMixedProducts(
        std::unique_ptr< Expr > expr, const RewriteOptions &opts
    ) { // NOLINT(readability-identifier-naming)
        auto cls = ClassifyStructural(*expr);
        if (HasFlag(cls.flags, kSfHasUnknownShape)
            || !HasFlag(cls.flags, kSfHasMixedProduct | kSfHasBitwiseOverArith))
        {
            return { .expr = std::move(expr), .rounds_applied = 0, .structure_changed = false };
        }

        // Counting the sites walks the whole expression and asks a predicate at
        // every node. Nothing has changed it since, so the count the guard
        // needs is also the one the first round is compared against, and asking
        // twice was a second walk of the same tree for the same answer.
        const uint32_t initial_sites = CountRewriteableSites(*expr);
        if (initial_sites == 0) {
            return { .expr = std::move(expr), .rounds_applied = 0, .structure_changed = false };
        }

        const uint32_t initial_count = NodeCount(*expr);
        auto old_flags               = cls.flags & kUnsupportedFlagMask;
        auto old_sites               = initial_sites;
        COBRA_TRACE(
            "MixedRewriter",
            "RewriteMixedProducts: max_rounds={} max_growth={} initial_nodes={} "
            "initial_sites={}",
            opts.max_rounds, opts.max_node_growth, initial_count, old_sites
        );

        RewriteResult result;
        result.expr              = std::move(expr);
        result.rounds_applied    = 0;
        result.structure_changed = false;

        const uint64_t node_limit =
            static_cast< uint64_t >(initial_count) * static_cast< uint64_t >(opts.max_node_growth);

        for (uint32_t round = 1; round <= opts.max_rounds; ++round) {
            const RewriteContext ctx;
            uint64_t produced = 0;
            bool aborted      = false;
            auto new_expr =
                ApplyXorLoweringImpl(CloneExpr(*result.expr), ctx, opts.bitwidth, node_limit, produced, aborted);
            if (aborted) {
                COBRA_TRACE(
                    "MixedRewriter", "Round {}: ABORT - node growth exceeded during rewrite", round
                );
                break;
            }

            const uint32_t new_count = NodeCount(*new_expr);
            if (new_count > initial_count * opts.max_node_growth) {
                COBRA_TRACE(
                    "MixedRewriter", "Round {}: ABORT — node growth exceeded ({}>{})", round,
                    new_count, initial_count * opts.max_node_growth
                );
                break;
            }

            auto new_cls   = ClassifyStructural(*new_expr);
            auto new_flags = new_cls.flags & kUnsupportedFlagMask;
            auto new_sites = CountRewriteableSites(*new_expr);
            COBRA_TRACE(
                "MixedRewriter", "Round {}: nodes={} sites={} flags=0x{:x}", round, new_count,
                new_sites, static_cast< uint32_t >(new_flags)
            );

            // Safety: reject if new unsupported flags appeared
            if ((new_flags & ~old_flags) != StructuralFlag{ 0 }) {
                COBRA_TRACE(
                    "MixedRewriter", "Round {}: ABORT — new unsupported flags appeared", round
                );
                break;
            }

            // Progress check
            const bool coarse_progress =
                (new_flags != old_flags) && ((new_flags & old_flags) == new_flags);
            const bool fine_progress = (new_sites < old_sites);
            COBRA_TRACE(
                "MixedRewriter", "Round {}: coarse_progress={} fine_progress={}", round,
                coarse_progress, fine_progress
            );

            if (!coarse_progress && !fine_progress) {
                COBRA_TRACE("MixedRewriter", "Round {}: STOP — no progress", round);
                break;
            }

            result.expr              = std::move(new_expr);
            old_flags                = new_flags;
            old_sites                = new_sites;
            result.structure_changed = true;
            result.rounds_applied    = round;

            if (new_sites == 0) {
                COBRA_TRACE("MixedRewriter", "Round {}: no rewriteable sites — done", round);
                break;
            }
        }

        COBRA_TRACE(
            "MixedRewriter", "RewriteMixedProducts: total_rounds={} changed={}",
            result.rounds_applied, result.structure_changed
        );
        return result;
    }

} // namespace cobra
