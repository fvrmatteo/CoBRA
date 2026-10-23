#include "cobra/core/SubtermReading.h"

#include "cobra/core/BitWidth.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureEval.h"

#include <algorithm>
#include <array>
#include <optional>
#include <random>
#include <unordered_map>
#include <vector>

namespace cobra {

    namespace {

        // How many subterms are tried alone, how many of them in pairs, and on how many inputs.
        constexpr size_t kMaxSubterms = 64;
        constexpr size_t kMaxPairedSubterms = 24;
        constexpr size_t kProbes = 128;

        size_t NodeCount(const Expr &expr) {
            size_t count = 1;
            for (const auto &child : expr.children) {
                count += NodeCount(*child);
            }
            return count;
        }

        // Whether `expr` computes with carries or shifts, so that the bitwise reading cannot see
        // through it: an atom of the semilinear reading, or a variable.
        bool IsArithmetic(const Expr &expr) {
            switch (expr.kind) {
                case Expr::Kind::kVariable:
                case Expr::Kind::kAdd:
                case Expr::Kind::kMul:
                case Expr::Kind::kNeg:
                case Expr::Kind::kShr:
                    return true;
                default:
                    return false;
            }
        }

        // The distinct arithmetic subterms of `expr` other than itself, largest first.
        std::vector< const Expr * > CollectSubterms(const Expr &root) {
            std::vector< const Expr * > found;
            std::unordered_map< size_t, std::vector< const Expr * > > by_hash;
            std::vector< const Expr * > work{ &root };
            while (!work.empty()) {
                const Expr *node = work.back();
                work.pop_back();
                for (const auto &child : node->children) {
                    work.push_back(child.get());
                }
                if (node == &root || !IsArithmetic(*node)) {
                    continue;
                }
                auto &bucket = by_hash[std::hash< Expr >{}(*node)];
                if (std::any_of(bucket.begin(), bucket.end(), [&](const Expr *seen) {
                        return ExprStructurallyEqual(*seen, *node);
                    }))
                {
                    continue;
                }
                bucket.push_back(node);
                found.push_back(node);
            }
            std::stable_sort(found.begin(), found.end(), [](const Expr *lhs, const Expr *rhs) {
                return NodeCount(*lhs) > NodeCount(*rhs);
            });
            if (found.size() > kMaxSubterms) {
                found.resize(kMaxSubterms);
            }
            return found;
        }

        // For each bit and each combination of the subterms' bits seen there, the root's bit:
        // -1 unseen, otherwise 0 or 1. Null when a combination is seen with both.
        std::optional< std::vector< std::array< int8_t, 4 > > > BitTables(
            const std::vector< uint64_t > &roots, const std::vector< std::vector< uint64_t > > &values,
            const std::vector< size_t > &subterms, uint32_t bitwidth
        ) {
            std::vector< std::array< int8_t, 4 > > tables(bitwidth, { -1, -1, -1, -1 });
            for (size_t probe = 0; probe < roots.size(); ++probe) {
                for (uint32_t bit = 0; bit < bitwidth; ++bit) {
                    unsigned combination = 0;
                    for (size_t index = 0; index < subterms.size(); ++index) {
                        combination |= ((values[subterms[index]][probe] >> bit) & 1) << index;
                    }
                    const auto value = static_cast< int8_t >((roots[probe] >> bit) & 1);
                    auto &entry      = tables[bit][combination];
                    if (entry == -1) {
                        entry = value;
                    } else if (entry != value) {
                        return std::nullopt;
                    }
                }
            }
            return tables;
        }

    } // namespace

    std::unique_ptr< Expr > ReadAsBitwiseOfSubterms(
        const Expr &expr, uint32_t num_vars, uint32_t bitwidth, const Options &opts
    ) {
        const uint64_t mask = Bitmask(bitwidth);
        const auto subterms = CollectSubterms(expr);

        // The same inputs for the root and every subterm: boundary values first, then words.
        std::mt19937_64 random(0xC0B5A11ULL ^ std::hash< Expr >{}(expr));
        static const uint64_t kBoundary[] = { 0, 1, 2, ~uint64_t{ 0 }, ~uint64_t{ 0 } - 1, uint64_t{ 1 } << 63,
                                              (uint64_t{ 1 } << 63) - 1 };
        std::vector< uint64_t > roots(kProbes);
        std::vector< std::vector< uint64_t > > values(subterms.size(), std::vector< uint64_t >(kProbes));
        std::vector< uint64_t > inputs(num_vars);
        for (size_t probe = 0; probe < kProbes; ++probe) {
            for (auto &input : inputs) {
                input = (probe < 24 ? kBoundary[random() % 7] : random()) & mask;
            }
            roots[probe] = EvalExpr(expr, inputs, bitwidth);
            for (size_t index = 0; index < subterms.size(); ++index) {
                values[index][probe] = EvalExpr(*subterms[index], inputs, bitwidth);
            }
        }

        if (std::all_of(roots.begin(), roots.end(), [&](uint64_t root) { return root == roots.front(); })) {
            return Expr::Constant(roots.front());
        }

        // The cheapest of the answers found: a large subterm that fits is no answer when a small
        // one does too.
        std::unique_ptr< Expr > best;
        const auto consider = [&](std::unique_ptr< Expr > answer) {
            if (!best || IsBetter(ComputeCost(*answer).cost, ComputeCost(*best).cost)) {
                best = std::move(answer);
            }
        };

        // One subterm: every bit a constant, the subterm's bit or its complement.
        for (size_t index = 0; index < subterms.size(); ++index) {
            const auto tables = BitTables(roots, values, { index }, bitwidth);
            if (!tables) {
                continue;
            }
            uint64_t kept = 0;
            uint64_t flipped = 0;
            for (uint32_t bit = 0; bit < bitwidth; ++bit) {
                const auto at_zero = (*tables)[bit][0];
                const auto at_one  = (*tables)[bit][1];
                if (at_zero != -1 && at_one != -1 && at_zero != at_one) {
                    kept |= uint64_t{ 1 } << bit;
                    flipped |= static_cast< uint64_t >(at_zero) << bit;
                } else {
                    // The bit does not vary with the subterm on any input seen: a constant.
                    flipped |= static_cast< uint64_t >(std::max(at_zero, at_one) == 1) << bit;
                }
            }
            auto answer = CloneExpr(*subterms[index]);
            if (kept != mask) {
                answer = Expr::BitwiseAnd(std::move(answer), Expr::Constant(kept));
            }
            if (flipped != 0) {
                answer = Expr::BitwiseXor(std::move(answer), Expr::Constant(flipped));
            }
            consider(std::move(answer));
        }
        if (best) {
            return best;
        }

        // Two subterms: the sixteen functions of two bits, written as the bits each
        // combination sets and simplified as a function of two variables.
        const size_t paired = std::min(subterms.size(), kMaxPairedSubterms);
        for (size_t first = 0; first < paired; ++first) {
            for (size_t second = first + 1; second < paired; ++second) {
                const auto tables = BitTables(roots, values, { first, second }, bitwidth);
                if (!tables) {
                    continue;
                }
                std::array< uint64_t, 4 > sets{};
                for (uint32_t bit = 0; bit < bitwidth; ++bit) {
                    for (unsigned combination = 0; combination < 4; ++combination) {
                        if ((*tables)[bit][combination] == 1) {
                            sets[combination] |= uint64_t{ 1 } << bit;
                        }
                    }
                }
                // Over two fresh variables: 0 is the first subterm, 1 the second.
                std::unique_ptr< Expr > function;
                for (unsigned combination = 0; combination < 4; ++combination) {
                    if (sets[combination] == 0) {
                        continue;
                    }
                    auto a = Expr::Variable(0);
                    auto b = Expr::Variable(1);
                    auto term = Expr::BitwiseAnd(
                        Expr::BitwiseAnd(
                            (combination & 1) != 0 ? std::move(a) : Expr::BitwiseNot(std::move(a)),
                            (combination & 2) != 0 ? std::move(b) : Expr::BitwiseNot(std::move(b))
                        ),
                        Expr::Constant(sets[combination])
                    );
                    function = function ? Expr::BitwiseOr(std::move(function), std::move(term)) : std::move(term);
                }
                if (!function) {
                    consider(Expr::Constant(0));
                    continue;
                }
                Options inner          = opts;
                inner.bitwidth         = bitwidth;
                inner.evaluator        = Evaluator{};
                inner.demanded_mask    = UINT64_MAX;
                inner.enabled_families = TechniqueFamily::kSemilinear | TechniqueFamily::kBitwise;
                const std::vector< std::string > names = { "a", "b" };
                auto result = Simplify(EvaluateBooleanSignature(*function, 2, bitwidth), names, function.get(), inner);
                auto simplified = CloneExpr(*function);
                if (result.has_value() && result->kind == SimplifyOutcome::Kind::kSimplified
                    && result->expr != nullptr)
                {
                    simplified = CloneExpr(*result->expr);
                    if (!result->real_vars.empty()) {
                        const auto support = TryBuildVarSupport(names, result->real_vars);
                        if (!support) {
                            continue;
                        }
                        RemapVarIndices(*simplified, *support);
                    }
                }
                // The fresh variables put back as the subterms they stand for.
                const auto substitute = [&](auto &&self, std::unique_ptr< Expr > node) -> std::unique_ptr< Expr > {
                    if (node->kind == Expr::Kind::kVariable) {
                        return CloneExpr(*subterms[node->var_index == 0 ? first : second]);
                    }
                    for (auto &child : node->children) {
                        child = self(self, std::move(child));
                    }
                    return node;
                };
                consider(substitute(substitute, std::move(simplified)));
            }
        }
        return best;
    }

} // namespace cobra
