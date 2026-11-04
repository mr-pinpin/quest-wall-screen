// MULTIROOM: rooms, links and the current room (room_lab/rooms.h). g++ -std=c++17 -I room_lab bench/rooms_test.cpp && ./a.out
// A simulated flat: room r1 (4 x 5 m) and room r2 (3 x 3 m) through a doorway, each with its own anchors in its own
// frame. Sessions put the flat somewhere in LOCAL; anchors locate with 4 mm noise, or not at all.
#include "rooms.h"
#include <cstdio>
#include <random>
using namespace rooms;
using anchors::yawQuat;
static int fails = 0;
static void check(bool c, const char* m) { if (!c) { printf("FAIL %s\n", m); fails++; } }
static bool near(const Pose& a, const Pose& b, float m = 0.02f, float deg = 1.0f) { return wallfit::len(a.p - b.p) < m && anchors::angleDeg(a.q, b.q) < deg; }

int main() {
    std::mt19937 rng(11); std::normal_distribution<float> noise(0, 0.004f);
    // The truth: r2's frame in r1's frame (rotated 90 deg, its origin 4.5 m along x), the doorway at x=4, z=1 in r1.
    const Pose r2InR1{yawQuat(1.5708f), {4.5f, 0, 1.0f}};
    Room r1; r1.id = "r1"; r1.anchors = {{"a1", {yawQuat(0), {0, 1, -2.5f}}}, {"a2", {yawQuat(1.57f), {-2, 1, 0}}}, {"a3", {yawQuat(3.14f), {0, 1, 2.5f}}}};
    boxFrom(r1, {{-2, 0, -2.5f}, {2, 0, -2.5f}, {2, 0, 2.5f}, {-2, 0, 2.5f}});      // walls: x -2..2, z -2.5..2.5
    Room r2; r2.id = "r2"; r2.anchors = {{"b1", {yawQuat(0), {0, 1, -1.5f}}}, {"b2", {yawQuat(-1.57f), {1.5f, 1, 0}}}};
    // r2 in its own frame: x -1.5..1.5, z -1.5..1.5 (its origin is 1.5 m past r1's wall at x=2 ... the box spans r1 x 3..6).
    boxFrom(r2, {{-1.5f, 0, -1.5f}, {1.5f, 0, 1.5f}});
    std::vector<Room> flat = {r1, r2};

    auto session = [&](Pose r1InLocal, bool seeR1, bool seeR2) {
        std::vector<anchors::Seen> s;
        auto add = [&](const Room& r, Pose rInLocal) { for (auto& a : r.anchors) { Pose l = compose(rInLocal, a.inRoom); l.p = l.p + V3{noise(rng), noise(rng), noise(rng)}; s.push_back({a.uuid, l}); } };
        if (seeR1) add(r1, r1InLocal);
        if (seeR2) add(r2, compose(r1InLocal, r2InR1));
        return s;
    };
    const Pose r1InLocal{yawQuat(-0.4f), {0.7f, -1.2f, 2.0f}};
    auto headAt = [&](V3 inR1) { return compose(r1InLocal, Pose{{0, 0, 0, 1}, inR1}).p; };

    // 1. No links yet, only r1's anchors: r1 placed, r2 not.
    auto ps = place(flat, {}, session(r1InLocal, true, false));
    check(find(ps, "r1")->ok && !find(ps, "r1")->viaLink && near(find(ps, "r1")->inLocal, r1InLocal), "r1 from its own anchors");
    check(!find(ps, "r2")->ok, "r2 unplaced without anchors or link");

    // 2. Both rooms' anchors located over several frames: the link is learned (b in a), within 2 cm / 1 deg.
    Learner L;
    for (int i = 0; i < 8; i++) L.sample(place(flat, {}, session(r1InLocal, true, true)));
    std::vector<std::string> changed; auto links = L.merge({}, &changed);
    check(links.size() == 1 && links[0].a == "r1" && links[0].b == "r2", "one link r1~r2");
    check(!links.empty() && near(links[0].bInA, r2InR1), "link pose = r2 in r1");
    check(changed.size() == 1 && changed[0] == "r1~r2", "new link reported as changed");
    // Fewer than minSamples: nothing learned.
    Learner few; for (int i = 0; i < 3; i++) few.sample(place(flat, {}, session(r1InLocal, true, true)));
    check(few.merge({}).empty(), "too few samples: no link");

    // 3. A later session in a different LOCAL frame, r2's anchors missing: r2 placed through the link.
    const Pose other{yawQuat(2.0f), {-3, 0.2f, 1}};
    ps = place(flat, links, session(other, true, false));
    const Placed* p2 = find(ps, "r2");
    check(p2->ok && p2->viaLink && p2->via == "r1", "r2 via link when its anchors are missing");
    check(near(p2->inLocal, compose(other, r2InR1), 0.03f, 1.0f), "r2 via link lands right");
    // ... and the other way round: r1's anchors missing, r1 placed from r2.
    ps = place(flat, links, session(other, false, true));
    check(find(ps, "r1")->ok && find(ps, "r1")->viaLink && near(find(ps, "r1")->inLocal, other, 0.03f, 1.0f), "r1 via link from r2");
    // None located: nothing placed.
    ps = place(flat, links, {});
    check(!find(ps, "r1")->ok && !find(ps, "r2")->ok, "no anchors: nothing placed");

    // 4. Walking from the middle of r1 through the doorway (x=2..3, z=1) into r2 and back, with r2's anchors missing.
    Tracker T; ps = place(flat, links, session(r1InLocal, true, false));
    T.update(flat, ps, headAt({0, 1.6f, 0})); check(T.current == "r1", "start in r1");
    std::vector<std::string> path; std::string last = T.current;
    for (float x = 0; x <= 5.0f; x += 0.1f) { T.update(flat, ps, headAt({x, 1.6f, 1.0f})); if (T.current != last) { path.push_back(T.current); last = T.current; } }
    check(T.current == "r2" && path.size() == 1, "walked into r2 once (no flicker at the door)");
    for (float x = 5.0f; x >= 0; x -= 0.1f) { T.update(flat, ps, headAt({x, 1.6f, 1.0f})); if (T.current != last) { path.push_back(T.current); last = T.current; } }
    check(T.current == "r1" && path.size() == 2, "walked back into r1");
    // Dithering on the door line does not flip rooms each step.
    int flips = 0; last = T.current;
    for (int i = 0; i < 40; i++) { T.update(flat, ps, headAt({2.1f + (i % 2 ? 0.15f : -0.15f), 1.6f, 1.0f})); if (T.current != last) { flips++; last = T.current; } }
    check(flips <= 1, "no flicker standing in the doorway");
    // The crossings give the doorway, in r1's frame near x 2..3, z 1.
    check(T.crossings.size() >= 2, "crossings recorded");
    for (auto& c : T.crossings) L.crossing(c, ps);
    changed.clear(); links = L.merge(links, &changed);
    check(links.size() == 1 && links[0].hasDoor, "doorway learned");
    check(links[0].hasDoor && links[0].doorInA.x > 1.8f && links[0].doorInA.x < 3.4f && std::fabs(links[0].doorInA.z - 1.0f) < 0.05f, "doorway where we crossed");
    check(changed.size() == 1, "new doorway reported");
    // Same samples again: nothing changed (no house write).
    changed.clear(); auto again = L.merge(links, &changed); check(changed.empty() && again.size() == 1, "steady link: no change");

    // 5. Head in neither box (in the hallway far away): stays in the last room.
    T.update(flat, ps, headAt({1, 1.6f, 9})); check(T.current == "r1", "outside every room: keep the last");
    // 6. Losing r1 entirely with no link: switch to the room that is still placed.
    auto ps2 = place(flat, {}, session(r1InLocal, false, true));
    T.update(flat, ps2, headAt({1, 1.6f, 9})); check(T.current == "r2", "current room gone: nearest placed one");

    // 7. A stored link in the opposite order (r2~r1) is matched, not duplicated, and its door converted.
    Link rev; rev.a = "r2"; rev.b = "r1"; rev.bInA = inverse(r2InR1); rev.samples = 9;
    rev.hasDoor = true; rev.doorInA = compose(inverse(r2InR1), Pose{{0, 0, 0, 1}, {2.5f, 1.6f, 1.0f}}).p;
    Learner L2; for (int i = 0; i < 6; i++) L2.sample(place(flat, {}, session(r1InLocal, true, true)));
    auto m = L2.merge({rev});
    check(m.size() == 1 && m[0].a == "r1" && m[0].b == "r2" && near(m[0].bInA, r2InR1), "reversed stored link merged");
    check(m.size() == 1 && m[0].hasDoor && wallfit::len(m[0].doorInA - V3{2.5f, 1.6f, 1.0f}) < 0.01f, "reversed door converted");
    // An unrelated old link is kept as it is.
    Link keep; keep.a = "r1"; keep.b = "r3"; keep.bInA = {yawQuat(0), {0, 0, 9}};
    Room r3; r3.id = "r3"; std::vector<Room> flat3 = {r1, r2, r3};
    check(L2.merge({keep}).size() == 2, "unrelated link kept");
    // A chain r2 -> r1 -> r3: r3 placed through two links.
    ps = place(flat3, {links[0], keep}, session(other, false, true));
    check(find(ps, "r3")->ok && find(ps, "r3")->via == "r1", "two-hop placement");

    // 8. Box without walls: anchors grown by 1.5 m; ids.
    Room bare; bare.id = "r9"; bare.anchors = {{"z", {yawQuat(0), {1, 1, 1}}}}; boxFrom(bare, {});
    check(bare.hasBox && bare.x0 < -0.4f && bare.x1 > 2.4f, "anchor box");
    check(nextId(flat) == "r3" && nextId({}) == "r1", "next room id");

    printf("rooms: %s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
