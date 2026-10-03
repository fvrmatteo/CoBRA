#include "cobra/core/BitWidth.h"
#include "cobra/core/Expr.h"
#include "cobra/core/RingProof.h"
#include "cobra/core/SignatureChecker.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <random>
#include <vector>

using namespace cobra;

namespace {

    using E = std::unique_ptr< Expr >;

    E V(uint32_t i) { return Expr::Variable(i); }
    E C(uint64_t c) { return Expr::Constant(c); }
    E Add(E a, E b) { return Expr::Add(std::move(a), std::move(b)); }
    E Sub(E a, E b) { return Expr::Add(std::move(a), Expr::Negate(std::move(b))); }
    E Mul(E a, E b) { return Expr::Mul(std::move(a), std::move(b)); }
    E Xor(E a, E b) { return Expr::BitwiseXor(std::move(a), std::move(b)); }
    E And(E a, E b) { return Expr::BitwiseAnd(std::move(a), std::move(b)); }
    E Or(E a, E b) { return Expr::BitwiseOr(std::move(a), std::move(b)); }
    E Not(E a) { return Expr::BitwiseNot(std::move(a)); }
    E Neg(E a) { return Expr::Negate(std::move(a)); }
    E Shr(E a, uint64_t k) { return Expr::LogicalShr(std::move(a), k); }
    E Slt(E a, E b) { return Expr::CmpSlt(std::move(a), std::move(b)); }
    E Ult(E a, E b) { return Expr::CmpUlt(std::move(a), std::move(b)); }
    E Eq(E a, E b) { return Expr::CmpEq(std::move(a), std::move(b)); }

    // `c ? a : b` the way the detector spells a select: `b ^ ((a ^ b) & -c)`.
    E Select(E c, E a, E b) {
        E b2 = CloneExpr(*b);
        return Xor(std::move(b), And(Xor(std::move(a), std::move(b2)), Neg(std::move(c))));
    }

    // Exhaustive over two variables at 8 bits.
    bool EqualEverywhere(const Expr &a, const Expr &b) {
        std::vector< uint64_t > values(2);
        for (uint64_t x = 0; x < 256; ++x) {
            for (uint64_t y = 0; y < 256; ++y) {
                values[0] = x;
                values[1] = y;
                if (EvalExpr(a, values, 8) != EvalExpr(b, values, 8)) {
                    return false;
                }
            }
        }
        return true;
    }

    constexpr uint64_t kOffset = 0xcbf29ce484222325ULL;
    constexpr uint64_t kPrime  = 0x100000001b3ULL;

} // namespace

TEST(RingProof, XorWithAComplementedConstantTimesAnOddConstant) {
    // ((v0 ^ ~K) + 1) * P == (-P) * (K ^ v0)
    E lhs = Mul(Add(Xor(V(0), C(~kOffset)), C(1)), C(kPrime));
    E rhs = Mul(C(0 - kPrime), Xor(C(kOffset), V(0)));
    EXPECT_TRUE(ProveEqualByRingNormalForm(*lhs, *rhs, 64));
}

TEST(RingProof, ProductBehindASignConditionalNegation) {
    // c = v0 <s 0; d = c ? -v1 : v1; e = d * P
    // (c ? (d * -P) ^ e : 0) ^ e == v1 * P
    const auto c = [] { return Slt(V(0), C(0)); };
    const auto d = [&] { return Select(c(), Neg(V(1)), V(1)); };
    const auto e = [&] { return Mul(d(), C(kPrime)); };
    E lhs = Xor(Select(c(), Xor(Mul(d(), C(0 - kPrime)), e()), C(0)), e());
    E rhs = Mul(V(1), C(kPrime));
    EXPECT_TRUE(ProveEqualByRingNormalForm(*lhs, *rhs, 64));
}

TEST(RingProof, DoesNotProveWhatOnlyLooksAlike) {
    // A product is not its sum, a xor is not an add, and a comparison's value
    // matters where the two sides use it differently.
    EXPECT_FALSE(ProveEqualByRingNormalForm(*Mul(V(0), C(2)), *Xor(V(0), V(0)), 64));
    EXPECT_FALSE(ProveEqualByRingNormalForm(*Xor(V(0), C(5)), *Add(V(0), C(5)), 64));
    EXPECT_FALSE(ProveEqualByRingNormalForm(*And(V(0), V(1)), *Or(V(0), V(1)), 64));
    EXPECT_FALSE(ProveEqualByRingNormalForm(*Mul(Slt(V(0), C(0)), V(1)), *C(0), 64));
    EXPECT_FALSE(
        ProveEqualByRingNormalForm(*Select(Eq(V(0), C(7)), V(1), C(3)), *C(3), 64)
    );
    // Shifts are not divisions in the ring.
    EXPECT_FALSE(ProveEqualByRingNormalForm(*Shr(Mul(V(0), C(2)), 1), *V(0), 64));
    // The top-bit flip `a ^ c == ~(a ^ ~c)` must not lose the constant.
    EXPECT_FALSE(ProveEqualByRingNormalForm(*Xor(V(0), C(~kOffset)), *Not(Xor(V(0), C(~kOffset))), 64));
}

TEST(RingProof, ProvesTheIdentitiesItIsMeantTo) {
    EXPECT_TRUE(ProveEqualByRingNormalForm(*Add(Not(V(0)), C(1)), *Neg(V(0)), 64));
    EXPECT_TRUE(ProveEqualByRingNormalForm(*Xor(Xor(V(0), V(1)), V(1)), *V(0), 64));
    EXPECT_TRUE(ProveEqualByRingNormalForm(*Xor(Not(V(0)), Not(V(1))), *Xor(V(1), V(0)), 64));
    EXPECT_TRUE(ProveEqualByRingNormalForm(*Mul(Neg(V(0)), Neg(V(1))), *Mul(V(1), V(0)), 64));
    EXPECT_TRUE(ProveEqualByRingNormalForm(*And(V(0), C(~uint64_t{ 0 })), *V(0), 64));
    EXPECT_TRUE(ProveEqualByRingNormalForm(*Or(And(V(0), V(1)), And(V(1), V(0))), *And(V(0), V(1)), 64));
    EXPECT_TRUE(ProveEqualByRingNormalForm(*Select(Ult(V(0), V(1)), V(1), V(1)), *V(1), 64));
}

namespace {

    // A random expression over two variables, comparisons and selects included.
    E Random(std::mt19937_64 &rng, int depth) {
        const auto pick = [&](uint32_t n) { return static_cast< uint32_t >(rng() % n); };
        if (depth == 0 || pick(4) == 0) {
            switch (pick(3)) {
                case 0:
                    return V(0);
                case 1:
                    return V(1);
                default: {
                    static const uint64_t kConstants[] = { 0, 1, 2, 3, 0x7f, 0x80, 0xff, 0x55, 0xaa,
                                                           0xfe };
                    return C(kConstants[pick(10)]);
                }
            }
        }
        switch (pick(13)) {
            case 0:
                return Add(Random(rng, depth - 1), Random(rng, depth - 1));
            case 1:
                return Mul(Random(rng, depth - 1), Random(rng, depth - 1));
            case 2:
                return And(Random(rng, depth - 1), Random(rng, depth - 1));
            case 3:
                return Or(Random(rng, depth - 1), Random(rng, depth - 1));
            case 4:
                return Xor(Random(rng, depth - 1), Random(rng, depth - 1));
            case 5:
                return Not(Random(rng, depth - 1));
            case 6:
                return Neg(Random(rng, depth - 1));
            case 7:
                return Shr(Random(rng, depth - 1), pick(9));
            case 8:
                return Slt(Random(rng, depth - 1), Random(rng, depth - 1));
            case 9:
                return Ult(Random(rng, depth - 1), Random(rng, depth - 1));
            case 10:
                return Eq(Random(rng, depth - 1), Random(rng, depth - 1));
            case 11:
                return Select(Slt(V(pick(2)), C(0)), Random(rng, depth - 1), Random(rng, depth - 1));
            default:
                return Sub(Random(rng, depth - 1), Random(rng, depth - 1));
        }
    }

    // `expr` rewritten by identities that hold, so that the two are equal and
    // the prover has something true to find. Each rule is applied at random.
    E Rewrite(std::mt19937_64 &rng, const Expr &expr) {
        const auto child = [&](size_t i) { return Rewrite(rng, *expr.children[i]); };
        const bool fire  = rng() % 2 == 0;
        switch (expr.kind) {
            case Expr::Kind::kAdd:
                return fire ? Add(child(1), child(0)) : Add(child(0), child(1));
            case Expr::Kind::kMul:
                return fire ? Mul(Neg(child(1)), Neg(child(0))) : Mul(child(0), child(1));
            case Expr::Kind::kXor:
                return fire ? Xor(Not(child(1)), Not(child(0))) : Xor(child(0), child(1));
            case Expr::Kind::kAnd:
                return fire ? And(child(1), child(0)) : And(child(0), child(1));
            case Expr::Kind::kOr:
                return fire ? Or(child(1), child(0)) : Or(child(0), child(1));
            case Expr::Kind::kNot:
                return fire ? Sub(C(0xff), child(0)) : Not(child(0));
            case Expr::Kind::kNeg:
                return fire ? Add(Not(child(0)), C(1)) : Neg(child(0));
            case Expr::Kind::kShr:
                return Shr(child(0), expr.constant_val);
            case Expr::Kind::kCmpSlt:
                return Slt(child(0), child(1));
            case Expr::Kind::kCmpUlt:
                return Ult(child(0), child(1));
            case Expr::Kind::kCmpEq:
                return fire ? Eq(child(1), child(0)) : Eq(child(0), child(1));
            default:
                return CloneExpr(expr);
        }
    }

    // `expr` with one leaf nudged: usually a different function, and the
    // kind of near miss a careless normal form would call equal.
    E Mutate(std::mt19937_64 &rng, const Expr &expr) {
        E copy = CloneExpr(expr);
        std::vector< Expr * > leaves;
        std::vector< Expr * > stack{ copy.get() };
        while (!stack.empty()) {
            Expr *node = stack.back();
            stack.pop_back();
            if (node->children.empty()) {
                leaves.push_back(node);
            }
            for (auto &c : node->children) {
                stack.push_back(c.get());
            }
        }
        Expr *leaf = leaves[rng() % leaves.size()];
        if (leaf->kind == Expr::Kind::kConstant) {
            leaf->constant_val = (leaf->constant_val + 1 + rng() % 3) & 0xff;
        } else {
            leaf->var_index ^= 1;
        }
        return copy;
    }

} // namespace

// The property that matters: whatever the prover proves is true. Checked
// exhaustively at 8 bits on random pairs - unrelated, rewritten by true
// identities, and nudged into near misses.
TEST(RingProof, NeverProvesAFalseEquality) {
    // `RING_PROOF_FUZZ_ROUNDS=n` runs n times as many pairs, for a longer soak.
    const char *rounds_text = std::getenv("RING_PROOF_FUZZ_ROUNDS");
    const int rounds        = rounds_text != nullptr ? std::max(1, std::atoi(rounds_text)) : 1;
    std::mt19937_64 rng(20261003);
    int proved_rewrites = 0;
    int checked         = 0;
    for (int i = 0; i < 6000 * rounds; ++i) {
        E a = Random(rng, 4 + (i / 6000) % 2);
        E b;
        switch (i % 3) {
            case 0:
                b = Random(rng, 4);
                break;
            case 1:
                b = Rewrite(rng, *a);
                break;
            default:
                b = Mutate(rng, *Rewrite(rng, *a));
                break;
        }
        if (ProveEqualByRingNormalForm(*a, *b, 8)) {
            ++checked;
            ASSERT_TRUE(EqualEverywhere(*a, *b))
                << "proved but differs:\n  " << Render(*a, { "x", "y" }, 8) << "\n  "
                << Render(*b, { "x", "y" }, 8);
            proved_rewrites += i % 3 == 1 ? 1 : 0;
        }
    }
    // It must also be worth having: most true rewrites are proved.
    EXPECT_GT(proved_rewrites, 1500);
    EXPECT_GT(checked, 1500);
}
