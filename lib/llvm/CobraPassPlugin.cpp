#include "cobra/llvm/CobraPass.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/Compiler.h"

#if LLVM_VERSION_MAJOR >= 22
    #include "llvm/Plugins/PassPlugin.h"
#else
    #include "llvm/Passes/PassPlugin.h"
#endif

// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo llvmGetPassPluginInfo() {
    return { .APIVersion                   = LLVM_PLUGIN_API_VERSION,
             .PluginName                   = "CobraPass",
             .PluginVersion                = LLVM_VERSION_STRING,
             .RegisterPassBuilderCallbacks = [](llvm::PassBuilder &pb) {
                 pb.registerPipelineParsingCallback(
                     [](llvm::StringRef name, llvm::FunctionPassManager &fpm,
                        llvm::ArrayRef< llvm::PassBuilder::PipelineElement >) {
                         if (name == "cobra-simplify") {
                             fpm.addPass(cobra::CobraPass());
                             return true;
                         }
                         // Every rewrite proved by the solver, which is what
                         // a tree that holds a comparison needs before it is
                         // looked at at all.
                         if (name == "cobra-simplify<verified>") {
                             cobra::CobraPassOptions options;
                             options.z3_verify = true;
                             fpm.addPass(cobra::CobraPass(options));
                             return true;
                         }
                         if (name == "cobra-simplify<dying-cost>") {
                             cobra::CobraPassOptions options;
                             options.cost_model = cobra::MbaCostModel::kDyingInstructions;
                             fpm.addPass(cobra::CobraPass(options));
                             return true;
                         }
                         return false;
                     }
                 );
             } };
}
