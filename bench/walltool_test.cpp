// Wall tool around the room (room_lab/wallshape.h, work-0e review 2026-11-04): lines bend along a chain of walls the
// shorter way round, every state has a hint, undo always steps back (fuzzed), shapes keep house wall ids.
//   g++ -std=c++17 -I room_lab bench/walltool_test.cpp && ./a.out
#include "wallshape.h"
#include <cstdio>
#include <cstdlib>
using namespace wallshape;
static int fails = 0;
static void check(bool c, const char* m) { if (!c) { printf("FAIL %s\n", m); fails++; } }
static WallRef wallAt(const char* id, V3 c, float yawDeg, float w, float h) {
    float a = yawDeg * 3.14159265f / 180; return {id, {{0, std::sin(a / 2), 0, std::cos(a / 2)}, c}, w, h};
}
// A 4 x 3 m room, walls 2.5 m high, normals into the room.
static std::vector<WallRef> room() {
    return {wallAt("w-south", {0, 1.25f, -1.5f}, 0, 4, 2.5f), wallAt("w-east", {2, 1.25f, 0}, -90, 3, 2.5f),
            wallAt("w-north", {0, 1.25f, 1.5f}, 180, 4, 2.5f), wallAt("w-west", {-2, 1.25f, 0}, 90, 3, 2.5f)};
}
static Input aim(V3 target, bool pinch) { V3 o{0, 1.25f, 0}; return {o, norm(target - o), pinch}; }

int main() {
    Tool T; T.setWalls(room());
    auto W = room();
    // 1. South wall (x -1, floor + 1.25 m) to the north wall at the same x, 0.5 m higher: the west way (5 m unfolded) wins
    // over the east way (9 m). Three pieces, joined at the corners, one straight line unfolded.
    auto pc = T.route(0, {-1, 0}, 2, {1, 0.5f});
    check(pc.size() == 3 && pc[0].wall == 0 && pc[1].wall == 3 && pc[2].wall == 2, "south -> west -> north (the shorter way round)");
    bool joined = true; for (size_t i = 0; i + 1 < pc.size(); i++) joined = joined && len(W[pc[i].wall].to3D(pc[i].b) - W[pc[i + 1].wall].to3D(pc[i + 1].a)) < 0.001f;
    check(joined, "the pieces meet at the corners (1 mm)");
    check(std::fabs(pc[0].b.y - 0.1f) < 0.001f && std::fabs(pc[1].b.y - 0.4f) < 0.001f, "one straight line unfolded: 0.5 m rise over 5 m, 0.1 m at the first corner, 0.4 m at the second");
    // The other side of the room: the east way.
    auto pe = T.route(0, {1.5f, 0}, 2, {-1.5f, 0});
    check(pe.size() == 3 && pe[1].wall == 1, "near the east corners: south -> east -> north");
    // Adjacent walls still use the exact corner bend; the same wall, one piece.
    check(T.route(0, {-1, 0}, 3, {0, 0}).size() == 2 && T.route(0, {-1, 0}, 0, {1, 0}).size() == 1, "adjacent: two pieces; same wall: one");
    // 2. The gesture: A on the south wall, B on the north wall, confirm: three saved shapes in one group.
    T.begin(Mode::Line);
    check(T.hint() == "PINCH WHERE THE LINE STARTS" || T.hint() == "POINT AT A WALL", "placing A: a hint");
    T.update(aim({-1, 1.25f, -1.5f}, false)); check(T.hint() == "PINCH WHERE THE LINE STARTS", "A: pinch where it starts");
    T.update(aim({-1, 1.25f, -1.5f}, true)); T.update(aim({-1, 1.75f, 1.5f}, false));
    check(T.state() == State::PlacingB && T.hint().rfind("PINCH WHERE IT ENDS", 0) == 0, "B over the north wall: reachable around the room");
    T.update(aim({-1, 1.75f, 1.5f}, true)); T.update(aim({-1, 1.75f, 1.5f}, false));
    check(T.state() == State::Preview, "preview");
    T.update(aim({-1, 1.75f, 1.5f}, true));
    check(T.state() == State::Confirmed && T.shapes().size() == 3, "confirmed: three shapes");
    std::string g; bool oneGroup = true; for (auto& [id, s] : T.shapes()) { if (g.empty()) g = s.group; oneGroup = oneGroup && !s.group.empty() && s.group == g; }
    check(oneGroup, "one group, so it selects, drags and deletes as one line");
    auto e = T.takeEdits(); check(e.size() == 3 && e[0].obj["wall"].str() == "w-south" && e[1].obj["wall"].str() == "w-west" && e[2].obj["wall"].str() == "w-north", "saved under the walls' house ids");
    // 3. Undo always works: back to before the line, and says when there is nothing left.
    check(T.undo() && T.shapes().empty() && T.state() == State::Idle, "undo removes the whole line");
    check(!T.undo() && !T.canUndo(), "nothing left: undo says so");
    // 4. Every state explains itself, and undo from any state steps back; fuzzed.
    srand(5); std::set<std::string> hints; int bad = 0;
    std::vector<V3> targets = {{-1, 1, -1.5f}, {1, 2, -1.5f}, {2, 1.2f, 0.5f}, {0.5f, 0.8f, 1.5f}, {-2, 1.5f, -1}, {0, 5, 0}, {0, 1.25f, 9}};
    for (int i = 0; i < 4000; i++) {
        int r = rand() % 12; V3 t = targets[rand() % targets.size()];
        if (r < 6) T.update(aim(t, rand() % 2));
        else if (r == 6) T.begin(rand() % 2 ? Mode::Line : Mode::Circle);
        else if (r == 7) T.confirm(); else if (r == 8) T.expand(); else if (r == 9) T.finish(); else if (r == 10) T.cancel(); else T.undo();
        std::string h = T.hint(); if (h.empty()) bad++; hints.insert(h);
        T.takeEdits();
    }
    check(bad == 0, "a hint in every state, every step");
    printf("  fuzz: 4000 steps, %zu distinct hints, %zu shapes at the end\n", hints.size(), T.shapes().size());
    int steps = 0; while (T.undo() && steps < 100000) steps++;
    check(T.shapes().empty() && !T.canUndo(), "undo all the way back: no shapes left, nothing to undo");
    // 5. House wall ids: Meta walls by scene uuid, our depth walls by their own id (never "w-w-d..."), else w- + uuid8.
    json::Value doc = json::parse(R"({"walls":[{"id":"w-kitchen","scene":["0123456789abcdef0123456789abcdef"]}]})");
    check(wallIdOf(doc, "0123456789abcdef0123456789abcdef") == "w-kitchen" && wallIdOf(doc, "w-d12ed9a") == "w-d12ed9a" && wallIdOf(doc, "fedcba9876543210fedcba9876543210") == "w-fedcba98",
          "wall ids: house id by scene uuid, depth walls keep theirs, else w-uuid8");
    printf("walltool: %s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
