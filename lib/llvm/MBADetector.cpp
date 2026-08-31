#include "MBADetector.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/Expr.h"
#include "cobra/core/Simplifier.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/Casting.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <queue>
#include <random>
#include <string>
#include <utility>
#include <vector>

#define DEBUG_TYPE "cobra"

STATISTIC(
    NumFingerprintSkips, "Number of MBA trees skipped (unchanged since last examined)"
);

namespace cobra {

    namespace {

        // Metadata kind holding an MbaFingerprint plus the options tag it was
        // recorded under. Unknown metadata is dropped rather than rewritten
        // when a pass rebuilds an instruction, so a lost record only costs a
        // redundant examination — never a missed simplification.
        constexpr const char *kFingerprintMD = "cobra.mba";

        uint64_t MixFingerprint(uint64_t hash, uint64_t value) {
            constexpr uint64_t kGoldenRatio = 0x9E3779B97F4A7C15ULL;
            return hash ^ (value + kGoldenRatio + (hash << 6) + (hash >> 2));
        }

        // Hash and height of the subtree at `v`, memoized so a shared operand
        // is folded once no matter how many parents reach it.
        std::pair< uint64_t, uint32_t > FingerprintNode(
            llvm::Value *v, const llvm::DenseMap< llvm::Value *, uint32_t > &leaf_index,
            const llvm::DenseSet< llvm::Value * > &tree_set,
            const llvm::DenseMap< llvm::Value *, llvm::Value * > &phi_redirects,
            llvm::DenseMap< llvm::Value *, std::pair< uint64_t, uint32_t > > &memo
        ) {
            if (auto it = memo.find(v); it != memo.end()) { return it->second; }

            // Leaves are checked before the redirect map so that a phi kept as
            // a leaf hashes by position, matching how it is later evaluated.
            if (auto it = leaf_index.find(v); it != leaf_index.end()) {
                const std::pair< uint64_t, uint32_t > leaf{ MixFingerprint(1, it->second), 0 };
                memo[v] = leaf;
                return leaf;
            }
            if (auto *ci = llvm::dyn_cast< llvm::ConstantInt >(v)) {
                const std::pair< uint64_t, uint32_t > c{ MixFingerprint(2, ci->getZExtValue()),
                                                         0 };
                memo[v] = c;
                return c;
            }
            if (auto it = phi_redirects.find(v); it != phi_redirects.end()) {
                auto through = FingerprintNode(
                    it->second, leaf_index, tree_set, phi_redirects, memo
                );
                memo[v] = through;
                return through;
            }

            auto *inst = llvm::dyn_cast< llvm::Instruction >(v);
            if (inst == nullptr || !tree_set.contains(v)) {
                // Anything the detector would not descend through is opaque;
                // its identity is irrelevant because it cannot be rewritten.
                const std::pair< uint64_t, uint32_t > opaque{ 3, 0 };
                memo[v] = opaque;
                return opaque;
            }

            // Guard against a cycle through an unverified phi by claiming the
            // slot before descending. A revisit then folds this placeholder,
            // which keeps the hash deterministic instead of recursing forever.
            memo[v] = { 4, 0 };

            uint64_t hash     = MixFingerprint(5, inst->getOpcode());
            uint32_t children = 0;
            for (llvm::Use &use : inst->operands()) {
                auto child =
                    FingerprintNode(use.get(), leaf_index, tree_set, phi_redirects, memo);
                hash     = MixFingerprint(hash, child.first);
                children = std::max(children, child.second);
            }

            const std::pair< uint64_t, uint32_t > result{ hash, children + 1 };
            memo[v] = result;
            return result;
        }

        MbaFingerprint FingerprintTree(
            llvm::Instruction *root, const std::vector< llvm::Value * > &leaves,
            const llvm::DenseSet< llvm::Value * > &tree_set,
            const llvm::DenseMap< llvm::Value *, llvm::Value * > &phi_redirects
        ) {
            llvm::DenseMap< llvm::Value *, uint32_t > leaf_index;
            for (uint32_t i = 0; i < leaves.size(); ++i) { leaf_index[leaves[i]] = i; }

            llvm::DenseMap< llvm::Value *, std::pair< uint64_t, uint32_t > > memo;
            auto walked = FingerprintNode(root, leaf_index, tree_set, phi_redirects, memo);

            return MbaFingerprint{ .structure = walked.first,
                                   .depth     = walked.second,
                                   .num_vars  = static_cast< uint32_t >(leaves.size()) };
        }

        // True when `inst` already carries exactly this fingerprint under the
        // same options tag, meaning a previous run reached the same tree.
        bool FingerprintUnchanged(
            const llvm::Instruction &inst, const MbaFingerprint &fp, uint64_t options_tag
        ) {
            auto *node = inst.getMetadata(kFingerprintMD);
            if (node == nullptr || node->getNumOperands() != 4) { return false; }

            const auto read = [node](unsigned i) -> std::optional< uint64_t > {
                auto *as_const = llvm::dyn_cast< llvm::ConstantAsMetadata >(node->getOperand(i));
                if (as_const == nullptr) { return std::nullopt; }
                auto *as_int = llvm::dyn_cast< llvm::ConstantInt >(as_const->getValue());
                if (as_int == nullptr) { return std::nullopt; }
                return as_int->getZExtValue();
            };

            auto structure = read(0);
            auto depth     = read(1);
            auto num_vars  = read(2);
            auto tag       = read(3);
            if (!structure || !depth || !num_vars || !tag) { return false; }

            return *structure == fp.structure && *depth == fp.depth
                && *num_vars == fp.num_vars && *tag == options_tag;
        }

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
                case llvm::Instruction::ZExt:
                case llvm::Instruction::SExt:
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
                    // LShr with variable shift amount is unsupported —
                    // treat the whole instruction as a leaf.
                    if (inst->getOpcode() == llvm::Instruction::LShr
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

        // Flattened, topologically ordered form of an MBA tree.
        //
        // Probe-based verification evaluates the same tree hundreds of
        // thousands of times, so the operand walk is lowered to a slot vector
        // once and each evaluation becomes a linear pass with no hashing and
        // no allocation.
        //
        // Lowering follows operands depth-first, which is what makes the
        // cycle rule below well defined: partially reconstructed trees can
        // still contain operand or phi cycles, so a back edge onto a node
        // that is still being lowered resolves to zero, while references
        // discovered after that node completes see its real value.
        class TreeEvalPlan
        {
          public:
            static TreeEvalPlan Build(
                llvm::Instruction *root, const std::vector< llvm::Value * > &leaves,
                uint32_t bitwidth,
                const llvm::DenseMap< llvm::Value *, llvm::Value * > &phi_redirects
            ) {
                TreeEvalPlan plan;
                plan.mask_ = Bitmask(bitwidth);

                llvm::DenseMap< llvm::Value *, uint32_t > leaf_index;
                for (size_t i = 0; i < leaves.size(); ++i) {
                    leaf_index.try_emplace(leaves[i], static_cast< uint32_t >(i));
                }

                // Slot 0 holds a literal zero. It backs both cycle back edges
                // and unsupported opcodes, which fold to zero.
                plan.consts_.push_back(0);
                plan.nodes_.push_back(Node{ .op = Op::kConst, .a = 0, .b = 0 });

                llvm::DenseMap< llvm::Value *, uint32_t > slot_of;
                llvm::DenseSet< llvm::Value * > in_progress;
                plan.Lower(root, leaf_index, phi_redirects, slot_of, in_progress);
                plan.root_ = slot_of.lookup(root);
                return plan;
            }

            size_t SlotCount() const { return nodes_.size(); }

            uint64_t Evaluate(
                const uint64_t *inputs, size_t num_inputs, std::vector< uint64_t > &slots
            ) const {
                if (slots.size() < nodes_.size()) {
                    slots.resize(nodes_.size());
                }
                const uint64_t mask = mask_;
                for (size_t i = 0; i < nodes_.size(); ++i) {
                    const Node &n = nodes_[i];
                    switch (n.op) {
                        case Op::kConst:
                            slots[i] = consts_[n.a];
                            break;
                        case Op::kInput:
                            slots[i] = n.a < num_inputs ? inputs[n.a] : 0;
                            break;
                        case Op::kMask:
                            slots[i] = slots[n.a] & mask;
                            break;
                        case Op::kAdd:
                            slots[i] = (slots[n.a] + slots[n.b]) & mask;
                            break;
                        case Op::kSub:
                            slots[i] = (slots[n.a] - slots[n.b]) & mask;
                            break;
                        case Op::kMul:
                            slots[i] = (slots[n.a] * slots[n.b]) & mask;
                            break;
                        case Op::kAnd:
                            slots[i] = (slots[n.a] & slots[n.b]) & mask;
                            break;
                        case Op::kOr:
                            slots[i] = (slots[n.a] | slots[n.b]) & mask;
                            break;
                        case Op::kXor:
                            slots[i] = (slots[n.a] ^ slots[n.b]) & mask;
                            break;
                        case Op::kShl:
                            slots[i] = ShiftLeft(slots[n.a], slots[n.b]) & mask;
                            break;
                        case Op::kLShr:
                            slots[i] = ShiftRight(slots[n.a], slots[n.b]) & mask;
                            break;
                    }
                }
                return slots[root_];
            }

          private:
            enum class Op : uint8_t {
                kConst,
                kInput,
                kMask,
                kAdd,
                kSub,
                kMul,
                kAnd,
                kOr,
                kXor,
                kShl,
                kLShr,
            };

            struct Node
            {
                Op op;
                uint32_t a;
                uint32_t b;
            };

            // Shift amounts come from an evaluated operand, so they are not
            // bounded by the type width. Saturate instead of shifting out of
            // range, matching ModShr's treatment of oversized amounts.
            static uint64_t ShiftLeft(uint64_t v, uint64_t amount) {
                return amount >= 64 ? 0 : v << amount;
            }

            static uint64_t ShiftRight(uint64_t v, uint64_t amount) {
                return amount >= 64 ? 0 : v >> amount;
            }

            uint32_t Emit(Op op, uint32_t a, uint32_t b) {
                auto slot = static_cast< uint32_t >(nodes_.size());
                nodes_.push_back(Node{ .op = op, .a = a, .b = b });
                return slot;
            }

            uint32_t Lower(
                llvm::Value *v, const llvm::DenseMap< llvm::Value *, uint32_t > &leaf_index,
                const llvm::DenseMap< llvm::Value *, llvm::Value * > &phi_redirects,
                llvm::DenseMap< llvm::Value *, uint32_t > &slot_of,
                llvm::DenseSet< llvm::Value * > &in_progress
            ) {
                auto known = slot_of.find(v);
                if (known != slot_of.end()) {
                    return known->second;
                }

                auto leaf = leaf_index.find(v);
                if (leaf != leaf_index.end()) {
                    const uint32_t slot = Emit(Op::kInput, leaf->second, 0);
                    slot_of[v]          = slot;
                    return slot;
                }

                if (auto *ci = llvm::dyn_cast< llvm::ConstantInt >(v)) {
                    const auto index = static_cast< uint32_t >(consts_.size());
                    consts_.push_back(ci->getZExtValue() & mask_);
                    const uint32_t slot = Emit(Op::kConst, index, 0);
                    slot_of[v]          = slot;
                    return slot;
                }

                if (!in_progress.insert(v).second) {
                    slot_of[v] = kZeroSlot;
                    return kZeroSlot;
                }

                const uint32_t slot =
                    LowerOperands(v, leaf_index, phi_redirects, slot_of, in_progress);
                slot_of[v] = slot;
                in_progress.erase(v);
                return slot;
            }

            uint32_t LowerOperands(
                llvm::Value *v, const llvm::DenseMap< llvm::Value *, uint32_t > &leaf_index,
                const llvm::DenseMap< llvm::Value *, llvm::Value * > &phi_redirects,
                llvm::DenseMap< llvm::Value *, uint32_t > &slot_of,
                llvm::DenseSet< llvm::Value * > &in_progress
            ) {
                auto lower = [&](llvm::Value *operand) {
                    return Lower(operand, leaf_index, phi_redirects, slot_of, in_progress);
                };

                auto redirect = phi_redirects.find(v);
                if (redirect != phi_redirects.end()) {
                    return lower(redirect->second);
                }

                auto *inst = llvm::cast< llvm::Instruction >(v);
                if (inst->getOpcode() == llvm::Instruction::ZExt
                    || inst->getOpcode() == llvm::Instruction::SExt)
                {
                    return Emit(Op::kMask, lower(inst->getOperand(0)), 0);
                }

                const uint32_t lhs = lower(inst->getOperand(0));
                const uint32_t rhs = lower(inst->getOperand(1));

                switch (inst->getOpcode()) {
                    case llvm::Instruction::Add:
                        return Emit(Op::kAdd, lhs, rhs);
                    case llvm::Instruction::Sub:
                        return Emit(Op::kSub, lhs, rhs);
                    case llvm::Instruction::Mul:
                        return Emit(Op::kMul, lhs, rhs);
                    case llvm::Instruction::And:
                        return Emit(Op::kAnd, lhs, rhs);
                    case llvm::Instruction::Or:
                        return Emit(Op::kOr, lhs, rhs);
                    case llvm::Instruction::Xor:
                        return Emit(Op::kXor, lhs, rhs);
                    case llvm::Instruction::LShr:
                        return Emit(Op::kLShr, lhs, rhs);
                    case llvm::Instruction::Shl:
                        return Emit(Op::kShl, lhs, rhs);
                    default:
                        return kZeroSlot;
                }
            }

            static constexpr uint32_t kZeroSlot = 0;

            std::vector< Node > nodes_;
            std::vector< uint64_t > consts_;
            uint32_t root_ = kZeroSlot;
            uint64_t mask_ = UINT64_MAX;
        };

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

            // Arms recur across probes and across phis, so each one is
            // lowered at most once.
            llvm::DenseMap< llvm::Value *, TreeEvalPlan > arm_plans;
            auto plan_for = [&](llvm::Value *arm) -> const TreeEvalPlan & {
                auto [it, inserted] = arm_plans.try_emplace(arm);
                if (inserted) {
                    it->second = TreeEvalPlan::Build(
                        llvm::cast< llvm::Instruction >(arm), leaves, bitwidth, phi_redirects
                    );
                }
                return it->second;
            };

            // NOLINTNEXTLINE(cert-msc32-c,cert-msc51-cpp)
            std::mt19937_64 rng(0xC0B7A);
            std::vector< uint64_t > inputs(leaves.size());
            std::vector< uint64_t > slots;

            for (const auto &[phi_val, chosen] : phi_redirects) {
                auto *phi = llvm::cast< llvm::PHINode >(phi_val);

                for (uint32_t probe = 0; probe < kNumProbes; ++probe) {
                    for (uint64_t &input : inputs) {
                        input = rng() & mask;
                    }

                    // Evaluate chosen arm.
                    uint64_t chosen_val = 0;
                    if (auto *ci = llvm::dyn_cast< llvm::ConstantInt >(chosen)) {
                        chosen_val = ci->getZExtValue() & mask;
                    } else {
                        chosen_val =
                            plan_for(chosen).Evaluate(inputs.data(), inputs.size(), slots);
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
                            inc_val =
                                plan_for(inc).Evaluate(inputs.data(), inputs.size(), slots);
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

    std::vector< MBACandidate >
    DetectMbaCandidates(
        llvm::Function &f, uint32_t min_ast_size, uint32_t /*max_vars*/, uint64_t options_tag
    ) {
        std::vector< MBACandidate > candidates;
        llvm::DenseSet< llvm::Instruction * > already_in_tree;

        // Post-order: process uses before defs across blocks.
        // Within each block, reverse iteration hits outermost roots
        // first, so the largest MBA tree claims inner nodes before
        // they can be emitted as standalone candidates.
        for (auto *bb : post_order(&f)) {
            for (auto &inst : llvm::reverse(*bb)) {
                if (!IsMbaOpcode(inst.getOpcode())) {
                    continue;
                }
                if (already_in_tree.contains(&inst) != 0u) {
                    continue;
                }

                if (!inst.getType()->isIntegerTy()) {
                    continue;
                }
                const uint32_t bw = inst.getType()->getIntegerBitWidth();
                if (bw > 64) {
                    continue;
                }

                llvm::SmallVector< llvm::Instruction *, 16 > tree_insts;
                std::vector< llvm::Value * > leaves;
                llvm::DenseMap< llvm::Value *, llvm::Value * > phi_redirects;
                CollectTree(&inst, tree_insts, leaves, phi_redirects);

                if (tree_insts.size() < min_ast_size) {
                    continue;
                }

                constexpr uint32_t kPreElimCap = 20;
                if (leaves.size() > kPreElimCap) {
                    continue;
                }

                const llvm::DenseSet< llvm::Value * > tree_set(
                    tree_insts.begin(), tree_insts.end()
                );
                if (HasPolynomialMul(&inst, tree_set)) {
                    continue;
                }

                // Everything past this point — phi arm verification, the
                // 2^leaves signature sweep, the AST build and the solve in the
                // caller — is decided entirely by the tree just collected. If a
                // previous run already reached this exact tree there is nothing
                // new to learn, so claim its instructions and move on. The tree
                // is still claimed because the enclosing root must keep winning
                // over its inner nodes whether or not it gets re-examined.
                const MbaFingerprint fingerprint =
                    FingerprintTree(&inst, leaves, tree_set, phi_redirects);
                if (FingerprintUnchanged(inst, fingerprint, options_tag)) {
                    ++NumFingerprintSkips;
                    for (auto *ti : tree_insts) {
                        already_in_tree.insert(ti);
                    }
                    continue;
                }

                // Verify transparent phis — if any arm diverges,
                // re-collect without phi transparency.
                if (!phi_redirects.empty() && !VerifyPhiArms(phi_redirects, leaves, bw)) {
                    tree_insts.clear();
                    leaves.clear();
                    phi_redirects.clear();
                    CollectTree(
                        &inst, tree_insts, leaves, phi_redirects,
                        /*try_phi_transparency=*/false
                    );

                    if (tree_insts.size() < min_ast_size) {
                        continue;
                    }
                    if (leaves.size() > kPreElimCap) {
                        continue;
                    }
                }

                for (auto *ti : tree_insts) {
                    already_in_tree.insert(ti);
                }

                const auto num_vars  = static_cast< uint32_t >(leaves.size());
                const size_t sig_len = 1ULL << num_vars;
                std::vector< uint64_t > sig(sig_len);

                auto plan = std::make_shared< const TreeEvalPlan >(
                    TreeEvalPlan::Build(&inst, leaves, bw, phi_redirects)
                );

                std::vector< uint64_t > inputs(num_vars);
                std::vector< uint64_t > slots(plan->SlotCount());
                for (size_t i = 0; i < sig_len; ++i) {
                    for (uint32_t v = 0; v < num_vars; ++v) {
                        inputs[v] = (i >> v) & 1;
                    }
                    sig[i] = plan->Evaluate(inputs.data(), num_vars, slots);
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
                auto expr = BuildExprFromIR(&inst, leaves, tree_set, mask, phi_redirects);

                // Build evaluator lambda for full-width verification.  The
                // plan is shared rather than captured by value so that copying
                // the Evaluator stays cheap; the per-call slot buffer is
                // mutable state owned by the closure, matching the Evaluator
                // thread-safety contract.
                Evaluator evaluator;
                if (expr != nullptr) {
                    evaluator = [plan, scratch = std::vector< uint64_t >(plan->SlotCount())](
                                    const std::vector< uint64_t > &vals
                                ) mutable -> uint64_t {
                        return plan->Evaluate(vals.data(), vals.size(), scratch);
                    };
                }

                candidates.push_back(
                    MBACandidate{ .root        = &inst,
                                  .leaf_values = std::move(leaves),
                                  .var_names   = std::move(var_names),
                                  .sig         = std::move(sig),
                                  .bitwidth    = bw,
                                  .expr        = std::move(expr),
                                  .evaluator   = std::move(evaluator),
                                  .fingerprint = fingerprint }
                );
            }
        }

        return candidates;
    }

    void RecordMbaFingerprint(
        llvm::Instruction *inst, const MbaFingerprint &fp, uint64_t options_tag
    ) {
        if (inst == nullptr) { return; }

        auto &ctx      = inst->getContext();
        auto *i64_type = llvm::Type::getInt64Ty(ctx);
        auto *i32_type = llvm::Type::getInt32Ty(ctx);

        const auto u64 = [&](uint64_t v) -> llvm::Metadata * {
            return llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64_type, v));
        };
        const auto u32 = [&](uint32_t v) -> llvm::Metadata * {
            return llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i32_type, v));
        };

        inst->setMetadata(
            kFingerprintMD,
            llvm::MDNode::get(
                ctx, { u64(fp.structure), u32(fp.depth), u32(fp.num_vars), u64(options_tag) }
            )
        );
    }

    std::optional< MbaFingerprint > ComputeMbaFingerprint(llvm::Instruction *inst) {
        if (inst == nullptr || !IsMbaOpcode(inst->getOpcode())
            || !inst->getType()->isIntegerTy() || inst->getType()->getIntegerBitWidth() > 64)
        {
            return std::nullopt;
        }

        llvm::SmallVector< llvm::Instruction *, 16 > tree_insts;
        std::vector< llvm::Value * > leaves;
        llvm::DenseMap< llvm::Value *, llvm::Value * > phi_redirects;
        CollectTree(inst, tree_insts, leaves, phi_redirects);

        const llvm::DenseSet< llvm::Value * > tree_set(tree_insts.begin(), tree_insts.end());
        return FingerprintTree(inst, leaves, tree_set, phi_redirects);
    }

} // namespace cobra
