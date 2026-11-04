// Placement / house review (work-k5, 2026-11-04): one failing case per real issue, run on the old rule and the fixed one.
//   g++ -std=c++17 -I room_lab bench/placement_review_test.cpp && ./a.out
#include "housemerge.h"
#include "stagerebase.h"
#include <cmath>
#include <cstdio>
static int fails = 0;
static void check(bool c, const char* m) { if (!c) { printf("FAIL %s\n", m); fails++; } }
using wallfit::Pose;

// The rebase vote plus the 2 s cache refresh, frame by frame, as updatePlacementGuard runs them. hold = the fix.
struct Guard {
    stagerebase::State s; std::map<std::string, Pose> cache; float lastRefresh = -10; bool hold;
    Pose applied{{0, 0, 0, 1}, {0, 0, 0}}; bool rebased = false;
    void frame(float t, const std::map<std::string, Pose>& real) {
        auto o = stagerebase::step(s, t, real, cache);
        if (o.rebase) { for (auto& [u, c] : cache) c = anchors::compose(o.T, c); applied = o.T; rebased = true; }
        if (t - lastRefresh >= 2) {
            for (auto& [u, r] : real) { auto it = cache.find(u); if (it == cache.end() || !hold || stagerebase::refreshAllowed(s)) cache[u] = r; }
            lastRefresh = t;
        }
    }
};

int main() {
    // 1. A screen another device just added (room.sh adds 9) must not be deleted by a local save before we applied it.
    {   std::set<int> local = {2, 3, 4}, house = {2, 3, 4, 9}, known = {2, 3, 4};
        std::set<int> oldRule; for (int id : house) if (!local.count(id)) oldRule.insert(id);  // houseQueueScreens before
        check(oldRule.count(9) == 1, "old rule: a local save deletes screen 9 (the bug reproduced)");
        check(housemerge::deletions(local, house, known).empty(), "fix: 9 is not ours to delete before we have seen it");
        std::set<int> afterDelete = {2, 4};
        check(housemerge::deletions(afterDelete, house, known) == std::set<int>({3}), "fix: a screen deleted on the headset is still deleted in the house");
    }
    // 2. A house screen whose wall the headset cannot place yet must not be dropped as "removed elsewhere".
    {   std::set<int> local = {4, 5}, houseResolved = {5}, houseAll = {4, 5}, known = {4, 5};
        auto oldD = housemerge::decide(local, houseResolved, known);   // houseUpdate skipped screen 4: its wall did not map
        check(oldD.removed.size() == 1 && oldD.removed[0] == 4, "old: screen 4 dropped because its wall did not resolve (the bug reproduced)");
        auto d = housemerge::decide(local, houseAll, known, {4});
        check(d.removed.empty() && d.keepAsIs == std::vector<int>({4}) && d.fromHouse == std::vector<int>({5}) && !d.push(), "fix: 4 kept as it is, 5 from the house, nothing pushed");
        auto d2 = housemerge::decide({5}, houseAll, known, {4});
        check(d2.fromHouse == std::vector<int>({5}) && d2.removed.empty(), "fix: an unplaceable house screen we lack is left for later, not added");
        check(housemerge::deletions({5}, houseAll, {5}).empty(), "fix: and it is never deleted by our push");
    }
    // 3. STAGE moved 1.2 m; wall A is located at 1 s, wall B at 3 s. The vote must rebase.
    {   Pose T{{0, 0.0436f, 0, 0.999f}, {1.2f, 0, -0.3f}};  // 5 deg yaw + 1.24 m
        T = stagerebase::unit(T);
        std::map<std::string, Pose> cache0 = {{"A", {{0, 0, 0, 1}, {2, 1.2f, -1}}}, {"B", {{0, 0.7071f, 0, 0.7071f}, {-1, 1.2f, 0.5f}}}, {"C", {{0, 0, 0, 1}, {0, 1.2f, 3}}}};
        for (bool hold : {false, true}) {
            Guard g; g.hold = hold; g.cache = cache0;
            for (float t = 0; t < 10; t += 0.1f) {
                std::map<std::string, Pose> real;
                if (t >= 1) real["A"] = anchors::compose(T, cache0["A"]);
                if (t >= 3) real["B"] = anchors::compose(T, cache0["B"]);
                g.frame(t, real);
            }
            Pose c = g.cache["C"], want = anchors::compose(T, cache0["C"]);  // C, never seen: a stand-in / depth wall's spot
            float err = wallfit::len(c.p - want.p);
            printf("  STAGE moved 1.24 m, walls seen at 1 s and 3 s, %s: rebased %s, unseen wall off by %.2f m\n", hold ? "fixed" : "old", g.rebased ? "yes" : "no", err);
            if (!hold) check(!g.rebased && err > 1.0f, "old: the refresh poisons the vote, nothing rebased (the bug reproduced)");
            else check(g.rebased && err < 0.01f, "fix: rebased, the unseen wall's spot follows STAGE");
        }
        // And a single wall that moved on its own (no STAGE move) after the wait: one estimate rebases (kept as before;
        // see the review notes: it is a risk we keep, a lone wall after 5 s is usually the whole story).
        Guard g; g.hold = true; g.cache = cache0;
        for (float t = 0; t < 8; t += 0.1f) { std::map<std::string, Pose> real; real["A"] = cache0["A"]; real["B"] = cache0["B"]; g.frame(t, real); }
        check(!g.rebased && g.s.decided, "no move: decided, no rebase, refresh resumes");
    }
    // 4. Edits queued but not yet in house-pending.json when the app stopped: push local first at the next start.
    check(housemerge::pushFirst(false, false) && housemerge::pushFirst(true, true) && !housemerge::pushFirst(true, false), "push first when never pushed, or when the last run left unsent edits");
    printf("placement review: %s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
