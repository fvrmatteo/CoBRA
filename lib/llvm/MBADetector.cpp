#include "MBADetector.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/Expr.h"
#include "cobra/core/Simplifier.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Casting.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <queue>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace cobra {

    namespace {

        bool IsMbaOpcode(unsigned opcode) {
            switch (opcode) { // NOLINT(hicpp-multiway-paths-covered)
                case llvm::Instruction::Add:
                case llvm::Instruction::Sub:
                case llvm::Instruction::Mul:
                case llvm::Instruction::And:
                case llvm::Instruction::Or:
                case llvm::Instruction::Xor:
                case llvm::Instruction::Shl:
                case llvm::Instruction::LShr:
                case llvm::Instruction::AShr:
                case llvm::Instruction::ZExt:
                case llvm::Instruction::SExt:
                case llvm::Instruction::Trunc:
                    return true;
                default:
                    return false;
            }
        }

        // BFS from root following operands.  MBA-opcode instructions
        // are added to tree_insts; everything else becomes a leaf.
        // PHI nodes are treated as transparent when all incoming
        // values are MBA opcodes — the first arm is followed and a
        // redirect entry is recorded for evaluation / expr building.
        void CollectTree(
            llvm::Instruction *root, llvm::SmallVector< llvm::Instruction *, 16 > &tree_insts,
            std::vector< llvm::Value * > &leaves,
            llvm::DenseMap< llvm::Value *, llvm::Value * > &phi_redirects,
            bool try_phi_transparency = true
        ) {
            llvm::DenseSet< llvm::Value * > visited;
            std::queue< llvm::Value * > work;
            work.push(root);

            while (!work.empty()) {
                auto *v = work.front();
                work.pop();
                if (!visited.insert(v).second) {
                    continue;
                }

                auto *inst = llvm::dyn_cast< llvm::Instruction >(v);
                if ((inst != nullptr) && IsMbaOpcode(inst->getOpcode())) {
                    // LShr/AShr with a variable shift amount is unsupported —
                    // treat the whole instruction as a leaf.
                    const auto opcode = inst->getOpcode();
                    if ((opcode == llvm::Instruction::LShr || opcode == llvm::Instruction::AShr)
                        && !llvm::isa< llvm::ConstantInt >(inst->getOperand(1)))
                    {
                        if (std::find(leaves.begin(), leaves.end(), v) == leaves.end()) {
                            leaves.push_back(v);
                        }
                        continue;
                    }

                    tree_insts.push_back(inst);
                    for (auto &op : inst->operands()) {
                        work.push(op.get());
                    }
                } else if (auto *phi = llvm::dyn_cast< llvm::PHINode >(v)) {
                    // Check if every incoming value is an MBA opcode
                    // instruction (or a constant).  If so, treat the
                    // phi as transparent and follow the first arm.
                    bool all_mba = phi->getNumIncomingValues() > 0;
                    for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
                        auto *inc = phi->getIncomingValue(i);
                        if (llvm::isa< llvm::ConstantInt >(inc)) {
                            continue;
                        }
                        auto *inc_inst = llvm::dyn_cast< llvm::Instruction >(inc);
                        if ((inc_inst == nullptr) || !IsMbaOpcode(inc_inst->getOpcode())) {
                            all_mba = false;
                            break;
                        }
                    }

                    if (all_mba && try_phi_transparency) {
                        auto *chosen       = phi->getIncomingValue(0);
                        phi_redirects[phi] = chosen;
                        work.push(chosen);
                    } else {
                        if (std::find(leaves.begin(), leaves.end(), v) == leaves.end()) {
                            leaves.push_back(v);
                        }
                    }
                } else {
                    if (!llvm::isa< llvm::ConstantInt >(v)) {
                        if (std::find(leaves.begin(), leaves.end(), v) == leaves.end()) {
                            leaves.push_back(v);
                        }
                    }
                }
            }
        }

        uint64_t EvaluateTree(
            llvm::Instruction *root,
            const llvm::DenseMap< llvm::Value *, uint64_t > &assignments, uint32_t bitwidth,
            const llvm::DenseMap< llvm::Value *, llvm::Value * > &phi_redirects
        ) {
            llvm::DenseMap< llvm::Value *, uint64_t > cache;
            llvm::DenseSet< llvm::Value * > in_progress;
            uint64_t mask = Bitmask(bitwidth);

            std::function< uint64_t(llvm::Value *) > eval = [&](llvm::Value *v) -> uint64_t {
                auto it = cache.find(v);
                if (it != cache.end()) {
                    return it->second;
                }

                auto ait = assignments.find(v);
                if (ait != assignments.end()) {
                    cache[v] = ait->second;
                    return ait->second;
                }

                if (auto *ci = llvm::dyn_cast< llvm::ConstantInt >(v)) {
                    const uint64_t val = ci->getZExtValue() & mask;
                    cache[v]           = val;
                    return val;
                }

                // Some partially reconstructed MBA trees can still contain
                // operand/phi cycles. Those are unsupported for constant
                // evaluation, so stop recursion and conservatively fold the
                // cyclic sub-expression to zero instead of recursing forever.
                if (!in_progress.insert(v).second) {
                    cache[v] = 0;
                    return 0;
                }

                uint64_t result = 0;

                // Phi redirect: follow the chosen arm.
                auto pit = phi_redirects.find(v);
                if (pit != phi_redirects.end()) {
                    result   = eval(pit->second);
                    cache[v] = result;
                    in_progress.erase(v);
                    return result;
                }

                auto *inst = llvm::cast< llvm::Instruction >(v);

                if (inst->getOpcode() == llvm::Instruction::ZExt
                    || inst->getOpcode() == llvm::Instruction::SExt)
                {
                    result   = eval(inst->getOperand(0)) & mask;
                    cache[v] = result;
                    in_progress.erase(v);
                    return result;
                }

                // Truncation keeps only the low bits of the operand.
                if (inst->getOpcode() == llvm::Instruction::Trunc) {
                    const uint32_t dest_bw = inst->getType()->getIntegerBitWidth();
                    result                 = eval(inst->getOperand(0)) & Bitmask(dest_bw);
                    cache[v]               = result;
                    in_progress.erase(v);
                    return result;
                }

                const uint64_t lhs = eval(inst->getOperand(0));
                const uint64_t rhs = eval(inst->getOperand(1));

                switch (inst->getOpcode()) {
                    case llvm::Instruction::Add:
                        result = ModAdd(lhs, rhs, bitwidth);
                        break;
                    case llvm::Instruction::Sub:
                        result = ModSub(lhs, rhs, bitwidth);
                        break;
                    case llvm::Instruction::Mul:
                        result = ModMul(lhs, rhs, bitwidth);
                        break;
                    case llvm::Instruction::And:
                        result = (lhs & rhs) & mask;
                        break;
                    case llvm::Instruction::Or:
                        result = (lhs | rhs) & mask;
                        break;
                    case llvm::Instruction::Xor:
                        result = (lhs ^ rhs) & mask;
                        break;
                    case llvm::Instruction::LShr:
                        result = (lhs >> rhs) & mask;
                        break;
                    case llvm::Instruction::AShr: {
                        // Arithmetic shift right: logical shift plus sign-fill.
                        result = (lhs >> rhs) & mask;
                        const uint64_t sign_bit = 1ULL << (bitwidth - 1);
                        if (rhs != 0 && (lhs & sign_bit) != 0) {
                            result |= (~0ULL << (bitwidth - rhs)) & mask;
                        }
                        break;
                    }
                    case llvm::Instruction::Shl:
                        result = (lhs << rhs) & mask;
                        break;
                    default:
                        result = 0;
                        break;
                }

                cache[v] = result;
                in_progress.erase(v);
                return result;
            };

            return eval(root);
        }

        bool HasPolynomialMul(
            llvm::Instruction *root, const llvm::DenseSet< llvm::Value * > &visited_tree
        ) {
            llvm::DenseMap< llvm::Value *, bool > depends_on_var;

            std::function< bool(llvm::Value *) > check = [&](llvm::Value *v) -> bool {
                auto it = depends_on_var.find(v);
                if (it != depends_on_var.end()) {
                    return it->second;
                }

                if (llvm::isa< llvm::ConstantInt >(v)) {
                    depends_on_var[v] = false;
                    return false;
                }

                auto *inst = llvm::dyn_cast< llvm::Instruction >(v);
                if (!inst || !visited_tree.contains(inst)) {
                    depends_on_var[v] = true;
                    return true;
                }

                if (inst->getOpcode() == llvm::Instruction::ZExt
                    || inst->getOpcode() == llvm::Instruction::SExt)
                {
                    const bool dep    = check(inst->getOperand(0));
                    depends_on_var[v] = dep;
                    return dep;
                }

                const bool lhs_dep = check(inst->getOperand(0));
                const bool rhs_dep =
                    inst->getNumOperands() > 1 ? check(inst->getOperand(1)) : false;
                const bool dep    = lhs_dep || rhs_dep;
                depends_on_var[v] = dep;

                if (inst->getOpcode() == llvm::Instruction::Mul && lhs_dep && rhs_dep) {
                    return true;
                }
                return dep;
            };

            llvm::DenseSet< llvm::Value * > visited;
            std::queue< llvm::Value * > work;
            work.push(root);

            while (!work.empty()) {
                auto *v = work.front();
                work.pop();
                if (!visited.insert(v).second) {
                    continue;
                }
                check(v);
                if (auto *inst = llvm::dyn_cast< llvm::Instruction >(v)) {
                    if (visited_tree.contains(inst) != 0u) {
                        for (auto &op : inst->operands()) {
                            work.push(op.get());
                        }
                    }
                }
            }

            for (auto *v : visited) {
                auto *inst = llvm::dyn_cast< llvm::Instruction >(v);
                if (inst == nullptr) {
                    continue;
                }
                if (inst->getOpcode() != llvm::Instruction::Mul) {
                    continue;
                }
                if (visited_tree.contains(inst) == 0u) {
                    continue;
                }
                const bool lhs_dep = depends_on_var.lookup(inst->getOperand(0));
                const bool rhs_dep = depends_on_var.lookup(inst->getOperand(1));
                if (lhs_dep && rhs_dep) {
                    return true;
                }
            }
            return false;
        }

        std::unique_ptr< Expr > BuildExprFromIR(
            llvm::Value *v, const std::vector< llvm::Value * > &leaves,
            const llvm::DenseSet< llvm::Value * > &tree_set, uint64_t mask,
            const llvm::DenseMap< llvm::Value *, llvm::Value * > &phi_redirects
        ) {
            // Phi redirect: build from the chosen arm.
            auto pit = phi_redirects.find(v);
            if (pit != phi_redirects.end()) {
                return BuildExprFromIR(pit->second, leaves, tree_set, mask, phi_redirects);
            }

            // Constant
            if (auto *ci = llvm::dyn_cast< llvm::ConstantInt >(v)) {
                return Expr::Constant(ci->getZExtValue() & mask);
            }

            // Leaf (variable)
            auto leaf_it = std::find(leaves.begin(), leaves.end(), v);
            if (leaf_it != leaves.end()) {
                auto idx = static_cast< uint32_t >(leaf_it - leaves.begin());
                return Expr::Variable(idx);
            }

            auto *inst = llvm::dyn_cast< llvm::Instruction >(v);
            if (inst == nullptr || !tree_set.contains(inst)) {
                return nullptr;
            }

            // ZExt/SExt — pass through to inner operand
            if (inst->getOpcode() == llvm::Instruction::ZExt
                || inst->getOpcode() == llvm::Instruction::SExt)
            {
                return BuildExprFromIR(
                    inst->getOperand(0), leaves, tree_set, mask, phi_redirects
                );
            }

            // Trunc — keep only the low `dest_bw` bits.
            if (inst->getOpcode() == llvm::Instruction::Trunc) {
                const uint32_t dest_bw = inst->getType()->getIntegerBitWidth();
                auto child =
                    BuildExprFromIR(inst->getOperand(0), leaves, tree_set, mask, phi_redirects);
                if (child == nullptr) { return nullptr; }
                return Expr::BitwiseAnd(std::move(child), Expr::Constant(Bitmask(dest_bw)));
            }

            // LShr with constant shift amount
            if (inst->getOpcode() == llvm::Instruction::LShr) {
                auto *shift_amt = llvm::dyn_cast< llvm::ConstantInt >(inst->getOperand(1));
                if (shift_amt == nullptr) {
                    return nullptr;
                }
                auto child =
                    BuildExprFromIR(inst->getOperand(0), leaves, tree_set, mask, phi_redirects);
                if (child == nullptr) {
                    return nullptr;
                }
                return Expr::LogicalShr(std::move(child), shift_amt->getZExtValue());
            }

            // Shl with constant shift amount
            if (inst->getOpcode() == llvm::Instruction::Shl) {
                auto *shift_amt = llvm::dyn_cast< llvm::ConstantInt >(inst->getOperand(1));
                if (shift_amt == nullptr) { return nullptr; }
                auto child =
                    BuildExprFromIR(inst->getOperand(0), leaves, tree_set, mask, phi_redirects);
                if (child == nullptr) { return nullptr; }
                uint64_t mul_val = 1ULL << shift_amt->getZExtValue();
                return Expr::Mul(std::move(child), Expr::Constant(mul_val));
            }

            // AShr with constant shift amount: arithmetic shift = logical
            // shift plus a sign-fill of the vacated high bits.
            if (inst->getOpcode() == llvm::Instruction::AShr) {
                auto *shift_amt = llvm::dyn_cast< llvm::ConstantInt >(inst->getOperand(1));
                if (shift_amt == nullptr) { return nullptr; }
                const uint64_t k = shift_amt->getZExtValue();
                auto child =
                    BuildExprFromIR(inst->getOperand(0), leaves, tree_set, mask, phi_redirects);
                if (child == nullptr) { return nullptr; }

                const uint32_t bw = static_cast< uint32_t >(std::bit_width(mask));
                // mask & ~Bitmask(bw-k) = bits [bw-k, bw) set.
                const uint64_t sign_fill = mask & ~Bitmask(bw - k);
                auto logical  = Expr::LogicalShr(CloneExpr(*child), k);
                auto sign     = Expr::LogicalShr(CloneExpr(*child), bw - 1);
                auto neg_sign = Expr::Negate(std::move(sign));
                auto masked   = Expr::BitwiseAnd(std::move(neg_sign), Expr::Constant(sign_fill));
                return Expr::BitwiseOr(std::move(logical), std::move(masked));
            }

            // Binary operations
            auto lhs =
                BuildExprFromIR(inst->getOperand(0), leaves, tree_set, mask, phi_redirects);
            auto rhs =
                BuildExprFromIR(inst->getOperand(1), leaves, tree_set, mask, phi_redirects);
            if (lhs == nullptr || rhs == nullptr) {
                return nullptr;
            }

            switch (inst->getOpcode()) {
                case llvm::Instruction::Add:
                    return Expr::Add(std::move(lhs), std::move(rhs));
                case llvm::Instruction::Sub:
                    return Expr::Add(std::move(lhs), Expr::Negate(std::move(rhs)));
                case llvm::Instruction::Mul:
                    return Expr::Mul(std::move(lhs), std::move(rhs));
                case llvm::Instruction::And:
                    return Expr::BitwiseAnd(std::move(lhs), std::move(rhs));
                case llvm::Instruction::Or:
                    return Expr::BitwiseOr(std::move(lhs), std::move(rhs));
                case llvm::Instruction::Xor: {
                    // Detect NOT: xor %x, -1
                    if (auto *ci = llvm::dyn_cast< llvm::ConstantInt >(inst->getOperand(1))) {
                        if (ci->isAllOnesValue()) {
                            return Expr::BitwiseNot(std::move(lhs));
                        }
                    }
                    if (auto *ci = llvm::dyn_cast< llvm::ConstantInt >(inst->getOperand(0))) {
                        if (ci->isAllOnesValue()) {
                            return Expr::BitwiseNot(std::move(rhs));
                        }
                    }
                    return Expr::BitwiseXor(std::move(lhs), std::move(rhs));
                }
                default:
                    return nullptr;
            }
        }

        // Check that every leaf dependency of an alternative phi arm
        // is either a constant or present in `leaf_set`.  This guards
        // against evaluating arms whose subtrees reference variables
        // outside the candidate's variable set.
        bool ArmDepsInLeafSet(
            llvm::Value *arm_root, const llvm::DenseSet< llvm::Value * > &leaf_set
        ) {
            llvm::DenseSet< llvm::Value * > visited;
            std::queue< llvm::Value * > work;
            work.push(arm_root);

            while (!work.empty()) {
                auto *v = work.front();
                work.pop();
                if (!visited.insert(v).second) {
                    continue;
                }

                if (llvm::isa< llvm::ConstantInt >(v)) {
                    continue;
                }
                if (leaf_set.contains(v)) {
                    continue;
                }

                auto *inst = llvm::dyn_cast< llvm::Instruction >(v);
                if ((inst == nullptr) || !IsMbaOpcode(inst->getOpcode())) {
                    return false;
                }

                // LShr with variable shift — can't evaluate
                if (inst->getOpcode() == llvm::Instruction::LShr
                    && !llvm::isa< llvm::ConstantInt >(inst->getOperand(1)))
                {
                    if (!leaf_set.contains(inst)) {
                        return false;
                    }
                    continue;
                }

                for (auto &op : inst->operands()) {
                    work.push(op.get());
                }
            }
            return true;
        }

        // Verify that all incoming arms of each transparent phi
        // evaluate identically for random inputs.  Returns false if
        // any arm diverges, indicating the phi should be treated as
        // a leaf instead.
        bool VerifyPhiArms(
            const llvm::DenseMap< llvm::Value *, llvm::Value * > &phi_redirects,
            const std::vector< llvm::Value * > &leaves, uint32_t bitwidth
        ) {
            if (phi_redirects.empty()) {
                return true;
            }

            const uint64_t mask           = Bitmask(bitwidth);
            constexpr uint32_t kNumProbes = 8;

            llvm::DenseSet< llvm::Value * > leaf_set;
            for (auto *lv : leaves) {
                leaf_set.insert(lv);
            }

            // NOLINTNEXTLINE(cert-msc32-c,cert-msc51-cpp)
            std::mt19937_64 rng(0xC0B7A);

            for (const auto &[phi_val, chosen] : phi_redirects) {
                auto *phi = llvm::cast< llvm::PHINode >(phi_val);

                for (uint32_t probe = 0; probe < kNumProbes; ++probe) {
                    llvm::DenseMap< llvm::Value *, uint64_t > assignments;
                    for (auto *leaf : leaves) {
                        assignments[leaf] = rng() & mask;
                    }

                    // Evaluate chosen arm.
                    uint64_t chosen_val = 0;
                    if (auto *ci = llvm::dyn_cast< llvm::ConstantInt >(chosen)) {
                        chosen_val = ci->getZExtValue() & mask;
                    } else {
                        chosen_val = EvaluateTree(
                            llvm::cast< llvm::Instruction >(chosen), assignments, bitwidth,
                            phi_redirects
                        );
                    }

                    // Check every other arm.
                    for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
                        auto *inc = phi->getIncomingValue(i);
                        if (inc == chosen) {
                            continue;
                        }

                        // Pre-check: arm's dependencies must be in our leaf set.
                        if (!ArmDepsInLeafSet(inc, leaf_set)) {
                            return false;
                        }

                        uint64_t inc_val = 0;
                        if (auto *ci = llvm::dyn_cast< llvm::ConstantInt >(inc)) {
                            inc_val = ci->getZExtValue() & mask;
                        } else {
                            inc_val = EvaluateTree(
                                llvm::cast< llvm::Instruction >(inc), assignments, bitwidth,
                                phi_redirects
                            );
                        }

                        if (inc_val != chosen_val) {
                            return false;
                        }
                    }
                }
            }
            return true;
        }

    } // namespace

    std::vector< MBACandidate > DetectMbaCandidates(
        llvm::Function &f, uint32_t min_ast_size, uint32_t /*max_vars*/,
        const std::set< unsigned > &opcodes, uint32_t contexts
    ) {
        std::vector< MBACandidate > candidates;
        llvm::DenseSet< llvm::Instruction * > already_in_tree;

        constexpr uint32_t kPreElimCap = 20;

        // Build and append a candidate rooted at `root` (an integer
        // instruction). Claims the whole collected subtree so it is not
        // emitted again as a standalone candidate.
        auto emit = [&](llvm::Instruction *root) {
            if (already_in_tree.contains(root) != 0u) {
                return;
            }
            if (!root->getType()->isIntegerTy()) {
                return;
            }
            const uint32_t bw = root->getType()->getIntegerBitWidth();
            if (bw > 64) {
                return;
            }

            llvm::SmallVector< llvm::Instruction *, 16 > tree_insts;
            std::vector< llvm::Value * > leaves;
            llvm::DenseMap< llvm::Value *, llvm::Value * > phi_redirects;
            CollectTree(root, tree_insts, leaves, phi_redirects);

            if (tree_insts.size() < min_ast_size) {
                return;
            }
            if (leaves.size() > kPreElimCap) {
                return;
            }

            const llvm::DenseSet< llvm::Value * > tree_set(tree_insts.begin(), tree_insts.end());
            if (HasPolynomialMul(root, tree_set)) {
                return;
            }

            // Verify transparent phis — if any arm diverges, re-collect
            // without phi transparency.
            if (!phi_redirects.empty() && !VerifyPhiArms(phi_redirects, leaves, bw)) {
                tree_insts.clear();
                leaves.clear();
                phi_redirects.clear();
                CollectTree(
                    root, tree_insts, leaves, phi_redirects, /*try_phi_transparency=*/false
                );

                if (tree_insts.size() < min_ast_size) {
                    return;
                }
                if (leaves.size() > kPreElimCap) {
                    return;
                }
            }

            for (auto *ti : tree_insts) {
                already_in_tree.insert(ti);
            }

            const auto num_vars  = static_cast< uint32_t >(leaves.size());
            const size_t sig_len = 1ULL << num_vars;
            std::vector< uint64_t > sig(sig_len);

            for (size_t i = 0; i < sig_len; ++i) {
                llvm::DenseMap< llvm::Value *, uint64_t > assignments;
                for (uint32_t v = 0; v < num_vars; ++v) {
                    assignments[leaves[v]] = (i >> v) & 1;
                }
                sig[i] = EvaluateTree(root, assignments, bw, phi_redirects);
            }

            std::vector< std::string > var_names;
            for (uint32_t v = 0; v < num_vars; ++v) {
                if (leaves[v]->hasName()) {
                    var_names.push_back(leaves[v]->getName().str());
                } else {
                    var_names.push_back("v" + std::to_string(v));
                }
            }

            const uint64_t mask = Bitmask(bw);
            auto expr           = BuildExprFromIR(root, leaves, tree_set, mask, phi_redirects);

            // Build evaluator lambda for full-width verification.
            // Captures raw pointers to LLVM Values which remain valid
            // for the lifetime of the function being processed.
            Evaluator evaluator;
            if (expr != nullptr) {
                evaluator = [root_inst = root, leaf_vals = leaves, bitwidth = bw,
                             redirects = phi_redirects](
                                const std::vector< uint64_t > &vals
                            ) -> uint64_t {
                    llvm::DenseMap< llvm::Value *, uint64_t > assignments;
                    for (size_t i = 0; i < leaf_vals.size(); ++i) {
                        assignments[leaf_vals[i]] = vals[i];
                    }
                    return EvaluateTree(root_inst, assignments, bitwidth, redirects);
                };
            }

            candidates.push_back(
                MBACandidate{ .root        = root,
                              .leaf_values = std::move(leaves),
                              .var_names   = std::move(var_names),
                              .sig         = std::move(sig),
                              .bitwidth    = bw,
                              .expr        = std::move(expr),
                              .evaluator   = std::move(evaluator) }
            );
        };

        // Integer offset of a load/store pointer: the single index of a
        // getelementptr (the decompiler's "ptradd" form) or the source of an
        // inttoptr. Returns nullptr for constants and unsupported pointers.
        auto get_pointer_offset = [](llvm::Value *ptr) -> llvm::Value * {
            if (auto *G = llvm::dyn_cast< llvm::GetElementPtrInst >(ptr)) {
                if (G->getNumIndices() != 1) {
                    return nullptr;
                }
                return G->getOperand(1);
            }
            if (auto *I2P = llvm::dyn_cast< llvm::IntToPtrInst >(ptr)) {
                return I2P->getOperand(0);
            }
            return nullptr;
        };

        // 1. MBA opcode roots. Post-order + reverse iteration hits the
        // outermost roots first, so the largest tree claims inner nodes
        // before they can be emitted as standalone candidates.
        if ((contexts & kMbaCtxBinaryOp) != 0u) {
            for (auto *bb : post_order(&f)) {
                for (auto &inst : llvm::reverse(*bb)) {
                    if (!IsMbaOpcode(inst.getOpcode())) {
                        continue;
                    }
                    if (!opcodes.empty() && !opcodes.contains(inst.getOpcode())) {
                        continue;
                    }
                    emit(&inst);
                }
            }
        }

        // 2. Context targets: pointer offsets of loads/stores, store value
        // operands, return operands, icmp operands and inttoptr sources.
        const uint32_t context_mask = kMbaCtxLoadPtr | kMbaCtxStorePtr | kMbaCtxStoreValue
                                      | kMbaCtxReturn | kMbaCtxICmp | kMbaCtxIntToPtr;
        if ((contexts & context_mask) != 0u) {
            for (auto &bb : f) {
                for (auto &inst : bb) {
                    if (auto *L = llvm::dyn_cast< llvm::LoadInst >(&inst)) {
                        if ((contexts & kMbaCtxLoadPtr) != 0u) {
                            if (auto *off = get_pointer_offset(L->getPointerOperand())) {
                                if (auto *off_inst = llvm::dyn_cast< llvm::Instruction >(off)) {
                                    emit(off_inst);
                                }
                            }
                        }
                    } else if (auto *S = llvm::dyn_cast< llvm::StoreInst >(&inst)) {
                        if ((contexts & kMbaCtxStorePtr) != 0u) {
                            if (auto *off = get_pointer_offset(S->getPointerOperand())) {
                                if (auto *off_inst = llvm::dyn_cast< llvm::Instruction >(off)) {
                                    emit(off_inst);
                                }
                            }
                        }
                        if ((contexts & kMbaCtxStoreValue) != 0u) {
                            if (auto *val =
                                    llvm::dyn_cast< llvm::Instruction >(S->getValueOperand()))
                            {
                                emit(val);
                            }
                        }
                    } else if (auto *R = llvm::dyn_cast< llvm::ReturnInst >(&inst)) {
                        if ((contexts & kMbaCtxReturn) != 0u) {
                            if (auto *op = llvm::dyn_cast< llvm::Instruction >(R->getReturnValue()))
                            {
                                emit(op);
                            }
                        }
                    } else if (auto *C = llvm::dyn_cast< llvm::ICmpInst >(&inst)) {
                        if ((contexts & kMbaCtxICmp) != 0u) {
                            for (auto *op : { C->getOperand(0), C->getOperand(1) }) {
                                if (auto *op_inst = llvm::dyn_cast< llvm::Instruction >(op)) {
                                    emit(op_inst);
                                }
                            }
                        }
                    } else if (auto *I2P = llvm::dyn_cast< llvm::IntToPtrInst >(&inst)) {
                        if ((contexts & kMbaCtxIntToPtr) != 0u) {
                            if (auto *op =
                                    llvm::dyn_cast< llvm::Instruction >(I2P->getOperand(0)))
                            {
                                emit(op);
                            }
                        }
                    }
                }
            }
        }

        return candidates;
    }

} // namespace cobra
