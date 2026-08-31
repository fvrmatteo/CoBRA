#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cobra {

    struct Expr
    {
        enum class Kind {
            kConstant,
            kVariable,
            kAdd,
            kMul,
            kAnd,
            kOr,
            kXor,
            kNot,
            kNeg,
            kShr,
            // Comparisons, each yielding 0 or 1 like the i1 they come from.
            // Only these three are needed: the remaining seven predicates are
            // built by swapping operands and negating logically with
            // `Xor(c, 1)` — note that kNot is bitwise and so cannot serve as a
            // logical negation of a 0/1 value.
            //
            // `select` needs no kind of its own. With a 0/1 condition it is
            // `B ^ ((A ^ B) & Neg(c))`, since Neg turns 1 into all-ones and 0
            // into 0. Keeping every comparison binary means no pass has to
            // learn about a three-child node.
            kCmpEq,
            kCmpUlt,
            kCmpSlt,
        };

        Kind kind;
        uint64_t constant_val = 0;
        uint32_t var_index    = 0;
        std::vector< std::unique_ptr< Expr > > children; // move-only by design

        static std::unique_ptr< Expr > Constant(uint64_t val);
        static std::unique_ptr< Expr > Variable(uint32_t index);
        static std::unique_ptr< Expr >
        Add(std::unique_ptr< Expr > lhs, std::unique_ptr< Expr > rhs);
        static std::unique_ptr< Expr >
        Mul(std::unique_ptr< Expr > lhs, std::unique_ptr< Expr > rhs);
        static std::unique_ptr< Expr >
        BitwiseAnd(std::unique_ptr< Expr > lhs, std::unique_ptr< Expr > rhs);
        static std::unique_ptr< Expr >
        BitwiseOr(std::unique_ptr< Expr > lhs, std::unique_ptr< Expr > rhs);
        static std::unique_ptr< Expr >
        BitwiseXor(std::unique_ptr< Expr > lhs, std::unique_ptr< Expr > rhs);
        static std::unique_ptr< Expr > BitwiseNot(std::unique_ptr< Expr > operand);
        static std::unique_ptr< Expr > Negate(std::unique_ptr< Expr > operand);
        static std::unique_ptr< Expr >
        LogicalShr(std::unique_ptr< Expr > operand, uint64_t amount);
        static std::unique_ptr< Expr >
        CmpEq(std::unique_ptr< Expr > lhs, std::unique_ptr< Expr > rhs);
        static std::unique_ptr< Expr >
        CmpUlt(std::unique_ptr< Expr > lhs, std::unique_ptr< Expr > rhs);
        static std::unique_ptr< Expr >
        CmpSlt(std::unique_ptr< Expr > lhs, std::unique_ptr< Expr > rhs);
    };

    // True for the comparison kinds, whose value is confined to 0 or 1 and
    // whose operands are compared as whole words rather than bit by bit. The
    // bitwise and semilinear machinery does not model either property, so
    // passes built on those models treat such a node as opaque.
    inline bool IsComparison(Expr::Kind kind) {
        return kind == Expr::Kind::kCmpEq || kind == Expr::Kind::kCmpUlt
            || kind == Expr::Kind::kCmpSlt;
    }

    // True when `expr` or any node beneath it is a comparison.
    bool ContainsComparison(const Expr &expr);

    std::unique_ptr< Expr > CloneExpr(const Expr &expr);

    std::string Render(
        const Expr &expr, const std::vector< std::string > &var_names, uint32_t bitwidth = 64
    );

} // namespace cobra

namespace cobra::detail {
    inline size_t hash_combine(size_t seed, size_t value) {
        return seed ^ (value + 0x9e3779b9 + (seed << 6) + (seed >> 2));
    }
} // namespace cobra::detail

template<>
struct std::hash< cobra::Expr >
{
    size_t operator()(const cobra::Expr &e) const {
        size_t h = std::hash< int >{}(static_cast< int >(e.kind));
        h        = cobra::detail::hash_combine(h, std::hash< uint64_t >{}(e.constant_val));
        h        = cobra::detail::hash_combine(h, std::hash< uint32_t >{}(e.var_index));
        for (const auto &child : e.children) {
            h = cobra::detail::hash_combine(h, std::hash< cobra::Expr >{}(*child));
        }
        return h;
    }
};
