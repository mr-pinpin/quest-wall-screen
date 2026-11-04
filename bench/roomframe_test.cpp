// The commit frame (room_lab/roomframe.h). A known room: Meta's walls and floor stored in the house in the room frame
// (as the first commit wrote them). A later session: LOCAL elsewhere, and our anchors drifted 10 cm down and 0.5 deg
// against Meta's scene (what the 02:15 / 03:03 / 09:32 commits show). Walls fitted from depth (1 cm noise) committed
// through the chosen frame must land on Meta's within 2 cm and the floor within 1 cm; through the anchors' frame (the
// old way) the floor is off by the drift, and floorCheck refuses that commit.
#include "roomframe.h"
#include <cstdio>
#include <random>
using namespace roomframe;
using anchors::yawQuat; using anchors::compose; using anchors::inverse;
static int fails = 0;
static void check(bool c, const char* m) { if (!c) { printf("FAIL %s\n", m); fails++; } }
int main() {
    std::mt19937 rng(5); std::normal_distribution<float> N(0, 0.004f);
    // Meta's planes in the room frame (centre poses): 4 walls of a 4 x 5 m room and the floor (normal up).
    struct P { std::string id; Pose inRoom; };
    const float s2 = std::sqrt(0.5f);
    std::vector<P> meta = {
        {"wN", {yawQuat(0), {0, 0, -2.5f}}}, {"wS", {yawQuat(3.14159f), {0, 0, 2.5f}}},
        {"wE", {yawQuat(-1.5708f), {2, 0, 0}}}, {"wW", {yawQuat(1.5708f), {-2, 0, 0}}},
        {"floor", {{-s2, 0, 0, s2}, {0, -1.4f, 0}}}};
    std::vector<anchors::Stored> stored; for (auto& m : meta) stored.push_back({m.id, m.inRoom});
    // This session: the room sits here in LOCAL (truth); Meta locates its planes there (4 mm noise).
    Pose roomTrue{yawQuat(0.8f), {1.3f, 0.35f, -0.6f}};
    std::vector<anchors::Seen> seen;
    for (auto& m : meta) { Pose l = compose(roomTrue, m.inRoom); l.p = l.p + V3{N(rng), N(rng), N(rng)}; seen.push_back({m.id, l}); }
    // Our anchors' frame drifted: 10 cm down and 0.5 deg of yaw.
    anchors::Frame af; af.ok = true; af.used = 3; af.roomInLocal = compose(roomTrue, Pose{yawQuat(0.0087f), {0, -0.10f, 0}});
    Result r = choose(stored, seen, af);
    check(r.ok && r.source == "scene" && r.used == 5, "frame from Meta's scene planes");
    check(r.anchorsVsSceneM > 0.09f && r.anchorsVsSceneM < 0.11f, "drift between anchors and scene reported");
    // Depth fits in LOCAL (1 cm noise): the north wall and the floor. Committed through the chosen frame.
    Pose fitN = compose(roomTrue, meta[0].inRoom); fitN.p = fitN.p + V3{0.01f, -0.006f, 0.008f};
    Pose fitF = compose(roomTrue, meta[4].inRoom); fitF.p = fitF.p + V3{0.2f, 0.004f, -0.3f};
    Pose cN = toRoom(r.roomInLocal, fitN), cF = toRoom(r.roomInLocal, fitF);
    check(std::fabs(cN.p.z - (-2.5f)) < 0.02f, "fitted wall lands on Meta's within 2 cm");
    check(std::fabs(cF.p.y - (-1.4f)) < 0.01f, "fitted floor lands on Meta's within 1 cm");
    // The old way (anchors' frame): the floor is 10 cm off.
    Pose oF = toRoom(af.roomInLocal, fitF);
    check(std::fabs(oF.p.y - (-1.4f)) > 0.08f, "control: the anchors' frame puts the floor 10 cm off");
    // Floor check in LOCAL: Meta's floor vs the fitted one (a table top at 0.75 m is ignored: lowest wins).
    float metaY = compose(roomTrue, meta[4].inRoom).p.y;
    check(floorCheck({{fitF.p.y, 8}, {fitF.p.y + 0.75f, 2}}, metaY, true).ok, "floor check passes the right floor");
    check(!floorCheck({{fitF.p.y + 0.12f, 6}}, metaY, true).ok, "floor 12 cm off: commit refused");
    check(!floorCheck({{fitF.p.y - 0.09f, 6}}, metaY, true).ok, "floor 9 cm below Meta's: refused");
    FloorCheck bed = floorCheck({{fitF.p.y + 0.21f, 3}}, metaY, true); check(bed.ok && !bed.known, "the bed 21 cm up with the floor unseen: no verdict");
    check(floorCheck({{fitF.p.y + 0.10f, 0.6f}}, metaY, true).ok, "a small low shelf is not the floor");
    check(floorCheck({}, metaY, true).ok && floorCheck({{fitF.p.y + 0.3f, 5}}, 0, false).ok, "unknown: no verdict");
    // Fallbacks: one scene plane is not enough -> anchors; none -> anchors; neither -> not ok.
    r = choose(stored, {seen[0]}, af); check(r.ok && r.source == "anchors", "one scene plane: anchors");
    r = choose({}, seen, af); check(r.ok && r.source == "anchors", "nothing stored yet (first commit): anchors");
    anchors::Frame none; r = choose({}, {}, none); check(!r.ok, "nothing at all: no frame");
    // A scene plane Meta moved (Space Setup redone for one wall) is outvoted.
    std::vector<anchors::Seen> moved = seen; moved[2].inLocal.p = moved[2].inLocal.p + V3{0.3f, 0, 0};
    r = choose(stored, moved, af); check(r.source == "scene" && r.used == 4 && wallfit::len(r.roomInLocal.p - roomTrue.p) < 0.01f, "a moved plane is outvoted");
    printf("roomframe: %s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
