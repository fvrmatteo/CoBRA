#include "CompetitionGroup.h"
#include "Orchestrator.h"

#include <cassert>

namespace cobra {

    GroupId CreateGroup(
        absl::flat_hash_map< GroupId, CompetitionGroup > &groups, GroupId &next_id,
        std::optional< ExprCost > baseline_cost
    ) {
        GroupId id = next_id++;
        CompetitionGroup group;
        group.open_handles  = 1;
        group.baseline_cost = std::move(baseline_cost);
        groups.emplace(id, std::move(group));
        return id;
    }

    bool SubmitCandidate(
        absl::flat_hash_map< GroupId, CompetitionGroup > &groups, GroupId group_id,
        CandidateRecord record
    ) {
        auto it = groups.find(group_id);
        // Parent group may already be resolved and erased in
        // fire-and-forget fanout — silently reject the submission.
        if (it == groups.end()) { return false; }
        auto &group = it->second;

        if (group.baseline_cost && !IsBetter(record.cost, *group.baseline_cost)) {
            return false;
        }

        if (!group.best) {
            group.best.emplace(std::move(record));
            return true;
        }

        if (IsBetter(record.cost, group.best->cost)) {
            group.best.emplace(std::move(record));
            return true;
        }

        return false;
    }

    bool
    AcquireHandle(absl::flat_hash_map< GroupId, CompetitionGroup > &groups, GroupId group_id) {
        auto it = groups.find(group_id);
        if (it == groups.end()) { return false; }
        ++it->second.open_handles;
        return true;
    }

    bool HasVerifiedCandidate(
        const absl::flat_hash_map< GroupId, CompetitionGroup > &groups, GroupId group_id,
        uint32_t max_weighted_size
    ) {
        auto it = groups.find(group_id);
        if (it == groups.end()) { return false; }
        return it->second.best.has_value()
            && it->second.best->verification == VerificationState::kVerified
            && it->second.best->cost.weighted_size <= max_weighted_size;
    }

    std::optional< WorkItem >
    ReleaseHandle(absl::flat_hash_map< GroupId, CompetitionGroup > &groups, GroupId group_id) {
        auto it = groups.find(group_id);
        // A child may finish after the parent group has already
        // resolved and been erased; treat the late release as a no-op.
        if (it == groups.end()) { return std::nullopt; }
        auto &group = it->second;
        // Defensive: a buggy caller that double-releases would
        // wrap open_handles past zero and wedge the group forever
        // (it would never reach 0 again). Treat double-release as a
        // no-op here so the bug is contained, and assert in debug
        // builds so we still notice during development.
        assert(group.open_handles > 0);
        if (group.open_handles == 0) { return std::nullopt; }

        --group.open_handles;
        if (group.open_handles > 0) { return std::nullopt; }

        WorkItem resolved;
        resolved.payload = CompetitionResolvedPayload{ .group_id = group_id };
        return resolved;
    }

} // namespace cobra
