#include "cobra/llvm/CobraPass.h"

#include <cstdlib>
#include <string>

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
                 // `COBRA_FAMILIES=semilinear,bitwise,...` restricts the
                 // techniques every pipeline below runs with. A consumer picks
                 // its own set (the deobfuscator's `cobra-techniques`), and a
                 // test that is about what the search *cannot* do has to be able
                 // to pin that set, or it passes because a family the consumer
                 // never enables answered.
                 // `COBRA_TIMEOUT_MS` gives every proof below a budget of its
                 // own. A test about a rewrite that needs a proof must not
                 // depend on how fast the machine happened to be - the budget is
                 // what decides whether the proof finishes, so a test pins it.
                 const auto timeout = [](uint32_t chosen) {
                     const char *value = std::getenv("COBRA_TIMEOUT_MS");
                     if (value == nullptr) {
                         return chosen;
                     }
                     const auto asked = std::strtoul(value, nullptr, 10);
                     return asked == 0 ? chosen : static_cast< uint32_t >(asked);
                 };

                 const auto families = [](cobra::TechniqueFamily chosen) {
                     const char *value = std::getenv("COBRA_FAMILIES");
                     if (value == nullptr) {
                         return chosen;
                     }
                     const std::string list = "," + std::string(value) + ",";
                     const auto has         = [&](const char *name) {
                         return list.find("," + std::string(name) + ",") != std::string::npos;
                     };
                     auto picked = static_cast< cobra::TechniqueFamily >(0);
                     if (has("semilinear")) { picked = picked | cobra::TechniqueFamily::kSemilinear; }
                     if (has("polynomial")) { picked = picked | cobra::TechniqueFamily::kPolynomial; }
                     if (has("template")) { picked = picked | cobra::TechniqueFamily::kTemplate; }
                     if (has("bitwise")) { picked = picked | cobra::TechniqueFamily::kBitwise; }
                     if (has("ghost")) { picked = picked | cobra::TechniqueFamily::kGhost; }
                     if (has("lifting")) { picked = picked | cobra::TechniqueFamily::kLifting; }
                     return picked;
                 };

                 pb.registerPipelineParsingCallback(
                     [families, timeout](llvm::StringRef name, llvm::FunctionPassManager &fpm,
                        llvm::ArrayRef< llvm::PassBuilder::PipelineElement >) {
                         if (name == "cobra-simplify") {
                             cobra::CobraPassOptions options;
                             options.enabled_families = families(options.enabled_families);
                             fpm.addPass(cobra::CobraPass(options));
                             return true;
                         }
                         // Every rewrite proved by the solver, which is what
                         // a tree that holds a comparison needs before it is
                         // looked at at all.
                         if (name == "cobra-simplify<verified>") {
                             cobra::CobraPassOptions options;
                             options.z3_verify              = true;
                             options.enabled_families       = families(options.enabled_families);
                             options.z3_settings.timeout_ms = timeout(options.z3_settings.timeout_ms);
                             fpm.addPass(cobra::CobraPass(options));
                             return true;
                         }
                         // The same, and the word-level sweep beside it. A
                         // tree whose answer is one of its own inputs is only
                         // offered where the sweep makes the proof it needs
                         // affordable, so a test of that reading asks for both.
                         if (name == "cobra-simplify<verified-sweep>") {
                             cobra::CobraPassOptions options;
                             options.z3_verify              = true;
                             options.z3_settings.sweep      = true;
                             options.enabled_families       = families(options.enabled_families);
                             options.z3_settings.timeout_ms = timeout(options.z3_settings.timeout_ms);
                             fpm.addPass(cobra::CobraPass(options));
                             return true;
                         }
                         // Conditions read as variables, every rewrite proved
                         // by the solver (`boolean_variables`).
                         if (name == "cobra-simplify<verified-boolean>") {
                             cobra::CobraPassOptions options;
                             options.z3_verify              = true;
                             options.boolean_variables      = true;
                             options.enabled_families       = families(options.enabled_families);
                             options.z3_settings.timeout_ms = timeout(options.z3_settings.timeout_ms);
                             fpm.addPass(cobra::CobraPass(options));
                             return true;
                         }
                         // The reading asked for without the solver it needs:
                         // it must then read nothing.
                         if (name == "cobra-simplify<boolean>") {
                             cobra::CobraPassOptions options;
                             options.boolean_variables = true;
                             options.enabled_families  = families(options.enabled_families);
                             fpm.addPass(cobra::CobraPass(options));
                             return true;
                         }
                         if (name == "cobra-simplify<dying-cost>") {
                             cobra::CobraPassOptions options;
                             options.cost_model       = cobra::MbaCostModel::kDyingInstructions;
                             options.enabled_families = families(options.enabled_families);
                             fpm.addPass(cobra::CobraPass(options));
                             return true;
                         }
                         return false;
                     }
                 );
             } };
}
