#include "cobra/core/SignatureEval.h"
#include "cobra/core/ExprTraversal.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/Expr.h"
#include "cobra/core/Profile.h"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

#ifdef COBRA_SIG_STATS
    #include "cobra/core/SignatureEvalStats.h"
    #include <chrono>
#endif

namespace cobra {

    namespace {

        // Reusable per-node buffers for EvalSigRecursive. Each node produces a
        // length-2^n result; binary nodes free their right child's buffer back
        // here so leaf acquisitions recycle it. This bounds live buffers (and
        // thus allocations) to ~tree depth instead of one per leaf.
        struct SigBufferPool
        {
            size_t len = 0;
            std::vector< std::vector< uint64_t > > free;

            std::vector< uint64_t > Acquire() {
                if (!free.empty()) {
                    std::vector< uint64_t > buf = std::move(free.back());
                    free.pop_back();
                    buf.resize(len); // no-op: every pooled buffer is already len
                    return buf;
                }
                return std::vector< uint64_t >(len);
            }

            void Release(std::vector< uint64_t > &&buf) { free.push_back(std::move(buf)); }
        };

        // Bottom-up evaluation: walk the expression tree once, computing all
        // 2^n outputs per node instead of calling eval_expr 2^n times.
        // Complexity: O(tree_size * 2^n) element-wise ops in a single tree
        // walk. Per-node result buffers are recycled through `pool` so the
        // 2^n vectors are not malloc'd once per leaf.
                // The work one node does once its children's buffers are in hand. Both
        // walks below share it, so the recursive and the iterative evaluation
        // are the same arithmetic by construction.
std::vector< uint64_t > CombineSig(
            const Expr &expr, SigBufferPool &pool, uint32_t bitwidth,
            std::vector< uint64_t > left, std::vector< uint64_t > right
        ) {
            const uint64_t kMask = Bitmask(bitwidth);
            const size_t len     = pool.len;

            switch (expr.kind) {
                case Expr::Kind::kConstant: {
                    auto r = pool.Acquire();
                    std::fill(r.begin(), r.end(), expr.constant_val & kMask);
                    return r;
                }
                case Expr::Kind::kVariable: {
                    auto r              = pool.Acquire();
                    const uint32_t kIdx = expr.var_index;
                    for (size_t i = 0; i < len; ++i) { r[i] = (i >> kIdx) & 1; }
                    return r;
                }
                case Expr::Kind::kNot: {
                    auto &child = left;
                    for (size_t i = 0; i < len; ++i) { child[i] = (~child[i]) & kMask; }
                    return std::move(child);
                }
                case Expr::Kind::kNeg: {
                    auto &child = left;
                    for (size_t i = 0; i < len; ++i) { child[i] = (-child[i]) & kMask; }
                    return std::move(child);
                }
                case Expr::Kind::kShr: {
                    auto &child       = left;
                    const uint64_t kK = expr.constant_val;
                    if (kK >= 64) {
                        std::fill(child.begin(), child.end(), 0);
                    } else {
                        for (size_t i = 0; i < len; ++i) {
                            child[i] = (child[i] >> kK) & kMask;
                        }
                    }
                    return std::move(child);
                }
                case Expr::Kind::kAdd: {
                                        for (size_t i = 0; i < len; ++i) { left[i] = (left[i] + right[i]) & kMask; }
                    pool.Release(std::move(right));
                    return std::move(left);
                }
                case Expr::Kind::kMul: {
                                        for (size_t i = 0; i < len; ++i) { left[i] = (left[i] * right[i]) & kMask; }
                    pool.Release(std::move(right));
                    return std::move(left);
                }
                case Expr::Kind::kAnd: {
                                        for (size_t i = 0; i < len; ++i) { left[i] = left[i] & right[i]; }
                    pool.Release(std::move(right));
                    return std::move(left);
                }
                case Expr::Kind::kOr: {
                                        for (size_t i = 0; i < len; ++i) { left[i] = (left[i] | right[i]) & kMask; }
                    pool.Release(std::move(right));
                    return std::move(left);
                }
                case Expr::Kind::kXor: {
                                        for (size_t i = 0; i < len; ++i) { left[i] = (left[i] ^ right[i]) & kMask; }
                    pool.Release(std::move(right));
                    return std::move(left);
                }
                case Expr::Kind::kCmpEq: {
                                        for (size_t i = 0; i < len; ++i) {
                        left[i] = ModCmpEq(left[i], right[i], bitwidth);
                    }
                    pool.Release(std::move(right));
                    return std::move(left);
                }
                case Expr::Kind::kCmpUlt: {
                                        for (size_t i = 0; i < len; ++i) {
                        left[i] = ModCmpUlt(left[i], right[i], bitwidth);
                    }
                    pool.Release(std::move(right));
                    return std::move(left);
                }
                case Expr::Kind::kCmpSlt: {
                                        for (size_t i = 0; i < len; ++i) {
                        left[i] = ModCmpSlt(left[i], right[i], bitwidth);
                    }
                    pool.Release(std::move(right));
                    return std::move(left);
                }
            }
            return std::vector< uint64_t >(len, 0);
        }

#if COBRA_NONRECURSIVE

        // Bottom-up over an explicit order rather than the call stack: the
        // depth of an obfuscated expression is chosen by the input, and a
        // signature is evaluated for every candidate, so this is the walk most
        // likely to meet a tree deeper than the stack allows.
        std::vector< uint64_t >
        EvalSigIterative(const Expr &root, SigBufferPool &pool, uint32_t bitwidth) {
            return FoldPostOrder< std::vector< uint64_t > >(
                root,
                [&](const Expr &node, std::vector< uint64_t > *kids, size_t count) {
                    std::vector< uint64_t > left;
                    std::vector< uint64_t > right;
                    if (count > 0) { left = std::move(kids[0]); }
                    if (count > 1) { right = std::move(kids[1]); }
                    return CombineSig(node, pool, bitwidth, std::move(left), std::move(right));
                }
            );
        }

#else

        std::vector< uint64_t >
        EvalSigRecursive(const Expr &expr, SigBufferPool &pool, uint32_t bitwidth) {
            std::vector< uint64_t > left;
            std::vector< uint64_t > right;
            if (!expr.children.empty()) {
                left = EvalSigRecursive(*expr.children[0], pool, bitwidth);
            }
            if (expr.children.size() > 1) {
                right = EvalSigRecursive(*expr.children[1], pool, bitwidth);
            }
            return CombineSig(expr, pool, bitwidth, std::move(left), std::move(right));
        }

#endif

#ifdef COBRA_SIG_STATS
        uint32_t CountNodesLocal(const Expr &e) {
            uint32_t n = 1;
            for (const auto &c : e.children) { n += CountNodesLocal(*c); }
            return n;
        }
#endif

    } // namespace

    std::vector< uint64_t >
    EvaluateBooleanSignature(const Expr &expr, uint32_t num_vars, uint32_t bitwidth) {
        COBRA_ZONE_N("EvaluateBooleanSignature");
        // 1 << num_vars is UB for num_vars >= 64. The orchestrator's
        // max_vars policy keeps callers in range; this assert protects
        // tests and direct callers.
        assert(
            num_vars < 64 && "EvaluateBooleanSignature: 1 << num_vars UB for num_vars >= 64"
        );
#ifdef COBRA_SIG_STATS
        auto t0 = std::chrono::high_resolution_clock::now();
#endif
        const size_t kLen = size_t{ 1 } << num_vars;
        SigBufferPool pool{ .len = kLen, .free = {} };
#if COBRA_NONRECURSIVE
        auto result = EvalSigIterative(expr, pool, bitwidth);
#else
        auto result = EvalSigRecursive(expr, pool, bitwidth);
#endif
#ifdef COBRA_SIG_STATS
        auto t1   = std::chrono::high_resolution_clock::now();
        double us = std::chrono::duration< double, std::micro >(t1 - t0).count();
        SigStatsRecordExpr(num_vars, CountNodesLocal(expr), us);
#endif
        return result;
    }

    std::vector< uint64_t >
    EvaluateBooleanSignature(const Evaluator &eval, uint32_t num_vars, uint32_t bitwidth) {
        // Same UB as the Expr overload — guarded uniformly.
        assert(
            num_vars < 64 && "EvaluateBooleanSignature: 1 << num_vars UB for num_vars >= 64"
        );
        // Enforce the producer/consumer arity contract on remapped or
        // arity-aware Evaluators. Function-path Evaluators constructed
        // without an explicit arity report InputArity() == 0 — accept
        // those as a wildcard ("trust the caller's num_vars"). Any
        // Evaluator that DOES report an arity must agree with num_vars,
        // or the closure would index its private buffer past num_vars.
        [[maybe_unused]] const auto kArity = eval.InputArity();
        assert(
            (kArity == 0 || kArity == num_vars)
            && "EvaluateBooleanSignature: Evaluator arity disagrees with num_vars"
        );
#ifdef COBRA_SIG_STATS
        auto t0 = std::chrono::high_resolution_clock::now();
#endif
        const size_t kLen    = size_t{ 1 } << num_vars;
        const uint64_t kMask = Bitmask(bitwidth);
        std::vector< uint64_t > sig(kLen);
        std::vector< uint64_t > point(num_vars);
        EvaluatorWorkspace workspace;
        for (size_t i = 0; i < kLen; ++i) {
            for (uint32_t v = 0; v < num_vars; ++v) { point[v] = (i >> v) & 1; }
            sig[i] = (eval.HasCompiledExpr() ? eval.EvaluateWithWorkspace(point, workspace)
                                             : eval(point))
                & kMask;
        }
#ifdef COBRA_SIG_STATS
        auto t1   = std::chrono::high_resolution_clock::now();
        double us = std::chrono::duration< double, std::micro >(t1 - t0).count();
        SigStatsRecordEval(num_vars, us);
#endif
        return sig;
    }

    // ---------------------------------------------------------------
    // Stats implementation (only when COBRA_SIG_STATS is defined)
    // ---------------------------------------------------------------

#ifdef COBRA_SIG_STATS

    namespace {
        thread_local SigEvalStats tl_stats{};
    }

    void SigStatsRecordExpr(uint32_t num_vars, uint32_t node_count, double elapsed_us) {
        tl_stats.calls++;
        tl_stats.expr_calls++;
        tl_stats.total_points += size_t{ 1 } << num_vars;
        tl_stats.total_nodes  += node_count;
        tl_stats.total_us     += elapsed_us;
    }

    void SigStatsRecordEval(uint32_t num_vars, double elapsed_us) {
        tl_stats.calls++;
        tl_stats.eval_calls++;
        tl_stats.total_points += size_t{ 1 } << num_vars;
        tl_stats.total_us     += elapsed_us;
    }

    SigEvalStats SigStatsSnapshot() { return tl_stats; }

    void SigStatsReset() { tl_stats = SigEvalStats{}; }

#endif

} // namespace cobra
