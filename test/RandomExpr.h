#pragma once

#include "cobra/core/BitWidth.h"
#include "cobra/core/Expr.h"

#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <vector>

// Random expressions over every node kind for the tests that check the many
// evaluators and lowerings of an expression agree. Constants are not reduced
// to the width, and shift amounts reach past it: every consumer has to.
namespace cobra::testing {

    class RandomExpr
    {
      public:
        RandomExpr(uint64_t seed, uint32_t vars, uint32_t bitwidth)
            : rng_(seed), vars_(vars), bitwidth_(bitwidth) {}

        std::unique_ptr< Expr > Build(int depth) {
            if (depth == 0 || Pick(4) == 0) {
                return Pick(3) == 0 ? Expr::Constant(Constant()) : Expr::Variable(Pick(vars_));
            }
            switch (Pick(13)) {
                case 0:
                    return Expr::Add(Build(depth - 1), Build(depth - 1));
                case 1:
                    return Expr::Mul(Build(depth - 1), Build(depth - 1));
                case 2:
                    return Expr::BitwiseAnd(Build(depth - 1), Build(depth - 1));
                case 3:
                    return Expr::BitwiseOr(Build(depth - 1), Build(depth - 1));
                case 4:
                    return Expr::BitwiseXor(Build(depth - 1), Build(depth - 1));
                case 5:
                    return Expr::BitwiseNot(Build(depth - 1));
                case 6:
                    return Expr::Negate(Build(depth - 1));
                case 7:
                    return Expr::LogicalShr(Build(depth - 1), Pick(bitwidth_ + 3));
                case 8:
                    return Expr::CmpEq(Build(depth - 1), Build(depth - 1));
                case 9:
                    return Expr::CmpUlt(Build(depth - 1), Build(depth - 1));
                case 10:
                    return Expr::CmpSlt(Build(depth - 1), Build(depth - 1));
                case 11:
                    return Expr::Add(Build(depth - 1), Expr::Constant(Constant()));
                default:
                    return Expr::Mul(Expr::Constant(Constant()), Build(depth - 1));
            }
        }

        uint64_t Input() { return Pick(4) == 0 ? Edge() : rng_(); }

      private:
        uint32_t Pick(uint32_t bound) { return static_cast< uint32_t >(rng_() % bound); }

        uint64_t Edge() {
            const uint64_t top = uint64_t{ 1 } << (bitwidth_ - 1);
            const uint64_t edges[] = {
                0, 1, 2, top, top - 1, top + 1, Bitmask(bitwidth_), ~uint64_t{ 0 }
            };
            return edges[Pick(8)];
        }

        uint64_t Constant() { return Pick(2) == 0 ? Edge() : rng_(); }

        std::mt19937_64 rng_;
        uint32_t vars_;
        uint32_t bitwidth_;
    };

    inline std::unique_ptr< Expr >
    Substitute(const Expr &expr, const std::vector< uint64_t > &values) {
        if (expr.kind == Expr::Kind::kVariable) {
            return Expr::Constant(values[expr.var_index]);
        }
        auto copy = CloneExpr(expr);
        for (size_t i = 0; i < expr.children.size(); ++i) {
            copy->children[i] = Substitute(*expr.children[i], values);
        }
        return copy;
    }

    // The widths the edge cases live at: one bit, odd widths, either side of a word.
    constexpr uint32_t kWidths[] = { 1, 2, 3, 7, 8, 13, 16, 31, 32, 33, 63, 64 };

    inline std::vector< std::string > VariableNames(uint32_t count) {
        std::vector< std::string > names;
        for (uint32_t i = 0; i < count; ++i) { names.push_back("v" + std::to_string(i)); }
        return names;
    }

} // namespace cobra::testing
