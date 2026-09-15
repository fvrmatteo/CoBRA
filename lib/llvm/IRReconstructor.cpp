#include "IRReconstructor.h"
#include "MBADetector.h"
#include "cobra/core/Expr.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Value.h"

#include <cstdint>
#include <vector>

namespace cobra {

    namespace {

        llvm::Value *MaterializeAtCandidateWidth(
            llvm::Value *value, llvm::IntegerType *target_ty, llvm::IRBuilder<> &builder
        ) {
            auto *value_ty = llvm::dyn_cast< llvm::IntegerType >(value->getType());
            if (value_ty == nullptr || value_ty == target_ty) {
                return value;
            }
            return builder.CreateZExtOrTrunc(value, target_ty, "cobra.cast");
        }

    } // namespace

    llvm::Value *ReconstructIr(
        const Expr &expr, const MBACandidate &candidate, llvm::IRBuilder<> &builder,
        const std::vector< uint32_t > &var_map, llvm::ArrayRef< llvm::Value * > leaf_override
    ) {
        auto *int_ty = builder.getIntNTy(candidate.bitwidth);
        const auto leaf_value = [&](uint32_t index) {
            return leaf_override.empty() ? candidate.leaf_values[index]
                                         : leaf_override[index];
        };

        switch (expr.kind) {
            case Expr::Kind::kConstant:
                return llvm::ConstantInt::get(int_ty, expr.constant_val);

            case Expr::Kind::kVariable: {
                uint32_t leaf_idx = expr.var_index;
                if (!var_map.empty()) {
                    leaf_idx = var_map[expr.var_index];
                }
                return MaterializeAtCandidateWidth(leaf_value(leaf_idx), int_ty, builder);
            }

            case Expr::Kind::kAdd: {
                auto *lhs = ReconstructIr(*expr.children[0], candidate, builder, var_map, leaf_override);
                auto *rhs = ReconstructIr(*expr.children[1], candidate, builder, var_map, leaf_override);
                return builder.CreateAdd(lhs, rhs, "cobra.add");
            }
            case Expr::Kind::kMul: {
                auto *lhs = ReconstructIr(*expr.children[0], candidate, builder, var_map, leaf_override);
                auto *rhs = ReconstructIr(*expr.children[1], candidate, builder, var_map, leaf_override);
                return builder.CreateMul(lhs, rhs, "cobra.mul");
            }
            case Expr::Kind::kAnd: {
                auto *lhs = ReconstructIr(*expr.children[0], candidate, builder, var_map, leaf_override);
                auto *rhs = ReconstructIr(*expr.children[1], candidate, builder, var_map, leaf_override);
                return builder.CreateAnd(lhs, rhs, "cobra.and");
            }
            case Expr::Kind::kOr: {
                auto *lhs = ReconstructIr(*expr.children[0], candidate, builder, var_map, leaf_override);
                auto *rhs = ReconstructIr(*expr.children[1], candidate, builder, var_map, leaf_override);
                return builder.CreateOr(lhs, rhs, "cobra.or");
            }
            case Expr::Kind::kXor: {
                auto *lhs = ReconstructIr(*expr.children[0], candidate, builder, var_map, leaf_override);
                auto *rhs = ReconstructIr(*expr.children[1], candidate, builder, var_map, leaf_override);
                return builder.CreateXor(lhs, rhs, "cobra.xor");
            }
            // A comparison yields 0 or 1, so the i1 is widened back to the
            // candidate width to keep every node in the tree the same type.
            case Expr::Kind::kCmpEq:
            case Expr::Kind::kCmpUlt:
            case Expr::Kind::kCmpSlt: {
                auto *lhs = ReconstructIr(*expr.children[0], candidate, builder, var_map, leaf_override);
                auto *rhs = ReconstructIr(*expr.children[1], candidate, builder, var_map, leaf_override);
                llvm::Value *cmp = nullptr;
                if (expr.kind == Expr::Kind::kCmpEq) {
                    cmp = builder.CreateICmpEQ(lhs, rhs, "cobra.cmpeq");
                } else if (expr.kind == Expr::Kind::kCmpUlt) {
                    cmp = builder.CreateICmpULT(lhs, rhs, "cobra.cmpult");
                } else {
                    cmp = builder.CreateICmpSLT(lhs, rhs, "cobra.cmpslt");
                }
                return builder.CreateZExt(cmp, int_ty, "cobra.cmpext");
            }
            case Expr::Kind::kNot: {
                auto *operand = ReconstructIr(*expr.children[0], candidate, builder, var_map, leaf_override);
                auto *neg_one = llvm::ConstantInt::getAllOnesValue(int_ty);
                return builder.CreateXor(operand, neg_one, "cobra.not");
            }
            case Expr::Kind::kNeg: {
                auto *operand = ReconstructIr(*expr.children[0], candidate, builder, var_map, leaf_override);
                return builder.CreateNeg(operand, "cobra.neg");
            }
            case Expr::Kind::kShr: {
                auto *operand = ReconstructIr(*expr.children[0], candidate, builder, var_map, leaf_override);
                auto *amount  = llvm::ConstantInt::get(int_ty, expr.constant_val);
                return builder.CreateLShr(operand, amount, "cobra.shr");
            }
        }
        return nullptr; // unreachable
    }

} // namespace cobra
