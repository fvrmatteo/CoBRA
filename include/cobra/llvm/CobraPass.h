#pragma once

#include "cobra/core/Simplifier.h"
#include "cobra/verify/Z3Verifier.h"

#include <cstdint>

#include "llvm/IR/PassManager.h"

namespace cobra {

    // Which leaves a rewrite freezes before reading them.
    //
    // A rewrite reads its leaves directly. Where the tree did not - through a
    // `freeze`, through the arm of a `select` it did not take - a leaf that is
    // poison could leave the root defined, and a leaf that is `undef` and read
    // more than once by the rewrite could be read as two different values.
    enum class LeafFreezing : uint8_t {
        // Every leaf that can be poison while the root is defined (behind a
        // `freeze` or a `select`'s arm only), and every `undef` leaf the
        // rewrite reads more than once. Exact for LLVM's semantics.
        kSound,

        // Only a tree collected through a `freeze`, and then every leaf that may
        // be poison. For a host that keeps its IR free of poison and `undef` by
        // construction, where a `freeze` only stands in the way of folding.
        kAcrossFreeze,
    };

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

        // At most how many times `z3_settings.timeout_ms` a proof is given: the budget
        // grows by one for every eight instructions of the tree being replaced
        // (`ProofBudgetScale`), up to this. A proof that comes back is quick whatever
        // it was given; the large budgets are spent on the ones that do not.
        uint32_t max_budget_scale = 8;

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

        // See `LeafFreezing`.
        LeafFreezing leaf_freezing = LeafFreezing::kSound;

        // Read every condition in a rejected tree as a variable of its own.
        //
        // A comparison, and the sign bit of a value (`x >> (w - 1)`, which is
        // also how `ashr x, w - 1` and the low bit of `fshl(a, b, 1)` read), is
        // 0 or 1 on every input. Searched as part of the word it stands in, it
        // is out of reach twice over: whether any bit of a word is set is not a
        // function of the word's bits one at a time, which is all a mixed
        // Boolean-arithmetic basis can say, and a shift by `w - 1` is not the
        // same expression at the narrower widths the search solves in. Read as
        // a variable that is 0 or 1, the tree above it is an ordinary
        // expression over those variables, and the search can answer it.
        //
        // Two such variables that stand for the same condition - or for one
        // condition and its negation - are one variable, but only once the
        // solver has proved it over the values the conditions are computed
        // from; a merge the solver does not prove is not made.
        //
        // A rewrite found this way is put back in terms of the conditions it
        // read and must then be proved equal to the tree by the solver: no
        // enumeration, ring normal form or `unknown` answer stands in for that
        // proof. So the reading is only tried with `z3_verify` on, and only for
        // a tree the ordinary search has already turned down.
        //
        // Off by default.
        bool boolean_variables = false;

        // Read a condition nothing but a branch looks at as a root of its own.
        //
        // The pass rewrites integer-valued roots: a condition is `i1`, worth
        // following into a wider tree but never a candidate itself, because
        // rewriting a value one bit wide into another says nothing. That holds
        // only while something wider reads it. A comparison whose readers are
        // branches is the top of its own tree, and the arithmetic under it -
        // the obfuscator's, with the comparison its last step - is then read by
        // nobody: a rotate of a sign-extended condition, a sum tested against a
        // constant, a product whose top bit a jump looks at. The tree is there
        // and the search can answer it; what is missing is a root.
        //
        // Such a comparison is rooted at the width of its operands, where its
        // value is the 0 or 1 the comparison yields, and only that bit is read
        // (`demanded_mask` of 1): what the rewrite has to agree on is the answer,
        // not a word. A rewrite is written back as a condition again - the
        // comparison the expression ends in, or the bit itself - so the branch
        // reads what it always read, one bit, computed by less.
        //
        // The reading costs what it adds: a comparison per branch is a root per
        // branch, and a function of nothing but tests pays for a tree at each
        // one. It is off by default, for a consumer that reads its output rather
        // than its speed.
        bool read_conditions = false;

        // Read a root the other readings turned down as a bitwise function of
        // one or two of its own arithmetic subterms (`ReadAsBitwiseOfSubterms`):
        // the root and the subterms evaluated on the same inputs, every bit of
        // the root a function of the same bit of the subterms. An obfuscator
        // writes `(a - b - 32) ^ k` through halvings and a carry picked by a
        // comparison; the semilinear reading holds those pieces as independent
        // atoms and finds nothing, while read together they are `d ^ k` over
        // the subterm `d` the tree already computes. The probes only propose:
        // the rewrite is proved like any other. Off by default.
        bool subterm_reading = false;

        // Read every root also as an affine function of its leaves xored with a
        // constant (`ReadAsAffineOfLeaves`): `(K + sum(a_i * L_i)) ^ C` over the
        // leaves as the tree reads them, the top bit of a narrow leaf included,
        // on the bits the root's readers look at. An obfuscator writes such a
        // sum through halvings, carries picked by comparisons and words taken
        // apart and put back, each piece an independent atom to the semilinear
        // reading; evaluated on the leaves, the root is the sum on every input.
        // The probes are cheap and only propose: the cheaper of their answer and
        // the search's is the rewrite, proved like any other, and one the proof
        // turns down hands the tree back to the other readings
        // (`VerifyCandidateOrOrdinary`). Off by default.
        bool affine_reading = false;

        // Read a root narrower than the values it reads at the width of those
        // values (`NarrowRootsReadWide`): a byte flag computed from the top bit
        // of a 64-bit sum, or a comparison of 64-bit values widened to a byte,
        // is read as a 64-bit tree demanding the root's own bits, and the
        // rewrite truncated back. Read at its own width such a root stops at the
        // wide values - a shift that brings high bits down, a comparison - and
        // whatever the obfuscator wrote into them is a free leaf: a flag that
        // is constant on every input is no function of a free leaf. The narrow
        // nodes of the tree are held zero-extended and masked, as in any wider
        // tree, so the reading sees everything the narrow one did. Off by
        // default.
        bool read_narrow_roots_wide = false;

        // Build a part of a rewrite that is one of two constants, picked by one
        // bit of another value, as the `select` of the two on that bit
        // (`ReadTwoValued` in the reconstruction), where that costs no more
        // instructions than the arithmetic the rewrite holds. The search writes
        // such a term in operators - `((t | -2) + 1) & -194` is `-194` or `0` by
        // the low bit of `t` - and what LLVM's known bits and value ranges make
        // of that is nothing; of `select (trunc t), 0, -194` they read the two
        // values, and a comparison or a sum over them folds downstream. The value
        // is the same either way. Off by default.
        bool two_valued_selects = false;

        // Never let a value other trees read stop a root from being read and
        // rewritten. A tree is collected through values the rest of the function
        // also reads, and a rewrite of the root leaves them for their other
        // readers; profit is the rewrite against the whole tree as the root reads
        // it, the shared values counted as if the root alone read them. Each of
        // these used to refuse where an intermediate had a second reader:
        //
        // - a truncation was no root (`SharedValuesRead`): `trunc ((hi << 16 | lo)
        //   ^ K) + 1` to sixteen bits is `lo + 1`, while the 32-bit sum another
        //   reader keeps needs `hi`;
        // - a value read whole by one reader and narrowly by another was read
        //   whole only: it is read again under each narrower reader's bits, and
        //   rewritten in those readers' uses (`ReaderMbaCandidates`); and a right
        //   shift at the top of a tree read narrowly reads its operand under the
        //   bits it brings down (`Options::read_shifted_operand`);
        // - a right shift a narrow tree reads the low bits of was a leaf when a
        //   store, a call or a comparison also read it whole;
        // - a comparison read by a branch and by a tree was no root of its own;
        // - an extension of a bitwise operation was not read over the extension
        //   the function holds, a sum of products by a shared factor did not have
        //   it taken out, and a `select` whose arms agree on what some readers
        //   read was replaced only when every reader read that little;
        // - a masked equality and a reading of arms under their condition were
        //   priced against what died with them, not against what they read;
        // - a tree a rewrite of a value it shares changed was left for the next
        //   run; it is read again in this one.
        //
        // Off by default.
        bool unblock_shared_values = false;

        // Read the intrinsics flags are written with as the operations they
        // are, not as leaves (`BitIntrinsicsRead`): `bitreverse` - InstCombine's
        // spelling of a flag word put together bit by bit - as its bits moved,
        // and the overflow bit of `ssub.with.overflow`, with the difference
        // beside it, as the sign of `(a ^ b) & (a ^ (a - b))`. A flag word and
        // the overflow of a comparison are then functions of the values they
        // read; as leaves they are free bits, and a flag the obfuscator made
        // constant is no constant of a free bit. Each is written in the
        // operators the model has, so every rewrite over them is proved like any
        // other. Only the bits of an operand that can be set are read. A
        // population count is not read: see `BitReverseOf`. Off by default.
        bool read_bit_intrinsics = false;

        // Read a root whose tree ends in more leaves than `max_vars` with its
        // tree cut short, instead of turning it down.
        //
        // The tree of a root is collected down to values nothing simplifies
        // further, and in a long chain - a hash taken one byte at a time - every
        // root's tree reaches all of its bytes. A root that computes, from the
        // value just below it, a word written the long way round (`-y` as
        // `((~y << 1) | 2) - (y ^ -2)`) is then never asked about, because its
        // tree holds every byte of the chain. Cut at the nodes nearest the root,
        // the tree is the identity over that one value. The first cut is the
        // smallest that is a candidate at all, and each rejection cuts again one
        // node deeper, up to `max_recut_nodes`, as the re-cuts of any rejected
        // root do.
        //
        // Every such root costs a ladder of small searches, so it is off by
        // default, for a consumer that reads its output rather than its speed.
        bool cut_wide_roots = false;

        // Hold a turned-down root's fingerprint until every slice, sum group
        // and cut queued from it has decided.
        //
        // A fingerprint tells later runs the tree at a root was examined and
        // holds nothing, and they skip it - with it every rung of its ladder.
        // Written as soon as the full collection is turned down, it also skips
        // the rungs whose proof ran out of time, whose rewrite did not fit the
        // budget or whose tree went stale this run, which a later run could
        // still win. With this on, such a root keeps no fingerprint and the
        // next run collects it again, at the price of collecting and re-cutting
        // it every run until its ladder decides. The outcome cache and the
        // memo of proofs that ran out of budget keep the repeated questions
        // cheap, but not free.
        //
        // Off by default.
        bool retry_undecided_roots = false;

        // Carry a root rewritten to a constant into the trees that read it, in
        // the same run.
        //
        // A tree queued before a root inside it was replaced is stale, and is
        // left for the next run, which collects it from the simplified
        // function. When the replacement is a constant, the next run only ever
        // sees the same tree with that constant in it, and a chain whose every
        // link reads the one before it - a running key, a word-by-word
        // decryption whose key is the sum of the words already decrypted -
        // then advances by the few links one tree spans per run. With this on,
        // the constant's readers whose operands are now all constants are
        // folded, and every root whose tree held the replaced root or a folded
        // reader is collected again from the function as it now stands, in
        // this run: queued again when it was already tried, and re-read in
        // place when it was not. One run then walks the whole chain.
        //
        // A root replaced by anything but a constant keeps the rule above.
        //
        // Off by default.
        bool propagate_constant_roots = false;

        // Answer every demanded-bits question both from the readers and from
        // LLVM's whole-function analysis, and report each disagreement on
        // stderr. A check on the pass, for its tests; it changes no answer.
        bool check_demanded_bits = false;

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
