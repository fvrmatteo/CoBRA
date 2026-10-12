#include "IRReconstructor.h"
#include "cobra/core/ExprUtils.h"
#include "MBADetector.h"
#include "cobra/core/Expr.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace cobra {

    namespace {

        // How many readers of the operands a lookup walks before it gives up
        // and builds the operator. A value read all over a large function has
        // a use list to match, and the instruction wanted is one the rewrite
        // can always afford to build.
        constexpr unsigned kMaxReadersScanned = 256;

        bool MayRead(const llvm::Instruction &inst, const IrReuse &reuse) {
            if (&inst == reuse.before || inst.getParent() == nullptr) {
                return false;
            }
            if (reuse.replaced != nullptr && reuse.replaced->contains(&inst)) {
                return false;
            }
            if (inst.hasPoisonGeneratingAnnotations()
                && (reuse.poisons_root == nullptr || !reuse.poisons_root->contains(&inst)))
            {
                return false;
            }
            return reuse.created.contains(&inst)
                || reuse.dominators->dominates(&inst, reuse.before);
        }

        // The instruction, if the function holds one the rewrite may read,
        // that `matches` among the readers of `lhs` and `rhs`. Whatever
        // computes an operator reads each of its operands, so the shorter of
        // the two use lists holds it, and walking them in step finds the end of
        // the shorter one without measuring either. A constant is read from
        // everywhere in the module and its use list is never walked.
        llvm::Instruction *FindReader(
            llvm::Value *lhs, llvm::Value *rhs, IrReuse &reuse,
            llvm::function_ref< bool(const llvm::Instruction &) > matches
        ) {
            llvm::Value *first  = llvm::isa< llvm::Constant >(lhs) ? nullptr : lhs;
            llvm::Value *second = rhs == nullptr || llvm::isa< llvm::Constant >(rhs) ? nullptr : rhs;
            if (first == nullptr) {
                std::swap(first, second);
            }
            if (first == nullptr) {
                return nullptr;
            }
            const auto accept = [&](llvm::User *user) -> llvm::Instruction * {
                auto *inst = llvm::dyn_cast< llvm::Instruction >(user);
                if (inst == nullptr || !matches(*inst) || !MayRead(*inst, reuse)) {
                    return nullptr;
                }
                return inst;
            };
            auto first_it  = first->user_begin();
            auto second_it = second != nullptr ? second->user_begin() : first->user_end();
            for (unsigned scanned = 0; scanned < kMaxReadersScanned; ++scanned) {
                if (first_it == first->user_end()) {
                    return nullptr;
                }
                if (auto *found = accept(*first_it++)) {
                    return found;
                }
                if (second == nullptr) {
                    continue;
                }
                if (second_it == second->user_end()) {
                    return nullptr;
                }
                if (auto *found = accept(*second_it++)) {
                    return found;
                }
            }
            return nullptr;
        }

        llvm::Value *Read(llvm::Instruction *found, IrReuse &reuse) {
            ++reuse.found;
            if (!reuse.created.contains(found) && reuse.seen.insert(found).second) {
                reuse.reused.push_back(found);
            }
            return found;
        }

        llvm::Value *Built(llvm::Value *value, IrReuse *reuse) {
            if (reuse != nullptr) {
                if (const auto *inst = llvm::dyn_cast< llvm::Instruction >(value)) {
                    reuse->created.insert(inst);
                }
            }
            return value;
        }

        llvm::Value *EmitBinary(
            llvm::Instruction::BinaryOps opcode, llvm::Value *lhs, llvm::Value *rhs,
            llvm::IRBuilder<> &builder, const char *name, IrReuse *reuse
        ) {
            if (reuse != nullptr) {
                const bool commutative = llvm::Instruction::isCommutative(opcode);
                auto *found = FindReader(lhs, rhs, *reuse, [&](const llvm::Instruction &inst) {
                    if (inst.getOpcode() != opcode) {
                        return false;
                    }
                    return (inst.getOperand(0) == lhs && inst.getOperand(1) == rhs)
                        || (commutative && inst.getOperand(0) == rhs && inst.getOperand(1) == lhs);
                });
                if (found != nullptr) {
                    return Read(found, *reuse);
                }
            }
            return Built(builder.CreateBinOp(opcode, lhs, rhs, name), reuse);
        }

        llvm::Value *EmitCast(
            llvm::Instruction::CastOps opcode, llvm::Value *value, llvm::Type *type,
            llvm::IRBuilder<> &builder, const char *name, IrReuse *reuse
        ) {
            if (value->getType() == type) {
                return value;
            }
            if (reuse != nullptr) {
                auto *found = FindReader(value, nullptr, *reuse, [&](const llvm::Instruction &inst) {
                    return inst.getOpcode() == opcode && inst.getType() == type
                        && inst.getOperand(0) == value;
                });
                if (found != nullptr) {
                    return Read(found, *reuse);
                }
            }
            return Built(builder.CreateCast(opcode, value, type, name), reuse);
        }

        llvm::Value *EmitCompare(
            llvm::CmpInst::Predicate predicate, llvm::Value *lhs, llvm::Value *rhs,
            llvm::IRBuilder<> &builder, const char *name, IrReuse *reuse
        ) {
            if (reuse != nullptr) {
                const auto swapped = llvm::CmpInst::getSwappedPredicate(predicate);
                auto *found = FindReader(lhs, rhs, *reuse, [&](const llvm::Instruction &inst) {
                    const auto *cmp = llvm::dyn_cast< llvm::ICmpInst >(&inst);
                    if (cmp == nullptr) {
                        return false;
                    }
                    return (cmp->getPredicate() == predicate && cmp->getOperand(0) == lhs
                            && cmp->getOperand(1) == rhs)
                        || (cmp->getPredicate() == swapped && cmp->getOperand(0) == rhs
                            && cmp->getOperand(1) == lhs);
                });
                if (found != nullptr) {
                    return Read(found, *reuse);
                }
            }
            return Built(builder.CreateICmp(predicate, lhs, rhs, name), reuse);
        }

        llvm::Value *EmitSelect(
            llvm::Value *cond, llvm::Value *then_value, llvm::Value *else_value,
            llvm::IRBuilder<> &builder, const char *name, IrReuse *reuse
        ) {
            if (reuse != nullptr) {
                auto *found = FindReader(then_value, else_value, *reuse, [&](const llvm::Instruction &inst) {
                    const auto *select = llvm::dyn_cast< llvm::SelectInst >(&inst);
                    return select != nullptr && select->getCondition() == cond
                        && select->getTrueValue() == then_value && select->getFalseValue() == else_value;
                });
                if (found != nullptr) {
                    return Read(found, *reuse);
                }
            }
            return Built(builder.CreateSelect(cond, then_value, else_value, name), reuse);
        }

        llvm::Value *MaterializeAtCandidateWidth(
            llvm::Value *value, llvm::IntegerType *target_ty, llvm::IRBuilder<> &builder,
            IrReuse *reuse
        ) {
            auto *value_ty = llvm::dyn_cast< llvm::IntegerType >(value->getType());
            if (value_ty == nullptr || value_ty == target_ty) {
                return value;
            }
            const auto opcode = value_ty->getBitWidth() < target_ty->getBitWidth()
                ? llvm::Instruction::ZExt
                : llvm::Instruction::Trunc;
            return EmitCast(opcode, value, target_ty, builder, "cobra.cast", reuse);
        }

    } // namespace

    namespace {
        // Under `condition_forms`, each structurally distinct sub-expression of the
        // rewrite is built once and read wherever it repeats. The reading of a
        // `select` spells its else arm twice, and a condition put back over the
        // values it was computed from repeats what they share; built once, a leaf
        // beneath them is read once, which is what lets the caller count its reads
        // over distinct sub-expressions when it decides what to freeze
        // (`LeafReadsOverDistinctSubtrees`) - without depending on `IrReuse`
        // finding the copy among a leaf's first readers.
        struct SubtreeMemo
        {
            std::unordered_multimap< size_t, std::pair< const Expr *, llvm::Value * > > built;
        };

        llvm::Value *ReconstructNode(
            const Expr &expr, const MBACandidate &candidate, llvm::IRBuilder<> &builder,
            const std::vector< uint32_t > &var_map, LeafSupply *leaves, IrReuse *reuse,
            bool condition_forms, SubtreeMemo *memo
        );

        llvm::Value *Reconstruct(
            const Expr &expr, const MBACandidate &candidate, llvm::IRBuilder<> &builder,
            const std::vector< uint32_t > &var_map, LeafSupply *leaves, IrReuse *reuse,
            bool condition_forms, SubtreeMemo *memo
        ) {
            if (memo == nullptr || expr.kind == Expr::Kind::kConstant) {
                return ReconstructNode(expr, candidate, builder, var_map, leaves, reuse, condition_forms, memo);
            }
            const size_t hash = std::hash< Expr >{}(expr);
            const auto range  = memo->built.equal_range(hash);
            for (auto it = range.first; it != range.second; ++it) {
                if (ExprStructurallyEqual(*it->second.first, expr)) {
                    return it->second.second;
                }
            }
            auto *value = ReconstructNode(expr, candidate, builder, var_map, leaves, reuse, condition_forms, memo);
            memo->built.emplace(hash, std::make_pair(&expr, value));
            return value;
        }
    } // namespace

    llvm::Value *ReconstructIr(
        const Expr &expr, const MBACandidate &candidate, llvm::IRBuilder<> &builder,
        const std::vector< uint32_t > &var_map, LeafSupply *leaves, IrReuse *reuse,
        bool condition_forms
    ) {
        SubtreeMemo memo;
        return Reconstruct(
            expr, candidate, builder, var_map, leaves, reuse, condition_forms,
            condition_forms ? &memo : nullptr
        );
    }

    namespace {
    llvm::Value *ReconstructNode(
        const Expr &expr, const MBACandidate &candidate, llvm::IRBuilder<> &builder,
        const std::vector< uint32_t > &var_map, LeafSupply *leaves, IrReuse *reuse,
        bool condition_forms, SubtreeMemo *memo
    ) {
        auto *int_ty = builder.getIntNTy(candidate.bitwidth);
        const auto leaf_value = [&](uint32_t index) -> llvm::Value * {
            if (leaves == nullptr) {
                return candidate.leaf_values[index];
            }
            if (leaves->freeze_on_use[index]) {
                auto *frozen = builder.CreateFreeze(leaves->values[index], "cobra.freeze");
                if (reuse != nullptr) {
                    if (auto *inst = llvm::dyn_cast< llvm::Instruction >(frozen)) {
                        reuse->created.insert(inst);
                    }
                }
                leaves->values[index]        = frozen;
                leaves->freeze_on_use[index] = false;
            }
            return leaves->values[index];
        };
        const auto child = [&](unsigned index) {
            return Reconstruct(
                *expr.children[index], candidate, builder, var_map, leaves, reuse, condition_forms, memo
            );
        };
        const auto build = [&](const Expr &node) {
            return Reconstruct(node, candidate, builder, var_map, leaves, reuse, condition_forms, memo);
        };
        // A comparison as the `i1` it is, its predicate inverted when `^ 1`
        // flips it; null for anything else.
        const auto condition = [&](const Expr &node) -> llvm::Value * {
            const Expr *cmp = &node;
            bool inverted   = false;
            if (node.kind == Expr::Kind::kXor && node.children.size() == 2) {
                for (unsigned side = 0; side < 2; ++side) {
                    const auto &other = *node.children[1 - side];
                    if (IsComparison(node.children[side]->kind) && other.kind == Expr::Kind::kConstant
                        && other.constant_val == 1)
                    {
                        cmp      = node.children[side].get();
                        inverted = true;
                    }
                }
            }
            if (!IsComparison(cmp->kind)) {
                return nullptr;
            }
            auto predicate = cmp->kind == Expr::Kind::kCmpEq ? llvm::CmpInst::ICMP_EQ
                : cmp->kind == Expr::Kind::kCmpUlt           ? llvm::CmpInst::ICMP_ULT
                                                             : llvm::CmpInst::ICMP_SLT;
            if (inverted) {
                predicate = llvm::CmpInst::getInversePredicate(predicate);
            }
            auto *lhs = build(*cmp->children[0]);
            auto *rhs = build(*cmp->children[1]);
            return EmitCompare(predicate, lhs, rhs, builder, "cobra.cond", reuse);
        };

        switch (expr.kind) {
            case Expr::Kind::kConstant:
                return llvm::ConstantInt::get(int_ty, expr.constant_val);

            case Expr::Kind::kVariable: {
                uint32_t leaf_idx = expr.var_index;
                if (!var_map.empty()) {
                    leaf_idx = var_map[expr.var_index];
                }
                return MaterializeAtCandidateWidth(leaf_value(leaf_idx), int_ty, builder, reuse);
            }

            case Expr::Kind::kAdd: {
                // `x + -y` is `x - y`, the way the IR writes it (and a
                // constant minus a value with it).
                if (condition_forms) {
                    for (unsigned side = 0; side < 2; ++side) {
                        if (expr.children[side]->kind == Expr::Kind::kNeg) {
                            auto *minuend    = build(*expr.children[1 - side]);
                            auto *subtrahend = build(*expr.children[side]->children[0]);
                            return EmitBinary(
                                llvm::Instruction::Sub, minuend, subtrahend, builder, "cobra.sub", reuse
                            );
                        }
                    }
                }
                auto *lhs = child(0);
                auto *rhs = child(1);
                return EmitBinary(llvm::Instruction::Add, lhs, rhs, builder, "cobra.add", reuse);
            }
            case Expr::Kind::kMul: {
                auto *lhs = child(0);
                auto *rhs = child(1);
                return EmitBinary(llvm::Instruction::Mul, lhs, rhs, builder, "cobra.mul", reuse);
            }
            // A mask on a leaf that only says what the leaf's known bits already
            // say is the tree's own reading of the leaf (`LeafBits`), not part of
            // the answer: emitted, it would be an instruction InstCombine removes
            // and the rewrite priced one higher than it is.
            case Expr::Kind::kAnd:
            case Expr::Kind::kOr: {
                const auto implied = [&](unsigned variable, unsigned constant) -> llvm::Value * {
                    const auto &leaf = *expr.children[variable];
                    const auto &mask = *expr.children[constant];
                    if (leaf.kind != Expr::Kind::kVariable || mask.kind != Expr::Kind::kConstant) {
                        return nullptr;
                    }
                    uint32_t leaf_idx = leaf.var_index;
                    if (!var_map.empty()) {
                        leaf_idx = var_map[leaf.var_index];
                    }
                    if (leaf_idx >= candidate.leaf_bits.size()) {
                        return nullptr;
                    }
                    const auto &bits     = candidate.leaf_bits[leaf_idx];
                    const uint64_t width = candidate.bitwidth >= 64 ? ~uint64_t{ 0 }
                                                                    : (uint64_t{ 1 } << candidate.bitwidth) - 1;
                    // The bits above a narrow leaf's own width are known zero
                    // too - `NarrowWidth` masks them wherever the leaf is read,
                    // which is why `LeafBitsOf` leaves them out as "not news" -
                    // so a mask that only keeps those bits clear says nothing
                    // either, and emitting it would price the answer one
                    // instruction higher than it is.
                    uint64_t zero = bits.zero;
                    if (leaf_idx < candidate.leaf_widths.size()) {
                        if (const auto own = candidate.leaf_widths[leaf_idx]; own != 0) {
                            zero |= ~((own >= 64 ? ~uint64_t{ 0 } : (uint64_t{ 1 } << own) - 1)) & width;
                        }
                    }
                    const bool redundant = expr.kind == Expr::Kind::kAnd
                        ? ((~zero & width) & ~mask.constant_val) == 0
                        : (mask.constant_val & width & ~bits.one) == 0;
                    return redundant ? child(variable) : nullptr;
                };
                if (auto *leaf = implied(0, 1)) {
                    return leaf;
                }
                if (auto *leaf = implied(1, 0)) {
                    return leaf;
                }
                auto *lhs = child(0);
                auto *rhs = child(1);
                return expr.kind == Expr::Kind::kAnd
                    ? EmitBinary(llvm::Instruction::And, lhs, rhs, builder, "cobra.and", reuse)
                    : EmitBinary(llvm::Instruction::Or, lhs, rhs, builder, "cobra.or", reuse);
            }
            case Expr::Kind::kXor: {
                // `B ^ ((A ^ B) & -c)` is how a `select` of a comparison is read
                // (`BuildExprFromIRImpl`): built as the `select` it was.
                if (condition_forms) {
                    for (unsigned side = 0; side < 2; ++side) {
                        const auto &other  = *expr.children[side];
                        const auto &masked = *expr.children[1 - side];
                        if (masked.kind != Expr::Kind::kAnd) {
                            continue;
                        }
                        for (unsigned mside = 0; mside < 2; ++mside) {
                            const auto &diff = *masked.children[mside];
                            const auto &neg  = *masked.children[1 - mside];
                            if (diff.kind != Expr::Kind::kXor || neg.kind != Expr::Kind::kNeg) {
                                continue;
                            }
                            for (unsigned dside = 0; dside < 2; ++dside) {
                                if (!ExprStructurallyEqual(*diff.children[1 - dside], other)) {
                                    continue;
                                }
                                auto *cond = condition(*neg.children[0]);
                                if (cond == nullptr) {
                                    continue;
                                }
                                auto *then_value = build(*diff.children[dside]);
                                auto *else_value = build(other);
                                return EmitSelect(cond, then_value, else_value, builder, "cobra.select", reuse);
                            }
                        }
                    }
                    if (auto *flipped = condition(expr); flipped != nullptr) {
                        return EmitCast(llvm::Instruction::ZExt, flipped, int_ty, builder, "cobra.condext", reuse);
                    }
                }
                auto *lhs = child(0);
                auto *rhs = child(1);
                return EmitBinary(llvm::Instruction::Xor, lhs, rhs, builder, "cobra.xor", reuse);
            }
            // A comparison yields 0 or 1, so the i1 is widened back to the
            // candidate width to keep every node in the tree the same type.
            case Expr::Kind::kCmpEq:
            case Expr::Kind::kCmpUlt:
            case Expr::Kind::kCmpSlt: {
                auto *lhs = child(0);
                auto *rhs = child(1);
                llvm::Value *cmp = nullptr;
                if (expr.kind == Expr::Kind::kCmpEq) {
                    cmp = EmitCompare(llvm::CmpInst::ICMP_EQ, lhs, rhs, builder, "cobra.cmpeq", reuse);
                } else if (expr.kind == Expr::Kind::kCmpUlt) {
                    cmp = EmitCompare(llvm::CmpInst::ICMP_ULT, lhs, rhs, builder, "cobra.cmpult", reuse);
                } else {
                    cmp = EmitCompare(llvm::CmpInst::ICMP_SLT, lhs, rhs, builder, "cobra.cmpslt", reuse);
                }
                return EmitCast(llvm::Instruction::ZExt, cmp, int_ty, builder, "cobra.cmpext", reuse);
            }
            case Expr::Kind::kNot: {
                auto *operand = child(0);
                auto *neg_one = llvm::ConstantInt::getAllOnesValue(int_ty);
                return EmitBinary(llvm::Instruction::Xor, operand, neg_one, builder, "cobra.not", reuse);
            }
            case Expr::Kind::kNeg: {
                if (condition_forms) {
                    const auto &negated = *expr.children[0];
                    if (negated.kind == Expr::Kind::kShr
                        && negated.constant_val + 1 == int_ty->getBitWidth())
                    {
                        auto *value  = build(*negated.children[0]);
                        auto *amount = llvm::ConstantInt::get(int_ty, negated.constant_val);
                        return EmitBinary(llvm::Instruction::AShr, value, amount, builder, "cobra.sign", reuse);
                    }
                    if (auto *cmp = condition(negated); cmp != nullptr) {
                        return EmitCast(llvm::Instruction::SExt, cmp, int_ty, builder, "cobra.condsext", reuse);
                    }
                }
                auto *operand = child(0);
                auto *zero    = llvm::ConstantInt::get(int_ty, 0);
                return EmitBinary(llvm::Instruction::Sub, zero, operand, builder, "cobra.neg", reuse);
            }
            case Expr::Kind::kShr: {
                auto *operand = child(0);
                // A shift by the width or more is 0 everywhere CoBRA reasons
                // about it (`ModShr`, the solver, the ring normal form); `lshr`
                // would make it poison instead, or wrap a huge amount around.
                if (expr.constant_val >= int_ty->getBitWidth()) {
                    return llvm::ConstantInt::get(int_ty, 0);
                }
                auto *amount  = llvm::ConstantInt::get(int_ty, expr.constant_val);
                return EmitBinary(llvm::Instruction::LShr, operand, amount, builder, "cobra.shr", reuse);
            }
        }
        return nullptr; // unreachable
    }
    } // namespace

} // namespace cobra
