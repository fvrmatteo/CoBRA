#include "cobra/llvm/CobraPass.h"
#include "cobra/llvm/CobraStats.h"

#include <cstdint>
#include <string>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#if LLVM_VERSION_MAJOR >= 22
    #include "llvm/Plugins/PassPlugin.h"
#else
    #include "llvm/Passes/PassPlugin.h"
#endif

namespace {

    llvm::Error Invalid(llvm::StringRef parameter) {
        return llvm::make_error< llvm::StringError >(
            "invalid cobra-simplify parameter '" + parameter.str() + "'",
            llvm::inconvertibleErrorCode()
        );
    }

    llvm::Expected< uint32_t > Number(llvm::StringRef parameter, llvm::StringRef value) {
        uint32_t number = 0;
        if (value.getAsInteger(10, number)) {
            return Invalid(parameter);
        }
        return number;
    }

    // `cobra-simplify<p1;p2;...>`, each parameter one `CobraPassOptions` field:
    //
    //   verified                 z3_verify
    //   sweep                    z3_settings.sweep
    //   boolean                  boolean_variables
    //   conditions               read_conditions
    //   wide-roots               cut_wide_roots
    //   dying-cost               cost_model = kDyingInstructions
    //   skip-loop-carried-phis   skip_loop_carried_phis
    //   across-freeze            leaf_freezing = kAcrossFreeze
    //   retry-undecided-roots    retry_undecided_roots
    //   propagate-constants      propagate_constant_roots
    //   demanded-check           check_demanded_bits
    //   unknown-as-equivalent    z3_settings.unknown_result_mode = kTreatAsEquivalent
    //   strategy=direct|llvm|race
    //   families=semilinear+polynomial+template+bitwise+ghost+lifting
    //   timeout=<ms>  rewrite-level=<n>  max-vars=<n>  min-ast-size=<n>
    //   max-tree-nodes=<n>  max-shared-cuts=<n>  max-recut-nodes=<n>  max-recut-vars=<n>
    //
    // `verified-sweep`, `verified-boolean` and `verified-conditions` stand for
    // the two together. A test
    // that is about what the search cannot do pins `families`, or it passes
    // because a family the consumer never enables answered; one about a rewrite
    // that needs a proof pins `timeout`, or how fast the machine happened to be
    // decides it.
    llvm::Expected< cobra::CobraPassOptions > ParseOptions(llvm::StringRef text) {
        cobra::CobraPassOptions options;
        llvm::SmallVector< llvm::StringRef, 8 > parameters;
        text.split(parameters, ';', -1, false);
        for (const llvm::StringRef parameter : parameters) {
            const auto [name, value] = parameter.split('=');
            const bool has_value     = parameter.contains('=');
            if (!has_value) {
                if (name == "verified") {
                    options.z3_verify = true;
                } else if (name == "sweep") {
                    options.z3_settings.sweep = true;
                } else if (name == "verified-sweep") {
                    options.z3_verify         = true;
                    options.z3_settings.sweep = true;
                } else if (name == "boolean") {
                    options.boolean_variables = true;
                } else if (name == "conditions") {
                    options.read_conditions = true;
                } else if (name == "wide-roots") {
                    options.cut_wide_roots = true;
                } else if (name == "verified-conditions") {
                    options.z3_verify       = true;
                    options.read_conditions = true;
                } else if (name == "verified-boolean") {
                    options.z3_verify         = true;
                    options.boolean_variables = true;
                } else if (name == "dying-cost") {
                    options.cost_model = cobra::MbaCostModel::kDyingInstructions;
                } else if (name == "skip-loop-carried-phis") {
                    options.skip_loop_carried_phis = true;
                } else if (name == "across-freeze") {
                    options.leaf_freezing = cobra::LeafFreezing::kAcrossFreeze;
                } else if (name == "retry-undecided-roots") {
                    options.retry_undecided_roots = true;
                } else if (name == "propagate-constants") {
                    options.propagate_constant_roots = true;
                } else if (name == "demanded-check") {
                    options.check_demanded_bits = true;
                } else if (name == "unknown-as-equivalent") {
                    options.z3_settings.unknown_result_mode =
                        cobra::Z3UnknownResultMode::kTreatAsEquivalent;
                } else {
                    return Invalid(parameter);
                }
                continue;
            }
            if (name == "strategy") {
                if (value == "direct") {
                    options.z3_settings.strategy = cobra::SmtStrategy::kDirect;
                } else if (value == "llvm") {
                    options.z3_settings.strategy = cobra::SmtStrategy::kLLVM;
                } else if (value == "race") {
                    options.z3_settings.strategy = cobra::SmtStrategy::kRace;
                } else {
                    return Invalid(parameter);
                }
                continue;
            }
            if (name == "families") {
                auto picked = cobra::TechniqueFamily::kNone;
                llvm::SmallVector< llvm::StringRef, 6 > names;
                value.split(names, '+', -1, false);
                for (const llvm::StringRef family : names) {
                    const auto bit =
                        llvm::StringSwitch< cobra::TechniqueFamily >(family)
                            .Case("semilinear", cobra::TechniqueFamily::kSemilinear)
                            .Case("polynomial", cobra::TechniqueFamily::kPolynomial)
                            .Case("template", cobra::TechniqueFamily::kTemplate)
                            .Case("bitwise", cobra::TechniqueFamily::kBitwise)
                            .Case("ghost", cobra::TechniqueFamily::kGhost)
                            .Case("lifting", cobra::TechniqueFamily::kLifting)
                            .Default(cobra::TechniqueFamily::kNone);
                    if (bit == cobra::TechniqueFamily::kNone) {
                        return Invalid(parameter);
                    }
                    picked = picked | bit;
                }
                options.enabled_families = picked;
                continue;
            }
            uint32_t *field = llvm::StringSwitch< uint32_t * >(name)
                                  .Case("timeout", &options.z3_settings.timeout_ms)
                                  .Case("rewrite-level", &options.z3_settings.rewrite_level)
                                  .Case("max-vars", &options.max_vars)
                                  .Case("min-ast-size", &options.min_ast_size)
                                  .Case("max-tree-nodes", &options.max_tree_nodes)
                                  .Case("max-shared-cuts", &options.max_shared_cuts)
                                  .Case("max-recut-nodes", &options.max_recut_nodes)
                                  .Case("max-recut-vars", &options.max_recut_vars)
                                  .Default(nullptr);
            if (field == nullptr) {
                return Invalid(parameter);
            }
            auto number = Number(parameter, value);
            if (!number) {
                return number.takeError();
            }
            *field = *number;
        }
        return options;
    }

    // `cobra-stats`: the pass's report so far (`PrintStatistics`) on stderr,
    // for a pipeline that ends with it.
    struct PrintStatisticsPass : llvm::PassInfoMixin< PrintStatisticsPass >
    {
        // NOLINTNEXTLINE(readability-identifier-naming) - LLVM interface
        llvm::PreservedAnalyses run(llvm::Module &, llvm::ModuleAnalysisManager &) {
            cobra::PrintStatistics(llvm::errs());
            return llvm::PreservedAnalyses::all();
        }

        // NOLINTNEXTLINE(readability-identifier-naming) - LLVM interface
        static bool isRequired() { return true; }
    };

} // namespace

// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo llvmGetPassPluginInfo() {
    return { .APIVersion                   = LLVM_PLUGIN_API_VERSION,
             .PluginName                   = "CobraPass",
             .PluginVersion                = LLVM_VERSION_STRING,
             .RegisterPassBuilderCallbacks = [](llvm::PassBuilder &pb) {
                 pb.registerPipelineParsingCallback(
                     [](llvm::StringRef name, llvm::FunctionPassManager &fpm,
                        llvm::ArrayRef< llvm::PassBuilder::PipelineElement >) {
                         if (!llvm::PassBuilder::checkParametrizedPassName(
                                 name, "cobra-simplify"
                             )) {
                             return false;
                         }
                         auto options = llvm::PassBuilder::parsePassParameters(
                             ParseOptions, name, "cobra-simplify"
                         );
                         if (!options) {
                             llvm::errs() << llvm::toString(options.takeError()) << "\n";
                             return false;
                         }
                         fpm.addPass(cobra::CobraPass(*options));
                         return true;
                     }
                 );
                 pb.registerPipelineParsingCallback(
                     [](llvm::StringRef name, llvm::ModulePassManager &mpm,
                        llvm::ArrayRef< llvm::PassBuilder::PipelineElement >) {
                         if (name != "cobra-stats") {
                             return false;
                         }
                         mpm.addPass(PrintStatisticsPass());
                         return true;
                     }
                 );
             } };
}
