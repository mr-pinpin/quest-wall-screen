// RELOCCHECK's sampling (2026-11-04 08:48Z: the user walked around 3-4 m from the main wall, partly beside it; depth hit
// things in front of the wall at a steep angle, the gap read -7 / -19 / +100 cm and "ROOM LOOKS MISALIGNED" fired while
// every screen sat 3.7 cm from its wall). A sample counts only when the viewer is in front of the screen (within maxDeg
// of its normal, within maxDist) and most of the screen has depth (valid >= minValid); samples older than 30 s drop
// out, so a view that no longer qualifies stops feeding the check instead of freezing its last value. GL-free;
// bench/reloc_test.cpp.
#pragma once
#include "wallfit.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <vector>

namespace reloc {
using wallfit::Pose; using wallfit::V3;

inline bool frontal(const Pose& wall, float x, float y, V3 head, float maxDist = 3.5f, float maxDeg = 45.0f) {
    V3 c = wall.p + wallfit::rotate(wall.q, {x, y, 0}), n = wallfit::rotate(wall.q, {0, 0, 1}), d = head - c;
    float l = wallfit::len(d); if (l < 1e-3f || l > maxDist) return false;
    float cosA = std::fabs(wallfit::dot(n, d)) / l;   // either side: the normal's sign is the wall's convention
    return cosA >= std::cos(maxDeg * 3.14159265f / 180);
}

struct Window {
    float minValid = 0.7f; double maxAge = 30; size_t cap = 20; size_t minN = 5;
    std::deque<std::pair<double, float>> s;   // (time s, gap m)
    void add(double t, float gap, float valid, bool front) {
        if (front && valid >= minValid && std::isfinite(gap)) s.push_back({t, gap});
        while (s.size() > cap) s.pop_front();
        while (!s.empty() && t - s.front().first > maxAge) s.pop_front();
    }
    float median() const {
        if (s.size() < minN) return NAN;
        std::vector<float> v; for (auto& p : s) v.push_back(p.second); std::sort(v.begin(), v.end()); return v[v.size() / 2];
    }
};
}  // namespace reloc
