#include "SmtStrategy.h"

#ifndef COBRA_HAS_BITWUZLA

// Built against Z3: every strategy is the question as built.
namespace cobra {
    Z3VerifyResult VerifyExprsWithStrategy(
        const Expr &lhs, const Expr &rhs, const std::vector< std::string > &var_names,
        uint32_t bitwidth, const Z3VerificationSettings &settings
    ) {
        return Z3VerifyExprs(lhs, rhs, var_names, bitwidth, settings);
    }

    SmtStrategyCounters SmtStrategyStatistics() { return {}; }
} // namespace cobra

#else

#include <bitwuzla/cpp/bitwuzla.h>

#include "cobra/verify/Sweep.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Passes/PassBuilder.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <unordered_map>

namespace cobra {

    namespace {

        using bitwuzla::Kind;
        using bitwuzla::Term;

        struct Counters
        {
            std::atomic< uint64_t > folded{ 0 };
            std::atomic< uint64_t > compiled{ 0 };
            std::atomic< uint64_t > fallbacks{ 0 };
            std::atomic< uint64_t > race_direct_won{ 0 };
            std::atomic< uint64_t > race_compiled_won{ 0 };
            std::atomic< uint64_t > race_both_unknown{ 0 };
            std::atomic< uint64_t > swept{ 0 };
            std::atomic< uint64_t > sweep_lemmas{ 0 };
            std::atomic< uint64_t > sweep_proved{ 0 };
            std::atomic< uint64_t > sweep_generalized{ 0 };
            std::atomic< uint64_t > sweep_synthesized{ 0 };
        };

        Counters &Stats() {
            static auto *counters = new Counters;
            return *counters;
        }

        // Something the translation back to the solver has no reading for.
        struct Untranslatable
        {};

        // A solver run ends early once the other side of a race has answered,
        // or at `deadline`.
        class FlagTerminator : public bitwuzla::Terminator
        {
          public:
            explicit FlagTerminator(
                const std::atomic< bool > &stop,
                std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max()
            )
                : stop_(stop), deadline_(deadline) {}

            bool terminate() override {
                return stop_.load(std::memory_order_relaxed) || std::chrono::steady_clock::now() >= deadline_;
            }

          private:
            const std::atomic< bool > &stop_;
            std::chrono::steady_clock::time_point deadline_;
        };

        // `SMT_QUERY_DUMP=<dir>`: a question neither the solver nor the sweep
        // answered in time goes to <dir>/cobra-unanswered, as the formula before
        // the sweep, so the hard cases can be replayed on their own.
        void DumpUnanswered(
            bitwuzla::TermManager &terms, const Term &question, const Z3VerificationSettings &settings,
            bool compiled, bool swept
        ) {
            static const std::string directory = [] {
                const char *where = std::getenv("SMT_QUERY_DUMP");
                if (where == nullptr || *where == '\0') {
                    return std::string{};
                }
                std::filesystem::create_directories(std::string(where) + "/cobra-unanswered");
                return std::string(where) + "/cobra-unanswered";
            }();
            if (directory.empty()) {
                return;
            }
            static std::atomic< uint64_t > sequence{ 0 };
            bitwuzla::Bitwuzla printer(terms);
            printer.assert_formula(question);
            std::ofstream out(directory + "/" + std::to_string(++sequence) + ".smt2");
            out << "; site cobra\n; method equivalence (" << (compiled ? "compiled" : "as built")
                << (swept ? ", swept" : "")
                << ")\n; budget-ms " << settings.timeout_ms << "\n; answer unknown\n; microseconds 0\n";
            printer.print_formula(out, "smt2");
        }

        // How long a compiled question is asked as it is before it is swept:
        // most are answered well inside it and never pay for the sweep.
        constexpr auto kSweepHeadStart = std::chrono::milliseconds(1);

        // ---- the question as an LLVM function -------------------------------

        // One per thread: a context, a module to build questions in, and the
        // pass pipeline, parsed once. The analyses are registered once too.
        struct Compiler
        {
            llvm::LLVMContext context;
            std::unique_ptr< llvm::Module > module;
            llvm::LoopAnalysisManager loops;
            llvm::FunctionAnalysisManager functions;
            llvm::CGSCCAnalysisManager sccs;
            llvm::ModuleAnalysisManager modules;
            std::unique_ptr< llvm::PassBuilder > builder;
            std::string pipeline;
            std::optional< llvm::FunctionPassManager > function_passes;
            std::optional< llvm::ModulePassManager > module_passes;
            bool usable = false;

            Compiler() {
                module = std::make_unique< llvm::Module >("cobra.smt", context);
                // Vectorizing a question only makes it harder to read back.
                llvm::PipelineTuningOptions tuning;
                tuning.SLPVectorization  = false;
                tuning.LoopVectorization = false;
                tuning.LoopUnrolling     = false;
                builder = std::make_unique< llvm::PassBuilder >(nullptr, tuning);
                builder->registerModuleAnalyses(modules);
                builder->registerCGSCCAnalyses(sccs);
                builder->registerFunctionAnalyses(functions);
                builder->registerLoopAnalyses(loops);
                builder->crossRegisterProxies(loops, functions, sccs, modules);
            }

            bool Configure(const std::string &text) {
                if (text == pipeline && (usable || text.empty())) {
                    return usable;
                }
                pipeline = text;
                function_passes.reset();
                module_passes.reset();
                llvm::FunctionPassManager fpm;
                if (!builder->parsePassPipeline(fpm, text)) {
                    function_passes.emplace(std::move(fpm));
                    usable = true;
                    return true;
                }
                llvm::ModulePassManager mpm;
                if (!builder->parsePassPipeline(mpm, text)) {
                    module_passes.emplace(std::move(mpm));
                    usable = true;
                    return true;
                }
                usable = false;
                return false;
            }

            void Optimize(llvm::Function &function) {
                if (function_passes) {
                    function_passes->run(function, functions);
                    functions.clear(function, function.getName());
                } else if (module_passes) {
                    module_passes->run(*module, modules);
                    modules.clear();
                }
            }
        };

        Compiler &ThreadCompiler() {
            static thread_local Compiler compiler;
            return compiler;
        }

        // `expr` at width `width`, every operation total.
        class Lowering
        {
          public:
            Lowering(llvm::IRBuilder<> &builder, llvm::ArrayRef< llvm::Value * > variables, unsigned width)
                : builder_(builder), variables_(variables), type_(builder.getIntNTy(width)),
                  width_(width) {}

            llvm::Value *Lower(const Expr &expr) {
                auto found = memo_.find(&expr);
                if (found != memo_.end()) {
                    return found->second;
                }
                llvm::Value *value = Build(expr);
                memo_.emplace(&expr, value);
                return value;
            }

          private:
            llvm::Value *Build(const Expr &expr) {
                const auto child = [&](size_t index) { return Lower(*expr.children.at(index)); };
                const auto fold = [&](auto combine) {
                    llvm::Value *result = child(0);
                    for (size_t index = 1; index < expr.children.size(); ++index) {
                        result = combine(result, child(index));
                    }
                    return result;
                };
                switch (expr.kind) {
                    case Expr::Kind::kConstant:
                        return llvm::ConstantInt::get(
                            type_, llvm::APInt(width_, width_ >= 64 ? expr.constant_val
                                                                    : expr.constant_val & ((uint64_t{ 1 } << width_) - 1))
                        );
                    case Expr::Kind::kVariable:
                        if (expr.var_index >= variables_.size()) {
                            throw Untranslatable{};
                        }
                        return variables_[expr.var_index];
                    case Expr::Kind::kAdd:
                        return fold([&](auto *a, auto *b) { return builder_.CreateAdd(a, b); });
                    case Expr::Kind::kMul:
                        return fold([&](auto *a, auto *b) { return builder_.CreateMul(a, b); });
                    case Expr::Kind::kAnd:
                        return fold([&](auto *a, auto *b) { return builder_.CreateAnd(a, b); });
                    case Expr::Kind::kOr:
                        return fold([&](auto *a, auto *b) { return builder_.CreateOr(a, b); });
                    case Expr::Kind::kXor:
                        return fold([&](auto *a, auto *b) { return builder_.CreateXor(a, b); });
                    case Expr::Kind::kNot:
                        return builder_.CreateNot(child(0));
                    case Expr::Kind::kNeg:
                        return builder_.CreateSub(llvm::ConstantInt::get(type_, 0), child(0));
                    case Expr::Kind::kShr:
                        // A shift by the width or more is zero, not poison.
                        if (expr.constant_val >= width_) {
                            return llvm::ConstantInt::get(type_, 0);
                        }
                        return builder_.CreateLShr(child(0), expr.constant_val);
                    case Expr::Kind::kCmpEq:
                        return builder_.CreateZExt(builder_.CreateICmpEQ(child(0), child(1)), type_);
                    case Expr::Kind::kCmpUlt:
                        return builder_.CreateZExt(builder_.CreateICmpULT(child(0), child(1)), type_);
                    case Expr::Kind::kCmpSlt:
                        return builder_.CreateZExt(builder_.CreateICmpSLT(child(0), child(1)), type_);
                }
                throw Untranslatable{};
            }

            llvm::IRBuilder<> &builder_;
            llvm::ArrayRef< llvm::Value * > variables_;
            llvm::IntegerType *type_;
            unsigned width_;
            std::unordered_map< const Expr *, llvm::Value * > memo_;
        };

        // ---- the optimized function, back to the solver ---------------------
        //
        // Every integer is a bitvector, an `i1` one of width one, so a
        // comparison is `ite(c, #b1, #b0)` and a condition is `v = #b1`.
        // Flags that make a value poison are read as if absent: the function
        // being translated computes the same value as a poison-free one on every
        // input, so no poison can reach its result, and a value that does not
        // reach the result may be read any way at all.
        class Translation
        {
          public:
            Translation(
                bitwuzla::TermManager &terms, const std::unordered_map< const llvm::Value *, Term > &arguments
            )
                : terms_(terms), values_(arguments) {}

            Term Translate(const llvm::Value *value) {
                auto found = values_.find(value);
                if (found != values_.end()) {
                    return found->second;
                }
                Term term = Build(value);
                values_.emplace(value, term);
                return term;
            }

          private:
            Term Bit(const Term &condition) {
                return terms_.mk_term(
                    Kind::ITE, { condition, terms_.mk_bv_one(terms_.mk_bv_sort(1)),
                                 terms_.mk_bv_zero(terms_.mk_bv_sort(1)) }
                );
            }

            Term Condition(const Term &bit) {
                return terms_.mk_term(Kind::EQUAL, { bit, terms_.mk_bv_one(terms_.mk_bv_sort(1)) });
            }

            Term Build(const llvm::Value *value) {
                if (const auto *constant = llvm::dyn_cast< llvm::ConstantInt >(value)) {
                    llvm::SmallString< 80 > digits;
                    constant->getValue().toString(digits, 2, false);
                    return terms_.mk_bv_value(
                        terms_.mk_bv_sort(constant->getBitWidth()), std::string(digits), 2
                    );
                }
                const auto *inst = llvm::dyn_cast< llvm::Instruction >(value);
                if (inst == nullptr || !inst->getType()->isIntegerTy()) {
                    throw Untranslatable{};
                }
                const auto op = [&](unsigned index) { return Translate(inst->getOperand(index)); };
                const unsigned width = inst->getType()->getIntegerBitWidth();
                const auto binary = [&](Kind kind) { return terms_.mk_term(kind, { op(0), op(1) }); };
                switch (inst->getOpcode()) {
                    case llvm::Instruction::Add:
                        return binary(Kind::BV_ADD);
                    case llvm::Instruction::Sub:
                        return binary(Kind::BV_SUB);
                    case llvm::Instruction::Mul:
                        return binary(Kind::BV_MUL);
                    case llvm::Instruction::And:
                        return binary(Kind::BV_AND);
                    case llvm::Instruction::Or:
                        return binary(Kind::BV_OR);
                    case llvm::Instruction::Xor:
                        return binary(Kind::BV_XOR);
                    case llvm::Instruction::Shl:
                        return binary(Kind::BV_SHL);
                    case llvm::Instruction::LShr:
                        return binary(Kind::BV_SHR);
                    case llvm::Instruction::AShr:
                        return binary(Kind::BV_ASHR);
                    case llvm::Instruction::ZExt:
                        return terms_.mk_term(
                            Kind::BV_ZERO_EXTEND, { op(0) },
                            { width - inst->getOperand(0)->getType()->getIntegerBitWidth() }
                        );
                    case llvm::Instruction::SExt:
                        return terms_.mk_term(
                            Kind::BV_SIGN_EXTEND, { op(0) },
                            { width - inst->getOperand(0)->getType()->getIntegerBitWidth() }
                        );
                    case llvm::Instruction::Trunc:
                        return terms_.mk_term(Kind::BV_EXTRACT, { op(0) }, { width - 1, 0 });
                    case llvm::Instruction::Freeze:
                        return op(0);
                    case llvm::Instruction::Select:
                        return terms_.mk_term(Kind::ITE, { Condition(op(0)), op(1), op(2) });
                    case llvm::Instruction::ICmp: {
                        static const std::unordered_map< llvm::CmpInst::Predicate, Kind > kPredicates = {
                            { llvm::CmpInst::ICMP_EQ, Kind::EQUAL },
                            { llvm::CmpInst::ICMP_NE, Kind::DISTINCT },
                            { llvm::CmpInst::ICMP_ULT, Kind::BV_ULT },
                            { llvm::CmpInst::ICMP_ULE, Kind::BV_ULE },
                            { llvm::CmpInst::ICMP_UGT, Kind::BV_UGT },
                            { llvm::CmpInst::ICMP_UGE, Kind::BV_UGE },
                            { llvm::CmpInst::ICMP_SLT, Kind::BV_SLT },
                            { llvm::CmpInst::ICMP_SLE, Kind::BV_SLE },
                            { llvm::CmpInst::ICMP_SGT, Kind::BV_SGT },
                            { llvm::CmpInst::ICMP_SGE, Kind::BV_SGE },
                        };
                        const auto *cmp = llvm::cast< llvm::ICmpInst >(inst);
                        const auto kind = kPredicates.find(cmp->getPredicate());
                        if (kind == kPredicates.end()) {
                            throw Untranslatable{};
                        }
                        return Bit(terms_.mk_term(kind->second, { op(0), op(1) }));
                    }
                    case llvm::Instruction::Call:
                        return Intrinsic(*inst, width);
                    default:
                        break;
                }
                throw Untranslatable{};
            }

            Term Intrinsic(const llvm::Instruction &inst, unsigned width) {
                const auto *call = llvm::dyn_cast< llvm::IntrinsicInst >(&inst);
                if (call == nullptr) {
                    throw Untranslatable{};
                }
                const auto arg = [&](unsigned index) { return Translate(call->getArgOperand(index)); };
                const auto pick = [&](Kind compare) {
                    Term a = arg(0), b = arg(1);
                    return terms_.mk_term(Kind::ITE, { terms_.mk_term(compare, { a, b }), a, b });
                };
                switch (call->getIntrinsicID()) {
                    case llvm::Intrinsic::umin:
                        return pick(Kind::BV_ULT);
                    case llvm::Intrinsic::umax:
                        return pick(Kind::BV_UGT);
                    case llvm::Intrinsic::smin:
                        return pick(Kind::BV_SLT);
                    case llvm::Intrinsic::smax:
                        return pick(Kind::BV_SGT);
                    case llvm::Intrinsic::abs: {
                        Term x = arg(0);
                        return terms_.mk_term(
                            Kind::ITE, { terms_.mk_term(Kind::BV_SLT, { x, terms_.mk_bv_zero(x.sort()) }),
                                         terms_.mk_term(Kind::BV_NEG, { x }), x }
                        );
                    }
                    case llvm::Intrinsic::uadd_sat: {
                        Term a = arg(0), b = arg(1);
                        Term sum = terms_.mk_term(Kind::BV_ADD, { a, b });
                        return terms_.mk_term(
                            Kind::ITE, { terms_.mk_term(Kind::BV_ULT, { sum, a }),
                                         terms_.mk_bv_ones(a.sort()), sum }
                        );
                    }
                    case llvm::Intrinsic::usub_sat: {
                        Term a = arg(0), b = arg(1);
                        return terms_.mk_term(
                            Kind::ITE, { terms_.mk_term(Kind::BV_ULT, { a, b }), terms_.mk_bv_zero(a.sort()),
                                         terms_.mk_term(Kind::BV_SUB, { a, b }) }
                        );
                    }
                    case llvm::Intrinsic::fshl:
                    case llvm::Intrinsic::fshr: {
                        const auto *amount = llvm::dyn_cast< llvm::ConstantInt >(call->getArgOperand(2));
                        if (amount == nullptr) {
                            throw Untranslatable{};
                        }
                        unsigned shift = static_cast< unsigned >(amount->getValue().urem(width));
                        if (call->getIntrinsicID() == llvm::Intrinsic::fshr) {
                            shift = (width - shift) % width;
                        }
                        Term high = arg(0), low = arg(1);
                        if (shift == 0) {
                            return call->getIntrinsicID() == llvm::Intrinsic::fshl ? high : low;
                        }
                        Term joined = terms_.mk_term(Kind::BV_CONCAT, { high, low });
                        return terms_.mk_term(Kind::BV_EXTRACT, { joined }, { 2 * width - 1 - shift, width - shift });
                    }
                    case llvm::Intrinsic::bswap: {
                        Term x = arg(0);
                        std::vector< Term > bytes;
                        for (unsigned index = 0; index < width / 8; ++index) {
                            bytes.push_back(terms_.mk_term(Kind::BV_EXTRACT, { x }, { 8 * index + 7, 8 * index }));
                        }
                        return terms_.mk_term(Kind::BV_CONCAT, bytes);
                    }
                    case llvm::Intrinsic::ctpop: {
                        Term x   = arg(0);
                        Term sum = terms_.mk_bv_zero(x.sort());
                        for (unsigned index = 0; index < width; ++index) {
                            Term bit = terms_.mk_term(Kind::BV_EXTRACT, { x }, { index, index });
                            sum = terms_.mk_term(
                                Kind::BV_ADD, { sum, terms_.mk_term(Kind::BV_ZERO_EXTEND, { bit }, { width - 1 }) }
                            );
                        }
                        return sum;
                    }
                    default:
                        break;
                }
                throw Untranslatable{};
            }

            bitwuzla::TermManager &terms_;
            std::unordered_map< const llvm::Value *, Term > values_;
        };

        enum class Answer : uint8_t { kEqual, kDiffer, kUnknown, kUntranslatable };

        // `lhs != rhs` as an LLVM function, optimized when `optimize` says so,
        // and handed to Bitwuzla. `stop` ends the solver run early.
        Answer Check(
            const Expr &lhs, const Expr &rhs, size_t num_vars, uint32_t bitwidth,
            const Z3VerificationSettings &settings, bool optimize, const std::atomic< bool > &stop,
            bool &timed_out
        ) {
            auto &compiler = ThreadCompiler();
            if (optimize && !compiler.Configure(settings.llvm_passes)) {
                return Answer::kUntranslatable;
            }
            auto &context = compiler.context;
            auto *type    = llvm::Type::getIntNTy(context, bitwidth);
            std::vector< llvm::Type * > parameters(num_vars, type);
            auto *function_type = llvm::FunctionType::get(llvm::Type::getInt1Ty(context), parameters, false);
            auto *function = llvm::Function::Create(
                function_type, llvm::GlobalValue::ExternalLinkage, "q", compiler.module.get()
            );
            // The function is removed however this ends.
            struct Erase
            {
                llvm::Function *function;
                ~Erase() { function->eraseFromParent(); }
            } erase{ function };

            std::vector< llvm::Value * > variables;
            for (auto &argument : function->args()) {
                argument.addAttr(llvm::Attribute::NoUndef);
                variables.push_back(&argument);
            }
            auto *entry = llvm::BasicBlock::Create(context, "entry", function);
            llvm::IRBuilder<> builder(entry);
            try {
                Lowering lowering(builder, variables, bitwidth);
                llvm::Value *left  = lowering.Lower(lhs);
                llvm::Value *right = lowering.Lower(rhs);
                builder.CreateRet(builder.CreateICmpNE(left, right));
            } catch (const Untranslatable &) {
                return Answer::kUntranslatable;
            }

            if (optimize) {
                compiler.Optimize(*function);
            }

            const auto *returned =
                llvm::cast< llvm::ReturnInst >(function->getEntryBlock().getTerminator())->getReturnValue();
            if (const auto *constant = llvm::dyn_cast< llvm::ConstantInt >(returned)) {
                if (optimize) {
                    Stats().folded.fetch_add(1, std::memory_order_relaxed);
                }
                return constant->isZero() ? Answer::kEqual : Answer::kDiffer;
            }

            bitwuzla::TermManager terms;
            std::unordered_map< const llvm::Value *, Term > arguments;
            for (auto &argument : function->args()) {
                arguments.emplace(
                    &argument,
                    terms.mk_const(terms.mk_bv_sort(bitwidth), "v" + std::to_string(argument.getArgNo()))
                );
            }
            Term question;
            try {
                Translation translation(terms, arguments);
                question = terms.mk_term(
                    Kind::EQUAL, { translation.Translate(returned), terms.mk_bv_one(terms.mk_bv_sort(1)) }
                );
            } catch (const Untranslatable &) {
                return Answer::kUntranslatable;
            }
            if (optimize) {
                Stats().compiled.fetch_add(1, std::memory_order_relaxed);
            }

            using Clock      = std::chrono::steady_clock;
            const auto begin = Clock::now();
            const auto deadline =
                settings.timeout_ms > 0 ? begin + std::chrono::milliseconds(settings.timeout_ms) : Clock::time_point::max();
            const auto ask = [&](const Term &formula, Clock::time_point until) {
                bitwuzla::Options options;
                if (settings.timeout_ms > 0) {
                    const auto left = std::chrono::duration_cast< std::chrono::milliseconds >(until - Clock::now());
                    options.set(
                        bitwuzla::Option::TIME_LIMIT_PER, static_cast< uint64_t >(std::max< int64_t >(1, left.count()))
                    );
                }
                options.set(bitwuzla::Option::REWRITE_LEVEL, static_cast< uint64_t >(settings.rewrite_level));
                bitwuzla::Bitwuzla solver(terms, options);
                FlagTerminator terminator(stop, until);
                solver.configure_terminator(&terminator);
                solver.assert_formula(formula);
                const auto answer = solver.check_sat();
                solver.configure_terminator(nullptr);
                return answer;
            };

            auto result = bitwuzla::Result::UNKNOWN;
            const Term compiled = question;
            bool swept          = false;
            // Both sides of a race are swept: compiling reassociates sums, which
            // can take apart the very intermediate a sweep needs, so each side
            // proves some of what the other cannot.
            if (settings.sweep) {
                result = ask(question, std::min(deadline, begin + kSweepHeadStart));
                if (result == bitwuzla::Result::UNKNOWN && !stop.load(std::memory_order_relaxed)
                    && Clock::now() < deadline)
                {
                    SweepSettings sweep{ .deadline = deadline, .stop = &stop, .rewrite_level = settings.rewrite_level };
                    SweepCounters counters;
                    question = SweepFormula(terms, { question }, sweep, &counters).front();
                    swept    = true;
                    auto &stats = Stats();
                    stats.swept.fetch_add(1, std::memory_order_relaxed);
                    stats.sweep_lemmas.fetch_add(counters.lemmas, std::memory_order_relaxed);
                    stats.sweep_proved.fetch_add(counters.proved, std::memory_order_relaxed);
                    stats.sweep_generalized.fetch_add(counters.generalized, std::memory_order_relaxed);
                    stats.sweep_synthesized.fetch_add(counters.synthesized, std::memory_order_relaxed);
                    result = ask(question, deadline);
                }
            } else {
                result = ask(question, deadline);
            }
            if (result == bitwuzla::Result::UNSAT) {
                return Answer::kEqual;
            }
            if (result == bitwuzla::Result::SAT) {
                return Answer::kDiffer;
            }
            timed_out = settings.timeout_ms > 0 && Clock::now() >= deadline;
            if (!stop.load(std::memory_order_relaxed)) {
                DumpUnanswered(terms, compiled, settings, optimize, swept);
            }
            return Answer::kUnknown;
        }

        Z3VerifyResult Result(Answer answer, bool timed_out, const Z3VerificationSettings &settings) {
            Z3VerifyResult result;
            if (answer == Answer::kEqual) {
                result.equivalent = true;
            } else if (answer == Answer::kUnknown) {
                result.unknown    = true;
                result.timed_out  = timed_out;
                result.equivalent = settings.unknown_result_mode == Z3UnknownResultMode::kTreatAsEquivalent;
                result.counterexample = timed_out ? "Bitwuzla returned unknown: timeout"
                                                  : "Bitwuzla returned unknown";
            }
            return result;
        }

        // ---- racing ----------------------------------------------------------

        // The other side of a race runs here: threads with a stack as deep as the
        // one the solver gets on the thread that asks.
        class RacePool
        {
          public:
            struct Job
            {
                std::shared_ptr< const Expr > lhs;
                std::shared_ptr< const Expr > rhs;
                size_t num_vars = 0;
                uint32_t bitwidth = 64;
                Z3VerificationSettings settings;
                // Set by the asker once it has its answer; stops this side.
                std::atomic< bool > asker_answered{ false };
                // Set by this side once it has a definite answer; stops the asker.
                std::atomic< bool > answered{ false };
                std::mutex mutex;
                std::condition_variable finished;
                enum State : uint8_t { kQueued, kRunning, kDone, kWithdrawn } state = kQueued;
                Answer answer  = Answer::kUnknown;
                bool timed_out = false;
            };

            static RacePool &Instance() {
                static auto *pool = new RacePool(ThreadCount());
                return *pool;
            }

            bool Enabled() const { return threads_ > 0; }

            void Submit(const std::shared_ptr< Job > &job) {
                const std::lock_guard< std::mutex > lock(mutex_);
                queue_.push_back(job);
                available_.notify_one();
            }

          private:
            static unsigned ThreadCount() {
                if (const char *count = std::getenv("COBRA_RACE_THREADS")) {
                    return static_cast< unsigned >(std::strtoul(count, nullptr, 10));
                }
                return 4;
            }

            explicit RacePool(unsigned threads) : threads_(threads) {
                for (unsigned index = 0; index < threads; ++index) {
                    pthread_attr_t attributes;
                    pthread_attr_init(&attributes);
                    pthread_attr_setstacksize(&attributes, size_t{ 64 } << 20);
                    pthread_t thread;
                    if (pthread_create(&thread, &attributes, &RacePool::Work, this) == 0) {
                        pthread_detach(thread);
                    }
                    pthread_attr_destroy(&attributes);
                }
            }

            static void *Work(void *self) {
                static_cast< RacePool * >(self)->Loop();
                return nullptr;
            }

            void Loop() {
                for (;;) {
                    std::shared_ptr< Job > job;
                    {
                        std::unique_lock< std::mutex > lock(mutex_);
                        available_.wait(lock, [&] { return !queue_.empty(); });
                        job = std::move(queue_.front());
                        queue_.pop_front();
                    }
                    {
                        const std::lock_guard< std::mutex > lock(job->mutex);
                        if (job->state != Job::kQueued) {
                            continue;
                        }
                        job->state = Job::kRunning;
                    }
                    bool timed_out = false;
                    Answer answer  = Answer::kUntranslatable;
                    try {
                        answer = Check(
                            *job->lhs, *job->rhs, job->num_vars, job->bitwidth, job->settings,
                            /*optimize=*/false, job->asker_answered, timed_out
                        );
                    } catch (...) {
                    }
                    if (answer == Answer::kEqual || answer == Answer::kDiffer) {
                        job->answered.store(true, std::memory_order_relaxed);
                    }
                    {
                        const std::lock_guard< std::mutex > lock(job->mutex);
                        job->answer    = answer;
                        job->timed_out = timed_out;
                        job->state     = Job::kDone;
                    }
                    job->finished.notify_all();
                }
            }

            unsigned threads_;
            std::mutex mutex_;
            std::condition_variable available_;
            std::deque< std::shared_ptr< Job > > queue_;
        };

        bool Definite(Answer answer) { return answer == Answer::kEqual || answer == Answer::kDiffer; }

    } // namespace

    Z3VerifyResult VerifyExprsWithStrategy(
        const Expr &lhs, const Expr &rhs, const std::vector< std::string > &var_names,
        uint32_t bitwidth, const Z3VerificationSettings &settings
    ) {
        if (settings.strategy == SmtStrategy::kDirect) {
            return Z3VerifyExprs(lhs, rhs, var_names, bitwidth, settings);
        }

        const std::atomic< bool > never{ false };
        if (settings.strategy == SmtStrategy::kLLVM || !RacePool::Instance().Enabled()) {
            bool timed_out = false;
            const Answer answer =
                Check(lhs, rhs, var_names.size(), bitwidth, settings, /*optimize=*/true, never, timed_out);
            if (answer == Answer::kUntranslatable) {
                Stats().fallbacks.fetch_add(1, std::memory_order_relaxed);
                return Z3VerifyExprs(lhs, rhs, var_names, bitwidth, settings);
            }
            return Result(answer, timed_out, settings);
        }

        // kRace: the question as built on a pool thread, the compiled one here.
        auto job      = std::make_shared< RacePool::Job >();
        job->lhs      = std::shared_ptr< const Expr >(CloneExpr(lhs));
        job->rhs      = std::shared_ptr< const Expr >(CloneExpr(rhs));
        job->num_vars = var_names.size();
        job->bitwidth = bitwidth;
        job->settings = settings;
        RacePool::Instance().Submit(job);

        bool timed_out = false;
        const Answer mine =
            Check(lhs, rhs, var_names.size(), bitwidth, settings, /*optimize=*/true, job->answered, timed_out);

        std::unique_lock< std::mutex > lock(job->mutex);
        if (Definite(mine)) {
            // The other side is stopped, or never started; the pool owns its
            // share of the job and lets go of it when it is done.
            job->asker_answered.store(true, std::memory_order_relaxed);
            if (job->state == RacePool::Job::kQueued) {
                job->state = RacePool::Job::kWithdrawn;
            }
            Stats().race_compiled_won.fetch_add(1, std::memory_order_relaxed);
            return Result(mine, timed_out, settings);
        }

        Answer theirs          = Answer::kUntranslatable;
        bool theirs_timed_out  = false;
        if (job->state == RacePool::Job::kQueued) {
            // No pool thread got to it: the question as built is asked here.
            job->state = RacePool::Job::kWithdrawn;
            lock.unlock();
            theirs = Check(
                lhs, rhs, var_names.size(), bitwidth, settings, /*optimize=*/false, never, theirs_timed_out
            );
        } else {
            job->finished.wait(lock, [&] { return job->state == RacePool::Job::kDone; });
            theirs           = job->answer;
            theirs_timed_out = job->timed_out;
        }

        if (Definite(theirs)) {
            Stats().race_direct_won.fetch_add(1, std::memory_order_relaxed);
            return Result(theirs, theirs_timed_out, settings);
        }
        if (mine == Answer::kUntranslatable && theirs == Answer::kUntranslatable) {
            Stats().fallbacks.fetch_add(1, std::memory_order_relaxed);
            return Z3VerifyExprs(lhs, rhs, var_names, bitwidth, settings);
        }
        Stats().race_both_unknown.fetch_add(1, std::memory_order_relaxed);
        return Result(Answer::kUnknown, timed_out || theirs_timed_out, settings);
    }

    SmtStrategyCounters SmtStrategyStatistics() {
        auto &stats = Stats();
        return SmtStrategyCounters{ .folded            = stats.folded.load(),
                                    .compiled          = stats.compiled.load(),
                                    .fallbacks         = stats.fallbacks.load(),
                                    .race_direct_won   = stats.race_direct_won.load(),
                                    .race_compiled_won = stats.race_compiled_won.load(),
                                    .race_both_unknown = stats.race_both_unknown.load(),
                                    .swept             = stats.swept.load(),
                                    .sweep_lemmas      = stats.sweep_lemmas.load(),
                                    .sweep_proved      = stats.sweep_proved.load(),
                                    .sweep_generalized = stats.sweep_generalized.load(),
                                    .sweep_synthesized = stats.sweep_synthesized.load() };
    }

} // namespace cobra

#endif // COBRA_HAS_BITWUZLA
