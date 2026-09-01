#pragma once

#include "cobra/core/Classification.h"
#include "cobra/core/Evaluator.h"
#include "cobra/core/Expr.h"
#include "cobra/core/PassContract.h"
#include "cobra/core/Result.h"
#include "cobra/core/SimplifyOutcome.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cobra {

    // Families of simplification technique, so a caller that knows the shape of
    // its input can decline the ones that cannot apply to it. Each family maps
    // to a set of orchestrator passes that are then never scheduled.
    //
    // Disabling a family the input does contain does not produce a wrong
    // answer, only a missed simplification.
    enum class TechniqueFamily : uint32_t {
        kNone       = 0,
        kSemilinear = 1U << 0U,
        kPolynomial = 1U << 1U,
        kTemplate   = 1U << 2U,
        kBitwise    = 1U << 3U,
        kGhost      = 1U << 4U,
        kLifting    = 1U << 5U,
        kAll        = (1U << 6U) - 1U,
    };

    constexpr TechniqueFamily operator|(TechniqueFamily a, TechniqueFamily b) {
        return static_cast< TechniqueFamily >(
            static_cast< uint32_t >(a) | static_cast< uint32_t >(b)
        );
    }

    constexpr TechniqueFamily operator&(TechniqueFamily a, TechniqueFamily b) {
        return static_cast< TechniqueFamily >(
            static_cast< uint32_t >(a) & static_cast< uint32_t >(b)
        );
    }

    constexpr TechniqueFamily operator~(TechniqueFamily a) {
        return static_cast< TechniqueFamily >(
            ~static_cast< uint32_t >(a) & static_cast< uint32_t >(TechniqueFamily::kAll)
        );
    }

    constexpr bool HasFamily(TechniqueFamily set, TechniqueFamily one) {
        return (set & one) != TechniqueFamily::kNone;
    }

    struct Options
    {
        uint32_t bitwidth                 = 64;
        uint32_t max_vars                 = 16;
        bool spot_check                   = true;
        bool enable_bitwise_decomposition = true;
        StructuralFlag structural_flags   = static_cast< StructuralFlag >(0);
        TechniqueFamily enabled_families  = TechniqueFamily::kAll;
        Evaluator evaluator;
    };

    Result< SimplifyOutcome > Simplify(
        const std::vector< uint64_t > &sig, const std::vector< std::string > &vars,
        const Expr *input_expr, const Options &opts
    );

} // namespace cobra
