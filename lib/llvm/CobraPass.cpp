#include "cobra/llvm/CobraPass.h"
#include "IRReconstructor.h"
#include "MBADetector.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/Simplifier.h"

#ifdef COBRA_HAS_Z3
    #include "cobra/verify/Z3Verifier.h"
#endif

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Analysis.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/Debug.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#define DEBUG_TYPE "cobra"

STATISTIC(NumCandidates, "Number of MBA candidates found");
STATISTIC(NumSimplified, "Number of MBA expressions simplified");
STATISTIC(NumSkippedCost, "Number of candidates skipped (cost gate)");
STATISTIC(NumSkippedUnsupported, "Number of candidates skipped (unsupported)");
STATISTIC(NumOutcomeCacheHits, "Number of candidates served from the outcome cache");

namespace cobra {

    namespace {

        // What solving a candidate produced: either a replacement expression
        // that cleared every verification and cost gate, or nothing.
        struct CandidateOutcome
        {
            std::unique_ptr< Expr > expr;
            std::vector< std::string > real_vars;
        };

        void AppendExprKey(const Expr &expr, std::string &out) {
            out += static_cast< char >('a' + static_cast< int >(expr.kind));
            switch (expr.kind) {
                case Expr::Kind::kConstant:
                case Expr::Kind::kShr: // constant_val carries the shift amount
                    out += std::to_string(expr.constant_val);
                    break;
                case Expr::Kind::kVariable:
                    out += std::to_string(expr.var_index);
                    break;
                default:
                    break;
            }
            out += '(';
            for (const auto &child : expr.children) {
                AppendExprKey(*child, out);
            }
            out += ')';
        }

        // Everything the solve below observes about a candidate. The
        // serialization is structural rather than a hash so that distinct
        // candidates can never collide onto a shared outcome.
        //
        // cand.evaluator is deliberately absent: it evaluates the same LLVM
        // tree that cand.expr was built from, so the AST already pins its
        // behaviour. cand.leaf_values is absent for the same reason the cache
        // is useful at all — the leaves differ between rediscoveries of the
        // same tree, and the outcome refers to them only by index.
        //
        // Only the arity of cand.var_names is included, not the names. A
        // solve reads variables as the indices cand.expr carries and reports
        // real_vars as indices too, so the names cannot change the outcome —
        // but they are LLVM value names, which differ every time the same
        // shape is rediscovered on different SSA values. Keying on them made
        // each rediscovery a miss, which is precisely the case the cache
        // exists to catch.
        std::string CandidateKey(const MBACandidate &cand, const CobraPassOptions &options) {
            std::string key;
            key += std::to_string(cand.bitwidth);
            key += '|';
            key += std::to_string(options.max_vars);
            key += options.z3_verify ? "|z" : "|-";
            key += std::to_string(options.z3_settings.timeout_ms);
            key += std::to_string(static_cast< int >(options.z3_settings.unknown_result_mode));
            key += '|';
            key += std::to_string(cand.var_names.size());
            key += '|';
            for (uint64_t v : cand.sig) {
                key += std::to_string(v);
                key += ',';
            }
            key += '|';
            if (cand.expr != nullptr) {
                AppendExprKey(*cand.expr, key);
            }
            return key;
        }

        // CobraPass is constructed fresh for every pass-manager build, and
        // hosts commonly re-optimize overlapping IR many times per lifting
        // session, so the same MBA trees are rediscovered and re-solved over
        // and over. The cache therefore outlives individual pass instances.
        // Entries are keyed purely on candidate content, which makes them
        // valid across functions and modules alike.
        class OutcomeCache
        {
          public:
            // A null entry records a candidate that every gate rejected; a
            // missing entry means the candidate has not been solved yet.
            // Entries are handed out as shared_ptr so a hit stays valid even
            // if the cache is trimmed afterwards.
            using Entry = std::shared_ptr< const CandidateOutcome >;

            std::optional< Entry > Find(const std::string &key) const {
                const std::lock_guard< std::mutex > lock(mutex_);
                auto it = entries_.find(key);
                if (it == entries_.end()) {
                    return std::nullopt;
                }
                return it->second;
            }

            Entry Insert(const std::string &key, Entry entry) {
                const std::lock_guard< std::mutex > lock(mutex_);
                if (entries_.size() >= kMaxEntries) {
                    entries_.clear();
                }
                entries_.insert_or_assign(key, entry);
                return entry;
            }

          private:
            // Bound the footprint on very large modules; the working set for a
            // single function is orders of magnitude smaller than this.
            static constexpr size_t kMaxEntries = 8192;

            mutable std::mutex mutex_;
            std::unordered_map< std::string, Entry > entries_;
        };

        OutcomeCache &SharedOutcomeCache() {
            static OutcomeCache cache;
            return cache;
        }

        // Fingerprints recorded in the IR outlive the pass instance that wrote
        // them, so they have to say which configuration produced them. Folding
        // every setting that can change a candidate's fate into one tag lets a
        // reconfigured run ignore records it must not trust, without having to
        // walk the module to erase them.
        uint64_t OptionsTag(const CobraPassOptions &options) {
            const auto mix = [](uint64_t hash, uint64_t value) {
                constexpr uint64_t kGoldenRatio = 0x9E3779B97F4A7C15ULL;
                return hash ^ (value + kGoldenRatio + (hash << 6) + (hash >> 2));
            };

            uint64_t tag = mix(0, options.max_vars);
            tag          = mix(tag, options.min_ast_size);
            tag          = mix(tag, options.z3_verify ? 1 : 0);
            tag          = mix(tag, options.z3_settings.timeout_ms);
            tag = mix(tag, static_cast< uint64_t >(options.z3_settings.unknown_result_mode));
            tag = mix(tag, static_cast< uint64_t >(options.enabled_families));
            return tag;
        }

        // Run the full solve-and-verify pipeline for one candidate. Returns
        // null when any gate rejects it.
        OutcomeCache::Entry
        SolveCandidate(const MBACandidate &cand, const CobraPassOptions &options) {
            Options opts{ .bitwidth         = cand.bitwidth,
                          .max_vars         = options.max_vars,
                          .spot_check       = true,
                          .enabled_families = options.enabled_families,
                          .evaluator        = cand.evaluator };

            // Pass AST when available — unlocks semilinear,
            // MixedRewrite, and decomposition pipelines.
            const Expr *ast = cand.expr.get();

            auto result = Simplify(cand.sig, cand.var_names, ast, opts);
            if (!result.has_value()) {
                ++NumSkippedUnsupported;
                LLVM_DEBUG(
                    llvm::dbgs() << "CoBRA: skipping candidate: " << result.error().message
                                 << "\n"
                );
                return nullptr;
            }

            if (result.value().kind != SimplifyOutcome::Kind::kSimplified) {
                ++NumSkippedUnsupported;
                LLVM_DEBUG(
                    llvm::dbgs() << "CoBRA: not simplified: " << result.value().diag.reason
                                 << "\n"
                );
                return nullptr;
            }

            // Cost gate: don't replace if simplified form is not
            // smaller. nuw/nsw flags are intentionally dropped —
            // CoBRA's Expr model is modular arithmetic and we
            // cannot soundly preserve wrapping guarantees.
            //
            // Runs before verification: it is a purely syntactic comparison of
            // the two forms, so a candidate it rejects is rejected whatever the
            // solver would have said, and checking it first keeps the rejected
            // majority out of the solver entirely.
            if (cand.expr != nullptr) {
                auto original_cost   = ComputeCost(*cand.expr);
                auto simplified_cost = ComputeCost(*result.value().expr);
                if (!IsBetter(simplified_cost.cost, original_cost.cost)) {
                    ++NumSkippedCost;
                    LLVM_DEBUG(
                        llvm::dbgs() << "CoBRA: skipping — simplified form is not smaller\n"
                    );
                    return nullptr;
                }
            }

#ifdef COBRA_HAS_Z3
            if (options.z3_verify) {
                if (ast == nullptr) {
                    ++NumSkippedUnsupported;
                    LLVM_DEBUG(
                        llvm::dbgs() << "CoBRA: skipping — Z3 verification requested but "
                                        "candidate AST is unavailable\n"
                    );
                    return nullptr;
                }

                auto z3_expr = CloneExpr(*result.value().expr);
                auto idx_map = TryBuildVarSupport(cand.var_names, result.value().real_vars);
                if (!idx_map.has_value()) {
                    ++NumSkippedUnsupported;
                    LLVM_DEBUG(
                        llvm::dbgs() << "CoBRA: skipping — real_vars not contained in "
                                        "candidate variable set\n"
                    );
                    return nullptr;
                }
                if (!idx_map->empty()) {
                    RemapVarIndices(*z3_expr, *idx_map);
                }

                auto z3_result = Z3VerifyExprs(
                    *ast, *z3_expr, cand.var_names, cand.bitwidth, options.z3_settings
                );
                if (!z3_result.equivalent) {
                    ++NumSkippedUnsupported;
                    LLVM_DEBUG(
                        llvm::dbgs() << "CoBRA: skipping — Z3 verification failed: "
                                     << z3_result.counterexample << "\n"
                    );
                    return nullptr;
                }
            }
#else
            if (options.z3_verify) {
                ++NumSkippedUnsupported;
                LLVM_DEBUG(
                    llvm::dbgs() << "CoBRA: skipping — built without Z3 support but Z3 "
                                    "verification requested\n"
                );
                return nullptr;
            }
#endif

            return std::make_shared< const CandidateOutcome >(CandidateOutcome{
                .expr      = std::move(result.value().expr),
                .real_vars = std::move(result.value().real_vars) });
        }

    } // namespace

    llvm::PreservedAnalyses
    CobraPass::run(llvm::Function &f, llvm::FunctionAnalysisManager & /*AM*/) {
        bool changed = false;

        const uint64_t options_tag = OptionsTag(options_);

        auto candidates =
            DetectMbaCandidates(f, options_.min_ast_size, options_.max_vars, options_tag);

        NumCandidates += candidates.size();

        auto &cache = SharedOutcomeCache();

        // Rejected candidates put their inner roots back on the queue, so an
        // expression stays reachable even when the tree enclosing it is not
        // something CoBRA can do anything with. Each descent strictly shrinks
        // the tree and `attempted` bars repeats, so the queue always drains.
        llvm::DenseSet< llvm::Instruction * > attempted;
        for (size_t index = 0; index < candidates.size(); ++index) {
            auto &cand = candidates[index];
            if (!attempted.insert(cand.root).second) {
                continue;
            }
            const std::string key = CandidateKey(cand, options_);

            OutcomeCache::Entry outcome;
            if (auto cached = cache.Find(key)) {
                ++NumOutcomeCacheHits;
                outcome = std::move(*cached);
            } else {
                outcome = cache.Insert(key, SolveCandidate(cand, options_));
            }
            if (outcome == nullptr) {
                // Nothing to rewrite here. Leaving the tree unmarked would have
                // the next run rediscover and re-solve it from scratch, which
                // is the dominant cost when a function is re-optimized, so the
                // shape that led nowhere is recorded on the root.
                RecordMbaFingerprint(cand.root, cand.fingerprint, options_tag);

                for (auto &inner : ExpandMbaCandidate(cand, options_.min_ast_size, options_tag))
                {
                    candidates.push_back(std::move(inner));
                }
                continue;
            }

            // Build variable index map for aux var elimination.
            // real_vars may be a subset of var_names with
            // reindexed positions.
            std::vector< uint32_t > var_map;
            const auto &real_vars = outcome->real_vars;
            if (!real_vars.empty() && real_vars.size() != cand.var_names.size()) {
                auto checked_var_map = TryBuildVarSupport(cand.var_names, real_vars);
                if (!checked_var_map.has_value()) {
                    ++NumSkippedUnsupported;
                    LLVM_DEBUG(
                        llvm::dbgs() << "CoBRA: skipping — real_vars not contained in "
                                        "candidate variable set\n"
                    );
                    RecordMbaFingerprint(cand.root, cand.fingerprint, options_tag);
                    continue;
                }
                var_map = std::move(*checked_var_map);
            }

            // Everything the builder emits lands between `mark` and the root, so
            // the replacement can be priced in the only currency that matters —
            // instructions — before anything commits to it.
            auto *block         = cand.root->getParent();
            const bool kAtFront = cand.root->getIterator() == block->begin();
            const auto kMark    = kAtFront ? block->end() : std::prev(cand.root->getIterator());

            llvm::IRBuilder<> builder(cand.root);
            auto *new_val = ReconstructIr(*outcome->expr, cand, builder, var_map);

            llvm::SmallVector< llvm::Instruction *, 16 > emitted;
            for (auto it = kAtFront ? block->begin() : std::next(kMark); &*it != cand.root;
                 ++it)
            {
                emitted.push_back(&*it);
            }

            // CoBRA measures its own output against the candidate's Expr, which
            // has already lost the sharing the IR had. Held to that yardstick a
            // single add reading two values computed elsewhere looks like the
            // whole cone beneath it, and expanding it into a polynomial reads as
            // progress. Comparing emitted instructions against removed ones is
            // what the rewrite is really worth.
            if (emitted.size() >= cand.dying_count) {
                for (auto *dead : llvm::reverse(emitted)) {
                    if (dead->use_empty()) {
                        dead->eraseFromParent();
                    }
                }
                ++NumSkippedCost;
                LLVM_DEBUG(
                    llvm::dbgs() << "CoBRA: skipping — rewrite emits " << emitted.size()
                                 << " instructions to remove " << cand.dying_count << "\n"
                );
                RecordMbaFingerprint(cand.root, cand.fingerprint, options_tag);
                for (auto &inner : ExpandMbaCandidate(cand, options_.min_ast_size, options_tag))
                {
                    candidates.push_back(std::move(inner));
                }
                continue;
            }

            cand.root->replaceAllUsesWith(new_val);
            ++NumSimplified;
            changed = true;

            // The replacement is itself an MBA tree the detector will find next
            // run, and re-solving it almost always just reproves that CoBRA's
            // own output cannot shrink further. Fingerprinting it now turns
            // that into a lookup; if a later pass reshapes it the recorded
            // shape stops matching and it is examined again.
            if (auto *new_inst = llvm::dyn_cast< llvm::Instruction >(new_val)) {
                if (auto new_fp = ComputeMbaFingerprint(new_inst)) {
                    RecordMbaFingerprint(new_inst, *new_fp, options_tag);
                }
            }

            LLVM_DEBUG(
                llvm::dbgs() << "CoBRA: simplified to "
                             << Render(*outcome->expr, real_vars, cand.bitwidth) << "\n"
            );
        }

        // DCE: iteratively erase dead instructions from replaced
        // trees across the whole function.  Needs multiple passes
        // because erasing %mul may make its operand %and dead.
        if (changed) {
            bool erased = true;
            while (erased) {
                erased = false;
                for (auto &bb : f) {
                    llvm::SmallVector< llvm::Instruction *, 16 > dead;
                    for (auto &inst : bb) {
                        if (inst.use_empty() && !inst.isTerminator()
                            && !inst.mayHaveSideEffects())
                        {
                            dead.push_back(&inst);
                        }
                    }
                    for (auto *inst : dead) {
                        inst->eraseFromParent();
                        erased = true;
                    }
                }
            }
        }

        return changed ? llvm::PreservedAnalyses::none() : llvm::PreservedAnalyses::all();
    }

} // namespace cobra
