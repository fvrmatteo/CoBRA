#include "cobra/core/AffineReading.h"

#include "cobra/core/BitWidth.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/SignatureChecker.h"

#include <algorithm>
#include <bit>
#include <optional>
#include <random>

namespace cobra {

    namespace {

        // Probes on random inputs, past the ones that move a single leaf.
        constexpr size_t kRandomProbes = 96;
        // How many fits the search for the constant xored in may try.
        constexpr size_t kMaxFits = 4096;

        // What a leaf can be: the bits that vary with its variable, and the
        // value it reads when the variable is zero.
        struct LeafRange
        {
            uint64_t free   = 0;
            uint64_t base   = 0;
            uint32_t lowest = 0;
            uint32_t top    = 0;
            // Whether the top bit of the leaf is a basis term of its own: the
            // leaf is narrower than the tree, and has more than one free bit.
            bool sign = false;
        };

        // The fitted function on the low `bits` bits: `f ^ xor_constant` is
        // `constant + sum(linear[i] * (L_i - L_i(0))) + sum(sign[i] * (s_i - s_i(0)))`.
        struct Fit
        {
            uint64_t constant = 0;
            std::vector< uint64_t > linear;
            std::vector< uint64_t > sign;
        };

        struct Probes
        {
            std::vector< uint64_t > root;
            // Per probe, per leaf: the leaf's value and its top bit, less their
            // values on the probe where every variable is zero.
            std::vector< std::vector< uint64_t > > moved;
            std::vector< std::vector< uint64_t > > sign_moved;
        };

        // The coefficients the probes that move one leaf say `f ^ xor_constant`
        // has on its low `bits` bits, if they are coefficients at all; and
        // whether every probe agrees with them.
        std::optional< Fit > FitLowBits(
            const Probes &probes, const std::vector< LeafRange > &ranges, uint64_t xor_constant,
            uint32_t bits
        ) {
            const uint64_t mask = Bitmask(bits);
            const size_t leaves = ranges.size();
            const auto value    = [&](size_t probe) { return (probes.root[probe] ^ xor_constant) & mask; };
            Fit fit;
            fit.constant = value(0);
            fit.linear.assign(leaves, 0);
            fit.sign.assign(leaves, 0);
            // Probe `1 + i` moves leaf `i` by its lowest free bit, and probe
            // `1 + leaves + i` by its top one.
            for (size_t leaf = 0; leaf < leaves; ++leaf) {
                const auto &range = ranges[leaf];
                if (range.free == 0) {
                    continue;
                }
                const uint64_t moved = (value(1 + leaf) - fit.constant) & mask;
                if (range.lowest >= bits) {
                    if (moved != 0) {
                        return std::nullopt;
                    }
                    continue;
                }
                if ((moved & Bitmask(range.lowest)) != 0) {
                    return std::nullopt;
                }
                fit.linear[leaf] = moved >> range.lowest;
                if (range.sign) {
                    const uint64_t top = (value(1 + leaves + leaf) - fit.constant) & mask;
                    fit.sign[leaf]     = (top - fit.linear[leaf] * (uint64_t{ 1 } << range.top)) & mask;
                }
            }
            for (size_t probe = 0; probe < probes.root.size(); ++probe) {
                uint64_t predicted = fit.constant;
                for (size_t leaf = 0; leaf < leaves; ++leaf) {
                    predicted += fit.linear[leaf] * probes.moved[probe][leaf]
                        + fit.sign[leaf] * probes.sign_moved[probe][leaf];
                }
                if (((predicted ^ value(probe)) & mask) != 0) {
                    return std::nullopt;
                }
            }
            return fit;
        }

        // `value` modulo `2^bits`, as the integer nearest zero: a coefficient of
        // all ones is -1, which the rewrite spells as a subtraction.
        uint64_t Signed(uint64_t value, uint32_t bits, uint32_t bitwidth) {
            value &= Bitmask(bits);
            if (bits < 64 && bits > 0 && ((value >> (bits - 1)) & 1) != 0) {
                value |= ~Bitmask(bits);
            }
            return value & Bitmask(bitwidth);
        }

        std::unique_ptr< Expr > Term(uint64_t coefficient, std::unique_ptr< Expr > operand, uint32_t bitwidth) {
            coefficient &= Bitmask(bitwidth);
            if (coefficient == 1) {
                return operand;
            }
            if (coefficient == Bitmask(bitwidth)) {
                return Expr::Negate(std::move(operand));
            }
            return Expr::Mul(Expr::Constant(coefficient), std::move(operand));
        }

        // `fit` written over the leaves as the tree reads them:
        // `(constant + sum(a_i * L_i) + sum(c_i * s_i)) ^ xor_constant` on the low
        // `bits` bits - or its complement, `A ^ C` being `~A ^ ~C` and `~A` the
        // same sum with every coefficient negated, less one. Which of the two is
        // cheaper depends on the signs the coefficients came out with.
        std::unique_ptr< Expr > Build(
            const Fit &fit, const std::vector< LeafRange > &ranges,
            const std::vector< std::unique_ptr< Expr > > &leaves, uint64_t xor_constant, uint32_t bits,
            uint32_t bitwidth, bool complement
        ) {
            const uint64_t full = Bitmask(bitwidth);
            const uint64_t mask = Bitmask(bits);
            const size_t count  = ranges.size();
            // The fit is relative to every leaf at its base; the sum is absolute.
            uint64_t constant = fit.constant;
            for (size_t leaf = 0; leaf < count; ++leaf) {
                const auto &range = ranges[leaf];
                if (range.free != 0 && range.lowest < bits) {
                    constant -= (fit.linear[leaf] & Bitmask(bits - range.lowest)) * range.base
                        + fit.sign[leaf] * ((range.base >> range.top) & 1);
                }
            }
            std::unique_ptr< Expr > sum;
            const auto add = [&](std::unique_ptr< Expr > term) {
                sum = sum == nullptr ? std::move(term) : Expr::Add(std::move(sum), std::move(term));
            };
            for (size_t leaf = 0; leaf < count; ++leaf) {
                const auto &range = ranges[leaf];
                if (range.free == 0 || range.lowest >= bits) {
                    continue;
                }
                const uint32_t span   = bits - range.lowest;
                const uint64_t linear = fit.linear[leaf] & Bitmask(span);
                const uint64_t a      = Signed(complement ? 0 - linear : linear, span, bitwidth);
                const uint64_t c      = (complement ? 0 - fit.sign[leaf] : fit.sign[leaf]) & mask;
                if ((a & Bitmask(span)) == 0 && c == 0) {
                    continue;
                }
                // `a * L - a * 2^(t+1) * s` is `a` times the sign extension of `L`
                // from its top free bit, `(L ^ 2^t) - 2^t`.
                const uint64_t sign_bit = uint64_t{ 1 } << range.top;
                if (range.sign && c != 0 && ((c + a * (sign_bit << 1)) & mask) == 0) {
                    auto extended = Expr::Add(
                        Expr::BitwiseXor(CloneExpr(*leaves[leaf]), Expr::Constant(sign_bit)),
                        Expr::Constant((~sign_bit + 1) & full)
                    );
                    add(Term(a, std::move(extended), bitwidth));
                    continue;
                }
                if ((a & Bitmask(span)) != 0) {
                    // On bits the leaf's masks leave alone the masks say nothing:
                    // a narrow leaf under a reader of its own width is the variable.
                    const bool unmasked = (range.free & mask) == mask && (range.base & mask) == 0;
                    add(Term(
                        a, unmasked ? Expr::Variable(static_cast< uint32_t >(leaf)) : CloneExpr(*leaves[leaf]),
                        bitwidth
                    ));
                }
                if (c != 0) {
                    auto top = Expr::LogicalShr(CloneExpr(*leaves[leaf]), range.top);
                    if (((range.base | range.free) >> (range.top + 1)) != 0) {
                        top = Expr::BitwiseAnd(std::move(top), Expr::Constant(1));
                    }
                    add(Term(Signed(c, bits, bitwidth), std::move(top), bitwidth));
                }
            }
            const uint64_t k = Signed(complement ? ~constant : constant, bits, bitwidth);
            const uint64_t x = (complement ? ~xor_constant : xor_constant) & mask;
            if (sum == nullptr) {
                return Expr::Constant((k ^ x) & mask);
            }
            if ((k & mask) != 0) {
                sum = Expr::Add(std::move(sum), Expr::Constant(k));
            }
            if (x != 0) {
                sum = Expr::BitwiseXor(std::move(sum), Expr::Constant(x));
            }
            return sum;
        }

    } // namespace

    std::unique_ptr< Expr > ReadAsAffineOfLeaves(
        const Expr &expr, const std::vector< std::unique_ptr< Expr > > &leaves, uint32_t bitwidth,
        uint64_t demanded
    ) {
        const uint64_t full = Bitmask(bitwidth);
        demanded &= full;
        if (demanded == 0 || bitwidth == 0 || bitwidth > 64) {
            return nullptr;
        }
        const uint32_t bits  = 64 - static_cast< uint32_t >(std::countl_zero(demanded));
        const size_t count   = leaves.size();
        const std::vector< uint64_t > zeros(count, 0);

        std::vector< LeafRange > ranges(count);
        for (size_t leaf = 0; leaf < count; ++leaf) {
            if (leaves[leaf] == nullptr) {
                return nullptr;
            }
            std::vector< uint64_t > ones(count, 0);
            ones[leaf]    = full;
            auto &range   = ranges[leaf];
            range.base    = EvalExpr(*leaves[leaf], zeros, bitwidth);
            range.free    = (range.base ^ EvalExpr(*leaves[leaf], ones, bitwidth)) & full;
            if (range.free == 0) {
                continue;
            }
            range.lowest = static_cast< uint32_t >(std::countr_zero(range.free));
            range.top    = 63 - static_cast< uint32_t >(std::countl_zero(range.free));
            range.sign   = range.top + 1 < bitwidth && range.top > range.lowest;
        }

        // The probe with every variable zero, then one per leaf moved by its
        // lowest free bit, one per leaf moved by its top bit, and the rest.
        std::vector< std::vector< uint64_t > > inputs;
        inputs.push_back(zeros);
        for (size_t leaf = 0; leaf < count; ++leaf) {
            auto input  = zeros;
            input[leaf] = ranges[leaf].free == 0 ? 0 : uint64_t{ 1 } << ranges[leaf].lowest;
            inputs.push_back(std::move(input));
        }
        for (size_t leaf = 0; leaf < count; ++leaf) {
            auto input  = zeros;
            input[leaf] = ranges[leaf].free == 0 ? 0 : uint64_t{ 1 } << ranges[leaf].top;
            inputs.push_back(std::move(input));
        }
        std::mt19937_64 random(0xAFF1E5EEDULL ^ std::hash< Expr >{}(expr));
        for (size_t leaf = 0; leaf < count; ++leaf) {
            auto input  = zeros;
            input[leaf] = full;
            inputs.push_back(std::move(input));
        }
        for (size_t probe = 0; probe < kRandomProbes; ++probe) {
            std::vector< uint64_t > input(count);
            for (auto &value : input) {
                // Some leaves at an edge of their range, the rest anywhere.
                switch (random() % 6) {
                    case 0:
                        value = 0;
                        break;
                    case 1:
                        value = full;
                        break;
                    default:
                        value = random() & full;
                        break;
                }
            }
            inputs.push_back(std::move(input));
        }

        Probes probes;
        probes.root.reserve(inputs.size());
        for (const auto &input : inputs) {
            probes.root.push_back(EvalExpr(expr, input, bitwidth));
            std::vector< uint64_t > moved(count, 0);
            std::vector< uint64_t > sign_moved(count, 0);
            for (size_t leaf = 0; leaf < count; ++leaf) {
                const auto &range = ranges[leaf];
                if (range.free == 0) {
                    continue;
                }
                const uint64_t value = EvalExpr(*leaves[leaf], input, bitwidth);
                moved[leaf]          = value - range.base;
                if (range.sign) {
                    sign_moved[leaf] = ((value >> range.top) & 1) - ((range.base >> range.top) & 1);
                }
            }
            probes.moved.push_back(std::move(moved));
            probes.sign_moved.push_back(std::move(sign_moved));
        }

        // The constant xored in. Where the sum has no constant of its own, it
        // is the root's value on the probe where every leaf is at its base,
        // which one fit tells. Otherwise it is read one bit at a time from the
        // bottom: both values of a bit can fit the bits up to it - the
        // coefficients' top bits take up the difference - and only one of them
        // go on fitting the bits above, so a bit that leads nowhere is taken
        // back.
        const uint64_t mask = Bitmask(bits);
        std::vector< uint64_t > xors;
        if (FitLowBits(probes, ranges, probes.root[0] & mask, bits)) {
            xors.push_back(probes.root[0] & mask);
        }
        size_t budget     = kMaxFits;
        const auto search = [&](auto &&self, uint32_t bit, uint64_t so_far) -> bool {
            if (bit == bits) {
                if (std::find(xors.begin(), xors.end(), so_far) == xors.end()) {
                    xors.push_back(so_far);
                }
                return true;
            }
            for (uint64_t choice = 0; choice < 2; ++choice) {
                if (budget == 0) {
                    return false;
                }
                --budget;
                const uint64_t next = so_far | (choice << bit);
                if (FitLowBits(probes, ranges, next, bit + 1) && self(self, bit + 1, next)) {
                    return true;
                }
            }
            return false;
        };
        search(search, 0, 0);

        std::unique_ptr< Expr > best;
        for (const uint64_t xor_constant : xors) {
            const auto fit = FitLowBits(probes, ranges, xor_constant, bits);
            if (!fit) {
                continue;
            }
            for (const bool complement : { false, true }) {
                auto answer = Build(*fit, ranges, leaves, xor_constant, bits, bitwidth, complement);
                if (!best || IsBetter(ComputeCost(*answer).cost, ComputeCost(*best).cost)) {
                    best = std::move(answer);
                }
            }
        }
        return best;
    }

} // namespace cobra
