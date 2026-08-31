// Bitwuzla implementation of the verification API declared in Z3Verifier.h.
//
// This is an alternative to Z3Verifier.cpp: the build compiles exactly one of
// the two into cobra-verify, so both provide the same Z3Verify/Z3VerifyExprs
// entry points. Bitwuzla is preferred when the superbuild provides it because
// it discharges these bitvector queries faster; Z3 remains the fallback.
#include "cobra/verify/Z3Verifier.h"
#include "cobra/core/Expr.h"
#include <bitwuzla/cpp/bitwuzla.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace cobra {

    namespace {

        bitwuzla::Term BuildSmtExpr(
            bitwuzla::TermManager &tm, const Expr &expr,
            const std::vector< bitwuzla::Term > &var_terms, uint32_t bitwidth
        ) {
            const auto kBinary = [&](bitwuzla::Kind kind) {
                return tm.mk_term(
                    kind,
                    { BuildSmtExpr(tm, *expr.children[0], var_terms, bitwidth),
                      BuildSmtExpr(tm, *expr.children[1], var_terms, bitwidth) }
                );
            };
            const auto kUnary = [&](bitwuzla::Kind kind) {
                return tm.mk_term(
                    kind, { BuildSmtExpr(tm, *expr.children[0], var_terms, bitwidth) }
                );
            };

            switch (expr.kind) {
                case Expr::Kind::kConstant:
                    return tm.mk_bv_value_uint64(tm.mk_bv_sort(bitwidth), expr.constant_val);
                case Expr::Kind::kVariable:
                    return var_terms[expr.var_index];
                case Expr::Kind::kAdd:
                    return kBinary(bitwuzla::Kind::BV_ADD);
                case Expr::Kind::kMul:
                    return kBinary(bitwuzla::Kind::BV_MUL);
                case Expr::Kind::kAnd:
                    return kBinary(bitwuzla::Kind::BV_AND);
                case Expr::Kind::kOr:
                    return kBinary(bitwuzla::Kind::BV_OR);
                case Expr::Kind::kXor:
                    return kBinary(bitwuzla::Kind::BV_XOR);
                case Expr::Kind::kNot:
                    return kUnary(bitwuzla::Kind::BV_NOT);
                case Expr::Kind::kNeg:
                    return kUnary(bitwuzla::Kind::BV_NEG);
                    // Comparisons are 0/1 valued, so the boolean result is
                    // widened back to the expression's bitvector sort.
                    case Expr::Kind::kCmpEq:
                    case Expr::Kind::kCmpUlt:
                    case Expr::Kind::kCmpSlt: {
                        const auto kKind = expr.kind == Expr::Kind::kCmpEq
                            ? bitwuzla::Kind::EQUAL
                            : (expr.kind == Expr::Kind::kCmpUlt ? bitwuzla::Kind::BV_ULT
                                                                : bitwuzla::Kind::BV_SLT);
                        auto cmp         = tm.mk_term(
                            kKind,
                            { BuildSmtExpr(tm, *expr.children[0], var_terms, bitwidth),
                                      BuildSmtExpr(tm, *expr.children[1], var_terms, bitwidth) }
                        );
                        const auto kSort = tm.mk_bv_sort(bitwidth);
                        return tm.mk_term(
                            bitwuzla::Kind::ITE,
                            { cmp, tm.mk_bv_value_uint64(kSort, 1),
                              tm.mk_bv_value_uint64(kSort, 0) }
                        );
                    }
                    case Expr::Kind::kShr: {
                    // For kShr, constant_val carries the shift amount.
                    auto operand = BuildSmtExpr(tm, *expr.children[0], var_terms, bitwidth);
                    auto amount =
                        tm.mk_bv_value_uint64(tm.mk_bv_sort(bitwidth), expr.constant_val);
                    return tm.mk_term(bitwuzla::Kind::BV_SHR, { operand, amount });
                }
            }
            return tm.mk_bv_value_uint64(tm.mk_bv_sort(bitwidth), 0); // unreachable
        }

        // Build the expression from CoB coefficients: sum over all subsets S of
        // variables of coeffs[S] * AND(vars in S). Index i encodes subset S as a
        // bitmask, so popcount(i)==0 is the constant, popcount(i)==1 is a single
        // variable, and popcount(i)>=2 is an AND-product of multiple variables.
        bitwuzla::Term BuildOriginalFromCoeffs(
            bitwuzla::TermManager &tm, const std::vector< uint64_t > &coeffs,
            const std::vector< bitwuzla::Term > &var_terms, uint32_t num_vars, uint32_t bitwidth
        ) {
            const auto kSort = tm.mk_bv_sort(bitwidth);
            auto result      = tm.mk_bv_value_uint64(kSort, coeffs[0]);

            const size_t kLen = 1ULL << num_vars;
            for (size_t i = 1; i < kLen; ++i) {
                if (coeffs[i] == 0) {
                    continue;
                }

                // Build AND-product of all variables whose bits are set in i
                bool have_product = false;
                bitwuzla::Term product;
                for (uint32_t v = 0; v < num_vars; ++v) {
                    if ((i & (1ULL << v)) == 0u) {
                        continue;
                    }
                    if (!have_product) {
                        product      = var_terms[v];
                        have_product = true;
                    } else {
                        product = tm.mk_term(bitwuzla::Kind::BV_AND, { product, var_terms[v] });
                    }
                }

                auto coeff = tm.mk_bv_value_uint64(kSort, coeffs[i]);
                auto term  = tm.mk_term(bitwuzla::Kind::BV_MUL, { coeff, product });
                result     = tm.mk_term(bitwuzla::Kind::BV_ADD, { result, term });
            }
            return result;
        }

        bitwuzla::Options BuildOptions(const Z3VerificationSettings &settings) {
            bitwuzla::Options options;
            // Models are only read on the SAT path, where they carry the
            // counterexample the caller reports.
            options.set(bitwuzla::Option::PRODUCE_MODELS, true);
            // Deliberately no ABSTRACTION_INC_BITBLAST here. It changes the
            // answers Bitwuzla gives for these multiplication-heavy equivalence
            // proofs, which lets unsound simplifications through.
            if (settings.timeout_ms > 0) {
                options.set(bitwuzla::Option::TIME_LIMIT_PER, settings.timeout_ms);
            }
            return options;
        }

        // Assert lhs != rhs: UNSAT means no input separates the two expressions,
        // so they agree on every bitvector.
        Z3VerifyResult CheckDisequality(
            bitwuzla::TermManager &tm, const bitwuzla::Term &lhs, const bitwuzla::Term &rhs,
            const std::vector< bitwuzla::Term > &var_terms,
            const std::vector< std::string > &var_names, const Z3VerificationSettings &settings
        ) {
            bitwuzla::Bitwuzla solver(tm, BuildOptions(settings));
            solver.assert_formula(tm.mk_term(bitwuzla::Kind::DISTINCT, { lhs, rhs }));

            const auto kStart     = std::chrono::steady_clock::now();
            const auto kStatus    = solver.check_sat();
            const auto kElapsedMs = std::chrono::duration_cast< std::chrono::milliseconds >(
                                        std::chrono::steady_clock::now() - kStart
            )
                                        .count();

            Z3VerifyResult result;

            if (kStatus == bitwuzla::Result::UNSAT) {
                result.equivalent = true;
                return result;
            }

            if (kStatus == bitwuzla::Result::SAT) {
                result.equivalent = false;
                std::ostringstream ss;
                for (size_t v = 0; v < var_terms.size(); ++v) {
                    if (v != 0) {
                        ss << ' ';
                    }
                    ss << var_names[v] << " = " << solver.get_value(var_terms[v]);
                }
                result.counterexample = ss.str();
                return result;
            }

            result.unknown = true;
            // Bitwuzla reports no reason for UNKNOWN, so infer the time limit
            // from how long the check actually ran.
            result.timed_out = settings.timeout_ms > 0
                && kElapsedMs >= static_cast< long long >(settings.timeout_ms);
            result.equivalent =
                settings.unknown_result_mode == Z3UnknownResultMode::kTreatAsEquivalent;
            result.counterexample = result.timed_out ? "Bitwuzla returned unknown: timeout"
                                                     : "Bitwuzla returned unknown";
            return result;
        }

        std::vector< bitwuzla::Term > MakeVars(
            bitwuzla::TermManager &tm, const std::vector< std::string > &var_names, size_t count,
            uint32_t bitwidth
        ) {
            const auto kSort = tm.mk_bv_sort(bitwidth);
            std::vector< bitwuzla::Term > vars;
            vars.reserve(count);
            for (size_t v = 0; v < count; ++v) {
                vars.push_back(tm.mk_const(kSort, var_names[v]));
            }
            return vars;
        }

    } // namespace

    Z3VerifyResult Z3Verify(
        const std::vector< uint64_t > &cob_coeffs, const Expr &simplified,
        const std::vector< std::string > &var_names, uint32_t num_vars, uint32_t bitwidth,
        Z3VerificationSettings settings
    ) {
        bitwuzla::TermManager tm;
        auto var_terms = MakeVars(tm, var_names, num_vars, bitwidth);

        auto original = BuildOriginalFromCoeffs(tm, cob_coeffs, var_terms, num_vars, bitwidth);
        auto simpl    = BuildSmtExpr(tm, simplified, var_terms, bitwidth);

        return CheckDisequality(tm, original, simpl, var_terms, var_names, settings);
    }

    Z3VerifyResult Z3VerifyExprs(
        const Expr &original, const Expr &simplified,
        const std::vector< std::string > &var_names, uint32_t bitwidth,
        Z3VerificationSettings settings
    ) {
        bitwuzla::TermManager tm;
        auto var_terms = MakeVars(tm, var_names, var_names.size(), bitwidth);

        auto lhs = BuildSmtExpr(tm, original, var_terms, bitwidth);
        auto rhs = BuildSmtExpr(tm, simplified, var_terms, bitwidth);

        return CheckDisequality(tm, lhs, rhs, var_terms, var_names, settings);
    }

} // namespace cobra
