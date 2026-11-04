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
struct Decision {
    std::vector<int> fromHouse;   // take the house's placement
    std::vector<int> keepLocal;   // the house never had it: keep ours, and push
    std::vector<int> removed;     // the house had it and no longer does: removed elsewhere
    bool push() const { return !keepLocal.empty(); }
};
inline Decision decide(const std::set<int>& local, const std::set<int>& house, const std::set<int>& known) {
    Decision d;
    for (int id : local) {
        if (house.count(id)) d.fromHouse.push_back(id);
        else if (known.count(id)) d.removed.push_back(id);
        else d.keepLocal.push_back(id);
    }
    for (int id : house) if (!local.count(id)) d.fromHouse.push_back(id);
    return d;
}
}  // namespace housemerge
