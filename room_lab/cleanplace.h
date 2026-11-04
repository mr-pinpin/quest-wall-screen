// CLEANLIVE's clean walls as placement targets (K27, rules from work-c4 2026-11-04). The clean depth walls of a SCAN
// (consolidate.h, LOCAL) become our depth walls (room_lab depthWalls, kept in STAGE by the caller):
//  - an existing depth wall that a clean wall matches (wallfit::match, 8 cm / 5 deg, as before) keeps its id: never a
//    new id for the same physical wall;
//  - a clean wall that merges several old depth walls keeps ALL their ids alive, each snapped onto the clean plane;
//  - each kept id keeps its own origin (projected onto the clean plane) and its rect is re-expressed around it, so a
//    screen stored in wall-local x, y on that id stays where it was (it only snaps onto the cleaned plane);
//  - an old id goes to its best-matching clean wall only (a wall split at a corner does not map one id twice);
//  - a clean wall nothing matches gets a new id, posed at its centre.
// GL-free; bench/cleanplace_test.cpp.
#pragma once
#include <functional>
#include <string>
#include <vector>
#include "wallfit.h"

namespace cleanplace {
using wallfit::Pose; using wallfit::V3;

struct Old { std::string id; Pose pose; float ox = 0, oy = 0, w = 0, h = 0; };    // a depth wall now (LOCAL): origin pose + rect
struct Placed { std::string id; Pose pose; float ox, oy, w, h; bool reused; int clean; };

inline Pose centrePose(const Old& o) {
    V3 r = wallfit::rotate(o.pose.q, {1, 0, 0}), u = wallfit::rotate(o.pose.q, {0, 1, 0});
    return {o.pose.q, o.pose.p + r * (o.ox + o.w / 2) + u * (o.oy + o.h / 2)};
}
// The clean wall's frame with its origin at p (projected onto the clean plane), and its rect around that origin.
inline Placed around(const wallfit::Wall& c, V3 origin, const std::string& id, bool reused, int ci) {
    V3 p = origin - c.n * wallfit::dot(c.n, origin - c.centre);
    V3 d = c.centre - p;
    float ox = wallfit::dot(d, c.right) - c.w / 2, oy = wallfit::dot(d, c.up) - c.h / 2;
    return {id, {wallfit::fromAxes(c.right, c.up, c.n), p}, ox, oy, c.w, c.h, reused, ci};
}
inline std::vector<Placed> assign(const std::vector<wallfit::Wall>& clean, const std::vector<Old>& old, const std::function<std::string()>& mint,
                                  float maxDist = 0.08f, float maxDeg = 5.0f) {
    std::vector<int> bestFor(old.size(), -1); std::vector<float> bestScore(old.size(), 1e9f);
    for (size_t j = 0; j < old.size(); j++) for (size_t i = 0; i < clean.size(); i++) {
        wallfit::Match m = wallfit::match(clean[i], {{centrePose(old[j]), old[j].w, old[j].h}}, maxDist, maxDeg);
        if (m.index < 0) continue;
        float s = m.offset / maxDist + m.angleDeg / maxDeg;
        if (s < bestScore[j]) { bestScore[j] = s; bestFor[j] = (int)i; }
    }
    std::vector<Placed> out;
    for (size_t i = 0; i < clean.size(); i++) {
        bool any = false;
        for (size_t j = 0; j < old.size(); j++) if (bestFor[j] == (int)i) { out.push_back(around(clean[i], old[j].pose.p, old[j].id, true, (int)i)); any = true; }
        if (!any) { const auto& c = clean[i]; out.push_back({mint(), {wallfit::fromAxes(c.right, c.up, c.n), c.centre}, -c.w / 2, -c.h / 2, c.w, c.h, false, (int)i}); }
    }
    return out;
}
// Where a wall-local point (x, y) of an id is in LOCAL.
inline V3 at(const Pose& p, float x, float y) { return p.p + wallfit::rotate(p.q, {1, 0, 0}) * x + wallfit::rotate(p.q, {0, 1, 0}) * y; }

}  // namespace cleanplace
