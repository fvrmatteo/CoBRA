#include "MBADetector.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprTraversal.h"
#include "cobra/core/Simplifier.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/KnownBits.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/Casting.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <numeric>
#include <optional>
#include <cstdlib>
#include <queue>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#define DEBUG_TYPE "cobra"

STATISTIC(NumFingerprintSkips, "Number of MBA trees skipped (unchanged since last examined)");

namespace cobra {

    // `ptradd`: a byte `getelementptr` with one index.
    //
    // Its value is the pointer it walks from plus that index, so over the
    // indices alone it is an addition - and that is how the detector reads
    // it. The base is left out of the expression entirely, which keeps the
    // whole tree in integers and makes the rewrite a `getelementptr` again
    // rather than a cast: the value of `gep(base, simplified)` is `base +
    // simplified`, and `simplified` is what was proved equal to the sum of
    // the indices.
    //
    // Without this a chain cuts the sum it spells in half - `gep(gep(p, a),
    // b)` hides `a + b` - and any identity the obfuscator wrote across it is
    // invisible: no candidate is ever built for the sum, and the IR
    // afterwards cannot be told from one that was examined and left alone.
    //
    // The target computes addresses at the DataLayout's index width, and
    // truncates a wider index to it - which the sum of the indices at their
    // own width survives, reduction being a homomorphism. A narrower index is
    // sign-extended first, which the sum does not say, and such a
    // `getelementptr` is not read this way.
    const llvm::GetElementPtrInst *ByteGepOf(const llvm::Value *v) {
        const auto *gep = llvm::dyn_cast_or_null< llvm::GetElementPtrInst >(v);
        if (gep == nullptr || gep->getNumIndices() != 1
            || !gep->getSourceElementType()->isIntegerTy(8) || gep->getModule() == nullptr)
        {
            return nullptr;
        }
        const auto *index = gep->getOperand(1)->getType();
        if (!index->isIntegerTy() || index->getIntegerBitWidth() > 64
            || index->getIntegerBitWidth()
                < gep->getModule()->getDataLayout().getIndexTypeSizeInBits(gep->getType()))
        {
            return nullptr;
        }
        return gep;
    }

    // The pointer a chain of `ptradd`s walks from: what the rewrite adds the
    // simplified sum of the indices back to.
    llvm::Value *ByteGepBase(const llvm::GetElementPtrInst *gep) {
        const llvm::Value *base = gep->getPointerOperand();
        while (const auto *inner = ByteGepOf(base)) {
            base = inner->getPointerOperand();
        }
        return const_cast< llvm::Value * >(base);
    }


    namespace {

        // The low 64 bits of a constant of any width: `getZExtValue` asserts
        // on a wider one, which a `trunc` of an i128 constant can bring into a
        // tree that is itself at most 64 bits wide.
        uint64_t LowWord(const llvm::ConstantInt *ci) {
            return ci->getValue().getBitWidth() <= 64 ? ci->getZExtValue()
                                                      : ci->getValue().trunc(64).getZExtValue();
        }

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
        const std::vector< LeafBits > kNoLeafBits;

        std::pair< uint64_t, uint32_t > FingerprintNode(
            llvm::Value *v, const llvm::DenseMap< llvm::Value *, uint32_t > &leaf_index,
            const llvm::DenseSet< llvm::Value * > &tree_set,
            const llvm::DenseMap< llvm::Value *, llvm::Value * > &phi_redirects,
            llvm::DenseMap< llvm::Value *, std::pair< uint64_t, uint32_t > > &memo,
            const std::vector< LeafBits > &leaf_bits
        ) {
            if (auto it = memo.find(v); it != memo.end()) {
                return it->second;
            }

            // Leaves are checked before the redirect map so that a phi kept as
            // a leaf hashes by position, matching how it is later evaluated.
            // What is known of the leaf's bits is part of the tree read, so it
            // is part of the identity - and a leaf nothing is known about
            // hashes as it always did.
            if (auto it = leaf_index.find(v); it != leaf_index.end()) {
                uint64_t hash = MixFingerprint(1, it->second);
                if (it->second < leaf_bits.size() && leaf_bits[it->second].Informative()) {
                    hash = MixFingerprint(hash, leaf_bits[it->second].zero);
                    hash = MixFingerprint(hash, leaf_bits[it->second].one);
                }
                const std::pair< uint64_t, uint32_t > leaf{ hash, 0 };
                memo[v] = leaf;
                return leaf;
            }
            if (auto *ci = llvm::dyn_cast< llvm::ConstantInt >(v)) {
                const std::pair< uint64_t, uint32_t > c{ MixFingerprint(2, LowWord(ci)),
                                                         0 };
                memo[v] = c;
                return c;
            }
            if (auto it = phi_redirects.find(v); it != phi_redirects.end()) {
                auto through =
                    FingerprintNode(it->second, leaf_index, tree_set, phi_redirects, memo, leaf_bits);
                // Read through only because every arm is the same computation
                // (`VerifyPhiArms`); how many arms there are is part of what
                // that was decided on.
                if (const auto *phi = llvm::dyn_cast< llvm::PHINode >(v)) {
                    through.first = MixFingerprint(through.first, 6 + phi->getNumIncomingValues());
                }
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

            // The operation, not just its opcode: two comparisons with
            // different predicates, two intrinsics behind one `call`, or two
            // casts from different widths compute different things, and a
            // verdict on one must not be read back for the other.
            uint64_t hash = MixFingerprint(5, inst->getOpcode());
            if (inst->getType()->isIntegerTy()) {
                hash = MixFingerprint(hash, inst->getType()->getIntegerBitWidth());
            }
            if (const auto *cmp = llvm::dyn_cast< llvm::CmpInst >(inst)) {
                hash = MixFingerprint(hash, cmp->getPredicate());
            }
            if (const auto *call = llvm::dyn_cast< llvm::CallBase >(inst)) {
                const auto *callee = call->getCalledFunction();
                hash               = MixFingerprint(
                    hash, callee != nullptr ? static_cast< uint64_t >(callee->getIntrinsicID()) + 1 : 0
                );
            }
            if (llvm::isa< llvm::CastInst >(inst) && inst->getOperand(0)->getType()->isIntegerTy()) {
                hash = MixFingerprint(hash, inst->getOperand(0)->getType()->getIntegerBitWidth());
            }
            uint32_t children = 0;
            for (llvm::Use &use : inst->operands()) {
                auto child =
                    FingerprintNode(use.get(), leaf_index, tree_set, phi_redirects, memo, leaf_bits);
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
            const llvm::DenseMap< llvm::Value *, llvm::Value * > &phi_redirects,
            const std::vector< LeafBits > &leaf_bits = kNoLeafBits
        ) {
            llvm::DenseMap< llvm::Value *, uint32_t > leaf_index;
            for (uint32_t i = 0; i < leaves.size(); ++i) {
                leaf_index[leaves[i]] = i;
            }

            llvm::DenseMap< llvm::Value *, std::pair< uint64_t, uint32_t > > memo;
            auto walked = FingerprintNode(root, leaf_index, tree_set, phi_redirects, memo, leaf_bits);

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
            if (node == nullptr || node->getNumOperands() != 4) {
                return false;
            }

            const auto read = [node](unsigned i) -> std::optional< uint64_t > {
                auto *as_const =
                    llvm::dyn_cast< llvm::ConstantAsMetadata >(node->getOperand(i));
                if (as_const == nullptr) {
                    return std::nullopt;
                }
                auto *as_int = llvm::dyn_cast< llvm::ConstantInt >(as_const->getValue());
                if (as_int == nullptr) {
                    return std::nullopt;
                }
                return as_int->getZExtValue();
            };

            auto structure = read(0);
            auto depth     = read(1);
            auto num_vars  = read(2);
            auto tag       = read(3);
            if (!structure || !depth || !num_vars || !tag) {
                return false;
            }

            return *structure == fp.structure && *depth == fp.depth && *num_vars == fp.num_vars
                && *tag == options_tag;
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
                case llvm::Instruction::AShr:
                case llvm::Instruction::ZExt:
                case llvm::Instruction::SExt:
                case llvm::Instruction::Trunc:
                case llvm::Instruction::Select:
                case llvm::Instruction::ICmp:
                    return true;
                default:
                    return false;
            }
        }

        // A funnel shift by a constant: what a rotate lifts to, and a bitwise
        // operator like any other once the amount is known -
        // `fshl(a, b, s) = (a << s) | (b >> (w - s))`. Left as a leaf it cuts
        // the value it rotates off from everything read through it, which is
        // where an obfuscator's identities over the same value then fail.
        const llvm::IntrinsicInst *FunnelShiftOf(const llvm::Value *v) {
            const auto *intrinsic = llvm::dyn_cast< llvm::IntrinsicInst >(v);
            if (intrinsic == nullptr
                || (intrinsic->getIntrinsicID() != llvm::Intrinsic::fshl
                    && intrinsic->getIntrinsicID() != llvm::Intrinsic::fshr)
                || !intrinsic->getType()->isIntegerTy()
                || !llvm::isa< llvm::ConstantInt >(intrinsic->getArgOperand(2)))
            {
                return nullptr;
            }
            return intrinsic;
        }

        // A byte swap: a fixed permutation of the value's bytes, which is a
        // bitwise operation like any other once the width is known -
        // `bswap(x) = OR_i (((x >> 8i) & 0xFF) << 8(n-1-i))`. Left as a leaf it
        // does what a funnel shift left as a leaf does: everything above it
        // reads an opaque variable in place of the value below, so an identity
        // an obfuscator wrote across it cannot be seen, and the value below
        // becomes a second variable standing for something the one above is a
        // function of.
        //
        // `bitreverse` is the same kind of permutation and is deliberately not
        // followed: it needs one term per *bit* rather than per byte, so the
        // tree it expands to is larger than anything the search could use.
        const llvm::IntrinsicInst *ByteSwapOf(const llvm::Value *v) {
            const auto *intrinsic = llvm::dyn_cast< llvm::IntrinsicInst >(v);
            if (intrinsic == nullptr || intrinsic->getIntrinsicID() != llvm::Intrinsic::bswap
                || !intrinsic->getType()->isIntegerTy())
            {
                return nullptr;
            }
            // The intrinsic is defined for a whole number of bytes, and two is
            // the smallest width that permutes anything.
            const auto width = intrinsic->getType()->getIntegerBitWidth();
            return width >= 16 && width % 8 == 0 ? intrinsic : nullptr;
        }

        // How many bytes a byte swap permutes.
        uint64_t ByteSwapBytes(const llvm::IntrinsicInst &bswap) {
            return bswap.getType()->getIntegerBitWidth() / 8;
        }

        // A two-operand minimum or maximum, which is the `select` over a
        // comparison it stands for. The model already writes a `select` in
        // existing operators (`B ^ ((A ^ B) & -c)`), so following one of these
        // costs nothing new; left as a leaf it cuts the two values it chooses
        // between off from everything read through it.
        const llvm::IntrinsicInst *MinMaxOf(const llvm::Value *v) {
            const auto *intrinsic = llvm::dyn_cast< llvm::IntrinsicInst >(v);
            if (intrinsic == nullptr || !intrinsic->getType()->isIntegerTy()) {
                return nullptr;
            }
            switch (intrinsic->getIntrinsicID()) { // NOLINT(hicpp-multiway-paths-covered)
                case llvm::Intrinsic::smax:
                case llvm::Intrinsic::smin:
                case llvm::Intrinsic::umax:
                case llvm::Intrinsic::umin:
                    return intrinsic;
                default:
                    return nullptr;
            }
        }

        // The comparison a minimum or maximum picks on: true when the *first*
        // operand is the one it returns.
        llvm::CmpInst::Predicate MinMaxPredicate(const llvm::IntrinsicInst &minmax) {
            switch (minmax.getIntrinsicID()) { // NOLINT(hicpp-multiway-paths-covered)
                case llvm::Intrinsic::smax:
                    return llvm::CmpInst::ICMP_SGT;
                case llvm::Intrinsic::smin:
                    return llvm::CmpInst::ICMP_SLT;
                case llvm::Intrinsic::umax:
                    return llvm::CmpInst::ICMP_UGT;
                default:
                    return llvm::CmpInst::ICMP_ULT;
            }
        }

        // The rotate amount reduced to the type's width, and the width itself.
        std::pair< uint64_t, uint64_t > FunnelShiftAmount(const llvm::IntrinsicInst &fsh) {
            const uint64_t width = fsh.getType()->getIntegerBitWidth();
            const uint64_t amount =
                llvm::cast< llvm::ConstantInt >(fsh.getArgOperand(2))->getValue().urem(width);
            return { amount, width };
        }

        // A `freeze` over an integer, which is the identity on every value its
        // operand can actually take. Left as a leaf it does to a tree what a
        // funnel shift left as a leaf does: everything above it reads an opaque
        // variable in place of the value below, so an identity written across
        // it - and an obfuscator has no reason to avoid one - cannot be seen.
        // Worse, the value below becomes a *second* variable standing for an
        // expression the one above is a function of, so the tree claims an
        // independence the program does not have and no rewrite of it can hold.
        //
        // Following it costs nothing in the model: the evaluator and the AST
        // probe leaves over the whole width, so they already describe `freeze`
        // on a defined operand. What it costs in the IR is a guarantee, and
        // `CandidateNeedsFrozenLeaves` is where that is paid back.
        bool IsTransparentFreeze(const llvm::Value *v) {
            const auto *freeze = llvm::dyn_cast< llvm::FreezeInst >(v);
            return freeze != nullptr && freeze->getType()->isIntegerTy();
        }

        bool IsMbaInstruction(const llvm::Instruction *inst) {
            return IsMbaOpcode(inst->getOpcode()) || FunnelShiftOf(inst) != nullptr
                || ByteSwapOf(inst) != nullptr || MinMaxOf(inst) != nullptr
                || IsTransparentFreeze(inst) || ByteGepOf(inst) != nullptr;
        }

        // `icmp` produces i1, which is useless as a candidate root: it is only
        // worth following as the condition feeding a `select` in a wider tree.
        // A truncation is no better: the value it narrows is wider than the
        // root, so it could never join the tree, and the root would only ever
        // be rewritten into itself.
        bool IsMbaRootOpcode(unsigned opcode) {
            return opcode != llvm::Instruction::ICmp && opcode != llvm::Instruction::Trunc
                && IsMbaOpcode(opcode);
        }

        // A `ptradd` is a root only when it is a chain whose two halves are both
        // dynamic. One step is no sum at all - rooting there says nothing that
        // rooting at its index does not - and a shared base plus a constant
        // displacement is how a canonicalization spells one location: rewriting
        // that into a single step would collapse the one-pointer-per-location
        // form the alias analysis and the store coalescing are built on, for an
        // identity that is not there. An obfuscator's split is two computed
        // halves, and that is the shape worth rooting at.
        bool IsPtrAddChain(const llvm::Instruction *inst) {
            const auto *gep = ByteGepOf(inst);
            if (gep == nullptr) {
                return false;
            }
            const auto *inner = ByteGepOf(gep->getPointerOperand());
            return inner != nullptr && !llvm::isa< llvm::ConstantInt >(gep->getOperand(1))
                && !llvm::isa< llvm::ConstantInt >(inner->getOperand(1));
        }

        bool IsMbaRoot(const llvm::Instruction *inst) {
            return IsMbaRootOpcode(inst->getOpcode()) || FunnelShiftOf(inst) != nullptr
                || ByteSwapOf(inst) != nullptr || MinMaxOf(inst) != nullptr
                || IsPtrAddChain(inst);
        }

        // The operators that read their operands bit by bit, and the ones that
        // carry between bits. An arithmetic value under a bitwise reader can
        // only ever be a variable to the reading above it - no sum of atoms
        // over `a` and `b` equals `(a + b) ^ c` - which is what the boundary
        // cut in `CollectTree` is about.
        bool IsArithmeticOpcode(unsigned opcode) {
            return opcode == llvm::Instruction::Add || opcode == llvm::Instruction::Sub
                || opcode == llvm::Instruction::Mul || opcode == llvm::Instruction::Shl;
        }

        bool IsBitwiseReaderOpcode(unsigned opcode) {
            switch (opcode) { // NOLINT(hicpp-multiway-paths-covered)
                case llvm::Instruction::And:
                case llvm::Instruction::Or:
                case llvm::Instruction::Xor:
                case llvm::Instruction::LShr:
                case llvm::Instruction::AShr:
                case llvm::Instruction::Trunc:
                case llvm::Instruction::ZExt:
                case llvm::Instruction::SExt:
                    return true;
                default:
                    return false;
            }
        }

        // The width of `v` when it is an integer narrower than the tree, else
        // zero. Such a value is held zero-extended and masked back to its own
        // width wherever an operation could carry into the bits above it.
        uint32_t NarrowWidth(const llvm::Value *v, uint32_t bw) {
            const auto *ty = v->getType();
            if (!ty->isIntegerTy() || ty->getIntegerBitWidth() >= bw) {
                return 0;
            }
            return ty->getIntegerBitWidth();
        }

        // Predicates the evaluator can reproduce from a value masked to the
        // tree width. Ordered comparisons are normalised to lt/le by swapping
        // operands, so gt/ge need no separate handling.
        bool IsSupportedPredicate(llvm::CmpInst::Predicate pred) {
            switch (pred) { // NOLINT(hicpp-multiway-paths-covered)
                case llvm::CmpInst::ICMP_EQ:
                case llvm::CmpInst::ICMP_NE:
                case llvm::CmpInst::ICMP_ULT:
                case llvm::CmpInst::ICMP_ULE:
                case llvm::CmpInst::ICMP_UGT:
                case llvm::CmpInst::ICMP_UGE:
                case llvm::CmpInst::ICMP_SLT:
                case llvm::CmpInst::ICMP_SLE:
                case llvm::CmpInst::ICMP_SGT:
                case llvm::CmpInst::ICMP_SGE:
                    return true;
                default:
                    return false;
            }
        }

        bool IsTreeWidthInt(const llvm::Value *v, uint32_t bw) {
            auto *ty = v->getType();
            return ty->isIntegerTy() && ty->getIntegerBitWidth() == bw;
        }

        // A value wider than the tree normally has no reading at the tree's
        // width and stays a leaf. A logical shift by a constant is the one
        // exception worth making.
        //
        // When every reader of the shift truncates its result, the shift is
        // only ever read for the bits it brought down; and when the highest of
        // those started below the tree's width, they all came from the low `bw`
        // bits of the value being shifted - which is exactly what the tree
        // holds for a wider value. Shifting those gives the same bits, and the
        // truncation above discards the positions the narrow shift filled with
        // zeros instead of the bits it could not reach.
        //
        // This is what lets a word taken apart into slices and put back
        // together be read as one expression over one variable. Without it each
        // slice is an opaque leaf of its own, so the halves of a value look
        // independent and no identity spanning them can be found - which is how
        // `(sext x ^ C)` split into two halves, each xored with a constant and
        // recombined, stays thirteen instructions when it is `x + 1`.
        bool WideSliceFitsTreeWidth(const llvm::Instruction *inst, uint32_t bw) {
            if (inst->getOpcode() != llvm::Instruction::LShr || inst->use_empty()) {
                return false;
            }
            const auto *amount = llvm::dyn_cast< llvm::ConstantInt >(inst->getOperand(1));
            if (amount == nullptr || amount->getValue().uge(bw)) {
                return false;
            }
            const uint32_t shift = static_cast< uint32_t >(amount->getZExtValue());
            for (const llvm::User *user : inst->users()) {
                const auto *trunc = llvm::dyn_cast< llvm::TruncInst >(user);
                if (trunc == nullptr || shift + trunc->getType()->getIntegerBitWidth() > bw) {
                    return false;
                }
            }
            return true;
        }

        // The bit a sign extension copies, as a mask over the tree width, or
        // zero when the extension is not modelled as the ring expression
        // `(v ^ 2^(n-1)) - 2^(n-1)` - because it is not a sign extension, or
        // because the node is wider than the tree and so reads through (see
        // `FitsTreeWidth` and `WideNodeFitsTreeWidth`).
        //
        // One answer for the three readings of a tree - the collection's width
        // check, the probe plan and the Expr - so none of them can model the
        // extension differently from the others.
        uint64_t SignBitOfExtensionSource(const llvm::Instruction *inst, uint32_t bw) {
            if (inst->getOpcode() != llvm::Instruction::SExt || !inst->getType()->isIntegerTy()
                || inst->getType()->getIntegerBitWidth() > bw)
            {
                return 0;
            }
            const auto *source = inst->getOperand(0)->getType();
            if (!source->isIntegerTy() || source->getIntegerBitWidth() >= bw) {
                return 0;
            }
            return uint64_t{ 1 } << (source->getIntegerBitWidth() - 1);
        }

        // The bit the sign lands on when an arithmetic right shift is read as
        // the ring expression `(lshr(v, c) ^ H) - H`, as a mask over the
        // shifted value's own width - or zero when the shift is not modelled
        // that way and stays a leaf.
        //
        // `ashr(v, c)` on a value of `w` bits held zero-extended is the logical
        // shift with the sign copied into the `c` bits it vacated, and
        // `H = 2^(w-1-c)` is where that sign bit lands: flipping it adds or
        // subtracts `H`, and subtracting it back leaves the value where the bit
        // was clear and the value with every higher bit set where it was set.
        // The same identity a sign extension is read by
        // (`SignBitOfExtensionSource`), one shift further down, and exact at
        // any width in the operators the model already has.
        //
        // Left as a leaf, an `ashr` cuts the value it shifts off from every
        // other reading of that value, which is the whole of an identity the
        // obfuscator writes across it: `zext(x ^ m) ^ sext(m)` is `sext x` only
        // when `m` is `ashr(x, 31)`, and with the shift a leaf `m` and `x` are
        // two independent variables and the identity is not there to find.
        //
        // Zero for a variable amount, which has no expression at all; for an
        // amount at or past the value's own width, which is poison; and for a
        // value wider than the tree, whose sign bit is not in the tree to begin
        // with.
        //
        // One answer for the three readings of a tree, for the reason
        // `SignBitOfExtensionSource` gives.
        uint64_t AShrSignBit(const llvm::Instruction *inst, uint32_t bw) {
            if (inst->getOpcode() != llvm::Instruction::AShr
                || !inst->getType()->isIntegerTy())
            {
                return 0;
            }
            const uint32_t width = inst->getType()->getIntegerBitWidth();
            if (width == 0 || width > bw) {
                return 0;
            }
            const auto *amount = llvm::dyn_cast< llvm::ConstantInt >(inst->getOperand(1));
            if (amount == nullptr || amount->getValue().uge(width)) {
                return 0;
            }
            return uint64_t{ 1 } << (width - 1 - amount->getZExtValue());
        }

        // Whether a node wider than the tree has an exact reading at the tree's
        // width `bw`, which is its low `bw` bits.
        //
        // The descent only ever arrives at such a node from a reader that looks
        // at nothing else: a truncation to `bw` bits or fewer, the slice above,
        // or - by induction - another node admitted here. So the question is
        // whether the node's own low `bw` bits are a function of its operands'
        // low `bw` bits, and for the ring operations they are: reduction modulo
        // `2^bw` is a homomorphism, which is the same fact that lets a narrower
        // node be held zero-extended and masked. The bitwise operators work bit
        // by bit; a left shift by a constant is a multiplication; extending a
        // value leaves its low bits where they were, as long as a sign extension
        // starts at or above `bw` - from below it the copied sign bit lands
        // inside the tree, and the model has no reading for that. A right shift
        // moves bits down from above, so it is exact only as the slice; division
        // and comparison read the whole word and stay leaves.
        //
        // Without this the tree stops at the first wide value it meets, and what
        // it rewrites is phrased over `trunc` of that value. An obfuscator widens
        // `x` before taking it apart - `sext x ^ C` - and parks the wide value in
        // a slot as well, so nothing downstream can sink the truncation through
        // a `xor` with other readers, and `x + 1` is left as
        // `(trunc (sext x ^ C) ^ K) + 1`.
        bool WideNodeFitsTreeWidth(const llvm::Instruction *inst, uint32_t bw) {
            if (WideSliceFitsTreeWidth(inst, bw)) {
                return true;
            }
            // Constants are read through `getZExtValue`.
            if (inst->getType()->getIntegerBitWidth() > 64) {
                return false;
            }
            switch (inst->getOpcode()) {
                case llvm::Instruction::Add:
                case llvm::Instruction::Sub:
                case llvm::Instruction::Mul:
                case llvm::Instruction::And:
                case llvm::Instruction::Or:
                case llvm::Instruction::Xor:
                    return true;
                case llvm::Instruction::Shl: {
                    const auto *amount = llvm::dyn_cast< llvm::ConstantInt >(inst->getOperand(1));
                    return amount != nullptr && amount->getValue().ult(bw);
                }
                case llvm::Instruction::ZExt:
                    return inst->getOperand(0)->getType()->isIntegerTy();
                case llvm::Instruction::SExt: {
                    const auto *src = inst->getOperand(0)->getType();
                    return src->isIntegerTy() && src->getIntegerBitWidth() >= bw;
                }
                default:
                    return false;
            }
        }

        // Every slot in a tree is evaluated under one mask, so a comparison of
        // operands narrower than the root would order them by a sign bit they
        // do not have. An `icmp` therefore only joins the tree when it compares
        // at the tree width; otherwise it stays a leaf, the behaviour that
        // predates its support.
        //
        // A `select` needs its condition to come from such an `icmp` or an i1
        // constant. Any other condition would enter the tree as an i1 leaf, and
        // leaves are probed across the full width, so it would be fed values a
        // one-bit value can never take. Its arms may be narrower than the tree:
        // it hands one of them on untouched, and a narrow value is held in range
        // like any other (see below), so the value chosen is in range as well -
        // a carry picked by a comparison of the whole word and merged into a
        // half of it is the shape this is for. One wider than the tree is left
        // to `WideNodeFitsTreeWidth`, which does not take it.
        //
        // `narrow_comparisons` (the boolean cut, see `BooleanCutMbaCandidate`)
        // also takes an equality or unsigned comparison of narrower operands,
        // in a `select` condition as well. Held zero-extended, two values are
        // equal, and ordered without regard to sign, exactly as they are at
        // their own width; only a signed predicate reads the bit that is not
        // there, and it stays a leaf.
        bool ComparesExactlyNarrow(const llvm::ICmpInst &cmp, uint32_t bw) {
            const auto *ty = cmp.getOperand(0)->getType();
            return ty->isIntegerTy() && ty->getIntegerBitWidth() <= bw
                && (cmp.isEquality() || cmp.isUnsigned());
        }

        bool FitsTreeWidth(const llvm::Instruction *inst, uint32_t bw, bool narrow_comparisons = false) {
            if (const auto *gep = ByteGepOf(inst)) {
                return bw == gep->getOperand(1)->getType()->getIntegerBitWidth();
            }
            if (const auto *cmp = llvm::dyn_cast< llvm::ICmpInst >(inst)) {
                if (narrow_comparisons && ComparesExactlyNarrow(*cmp, bw)) {
                    return true;
                }
                return IsTreeWidthInt(cmp->getOperand(0), bw)
                    && IsSupportedPredicate(cmp->getPredicate());
            }

            // A byte swap is exact at the value's own width, which the lowering
            // masks its operand to; wider than the tree it stays a leaf.
            if (const auto *bswap = ByteSwapOf(inst)) {
                return bswap->getType()->getIntegerBitWidth() <= bw;
            }

            // A minimum or maximum compares whole words, so it is only exact at
            // the width the tree is evaluated at: a narrower value is held
            // zero-extended inside the tree, and a signed comparison of the
            // extended words is not the one the program made.
            if (const auto *minmax = MinMaxOf(inst)) {
                return minmax->getType()->getIntegerBitWidth() == bw;
            }

            // A `select` reads its condition as the 0/1 value it is, whatever
            // computes it: a comparison the tree can read is read, and any
            // other `i1` - an argument, a load, a comparison too wide for the
            // tree - is a leaf, held as 0 or 1 by the same narrow-leaf mask
            // every reading applies.
            if (inst->getOpcode() == llvm::Instruction::Select) {
                if (!inst->getType()->isIntegerTy() || inst->getType()->getIntegerBitWidth() > bw) {
                    return false;
                }
                return inst->getOperand(0)->getType()->isIntegerTy(1);
            }

            // Everything else is held at its own width inside a tree evaluated
            // at the root's. A narrower node is zero-extended and masked back
            // to its width after every operation that can carry into the bits
            // above it, which is exact: reducing a ring computation modulo 2^n
            // commutes with computing it, the logical shift and the bitwise
            // operators keep an in-range value in range, and a constant is
            // already the value its own width gives it. A wider node is read
            // at the root's width where `WideNodeFitsTreeWidth` says that is
            // exact, and stays a leaf otherwise.
            const auto *ty = inst->getType();
            if (!ty->isIntegerTy()) {
                return false;
            }
            if (ty->getIntegerBitWidth() > bw) {
                return WideNodeFitsTreeWidth(inst, bw);
            }

            // A zero extension is the identity on a value held zero-extended.
            //
            // A sign extension is the same value with the source's top bit
            // copied into every bit above it, which on a value held
            // zero-extended is `(v ^ 2^(n-1)) - 2^(n-1)` for a source of `n`
            // bits: flipping the sign bit adds or subtracts `2^(n-1)`, and
            // subtracting it back leaves `v` where the bit was clear and `v`
            // with every higher bit set where it was set. That is a ring
            // expression in the operators the model already has, exact at any
            // width, so a sign extension is as transparent as a zero
            // extension. It used to be modelled from an `i1` only - the same
            // formula with `n = 1`, where it reads as a negation - and from
            // anything wider the extension was a leaf, which cut the value it
            // widens off from every other reading of that value: the obfuscator
            // writes `zext(x ^ m) ^ sext(m)` for `sext x`, and with the `sext`
            // a leaf the two halves of that identity never meet.
            if (inst->getOpcode() == llvm::Instruction::ZExt) {
                return inst->getOperand(0)->getType()->isIntegerTy();
            }
            if (inst->getOpcode() == llvm::Instruction::SExt) {
                // Asked of the same helper the two readings below use, so a
                // sign extension the model has no expression for stays a leaf
                // rather than being collected and then read as a zero
                // extension.
                return SignBitOfExtensionSource(inst, bw) != 0;
            }

            // A truncation is a mask. The value it narrows may be wider than
            // the tree: the tree then holds that value's low `bw` bits, and a
            // mask that keeps a subset of them is exact whatever sits above.
            if (inst->getOpcode() == llvm::Instruction::Trunc) {
                return inst->getOperand(0)->getType()->isIntegerTy();
            }

            // An arithmetic right shift is the logical one with the sign copied
            // into the bits it vacated, which is a ring expression at the
            // value's own width - asked of the same helper the two readings
            // below use, so a shift the model has no expression for stays a
            // leaf rather than being collected and then read as a logical one.
            if (inst->getOpcode() == llvm::Instruction::AShr) {
                return AShrSignBit(inst, bw) != 0;
            }

            // The model shifts by constants only. A shift by a variable amount
            // collected into the tree leaves the tree with no expression at all
            // - and so with nothing to prove a rewrite against - where as a leaf
            // it is one more variable.
            if ((inst->getOpcode() == llvm::Instruction::Shl
                 || inst->getOpcode() == llvm::Instruction::LShr)
                && !llvm::isa< llvm::ConstantInt >(inst->getOperand(1)))
            {
                return false;
            }

            return true;
        }

        // True when an incoming value of `phi` leads back to `phi`, i.e. it
        // carries a value around a loop.
        //
        // Such a phi must not be treated as transparent. Following the arm would
        // re-enter the phi, and the recursion guards downstream break that cycle
        // by substituting literal zero -- so a counter like
        // `phi [1, entry], [iv + 1, body]` gets modelled as the constant `0 + 1`.
        // That is not a conservative approximation but a different function, and
        // both the evaluator and the AST agree on it, so the solver confirms a
        // rewrite of an expression the program never computes. Worse, the arm
        // divergence check then compares the folded `1` against the entry arm's
        // literal `1`, finds them equal, and ratifies the transparency.
        bool PhiIsLoopCarried(llvm::PHINode *phi) {
            // Cap the walk so a wide operand graph cannot make detection
            // quadratic in the size of the surrounding function.
            constexpr unsigned kMaxVisited = 512;

            llvm::DenseSet< llvm::Value * > seen;
            llvm::SmallVector< llvm::Value *, 16 > work;
            for (llvm::Value *inc : phi->incoming_values()) {
                work.push_back(inc);
            }

            while (!work.empty()) {
                llvm::Value *v = work.pop_back_val();
                if (v == phi) {
                    return true;
                }
                if (!seen.insert(v).second) {
                    continue;
                }
                // Out of budget is not "no cycle": the walk has not looked at
                // everything the phi reaches, and reading a loop-carried phi
                // as transparent is the miscompilation described above.
                if (seen.size() > kMaxVisited) {
                    return true;
                }
                auto *inst = llvm::dyn_cast< llvm::Instruction >(v);
                if (inst == nullptr) {
                    continue;
                }
                for (llvm::Use &use : inst->operands()) {
                    work.push_back(use.get());
                }
            }
            return false;
        }

        // BFS from root following operands.  MBA-opcode instructions
        // are added to tree_insts; everything else becomes a leaf.
        // PHI nodes are treated as transparent when all incoming
        // values are MBA opcodes — the first arm is followed and a
        // redirect entry is recorded for evaluation / expr building.
        //
        // `max_nodes`, when non-zero, stops the tree at that many instructions:
        // once the budget is spent every value still to be visited is kept as a
        // leaf rather than expanded. The walk is breadth-first, so the budget
        // buys the nodes nearest the root and the result is a connected prefix
        // of the full tree.
        //
        // `cut_arith_under_bitwise` keeps every arithmetic value that a bitwise
        // operator reads as a leaf, whatever it is computed from. That is the
        // boundary the semilinear reading needs: an atom is a bitwise function
        // of its variables, so `(a + b) ^ c` has one only with `a + b` as the
        // variable, and an expansion past it dissolves the identity it takes
        // part in rather than exposing another.
        //
        // `opaque`, when given, names values to keep as leaves wherever the
        // walk meets them (see `SharedCutMbaCandidate`). The root is never one.
        //
        // `reads_as`, when given, names values the caller wants read as
        // something else - zero, for a summand left out of a sum group (see
        // `SumGroupSliceMbaCandidates`). The substitution is recorded where a
        // transparent phi's is, so every later reading of the tree follows it
        // too: the expression, the probe plan and the fingerprint all consult
        // that map. What the value stands for is walked in its place, so
        // nothing below it reaches the tree.
        void CollectTree(
            llvm::Instruction *root, uint32_t bw,
            llvm::SmallVector< llvm::Instruction *, 16 > &tree_insts,
            std::vector< llvm::Value * > &leaves,
            llvm::DenseMap< llvm::Value *, llvm::Value * > &phi_redirects,
            bool try_phi_transparency = true, uint32_t max_nodes = 0,
            bool cut_arith_under_bitwise = false, uint32_t max_tree_nodes = 0,
            const llvm::DenseSet< const llvm::Value * > *opaque = nullptr,
            const llvm::DenseMap< llvm::Value *, llvm::Value * > *reads_as = nullptr,
            bool narrow_comparisons = false
        ) {
            // A re-cut's budget and the cap on any collection are both
            // budgets on the same walk; the tighter one applies.
            const uint32_t budget = max_nodes == 0 ? max_tree_nodes
                : max_tree_nodes == 0                ? max_nodes
                                                     : std::min(max_nodes, max_tree_nodes);

            llvm::DenseSet< llvm::Value * > visited;
            // Each entry says whether the value was reached through a bitwise
            // reader, which is what decides the boundary cut.
            std::queue< std::pair< llvm::Value *, bool > > work;
            work.push({ root, false });

            while (!work.empty()) {
                auto [v, from_bitwise] = work.front();
                work.pop();
                if (!visited.insert(v).second) {
                    continue;
                }

                auto *inst = llvm::dyn_cast< llvm::Instruction >(v);

                // Read as something else, on the caller's word, before any
                // other reading of the value is considered.
                if (reads_as != nullptr && v != root) {
                    if (auto it = reads_as->find(v); it != reads_as->end()) {
                        phi_redirects[v] = it->second;
                        work.push({ it->second, from_bitwise });
                        continue;
                    }
                }

                // Out of budget: the node is whatever it computes, not how it
                // computes it. Constants stay constants — turning one into a
                // variable would only widen the signature with a column that
                // never varies.
                if (budget != 0 && tree_insts.size() >= budget
                    && !llvm::isa< llvm::Constant >(v))
                {
                    if (std::find(leaves.begin(), leaves.end(), v) == leaves.end()) {
                        leaves.push_back(v);
                    }
                    continue;
                }

                if (opaque != nullptr && v != root && opaque->contains(v)) {
                    if (std::find(leaves.begin(), leaves.end(), v) == leaves.end()) {
                        leaves.push_back(v);
                    }
                    continue;
                }

                if ((inst != nullptr) && IsMbaInstruction(inst)
                    && FitsTreeWidth(inst, bw, narrow_comparisons)) {
                    if (cut_arith_under_bitwise && from_bitwise
                        && IsArithmeticOpcode(inst->getOpcode()))
                    {
                        if (std::find(leaves.begin(), leaves.end(), v) == leaves.end()) {
                            leaves.push_back(v);
                        }
                        continue;
                    }

                    // A chain of `ptradd`s is taken whole or not at all: the
                    // node's value is the sum of the indices, so an inner step
                    // left outside the tree would stand for the pointer it
                    // computes and the two readings would disagree.
                    if (ByteGepOf(inst) != nullptr) {
                        const llvm::Value *step = inst;
                        while (const auto *ptradd = ByteGepOf(step)) {
                            auto *node = const_cast< llvm::GetElementPtrInst * >(ptradd);
                            if (node != inst) {
                                if (!visited.insert(node).second) {
                                    break;
                                }
                                tree_insts.push_back(node);
                            }
                            work.push({ node->getOperand(1), from_bitwise });
                            step = node->getPointerOperand();
                        }
                        tree_insts.push_back(inst);
                        continue;
                    }
                    tree_insts.push_back(inst);
                    if (const auto *fsh = FunnelShiftOf(inst)) {
                        // The callee and the amount are not values to follow.
                        work.push({ fsh->getArgOperand(0), true });
                        work.push({ fsh->getArgOperand(1), true });
                        continue;
                    }
                    // A byte swap reads its operand bit by bit, like the shifts
                    // and masks it expands to.
                    if (const auto *bswap = ByteSwapOf(inst)) {
                        work.push({ bswap->getArgOperand(0), true });
                        continue;
                    }
                    // A minimum or maximum picks between two whole words, which
                    // is what a comparison feeding a `select` does, so its
                    // operands are followed the way a `select`'s arms are.
                    if (const auto *minmax = MinMaxOf(inst)) {
                        work.push({ minmax->getArgOperand(0), from_bitwise });
                        work.push({ minmax->getArgOperand(1), from_bitwise });
                        continue;
                    }
                    // A freeze is not an operator, so it neither starts nor
                    // stops a bitwise reading: the boundary cut has to see the
                    // same thing through it that it would see without it.
                    if (IsTransparentFreeze(inst)) {
                        work.push({ inst->getOperand(0), from_bitwise });
                        continue;
                    }
                    const bool kReadsBitwise = IsBitwiseReaderOpcode(inst->getOpcode());
                    for (auto &op : inst->operands()) {
                        work.push({ op.get(), kReadsBitwise });
                    }
                } else if (auto *phi = llvm::dyn_cast< llvm::PHINode >(v)) {
                    // Check if every incoming value is an instruction the
                    // walk reads (or a constant) - the same predicate the walk
                    // itself asks, so a rotate, a byte swap or a minimum is an
                    // arm as much as an `add` is. If so, treat the phi as
                    // transparent and follow the first arm.
                    bool all_mba = phi->getNumIncomingValues() > 0;
                    for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
                        auto *inc = phi->getIncomingValue(i);
                        if (llvm::isa< llvm::ConstantInt >(inc)) {
                            continue;
                        }
                        auto *inc_inst = llvm::dyn_cast< llvm::Instruction >(inc);
                        if ((inc_inst == nullptr) || !IsMbaInstruction(inc_inst)
                            || !FitsTreeWidth(inc_inst, bw, narrow_comparisons))
                        {
                            all_mba = false;
                            break;
                        }
                    }

                    if (all_mba && try_phi_transparency && !PhiIsLoopCarried(phi)) {
                        auto *chosen       = phi->getIncomingValue(0);
                        phi_redirects[phi] = chosen;
                        work.push({ chosen, from_bitwise });
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
        // What LLVM's value tracking knows of each leaf's bits where the tree
        // reads it (`LeafBits`). Bits above a narrow leaf's own width are not
        // news - `NarrowWidth` masks them wherever the leaf is read - so only
        // the bits within that width count, and a leaf nothing is known about
        // carries nothing.
        std::vector< LeafBits > LeafBitsOf(
            const std::vector< llvm::Value * > &leaves, uint32_t bw, const llvm::DataLayout &layout
        ) {
            std::vector< LeafBits > facts(leaves.size());
            for (size_t i = 0; i < leaves.size(); ++i) {
                auto *leaf = leaves[i];
                if (!leaf->getType()->isIntegerTy() || leaf->getType()->getIntegerBitWidth() > 64) {
                    continue;
                }
                const llvm::KnownBits known = llvm::computeKnownBits(leaf, layout);
                const uint32_t narrow      = NarrowWidth(leaf, bw);
                const uint64_t own         = (narrow == 0 ? Bitmask(bw) : Bitmask(narrow));
                facts[i].zero              = known.Zero.getZExtValue() & own;
                facts[i].one               = known.One.getZExtValue() & own;
            }
            return facts;
        }

        class TreeEvalPlan
        {
          public:
            static TreeEvalPlan Build(
                llvm::Instruction *root, const std::vector< llvm::Value * > &leaves,
                uint32_t bitwidth,
                const llvm::DenseMap< llvm::Value *, llvm::Value * > &phi_redirects,
                const std::vector< LeafBits > &leaf_bits = kNoLeafBits
            ) {
                TreeEvalPlan plan;
                plan.mask_      = Bitmask(bitwidth);
                plan.bits_      = bitwidth;
                plan.leaf_bits_ = leaf_bits;

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
                        // Conditions are held as 0/1, so a plain test against
                        // zero matches the i1 the IR would have produced.
                        case Op::kSelect:
                            slots[i] = slots[n.a] != 0 ? slots[n.b] : slots[n.c];
                            break;
                        case Op::kCmpEq:
                            slots[i] = slots[n.a] == slots[n.b] ? 1 : 0;
                            break;
                        case Op::kCmpNe:
                            slots[i] = slots[n.a] != slots[n.b] ? 1 : 0;
                            break;
                        case Op::kCmpUlt:
                            slots[i] = slots[n.a] < slots[n.b] ? 1 : 0;
                            break;
                        case Op::kCmpUle:
                            slots[i] = slots[n.a] <= slots[n.b] ? 1 : 0;
                            break;
                        // Operands are masked to the tree width, so the sign
                        // bit has to be re-extended before comparing.
                        case Op::kCmpSlt:
                            slots[i] =
                                SignExtend(slots[n.a], bits_) < SignExtend(slots[n.b], bits_)
                                ? 1
                                : 0;
                            break;
                        case Op::kCmpSle:
                            slots[i] =
                                SignExtend(slots[n.a], bits_) <= SignExtend(slots[n.b], bits_)
                                ? 1
                                : 0;
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
                kSelect,
                kCmpEq,
                kCmpNe,
                kCmpUlt,
                kCmpUle,
                kCmpSlt,
                kCmpSle,
            };

            struct Node
            {
                Op op;
                uint32_t a;
                uint32_t b;
                // Third operand, used only by kSelect.
                uint32_t c = 0;
            };

            static int64_t SignExtend(uint64_t v, uint32_t bits) {
                if (bits >= 64) {
                    return static_cast< int64_t >(v);
                }
                const uint64_t sign = 1ULL << (bits - 1);
                return static_cast< int64_t >((v ^ sign) - sign);
            }

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

            uint32_t Emit3(Op op, uint32_t a, uint32_t b, uint32_t c) {
                auto slot = static_cast< uint32_t >(nodes_.size());
                nodes_.push_back(Node{ .op = op, .a = a, .b = b, .c = c });
                return slot;
            }

            // Arithmetic ops mask their own result, but inputs are held raw, so
            // a comparison has to mask its operands itself: an out-of-range
            // probe value would otherwise order differently than the IR does.
            // gt/ge become lt/le with the operands swapped.
            template< typename LowerFn >
            uint32_t LowerCompare(llvm::ICmpInst &cmp, LowerFn &&lower) {
                const uint32_t a = Emit(Op::kMask, lower(cmp.getOperand(0)), 0);
                const uint32_t b = Emit(Op::kMask, lower(cmp.getOperand(1)), 0);

                switch (cmp.getPredicate()) { // NOLINT(hicpp-multiway-paths-covered)
                    case llvm::CmpInst::ICMP_EQ:
                        return Emit(Op::kCmpEq, a, b);
                    case llvm::CmpInst::ICMP_NE:
                        return Emit(Op::kCmpNe, a, b);
                    case llvm::CmpInst::ICMP_ULT:
                        return Emit(Op::kCmpUlt, a, b);
                    case llvm::CmpInst::ICMP_ULE:
                        return Emit(Op::kCmpUle, a, b);
                    case llvm::CmpInst::ICMP_UGT:
                        return Emit(Op::kCmpUlt, b, a);
                    case llvm::CmpInst::ICMP_UGE:
                        return Emit(Op::kCmpUle, b, a);
                    case llvm::CmpInst::ICMP_SLT:
                        return Emit(Op::kCmpSlt, a, b);
                    case llvm::CmpInst::ICMP_SLE:
                        return Emit(Op::kCmpSle, a, b);
                    case llvm::CmpInst::ICMP_SGT:
                        return Emit(Op::kCmpSlt, b, a);
                    case llvm::CmpInst::ICMP_SGE:
                        return Emit(Op::kCmpSle, b, a);
                    default:
                        return kZeroSlot;
                }
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
                    // Probes hand a leaf any word; a narrow leaf only ever
                    // holds its own width of it, a leaf with known bits only
                    // ever holds a word with them, and the Expr says the same.
                    const uint32_t slot =
                        ApplyLeafBits(leaf->second, MaskToWidth(v, Emit(Op::kInput, leaf->second, 0)));
                    slot_of[v] = slot;
                    return slot;
                }

                if (auto *ci = llvm::dyn_cast< llvm::ConstantInt >(v)) {
                    const auto index = static_cast< uint32_t >(consts_.size());
                    consts_.push_back(LowWord(ci) & mask_);
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
                    const uint32_t child = lower(inst->getOperand(0));
                    // A sign extension of a value held zero-extended is
                    // `(v ^ 2^(n-1)) - 2^(n-1)` (see `FitsTreeWidth`). Masking
                    // the child instead would leave the source's own bits and
                    // model a zero extension, disagreeing with the Expr the
                    // verifier builds. A node wider than the tree is a
                    // different question: it is only admitted when the source
                    // is at least as wide as the tree, and then the tree's bits
                    // are the source's own, so it passes through.
                    if (inst->getOpcode() == llvm::Instruction::SExt
                        && SignBitOfExtensionSource(inst, bits_) != 0)
                    {
                        const uint32_t sign = ConstSlot(SignBitOfExtensionSource(inst, bits_));
                        return MaskToWidth(v, Emit(Op::kSub, Emit(Op::kXor, child, sign), sign));
                    }
                    return Emit(Op::kMask, child, 0);
                }

                // A truncation keeps the low bits of a value held at a wider
                // width: a mask, at the width it narrows to.
                if (inst->getOpcode() == llvm::Instruction::Trunc) {
                    const uint32_t child = lower(inst->getOperand(0));
                    const auto width     = inst->getType()->getIntegerBitWidth();
                    return Emit(Op::kAnd, child, ConstSlot(Bitmask(width)));
                }

                if (inst->getOpcode() == llvm::Instruction::Select) {
                    const uint32_t cond = lower(inst->getOperand(0));
                    const uint32_t t    = lower(inst->getOperand(1));
                    const uint32_t e    = lower(inst->getOperand(2));
                    // An arm may be a raw input slot, so mask the chosen value
                    // rather than relying on a later op to do it.
                    return Emit(Op::kMask, Emit3(Op::kSelect, cond, t, e), 0);
                }

                if (auto *cmp = llvm::dyn_cast< llvm::ICmpInst >(inst)) {
                    return LowerCompare(*cmp, lower);
                }

                if (IsTransparentFreeze(inst)) {
                    return lower(inst->getOperand(0));
                }

                if (const auto *fsh = FunnelShiftOf(inst)) {
                    const auto [amount, width] = FunnelShiftAmount(*fsh);
                    const uint32_t a           = lower(fsh->getArgOperand(0));
                    const uint32_t b           = lower(fsh->getArgOperand(1));
                    const bool kLeft           = fsh->getIntrinsicID() == llvm::Intrinsic::fshl;
                    if (amount == 0) {
                        return kLeft ? a : b;
                    }
                    const uint64_t left  = kLeft ? amount : width - amount;
                    const uint64_t right = width - left;
                    const uint32_t high  = MaskToWidth(v, Emit(Op::kShl, a, ConstSlot(left)));
                    const uint32_t low   = Emit(Op::kLShr, b, ConstSlot(right));
                    return Emit(Op::kOr, high, low);
                }

                // A byte swap, byte by byte: the operand held at its own width,
                // each byte selected where it is and or'd in where it goes.
                if (const auto *bswap = ByteSwapOf(inst)) {
                    const uint64_t bytes  = ByteSwapBytes(*bswap);
                    const uint32_t source = MaskToWidth(v, lower(bswap->getArgOperand(0)));
                    uint32_t result       = kZeroSlot;
                    for (uint64_t index = 0; index < bytes; ++index) {
                        const uint64_t from = index * 8;
                        const uint64_t to   = (bytes - 1 - index) * 8;
                        uint32_t byte = from == 0 ? source : Emit(Op::kLShr, source, ConstSlot(from));
                        byte          = Emit(Op::kAnd, byte, ConstSlot(0xFFULL));
                        if (to != 0) {
                            byte = Emit(Op::kShl, byte, ConstSlot(to));
                        }
                        result = index == 0 ? byte : Emit(Op::kOr, result, byte);
                    }
                    return result;
                }

                // A minimum or maximum as the choice it is: the comparison of
                // the two whole words, and the `select` over it.
                if (const auto *minmax = MinMaxOf(inst)) {
                    const uint32_t a = Emit(Op::kMask, lower(minmax->getArgOperand(0)), 0);
                    const uint32_t b = Emit(Op::kMask, lower(minmax->getArgOperand(1)), 0);
                    uint32_t cond    = kZeroSlot;
                    switch (MinMaxPredicate(*minmax)) { // NOLINT(hicpp-multiway-paths-covered)
                        case llvm::CmpInst::ICMP_SGT:
                            cond = Emit(Op::kCmpSlt, b, a);
                            break;
                        case llvm::CmpInst::ICMP_SLT:
                            cond = Emit(Op::kCmpSlt, a, b);
                            break;
                        case llvm::CmpInst::ICMP_UGT:
                            cond = Emit(Op::kCmpUlt, b, a);
                            break;
                        default:
                            cond = Emit(Op::kCmpUlt, a, b);
                            break;
                    }
                    return Emit(Op::kMask, Emit3(Op::kSelect, cond, a, b), 0);
                }

                // A `ptradd` is the addition it spells, over the indices only:
                // the pointer it walks from is not part of the value the tree
                // describes, and the rewrite puts it back.
                if (ByteGepOf(inst) != nullptr) {
                    const uint32_t index = lower(inst->getOperand(1));
                    if (ByteGepOf(inst->getOperand(0)) == nullptr) {
                        return Emit(Op::kMask, index, 0);
                    }
                    return Emit(Op::kAdd, lower(inst->getOperand(0)), index);
                }

                const uint32_t lhs = lower(inst->getOperand(0));
                const uint32_t rhs = lower(inst->getOperand(1));

                // The ring operations and the left shift can carry into the
                // bits above a narrow node's width; the bitwise ones and the
                // logical right shift keep in-range operands in range.
                switch (inst->getOpcode()) {
                    case llvm::Instruction::Add:
                        return MaskToWidth(v, Emit(Op::kAdd, lhs, rhs));
                    case llvm::Instruction::Sub:
                        return MaskToWidth(v, Emit(Op::kSub, lhs, rhs));
                    case llvm::Instruction::Mul:
                        return MaskToWidth(v, Emit(Op::kMul, lhs, rhs));
                    case llvm::Instruction::And:
                        return Emit(Op::kAnd, lhs, rhs);
                    case llvm::Instruction::Or:
                        return Emit(Op::kOr, lhs, rhs);
                    case llvm::Instruction::Xor:
                        return Emit(Op::kXor, lhs, rhs);
                    case llvm::Instruction::LShr:
                        return Emit(Op::kLShr, lhs, rhs);
                    case llvm::Instruction::AShr: {
                        // The logical shift with the sign copied in
                        // (`AShrSignBit`). A shift the model has no expression
                        // for never reaches here: the collection kept it as a
                        // leaf.
                        const uint64_t sign = AShrSignBit(inst, bits_);
                        if (sign == 0) {
                            return kZeroSlot;
                        }
                        const uint32_t bit = ConstSlot(sign);
                        return MaskToWidth(
                            v, Emit(Op::kSub, Emit(Op::kXor, Emit(Op::kLShr, lhs, rhs), bit), bit)
                        );
                    }
                    case llvm::Instruction::Shl:
                        return MaskToWidth(v, Emit(Op::kShl, lhs, rhs));
                    default:
                        return kZeroSlot;
                }
            }

            uint32_t ConstSlot(uint64_t value) {
                const auto index = static_cast< uint32_t >(consts_.size());
                consts_.push_back(value & mask_);
                return Emit(Op::kConst, index, 0);
            }

            // Mask `slot` back to the width of `v` when that is narrower than
            // the tree; a node at the tree's width is already masked by its op.
            uint32_t MaskToWidth(const llvm::Value *v, uint32_t slot) {
                const uint32_t width = NarrowWidth(v, bits_);
                if (width == 0) {
                    return slot;
                }
                return Emit(Op::kAnd, slot, ConstSlot(Bitmask(width)));
            }

            // Clear the bits of leaf `index` known to be zero and set the ones
            // known to be one, as the Expr does for the same leaf.
            uint32_t ApplyLeafBits(uint32_t index, uint32_t slot) {
                if (index >= leaf_bits_.size()) {
                    return slot;
                }
                const auto &bits = leaf_bits_[index];
                if (bits.zero != 0) {
                    slot = Emit(Op::kAnd, slot, ConstSlot(~bits.zero & mask_));
                }
                if (bits.one != 0) {
                    slot = Emit(Op::kOr, slot, ConstSlot(bits.one & mask_));
                }
                return slot;
            }

            static constexpr uint32_t kZeroSlot = 0;

            std::vector< Node > nodes_;
            std::vector< uint64_t > consts_;
            std::vector< LeafBits > leaf_bits_;
            uint32_t root_ = kZeroSlot;
            uint64_t mask_ = UINT64_MAX;
            uint32_t bits_ = 64;
        };

        // `COBRA_DETECT_DEBUG=1`: why a root was not offered as a candidate.
        //
        // A root that is never built into a candidate is invisible afterwards:
        // it carries no fingerprint, so it cannot be told from one that was
        // examined and left alone, and it never reaches the re-cut ladder
        // either, because that is driven from a rejection. Naming the gate is
        // what turns "CoBRA did not simplify this" into something to act on.
        void DeclinedRoot(
            const llvm::Instruction &root, llvm::StringRef gate, size_t tree, size_t leaves
        ) {
            static const bool enabled = std::getenv("COBRA_DETECT_DEBUG") != nullptr;
            if (!enabled) {
                return;
            }
            llvm::errs() << "cobra: declined root" << root << ": " << gate << " (tree " << tree
                         << ", leaves " << leaves << ")\n";
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

                // Every operand, a `select`'s false arm and a phi's later arms
                // included. Marked first, so a walk that came back here would
                // stop rather than recurse.
                depends_on_var[v] = true;
                bool dep          = false;
                const auto *call  = llvm::dyn_cast< llvm::CallBase >(inst);
                const unsigned count =
                    call != nullptr ? call->arg_size() : inst->getNumOperands();
                for (unsigned index = 0; index < count; ++index) {
                    dep = check(inst->getOperand(index)) || dep;
                }
                depends_on_var[v] = dep;
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

        // Logical negation of a 0/1 value. Expr::BitwiseNot is bitwise, so it
        // would map 1 to all-ones-but-the-low-bit rather than to 0.
        std::unique_ptr< Expr > LogicalNot(std::unique_ptr< Expr > cond) {
            return Expr::BitwiseXor(std::move(cond), Expr::Constant(1));
        }

        // Expr carries only equality, unsigned-less-than and signed-less-than.
        // The greater-than forms swap operands and the or-equal forms negate the
        // strict comparison with the operands swapped.
        std::unique_ptr< Expr > BuildComparison(
            llvm::CmpInst::Predicate pred, std::unique_ptr< Expr > lhs,
            std::unique_ptr< Expr > rhs
        ) {
            switch (pred) { // NOLINT(hicpp-multiway-paths-covered)
                case llvm::CmpInst::ICMP_EQ:
                    return Expr::CmpEq(std::move(lhs), std::move(rhs));
                case llvm::CmpInst::ICMP_NE:
                    return LogicalNot(Expr::CmpEq(std::move(lhs), std::move(rhs)));
                case llvm::CmpInst::ICMP_ULT:
                    return Expr::CmpUlt(std::move(lhs), std::move(rhs));
                case llvm::CmpInst::ICMP_UGT:
                    return Expr::CmpUlt(std::move(rhs), std::move(lhs));
                case llvm::CmpInst::ICMP_ULE:
                    return LogicalNot(Expr::CmpUlt(std::move(rhs), std::move(lhs)));
                case llvm::CmpInst::ICMP_UGE:
                    return LogicalNot(Expr::CmpUlt(std::move(lhs), std::move(rhs)));
                case llvm::CmpInst::ICMP_SLT:
                    return Expr::CmpSlt(std::move(lhs), std::move(rhs));
                case llvm::CmpInst::ICMP_SGT:
                    return Expr::CmpSlt(std::move(rhs), std::move(lhs));
                case llvm::CmpInst::ICMP_SLE:
                    return LogicalNot(Expr::CmpSlt(std::move(rhs), std::move(lhs)));
                case llvm::CmpInst::ICMP_SGE:
                    return LogicalNot(Expr::CmpSlt(std::move(lhs), std::move(rhs)));
                default:
                    return nullptr;
            }
        }

        // Removes a value from the in-progress path on the way back out, so a
        // node shared by sibling operands is still expanded for each of them.
        class PathScope
        {
          public:
            PathScope(llvm::DenseSet< llvm::Value * > &set, llvm::Value *key)
                : set_(set), key_(key) {}

            ~PathScope() { set_.erase(key_); }

            PathScope(const PathScope &)            = delete;
            PathScope &operator=(const PathScope &) = delete;
            PathScope(PathScope &&)                 = delete;
            PathScope &operator=(PathScope &&)      = delete;

          private:
            llvm::DenseSet< llvm::Value * > &set_;
            llvm::Value *key_;
        };

        // An Expr spells the operand graph out as a tree, so a value with two
        // readers appears twice in it and a chain of such values doubles at
        // every level. Past this many nodes the tree is nothing the solver
        // could use, and building it is what runs out of memory; the
        // candidate then carries no Expr, like one holding a select, and only
        // its signature is worked on.
        constexpr size_t kMaxExprNodes = 4096;

        // A copy of `expr` charged to `budget`, or nothing when the budget
        // cannot pay for it. Lowering a `select`, a minimum or maximum, or a
        // byte swap writes an operand more than once, and a copy costs what the
        // original did: uncharged, a chain of k selects held 2^k nodes and one
        // of min/max 3^k, all inside a budget that only counted the IR visited.
        std::unique_ptr< Expr > ChargedClone(const Expr &expr, size_t &budget) {
            const size_t nodes = CountNodes(expr);
            if (nodes > budget) {
                budget = 0;
                return nullptr;
            }
            budget -= nodes;
            return CloneExpr(expr);
        }

        std::unique_ptr< Expr > BuildExprFromIRImpl(
            llvm::Value *v, const std::vector< llvm::Value * > &leaves,
            const llvm::DenseSet< llvm::Value * > &tree_set, uint64_t mask,
            const llvm::DenseMap< llvm::Value *, llvm::Value * > &phi_redirects,
            llvm::DenseSet< llvm::Value * > &in_progress, size_t &budget,
            const std::vector< LeafBits > &leaf_bits
        ) {
            if (budget == 0) {
                return nullptr;
            }
            --budget;

            // Following a transparent phi can lead back to a value already on
            // the current path, so the operand graph is not necessarily a tree.
            // TreeEvalPlan folds such a back edge to literal zero; do the same
            // here, because the Expr has to describe the function the evaluator
            // probes or verification would compare against a different original.
            if (!in_progress.insert(v).second) {
                return Expr::Constant(0);
            }
            const PathScope kPathScope{ in_progress, v };

            // Phi redirect: build from the chosen arm.
            auto pit = phi_redirects.find(v);
            if (pit != phi_redirects.end()) {
                return BuildExprFromIRImpl(
                    pit->second, leaves, tree_set, mask, phi_redirects, in_progress, budget, leaf_bits
                );
            }

            // Constant
            if (auto *ci = llvm::dyn_cast< llvm::ConstantInt >(v)) {
                return Expr::Constant(LowWord(ci) & mask);
            }

            const auto tree_width = static_cast< uint32_t >(std::popcount(mask));
            // The Expr must describe the same function the evaluator probes,
            // so a value narrower than the tree is masked exactly where the
            // plan masks it.
            const auto mask_to_width = [&](std::unique_ptr< Expr > expr) {
                const uint32_t width = NarrowWidth(v, tree_width);
                if (width == 0) {
                    return expr;
                }
                return Expr::BitwiseAnd(std::move(expr), Expr::Constant(Bitmask(width)));
            };

            // Leaf (variable), masked by what is known of its bits exactly as
            // the probe plan masks the input it hands the same leaf.
            auto leaf_it = std::find(leaves.begin(), leaves.end(), v);
            if (leaf_it != leaves.end()) {
                auto idx   = static_cast< uint32_t >(leaf_it - leaves.begin());
                auto leaf  = mask_to_width(Expr::Variable(idx));
                if (idx < leaf_bits.size()) {
                    const auto &bits = leaf_bits[idx];
                    if (bits.zero != 0) {
                        leaf = Expr::BitwiseAnd(std::move(leaf), Expr::Constant(~bits.zero & mask));
                    }
                    if (bits.one != 0) {
                        leaf = Expr::BitwiseOr(std::move(leaf), Expr::Constant(bits.one & mask));
                    }
                }
                return leaf;
            }

            auto *inst = llvm::dyn_cast< llvm::Instruction >(v);
            if (inst == nullptr || !tree_set.contains(inst)) {
                return nullptr;
            }

            // ZExt/SExt — pass through to inner operand
            if (inst->getOpcode() == llvm::Instruction::ZExt
                || inst->getOpcode() == llvm::Instruction::SExt)
            {
                auto child = BuildExprFromIRImpl(
                    inst->getOperand(0), leaves, tree_set, mask, phi_redirects, in_progress, budget, leaf_bits
                );
                if (child == nullptr) {
                    return nullptr;
                }
                // A sign extension is `(v ^ 2^(n-1)) - 2^(n-1)` on the
                // zero-extended value (see `FitsTreeWidth`); passing it
                // through unchanged would model a zero extension instead. A
                // node wider than the tree passes through, because it is only
                // admitted when its source covers the tree's bits.
                if (const uint64_t sign = SignBitOfExtensionSource(inst, tree_width);
                    inst->getOpcode() == llvm::Instruction::SExt && sign != 0)
                {
                    auto flipped = Expr::BitwiseXor(std::move(child), Expr::Constant(sign));
                    return mask_to_width(
                        Expr::Add(std::move(flipped), Expr::Constant((~sign + 1) & mask))
                    );
                }
                return child;
            }

            // Trunc keeps the low bits: a mask at the width it narrows to.
            if (inst->getOpcode() == llvm::Instruction::Trunc) {
                auto child = BuildExprFromIRImpl(
                    inst->getOperand(0), leaves, tree_set, mask, phi_redirects, in_progress, budget, leaf_bits
                );
                if (child == nullptr) {
                    return nullptr;
                }
                return Expr::BitwiseAnd(
                    std::move(child), Expr::Constant(Bitmask(inst->getType()->getIntegerBitWidth()))
                );
            }

            if (auto *cmp = llvm::dyn_cast< llvm::ICmpInst >(inst)) {
                auto lhs = BuildExprFromIRImpl(
                    cmp->getOperand(0), leaves, tree_set, mask, phi_redirects, in_progress, budget, leaf_bits
                );
                auto rhs = BuildExprFromIRImpl(
                    cmp->getOperand(1), leaves, tree_set, mask, phi_redirects, in_progress, budget, leaf_bits
                );
                if (lhs == nullptr || rhs == nullptr) {
                    return nullptr;
                }
                return BuildComparison(cmp->getPredicate(), std::move(lhs), std::move(rhs));
            }

            if (inst->getOpcode() == llvm::Instruction::Select) {
                auto cond = BuildExprFromIRImpl(
                    inst->getOperand(0), leaves, tree_set, mask, phi_redirects, in_progress, budget, leaf_bits
                );
                auto then_arm = BuildExprFromIRImpl(
                    inst->getOperand(1), leaves, tree_set, mask, phi_redirects, in_progress, budget, leaf_bits
                );
                auto else_arm = BuildExprFromIRImpl(
                    inst->getOperand(2), leaves, tree_set, mask, phi_redirects, in_progress, budget, leaf_bits
                );
                if (cond == nullptr || then_arm == nullptr || else_arm == nullptr) {
                    return nullptr;
                }
                // select(c, A, B) == B ^ ((A ^ B) & -c), because a 0/1 condition
                // negates to all-ones or zero. Written in existing operators so
                // no pass has to handle a three-child node. The clone is taken
                // before `else_arm` is moved from, since argument evaluation
                // order is unspecified.
                auto else_copy = ChargedClone(*else_arm, budget);
                if (else_copy == nullptr) {
                    return nullptr;
                }
                auto diff   = Expr::BitwiseXor(std::move(then_arm), std::move(else_copy));
                auto masked = Expr::BitwiseAnd(std::move(diff), Expr::Negate(std::move(cond)));
                return Expr::BitwiseXor(std::move(else_arm), std::move(masked));
            }

            // LShr with constant shift amount
            if (inst->getOpcode() == llvm::Instruction::LShr) {
                auto *shift_amt = llvm::dyn_cast< llvm::ConstantInt >(inst->getOperand(1));
                if (shift_amt == nullptr) {
                    return nullptr;
                }
                auto child = BuildExprFromIRImpl(
                    inst->getOperand(0), leaves, tree_set, mask, phi_redirects, in_progress, budget, leaf_bits
                );
                if (child == nullptr) {
                    return nullptr;
                }
                return Expr::LogicalShr(std::move(child), shift_amt->getZExtValue());
            }

            // AShr with constant shift amount: the logical shift with the sign
            // copied into the bits it vacated (`AShrSignBit`), written with the
            // same two's complement the sign extension above uses.
            if (inst->getOpcode() == llvm::Instruction::AShr) {
                const uint64_t sign = AShrSignBit(inst, tree_width);
                const auto *shift_amt = llvm::dyn_cast< llvm::ConstantInt >(inst->getOperand(1));
                if (sign == 0 || shift_amt == nullptr) {
                    return nullptr;
                }
                auto child = BuildExprFromIRImpl(
                    inst->getOperand(0), leaves, tree_set, mask, phi_redirects, in_progress,
                    budget, leaf_bits
                );
                if (child == nullptr) {
                    return nullptr;
                }
                auto shifted =
                    Expr::LogicalShr(std::move(child), shift_amt->getZExtValue());
                auto flipped = Expr::BitwiseXor(std::move(shifted), Expr::Constant(sign));
                return mask_to_width(
                    Expr::Add(std::move(flipped), Expr::Constant((~sign + 1) & mask))
                );
            }

            // Shl with constant shift amount
            if (inst->getOpcode() == llvm::Instruction::Shl) {
                auto *shift_amt = llvm::dyn_cast< llvm::ConstantInt >(inst->getOperand(1));
                if (shift_amt == nullptr) {
                    return nullptr;
                }
                auto child = BuildExprFromIRImpl(
                    inst->getOperand(0), leaves, tree_set, mask, phi_redirects, in_progress, budget, leaf_bits
                );
                if (child == nullptr) {
                    return nullptr;
                }
                // `shl` by the width or more is poison; 0 refines it and keeps
                // the C++ shift defined.
                const uint64_t amount  = shift_amt->getValue().getLimitedValue(64);
                uint64_t mul_val       = amount >= 64 ? 0 : 1ULL << amount;
                return mask_to_width(Expr::Mul(std::move(child), Expr::Constant(mul_val)));
            }

            // A freeze is the identity on a defined operand, and the model
            // only ever hands it defined operands.
            if (IsTransparentFreeze(inst)) {
                return BuildExprFromIRImpl(
                    inst->getOperand(0), leaves, tree_set, mask, phi_redirects, in_progress,
                    budget, leaf_bits
                );
            }

            // A funnel shift by a constant: the high part shifted up, masked to
            // the width, or'd with the low part shifted down.
            if (const auto *fsh = FunnelShiftOf(inst)) {
                const auto [amount, width] = FunnelShiftAmount(*fsh);
                const bool kLeft           = fsh->getIntrinsicID() == llvm::Intrinsic::fshl;
                auto a                     = BuildExprFromIRImpl(
                    fsh->getArgOperand(0), leaves, tree_set, mask, phi_redirects, in_progress, budget, leaf_bits
                );
                auto b = BuildExprFromIRImpl(
                    fsh->getArgOperand(1), leaves, tree_set, mask, phi_redirects, in_progress, budget, leaf_bits
                );
                if (a == nullptr || b == nullptr) {
                    return nullptr;
                }
                if (amount == 0) {
                    return kLeft ? std::move(a) : std::move(b);
                }
                const uint64_t left  = kLeft ? amount : width - amount;
                const uint64_t right = width - left;
                auto high = mask_to_width(Expr::Mul(std::move(a), Expr::Constant(1ULL << left)));
                auto low  = Expr::LogicalShr(std::move(b), right);
                return Expr::BitwiseOr(std::move(high), std::move(low));
            }

            // A byte swap, byte by byte. The operand appears once per byte, so a
            // wide swap over a large subtree is what the node budget is for: at
            // eight bytes this is eight copies of whatever it permutes, and past
            // the budget the value stays a leaf rather than a tree nothing can
            // use.
            if (const auto *bswap = ByteSwapOf(inst)) {
                const uint64_t bytes = ByteSwapBytes(*bswap);
                auto source          = BuildExprFromIRImpl(
                    bswap->getArgOperand(0), leaves, tree_set, mask, phi_redirects, in_progress,
                    budget, leaf_bits
                );
                if (source == nullptr) {
                    return nullptr;
                }
                std::unique_ptr< Expr > result;
                for (uint64_t index = 0; index < bytes; ++index) {
                    if (budget == 0) {
                        return nullptr;
                    }
                    --budget;
                    const uint64_t from = index * 8;
                    const uint64_t to   = (bytes - 1 - index) * 8;
                    auto byte           = ChargedClone(*source, budget);
                    if (byte == nullptr) {
                        return nullptr;
                    }
                    if (from != 0) {
                        byte = Expr::LogicalShr(std::move(byte), from);
                    }
                    byte = Expr::BitwiseAnd(std::move(byte), Expr::Constant(0xFFULL));
                    if (to != 0) {
                        byte = Expr::Mul(std::move(byte), Expr::Constant(1ULL << to));
                    }
                    result = result == nullptr ? std::move(byte)
                                               : Expr::BitwiseOr(std::move(result), std::move(byte));
                }
                return result;
            }

            // A minimum or maximum: the comparison of the two whole words, and
            // the `select` over it, written the way the `select` case writes one.
            if (const auto *minmax = MinMaxOf(inst)) {
                auto a = BuildExprFromIRImpl(
                    minmax->getArgOperand(0), leaves, tree_set, mask, phi_redirects, in_progress,
                    budget, leaf_bits
                );
                auto b = BuildExprFromIRImpl(
                    minmax->getArgOperand(1), leaves, tree_set, mask, phi_redirects, in_progress,
                    budget, leaf_bits
                );
                if (a == nullptr || b == nullptr) {
                    return nullptr;
                }
                auto a_copy = ChargedClone(*a, budget);
                auto b_copy = a_copy != nullptr ? ChargedClone(*b, budget) : nullptr;
                auto b_again = b_copy != nullptr ? ChargedClone(*b, budget) : nullptr;
                if (b_again == nullptr) {
                    return nullptr;
                }
                auto cond = BuildComparison(
                    MinMaxPredicate(*minmax), std::move(a_copy), std::move(b_copy)
                );
                if (cond == nullptr) {
                    return nullptr;
                }
                // `select(c, A, B) == B ^ ((A ^ B) & -c)`, as above.
                auto diff   = Expr::BitwiseXor(std::move(a), std::move(b_again));
                auto masked = Expr::BitwiseAnd(std::move(diff), Expr::Negate(std::move(cond)));
                return Expr::BitwiseXor(std::move(b), std::move(masked));
            }

            // A `ptradd`, read as the addition of its indices (see `ByteGepOf`).
            if (ByteGepOf(inst) != nullptr) {
                auto index = BuildExprFromIRImpl(
                    inst->getOperand(1), leaves, tree_set, mask, phi_redirects, in_progress,
                    budget, leaf_bits
                );
                if (index == nullptr) {
                    return nullptr;
                }
                if (ByteGepOf(inst->getOperand(0)) == nullptr) {
                    return mask_to_width(std::move(index));
                }
                auto below = BuildExprFromIRImpl(
                    inst->getOperand(0), leaves, tree_set, mask, phi_redirects, in_progress,
                    budget, leaf_bits
                );
                if (below == nullptr) {
                    return nullptr;
                }
                return mask_to_width(Expr::Add(std::move(below), std::move(index)));
            }

            // Binary operations
            auto lhs = BuildExprFromIRImpl(
                inst->getOperand(0), leaves, tree_set, mask, phi_redirects, in_progress, budget, leaf_bits
            );
            auto rhs = BuildExprFromIRImpl(
                inst->getOperand(1), leaves, tree_set, mask, phi_redirects, in_progress, budget, leaf_bits
            );
            if (lhs == nullptr || rhs == nullptr) {
                return nullptr;
            }

            switch (inst->getOpcode()) {
                case llvm::Instruction::Add:
                    return mask_to_width(Expr::Add(std::move(lhs), std::move(rhs)));
                case llvm::Instruction::Sub:
                    return mask_to_width(
                        Expr::Add(std::move(lhs), Expr::Negate(std::move(rhs)))
                    );
                case llvm::Instruction::Mul:
                    return mask_to_width(Expr::Mul(std::move(lhs), std::move(rhs)));
                case llvm::Instruction::And:
                    return Expr::BitwiseAnd(std::move(lhs), std::move(rhs));
                case llvm::Instruction::Or:
                    return Expr::BitwiseOr(std::move(lhs), std::move(rhs));
                case llvm::Instruction::Xor: {
                    // Detect NOT: xor %x, -1. Only at the tree's width: a NOT
                    // there sets every bit, whereas a narrower all-ones
                    // constant is the mask of its own width and stays an xor.
                    if (NarrowWidth(v, tree_width) == 0) {
                        if (auto *ci =
                                llvm::dyn_cast< llvm::ConstantInt >(inst->getOperand(1))) {
                            if (ci->isAllOnesValue()) {
                                return Expr::BitwiseNot(std::move(lhs));
                            }
                        }
                        if (auto *ci =
                                llvm::dyn_cast< llvm::ConstantInt >(inst->getOperand(0))) {
                            if (ci->isAllOnesValue()) {
                                return Expr::BitwiseNot(std::move(rhs));
                            }
                        }
                    }
                    return Expr::BitwiseXor(std::move(lhs), std::move(rhs));
                }
                default:
                    return nullptr;
            }
        }

        std::unique_ptr< Expr > BuildExprFromIR(
            llvm::Value *v, const std::vector< llvm::Value * > &leaves,
            const llvm::DenseSet< llvm::Value * > &tree_set, uint64_t mask,
            const llvm::DenseMap< llvm::Value *, llvm::Value * > &phi_redirects,
            const std::vector< LeafBits > &leaf_bits = kNoLeafBits
        ) {
            llvm::DenseSet< llvm::Value * > in_progress;
            size_t budget = kMaxExprNodes;
            return BuildExprFromIRImpl(
                v, leaves, tree_set, mask, phi_redirects, in_progress, budget, leaf_bits
            );
        }

        // What became of the transparent phis the collection checked
        // (`VerifyPhiArms`), for the report.
        std::atomic< uint64_t > phi_arms_checked{ 0 };
        std::atomic< uint64_t > phi_arms_exact_accept{ 0 };
        std::atomic< uint64_t > phi_arms_leaf_unavailable{ 0 };

        // Whether `a` and `b` compute the same value: the very same value, or
        // the same operation - opcode, types, predicate, callee and attributes,
        // as `isSameOperationAs` compares them - over operands that are in turn
        // the same computation. Equal for every input by construction, which a
        // probe is not: arms that differ only where an `icmp eq` or a `select`
        // flips agree on almost every random input.
        //
        // A phi is only ever the same as itself, and nothing that touches
        // memory is followed, since the same load on two paths can read two
        // different things. Bounded: past `budget` pairs the answer is no.
        // For one transparent phi: per value of the followed arm, the arms whose
        // computation reaches it as the very same value (see `SameComputation`).
        using ArmIdentity = llvm::DenseMap< llvm::Value *, llvm::SmallBitVector >;

        bool SameComputation(
            llvm::Value *a, llvm::Value *b,
            llvm::DenseSet< std::pair< llvm::Value *, llvm::Value * > > &proven, unsigned &budget,
            ArmIdentity &identity, unsigned arm, unsigned arms
        ) {
            if (a == b) {
                auto &bits = identity[b];
                if (bits.size() != arms) {
                    bits.resize(arms);
                }
                bits.set(arm);
                return true;
            }
            if (a->getType() != b->getType()) {
                return false;
            }
            auto *ia = llvm::dyn_cast< llvm::Instruction >(a);
            auto *ib = llvm::dyn_cast< llvm::Instruction >(b);
            if (ia == nullptr || ib == nullptr || llvm::isa< llvm::PHINode >(ia)
                || llvm::isa< llvm::PHINode >(ib) || ia->mayReadOrWriteMemory()
                || ib->mayReadOrWriteMemory() || !ia->isSameOperationAs(ib))
            {
                return false;
            }
            if (budget == 0) {
                return false;
            }
            --budget;
            if (!proven.insert({ a, b }).second) {
                return true;
            }
            for (unsigned i = 0; i < ia->getNumOperands(); ++i) {
                if (!SameComputation(
                        ia->getOperand(i), ib->getOperand(i), proven, budget, identity, arm, arms
                    ))
                {
                    return false;
                }
            }
            return true;
        }

        // Whether every value the tree reads as a leaf is available at the
        // root, which is where the rewrite reads it.
        //
        // A tree read through a transparent phi follows one arm, and that arm
        // is computed in a predecessor: a leaf inside it - a variable shift, a
        // load, anything the walk stops at - is defined on that path only,
        // however surely the other arms compute the same thing from their own
        // copies of it. A value dominates the phi, and so the root, when every
        // arm's computation reaches it as the very same value, at it or at a
        // value above it in the followed arm (each arm's value is available at
        // the end of its own predecessor, and so is everything it is computed
        // from). So the walk from the root marks what it reaches as available,
        // and at a transparent phi carries on only into the part of the
        // followed arm every arm shares.
        bool LeavesAvailableAtRoot(
            llvm::Instruction *root,
            const llvm::DenseMap< llvm::Value *, llvm::Value * > &phi_redirects,
            const llvm::DenseMap< const llvm::PHINode *, ArmIdentity > &identities,
            llvm::ArrayRef< llvm::Instruction * > tree_insts, const std::vector< llvm::Value * > &leaves
        ) {
            const llvm::DenseSet< const llvm::Value * > tree(tree_insts.begin(), tree_insts.end());
            const llvm::DenseSet< const llvm::Value * > leaf_set(leaves.begin(), leaves.end());
            llvm::DenseSet< llvm::Value * > available;
            llvm::SmallVector< llvm::Value *, 32 > work{ root };
            while (!work.empty()) {
                llvm::Value *value = work.pop_back_val();
                if (!available.insert(value).second || leaf_set.contains(value)) {
                    continue;
                }
                auto *inst = llvm::dyn_cast< llvm::Instruction >(value);
                if (inst == nullptr) {
                    continue;
                }
                if (auto redirect = phi_redirects.find(value); redirect != phi_redirects.end()) {
                    // A reading a sum group asked for stands for a constant.
                    const auto *phi = llvm::dyn_cast< llvm::PHINode >(value);
                    if (phi == nullptr) {
                        continue;
                    }
                    const auto found = identities.find(phi);
                    if (found == identities.end()) {
                        continue;
                    }
                    const ArmIdentity &identity = found->second;
                    const unsigned arms         = phi->getNumIncomingValues();
                    // The arms reaching each value of the followed arm as itself,
                    // at it or above it.
                    llvm::DenseMap< llvm::Value *, llvm::SmallBitVector > covered;
                    llvm::SmallVector< std::pair< llvm::Value *, llvm::SmallBitVector >, 32 > down;
                    down.push_back({ redirect->second, llvm::SmallBitVector(arms) });
                    while (!down.empty()) {
                        auto [node, from_above] = down.pop_back_val();
                        if (auto it = identity.find(node); it != identity.end()) {
                            from_above |= it->second;
                        }
                        auto [entry, inserted] = covered.try_emplace(node, arms);
                        const llvm::SmallBitVector before = entry->second;
                        entry->second |= from_above;
                        if (!inserted && entry->second == before) {
                            continue;
                        }
                        if (entry->second.all()) {
                            work.push_back(node);
                            continue;
                        }
                        auto *below = llvm::dyn_cast< llvm::Instruction >(node);
                        if (below == nullptr || !tree.contains(below) || llvm::isa< llvm::PHINode >(below)) {
                            continue;
                        }
                        for (llvm::Value *operand : below->operands()) {
                            down.push_back({ operand, entry->second });
                        }
                    }
                    continue;
                }
                if (!tree.contains(inst)) {
                    continue;
                }
                for (llvm::Value *operand : inst->operands()) {
                    work.push_back(operand);
                }
            }
            return llvm::all_of(leaves, [&](llvm::Value *leaf) {
                return !llvm::isa< llvm::Instruction >(leaf) || available.contains(leaf);
            });
        }

        // Whether every arm of every transparent phi is the same computation
        // as the arm the collection followed, recording for each phi which
        // values every arm reaches as themselves (`LeavesAvailableAtRoot`).
        bool PhiArmsAreTheSameComputation(
            const llvm::DenseMap< llvm::Value *, llvm::Value * > &phi_redirects,
            llvm::DenseMap< const llvm::PHINode *, ArmIdentity > &identities
        ) {
            llvm::DenseSet< std::pair< llvm::Value *, llvm::Value * > > proven;
            unsigned budget = 4096;
            for (const auto &[phi_val, chosen] : phi_redirects) {
                // The map also carries the readings a sum group asked for
                // (`CollectTree`'s `reads_as`), which are not phis and have no
                // arms to agree.
                auto *phi = llvm::dyn_cast< llvm::PHINode >(phi_val);
                if (phi == nullptr) {
                    continue;
                }
                auto &identity      = identities[phi];
                const unsigned arms = phi->getNumIncomingValues();
                for (unsigned arm = 0; arm < arms; ++arm) {
                    // Per arm: a pair already proved for another arm says
                    // nothing about which values this one shares.
                    proven.clear();
                    if (!SameComputation(
                            phi->getIncomingValue(arm), chosen, proven, budget, identity, arm, arms
                        ))
                    {
                        return false;
                    }
                }
            }
            return true;
        }

        // Whether the collection may read each transparent phi as the arm it
        // followed. The tree is that arm, so every other arm has to be equal
        // to it on every input, or the expression is not the value.
        //
        // This used to be decided by evaluating the arms on eight random
        // inputs. That is no proof: arms that differ only where a comparison
        // or a `select` flips agree on almost every random input, and the
        // other arms' operations were evaluated without the width checks the
        // followed arm passed (a `sext` read as a `zext`). Either way the
        // rewrite was proved against an expression only one path computes.
        // Now every other arm has to be the same computation, which is exact.
        //
        // The arms agreeing is not all: the tree reads its leaves at the root,
        // so each has to be available there (`LeavesAvailableAtRoot`), and a
        // phi whose followed arm reads one that is not is a leaf too.
        bool VerifyPhiArms(
            llvm::Instruction *root,
            const llvm::DenseMap< llvm::Value *, llvm::Value * > &phi_redirects,
            llvm::ArrayRef< llvm::Instruction * > tree_insts,
            const std::vector< llvm::Value * > &leaves
        ) {
            if (phi_redirects.empty()) {
                return true;
            }
            llvm::DenseMap< const llvm::PHINode *, ArmIdentity > identities;
            const bool exact = PhiArmsAreTheSameComputation(phi_redirects, identities);
            if (exact && !LeavesAvailableAtRoot(root, phi_redirects, identities, tree_insts, leaves)) {
                phi_arms_leaf_unavailable.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            phi_arms_checked.fetch_add(1, std::memory_order_relaxed);
            phi_arms_exact_accept.fetch_add(exact ? 1 : 0, std::memory_order_relaxed);
            return exact;
        }

    } // namespace

    namespace {

        // One sweep of the candidate builder over `roots`, in the order given.
        // A root whose tree swallows a later root wins: the inner one is only
        // reconsidered if the outer candidate goes on to fail, which the caller
        // arranges by feeding `MBACandidate::inner_roots` back in.
        //
        // `shared_cut`, when given, is one rung of `SharedCutMbaCandidate`: the
        // values to keep as leaves and the `node_limit` that names the rung.
        struct SharedCut
        {
            llvm::DenseSet< const llvm::Value * > opaque;
            uint32_t node_limit = kSharedCutLimit;
        };

        // What a slice candidate is collected for: the width to read the root
        // at, and how the root is recovered from those bits (see
        // `SliceMbaCandidate`).
        struct Slice
        {
            uint32_t width            = 0;
            SliceExtension extension = SliceExtension::kNone;
        };

        // What a sum group is collected for: the root's other summands, each
        // with the sign it stands at in the root's sum, and which group of the
        // split this is (see `SumGroupSliceMbaCandidates`). The collection
        // reads every one of them as zero, so the candidate is the group's own
        // sum; the rewrite adds them back.
        struct SumGroup
        {
            std::vector< std::pair< llvm::Value *, bool > > residual;
            uint32_t index = 0;
        };

        // Whether `inst` stands for the sum of its two operands, so that a
        // chain above it may be split there. `or` does only when it is
        // disjoint - the flag that says its operands share no bit, which is
        // what makes it an addition; without it a split there would not be the
        // root's value. A constant operand stops the split: a constant has no
        // leaf support to be grouped by, and reading it as zero would read
        // every other occurrence of that same constant as zero with it.
        bool IsSplittableSum(const llvm::Instruction *inst) {
            if (inst->getNumOperands() != 2
                || llvm::isa< llvm::Constant >(inst->getOperand(0))
                || llvm::isa< llvm::Constant >(inst->getOperand(1)))
            {
                return false;
            }
            switch (inst->getOpcode()) {
                case llvm::Instruction::Add:
                case llvm::Instruction::Sub: return true;
                case llvm::Instruction::Or:
                    return llvm::cast< llvm::PossiblyDisjointInst >(inst)->isDisjoint();
                default: return false;
            }
        }

        // The candidate's leaves that `v`'s cone reaches - or nothing, when the
        // cone holds a value the candidate's own lists do not name.
        //
        // A phi the collection read through is in neither list: it is not an
        // instruction of the tree and it is not a leaf, only a reading recorded
        // beside them. The leaves beneath such a value are unknown, and a
        // grouping that put a term holding one apart from another could leave a
        // summand read as zero inside the group that is kept - which would drop
        // that summand from the root's value altogether. So an unknown cone
        // refuses the split rather than guessing at it.
        std::optional< llvm::SmallBitVector > LeafSupportOf(
            llvm::Value *v, const llvm::DenseMap< const llvm::Value *, unsigned > &leaf_index,
            const llvm::DenseSet< const llvm::Value * > &tree,
            llvm::DenseMap< llvm::Value *, llvm::SmallBitVector > &memo
        ) {
            if (auto it = memo.find(v); it != memo.end()) {
                return it->second;
            }
            llvm::SmallBitVector support(leaf_index.size());
            llvm::SmallVector< llvm::Value *, 32 > work{ v };
            llvm::DenseSet< llvm::Value * > seen;
            while (!work.empty()) {
                llvm::Value *node = work.pop_back_val();
                if (!seen.insert(node).second) {
                    continue;
                }
                if (auto it = leaf_index.find(node); it != leaf_index.end()) {
                    support.set(it->second);
                    continue;
                }
                if (llvm::isa< llvm::Constant >(node)) {
                    continue;
                }
                auto *inst = llvm::dyn_cast< llvm::Instruction >(node);
                if (inst == nullptr || !tree.contains(inst)) {
                    return std::nullopt;
                }
                for (auto &op : inst->operands()) {
                    work.push_back(op.get());
                }
            }
            memo.try_emplace(v, support);
            return support;
        }

        // The root's sum, split into groups no two of which share a leaf.
        struct SumSplit
        {
            // One entry per summand: the value, and whether it stands negated
            // in the root's sum.
            std::vector< std::pair< llvm::Value *, bool > > terms;
            // One entry per summand: which group it belongs to.
            std::vector< uint32_t > group;
            uint32_t groups = 0;
        };

        // How many summands a chain is taken apart into before the split is
        // given up on. The chain is read only as far as it takes to separate
        // the groups, so this is a guard and not a budget that shapes the
        // answer.
        constexpr size_t kMaxSumTerms = 16;

        // Read `cand`'s root as a sum and group its summands by the leaves they
        // reach.
        //
        // The chain is opened one node at a time and only while the groups have
        // not come apart: the shallowest reading that separates them is the one
        // kept, so a base the function already computes as one value stays one
        // summand and the rewrite reads it back instead of building it again.
        // The node opened is the one reaching the most leaves, which is the one
        // most likely to be holding two groups together.
        std::optional< SumSplit > SplitRootSum(const MBACandidate &cand) {
            if (cand.root == nullptr || !IsSplittableSum(cand.root)) {
                return std::nullopt;
            }
            llvm::DenseMap< const llvm::Value *, unsigned > leaf_index;
            for (size_t i = 0; i < cand.leaf_values.size(); ++i) {
                leaf_index.try_emplace(cand.leaf_values[i], static_cast< unsigned >(i));
            }
            const llvm::DenseSet< const llvm::Value * > tree(cand.tree.begin(), cand.tree.end());
            llvm::DenseMap< llvm::Value *, llvm::SmallBitVector > memo;

            std::vector< std::pair< llvm::Value *, bool > > terms{ { cand.root, false } };
            for (;;) {
                std::vector< llvm::SmallBitVector > support;
                support.reserve(terms.size());
                for (const auto &[term, negated] : terms) {
                    auto reached = LeafSupportOf(term, leaf_index, tree, memo);
                    // A term reaching no leaf at all is a value computed from
                    // constants: reading it as zero is not the same as leaving
                    // it out, so there is nothing to group it by either.
                    if (!reached || reached->none()) {
                        return std::nullopt;
                    }
                    support.push_back(std::move(*reached));
                }

                // Terms that share a leaf are one group, transitively.
                std::vector< uint32_t > parent(terms.size());
                std::iota(parent.begin(), parent.end(), 0U);
                const auto find = [&](uint32_t x) {
                    while (parent[x] != x) {
                        parent[x] = parent[parent[x]];
                        x         = parent[x];
                    }
                    return x;
                };
                for (size_t i = 0; i < terms.size(); ++i) {
                    for (size_t j = i + 1; j < terms.size(); ++j) {
                        if (support[i].anyCommon(support[j])) {
                            parent[find(static_cast< uint32_t >(i))] =
                                find(static_cast< uint32_t >(j));
                        }
                    }
                }
                llvm::DenseMap< uint32_t, uint32_t > label;
                SumSplit split;
                split.group.resize(terms.size());
                for (size_t i = 0; i < terms.size(); ++i) {
                    const auto root = find(static_cast< uint32_t >(i));
                    const auto [it, fresh] = label.try_emplace(root, split.groups);
                    if (fresh) {
                        ++split.groups;
                    }
                    split.group[i] = it->second;
                }
                if (split.groups >= 2) {
                    split.terms = std::move(terms);
                    return split;
                }

                // One group still: open the summand reaching the most leaves.
                size_t widest     = terms.size();
                unsigned reaching = 0;
                for (size_t i = 0; i < terms.size(); ++i) {
                    auto *inst = llvm::dyn_cast< llvm::Instruction >(terms[i].first);
                    if (inst == nullptr || !tree.contains(inst) || !IsSplittableSum(inst)) {
                        continue;
                    }
                    const auto count = static_cast< unsigned >(support[i].count());
                    if (widest == terms.size() || count > reaching) {
                        widest   = i;
                        reaching = count;
                    }
                }
                if (widest == terms.size() || terms.size() + 1 > kMaxSumTerms) {
                    return std::nullopt;
                }
                auto *node          = llvm::cast< llvm::Instruction >(terms[widest].first);
                const bool negated  = terms[widest].second;
                const bool subtract = node->getOpcode() == llvm::Instruction::Sub;
                terms[widest]       = { node->getOperand(0), negated };
                terms.push_back({ node->getOperand(1), negated != subtract });
            }
        }

        void BuildCandidates(
            llvm::ArrayRef< llvm::Instruction * > roots, uint32_t min_ast_size,
            uint32_t max_vars, MbaCostModel cost_model, uint64_t options_tag,
            std::vector< MBACandidate > &candidates, uint32_t max_nodes = 0,
            bool cut_arith_under_bitwise = false, uint32_t max_tree_nodes = 0,
            const DemandedMaskFn &demanded = {}, const SharedCut *shared_cut = nullptr,
            const Slice *slice = nullptr, const SumGroup *sum_group = nullptr,
            bool boolean_cut = false
        ) {
            const auto *opaque = shared_cut != nullptr ? &shared_cut->opaque : nullptr;
            // The summands this group leaves out, read as zero wherever the
            // collection meets them. Zero is the identity of every operation
            // the sum chain is built from, and a summand stands in that chain
            // once, so what the root reads as is the group's own sum.
            llvm::DenseMap< llvm::Value *, llvm::Value * > zeroed;
            if (sum_group != nullptr) {
                for (const auto &[term, negated] : sum_group->residual) {
                    zeroed[term] = llvm::Constant::getNullValue(term->getType());
                }
            }
            const auto *reads_as = sum_group != nullptr ? &zeroed : nullptr;
            llvm::DenseSet< llvm::Instruction * > already_in_tree;

            for (auto *root : roots) {
                auto &inst = *root;
                if (already_in_tree.contains(&inst) != 0u) {
                    DeclinedRoot(inst, "claimed by an enclosing root's tree", 0, 0);
                    continue;
                }

                const auto *gep   = ByteGepOf(&inst);
                const bool ptradd = gep != nullptr;
                if (!ptradd && !inst.getType()->isIntegerTy()) {
                    DeclinedRoot(inst, "not an integer", 0, 0);
                    continue;
                }
                // A slice reads the root at the width the caller proved it to
                // be an extension of, not at the width of its type.
                const uint32_t bw = slice != nullptr ? slice->width
                    : ptradd ? gep->getOperand(1)->getType()->getIntegerBitWidth()
                             : inst.getType()->getIntegerBitWidth();
                if (bw > 64 || bw == 0) {
                    continue;
                }
                // A `ptradd`'s readers are pointers: every bit of the index is
                // live, so there is no narrower mask to read it under.
                const uint64_t demanded_mask = (!ptradd && demanded)
                    ? CandidateDemand(
                          demanded(&inst), bw,
                          slice != nullptr ? slice->extension : SliceExtension::kNone,
                          sum_group != nullptr
                      )
                    : Bitmask(bw);

                llvm::SmallVector< llvm::Instruction *, 16 > tree_insts;
                std::vector< llvm::Value * > leaves;
                llvm::DenseMap< llvm::Value *, llvm::Value * > phi_redirects;
                CollectTree(
                    &inst, bw, tree_insts, leaves, phi_redirects,
                    /*try_phi_transparency=*/true, max_nodes, cut_arith_under_bitwise,
                    max_tree_nodes, opaque, reads_as, /*narrow_comparisons=*/boolean_cut
                );

                // The sweep below is 2^leaves, so every leaf doubles the price
                // of finding out whether the solver can use the tree at all.
                // Auxiliary variables are eliminated only after it has been
                // paid, and the orchestrator then rejects anything still over
                // `max_vars` — so collecting past that budget buys a candidate
                // that has to shed the excess as decoration to survive, at
                // twice the cost per variable of hope. The absolute ceiling
                // stays as a guard against a caller asking for 2^40 entries.
                constexpr uint32_t kPreElimCap = 20;
                const auto over_cap = [&] { return leaves.size() > std::min(max_vars, kPreElimCap); };

                // Verify transparent phis first: if any arm is not the same
                // computation as the one followed, the tree is collected again
                // with the phi as a leaf, and everything below - the size gate,
                // the polynomial check, the fingerprint, the expression - is
                // about that tree. (They used to be taken from the first
                // collection, so a tree whose phi turned out not to be
                // transparent was fingerprinted, and its expression built,
                // against a set of instructions it no longer held; and the size
                // gate ran first, so a phi read through to a constant arm made a
                // tree look two nodes small and declined it before the leaf it
                // really is was ever seen.)
                // Only the transparent phis have arms to agree; a reading a
                // sum group asked for is in the same map (`CollectTree`) and
                // is not one.
                const auto has_phi_redirect = [&] {
                    return llvm::any_of(phi_redirects, [](const auto &entry) {
                        return llvm::isa< llvm::PHINode >(entry.first);
                    });
                };
                //
                // A tree past the leaf cap is collected again the same way when
                // it read through a phi: the arms' cones bring their own leaves,
                // and with the phi a leaf the tree may well fit.
                if (has_phi_redirect()
                    && (over_cap() || !VerifyPhiArms(&inst, phi_redirects, tree_insts, leaves)))
                {
                    tree_insts.clear();
                    leaves.clear();
                    phi_redirects.clear();
                    CollectTree(
                        &inst, bw, tree_insts, leaves, phi_redirects,
                        /*try_phi_transparency=*/false, max_nodes, cut_arith_under_bitwise,
                        max_tree_nodes, opaque, reads_as, /*narrow_comparisons=*/boolean_cut
                    );
                }
                if (over_cap()) {
                    DeclinedRoot(inst, "more leaves than max_vars", tree_insts.size(), leaves.size());
                    continue;
                }

                // What is known of the leaves' bits is read into the tree as a
                // mask on each such leaf, so it counts toward the size of the
                // expression the tree stands for: `(v ^ C) + v` is two
                // instructions and, with `v` known to lie inside `C`, the
                // five-node expression `((v & C) ^ C) + (v & C)`, which is the
                // one worth the solver's time.
                const std::vector< LeafBits > leaf_bits =
                    LeafBitsOf(leaves, bw, inst.getModule()->getDataLayout());
                const size_t informative_leaves = static_cast< size_t >(
                    std::count_if(leaf_bits.begin(), leaf_bits.end(), [](const LeafBits &bits) {
                        return bits.Informative();
                    })
                );
                if (tree_insts.size() + informative_leaves < min_ast_size) {
                    DeclinedRoot(inst, "smaller than min_ast_size", tree_insts.size(), leaves.size());
                    continue;
                }

                const llvm::DenseSet< llvm::Value * > tree_set(
                    tree_insts.begin(), tree_insts.end()
                );
                if (HasPolynomialMul(&inst, tree_set)) {
                    DeclinedRoot(inst, "a polynomial multiplication", tree_insts.size(), leaves.size());
                    continue;
                }

                // Which of the tree's instructions actually disappear if the root
                // is replaced. The root always does; anything else only follows
                // once every one of its readers has gone. A node that is also
                // stored, or feeds a sibling computation, therefore survives and
                // keeps its own operands alive with it.
                //
                // This is both the honest measure of what a rewrite buys and the
                // right rule for claiming: a surviving node is not subsumed by
                // the root, so it stays eligible to be simplified in its own
                // right - whether the root is examined now or skipped below.
                llvm::DenseSet< llvm::Instruction * > dying{ &inst };
                for (bool growing = true; growing;) {
                    growing = false;
                    for (auto *ti : tree_insts) {
                        if (dying.contains(ti)) {
                            continue;
                        }
                        const bool kConsumed =
                            llvm::all_of(ti->users(), [&](const llvm::User *u) {
                                auto *ui = llvm::dyn_cast< llvm::Instruction >(u);
                                return ui != nullptr && dying.contains(ui);
                            });
                        if (kConsumed) {
                            dying.insert(ti);
                            growing = true;
                        }
                    }
                }

                // `min_ast_size` is meant to keep the solver away from
                // expressions too small to be worth its time. Measured on the
                // collected tree it says the wrong thing about a root that reads
                // values the rest of the block also needs: the tree is large,
                // but replacing the root removes one instruction, so no rewrite
                // can repay the search. What has to clear the bar is the number
                // of instructions actually at stake.
                //
                // Unless the caller prices the whole tree, in which case a root
                // with shared operands is exactly what it means to keep, and the
                // bar has already been cleared by the tree itself.
                //
                // Asked before the root claims its tree: a root this declines is
                // not examined, so the roots inside it have to be.
                if (cost_model == MbaCostModel::kDyingInstructions
                    && dying.size() < min_ast_size)
                {
                    continue;
                }

                for (auto *ti : dying) {
                    already_in_tree.insert(ti);
                }

                // Everything past this point — the 2^leaves signature sweep, the
                // AST build and the solve in the caller — is decided entirely by
                // the tree just collected. If a previous run already reached this
                // exact tree there is nothing new to learn, so move on; its dying
                // instructions are claimed above, exactly as an examined root's
                // are. (Claiming the whole tree here took the root's shared
                // operands away from the roots that read them as well.)
                //
                // A re-cut is exempt on both counts. Its fingerprint describes a
                // partial view of the root, so matching it against the record —
                // which is written for the full tree — compares two different
                // things, and it is only ever built because the full tree has
                // already been tried and rejected in this very run.
                const MbaFingerprint fingerprint = WithDemandedMask(
                    FingerprintTree(&inst, leaves, tree_set, phi_redirects, leaf_bits), demanded_mask, bw
                );
                // `COBRA_FINGERPRINT=off` examines every tree again, however
                // it was left. The record says "this exact tree was examined
                // under these options", not "and there is nothing to find", so
                // when a verdict turns out to have depended on something other
                // than the tree the record is what makes it permanent.
                static const bool skip_unchanged = [] {
                    const char *choice = std::getenv("COBRA_FINGERPRINT");
                    return choice == nullptr || std::string_view(choice) != "off";
                }();
                if (skip_unchanged && max_nodes == 0 && !cut_arith_under_bitwise && !boolean_cut
                    && shared_cut == nullptr && slice == nullptr && sum_group == nullptr
                    && FingerprintUnchanged(inst, fingerprint, options_tag))
                {
                    ++NumFingerprintSkips;
                    continue;
                }

                const auto num_vars  = static_cast< uint32_t >(leaves.size());
                const size_t sig_len = 1ULL << num_vars;
                std::vector< uint64_t > sig(sig_len);

                auto plan = std::make_shared< const TreeEvalPlan >(
                    TreeEvalPlan::Build(&inst, leaves, bw, phi_redirects, leaf_bits)
                );

                std::vector< uint64_t > inputs(num_vars);
                std::vector< uint64_t > slots(plan->SlotCount());
                for (size_t i = 0; i < sig_len; ++i) {
                    for (uint32_t v = 0; v < num_vars; ++v) {
                        inputs[v] = (i >> v) & 1;
                    }
                    sig[i] = plan->Evaluate(inputs.data(), num_vars, slots);
                }

                // Names are how a solved rewrite's variables are matched back
                // to leaves (`TryBuildVarSupport`), so no two may be the same:
                // an unnamed leaf's `v3` could otherwise be a named leaf's `v3`
                // too, and the rewrite read the wrong one.
                std::vector< std::string > var_names;
                llvm::StringSet<> taken;
                for (uint32_t v = 0; v < num_vars; ++v) {
                    if (leaves[v]->hasName()) {
                        taken.insert(leaves[v]->getName());
                    }
                }
                for (uint32_t v = 0; v < num_vars; ++v) {
                    if (leaves[v]->hasName()) {
                        var_names.push_back(leaves[v]->getName().str());
                        continue;
                    }
                    std::string name = "v" + std::to_string(v);
                    while (!taken.insert(name).second) {
                        name += "'";
                    }
                    var_names.push_back(std::move(name));
                }

                const uint64_t mask = Bitmask(bw);
                auto expr = BuildExprFromIR(&inst, leaves, tree_set, mask, phi_redirects, leaf_bits);

                // Build evaluator lambda for full-width verification.  The
                // plan is shared rather than captured by value so that copying
                // the Evaluator stays cheap; the per-call slot buffer is
                // mutable state owned by the closure, matching the Evaluator
                // thread-safety contract.
                //
                // The plan is built from the IR, so it stands on its own even
                // when no Expr could be: trees holding a select or icmp have no
                // Expr equivalent, and the 2^n signature alone samples each
                // variable at 0 and 1 only. Full-width probing is what keeps
                // those candidates honest, so the evaluator is always provided.
                Evaluator evaluator = [plan,
                                       scratch = std::vector< uint64_t >(plan->SlotCount())](
                                          const std::vector< uint64_t > &vals
                                      ) mutable -> uint64_t {
                    return plan->Evaluate(vals.data(), vals.size(), scratch);
                };

                // Everything the tree covers apart from the root itself. If the
                // root turns out to be irreducible these are where a smaller,
                // reducible expression can still be hiding.
                std::vector< llvm::Instruction * > inner_roots;
                for (auto *ti : tree_insts) {
                    if (ti != &inst && IsMbaRoot(ti) && ti->getType()->isIntegerTy()
                        && ti->getType()->getIntegerBitWidth() <= 64)
                    {
                        inner_roots.push_back(ti);
                    }
                }

                const bool spans_freeze =
                    llvm::any_of(tree_insts, [](const llvm::Instruction *ti) {
                        return IsTransparentFreeze(ti);
                    });
                const bool poison_reaches_root = !has_phi_redirect() && !spans_freeze
                    && llvm::none_of(tree_insts, [](const llvm::Instruction *ti) {
                           return llvm::isa< llvm::SelectInst >(ti);
                       });

                // Before `leaves` is moved into the candidate.
                auto leaves_copy = leaves;
                candidates.push_back(
                    MBACandidate{ .root        = &inst,
                                  .leaf_values = std::move(leaves),
                                  .leaf_bits   = leaf_bits,
                                  .leaf_widths = LeafWidthsOf(leaves_copy, bw),
                                  .var_names   = std::move(var_names),
                                  .sig         = std::move(sig),
                                  .bitwidth    = bw,
                                  .expr        = std::move(expr),
                                  .evaluator   = std::move(evaluator),
                                  .fingerprint = fingerprint,
                                  .inner_roots = std::move(inner_roots),
                                  .dying_count = static_cast< uint32_t >(dying.size()),
                                  .tree        = { tree_insts.begin(), tree_insts.end() },
                                  .dying       = { dying.begin(), dying.end() },
                                  .poison_reaches_root = poison_reaches_root,
                                  .node_limit = sum_group != nullptr
                                      ? SumGroupNodeLimit(
                                            sum_group->index, slice != nullptr ? slice->width : 0
                                        )
                                      : slice != nullptr ? SliceNodeLimit(slice->width)
                                      : shared_cut != nullptr ? shared_cut->node_limit
                                      : cut_arith_under_bitwise ? kBoundaryCutLimit
                                      : boolean_cut             ? kBooleanCutLimit
                                                                : max_nodes,
                                  .tree_size    = static_cast< uint32_t >(tree_insts.size()),
                                  .boundary_cut  = cut_arith_under_bitwise,
                                  .demanded_mask = demanded_mask,
                                  .spans_freeze  = spans_freeze,
                                  .spans_select  = llvm::any_of(
                                      tree_insts,
                                      [](const llvm::Instruction *ti) {
                                          return llvm::isa< llvm::SelectInst >(ti);
                                      }
                                  ),
                                  .holds_comparison = llvm::any_of(
                                      tree_insts,
                                      [](const llvm::Instruction *ti) {
                                          return llvm::isa< llvm::ICmpInst >(ti)
                                              || llvm::isa< llvm::SelectInst >(ti);
                                      }
                                  ),
                                  .slice_extension = slice != nullptr ? slice->extension
                                                                     : SliceExtension::kNone,
                                  .sum_residual = sum_group != nullptr ? sum_group->residual
                                      : std::vector< std::pair< llvm::Value *, bool > >{} }
                );
            }
        }

    } // namespace

    std::vector< MBACandidate > DetectMbaCandidates(
        llvm::Function &f, uint32_t min_ast_size, uint32_t max_vars, uint64_t options_tag,
        MbaCostModel cost_model, uint32_t max_tree_nodes, const DemandedMaskFn &demanded
    ) {
        // Post-order: process uses before defs across blocks. Within each block,
        // reverse iteration hits outermost roots first, so the largest MBA tree
        // claims inner nodes before they can be emitted as standalone candidates.
        std::vector< llvm::Instruction * > roots;
        for (auto *bb : post_order(&f)) {
            for (auto &inst : llvm::reverse(*bb)) {
                if (IsMbaRoot(&inst)) {
                    roots.push_back(&inst);
                }
            }
        }

        std::vector< MBACandidate > candidates;
        {
            static const bool trace = std::getenv("COBRA_DETECT_DEBUG") != nullptr;
            if (trace) {
                // How much of the function is addressed through a chain of byte
                // geps rather than through one sum: a chain is an addition the
                // detector cannot see, because `getelementptr` is not an MBA
                // opcode, so every identity the obfuscator wrote across it is
                // cut in half before a candidate is even built.
                size_t chained = 0;
                for (const auto &block : f) {
                    for (const auto &inst : block) {
                        const auto *gep = llvm::dyn_cast< llvm::GetElementPtrInst >(&inst);
                        if (gep != nullptr
                            && llvm::isa< llvm::GetElementPtrInst >(gep->getPointerOperand()))
                        {
                            ++chained;
                        }
                    }
                }
                llvm::errs() << "cobra: detect over " << f.getName() << ": " << roots.size()
                             << " root(s) of " << f.getInstructionCount() << " instruction(s), "
                             << chained << " chained byte gep(s)\n";
                if (const char *where = std::getenv("COBRA_DETECT_DUMP")) {
                    static unsigned sequence = 0;
                    std::error_code code;
                    llvm::raw_fd_ostream out(
                        std::string(where) + "/detect-" + std::to_string(++sequence) + ".ll", code
                    );
                    if (!code) {
                        out << f;
                    }
                }
            }
        }
        BuildCandidates(
            roots, min_ast_size, max_vars, cost_model, options_tag, candidates, 0, false,
            max_tree_nodes, demanded
        );
        return candidates;
    }

    std::vector< MBACandidate > ExpandMbaCandidate(
        llvm::ArrayRef< llvm::Instruction * > inner_roots, uint32_t min_ast_size,
        uint32_t max_vars, uint64_t options_tag, MbaCostModel cost_model,
        uint32_t max_tree_nodes, const DemandedMaskFn &demanded
    ) {
        std::vector< MBACandidate > candidates;
        BuildCandidates(
            inner_roots, min_ast_size, max_vars, cost_model, options_tag, candidates, 0, false,
            max_tree_nodes, demanded
        );
        return candidates;
    }

    std::vector< MBACandidate > RecutMbaCandidate(
        const MBACandidate &cand, uint32_t min_ast_size, uint32_t max_vars,
        uint32_t max_recut_nodes, uint32_t max_recut_vars, uint64_t options_tag,
        MbaCostModel cost_model, const DemandedMaskFn &demanded
    ) {
        std::vector< MBACandidate > candidates;
        if (cand.root == nullptr || cand.node_limit >= max_recut_nodes) {
            return candidates;
        }

        // A cut is a search over the ways this root could have been written, and
        // the solver only finds identities over a few variables at a time. A
        // collection that already ends in many independent leaves is not one the
        // expansion buried anything in — its cuts are smaller expressions with
        // no fewer variables, which is the direction the search gets worse in.
        if (cand.node_limit == 0 && cand.var_names.size() > max_recut_vars) {
            return candidates;
        }

        // One cut per rejection, smallest first. Handing back the whole ladder
        // at once would keep solving cuts of a root that a smaller one has
        // already rewritten; produced one at a time, the chain simply stops
        // advancing as soon as a cut succeeds. The budget strictly increases and
        // is capped, so the chain always ends.
        for (uint32_t nodes = cand.node_limit + 1; nodes <= max_recut_nodes; ++nodes) {
            BuildCandidates(
                { cand.root }, min_ast_size, max_vars, cost_model, options_tag, candidates,
                nodes, false, 0, demanded
            );
            if (candidates.empty()) {
                continue;
            }

            // A cut that did not spend its whole budget ran out of tree first,
            // so it is the full collection — which is the candidate that sent
            // us here. There is nothing left to cut.
            if (candidates.front().tree_size < nodes) {
                candidates.clear();
            }
            return candidates;
        }

        return candidates;
    }

    llvm::SmallVector< uint32_t, 4 > SliceWidthsOf(const MBACandidate &cand) {
        llvm::SmallVector< uint32_t, 4 > widths;
        // Only a full collection: a cut is a partial view of the root, so the
        // extensions it happens to hold say nothing about where the root's
        // own expression crosses widths. A `ptradd` root is an address, whose
        // every bit is live (`BuildCandidates`), so there is no narrower
        // reading of it either.
        // Only a full collection, of the root or of one group of its sum: a cut
        // is a partial view of the root, so the extensions it happens to hold
        // say nothing about where the root's own expression crosses widths.
        if (cand.root == nullptr
            || (cand.node_limit != 0 && !IsWholeSumGroupLimit(cand.node_limit))
            || ByteGepOf(cand.root) != nullptr || !cand.root->getType()->isIntegerTy())
        {
            return widths;
        }
        const uint32_t bw = cand.root->getType()->getIntegerBitWidth();
        if (bw > 64) {
            return widths;
        }
        for (const auto *node : cand.tree) {
            if (node->getOpcode() != llvm::Instruction::ZExt
                && node->getOpcode() != llvm::Instruction::SExt)
            {
                continue;
            }
            const auto *source = node->getOperand(0)->getType();
            if (!source->isIntegerTy()) {
                continue;
            }
            const uint32_t width = source->getIntegerBitWidth();
            // A slice has to be narrower than the root to say anything, and
            // wide enough to be a problem over words rather than over bits.
            if (width < 2 || width >= bw) {
                continue;
            }
            if (!llvm::is_contained(widths, width)) {
                widths.push_back(width);
            }
        }
        // Narrowest first: the narrowest slice is the strongest claim about the
        // root and the smallest problem to hand the solver, and a root that is
        // the extension of its low 8 bits is also the extension of its low 32,
        // so the answer found at 8 covers the one at 32.
        llvm::sort(widths);
        return widths;
    }

    uint64_t
    CandidateDemand(uint64_t root_demand, uint32_t width, SliceExtension extension, bool sum_group) {
        uint64_t demand = root_demand;
        if (sum_group && demand != 0) {
            const auto top = static_cast< uint32_t >(std::bit_width(demand));
            demand         = Bitmask(top);
        }
        if (extension == SliceExtension::kSign && width < 64 && (demand & ~Bitmask(width)) != 0) {
            demand |= uint64_t{ 1 } << (width - 1);
        }
        return demand & Bitmask(width);
    }

    std::vector< MBACandidate > SliceMbaCandidate(
        const MBACandidate &cand, uint32_t width, SliceExtension extension,
        uint32_t min_ast_size, uint32_t max_vars, uint64_t options_tag,
        MbaCostModel cost_model, uint32_t max_tree_nodes, const DemandedMaskFn &demanded
    ) {
        std::vector< MBACandidate > candidates;
        if (cand.root == nullptr || cand.node_limit != 0 || ByteGepOf(cand.root) != nullptr
            || !cand.root->getType()->isIntegerTy() || extension == SliceExtension::kNone
            || width < 2 || width > kMaxSliceWidth
            || width >= cand.root->getType()->getIntegerBitWidth())
        {
            return candidates;
        }
        const Slice cut{ .width = width, .extension = extension };
        BuildCandidates(
            { cand.root }, min_ast_size, max_vars, cost_model, options_tag, candidates, 0,
            /*cut_arith_under_bitwise=*/false, max_tree_nodes, demanded,
            /*shared_cut=*/nullptr, &cut
        );
        // The root has no reading at this width: the collection kept it as a
        // leaf, so the candidate is the root read back as itself.
        if (!candidates.empty()
            && !llvm::is_contained(candidates.front().tree, candidates.front().root))
        {
            candidates.clear();
        }
        return candidates;
    }

    std::vector< MBACandidate > SumGroupSliceMbaCandidates(
        const MBACandidate &cand, uint32_t min_ast_size, uint32_t max_vars,
        uint64_t options_tag, const SumGroupExtensionFn &is_extension,
        MbaCostModel cost_model, uint32_t max_tree_nodes, const DemandedMaskFn &demanded
    ) {
        std::vector< MBACandidate > candidates;
        if (cand.root == nullptr || cand.node_limit != 0 || ByteGepOf(cand.root) != nullptr
            || !cand.root->getType()->isIntegerTy() || !is_extension)
        {
            return candidates;
        }
        const uint32_t bw = cand.root->getType()->getIntegerBitWidth();
        if (bw > 64) {
            return candidates;
        }
        const auto split = SplitRootSum(cand);
        if (!split || split->groups < 2 || split->groups > kMaxSumGroups) {
            return candidates;
        }

        // The split is a fact about the root's own collection: no summand left
        // out is reached from inside a group kept, because two summands that
        // reach a common leaf are one group. A group's collection must read the
        // root no further than that one did, or it can reach a summand it
        // reads as zero from where it is not a summand - a phi that was a leaf
        // there turns transparent once the summand holding the other phi is
        // left out, or a node the budget cut off is now within it. So the
        // collection's leaves stay leaves.
        SharedCut as_collected{ .opaque = {}, .node_limit = 0 };
        for (auto *leaf : cand.leaf_values) {
            as_collected.opaque.insert(leaf);
        }

        for (uint32_t index = 0; index < split->groups; ++index) {
            SumGroup group;
            group.index = index;
            for (size_t term = 0; term < split->terms.size(); ++term) {
                if (split->group[term] != index) {
                    group.residual.push_back(split->terms[term]);
                }
            }

            // The group read at the root's own width: the expression the claim
            // about it is proved over. It is never queued - what the search is
            // handed is the narrow reading below.
            std::vector< MBACandidate > whole;
            BuildCandidates(
                { cand.root }, min_ast_size, max_vars, cost_model, options_tag, whole, 0,
                /*cut_arith_under_bitwise=*/false, max_tree_nodes, demanded, &as_collected,
                /*slice=*/nullptr, &group
            );
            // The root has no reading with the rest of the sum left out: the
            // collection kept it as a leaf, so the candidate is the root read
            // back as itself. (The tree is no smaller than the whole sum's,
            // and must not be asked to be: a summand left out is often a leaf
            // of the full reading already, and what the group changes is which
            // leaves the tree stands on, not how many nodes it holds.)
            if (whole.empty() || !llvm::is_contained(whole.front().tree, whole.front().root)) {
                continue;
            }
            // Probes decide which extension the group is read as; the claim is
            // proved later (`ExtensionClaim`), so a group without an expression
            // has nothing to prove it over.
            if (whole.front().expr == nullptr) {
                continue;
            }
            const auto read = is_extension(whole.front());
            if (!read || read->second == SliceExtension::kNone || read->first < 2
                || read->first > kMaxSliceWidth || read->first >= bw)
            {
                continue;
            }

            const Slice cut{ .width = read->first, .extension = read->second };
            std::vector< MBACandidate > sliced;
            BuildCandidates(
                { cand.root }, min_ast_size, max_vars, cost_model, options_tag, sliced, 0,
                /*cut_arith_under_bitwise=*/false, max_tree_nodes, demanded, &as_collected, &cut,
                &group
            );
            if (sliced.empty() || !llvm::is_contained(sliced.front().tree, sliced.front().root)) {
                continue;
            }
            const auto &claimed = whole.front();
            sliced.front().extension_claim = std::make_shared< const ExtensionClaim >(
                ExtensionClaim{ .expr = std::shared_ptr< const Expr >(CloneExpr(*claimed.expr)),
                                .bitwidth  = claimed.bitwidth,
                                .vars      = static_cast< uint32_t >(claimed.var_names.size()),
                                .tree_size = static_cast< uint32_t >(claimed.tree_size) }
            );
            candidates.push_back(std::move(sliced.front()));
        }
        return candidates;
    }

    std::vector< MBACandidate > BoundaryCutMbaCandidate(
        const MBACandidate &cand, uint32_t min_ast_size, uint32_t max_vars,
        uint64_t options_tag, MbaCostModel cost_model, uint32_t max_tree_nodes,
        const DemandedMaskFn &demanded
    ) {
        std::vector< MBACandidate > candidates;
        if (cand.root == nullptr || cand.node_limit != 0) {
            return candidates;
        }
        BuildCandidates(
            { cand.root }, min_ast_size, max_vars, cost_model, options_tag, candidates, 0,
            /*cut_arith_under_bitwise=*/true, max_tree_nodes, demanded
        );
        // A cut that took nothing out is the full collection again.
        if (!candidates.empty() && candidates.front().tree_size >= cand.tree_size) {
            candidates.clear();
        }
        return candidates;
    }

    std::vector< MBACandidate > BooleanCutMbaCandidate(
        const MBACandidate &cand, uint32_t min_ast_size, uint32_t max_vars,
        uint64_t options_tag, MbaCostModel cost_model, uint32_t max_tree_nodes,
        const DemandedMaskFn &demanded
    ) {
        std::vector< MBACandidate > candidates;
        if (cand.root == nullptr || cand.node_limit != 0) {
            return candidates;
        }
        BuildCandidates(
            { cand.root }, min_ast_size, max_vars, cost_model, options_tag, candidates, 0,
            /*cut_arith_under_bitwise=*/false, max_tree_nodes, demanded, nullptr, nullptr, nullptr,
            /*boolean_cut=*/true
        );
        return candidates;
    }

    std::vector< MBACandidate > SharedCutMbaCandidate(
        const MBACandidate &cand, uint32_t min_ast_size, uint32_t max_vars,
        uint32_t max_shared_cuts, uint64_t options_tag, MbaCostModel cost_model,
        uint32_t max_tree_nodes, const DemandedMaskFn &demanded
    ) {
        std::vector< MBACandidate > candidates;
        if (cand.root == nullptr) {
            return candidates;
        }

        // The ladder starts at a rejected full collection and is only ever
        // advanced by one of its own rungs.
        uint32_t rung = 0;
        if (cand.node_limit != 0) {
            if (!IsSharedCutLimit(cand.node_limit)) {
                return candidates;
            }
            rung = kSharedCutLimit - cand.node_limit + 1;
        }

        // The full collection again: what it reads more than once is a fact
        // about the whole tree, not about the rung that sent us here. The walk
        // is breadth-first, so `tree_insts` lists the shared values nearest the
        // root first, which is the order the rungs expand them in.
        const uint32_t bw = cand.bitwidth;
        llvm::SmallVector< llvm::Instruction *, 16 > tree_insts;
        std::vector< llvm::Value * > leaves;
        llvm::DenseMap< llvm::Value *, llvm::Value * > phi_redirects;
        CollectTree(
            cand.root, bw, tree_insts, leaves, phi_redirects, /*try_phi_transparency=*/true, 0,
            false, max_tree_nodes
        );
        const llvm::DenseSet< const llvm::Value * > tree_set(
            tree_insts.begin(), tree_insts.end()
        );
        llvm::SmallVector< llvm::Instruction *, 8 > shared;
        for (auto *ti : tree_insts) {
            if (ti == cand.root) {
                continue;
            }
            const auto reads = llvm::count_if(ti->uses(), [&](const llvm::Use &use) {
                return tree_set.contains(use.getUser());
            });
            if (reads > 1) {
                shared.push_back(ti);
            }
        }

        const auto rungs =
            std::min< size_t >({ shared.size(), max_shared_cuts, kSharedCutRungs });
        for (; rung < rungs; ++rung) {
            SharedCut cut;
            cut.opaque.insert(shared.begin() + rung, shared.end());
            cut.node_limit = kSharedCutLimit - rung;
            BuildCandidates(
                { cand.root }, min_ast_size, max_vars, cost_model, options_tag, candidates, 0,
                false, max_tree_nodes, demanded, &cut
            );
            if (candidates.empty()) {
                continue;
            }
            // The rungs only ever grow the tree, so one no larger than the
            // rung that was just rejected is that rung again: the value it
            // expanded is read only from under one that is still opaque.
            if (cand.node_limit != 0 && candidates.front().tree_size <= cand.tree_size) {
                candidates.clear();
                continue;
            }
            // And one that kept nothing out is the full collection.
            if (candidates.front().tree_size >= tree_insts.size()) {
                candidates.clear();
            }
            return candidates;
        }
        return candidates;
    }

    MbaFingerprint WithDemandedMask(MbaFingerprint fp, uint64_t mask, uint32_t bitwidth) {
        if (mask != Bitmask(bitwidth)) {
            fp.structure = MixFingerprint(fp.structure, mask);
        }
        return fp;
    }

    std::vector< uint32_t >
    LeafWidthsOf(const std::vector< llvm::Value * > &leaves, const uint32_t bw) {
        std::vector< uint32_t > widths(leaves.size());
        for (size_t i = 0; i < leaves.size(); ++i) {
            widths[i] = NarrowWidth(leaves[i], bw);
        }
        return widths;
    }

    std::unique_ptr< Expr > LeafExpr(const MBACandidate &cand, const uint32_t index) {
        if (index >= cand.leaf_widths.size()) {
            return nullptr;
        }
        const uint64_t mask = Bitmask(cand.bitwidth);
        auto leaf           = Expr::Variable(index);
        if (const uint32_t width = cand.leaf_widths[index]; width != 0) {
            leaf = Expr::BitwiseAnd(std::move(leaf), Expr::Constant(Bitmask(width)));
        }
        if (index < cand.leaf_bits.size()) {
            const auto &bits = cand.leaf_bits[index];
            if (bits.zero != 0) {
                leaf = Expr::BitwiseAnd(std::move(leaf), Expr::Constant(~bits.zero & mask));
            }
            if (bits.one != 0) {
                leaf = Expr::BitwiseOr(std::move(leaf), Expr::Constant(bits.one & mask));
            }
        }
        return leaf;
    }

    void RecordMbaFingerprint(
        llvm::Instruction *inst, const MbaFingerprint &fp, uint64_t options_tag
    ) {
        if (inst == nullptr) {
            return;
        }

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

    PhiArmCounters PhiArmStatistics() {
        return PhiArmCounters{ .checked          = phi_arms_checked.load(),
                               .exact_accept     = phi_arms_exact_accept.load(),
                               .leaf_unavailable = phi_arms_leaf_unavailable.load() };
    }

    std::optional< MbaFingerprint >
    ComputeMbaFingerprint(llvm::Instruction *inst, uint32_t max_tree_nodes) {
        if (inst == nullptr || !IsMbaRoot(inst)
            || !inst->getType()->isIntegerTy() || inst->getType()->getIntegerBitWidth() > 64)
        {
            return std::nullopt;
        }

        llvm::SmallVector< llvm::Instruction *, 16 > tree_insts;
        std::vector< llvm::Value * > leaves;
        llvm::DenseMap< llvm::Value *, llvm::Value * > phi_redirects;
        const uint32_t bw = inst->getType()->getIntegerBitWidth();
        CollectTree(
            inst, bw, tree_insts, leaves, phi_redirects, /*try_phi_transparency=*/true, 0, false,
            max_tree_nodes
        );
        // As the detector collects it: a phi only read through when its arms
        // are the same computation, or the fingerprint would describe a tree
        // the detector never builds and never match.
        if (!phi_redirects.empty() && !VerifyPhiArms(inst, phi_redirects, tree_insts, leaves)) {
            tree_insts.clear();
            leaves.clear();
            phi_redirects.clear();
            CollectTree(
                inst, bw, tree_insts, leaves, phi_redirects, /*try_phi_transparency=*/false, 0,
                false, max_tree_nodes
            );
        }

        const llvm::DenseSet< llvm::Value * > tree_set(tree_insts.begin(), tree_insts.end());
        return FingerprintTree(
            inst, leaves, tree_set, phi_redirects,
            LeafBitsOf(leaves, bw, inst->getModule()->getDataLayout())
        );
    }

} // namespace cobra
