#include "cobra/core/RingProof.h"

#include "cobra/core/BitWidth.h"

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cobra {

    namespace {

        // A product of atoms, by id, sorted; an atom appears once per power.
        using Monomial = std::vector< uint32_t >;
        // Monomial -> coefficient, every coefficient non-zero and reduced
        // modulo 2^bitwidth. Ordered, so that two equal polynomials compare
        // and print equal.
        using Poly = std::map< Monomial, uint64_t >;

        // An indeterminate of the ring: a variable, or an operation the ring
        // cannot see into, over operands already in normal form.
        struct Atom
        {
            Expr::Kind kind     = Expr::Kind::kVariable;
            uint32_t variable   = 0;
            // The folded constant operand of `&`, `|` and `^`; the amount of a
            // shift.
            uint64_t constant   = 0;
            std::vector< Poly > operands;
        };

        class Normalizer
        {
          public:
            Normalizer(uint32_t bitwidth, const RingProofLimits &limits)
                : bitwidth_(bitwidth), mask_(Bitmask(bitwidth)), limits_(limits) {}

            // The normal form of `expr`, with each comparison in `cases` read as
            // the constant it maps to, and any other one as an atom. Nothing
            // when a limit was reached.
            std::optional< Poly >
            Run(const Expr &expr, const std::unordered_map< const Expr *, uint64_t > &cases) {
                cases_  = &cases;
                failed_ = false;
                Poly result = Normalize(expr);
                if (failed_) {
                    return std::nullopt;
                }
                return result;
            }

          private:
            Poly Constant(uint64_t value) const {
                value &= mask_;
                Poly result;
                if (value != 0) {
                    result.emplace(Monomial{}, value);
                }
                return result;
            }

            static std::optional< uint64_t > ConstantOf(const Poly &poly) {
                if (poly.empty()) {
                    return 0;
                }
                if (poly.size() == 1 && poly.begin()->first.empty()) {
                    return poly.begin()->second;
                }
                return std::nullopt;
            }

            void AddInto(Poly &into, const Poly &term, bool negate = false) const {
                for (const auto &[monomial, coefficient] : term) {
                    const uint64_t value = negate ? ModNeg(coefficient, bitwidth_) : coefficient;
                    auto [slot, inserted] = into.try_emplace(monomial, value);
                    if (!inserted) {
                        slot->second = ModAdd(slot->second, value, bitwidth_);
                        if (slot->second == 0) {
                            into.erase(slot);
                        }
                    }
                }
            }

            Poly Negate(const Poly &poly) const {
                Poly result;
                AddInto(result, poly, /*negate=*/true);
                return result;
            }

            // `~poly`, which is `-1 - poly`.
            Poly Complement(const Poly &poly) const {
                Poly result = Constant(mask_);
                AddInto(result, poly, /*negate=*/true);
                return result;
            }

            Poly Multiply(const Poly &lhs, const Poly &rhs) {
                Poly result;
                if (lhs.size() * rhs.size() > limits_.max_terms * 16) {
                    failed_ = true;
                    return result;
                }
                for (const auto &[left, a] : lhs) {
                    for (const auto &[right, b] : rhs) {
                        const uint64_t coefficient = ModMul(a, b, bitwidth_);
                        if (coefficient == 0) {
                            continue;
                        }
                        Monomial product;
                        product.reserve(left.size() + right.size());
                        std::merge(
                            left.begin(), left.end(), right.begin(), right.end(),
                            std::back_inserter(product)
                        );
                        auto [slot, inserted] = result.try_emplace(std::move(product), coefficient);
                        if (!inserted) {
                            slot->second = ModAdd(slot->second, coefficient, bitwidth_);
                            if (slot->second == 0) {
                                result.erase(slot);
                            }
                        }
                    }
                }
                if (result.size() > limits_.max_terms) {
                    failed_ = true;
                }
                return result;
            }

            static std::string Serialize(const Poly &poly) {
                std::string out;
                for (const auto &[monomial, coefficient] : poly) {
                    out += std::to_string(coefficient);
                    out += ':';
                    for (const uint32_t atom : monomial) {
                        out += std::to_string(atom);
                        out += ',';
                    }
                    out += ';';
                }
                return out;
            }

            Poly AtomPoly(uint32_t id) const {
                Poly result;
                result.emplace(Monomial{ id }, 1);
                return result;
            }

            // The atom `poly` is, when it is exactly one atom of kind `kind`.
            std::optional< uint32_t > SingleAtom(const Poly &poly, Expr::Kind kind) const {
                if (poly.size() != 1) {
                    return std::nullopt;
                }
                const auto &[monomial, coefficient] = *poly.begin();
                if (coefficient != 1 || monomial.size() != 1 || atoms_[monomial[0]].kind != kind) {
                    return std::nullopt;
                }
                return monomial[0];
            }

            // The atom with these parts, made once: the same parts always name
            // the same id, which is what makes equal normal forms comparable.
            Poly Intern(Expr::Kind kind, uint64_t constant, std::vector< Poly > operands, uint32_t variable = 0) {
                std::string key = std::to_string(static_cast< int >(kind));
                key += '/';
                key += std::to_string(variable);
                key += '/';
                key += std::to_string(constant);
                for (const auto &operand : operands) {
                    key += '[';
                    key += Serialize(operand);
                    key += ']';
                }
                auto [slot, inserted] = ids_.try_emplace(std::move(key), static_cast< uint32_t >(atoms_.size()));
                if (inserted) {
                    atoms_.push_back(Atom{ .kind = kind, .variable = variable, .constant = constant,
                                           .operands = std::move(operands) });
                }
                return AtomPoly(slot->second);
            }

            // Operands sorted by their printed form, so the order they were
            // written in does not matter.
            static void SortOperands(std::vector< Poly > &operands, std::vector< std::string > &printed) {
                std::vector< size_t > order(operands.size());
                for (size_t i = 0; i < order.size(); ++i) {
                    order[i] = i;
                }
                std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                    return printed[a] < printed[b];
                });
                std::vector< Poly > sorted_operands;
                std::vector< std::string > sorted_printed;
                sorted_operands.reserve(order.size());
                sorted_printed.reserve(order.size());
                for (const size_t index : order) {
                    sorted_operands.push_back(std::move(operands[index]));
                    sorted_printed.push_back(std::move(printed[index]));
                }
                operands = std::move(sorted_operands);
                printed  = std::move(sorted_printed);
            }

            bool TopBitSet(uint64_t value) const { return ((value >> (bitwidth_ - 1)) & 1) != 0; }

            // `a ^ b ^ ... ^ constant`. Flattened, each operand read as itself or
            // as its complement - whichever prints first, with the constant
            // taking the difference (`~a == a ^ -1`) - and equal operands
            // cancelled in pairs.
            Poly Xor(const std::vector< Poly > &raw) {
                uint64_t constant = 0;
                std::vector< Poly > operands;
                std::vector< Poly > work(raw.begin(), raw.end());
                while (!work.empty()) {
                    Poly poly = std::move(work.back());
                    work.pop_back();
                    if (const auto value = ConstantOf(poly)) {
                        constant ^= *value;
                        continue;
                    }
                    if (const auto atom = SingleAtom(poly, Expr::Kind::kXor)) {
                        constant ^= atoms_[*atom].constant;
                        for (const auto &operand : atoms_[*atom].operands) {
                            operands.push_back(operand);
                        }
                        continue;
                    }
                    Poly complement = Complement(poly);
                    if (const auto atom = SingleAtom(complement, Expr::Kind::kXor)) {
                        constant ^= mask_ ^ atoms_[*atom].constant;
                        for (const auto &operand : atoms_[*atom].operands) {
                            operands.push_back(operand);
                        }
                        continue;
                    }
                    if (Serialize(complement) < Serialize(poly)) {
                        constant ^= mask_;
                        operands.push_back(std::move(complement));
                    } else {
                        operands.push_back(std::move(poly));
                    }
                }

                std::vector< std::string > printed;
                printed.reserve(operands.size());
                for (const auto &operand : operands) {
                    printed.push_back(Serialize(operand));
                }
                SortOperands(operands, printed);
                std::vector< Poly > kept;
                for (size_t i = 0; i < operands.size();) {
                    if (i + 1 < operands.size() && printed[i] == printed[i + 1]) {
                        i += 2;
                        continue;
                    }
                    kept.push_back(std::move(operands[i]));
                    ++i;
                }
                constant &= mask_;
                return XorOf(std::move(kept), constant);
            }

            // The canonical spelling of `kept[0] ^ ... ^ constant`, operands
            // already sorted, distinct and in their chosen polarity.
            Poly XorOf(std::vector< Poly > kept, uint64_t constant) {
                if (kept.empty()) {
                    return Constant(constant);
                }
                // A constant with its top bit set is spelled as the complement
                // of the xor with the constant's complement, so that `a ^ c` and
                // `~(a ^ ~c)` reach the same atom.
                if (TopBitSet(constant)) {
                    return Complement(XorOf(std::move(kept), constant ^ mask_));
                }
                if (kept.size() == 1 && constant == 0) {
                    return std::move(kept[0]);
                }
                return Intern(Expr::Kind::kXor, constant, std::move(kept));
            }

            // `a & b & ... & constant` or `a | b | ... | constant`: flattened,
            // constants folded, duplicates dropped.
            Poly AndOr(Expr::Kind kind, const std::vector< Poly > &raw) {
                const bool is_and = kind == Expr::Kind::kAnd;
                uint64_t constant = is_and ? mask_ : 0;
                std::vector< Poly > operands;
                std::vector< Poly > work(raw.begin(), raw.end());
                while (!work.empty()) {
                    Poly poly = std::move(work.back());
                    work.pop_back();
                    if (const auto value = ConstantOf(poly)) {
                        constant = is_and ? (constant & *value) : (constant | *value);
                        continue;
                    }
                    if (const auto atom = SingleAtom(poly, kind)) {
                        constant = is_and ? (constant & atoms_[*atom].constant)
                                          : (constant | atoms_[*atom].constant);
                        for (const auto &operand : atoms_[*atom].operands) {
                            operands.push_back(operand);
                        }
                        continue;
                    }
                    operands.push_back(std::move(poly));
                }
                // The absorbing constant decides it whatever the operands are.
                if (constant == (is_and ? 0 : mask_)) {
                    return Constant(constant);
                }
                std::vector< std::string > printed;
                printed.reserve(operands.size());
                for (const auto &operand : operands) {
                    printed.push_back(Serialize(operand));
                }
                SortOperands(operands, printed);
                std::vector< Poly > kept;
                for (size_t i = 0; i < operands.size(); ++i) {
                    if (i > 0 && printed[i] == printed[i - 1]) {
                        continue; // a & a == a, a | a == a
                    }
                    kept.push_back(std::move(operands[i]));
                }
                if (kept.empty()) {
                    return Constant(constant);
                }
                if (kept.size() == 1 && constant == (is_and ? mask_ : 0)) {
                    return std::move(kept[0]);
                }
                return Intern(kind, constant, std::move(kept));
            }

            Poly Normalize(const Expr &expr) {
                if (failed_) {
                    return {};
                }
                if (++nodes_ > limits_.max_nodes) {
                    failed_ = true;
                    return {};
                }
                switch (expr.kind) {
                    case Expr::Kind::kConstant:
                        return Constant(expr.constant_val);
                    case Expr::Kind::kVariable:
                        return Intern(Expr::Kind::kVariable, 0, {}, expr.var_index);
                    case Expr::Kind::kAdd: {
                        Poly result = Normalize(*expr.children[0]);
                        AddInto(result, Normalize(*expr.children[1]));
                        return result;
                    }
                    case Expr::Kind::kMul: {
                        const Poly lhs = Normalize(*expr.children[0]);
                        const Poly rhs = Normalize(*expr.children[1]);
                        return Multiply(lhs, rhs);
                    }
                    case Expr::Kind::kNeg:
                        return Negate(Normalize(*expr.children[0]));
                    case Expr::Kind::kNot:
                        return Complement(Normalize(*expr.children[0]));
                    case Expr::Kind::kXor:
                        return Xor({ Normalize(*expr.children[0]), Normalize(*expr.children[1]) });
                    case Expr::Kind::kAnd:
                    case Expr::Kind::kOr:
                        return AndOr(
                            expr.kind, { Normalize(*expr.children[0]), Normalize(*expr.children[1]) }
                        );
                    case Expr::Kind::kShr: {
                        Poly operand = Normalize(*expr.children[0]);
                        const uint64_t amount = expr.constant_val;
                        if (amount >= bitwidth_) {
                            return {};
                        }
                        if (amount == 0) {
                            return operand;
                        }
                        if (const auto value = ConstantOf(operand)) {
                            return Constant(ModShr(*value, amount, bitwidth_));
                        }
                        std::vector< Poly > operands;
                        operands.push_back(std::move(operand));
                        return Intern(Expr::Kind::kShr, amount, std::move(operands));
                    }
                    case Expr::Kind::kCmpEq:
                    case Expr::Kind::kCmpUlt:
                    case Expr::Kind::kCmpSlt: {
                        if (const auto found = cases_->find(&expr); found != cases_->end()) {
                            return Constant(found->second);
                        }
                        Poly lhs = Normalize(*expr.children[0]);
                        Poly rhs = Normalize(*expr.children[1]);
                        const auto a = ConstantOf(lhs);
                        const auto b = ConstantOf(rhs);
                        if (a && b) {
                            const uint64_t value = expr.kind == Expr::Kind::kCmpEq
                                ? ModCmpEq(*a, *b, bitwidth_)
                                : expr.kind == Expr::Kind::kCmpUlt ? ModCmpUlt(*a, *b, bitwidth_)
                                                                   : ModCmpSlt(*a, *b, bitwidth_);
                            return Constant(value);
                        }
                        if (expr.kind == Expr::Kind::kCmpEq && Serialize(rhs) < Serialize(lhs)) {
                            std::swap(lhs, rhs);
                        }
                        std::vector< Poly > operands;
                        operands.push_back(std::move(lhs));
                        operands.push_back(std::move(rhs));
                        return Intern(expr.kind, 0, std::move(operands));
                    }
                }
                failed_ = true;
                return {};
            }

            uint32_t bitwidth_;
            uint64_t mask_;
            RingProofLimits limits_;
            const std::unordered_map< const Expr *, uint64_t > *cases_ = nullptr;
            bool failed_  = false;
            size_t nodes_ = 0;
            std::unordered_map< std::string, uint32_t > ids_;
            std::vector< Atom > atoms_;
        };

        // The tree under `expr`, spelled out, to tell comparisons apart.
        void Spell(const Expr &expr, std::string &out) {
            out += static_cast< char >('a' + static_cast< int >(expr.kind));
            if (expr.kind == Expr::Kind::kConstant || expr.kind == Expr::Kind::kShr) {
                out += std::to_string(expr.constant_val);
            } else if (expr.kind == Expr::Kind::kVariable) {
                out += std::to_string(expr.var_index);
            }
            out += '(';
            for (const auto &child : expr.children) {
                Spell(*child, out);
            }
            out += ')';
        }

        // Every comparison under `expr`, by its spelling: the same comparison
        // written twice is split on once.
        void CollectComparisons(
            const Expr &expr, std::unordered_map< std::string, uint32_t > &index,
            std::vector< std::pair< const Expr *, uint32_t > > &nodes
        ) {
            if (IsComparison(expr.kind)) {
                std::string spelling;
                Spell(expr, spelling);
                const auto [slot, inserted] =
                    index.try_emplace(std::move(spelling), static_cast< uint32_t >(index.size()));
                nodes.emplace_back(&expr, slot->second);
            }
            for (const auto &child : expr.children) {
                CollectComparisons(*child, index, nodes);
            }
        }

    } // namespace

    bool ProveEqualByRingNormalForm(
        const Expr &lhs, const Expr &rhs, uint32_t bitwidth, const RingProofLimits &limits
    ) {
        if (bitwidth == 0 || bitwidth > 64) {
            return false;
        }
        Normalizer normalizer(bitwidth, limits);
        const auto agree = [&](const std::unordered_map< const Expr *, uint64_t > &cases) {
            const auto left = normalizer.Run(lhs, cases);
            if (!left) {
                return false;
            }
            const auto right = normalizer.Run(rhs, cases);
            return right && *left == *right;
        };

        // First with every comparison an atom: when the two sides compare the
        // same things the same way, no split is needed.
        const std::unordered_map< const Expr *, uint64_t > none;
        if (agree(none)) {
            return true;
        }

        std::unordered_map< std::string, uint32_t > index;
        std::vector< std::pair< const Expr *, uint32_t > > nodes;
        CollectComparisons(lhs, index, nodes);
        CollectComparisons(rhs, index, nodes);
        if (index.empty() || index.size() > limits.max_comparisons) {
            return false;
        }
        const uint64_t cases = uint64_t{ 1 } << index.size();
        std::unordered_map< const Expr *, uint64_t > assignment;
        for (uint64_t bits = 0; bits < cases; ++bits) {
            assignment.clear();
            for (const auto &[node, which] : nodes) {
                assignment[node] = (bits >> which) & 1;
            }
            if (!agree(assignment)) {
                return false;
            }
        }
        return true;
    }

} // namespace cobra
