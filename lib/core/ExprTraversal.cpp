#include "cobra/core/ExprTraversal.h"

#include <cstddef>

namespace cobra {

    namespace {

        struct Frame
        {
            const Expr *node;
            uint32_t next_child;
        };

    } // namespace

    ExprPostOrder BuildPostOrder(const Expr &root) {
        ExprPostOrder order;

        // A node is emitted once every one of its children has been, so the
        // children's positions are already known when the parent is written and
        // the compressed rows can be filled as we go. Recording them here is
        // what keeps the traversal free of a side table mapping nodes back to
        // positions: these walks run on small expressions far more often than
        // on large ones, and a hash lookup per node costs more than the
        // recursion this replaces.
        std::vector< Frame > stack;
        std::vector< uint32_t > completed;
        stack.push_back({ .node = &root, .next_child = 0 });

        while (!stack.empty()) {
            Frame &frame = stack.back();
            const auto &children = frame.node->children;
            if (frame.next_child < children.size()) {
                const Expr *child = children[frame.next_child++].get();
                if (child != nullptr) {
                    stack.push_back({ .node = child, .next_child = 0 });
                }
                continue;
            }

            const auto emitted = static_cast< uint32_t >(order.nodes.size());
            order.child_start.push_back(static_cast< uint32_t >(order.child_index.size()));
            std::size_t present = 0;
            for (const auto &child : children) {
                if (child) { ++present; }
            }
            for (std::size_t k = completed.size() - present; k < completed.size(); ++k) {
                order.child_index.push_back(completed[k]);
            }
            completed.resize(completed.size() - present);
            order.nodes.push_back(frame.node);
            completed.push_back(emitted);
            stack.pop_back();
        }
        order.child_start.push_back(static_cast< uint32_t >(order.child_index.size()));
        return order;
    }

    size_t CountNodes(const Expr &root) {
        // Counting needs no order and no rows, only a worklist.
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
