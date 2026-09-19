#include "cobra/verify/Sweep.h"

#include <algorithm>
#include <map>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace cobra {

    namespace {

        using bitwuzla::Kind;
        using bitwuzla::Term;
        using Clock = std::chrono::steady_clock;

        constexpr unsigned kRandomSamples      = 64;
        constexpr unsigned kScratch            = kRandomSamples; // one cone, at chosen inputs
        constexpr unsigned kFirstCounterexample = kRandomSamples + 1;
        constexpr unsigned kMaxCounterexamples = 64;
        constexpr size_t kMaxSupport           = 3;
        constexpr size_t kMaxSynthesisCone     = 4096;
        constexpr auto kGeneralizedBudget      = std::chrono::milliseconds(10);
        constexpr auto kLinearBudget           = std::chrono::milliseconds(20);

        uint64_t Mask(unsigned width) {
            return width >= 64 ? ~uint64_t{ 0 } : (uint64_t{ 1 } << width) - 1;
        }

        int64_t SignExtend(uint64_t value, unsigned width) {
            if (width == 0 || width >= 64) {
                return static_cast< int64_t >(value);
            }
            return static_cast< int64_t >(value << (64 - width)) >> (64 - width);
        }

        class LemmaTerminator final : public bitwuzla::Terminator
        {
          public:
            LemmaTerminator(Clock::time_point deadline, const std::atomic< bool > *stop)
                : deadline_(deadline), stop_(stop) {}

            bool terminate() override {
                return (stop_ != nullptr && stop_->load(std::memory_order_relaxed))
                    || Clock::now() >= deadline_;
            }

          private:
            Clock::time_point deadline_;
            const std::atomic< bool > *stop_;
        };

        struct Node
        {
            Term term;
            std::vector< uint32_t > children;
            unsigned width   = 0;     // 0: Boolean
            bool variable    = false; // a free constant
            bool simulated   = false; // its values below are meaningful
        };

        class Sweeper
        {
          public:
            Sweeper(bitwuzla::TermManager &terms, const SweepSettings &settings, SweepCounters &counters)
                : terms_(terms), settings_(settings), counters_(counters) {}

            std::vector< Term > Run(const std::vector< Term > &assertions) {
                for (const auto &assertion : assertions) {
                    Collect(assertion);
                }
                Simulate();

                rebuilt_.resize(nodes_.size());
                std::map< std::pair< unsigned, std::vector< uint64_t > >, std::vector< uint32_t > > classes;
                bool sweeping = true;
                for (uint32_t i = 0; i < nodes_.size(); ++i) {
                    const Node &node = nodes_[i];
                    rebuilt_[i]      = Rebuilt(i);
                    if (!node.simulated) {
                        continue;
                    }
                    auto &members = classes[{ node.width, Signature(i) }];
                    if (node.variable || node.term.is_value()) {
                        members.push_back(i);
                        continue;
                    }
                    if (sweeping && Stopped()) {
                        sweeping = false;
                    }
                    if (!sweeping) {
                        continue;
                    }

                    std::optional< Term > candidate;
                    bool synthesized = false;
                    bool linear      = false;
                    if (IsConstant(i)) {
                        candidate = node.width == 0
                            ? (values_[i][0] != 0 ? terms_.mk_true() : terms_.mk_false())
                            : terms_.mk_bv_value_uint64(terms_.mk_bv_sort(node.width), values_[i][0]);
                    } else {
                        for (uint32_t member : members) {
                            if (AgreeOnCounterexamples(i, member)) {
                                candidate = rebuilt_[member];
                                break;
                            }
                        }
                        if (!candidate) {
                            members.push_back(i);
                            candidate = Synthesize(i);
                            if (!candidate) {
                                candidate = SynthesizeLinear(i);
                                linear    = candidate.has_value();
                            }
                            synthesized = candidate.has_value();
                        }
                    }
                    if (!candidate) {
                        continue;
                    }
                    if (candidate->id() == rebuilt_[i].id()) {
                        continue;
                    }

                    ++counters_.lemmas;
                    if (synthesized) {
                        ++counters_.synthesized;
                    }
                    const auto answer = Prove(rebuilt_[i], *candidate, linear);
                    if (answer == bitwuzla::Result::UNSAT) {
                        ++counters_.proved;
                        rebuilt_[i] = *candidate;
                    } else if (answer == bitwuzla::Result::SAT) {
                        ++counters_.refuted;
                        if (std::find(members.begin(), members.end(), i) == members.end()) {
                            members.push_back(i);
                        }
                    }
                }

                std::vector< Term > result;
                result.reserve(assertions.size());
                for (const auto &assertion : assertions) {
                    result.push_back(rebuilt_[index_.at(assertion.id())]);
                }
                return result;
            }

            // `SimulateTerm`: the simulation alone, with nothing asked.
            std::optional< std::vector< uint64_t > > Values(const Term &term) {
                Collect(term);
                Simulate();
                const uint32_t i = index_.at(term.id());
                if (!nodes_[i].simulated) {
                    return std::nullopt;
                }
                return std::vector< uint64_t >(values_[i].begin(), values_[i].begin() + kRandomSamples);
            }

            // `EvaluateTerm`: the node table filled from given inputs rather
            // than drawn ones. Every node under the root is needed - nothing is
            // evaluated lazily - so one the sweep cannot evaluate settles it.
            std::optional< std::vector< uint64_t > >
            Values(const Term &term, const std::vector< std::unordered_map< uint64_t, uint64_t > > &inputs) {
                Collect(term);
                values_.assign(nodes_.size(), std::vector< uint64_t >(inputs.size(), 0));
                for (uint32_t i = 0; i < nodes_.size(); ++i) {
                    Node &node     = nodes_[i];
                    node.simulated = Simulable(node);
                    for (uint32_t child : node.children) {
                        node.simulated = node.simulated && nodes_[child].simulated;
                    }
                    if (!node.simulated) {
                        return std::nullopt;
                    }
                    if (node.term.is_value()) {
                        std::fill(values_[i].begin(), values_[i].end(), ValueOf(node.term, node.width));
                        continue;
                    }
                    for (size_t slot = 0; slot < inputs.size(); ++slot) {
                        if (!node.variable) {
                            values_[i][slot] = Evaluate(i, static_cast< unsigned >(slot));
                            continue;
                        }
                        const auto found = inputs[slot].find(node.term.id());
                        if (found == inputs[slot].end()) {
                            return std::nullopt;
                        }
                        values_[i][slot] = found->second & (node.width == 0 ? 1 : Mask(node.width));
                    }
                }
                return values_[index_.at(term.id())];
            }

          private:
            bool Stopped() const {
                return (settings_.stop != nullptr && settings_.stop->load(std::memory_order_relaxed))
                    || Clock::now() >= settings_.deadline;
            }

            // Every subterm once, children before parents.
            void Collect(const Term &root) {
                std::vector< std::pair< Term, bool > > stack{ { root, false } };
                while (!stack.empty()) {
                    auto [term, expanded] = stack.back();
                    stack.pop_back();
                    if (index_.count(term.id()) != 0) {
                        continue;
                    }
                    const auto children = term.children();
                    if (!expanded) {
                        stack.emplace_back(term, true);
                        for (auto child = children.rbegin(); child != children.rend(); ++child) {
                            if (index_.count(child->id()) == 0) {
                                stack.emplace_back(*child, false);
                            }
                        }
                        continue;
                    }
                    Node node;
                    node.term     = term;
                    node.variable = term.is_const();
                    node.children.reserve(children.size());
                    for (const auto &child : children) {
                        node.children.push_back(index_.at(child.id()));
                    }
                    const auto sort = term.sort();
                    if (sort.is_bool()) {
                        node.width = 0;
                    } else if (sort.is_bv() && sort.bv_size() <= 64) {
                        node.width = static_cast< unsigned >(sort.bv_size());
                    } else {
                        node.width = 65; // never simulated
                    }
                    index_.emplace(term.id(), static_cast< uint32_t >(nodes_.size()));
                    nodes_.push_back(std::move(node));
                }
            }

            std::vector< uint64_t > Signature(uint32_t i) const {
                return { values_[i].begin(), values_[i].begin() + kRandomSamples };
            }

            bool IsConstant(uint32_t i) const {
                const auto &values = values_[i];
                for (size_t slot = 1; slot < values.size(); ++slot) {
                    if (slot != kScratch && values[slot] != values[0]) {
                        return false;
                    }
                }
                return true;
            }

            bool AgreeOnCounterexamples(uint32_t a, uint32_t b) const {
                for (unsigned slot = kFirstCounterexample; slot < values_[a].size(); ++slot) {
                    if (values_[a][slot] != values_[b][slot]) {
                        return false;
                    }
                }
                return true;
            }

            uint64_t Sample(unsigned width, unsigned slot) {
                if (width == 0) {
                    return random_() & 1;
                }
                const uint64_t mask = Mask(width);
                switch (slot) {
                    case 0:
                        return 0;
                    case 1:
                        return mask;
                    case 2:
                        return 1;
                    case 3:
                        return mask >> 1;
                    case 4:
                        return ((mask >> 1) + 1) & mask;
                    default:
                        break;
                }
                return slot < 16 ? random_() & 0xff & mask : random_() & mask;
            }

            // Whether `Evaluate` knows the operator of `node`.
            static bool Simulable(const Node &node) {
                if (node.width > 64) {
                    return false;
                }
                if (node.variable || node.term.is_value()) {
                    return true;
                }
                switch (node.term.kind()) {
                    case Kind::NOT:
                    case Kind::AND:
                    case Kind::OR:
                    case Kind::XOR:
                    case Kind::IMPLIES:
                    case Kind::BV_NOT:
                    case Kind::BV_AND:
                    case Kind::BV_OR:
                    case Kind::BV_XOR:
                    case Kind::BV_XNOR:
                    case Kind::BV_NAND:
                    case Kind::BV_NOR:
                    case Kind::BV_ADD:
                    case Kind::BV_MUL:
                    case Kind::BV_SUB:
                    case Kind::BV_NEG:
                    case Kind::BV_INC:
                    case Kind::BV_DEC:
                    case Kind::BV_UDIV:
                    case Kind::BV_UREM:
                    case Kind::BV_SHL:
                    case Kind::BV_SHR:
                    case Kind::BV_ASHR:
                    case Kind::BV_CONCAT:
                    case Kind::BV_EXTRACT:
                    case Kind::BV_ZERO_EXTEND:
                    case Kind::BV_SIGN_EXTEND:
                    case Kind::EQUAL:
                    case Kind::DISTINCT:
                    case Kind::ITE:
                    case Kind::BV_ULT:
                    case Kind::BV_ULE:
                    case Kind::BV_UGT:
                    case Kind::BV_UGE:
                    case Kind::BV_SLT:
                    case Kind::BV_SLE:
                    case Kind::BV_SGT:
                    case Kind::BV_SGE:
                        return true;
                    default:
                        return false;
                }
            }

            // One operator applied to its operands' values (`widths` are theirs;
            // 0 is Boolean). What the node table and the check of a synthesized
            // candidate both evaluate with.
            static uint64_t Apply(
                const Term &term, unsigned w, const std::vector< uint64_t > &v, const std::vector< unsigned > &cw
            ) {
                const uint64_t m = w == 0 ? 1 : Mask(w);
                switch (term.kind()) {
                    case Kind::NOT:
                        return v[0] ^ 1;
                    case Kind::BV_NOT:
                        return ~v[0] & m;
                    case Kind::AND:
                    case Kind::BV_AND: {
                        uint64_t r = m;
                        for (size_t k = 0; k < v.size(); ++k) {
                            r &= v[k];
                        }
                        return r;
                    }
                    case Kind::OR:
                    case Kind::BV_OR: {
                        uint64_t r = 0;
                        for (size_t k = 0; k < v.size(); ++k) {
                            r |= v[k];
                        }
                        return r;
                    }
                    case Kind::XOR:
                    case Kind::BV_XOR: {
                        uint64_t r = 0;
                        for (size_t k = 0; k < v.size(); ++k) {
                            r ^= v[k];
                        }
                        return r;
                    }
                    case Kind::IMPLIES:
                        return (v[0] ^ 1) | v[1];
                    case Kind::BV_XNOR:
                        return ~(v[0] ^ v[1]) & m;
                    case Kind::BV_NAND:
                        return ~(v[0] & v[1]) & m;
                    case Kind::BV_NOR:
                        return ~(v[0] | v[1]) & m;
                    case Kind::BV_ADD: {
                        uint64_t r = 0;
                        for (size_t k = 0; k < v.size(); ++k) {
                            r += v[k];
                        }
                        return r & m;
                    }
                    case Kind::BV_MUL: {
                        uint64_t r = 1;
                        for (size_t k = 0; k < v.size(); ++k) {
                            r *= v[k];
                        }
                        return r & m;
                    }
                    case Kind::BV_SUB:
                        return (v[0] - v[1]) & m;
                    case Kind::BV_NEG:
                        return (0 - v[0]) & m;
                    case Kind::BV_INC:
                        return (v[0] + 1) & m;
                    case Kind::BV_DEC:
                        return (v[0] - 1) & m;
                    case Kind::BV_UDIV:
                        return v[1] == 0 ? m : v[0] / v[1];
                    case Kind::BV_UREM:
                        return v[1] == 0 ? v[0] : v[0] % v[1];
                    case Kind::BV_SHL:
                        return v[1] >= w ? 0 : (v[0] << v[1]) & m;
                    case Kind::BV_SHR:
                        return v[1] >= w ? 0 : v[0] >> v[1];
                    case Kind::BV_ASHR: {
                        const uint64_t amount = v[1] >= w ? w - 1 : v[1];
                        return static_cast< uint64_t >(SignExtend(v[0], w) >> amount) & m;
                    }
                    case Kind::BV_CONCAT: {
                        uint64_t r = 0;
                        for (size_t k = 0; k < v.size(); ++k) {
                            r = (cw[k] >= 64 ? 0 : r << cw[k]) | v[k];
                        }
                        return r & m;
                    }
                    case Kind::BV_EXTRACT:
                        return (v[0] >> term.indices()[1]) & m;
                    case Kind::BV_ZERO_EXTEND:
                        return v[0];
                    case Kind::BV_SIGN_EXTEND:
                        return static_cast< uint64_t >(SignExtend(v[0], cw[0])) & m;
                    case Kind::EQUAL:
                        return v[0] == v[1] ? 1 : 0;
                    case Kind::DISTINCT:
                        return v[0] != v[1] ? 1 : 0;
                    case Kind::ITE:
                        return v[0] != 0 ? v[1] : v[2];
                    case Kind::BV_ULT:
                        return v[0] < v[1] ? 1 : 0;
                    case Kind::BV_ULE:
                        return v[0] <= v[1] ? 1 : 0;
                    case Kind::BV_UGT:
                        return v[0] > v[1] ? 1 : 0;
                    case Kind::BV_UGE:
                        return v[0] >= v[1] ? 1 : 0;
                    case Kind::BV_SLT:
                        return SignExtend(v[0], cw[0]) < SignExtend(v[1], cw[1]) ? 1 : 0;
                    case Kind::BV_SLE:
                        return SignExtend(v[0], cw[0]) <= SignExtend(v[1], cw[1]) ? 1 : 0;
                    case Kind::BV_SGT:
                        return SignExtend(v[0], cw[0]) > SignExtend(v[1], cw[1]) ? 1 : 0;
                    case Kind::BV_SGE:
                        return SignExtend(v[0], cw[0]) >= SignExtend(v[1], cw[1]) ? 1 : 0;
                    default:
                        return 0;
                }
            }

            // Called once per node and input, so the operand lists are reused
            // rather than allocated each time.
            uint64_t Evaluate(uint32_t i, unsigned slot) const {
                const Node &node = nodes_[i];
                operands_.clear();
                widths_.clear();
                for (uint32_t child : node.children) {
                    operands_.push_back(values_[child][slot]);
                    widths_.push_back(nodes_[child].width);
                }
                return Apply(node.term, node.width, operands_, widths_);
            }

            static uint64_t ValueOf(const Term &value, unsigned width) {
                if (width == 0) {
                    return value.is_true() ? 1 : 0;
                }
                return std::stoull(value.value< std::string >(2), nullptr, 2);
            }

            void Simulate() {
                values_.assign(nodes_.size(), std::vector< uint64_t >(kRandomSamples + 1, 0));
                for (uint32_t i = 0; i < nodes_.size(); ++i) {
                    Node &node    = nodes_[i];
                    node.simulated = Simulable(node);
                    for (uint32_t child : node.children) {
                        node.simulated = node.simulated && nodes_[child].simulated;
                    }
                    if (!node.simulated) {
                        continue;
                    }
                    if (node.term.is_value()) {
                        std::fill(values_[i].begin(), values_[i].end(), ValueOf(node.term, node.width));
                        continue;
                    }
                    for (unsigned slot = 0; slot < kRandomSamples; ++slot) {
                        values_[i][slot] = node.variable ? Sample(node.width, slot) : Evaluate(i, slot);
                    }
                }
            }

            // A model that told a candidate from its subterm apart, added to the
            // inputs so that it tells apart every other pair it separates
            // without asking the solver.
            void AddCounterexample(bitwuzla::Bitwuzla &solver) {
                std::vector< uint64_t > model(nodes_.size(), 0);
                try {
                    for (uint32_t i = 0; i < nodes_.size(); ++i) {
                        if (nodes_[i].variable && nodes_[i].simulated) {
                            model[i] = ValueOf(solver.get_value(nodes_[i].term), nodes_[i].width);
                        }
                    }
                } catch (...) {
                    return;
                }
                const unsigned slot = static_cast< unsigned >(values_.front().size());
                for (uint32_t i = 0; i < nodes_.size(); ++i) {
                    const Node &node = nodes_[i];
                    uint64_t value   = 0;
                    if (node.simulated) {
                        if (node.term.is_value()) {
                            value = values_[i][0];
                        } else if (node.variable) {
                            value = model[i];
                        } else {
                            values_[i].push_back(0);
                            value = Evaluate(i, slot);
                            values_[i].back() = value;
                            continue;
                        }
                    }
                    values_[i].push_back(value);
                }
                ++counterexamples_;
            }

            // The node with the substitutions proved so far, children first.
            Term Rebuilt(uint32_t i) {
                const Node &node = nodes_[i];
                if (node.variable || node.term.is_value() || node.children.empty()) {
                    return node.term;
                }
                std::vector< Term > children;
                children.reserve(node.children.size());
                bool changed = false;
                const auto original = node.term.children();
                for (size_t k = 0; k < node.children.size(); ++k) {
                    children.push_back(rebuilt_[node.children[k]]);
                    changed = changed || children.back().id() != original[k].id();
                }
                if (!changed) {
                    return node.term;
                }
                return terms_.mk_term(node.term.kind(), children, node.term.indices());
            }

            // The free constants `term` depends on, as nodes, or nothing when
            // there are more than kMaxSupport.
            const std::vector< uint32_t > *Support(const Term &term) {
                if (auto found = support_.find(term.id()); found != support_.end()) {
                    return found->second ? &*found->second : nullptr;
                }
                std::optional< std::vector< uint32_t > > result;
                if (term.is_const()) {
                    if (auto found = index_.find(term.id()); found != index_.end()) {
                        result = std::vector< uint32_t >{ found->second };
                    }
                } else {
                    std::vector< uint32_t > all;
                    bool fits = true;
                    for (const auto &child : term.children()) {
                        const auto *inner = Support(child);
                        if (inner == nullptr) {
                            fits = false;
                            break;
                        }
                        for (uint32_t variable : *inner) {
                            if (std::find(all.begin(), all.end(), variable) == all.end()) {
                                all.push_back(variable);
                            }
                        }
                        if (all.size() > kMaxSupport) {
                            fits = false;
                            break;
                        }
                    }
                    if (fits) {
                        std::sort(all.begin(), all.end());
                        result = std::move(all);
                    }
                }
                auto &slot = support_[term.id()];
                slot       = std::move(result);
                return slot ? &*slot : nullptr;
            }

            // Whether `term` computes anything but bitwise logic.
            bool Arithmetic(const Term &term) {
                if (auto found = arithmetic_.find(term.id()); found != arithmetic_.end()) {
                    return found->second;
                }
                bool result = false;
                if (!term.is_const() && !term.is_value()) {
                    switch (term.kind()) {
                        case Kind::BV_AND:
                        case Kind::BV_OR:
                        case Kind::BV_XOR:
                        case Kind::BV_NOT:
                        case Kind::BV_XNOR:
                        case Kind::BV_NAND:
                        case Kind::BV_NOR:
                            break;
                        default:
                            result = true;
                    }
                    for (const auto &child : term.children()) {
                        result = result || Arithmetic(child);
                    }
                }
                arithmetic_[term.id()] = result;
                return result;
            }

            // The bitwise function of its variables node `i` computes, when its
            // values say it computes one: evaluated with each variable all ones
            // or all zeros, turned into algebraic normal form (an XOR of ANDs of
            // variables, each under a constant mask), and checked against every
            // input there is.
            // ---- linear MBA over atoms ------------------------------------

            // The subterms a linear MBA reading of a term treats as opaque: what
            // is below `+`, `-`, negation and multiplication by a constant, and
            // below `&`, `|`, `^` and `~` with no constant but 0 and -1 in them.
            struct LinearShape
            {
                std::vector< Term > atoms;
                bool fits = true;
            };

            static int AtomIndex(const LinearShape &shape, const Term &term) {
                for (size_t index = 0; index < shape.atoms.size(); ++index) {
                    if (shape.atoms[index].id() == term.id()) {
                        return static_cast< int >(index);
                    }
                }
                return -1;
            }

            static void AtomsOf(const Term &term, bool bitwise, LinearShape &shape) {
                if (!shape.fits) {
                    return;
                }
                if (term.is_value()) {
                    if (bitwise) {
                        const auto width = static_cast< unsigned >(term.sort().bv_size());
                        const uint64_t value = ValueOf(term, width);
                        shape.fits = value == 0 || value == Mask(width);
                    }
                    return;
                }
                if (!term.is_const()) {
                    const auto children = term.children();
                    switch (term.kind()) {
                        case Kind::BV_ADD:
                        case Kind::BV_SUB:
                        case Kind::BV_NEG:
                            if (bitwise) {
                                break;
                            }
                            for (const auto &child : children) {
                                AtomsOf(child, false, shape);
                            }
                            return;
                        case Kind::BV_MUL:
                            if (bitwise || children.size() != 2
                                || !(children[0].is_value() || children[1].is_value()))
                            {
                                break;
                            }
                            AtomsOf(children[0].is_value() ? children[1] : children[0], false, shape);
                            return;
                        case Kind::BV_AND:
                        case Kind::BV_OR:
                        case Kind::BV_XOR:
                        case Kind::BV_NOT:
                            for (const auto &child : children) {
                                AtomsOf(child, true, shape);
                            }
                            return;
                        default:
                            break;
                    }
                }
                if (AtomIndex(shape, term) < 0) {
                    shape.atoms.push_back(term);
                    shape.fits = shape.atoms.size() <= kMaxSupport;
                }
            }

            // The bitwise part at one-bit inputs: 0 or 1.
            static uint64_t EvaluateBit(const Term &term, const LinearShape &shape, size_t bits) {
                if (term.is_value()) {
                    return ValueOf(term, static_cast< unsigned >(term.sort().bv_size())) != 0 ? 1 : 0;
                }
                if (const int atom = AtomIndex(shape, term); atom >= 0) {
                    return (bits >> atom) & 1;
                }
                const auto children = term.children();
                uint64_t result     = term.kind() == Kind::BV_AND ? 1 : 0;
                for (const auto &child : children) {
                    const uint64_t value = EvaluateBit(child, shape, bits);
                    switch (term.kind()) {
                        case Kind::BV_NOT:
                            return value ^ 1;
                        case Kind::BV_AND:
                            result &= value;
                            break;
                        case Kind::BV_OR:
                            result |= value;
                            break;
                        default:
                            result ^= value;
                            break;
                    }
                }
                return result;
            }

            // The linear MBA signature: integers modulo the width, the bitwise
            // parts at one-bit inputs, and a constant c as -c times the
            // all-ones function.
            static uint64_t EvaluateLinear(const Term &term, const LinearShape &shape, size_t bits, uint64_t mask) {
                if (term.is_value()) {
                    return (0 - ValueOf(term, static_cast< unsigned >(term.sort().bv_size()))) & mask;
                }
                if (const int atom = AtomIndex(shape, term); atom >= 0) {
                    return (bits >> atom) & 1;
                }
                const auto children = term.children();
                switch (term.kind()) {
                    case Kind::BV_ADD: {
                        uint64_t sum = 0;
                        for (const auto &child : children) {
                            sum += EvaluateLinear(child, shape, bits, mask);
                        }
                        return sum & mask;
                    }
                    case Kind::BV_SUB:
                        return (EvaluateLinear(children[0], shape, bits, mask)
                                - EvaluateLinear(children[1], shape, bits, mask))
                            & mask;
                    case Kind::BV_NEG:
                        return (0 - EvaluateLinear(children[0], shape, bits, mask)) & mask;
                    case Kind::BV_MUL: {
                        const Term &factor = children[0].is_value() ? children[0] : children[1];
                        const Term &other  = children[0].is_value() ? children[1] : children[0];
                        return (ValueOf(factor, static_cast< unsigned >(factor.sort().bv_size()))
                                * EvaluateLinear(other, shape, bits, mask))
                            & mask;
                    }
                    default:
                        return EvaluateBit(term, shape, bits);
                }
            }

            // The value of a term built from nodes, at one input slot.
            uint64_t EvaluateTerm(
                const Term &term, unsigned slot, std::unordered_map< uint64_t, uint64_t > &memo
            ) const {
                if (const auto found = index_.find(term.id()); found != index_.end()) {
                    if (!nodes_[found->second].simulated) {
                        throw std::invalid_argument("not simulated");
                    }
                    return values_[found->second][slot];
                }
                if (const auto found = memo.find(term.id()); found != memo.end()) {
                    return found->second;
                }
                const unsigned width = WidthOf(term);
                uint64_t result      = 0;
                if (term.is_value()) {
                    result = ValueOf(term, width);
                } else if (term.is_const()) {
                    throw std::invalid_argument("a constant the formula does not hold");
                } else {
                    Node shape;
                    shape.term  = term;
                    shape.width = width;
                    if (!Simulable(shape)) {
                        throw std::invalid_argument("an operator the sweep does not evaluate");
                    }
                    std::vector< uint64_t > operands;
                    std::vector< unsigned > widths;
                    for (const auto &child : term.children()) {
                        operands.push_back(EvaluateTerm(child, slot, memo));
                        widths.push_back(WidthOf(child));
                    }
                    result = Apply(term, width, operands, widths);
                }
                memo.emplace(term.id(), result);
                return result;
            }

            static unsigned WidthOf(const Term &term) {
                const auto sort = term.sort();
                if (sort.is_bool()) {
                    return 0;
                }
                if (!sort.is_bv() || sort.bv_size() > 64) {
                    throw std::invalid_argument("wider than the sweep evaluates");
                }
                return static_cast< unsigned >(sort.bv_size());
            }

            static size_t OperationsAbove(
                const Term &term, const LinearShape &shape, std::unordered_set< uint64_t > &seen
            ) {
                if (term.is_value() || term.is_const() || AtomIndex(shape, term) >= 0
                    || !seen.insert(term.id()).second)
                {
                    return 0;
                }
                size_t count = 1;
                for (const auto &child : term.children()) {
                    count += OperationsAbove(child, shape, seen);
                }
                return count;
            }

            // Node `i` read as a linear MBA over at most three atoms - arithmetic
            // subterms the bitwise operators apply to, as `t` in
            // `2 * (t & x) + (t ^ x)` - and rewritten from its signature in the
            // conjunction basis, `sum c_S * AND(S)` with the empty AND all ones,
            // when that is smaller: the candidate a sum of partial products
            // hides. The linear MBA theorem makes the two equal whatever the
            // atoms are; `Prove` still asks, with the atoms cut free.
            std::optional< Term > SynthesizeLinear(uint32_t i) {
                const Node &node = nodes_[i];
                if (node.width == 0) {
                    return std::nullopt;
                }
                const Term &rebuilt = rebuilt_[i];
                if (rebuilt.is_const() || rebuilt.is_value()) {
                    return std::nullopt;
                }
                LinearShape shape;
                AtomsOf(rebuilt, false, shape);
                if (!shape.fits || shape.atoms.empty()
                    || (shape.atoms.size() == 1 && shape.atoms[0].id() == rebuilt.id()))
                {
                    return std::nullopt;
                }
                for (const auto &atom : shape.atoms) {
                    if (!atom.sort().is_bv() || atom.sort().bv_size() != node.width) {
                        return std::nullopt;
                    }
                }
                std::unordered_set< uint64_t > seen;
                const size_t before = OperationsAbove(rebuilt, shape, seen);

                const uint64_t mask = Mask(node.width);
                const size_t count  = shape.atoms.size();
                std::vector< uint64_t > coefficients(size_t{ 1 } << count);
                for (size_t bits = 0; bits < coefficients.size(); ++bits) {
                    coefficients[bits] = EvaluateLinear(rebuilt, shape, bits, mask);
                }
                for (size_t bit = 0; bit < count; ++bit) {
                    for (size_t bits = 0; bits < coefficients.size(); ++bits) {
                        if ((bits >> bit) & 1) {
                            coefficients[bits] =
                                (coefficients[bits] - coefficients[bits ^ (size_t{ 1 } << bit)]) & mask;
                        }
                    }
                }

                std::unordered_map< uint64_t, uint64_t > memo;
                try {
                    for (unsigned slot = 0; slot < values_[i].size(); ++slot) {
                        if (slot == kScratch) {
                            continue;
                        }
                        memo.clear();
                        std::vector< uint64_t > atoms;
                        for (const auto &atom : shape.atoms) {
                            atoms.push_back(EvaluateTerm(atom, slot, memo));
                        }
                        uint64_t value = 0;
                        for (size_t subset = 0; subset < coefficients.size(); ++subset) {
                            uint64_t product = mask;
                            for (size_t position = 0; position < count; ++position) {
                                if ((subset >> position) & 1) {
                                    product &= atoms[position];
                                }
                            }
                            value += coefficients[subset] * product;
                        }
                        if ((value & mask) != values_[i][slot]) {
                            return std::nullopt;
                        }
                    }
                } catch (const std::invalid_argument &) {
                    return std::nullopt;
                }

                const auto sort = terms_.mk_bv_sort(node.width);
                std::optional< Term > result;
                for (size_t subset = 0; subset < coefficients.size(); ++subset) {
                    const uint64_t coefficient = coefficients[subset];
                    if (coefficient == 0) {
                        continue;
                    }
                    std::optional< Term > product;
                    for (size_t position = 0; position < count; ++position) {
                        if ((subset >> position) & 1) {
                            const Term &atom = shape.atoms[position];
                            product = product ? terms_.mk_term(Kind::BV_AND, { *product, atom }) : atom;
                        }
                    }
                    const Term monomial = !product
                        ? terms_.mk_bv_value_uint64(sort, (0 - coefficient) & mask)
                        : coefficient == 1    ? *product
                        : coefficient == mask ? terms_.mk_term(Kind::BV_NEG, { *product })
                                              : terms_.mk_term(
                                                    Kind::BV_MUL,
                                                    { terms_.mk_bv_value_uint64(sort, coefficient), *product }
                                                );
                    result = result ? terms_.mk_term(Kind::BV_ADD, { *result, monomial }) : monomial;
                }
                const Term candidate = result ? *result : terms_.mk_bv_zero(sort);
                std::unordered_set< uint64_t > seen_after;
                if (OperationsAbove(candidate, shape, seen_after) >= before) {
                    return std::nullopt;
                }
                return candidate;
            }

            std::optional< Term > Synthesize(uint32_t i) {
                const Node &node = nodes_[i];
                if (node.width == 0) {
                    return std::nullopt;
                }
                const auto *variables = Support(rebuilt_[i]);
                if (variables == nullptr || variables->empty() || !Arithmetic(rebuilt_[i])) {
                    return std::nullopt;
                }
                for (uint32_t variable : *variables) {
                    if (nodes_[variable].width != node.width) {
                        return std::nullopt;
                    }
                }

                std::vector< uint32_t > cone;
                {
                    std::unordered_set< uint32_t > seen{ i };
                    std::vector< uint32_t > stack{ i };
                    while (!stack.empty()) {
                        const uint32_t at = stack.back();
                        stack.pop_back();
                        cone.push_back(at);
                        for (uint32_t child : nodes_[at].children) {
                            if (seen.insert(child).second) {
                                stack.push_back(child);
                            }
                        }
                        if (cone.size() > kMaxSynthesisCone) {
                            return std::nullopt;
                        }
                    }
                    std::sort(cone.begin(), cone.end()); // children first
                }

                const size_t count = variables->size();
                const uint64_t mask = Mask(node.width);
                std::vector< uint64_t > anf(size_t{ 1 } << count);
                for (size_t minterm = 0; minterm < anf.size(); ++minterm) {
                    for (uint32_t at : cone) {
                        const Node &inner = nodes_[at];
                        if (inner.term.is_value()) {
                            continue; // the same in every slot
                        }
                        if (inner.variable) {
                            const auto position =
                                std::find(variables->begin(), variables->end(), at) - variables->begin();
                            const bool set = static_cast< size_t >(position) < count && ((minterm >> position) & 1);
                            values_[at][kScratch] = set ? Mask(inner.width) : 0;
                        } else {
                            values_[at][kScratch] = Evaluate(at, kScratch);
                        }
                    }
                    anf[minterm] = values_[i][kScratch];
                }
                for (size_t bit = 0; bit < count; ++bit) {
                    for (size_t minterm = 0; minterm < anf.size(); ++minterm) {
                        if ((minterm >> bit) & 1) {
                            anf[minterm] ^= anf[minterm ^ (size_t{ 1 } << bit)];
                        }
                    }
                }
                for (size_t slot = 0; slot < values_[i].size(); ++slot) {
                    if (slot == kScratch) {
                        continue;
                    }
                    uint64_t value = 0;
                    for (size_t minterm = 0; minterm < anf.size(); ++minterm) {
                        if (anf[minterm] == 0) {
                            continue;
                        }
                        uint64_t product = mask;
                        for (size_t position = 0; position < count; ++position) {
                            if ((minterm >> position) & 1) {
                                product &= values_[(*variables)[position]][slot];
                            }
                        }
                        value ^= product & anf[minterm];
                    }
                    if ((value & mask) != values_[i][slot]) {
                        return std::nullopt;
                    }
                }

                const auto sort = terms_.mk_bv_sort(node.width);
                std::optional< Term > result;
                for (size_t minterm = 0; minterm < anf.size(); ++minterm) {
                    if (anf[minterm] == 0) {
                        continue;
                    }
                    std::optional< Term > product;
                    for (size_t position = 0; position < count; ++position) {
                        if ((minterm >> position) & 1) {
                            const Term &variable = nodes_[(*variables)[position]].term;
                            product = product ? terms_.mk_term(Kind::BV_AND, { *product, variable }) : variable;
                        }
                    }
                    const Term coefficient = terms_.mk_bv_value_uint64(sort, anf[minterm]);
                    const Term monomial    = !product ? coefficient
                           : anf[minterm] == mask   ? *product
                                                    : terms_.mk_term(Kind::BV_AND, { *product, coefficient });
                    result = result ? terms_.mk_term(Kind::BV_XOR, { *result, monomial }) : monomial;
                }
                return result ? *result : terms_.mk_bv_zero(sort);
            }

            // `lhs != rhs` with every subterm the two sides share replaced by a
            // fresh constant: if that is unsatisfiable, so is the lemma.
            Term Generalized(const Term &lhs, const Term &rhs) {
                const auto gather = [](const Term &root) {
                    std::unordered_set< uint64_t > seen;
                    std::vector< Term > stack{ root };
                    while (!stack.empty()) {
                        Term term = stack.back();
                        stack.pop_back();
                        if (!seen.insert(term.id()).second) {
                            continue;
                        }
                        for (const auto &child : term.children()) {
                            stack.push_back(child);
                        }
                    }
                    return seen;
                };
                const auto left  = gather(lhs);
                const auto right = gather(rhs);
                std::unordered_map< uint64_t, Term > cut;
                const auto rebuild = [&](const Term &root) {
                    std::vector< std::pair< Term, bool > > stack{ { root, false } };
                    while (!stack.empty()) {
                        auto [term, expanded] = stack.back();
                        stack.pop_back();
                        if (cut.count(term.id()) != 0) {
                            continue;
                        }
                        if (term.is_value() || term.is_const()) {
                            cut.emplace(term.id(), term);
                            continue;
                        }
                        if (left.count(term.id()) != 0 && right.count(term.id()) != 0) {
                            cut.emplace(term.id(), terms_.mk_const(term.sort()));
                            continue;
                        }
                        const auto children = term.children();
                        if (!expanded) {
                            stack.emplace_back(term, true);
                            for (const auto &child : children) {
                                stack.emplace_back(child, false);
                            }
                            continue;
                        }
                        std::vector< Term > rebuilt;
                        bool changed = false;
                        for (const auto &child : children) {
                            rebuilt.push_back(cut.at(child.id()));
                            changed = changed || rebuilt.back().id() != child.id();
                        }
                        cut.emplace(
                            term.id(), changed ? terms_.mk_term(term.kind(), rebuilt, term.indices()) : term
                        );
                    }
                    return cut.at(root.id());
                };
                const Term l = rebuild(lhs);
                const Term r = rebuild(rhs);
                return terms_.mk_term(Kind::DISTINCT, { l, r });
            }

            bitwuzla::Result Ask(
                const Term &formula, Clock::time_point deadline, bool models, bitwuzla::Bitwuzla **kept
            ) {
                const auto left = std::chrono::duration_cast< std::chrono::milliseconds >(deadline - Clock::now());
                bitwuzla::Options options;
                options.set(
                    bitwuzla::Option::TIME_LIMIT_PER, static_cast< uint64_t >(std::max< int64_t >(1, left.count()))
                );
                options.set(bitwuzla::Option::REWRITE_LEVEL, settings_.rewrite_level);
                options.set(bitwuzla::Option::PRODUCE_MODELS, models);
                solver_ = std::make_unique< bitwuzla::Bitwuzla >(terms_, options);
                LemmaTerminator terminator(deadline, settings_.stop);
                solver_->configure_terminator(&terminator);
                solver_->assert_formula(formula);
                const auto answer = solver_->check_sat();
                solver_->configure_terminator(nullptr);
                if (kept != nullptr) {
                    *kept = solver_.get();
                }
                return answer;
            }

            // A linear candidate is only ever asked with its atoms cut free: that
            // is the whole claim, and small, so what it cannot settle quickly is
            // not worth the rest of the budget.
            bitwuzla::Result Prove(const Term &lhs, const Term &rhs, bool linear) {
                const Term full    = terms_.mk_term(Kind::DISTINCT, { lhs, rhs });
                const Term general = Generalized(lhs, rhs);
                if (linear) {
                    const auto answer =
                        Ask(general, std::min(settings_.deadline, Clock::now() + kLinearBudget), false, nullptr);
                    if (answer == bitwuzla::Result::UNSAT) {
                        ++counters_.generalized;
                        return answer;
                    }
                    return bitwuzla::Result::UNKNOWN;
                }
                if (general.id() != full.id()) {
                    const auto answer =
                        Ask(general, std::min(settings_.deadline, Clock::now() + kGeneralizedBudget), false, nullptr);
                    if (answer == bitwuzla::Result::UNSAT) {
                        ++counters_.generalized;
                        return answer;
                    }
                    if (Stopped()) {
                        return bitwuzla::Result::UNKNOWN;
                    }
                }
                const bool models = counterexamples_ < kMaxCounterexamples;
                bitwuzla::Bitwuzla *solver = nullptr;
                const auto answer          = Ask(full, settings_.deadline, models, &solver);
                if (answer == bitwuzla::Result::SAT && models) {
                    AddCounterexample(*solver);
                }
                return answer;
            }

            bitwuzla::TermManager &terms_;
            const SweepSettings &settings_;
            SweepCounters &counters_;
            std::vector< Node > nodes_;
            std::unordered_map< uint64_t, uint32_t > index_;
            std::vector< std::vector< uint64_t > > values_; // per node: samples, scratch, counterexamples
            std::vector< Term > rebuilt_;
            std::unordered_map< uint64_t, std::optional< std::vector< uint32_t > > > support_;
            std::unordered_map< uint64_t, bool > arithmetic_;
            std::unique_ptr< bitwuzla::Bitwuzla > solver_;
            std::mt19937_64 random_{ 0x5eedC0B7A };
            unsigned counterexamples_ = 0;
            mutable std::vector< uint64_t > operands_; // `Evaluate`'s
            mutable std::vector< unsigned > widths_;
        };

    } // namespace

    std::vector< bitwuzla::Term > SweepFormula(
        bitwuzla::TermManager &terms, const std::vector< bitwuzla::Term > &assertions,
        const SweepSettings &settings, SweepCounters *counters
    ) {
        SweepCounters local;
        Sweeper sweeper(terms, settings, counters != nullptr ? *counters : local);
        return sweeper.Run(assertions);
    }

    std::optional< std::vector< uint64_t > >
    SimulateTerm(bitwuzla::TermManager &terms, const bitwuzla::Term &term) {
        const SweepSettings settings;
        SweepCounters counters;
        Sweeper sweeper(terms, settings, counters);
        return sweeper.Values(term);
    }

    std::optional< std::vector< uint64_t > > EvaluateTerm(
        bitwuzla::TermManager &terms, const bitwuzla::Term &term,
        const std::vector< std::unordered_map< uint64_t, uint64_t > > &inputs
    ) {
        const SweepSettings settings;
        SweepCounters counters;
        Sweeper sweeper(terms, settings, counters);
        return sweeper.Values(term, inputs);
    }

} // namespace cobra
