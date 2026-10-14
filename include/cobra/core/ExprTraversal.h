#pragma once

#include "cobra/core/Expr.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace cobra {

    // Walks over an expression without recursion.
    //
    // Nearly every walk over an expression is a post-order walk, and written as
    // recursion each costs a stack frame per level of the tree. The depth is
    // set by the input rather than by anything CoBRA chooses: obfuscated code
    // nests as deeply as it likes, and a lowered MBA chain nests as far as the
    // rewriter's growth budget allows. A deep enough expression therefore
    // overflows the stack instead of returning an answer. These walk an
    // explicit worklist instead.

    // The number of nodes under `root`.
    size_t CountNodes(const Expr &root);

    // True when any node under `root` satisfies `pred`, stopping at the first.
    // The worklist is kept between calls for the same reason the fold's stacks
    // are, and trimmed back to where this call found it so one query may run
    // inside another.
    template < typename Pred >
    bool AnyNode(const Expr &root, Pred pred) {
        thread_local std::vector< const Expr * > worklist;
        const size_t base = worklist.size();
        worklist.push_back(&root);
        while (worklist.size() > base) {
            const Expr *node = worklist.back();
            worklist.pop_back();
            if (pred(*node)) {
                worklist.resize(base);
                return true;
            }
            for (const auto &child : node->children) {
                if (child) { worklist.push_back(child.get()); }
            }
        }
        return false;
    }

    // Fold every node bottom-up without recursion and without allocating.
    //
    // `combine(node, operands, count)` is called once per node, after its
    // children, with the results already produced for them, and returns the
    // result for this node. Nothing is materialised but a frame stack and an
    // operand stack, each as deep as the expression rather than as large as it,
    // and both are kept between calls so a walk of a small expression costs no
    // allocation at all once the buffers have grown. Recording where this call
    // found them and trimming back to it lets one fold run inside another -
    // of another instantiation: `combine` is handed a pointer into the operand
    // stack, which a fold of the same `T` and `Combine` started from inside it
    // could reallocate. No `combine` does; a debug build checks.
    template < typename T, typename Combine >
    T FoldPostOrder(const Expr &root, Combine combine) {
        struct Frame
        {
            const Expr *node;
            uint32_t next_child;
        };

        // One thread-local lookup each, not one per access.
        thread_local std::vector< Frame > frame_storage;
        thread_local std::vector< T > operand_storage;
        auto &frames              = frame_storage;
        auto &operands            = operand_storage;
        const size_t frame_base   = frames.size();
        const size_t operand_base = operands.size();

#ifndef NDEBUG
        thread_local bool combining = false;
        assert(!combining && "FoldPostOrder: a combine started a fold of its own instantiation");
#endif

        frames.push_back({ .node = &root, .next_child = 0 });
        while (frames.size() > frame_base) {
            Frame &frame         = frames.back();
            const auto &children = frame.node->children;
            if (frame.next_child < children.size()) {
                const Expr *child = children[frame.next_child++].get();
                // A missing child would shift every later one's position.
                assert(child != nullptr && "FoldPostOrder: null child");
                frames.push_back({ .node = child, .next_child = 0 });
                continue;
            }

            const size_t present = children.size();
            const size_t first   = operands.size() - present;
#ifndef NDEBUG
            combining = true;
#endif
            T value = combine(*frame.node, operands.data() + first, present);
#ifndef NDEBUG
            combining = false;
#endif
            // The node's own result takes the place of its children's.
            if (present > 0) {
                operands[first] = std::move(value);
                operands.resize(first + 1);
            } else {
                operands.push_back(std::move(value));
            }
            frames.pop_back();
        }

        T result = std::move(operands[operand_base]);
        operands.resize(operand_base);
        return result;
    }

} // namespace cobra
