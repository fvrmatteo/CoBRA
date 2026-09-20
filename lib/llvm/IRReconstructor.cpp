#include "IRReconstructor.h"
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

    llvm::Value *ReconstructIr(
        const Expr &expr, const MBACandidate &candidate, llvm::IRBuilder<> &builder,
        const std::vector< uint32_t > &var_map, llvm::ArrayRef< llvm::Value * > leaf_override,
        IrReuse *reuse
    ) {
        auto *int_ty = builder.getIntNTy(candidate.bitwidth);
        const auto leaf_value = [&](uint32_t index) {
            return leaf_override.empty() ? candidate.leaf_values[index]
                                         : leaf_override[index];
        };
        const auto child = [&](unsigned index) {
            return ReconstructIr(
                *expr.children[index], candidate, builder, var_map, leaf_override, reuse
            );
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
                auto *lhs = child(0);
                auto *rhs = child(1);
                return EmitBinary(llvm::Instruction::Add, lhs, rhs, builder, "cobra.add", reuse);
            }
            case Expr::Kind::kMul: {
                auto *lhs = child(0);
                auto *rhs = child(1);
                return EmitBinary(llvm::Instruction::Mul, lhs, rhs, builder, "cobra.mul", reuse);
            }
            case Expr::Kind::kAnd: {
                auto *lhs = child(0);
                auto *rhs = child(1);
                return EmitBinary(llvm::Instruction::And, lhs, rhs, builder, "cobra.and", reuse);
            }
            case Expr::Kind::kOr: {
                auto *lhs = child(0);
                auto *rhs = child(1);
                return EmitBinary(llvm::Instruction::Or, lhs, rhs, builder, "cobra.or", reuse);
            }
            case Expr::Kind::kXor: {
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
                auto *operand = child(0);
                auto *zero    = llvm::ConstantInt::get(int_ty, 0);
                return EmitBinary(llvm::Instruction::Sub, zero, operand, builder, "cobra.neg", reuse);
            }
            case Expr::Kind::kShr: {
                auto *operand = child(0);
                auto *amount  = llvm::ConstantInt::get(int_ty, expr.constant_val);
                return EmitBinary(llvm::Instruction::LShr, operand, amount, builder, "cobra.shr", reuse);
            }
        }
        return nullptr; // unreachable
    }

} // namespace cobra
