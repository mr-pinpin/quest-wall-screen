// MULTIROOM and the house file: rooms[] (anchors, walls, floor -> each room's box) and links[] in, link objects out.
// house/schema.py checks what linkJson writes (bench/rooms_house_test.cpp + bench/rooms_schema_check.py).
#pragma once
#include "house.h"
#include "rooms.h"

namespace rooms {
inline Pose poseOf(const json::Value& o) {
    Pose p; const json::Value &pp = o["p"], &q = o["q"];
    p.p = {(float)pp[0].num(), (float)pp[1].num(), (float)pp[2].num()}; p.q = {(float)q[0].num(), (float)q[1].num(), (float)q[2].num(), (float)q[3].num(1)};
    return p;
}
inline json::Value poseValue(const Pose& p) {
    json::Value o = house::obj(), pp = house::arr(), q = house::arr();
    pp.a = {house::num(p.p.x), house::num(p.p.y), house::num(p.p.z)}; q.a = {house::num(p.q.x), house::num(p.q.y), house::num(p.q.z), house::num(p.q.w)};
    o.o["p"] = pp; o.o["q"] = q; return o;
}
inline void load(const json::Value& doc, std::vector<Room>& rs, std::vector<Link>& ls) {
    rs.clear(); ls.clear();
    for (auto& r : doc["rooms"].a) {
        Room R; R.id = r["id"].str(); if (R.id.empty()) continue;
        for (auto& a : r["anchors"].a) R.anchors.push_back({a["uuid"].str(), poseOf(a["pose"])});
        std::vector<V3> pts;
        for (auto& f : r["floor"].a) pts.push_back({(float)f[0].num(), 0, (float)f[1].num()});
        for (auto& w : doc["walls"].a) if (w["room"].str() == R.id && w.has("pose") && w["kind"].str() != "floor" && w["kind"].str() != "ceiling") {
            Pose wp = poseOf(w["pose"]); V3 rt = wallfit::rotate(wp.q, {1, 0, 0}); float hw = (float)w["w"].num() / 2;
            pts.push_back(wp.p + rt * hw); pts.push_back(wp.p - rt * hw);
        }
        boxFrom(R, pts); rs.push_back(R);
    }
    for (auto& l : doc["links"].a) {
        Link L; L.a = l["a"].str(); L.b = l["b"].str(); if (L.a.empty() || L.b.empty() || !l.has("pose")) continue;
        L.bInA = poseOf(l["pose"]); L.samples = (int)l["samples"].num();
        if (l["door"].type == json::Value::Arr) { L.hasDoor = true; L.doorInA = {(float)l["door"][0].num(), (float)l["door"][1].num(), (float)l["door"][2].num()}; L.doorSamples = (int)l["doorSamples"].num(); }
        ls.push_back(L);
    }
}
inline json::Value linkJson(const Link& L, const std::string& stamp) {
    json::Value o = house::obj(); o.o["id"] = house::str(L.id()); o.o["a"] = house::str(L.a); o.o["b"] = house::str(L.b);
    o.o["pose"] = poseValue(L.bInA); o.o["samples"] = house::num(L.samples); o.o["updated"] = house::str(stamp);
    if (L.hasDoor) { json::Value dv = house::arr(); dv.a = {house::num(L.doorInA.x), house::num(L.doorInA.y), house::num(L.doorInA.z)}; o.o["door"] = dv; o.o["doorSamples"] = house::num(L.doorSamples); }
    return o;
}
// The house edits for the links that changed; a stored link of the same pair in the other order is removed.
inline std::vector<house::Edit> linkEdits(const std::vector<Link>& merged, const std::vector<Link>& old, const std::vector<std::string>& changed, const std::string& stamp) {
    std::vector<house::Edit> e;
    for (auto& L : merged) {
        if (std::find(changed.begin(), changed.end(), L.id()) == changed.end()) continue;
        for (auto& l : old) if (l.a == L.b && l.b == L.a) e.push_back({"links", l.id(), json::Value()});
        e.push_back({"links", L.id(), linkJson(L, stamp)});
    }
    return e;
}
}  // namespace rooms
