// CLEANLIVE placement targets (room_lab/cleanplace.h). g++ -std=c++17 -I room_lab bench/cleanplace_test.cpp && ./a.out
#include "cleanplace.h"
#include <cmath>
#include <cstdio>
using namespace cleanplace;
static int fails = 0;
static void check(bool c, const char* m) { if (!c) { printf("FAIL %s\n", m); fails++; } }
static wallfit::Wall wall(V3 c, float yawDeg, float w, float h) {
    float a = yawDeg * 3.14159265f / 180; wallfit::Wall W; W.n = {std::sin(a), 0, std::cos(a)}; W.right = {std::cos(a), 0, -std::sin(a)}; W.up = {0, 1, 0};
    W.centre = c; W.w = w; W.h = h; W.d = -wallfit::dot(W.n, c); return W;
}
static Old old(const std::string& id, V3 c, float yawDeg, float w, float h) { auto W = wall(c, yawDeg, w, h); return {id, W.pose(), -w / 2, -h / 2, w, h}; }

int main() {
    int n = 0; auto mint = [&] { return "w-dnew" + std::to_string(n++); };
    // Two old depth walls along one physical wall (z = 0, facing +z), 1 cm and 2 cm off it, 1.5 m and 1.4 m wide;
    // the clean wall is the whole 3 m on the plane.
    Old A = old("w-dA", {0.75f, 1.2f, 0.01f}, 0.5f, 1.5f, 2.4f), B = old("w-dB", {2.3f, 1.2f, -0.02f}, -0.4f, 1.4f, 2.4f);
    auto clean = std::vector<wallfit::Wall>{wall({1.5f, 1.25f, 0}, 0, 3.0f, 2.5f)};
    auto P = assign(clean, {A, B}, mint);
    check(P.size() == 2 && P[0].id == "w-dA" && P[1].id == "w-dB" && P[0].reused && P[1].reused && n == 0, "a clean wall merging two old ones keeps both ids, mints none");
    // Screens on each id (wall-local spots) stay put: only the snap onto the plane (<= 2 cm) and the small yaw change.
    float worst = 0;
    for (auto [o, p] : {std::pair<Old, Placed>{A, P[0]}, std::pair<Old, Placed>{B, P[1]}})
        for (float x : {-0.5f, 0.0f, 0.6f}) for (float y : {-0.3f, 0.4f}) {
            V3 before = at(o.pose, x, y), after = at(p.pose, x, y);
            worst = std::max(worst, wallfit::len(before - after));
        }
    printf("  merged walls: screens on either id move at most %.1f cm (the plane snap + <= 0.5 deg of yaw)\n", worst * 100);
    check(worst < 0.025f, "screens on the old ids stay put (< 2.5 cm)");
    // Both ids' rects cover the clean wall: the same rect, around their own origins.
    for (auto& p : P) { V3 c0 = at(p.pose, p.ox + p.w / 2, p.oy + p.h / 2); check(wallfit::len(c0 - clean[0].centre) < 1e-4f && std::fabs(p.w - 3.0f) < 1e-5f, "each id's rect is the clean wall"); }
    // A new wall nobody had: a new id at its centre.
    auto Q = assign({wall({0, 1.2f, 3}, 180, 2, 2.4f)}, {A, B}, mint);
    check(Q.size() == 1 && !Q[0].reused && Q[0].id == "w-dnew0" && std::fabs(Q[0].ox + 1) < 1e-5f, "an unmatched clean wall gets one new id at its centre");
    // An old wall split by a corner into two clean walls: its id goes to the better match only.
    Old L = old("w-dL", {0.5f, 1.2f, 0}, 0, 3.0f, 2.4f);
    auto R = assign({wall({0.0f, 1.2f, 0.01f}, 0, 1.5f, 2.4f), wall({1.0f, 1.2f, 0.06f}, 0, 1.5f, 2.4f)}, {L}, mint);
    int uses = 0; for (auto& p : R) uses += p.id == "w-dL";
    check(uses == 1 && R[0].id == "w-dL", "a split old wall maps to its best clean wall only, once");
    check(R.size() == 2 && !R[1].reused, "the other piece gets its own new id");
    // Nothing old: every clean wall new; nothing clean: nothing.
    check(assign({}, {A}, mint).empty(), "no clean walls: nothing placed (the old depth walls stay as they are)");
    printf("cleanplace: %s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
