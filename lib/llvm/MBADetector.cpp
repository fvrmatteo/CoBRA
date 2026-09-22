#include "MBADetector.h"
#include "cobra/core/BitWidth.h"
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
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/Casting.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <cstdlib>
#include <queue>
#include <random>
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
    const llvm::GetElementPtrInst *ByteGepOf(const llvm::Value *v) {
        const auto *gep = llvm::dyn_cast_or_null< llvm::GetElementPtrInst >(v);
        if (gep == nullptr || gep->getNumIndices() != 1
            || !gep->getSourceElementType()->isIntegerTy(8)
            || !gep->getOperand(1)->getType()->isIntegerTy(64))
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
            if (auto it = memo.find(v); it != memo.end()) {
                return it->second;
            }

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
                auto through =
                    FingerprintNode(it->second, leaf_index, tree_set, phi_redirects, memo);
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
            for (uint32_t i = 0; i < leaves.size(); ++i) {
                leaf_index[leaves[i]] = i;
            }

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
        // identity that is not there. A protector's split is two computed
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
        // it rewrites is phrased over `trunc` of that value. A protector widens
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
        bool FitsTreeWidth(const llvm::Instruction *inst, uint32_t bw) {
            if (ByteGepOf(inst) != nullptr) {
                return bw == 64; // the index width, which is the pointer width
            }
            if (const auto *cmp = llvm::dyn_cast< llvm::ICmpInst >(inst)) {
                return IsTreeWidthInt(cmp->getOperand(0), bw)
                    && IsSupportedPredicate(cmp->getPredicate());
            }

            if (inst->getOpcode() == llvm::Instruction::Select) {
                if (!inst->getType()->isIntegerTy() || inst->getType()->getIntegerBitWidth() > bw) {
                    return false;
                }
                const auto *cond = inst->getOperand(0);
                if (llvm::isa< llvm::ConstantInt >(cond)) {
                    return true;
                }
                const auto *cmp = llvm::dyn_cast< llvm::ICmpInst >(cond);
                return cmp != nullptr && IsTreeWidthInt(cmp->getOperand(0), bw)
                    && IsSupportedPredicate(cmp->getPredicate());
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
            // A sign extension is only modelled from an i1, whose 0/1 the
            // model reproduces exactly and whose extension is a negation; from
            // anything wider it stays a leaf.
            if (inst->getOpcode() == llvm::Instruction::ZExt) {
                return inst->getOperand(0)->getType()->isIntegerTy();
            }
            if (inst->getOpcode() == llvm::Instruction::SExt) {
                return inst->getOperand(0)->getType()->isIntegerTy(1);
            }

            // A truncation is a mask. The value it narrows may be wider than
            // the tree: the tree then holds that value's low `bw` bits, and a
            // mask that keeps a subset of them is exact whatever sits above.
            if (inst->getOpcode() == llvm::Instruction::Trunc) {
                return inst->getOperand(0)->getType()->isIntegerTy();
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
                if (!seen.insert(v).second || seen.size() > kMaxVisited) {
                    continue;
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
        void CollectTree(
            llvm::Instruction *root, uint32_t bw,
            llvm::SmallVector< llvm::Instruction *, 16 > &tree_insts,
            std::vector< llvm::Value * > &leaves,
            llvm::DenseMap< llvm::Value *, llvm::Value * > &phi_redirects,
            bool try_phi_transparency = true, uint32_t max_nodes = 0,
            bool cut_arith_under_bitwise = false, uint32_t max_tree_nodes = 0,
            const llvm::DenseSet< const llvm::Value * > *opaque = nullptr
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

                if ((inst != nullptr) && IsMbaInstruction(inst) && FitsTreeWidth(inst, bw)) {
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
                        if ((inc_inst == nullptr) || !IsMbaOpcode(inc_inst->getOpcode())
                            || !FitsTreeWidth(inc_inst, bw))
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
                plan.bits_ = bitwidth;

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
                    // holds its own width of it, and the Expr says the same.
                    const uint32_t slot = MaskToWidth(v, Emit(Op::kInput, leaf->second, 0));
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
                    const uint32_t child = lower(inst->getOperand(0));
                    // Sign-extending an i1 gives 0 or all-ones. Masking would
                    // leave 0/1, disagreeing with the Expr the verifier builds.
                    if (inst->getOpcode() == llvm::Instruction::SExt
                        && inst->getOperand(0)->getType()->isIntegerTy(1))
                    {
                        return MaskToWidth(v, Emit(Op::kSub, kZeroSlot, child));
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

            static constexpr uint32_t kZeroSlot = 0;

            std::vector< Node > nodes_;
            std::vector< uint64_t > consts_;
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

        std::unique_ptr< Expr > BuildExprFromIRImpl(
            llvm::Value *v, const std::vector< llvm::Value * > &leaves,
            const llvm::DenseSet< llvm::Value * > &tree_set, uint64_t mask,
            const llvm::DenseMap< llvm::Value *, llvm::Value * > &phi_redirects,
            llvm::DenseSet< llvm::Value * > &in_progress, size_t &budget
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
                    pit->second, leaves, tree_set, mask, phi_redirects, in_progress, budget
                );
            }

            // Constant
            if (auto *ci = llvm::dyn_cast< llvm::ConstantInt >(v)) {
                return Expr::Constant(ci->getZExtValue() & mask);
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

            // Leaf (variable)
            auto leaf_it = std::find(leaves.begin(), leaves.end(), v);
            if (leaf_it != leaves.end()) {
                auto idx = static_cast< uint32_t >(leaf_it - leaves.begin());
                return mask_to_width(Expr::Variable(idx));
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
                    inst->getOperand(0), leaves, tree_set, mask, phi_redirects, in_progress, budget
                );
                if (child == nullptr) {
                    return nullptr;
                }
                // Sign-extending an i1 gives 0 or all-ones, which is exactly
                // the negation of the 0/1 value. Passing it through unchanged
                // would leave 0/1 and model a different function than the IR.
                if (inst->getOpcode() == llvm::Instruction::SExt
                    && inst->getOperand(0)->getType()->isIntegerTy(1))
                {
                    return mask_to_width(Expr::Negate(std::move(child)));
                }
                return child;
            }

            // Trunc keeps the low bits: a mask at the width it narrows to.
            if (inst->getOpcode() == llvm::Instruction::Trunc) {
                auto child = BuildExprFromIRImpl(
                    inst->getOperand(0), leaves, tree_set, mask, phi_redirects, in_progress, budget
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
                    cmp->getOperand(0), leaves, tree_set, mask, phi_redirects, in_progress, budget
                );
                auto rhs = BuildExprFromIRImpl(
                    cmp->getOperand(1), leaves, tree_set, mask, phi_redirects, in_progress, budget
                );
                if (lhs == nullptr || rhs == nullptr) {
                    return nullptr;
                }
                return BuildComparison(cmp->getPredicate(), std::move(lhs), std::move(rhs));
            }

            if (inst->getOpcode() == llvm::Instruction::Select) {
                auto cond = BuildExprFromIRImpl(
                    inst->getOperand(0), leaves, tree_set, mask, phi_redirects, in_progress, budget
                );
                auto then_arm = BuildExprFromIRImpl(
                    inst->getOperand(1), leaves, tree_set, mask, phi_redirects, in_progress, budget
                );
                auto else_arm = BuildExprFromIRImpl(
                    inst->getOperand(2), leaves, tree_set, mask, phi_redirects, in_progress, budget
                );
                if (cond == nullptr || then_arm == nullptr || else_arm == nullptr) {
                    return nullptr;
                }
                // select(c, A, B) == B ^ ((A ^ B) & -c), because a 0/1 condition
                // negates to all-ones or zero. Written in existing operators so
                // no pass has to handle a three-child node. The clone is taken
                // before `else_arm` is moved from, since argument evaluation
                // order is unspecified.
                auto diff   = Expr::BitwiseXor(std::move(then_arm), CloneExpr(*else_arm));
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
                    inst->getOperand(0), leaves, tree_set, mask, phi_redirects, in_progress, budget
                );
                if (child == nullptr) {
                    return nullptr;
                }
                return Expr::LogicalShr(std::move(child), shift_amt->getZExtValue());
            }

            // Shl with constant shift amount
            if (inst->getOpcode() == llvm::Instruction::Shl) {
                auto *shift_amt = llvm::dyn_cast< llvm::ConstantInt >(inst->getOperand(1));
                if (shift_amt == nullptr) {
                    return nullptr;
                }
                auto child = BuildExprFromIRImpl(
                    inst->getOperand(0), leaves, tree_set, mask, phi_redirects, in_progress, budget
                );
                if (child == nullptr) {
                    return nullptr;
                }
                uint64_t mul_val = 1ULL << shift_amt->getZExtValue();
                return mask_to_width(Expr::Mul(std::move(child), Expr::Constant(mul_val)));
            }

            // A freeze is the identity on a defined operand, and the model
            // only ever hands it defined operands.
            if (IsTransparentFreeze(inst)) {
                return BuildExprFromIRImpl(
                    inst->getOperand(0), leaves, tree_set, mask, phi_redirects, in_progress,
                    budget
                );
            }

            // A funnel shift by a constant: the high part shifted up, masked to
            // the width, or'd with the low part shifted down.
            if (const auto *fsh = FunnelShiftOf(inst)) {
                const auto [amount, width] = FunnelShiftAmount(*fsh);
                const bool kLeft           = fsh->getIntrinsicID() == llvm::Intrinsic::fshl;
                auto a                     = BuildExprFromIRImpl(
                    fsh->getArgOperand(0), leaves, tree_set, mask, phi_redirects, in_progress, budget
                );
                auto b = BuildExprFromIRImpl(
                    fsh->getArgOperand(1), leaves, tree_set, mask, phi_redirects, in_progress, budget
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

            // A `ptradd`, read as the addition of its indices (see `ByteGepOf`).
            if (ByteGepOf(inst) != nullptr) {
                auto index = BuildExprFromIRImpl(
                    inst->getOperand(1), leaves, tree_set, mask, phi_redirects, in_progress,
                    budget
                );
                if (index == nullptr) {
                    return nullptr;
                }
                if (ByteGepOf(inst->getOperand(0)) == nullptr) {
                    return mask_to_width(std::move(index));
                }
                auto below = BuildExprFromIRImpl(
                    inst->getOperand(0), leaves, tree_set, mask, phi_redirects, in_progress,
                    budget
                );
                if (below == nullptr) {
                    return nullptr;
                }
                return mask_to_width(Expr::Add(std::move(below), std::move(index)));
            }

            // Binary operations
            auto lhs = BuildExprFromIRImpl(
                inst->getOperand(0), leaves, tree_set, mask, phi_redirects, in_progress, budget
            );
            auto rhs = BuildExprFromIRImpl(
                inst->getOperand(1), leaves, tree_set, mask, phi_redirects, in_progress, budget
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
            const llvm::DenseMap< llvm::Value *, llvm::Value * > &phi_redirects
        ) {
            llvm::DenseSet< llvm::Value * > in_progress;
            size_t budget = kMaxExprNodes;
            return BuildExprFromIRImpl(
                v, leaves, tree_set, mask, phi_redirects, in_progress, budget
            );
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
                if ((inst == nullptr)
                    || (!IsMbaOpcode(inst->getOpcode()) && !IsTransparentFreeze(inst)))
                {
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

        void BuildCandidates(
            llvm::ArrayRef< llvm::Instruction * > roots, uint32_t min_ast_size,
            uint32_t max_vars, MbaCostModel cost_model, uint64_t options_tag,
            std::vector< MBACandidate > &candidates, uint32_t max_nodes = 0,
            bool cut_arith_under_bitwise = false, uint32_t max_tree_nodes = 0,
            const DemandedMaskFn &demanded = {}, const SharedCut *shared_cut = nullptr
        ) {
            const auto *opaque = shared_cut != nullptr ? &shared_cut->opaque : nullptr;
            llvm::DenseSet< llvm::Instruction * > already_in_tree;

            for (auto *root : roots) {
                auto &inst = *root;
                if (already_in_tree.contains(&inst) != 0u) {
                    DeclinedRoot(inst, "claimed by an enclosing root's tree", 0, 0);
                    continue;
                }

                const bool ptradd = ByteGepOf(&inst) != nullptr;
                if (!ptradd && !inst.getType()->isIntegerTy()) {
                    DeclinedRoot(inst, "not an integer", 0, 0);
                    continue;
                }
                const uint32_t bw = ptradd ? 64u : inst.getType()->getIntegerBitWidth();
                if (bw > 64) {
                    continue;
                }
                // A `ptradd`'s readers are pointers: every bit of the index is
                // live, so there is no narrower mask to read it under.
                const uint64_t demanded_mask = (!ptradd && demanded)
                    ? (demanded(&inst) & Bitmask(bw))
                    : Bitmask(bw);

                llvm::SmallVector< llvm::Instruction *, 16 > tree_insts;
                std::vector< llvm::Value * > leaves;
                llvm::DenseMap< llvm::Value *, llvm::Value * > phi_redirects;
                CollectTree(
                    &inst, bw, tree_insts, leaves, phi_redirects,
                    /*try_phi_transparency=*/true, max_nodes, cut_arith_under_bitwise,
                    max_tree_nodes, opaque
                );

                if (tree_insts.size() < min_ast_size) {
                    DeclinedRoot(inst, "smaller than min_ast_size", tree_insts.size(), leaves.size());
                    continue;
                }

                // The sweep below is 2^leaves, so every leaf doubles the price
                // of finding out whether the solver can use the tree at all.
                // Auxiliary variables are eliminated only after it has been
                // paid, and the orchestrator then rejects anything still over
                // `max_vars` — so collecting past that budget buys a candidate
                // that has to shed the excess as decoration to survive, at
                // twice the cost per variable of hope. The absolute ceiling
                // stays as a guard against a caller asking for 2^40 entries.
                constexpr uint32_t kPreElimCap = 20;
                if (leaves.size() > std::min(max_vars, kPreElimCap)) {
                    DeclinedRoot(inst, "more leaves than max_vars", tree_insts.size(), leaves.size());
                    continue;
                }

                const llvm::DenseSet< llvm::Value * > tree_set(
                    tree_insts.begin(), tree_insts.end()
                );
                if (HasPolynomialMul(&inst, tree_set)) {
                    DeclinedRoot(inst, "a polynomial multiplication", tree_insts.size(), leaves.size());
                    continue;
                }

                // Everything past this point — phi arm verification, the
                // 2^leaves signature sweep, the AST build and the solve in the
                // caller — is decided entirely by the tree just collected. If a
                // previous run already reached this exact tree there is nothing
                // new to learn, so claim its instructions and move on. The tree
                // is still claimed because the enclosing root must keep winning
                // over its inner nodes whether or not it gets re-examined.
                //
                // A re-cut is exempt on both counts. Its fingerprint describes a
                // partial view of the root, so matching it against the record —
                // which is written for the full tree — compares two different
                // things, and it is only ever built because the full tree has
                // already been tried and rejected in this very run.
                const MbaFingerprint fingerprint = WithDemandedMask(
                    FingerprintTree(&inst, leaves, tree_set, phi_redirects), demanded_mask, bw
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
                if (skip_unchanged && max_nodes == 0 && !cut_arith_under_bitwise
                    && shared_cut == nullptr && FingerprintUnchanged(inst, fingerprint, options_tag))
                {
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
                        &inst, bw, tree_insts, leaves, phi_redirects,
                        /*try_phi_transparency=*/false, max_nodes, cut_arith_under_bitwise,
                        max_tree_nodes, opaque
                    );

                    if (tree_insts.size() < min_ast_size) {
                        continue;
                    }
                    if (leaves.size() > std::min(max_vars, kPreElimCap)) {
                        continue;
                    }
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
                // right.
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

                for (auto *ti : dying) {
                    already_in_tree.insert(ti);
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
                if (cost_model == MbaCostModel::kDyingInstructions
                    && dying.size() < min_ast_size)
                {
                    continue;
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
                const bool poison_reaches_root = phi_redirects.empty() && !spans_freeze
                    && llvm::none_of(tree_insts, [](const llvm::Instruction *ti) {
                           return llvm::isa< llvm::SelectInst >(ti);
                       });

                candidates.push_back(
                    MBACandidate{ .root        = &inst,
                                  .leaf_values = std::move(leaves),
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
                                  .node_limit = shared_cut != nullptr ? shared_cut->node_limit
                                      : cut_arith_under_bitwise      ? kBoundaryCutLimit
                                                                     : max_nodes,
                                  .tree_size    = static_cast< uint32_t >(tree_insts.size()),
                                  .boundary_cut  = cut_arith_under_bitwise,
                                  .demanded_mask = demanded_mask,
                                  .spans_freeze  = spans_freeze }
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
                // opcode, so every identity the protector wrote across it is
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
        CollectTree(
            inst, inst->getType()->getIntegerBitWidth(), tree_insts, leaves, phi_redirects,
            /*try_phi_transparency=*/true, 0, false, max_tree_nodes
        );

        const llvm::DenseSet< llvm::Value * > tree_set(tree_insts.begin(), tree_insts.end());
        return FingerprintTree(inst, leaves, tree_set, phi_redirects);
    }

} // namespace cobra
