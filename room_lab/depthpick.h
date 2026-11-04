// DEPTHPLACE (default on; the user, 2026-11-04: "in the room near the TV, pinching to place a screen says no"): Meta's
// Space Setup only covers one room, so a summon or move ray there hits no scene wall. The depth worker already fits
// solid walls everywhere you look (wallfit fuser, status depthGap.fused); pick() returns the solid fitted wall the ray
// hits first (not floor/ceiling, confidence >= minConf, at least 0.6 x 0.6 m, not a wall Meta already has), and
// room_lab keeps it as one of SCAN's depth walls ("w-d...", files/depthwalls.json) so the screen has a wall to live on.
// GL-free; bench/depthpick_test.cpp.
#pragma once
#include "wallfit.h"
#include <vector>

namespace depthpick {
using wallfit::V3;

inline int pick(const std::vector<wallfit::Wall>& walls, V3 o, V3 d, const std::vector<wallfit::HouseWall>& scene,
                float minConf = 0.5f, float maxT = 8.0f, float* hitT = nullptr) {
    int best = -1; float bestT = maxT;
    for (size_t i = 0; i < walls.size(); i++) {
        const wallfit::Wall& w = walls[i];
        if (w.kind != wallfit::Kind::wall || w.confidence < minConf || w.w < 0.6f || w.h < 0.6f) continue;
        float dn = wallfit::dot(d, w.n); if (std::fabs(dn) < 1e-4f) continue;
        float t = wallfit::dot(w.centre - o, w.n) / dn; if (t <= 0 || t >= bestT) continue;
        V3 p = o + d * t - w.centre;
        if (std::fabs(wallfit::dot(p, w.right)) > w.w / 2 || std::fabs(wallfit::dot(p, w.up)) > w.h / 2) continue;
        if (!scene.empty() && wallfit::match(w, scene, 0.08f, 5.0f).index >= 0) continue;  // Meta has it: use Meta's
        best = (int)i; bestT = t;
    }
    if (hitT && best >= 0) *hitT = bestT;
    return best;
}
}  // namespace depthpick
