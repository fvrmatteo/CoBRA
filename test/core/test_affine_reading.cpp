#include "cobra/core/AffineReading.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/SignatureChecker.h"
#include <gtest/gtest.h>
#include <random>

namespace cobra {

    namespace {

        std::vector< std::unique_ptr< Expr > > Leaves(uint32_t count) {
            std::vector< std::unique_ptr< Expr > > leaves;
            for (uint32_t index = 0; index < count; ++index) {
                leaves.push_back(Expr::Variable(index));
            }
            return leaves;
        }

        // A leaf of `width` bits held zero-extended, as a tree reads a narrow leaf.
        std::unique_ptr< Expr > Narrow(uint32_t index, uint32_t width) {
            return Expr::BitwiseAnd(Expr::Variable(index), Expr::Constant(Bitmask(width)));
        }

        // Whether `answer` computes `expr` on the bits of `demanded`, on random inputs.
        void ExpectSameFunction(
            const Expr &expr, const Expr &answer, uint32_t vars, uint32_t bitwidth, uint64_t demanded
        ) {
            std::mt19937_64 random(17);
            for (int probe = 0; probe < 2048; ++probe) {
                std::vector< uint64_t > inputs(vars);
                for (auto &input : inputs) {
                    input = probe < 16 ? static_cast< uint64_t >(probe) : random();
                }
                ASSERT_EQ(
                    EvalExpr(expr, inputs, bitwidth) & demanded, EvalExpr(answer, inputs, bitwidth) & demanded
                );
            }
        }

        size_t Nodes(const Expr &expr) {
            size_t count = 1;
            for (const auto &child : expr.children) {
                count += Nodes(*child);
            }
            return count;
        }

    } // namespace

    // `(x + ~y - w + K) - ((2 * (x + ~y - w) + 2) & (2K - 2))`: an xor of the sum
    // `x - y - w` with `K - 1`, written through the identity
    // `a ^ b = a + b - 2 * (a & b)`. The constant xored in is no constant of the
    // tree, and the semilinear reading holds `x + ~y - w` under the `&` as an
    // atom.
    TEST(AffineReading, ReadsAnXorEncodedSum) {
        const uint64_t k = 0x6A09E667F3BCC908ULL;
        const auto sum   = [] {
            return Expr::Add(
                Expr::Add(Expr::Variable(0), Expr::BitwiseNot(Expr::Variable(1))),
                Expr::Negate(Expr::Variable(2))
            );
        };
        auto tree = Expr::Add(
            Expr::Add(sum(), Expr::Constant(k)),
            Expr::Negate(Expr::BitwiseAnd(
                Expr::Add(Expr::Mul(Expr::Constant(2), sum()), Expr::Constant(2)), Expr::Constant(2 * k - 2)
            ))
        );
        auto answer = ReadAsAffineOfLeaves(*tree, Leaves(3), 64, UINT64_MAX);
        ASSERT_NE(answer, nullptr);
        EXPECT_EQ(Render(*answer, { "x", "y", "w" }), "x + -y + -w ^ 7640891576956012807");
        ExpectSameFunction(*tree, *answer, 3, 64, UINT64_MAX);
    }

    // Two halves of a word, each xored with a constant, packed with a shift and
    // a disjoint or, and the word xored with a third constant that cancels the
    // other two: `0x10000 * zext(a) + zext(b)`.
    TEST(AffineReading, ReadsAPackedWordAsASum) {
        auto high = Expr::Mul(
            Expr::Constant(0x10000),
            Expr::BitwiseAnd(Expr::BitwiseXor(Narrow(0, 16), Expr::Constant(0x1F2E)), Expr::Constant(0xFFFF))
        );
        auto low  = Expr::BitwiseAnd(Expr::BitwiseXor(Narrow(1, 16), Expr::Constant(0x3D4C)), Expr::Constant(0xFFFF));
        auto tree = Expr::BitwiseXor(Expr::BitwiseOr(std::move(high), std::move(low)), Expr::Constant(0x1F2E3D4C));
        std::vector< std::unique_ptr< Expr > > leaves;
        leaves.push_back(Narrow(0, 16));
        leaves.push_back(Narrow(1, 16));
        auto answer = ReadAsAffineOfLeaves(*tree, leaves, 32, Bitmask(32));
        ASSERT_NE(answer, nullptr);
        EXPECT_EQ(Render(*answer, { "a", "b" }, 32), "65536 * (a & 65535) + (b & 65535)");
        ExpectSameFunction(*tree, *answer, 2, 32, Bitmask(32));
    }

    // A narrow leaf's sign extension written out by its sign bit:
    // `(-(x >> 31) | 0x7FFFFFFF) & (x | 0xFFFFFFFF00000000)` over a 32-bit `x`.
    // An affine function of the leaf and its top bit, and that one is the
    // `(x ^ 2^31) - 2^31` a sign extension is.
    TEST(AffineReading, ReadsASignExtensionThroughTheTopBit) {
        auto tree = Expr::BitwiseAnd(
            Expr::BitwiseOr(Expr::Negate(Expr::LogicalShr(Narrow(0, 32), 31)), Expr::Constant(0x7FFFFFFF)),
            Expr::BitwiseOr(Narrow(0, 32), Expr::Constant(0xFFFFFFFF00000000ULL))
        );
        std::vector< std::unique_ptr< Expr > > leaves;
        leaves.push_back(Narrow(0, 32));
        auto answer = ReadAsAffineOfLeaves(*tree, leaves, 64, UINT64_MAX);
        ASSERT_NE(answer, nullptr);
        EXPECT_EQ(Render(*answer, { "x" }), "(x & 4294967295 ^ 2147483648) + -2147483648");
        ExpectSameFunction(*tree, *answer, 1, 64, UINT64_MAX);
    }

    // Affine only on the bits the readers look at: above the low sixteen the
    // tree adds a product, which a reader of the low sixteen never sees.
    TEST(AffineReading, FitsOnTheDemandedBitsOnly) {
        auto tree = Expr::Add(
            Expr::Add(Expr::Add(Expr::Variable(0), Expr::Variable(1)), Expr::Constant(7)),
            Expr::Mul(Expr::Constant(0x10000), Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1)))
        );
        EXPECT_EQ(ReadAsAffineOfLeaves(*tree, Leaves(2), 64, UINT64_MAX), nullptr);
        auto answer = ReadAsAffineOfLeaves(*tree, Leaves(2), 64, 0xFFFF);
        ASSERT_NE(answer, nullptr);
        EXPECT_EQ(Render(*answer, { "x", "y" }), "x + y + 7");
        ExpectSameFunction(*tree, *answer, 2, 64, 0xFFFF);
    }

    // A tree that is constant on the demanded bits is that constant.
    TEST(AffineReading, ReadsAConstant) {
        // `(x | 1) & 1` and the low bit of `2x`, on the low bit.
        auto tree = Expr::Add(
            Expr::BitwiseAnd(Expr::BitwiseOr(Expr::Variable(0), Expr::Constant(1)), Expr::Constant(1)),
            Expr::Mul(Expr::Constant(2), Expr::Variable(1))
        );
        auto answer = ReadAsAffineOfLeaves(*tree, Leaves(2), 64, 1);
        ASSERT_NE(answer, nullptr);
        ASSERT_EQ(answer->kind, Expr::Kind::kConstant);
        EXPECT_EQ(answer->constant_val & 1, 1u);
    }

    // A bitwise function of two leaves is no affine function of them.
    TEST(AffineReading, TurnsDownWhatIsNotAffine) {
        auto tree = Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1));
        EXPECT_EQ(ReadAsAffineOfLeaves(*tree, Leaves(2), 64, UINT64_MAX), nullptr);
        auto product = Expr::Mul(Expr::Variable(0), Expr::Variable(1));
        EXPECT_EQ(ReadAsAffineOfLeaves(*product, Leaves(2), 64, UINT64_MAX), nullptr);
    }

    // Of the ways to write the answer, the cheaper: `A ^ C` and `~A ^ ~C` are the
    // same function, and the sum with fewer negations is the one returned.
    TEST(AffineReading, PrefersTheCheaperComplement) {
        // `(-1 - x - y) ^ ~K` is `(x + y) ^ K`.
        const uint64_t k = 0x1234567890ABCDEFULL;
        auto tree        = Expr::BitwiseXor(
            Expr::Add(
                Expr::Add(Expr::Constant(UINT64_MAX), Expr::Negate(Expr::Variable(0))), Expr::Negate(Expr::Variable(1))
            ),
            Expr::Constant(~k)
        );
        auto answer = ReadAsAffineOfLeaves(*tree, Leaves(2), 64, UINT64_MAX);
        ASSERT_NE(answer, nullptr);
        EXPECT_EQ(Render(*answer, { "x", "y" }), "x + y ^ 1311768467294899695");
        EXPECT_LE(Nodes(*answer), Nodes(*tree));
        ExpectSameFunction(*tree, *answer, 2, 64, UINT64_MAX);
    }

} // namespace cobra
