// "Where I place things, they stay there, no matter what" (2026-11-04, after a restart that raced Meta's
// relocalisation got an empty scene and showed no screens). GL-free logic; room_lab wires it in, bench tests it.
//  - Requery: while any wall a placed screen needs is missing or unlocated, query the scene again every 2 s for the
//    first 30 s, then every 10 s; at once on a reference-space change or focus regained; one query in flight at a time.
//  - Stand-ins: each needed wall's last good pose is cached in STAGE space (it persists across app launches and is
//    relocalised with the room), with its rect. Until the real wall is located, a stand-in wall built from the cache
//    takes its uuid, so every screen draws at its saved spot from the first frame; the real wall replaces it on arrival.
//  - Cache writes only when a wall moved more than 1 cm or 0.5 deg (the file is rewritten rarely).
#pragma once
#include "anchors.h"
#include "house.h"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace pguard {
using wallfit::Pose; using wallfit::V3;

struct CachedWall { std::string uuid; Pose stage; float ox = 0, oy = 0, w = 0, h = 0; };  // pose in STAGE, rect offset/extent

struct Requery {
    double lastQuery = -1e9, firstMissing = -1; bool inFlight = false, forced = false; int queries = 0;
    static bool missingAny(const std::set<std::string>& need, const std::set<std::string>& have) {
        if (have.empty()) return true;
        for (auto& u : need) if (!have.count(u)) return true;
        return false;
    }
    // need: walls the placed screens use; have: real walls located now.
    bool due(double t, const std::set<std::string>& need, const std::set<std::string>& have) {
        bool missing = missingAny(need, have);
        if (!missing) { firstMissing = -1; if (!forced) return false; }
        else if (firstMissing < 0) firstMissing = t;
        if (inFlight && t - lastQuery < 5) return false;  // wait for the one in flight (5 s at most)
        if (forced) return t - lastQuery >= 0.5;
        double gap = t - firstMissing < 30 ? 2.0 : 10.0;
        return t - lastQuery >= gap;
    }
    void started(double t) { lastQuery = t; inFlight = true; forced = false; queries++; }
    void finished() { inFlight = false; }
};

// The uuids that need a stand-in now: needed, not located for real, and cached.
inline std::vector<std::string> standIns(const std::set<std::string>& need, const std::set<std::string>& haveReal, const std::map<std::string, CachedWall>& cache) {
    std::vector<std::string> out;
    for (auto& u : need) if (!haveReal.count(u) && cache.count(u)) out.push_back(u);
    return out;
}
inline bool moved(const CachedWall& a, const CachedWall& b) {
    return wallfit::len(a.stage.p - b.stage.p) > 0.01f || anchors::angleDeg(a.stage.q, b.stage.q) > 0.5f ||
           std::fabs(a.w - b.w) > 0.01f || std::fabs(a.h - b.h) > 0.01f || std::fabs(a.ox - b.ox) > 0.01f || std::fabs(a.oy - b.oy) > 0.01f;
}
// LOCAL pose of a cached wall, given where STAGE is in LOCAL now.
inline Pose toLocal(const Pose& stageInLocal, const CachedWall& c) { return anchors::compose(stageInLocal, c.stage); }
inline CachedWall fromLocal(const std::string& uuid, const Pose& stageInLocal, const Pose& wallLocal, float ox, float oy, float w, float h) {
    return {uuid, anchors::compose(anchors::inverse(stageInLocal), wallLocal), ox, oy, w, h};
}

inline std::string dump(const std::map<std::string, CachedWall>& cache) {
    json::Value a = house::arr();
    for (auto& [u, c] : cache) {
        json::Value o = house::obj(), p = house::arr(), q = house::arr();
        p.a = {house::num(c.stage.p.x), house::num(c.stage.p.y), house::num(c.stage.p.z)};
        q.a = {house::num(c.stage.q.x), house::num(c.stage.q.y), house::num(c.stage.q.z), house::num(c.stage.q.w)};
        o.o["uuid"] = house::str(u); o.o["p"] = p; o.o["q"] = q;
        o.o["ox"] = house::num(c.ox); o.o["oy"] = house::num(c.oy); o.o["w"] = house::num(c.w); o.o["h"] = house::num(c.h);
        a.a.push_back(o);
    }
    json::Value d = house::obj(); d.o["schema"] = house::num(1); d.o["walls"] = a;
    return house::dump(d);
}
inline std::map<std::string, CachedWall> load(const std::string& text) {
    std::map<std::string, CachedWall> m; bool ok = false; json::Value d = json::parse(text, &ok);
    if (!ok) return m;
    for (auto& o : d["walls"].a) {
        CachedWall c; c.uuid = o["uuid"].str(); if (c.uuid.empty()) continue;
        c.stage.p = {(float)o["p"][0].num(), (float)o["p"][1].num(), (float)o["p"][2].num()};
        c.stage.q = {(float)o["q"][0].num(), (float)o["q"][1].num(), (float)o["q"][2].num(), (float)o["q"][3].num(1)};
        c.ox = (float)o["ox"].num(); c.oy = (float)o["oy"].num(); c.w = (float)o["w"].num(); c.h = (float)o["h"].num();
        m[c.uuid] = c;
    }
    return m;
}

}  // namespace pguard
