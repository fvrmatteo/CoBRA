#include "cobra/llvm/CobraPass.h"
#include "IRReconstructor.h"
#include "LocalDemandedBits.h"
#include "SmtStrategy.h"
#include "DeferredFingerprints.h"
#include "MBADetector.h"
#include "cobra/llvm/CobraStats.h"
#include "../verify/VerifyCache.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/CompiledExpr.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/ExprTraversal.h"
#include "cobra/core/RingProof.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureEval.h"
#include "cobra/core/Simplifier.h"

#ifdef COBRA_HAS_Z3
    #include "cobra/verify/Z3Verifier.h"
#endif

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Sequence.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/DemandedBits.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Analysis.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/Debug.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <functional>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <ctime>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <condition_variable>
#include <deque>
#include <thread>
#include <pthread.h>
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
        // that cleared the cost gate, or nothing.
        struct CandidateOutcome
        {
            std::unique_ptr< Expr > expr;
            std::vector< std::string > real_vars;
            // Where each of `real_vars` sits among the variables of the candidate
            // that was solved. The outcome is shared by every candidate with the
            // same key, and the key describes variables by position only, so a
            // position is the one thing about them that holds for all of those
            // candidates. Their names do not: the same name can stand at another
            // position in another tree - obfuscated code reads the same register
            // and frame slots in different roles - and binding a rewrite by name
            // there hands it the wrong leaves, with nothing on a cache hit to
            // catch it.
            std::vector< uint32_t > real_var_positions;
            // The solver could not decide this rewrite and the host's mode took
            // that for equivalence. Carried on the outcome because the outcome
            // is what the cache hands back: every rewrite it serves later stands
            // on the same unknown answer, and is counted as such.
            bool accepted_unknown = false;
            // Whether the rewrite has been through verification and passed it.
            // A rewrite is verified only once a candidate has shown it fits the
            // instruction budget: that check needs the rewrite built into the IR,
            // the proof does not need the IR at all, and the two are
            // independent - so a rewrite the budget turns down never has to be
            // proved, and one that fits is proved exactly as before. Until then
            // the cache holds it unverified, and the next candidate with the
            // same key that fits the budget pays for the proof.
            bool verified = false;
            // Found by reading the tree's conditions as variables
            // (`CobraPassOptions::boolean_variables`). Such a rewrite stands on
            // the merges of conditions the solver proved, and it is accepted
            // only on the solver's own proof that it equals the tree - see
            // `VerifyCandidate`.
            bool boolean_reading = false;
        };

        // Which bits of a value its readers look at, as LLVM's demanded-bits
        // analysis says. A rewrite only has to agree with the tree on those, and
        // for a shift over a sum that is the difference between a semilinear
        // reading and none. The answer goes stale the moment a rewrite lands -
        // the new tree reads its leaves through different operators than the
        // old one did - so everything known is dropped after every one.
        //
        // The whole-function analysis is what it used to be rebuilt from, and
        // that rebuild was more than a quarter of the pass: a walk over every
        // instruction per rewrite. The question is almost always about a value
        // with a few readers, so it is answered over those readers instead
        // (`LocalDemandedBits`), and the whole-function analysis is kept for a
        // value whose readers reach much of the function.
        //
        // `CobraPassOptions::check_demanded_bits` answers every question both
        // ways and reports the ones where they disagree.
        class DemandedBitsOracle
        {
          public:
            // The function's assumptions and dominator tree, from the host's
            // analysis manager: the pass never changes the control flow, so
            // they hold for the whole run, and a host that already has them
            // need not build them again. Registered first, which does nothing
            // where the host has registered them already.
            DemandedBitsOracle(
                llvm::Function &f, CobraStatistics &stats, llvm::FunctionAnalysisManager &am,
                bool check
            )
                : f_(f), stats_(stats), check_(check),
                  assumptions_(Analysis< llvm::AssumptionAnalysis >(f, am)),
                  dominators_(Analysis< llvm::DominatorTreeAnalysis >(f, am)),
                  local_(assumptions_, dominators_, kMaxLocalRegion),
                  function_size_(f.getInstructionCount()) {}

            template< typename AnalysisT >
            static typename AnalysisT::Result &
            Analysis(llvm::Function &f, llvm::FunctionAnalysisManager &am) {
                am.registerPass([] { return AnalysisT(); });
                return am.getResult< AnalysisT >(f);
            }

            uint64_t MaskOf(llvm::Instruction *inst) {
                if (inst == nullptr || !inst->getType()->isIntegerTy()
                    || inst->getType()->getIntegerBitWidth() > 64)
                {
                    return UINT64_MAX;
                }
                const auto begin    = std::chrono::steady_clock::now();
                const auto before   = Counters();
                const uint64_t mask = Through(inst, 0);
                const auto after    = Counters();
                stats_.demanded_local_settled += after[0] - before[0];
                stats_.demanded_local_regions += after[1] - before[1];
                stats_.demanded_local_nodes += after[2] - before[2];
                stats_.demanded_local_declined += after[3] - before[3];
                stats_.demanded_nanoseconds += static_cast< uint64_t >(
                    std::chrono::duration_cast< std::chrono::nanoseconds >(
                        std::chrono::steady_clock::now() - begin
                    )
                        .count()
                );
                return mask;
            }

            void Invalidate() {
                analysis_.reset();
                local_.Invalidate();
            }

            // The function's dominator tree. The pass never changes the control
            // flow, so the one built for the analysis serves the whole run.
            const llvm::DominatorTree &Dominators() const { return dominators_; }

          private:
            std::array< uint64_t, 4 > Counters() const {
                return { local_.settled, local_.regions, local_.region_nodes, local_.declined };
            }

            // `DemandedBits::getDemandedBits(inst)` over the function as it is.
            //
            // Answered from the readers while that is cheap. Once the questions
            // since the last rewrite have visited as many instructions as the
            // function holds - which the detection sweep, asking about every
            // root, does - one whole-function build answers the rest of them
            // for less.
            llvm::APInt Demanded(llvm::Instruction *inst) {
                const bool local_is_cheaper =
                    !analysis_.has_value() && local_.work_this_epoch < function_size_;
                if (check_ || local_is_cheaper) {
                    if (auto local = local_.Get(inst)) {
                        if (check_) {
                            const llvm::APInt global = Global(inst);
                            if (global != *local) {
                                ++stats_.demanded_check_mismatches;
                                ReportMismatch(inst, *local, global);
                            }
                            ++stats_.demanded_check_compared;
                        }
                        return *local;
                    }
                }
                return Global(inst);
            }

            llvm::APInt Global(llvm::Instruction *inst) {
                if (!analysis_.has_value()) {
                    analysis_.emplace(f_, assumptions_, dominators_);
                    ++stats_.demanded_builds;
                }
                return analysis_->getDemandedBits(inst);
            }

            void ReportMismatch(
                llvm::Instruction *inst, const llvm::APInt &local, const llvm::APInt &global
            ) {
                static unsigned printed = 0;
                if (printed >= 20) {
                    return;
                }
                ++printed;
                llvm::errs() << "cobra: demanded-bits mismatch: " << f_.getName()
                             << ": local 0x" << llvm::utohexstr(local.getZExtValue())
                             << ", global 0x" << llvm::utohexstr(global.getZExtValue())
                             << " for" << *inst << "\n";
            }

            // What the readers of `inst` look at, following `freeze`.
            //
            // LLVM's demanded-bits analysis has no rule for `freeze` and falls
            // back to giving the operand every bit. That is the safe answer for
            // a transform that has to preserve the freeze, and the wrong one
            // for a reader that is asking what the value is *used* for: a
            // `freeze` is the identity bit for bit, so what its readers look at
            // is exactly what it looks at. One `freeze` between an expression
            // and the single bit that reads it is enough to make the expression
            // look read in full, which is the difference between a rewrite
            // costed on one bit and one that has to reproduce all sixty-four.
            uint64_t Through(llvm::Instruction *inst, unsigned depth) {
                const uint64_t own = Demanded(inst).getZExtValue()
                    & Bitmask(inst->getType()->getIntegerBitWidth());

                // Only worth asking when the analysis gave up, which for this
                // shape it does by handing back every bit.
                if (depth >= kMaxFreezeDepth || inst->use_empty()
                    || own != Bitmask(inst->getType()->getIntegerBitWidth()))
                {
                    return own;
                }

                uint64_t through = 0;
                for (llvm::User *user : inst->users()) {
                    auto *freeze = llvm::dyn_cast< llvm::FreezeInst >(user);
                    if (freeze == nullptr || freeze->getType() != inst->getType()) {
                        return own;
                    }
                    through |= Through(freeze, depth + 1);
                }
                return through & own;
            }

            // A chain of freezes over one value is degenerate; this only has to
            // be deep enough that the ordinary one or two do not hit it.
            static constexpr unsigned kMaxFreezeDepth = 8;

            // How many readers, transitively, a value may have before the
            // question is put to the whole-function analysis instead. A region
            // costs about what the whole-function analysis costs on that many
            // instructions, so past a few thousand the local answer stops
            // paying, and one whole-function build answers every question
            // until the next rewrite.
            static constexpr size_t kMaxLocalRegion = 4096;

          private:
            llvm::Function &f_;
            CobraStatistics &stats_;
            bool check_;
            llvm::AssumptionCache &assumptions_;
            llvm::DominatorTree &dominators_;
            LocalDemandedBits local_;
            uint64_t function_size_;
            std::optional< llvm::DemandedBits > analysis_;
        };



        // An expression as SSA: one line per distinct node, sharing named
        // rather than repeated. `Render` spells the tree out instead, and a node
        // with two readers appears twice in it, so the expressions CoBRA is slow
        // on - wide and deep, with heavy sharing - render into megabytes. This
        // is linear in the nodes, and it shows the sharing, which is the part
        // that explains the cost.
        const char *ExprKindName(Expr::Kind kind) {
            switch (kind) {
                case Expr::Kind::kConstant: return "const";
                case Expr::Kind::kVariable: return "var";
                case Expr::Kind::kAdd:      return "add";
                case Expr::Kind::kMul:      return "mul";
                case Expr::Kind::kAnd:      return "and";
                case Expr::Kind::kOr:       return "or";
                case Expr::Kind::kXor:      return "xor";
                case Expr::Kind::kNot:      return "not";
                case Expr::Kind::kNeg:      return "neg";
                case Expr::Kind::kShr:      return "shr";
                case Expr::Kind::kCmpEq:    return "cmp.eq";
                case Expr::Kind::kCmpUlt:   return "cmp.ult";
                case Expr::Kind::kCmpSlt:   return "cmp.slt";
            }
            return "?";
        }

        std::string RenderSSA(
            const Expr &root, const std::vector< std::string > &var_names, size_t max_nodes = 4096
        ) {
            std::unordered_map< const Expr *, size_t > numbering;
            std::vector< const Expr * > order;
            std::vector< std::pair< const Expr *, bool > > work{ { &root, false } };
            bool truncated = false;
            while (!work.empty()) {
                auto [node, expanded] = work.back();
                work.pop_back();
                if (expanded) {
                    if (numbering.contains(node)) {
                        continue;
                    }
                    numbering.emplace(node, order.size());
                    order.push_back(node);
                    continue;
                }
                if (numbering.contains(node)) {
                    continue;
                }
                if (order.size() + work.size() > max_nodes) {
                    truncated = true;
                    continue;
                }
                work.push_back({ node, true });
                for (const auto &child : node->children) {
                    work.push_back({ child.get(), false });
                }
            }

            std::string out;
            for (size_t index = 0; index < order.size(); ++index) {
                const auto *node = order[index];
                out += "\n    %" + std::to_string(index) + " = ";
                if (node->kind == Expr::Kind::kConstant) {
                    out += "const " + std::to_string(node->constant_val);
                } else if (node->kind == Expr::Kind::kVariable) {
                    out += "var ";
                    out += node->var_index < var_names.size() ? var_names[node->var_index]
                                                              : ("v" + std::to_string(node->var_index));
                } else {
                    out += ExprKindName(node->kind);
                    if (node->kind == Expr::Kind::kShr) {
                        out += " " + std::to_string(node->constant_val);
                    }
                    for (const auto &child : node->children) {
                        const auto found = numbering.find(child.get());
                        out += found != numbering.end() ? " %" + std::to_string(found->second) : " ?";
                    }
                }
            }
            if (truncated) {
                out += "\n    ... truncated at " + std::to_string(max_nodes) + " nodes";
            }
            out += "\n    root %" + std::to_string(numbering.at(&root));
            return out;
        }

        // ---- what the time was spent on -----------------------------------
        //
        // The pass is handed every MBA root a function holds, and it cannot see
        // from a root whether simplifying it is worth anything: the tree that
        // decides an address and the tree that feeds a dead flag word look the
        // same from below. Classifying each candidate by what reads it, and
        // adding the time up per class, is what turns "CoBRA is the slowest
        // pass" into a statement about which of its work pays.

        constexpr uint32_t ConsumerBit(CandidateConsumer consumer) {
            return 1U << static_cast< unsigned >(consumer);
        }

        // How many readers the classification may look at. It has to cost a
        // fraction of a solve, not be exact about a value a hundred
        // instructions read; the class of a value is decided by the first
        // reader that says what it is for, and that is almost always close.
        constexpr unsigned kMaxConsumerVisits = 96;

        // The classes the value under `root` reaches, as a bitmask.
        uint32_t ConsumerMask(const llvm::Instruction *root) {
            if (root->use_empty()) {
                return ConsumerBit(CandidateConsumer::kUnused);
            }

            uint32_t mask = 0;
            unsigned visits = 0;
            llvm::DenseSet< const llvm::Value * > seen;
            llvm::SmallVector< const llvm::Value *, 16 > work{ root };
            while (!work.empty() && visits < kMaxConsumerVisits) {
                const llvm::Value *value = work.pop_back_val();
                for (const llvm::User *user : value->users()) {
                    if (++visits >= kMaxConsumerVisits) {
                        break;
                    }
                    if (const auto *store = llvm::dyn_cast< llvm::StoreInst >(user)) {
                        mask |= ConsumerBit(
                            store->getValueOperand() == value ? CandidateConsumer::kStoredValue
                                                              : CandidateConsumer::kPointerOffset
                        );
                        continue;
                    }
                    // A load reads it as an address; a GEP and an `inttoptr`
                    // build one out of it, which is the `base + index * scale +
                    // displacement` an address simplification is meant to own.
                    if (llvm::isa< llvm::LoadInst >(user)
                        || llvm::isa< llvm::GetElementPtrInst >(user)
                        || llvm::isa< llvm::IntToPtrInst >(user))
                    {
                        mask |= ConsumerBit(CandidateConsumer::kPointerOffset);
                        continue;
                    }
                    if (llvm::isa< llvm::ICmpInst >(user)) {
                        mask |= ConsumerBit(CandidateConsumer::kCompared);
                        continue;
                    }
                    if (llvm::isa< llvm::ReturnInst >(user)) {
                        mask |= ConsumerBit(CandidateConsumer::kReturned);
                        continue;
                    }
                    if (llvm::isa< llvm::CallBase >(user)) {
                        mask |= ConsumerBit(CandidateConsumer::kCallArgument);
                        continue;
                    }
                    if (llvm::isa< llvm::CastInst >(user) || llvm::isa< llvm::PHINode >(user)
                        || llvm::isa< llvm::BinaryOperator >(user)
                        || llvm::isa< llvm::SelectInst >(user)
                        || llvm::isa< llvm::FreezeInst >(user))
                    {
                        if (seen.insert(user).second) {
                            work.push_back(user);
                        }
                        continue;
                    }
                    mask |= ConsumerBit(CandidateConsumer::kOther);
                }
            }
            return mask != 0 ? mask : ConsumerBit(CandidateConsumer::kOther);
        }

        // The class a candidate is counted under when it reaches several, most
        // valuable first.
        CandidateConsumer PrimaryConsumer(uint32_t mask) {
            for (const auto consumer :
                 { CandidateConsumer::kPointerOffset, CandidateConsumer::kCompared,
                   CandidateConsumer::kStoredValue, CandidateConsumer::kCallArgument,
                   CandidateConsumer::kReturned, CandidateConsumer::kOther,
                   CandidateConsumer::kUnused })
            {
                if ((mask & ConsumerBit(consumer)) != 0) {
                    return consumer;
                }
            }
            return CandidateConsumer::kOther;
        }

        struct StatisticsState
        {
            std::mutex mutex;
            bool enabled = false;
            CobraStatistics totals;
        };

        // Never destroyed: worker threads report into it (see `Speculation`).
        StatisticsState &Stats() {
            static auto *state = new StatisticsState;
            return *state;
        }

        // The verification ledger of `CobraStats.h`. Separate from the report's
        // state because it is kept when the report is off.
        struct VerificationState
        {
            std::mutex mutex;
            VerificationCounters counters;
        };

        VerificationState &VerificationLedger() {
            static auto *state = new VerificationState;
            return *state;
        }

        template< typename Update > void CountVerification(Update &&update) {
            auto &state = VerificationLedger();
            const std::lock_guard< std::mutex > lock(state.mutex);
            update(state.counters);
        }

        void Merge(CandidateClass &into, const CandidateClass &from) {
            into.candidates += from.candidates;
            into.solved += from.solved;
            into.nanoseconds += from.nanoseconds;
        }

        // A run accumulates into its own copy and merges once, so nothing on
        // the hot path touches shared state.
        void Accumulate(CobraStatistics &into, const CobraStatistics &run);

        void MergeStatistics(const CobraStatistics &run) {
            auto &state = Stats();
            const std::lock_guard< std::mutex > lock(state.mutex);
            Accumulate(state.totals, run);
        }

        void Accumulate(CobraStatistics &into, const CobraStatistics &run) {
            into.runs += run.runs;
            into.nanoseconds += run.nanoseconds;
            into.detect_nanoseconds += run.detect_nanoseconds;
            into.ladder_nanoseconds += run.ladder_nanoseconds;
            into.solve_nanoseconds += run.solve_nanoseconds;
            into.rebuild_nanoseconds += run.rebuild_nanoseconds;
            into.detected += run.detected;
            into.requeued += run.requeued;
            into.dead_roots += run.dead_roots;
            into.cache_hits += run.cache_hits;
            into.solved += run.solved;
            into.rejected_unsolved += run.rejected_unsolved;
            into.rejected_cost += run.rejected_cost;
            into.rejected_budget += run.rejected_budget;
            into.rewrites_reusing += run.rewrites_reusing;
            into.reused_instructions += run.reused_instructions;
            into.rewrites_fit_by_reuse += run.rewrites_fit_by_reuse;
            into.rejected_cached += run.rejected_cached;
            into.rejected_unverified += run.rejected_unverified;
            into.solved_nanoseconds += run.solved_nanoseconds;
            into.rejected_nanoseconds += run.rejected_nanoseconds;
            into.cache_nanoseconds += run.cache_nanoseconds;
            into.reaching_pointer_offset += run.reaching_pointer_offset;
            into.solved_reaching_pointer_offset += run.solved_reaching_pointer_offset;
            into.pointer_offset_nanoseconds += run.pointer_offset_nanoseconds;
            into.search_nanoseconds += run.search_nanoseconds;
            into.search_found += run.search_found;
            into.search_found_nanoseconds += run.search_found_nanoseconds;
            into.leaf_identities += run.leaf_identities;
            into.boolean_trees += run.boolean_trees;
            into.boolean_conditions += run.boolean_conditions;
            into.boolean_merged += run.boolean_merged;
            into.boolean_undecided += run.boolean_undecided;
            into.boolean_found += run.boolean_found;
            into.free_leaf_trees += run.free_leaf_trees;
            into.free_leaf_found += run.free_leaf_found;
            into.keyed_leaf_trees += run.keyed_leaf_trees;
            into.keyed_leaf_found += run.keyed_leaf_found;
            into.enumeration_nanoseconds += run.enumeration_nanoseconds;
            into.ring_attempted += run.ring_attempted;
            into.ring_proved += run.ring_proved;
            into.ring_nanoseconds += run.ring_nanoseconds;
            into.reader_flags_dropped += run.reader_flags_dropped;
            into.demanded_grew += run.demanded_grew;
            into.unreachable_roots += run.unreachable_roots;
            into.unavailable_leaves += run.unavailable_leaves;
            into.stale_trees += run.stale_trees;
            into.declined_no_expression += run.declined_no_expression;
            into.background_withdrawn_unneeded += run.background_withdrawn_unneeded;
            into.unknown_starved += run.unknown_starved;
            into.solver_calls += run.solver_calls;
            into.solver_nanoseconds += run.solver_nanoseconds;
            into.solver_proved_nanoseconds += run.solver_proved_nanoseconds;
            into.solver_refuted_nanoseconds += run.solver_refuted_nanoseconds;
            into.solver_unknown_nanoseconds += run.solver_unknown_nanoseconds;
            into.solver_unknown_skipped += run.solver_unknown_skipped;
            into.declined_loop_carried += run.declined_loop_carried;
            into.proved_over_budget += run.proved_over_budget;
            into.proved_over_budget_nanoseconds += run.proved_over_budget_nanoseconds;
            into.cached_over_budget += run.cached_over_budget;
            into.resolved_after_eviction += run.resolved_after_eviction;
            into.resolved_after_eviction_nanoseconds += run.resolved_after_eviction_nanoseconds;
            into.demanded_builds += run.demanded_builds;
            into.demanded_nanoseconds += run.demanded_nanoseconds;
            into.demanded_local_settled += run.demanded_local_settled;
            into.demanded_local_regions += run.demanded_local_regions;
            into.demanded_local_nodes += run.demanded_local_nodes;
            into.demanded_local_declined += run.demanded_local_declined;
            into.demanded_check_compared += run.demanded_check_compared;
            into.demanded_check_mismatches += run.demanded_check_mismatches;
            into.boundary_nanoseconds += run.boundary_nanoseconds;
            into.shared_nanoseconds += run.shared_nanoseconds;
            into.recut_nanoseconds += run.recut_nanoseconds;
            into.inner_nanoseconds += run.inner_nanoseconds;
            into.slice_nanoseconds += run.slice_nanoseconds;
            into.slice_asked += run.slice_asked;
            into.slice_proved += run.slice_proved;
            into.slice_refuted += run.slice_refuted;
            into.slice_unknown += run.slice_unknown;
            into.slice_claim_failed += run.slice_claim_failed;
            into.sum_group_nanoseconds += run.sum_group_nanoseconds;
            into.sum_group_roots += run.sum_group_roots;
            into.sum_group_split += run.sum_group_split;
            into.sum_group_groups += run.sum_group_groups;
            into.background_submitted += run.background_submitted;
            into.background_solved += run.background_solved;
            into.background_unknown += run.background_unknown;
            into.background_unknown_nanoseconds += run.background_unknown_nanoseconds;
            into.after_background_unknown_proved += run.after_background_unknown_proved;
            into.after_background_unknown_refuted += run.after_background_unknown_refuted;
            into.after_background_unknown_unknown += run.after_background_unknown_unknown;
            into.after_background_unknown_nanoseconds += run.after_background_unknown_nanoseconds;
            into.background_proofs_skipped += run.background_proofs_skipped;
            into.background_settled += run.background_settled;
            into.background_withdrawn += run.background_withdrawn;
            into.background_wait_nanoseconds += run.background_wait_nanoseconds;
            into.background_drain_nanoseconds += run.background_drain_nanoseconds;
            into.background_nanoseconds += run.background_nanoseconds;
            for (unsigned index = 0;
                 index < static_cast< unsigned >(CandidateConsumer::kCount); ++index)
            {
                Merge(into.by_consumer[index], run.by_consumer[index]);
            }
            for (unsigned index = 0; index < static_cast< unsigned >(CandidateOrigin::kCount);
                 ++index)
            {
                Merge(into.by_origin[index], run.by_origin[index]);
            }
            for (unsigned index = 0; index <= CobraStatistics::kMaxVariableRow; ++index) {
                Merge(into.by_variables[index], run.by_variables[index]);
            }
            for (unsigned index = 0; index < 2; ++index) {
                Merge(into.by_loop_phi[index], run.by_loop_phi[index]);
                Merge(into.by_loop_phi_constant_leaves[index], run.by_loop_phi_constant_leaves[index]);
                for (unsigned consumer = 0;
                     consumer < static_cast< unsigned >(CandidateConsumer::kCount); ++consumer)
                {
                    Merge(
                        into.by_loop_phi_consumer[index][consumer],
                        run.by_loop_phi_consumer[index][consumer]
                    );
                }
            }
        }

        // ---- where the time goes ------------------------------------------
        //
        // A single `CobraPass::run` on one obfuscated function has been
        // measured at over two minutes, and per-pass timing outside CoBRA can
        // only say that much: it cannot name the expression the time went into,
        // and without the expression there is nothing to fix.
        //
        // Why the last candidate this thread proposed was turned down, for the
        // slow report. A rejection is otherwise one word, and the word does
        // not say whether the search found nothing, found something the cost
        // gate refused, or was never run.
        std::string &RejectReason() {
            static thread_local std::string reason;
            return reason;
        }

        // Whether the last search this thread ran turned something down on a
        // proof that did not finish - two conditions the boolean reading could
        // not prove the same, say. A search that then finds nothing has not
        // shown there is nothing to find, so its "nothing" is neither cached
        // nor written on the root: both would make one timeout permanent.
        bool &RestedOnUnknown() {
            static thread_local bool unknown = false;
            return unknown;
        }

        // With `COBRA_SLOW_REPORT` set to a directory, every candidate whose
        // solve costs at least `COBRA_SLOW_MS` milliseconds (default 100) is
        // recorded there - what went in, what came out, what it cost, and the
        // shape of the tree - and the IR it was read from is written beside it
        // as a module holding one function, so the case can be re-run on its
        // own instead of by reproducing the workload that found it.
        struct SlowCandidateReport
        {
            bool enabled = false;
            std::string directory;
            uint64_t threshold_ms = 100;
            // Record a candidate holding at least this many variables whatever
            // it cost. Zero leaves the cost as the only trigger.
            uint32_t variable_floor = 0;

            // With `COBRA_ADDRESS_REPORT` set to a directory, every rewrite of
            // an expression that reaches an address is written there too.
            // Counting them says how much of the pass's time goes into
            // addresses; it does not say what they look like, and that is what
            // a cheaper pass dedicated to addresses has to be built against.
            std::string addresses;
            uint64_t address_cap = 200;
        };

        const SlowCandidateReport &SlowReport() {
            static const SlowCandidateReport report = [] {
                SlowCandidateReport out;
                if (const char *where = std::getenv("COBRA_ADDRESS_REPORT")) {
                    if (*where != '\0') {
                        out.addresses = where;
                        llvm::sys::fs::create_directories(out.addresses, true);
                        if (const char *cap = std::getenv("COBRA_ADDRESS_CAP")) {
                            out.address_cap = std::strtoull(cap, nullptr, 10);
                        }
                    }
                }
                const char *directory = std::getenv("COBRA_SLOW_REPORT");
                if (directory == nullptr || *directory == '\0') {
                    return out;
                }
                out.enabled   = true;
                out.directory = directory;
                llvm::sys::fs::create_directories(out.directory + "/verify", true);
                llvm::sys::fs::create_directories(out.directory + "/budget", true);
                if (const char *threshold = std::getenv("COBRA_SLOW_MS")) {
                    out.threshold_ms = std::strtoull(threshold, nullptr, 10);
                }
                if (const char *floor = std::getenv("COBRA_VARS_REPORT")) {
                    out.variable_floor = static_cast< uint32_t >(std::strtoul(floor, nullptr, 10));
                }
                return out;
            }();
            return report;
        }

        // `COBRA_REWRITE_LOG=<file>`: every rewrite the pass applies, one line
        // each - the root it replaces, the expression in and out, what the
        // rewrite emitted against its budget, and whether it was proved - so two
        // runs can be compared rewrite by rewrite.
        void LogRewrite(
            const MBACandidate &cand, const CandidateOutcome &outcome, size_t emitted, uint32_t budget
        ) {
            static const std::string path = [] {
                const char *where = std::getenv("COBRA_REWRITE_LOG");
                return where == nullptr ? std::string{} : std::string(where);
            }();
            if (path.empty()) {
                return;
            }
            static std::mutex mutex;
            const std::lock_guard< std::mutex > lock(mutex);
            std::error_code code;
            llvm::raw_fd_ostream log(
                path, code, llvm::sys::fs::CD_OpenAlways, llvm::sys::fs::FA_Write,
                llvm::sys::fs::OF_Append | llvm::sys::fs::OF_Text
            );
            if (code) {
                return;
            }
            // By position, not by `real_vars`: an outcome served from the cache
            // carries the names of the candidate that first solved it.
            std::vector< std::string > names = cand.var_names;
            if (!outcome.real_var_positions.empty()) {
                names.clear();
                for (const uint32_t position : outcome.real_var_positions) {
                    names.push_back(
                        position < cand.var_names.size() ? cand.var_names[position] : "?"
                    );
                }
            }
            log << cand.root->getFunction()->getName() << " | emitted " << emitted << " of " << budget
                << (outcome.accepted_unknown ? " | unknown accepted" : "") << " | in "
                << (cand.expr ? RenderSSA(*cand.expr, cand.var_names) : std::string("?")) << " | out "
                << (outcome.expr ? RenderSSA(*outcome.expr, names) : std::string("?")) << "\n";
        }

        std::mutex &SlowReportMutex() {
            static std::mutex mutex;
            return mutex;
        }

        std::string PrintedValue(const llvm::Value *value) {
            std::string text;
            llvm::raw_string_ostream stream(text);
            value->print(stream);
            return text;
        }

        // The candidate's tree, as a module with one function: the leaves become
        // arguments and the root becomes the return value, so the expression can
        // be handed to `opt` or to CoBRA's own driver on its own.
        //
        // A tree that reaches across basic blocks is not written - a phi cannot
        // be cloned into a straight line, and the detector only ever looks
        // through one when every arm agrees - but it is still reported, with the
        // reason.
        std::string WriteCandidateModule(
            const MBACandidate &cand, const std::string &path, std::string &note
        ) {
            llvm::DenseMap< const llvm::Value *, unsigned > leaf_index;
            for (unsigned index = 0; index < cand.leaf_values.size(); ++index) {
                leaf_index.try_emplace(cand.leaf_values[index], index);
            }

            // The cone, deepest first, stopping at the leaves.
            std::vector< llvm::Instruction * > cone;
            unsigned promoted = 0;
            llvm::DenseSet< llvm::Instruction * > seen;
            std::vector< std::pair< llvm::Instruction *, bool > > work{
                { cand.root, false }
            };
            auto *block = cand.root->getParent();
            while (!work.empty()) {
                auto [inst, expanded] = work.back();
                work.pop_back();
                if (expanded) {
                    cone.push_back(inst);
                    continue;
                }
                if (!seen.insert(inst).second) {
                    continue;
                }
                work.push_back({ inst, true });
                for (auto &operand : inst->operands()) {
                    auto *value = operand.get();
                    if (leaf_index.contains(value) || llvm::isa< llvm::Constant >(value)) {
                        continue;
                    }
                    // Anything the root's own block does not compute becomes an
                    // argument: a value from another block, and a phi, which is
                    // where the tree left the block in the first place. The
                    // expression is then a straight line that can be run on its
                    // own, and `promoted` says how much of it came from outside.
                    if (auto *operand_inst = llvm::dyn_cast< llvm::Instruction >(value)) {
                        if (operand_inst->getParent() == block
                            && !llvm::isa< llvm::PHINode >(operand_inst))
                        {
                            work.push_back({ operand_inst, false });
                            continue;
                        }
                        ++promoted;
                    }
                    leaf_index.try_emplace(value, static_cast< unsigned >(leaf_index.size()));
                }
            }

            // One argument per leaf, in a stable order.
            std::vector< const llvm::Value * > leaves(leaf_index.size(), nullptr);
            for (const auto &entry : leaf_index) {
                if (entry.second < leaves.size()) {
                    leaves[entry.second] = entry.first;
                }
            }
            std::vector< llvm::Type * > argument_types;
            for (const auto *leaf : leaves) {
                argument_types.push_back(
                    leaf != nullptr ? leaf->getType() : cand.root->getType()
                );
            }

            auto &context = cand.root->getContext();
            auto module   = std::make_unique< llvm::Module >("cobra.slow", context);
            module->setDataLayout(cand.root->getModule()->getDataLayout());
            module->setTargetTriple(cand.root->getModule()->getTargetTriple());
            auto *type = llvm::FunctionType::get(cand.root->getType(), argument_types, false);
            auto *function =
                llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage, "cobra_slow", module.get());
            auto *entry = llvm::BasicBlock::Create(context, "entry", function);
            llvm::IRBuilder<> builder(entry);

            llvm::DenseMap< const llvm::Value *, llvm::Value * > mapping;
            for (unsigned index = 0; index < leaves.size(); ++index) {
                if (leaves[index] != nullptr) {
                    mapping[leaves[index]] = function->getArg(index);
                }
            }

            llvm::Value *result = nullptr;
            for (auto *inst : cone) {
                auto *clone = inst->clone();
                clone->setName("");
                for (unsigned index = 0; index < clone->getNumOperands(); ++index) {
                    auto *operand = clone->getOperand(index);
                    if (llvm::isa< llvm::Constant >(operand)) {
                        continue;
                    }
                    const auto mapped = mapping.find(operand);
                    if (mapped == mapping.end()) {
                        clone->deleteValue();
                        note = "tree reads a value the cone does not cover";
                        return {};
                    }
                    clone->setOperand(index, mapped->second);
                }
                builder.Insert(clone);
                mapping[inst] = clone;
                result        = clone;
            }
            if (result == nullptr) {
                note = "empty tree";
                return {};
            }
            builder.CreateRet(result);

            std::error_code code;
            llvm::raw_fd_ostream out(path, code);
            if (code) {
                note = "could not write: " + code.message();
                return {};
            }
            module->print(out, nullptr);
            note = std::to_string(promoted) + " value(s) promoted to arguments, "
                 + std::to_string(cone.size()) + " instruction(s) in line";
            return path;
        }

        void RecordCandidate(
            const std::string &directory, uint64_t &sequence, const llvm::Function &f,
            const MBACandidate &cand, const CandidateOutcome *outcome, uint64_t milliseconds,
            const char *verdict
        ) {
            const std::lock_guard< std::mutex > lock(SlowReportMutex());
            const uint64_t index = ++sequence;

            std::string note;
            const std::string module_path = WriteCandidateModule(
                cand, directory + "/cobra-slow-" + std::to_string(index) + ".ll", note
            );

            std::error_code code;
            llvm::raw_fd_ostream log(
                directory + "/cobra-slow.log", code, llvm::sys::fs::CD_OpenAlways,
                llvm::sys::fs::FA_Write, llvm::sys::fs::OF_Append | llvm::sys::fs::OF_Text
            );
            if (code) {
                return;
            }
            log << "## candidate " << index << "\n";
            log << "milliseconds   " << milliseconds << "\n";
            log << "verdict        " << verdict << "\n";
            if (!RejectReason().empty()) {
                log << "reason         " << RejectReason() << "\n";
            }
            log << "signature      [";
            for (size_t i = 0; i < cand.sig.size(); ++i) {
                log << (i ? ", " : "") << cand.sig[i];
            }
            log << "]\n";
            log << "function       " << f.getName() << "\n";
            log << "bitwidth       " << cand.bitwidth << "\n";
            log << "tree-size      " << cand.tree_size << "\n";
            log << "dying-count    " << cand.dying_count << "\n";
            if (IsSharedCutLimit(cand.node_limit)) {
                log << "node-limit     shared values, rung " << kSharedCutLimit - cand.node_limit
                    << "\n";
            } else {
                log << "node-limit     " << cand.node_limit << "\n";
            }
            log << "variables      " << cand.var_names.size() << "\n";
            if (cand.expr) {
                // The tree as the solve reads it: its node count, which can be far
                // above `tree-size` once the IR's sharing is expanded, and whether
                // a comparison in it makes the proof the only check there is.
                log << "expr-nodes     " << CountNodes(*cand.expr)
                    << (ContainsComparison(*cand.expr) ? ", holds a comparison" : "") << "\n";
            }
            log << "consumer       " << NameOf(PrimaryConsumer(ConsumerMask(cand.root))) << "\n";
            log << "root           " << PrintedValue(cand.root) << "\n";
            if (cand.expr) {
                log << "expression-in  " << RenderSSA(*cand.expr, cand.var_names) << "\n";
            }
            if (outcome != nullptr && outcome->expr) {
                const auto &names = outcome->real_vars.empty() ? cand.var_names : outcome->real_vars;
                log << "expression-out " << RenderSSA(*outcome->expr, names) << "\n";
            } else {
                log << "expression-out <none>\n";
            }
            if (!module_path.empty()) {
                log << "module         " << module_path << "\n";
                if (!note.empty()) {
                    log << "cone           " << note << "\n";
                }
            } else if (!note.empty()) {
                log << "module         <not written: " << note << ">\n";
            }
            log << "\n";
        }

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
        // FNV-1a: unlike `std::hash`, the same in every build, for what a key
        // or a tag outlives the process in (a saved memo, the IR).
        uint64_t StableHash(std::string_view text) {
            uint64_t hash = 0xCBF29CE484222325ULL;
            for (const unsigned char c : text) {
                hash = (hash ^ c) * 0x100000001B3ULL;
            }
            return hash;
        }

        std::string CandidateKey(const MBACandidate &cand, const CobraPassOptions &options) {
            std::string key;
            key += std::to_string(cand.bitwidth);
            key += '|';
            key += std::to_string(cand.demanded_mask);
            key += '|';
            key += std::to_string(options.max_vars);
            // Every number is followed by a separator, so no two settings
            // spell the same key: a key is also what a saved memo of unknown
            // proofs (`SaveUnknownMemo`) is matched by in a later process.
            key += options.z3_verify ? "|z" : "|-";
            key += std::to_string(options.z3_settings.timeout_ms);
            key += ',';
            key += std::to_string(static_cast< int >(options.z3_settings.unknown_result_mode));
            key += ',';
            key += std::to_string(options.z3_settings.rewrite_level);
            key += ',';
            key += std::to_string(static_cast< int >(options.z3_settings.strategy));
            key += options.z3_settings.sweep ? ",s," : ",,";
            key += std::to_string(static_cast< uint64_t >(options.enabled_families));
            key += options.boolean_variables ? "|b" : "|";
            // The boolean cut is searched by the boolean reading alone, so its
            // verdict is not the one an identical tree read the ordinary way has.
            key += cand.node_limit == kBooleanCutLimit ? "B" : "";
            key += ':';
            key += std::to_string(StableHash(options.z3_settings.llvm_passes));
            key += '|';
            key += std::to_string(cand.var_names.size());
            key += '|';
            // The signature is the tree's values on the Boolean cube, so the
            // expression below already determines it, and it is 2^variables
            // numbers long: written out in full it was most of every key past
            // a handful of variables, copied a dozen times per candidate and
            // held in every cache entry. A key without an expression is never
            // shared (below), so there it has nothing to tell apart either.
            if (cand.expr != nullptr) {
                AppendExprKey(*cand.expr, key);
            } else {
                // Without an expression the key is the signature alone, which
                // two different trees can share - and with the solver off a
                // rewrite found for one would be served to the other unprobed.
                // Such a key is never shared.
                static std::atomic< uint64_t > unshared{ 0 };
                key += "|unshared:";
                key += std::to_string(unshared.fetch_add(1, std::memory_order_relaxed));
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

            // An entry served from the old generation is moved into the young
            // one, so that what is still being asked for survives the next
            // rotation instead of being dropped with what is not.
            std::optional< Entry > Find(const std::string &key) {
                const std::lock_guard< std::mutex > lock(mutex_);
                if (auto it = young_.find(key); it != young_.end()) {
                    return it->second;
                }
                if (auto it = old_.find(key); it != old_.end()) {
                    Entry entry = it->second;
                    old_.erase(it);
                    if (young_.size() >= kGeneration) {
                        old_ = std::move(young_);
                        young_.clear();
                    }
                    young_.emplace(key, entry);
                    return entry;
                }
                return std::nullopt;
            }

            Entry Insert(const std::string &key, Entry entry) {
                // `COBRA_CACHE_DEBUG=1`: what is being remembered, and why.
                // A candidate the loop reports as `rejected (cached)` was
                // turned down somewhere the per-candidate report does not
                // reach, and without this there is no way to see where.
                static const bool trace = std::getenv("COBRA_CACHE_DEBUG") != nullptr;
                if (trace) {
                    llvm::errs() << "cobra: cache insert " << (entry == nullptr ? "rejected" : "solved")
                                 << (RejectReason().empty() ? "" : " (" + RejectReason() + ")") << " key "
                                 << key << "\n";
                }
                const std::lock_guard< std::mutex > lock(mutex_);
                // A solved entry is a rewrite something proved; a null entry is
                // only "nothing was found this time". The two are not
                // interchangeable, and the same key receives both - the loop
                // and a worker can each reach the same expression, and the
                // search is not a function of the key alone once a proof can
                // time out. Letting the second overwrite the first threw away
                // a proved rewrite and replaced it with a refusal that every
                // later encounter was then served.
                //
                // The same holds one step up: a rewrite proved is worth more
                // than one only proposed, which the next encounter has to prove
                // again - and, if that proof runs out of time where the first
                // did not, loses. So an entry is only ever replaced by one that
                // knows more: proved over proposed over refused.
                const auto rank = [](const Entry &e) { return e == nullptr ? 0 : e->verified ? 2 : 1; };
                for (auto *table : { &young_, &old_ }) {
                    if (auto it = table->find(key); it != table->end() && rank(it->second) > rank(entry)) {
                        return it->second;
                    }
                }
                if (young_.size() >= kGeneration) {
                    old_ = std::move(young_);
                    young_.clear();
                }
                young_.insert_or_assign(key, entry);
                // Report-only bookkeeping; bounded like the entries themselves.
                if (ever_inserted_.size() >= kMaxTracked) {
                    ever_inserted_.clear();
                }
                ever_inserted_.insert(std::hash< std::string >{}(key));
                return entry;
            }

            // Whether `key` was inserted once and has since been dropped. Only
            // the report asks, to say what the size limit costs in re-solves.
            bool WasEvicted(const std::string &key) const {
                const std::lock_guard< std::mutex > lock(mutex_);
                return !young_.contains(key) && !old_.contains(key)
                    && ever_inserted_.contains(std::hash< std::string >{}(key));
            }

            // ---- proofs that did not finish ------------------------------------------
            //
            // A third state, and it has to be a third state. A proof the solver could not
            // finish says nothing about the rewrite, so it must never become a `nullptr`
            // entry: that is a refusal, and every later encounter would be served it. But
            // leaving *no* trace at all - which is what this did - means the identical
            // query is asked again on the next run, and the next, each time running the
            // whole budget out.
            //
            // Measured by replaying one large obfuscated function through a host's
            // optimization pipeline, 11 runs: of 128.41 s spent in the solver, **121.49 s was
            // spent on queries that came back UNKNOWN** - and `cobra-unknown-as-unsat` is
            // off, so not one of them could ever have produced a rewrite. The same
            // expressions, every run.
            //
            // So the question is remembered as having run out of time, together with the
            // budget it ran out of. `CandidateKey` mixes in `z3_settings.timeout_ms`, but the
            // proof's budget is that times a scale read from the candidate's IR tree size
            // (`ProofBudgetScale`), which the key does not describe: two candidates with the
            // same key can be given different budgets. A question is therefore skipped only
            // when it already ran out of a budget at least as large as the one it would be
            // given now, which is what keeps the original concern ("one timeout and the tree
            // is never examined again, however much budget a later run would have given it")
            // true. Nothing is written into the IR, so nothing travels with it, and the entry
            // is never served as an outcome - `Find` cannot see it.
            void RecordUnknown(const std::string &key, uint32_t scale) {
                const std::lock_guard< std::mutex > lock(mutex_);
                if (unknown_.size() >= kMaxUnknown) {
                    unknown_.clear();
                }
                auto &recorded = unknown_[key];
                recorded       = std::max(recorded, scale);
            }

            // Records that the rewrite cached under `key` was refuted: the
            // enumeration or the solver found an input it gets wrong.
            //
            // `Insert` will not let a refusal displace a non-null entry, which
            // is right for a *proved* rewrite and wrong for the unverified
            // proposal the proof just refuted: kept, it is served to the next
            // candidate with this key, which rebuilds the rewrite and proves it
            // wrong again, and the one after that. A refutation is definite, so
            // it replaces the proposal; a proved rewrite is still never
            // displaced.
            void Refute(const std::string &key) {
                {
                    const std::lock_guard< std::mutex > lock(mutex_);
                    for (auto *generation : { &young_, &old_ }) {
                        if (auto it = generation->find(key);
                            it != generation->end() && it->second != nullptr && !it->second->verified)
                        {
                            it->second = nullptr;
                        }
                    }
                }
                Insert(key, nullptr);
            }

            // Whether a proof of this exact question has already run out of a budget at
            // least as large as `scale` gives it.
            bool WasUnknown(const std::string &key, uint32_t scale) const {
                const std::lock_guard< std::mutex > lock(mutex_);
                const auto found = unknown_.find(key);
                return found != unknown_.end() && found->second >= scale;
            }

            // Which of those a worker thread recorded, for the report alone: it says
            // whether the loop, had it asked again, would have done any better.
            // The memo of questions that ran out of budget, for `SaveUnknownMemo`.
            std::vector< std::pair< std::string, uint32_t > > Unknowns() const {
                const std::lock_guard< std::mutex > lock(mutex_);
                return { unknown_.begin(), unknown_.end() };
            }

            std::vector< std::string > BackgroundUnknowns() const {
                const std::lock_guard< std::mutex > lock(mutex_);
                return { background_unknown_.begin(), background_unknown_.end() };
            }

            void RestoreUnknowns(
                const std::vector< std::pair< std::string, uint32_t > > &unknowns,
                const std::vector< std::string > &background
            ) {
                const std::lock_guard< std::mutex > lock(mutex_);
                unknown_.clear();
                unknown_.insert(unknowns.begin(), unknowns.end());
                background_unknown_.clear();
                background_unknown_.insert(background.begin(), background.end());
            }

            void MarkBackgroundUnknown(const std::string &key) {
                const std::lock_guard< std::mutex > lock(mutex_);
                if (background_unknown_.size() >= kMaxUnknown) {
                    background_unknown_.clear();
                }
                background_unknown_.insert(key);
            }

            bool WasBackgroundUnknown(const std::string &key) const {
                const std::lock_guard< std::mutex > lock(mutex_);
                return background_unknown_.contains(key);
            }

          private:
            // Bound the footprint on very large modules. The cache is two
            // generations: when the young one fills up it becomes the old one
            // and the previous old one is dropped, so an entry survives at
            // least one whole generation after it was written. Clearing
            // everything at once, as the cache used to, dropped the entries
            // the pass was about to ask for together with the stale ones - over
            // one long workload a third of every solve was a candidate solved
            // before and forgotten. An entry is a key of a few hundred bytes
            // and a small expression, so this is tens of megabytes at most.
            static constexpr size_t kGeneration = 32768;
            static constexpr size_t kMaxTracked = size_t{ 1 } << 20;

            // Dropped whole when it fills rather than aged in generations: an entry here
            // only ever saves a timeout, so losing one costs the budget once instead of
            // costing a rewrite.
            static constexpr size_t kMaxUnknown = size_t{ 1 } << 18;

            mutable std::mutex mutex_;
            std::unordered_map< std::string, Entry > young_;
            std::unordered_map< std::string, Entry > old_;
            std::unordered_set< size_t > ever_inserted_;
            // The questions whose proof ran out of its budget, by the same content key the
            // outcomes use. Deliberately not an `Entry`: an outcome is served to callers
            // and a `nullptr` outcome is a refusal, which this must never become.
            // The budget scale each one ran out of (see `RecordUnknown`).
            std::unordered_map< std::string, uint32_t > unknown_;
            std::unordered_set< std::string > background_unknown_;
        };

        // Never destroyed: worker threads write to it (see `Speculation`).
        OutcomeCache &SharedOutcomeCache() {
            static auto *cache = new OutcomeCache;
            return *cache;
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
            tag          = mix(tag, options.max_recut_nodes);
            tag          = mix(tag, options.max_recut_vars);
            tag          = mix(tag, options.max_shared_cuts);
            tag          = mix(tag, options.max_tree_nodes);
            tag          = mix(tag, options.z3_verify ? 1 : 0);
            tag          = mix(tag, options.z3_settings.timeout_ms);
            tag = mix(tag, static_cast< uint64_t >(options.z3_settings.unknown_result_mode));
            tag = mix(tag, options.z3_settings.rewrite_level);
            tag = mix(tag, static_cast< uint64_t >(options.z3_settings.strategy));
            tag = mix(tag, options.z3_settings.sweep ? 1 : 0);
            tag = mix(tag, static_cast< uint64_t >(options.enabled_families));
            tag = mix(tag, static_cast< uint64_t >(options.cost_model));
            tag = mix(tag, StableHash(options.z3_settings.llvm_passes));
            tag = mix(tag, static_cast< uint64_t >(options.leaf_freezing));
            tag = mix(tag, options.boolean_variables ? 1 : 0);
            // What the records mean changes with the pass itself as well as
            // with its settings: a host that keeps `!cobra.mba` on IR it saves
            // and loads again must not read a verdict an older build wrote
            // under another fingerprint or another set of techniques. Bumped
            // whenever either changes.
            constexpr uint64_t kRecordVersion = 4;
            tag = mix(tag, kRecordVersion);
            return tag;
        }

        // Why a solve ended the way it did. The two rejections are worth
        // telling apart in the report: one says the search found nothing, the
        // other says it found something and the answer was no smaller, and
        // only the first is a missing capability.
        enum class SolveVerdict : uint8_t { kSolved, kUnsolved, kNotCheaper };


        // How many bits of each variable `expr` can see, read off the tree. A
        // leaf narrower than the tree enters it as `var & (2^w - 1)` - that is
        // how the detector models the extension that carries it up - so a
        // variable met only under such a mask has `w` bits that matter and no
        // others. Met anywhere else it is read in full. The answer depends on
        // the tree alone, which keeps it inside what the outcome cache keys on.
        void ReadWidths(const Expr &expr, uint32_t bitwidth, std::vector< uint32_t > &widths) {
            const auto note = [&](uint32_t variable, uint32_t width) {
                if (variable >= widths.size()) {
                    widths.resize(variable + 1, 0);
                }
                widths[variable] = std::max(widths[variable], width);
            };
            const auto narrow_mask = [&](const Expr &node) -> uint32_t {
                const uint64_t mask = node.constant_val;
                if (node.kind != Expr::Kind::kConstant || mask == 0 || (mask & (mask + 1)) != 0) {
                    return 0;
                }
                return std::min(static_cast< uint32_t >(std::popcount(mask)), bitwidth);
            };

            std::vector< const Expr * > pending{ &expr };
            while (!pending.empty()) {
                const Expr *node = pending.back();
                pending.pop_back();
                if (node->kind == Expr::Kind::kVariable) {
                    note(node->var_index, bitwidth);
                    continue;
                }
                if (node->kind == Expr::Kind::kAnd && node->children.size() == 2) {
                    const Expr &lhs = *node->children[0];
                    const Expr &rhs = *node->children[1];
                    if (lhs.kind == Expr::Kind::kVariable && narrow_mask(rhs) != 0) {
                        note(lhs.var_index, narrow_mask(rhs));
                        continue;
                    }
                    if (rhs.kind == Expr::Kind::kVariable && narrow_mask(lhs) != 0) {
                        note(rhs.var_index, narrow_mask(lhs));
                        continue;
                    }
                }
                for (const auto &child : node->children) {
                    pending.push_back(child.get());
                }
            }
        }

        // Settles `lhs == rhs` by running both over every input, when there are
        // few enough of them to do that. Probes cannot stand in for this: a
        // product masked down to one bit is an equality test in disguise -
        // `((v ^ K) * (2^34 - 4)) & 2^33` is `v != K` - which differs from a
        // constant at one input in 2^16, and neither the Boolean cube nor a
        // random point ever lands on it. Nor can a solver be relied on to: it
        // ran out of its budget on exactly that tree, a host that takes an
        // unknown answer for a yes then accepted the constant, and `K` was one
        // of the only two values the variable ever held - the flag an
        // obfuscated conditional jump tests. Sixty-five thousand evaluations cost less
        // than the solver's start-up and leave nothing to believe.
        //
        // Empty when the inputs are too many to walk - by count, or by count
        // times the size of the two sides: a million inputs over a tree the
        // detector expanded to thousands of nodes is billions of evaluations,
        // past any budget the solver it stands in for would have been given.
        std::optional< bool > DecideByEnumeration(
            const Expr &lhs, const Expr &rhs, uint32_t num_vars, uint32_t bitwidth
        ) {
            constexpr uint32_t kMaxInputBits   = 20;
            constexpr uint64_t kMaxEvaluations = uint64_t{ 1 } << 27;

            std::vector< uint32_t > widths(num_vars, 0);
            ReadWidths(lhs, bitwidth, widths);
            ReadWidths(rhs, bitwidth, widths);
            uint32_t input_bits = 0;
            for (const uint32_t width : widths) {
                input_bits += width;
                if (input_bits > kMaxInputBits) {
                    return std::nullopt;
                }
            }

            const uint64_t nodes = CountNodes(lhs) + CountNodes(rhs);
            if ((nodes << input_bits) > kMaxEvaluations) {
                return std::nullopt;
            }

            const auto compiled_lhs = CompileExpr(lhs, bitwidth);
            const auto compiled_rhs = CompileExpr(rhs, bitwidth);
            std::vector< uint64_t > values(widths.size(), 0);
            std::vector< uint64_t > stack_lhs;
            std::vector< uint64_t > stack_rhs;
            const uint64_t inputs = uint64_t{ 1 } << input_bits;
            for (uint64_t input = 0; input < inputs; ++input) {
                uint64_t rest = input;
                for (size_t variable = 0; variable < widths.size(); ++variable) {
                    values[variable] = rest & Bitmask(widths[variable]);
                    rest >>= widths[variable];
                }
                if (EvalCompiledExpr(compiled_lhs, values, stack_lhs)
                    != EvalCompiledExpr(compiled_rhs, values, stack_rhs))
                {
                    return false;
                }
            }
            return true;
        }

#ifdef COBRA_HAS_Z3
        // `CobraPassOptions::boolean_variables`: a tree read with every condition
        // in it as a variable that is 0 or 1.
        namespace boolean_reading {

            // At most how many of the low bits of `expr` can be set, on any input
            // of a tree `bw` wide.
            uint32_t SignificantBits(const Expr &expr, uint32_t bw) {
                switch (expr.kind) {
                    case Expr::Kind::kConstant:
                        return static_cast< uint32_t >(std::bit_width(expr.constant_val & Bitmask(bw)));
                    case Expr::Kind::kCmpEq:
                    case Expr::Kind::kCmpUlt:
                    case Expr::Kind::kCmpSlt:
                        return 1;
                    case Expr::Kind::kAnd:
                        return std::min(
                            SignificantBits(*expr.children[0], bw), SignificantBits(*expr.children[1], bw)
                        );
                    case Expr::Kind::kOr:
                    case Expr::Kind::kXor:
                        return std::max(
                            SignificantBits(*expr.children[0], bw), SignificantBits(*expr.children[1], bw)
                        );
                    case Expr::Kind::kShr: {
                        const uint32_t bits = SignificantBits(*expr.children[0], bw);
                        return bits > expr.constant_val ? bits - static_cast< uint32_t >(expr.constant_val) : 0;
                    }
                    default:
                        return bw;
                }
            }

            bool HasVariable(const Expr &expr) {
                if (expr.kind == Expr::Kind::kVariable) {
                    return true;
                }
                return llvm::any_of(expr.children, [](const auto &child) { return HasVariable(*child); });
            }

            // A condition: a comparison, or the top bit of what a logical shift
            // reads - the sign bit of a value of `amount + 1` bits, which is how
            // `lshr x, w - 1`, `ashr x, w - 1` and the low bit of `fshl(a, b, 1)`
            // all read. Either is 0 or 1 on every input. One over constants alone
            // is the constant it folds to, and is left to the search.
            bool IsCondition(const Expr &expr, uint32_t bw) {
                if (!HasVariable(expr)) {
                    return false;
                }
                if (IsComparison(expr.kind)) {
                    return true;
                }
                return expr.kind == Expr::Kind::kShr && expr.constant_val >= 1
                    && SignificantBits(*expr.children[0], bw) == expr.constant_val + 1;
            }

            // The outermost conditions of `expr`, each structurally distinct one
            // once, and where each occurrence stands.
            void CollectConditions(
                const Expr &expr, uint32_t bw, std::vector< const Expr * > &conditions,
                std::unordered_map< const Expr *, uint32_t > &slot_of
            ) {
                if (IsCondition(expr, bw)) {
                    for (uint32_t slot = 0; slot < conditions.size(); ++slot) {
                        if (ExprStructurallyEqual(*conditions[slot], expr)) {
                            slot_of[&expr] = slot;
                            return;
                        }
                    }
                    slot_of[&expr] = static_cast< uint32_t >(conditions.size());
                    conditions.push_back(&expr);
                    return;
                }
                for (const auto &child : expr.children) {
                    CollectConditions(*child, bw, conditions, slot_of);
                }
            }

            // `expr` with each condition read as the variable of its class: the
            // variable masked to its one bit, negated where the condition is the
            // class's negation.
            std::unique_ptr< Expr > Abstract(
                const Expr &expr, const std::unordered_map< const Expr *, uint32_t > &slot_of,
                const std::vector< uint32_t > &representative, const std::vector< bool > &negated,
                uint32_t first_condition
            ) {
                if (const auto found = slot_of.find(&expr); found != slot_of.end()) {
                    const uint32_t slot = found->second;
                    auto variable       = Expr::BitwiseAnd(
                        Expr::Variable(first_condition + representative[slot]), Expr::Constant(1)
                    );
                    if (negated[slot]) {
                        return Expr::BitwiseXor(std::move(variable), Expr::Constant(1));
                    }
                    return variable;
                }
                auto copy = std::make_unique< Expr >();
                copy->kind         = expr.kind;
                copy->constant_val = expr.constant_val;
                copy->var_index    = expr.var_index;
                for (const auto &child : expr.children) {
                    copy->children.push_back(
                        Abstract(*child, slot_of, representative, negated, first_condition)
                    );
                }
                return copy;
            }

            void UsedVariables(const Expr &expr, std::vector< bool > &used) {
                if (expr.kind == Expr::Kind::kVariable) {
                    used.at(expr.var_index) = true;
                    return;
                }
                for (const auto &child : expr.children) {
                    UsedVariables(*child, used);
                }
            }

            // `expr` with variable `v` replaced by `meaning[v]`.
            std::unique_ptr< Expr > Substitute(
                const Expr &expr, const std::vector< std::unique_ptr< Expr > > &meaning
            ) {
                if (expr.kind == Expr::Kind::kVariable) {
                    return CloneExpr(*meaning.at(expr.var_index));
                }
                auto copy = std::make_unique< Expr >();
                copy->kind         = expr.kind;
                copy->constant_val = expr.constant_val;
                copy->var_index    = expr.var_index;
                for (const auto &child : expr.children) {
                    copy->children.push_back(Substitute(*child, meaning));
                }
                return copy;
            }

            bool IsConstant(const Expr &expr, uint64_t value, uint32_t bw) {
                return expr.kind == Expr::Kind::kConstant
                    && (expr.constant_val & Bitmask(bw)) == (value & Bitmask(bw));
            }

            // `expr` without the identities the IR does not spell: `0 + x`,
            // `x ^ 0`, `x | 0`, `1 * x`, `-1 * x` as `-x`, and a condition masked
            // to the one bit it already is. The rebuild reads an existing
            // instruction only where the rewrite applies the same operator to
            // the same operands (`IrReuse`), so a condition put back as
            // `y | (0 + -y)` would be built again beside the `y | -y` the
            // function holds.
            std::unique_ptr< Expr > Tidy(std::unique_ptr< Expr > expr, uint32_t bw) {
                for (auto &child : expr->children) {
                    child = Tidy(std::move(child), bw);
                }
                const auto take = [&](size_t index) { return std::move(expr->children[index]); };
                switch (expr->kind) {
                    case Expr::Kind::kAdd:
                    case Expr::Kind::kOr:
                    case Expr::Kind::kXor:
                        for (size_t side = 0; side < 2; ++side) {
                            if (IsConstant(*expr->children[side], 0, bw)) {
                                return take(1 - side);
                            }
                        }
                        break;
                    case Expr::Kind::kMul:
                        for (size_t side = 0; side < 2; ++side) {
                            if (IsConstant(*expr->children[side], 1, bw)) {
                                return take(1 - side);
                            }
                            if (IsConstant(*expr->children[side], ~uint64_t{ 0 }, bw)) {
                                return Expr::Negate(take(1 - side));
                            }
                        }
                        break;
                    case Expr::Kind::kAnd:
                        for (size_t side = 0; side < 2; ++side) {
                            if (IsConstant(*expr->children[side], 1, bw)
                                && SignificantBits(*expr->children[1 - side], bw) <= 1)
                            {
                                return take(1 - side);
                            }
                        }
                        break;
                    default:
                        break;
                }
                return expr;
            }

            // A function of conditions alone, from its values on the `2^k`
            // assignments of them (`sig`, variable `v` at bit `v`): the sum over
            // the sets of conditions of a coefficient times their conjunction,
            // the coefficients being the Moebius transform of the values. Exact
            // on every assignment of 0s and 1s, which is every value conditions
            // take - so there is nothing to search for.
            std::unique_ptr< Expr > MultilinearForm(
                const std::vector< uint64_t > &sig, uint32_t variables, uint32_t bw
            ) {
                const uint64_t mask = Bitmask(bw);
                std::vector< uint64_t > coefficient(sig.begin(), sig.end());
                for (uint32_t variable = 0; variable < variables; ++variable) {
                    for (size_t set = 0; set < coefficient.size(); ++set) {
                        if ((set >> variable) & 1) {
                            coefficient[set] =
                                (coefficient[set] - coefficient[set ^ (size_t{ 1 } << variable)]) & mask;
                        }
                    }
                }
                std::unique_ptr< Expr > sum;
                for (size_t set = 0; set < coefficient.size(); ++set) {
                    if (coefficient[set] == 0) {
                        continue;
                    }
                    std::unique_ptr< Expr > term;
                    for (uint32_t variable = 0; variable < variables; ++variable) {
                        if ((set >> variable) & 1) {
                            auto factor = Expr::Variable(variable);
                            term = term == nullptr ? std::move(factor)
                                                   : Expr::BitwiseAnd(std::move(term), std::move(factor));
                        }
                    }
                    term = term == nullptr ? Expr::Constant(coefficient[set])
                                           : Expr::Mul(Expr::Constant(coefficient[set]), std::move(term));
                    sum = sum == nullptr ? std::move(term) : Expr::Add(std::move(sum), std::move(term));
                }
                return Tidy(sum == nullptr ? Expr::Constant(0) : std::move(sum), bw);
            }

        } // namespace boolean_reading

        // Whether `cand` holds as a leaf a comparison, or a `select` on one: a
        // narrow comparison the exact reading takes in where this collection
        // did not (`ExactNarrowReadingMbaCandidate`).
        bool HoldsComparisonLeaf(const MBACandidate &cand) {
            return llvm::any_of(cand.leaf_values, [](const llvm::Value *leaf) {
                if (const auto *select = llvm::dyn_cast< llvm::SelectInst >(leaf)) {
                    return llvm::isa< llvm::ICmpInst >(select->getCondition());
                }
                return llvm::isa< llvm::ICmpInst >(leaf);
            });
        }

        // Whether the boolean reading has anything to read in `cand`: a
        // condition in its tree, or a comparison it reads as a leaf, which the
        // boolean cut takes in.
        bool ReadsConditions(const MBACandidate &cand) {
            if (llvm::any_of(cand.leaf_values, [](const llvm::Value *leaf) {
                    return llvm::isa< llvm::ICmpInst >(leaf);
                }))
            {
                return true;
            }
            if (cand.expr == nullptr) {
                return false;
            }
            std::vector< const Expr * > conditions;
            std::unordered_map< const Expr *, uint32_t > slot_of;
            boolean_reading::CollectConditions(*cand.expr, cand.bitwidth, conditions, slot_of);
            return !conditions.empty();
        }

        // Search for a rewrite of `cand` with its conditions read as variables
        // (`CobraPassOptions::boolean_variables`): the search the boolean cut of a
        // root the ordinary one turned down is given (`BooleanCutMbaCandidate`).
        //
        // A comparison, and the sign bit of a value, is 0 or 1 on every input,
        // and replacing it with a variable masked to one bit changes nothing the
        // tree computes - only what the search is shown. Over the words, "is any
        // bit of `x` set" is not a function of the bits of `x` one at a time,
        // and a shift by `w - 1` does not mean the same at the narrower widths
        // the search solves in; over a 0/1 variable, `fshl(-c, b, 1)` with the
        // sign of `b` being `c` is `-c`.
        //
        // Two conditions are one variable only where the solver proves them the
        // same, or one the negation of the other, over the tree's variables; the
        // probes only choose which pairs it is asked about, and an undecided
        // answer leaves the two apart. The rewrite is then put back in terms of
        // the conditions it read, and `VerifyCandidate` proves it against the
        // tree with the solver alone - the reading and the merges included.
        OutcomeCache::Entry ProposeBooleanRewrite(
            const MBACandidate &cand, const CobraPassOptions &options, CobraStatistics &stats
        ) {
            using namespace boolean_reading;
            if (cand.expr == nullptr || !options.z3_verify) {
                return nullptr;
            }
            const uint32_t bw   = cand.bitwidth;
            const auto base     = static_cast< uint32_t >(cand.var_names.size());
            std::vector< const Expr * > conditions;
            std::unordered_map< const Expr *, uint32_t > slot_of;
            CollectConditions(*cand.expr, bw, conditions, slot_of);
            // Each condition is a variable the search has to sweep, and a pair is
            // a question to the solver: past a handful there is neither.
            constexpr size_t kMaxConditions = 8;
            if (conditions.empty() || conditions.size() > kMaxConditions) {
                return nullptr;
            }
            ++stats.boolean_trees;
            stats.boolean_conditions += conditions.size();

            // The same inputs for every condition, so that two are compared on
            // the values they take together: random words and the boundary
            // values a comparison or a sign turns on.
            constexpr int kProbes = 96;
            static const uint64_t kBoundary[] = { 0, 1, 2, ~uint64_t{ 0 }, ~uint64_t{ 0 } - 1,
                                                  uint64_t{ 1 } << 63, (uint64_t{ 1 } << 63) - 1 };
            uint64_t state  = 0x9E3779B97F4A7C15ULL ^ cand.fingerprint.structure;
            const auto next = [&] {
                state ^= state << 13;
                state ^= state >> 7;
                state ^= state << 17;
                return state;
            };
            std::vector< std::vector< uint64_t > > probes(kProbes, std::vector< uint64_t >(base));
            for (int round = 0; round < kProbes; ++round) {
                for (auto &value : probes[round]) {
                    value = (round < 16 ? kBoundary[next() % 7] : next()) & Bitmask(bw);
                }
            }
            std::vector< std::vector< uint64_t > > seen(conditions.size());
            for (size_t slot = 0; slot < conditions.size(); ++slot) {
                for (const auto &values : probes) {
                    const uint64_t value = EvalExpr(*conditions[slot], values, bw);
                    if (value > 1) {
                        // Not a condition after all; nothing is read as one.
                        RejectReason() = "boolean reading: a condition evaluated to " + std::to_string(value);
                        return nullptr;
                    }
                    seen[slot].push_back(value);
                }
            }

            std::vector< uint32_t > representative(conditions.size());
            std::vector< bool > negated(conditions.size(), false);
            for (uint32_t slot = 0; slot < conditions.size(); ++slot) {
                representative[slot] = slot;
                for (uint32_t other = 0; other < slot; ++other) {
                    if (representative[other] != other) {
                        continue;
                    }
                    const bool same     = seen[slot] == seen[other];
                    const bool opposite = !same && llvm::all_of(llvm::seq< size_t >(0, kProbes), [&](size_t k) {
                        return seen[slot][k] == (seen[other][k] ^ 1);
                    });
                    if (!same && !opposite) {
                        continue;
                    }
                    auto claim = CloneExpr(*conditions[other]);
                    if (opposite) {
                        claim = Expr::BitwiseXor(std::move(claim), Expr::Constant(1));
                    }
                    auto settings = options.z3_settings;
                    // An undecided merge is not made, whatever the host takes
                    // an unknown answer to mean elsewhere.
                    settings.unknown_result_mode = Z3UnknownResultMode::kFail;
                    const auto proof =
                        VerifyExprsWithStrategy(*conditions[slot], *claim, cand.var_names, bw, settings);
                    ++stats.solver_calls;
                    if (proof.unknown) {
                        ++stats.boolean_undecided;
                        RestedOnUnknown() = true;
                        continue;
                    }
                    if (proof.equivalent) {
                        ++stats.boolean_merged;
                        representative[slot] = other;
                        negated[slot]        = opposite;
                        break;
                    }
                }
            }

            // The tree over its variables and one per class of conditions, then
            // only the variables it still reads, in that order.
            auto abstracted = Abstract(*cand.expr, slot_of, representative, negated, base);
            std::vector< bool > used(base + conditions.size(), false);
            UsedVariables(*abstracted, used);
            std::vector< uint32_t > compact(used.size(), 0);
            std::vector< std::string > names;
            std::vector< std::unique_ptr< Expr > > meaning;
            bool only_conditions = true;
            llvm::StringSet<> taken;
            for (const auto &name : cand.var_names) {
                taken.insert(name);
            }
            for (uint32_t variable = 0; variable < used.size(); ++variable) {
                if (!used[variable]) {
                    continue;
                }
                compact[variable] = static_cast< uint32_t >(names.size());
                if (variable < base) {
                    names.push_back(cand.var_names[variable]);
                    meaning.push_back(Expr::Variable(variable));
                    only_conditions = false;
                    continue;
                }
                std::string name = "cond" + std::to_string(variable - base);
                while (!taken.insert(name).second) {
                    name += "'";
                }
                names.push_back(std::move(name));
                meaning.push_back(Tidy(CloneExpr(*conditions[variable - base]), bw));
            }
            RemapVarIndices(*abstracted, compact);
            const auto variables = static_cast< uint32_t >(names.size());
            if (variables > options.max_vars || variables > 20) {
                RejectReason() = "boolean reading: more variables than max_vars";
                return nullptr;
            }

            Options opts{ .bitwidth         = bw,
                          .max_vars         = options.max_vars,
                          .spot_check       = true,
                          .enabled_families = options.enabled_families,
                          .evaluator        = Evaluator::FromExpr(*abstracted, bw),
                          .demanded_mask    = cand.demanded_mask };
            const auto sig = EvaluateBooleanSignature(*abstracted, variables, bw);
            // A tree that reads nothing but conditions is its table of values;
            // one that reads words as well goes to the search.
            Result< SimplifyOutcome > result = only_conditions
                ? Result< SimplifyOutcome >(Ok(SimplifyOutcome{
                      .kind = SimplifyOutcome::Kind::kSimplified,
                      .expr = MultilinearForm(sig, variables, bw) }))
                : Simplify(sig, names, abstracted.get(), opts);
            // `COBRA_BOOLEAN_DEBUG=1`: the tree as the search was given it, and what came of it.
            static const bool debug = std::getenv("COBRA_BOOLEAN_DEBUG") != nullptr;
            if (debug) {
                llvm::errs() << "cobra: boolean reading of " << RenderSSA(*cand.expr, cand.var_names) << "\n  "
                             << conditions.size() << " condition(s), read as "
                             << RenderSSA(*abstracted, names) << "\n  -> "
                             << (result.has_value() && result.value().kind == SimplifyOutcome::Kind::kSimplified
                                     ? RenderSSA(*result.value().expr, result.value().real_vars.empty() ? names : result.value().real_vars)
                                     : std::string("not simplified: ")
                                           + (result.has_value() ? result.value().diag.reason : result.error().message))
                             << "\n";
                for (uint32_t slot = 0; slot < conditions.size(); ++slot) {
                    llvm::errs() << "  condition " << slot << ": " << RenderSSA(*conditions[slot], cand.var_names)
                                 << (representative[slot] != slot
                                         ? std::string(" = ") + (negated[slot] ? "not " : "") + "condition "
                                               + std::to_string(representative[slot])
                                         : std::string())
                                 << "\n";
                }
            }
            if (!result.has_value() || result.value().kind != SimplifyOutcome::Kind::kSimplified) {
                RejectReason() = "boolean reading (" + std::to_string(conditions.size())
                    + " condition(s)): not simplified"
                    + (result.has_value() ? ": " + result.value().diag.reason : std::string());
                return nullptr;
            }
            auto rewrite = std::move(result.value().expr);
            if (!result.value().real_vars.empty()) {
                auto support = TryBuildVarSupport(names, result.value().real_vars);
                if (!support.has_value()) {
                    RejectReason() = "boolean reading: the rewrite names a variable it was not given";
                    return nullptr;
                }
                RemapVarIndices(*rewrite, *support);
            }
            // Back in terms of the tree's own variables: what `VerifyCandidate`
            // proves, and what the IR is rebuilt from.
            auto restored = Tidy(Substitute(*rewrite, meaning), bw);
            if (std::getenv("COBRA_BOOLEAN_DEBUG") != nullptr) {
                llvm::errs() << "  restored " << RenderSSA(*restored, cand.var_names) << "\n";
            }
            if (!IsBetter(ComputeCost(*restored).cost, ComputeCost(*cand.expr).cost)) {
                RejectReason() = "boolean reading: not cheaper: " + RenderSSA(*restored, cand.var_names);
                return nullptr;
            }
            ++stats.boolean_found;
            return std::make_shared< const CandidateOutcome >(CandidateOutcome{
                .expr               = std::move(restored),
                .real_vars          = {},
                .real_var_positions = {},
                .accepted_unknown   = false,
                .verified           = false,
                .boolean_reading    = true });
        }
#endif

        // Search for a rewrite of one candidate and put it through the cost
        // gate. Returns null when either rejects it, and otherwise a rewrite that
        // still has to be verified (`VerifyCandidate`).
        OutcomeCache::Entry ProposeOrdinaryRewrite(
            const MBACandidate &cand, const CobraPassOptions &options, SolveVerdict &verdict,
            CobraStatistics &stats
        ) {
            const auto since = [](std::chrono::steady_clock::time_point begin) {
                return static_cast< uint64_t >(
                    std::chrono::duration_cast< std::chrono::nanoseconds >(
                        std::chrono::steady_clock::now() - begin
                    )
                        .count()
                );
            };
            verdict = SolveVerdict::kUnsolved;
            RejectReason().clear();
            Options opts{ .bitwidth         = cand.bitwidth,
                          .max_vars         = options.max_vars,
                          .spot_check       = true,
                          .enabled_families = options.enabled_families,
                          .evaluator        = cand.evaluator,
                          .demanded_mask    = cand.demanded_mask };

            // Pass AST when available — unlocks semilinear,
            // MixedRewrite, and decomposition pipelines.
            const Expr *ast = cand.expr.get();

            // A comparison is true on a vanishing fraction of its inputs - an
            // equality of two words on one in 2^64 - so no probe, Boolean or
            // random, will ever see both sides of it, and a result fitted to
            // the probes is a guess about the side they missed. With the
            // narrow-width reading of trees such chains reach the values they
            // compare, and the guess folds a loop's exit test to a constant.
            // Only a proof can accept such a rewrite, so without one the
            // candidate is declined.
            if (!options.z3_verify
                && (cand.holds_comparison || (ast != nullptr && ContainsComparison(*ast))))
            {
                ++NumSkippedUnsupported;
                RejectReason() = "a comparison needs solver verification";
                LLVM_DEBUG(
                    llvm::dbgs() << "CoBRA: skipping — a comparison needs solver verification\n"
                );
                return nullptr;
            }

            const auto search_begin = std::chrono::steady_clock::now();
            auto result             = Simplify(cand.sig, cand.var_names, ast, opts);
            const uint64_t searching = since(search_begin);


            stats.search_nanoseconds += searching;
            if (result.has_value() && result.value().kind == SimplifyOutcome::Kind::kSimplified) {
                ++stats.search_found;
                stats.search_found_nanoseconds += searching;
            }

            // A tree that computes one of its own inputs. Obfuscated identity
            // updates - `x` rewritten through a borrow, a shift and an add chain
            // that cancel - are such trees, and once a comparison stands in for
            // the borrow no technique of the search above reads them. Random
            // full-width inputs say which input it is; the proof decides, so this
            // is only offered when the sweep makes such proofs affordable.
            //
            // The leaf is phrased the way the tree reads it (`LeafExpr`), not as
            // a bare variable. Where a leaf is narrower than the tree every
            // reading of the tree masks it to its own width, so a bare variable
            // is a different function and the probe below fails on an identity
            // that holds. That hid the whole family wherever the value an
            // obfuscator rebuilt had been read at a narrower width than the
            // arithmetic above it - a 32-bit read summed into a 64-bit address,
            // say - which is most of where this shape is written.
            const bool searched = result.has_value() && result.value().kind == SimplifyOutcome::Kind::kSimplified;
            if (!searched && options.z3_verify && options.z3_settings.sweep && ast != nullptr) {
                const auto count = static_cast< uint32_t >(cand.var_names.size());
                for (uint32_t index = 0; index < count; ++index) {
                    auto leaf = LeafExpr(cand, index);
                    if (leaf == nullptr) {
                        continue;
                    }
                    if (FullWidthCheckEval(cand.evaluator, count, *leaf, cand.bitwidth, 32).passed) {
                        ++stats.leaf_identities;
                        verdict = SolveVerdict::kSolved;
                        return std::make_shared< const CandidateOutcome >(CandidateOutcome{
                            .expr               = std::move(leaf),
                            .real_vars          = {},
                            .real_var_positions = {},
                            .accepted_unknown   = false,
                            .verified           = false });
                    }
                }
            }
            if (!result.has_value()) {
                ++NumSkippedUnsupported;
                RejectReason() = "error: " + result.error().message;
                LLVM_DEBUG(
                    llvm::dbgs() << "CoBRA: skipping candidate: " << result.error().message
                                 << "\n"
                );
                return nullptr;
            }

            if (result.value().kind != SimplifyOutcome::Kind::kSimplified) {
                ++NumSkippedUnsupported;
                RejectReason() = "not simplified: " + result.value().diag.reason;
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
                    verdict = SolveVerdict::kNotCheaper;
                    ++NumSkippedCost;
                    RejectReason() = "not cheaper: " + RenderSSA(*result.value().expr, cand.var_names);
                    LLVM_DEBUG(
                        llvm::dbgs() << "CoBRA: skipping — simplified form is not smaller\n"
                    );
                    return nullptr;
                }
            }

            // Resolved against the candidate the rewrite was found for, while its
            // names still mean what the solve meant by them.
            std::vector< uint32_t > positions;
            if (!result.value().real_vars.empty()) {
                auto support = TryBuildVarSupport(cand.var_names, result.value().real_vars);
                if (!support.has_value()) {
                    ++NumSkippedUnsupported;
                    {
                        std::string names;
                        for (const auto &n : result.value().real_vars) { names += n + " "; }
                        std::string have;
                        for (const auto &n : cand.var_names) { have += n + " "; }
                        RejectReason() = "real_vars {" + names + "} not within {" + have + "}";
                    }
                    LLVM_DEBUG(
                        llvm::dbgs() << "CoBRA: skipping — real_vars not contained in "
                                        "candidate variable set\n"
                    );
                    return nullptr;
                }
                positions = std::move(*support);
            }

            verdict = SolveVerdict::kSolved;
            return std::make_shared< const CandidateOutcome >(CandidateOutcome{
                .expr               = std::move(result.value().expr),
                .real_vars          = std::move(result.value().real_vars),
                .real_var_positions = std::move(positions),
                .accepted_unknown   = false,
                .verified           = false });
        }

        namespace free_leaf_reading {

            // The variable `expr` is, read through the mask of `meaning`, when
            // it is one of the freed leaves; and `count` otherwise.
            uint32_t FreedLeaf(
                const Expr &expr, const std::vector< std::unique_ptr< Expr > > &meaning,
                const std::vector< bool > &freed
            ) {
                if (expr.kind != Expr::Kind::kAnd && expr.kind != Expr::Kind::kOr) {
                    return static_cast< uint32_t >(meaning.size());
                }
                for (uint32_t variable = 0; variable < meaning.size(); ++variable) {
                    if (freed[variable] && ExprStructurallyEqual(expr, *meaning[variable])) {
                        return variable;
                    }
                }
                return static_cast< uint32_t >(meaning.size());
            }

            // Which of the `freed` variables `expr` reads outside their masks.
            void MarkBare(
                const Expr &expr, const std::vector< std::unique_ptr< Expr > > &meaning,
                const std::vector< bool > &freed, std::vector< bool > &bare
            ) {
                if (FreedLeaf(expr, meaning, freed) < meaning.size()) {
                    return;
                }
                if (expr.kind == Expr::Kind::kVariable) {
                    if (expr.var_index < bare.size()) {
                        bare[expr.var_index] = true;
                    }
                    return;
                }
                for (const auto &child : expr.children) {
                    MarkBare(*child, meaning, freed, bare);
                }
            }

            // `expr` with every freed leaf's mask taken off.
            std::unique_ptr< Expr > Free(
                const Expr &expr, const std::vector< std::unique_ptr< Expr > > &meaning,
                const std::vector< bool > &freed
            ) {
                if (const uint32_t variable = FreedLeaf(expr, meaning, freed); variable < meaning.size()) {
                    return Expr::Variable(variable);
                }
                auto copy = std::make_unique< Expr >();
                copy->kind         = expr.kind;
                copy->constant_val = expr.constant_val;
                copy->var_index    = expr.var_index;
                for (const auto &child : expr.children) {
                    copy->children.push_back(Free(*child, meaning, freed));
                }
                return copy;
            }

            // `expr` with every freed variable read through its mask again.
            std::unique_ptr< Expr > Mask(
                const Expr &expr, const std::vector< std::unique_ptr< Expr > > &meaning,
                const std::vector< bool > &freed
            ) {
                if (expr.kind == Expr::Kind::kVariable && expr.var_index < freed.size()
                    && freed[expr.var_index])
                {
                    return CloneExpr(*meaning[expr.var_index]);
                }
                auto copy = std::make_unique< Expr >();
                copy->kind         = expr.kind;
                copy->constant_val = expr.constant_val;
                copy->var_index    = expr.var_index;
                for (const auto &child : expr.children) {
                    copy->children.push_back(Mask(*child, meaning, freed));
                }
                return copy;
            }

        } // namespace free_leaf_reading

        // A tree the search turned down, read again with its masked leaves free.
        //
        // A leaf narrower than the tree, or one with bits known, is read through
        // a mask (`LeafExpr`): a 32-bit read summed into a 64-bit word is
        // `v & 0xffffffff` wherever the tree reads it. The search takes that mask
        // for an operator applied to a variable, and an identity of the masked
        // value can then be one no technique of it reads: a base plus an offset
        // spelled `((a + b | 1) - (a ^ b) & -2) + (a ^ b)` is `a + b`, found at
        // once with `b` free and not at all with `b` a 32-bit read.
        //
        // A rewrite that holds for every value of a variable holds for the values
        // its mask lets through, so each leaf the tree reads only through its
        // mask is made a free variable, the search is asked again, and its answer
        // is read back with the mask around each such variable - the tree's own
        // function, which `VerifyCandidate` then checks as it checks any rewrite.
        // A leaf also read bare somewhere keeps its mask: freeing it in one place
        // and not the other would hand the search another function.
        OutcomeCache::Entry ProposeFreeLeafRewrite(
            const MBACandidate &cand, const CobraPassOptions &options, CobraStatistics &stats
        ) {
            using namespace free_leaf_reading;
            if (cand.expr == nullptr) {
                return nullptr;
            }
            const uint32_t bw    = cand.bitwidth;
            const auto variables = static_cast< uint32_t >(cand.var_names.size());
            std::vector< std::unique_ptr< Expr > > meaning;
            std::vector< bool > freed(variables, false);
            for (uint32_t variable = 0; variable < variables; ++variable) {
                auto leaf       = LeafExpr(cand, variable);
                freed[variable] = leaf != nullptr && leaf->kind != Expr::Kind::kVariable;
                meaning.push_back(freed[variable] ? std::move(leaf) : Expr::Variable(variable));
            }
            std::vector< bool > bare(variables, false);
            MarkBare(*cand.expr, meaning, freed, bare);
            for (uint32_t variable = 0; variable < variables; ++variable) {
                freed[variable] = freed[variable] && !bare[variable];
            }
            if (llvm::none_of(freed, [](bool free) { return free; })) {
                return nullptr;
            }
            auto read = Free(*cand.expr, meaning, freed);
            if (!options.z3_verify && ContainsComparison(*read)) {
                return nullptr;
            }
            ++stats.free_leaf_trees;

            Options opts{ .bitwidth         = bw,
                          .max_vars         = options.max_vars,
                          .spot_check       = true,
                          .enabled_families = options.enabled_families,
                          .evaluator        = Evaluator::FromExpr(*read, bw),
                          .demanded_mask    = cand.demanded_mask };
            const auto sig = EvaluateBooleanSignature(*read, variables, bw);
            auto result    = Simplify(sig, cand.var_names, read.get(), opts);
            if (!result.has_value() || result.value().kind != SimplifyOutcome::Kind::kSimplified) {
                RejectReason() = "free-leaf reading: not simplified"
                    + (result.has_value() ? ": " + result.value().diag.reason : std::string());
                return nullptr;
            }
            auto rewrite = std::move(result.value().expr);
            if (!result.value().real_vars.empty()) {
                auto support = TryBuildVarSupport(cand.var_names, result.value().real_vars);
                if (!support.has_value()) {
                    RejectReason() = "free-leaf reading: the rewrite names a variable it was not given";
                    return nullptr;
                }
                RemapVarIndices(*rewrite, *support);
            }
            auto restored = Mask(*rewrite, meaning, freed);
            if (!IsBetter(ComputeCost(*restored).cost, ComputeCost(*cand.expr).cost)) {
                RejectReason() = "free-leaf reading: not cheaper: " + RenderSSA(*restored, cand.var_names);
                return nullptr;
            }
            ++stats.free_leaf_found;
            return std::make_shared< const CandidateOutcome >(CandidateOutcome{
                .expr               = std::move(restored),
                .real_vars          = {},
                .real_var_positions = {},
                .accepted_unknown   = false,
                .verified           = false });
        }

        // Search for a rewrite of one candidate and put it through the cost gate
        // (`ProposeOrdinaryRewrite`), and with `boolean_variables` read its
        // conditions as variables (`ProposeBooleanRewrite`) - first for a tree that
        // compares, and otherwise where the search finds nothing:
        // on every reading of a root - the full collection, its re-cuts and the
        // cuts that keep a shared value whole, where the operand of a sign bit is a
        // leaf and the rewrite is the shift the function already holds - and alone
        // on the boolean cut, whose narrow comparisons every other search reads as
        // opaque.
        OutcomeCache::Entry ProposeRewriteAsRead(
            const MBACandidate &cand, const CobraPassOptions &options, SolveVerdict &verdict,
            CobraStatistics &stats
        ) {
            if (cand.node_limit == kBooleanCutLimit) {
                verdict = SolveVerdict::kUnsolved;
                RejectReason().clear();
#ifdef COBRA_HAS_Z3
                if (options.boolean_variables) {
                    auto outcome = ProposeBooleanRewrite(cand, options, stats);
                    if (outcome != nullptr) {
                        verdict = SolveVerdict::kSolved;
                    }
                    return outcome;
                }
#endif
                RejectReason() = "a boolean cut without the boolean reading";
                return nullptr;
            }
#ifdef COBRA_HAS_Z3
            const bool reads_conditions =
                options.boolean_variables && options.z3_verify && cand.expr != nullptr;
            // A tree that compares goes to the reading of its conditions first. The
            // search fits a comparison to probes that almost never land where it
            // turns (`ProposeOrdinaryRewrite` says why), so what it proposes for
            // such a tree is mostly a guess the proof then refutes - and a refuted
            // proposal is the candidate's last word: the reading would never be
            // asked.
            if (reads_conditions && ContainsComparison(*cand.expr)) {
                verdict = SolveVerdict::kUnsolved;
                RejectReason().clear();
                if (auto outcome = ProposeBooleanRewrite(cand, options, stats)) {
                    verdict = SolveVerdict::kSolved;
                    return outcome;
                }
                const std::string reason = RejectReason();
                auto ordinary = ProposeOrdinaryRewrite(cand, options, verdict, stats);
                if (ordinary == nullptr && !reason.empty()) {
                    RejectReason() += "; " + reason;
                }
                return ordinary;
            }
#endif
            auto ordinary = ProposeOrdinaryRewrite(cand, options, verdict, stats);
#ifdef COBRA_HAS_Z3
            if (ordinary == nullptr && reads_conditions) {
                const std::string reason = RejectReason();
                if (auto outcome = ProposeBooleanRewrite(cand, options, stats)) {
                    verdict = SolveVerdict::kSolved;
                    return outcome;
                }
                RejectReason() = reason + (RejectReason().empty() ? "" : "; " + RejectReason());
            }
#endif
            return ordinary;
        }

        namespace keyed_leaf_reading {

            // The variable and the constant of `expr` when it is `v ^ c`, either way
            // round, with `c` not zero.
            std::optional< std::pair< uint32_t, uint64_t > > KeyedLeaf(const Expr &expr, uint32_t bw) {
                if (expr.kind != Expr::Kind::kXor) {
                    return std::nullopt;
                }
                for (size_t side = 0; side < 2; ++side) {
                    const Expr &leaf = *expr.children[side];
                    const Expr &key  = *expr.children[1 - side];
                    if (leaf.kind == Expr::Kind::kVariable && key.kind == Expr::Kind::kConstant
                        && (key.constant_val & Bitmask(bw)) != 0)
                    {
                        return std::make_pair(leaf.var_index, key.constant_val & Bitmask(bw));
                    }
                }
                return std::nullopt;
            }

            // The constants the tree xors each variable with, in the order met.
            void CollectKeys(const Expr &expr, uint32_t bw, std::vector< std::vector< uint64_t > > &keys) {
                if (const auto keyed = KeyedLeaf(expr, bw); keyed && keyed->first < keys.size()) {
                    auto &found = keys[keyed->first];
                    if (llvm::find(found, keyed->second) == found.end()) {
                        found.push_back(keyed->second);
                    }
                }
                for (const auto &child : expr.children) {
                    CollectKeys(*child, bw, keys);
                }
            }

            // `expr` with each variable `v` that has a key read as `v ^ key[v]`.
            // Where the tree itself xors `v` with a constant, or compares it with
            // one, the key goes into that constant: `(v ^ k) ^ c` is `v ^ (k ^ c)`
            // and `(v ^ k) == c` is `v == (k ^ c)`. Xor being its own inverse, the
            // same call reads the tree over the keyed variables and puts a rewrite
            // found over them back over the tree's own.
            std::unique_ptr< Expr > Rekey(
                const Expr &expr, const std::vector< std::optional< uint64_t > > &key, uint32_t bw
            ) {
                const auto key_of = [&](const Expr &node) -> std::optional< uint64_t > {
                    if (node.kind != Expr::Kind::kVariable || node.var_index >= key.size()) {
                        return std::nullopt;
                    }
                    return key[node.var_index];
                };
                if (expr.kind == Expr::Kind::kXor || expr.kind == Expr::Kind::kCmpEq) {
                    for (size_t side = 0; side < 2; ++side) {
                        const Expr &leaf     = *expr.children[side];
                        const Expr &constant = *expr.children[1 - side];
                        const auto leaf_key  = key_of(leaf);
                        if (!leaf_key || constant.kind != Expr::Kind::kConstant) {
                            continue;
                        }
                        const uint64_t folded = (constant.constant_val ^ *leaf_key) & Bitmask(bw);
                        if (expr.kind == Expr::Kind::kXor) {
                            if (folded == 0) {
                                return Expr::Variable(leaf.var_index);
                            }
                            return Expr::BitwiseXor(Expr::Variable(leaf.var_index), Expr::Constant(folded));
                        }
                        return Expr::CmpEq(Expr::Variable(leaf.var_index), Expr::Constant(folded));
                    }
                }
                if (const auto leaf_key = key_of(expr)) {
                    return Expr::BitwiseXor(Expr::Variable(expr.var_index), Expr::Constant(*leaf_key));
                }
                auto copy          = std::make_unique< Expr >();
                copy->kind         = expr.kind;
                copy->constant_val = expr.constant_val;
                copy->var_index    = expr.var_index;
                for (const auto &child : expr.children) {
                    copy->children.push_back(Rekey(*child, key, bw));
                }
                return copy;
            }

        } // namespace keyed_leaf_reading

        // A tree the search turned down, read again over the keyed values of its
        // leaves.
        //
        // An obfuscator that stores a value encrypted with a constant key reads it
        // back as `v ^ k` and computes on that, and once part of such a tree has
        // been rewritten, or a comparison stands in for the borrow of `(v ^ k) - 1`,
        // the tree reads `v` both through the key and bare: `v == k`, `v >> 1`,
        // `v & k`. The search probes its variables at structured points - the
        // Boolean ones, single bits, the boundaries a comparison turns on - and
        // over `v` those land on points of `v ^ k` with no structure at all. The
        // same tree read over `y = v ^ k` puts the probes where the tree's own
        // arithmetic turns: `select(v == k, -2, 2^63 - 2) + ((v ^ k) - 1 >> 1) -
        // (v >> 1 ^ ~(k >> 1) & (2^63 - 1))` is `(v ^ k) - 2`, out of every
        // technique's reach over `v` and found at once over `y`.
        //
        // `y` is `v` under a bijection, so the tree over `y` is the same function
        // and a rewrite of it holds for every `y`, which is every `v`. Each
        // variable the tree xors with a constant is read through that key, the
        // reading goes through the search and the reading of conditions as it
        // would for the tree itself (`ProposeRewriteAsRead`), and the answer is
        // put back with the key around each such variable - the tree's own
        // function, which `VerifyCandidate` then checks against the tree as it
        // checks any rewrite. A variable xored with several constants is read
        // through each in turn, a bounded number of times.
        OutcomeCache::Entry ProposeKeyedLeafRewrite(
            const MBACandidate &cand, const CobraPassOptions &options, CobraStatistics &stats
        ) {
            using namespace keyed_leaf_reading;
            if (cand.expr == nullptr) {
                return nullptr;
            }
            const uint32_t bw    = cand.bitwidth;
            const auto variables = static_cast< uint32_t >(cand.var_names.size());
            std::vector< std::vector< uint64_t > > keys(variables);
            CollectKeys(*cand.expr, bw, keys);
            size_t readings = 0;
            for (const auto &found : keys) {
                readings = std::max(readings, found.size());
            }
            // Each reading is a search and the proofs behind it.
            constexpr size_t kMaxReadings = 2;
            readings = std::min(readings, kMaxReadings);

            std::string reasons;
            for (size_t reading = 0; reading < readings; ++reading) {
                std::vector< std::optional< uint64_t > > key(variables);
                for (uint32_t variable = 0; variable < variables; ++variable) {
                    if (reading < keys[variable].size()) {
                        key[variable] = keys[variable][reading];
                    }
                }
                // What the search and the proof read of a candidate (the copy a
                // worker is handed), over the keyed variables. A keyed variable
                // keeps nothing known of the leaf: those are facts about `v`.
                MBACandidate keyed;
                keyed.root          = nullptr;
                keyed.var_names     = cand.var_names;
                keyed.bitwidth      = bw;
                keyed.expr          = Rekey(*cand.expr, key, bw);
                keyed.evaluator     = Evaluator::FromExpr(*keyed.expr, bw);
                keyed.sig           = EvaluateBooleanSignature(*keyed.expr, variables, bw);
                keyed.demanded_mask = cand.demanded_mask;
                keyed.leaf_bits     = cand.leaf_bits;
                keyed.leaf_widths   = cand.leaf_widths;
                keyed.holds_comparison = cand.holds_comparison;
                for (uint32_t variable = 0; variable < variables; ++variable) {
                    if (!key[variable]) {
                        continue;
                    }
                    if (variable < keyed.leaf_bits.size()) {
                        keyed.leaf_bits[variable] = LeafBits{};
                    }
                    if (variable < keyed.leaf_widths.size()) {
                        keyed.leaf_widths[variable] = 0;
                    }
                }
                keyed.tree_size   = cand.tree_size;
                keyed.node_limit  = cand.node_limit;
                keyed.fingerprint = cand.fingerprint;
                ++stats.keyed_leaf_trees;

                SolveVerdict verdict = SolveVerdict::kUnsolved;
                const auto found     = ProposeRewriteAsRead(keyed, options, verdict, stats);
                if (found == nullptr) {
                    reasons += (reasons.empty() ? "" : "; ") + RejectReason();
                    continue;
                }
                auto rewrite = CloneExpr(*found->expr);
                if (!found->real_var_positions.empty()) {
                    RemapVarIndices(*rewrite, found->real_var_positions);
                }
                auto restored = Rekey(*rewrite, key, bw);
                if (!IsBetter(ComputeCost(*restored).cost, ComputeCost(*cand.expr).cost)) {
                    reasons += (reasons.empty() ? "" : "; ") + std::string("not cheaper: ")
                        + RenderSSA(*restored, cand.var_names);
                    continue;
                }
                ++stats.keyed_leaf_found;
                return std::make_shared< const CandidateOutcome >(CandidateOutcome{
                    .expr               = std::move(restored),
                    .real_vars          = {},
                    .real_var_positions = {},
                    .accepted_unknown   = false,
                    .verified           = false,
                    .boolean_reading    = found->boolean_reading });
            }
            if (readings != 0) {
                RejectReason() = "keyed-leaf reading: " + reasons;
            }
            return nullptr;
        }

        // `ProposeRewriteAsRead`, and where it finds nothing at all, the tree with
        // its masked leaves free (`ProposeFreeLeafRewrite`) and over the keyed
        // values of its leaves (`ProposeKeyedLeafRewrite`). A rewrite found but
        // not cheaper is not asked about again: freeing a leaf only drops a mask,
        // and keying one only moves the constants.
        OutcomeCache::Entry ProposeRewrite(
            const MBACandidate &cand, const CobraPassOptions &options, SolveVerdict &verdict,
            CobraStatistics &stats
        ) {
            RestedOnUnknown() = false;
            auto outcome = ProposeRewriteAsRead(cand, options, verdict, stats);
            if (outcome != nullptr || verdict != SolveVerdict::kUnsolved
                || cand.node_limit == kBooleanCutLimit)
            {
                return outcome;
            }
            const std::string reason = RejectReason();
            const auto append = [&] {
                RejectReason() = reason
                    + (RejectReason().empty() || RejectReason() == reason ? "" : "; " + RejectReason());
            };
            if (auto freed = ProposeFreeLeafRewrite(cand, options, stats)) {
                verdict = SolveVerdict::kSolved;
                return freed;
            }
            append();
            const std::string so_far = RejectReason();
            if (auto keyed = ProposeKeyedLeafRewrite(cand, options, stats)) {
                verdict = SolveVerdict::kSolved;
                return keyed;
            }
            RejectReason() = so_far
                + (RejectReason().empty() || RejectReason() == so_far ? ""
                                                                      : "; " + RejectReason());
            return nullptr;
        }

        // `COBRA_RING_CHECK=1`: every ring proof is also checked by evaluating both
        // sides on random and boundary inputs, and one they disagree on is
        // reported and not taken. A check on the prover, not a proof: for
        // validating it on a real workload.
        bool RingProofSurvivesSampling(const Expr &lhs, const Expr &rhs, const MBACandidate &cand) {
            static const bool check = std::getenv("COBRA_RING_CHECK") != nullptr;
            if (!check) {
                return true;
            }
            const auto variables = static_cast< uint32_t >(cand.var_names.size());
            const uint64_t mask  = Bitmask(cand.bitwidth);
            uint64_t state       = 0x9E3779B97F4A7C15ULL ^ std::hash< std::string >{}(Render(lhs, cand.var_names, cand.bitwidth));
            const auto next      = [&] {
                state ^= state << 13;
                state ^= state >> 7;
                state ^= state << 17;
                return state;
            };
            static const uint64_t kBoundary[] = { 0, 1, 2, ~uint64_t{ 0 }, ~uint64_t{ 0 } - 1,
                                                  uint64_t{ 1 } << 63, (uint64_t{ 1 } << 63) - 1 };
            std::vector< uint64_t > values(variables);
            for (int round = 0; round < 4096; ++round) {
                for (auto &value : values) {
                    value = (round < 64 ? kBoundary[next() % 7] : next()) & mask;
                }
                if (EvalExpr(lhs, values, cand.bitwidth) != EvalExpr(rhs, values, cand.bitwidth)) {
                    llvm::errs() << "CoBRA: COBRA_RING_CHECK: a ring proof does not survive sampling\n  lhs "
                                 << Render(lhs, cand.var_names, cand.bitwidth) << "\n  rhs "
                                 << Render(rhs, cand.var_names, cand.bitwidth) << "\n";
                    return false;
                }
            }
            return true;
        }

        // Whether `value` is never `undef` - it may still be poison - however
        // far beneath it the values that settle the question are. LLVM's own
        // answer (`isGuaranteedNotToBeUndef`) gives up six operands down, and
        // a leaf CoBRA reads is often deeper than that above them: a hash of
        // two loads the host marks `!noundef` is a dozen operations over them,
        // each of which only passes `undef` on. A leaf that is not `undef`
        // never needed the freeze the depth limit made it pay for - and a
        // frozen leaf matches nothing the function already computes from it,
        // so the rewrite is built afresh beside what is there and is priced
        // out, or kept as a second copy no pass relates to the first.
        //
        // The walk goes through the instructions that make no `undef` of
        // operands that are not - arithmetic, casts, comparisons, `select`,
        // `getelementptr`, and phis - and asks LLVM about every other value:
        // constants, `noundef` arguments, `!noundef` loads, `noundef` call
        // results and `freeze` answer yes there, any other load or call no.
        // The answer is for the whole cone at once: one value in it that may
        // be `undef` makes the leaf one that may be, since every instruction
        // walked through passes it on.
        //
        // A phi is asked about all of its incoming values, and a value met
        // again on its own path - the back edge of a loop counter - is taken
        // to be what the rest of its cycle shows. That is sound because the
        // answer covers the whole cone: whenever the program computes a value
        // of it, the operands read were computed before (an instruction's
        // operands dominate it, and a phi reads the value of the edge just
        // taken), so by induction over the run, if every value the cone reads
        // from outside itself is defined, every value in it is - the first
        // value of a cycle the program computes reads the cycle's entry from
        // outside. An instruction in a block nothing reaches, one reading
        // itself say, is never computed, and nothing is claimed of it.
        //
        // `proved` holds what earlier walks answered yes for - each value of a
        // cone that answers yes is itself never `undef` - and one walk visits
        // at most `kNeverUndefBudget` values: past that the answer is no, as
        // LLVM's is past its depth.
        constexpr size_t kNeverUndefBudget = 4096;

        bool NeverUndef(const llvm::Value *value, llvm::SmallPtrSetImpl< const llvm::Value * > &proved) {
            if (proved.contains(value)) {
                return true;
            }
            llvm::SmallPtrSet< const llvm::Value *, 32 > visited;
            llvm::SmallVector< const llvm::Value *, 32 > pending{ value };
            while (!pending.empty()) {
                const llvm::Value *current = pending.pop_back_val();
                if (proved.contains(current) || !visited.insert(current).second) {
                    continue;
                }
                if (visited.size() > kNeverUndefBudget) {
                    return false;
                }
                const auto *inst = llvm::dyn_cast< llvm::Instruction >(current);
                const bool passes_on = inst != nullptr
                    && (llvm::isa< llvm::BinaryOperator >(inst) || llvm::isa< llvm::CastInst >(inst)
                        || llvm::isa< llvm::CmpInst >(inst) || llvm::isa< llvm::SelectInst >(inst)
                        || llvm::isa< llvm::GetElementPtrInst >(inst) || llvm::isa< llvm::PHINode >(inst));
                if (!passes_on) {
                    if (!llvm::isGuaranteedNotToBeUndef(current)) {
                        return false;
                    }
                    continue;
                }
                for (const llvm::Value *operand : inst->operands()) {
                    pending.push_back(operand);
                }
            }
            proved.insert(visited.begin(), visited.end());
            return true;
        }

        // The leaves whose poison is the root's poison: reached from the root
        // along operands that pass poison on - every operand of every
        // operation, except a `select`'s arms (only the arm taken is read), a
        // `freeze`'s operand (it stops poison) and a phi's (only one arm is
        // read). The root is poison wherever such a leaf is, so a rewrite that
        // reads one adds no poison and the leaf needs no freeze for it.
        //
        // `through_both_arms` (a rewrite of the boolean reading) also follows a
        // `select` into its arms for the leaves both of them carry: such a leaf
        // is poison in the arm taken whichever it is, so the `select` is poison
        // with it. A condition the reading puts back over a value chosen by a
        // `select` reads what both arms are computed from - a hash and its
        // negation, say - and frozen for the arm it could have skipped, that leaf
        // matches nothing the function holds.
        llvm::DenseSet< const llvm::Value * > LeavesPoisoningBothArms(const MBACandidate &cand);

        llvm::DenseSet< const llvm::Value * > LeavesPoisoningTheRoot(
            const MBACandidate &cand, bool through_both_arms = false
        ) {
            if (through_both_arms) {
                return LeavesPoisoningBothArms(cand);
            }
            const llvm::DenseSet< const llvm::Value * > tree(cand.tree.begin(), cand.tree.end());
            const llvm::DenseSet< const llvm::Value * > leaves(
                cand.leaf_values.begin(), cand.leaf_values.end()
            );
            llvm::DenseSet< const llvm::Value * > seen;
            llvm::DenseSet< const llvm::Value * > poisoning;
            llvm::SmallVector< const llvm::Value *, 32 > pending{ cand.root };
            while (!pending.empty()) {
                const llvm::Value *value = pending.pop_back_val();
                if (!seen.insert(value).second) {
                    continue;
                }
                if (leaves.contains(value)) {
                    poisoning.insert(value);
                    continue;
                }
                const auto *inst = llvm::dyn_cast< llvm::Instruction >(value);
                if (inst == nullptr || !tree.contains(inst) || llvm::isa< llvm::FreezeInst >(inst)
                    || llvm::isa< llvm::PHINode >(inst))
                {
                    continue;
                }
                if (const auto *select = llvm::dyn_cast< llvm::SelectInst >(inst)) {
                    pending.push_back(select->getCondition());
                    continue;
                }
                for (const llvm::Value *operand : inst->operands()) {
                    pending.push_back(operand);
                }
            }
            return poisoning;
        }

        llvm::DenseSet< const llvm::Value * > LeavesPoisoningBothArms(const MBACandidate &cand) {
            const llvm::DenseSet< const llvm::Value * > tree(cand.tree.begin(), cand.tree.end());
            const llvm::DenseSet< const llvm::Value * > leaves(
                cand.leaf_values.begin(), cand.leaf_values.end()
            );
            // The leaves whose poison makes each value poison, memoised: the tree is
            // a DAG, and a value read twice is asked about once.
            llvm::DenseMap< const llvm::Value *, llvm::DenseSet< const llvm::Value * > > poisons;
            const std::function< const llvm::DenseSet< const llvm::Value * > &(const llvm::Value *) >
                of = [&](const llvm::Value *value) -> const llvm::DenseSet< const llvm::Value * > & {
                if (auto found = poisons.find(value); found != poisons.end()) {
                    return found->second;
                }
                // Placed first, so a value met again on its own path (a phi the
                // collection looked through) reads as poisoning nothing.
                poisons[value];
                llvm::DenseSet< const llvm::Value * > result;
                const auto *inst = llvm::dyn_cast< llvm::Instruction >(value);
                if (leaves.contains(value)) {
                    result.insert(value);
                } else if (inst == nullptr || !tree.contains(inst) || llvm::isa< llvm::FreezeInst >(inst)
                           || llvm::isa< llvm::PHINode >(inst))
                {
                    // Stops poison, or is not part of the tree.
                } else if (const auto *select = llvm::dyn_cast< llvm::SelectInst >(inst)) {
                    const auto cond = of(select->getCondition());
                    result.insert(cond.begin(), cond.end());
                    const auto then_poisons = of(select->getTrueValue());
                    const auto &else_poisons = of(select->getFalseValue());
                    for (const auto *leaf : then_poisons) {
                        if (else_poisons.contains(leaf)) {
                            result.insert(leaf);
                        }
                    }
                } else {
                    for (const llvm::Value *operand : inst->operands()) {
                        const auto operand_poisons = of(operand);
                        result.insert(operand_poisons.begin(), operand_poisons.end());
                    }
                }
                return poisons[value] = std::move(result);
            };
            return of(cand.root);
        }

        // `COBRA_DETECT_DEBUG=1`: whether the per-candidate trace is on. The
        // detector prints the roots it declined under the same flag
        // (`DeclinedRoot`); this is for the gates the driver applies after a
        // candidate has been built.
        bool DetectDebug() {
            static const bool enabled = std::getenv("COBRA_DETECT_DEBUG") != nullptr;
            return enabled;
        }

        // CPU time this thread has run, in nanoseconds.
        uint64_t ThreadCpuNanoseconds() {
            timespec now{};
            if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) != 0) {
                return 0;
            }
            return static_cast< uint64_t >(now.tv_sec) * 1000000000ULL
                + static_cast< uint64_t >(now.tv_nsec);
        }

        // Whether a proof that ran out of its budget had the processor while it
        // ran. The budget is wall-clock time, so on a machine busy with other
        // work - other workers, the race's pool, the rest of the host - a
        // thread can be given a fraction of it and run out for that reason
        // alone. Such a timeout says nothing about the question, so it is not
        // recorded: recorded, it would stop the question being asked again for
        // the rest of the process.
        bool HadTheProcessor(uint64_t wall_nanoseconds, uint64_t cpu_nanoseconds) {
            return cpu_nanoseconds * 10 >= wall_nanoseconds * 7;
        }

        // How many times `z3_settings.timeout_ms` the proof of `cand` is given: one per
        // `kNodesPerBudget` instructions of its IR tree, at least one and at most
        // `kMaxBudgetScale`. See `VerifyCandidate` for why the budget grows with the tree.
        uint32_t ProofBudgetScale(uint32_t tree_size) {
            constexpr uint32_t kNodesPerBudget = 8;
            constexpr uint32_t kMaxBudgetScale = 8;
            return std::min(kMaxBudgetScale, std::max(1U, tree_size / kNodesPerBudget));
        }

        uint32_t ProofBudgetScale(const MBACandidate &cand) {
            return ProofBudgetScale(static_cast< uint32_t >(cand.tree_size));
        }

        // Prove `proposal` equal to the tree of `cand` on the bits its readers
        // look at. `cand` need not be the candidate the rewrite was found for:
        // any candidate with the same key has the same tree, width, demanded
        // bits and variable count, which is everything the proof reads, and the
        // rewrite names its variables by position. Returns the rewrite marked
        // verified, or null when the proof fails or cannot be had.
        OutcomeCache::Entry VerifyCandidate(
            const MBACandidate &cand, const CandidateOutcome &proposal,
            const CobraPassOptions &options, CobraStatistics &stats, bool *unknown = nullptr
        ) {
            const auto since = [](std::chrono::steady_clock::time_point begin) {
                return static_cast< uint64_t >(
                    std::chrono::duration_cast< std::chrono::nanoseconds >(
                        std::chrono::steady_clock::now() - begin
                    )
                        .count()
                );
            };
            const Expr *ast       = cand.expr.get();
            const auto &positions = proposal.real_var_positions;

            // A rewrite the boolean reading found stands on merges of
            // conditions and on a search over a tree that is not the one it
            // replaces: the solver proves it against the tree or it is not
            // taken. Enumeration and the ring normal form may still refute it,
            // but a proof from either is not the proof asked for, and neither
            // is an `unknown` answer, whatever the host takes one to mean.
            const bool solver_only = proposal.boolean_reading;
            if (solver_only && (!options.z3_verify || ast == nullptr)) {
                ++NumSkippedUnsupported;
                return nullptr;
            }

            // Proved outright where the inputs can be walked, on the bits the
            // rewrite was found for. This runs whether or not a solver is asked
            // for: a rewrite it refutes is wrong either way, and one it proves
            // has nothing left for the solver to say.
            bool proved           = false;
            bool accepted_unknown = false;
            if (ast != nullptr) {
                auto rewritten = CloneExpr(*proposal.expr);
                if (!positions.empty()) {
                    RemapVarIndices(*rewritten, positions);
                }
                auto original = CloneExpr(*ast);
                if (cand.demanded_mask != Bitmask(cand.bitwidth)) {
                    original =
                        Expr::BitwiseAnd(std::move(original), Expr::Constant(cand.demanded_mask));
                    rewritten =
                        Expr::BitwiseAnd(std::move(rewritten), Expr::Constant(cand.demanded_mask));
                }
                const auto enumeration_begin = std::chrono::steady_clock::now();
                const auto decided           = DecideByEnumeration(
                    *original, *rewritten, static_cast< uint32_t >(cand.var_names.size()),
                    cand.bitwidth
                );
                stats.enumeration_nanoseconds += since(enumeration_begin);
                if (decided.has_value()) {
                    CountVerification([&](VerificationCounters &counters) {
                        ++(*decided ? counters.proved_by_enumeration
                                    : counters.refuted_by_enumeration);
                    });
                }
                if (decided.has_value() && !*decided) {
                    ++NumSkippedUnsupported;
                    LLVM_DEBUG(
                        llvm::dbgs() << "CoBRA: skipping — the rewrite differs from the tree on "
                                        "an input\n"
                    );
                    return nullptr;
                }
                proved = decided.has_value() && !solver_only;

                // Then by algebra, before any solver: a normal form over the ring
                // settles in microseconds what bit-blasting a multiplier cannot in
                // seconds (see `ProveEqualByRingNormalForm`). It only ever proves,
                // so what it does not settle goes on to the solver as before.
                if (!proved && !solver_only) {
                    const auto ring_begin = std::chrono::steady_clock::now();
                    ++stats.ring_attempted;
                    proved = ProveEqualByRingNormalForm(*original, *rewritten, cand.bitwidth);
                    if (proved && !RingProofSurvivesSampling(*original, *rewritten, cand)) {
                        proved = false;
                    }
                    stats.ring_nanoseconds += since(ring_begin);
                    stats.ring_proved += proved ? 1 : 0;
                }
            }

#ifdef COBRA_HAS_Z3
            if (options.z3_verify && !proved) {
                if (ast == nullptr) {
                    ++NumSkippedUnsupported;
                    LLVM_DEBUG(
                        llvm::dbgs() << "CoBRA: skipping — Z3 verification requested but "
                                        "candidate AST is unavailable\n"
                    );
                    return nullptr;
                }

                auto z3_expr = CloneExpr(*proposal.expr);
                if (!positions.empty()) {
                    RemapVarIndices(*z3_expr, positions);
                }

                // A rewrite found for the demanded bits is proved on those:
                // both sides are masked to them.
                auto proved_ast = CloneExpr(*ast);
                if (cand.demanded_mask != Bitmask(cand.bitwidth)) {
                    proved_ast =
                        Expr::BitwiseAnd(std::move(proved_ast), Expr::Constant(cand.demanded_mask));
                    z3_expr =
                        Expr::BitwiseAnd(std::move(z3_expr), Expr::Constant(cand.demanded_mask));
                }
                // The budget is the size of the obligation, not a constant.
                //
                // A proof over a thirty-node tree of xors, shifts and a
                // comparison is not the same question as one over a three-node
                // ring identity, and giving both the same milliseconds means
                // the big trees are the ones that time out - the very ones a
                // rewrite is worth most on. Worse, the outcome is then decided
                // by how fast the machine happened to be: on one workload
                // an obfuscated store address was proved equal to `rdx + 8` by
                // the search and then dropped because the proof wanted 255 ms
                // against a flat 250, and the store went on may-aliasing the
                // frame and stopping the host's analysis.
                //
                // Scaling is close to free. The candidates are overwhelmingly
                // small, and a small tree keeps exactly the budget it had; only
                // a tree past `kNodesPerBudget` asks for more, and the cap
                // keeps the worst case bounded. Nothing here accepts an
                // unproved rewrite: a timeout is still a timeout, this only
                // decides how long the solver is given before it is called one.
                auto settings = options.z3_settings;
                settings.timeout_ms *= ProofBudgetScale(cand);
                const auto solver_begin = std::chrono::steady_clock::now();
                auto z3_result          = VerifyExprsWithStrategy(
                    *proved_ast, *z3_expr, cand.var_names, cand.bitwidth, settings
                );
                const uint64_t solving = since(solver_begin);
                ++stats.solver_calls;
                stats.solver_nanoseconds += solving;
                (z3_result.unknown        ? stats.solver_unknown_nanoseconds
                 : z3_result.equivalent ? stats.solver_proved_nanoseconds
                                        : stats.solver_refuted_nanoseconds) += solving;

                // A solver that ran out of time has proved nothing. A host may
                // still choose to take its silence for a yes, and for a tree of
                // ring operations that is a bet with the probes behind it: the
                // rewrite already agrees with the tree on the Boolean cube and
                // at random points, which two different polynomials almost
                // never do. Over a comparison the probes are no evidence at all
                // (see above) - they sit on one side of it, and a rewrite that
                // replaced the comparison by the side they saw passes every one
                // of them. There the proof is the only check there is, so an
                // unknown answer declines the candidate whatever the mode: taken
                // as a yes, it folded the `icmp ult` behind an obfuscated
                // conditional jump to true and the jump's displacement to a constant.
                //
                // The same holds for a comparison the rewrite introduces, and
                // the bet needs the Boolean cube actually checked: a rewrite
                // found by full-width random points alone (the leaf identities)
                // never was. So an unknown answer is taken for a yes only when
                // neither side compares and the rewrite matches the tree's
                // signature on every Boolean input.
                const auto cube_agrees = [&] {
                    const auto variables = static_cast< uint32_t >(cand.var_names.size());
                    if (variables > 20 || cand.sig.size() != (size_t{ 1 } << variables)) {
                        return false;
                    }
                    std::vector< uint64_t > values(variables);
                    for (size_t input = 0; input < cand.sig.size(); ++input) {
                        for (uint32_t v = 0; v < variables; ++v) {
                            values[v] = (input >> v) & 1;
                        }
                        if (EvalExpr(*z3_expr, values, cand.bitwidth)
                            != (cand.sig[input] & cand.demanded_mask & Bitmask(cand.bitwidth)))
                        {
                            return false;
                        }
                    }
                    return true;
                };
                const bool declined_comparison = z3_result.equivalent && z3_result.unknown
                    && (ContainsComparison(*ast) || ContainsComparison(*z3_expr) || !cube_agrees());
                CountVerification([&](VerificationCounters &counters) {
                    if (!z3_result.unknown) {
                        ++(z3_result.equivalent ? counters.proved_by_solver
                                                : counters.refuted_by_solver);
                    } else if (z3_result.equivalent && !declined_comparison) {
                        ++counters.unknown_accepted;
                        counters.unknown_accepted_timeouts += z3_result.timed_out ? 1 : 0;
                    } else {
                        ++counters.unknown_declined;
                    }
                });
                accepted_unknown = z3_result.unknown && z3_result.equivalent;
                if (unknown != nullptr) {
                    *unknown = z3_result.unknown;
                }
                if (solver_only && z3_result.unknown) {
                    ++NumSkippedUnsupported;
                    LLVM_DEBUG(
                        llvm::dbgs() << "CoBRA: skipping — a boolean reading needs a proof and the "
                                        "solver returned unknown\n"
                    );
                    return nullptr;
                }
                if (declined_comparison) {
                    ++NumSkippedUnsupported;
                    LLVM_DEBUG(
                        llvm::dbgs() << "CoBRA: skipping — a comparison needs a proof and Z3 "
                                        "returned unknown\n"
                    );
                    return nullptr;
                }
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
            if (options.z3_verify && !proved) {
                ++NumSkippedUnsupported;
                LLVM_DEBUG(
                    llvm::dbgs() << "CoBRA: skipping — built without Z3 support but Z3 "
                                    "verification requested\n"
                );
                return nullptr;
            }
#endif

            return std::make_shared< const CandidateOutcome >(CandidateOutcome{
                .expr               = CloneExpr(*proposal.expr),
                .real_vars          = proposal.real_vars,
                .real_var_positions = proposal.real_var_positions,
                .accepted_unknown   = accepted_unknown,
                .verified           = true,
                .boolean_reading    = proposal.boolean_reading });
        }

        // `VerifyCandidate`, and when the rewrite it was asked about came from the
        // reading of the tree's conditions and the solver did not prove it, the
        // ordinary reading of the same tree, proposed and proved in its place.
        //
        // A tree that compares is read with its conditions as variables first
        // (`ProposeRewriteAsRead`), and when that reading answers the ordinary
        // search is never asked. The answer is the right one, but it is proved
        // with the conditions put back over the values they compare, which can be
        // far harder for the solver than the same rewrite found by the ordinary
        // search: a step `x <s 0 ? -(-x * k) : x * k` masked to 32 bits read
        // either way is `x * k`, and the proof of the reading's rewrite outlived
        // five seconds where the ordinary one is immediate. An unproved answer was
        // the candidate's last word, and the tree stayed as written. The ordinary
        // rewrite is proved as any other is; nothing is taken without its proof.
        //
        // Returns the proved rewrite - the one asked about, or the ordinary one,
        // whose `boolean_reading` says which - or null. `unknown` is the first
        // proof's: an unfinished proof of the reading is not turned into a
        // refutation by a refuted ordinary rewrite.
        OutcomeCache::Entry VerifyCandidateOrOrdinary(
            const MBACandidate &cand, const CandidateOutcome &proposal,
            const CobraPassOptions &options, CobraStatistics &stats, bool *unknown = nullptr
        ) {
            auto verified = VerifyCandidate(cand, proposal, options, stats, unknown);
            if (verified != nullptr || !proposal.boolean_reading || cand.node_limit == kBooleanCutLimit) {
                return verified;
            }
            auto verdict  = SolveVerdict::kUnsolved;
            auto ordinary = ProposeOrdinaryRewrite(cand, options, verdict, stats);
            if (ordinary == nullptr || ordinary->boolean_reading) {
                return nullptr;
            }
            auto proved = VerifyCandidate(cand, *ordinary, options, stats);
            if (proved != nullptr && unknown != nullptr) {
                *unknown = false;
            }
            return proved;
        }

        // The width at which `cand`'s root can be read as a narrower problem,
        // with how the root is recovered from those bits - or nothing, when it
        // cannot.
        //
        // An obfuscator that writes a sign extension as `zext(x ^ m) ^ sext(m)`
        // leaves an expression no single-width reading can fold: the two halves
        // live at different widths, so neither LLVM's folds nor the semilinear
        // synthesis ever presents them a common root, and the 64-bit tree
        // stands as written. The same tree read at 32 bits is `(m ^ x) ^ m`,
        // which cancels to `x` at once, and the 64-bit value is the sign
        // extension of that.
        //
        // So the question is whether the root is an extension of its own low
        // bits, and it is **proved**, not assumed: the claim is written as an
        // identity over the tree's own expression -
        //
        //     zero: E == E & low         sign: E == ((E & low) ^ H) - H
        //
        // with `H` the top bit of the slice - and handed to the same proof
        // machinery a rewrite is. LLVM's value tracking cannot answer it here,
        // and that is the whole difficulty: it reads the two sides of the `xor`
        // independently, and `zext i32` alone leaves 32 sign bits where the
        // claim needs 33. The relation between the two sides is what makes the
        // root a sign extension, and only something that reasons over the whole
        // expression can see it.
        //
        // Probes pick the reading: a root that is not an extension of its low
        // bits says so on one of the first inputs. The proof waits
        // (`ProveExtensionClaim`): it used to be asked of every root the probes
        // let through, and nearly every such root is an extension - measured
        // over a replayed function, 695 of 715 proofs succeeded, at a quarter
        // of the pass's time - while a rewrite of the slice is rare. So the
        // claim travels with the slice (`ExtensionClaim`) and is proved only
        // once a rewrite of it fits the budget.
        //
        // `reading`, when given, is another collection of the same root that
        // reads more of it exactly - the boolean cut, which takes in the narrow
        // comparisons every other collection keeps as leaves. The widths are
        // still the ones `cand` crosses, and the probes and the claim are about
        // `reading`'s expression: a `select` on a narrow comparison is a free
        // leaf to `cand`, and with it free the root is the extension of
        // nothing, while read through its condition it may be.
        std::optional< std::pair< uint32_t, SliceExtension > > SliceToRead(
            const MBACandidate &cand, const CobraPassOptions &options, CobraStatistics &stats,
            const MBACandidate *reading = nullptr
        ) {
            const MBACandidate &model = reading != nullptr ? *reading : cand;
            // Without an expression there is nothing to prove the claim over,
            // and without a solver nothing to prove it with: the probes are
            // evidence, not a proof, and a slice rewritten on them alone would
            // be a guess about every bit above the slice.
            if (model.expr == nullptr || !options.z3_verify || model.bitwidth != cand.bitwidth) {
                return std::nullopt;
            }
            const auto widths = SliceWidthsOf(cand);
            if (widths.empty()) {
                return std::nullopt;
            }

            const uint32_t bw   = model.bitwidth;
            const uint64_t mask = Bitmask(bw);
            const auto variables = static_cast< uint32_t >(model.var_names.size());

            // Both readings of every candidate width, narrowest and zero
            // extension first: the narrowest slice is the smallest problem, and
            // a zero extension is the cheaper cast of the two.
            llvm::SmallVector< std::pair< uint32_t, SliceExtension >, 8 > hypotheses;
            for (const uint32_t width : widths) {
                hypotheses.push_back({ width, SliceExtension::kZero });
                hypotheses.push_back({ width, SliceExtension::kSign });
            }

            // The shape itself, on one value: what the claim says about the
            // bits above the slice.
            const auto holds = [&](uint64_t value, uint32_t width, SliceExtension extension) {
                const uint64_t low = Bitmask(width);
                if (extension == SliceExtension::kZero) {
                    return (value & ~low & mask) == 0;
                }
                const uint64_t sign = uint64_t{ 1 } << (width - 1);
                return value == ((((value & low) ^ sign) - sign) & mask);
            };

            // Random and boundary inputs, as the ring check uses: a sign
            // extension is decided by the top bit of the slice, so the values
            // that have it set and clear are the ones that decide.
            uint64_t state  = 0x9E3779B97F4A7C15ULL ^ model.fingerprint.structure;
            const auto next = [&] {
                state ^= state << 13;
                state ^= state >> 7;
                state ^= state << 17;
                return state;
            };
            static const uint64_t kBoundary[] = { 0,
                                                  1,
                                                  ~uint64_t{ 0 },
                                                  uint64_t{ 1 } << 63,
                                                  (uint64_t{ 1 } << 63) - 1,
                                                  uint64_t{ 1 } << 31,
                                                  (uint64_t{ 1 } << 31) - 1,
                                                  uint64_t{ 1 } << 15 };
            std::vector< uint64_t > values(variables);
            constexpr unsigned kProbeRounds = 64;
            for (unsigned round = 0; round < kProbeRounds && !hypotheses.empty(); ++round) {
                for (auto &value : values) {
                    value = (round < 16 ? kBoundary[next() % 8] : next()) & mask;
                }
                const uint64_t value = EvalExpr(*model.expr, values, bw);
                llvm::erase_if(hypotheses, [&](const std::pair< uint32_t, SliceExtension > &one) {
                    return !holds(value, one.first, one.second);
                });
            }

            if (hypotheses.empty()) {
                return std::nullopt;
            }
            return hypotheses.front();
        }

        enum class ClaimVerdict : uint8_t { kProved, kRefuted, kUnknown };

        // Prove that `claim.expr` is the `extension` of its low `width` bits -
        //
        //     zero: E == E & low         sign: E == ((E & low) ^ H) - H
        //
        // with `H` the top bit of the slice - by running every input where
        // there are few enough of them, and with the solver otherwise. An
        // unknown is not a yes here, whatever the host's mode says: the bits
        // above the slice are not probed by the slice's signature, so there is
        // nothing behind the bet, and a slice taken on silence would replace
        // the root by a different value.
        ClaimVerdict ProveExtensionClaim(
            const ExtensionClaim &claim, uint32_t width, SliceExtension extension,
            const CobraPassOptions &options, CobraStatistics &stats
        ) {
            const uint32_t bw   = claim.bitwidth;
            const uint64_t mask = Bitmask(bw);
            const uint64_t low  = Bitmask(width);
            const uint64_t sign = uint64_t{ 1 } << (width - 1);
            auto sliced  = Expr::BitwiseAnd(CloneExpr(*claim.expr), Expr::Constant(low));
            auto claimed = extension == SliceExtension::kZero
                ? std::move(sliced)
                : Expr::Add(
                      Expr::BitwiseXor(std::move(sliced), Expr::Constant(sign)),
                      Expr::Constant((~sign + 1) & mask)
                  );
            ++stats.slice_asked;
            if (const auto decided = DecideByEnumeration(*claim.expr, *claimed, claim.vars, bw))
            {
                ++(*decided ? stats.slice_proved : stats.slice_refuted);
                return *decided ? ClaimVerdict::kProved : ClaimVerdict::kRefuted;
            }
#ifdef COBRA_HAS_Z3
            auto settings = options.z3_settings;
            settings.timeout_ms *= ProofBudgetScale(claim.tree_size);
            std::vector< std::string > names;
            for (uint32_t index = 0; index < claim.vars; ++index) {
                names.push_back("v" + std::to_string(index));
            }
            const auto result =
                VerifyExprsWithStrategy(*claim.expr, *claimed, names, bw, settings);
            if (result.unknown) {
                ++stats.slice_unknown;
                return ClaimVerdict::kUnknown;
            }
            ++(result.equivalent ? stats.slice_proved : stats.slice_refuted);
            return result.equivalent ? ClaimVerdict::kProved : ClaimVerdict::kRefuted;
#else
            (void) options;
            ++stats.slice_unknown;
            return ClaimVerdict::kUnknown;
#endif
        }

        // How many instructions `ReconstructIr` emits for `expr` at the least.
        // It builds one per operator that reads a variable - an operator over
        // constants alone is folded by the builder - plus the extension of
        // every comparison and a cast for every use of a leaf held at another
        // width; freezing leaves can only add to that. So a rewrite whose count
        // reaches the candidate's budget is one the loop will turn down, and
        // proving it ahead of the loop would be wasted.
        uint32_t MinimumInstructions(
            const Expr &expr, const std::vector< uint32_t > &positions,
            const std::vector< bool > &leaf_needs_cast, bool &reads_variable
        ) {
            if (expr.kind == Expr::Kind::kVariable) {
                reads_variable      = true;
                const uint32_t leaf = positions.empty() ? expr.var_index
                                                        : positions[expr.var_index];
                return leaf < leaf_needs_cast.size() && leaf_needs_cast[leaf] ? 1 : 0;
            }
            reads_variable = false;
            uint32_t count = 0;
            for (const auto &child : expr.children) {
                bool child_reads = false;
                count += MinimumInstructions(*child, positions, leaf_needs_cast, child_reads);
                reads_variable = reads_variable || child_reads;
            }
            if (expr.children.empty() || !reads_variable) {
                return count;
            }
            const bool comparison = expr.kind == Expr::Kind::kCmpEq
                || expr.kind == Expr::Kind::kCmpUlt || expr.kind == Expr::Kind::kCmpSlt;
            return count + (comparison ? 2 : 1);
        }

        // ---- solving ahead of the loop ------------------------------------
        //
        // What a candidate's search and proof produce depends on nothing but
        // the candidate as it was collected - its tree, signature, width,
        // demanded bits and variable count, which is what the outcome cache
        // keys it on - and on the options. Only building a rewrite into the IR
        // and committing it has to follow the loop's order, because each
        // rewrite changes what the next one is built into. So candidates are
        // solved on worker threads as soon as they are queued, in the order the
        // loop will reach them, and each answer goes into the outcome cache
        // under the candidate's key: the answer the loop would have computed
        // itself, so the loop commits the same rewrites it always did.
        //
        // A worker proves a rewrite before anyone knows whether it fits the
        // instruction budget, which the loop only learns by building it. A
        // rewrite that plainly cannot fit is left unproved, as the loop leaves
        // it; one that might is proved, and a wasted proof costs a worker's
        // time instead of the loop's.
        //
        // The one answer that depends on more than the candidate is a proof the
        // solver could not finish within its budget. A worker has the same
        // budget and a core of its own, so its "unknown" is taken as the loop's
        // would have been.
        //
        // `COBRA_THREADS` sets the number of workers; 0 solves everything in
        // the loop, as the pass used to.
        // Whether a phi is carried by a loop: one of its own incoming edges comes from a
        // block it dominates, which is what a back edge is.
        //
        // Asked of the dominator tree the pass already builds rather than of `LoopInfo`,
        // so it costs nothing extra.
        bool IsLoopCarriedPhi(const llvm::PHINode *phi, const llvm::DominatorTree &dominators) {
            if (phi == nullptr) {
                return false;
            }
            for (const auto *incoming : phi->blocks()) {
                // Every block dominates an unreachable one, which is no back edge.
                if (dominators.isReachableFromEntry(incoming)
                    && dominators.dominates(phi->getParent(), incoming))
                {
                    return true;
                }
            }
            return false;
        }

        // Whether this candidate's tree involves a value that stands for a different thing
        // on every iteration of a loop.
        //
        // The collection looks through phis, handing on one arm, so a loop-carried phi can
        // sit inside the tree or be one of its leaves. Either way the expression the
        // candidate describes is not one expression: the search is being asked to find a
        // closed form for a value that has none.
        bool TouchesLoopCarriedPhi(const MBACandidate &cand, const llvm::DominatorTree &dominators) {
            for (const auto *instruction : cand.tree) {
                if (IsLoopCarriedPhi(llvm::dyn_cast_or_null< llvm::PHINode >(instruction), dominators)) {
                    return true;
                }
            }
            for (const auto *leaf : cand.leaf_values) {
                if (IsLoopCarriedPhi(llvm::dyn_cast_or_null< llvm::PHINode >(leaf), dominators)) {
                    return true;
                }
            }
            return false;
        }

        // Drops the poison-generating flags and metadata of every reader the
        // rewrite of `replaced` may have changed - what LLVM's BDCE does in
        // `clearAssumptionsOfUsers`, and for the same reason.
        //
        // The rewrite is proved equal to the tree on the bits its readers
        // demand, and demanded bits are worked out ignoring `nuw`/`nsw`,
        // `exact`, `or disjoint`, `trunc nuw`/`nsw` and `zext nneg`: `or
        // disjoint %r, 256` does not demand bit 8 of `%r`, yet it is poison if
        // that bit is set. A rewrite that differs from the tree only on bits
        // nobody demands can therefore turn such a reader into poison where
        // the original was defined. Every reader is cleared, and so is every
        // reader of a reader that does not demand all of its own bits - only
        // through those can a changed bit travel on.
        //
        // `readers` are the root's readers as they were before the rewrite was
        // put in: not the replacement's, which may be a constant read all over
        // the module or a value the rest of the function already used.
        uint64_t DropAssumptionsOfReaders(
            llvm::ArrayRef< llvm::Instruction * > readers, DemandedBitsOracle &demanded
        ) {
            uint64_t dropped = 0;
            llvm::SmallPtrSet< llvm::Instruction *, 16 > visited;
            llvm::SmallVector< llvm::Instruction *, 16 > work;
            for (llvm::Instruction *reader : readers) {
                if (visited.insert(reader).second) {
                    work.push_back(reader);
                }
            }
            while (!work.empty()) {
                llvm::Instruction *reader = work.pop_back_val();
                if (reader->hasPoisonGeneratingAnnotations()) {
                    reader->dropPoisonGeneratingAnnotations();
                    ++dropped;
                }
                // Only an integer reader that some of its own bits go unread
                // in can pass a changed bit on; a wider-than-64-bit one is
                // followed too, since the oracle does not answer for it.
                auto *type = llvm::dyn_cast< llvm::IntegerType >(reader->getType());
                if (type == nullptr) {
                    continue;
                }
                if (type->getBitWidth() <= 64
                    && demanded.MaskOf(reader) == Bitmask(type->getBitWidth()))
                {
                    continue;
                }
                for (llvm::User *user : reader->users()) {
                    if (auto *next = llvm::dyn_cast< llvm::Instruction >(user);
                        next != nullptr && visited.insert(next).second)
                    {
                        work.push_back(next);
                    }
                }
            }
            return dropped;
        }

        class Speculation
        {
          public:
            static Speculation &Instance() {
                // Never destroyed: see `Drain`.
                static auto *instance = new Speculation(ThreadCount());
                return *instance;
            }

            bool Enabled() const { return !workers_.empty(); }

            // A run of the pass, for telling its tasks from another run's: the
            // pass can run on several functions at once, and every run shares
            // the workers.
            uint64_t BeginRun() {
                const std::lock_guard< std::mutex > lock(mutex_);
                const uint64_t run = ++last_run_;
                running_by_run_[run] = 0;
                return run;
            }

            // False when `key` is being solved already.
            bool Submit(
                const std::string &key, const MBACandidate &cand, const CobraPassOptions &options,
                uint32_t budget, uint64_t run
            ) {
                auto task      = std::make_shared< Task >();
                task->key      = key;
                task->run      = run;
                task->options  = options;
                task->budget   = budget;
                // Only what the search and the proof read: no IR. The evaluator
                // is copied, which is what makes it safe to run on another
                // thread (see the contract in Evaluator.h).
                task->input.root          = nullptr;
                task->input.var_names     = cand.var_names;
                task->input.sig           = cand.sig;
                task->input.bitwidth      = cand.bitwidth;
                task->input.expr          = cand.expr ? CloneExpr(*cand.expr) : nullptr;
                task->input.evaluator     = cand.evaluator;
                task->input.demanded_mask = cand.demanded_mask;
                // How the tree reads each leaf. Facts, not IR: a candidate the
                // search phrases over a leaf has to be phrased the same way
                // here as on the loop (`LeafExpr`), or the worker answers a
                // different question and its answer is the one the cache keeps.
                task->input.leaf_bits     = cand.leaf_bits;
                task->input.leaf_widths   = cand.leaf_widths;
                task->input.holds_comparison = cand.holds_comparison;
                // The proof's budget is scaled by it (`ProofBudgetScale`). Without it a
                // worker proved every rewrite at the unscaled budget, ran out of time on
                // the larger trees, and left the loop to ask the same question again at
                // the budget it should have had: over one workload, 63 worker timeouts
                // (64 s) followed by 63 proofs on the loop (153 s), of which 3 succeeded.
                task->input.tree_size     = cand.tree_size;
                // Which reading the candidate is: the boolean cut is searched by the
                // boolean reading alone (`ProposeRewrite`), and that reading probes its
                // conditions on inputs seeded by the fingerprint, so a worker given
                // neither would answer a different question than the loop.
                task->input.node_limit    = cand.node_limit;
                task->input.fingerprint   = cand.fingerprint;
                task->leaf_needs_cast.reserve(cand.leaf_values.size());
                for (const auto *leaf : cand.leaf_values) {
                    task->leaf_needs_cast.push_back(
                        !leaf->getType()->isIntegerTy(cand.bitwidth)
                    );
                }
                const std::lock_guard< std::mutex > lock(mutex_);
                if (!by_key_.try_emplace(key, task).second) {
                    return false;
                }
                queue_.push_back(std::move(task));
                work_.notify_one();
                return true;
            }

            // Called by the loop on reaching `key`. A worker still on it is
            // waited for; a task no worker has started is withdrawn and the
            // loop solves it itself. True when a worker's answer is in the
            // outcome cache.
            bool Settle(const std::string &key, CobraStatistics &stats) {
                std::unique_lock< std::mutex > lock(mutex_);
                auto found = by_key_.find(key);
                if (found == by_key_.end()) {
                    return false;
                }
                auto task = found->second;
                by_key_.erase(found);
                if (task->state == Task::kPending) {
                    task->state = Task::kWithdrawn;
                    ++stats.background_withdrawn;
                    return false;
                }
                if (task->state == Task::kRunning) {
                    const auto begin = std::chrono::steady_clock::now();
                    done_.wait(lock, [&] { return task->state == Task::kDone; });
                    stats.background_wait_nanoseconds += Since(begin);
                }
                ++stats.background_settled;
                return true;
            }

            // The loop will not ask for `key`: a task no worker has started is
            // withdrawn, so the workers do not spend time on it and `Drain`
            // does not wait for it. One already running is left to finish.
            void Withdraw(const std::string &key, CobraStatistics &stats) {
                const std::lock_guard< std::mutex > lock(mutex_);
                auto found = by_key_.find(key);
                if (found == by_key_.end() || found->second->state != Task::kPending) {
                    return;
                }
                found->second->state = Task::kWithdrawn;
                by_key_.erase(found);
                ++stats.background_withdrawn_unneeded;
            }

            // Withdraws everything `run` queued that has not started and waits
            // for what of it is running, so that no work outlives the run that
            // queued it and no worker is busy when the process exits. Another
            // run's tasks are its own: withdrawn here, they were lost to a run
            // still counting on them, and waited for, they kept this one
            // waiting on work that was not its own.
            void Drain(uint64_t run, CobraStatistics &stats) {
                std::unique_lock< std::mutex > lock(mutex_);
                std::erase_if(queue_, [&](const std::shared_ptr< Task > &task) {
                    if (task->run != run) {
                        return false;
                    }
                    if (task->state == Task::kPending) {
                        task->state = Task::kWithdrawn;
                    }
                    return true;
                });
                std::erase_if(by_key_, [&](const auto &entry) { return entry.second->run == run; });
                const auto begin = std::chrono::steady_clock::now();
                done_.wait(lock, [&] { return running_by_run_[run] == 0; });
                running_by_run_.erase(run);
                stats.background_drain_nanoseconds += Since(begin);
            }

          private:
            struct Task
            {
                enum State : uint8_t { kPending, kRunning, kDone, kWithdrawn };

                std::string key;
                uint64_t run = 0;
                MBACandidate input{};
                CobraPassOptions options;
                uint32_t budget = 0;
                std::vector< bool > leaf_needs_cast;
                State state     = kPending;
            };

            static uint64_t Since(std::chrono::steady_clock::time_point begin) {
                return static_cast< uint64_t >(
                    std::chrono::duration_cast< std::chrono::nanoseconds >(
                        std::chrono::steady_clock::now() - begin
                    )
                        .count()
                );
            }

            static unsigned ThreadCount() {
                if (const char *count = std::getenv("COBRA_THREADS")) {
                    return static_cast< unsigned >(std::strtoul(count, nullptr, 10));
                }
                const unsigned hardware = std::thread::hardware_concurrency();
                return hardware > 2 ? std::min(4U, hardware - 2) : 0U;
            }

            explicit Speculation(unsigned threads) {
                for (unsigned index = 0; index < threads; ++index) {
                    // The search and the proof recurse over trees of up to a
                    // few thousand nodes, as they do on the loop's thread; a
                    // secondary thread's default stack is a fraction of that.
                    pthread_attr_t attributes;
                    pthread_attr_init(&attributes);
                    pthread_attr_setstacksize(&attributes, kStackBytes);
                    pthread_t thread;
                    if (pthread_create(&thread, &attributes, &Speculation::Work, this) == 0) {
                        pthread_detach(thread);
                        workers_.push_back(thread);
                    }
                    pthread_attr_destroy(&attributes);
                }
            }

            static void *Work(void *self) {
                static_cast< Speculation * >(self)->Loop();
                return nullptr;
            }

            void Loop() {
                for (;;) {
                    std::shared_ptr< Task > task;
                    {
                        std::unique_lock< std::mutex > lock(mutex_);
                        work_.wait(lock, [&] { return !queue_.empty(); });
                        task = std::move(queue_.front());
                        queue_.pop_front();
                        if (task->state != Task::kPending) {
                            continue;
                        }
                        task->state = Task::kRunning;
                        ++running_;
                        ++running_by_run_[task->run];
                    }
                    // A failure is the loop's to meet, where it would have met
                    // it without the workers: nothing is recorded for the key,
                    // and the loop solves the candidate itself.
                    try {
                        Solve(*task);
                    } catch (...) {
                    }
                    {
                        const std::lock_guard< std::mutex > lock(mutex_);
                        task->state = Task::kDone;
                        --running_;
                        --running_by_run_[task->run];
                    }
                    done_.notify_all();
                }
            }

            static void Solve(Task &task) {
                const auto begin = std::chrono::steady_clock::now();
                CobraStatistics stats;
                auto verdict  = SolveVerdict::kUnsolved;
                auto proposal = ProposeRewrite(task.input, task.options, verdict, stats);
                // Nothing found on an unfinished proof is no answer: the loop
                // asks for itself rather than being served a refusal.
                if (proposal == nullptr && RestedOnUnknown()) {
                    ++stats.background_solved;
                    stats.background_nanoseconds += Since(begin);
                    MergeStatistics(stats);
                    return;
                }
                OutcomeCache::Entry entry = proposal;
                if (proposal != nullptr) {
                    bool reads_variable = false;
                    if (MinimumInstructions(
                            *proposal->expr, proposal->real_var_positions, task.leaf_needs_cast,
                            reads_variable
                        )
                        < task.budget)
                    {
                        bool unknown  = false;
                        const auto verify_begin = std::chrono::steady_clock::now();
                        const uint64_t cpu_begin = ThreadCpuNanoseconds();
                        auto verified = VerifyCandidateOrOrdinary(
                            task.input, *proposal, task.options, stats, &unknown
                        );
                        // A proof the solver could not finish is not a refutation:
                        // the entry keeps the proposal, unproved. A worker's timeout
                        // read as "no rewrite" and cached under the key made what is
                        // simplified depend on thread timing - with workers off the
                        // loop proved what, with workers on, a worker had abandoned.
                        if (verified != nullptr) {
                            entry = std::move(verified);
                        } else if (!unknown) {
                            entry = nullptr;
                        }
                        // The same question at the same budget the loop would ask it
                        // with, so its timeout is recorded exactly as the loop's own is
                        // (`OutcomeCache::RecordUnknown`): the loop does not ask again.
                        // The rewrite stays unproved in the cache, never refused.
                        if (verified == nullptr && unknown) {
                            ++stats.background_unknown;
                            const uint64_t wall = Since(verify_begin);
                            stats.background_unknown_nanoseconds += wall;
                            const bool had_it = HadTheProcessor(wall, ThreadCpuNanoseconds() - cpu_begin);
                            stats.unknown_starved += had_it ? 0 : 1;
                            if (had_it) {
                                SharedOutcomeCache().RecordUnknown(
                                    task.key, ProofBudgetScale(task.input)
                                );
                            }
                            SharedOutcomeCache().MarkBackgroundUnknown(task.key);
                        }
                    } else {
                        ++stats.background_proofs_skipped;
                    }
                }
                SharedOutcomeCache().Insert(task.key, std::move(entry));
                ++stats.background_solved;
                stats.background_nanoseconds += Since(begin);
                MergeStatistics(stats);
            }

            static constexpr size_t kStackBytes = size_t{ 64 } << 20;

            std::mutex mutex_;
            std::condition_variable work_;
            std::condition_variable done_;
            std::deque< std::shared_ptr< Task > > queue_;
            std::unordered_map< std::string, std::shared_ptr< Task > > by_key_;
            std::vector< pthread_t > workers_;
            unsigned running_ = 0;
            uint64_t last_run_ = 0;
            std::unordered_map< uint64_t, unsigned > running_by_run_;
        };

    } // namespace

    llvm::PreservedAnalyses
    CobraPass::run(llvm::Function &f, llvm::FunctionAnalysisManager &AM) {
        bool changed = false;

        // What this run contributes to the report. It is accumulated locally
        // and merged once at the end, so nothing in the loop below touches
        // shared state.
        CobraStatistics stats;
        stats.runs           = 1;
        const bool classify  = StatisticsEnabled();
        const auto run_begin = std::chrono::steady_clock::now();
        const auto since     = [](std::chrono::steady_clock::time_point begin) {
            return static_cast< uint64_t >(
                std::chrono::duration_cast< std::chrono::nanoseconds >(
                    std::chrono::steady_clock::now() - begin
                )
                    .count()
            );
        };

        const uint64_t options_tag = OptionsTag(options_);

        DemandedBitsOracle demanded_bits(f, stats, AM, options_.check_demanded_bits);
        const DemandedMaskFn demanded = [&](llvm::Instruction *inst) {
            return demanded_bits.MaskOf(inst);
        };

        const auto detect_begin = std::chrono::steady_clock::now();
        auto candidates = DetectMbaCandidates(
            f, options_.min_ast_size, options_.max_vars, options_tag, options_.cost_model,
            options_.max_tree_nodes, demanded
        );
        stats.detect_nanoseconds += since(detect_begin);
        stats.detected = candidates.size();

        // What each queued candidate is, parallel to `candidates`: only these
        // first ones are expressions the function holds.
        std::vector< CandidateOrigin > origins(candidates.size(), CandidateOrigin::kDetected);

        // `CobraPassOptions::retry_undecided_roots`: the fingerprints of turned-down
        // roots, held until their ladders decide.
        DeferredFingerprints deferred;

        NumCandidates += candidates.size();

        auto &cache = SharedOutcomeCache();

        // Every queued candidate's key, parallel to `candidates`, and the
        // candidates handed to the workers as they are queued (`Speculation`).
        auto &speculation    = Speculation::Instance();
        const uint64_t run_id = speculation.Enabled() ? speculation.BeginRun() : 0;
        std::vector< std::string > keys;
        const auto queue_key = [&](const MBACandidate &queued) {
            keys.push_back(CandidateKey(queued, options_));
            if (speculation.Enabled() && queued.expr != nullptr && !cache.Find(keys.back()).has_value()) {
                const uint32_t budget = options_.cost_model == MbaCostModel::kTreeInstructions
                    ? queued.tree_size
                    : queued.dying_count;
                if (speculation.Submit(keys.back(), queued, options_, budget, run_id)) {
                    ++stats.background_submitted;
                }
            }
        };
        for (const auto &detected : candidates) {
            queue_key(detected);
        }

        // Rejected candidates put their inner roots back on the queue, so an
        // expression stays reachable even when the tree enclosing it is not
        // something CoBRA can do anything with. Each descent strictly shrinks
        // the tree and `attempted` bars repeats, so the queue always drains.
        //
        // They also put the same root back with the tree cut shorter. Descending
        // reaches expressions the root buries; re-cutting reaches the one the
        // root *is*, written in operands the full expansion dissolved. A tree is
        // therefore attempted once per cut, not once per root.
        //
        // A tree is claimed when it is queued, not when it is reached: the same
        // sub-expression is an inner root of every tree that encloses it, and
        // building the candidate is what costs — a tree walk and a signature
        // sweep — whether or not the queue ever gets to it.
        llvm::DenseSet< std::pair< llvm::Instruction *, uint32_t > > attempted;
        for (const auto &cand : candidates) {
            attempted.insert({ cand.root, cand.node_limit });
        }

        // For reading what the function already holds in place of building it
        // (`IrReuse`): the dominator tree, built when the first rewrite needs
        // it - the pass never touches the control flow - and the instructions
        // earlier rewrites of this run left without readers, which stay in the
        // function until the run ends.
        // The oracle's: built once per run, and the control flow never changes.
        const llvm::DominatorTree *dominators = &demanded_bits.Dominators();
        llvm::DenseSet< const llvm::Instruction * > retired;
        // Rewrites applied so far in this run, and the roots they replaced.
        uint64_t rewrites_this_run = 0;
        llvm::DenseSet< const llvm::Value * > replaced_roots;

        for (size_t index = 0; index < candidates.size(); ++index) {
            auto &cand = candidates[index];

            // Whether this candidate's question is left for a later run: its proof
            // ran out of time, its rewrite did not fit, or its tree went stale. A
            // root whose ladder holds such a rung keeps no fingerprint under
            // `retry_undecided_roots`, so the next run collects it again.
            bool left_for_later = false;
            // Set when the candidate is taken again at once (`VerifyCandidateOrOrdinary`):
            // the pass that builds the rewrite finishes it.
            bool taken_again = false;
            const auto finished = llvm::make_scope_exit([&] {
                if (!options_.retry_undecided_roots || taken_again) {
                    return;
                }
                if (const auto head = deferred.Finish(index, left_for_later)) {
                    const auto &owner = candidates[*head];
                    if (!replaced_roots.contains(owner.root)) {
                        RecordMbaFingerprint(owner.root, owner.fingerprint, options_tag);
                    }
                }
            });

            // A root an earlier rewrite replaced has no readers left. Its
            // re-cuts and inner roots were queued before that happened, and
            // solving them now buys nothing.
            // Every way of passing a queued candidate by without solving it
            // also takes back what was handed to the workers for it.
            const auto pass_by = [&] {
                if (speculation.Enabled()) {
                    speculation.Withdraw(keys[index], stats);
                }
            };

            if (cand.root->use_empty()) {
                ++stats.dead_roots;
                pass_by();
                continue;
            }

            // An unreachable root is no value at all, and reading what dominates
            // it is meaningless there: every definition dominates an
            // unreachable use, so a rewrite could read the root's own readers
            // and close a cycle through it.
            if (!dominators->isReachableFromEntry(cand.root->getParent())) {
                ++stats.unreachable_roots;
                pass_by();
                continue;
            }

            // A tree collected before a root inside it was replaced describes
            // values the root no longer reads: the replaced root stays in the
            // function until the run ends, unread, still computing the old tree
            // with its old flags and its own reading of what may be poison. As
            // a leaf it would be read back by the rewrite; inside the tree it
            // is a shape the next run will not see. Either way the tree is left
            // for the next run, which collects it from the simplified function.
            //
            // Measured both ways over one whole workload, with identical
            // labels: skipping only trees that *read* a replaced root as a leaf
            // cost 98.5 s and emitted 2814 instructions; skipping every tree
            // that holds one cost 45.5 s and emitted 2756 - the trees built on
            // a replaced root were mostly the ones the solver ran out of time on.
            //
            // The summands a sum-group candidate adds back (`sum_residual`) are
            // read the same way, as they were when the candidate was queued, so
            // a replaced one is the same stale reading: added back, it would give
            // a replaced root a reader again - one the deletion of the run's
            // leftovers below does not expect, and would delete from under it.
            if (!replaced_roots.empty()
                && (llvm::any_of(cand.tree, [&](const llvm::Instruction *node) {
                        return replaced_roots.contains(node);
                    })
                    || llvm::any_of(cand.leaf_values, [&](const llvm::Value *leaf) {
                           return replaced_roots.contains(leaf);
                       })
                    || llvm::any_of(cand.sum_residual, [&](const auto &summand) {
                           return replaced_roots.contains(summand.first);
                       })))
            {
                ++stats.stale_trees;
                left_for_later = true;
                pass_by();
                continue;
            }

            // The demanded mask was read when the candidate was queued. A
            // rewrite since then can give the root readers it did not have -
            // an earlier rewrite may *be* this root, read back as the cheapest
            // way to build its replacement - and a proof on the old, narrower
            // mask says nothing about the bits those readers look at. So the
            // mask is read again, and a candidate whose readers now look at
            // more than it was collected for is left for the next run, which
            // collects it afresh.
            if (rewrites_this_run != 0 && cand.root->getType()->isIntegerTy()
                && cand.root->getType()->getIntegerBitWidth() <= 64)
            {
                const uint64_t now = CandidateDemand(
                    demanded_bits.MaskOf(cand.root), cand.bitwidth, cand.slice_extension,
                    !cand.sum_residual.empty()
                );
                if ((now & ~cand.demanded_mask) != 0) {
                    ++stats.demanded_grew;
                    left_for_later = true;
                    pass_by();
                    continue;
                }
            }

            // Only a full collection speaks for the tree rooted at an
            // instruction; a re-cut saw part of it, so recording its verdict
            // would tell the next run the whole tree was examined.
            bool undecided_refusal = false;
            const auto record_rejection = [&] {
                if (cand.node_limit != 0 || undecided_refusal) {
                    return;
                }
                if (options_.retry_undecided_roots) {
                    deferred.Defer(index);
                } else {
                    RecordMbaFingerprint(cand.root, cand.fingerprint, options_tag);
                }
            };

            // Re-cuts first: they are the only candidates that read the same
            // operands this one did, so they are worth trying while the IR still
            // looks the way they were collected from.
            //
            // Every list is built before any is queued: pushing can
            // reallocate `candidates`, and `cand` is a reference into it.
            const auto requeue_retries = [&] {
                const auto retry_begin = std::chrono::steady_clock::now();
                // The slice first. Every other rung re-reads the root at its
                // own width, so none of them can reach an identity the
                // obfuscator wrote across an extension; and the slice is decided
                // by a proof about the root rather than by a search for a
                // rewrite, so a root that is not an extension of its low bits
                // costs probes and nothing else.
                const auto slice_begin = std::chrono::steady_clock::now();
                std::vector< MBACandidate > slices;
                // The question is asked of the root as collected, and then of
                // its exact reading: a sign extension written as a `select` on
                // the sign of the narrow value is no extension of anything
                // while that `select` is a free leaf, and one exactly when its
                // narrow comparison is read (`SliceToRead`). The exact reading
                // only answers the question; it is never searched itself, so
                // no rewrite spells a narrow condition anew at the tree width,
                // apart from the very `icmp` the rest of the function shares.
                const MBACandidate *claimed = &cand;
                auto read                   = SliceToRead(cand, options_, stats);
                std::vector< MBACandidate > exact;
#ifdef COBRA_HAS_Z3
                if (!read && options_.boolean_variables && options_.z3_verify && HoldsComparisonLeaf(cand)) {
                    exact = ExactNarrowReadingMbaCandidate(
                        cand, options_.min_ast_size, options_.max_vars, options_tag,
                        options_.cost_model, options_.max_tree_nodes, demanded
                    );
                }
#endif
                for (const auto &reading : exact) {
                    if ((read = SliceToRead(cand, options_, stats, &reading))) {
                        claimed = &reading;
                        break;
                    }
                }
                if (read) {
                    slices = SliceMbaCandidate(
                        cand, read->first, read->second, options_.min_ast_size, options_.max_vars,
                        options_tag, options_.cost_model, options_.max_tree_nodes, demanded
                    );
                    if (!slices.empty()) {
                        const auto claim =
                            std::make_shared< const ExtensionClaim >(ExtensionClaim{
                                .expr = std::shared_ptr< const Expr >(CloneExpr(*claimed->expr)),
                                .bitwidth  = claimed->bitwidth,
                                .vars      = static_cast< uint32_t >(claimed->var_names.size()),
                                .tree_size = static_cast< uint32_t >(claimed->tree_size) });
                        for (auto &slice : slices) {
                            slice.extension_claim = claim;
                        }
                    }
                }
                stats.slice_nanoseconds += since(slice_begin);

                // Then the same question about one group of the root's sum. A
                // base added into the sum is enough to refuse the slice above -
                // the high bits of `base + 136 + sext(e)` are the base's, so
                // the root is the extension of nothing - while the part
                // standing on `e` is a sign extension and reads as `e` at 32
                // bits. The groups that share no leaf come apart, and the
                // question is asked of each (`SumGroupSliceMbaCandidates`).
                //
                // Only when the slice of the whole root found nothing: a root
                // that *is* an extension of its low bits is already the
                // smaller problem, and splitting its sum would hand the search
                // the same identity with a residual to add back.
                const auto sum_begin = std::chrono::steady_clock::now();
                std::vector< MBACandidate > groups;
                if (slices.empty()) {
                    ++stats.sum_group_roots;
                    groups = SumGroupSliceMbaCandidates(
                        cand, options_.min_ast_size, options_.max_vars, options_tag,
                        [&](const MBACandidate &group) {
                            return SliceToRead(group, options_, stats);
                        },
                        options_.cost_model, options_.max_tree_nodes, demanded
                    );
                    if (!groups.empty()) {
                        ++stats.sum_group_split;
                        stats.sum_group_groups += groups.size();
                    }
                }
                stats.sum_group_nanoseconds += since(sum_begin);

                // The boolean cut (`boolean_variables`): the root with its
                // narrow comparisons taken in, so that the reading of its
                // conditions as variables can prove one of them the same as a
                // sign computed from the values it compares.
                std::vector< MBACandidate > boolean;
#ifdef COBRA_HAS_Z3
                if (options_.boolean_variables && options_.z3_verify && ReadsConditions(cand)) {
                    boolean = BooleanCutMbaCandidate(
                        cand, options_.min_ast_size, options_.max_vars, options_tag,
                        options_.cost_model, options_.max_tree_nodes, demanded
                    );
                }
#endif

                // The boundary cut: it is the one reading of the root
                // the ladder below cannot reach, and it costs a single solve.
                auto boundary = BoundaryCutMbaCandidate(
                    cand, options_.min_ast_size, options_.max_vars, options_tag,
                    options_.cost_model, options_.max_tree_nodes, demanded
                );
                stats.boundary_nanoseconds += since(retry_begin);
                // Then the reading that keeps what the tree reads more than
                // once whole: the one that can answer with a value the
                // function already holds.
                const auto shared_begin = std::chrono::steady_clock::now();
                auto shared = SharedCutMbaCandidate(
                    cand, options_.min_ast_size, options_.max_vars, options_.max_shared_cuts,
                    options_tag, options_.cost_model, options_.max_tree_nodes, demanded
                );
                stats.shared_nanoseconds += since(shared_begin);
                const auto recut_begin = std::chrono::steady_clock::now();
                auto recuts = RecutMbaCandidate(
                    cand, options_.min_ast_size, options_.max_vars, options_.max_recut_nodes,
                    options_.max_recut_vars, options_tag, options_.cost_model, demanded
                );
                stats.recut_nanoseconds += since(recut_begin);
                const auto inner_begin = std::chrono::steady_clock::now();

                // Only a full collection speaks for what the tree holds. A
                // re-cut is another view of the same root, not a different set
                // of sub-expressions: its inner roots are a subset of the ones
                // the full candidate already handed back, so descending again
                // from every rung of the ladder re-queues the same trees over
                // and over.
                llvm::SmallVector< llvm::Instruction *, 8 > fresh;
                if (cand.node_limit == 0) {
                    for (auto *inner : cand.inner_roots) {
                        if (!attempted.contains({ inner, 0u })) {
                            fresh.push_back(inner);
                        }
                    }
                }
                auto inners = ExpandMbaCandidate(
                    fresh, options_.min_ast_size, options_.max_vars, options_tag,
                    options_.cost_model, options_.max_tree_nodes, demanded
                );
                stats.inner_nanoseconds += since(inner_begin);
                for (auto &cut : slices) {
                    if (attempted.insert({ cut.root, cut.node_limit }).second) {
                        queue_key(cut);
                        candidates.push_back(std::move(cut));
                        origins.push_back(CandidateOrigin::kSlice);
                        if (options_.retry_undecided_roots) {
                            deferred.Queue(candidates.size() - 1, index);
                        }
                        ++stats.requeued;
                    }
                }
                for (auto &cut : groups) {
                    if (attempted.insert({ cut.root, cut.node_limit }).second) {
                        queue_key(cut);
                        candidates.push_back(std::move(cut));
                        origins.push_back(CandidateOrigin::kSumGroup);
                        if (options_.retry_undecided_roots) {
                            deferred.Queue(candidates.size() - 1, index);
                        }
                        ++stats.requeued;
                    }
                }
                for (auto &cut : boolean) {
                    if (attempted.insert({ cut.root, cut.node_limit }).second) {
                        queue_key(cut);
                        candidates.push_back(std::move(cut));
                        origins.push_back(CandidateOrigin::kBooleanCut);
                        if (options_.retry_undecided_roots) {
                            deferred.Queue(candidates.size() - 1, index);
                        }
                        ++stats.requeued;
                    }
                }
                for (auto &cut : boundary) {
                    if (attempted.insert({ cut.root, cut.node_limit }).second) {
                        queue_key(cut);
                        candidates.push_back(std::move(cut));
                        origins.push_back(CandidateOrigin::kBoundaryCut);
                        if (options_.retry_undecided_roots) {
                            deferred.Queue(candidates.size() - 1, index);
                        }
                        ++stats.requeued;
                    }
                }
                for (auto &cut : shared) {
                    if (attempted.insert({ cut.root, cut.node_limit }).second) {
                        queue_key(cut);
                        candidates.push_back(std::move(cut));
                        origins.push_back(CandidateOrigin::kSharedCut);
                        if (options_.retry_undecided_roots) {
                            deferred.Queue(candidates.size() - 1, index);
                        }
                        ++stats.requeued;
                    }
                }
                for (auto &recut : recuts) {
                    if (attempted.insert({ recut.root, recut.node_limit }).second) {
                        queue_key(recut);
                        candidates.push_back(std::move(recut));
                        origins.push_back(CandidateOrigin::kRecut);
                        if (options_.retry_undecided_roots) {
                            deferred.Queue(candidates.size() - 1, index);
                        }
                        ++stats.requeued;
                    }
                }
                for (auto &inner : inners) {
                    if (attempted.insert({ inner.root, inner.node_limit }).second) {
                        queue_key(inner);
                        candidates.push_back(std::move(inner));
                        origins.push_back(CandidateOrigin::kInnerRoot);
                        ++stats.requeued;
                    }
                }
                stats.ladder_nanoseconds += since(retry_begin);
            };

            // Everything the report wants to know about this candidate is read
            // before the solve: afterwards the root may have been replaced, and
            // what used to read it reads the rewrite instead.
            const unsigned variables = static_cast< unsigned >(
                std::min< size_t >(cand.var_names.size(), CobraStatistics::kMaxVariableRow)
            );
            // Asked unconditionally when the policy needs it: it decides whether a
            // loop-carried candidate is one of the cheap address-reaching ones.
            const uint32_t consumers = (classify || options_.skip_loop_carried_phis)
                ? ConsumerMask(cand.root)
                : 0U;
            const auto consumer      = PrimaryConsumer(consumers);
            // Whether this candidate stands on a value a loop carries. The dominator tree
            // is built on demand, here as well as for the rewrites, because this question
            // is asked of every candidate.
            const bool loop_phi = TouchesLoopCarriedPhi(cand, *dominators);
            const bool addresses =
                (consumers & ConsumerBit(CandidateConsumer::kPointerOffset)) != 0;

            // What this candidate cost, filled in once the solve is done, and
            // added to every row it belongs to when its fate is known.
            uint64_t spent = 0;
            // Whether every leaf is a constant: then the root is something a plain
            // constant fold reaches and the search was never the thing that would settle
            // it. Read here, before the solve, like everything else the report wants.
            const bool constant_leaves =
                !cand.leaf_values.empty()
                && llvm::all_of(cand.leaf_values, [](const llvm::Value *leaf) {
                       return llvm::isa< llvm::Constant >(leaf);
                   });
            const auto account = [&](bool solved) {
                const unsigned phi_row = loop_phi ? 1 : 0;
                auto &by_phi = stats.by_loop_phi[phi_row];
                ++by_phi.candidates;
                by_phi.solved += solved ? 1 : 0;
                by_phi.nanoseconds += spent;
                if (classify) {
                    auto &crossed = stats.by_loop_phi_consumer[phi_row][static_cast< unsigned >(consumer)];
                    ++crossed.candidates;
                    crossed.solved += solved ? 1 : 0;
                    crossed.nanoseconds += spent;
                }
                if (constant_leaves) {
                    auto &leaves = stats.by_loop_phi_constant_leaves[phi_row];
                    ++leaves.candidates;
                    leaves.solved += solved ? 1 : 0;
                    leaves.nanoseconds += spent;
                }
                auto &by_variables = stats.by_variables[variables];
                ++by_variables.candidates;
                by_variables.solved += solved ? 1 : 0;
                by_variables.nanoseconds += spent;
                auto &by_origin = stats.by_origin[static_cast< unsigned >(origins[index])];
                ++by_origin.candidates;
                by_origin.solved += solved ? 1 : 0;
                by_origin.nanoseconds += spent;
                if (!classify) {
                    return;
                }
                auto &by_consumer = stats.by_consumer[static_cast< unsigned >(consumer)];
                ++by_consumer.candidates;
                by_consumer.solved += solved ? 1 : 0;
                by_consumer.nanoseconds += spent;
                if (addresses) {
                    ++stats.reaching_pointer_offset;
                    stats.solved_reaching_pointer_offset += solved ? 1 : 0;
                    stats.pointer_offset_nanoseconds += spent;
                }
            };

            // Declined before anything is searched or proved, when the caller asked for
            // that. After `account` so the row still counts it; nothing below has run, so
            // its cost is the zero it reports.
            //
            // **Except the ones that reach an address**, and that exception is what makes
            // the setting worth having. Crossing the loop-carried split with what reads the
            // root, on the same CFG and the same 11 runs:
            //
            //     of the 2597 candidates standing on a loop-carried value
            //       pointer offset   679 candidates   11 rewrites    0.13 s
            //       stored value     542               8 rewrites   29.69 s
            //       compared         398               7 rewrites   24.97 s
            //       call argument    741               7 rewrites    0.16 s
            //       other            237               9 rewrites    0.29 s
            //
            // The expensive ones are not the addresses. Keeping the address-reaching ones
            // costs 0.13 s of the 55.24 s and keeps every rewrite that can make a pointer
            // fold, which for a caller recovering control flow is the whole point of the
            // pass. See `CobraPassOptions::skip_loop_carried_phis`.
            const bool reaches_an_address =
                (consumers & ConsumerBit(CandidateConsumer::kPointerOffset)) != 0;
            if (loop_phi && options_.skip_loop_carried_phis && !reaches_an_address) {
                ++stats.declined_loop_carried;
                pass_by();
                account(false);
                continue;
            }

            // Without an expression there is nothing for the solver to prove
            // the rewrite against, and `VerifyCandidate` turns every such
            // rewrite down: searching for one is time spent on an answer that
            // cannot be used. Treated exactly as that refusal is.
            if (cand.expr == nullptr && options_.z3_verify) {
                ++stats.declined_no_expression;
                account(false);
                record_rejection();
                requeue_retries();
                continue;
            }

            // A copy: queuing more candidates below can move `keys`.
            const std::string key = keys[index];

            OutcomeCache::Entry outcome;
            bool served_from_cache = false;
            auto verdict           = SolveVerdict::kUnsolved;
            const auto solve_begin = std::chrono::steady_clock::now();
            if (speculation.Enabled()) {
                speculation.Settle(key, stats);
            }
            if (auto cached = cache.Find(key)) {
                ++NumOutcomeCacheHits;
                outcome           = std::move(*cached);
                served_from_cache = true;
            } else {
                const bool evicted = cache.WasEvicted(key);
                auto proposal = ProposeRewrite(cand, options_, verdict, stats);
                undecided_refusal = proposal == nullptr && RestedOnUnknown();
                left_for_later    = undecided_refusal;
                outcome = undecided_refusal ? nullptr : cache.Insert(key, std::move(proposal));
                if (evicted) {
                    ++stats.resolved_after_eviction;
                    stats.resolved_after_eviction_nanoseconds += since(solve_begin);
                }
            }
            spent = since(solve_begin);
            stats.solve_nanoseconds += spent;
            if (served_from_cache) {
                ++stats.cache_hits;
                stats.cache_nanoseconds += spent;
                if (outcome == nullptr) {
                    ++stats.rejected_cached;
                }
            } else if (outcome == nullptr) {
                stats.rejected_nanoseconds += spent;
                ++(verdict == SolveVerdict::kNotCheaper ? stats.rejected_cost
                                                        : stats.rejected_unsolved);
            } else {
                stats.solved_nanoseconds += spent;
            }
            // `COBRA_DETECT_DEBUG=1`: every candidate and what became of it.
            //
            // The statistics say how many were rejected but not which, and a
            // candidate that is solved on a worker thread arrives at the loop as
            // a cache hit, so neither the counters nor the slow-candidate report
            // can name the expression that did not fold. This line can.
            {
                static const bool trace = std::getenv("COBRA_DETECT_DEBUG") != nullptr;
                if (trace) {
                    llvm::errs() << "cobra: candidate" << *cand.root << "  [cut " << cand.node_limit
                                 << ", tree " << cand.tree.size() << ", vars "
                                 << cand.var_names.size() << "] -> "
                                 << (outcome == nullptr ? "rejected" : "solved")
                                 << (served_from_cache ? " (cached)" : "")
                                 << (outcome == nullptr && !RejectReason().empty()
                                         ? ": " + RejectReason()
                                         : std::string())
                                 << "\n";
                }
            }
            const uint64_t solve_ms = spent / 1000000;
            const bool wide = SlowReport().variable_floor != 0
                && cand.var_names.size() >= SlowReport().variable_floor;
            if (SlowReport().enabled && !served_from_cache
                && (solve_ms >= SlowReport().threshold_ms || wide))
            {
                static uint64_t slow_sequence = 0;
                RecordCandidate(
                    SlowReport().directory, slow_sequence, f, cand, outcome.get(), solve_ms,
                    outcome == nullptr ? "rejected" : "solved"
                );
            }
            // An address expression CoBRA rewrote is one a pass dedicated to
            // addresses could have folded more cheaply, so its shape is written
            // out for that pass to be built against.
            if (addresses && outcome != nullptr && !SlowReport().addresses.empty()) {
                static uint64_t address_sequence = 0;
                if (address_sequence < SlowReport().address_cap) {
                    RecordCandidate(
                        SlowReport().addresses, address_sequence, f, cand, outcome.get(),
                        solve_ms, "reaches an address"
                    );
                }
            }
            if (outcome == nullptr) {
                // Nothing to rewrite here. Leaving the tree unmarked would have
                // the next run rediscover and re-solve it from scratch, which
                // is the dominant cost when a function is re-optimized, so the
                // shape that led nowhere is recorded on the root.
                account(false);
                record_rejection();
                requeue_retries();
                continue;
            }

            // A proof of this exact question has already run its budget out, so asking it
            // again at the same budget cannot do better - see `OutcomeCache::RecordUnknown`
            // for what that was costing. Handled exactly as a fresh timeout is: the rewrite
            // is not made this run and nothing is recorded against the tree. Asked before
            // the rewrite is built, since nothing it says depends on the IR.
            if (!outcome->verified && cache.WasUnknown(key, ProofBudgetScale(cand))) {
                ++stats.solver_unknown_skipped;
                ++stats.rejected_unverified;
                left_for_later = true;
                account(false);
                requeue_retries();
                continue;
            }

            // Build variable index map for aux var elimination.
            // real_vars may be a subset of var_names with
            // reindexed positions.
            // By position, never by name: see `CandidateOutcome::real_var_positions`.
            std::vector< uint32_t > var_map;
            const auto &real_vars  = outcome->real_vars;
            const auto &positions  = outcome->real_var_positions;
            bool identity          = positions.size() == cand.var_names.size();
            for (uint32_t i = 0; identity && i < positions.size(); ++i) {
                identity = positions[i] == i;
            }
            if (!real_vars.empty() && !identity) {
                var_map = positions;
            }

            const auto rebuild_begin = std::chrono::steady_clock::now();

            // Everything the builder emits lands between `mark` and the root, so
            // the replacement can be priced in the only currency that matters —
            // instructions — before anything commits to it.
            auto *block         = cand.root->getParent();
            const bool kAtFront = cand.root->getIterator() == block->begin();
            const auto kMark    = kAtFront ? block->end() : std::prev(cand.root->getIterator());

            llvm::IRBuilder<> builder(cand.root);

            // A tree collected through a `freeze` is rewritten without it: the
            // replacement reads the leaves directly. The `freeze` was what made
            // the root defined when the value under it was not, so dropping it
            // would let poison reach a reader that used to be safe from it -
            // the one direction a rewrite may not take.
            //
            // Freezing the leaves restores it at the other end. Reconstruction
            // emits no poison-generating flag of its own, so a replacement built
            // from defined leaves is defined for every input, and a defined
            // value refines whatever the original produced. Leaves already known
            // not to be poison are left alone, which is most of them - a load
            // carrying `!noundef`, an argument declared `noundef` - so this
            // usually costs nothing at all.
            //
            // A `select` asks for the same thing for a different reason: it does
            // not read the arm it does not take, so a leaf under that arm can be
            // poison while the root is not, and the rewrite reads every leaf it
            // names (`select (icmp ult %n, 32), (lshr %x, %n), 0` is defined for
            // every `%n`; a rewrite reading the `lshr` is not). A frozen leaf
            // holds some value, and the rewrite is proved for every value of its
            // leaves, so freezing one cannot change the result.
            //
            // A leaf is frozen the first time the rewrite reads it, not before:
            // one the rewrite does not read would cost an instruction for nothing.
            //
            // An `undef` leaf the rewrite reads more than once is owed one in
            // any tree. Each read of `undef` may see a different value, so
            // `x + x` written for `2 * x` can produce a value the tree never
            // could; read once, the leaf is one value and the proof covers it.
            // Frozen, it is one value however often it is read. Loads the host
            // marks `!noundef` and `noundef` arguments are known not to be
            // `undef`, and so is whatever is computed from them alone, however
            // deep (`NeverUndef`), so this rarely costs anything.
            std::vector< uint32_t > reads(cand.leaf_values.size(), 0);
            if (outcome->boolean_reading) {
                // Built with each distinct sub-expression once (`ReconstructIr`'s
                // `condition_forms`), so a leaf is read once per distinct reader:
                // the reading of a `select` spells its else arm twice and a
                // condition put back over its values repeats what they share,
                // and counted per occurrence such a leaf would be frozen for
                // reads the IR never makes - and, frozen, match no instruction
                // the function already holds.
                std::vector< const Expr * > distinct;
                std::unordered_multimap< size_t, const Expr * > seen;
                llvm::SmallVector< const Expr *, 32 > pending{ outcome->expr.get() };
                while (!pending.empty()) {
                    const Expr *node = pending.pop_back_val();
                    const size_t hash = std::hash< Expr >{}(*node);
                    const auto range  = seen.equal_range(hash);
                    if (std::any_of(range.first, range.second, [&](const auto &entry) {
                            return ExprStructurallyEqual(*entry.second, *node);
                        }))
                    {
                        continue;
                    }
                    seen.emplace(hash, node);
                    distinct.push_back(node);
                    for (const auto &child : node->children) {
                        pending.push_back(child.get());
                    }
                }
                const auto read = [&](const Expr &node) {
                    if (node.kind == Expr::Kind::kVariable) {
                        const uint32_t leaf = var_map.empty() ? node.var_index : var_map[node.var_index];
                        if (leaf < reads.size()) {
                            ++reads[leaf];
                        }
                    }
                };
                read(*outcome->expr);
                for (const Expr *node : distinct) {
                    for (const auto &child : node->children) {
                        read(*child);
                    }
                }
            } else {
                llvm::SmallVector< const Expr *, 32 > pending{ outcome->expr.get() };
                while (!pending.empty()) {
                    const Expr *node = pending.pop_back_val();
                    if (node->kind == Expr::Kind::kVariable) {
                        const uint32_t leaf = var_map.empty() ? node->var_index : var_map[node->var_index];
                        if (leaf < reads.size()) {
                            ++reads[leaf];
                        }
                    }
                    for (const auto &child : node->children) {
                        pending.push_back(child.get());
                    }
                }
            }
            // The rewrite is built at the root and reads its leaves there, so
            // each one it reads has to be available at the root. A tree read
            // through a transparent phi follows one arm, and a leaf inside that
            // arm - `lshr %x, %s` in the predecessor, as opaque as any other
            // variable shift - is defined on that path only, however surely the
            // other arms compute the same thing. A rewrite that does not read
            // such a leaf is a function of the others, which every arm agrees
            // on, so only the leaves read are asked about.
            {
                const auto available = [&](const llvm::Value *value) {
                    const auto *inst = llvm::dyn_cast< llvm::Instruction >(value);
                    return inst == nullptr || dominators->dominates(inst, cand.root);
                };
                bool reads_unavailable = !llvm::all_of(cand.sum_residual, [&](const auto &summand) {
                    return available(summand.first);
                });
                for (size_t index = 0; !reads_unavailable && index < reads.size(); ++index) {
                    reads_unavailable = reads[index] != 0 && !available(cand.leaf_values[index]);
                }
                if (reads_unavailable) {
                    ++stats.unavailable_leaves;
                    account(false);
                    requeue_retries();
                    continue;
                }
            }

            LeafSupply leaf_supply;
            const bool poison_owed = cand.spans_freeze || cand.spans_select;
            const auto poisoning   = poison_owed ? LeavesPoisoningTheRoot(cand, outcome->boolean_reading)
                                                 : llvm::DenseSet< const llvm::Value * >{};
            leaf_supply.values     = cand.leaf_values;
            leaf_supply.freeze_on_use.reserve(cand.leaf_values.size());
            bool freeze_leaves = false;
            llvm::SmallPtrSet< const llvm::Value *, 32 > never_undef;
            for (size_t index = 0; index < cand.leaf_values.size(); ++index) {
                auto *leaf        = cand.leaf_values[index];
                const bool poison = poison_owed && !poisoning.contains(leaf)
                    && !llvm::isGuaranteedNotToBePoison(leaf);
                const bool undef  = reads[index] > 1 && !llvm::isGuaranteedNotToBeUndef(leaf)
                    && !NeverUndef(leaf, never_undef);
                const bool freeze = options_.leaf_freezing == LeafFreezing::kAcrossFreeze
                    ? cand.spans_freeze && !llvm::isGuaranteedNotToBePoison(leaf)
                    : poison || undef;
                leaf_supply.freeze_on_use.push_back(freeze);
                freeze_leaves |= freeze;
            }

            // What the function already computes is read, not built again. A
            // flagged instruction of the tree can be read where the root was
            // poison whenever it was; the frozen leaves of a tree that spans a
            // `freeze` are new values nothing reads yet, so there the question
            // does not come up.
            IrReuse reuse;
            llvm::SmallPtrSet< const llvm::Instruction *, 32 > poisons_root;
            reuse.before     = cand.root;
            reuse.dominators = dominators;
            reuse.replaced   = &replaced_roots;
            if (cand.poison_reaches_root) {
                poisons_root.insert(cand.tree.begin(), cand.tree.end());
                reuse.poisons_root = &poisons_root;
            }

            auto *new_val = ReconstructIr(
                *outcome->expr, cand, builder, var_map, freeze_leaves ? &leaf_supply : nullptr,
                &reuse, outcome->boolean_reading
            );

            // A slice was searched and proved on the low bits of its root, so
            // what replaces the root is the extension the claim named
            // (`SliceToRead`): the rewrite of the bits read, widened back to
            // the root's type. Built here, before the emitted instructions are
            // gathered, so the cast is priced with the rest of the rewrite -
            // and it is the one instruction the whole reading costs, against a
            // tree the other width could not fold at all.
            if (new_val != nullptr && cand.slice_extension != SliceExtension::kNone) {
                new_val = builder.CreateCast(
                    cand.slice_extension == SliceExtension::kZero ? llvm::Instruction::ZExt
                                                                  : llvm::Instruction::SExt,
                    new_val, cand.root->getType(), "cobra.slice"
                );
            }

            // And the summands the candidate left out of the root's sum
            // (`SumGroupSliceMbaCandidates`): the group is what the search
            // answered, the residual is the rest of the root's value, and the
            // root is the two added. The summands are read from the IR that
            // already computes them - they are operands of the sum chain, so
            // they dominate the root - and no flag is carried over: the sum is
            // re-associated, so the intermediate values are not the ones the
            // old chain's `nsw` and `nuw` were asserted of. Built here, before
            // the emitted instructions are gathered, so every one of them is
            // priced with the rest of the rewrite.
            if (new_val != nullptr) {
                for (const auto &[term, negated] : cand.sum_residual) {
                    new_val = negated ? builder.CreateSub(new_val, term, "cobra.sum")
                                      : builder.CreateAdd(new_val, term, "cobra.sum");
                }
            }

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
            // progress. Pricing it in instructions is what the rewrite is really
            // worth; which instructions count is the caller's call.
            const uint32_t budget = options_.cost_model == MbaCostModel::kTreeInstructions
                ? cand.tree_size
                : cand.dying_count;
            const auto discard_rewrite = [&] {
                for (auto *dead : llvm::reverse(emitted)) {
                    if (dead->use_empty()) {
                        dead->eraseFromParent();
                    }
                }
            };

            // An instruction the rewrite reads is priced like one it builds
            // wherever the cost model would have counted it gone. Measured on
            // the whole expression that is every one of them: the expression
            // after the rewrite is what it built and what it read, whoever
            // computed the latter first. Measured on what the rewrite removes,
            // it is the ones that were going to disappear - with this root, or
            // with one an earlier rewrite replaced - and now stay; one the rest
            // of the function keeps alive regardless costs nothing to read.
            size_t charged = 0;
            if (options_.cost_model == MbaCostModel::kTreeInstructions) {
                charged = reuse.reused.size();
            } else if (!reuse.reused.empty()) {
                const llvm::SmallPtrSet< const llvm::Instruction *, 32 > dying(
                    cand.dying.begin(), cand.dying.end()
                );
                charged = static_cast< size_t >(
                    llvm::count_if(reuse.reused, [&](const llvm::Instruction *read) {
                        return dying.contains(read) || retired.contains(read);
                    })
                );
            }
            const size_t cost = emitted.size() + charged;

            if (cost >= budget) {
                if (DetectDebug() && std::getenv("COBRA_BOOLEAN_DEBUG") != nullptr) {
                    for (const auto *inst : emitted) {
                        llvm::errs() << "    built" << *inst << "\n";
                    }
                    for (const auto *inst : reuse.reused) {
                        llvm::errs() << "    read " << *inst << "\n";
                    }
                }
                discard_rewrite();
                ++NumSkippedCost;
                LLVM_DEBUG(
                    llvm::dbgs() << "CoBRA: skipping — rewrite emits " << emitted.size()
                                 << " instructions and keeps " << charged
                                 << " against a budget of " << budget << "\n"
                );
                // The one gate the trace could not name. A candidate the search
                // answered and the budget turned down reads as "solved" beside
                // one whose rewrite reached the function, so a rewrite that is
                // found and then priced out is invisible - which is exactly the
                // case to act on, because it says the answer is there and the
                // reconstruction is what is too expensive.
                if (DetectDebug()) {
                    llvm::errs() << "cobra: priced out" << *cand.root << "  [cut "
                                 << cand.node_limit << "] -> built " << emitted.size()
                                 << ", kept " << charged << ", budget " << budget << "\n";
                }
                const uint64_t rebuilding = since(rebuild_begin);
                stats.rebuild_nanoseconds += rebuilding;
                stats.rejected_nanoseconds += rebuilding;
                spent += rebuilding;
                ++stats.rejected_budget;
                // The rewrites that are found and then turned down are the ones
                // a better reconstruction could still win, so with the report
                // on each is written out with what it came to.
                if (SlowReport().enabled) {
                    static uint64_t budget_sequence = 0;
                    const std::string verdict = "over budget: built " + std::to_string(emitted.size())
                        + ", read " + std::to_string(reuse.reused.size()) + " (" + std::to_string(charged)
                        + " priced), against " + std::to_string(budget);
                    RecordCandidate(
                        SlowReport().directory + "/budget", budget_sequence, f, cand, outcome.get(),
                        since(rebuild_begin) / 1000000, verdict.c_str()
                    );
                }
                if (served_from_cache) {
                    ++stats.cached_over_budget;
                } else {
                    ++stats.proved_over_budget;
                    stats.proved_over_budget_nanoseconds += spent;
                }
                // Not recorded against the tree: the rewrite exists, and what
                // it costs depends on more than the tree - which instructions
                // the function already holds to read, which leaves need a
                // freeze, how many of the tree's instructions die with the
                // root. A later run may find it fits; the outcome cache keeps
                // the search from being paid again.
                left_for_later = true;
                account(false);
                requeue_retries();
                continue;
            }

            // A slice stands on its claim about the root, proved now that a
            // rewrite of it fits (`ExtensionClaim`). The claim is no rewrite,
            // so its verdict is not cached against the slice's key: a later
            // root with the same slice makes its own claim.
            if (cand.slice_extension != SliceExtension::kNone) {
                const auto claim_begin = std::chrono::steady_clock::now();
                const auto claim         = cand.extension_claim == nullptr
                            ? ClaimVerdict::kRefuted
                            : ProveExtensionClaim(
                          *cand.extension_claim, cand.bitwidth, cand.slice_extension, options_,
                          stats
                      );
                const uint64_t claiming = since(claim_begin);
                stats.slice_nanoseconds += claiming;
                spent += claiming;
                if (claim != ClaimVerdict::kProved) {
                    discard_rewrite();
                    stats.rebuild_nanoseconds += since(rebuild_begin) - claiming;
                    ++stats.slice_claim_failed;
                    left_for_later = claim == ClaimVerdict::kUnknown;
                    account(false);
                    requeue_retries();
                    continue;
                }
            }

            // The rewrite fits, so it is worth proving now - once per key: the
            // verified rewrite replaces the proposal in the cache, and a failed
            // proof replaces it with the rejection it would always have been.
            uint64_t verifying = 0;
            if (!outcome->verified) {
                const auto verify_begin  = std::chrono::steady_clock::now();
                const uint64_t cpu_begin = ThreadCpuNanoseconds();
                bool unknown             = false;
                auto verified            = VerifyCandidateOrOrdinary(cand, *outcome, options_, stats, &unknown);
                // A proof that did not finish says nothing about the rewrite,
                // so it leaves no trace: neither a null in the cache nor a
                // fingerprint on the root. Both would turn "the solver ran out
                // of time on this thread at this moment" into "this tree has
                // nothing to find", and the second is written into the IR and
                // travels with it - one timeout and the tree is never examined
                // again, however much budget a later run would have given it.
                // A refutation is different and is recorded: the solver found
                // a counterexample, and it will find it again.
                // Read from the proof itself, not inferred from whether the
                // unknown-time counter moved.
                const bool proof_unknown = verified == nullptr && unknown;
                if (cache.WasBackgroundUnknown(key)) {
                    ++(verified != nullptr ? stats.after_background_unknown_proved
                       : proof_unknown     ? stats.after_background_unknown_unknown
                                           : stats.after_background_unknown_refuted);
                    stats.after_background_unknown_nanoseconds += since(verify_begin);
                }
                if (proof_unknown) {
                    // See `HadTheProcessor`.
                    if (HadTheProcessor(since(verify_begin), ThreadCpuNanoseconds() - cpu_begin)) {
                        cache.RecordUnknown(key, ProofBudgetScale(cand));
                    } else {
                        ++stats.unknown_starved;
                    }
                } else if (verified == nullptr) {
                    cache.Refute(key);
                } else {
                    cache.Insert(key, verified);
                }
                verifying = since(verify_begin);
                spent += verifying;
                stats.solve_nanoseconds += verifying;
                if (SlowReport().enabled && verifying / 1000000 >= SlowReport().threshold_ms) {
                    static uint64_t verify_sequence = 0;
                    const char *verdict = verified != nullptr ? "verified"
                        : proof_unknown                       ? "verification unknown"
                                                              : "verification refuted";
                    RecordCandidate(
                        SlowReport().directory + "/verify", verify_sequence, f, cand, outcome.get(),
                        verifying / 1000000, verdict
                    );
                }
                if (verified == nullptr) {
                    // Like a rewrite priced out, one the proof turned down read as
                    // "solved" in the trace and then never reached the function.
                    if (DetectDebug()) {
                        llvm::errs() << "cobra: proof " << (proof_unknown ? "unknown" : "refuted")
                                     << *cand.root << "  [cut " << cand.node_limit << "]"
                                     << (outcome->boolean_reading ? " (boolean reading)" : "")
                                     << " after " << verifying / 1000000 << " ms\n";
                    }
                    discard_rewrite();
                    const uint64_t rebuilding = since(rebuild_begin) - verifying;
                    stats.rebuild_nanoseconds += rebuilding;
                    stats.rejected_nanoseconds += rebuilding + verifying;
                    spent += rebuilding;
                    ++stats.rejected_unverified;
                    left_for_later = proof_unknown;
                    account(false);
                    if (!proof_unknown) {
                        record_rejection();
                    }
                    requeue_retries();
                    continue;
                }
                stats.solved_nanoseconds += verifying;
                // The ordinary reading answered in place of the reading of the
                // conditions (`VerifyCandidateOrOrdinary`), so what was built is
                // the other rewrite. The candidate is taken again, and served the
                // proved one from the cache.
                if (verified->boolean_reading != outcome->boolean_reading) {
                    if (DetectDebug()) {
                        llvm::errs() << "cobra: proof of the reading unknown or refuted, ordinary rewrite proved"
                                     << *cand.root << "  [cut " << cand.node_limit << "]\n";
                    }
                    discard_rewrite();
                    stats.solve_nanoseconds += since(rebuild_begin) - verifying;
                    taken_again = true;
                    --index;
                    continue;
                }
                outcome = std::move(verified);
            }

            {
                const uint64_t rebuilding = since(rebuild_begin) - verifying;
                stats.rebuild_nanoseconds += rebuilding;
                stats.solved_nanoseconds += rebuilding;
                spent += rebuilding;
                ++stats.solved;
                account(true);
            }

            if (!reuse.reused.empty()) {
                ++stats.rewrites_reusing;
                stats.reused_instructions += reuse.reused.size();
                // What the rewrite would have emitted building everything
                // itself: every lookup that found something, again.
                if (emitted.size() + reuse.found >= budget) {
                    ++stats.rewrites_fit_by_reuse;
                }
            }

            LogRewrite(cand, *outcome, emitted.size(), budget);
            // A `ptradd` root was read as the addition of its indices, so what
            // was proved is the sum the chain walks by. The pointer it walks
            // from goes back on here, which makes the replacement a
            // `getelementptr` of the same type as the root rather than a cast.
            if (const auto *ptradd = ByteGepOf(cand.root)) {
                llvm::IRBuilder<> rebuild(cand.root);
                new_val = rebuild.CreateGEP(
                    ptradd->getSourceElementType(),
                    ByteGepBase(ptradd), { new_val }
                );
                // The builder folds a constant base walked by a constant
                // index into a constant expression, which carries no
                // metadata; what it says about the root is then already said
                // by the constant itself.
                if (auto *type = cand.root->getMetadata("pointer-type")) {
                    if (auto *rebuilt = llvm::dyn_cast< llvm::Instruction >(new_val)) {
                        rebuilt->setMetadata("pointer-type", type);
                    }
                }
            }
            // Proved on the demanded bits only: the root's readers whose flags
            // read the rest must lose them (see `DropAssumptionsOfReaders`).
            llvm::SmallVector< llvm::Instruction *, 8 > root_readers;
            if (cand.demanded_mask != Bitmask(cand.bitwidth)) {
                for (llvm::User *user : cand.root->users()) {
                    if (auto *reader = llvm::dyn_cast< llvm::Instruction >(user)) {
                        root_readers.push_back(reader);
                    }
                }
            }
            cand.root->replaceAllUsesWith(new_val);
            replaced_roots.insert(cand.root);
            // What the oracle knew described the function before the
            // replacement; the walk below asks about the readers after it.
            demanded_bits.Invalidate();
            if (!root_readers.empty()) {
                stats.reader_flags_dropped += DropAssumptionsOfReaders(root_readers, demanded_bits);
            }
            ++rewrites_this_run;

            // What this rewrite takes out of the function, for the rewrites
            // after it: an instruction of the tree goes once everything that
            // reads it has gone, starting with the root.
            if (options_.cost_model == MbaCostModel::kDyingInstructions) {
                retired.insert(cand.root);
                for (bool growing = true; growing;) {
                    growing = false;
                    for (const auto *ti : cand.tree) {
                        if (retired.contains(ti)) {
                            continue;
                        }
                        const bool unread = llvm::all_of(ti->users(), [&](const llvm::User *u) {
                            const auto *ui = llvm::dyn_cast< llvm::Instruction >(u);
                            return ui != nullptr && retired.contains(ui);
                        });
                        if (unread) {
                            retired.insert(ti);
                            growing = true;
                        }
                    }
                }
            }
            ++NumSimplified;
            if (outcome->accepted_unknown) {
                CountVerification([](VerificationCounters &counters) {
                    ++counters.rewrites_on_unknown;
                });
            }
            changed = true;
            demanded_bits.Invalidate();

            // The replacement is itself an MBA tree the detector will find next
            // run, and re-solving it almost always just reproves that CoBRA's
            // own output cannot shrink further. Fingerprinting it now turns
            // that into a lookup; if a later pass reshapes it the recorded
            // shape stops matching and it is examined again.
            //
            // A replacement that is an instruction the function already held is
            // not CoBRA's output, and nothing has been learnt about it here.
            auto *new_inst = llvm::dyn_cast< llvm::Instruction >(new_val);
            // Nor is a leaf the rewrite came down to: it was never solved.
            //
            // And only a full collection's: the replacement's own collection
            // stops at the leaves the solve read only when those were where the
            // tree really ended. A cut, a boundary cut, a shared-value cut or a
            // collection stopped at `max_tree_nodes` has leaves inside the
            // expression, and the replacement's fingerprint would then describe
            // a larger tree - one nobody examined - and have it skipped.
            const bool whole_tree = cand.node_limit == 0 && !cand.boundary_cut
                && (options_.max_tree_nodes == 0 || cand.tree_size < options_.max_tree_nodes);
            if (whole_tree && new_inst != nullptr && !reuse.seen.contains(new_inst)
                && !llvm::is_contained(cand.leaf_values, new_val))
            {
                if (auto new_fp = ComputeMbaFingerprint(new_inst, options_.max_tree_nodes)) {
                    // The replacement has the root's readers, so its demand is
                    // the root's.
                    RecordMbaFingerprint(
                        new_inst, WithDemandedMask(*new_fp, cand.demanded_mask, cand.bitwidth),
                        options_tag
                    );
                }
            }

            LLVM_DEBUG(
                llvm::dbgs() << "CoBRA: simplified to "
                             << Render(*outcome->expr, real_vars, cand.bitwidth) << "\n"
            );
        }

        // What the rewrites left without readers: the replaced roots, and
        // whatever only they read, transitively. Seeded from the roots rather
        // than swept over the whole function - which removed one level of a
        // dead chain per sweep, took dead code CoBRA had nothing to do with,
        // and judged deadness by `mayHaveSideEffects` alone, which an unread
        // `landingpad` passes - so only CoBRA's leftovers go, each once, by
        // LLVM's own rule for what may be deleted.
        if (changed) {
            // `RecursivelyDeleteTriviallyDeadInstructions` deletes what it is
            // given, readers or not - only an assertion checks - so a replaced
            // root something reads again is not given to it. None should be: a
            // candidate that would read one is stale (above). One that is all
            // the same is a defect to find, and is said; deleting it would leave
            // its reader reading a deleted value.
            llvm::SmallVector< llvm::WeakTrackingVH, 64 > dead;
            for (const llvm::Value *root : replaced_roots) {
                if (root->use_empty()) {
                    dead.emplace_back(const_cast< llvm::Value * >(root));
                    continue;
                }
                llvm::errs() << "CoBRA: a replaced root is read again, and is left in place: ";
                root->print(llvm::errs());
                llvm::errs() << "\n";
            }
            llvm::RecursivelyDeleteTriviallyDeadInstructions(dead);
        }

        if (speculation.Enabled()) {
            speculation.Drain(run_id, stats);
        }

        stats.nanoseconds = since(run_begin);
        MergeStatistics(stats);

        // Instructions are replaced and deleted, never blocks or edges.
        if (!changed) {
            return llvm::PreservedAnalyses::all();
        }
        llvm::PreservedAnalyses preserved;
        preserved.preserveSet< llvm::CFGAnalyses >();
        return preserved;
    }

    const char *NameOf(CandidateConsumer consumer) {
        switch (consumer) {
            case CandidateConsumer::kPointerOffset: return "pointer offset";
            case CandidateConsumer::kStoredValue:   return "stored value";
            case CandidateConsumer::kCompared:      return "compared";
            case CandidateConsumer::kCallArgument:  return "call argument";
            case CandidateConsumer::kReturned:      return "returned";
            case CandidateConsumer::kOther:         return "other";
            case CandidateConsumer::kUnused:        return "unused";
            case CandidateConsumer::kCount:         break;
        }
        return "?";
    }

    const char *NameOf(CandidateOrigin origin) {
        switch (origin) {
            case CandidateOrigin::kDetected:    return "detected in the function";
            case CandidateOrigin::kBoundaryCut: return "re-read at a bitwise boundary";
            case CandidateOrigin::kBooleanCut: return "re-read with narrow comparisons taken in";
            case CandidateOrigin::kSharedCut:   return "re-read over its shared values";
            case CandidateOrigin::kRecut:       return "the same root, cut shorter";
            case CandidateOrigin::kInnerRoot:   return "a root the tree buried";
            case CandidateOrigin::kSlice:       return "re-read at an extension's width";
            case CandidateOrigin::kSumGroup:
                return "one group of its sum, at an extension's width";
            case CandidateOrigin::kCount:       break;
        }
        return "?";
    }

    void EnableStatistics(bool enabled) {
        auto &state = Stats();
        const std::lock_guard< std::mutex > lock(state.mutex);
        state.enabled = enabled;
    }

    bool StatisticsEnabled() {
        auto &state = Stats();
        const std::lock_guard< std::mutex > lock(state.mutex);
        return state.enabled;
    }

    UnknownMemo SaveUnknownMemo() {
        UnknownMemo memo;
        memo.outcomes   = SharedOutcomeCache().Unknowns();
        memo.background = SharedOutcomeCache().BackgroundUnknowns();
        for (auto &[key, result] : verify_detail::Cache().Unknowns()) {
            memo.verify.push_back({ std::move(key), result.timed_out });
        }
        // Sorted, so that two equal memos are equal however the tables are laid out.
        std::sort(memo.outcomes.begin(), memo.outcomes.end());
        std::sort(memo.background.begin(), memo.background.end());
        std::sort(memo.verify.begin(), memo.verify.end());
        return memo;
    }

    void RestoreUnknownMemo(const UnknownMemo &memo) {
        SharedOutcomeCache().RestoreUnknowns(memo.outcomes, memo.background);
        for (const auto &[key, timed_out] : memo.verify) {
            Z3VerifyResult result;
            result.unknown   = true;
            result.timed_out = timed_out;
            verify_detail::Cache().Insert(key, result);
        }
    }

    CobraStatistics Statistics() {
        auto &state = Stats();
        const std::lock_guard< std::mutex > lock(state.mutex);
        return state.totals;
    }

    void ResetStatistics() {
        auto &state = Stats();
        const std::lock_guard< std::mutex > lock(state.mutex);
        state.totals = CobraStatistics{};
    }

    VerificationCounters Verification() {
        auto &state = VerificationLedger();
        const std::lock_guard< std::mutex > lock(state.mutex);
        return state.counters;
    }

    void ResetVerification() {
        auto &state = VerificationLedger();
        const std::lock_guard< std::mutex > lock(state.mutex);
        state.counters = VerificationCounters{};
    }

    void PrintStatistics(llvm::raw_ostream &out) {
        const auto stats = Statistics();
        if (stats.runs == 0) {
            return;
        }
        const auto smt = SmtStrategyStatistics();

        const auto seconds = [](uint64_t nanoseconds) {
            return static_cast< double >(nanoseconds) / 1e9;
        };
        // A count with no time of its own: it is part of the line above it.
        const auto part = [&](const char *what, uint64_t count) {
            out << "  " << llvm::left_justify(what, 36) << llvm::format("%10llu\n", count);
        };
        const auto line = [&](const char *what, uint64_t count, uint64_t nanoseconds) {
            out << "  " << llvm::left_justify(what, 36) << llvm::format("%10llu", count)
                << llvm::format("%12.2f s\n", seconds(nanoseconds));
        };
        const auto cost = [&](const char *what, uint64_t nanoseconds) {
            out << "  " << llvm::left_justify(what, 36) << llvm::right_justify("", 10)
                << llvm::format("%12.2f s\n", seconds(nanoseconds));
        };
        const auto table = [&](const char *heading, const char *first,
                               const CandidateClass *rows, unsigned count,
                               const char *(*name)(unsigned)) {
            out << "  " << heading << ":\n";
            out << "    " << llvm::left_justify(first, 30)
                << llvm::right_justify("candidates", 12) << llvm::right_justify("solved", 9)
                << llvm::right_justify("time", 12) << '\n';
            for (unsigned index = 0; index < count; ++index) {
                if (rows[index].candidates == 0) {
                    continue;
                }
                out << "    " << llvm::left_justify(name(index), 30)
                    << llvm::format("%12llu", rows[index].candidates)
                    << llvm::format("%9llu", rows[index].solved)
                    << llvm::format("%10.2f s\n", seconds(rows[index].nanoseconds));
            }
        };

        const uint64_t looked_at = stats.detected + stats.requeued;
        out << "CoBRA statistics (cumulative):\n";
        line("runs", stats.runs, stats.nanoseconds);
        line("candidates looked at", looked_at,
             stats.solve_nanoseconds + stats.rebuild_nanoseconds);
        part("  detected in the function", stats.detected);
        part("  re-cut and descended into", stats.requeued);
        part("  roots already rewritten", stats.dead_roots);
        line("candidates solved", stats.solved, stats.solved_nanoseconds);
        part("  reading what the function held", stats.rewrites_reusing);
        part("    instructions read, not rebuilt", stats.reused_instructions);
        part("    over the budget had they been built", stats.rewrites_fit_by_reuse);
        line("candidates rejected",
             stats.rejected_unsolved + stats.rejected_cost + stats.rejected_budget
                 + stats.rejected_unverified + stats.rejected_cached,
             stats.rejected_nanoseconds);
        part("  no simplification found", stats.rejected_unsolved);
        part("  simplification not cheaper", stats.rejected_cost);
        part("  rebuilt IR over budget", stats.rejected_budget);
        part("  fits the budget, not verified", stats.rejected_unverified);
        part("  rejected by an earlier run", stats.rejected_cached);
        line("served from the outcome cache", stats.cache_hits, stats.cache_nanoseconds);
        cost("finding the candidates", stats.detect_nanoseconds);
        cost("  re-cutting and descending", stats.ladder_nanoseconds);
        cost("rebuilding the IR", stats.rebuild_nanoseconds);

        out << "  where the time goes (solved, not cached):\n";
        line("  searching for a rewrite", stats.search_found, stats.search_nanoseconds);
        cost("    of which the searches that found one", stats.search_found_nanoseconds);
        if (stats.leaf_identities != 0) {
            part("    a tree equal to one of its inputs (sweep)", stats.leaf_identities);
        }
        if (stats.boolean_trees != 0) {
            part("    trees read with their conditions as variables", stats.boolean_trees);
            part("      conditions read", stats.boolean_conditions);
            part("      pairs proved one condition", stats.boolean_merged);
            part("      pairs the solver could not decide", stats.boolean_undecided);
            part("      rewrites found", stats.boolean_found);
        }
        if (stats.free_leaf_trees != 0) {
            part("    trees read again with their masked leaves free", stats.free_leaf_trees);
            part("      rewrites found", stats.free_leaf_found);
        }
        if (stats.keyed_leaf_trees != 0) {
            part("    trees read again over the keyed values of their leaves", stats.keyed_leaf_trees);
            part("      rewrites found", stats.keyed_leaf_found);
        }
        cost("  proving by running every input", stats.enumeration_nanoseconds);
        line("  proving in a ring normal form (attempts)", stats.ring_attempted, stats.ring_nanoseconds);
        part("    proved without a solver", stats.ring_proved);
        part("  readers' poison flags dropped after a partial-bits rewrite", stats.reader_flags_dropped);
        part("  queued candidates whose root's readers grew", stats.demanded_grew);
        part("  queued candidates on an unreachable root", stats.unreachable_roots);
        part("  queued candidates reading a leaf the root does not see", stats.unavailable_leaves);
        part("  queued candidates over a root replaced this run", stats.stale_trees);
        part("  candidates without an expression to prove", stats.declined_no_expression);
        part("  worker tasks withdrawn for candidates passed by", stats.background_withdrawn_unneeded);
        part("  timeouts not recorded: the thread lacked the processor", stats.unknown_starved);
        {
            const auto phis = PhiArmStatistics();
            if (phis.checked != 0) {
                part("  transparent phi sets checked", phis.checked);
                part("    other arms the same computation", phis.exact_accept);
                part("    read as leaves: a leaf of the arm unavailable at the root", phis.leaf_unavailable);
            }
        }
        line("  proving with the solver", stats.solver_calls, stats.solver_nanoseconds);
        cost("    proved", stats.solver_proved_nanoseconds);
        cost("    refuted", stats.solver_refuted_nanoseconds);
        cost("    unknown", stats.solver_unknown_nanoseconds);
        part("    not re-asked, already unknown", stats.solver_unknown_skipped);
        part("  declined, stands on a loop-carried value", stats.declined_loop_carried);
        line("  found, then over the IR budget", stats.proved_over_budget,
             stats.proved_over_budget_nanoseconds);
        part("  cached rewrite over the IR budget", stats.cached_over_budget);
        line("  re-solved after a cache eviction", stats.resolved_after_eviction,
             stats.resolved_after_eviction_nanoseconds);
        line("  demanded bits (builds, queries)", stats.demanded_builds,
             stats.demanded_nanoseconds);
        part("    instructions settled from readers", stats.demanded_local_settled);
        part("    of which by a fixpoint over a cycle", stats.demanded_local_nodes);
        part("    cycles settled that way", stats.demanded_local_regions);
        part("    cycles too large, whole function", stats.demanded_local_declined);
        if (stats.demanded_check_compared != 0) {
            part("    checked against the whole function", stats.demanded_check_compared);
            part("      disagreements", stats.demanded_check_mismatches);
        }
        if (stats.background_submitted != 0) {
            out << "  solved ahead of the loop, on worker threads:\n";
            part("  candidates handed to the workers", stats.background_submitted);
            line("  solved by a worker (worker time)", stats.background_solved,
                 stats.background_nanoseconds);
            part("    left unproved, plainly over budget", stats.background_proofs_skipped);
            line("    proof ran out of budget, left to the loop", stats.background_unknown,
                 stats.background_unknown_nanoseconds);
            line("      the loop proved it again", stats.after_background_unknown_proved
                     + stats.after_background_unknown_refuted + stats.after_background_unknown_unknown,
                 stats.after_background_unknown_nanoseconds);
            part("        proved", stats.after_background_unknown_proved);
            part("        refuted", stats.after_background_unknown_refuted);
            part("        unknown again", stats.after_background_unknown_unknown);
            line("  answers the loop waited for", stats.background_settled,
                 stats.background_wait_nanoseconds);
            part("  withdrawn, solved by the loop", stats.background_withdrawn);
            cost("  waiting for workers at run end", stats.background_drain_nanoseconds);
        }
        if (smt.compiled + smt.folded + smt.fallbacks != 0) {
            out << "  proofs compiled through LLVM (smt-strategy):\n";
            part("  answered by the optimizer alone", smt.folded);
            part("    confirmed by the solver as built", smt.folded_confirmed);
            part("    left to the optimizer (solver out of time)", smt.folded_unconfirmed);
            part("  optimizer's 'equal' overturned by an independent check", smt.llvm_refuted);
            part("  optimized, then solved", smt.compiled);
            part("  not translatable, asked as built", smt.fallbacks);
            part("  race won by the compiled question", smt.race_compiled_won);
            part("  race won by the question as built", smt.race_direct_won);
            part("  race, neither answered", smt.race_both_unknown);
            if (smt.swept != 0) {
                part("  swept after the head start (smt-sweep)", smt.swept);
                part("    lemmas asked", smt.sweep_lemmas);
                part("    lemmas proved", smt.sweep_proved);
                part("      of which with shared subterms cut", smt.sweep_generalized);
                part("    candidates synthesized", smt.sweep_synthesized);
            }
        }
        cost("  ladder: boundary cuts", stats.boundary_nanoseconds);
        cost("  ladder: shared-value cuts", stats.shared_nanoseconds);
        cost("  ladder: re-cuts", stats.recut_nanoseconds);
        cost("  ladder: inner roots", stats.inner_nanoseconds);
        cost("  ladder: slices", stats.slice_nanoseconds);
        if (stats.slice_asked != 0) {
            out << "    slice claims asked " << stats.slice_asked << ", proved "
                << stats.slice_proved << ", refuted " << stats.slice_refuted << ", unknown "
                << stats.slice_unknown
                << " (a fitting rewrite dropped for it: " << stats.slice_claim_failed << ")\n";
        }
        cost("  ladder: sum groups", stats.sum_group_nanoseconds);
        if (stats.sum_group_roots != 0) {
            out << "    sums split " << stats.sum_group_split << " of " << stats.sum_group_roots
                << " asked, into " << stats.sum_group_groups
                << " group(s) read at an extension's width\n";
        }

        table("by where the candidate came from", "origin", stats.by_origin,
              static_cast< unsigned >(CandidateOrigin::kCount), [](unsigned index) {
                  return NameOf(static_cast< CandidateOrigin >(index));
              });

        uint64_t classified = 0;
        for (const auto &row : stats.by_consumer) {
            classified += row.candidates;
        }
        if (classified != 0) {
            table("by what reads the candidate", "consumer", stats.by_consumer,
                  static_cast< unsigned >(CandidateConsumer::kCount), [](unsigned index) {
                      return NameOf(static_cast< CandidateConsumer >(index));
                  });
            out << "    " << llvm::left_justify("reaching an address at all", 30)
                << llvm::format("%12llu", stats.reaching_pointer_offset)
                << llvm::format("%9llu", stats.solved_reaching_pointer_offset)
                << llvm::format("%10.2f s\n", seconds(stats.pointer_offset_nanoseconds));
        }

        out << "  by whether the tree stands on a loop-carried value:\n";
        for (unsigned index = 0; index < 2; ++index) {
            const auto &row = stats.by_loop_phi[index];
            if (row.candidates == 0) {
                continue;
            }
            out << "    "
                << llvm::left_justify(index == 1 ? "touches a loop-carried phi" : "does not", 30)
                << llvm::format("%12llu", row.candidates) << llvm::format("%9llu", row.solved)
                << llvm::format("%10.2f s\n", seconds(row.nanoseconds));
        }

        for (unsigned phi_row = 0; phi_row < 2; ++phi_row) {
            bool any = false;
            for (unsigned index = 0; index < static_cast< unsigned >(CandidateConsumer::kCount); ++index) {
                any |= stats.by_loop_phi_consumer[phi_row][index].candidates != 0;
            }
            if (!any) {
                continue;
            }
            out << (phi_row == 1 ? "    of those touching a loop-carried phi, by consumer:\n"
                                 : "    of those that do not, by consumer:\n");
            for (unsigned index = 0; index < static_cast< unsigned >(CandidateConsumer::kCount); ++index) {
                const auto &row = stats.by_loop_phi_consumer[phi_row][index];
                if (row.candidates == 0) {
                    continue;
                }
                out << "      "
                    << llvm::left_justify(NameOf(static_cast< CandidateConsumer >(index)), 28)
                    << llvm::format("%12llu", row.candidates) << llvm::format("%9llu", row.solved)
                    << llvm::format("%10.2f s\n", seconds(row.nanoseconds));
            }
            const auto &leaves = stats.by_loop_phi_constant_leaves[phi_row];
            if (leaves.candidates != 0) {
                out << "      " << llvm::left_justify("(every leaf a constant)", 28)
                    << llvm::format("%12llu", leaves.candidates) << llvm::format("%9llu", leaves.solved)
                    << llvm::format("%10.2f s\n", seconds(leaves.nanoseconds));
            }
        }

        out << "  by independent leaves:\n";
        for (unsigned index = 0; index <= CobraStatistics::kMaxVariableRow; ++index) {
            const auto &row = stats.by_variables[index];
            if (row.candidates == 0) {
                continue;
            }
            out << "    " << llvm::left_justify(std::to_string(index), 30)
                << llvm::format("%12llu", row.candidates) << llvm::format("%9llu", row.solved)
                << llvm::format("%10.2f s\n", seconds(row.nanoseconds));
        }
    }

} // namespace cobra

