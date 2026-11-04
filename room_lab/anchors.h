// Our own spatial anchors (SCAN; docs/house-onboarding.md section 3). Each room of house.json keeps 2-4 anchors we
// created and saved (XR_FB_spatial_entity + storage), each with its pose in the room frame. On launch, every anchor
// that locates gives one estimate of where the room is in LOCAL space; solve() takes the estimate most of the others
// agree with (within 5 cm / 2 deg), averages the agreeing ones and names the rest as drifted. One anchor is enough to
// place the room; two or more also check each other. GL-free (wallfit.h math); bench/anchors_test.cpp.
#pragma once
#include "wallfit.h"
#include <string>
#include <vector>

namespace anchors {
using wallfit::Pose; using wallfit::Quat; using wallfit::V3;

inline Quat qmul(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline Pose compose(const Pose& a, const Pose& b) { return {qmul(a.q, b.q), a.p + wallfit::rotate(a.q, b.p)}; }  // b expressed in a
inline Pose inverse(const Pose& a) { Quat c = wallfit::conj(a.q); return {c, wallfit::rotate(c, a.p * -1.0f)}; }
inline float angleDeg(Quat a, Quat b) {
    float d = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    return 2 * std::acos(std::min(1.0f, d)) * 57.29578f;
}
// Upright (gravity-aligned) rotation facing yaw; yaw 0 looks down -z.
inline Quat yawQuat(float yaw) { return {0, std::sin(yaw / 2), 0, std::cos(yaw / 2)}; }

struct Stored { std::string uuid; Pose inRoom; };  // house.json rooms[].anchors
struct Seen { std::string uuid; Pose inLocal; };   // located this session
struct Frame {
    bool ok = false; Pose roomInLocal;
    int used = 0;                                   // anchors that agreed
    std::vector<std::string> drifted;               // located, but disagreeing with the consensus
    float spreadM = 0, spreadDeg = 0;               // largest disagreement among the agreeing ones
};

inline Frame solve(const std::vector<Stored>& stored, const std::vector<Seen>& seen, float tolM = 0.05f, float tolDeg = 2.0f) {
    struct Est { std::string uuid; Pose room; };
    std::vector<Est> est;
    for (auto& s : seen) for (auto& st : stored) if (st.uuid == s.uuid) est.push_back({s.uuid, compose(s.inLocal, inverse(st.inRoom))});
    Frame f; if (est.empty()) return f;
    auto agree = [&](const Pose& a, const Pose& b) { return wallfit::len(a.p - b.p) <= tolM && angleDeg(a.q, b.q) <= tolDeg; };
    size_t best = 0; int bestN = -1;
    for (size_t i = 0; i < est.size(); i++) {
        int n = 0; for (auto& e : est) n += agree(est[i].room, e.room);
        if (n > bestN) { bestN = n; best = i; }
    }
    V3 p{0, 0, 0}; Quat q{0, 0, 0, 0}; const Quat ref = est[best].room.q;
    for (auto& e : est) {
        if (!agree(est[best].room, e.room)) { f.drifted.push_back(e.uuid); continue; }
        Quat r = e.room.q; if (r.x * ref.x + r.y * ref.y + r.z * ref.z + r.w * ref.w < 0) r = {-r.x, -r.y, -r.z, -r.w};
        p = p + e.room.p; q = {q.x + r.x, q.y + r.y, q.z + r.z, q.w + r.w}; f.used++;
    }
    float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    f.roomInLocal = {{q.x / l, q.y / l, q.z / l, q.w / l}, p * (1.0f / f.used)};
    for (auto& a : est) for (auto& b : est) {
        if (!agree(est[best].room, a.room) || !agree(est[best].room, b.room)) continue;
        f.spreadM = std::max(f.spreadM, wallfit::len(a.room.p - b.room.p)); f.spreadDeg = std::max(f.spreadDeg, angleDeg(a.room.q, b.room.q));
    }
    f.ok = true; return f;
}

// Where to create a room's anchors: up to n of the candidate points (the fitted wall centres), greedily the farthest
// apart and at least minSep from each other. Spread anchors check each other best.
inline std::vector<V3> choosePlaces(const std::vector<V3>& candidates, int n = 3, float minSep = 1.0f) {
    std::vector<V3> out; if (candidates.empty()) return out;
    out.push_back(candidates[0]);
    while ((int)out.size() < n) {
        float bestD = -1; size_t bi = 0;
        for (size_t i = 0; i < candidates.size(); i++) {
            float d = 1e9f; for (auto& o : out) d = std::min(d, wallfit::len(candidates[i] - o));
            if (d > bestD) { bestD = d; bi = i; }
        }
        if (bestD < minSep) break;
        out.push_back(candidates[bi]);
    }
    return out;
}

}  // namespace anchors
