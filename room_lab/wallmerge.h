// How a SCAN result changes a house wall (2026-11-04: a SCAN from the charger's partial view shrank three Space Setup
// walls to its fitted extents, 2.90 -> 0.90 m, and a screen ended up wider than its wall).
//  - A Meta scene wall keeps its pose and size: the fitted plane is a measurement of it, stored under "fit" only
//    (rms_m, samples, updated, w, h).
//  - Our own depth wall (source "depth") can only grow: w/h become the max of the old and the new fit; its pose is
//    kept once set (a later partial view must not move it); a new one takes the fit.
// GL-free; bench/wallmerge_test.cpp.
#pragma once
#include "house.h"
#include <algorithm>

namespace wallmerge {

inline json::Value merge(const json::Value& existing, bool sceneWall, const json::Value& fitPose, float fitW, float fitH, float rms, double samples, const std::string& stamp) {
    json::Value o = existing.type == json::Value::Obj ? existing : house::obj();
    json::Value fit = house::obj();
    fit.o["rms_m"] = house::num(rms); fit.o["samples"] = house::num(samples); fit.o["updated"] = house::str(stamp);
    fit.o["w"] = house::num(fitW); fit.o["h"] = house::num(fitH);
    o.o["fit"] = fit;
    if (sceneWall) return o;  // pose / w / h stay what Space Setup measured
    if (!o.has("pose")) o.o["pose"] = fitPose;
    o.o["w"] = house::num(std::max((float)o["w"].num(0), fitW));
    o.o["h"] = house::num(std::max((float)o["h"].num(0), fitH));
    return o;
}

}  // namespace wallmerge
