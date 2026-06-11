#include "cobra/core/ExprUtils.h"

#include <gtest/gtest.h>

namespace cobra {

    TEST(ExprUtils, TryBuildVarSupportMapsSubset) {
        const std::vector< std::string > vars   = { "x", "y", "z" };
        const std::vector< std::string > subset = { "z", "x" };

        auto support = TryBuildVarSupport(vars, subset);

        ASSERT_TRUE(support.has_value());
        EXPECT_EQ(*support, (std::vector< uint32_t >{ 2, 0 }));
    }

    TEST(ExprUtils, TryBuildVarSupportRejectsUnknownVariable) {
        const std::vector< std::string > vars   = { "x", "y" };
        const std::vector< std::string > subset = { "y", "missing" };

        EXPECT_FALSE(TryBuildVarSupport(vars, subset).has_value());
        EXPECT_THROW((void) BuildVarSupport(vars, subset), std::out_of_range);
    }

} // namespace cobra
