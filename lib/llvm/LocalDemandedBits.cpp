#include "LocalDemandedBits.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/DemandedBits.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/Support/KnownBits.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>

namespace cobra {

    namespace {

        using namespace llvm;
        using namespace llvm::PatternMatch;

        // `isAlwaysLive` and `determineLiveOperandBits` below are LLVM's own,
        // from `lib/Analysis/DemandedBits.cpp` of LLVM 19.1.7 (Apache-2.0 WITH
        // LLVM-exception), copied because the second is a private member of
        // `DemandedBits`. They are the transfer function of the analysis this
        // file has to agree with bit for bit, so they are kept verbatim apart
        // from taking the assumption cache and the dominator tree as arguments.
        // A newer LLVM has to be diffed against them.

        bool isAlwaysLive(Instruction *I) {
            return I->isTerminator() || isa< DbgInfoIntrinsic >(I) || I->isEHPad()
                || I->mayHaveSideEffects();
        }

        void determineLiveOperandBits(
            AssumptionCache &AC, DominatorTree &DT, const Instruction *UserI, const Value *Val,
            unsigned OperandNo, const APInt &AOut, APInt &AB, KnownBits &Known,
            KnownBits &Known2, bool &KnownBitsComputed
        ) {
            unsigned BitWidth = AB.getBitWidth();

            // We're called once per operand, but for some instructions, we need to
            // compute known bits of both operands in order to determine the live bits of
            // either (when both operands are instructions themselves). We don't,
            // however, want to do this twice, so we cache the result in APInts that live
            // in the caller. For the two-relevant-operands case, both operand values are
            // provided here.
            auto ComputeKnownBits = [&](unsigned BitWidth, const Value *V1, const Value *V2) {
                if (KnownBitsComputed)
                    return;
                KnownBitsComputed = true;

                const DataLayout &DL = UserI->getDataLayout();
                Known                = KnownBits(BitWidth);
                computeKnownBits(V1, Known, DL, 0, &AC, UserI, &DT);

                if (V2) {
                    Known2 = KnownBits(BitWidth);
                    computeKnownBits(V2, Known2, DL, 0, &AC, UserI, &DT);
                }
            };

            switch (UserI->getOpcode()) {
                default:
                    break;
                case Instruction::Call:
                case Instruction::Invoke:
                    if (const auto *II = dyn_cast< IntrinsicInst >(UserI)) {
                        switch (II->getIntrinsicID()) {
                            default:
                                break;
                            case Intrinsic::bswap:
                                // The alive bits of the input are the swapped alive bits of
                                // the output.
                                AB = AOut.byteSwap();
                                break;
                            case Intrinsic::bitreverse:
                                // The alive bits of the input are the reversed alive bits of
                                // the output.
                                AB = AOut.reverseBits();
                                break;
                            case Intrinsic::ctlz:
                                if (OperandNo == 0) {
                                    // We need some output bits, so we need all bits of the
                                    // input to the left of, and including, the leftmost bit
                                    // known to be one.
                                    ComputeKnownBits(BitWidth, Val, nullptr);
                                    AB = APInt::getHighBitsSet(
                                        BitWidth,
                                        std::min(BitWidth, Known.countMaxLeadingZeros() + 1)
                                    );
                                }
                                break;
                            case Intrinsic::cttz:
                                if (OperandNo == 0) {
                                    // We need some output bits, so we need all bits of the
                                    // input to the right of, and including, the rightmost bit
                                    // known to be one.
                                    ComputeKnownBits(BitWidth, Val, nullptr);
                                    AB = APInt::getLowBitsSet(
                                        BitWidth,
                                        std::min(BitWidth, Known.countMaxTrailingZeros() + 1)
                                    );
                                }
                                break;
                            case Intrinsic::fshl:
                            case Intrinsic::fshr: {
                                const APInt *SA;
                                if (OperandNo == 2) {
                                    // Shift amount is modulo the bitwidth. For powers of two we have
                                    // SA % BW == SA & (BW - 1).
                                    if (isPowerOf2_32(BitWidth))
                                        AB = BitWidth - 1;
                                } else if (match(II->getOperand(2), m_APInt(SA))) {
                                    // Normalize to funnel shift left. APInt shifts of BitWidth are well-
                                    // defined, so no need to special-case zero shifts here.
                                    uint64_t ShiftAmt = SA->urem(BitWidth);
                                    if (II->getIntrinsicID() == Intrinsic::fshr)
                                        ShiftAmt = BitWidth - ShiftAmt;

                                    if (OperandNo == 0)
                                        AB = AOut.lshr(ShiftAmt);
                                    else if (OperandNo == 1)
                                        AB = AOut.shl(BitWidth - ShiftAmt);
                                }
                                break;
                            }
                            case Intrinsic::umax:
                            case Intrinsic::umin:
                            case Intrinsic::smax:
                            case Intrinsic::smin:
                                // If low bits of result are not demanded, they are also not demanded
                                // for the min/max operands.
                                AB = APInt::getBitsSetFrom(BitWidth, AOut.countr_zero());
                                break;
                        }
                    }
                    break;
                case Instruction::Add:
                    if (AOut.isMask()) {
                        AB = AOut;
                    } else {
                        ComputeKnownBits(BitWidth, UserI->getOperand(0), UserI->getOperand(1));
                        AB = DemandedBits::determineLiveOperandBitsAdd(OperandNo, AOut, Known, Known2);
                    }
                    break;
                case Instruction::Sub:
                    if (AOut.isMask()) {
                        AB = AOut;
                    } else {
                        ComputeKnownBits(BitWidth, UserI->getOperand(0), UserI->getOperand(1));
                        AB = DemandedBits::determineLiveOperandBitsSub(OperandNo, AOut, Known, Known2);
                    }
                    break;
                case Instruction::Mul:
                    // Find the highest live output bit. We don't need any more input
                    // bits than that (adds, and thus subtracts, ripple only to the
                    // left).
                    AB = APInt::getLowBitsSet(BitWidth, AOut.getActiveBits());
                    break;
                case Instruction::Shl:
                    if (OperandNo == 0) {
                        const APInt *ShiftAmtC;
                        if (match(UserI->getOperand(1), m_APInt(ShiftAmtC))) {
                            uint64_t ShiftAmt = ShiftAmtC->getLimitedValue(BitWidth - 1);
                            AB                = AOut.lshr(ShiftAmt);

                            // If the shift is nuw/nsw, then the high bits are not dead
                            // (because we've promised that they *must* be zero).
                            const auto *S = cast< ShlOperator >(UserI);
                            if (S->hasNoSignedWrap())
                                AB |= APInt::getHighBitsSet(BitWidth, ShiftAmt + 1);
                            else if (S->hasNoUnsignedWrap())
                                AB |= APInt::getHighBitsSet(BitWidth, ShiftAmt);
                        }
                    }
                    break;
                case Instruction::LShr:
                    if (OperandNo == 0) {
                        const APInt *ShiftAmtC;
                        if (match(UserI->getOperand(1), m_APInt(ShiftAmtC))) {
                            uint64_t ShiftAmt = ShiftAmtC->getLimitedValue(BitWidth - 1);
                            AB                = AOut.shl(ShiftAmt);

                            // If the shift is exact, then the low bits are not dead
                            // (they must be zero).
                            if (cast< LShrOperator >(UserI)->isExact())
                                AB |= APInt::getLowBitsSet(BitWidth, ShiftAmt);
                        }
                    }
                    break;
                case Instruction::AShr:
                    if (OperandNo == 0) {
                        const APInt *ShiftAmtC;
                        if (match(UserI->getOperand(1), m_APInt(ShiftAmtC))) {
                            uint64_t ShiftAmt = ShiftAmtC->getLimitedValue(BitWidth - 1);
                            AB                = AOut.shl(ShiftAmt);
                            // Because the high input bit is replicated into the
                            // high-order bits of the result, if we need any of those
                            // bits, then we must keep the highest input bit.
                            if ((AOut & APInt::getHighBitsSet(BitWidth, ShiftAmt)).getBoolValue())
                                AB.setSignBit();

                            // If the shift is exact, then the low bits are not dead
                            // (they must be zero).
                            if (cast< AShrOperator >(UserI)->isExact())
                                AB |= APInt::getLowBitsSet(BitWidth, ShiftAmt);
                        }
                    }
                    break;
                case Instruction::And:
                    AB = AOut;

                    // For bits that are known zero, the corresponding bits in the
                    // other operand are dead (unless they're both zero, in which
                    // case they can't both be dead, so just mark the LHS bits as
                    // dead).
                    ComputeKnownBits(BitWidth, UserI->getOperand(0), UserI->getOperand(1));
                    if (OperandNo == 0)
                        AB &= ~Known2.Zero;
                    else
                        AB &= ~(Known.Zero & ~Known2.Zero);
                    break;
                case Instruction::Or:
                    AB = AOut;

                    // For bits that are known one, the corresponding bits in the
                    // other operand are dead (unless they're both one, in which
                    // case they can't both be dead, so just mark the LHS bits as
                    // dead).
                    ComputeKnownBits(BitWidth, UserI->getOperand(0), UserI->getOperand(1));
                    if (OperandNo == 0)
                        AB &= ~Known2.One;
                    else
                        AB &= ~(Known.One & ~Known2.One);
                    break;
                case Instruction::Xor:
                case Instruction::PHI:
                    AB = AOut;
                    break;
                case Instruction::Trunc:
                    AB = AOut.zext(BitWidth);
                    break;
                case Instruction::ZExt:
                    AB = AOut.trunc(BitWidth);
                    break;
                case Instruction::SExt:
                    AB = AOut.trunc(BitWidth);
                    // Because the high input bit is replicated into the
                    // high-order bits of the result, if we need any of those
                    // bits, then we must keep the highest input bit.
                    if ((AOut
                         & APInt::getHighBitsSet(AOut.getBitWidth(), AOut.getBitWidth() - BitWidth))
                            .getBoolValue())
                        AB.setSignBit();
                    break;
                case Instruction::Select:
                    if (OperandNo != 0)
                        AB = AOut;
                    break;
                case Instruction::ExtractElement:
                    if (OperandNo == 0)
                        AB = AOut;
                    break;
                case Instruction::InsertElement:
                case Instruction::ShuffleVector:
                    if (OperandNo == 0 || OperandNo == 1)
                        AB = AOut;
                    break;
            }
        }

        // Where `inst` stands in the function, for putting the region's roots
        // in the order `DemandedBits::performAnalysis` meets them.
        bool InFunctionOrder(
            const Instruction *lhs, const Instruction *rhs,
            const DenseMap< const BasicBlock *, unsigned > &block_index
        ) {
            if (lhs->getParent() != rhs->getParent()) {
                return block_index.lookup(lhs->getParent()) < block_index.lookup(rhs->getParent());
            }
            return lhs->comesBefore(rhs);
        }

        bool IsInteger(const Instruction *inst) {
            return inst->getType()->isIntOrIntVectorTy();
        }

    } // namespace

    llvm::APInt LocalDemandedBits::Answer(llvm::Instruction *inst) const {
        const Settled &entry = settled_.find(inst)->second;
        if (entry.processed) {
            return entry.alive;
        }
        // `DemandedBits::getDemandedBits` of an instruction the analysis never
        // reached.
        return llvm::APInt::getAllOnes(
            inst->getDataLayout().getTypeSizeInBits(inst->getType()->getScalarType())
        );
    }

    std::optional< llvm::APInt > LocalDemandedBits::Get(llvm::Instruction *inst) {
        if (!settled_.contains(inst) && !Walk(inst) && !Fixpoint(inst)) {
            ++declined;
            return std::nullopt;
        }
        return Answer(inst);
    }

    llvm::APInt LocalDemandedBits::Contribution(llvm::Use &use, const Settled &reader) {
        using namespace llvm;
        auto *user          = cast< Instruction >(use.getUser());
        const unsigned width = use->getType()->getScalarSizeInBits();

        // The propagation step of `DemandedBits::performAnalysis` for one
        // operand of a reader it has visited.
        APInt alive_out;
        bool input_is_known_dead = false;
        if (IsInteger(user)) {
            alive_out           = reader.alive;
            input_is_known_dead = !alive_out && !isAlwaysLive(user);
        }
        if (input_is_known_dead) {
            return APInt(width, 0);
        }
        APInt alive      = APInt::getAllOnes(width);
        auto &operands   = known_[user];
        determineLiveOperandBits(
            assumptions_, dominators_, user, use.get(), use.getOperandNo(), alive_out, alive,
            operands.first, operands.second, operands.computed
        );
        return alive;
    }

    bool LocalDemandedBits::Walk(llvm::Instruction *inst) {
        using namespace llvm;

        // Depth first over readers. A frame is settled once every reader it
        // needs is; a reader still on the stack is a cycle.
        struct Frame
        {
            Instruction *node;
            Value::use_iterator next;
            Settled value;
        };
        SmallVector< Frame, 16 > stack;
        SmallPtrSet< Instruction *, 16 > open;

        const auto push = [&](Instruction *node) {
            Settled value;
            value.processed = isAlwaysLive(node);
            if (IsInteger(node)) {
                value.alive = APInt(node->getType()->getScalarSizeInBits(), 0);
            }
            stack.push_back(Frame{ node, node->use_begin(), std::move(value) });
            open.insert(node);
            ++work_this_epoch;
        };
        // Nothing more can change about `frame`: an integer every bit of which
        // is read, or anything else once it is live at all.
        const auto saturated = [](const Frame &frame) {
            return IsInteger(frame.node) ? frame.value.processed && frame.value.alive.isAllOnes()
                                         : frame.value.processed;
        };

        push(inst);
        while (!stack.empty()) {
            const size_t depth = stack.size() - 1;
            bool descended     = false;
            while (stack[depth].next != stack[depth].node->use_end() && !saturated(stack[depth])) {
                Use &use    = *stack[depth].next;
                auto *reader = dyn_cast< Instruction >(use.getUser());
                if (reader == nullptr) {
                    ++stack[depth].next;
                    continue;
                }
                auto found = settled_.find(reader);
                if (found == settled_.end()) {
                    if (open.contains(reader)) {
                        return false;
                    }
                    push(reader);
                    descended = true;
                    break;
                }
                ++stack[depth].next;
                if (!found->second.processed) {
                    continue;
                }
                stack[depth].value.processed = true;
                if (IsInteger(stack[depth].node)) {
                    // `Contribution` may grow `known_`, never `settled_`, so
                    // the reference is safe across it.
                    stack[depth].value.alive |= Contribution(use, found->second);
                }
            }
            if (descended) {
                continue;
            }
            Frame done = std::move(stack.back());
            stack.pop_back();
            open.erase(done.node);
            settled_[done.node] = std::move(done.value);
            ++settled;
        }
        return true;
    }

    bool LocalDemandedBits::Fixpoint(llvm::Instruction *inst) {
        using namespace llvm;

        // The instructions `inst` reaches through readers that are not settled
        // yet. A settled instruction has all of its readers settled too - it
        // could not have been settled otherwise - so the region is closed under
        // readers once the settled ones are taken as given.
        SmallVector< Instruction *, 32 > region{ inst };
        SmallPtrSet< Instruction *, 32 > in_region;
        SmallVector< Instruction *, 16 > boundary;
        SmallPtrSet< Instruction *, 16 > on_boundary;
        in_region.insert(inst);
        for (size_t next = 0; next < region.size(); ++next) {
            for (User *user : region[next]->users()) {
                auto *reader = dyn_cast< Instruction >(user);
                if (reader == nullptr) {
                    continue;
                }
                if (settled_.contains(reader)) {
                    if (on_boundary.insert(reader).second) {
                        boundary.push_back(reader);
                    }
                    continue;
                }
                if (!in_region.insert(reader).second) {
                    continue;
                }
                if (region.size() >= max_region_) {
                    return false;
                }
                region.push_back(reader);
            }
        }
        ++regions;
        region_nodes += region.size();
        work_this_epoch += region.size();

        // `DemandedBits::performAnalysis`, over the region, with the settled
        // readers on its boundary entering as the visited users they are. The
        // roots are taken in function order and the propagation is the same
        // last-in-first-out walk; the result does not depend on the order,
        // since each operand's alive bits only ever grow and the transfer
        // function is monotone in the bits its user needs, but there is no
        // reason to differ.
        SmallVector< Instruction *, 16 > roots;
        for (Instruction *member : region) {
            if (isAlwaysLive(member)) {
                roots.push_back(member);
            }
        }
        if (roots.size() > 1) {
            DenseMap< const BasicBlock *, unsigned > block_index;
            unsigned index = 0;
            for (const BasicBlock &block : *inst->getFunction()) {
                block_index[&block] = index++;
            }
            std::stable_sort(roots.begin(), roots.end(), [&](Instruction *lhs, Instruction *rhs) {
                return InFunctionOrder(lhs, rhs, block_index);
            });
        }

        DenseMap< Instruction *, APInt > alive_bits;
        SmallPtrSet< Instruction *, 16 > visited;
        SmallSetVector< Instruction *, 16 > worklist;

        for (Instruction *reader : boundary) {
            const Settled &known = settled_.find(reader)->second;
            if (!known.processed) {
                continue;
            }
            if (IsInteger(reader)) {
                alive_bits[reader] = known.alive;
            } else {
                visited.insert(reader);
            }
            worklist.insert(reader);
        }

        for (Instruction *root : roots) {
            Type *type = root->getType();
            if (type->isIntOrIntVectorTy()) {
                if (alive_bits.try_emplace(root, type->getScalarSizeInBits(), 0).second) {
                    worklist.insert(root);
                }
                continue;
            }
            for (Use &operand : root->operands()) {
                auto *operand_inst = dyn_cast< Instruction >(operand);
                if (operand_inst == nullptr || !in_region.contains(operand_inst)) {
                    continue;
                }
                Type *operand_type = operand_inst->getType();
                if (operand_type->isIntOrIntVectorTy()) {
                    alive_bits[operand_inst] =
                        APInt::getAllOnes(operand_type->getScalarSizeInBits());
                } else {
                    visited.insert(operand_inst);
                }
                worklist.insert(operand_inst);
            }
        }

        while (!worklist.empty()) {
            Instruction *user = worklist.pop_back_val();

            APInt alive_out;
            bool input_is_known_dead = false;
            if (user->getType()->isIntOrIntVectorTy()) {
                alive_out           = alive_bits[user];
                input_is_known_dead = !alive_out && !isAlwaysLive(user);
            }

            auto &operands = known_[user];
            for (Use &operand : user->operands()) {
                // Only the region's instructions are being decided here; the
                // others either are settled or do not read anything the
                // region holds.
                auto *operand_inst = dyn_cast< Instruction >(operand);
                if (operand_inst == nullptr || !in_region.contains(operand_inst)) {
                    continue;
                }

                Type *type = operand->getType();
                if (type->isIntOrIntVectorTy()) {
                    const unsigned width = type->getScalarSizeInBits();
                    APInt alive          = APInt::getAllOnes(width);
                    if (input_is_known_dead) {
                        alive = APInt(width, 0);
                    } else {
                        determineLiveOperandBits(
                            assumptions_, dominators_, user, operand, operand.getOperandNo(),
                            alive_out, alive, operands.first, operands.second, operands.computed
                        );
                    }
                    auto result = alive_bits.try_emplace(operand_inst);
                    if (result.second || (alive |= result.first->second) != result.first->second)
                    {
                        result.first->second = std::move(alive);
                        worklist.insert(operand_inst);
                    }
                } else if (visited.insert(operand_inst).second) {
                    worklist.insert(operand_inst);
                }
            }
        }

        for (Instruction *member : region) {
            Settled value;
            if (IsInteger(member)) {
                auto found      = alive_bits.find(member);
                value.processed = found != alive_bits.end();
                value.alive     = value.processed
                        ? found->second
                        : APInt(member->getType()->getScalarSizeInBits(), 0);
            } else {
                value.processed = visited.contains(member) || isAlwaysLive(member);
            }
            settled_[member] = std::move(value);
            ++settled;
        }
        return true;
    }

} // namespace cobra
