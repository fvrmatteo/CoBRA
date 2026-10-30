#include "RealInputReading.h"
#include "MBADetector.h"

#include "cobra/core/AffineReading.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/Evaluator.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/SignatureEval.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <algorithm>
#include <random>
#include <utility>

namespace cobra {

    namespace {

        // The intrinsics the translation of instructions reads, with the
        // operands it needs constant.
        bool IsSlicedIntrinsic(const llvm::IntrinsicInst &call) {
            switch (call.getIntrinsicID()) {
                case llvm::Intrinsic::umin:
                case llvm::Intrinsic::umax:
                case llvm::Intrinsic::smin:
                case llvm::Intrinsic::smax:
                case llvm::Intrinsic::abs:
                case llvm::Intrinsic::uadd_sat:
                case llvm::Intrinsic::usub_sat:
                case llvm::Intrinsic::sadd_sat:
                case llvm::Intrinsic::ssub_sat:
                case llvm::Intrinsic::bswap:
                case llvm::Intrinsic::ctpop:
                    return true;
                case llvm::Intrinsic::fshl:
                case llvm::Intrinsic::fshr:
                    return llvm::isa< llvm::ConstantInt >(call.getArgOperand(2));
                default:
                    return false;
            }
        }

        // An integer operation the slice is followed through: one the
        // translation reads, over integer (not vector) operands.
        bool IsSlicedOperation(const llvm::Instruction &inst) {
            if (!inst.getType()->isIntegerTy()) {
                return false;
            }
            switch (inst.getOpcode()) {
                case llvm::Instruction::Add:
                case llvm::Instruction::Sub:
                case llvm::Instruction::Mul:
                case llvm::Instruction::And:
                case llvm::Instruction::Or:
                case llvm::Instruction::Xor:
                case llvm::Instruction::Shl:
                case llvm::Instruction::LShr:
                case llvm::Instruction::AShr:
                case llvm::Instruction::Freeze:
                    return true;
                case llvm::Instruction::ZExt:
                case llvm::Instruction::SExt:
                case llvm::Instruction::Trunc:
                case llvm::Instruction::ICmp:
                    return inst.getOperand(0)->getType()->isIntegerTy();
                case llvm::Instruction::Select:
                    return inst.getOperand(0)->getType()->isIntegerTy(1);
                case llvm::Instruction::Call:
                    if (const auto *call = llvm::dyn_cast< llvm::IntrinsicInst >(&inst)) {
                        return IsSlicedIntrinsic(*call);
                    }
                    return false;
                default:
                    return false;
            }
        }

        // The slice evaluated on values of its inputs. Every operation is
        // total, as the translation reads it: a shift by the width or more is
        // zero (or the sign, for `ashr`), and no flag makes anything poison.
        class SliceEvaluator
        {
          public:
            explicit SliceEvaluator(const RealInputSlice &slice) : slice_(slice) {
                values_.reserve(slice.inputs.size() + slice.nodes.size());
                for (auto *input : slice.inputs) {
                    slot_[input] = static_cast< uint32_t >(values_.size());
                    values_.emplace_back(input->getType()->getIntegerBitWidth(), 0);
                }
                for (auto *node : slice.nodes) {
                    for (const llvm::Use &operand : node->operands()) {
                        if (const auto *constant = llvm::dyn_cast< llvm::ConstantInt >(operand.get());
                            constant != nullptr && !slot_.contains(constant))
                        {
                            slot_[constant] = static_cast< uint32_t >(values_.size());
                            values_.push_back(constant->getValue());
                        }
                    }
                }
                for (auto *node : slice.nodes) {
                    slot_[node] = static_cast< uint32_t >(values_.size());
                    values_.emplace_back(node->getType()->getIntegerBitWidth(), 0);
                }
                operands_.reserve(slice.nodes.size());
                for (auto *node : slice.nodes) {
                    std::vector< uint32_t > slots;
                    const unsigned count = llvm::isa< llvm::CallBase >(node)
                        ? llvm::cast< llvm::CallBase >(node)->arg_size()
                        : node->getNumOperands();
                    for (unsigned index = 0; index < count; ++index) {
                        slots.push_back(slot_.lookup(node->getOperand(index)));
                    }
                    operands_.push_back(std::move(slots));
                }
            }

            llvm::APInt &Input(size_t index) { return values_[index]; }

            // What the last run computed for `value`, an input or an operation.
            const llvm::APInt &ValueOf(const llvm::Value *value) const { return values_[slot_.lookup(value)]; }

            const llvm::APInt &Run() {
                const size_t first = values_.size() - slice_.nodes.size();
                for (size_t index = 0; index < slice_.nodes.size(); ++index) {
                    values_[first + index] = Evaluate(*slice_.nodes[index], operands_[index]);
                }
                return values_.back();
            }

          private:
            llvm::APInt Evaluate(const llvm::Instruction &inst, const std::vector< uint32_t > &slots) {
                const auto op = [&](unsigned index) -> const llvm::APInt & { return values_[slots[index]]; };
                const unsigned width = inst.getType()->getIntegerBitWidth();
                const auto shift = [&](bool left) {
                    const uint64_t amount = op(1).getLimitedValue(width);
                    if (amount >= width) {
                        return llvm::APInt(width, 0);
                    }
                    return left ? op(0).shl(static_cast< unsigned >(amount))
                                : op(0).lshr(static_cast< unsigned >(amount));
                };
                switch (inst.getOpcode()) {
                    case llvm::Instruction::Add:
                        return op(0) + op(1);
                    case llvm::Instruction::Sub:
                        return op(0) - op(1);
                    case llvm::Instruction::Mul:
                        return op(0) * op(1);
                    case llvm::Instruction::And:
                        return op(0) & op(1);
                    case llvm::Instruction::Or:
                        return op(0) | op(1);
                    case llvm::Instruction::Xor:
                        return op(0) ^ op(1);
                    case llvm::Instruction::Shl:
                        return shift(true);
                    case llvm::Instruction::LShr:
                        return shift(false);
                    case llvm::Instruction::AShr: {
                        const uint64_t amount = op(1).getLimitedValue(width);
                        if (amount >= width) {
                            return op(0).isNegative() ? llvm::APInt::getAllOnes(width) : llvm::APInt(width, 0);
                        }
                        return op(0).ashr(static_cast< unsigned >(amount));
                    }
                    case llvm::Instruction::Freeze:
                        return op(0);
                    case llvm::Instruction::ZExt:
                        return op(0).zext(width);
                    case llvm::Instruction::SExt:
                        return op(0).sext(width);
                    case llvm::Instruction::Trunc:
                        return op(0).trunc(width);
                    case llvm::Instruction::ICmp:
                        return llvm::APInt(
                            1, llvm::ICmpInst::compare(op(0), op(1), llvm::cast< llvm::ICmpInst >(inst).getPredicate())
                        );
                    case llvm::Instruction::Select:
                        return op(0).isOne() ? op(1) : op(2);
                    default:
                        break;
                }
                const auto &call = llvm::cast< llvm::IntrinsicInst >(inst);
                switch (call.getIntrinsicID()) {
                    case llvm::Intrinsic::umin:
                        return llvm::APIntOps::umin(op(0), op(1));
                    case llvm::Intrinsic::umax:
                        return llvm::APIntOps::umax(op(0), op(1));
                    case llvm::Intrinsic::smin:
                        return llvm::APIntOps::smin(op(0), op(1));
                    case llvm::Intrinsic::smax:
                        return llvm::APIntOps::smax(op(0), op(1));
                    case llvm::Intrinsic::abs:
                        return op(0).abs();
                    case llvm::Intrinsic::uadd_sat:
                        return op(0).uadd_sat(op(1));
                    case llvm::Intrinsic::usub_sat:
                        return op(0).usub_sat(op(1));
                    case llvm::Intrinsic::sadd_sat:
                        return op(0).sadd_sat(op(1));
                    case llvm::Intrinsic::ssub_sat:
                        return op(0).ssub_sat(op(1));
                    case llvm::Intrinsic::bswap:
                        return op(0).byteSwap();
                    case llvm::Intrinsic::ctpop:
                        return llvm::APInt(width, op(0).popcount());
                    case llvm::Intrinsic::fshl:
                    case llvm::Intrinsic::fshr: {
                        const auto *amount = llvm::cast< llvm::ConstantInt >(call.getArgOperand(2));
                        unsigned by = static_cast< unsigned >(amount->getValue().urem(width));
                        if (call.getIntrinsicID() == llvm::Intrinsic::fshr) {
                            by = (width - by) % width;
                        }
                        if (by == 0) {
                            return call.getIntrinsicID() == llvm::Intrinsic::fshl ? op(0) : op(1);
                        }
                        return op(0).shl(by) | op(1).lshr(width - by);
                    }
                    default:
                        break;
                }
                return llvm::APInt(width, 0);
            }

            const RealInputSlice &slice_;
            llvm::DenseMap< const llvm::Value *, uint32_t > slot_;
            std::vector< llvm::APInt > values_;
            std::vector< std::vector< uint32_t > > operands_;
        };

        // A probe value of `width` bits: the edges of the range as often as a
        // random word, since a test of a value against zero or all ones is what
        // a word drawn at random never meets.
        llvm::APInt Draw(std::mt19937_64 &random, unsigned width) {
            switch (random() % 8) {
                case 0:
                    return llvm::APInt(width, 0);
                case 1:
                    return llvm::APInt::getAllOnes(width);
                case 2:
                    return llvm::APInt(width, random() % 16);
                default: {
                    llvm::SmallVector< uint64_t, 2 > words((width + 63) / 64);
                    for (auto &word : words) {
                        word = random();
                    }
                    return llvm::APInt(width, words);
                }
            }
        }

        // Where the specialized trees are built: a context of their own, so that
        // the constants they fold to do not stay in the function's context for
        // its lifetime, replaced after this many trees for the same reason.
        struct Scratch
        {
            llvm::LLVMContext context;
            std::unique_ptr< llvm::Module > module;
            unsigned uses = 0;
        };
        constexpr unsigned kTreesPerScratch = 1024;

        Scratch &ThreadScratch() {
            static thread_local std::unique_ptr< Scratch > scratch;
            if (scratch == nullptr || ++scratch->uses > kTreesPerScratch) {
                scratch.reset();
                scratch         = std::make_unique< Scratch >();
                scratch->module = std::make_unique< llvm::Module >("cobra.real-inputs", scratch->context);
            }
            return *scratch;
        }

        // `slice` built again in `function`, whose arguments are the supporting
        // inputs in order: every other input is the value the evaluator's last
        // run gave it, and an operation on those alone the value that run
        // computed for it. What the root is there.
        llvm::Value *Specialize(
            const RealInputSlice &slice, const std::vector< bool > &supporting, const SliceEvaluator &evaluator,
            llvm::Function &function, llvm::DenseMap< const llvm::Value *, llvm::Instruction * > &origin
        ) {
            auto &context = function.getContext();
            llvm::IRBuilder<> builder(llvm::BasicBlock::Create(context, "entry", &function));
            llvm::DenseMap< const llvm::Value *, llvm::Value * > built;
            const auto constant = [&](const llvm::APInt &value) { return llvm::ConstantInt::get(context, value); };
            unsigned argument = 0;
            for (size_t index = 0; index < slice.inputs.size(); ++index) {
                const auto *input = slice.inputs[index];
                built[input]      = supporting[index] ? static_cast< llvm::Value * >(function.getArg(argument++))
                                                      : constant(evaluator.ValueOf(input));
            }
            const auto operand = [&](const llvm::Value *value) -> llvm::Value * {
                if (const auto *literal = llvm::dyn_cast< llvm::ConstantInt >(value)) {
                    return constant(literal->getValue());
                }
                return built.lookup(value);
            };
            for (auto *node : slice.nodes) {
                const unsigned count = llvm::isa< llvm::CallBase >(node) ? llvm::cast< llvm::CallBase >(node)->arg_size()
                                                                          : node->getNumOperands();
                std::vector< llvm::Value * > operands;
                bool folded = true;
                for (unsigned index = 0; index < count; ++index) {
                    operands.push_back(operand(node->getOperand(index)));
                    folded = folded && llvm::isa< llvm::ConstantInt >(operands.back());
                }
                if (folded) {
                    built[node] = constant(evaluator.ValueOf(node));
                    continue;
                }
                auto *type = llvm::IntegerType::get(context, node->getType()->getIntegerBitWidth());
                llvm::Value *value = nullptr;
                if (const auto *binary = llvm::dyn_cast< llvm::BinaryOperator >(node)) {
                    value = builder.CreateBinOp(binary->getOpcode(), operands[0], operands[1]);
                } else if (const auto *cast = llvm::dyn_cast< llvm::CastInst >(node)) {
                    value = builder.CreateCast(cast->getOpcode(), operands[0], type);
                } else if (const auto *cmp = llvm::dyn_cast< llvm::ICmpInst >(node)) {
                    value = builder.CreateICmp(cmp->getPredicate(), operands[0], operands[1]);
                } else if (llvm::isa< llvm::SelectInst >(node)) {
                    value = builder.CreateSelect(operands[0], operands[1], operands[2]);
                } else if (llvm::isa< llvm::FreezeInst >(node)) {
                    // Total, as the evaluator and the proof read it.
                    value = operands[0];
                } else if (const auto *call = llvm::dyn_cast< llvm::IntrinsicInst >(node)) {
                    value = builder.CreateIntrinsic(call->getIntrinsicID(), { type }, operands);
                }
                if (value == nullptr) {
                    return nullptr;
                }
                built[node] = value;
                origin[value] = node;
            }
            llvm::Value *root = built.lookup(slice.root);
            builder.CreateRet(root);
            return root;
        }

        constexpr unsigned kBasePoints   = 16;
        constexpr unsigned kMovesPerBase = 2;

    } // namespace

    bool IsRealInputOperation(const llvm::Instruction &inst) { return IsSlicedOperation(inst); }

    const llvm::Value *DominatingCut(const RealInputSlice &slice) {
        if (slice.inputs.empty()) {
            return nullptr;
        }
        // The slice's values in an order where each comes after every value of
        // the slice that reads it: the root first, the inputs last.
        llvm::DenseMap< const llvm::Value *, std::vector< const llvm::Value * > > readers;
        for (auto *node : slice.nodes) {
            const unsigned count = llvm::isa< llvm::CallBase >(node) ? llvm::cast< llvm::CallBase >(node)->arg_size()
                                                                      : node->getNumOperands();
            for (unsigned index = 0; index < count; ++index) {
                const auto *operand = node->getOperand(index);
                if (!llvm::isa< llvm::ConstantInt >(operand)) {
                    auto &list = readers[operand];
                    if (list.empty() || list.back() != node) {
                        list.push_back(node);
                    }
                }
            }
        }
        std::vector< const llvm::Value * > order;
        for (auto it = slice.nodes.rbegin(); it != slice.nodes.rend(); ++it) {
            order.push_back(*it);
        }
        for (auto *input : slice.inputs) {
            order.push_back(input);
        }
        llvm::DenseMap< const llvm::Value *, unsigned > number;
        for (unsigned index = 0; index < order.size(); ++index) {
            number[order[index]] = index;
        }
        // `slice.nodes` lists each operation after its operands, so the reverse
        // has every reader first; the inputs come after all of them.
        std::vector< int > dominator(order.size(), -1);
        dominator[0]           = 0;
        const auto intersect = [&](int a, int b) {
            while (a != b) {
                while (a > b) {
                    a = dominator[static_cast< size_t >(a)];
                }
                while (b > a) {
                    b = dominator[static_cast< size_t >(b)];
                }
            }
            return a;
        };
        for (unsigned index = 1; index < order.size(); ++index) {
            int idom = -1;
            for (const auto *reader : readers.lookup(order[index])) {
                const int at = static_cast< int >(number.lookup(reader));
                idom         = idom < 0 ? at : intersect(idom, at);
            }
            dominator[index] = idom < 0 ? 0 : idom;
        }
        int common = -1;
        for (auto *input : slice.inputs) {
            const int at = static_cast< int >(number.lookup(input));
            common       = common < 0 ? dominator[static_cast< size_t >(at)] : intersect(common, dominator[static_cast< size_t >(at)]);
        }
        if (common <= 0) {
            return nullptr;
        }
        const auto *cut = order[static_cast< size_t >(common)];
        return llvm::is_contained(slice.inputs, cut) ? nullptr : cut;
    }

    std::optional< RealInputSlice > SliceToRealInputs(llvm::Instruction &root, uint32_t max_nodes, uint32_t max_inputs) {
        if (!IsSlicedOperation(root)) {
            return std::nullopt;
        }
        RealInputSlice slice;
        slice.root = &root;
        llvm::DenseMap< const llvm::Value *, bool > seen;
        // Post-order without recursion: a node is emitted once its operands are.
        std::vector< std::pair< llvm::Instruction *, unsigned > > stack{ { &root, 0 } };
        seen[&root] = true;
        while (!stack.empty()) {
            auto &[node, next] = stack.back();
            const unsigned count = llvm::isa< llvm::CallBase >(node) ? llvm::cast< llvm::CallBase >(node)->arg_size()
                                                                      : node->getNumOperands();
            if (next == count) {
                slice.nodes.push_back(node);
                stack.pop_back();
                if (slice.nodes.size() > max_nodes) {
                    return std::nullopt;
                }
                continue;
            }
            llvm::Value *operand = node->getOperand(next++);
            if (llvm::isa< llvm::ConstantInt >(operand) || !seen.try_emplace(operand, true).second) {
                continue;
            }
            auto *inst = llvm::dyn_cast< llvm::Instruction >(operand);
            if (inst != nullptr && IsSlicedOperation(*inst)) {
                stack.emplace_back(inst, 0);
                continue;
            }
            if (!operand->getType()->isIntegerTy()) {
                return std::nullopt;
            }
            slice.inputs.push_back(operand);
            if (slice.inputs.size() > max_inputs) {
                return std::nullopt;
            }
        }
        return slice;
    }

    RealInputReading ReadOverRealInputs(
        const RealInputSlice &slice, const Options &options, uint64_t seed, uint64_t options_tag,
        MbaCostModel cost_model, bool affine_reading
    ) {
        RealInputReading reading;
        const unsigned root_width = slice.root->getType()->getIntegerBitWidth();
        if (root_width > 64) {
            reading.reason = "the root is wider than 64 bits";
            return reading;
        }
        SliceEvaluator evaluator(slice);
        std::mt19937_64 random(seed);

        // Which inputs move the root: each moved on its own from a base point,
        // twice from each of several. One that never does is not read.
        std::vector< bool > supporting(slice.inputs.size(), false);
        size_t supported = 0;
        std::vector< llvm::APInt > base;
        for (unsigned point = 0; point < kBasePoints; ++point) {
            for (size_t index = 0; index < slice.inputs.size(); ++index) {
                evaluator.Input(index) = Draw(random, slice.inputs[index]->getType()->getIntegerBitWidth());
            }
            const llvm::APInt at_base = evaluator.Run();
            if (point == 0) {
                for (size_t index = 0; index < slice.inputs.size(); ++index) {
                    base.push_back(evaluator.Input(index));
                }
            }
            for (size_t index = 0; index < slice.inputs.size(); ++index) {
                if (supporting[index]) {
                    continue;
                }
                const llvm::APInt kept = evaluator.Input(index);
                for (unsigned move = 0; move < kMovesPerBase && !supporting[index]; ++move) {
                    evaluator.Input(index) = Draw(random, kept.getBitWidth());
                    if (evaluator.Run() != at_base) {
                        supporting[index] = true;
                        ++supported;
                    }
                }
                evaluator.Input(index) = kept;
            }
            if (supported > options.max_vars) {
                reading.reason = "the root reads " + std::to_string(supported) + " or more of its "
                    + std::to_string(slice.inputs.size()) + " inputs";
                return reading;
            }
        }

        uint32_t bitwidth = root_width;
        for (size_t index = 0; index < slice.inputs.size(); ++index) {
            if (supporting[index]) {
                reading.support.push_back(static_cast< uint32_t >(index));
                bitwidth = std::max(bitwidth, slice.inputs[index]->getType()->getIntegerBitWidth());
            }
        }
        if (bitwidth > 64) {
            reading.reason = "an input it reads is wider than 64 bits";
            return reading;
        }
        reading.bitwidth = bitwidth;

        // Every input outside the support at its first base value, which is as
        // good as any: the root reads none of them.
        for (size_t index = 0; index < slice.inputs.size(); ++index) {
            evaluator.Input(index) = base[index];
        }
        if (reading.support.empty()) {
            reading.expr = Expr::Constant(evaluator.Run().getZExtValue());
            return reading;
        }
        evaluator.Run();

        // The tree over the support alone, searched as any tree is.
        {
            auto &scratch = ThreadScratch();
            std::vector< llvm::Type * > parameters;
            for (const uint32_t index : reading.support) {
                parameters.push_back(
                    llvm::IntegerType::get(scratch.context, slice.inputs[index]->getType()->getIntegerBitWidth())
                );
            }
            auto *function = llvm::Function::Create(
                llvm::FunctionType::get(
                    llvm::IntegerType::get(scratch.context, root_width), parameters, false
                ),
                llvm::GlobalValue::ExternalLinkage, "specialized", scratch.module.get()
            );
            struct Erase
            {
                llvm::Function *function;
                ~Erase() { function->eraseFromParent(); }
            } erase{ function };
            llvm::DenseMap< const llvm::Value *, llvm::Instruction * > origin;
            llvm::Value *root = Specialize(slice, supporting, evaluator, *function, origin);
            const auto variable_of = [&](const llvm::Value *value) -> std::optional< uint32_t > {
                if (const auto *argument = llvm::dyn_cast_or_null< llvm::Argument >(value)) {
                    return reading.support[argument->getArgNo()];
                }
                return std::nullopt;
            };
            if (const auto *literal = llvm::dyn_cast_or_null< llvm::ConstantInt >(root)) {
                reading.support.clear();
                reading.expr = Expr::Constant(literal->getZExtValue());
                return reading;
            }
            if (const auto input = variable_of(root)) {
                reading.support  = { *input };
                reading.leaves   = { slice.inputs[*input] };
                reading.bitwidth = root_width;
                reading.expr     = Expr::Variable(0);
                return reading;
            }
            auto *specialized = llvm::dyn_cast_or_null< llvm::Instruction >(root);
            auto candidates   = specialized != nullptr
                  ? ExpandMbaCandidate({ specialized }, 1, options.max_vars, options_tag, cost_model, 0, {})
                  : std::vector< MBACandidate >{};
            for (auto &candidate : candidates) {
                if (candidate.root != specialized || candidate.node_limit != 0 || candidate.expr == nullptr) {
                    continue;
                }
                // Each leaf as the value of the function it stands for: a supporting
                // input, or the operation of the slice it was built from.
                std::vector< llvm::Value * > originals;
                for (const auto *leaf : candidate.leaf_values) {
                    if (const auto input = variable_of(leaf)) {
                        originals.push_back(slice.inputs[*input]);
                    } else if (const auto found = origin.find(leaf); found != origin.end()) {
                        originals.push_back(found->second);
                    }
                }
                if (originals.size() != candidate.leaf_values.size()) {
                    reading.reason = "the tree over the support ends in values it was not built from";
                    break;
                }
                // A collection that stops at the root itself - a `select` of
                // constants it cannot spell - read nothing: its answer is the
                // root over itself, which proves and, put in the root's place,
                // reads its own result.
                if (llvm::is_contained(originals, slice.root)) {
                    reading.reason = "the tree over the support ends in its own root";
                    break;
                }
                Options search{ .bitwidth         = candidate.bitwidth,
                                .max_vars         = options.max_vars,
                                .spot_check       = true,
                                .enabled_families = options.enabled_families,
                                .evaluator        = candidate.evaluator,
                                .demanded_mask    = candidate.demanded_mask };
                auto result = Simplify(candidate.sig, candidate.var_names, candidate.expr.get(), search);
                if (affine_reading
                    && (!result.has_value() || result.value().kind != SimplifyOutcome::Kind::kSimplified
                        || result.value().expr == nullptr)
                    && candidate.var_names.size() == candidate.leaf_widths.size())
                {
                    // Each leaf as the tree reads it, under its own width: a word
                    // packed from narrow loads is affine in them, and the search on
                    // values below reads every variable at the full width.
                    std::vector< std::unique_ptr< Expr > > leaves;
                    for (uint32_t index = 0; index < candidate.var_names.size(); ++index) {
                        leaves.push_back(LeafExpr(candidate, index));
                    }
                    if (auto affine = ReadAsAffineOfLeaves(
                            *candidate.expr, leaves, candidate.bitwidth, candidate.demanded_mask
                        );
                        affine != nullptr && IsBetter(ComputeCost(*affine).cost, ComputeCost(*candidate.expr).cost))
                    {
                        reading.leaves   = std::move(originals);
                        reading.bitwidth = candidate.bitwidth;
                        reading.expr     = std::move(affine);
                        return reading;
                    }
                }
                if (!result.has_value() || result.value().kind != SimplifyOutcome::Kind::kSimplified
                    || result.value().expr == nullptr)
                {
                    reading.reason = "over the tree of " + std::to_string(candidate.tree.size()) + " operations and "
                        + std::to_string(candidate.var_names.size()) + " variables, the search found nothing"
                        + (result.has_value() ? ": " + result.value().diag.reason : std::string());
                    break;
                }
                auto &outcome      = result.value();
                const auto &kept   = outcome.real_vars.empty() ? candidate.var_names : outcome.real_vars;
                std::vector< llvm::Value * > answer_leaves;
                for (const auto &name : kept) {
                    const auto at = std::find(candidate.var_names.begin(), candidate.var_names.end(), name);
                    if (at == candidate.var_names.end()) {
                        answer_leaves.clear();
                        break;
                    }
                    answer_leaves.push_back(originals[static_cast< size_t >(at - candidate.var_names.begin())]);
                }
                if (answer_leaves.size() != kept.size()) {
                    reading.reason = "the answer reads a variable that is no leaf";
                    break;
                }
                reading.leaves   = std::move(answer_leaves);
                reading.bitwidth = candidate.bitwidth;
                reading.expr     = std::move(outcome.expr);
                return reading;
            }
        }

        // What the tree over the support said, kept for the report of the search below.
        const std::string over_tree = reading.reason.empty() ? std::string("no tree over the support") : reading.reason;
        const auto support = reading.support;
        auto *shared       = &evaluator;
        const auto *inputs = &slice.inputs;
        Evaluator function(
            [shared, support, inputs](const std::vector< uint64_t > &values) -> uint64_t {
                for (size_t variable = 0; variable < support.size(); ++variable) {
                    const unsigned own = (*inputs)[support[variable]]->getType()->getIntegerBitWidth();
                    shared->Input(support[variable]) = llvm::APInt(64, values[variable]).trunc(own);
                }
                return shared->Run().getZExtValue();
            },
            static_cast< uint32_t >(support.size()), EvaluatorTraceKind::kNone
        );
        std::vector< std::string > names;
        for (size_t variable = 0; variable < support.size(); ++variable) {
            names.push_back("x" + std::to_string(variable));
        }
        Options search      = options;
        search.bitwidth     = bitwidth;
        search.evaluator    = function;
        search.demanded_mask = Bitmask(root_width);
        search.max_vars     = std::max< uint32_t >(search.max_vars, static_cast< uint32_t >(support.size()));
        const auto signature = EvaluateBooleanSignature(function, static_cast< uint32_t >(support.size()), bitwidth);
        auto result          = Simplify(signature, names, nullptr, search);
        if (!result.has_value()) {
            reading.reason = over_tree + "; on its values, the search failed: " + result.error().message;
            return reading;
        }
        auto &outcome = result.value();
        if (outcome.kind != SimplifyOutcome::Kind::kSimplified || outcome.expr == nullptr) {
            reading.reason = over_tree + "; on its values, the search found nothing: " + outcome.diag.reason;
            return reading;
        }
        // The answer's variables are the ones it kept, renumbered; back to the
        // inputs they stand for.
        if (!outcome.real_vars.empty() && outcome.real_vars != names) {
            std::vector< uint32_t > kept;
            for (const auto &name : outcome.real_vars) {
                const auto at = std::find(names.begin(), names.end(), name);
                if (at == names.end()) {
                    reading.reason = "the answer reads a variable that is no input";
                    return reading;
                }
                kept.push_back(support[static_cast< size_t >(at - names.begin())]);
            }
            reading.support = std::move(kept);
        }
        for (const uint32_t index : reading.support) {
            reading.leaves.push_back(slice.inputs[index]);
        }
        reading.expr = std::move(outcome.expr);
        return reading;
    }

} // namespace cobra
