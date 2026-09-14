#include "cobra/core/ShiftLifting.h"

#include "cobra/core/BitWidth.h"
#include "cobra/core/ExprUtils.h"

#include <algorithm>
#include <bit>
#include <utility>
#include <vector>

namespace cobra {

    namespace {

        bool IsShiftOverArithmetic(const Expr &expr) {
            return expr.kind == Expr::Kind::kShr && !IsBitwiseAtom(*expr.children[0]);
        }

        uint32_t LargestShiftOverArithmetic(const Expr &expr) {
            uint32_t largest = 0;
            if (IsShiftOverArithmetic(expr)) {
                largest = static_cast< uint32_t >(std::min< uint64_t >(expr.constant_val, 64));
            }
            for (const auto &child : expr.children) {
                largest = std::max(largest, LargestShiftOverArithmetic(*child));
            }
            return largest;
        }

        uint64_t PowerOfTwo(uint32_t shift, uint32_t bitwidth) {
            return shift >= 64 ? 0 : (1ULL << shift) & Bitmask(bitwidth);
        }

        // `2^shift * expr`.
        std::unique_ptr< Expr >
        Scale(std::unique_ptr< Expr > expr, uint32_t shift, uint32_t bitwidth) {
            if (shift == 0) { return expr; }
            if (expr->kind == Expr::Kind::kConstant) {
                return Expr::Constant(
                    ModMul(expr->constant_val, PowerOfTwo(shift, bitwidth), bitwidth)
                );
            }
            return Expr::Mul(Expr::Constant(PowerOfTwo(shift, bitwidth)), std::move(expr));
        }

        // A constant factor and the other operand of a product, when it has one.
        std::pair< const Expr *, const Expr * > ConstantFactorOf(const Expr &product) {
            if (IsConstantSubtree(*product.children[0])) {
                return { product.children[0].get(), product.children[1].get() };
            }
            if (IsConstantSubtree(*product.children[1])) {
                return { product.children[1].get(), product.children[0].get() };
            }
            return { nullptr, nullptr };
        }

        struct LinearTerm
        {
            uint64_t coeff;
            const Expr *expr;
        };

        // The terms of a sum, read through additions, negations and constant
        // factors, with the constant part kept aside.
        void CollectLinearTerms(
            const Expr &expr, uint64_t coeff, uint32_t bitwidth, std::vector< LinearTerm > &terms,
            uint64_t &constant
        ) {
            switch (expr.kind) {
                case Expr::Kind::kConstant:
                    constant = ModAdd(constant, ModMul(coeff, expr.constant_val, bitwidth), bitwidth);
                    return;
                case Expr::Kind::kAdd:
                    CollectLinearTerms(*expr.children[0], coeff, bitwidth, terms, constant);
                    CollectLinearTerms(*expr.children[1], coeff, bitwidth, terms, constant);
                    return;
                case Expr::Kind::kNeg:
                    CollectLinearTerms(
                        *expr.children[0], ModNeg(coeff, bitwidth), bitwidth, terms, constant
                    );
                    return;
                case Expr::Kind::kMul: {
                    const auto [factor, other] = ConstantFactorOf(expr);
                    if (factor != nullptr) {
                        CollectLinearTerms(
                            *other, ModMul(coeff, EvalConstantExpr(*factor, bitwidth), bitwidth),
                            bitwidth, terms, constant
                        );
                        return;
                    }
                    break;
                }
                default:
                    break;
            }
            terms.push_back({ .coeff = coeff, .expr = &expr });
        }

        // `sum mod 2` as a bitwise atom: the low bit of a sum is the parity of
        // its odd-coefficient terms and of its constant, whatever the terms
        // themselves are.
        std::unique_ptr< Expr > ParityOf(const Expr &sum, uint32_t bitwidth) {
            std::vector< LinearTerm > terms;
            uint64_t constant = 0;
            CollectLinearTerms(sum, 1, bitwidth, terms, constant);

            std::unique_ptr< Expr > parity;
            const auto fold = [&](std::unique_ptr< Expr > piece) {
                parity = parity ? Expr::BitwiseXor(std::move(parity), std::move(piece))
                                : std::move(piece);
            };
            for (const auto &term : terms) {
                if ((term.coeff & 1) != 0) { fold(CloneExpr(*term.expr)); }
            }
            if ((constant & 1) != 0) { fold(Expr::Constant(1)); }
            if (!parity) { return Expr::Constant(0); }
            return Expr::BitwiseAnd(std::move(parity), Expr::Constant(1));
        }

        // True when some right shift in `expr` is by `shift` or less: one
        // that scaling by `2^shift` turns into a sum.
        bool HasShiftWithin(const Expr &expr, uint32_t shift) {
            if (expr.kind == Expr::Kind::kShr && expr.constant_val <= shift) { return true; }
            return std::ranges::any_of(expr.children, [shift](const auto &child) {
                return HasShiftWithin(*child, shift);
            });
        }

        // `2^shift * expr` with every shift by `shift` or less turned into a
        // sum, or null. A shift by more stays the atom it is, and so does one
        // under a bitwise operator, which scaling does not distribute into.
        std::unique_ptr< Expr > Lift(const Expr &expr, uint32_t shift, uint32_t bitwidth) {
            if (!HasShiftWithin(expr, shift)) { return Scale(CloneExpr(expr), shift, bitwidth); }

            switch (expr.kind) {
                case Expr::Kind::kAdd: {
                    auto lhs = Lift(*expr.children[0], shift, bitwidth);
                    auto rhs = Lift(*expr.children[1], shift, bitwidth);
                    if (!lhs || !rhs) { return nullptr; }
                    return Expr::Add(std::move(lhs), std::move(rhs));
                }
                case Expr::Kind::kNeg: {
                    auto operand = Lift(*expr.children[0], shift, bitwidth);
                    return operand ? Expr::Negate(std::move(operand)) : nullptr;
                }
                case Expr::Kind::kMul: {
                    const auto [factor, other] = ConstantFactorOf(expr);
                    if (factor == nullptr) { return nullptr; }
                    auto lifted = Lift(*other, shift, bitwidth);
                    if (!lifted) { return nullptr; }
                    return Expr::Mul(
                        Expr::Constant(EvalConstantExpr(*factor, bitwidth)), std::move(lifted)
                    );
                }
                case Expr::Kind::kShr: {
                    // 2^shift * (L >> t) == 2^(shift - t) * (L - (L mod 2^t)).
                    const Expr &operand = *expr.children[0];
                    const uint64_t amount = expr.constant_val;
                    if (amount > shift) { return Scale(CloneExpr(expr), shift, bitwidth); }
                    if (HasShiftOverArithmetic(operand) || HasShiftWithin(operand, shift)) {
                        return nullptr;
                    }
                    std::unique_ptr< Expr > remainder;
                    if (IsBitwiseAtom(operand)) {
                        remainder = Expr::BitwiseAnd(
                            CloneExpr(operand), Expr::Constant(Bitmask(static_cast< uint32_t >(amount)))
                        );
                    } else if (amount == 1) {
                        remainder = ParityOf(operand, bitwidth);
                    } else {
                        // The low bits of a sum are no bitwise atom past the first.
                        return nullptr;
                    }
                    auto floored = Expr::Add(CloneExpr(operand), Expr::Negate(std::move(remainder)));
                    return Scale(
                        std::move(floored), shift - static_cast< uint32_t >(amount), bitwidth
                    );
                }
                default:
                    // A bitwise operator or a comparison: scaling does not
                    // distribute into it, so what it holds stays an atom - and
                    // a shift over arithmetic is no atom.
                    if (HasShiftOverArithmetic(expr)) { return nullptr; }
                    return Scale(CloneExpr(expr), shift, bitwidth);
            }
        }

    } // namespace

    bool IsBitwiseAtom(const Expr &expr) {
        switch (expr.kind) {
            case Expr::Kind::kConstant:
            case Expr::Kind::kVariable:
                return true;
            case Expr::Kind::kAnd:
            case Expr::Kind::kOr:
            case Expr::Kind::kXor:
                return IsBitwiseAtom(*expr.children[0]) && IsBitwiseAtom(*expr.children[1]);
            case Expr::Kind::kNot:
            case Expr::Kind::kShr:
                return IsBitwiseAtom(*expr.children[0]);
            default:
                return false;
        }
    }

    bool HasShiftOverArithmetic(const Expr &expr) {
        if (IsShiftOverArithmetic(expr)) { return true; }
        return std::ranges::any_of(expr.children, [](const auto &child) {
            return HasShiftOverArithmetic(*child);
        });
    }

    uint32_t DemandedWidth(uint64_t mask, uint32_t bitwidth) {
        mask &= Bitmask(bitwidth);
        if (mask == 0) { return 0; }
        return static_cast< uint32_t >(64 - std::countl_zero(mask));
    }

    std::optional< LiftedShifts > LiftShifts(const Expr &expr, uint32_t bitwidth) {
        const uint32_t shift = LargestShiftOverArithmetic(expr);
        if (shift == 0 || shift >= bitwidth) { return std::nullopt; }
        auto scaled = Lift(expr, shift, bitwidth);
        if (!scaled) { return std::nullopt; }
        return LiftedShifts{ .scaled = std::move(scaled), .shift = shift };
    }

    std::unique_ptr< Expr >
    DivideByPowerOfTwo(const Expr &expr, uint32_t shift, uint32_t bitwidth) {
        if (shift == 0) { return CloneExpr(expr); }
        if (shift >= bitwidth) { return nullptr; }
        const uint64_t low = Bitmask(shift);

        // Exact as a signed quotient, so `-2 * a` divides to `-a` rather than
        // to a large positive coefficient.
        const auto quotient = [&](uint64_t value) -> std::optional< uint64_t > {
            if ((value & low) != 0) { return std::nullopt; }
            return static_cast< uint64_t >(SignExtend(value, bitwidth) >> shift)
                & Bitmask(bitwidth);
        };

        switch (expr.kind) {
            case Expr::Kind::kConstant: {
                const auto q = quotient(expr.constant_val);
                return q ? Expr::Constant(*q) : nullptr;
            }
            case Expr::Kind::kAdd: {
                auto lhs = DivideByPowerOfTwo(*expr.children[0], shift, bitwidth);
                auto rhs = DivideByPowerOfTwo(*expr.children[1], shift, bitwidth);
                if (!lhs || !rhs) { return nullptr; }
                return Expr::Add(std::move(lhs), std::move(rhs));
            }
            case Expr::Kind::kNeg: {
                auto operand = DivideByPowerOfTwo(*expr.children[0], shift, bitwidth);
                return operand ? Expr::Negate(std::move(operand)) : nullptr;
            }
            case Expr::Kind::kMul: {
                const auto [factor, other] = ConstantFactorOf(expr);
                if (factor == nullptr) { return nullptr; }
                const auto q = quotient(EvalConstantExpr(*factor, bitwidth));
                if (!q) { return nullptr; }
                return ApplyCoefficient(CloneExpr(*other), *q, bitwidth);
            }
            default:
                return nullptr;
        }
    }

} // namespace cobra
