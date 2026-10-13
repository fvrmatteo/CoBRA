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
#include <mutex>
#include <optional>
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
        out += ':';
        out += std::to_string(settings.rewrite_level);
        // How many variables, not what they are called: the names never reach
        // the solver, and a host names its leaves afresh every time it finds
        // them, so a key holding them missed every question asked again. They
        // only appear in a counterexample's text, which a hit replays from
        // whichever asker was first.
        out += ':';
        out += std::to_string(var_names.size());
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

    // The key of a question asked through `VerifyExprsWithStrategy`: a definite
    // answer is the same whoever gave it, but whether a question comes back
    // unknown depends on how it was asked as well.
    inline std::string StrategyQueryKey(
        const Expr &original, const Expr &simplified,
        const std::vector< std::string > &var_names, uint32_t bitwidth,
        const Z3VerificationSettings &settings
    ) {
        std::string key = ExprQueryKey(original, simplified, var_names, bitwidth, settings);
        key += '|';
        key += std::to_string(static_cast< int >(settings.strategy));
        key += settings.sweep ? ":sweep:" : ":";
        key += settings.llvm_passes;
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
    //
    // Callers may ask from several threads at once (CoBRA's LLVM pass solves
    // queued candidates on worker threads), so the table is locked and hands
    // answers out by value: a reference into it would not survive another
    // thread's insertion clearing it.
    class QueryCache
    {
      public:
        // An unknown is answered as the caller's policy says it is to be
        // taken, as it would be had the solver been asked again: the key holds
        // that policy too, but an unknown restored from a saved memo
        // (`RestoreUnknownMemo`) knows only that it was unknown.
        std::optional< Z3VerifyResult >
        Find(const std::string &key, Z3UnknownResultMode unknown_mode) const {
            const std::lock_guard< std::mutex > lock(mutex_);
            if (const auto kEntry = entries_.find(key); kEntry != entries_.end()) {
                return kEntry->second;
            }
            const auto kUnknown = unknowns_.find(key);
            if (kUnknown == unknowns_.end()) {
                return std::nullopt;
            }
            Z3VerifyResult result = kUnknown->second;
            result.equivalent     = unknown_mode == Z3UnknownResultMode::kTreatAsEquivalent;
            return result;
        }

        // The questions answered "unknown", for a host that saves its analysis and
        // resumes it: asked again, an unknown is the one answer that can come out
        // differently, so a resumed run has to remember the same ones.
        std::vector< std::pair< std::string, Z3VerifyResult > > Unknowns() const {
            const std::lock_guard< std::mutex > lock(mutex_);
            return { unknowns_.begin(), unknowns_.end() };
        }

        Z3VerifyResult Insert(std::string key, const Z3VerifyResult &result) {
            const std::lock_guard< std::mutex > lock(mutex_);
            // Bounded because a run keeps producing new expressions; dropping
            // a definite answer costs re-solving, not correctness.
            //
            // The unknowns are kept apart and only dropped past a much larger
            // bound: asked again, an unknown is the one answer that can come
            // out differently, so dropping one - a restored one above all -
            // is how a resumed run stops deciding what the first one decided.
            auto &table = result.unknown ? unknowns_ : entries_;
            if (table.size() >= (result.unknown ? kMaxUnknowns : kMaxEntries)) {
                table.clear();
            }
            return table.emplace(std::move(key), result).first->second;
        }

      private:
        static constexpr size_t kMaxEntries  = size_t{ 1 } << 16;
        static constexpr size_t kMaxUnknowns = size_t{ 1 } << 18;
        mutable std::mutex mutex_;
        std::unordered_map< std::string, Z3VerifyResult > entries_;
        std::unordered_map< std::string, Z3VerifyResult > unknowns_;
    };

    inline QueryCache &Cache() {
        static QueryCache cache;
        return cache;
    }

} // namespace cobra::verify_detail
