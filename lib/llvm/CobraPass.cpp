#include "cobra/llvm/CobraPass.h"
#include "IRReconstructor.h"
#include "MBADetector.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/Simplifier.h"

#ifdef COBRA_HAS_Z3
    #include "cobra/verify/Z3Verifier.h"
#endif

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/IR/Analysis.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

#define DEBUG_TYPE "cobra"

STATISTIC(NumCandidates, "Number of MBA candidates found");
STATISTIC(NumSimplified, "Number of MBA expressions simplified");
STATISTIC(NumSkippedCost, "Number of candidates skipped (cost gate)");
STATISTIC(NumSkippedUnsupported, "Number of candidates skipped (unsupported)");
STATISTIC(NumSkippedUnchanged, "Number of candidates skipped (signature unchanged)");

llvm::cl::opt< bool > CobraLogSkips(
    "cobra-log-skips",
    llvm::cl::desc("Log CoBRA candidates skipped because their signature is unchanged"),
    llvm::cl::value_desc("cobra-log-skips"), llvm::cl::init(false), llvm::cl::Optional
);

llvm::cl::list< std::string > CobraTargetOpcodes(
    "cobra-target-opcode",
    llvm::cl::desc(
        "Only run CoBRA on these opcodes (add, sub, mul, and, or, xor, shl, lshr, ashr, zext, "
        "sext, trunc). Repeatable or comma-separated. Empty means all supported opcodes."
    ),
    llvm::cl::value_desc("opcode"), llvm::cl::CommaSeparated
);

llvm::cl::list< std::string > CobraTargetContexts(
    "cobra-target-context",
    llvm::cl::desc(
        "Additional roots to run CoBRA on: binaryop, loadptr, storeptr, storevalue, return, "
        "icmp, inttoptr. Repeatable or comma-separated. Defaults to binaryop (plus whatever is "
        "set programmatically in CobraPassOptions)."
    ),
    llvm::cl::value_desc("context"), llvm::cl::CommaSeparated
);

namespace {

    // Map a lower-case LLVM opcode name to its numeric opcode. Returns 0 for
    // unknown names, mirroring the MBA opcodes accepted by DetectMbaCandidates.
    unsigned OpcodeFromName(llvm::StringRef name) {
        return llvm::StringSwitch< unsigned >(name)
            .Case("add", llvm::Instruction::Add)
            .Case("sub", llvm::Instruction::Sub)
            .Case("mul", llvm::Instruction::Mul)
            .Case("and", llvm::Instruction::And)
            .Case("or", llvm::Instruction::Or)
            .Case("xor", llvm::Instruction::Xor)
            .Case("shl", llvm::Instruction::Shl)
            .Case("lshr", llvm::Instruction::LShr)
            .Case("ashr", llvm::Instruction::AShr)
            .Case("zext", llvm::Instruction::ZExt)
            .Case("sext", llvm::Instruction::SExt)
            .Case("trunc", llvm::Instruction::Trunc)
            .Default(0);
    }

    // Map a context name to its MbaTargetContext flag. Returns 0 for unknown
    // names.
    uint32_t ContextFromName(llvm::StringRef name) {
        return llvm::StringSwitch< uint32_t >(name)
            .Case("binaryop", cobra::kMbaCtxBinaryOp)
            .Case("loadptr", cobra::kMbaCtxLoadPtr)
            .Case("storeptr", cobra::kMbaCtxStorePtr)
            .Case("storevalue", cobra::kMbaCtxStoreValue)
            .Case("return", cobra::kMbaCtxReturn)
            .Case("icmp", cobra::kMbaCtxICmp)
            .Case("inttoptr", cobra::kMbaCtxIntToPtr)
            .Default(0);
    }

    // 64-bit FNV-1a hash of a candidate's semantic signature.  Candidates
    // that agree on bitwidth, variable count and the full truth table are
    // functionally identical, so re-running the solver on them is wasted.
    uint64_t CandidateFingerprint(const cobra::MBACandidate &cand) {
        uint64_t h = 1469598103934665603ULL;  // FNV-1a offset basis
        auto mix   = [&](uint64_t v) {
            for (unsigned i = 0; i < 8; ++i) {
                h ^= static_cast< uint8_t >(v >> (i * 8));
                h *= 1099511628211ULL;  // FNV-1a prime
            }
        };
        mix(cand.bitwidth);
        mix(static_cast< uint64_t >(cand.var_names.size()));
        for (uint64_t s : cand.sig) {
            mix(s);
        }
        return h;
    }

    // Retrieve the fingerprint previously stamped on an instruction, if any.
    std::optional< uint64_t > ReadFingerprint(llvm::Instruction *inst, unsigned kind_id) {
        if (!inst->hasMetadata(kind_id)) {
            return std::nullopt;
        }
        const auto *node = inst->getMetadata(kind_id);
        if (const auto *cam = llvm::dyn_cast< llvm::ConstantAsMetadata >(node->getOperand(0))) {
            if (const auto *ci = llvm::dyn_cast< llvm::ConstantInt >(cam->getValue())) {
                return ci->getLimitedValue();
            }
        }
        return std::nullopt;
    }

    // Stamp a fingerprint on an instruction.  Survives across pipeline
    // iterations for any instruction that is not otherwise rewritten.
    void WriteFingerprint(llvm::Instruction *inst, unsigned kind_id, uint64_t fp) {
        auto &ctx   = inst->getContext();
        auto *value = llvm::ConstantInt::get(ctx, llvm::APInt(64, fp));
        auto *node  = llvm::MDNode::get(ctx, llvm::ConstantAsMetadata::get(value));
        inst->setMetadata(kind_id, node);
    }

}  // namespace

namespace cobra {

    llvm::PreservedAnalyses
    CobraPass::run(llvm::Function &f, llvm::FunctionAnalysisManager & /*AM*/) {
        bool changed = false;
        size_t skipped_unchanged = 0;

        // Resolve the set of opcodes CoBRA is allowed to simplify: the union of
        // the programmatic options and the command-line flag. An empty set means
        // "all supported MBA opcodes".
        std::set< unsigned > target_opcodes = options_.target_opcodes;
        for (const auto &name : CobraTargetOpcodes) {
            if (const unsigned op = OpcodeFromName(name); op != 0) {
                target_opcodes.insert(op);
            } else {
                llvm::errs() << "CoBRA: unknown target opcode '" << name << "'\n";
            }
        }

        // Resolve the target contexts: the union of the programmatic options and
        // the command-line flag.
        uint32_t target_contexts = options_.target_contexts;
        for (const auto &name : CobraTargetContexts) {
            if (const uint32_t ctx = ContextFromName(name); ctx != 0) {
                target_contexts |= ctx;
            } else {
                llvm::errs() << "CoBRA: unknown target context '" << name << "'\n";
            }
        }

        auto candidates = DetectMbaCandidates(
            f, options_.min_ast_size, options_.max_vars, target_opcodes, target_contexts
        );

        NumCandidates += candidates.size();

        const unsigned kSigKindId = f.getContext().getMDKindID("cobra-sig");

        for (auto &cand : candidates) {
            // Skip candidates whose semantic signature is unchanged since we
            // last attempted them — the solver would only reproduce the same
            // (non-)result.
            const uint64_t fp = CandidateFingerprint(cand);
            const auto existing = ReadFingerprint(cand.root, kSigKindId);
            if (existing.has_value() && *existing == fp) {
                ++NumSkippedUnchanged;
                ++skipped_unchanged;
                if (CobraLogSkips) {
                    llvm::errs() << "CoBRA: skip (signature unchanged): " << *cand.root << "\n";
                }
                continue;
            }
            // Stamp before solving: if the root survives (no simplification)
            // this marker makes the next pipeline iteration skip it.
            WriteFingerprint(cand.root, kSigKindId, fp);

            Options opts{ .bitwidth   = cand.bitwidth,
                          .max_vars   = options_.max_vars,
                          .spot_check = true,
                          .evaluator  = cand.evaluator };

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
                continue;
            }

            if (result.value().kind != SimplifyOutcome::Kind::kSimplified) {
                ++NumSkippedUnsupported;
                LLVM_DEBUG(
                    llvm::dbgs() << "CoBRA: not simplified: " << result.value().diag.reason
                                 << "\n"
                );
                continue;
            }

#ifdef COBRA_HAS_Z3
            if (options_.z3_verify) {
                if (ast == nullptr) {
                    ++NumSkippedUnsupported;
                    LLVM_DEBUG(
                        llvm::dbgs() << "CoBRA: skipping — Z3 verification requested but "
                                        "candidate AST is unavailable\n"
                    );
                    continue;
                }

                auto z3_expr = CloneExpr(*result.value().expr);
                auto idx_map = TryBuildVarSupport(cand.var_names, result.value().real_vars);
                if (!idx_map.has_value()) {
                    ++NumSkippedUnsupported;
                    LLVM_DEBUG(
                        llvm::dbgs() << "CoBRA: skipping — real_vars not contained in "
                                        "candidate variable set\n"
                    );
                    continue;
                }
                if (!idx_map->empty()) {
                    RemapVarIndices(*z3_expr, *idx_map);
                }

                auto z3_result = Z3VerifyExprs(
                    *ast, *z3_expr, cand.var_names, cand.bitwidth, options_.z3_settings
                );
                if (!z3_result.equivalent) {
                    ++NumSkippedUnsupported;
                    LLVM_DEBUG(
                        llvm::dbgs() << "CoBRA: skipping — Z3 verification failed: "
                                     << z3_result.counterexample << "\n"
                    );
                    continue;
                }
            }
#else
            if (options_.z3_verify) {
                ++NumSkippedUnsupported;
                LLVM_DEBUG(
                    llvm::dbgs() << "CoBRA: skipping — built without Z3 support but Z3 "
                                    "verification requested\n"
                );
                continue;
            }
#endif

            // Cost gate: don't replace if simplified form is not
            // smaller. nuw/nsw flags are intentionally dropped —
            // CoBRA's Expr model is modular arithmetic and we
            // cannot soundly preserve wrapping guarantees.
            if (cand.expr != nullptr) {
                auto original_cost   = ComputeCost(*cand.expr);
                auto simplified_cost = ComputeCost(*result.value().expr);
                if (!IsBetter(simplified_cost.cost, original_cost.cost)) {
                    ++NumSkippedCost;
                    LLVM_DEBUG(
                        llvm::dbgs() << "CoBRA: skipping — simplified form is not smaller\n"
                    );
                    continue;
                }
            }

            // Build variable index map for aux var elimination.
            // real_vars may be a subset of var_names with
            // reindexed positions.
            std::vector< uint32_t > var_map;
            const auto &real_vars = result.value().real_vars;
            if (!real_vars.empty() && real_vars.size() != cand.var_names.size()) {
                auto checked_var_map = TryBuildVarSupport(cand.var_names, real_vars);
                if (!checked_var_map.has_value()) {
                    ++NumSkippedUnsupported;
                    LLVM_DEBUG(
                        llvm::dbgs() << "CoBRA: skipping — real_vars not contained in "
                                        "candidate variable set\n"
                    );
                    continue;
                }
                var_map = std::move(*checked_var_map);
            }

            llvm::IRBuilder<> builder(cand.root);
            auto *new_val = ReconstructIr(*result.value().expr, cand, builder, var_map);

            cand.root->replaceAllUsesWith(new_val);
            ++NumSimplified;
            changed = true;

            LLVM_DEBUG(
                llvm::dbgs()
                << "CoBRA: simplified to "
                << Render(*result.value().expr, result.value().real_vars, cand.bitwidth) << "\n"
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

        if (CobraLogSkips && skipped_unchanged > 0) {
            llvm::errs() << "CoBRA: " << f.getName() << " skipped " << skipped_unchanged << "/"
                         << candidates.size() << " candidates (signature unchanged)\n";
        }

        return changed ? llvm::PreservedAnalyses::none() : llvm::PreservedAnalyses::all();
    }

} // namespace cobra
