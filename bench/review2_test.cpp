// Review, part 2 (work-k5, 2026-11-04): MULTIROOM doorway learning, the depth-wall repair that skipped forever, and
// Space Setup re-runs that silently changed the house frame. Each case fails on the old rule.
//   g++ -std=c++17 -I room_lab bench/review2_test.cpp && ./a.out
#include "rooms.h"
#include "placement_watch.h"
#include <cstdio>
static int fails = 0;
static void check(bool c, const char* m) { if (!c) { printf("FAIL %s\n", m); fails++; } }
using namespace rooms;

int main() {
    // 1. MULTIROOM: r1 (0..4 x 0..5) and r2 (5..8 x 0..3), both placed, door between them at x 4..5, z 1. The user stands
    // in r1 (x 1, z 4) when r1's anchors drop out (unplaced): the tracker moves to r2 (the only placed room), and used to
    // record that as a doorway crossing 4 m from any door, dragging the learned door off.
    {   Room r1, r2; r1.id = "r1"; r2.id = "r2"; r1.hasBox = r2.hasBox = true;
        r1.x0 = 0; r1.x1 = 4; r1.z0 = 0; r1.z1 = 5; r2.x0 = 5; r2.x1 = 8; r2.z0 = 0; r2.z1 = 3;
        Placed p1, p2; p1.id = "r1"; p2.id = "r2"; p1.ok = p2.ok = true; p1.frame.ok = p2.frame.ok = true;
        p1.inLocal = p2.inLocal = Pose{{0, 0, 0, 1}, {0, 0, 0}};   // both in one frame for the test
        std::vector<Room> rs = {r1, r2};
        Tracker T; std::vector<Placed> ps = {p1, p2};
        T.update(rs, ps, {1, 1.6f, 4}); check(T.current == "r1", "starts in r1");
        T.update(rs, ps, {4.5f, 1.6f, 1}); T.update(rs, ps, {5.6f, 1.6f, 1});   // a real walk through the door
        check(T.current == "r2" && T.crossings.size() == 1, "a real crossing at the door is recorded");
        T.update(rs, ps, {4.5f, 1.6f, 1}); T.update(rs, ps, {3.0f, 1.6f, 1}); T.update(rs, ps, {1, 1.6f, 4});
        size_t before = T.crossings.size();
        ps[0].ok = false; ps[0].frame.ok = false;                         // r1's anchors drop out
        T.update(rs, ps, {1, 1.6f, 4});
        bool bogus = T.crossings.size() > before;
        check(!bogus, "losing r1's anchors in the middle of r1 is not a doorway crossing");
        Learner L; for (auto& c : T.crossings) L.crossing(c, ps.size() ? std::vector<Placed>{p1, p2} : ps);
        V3 d = L.doors[{"r1", "r2"}].sum * (1.0f / std::max(1, L.doors[{"r1", "r2"}].n));
        check(std::fabs(d.x - 4.5f) < 1.0f && std::fabs(d.z - 1) < 0.5f, "the learned door stays at the door");
        printf("  learned door at x %.2f z %.2f from %d crossings (door at x 4..5, z 1)\n", d.x, d.z, L.doors[{"r1", "r2"}].n);
    }
    // 2. The depth-wall repair: a stale cached house (0 matches) must not end it for good.
    {   using pwatch::Repair; using pwatch::repairVerdict;
        bool oldDone = true;   // old: depthwalls-repaired written on the first try, whatever the result
        check(oldDone && repairVerdict(0, 0, false, 1) == Repair::Wait, "0 matches on the first document: wait and retry (the old code gave up for good)");
        check(repairVerdict(3, 3, true, 2) == Repair::Apply, "the fetched house matches 3 walls: apply");
        check(repairVerdict(3, 1, true, 4) == Repair::GiveUp, "matches that keep disagreeing: give up, visibly");
        check(repairVerdict(0, 0, false, 4) == Repair::Nothing, "a house with no depth walls at all: nothing to repair");
    }
    // 3. Space Setup re-run: the house stores 6 Meta planes, none located now, 5 new ones located.
    {   using pwatch::sceneChanged;
        check(sceneChanged(6, 0, 5, 12), "new uuids for every plane: detected");
        check(!sceneChanged(6, 0, 5, 3), "not in the first seconds (Meta still locating)");
        check(!sceneChanged(6, 1, 5, 30), "one stored plane still seen: the same scene");
        check(!sceneChanged(0, 0, 5, 30), "a house with no stored planes yet (first commit): nothing to compare");
        check(!sceneChanged(6, 0, 0, 30), "nothing located at all (dark room, no relocalisation yet): not a re-setup");
    }
    printf("review 2: %s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
