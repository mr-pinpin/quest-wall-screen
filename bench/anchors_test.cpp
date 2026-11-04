// Room frame from our own anchors (room_lab/anchors.h). g++ -std=c++17 -I room_lab bench/anchors_test.cpp && ./a.out
#include "anchors.h"
#include <cstdio>
#include <random>
using namespace anchors;
static int fails = 0;
static void check(bool c, const char* m) { if (!c) { printf("FAIL %s\n", m); fails++; } }
int main() {
    std::mt19937 rng(7); std::normal_distribution<float> noise(0, 0.004f);
    Pose room{yawQuat(0.7f), {1.5f, -0.2f, -3.0f}};  // where the room really is in LOCAL this session
    std::vector<Stored> st = {{"a", {yawQuat(0.0f), {0, 0, 0}}}, {"b", {yawQuat(1.2f), {3.0f, 0.3f, 0.5f}}}, {"c", {yawQuat(-2.0f), {1.0f, 1.0f, 3.5f}}}};
    auto see = [&](const Stored& s, V3 off = {0, 0, 0}) { Pose l = compose(room, s.inRoom); l.p = l.p + off + V3{noise(rng), noise(rng), noise(rng)}; return Seen{s.uuid, l}; };
    // Inverse and compose round trip.
    Pose x{yawQuat(0.3f), {1, 2, 3}}, y = compose(x, inverse(x));
    check(wallfit::len(y.p) < 1e-5f && angleDeg(y.q, Quat{}) < 0.01f, "pose inverse");
    // Three anchors with 4 mm noise: the room within 1 cm / 0.5 deg.
    Frame f = solve(st, {see(st[0]), see(st[1]), see(st[2])});
    check(f.ok && f.used == 3 && f.drifted.empty(), "three agree");
    check(wallfit::len(f.roomInLocal.p - room.p) < 0.01f && angleDeg(f.roomInLocal.q, room.q) < 0.5f, "room recovered");
    // One anchor drifted 20 cm: rejected, the other two place the room.
    f = solve(st, {see(st[0]), see(st[1], {0.2f, 0, 0}), see(st[2])});
    check(f.ok && f.used == 2 && f.drifted.size() == 1 && f.drifted[0] == "b", "drifted anchor named");
    check(wallfit::len(f.roomInLocal.p - room.p) < 0.01f, "room still right");
    // One anchor alone is enough; an unknown uuid is ignored; nothing located is not ok.
    f = solve(st, {see(st[2]), Seen{"zz", {}}}); check(f.ok && f.used == 1 && wallfit::len(f.roomInLocal.p - room.p) < 0.01f, "one anchor places the room");
    check(!solve(st, {}).ok, "none located");
    // A small yaw error on one anchor (3 deg) is beyond 2 deg: drifted.
    Seen bad = see(st[1]); bad.inLocal.q = qmul(yawQuat(0.0524f), bad.inLocal.q);
    f = solve(st, {see(st[0]), bad, see(st[2])}); check(f.used == 2 && f.drifted.size() == 1, "3 deg yaw drift rejected");
    check(f.spreadM < 0.03f, "spread among the agreeing ones small");
    // Places: spread, at least 1 m apart.
    std::vector<V3> c = {{0, 1, 0}, {0.3f, 1, 0}, {4, 1, 0}, {2, 1, 3}, {0.1f, 1, 0.2f}};
    auto pl = choosePlaces(c, 3); check(pl.size() == 3, "three places");
    for (size_t i = 0; i < pl.size(); i++) for (size_t j = i + 1; j < pl.size(); j++) check(wallfit::len(pl[i] - pl[j]) >= 1.0f, "places 1 m apart");
    check(choosePlaces({{0, 0, 0}, {0.5f, 0, 0}}, 3).size() == 1, "no crowding");
    printf("anchors: %s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
