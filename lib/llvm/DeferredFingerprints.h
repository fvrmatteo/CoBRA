#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>

namespace cobra {

    // The fingerprints of rejected roots, held until every rung of each root's
    // ladder has decided (`CobraPassOptions::retry_undecided_roots`).
    //
    // A fingerprint tells later runs that the tree at a root was examined and
    // holds nothing, so they skip it. Written as soon as the full collection is
    // turned down, it also skips the slices, sum groups and cuts queued from
    // it - including any whose proof ran out of time or whose rewrite did not
    // fit this time, which a later run could still win. Held here, it is
    // written only once all of them decided.
    //
    // Candidates are named by their index in the run's queue. A candidate
    // queued from another belongs to that one's head: the full collection the
    // chain of rungs started from.
    class DeferredFingerprints
    {
      public:
        // `head`, a full collection, was turned down; its fingerprint waits
        // for the rungs queued from it.
        void Defer(size_t head) { pending_.try_emplace(head); }

        // `child` was queued from `parent`.
        void Queue(size_t child, size_t parent) {
            const size_t head = HeadOf(parent);
            head_of_[child]   = head;
            if (auto it = pending_.find(head); it != pending_.end()) {
                ++it->second.outstanding;
            }
        }

        // `index` was dealt with; `undecided` when it left its question for a
        // later run. Returns the head whose fingerprint is now to be written.
        std::optional< size_t > Finish(size_t index, bool undecided) {
            const size_t head = HeadOf(index);
            auto it           = pending_.find(head);
            if (it == pending_.end()) {
                return std::nullopt;
            }
            auto &state = it->second;
            if (index != head) {
                state.undecided = state.undecided || undecided;
                --state.outstanding;
            }
            if (state.outstanding != 0) {
                return std::nullopt;
            }
            const bool write = !state.undecided;
            pending_.erase(it);
            return write ? std::optional< size_t >(head) : std::nullopt;
        }

      private:
        size_t HeadOf(size_t index) const {
            const auto it = head_of_.find(index);
            return it == head_of_.end() ? index : it->second;
        }

        struct State
        {
            uint32_t outstanding = 0;
            bool undecided       = false;
        };

        std::unordered_map< size_t, size_t > head_of_;
        std::unordered_map< size_t, State > pending_;
    };

} // namespace cobra
