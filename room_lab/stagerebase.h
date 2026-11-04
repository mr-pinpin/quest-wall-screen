// The STAGE rebase vote (room_lab.cpp updatePlacementGuard), GL-free so it can be tested (work-k5 review, 2026-11-04).
// STAGE can move between sessions (a relocalisation); Meta's walls located now, against their cached STAGE spots
// (files/lastwalls.json), give one estimate each of the correction, and their consensus (anchors::solve, 10 cm /
// 3 deg) rebases the cache and our depth walls, at most once per session. The first 5 s wait for a second wall.
// Fix: the 2 s cache refresh must not overwrite a cached Meta wall before the vote is decided. It used to, so a wall
// seen alone in the first seconds had its cache entry replaced by its new spot; when a second wall arrived, the two
// estimates disagreed (identity vs the real move), nothing was rebased, and every stand-in and depth wall stayed at
// the stale spot for the session: those screens appeared moved. refreshAllowed() holds existing entries until then.
#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>
#include "anchors.h"

namespace stagerebase {
using wallfit::Pose;

struct State { std::set<std::string> checked; bool rebased = false, decided = false; };
struct Out { bool rebase = false; Pose T; int used = 0, ests = 0; float worst = 0; std::string ref; bool disagree = false; };

inline Pose unit(Pose p) {
    float l = std::sqrt(p.q.x * p.q.x + p.q.y * p.q.y + p.q.z * p.q.z + p.q.w * p.q.w);
    if (l > 1e-6f) p.q = {p.q.x / l, p.q.y / l, p.q.z / l, p.q.w / l};
    return p;
}
// real: Meta walls located now (pose in STAGE); cache: cached STAGE poses. One call per frame.
inline Out step(State& s, float now, const std::map<std::string, Pose>& real, const std::map<std::string, Pose>& cache, float waitS = 5) {
    Out o; if (s.rebased) return o;
    std::vector<anchors::Seen> est; std::vector<std::string> seen;
    for (auto& [u, r] : real) {
        auto c = cache.find(u); if (c == cache.end() || s.checked.count(u)) continue;
        seen.push_back(u); Pose cs = unit(c->second);
        est.push_back({"x", unit(anchors::compose(r, anchors::inverse(cs)))});
        float j = wallfit::len(r.p - cs.p); if (j > o.worst) { o.worst = j; o.ref = u; }
    }
    if (est.size() < 2 && now < waitS) return o;   // wait (up to waitS) for a second wall to vote
    for (auto& u : seen) s.checked.insert(u);
    if (est.empty()) return o;
    s.decided = true; o.ests = (int)est.size();
    anchors::Frame f = anchors::solve({{"x", Pose{{0, 0, 0, 1}, {0, 0, 0}}}}, est, 0.10f, 3.0f); o.used = f.used;
    if (f.ok && wallfit::len(f.roomInLocal.p) > 0.10f && (f.used >= 2 || est.size() == 1)) { o.rebase = true; o.T = unit(f.roomInLocal); s.rebased = true; }
    else if (o.worst > 0.10f) o.disagree = true;
    return o;
}
// May the cache refresh overwrite an existing entry now? New entries are always fine.
inline bool refreshAllowed(const State& s) { return s.decided || s.rebased; }

}  // namespace stagerebase
