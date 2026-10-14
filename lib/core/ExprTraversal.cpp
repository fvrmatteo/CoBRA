#include "cobra/core/ExprTraversal.h"

#include <cstddef>

namespace cobra {

    size_t CountNodes(const Expr &root) {
        size_t count = 0;
        std::vector< const Expr * > worklist{ &root };
        while (!worklist.empty()) {
            const Expr *node = worklist.back();
            worklist.pop_back();
            ++count;
            for (const auto &child : node->children) {
                if (child) { worklist.push_back(child.get()); }
            }
        }
        return count;
    }

} // namespace cobra
