#include "cobra/llvm/CobraPass.h"
#include "IRReconstructor.h"
#include "LocalDemandedBits.h"
#include "SmtStrategy.h"
#include "MBADetector.h"
#include "cobra/llvm/CobraStats.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/CompiledExpr.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/Simplifier.h"

#ifdef COBRA_HAS_Z3
    #include "cobra/verify/Z3Verifier.h"
#endif

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/DemandedBits.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Analysis.h"
#include "llvm/IR/Dominators.h"
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
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
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
            // position in another tree - an interpreter's handlers read the same
            // register and frame slots in different roles - and binding a rewrite by name
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
        // `COBRA_DEMANDED=global` answers everything from the whole-function
        // analysis, as before; `COBRA_DEMANDED_CHECK=1` answers every question
        // both ways and counts the ones where they disagree.
        class DemandedBitsOracle
        {
          public:
            DemandedBitsOracle(llvm::Function &f, CobraStatistics &stats)
                : f_(f), stats_(stats), assumptions_(f), dominators_(f),
                  local_(assumptions_, dominators_, kMaxLocalRegion),
                  function_size_(f.getInstructionCount()) {}

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

          private:
            enum class Mode : uint8_t { kLocal, kGlobal, kCheck };

            static Mode Configured() {
                static const Mode mode = [] {
                    if (const char *check = std::getenv("COBRA_DEMANDED_CHECK");
                        check != nullptr && *check != '\0' && *check != '0')
                    {
                        return Mode::kCheck;
                    }
                    const char *choice = std::getenv("COBRA_DEMANDED");
                    return choice != nullptr && std::string_view(choice) == "global"
                        ? Mode::kGlobal
                        : Mode::kLocal;
                }();
                return mode;
            }

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
                const Mode mode = Configured();
                const bool local_is_cheaper =
                    !analysis_.has_value() && local_.work_this_epoch < function_size_;
                if (mode == Mode::kCheck || (mode == Mode::kLocal && local_is_cheaper)) {
                    if (auto local = local_.Get(inst)) {
                        if (mode == Mode::kCheck) {
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
                llvm::errs() << "COBRA_DEMANDED_CHECK: " << f_.getName() << ": local 0x"
                             << llvm::utohexstr(local.getZExtValue()) << ", global 0x"
                             << llvm::utohexstr(global.getZExtValue()) << " for" << *inst
                             << "\n";
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
            llvm::AssumptionCache assumptions_;
            llvm::DominatorTree dominators_;
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
            into.enumeration_nanoseconds += run.enumeration_nanoseconds;
            into.solver_calls += run.solver_calls;
            into.solver_nanoseconds += run.solver_nanoseconds;
            into.solver_proved_nanoseconds += run.solver_proved_nanoseconds;
            into.solver_refuted_nanoseconds += run.solver_refuted_nanoseconds;
            into.solver_unknown_nanoseconds += run.solver_unknown_nanoseconds;
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
            into.background_submitted += run.background_submitted;
            into.background_solved += run.background_solved;
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
        }

        // ---- where the time goes ------------------------------------------
        //
        // A single `CobraPass::run` on an obfuscated partial CFG has been
        // measured at over two minutes, and per-pass timing outside CoBRA can
        // only say that much: it cannot name the expression the time went into,
        // and without the expression there is nothing to fix.
        //
        // With `COBRA_SLOW_REPORT` set to a directory, every candidate whose
        // solve costs at least `COBRA_SLOW_MS` milliseconds (default 100) is
        // recorded there - what went in, what came out, what it cost, and the
        // shape of the tree - and the IR it was read from is written beside it
        // as a module holding one function, so the case can be re-run on its
        // own instead of by reproducing the exploration that found it.
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
                std::error_code code;
                llvm::sys::fs::create_directories(out.directory, true);
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
            const auto &names = outcome.real_vars.empty() ? cand.var_names : outcome.real_vars;
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
        std::string CandidateKey(const MBACandidate &cand, const CobraPassOptions &options) {
            std::string key;
            key += std::to_string(cand.bitwidth);
            key += '|';
            key += std::to_string(cand.demanded_mask);
            key += '|';
            key += std::to_string(options.max_vars);
            key += options.z3_verify ? "|z" : "|-";
            key += std::to_string(options.z3_settings.timeout_ms);
            key += std::to_string(static_cast< int >(options.z3_settings.unknown_result_mode));
            key += ':';
            key += std::to_string(options.z3_settings.rewrite_level);
            key += std::to_string(static_cast< int >(options.z3_settings.strategy));
            key += options.z3_settings.sweep ? "s" : "";
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
                if (auto it = young_.find(key); it != young_.end()) {
                    return it->second;
                }
                if (auto it = old_.find(key); it != old_.end()) {
                    return it->second;
                }
                return std::nullopt;
            }

            Entry Insert(const std::string &key, Entry entry) {
                const std::lock_guard< std::mutex > lock(mutex_);
                if (young_.size() >= Generation()) {
                    // `COBRA_OUTCOME_CACHE=legacy`: one generation of 8192,
                    // cleared when it fills, as the cache used to be.
                    old_ = Legacy() ? decltype(old_){} : std::move(young_);
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

          private:
            // Bound the footprint on very large modules. The cache is two
            // generations: when the young one fills up it becomes the old one
            // and the previous old one is dropped, so an entry survives at
            // least one whole generation after it was written. Clearing
            // everything at once, as the cache used to, dropped the entries
            // the pass was about to ask for together with the stale ones - over
            // one long exploration a third of every solve was a candidate solved
            // before and forgotten. An entry is a key of a few hundred bytes
            // and a small expression, so this is tens of megabytes at most.
            static constexpr size_t kGeneration = 32768;
            static constexpr size_t kMaxTracked = size_t{ 1 } << 20;

            static bool Legacy() {
                static const bool legacy = [] {
                    const char *choice = std::getenv("COBRA_OUTCOME_CACHE");
                    return choice != nullptr && std::string_view(choice) == "legacy";
                }();
                return legacy;
            }

            static size_t Generation() { return Legacy() ? 8192 : kGeneration; }

            mutable std::mutex mutex_;
            std::unordered_map< std::string, Entry > young_;
            std::unordered_map< std::string, Entry > old_;
            std::unordered_set< size_t > ever_inserted_;
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
        // interpreter's conditional jump tests. Sixty-five thousand evaluations cost less
        // than the solver's start-up and leave nothing to believe.
        //
        // Empty when the inputs are too many to walk.
        std::optional< bool > DecideByEnumeration(
            const Expr &lhs, const Expr &rhs, uint32_t num_vars, uint32_t bitwidth
        ) {
            constexpr uint32_t kMaxInputBits = 20;

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

        // Search for a rewrite of one candidate and put it through the cost
        // gate. Returns null when either rejects it, and otherwise a rewrite that
        // still has to be verified (`VerifyCandidate`).
        OutcomeCache::Entry ProposeRewrite(
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
            if (!options.z3_verify && ast != nullptr && ContainsComparison(*ast)) {
                ++NumSkippedUnsupported;
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
            const bool searched = result.has_value() && result.value().kind == SimplifyOutcome::Kind::kSimplified;
            if (!searched && options.z3_verify && options.z3_settings.sweep && ast != nullptr) {
                const auto count = static_cast< uint32_t >(cand.var_names.size());
                for (uint32_t index = 0; index < count; ++index) {
                    auto leaf = Expr::Variable(index);
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
                    verdict = SolveVerdict::kNotCheaper;
                    ++NumSkippedCost;
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
                proved = decided.has_value();
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
                const auto solver_begin = std::chrono::steady_clock::now();
                auto z3_result          = VerifyExprsWithStrategy(
                    *proved_ast, *z3_expr, cand.var_names, cand.bitwidth, options.z3_settings
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
                // as a yes, it folded the `icmp ult` behind an interpreter's
                // conditional jump to true and the jump's displacement to a constant.
                const bool declined_comparison =
                    z3_result.equivalent && z3_result.unknown && ContainsComparison(*ast);
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
                .verified           = true });
        }

        // `COBRA_REUSE=0`: build every rewrite from its leaves, as the pass used
        // to, reading nothing the function already holds (see `IrReuse`).
        bool ReuseInstructions() {
            static const bool reuse = [] {
                const char *choice = std::getenv("COBRA_REUSE");
                return choice == nullptr || *choice == '\0' || *choice != '0';
            }();
            return reuse;
        }

        // `COBRA_SHARED_CUTS=<n>`: how many rungs the shared-value ladder may
        // climb, in place of `CobraPassOptions::max_shared_cuts`; zero turns the
        // ladder off.
        uint32_t SharedCuts(const CobraPassOptions &options) {
            static const int64_t chosen = [] {
                const char *choice = std::getenv("COBRA_SHARED_CUTS");
                return choice == nullptr || *choice == '\0'
                    ? int64_t{ -1 }
                    : static_cast< int64_t >(std::strtoul(choice, nullptr, 10));
            }();
            return chosen < 0 ? options.max_shared_cuts : static_cast< uint32_t >(chosen);
        }

        // `COBRA_VERIFY_BEFORE_BUDGET=1`: prove every rewrite as soon as it is
        // found, as the pass used to, whether or not it fits the budget.
        bool VerifyBeforeBudget() {
            static const bool verify_first = [] {
                const char *choice = std::getenv("COBRA_VERIFY_BEFORE_BUDGET");
                return choice != nullptr && *choice != '\0' && *choice != '0';
            }();
            return verify_first;
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
        // would have been; `COBRA_BACKGROUND_UNKNOWN=retry` leaves such a
        // rewrite unproved instead, and the loop tries the proof itself.
        //
        // `COBRA_THREADS` sets the number of workers; 0 solves everything in
        // the loop, as the pass used to.
        class Speculation
        {
          public:
            static Speculation &Instance() {
                // Never destroyed: see `Drain`.
                static auto *instance = new Speculation(ThreadCount());
                return *instance;
            }

            bool Enabled() const { return !workers_.empty(); }

            // False when `key` is being solved already.
            bool Submit(
                const std::string &key, const MBACandidate &cand, const CobraPassOptions &options,
                uint32_t budget
            ) {
                auto task      = std::make_shared< Task >();
                task->key      = key;
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

            // Withdraws everything not started and waits for what is running,
            // so that no work outlives the run that queued it and no worker is
            // ever busy when the process exits.
            void Drain(CobraStatistics &stats) {
                std::unique_lock< std::mutex > lock(mutex_);
                for (auto &task : queue_) {
                    if (task->state == Task::kPending) {
                        task->state = Task::kWithdrawn;
                    }
                }
                queue_.clear();
                by_key_.clear();
                const auto begin = std::chrono::steady_clock::now();
                done_.wait(lock, [&] { return running_ == 0; });
                stats.background_drain_nanoseconds += Since(begin);
            }

          private:
            struct Task
            {
                enum State : uint8_t { kPending, kRunning, kDone, kWithdrawn };

                std::string key;
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

            static bool RetryUnknown() {
                static const bool retry = [] {
                    const char *choice = std::getenv("COBRA_BACKGROUND_UNKNOWN");
                    return choice != nullptr && std::string_view(choice) == "retry";
                }();
                return retry;
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
                    }
                    done_.notify_all();
                }
            }

            static void Solve(Task &task) {
                const auto begin = std::chrono::steady_clock::now();
                CobraStatistics stats;
                auto verdict  = SolveVerdict::kUnsolved;
                auto proposal = ProposeRewrite(task.input, task.options, verdict, stats);
                OutcomeCache::Entry entry = proposal;
                if (proposal != nullptr) {
                    bool reads_variable = false;
                    if (VerifyBeforeBudget()
                        || MinimumInstructions(
                               *proposal->expr, proposal->real_var_positions,
                               task.leaf_needs_cast, reads_variable
                           ) < task.budget)
                    {
                        bool unknown  = false;
                        auto verified = VerifyCandidate(
                            task.input, *proposal, task.options, stats, &unknown
                        );
                        if (verified != nullptr) {
                            entry = std::move(verified);
                        } else if (!(unknown && RetryUnknown())) {
                            entry = nullptr;
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
        };

    } // namespace

    llvm::PreservedAnalyses
    CobraPass::run(llvm::Function &f, llvm::FunctionAnalysisManager & /*AM*/) {
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

        DemandedBitsOracle demanded_bits(f, stats);
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

        NumCandidates += candidates.size();

        auto &cache = SharedOutcomeCache();

        // Every queued candidate's key, parallel to `candidates`, and the
        // candidates handed to the workers as they are queued (`Speculation`).
        auto &speculation = Speculation::Instance();
        std::vector< std::string > keys;
        const auto queue_key = [&](const MBACandidate &queued) {
            keys.push_back(CandidateKey(queued, options_));
            if (speculation.Enabled() && !cache.Find(keys.back()).has_value()) {
                const uint32_t budget = options_.cost_model == MbaCostModel::kTreeInstructions
                    ? queued.tree_size
                    : queued.dying_count;
                if (speculation.Submit(keys.back(), queued, options_, budget)) {
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
        std::optional< llvm::DominatorTree > dominators;
        llvm::DenseSet< const llvm::Instruction * > retired;

        for (size_t index = 0; index < candidates.size(); ++index) {
            auto &cand = candidates[index];

            // A root an earlier rewrite replaced has no readers left. Its
            // re-cuts and inner roots were queued before that happened, and
            // solving them now buys nothing.
            if (cand.root->use_empty()) {
                ++stats.dead_roots;
                continue;
            }

            // Only a full collection speaks for the tree rooted at an
            // instruction; a re-cut saw part of it, so recording its verdict
            // would tell the next run the whole tree was examined.
            const auto record_rejection = [&] {
                if (cand.node_limit == 0) {
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
                // The boundary cut first: it is the one reading of the root
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
                    cand, options_.min_ast_size, options_.max_vars, SharedCuts(options_),
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
                for (auto &cut : boundary) {
                    if (attempted.insert({ cut.root, cut.node_limit }).second) {
                        queue_key(cut);
                        candidates.push_back(std::move(cut));
                        origins.push_back(CandidateOrigin::kBoundaryCut);
                        ++stats.requeued;
                    }
                }
                for (auto &cut : shared) {
                    if (attempted.insert({ cut.root, cut.node_limit }).second) {
                        queue_key(cut);
                        candidates.push_back(std::move(cut));
                        origins.push_back(CandidateOrigin::kSharedCut);
                        ++stats.requeued;
                    }
                }
                for (auto &recut : recuts) {
                    if (attempted.insert({ recut.root, recut.node_limit }).second) {
                        queue_key(recut);
                        candidates.push_back(std::move(recut));
                        origins.push_back(CandidateOrigin::kRecut);
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
            const uint32_t consumers = classify ? ConsumerMask(cand.root) : 0U;
            const auto consumer      = PrimaryConsumer(consumers);
            const bool addresses =
                (consumers & ConsumerBit(CandidateConsumer::kPointerOffset)) != 0;

            // What this candidate cost, filled in once the solve is done, and
            // added to every row it belongs to when its fate is known.
            uint64_t spent = 0;
            const auto account = [&](bool solved) {
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
                if (VerifyBeforeBudget() && proposal != nullptr) {
                    proposal = VerifyCandidate(cand, *proposal, options_, stats);
                    if (proposal == nullptr) {
                        verdict = SolveVerdict::kUnsolved;
                    }
                }
                outcome = cache.Insert(key, std::move(proposal));
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
            std::vector< llvm::Value * > frozen_leaves;
            if (cand.spans_freeze) {
                frozen_leaves = cand.leaf_values;
                for (auto *&leaf : frozen_leaves) {
                    if (!llvm::isGuaranteedNotToBePoison(leaf)) {
                        leaf = builder.CreateFreeze(leaf, "cobra.freeze");
                    }
                }
            }

            // What the function already computes is read, not built again. A
            // flagged instruction of the tree can be read where the root was
            // poison whenever it was; the frozen leaves of a tree that spans a
            // `freeze` are new values nothing reads yet, so there the question
            // does not come up.
            IrReuse reuse;
            llvm::SmallPtrSet< const llvm::Instruction *, 32 > poisons_root;
            if (ReuseInstructions()) {
                if (!dominators.has_value()) {
                    dominators.emplace(f);
                }
                reuse.before     = cand.root;
                reuse.dominators = &*dominators;
                if (cand.poison_reaches_root) {
                    poisons_root.insert(cand.tree.begin(), cand.tree.end());
                    reuse.poisons_root = &poisons_root;
                }
            }

            auto *new_val = ReconstructIr(
                *outcome->expr, cand, builder, var_map, frozen_leaves,
                ReuseInstructions() ? &reuse : nullptr
            );

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
                discard_rewrite();
                ++NumSkippedCost;
                LLVM_DEBUG(
                    llvm::dbgs() << "CoBRA: skipping — rewrite emits " << emitted.size()
                                 << " instructions and keeps " << charged
                                 << " against a budget of " << budget << "\n"
                );
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
                account(false);
                record_rejection();
                requeue_retries();
                continue;
            }

            // The rewrite fits, so it is worth proving now - once per key: the
            // verified rewrite replaces the proposal in the cache, and a failed
            // proof replaces it with the rejection it would always have been.
            uint64_t verifying = 0;
            if (!outcome->verified) {
                const auto verify_begin  = std::chrono::steady_clock::now();
                const uint64_t unknown_before = stats.solver_unknown_nanoseconds;
                auto verified            = VerifyCandidate(cand, *outcome, options_, stats);
                cache.Insert(key, verified);
                verifying = since(verify_begin);
                spent += verifying;
                stats.solve_nanoseconds += verifying;
                if (SlowReport().enabled && verifying / 1000000 >= SlowReport().threshold_ms) {
                    static uint64_t verify_sequence = 0;
                    const char *verdict = verified != nullptr ? "verified"
                        : stats.solver_unknown_nanoseconds != unknown_before
                        ? "verification unknown"
                        : "verification refuted";
                    RecordCandidate(
                        SlowReport().directory + "/verify", verify_sequence, f, cand, outcome.get(),
                        verifying / 1000000, verdict
                    );
                }
                if (verified == nullptr) {
                    discard_rewrite();
                    const uint64_t rebuilding = since(rebuild_begin) - verifying;
                    stats.rebuild_nanoseconds += rebuilding;
                    stats.rejected_nanoseconds += rebuilding + verifying;
                    spent += rebuilding;
                    ++stats.rejected_unverified;
                    account(false);
                    record_rejection();
                    requeue_retries();
                    continue;
                }
                stats.solved_nanoseconds += verifying;
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
            cand.root->replaceAllUsesWith(new_val);

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
            if (new_inst != nullptr && !reuse.seen.contains(new_inst)) {
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

        if (speculation.Enabled()) {
            speculation.Drain(stats);
        }

        stats.nanoseconds = since(run_begin);
        MergeStatistics(stats);

        return changed ? llvm::PreservedAnalyses::none() : llvm::PreservedAnalyses::all();
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
            case CandidateOrigin::kSharedCut:   return "re-read over its shared values";
            case CandidateOrigin::kRecut:       return "the same root, cut shorter";
            case CandidateOrigin::kInnerRoot:   return "a root the tree buried";
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
        cost("  proving by running every input", stats.enumeration_nanoseconds);
        line("  proving with the solver", stats.solver_calls, stats.solver_nanoseconds);
        cost("    proved", stats.solver_proved_nanoseconds);
        cost("    refuted", stats.solver_refuted_nanoseconds);
        cost("    unknown", stats.solver_unknown_nanoseconds);
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
            line("  answers the loop waited for", stats.background_settled,
                 stats.background_wait_nanoseconds);
            part("  withdrawn, solved by the loop", stats.background_withdrawn);
            cost("  waiting for workers at run end", stats.background_drain_nanoseconds);
        }
        if (smt.compiled + smt.folded + smt.fallbacks != 0) {
            out << "  proofs compiled through LLVM (smt-strategy):\n";
            part("  answered by the optimizer alone", smt.folded);
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

