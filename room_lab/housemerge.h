// HOUSE sync without losing screens (work-df, 2026-11-04: the house knew screens 1-3, the headset had 1-7; adopting
// the house's list would have removed 4-7). decide(): a screen the house has is taken from the house (so moves made
// elsewhere, e.g. work-42's room.sh, show live); a local screen the house does not have is kept and pushed, unless the
// house knew it before (it was in a rev we applied: then it was removed elsewhere and goes); a house screen we do not
// have yet is added. `known` is the set of extra screen ids seen in applied revs (files/house-known). The first sync
// after HOUSE is turned on pushes everything local before adopting anything (room_lab: files/house-pushed). GL-free;
// bench/housemerge_test.cpp.
#pragma once
#include <set>
#include <vector>

namespace housemerge {
// Review fixes (work-k5, 2026-11-04): `unresolved` are house screens whose wall the headset cannot map to a scene wall
// right now (no scene uuid yet, a wall another device added): they are still the house's screens, so a local copy is
// kept as it is (not taken, not pushed) instead of being dropped as "removed elsewhere", and one we do not have is
// left for later. deletions(): the only house screens a push may delete are ones the headset has seen in an applied
// rev and no longer has locally; a screen added elsewhere that we have not applied yet is never deleted.
struct Decision {
    std::vector<int> fromHouse;   // take the house's placement
    std::vector<int> keepLocal;   // the house never had it: keep ours, and push
    std::vector<int> keepAsIs;    // the house has it but its wall cannot be placed here yet: keep ours, do not push
    std::vector<int> removed;     // the house had it and no longer does: removed elsewhere
    bool push() const { return !keepLocal.empty(); }
};
inline Decision decide(const std::set<int>& local, const std::set<int>& house, const std::set<int>& known, const std::set<int>& unresolved = {}) {
    Decision d;
    for (int id : local) {
        if (house.count(id)) (unresolved.count(id) ? d.keepAsIs : d.fromHouse).push_back(id);
        else if (known.count(id)) d.removed.push_back(id);
        else d.keepLocal.push_back(id);
    }
    for (int id : house) if (!local.count(id) && !unresolved.count(id)) d.fromHouse.push_back(id);
    return d;
}
inline std::set<int> deletions(const std::set<int>& local, const std::set<int>& house, const std::set<int>& known) {
    std::set<int> del; for (int id : house) if (known.count(id) && !local.count(id)) del.insert(id);
    return del;
}
// At start: push the local screens before adopting the house when HOUSE was never pushed, or when the last run queued
// screen edits the sync thread had not yet written to house-pending.json (files/house-unsent): otherwise the cached
// house, older than screens.json, would move those screens back.
inline bool pushFirst(bool everPushed, bool unsent) { return !everPushed || unsent; }
}  // namespace housemerge
