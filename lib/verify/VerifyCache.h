// Memoization of equivalence queries, independent of the solver behind them.
//
// Both Z3Verifier.cpp and BitwuzlaVerify.cpp include this: the two are
// alternative implementations of the same entry points, and the cache key does
// not depend on which solver discharges the query.
#pragma once

#include "cobra/core/Expr.h"
#include "cobra/verify/Z3Verifier.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace cobra::verify_detail {

    // Post-order text of the tree. Variables are already indices rather than
    // names, so this is a canonical form: two expressions serialise identically
    // exactly when they are the same tree.
    inline void AppendExpr(std::string &out, const Expr &expr) {
        out += static_cast< char >('a' + static_cast< int >(expr.kind));
        switch (expr.kind) {
            case Expr::Kind::kConstant:
            case Expr::Kind::kShr:
                out += std::to_string(expr.constant_val);
                break;
            case Expr::Kind::kVariable:
                out += std::to_string(expr.var_index);
                break;
            default:
                break;
        }
        out += '(';
        for (const auto &child : expr.children) {
            AppendExpr(out, *child);
        }
        out += ')';
    }

    inline void AppendSettings(
        std::string &out, uint32_t bitwidth, const std::vector< std::string > &var_names,
        const Z3VerificationSettings &settings
    ) {
        out += '#';
        out += std::to_string(bitwidth);
        out += ':';
        out += std::to_string(settings.timeout_ms);
        out += ':';
        out += std::to_string(static_cast< int >(settings.unknown_result_mode));
        // The names never reach the solver, but they are read back into the
        // counterexample a hit would replay.
        for (const auto &name : var_names) {
            out += ':';
            out += name;
        }
    }

    inline std::string ExprQueryKey(
        const Expr &original, const Expr &simplified,
        const std::vector< std::string > &var_names, uint32_t bitwidth,
        const Z3VerificationSettings &settings
    ) {
        std::string key;
        AppendExpr(key, original);
        key += '=';
        AppendExpr(key, simplified);
        AppendSettings(key, bitwidth, var_names, settings);
        return key;
    }

    inline std::string CoeffQueryKey(
        const std::vector< uint64_t > &cob_coeffs, const Expr &simplified,
        const std::vector< std::string > &var_names, uint32_t num_vars, uint32_t bitwidth,
        const Z3VerificationSettings &settings
    ) {
        std::string key = std::to_string(num_vars);
        for (uint64_t coeff : cob_coeffs) {
            key += ',';
            key += std::to_string(coeff);
        }
        key += '=';
        AppendExpr(key, simplified);
        AppendSettings(key, bitwidth, var_names, settings);
        return key;
    }

    // Answers keyed by the exact query that was posed.
    //
    // Sound for both outcomes, unlike the shape caches the callers keep: the
    // key spans both trees, the width, and the settings that decide how an
    // UNKNOWN is reported, so a hit is the same question and not merely a
    // similar one.
    class QueryCache
    {
      public:
        const Z3VerifyResult *Find(const std::string &key) const {
            const auto kEntry = entries_.find(key);
            return kEntry == entries_.end() ? nullptr : &kEntry->second;
        }

        const Z3VerifyResult &Insert(std::string key, const Z3VerifyResult &result) {
            // Bounded because a run keeps producing new expressions; dropping
            // the table costs re-solving, not correctness.
            if (entries_.size() >= kMaxEntries) {
                entries_.clear();
            }
            return entries_.emplace(std::move(key), result).first->second;
        }

      private:
        static constexpr size_t kMaxEntries = size_t{ 1 } << 16;
        std::unordered_map< std::string, Z3VerifyResult > entries_;
    };

    inline QueryCache &Cache() {
        static QueryCache cache;
        return cache;
    }

} // namespace cobra::verify_detail
