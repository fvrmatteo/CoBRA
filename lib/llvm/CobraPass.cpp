#include "cobra/llvm/CobraPass.h"
#include "IRReconstructor.h"
#include "MBADetector.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/Simplifier.h"

#ifdef COBRA_HAS_Z3
    #include "cobra/verify/Z3Verifier.h"
#endif

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/DemandedBits.h"
#include "llvm/IR/Analysis.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/Debug.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
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

        // Which bits of a value its readers look at, from LLVM's demanded-bits
        // analysis. A rewrite only has to agree with the tree on those, and
        // for a shift over a sum that is the difference between a semilinear
        // reading and none. The analysis covers the whole function and goes
        // stale the moment a rewrite lands - the new tree reads its leaves
        // through different operators than the old one did - so it is dropped
        // after every one and rebuilt on the next question.
        class DemandedBitsOracle
        {
          public:
            explicit DemandedBitsOracle(llvm::Function &f)
                : f_(f), assumptions_(f), dominators_(f) {}

            uint64_t MaskOf(llvm::Instruction *inst) {
                if (inst == nullptr || !inst->getType()->isIntegerTy()
                    || inst->getType()->getIntegerBitWidth() > 64)
                {
                    return UINT64_MAX;
                }
                if (!analysis_.has_value()) {
                    analysis_.emplace(f_, assumptions_, dominators_);
                }
                const llvm::APInt bits = analysis_->getDemandedBits(inst);
                return bits.getZExtValue() & Bitmask(inst->getType()->getIntegerBitWidth());
            }

            void Invalidate() { analysis_.reset(); }

          private:
            llvm::Function &f_;
            llvm::AssumptionCache assumptions_;
            llvm::DominatorTree dominators_;
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
        };

        const SlowCandidateReport &SlowReport() {
            static const SlowCandidateReport report = [] {
                SlowCandidateReport out;
                const char *directory = std::getenv("COBRA_SLOW_REPORT");
                if (directory == nullptr || *directory == '\0') {
                    return out;
                }
                out.enabled   = true;
                out.directory = directory;
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

        void RecordSlowCandidate(
            const llvm::Function &f, const MBACandidate &cand, const CandidateOutcome *outcome,
            uint64_t milliseconds, const char *verdict
        ) {
            const auto &report = SlowReport();
            const std::lock_guard< std::mutex > lock(SlowReportMutex());
            static uint64_t sequence = 0;
            const uint64_t index     = ++sequence;

            std::string note;
            const std::string module_path = WriteCandidateModule(
                cand, report.directory + "/cobra-slow-" + std::to_string(index) + ".ll", note
            );

            std::error_code code;
            llvm::raw_fd_ostream log(
                report.directory + "/cobra-slow.log", code, llvm::sys::fs::CD_OpenAlways,
                llvm::sys::fs::FA_Write, llvm::sys::fs::OF_Append | llvm::sys::fs::OF_Text
            );
            if (code) {
                return;
            }
            log << "## slow candidate " << index << "\n";
            log << "milliseconds   " << milliseconds << "\n";
            log << "verdict        " << verdict << "\n";
            log << "function       " << f.getName() << "\n";
            log << "bitwidth       " << cand.bitwidth << "\n";
            log << "tree-size      " << cand.tree_size << "\n";
            log << "dying-count    " << cand.dying_count << "\n";
            log << "node-limit     " << cand.node_limit << "\n";
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
            tag          = mix(tag, options.max_recut_nodes);
            tag          = mix(tag, options.max_recut_vars);
            tag          = mix(tag, options.max_tree_nodes);
            tag          = mix(tag, options.z3_verify ? 1 : 0);
            tag          = mix(tag, options.z3_settings.timeout_ms);
            tag = mix(tag, static_cast< uint64_t >(options.z3_settings.unknown_result_mode));
            tag = mix(tag, static_cast< uint64_t >(options.enabled_families));
            tag = mix(tag, static_cast< uint64_t >(options.cost_model));
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

                // A rewrite found for the demanded bits is proved on those:
                // both sides are masked to them.
                auto proved_ast = CloneExpr(*ast);
                if (cand.demanded_mask != Bitmask(cand.bitwidth)) {
                    proved_ast =
                        Expr::BitwiseAnd(std::move(proved_ast), Expr::Constant(cand.demanded_mask));
                    z3_expr =
                        Expr::BitwiseAnd(std::move(z3_expr), Expr::Constant(cand.demanded_mask));
                }
                auto z3_result = Z3VerifyExprs(
                    *proved_ast, *z3_expr, cand.var_names, cand.bitwidth, options.z3_settings
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

        DemandedBitsOracle demanded_bits(f);
        const DemandedMaskFn demanded = [&](llvm::Instruction *inst) {
            return demanded_bits.MaskOf(inst);
        };

        auto candidates = DetectMbaCandidates(
            f, options_.min_ast_size, options_.max_vars, options_tag, options_.cost_model,
            options_.max_tree_nodes, demanded
        );

        NumCandidates += candidates.size();

        auto &cache = SharedOutcomeCache();

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

        for (size_t index = 0; index < candidates.size(); ++index) {
            auto &cand = candidates[index];

            // A root an earlier rewrite replaced has no readers left. Its
            // re-cuts and inner roots were queued before that happened, and
            // solving them now buys nothing.
            if (cand.root->use_empty()) {
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
            // Both lists are built before either is queued: pushing can
            // reallocate `candidates`, and `cand` is a reference into it.
            const auto requeue_retries = [&] {
                // The boundary cut first: it is the one reading of the root
                // the ladder below cannot reach, and it costs a single solve.
                auto boundary = BoundaryCutMbaCandidate(
                    cand, options_.min_ast_size, options_.max_vars, options_tag,
                    options_.cost_model, options_.max_tree_nodes, demanded
                );
                auto recuts = RecutMbaCandidate(
                    cand, options_.min_ast_size, options_.max_vars, options_.max_recut_nodes,
                    options_.max_recut_vars, options_tag, options_.cost_model, demanded
                );

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
                for (auto &cut : boundary) {
                    if (attempted.insert({ cut.root, cut.node_limit }).second) {
                        candidates.push_back(std::move(cut));
                    }
                }
                for (auto &recut : recuts) {
                    if (attempted.insert({ recut.root, recut.node_limit }).second) {
                        candidates.push_back(std::move(recut));
                    }
                }
                for (auto &inner : inners) {
                    if (attempted.insert({ inner.root, inner.node_limit }).second) {
                        candidates.push_back(std::move(inner));
                    }
                }
            };

            const std::string key = CandidateKey(cand, options_);

            OutcomeCache::Entry outcome;
            bool served_from_cache = false;
            const auto solve_begin = std::chrono::steady_clock::now();
            if (auto cached = cache.Find(key)) {
                ++NumOutcomeCacheHits;
                outcome           = std::move(*cached);
                served_from_cache = true;
            } else {
                outcome = cache.Insert(key, SolveCandidate(cand, options_));
            }
            const auto solve_ms = static_cast< uint64_t >(
                std::chrono::duration_cast< std::chrono::milliseconds >(
                    std::chrono::steady_clock::now() - solve_begin
                )
                    .count()
            );
            const bool wide = SlowReport().variable_floor != 0
                && cand.var_names.size() >= SlowReport().variable_floor;
            if (SlowReport().enabled && !served_from_cache
                && (solve_ms >= SlowReport().threshold_ms || wide))
            {
                RecordSlowCandidate(
                    f, cand, outcome.get(), solve_ms, outcome == nullptr ? "rejected" : "solved"
                );
            }
            if (outcome == nullptr) {
                // Nothing to rewrite here. Leaving the tree unmarked would have
                // the next run rediscover and re-solve it from scratch, which
                // is the dominant cost when a function is re-optimized, so the
                // shape that led nowhere is recorded on the root.
                record_rejection();
                requeue_retries();
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
                    record_rejection();
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
            // progress. Pricing it in instructions is what the rewrite is really
            // worth; which instructions count is the caller's call.
            const uint32_t budget = options_.cost_model == MbaCostModel::kTreeInstructions
                ? cand.tree_size
                : cand.dying_count;
            if (emitted.size() >= budget) {
                for (auto *dead : llvm::reverse(emitted)) {
                    if (dead->use_empty()) {
                        dead->eraseFromParent();
                    }
                }
                ++NumSkippedCost;
                LLVM_DEBUG(
                    llvm::dbgs() << "CoBRA: skipping — rewrite emits " << emitted.size()
                                 << " instructions against a budget of " << budget << "\n"
                );
                record_rejection();
                requeue_retries();
                continue;
            }

            cand.root->replaceAllUsesWith(new_val);
            ++NumSimplified;
            changed = true;
            demanded_bits.Invalidate();

            // The replacement is itself an MBA tree the detector will find next
            // run, and re-solving it almost always just reproves that CoBRA's
            // own output cannot shrink further. Fingerprinting it now turns
            // that into a lookup; if a later pass reshapes it the recorded
            // shape stops matching and it is examined again.
            if (auto *new_inst = llvm::dyn_cast< llvm::Instruction >(new_val)) {
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

        return changed ? llvm::PreservedAnalyses::none() : llvm::PreservedAnalyses::all();
    }

} // namespace cobra
