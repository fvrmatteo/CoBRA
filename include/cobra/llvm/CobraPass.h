#pragma once

#include "cobra/core/Simplifier.h"
#include "cobra/verify/Z3Verifier.h"

#include <cstdint>

#include "llvm/IR/PassManager.h"

namespace cobra {

    // What a rewrite has to be cheaper than before it is worth committing.
    enum class MbaCostModel : uint8_t {
        // Every instruction the tree was built from, which is the expression
        // CoBRA was handed. A shared operand is left to the other readers that
        // need it anyway, so what gets priced is the IR in against the IR out.
        kTreeInstructions,

        // Only the instructions the rewrite deletes: the root, plus everything
        // beneath it that nothing else reads. This never grows the function, but
        // it also declines a root whose operands are shared with the rest of the
        // block no matter how much simpler the root itself becomes — and a
        // simpler root is often the whole point, because it is what alias
        // analysis and the later passes actually look at.
        kDyingInstructions,
    };

    struct CobraPassOptions
    {
        uint32_t max_vars     = 16;
        uint32_t min_ast_size = 3;

        // How much tree a re-cut of a rejected root is allowed to keep. The
        // cuts that pay for themselves are small: measured over a heavily
        // obfuscated function, every rewrite a re-cut ever won came from a cut
        // of eight instructions or fewer, while the uncapped ladder went on to
        // build several thousand larger cuts that won nothing and cost a
        // signature sweep and a solve apiece.
        uint32_t max_recut_nodes = 12;

        // How many variables the collection that spawns a ladder may end in.
        // Re-cutting is worth doing when exhaustive expansion hid an identity
        // the root was written in; a collection that already resolves to many
        // independent leaves hid nothing. Measured over a heavily obfuscated
        // function, ladders started from collections of five variables or more
        // were a quarter of every cut ever built and won two rewrites out of
        // twenty-eight.
        uint32_t max_recut_vars = 6;

        // How many rungs the shared-value ladder of a rejected root may climb:
        // the root re-collected with the values its tree reads more than once
        // kept as variables, one more of them expanded per rejection. The
        // first rungs are the ones that read an identity in the operands it was
        // written in; each further one is closer to the full collection that
        // was already rejected. Zero turns the ladder off.
        uint32_t max_shared_cuts = 4;

        // How many instructions a collection may hold before the walk stops
        // expanding and keeps what remains as leaves. Values narrower than the
        // root join a tree through their casts, so a tree is no longer bounded
        // by the width changes in the code it reads; without a cap a
        // flag-word computation of eighty instructions and two variables is
        // collected once per bit that reads it, and every one of those
        // collections is solved and rejected at a hundred milliseconds apiece.
        // The expressions a rewrite comes from are far smaller: a word split
        // into four masked halves and put back together is under thirty.
        // Zero leaves the walk unbounded.
        uint32_t max_tree_nodes = 48;

        // Decline a candidate whose tree stands on a value a loop carries.
        //
        // The collection looks through phis, handing on one arm, so a phi at the head
        // of a loop can sit inside a tree or be one of its leaves - and then the
        // expression the candidate describes is not one expression: the search is being
        // asked for a closed form of a value that has none. The solver mostly answers
        // `unknown`, which produces no rewrite at all.
        //
        // A candidate that reaches a **pointer offset** is kept whatever this says,
        // because crossing the split with what reads the root shows the cost is not there:
        // of the 2597 loop-carried candidates on one measured CFG, the 679 that reach an
        // address cost 0.13 s of the 55.24 s, while `stored value` (29.69 s) and
        // `compared` (24.97 s) are the whole of it. So this keeps every rewrite that can
        // make a pointer fold.
        //
        // Not free even so, which is why it is a setting rather than a rule. On the same
        // CFG and the same 11 runs, with the whole pass at 58.43 s and 321 rewrites: the
        // candidates standing on a loop-carried value cost **55.27 s (95%)** and produced
        // **42 of the 321 rewrites (13%)**. Declining the non-address ones is most of that
        // 95% for 31 of those 42 - a trade worth offering and not worth making silently.
        bool skip_loop_carried_phis = false;

        bool z3_verify = false;
        Z3VerificationSettings z3_settings;
        TechniqueFamily enabled_families = TechniqueFamily::kAll;
        MbaCostModel cost_model          = MbaCostModel::kTreeInstructions;
    };

    class CobraPass : public llvm::PassInfoMixin< CobraPass >
    {
      public:
        explicit CobraPass(uint32_t max_vars = 16, uint32_t min_ast_size = 3)
            : CobraPass(
                  CobraPassOptions{ .max_vars = max_vars, .min_ast_size = min_ast_size }
              ) {}

        explicit CobraPass(CobraPassOptions options) : options_(options) {}

        // NOLINTNEXTLINE(readability-identifier-naming) - LLVM PassInfoMixin requires 'run'
        llvm::PreservedAnalyses run(llvm::Function &f, llvm::FunctionAnalysisManager &am);

        static bool isRequired() {
            return false;
        } // NOLINT(readability-identifier-naming) - LLVM interface

      private:
        CobraPassOptions options_;
    };

} // namespace cobra
