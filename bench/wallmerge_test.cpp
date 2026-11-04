// SCAN never shrinks or moves a house wall (room_lab/wallmerge.h). g++ -std=c++17 -I room_lab bench/wallmerge_test.cpp && ./a.out
#include "wallmerge.h"
#include <cstdio>
static int fails = 0;
static void check(bool c, const char* m) { if (!c) { printf("FAIL %s\n", m); fails++; } }
static json::Value pose(double x) { json::Value p = house::obj(), a = house::arr(), q = house::arr(); a.a = {house::num(x), house::num(0), house::num(0)}; q.a = {house::num(0), house::num(0), house::num(0), house::num(1)}; p.o["p"] = a; p.o["q"] = q; return p; }
int main() {
    // The 2026-11-04 case: a Space Setup wall 2.90 x 2.51, a partial fit 0.90 x 2.50 from the charger.
    json::Value scene = house::obj(); scene.o["id"] = house::str("w-488e679d"); scene.o["source"] = house::str("scene");
    scene.o["w"] = house::num(2.8956); scene.o["h"] = house::num(2.5095); scene.o["pose"] = pose(1.0);
    json::Value m = wallmerge::merge(scene, true, pose(1.3), 0.90f, 2.50f, 0.03f, 1200, "t");
    check(std::fabs(m["w"].num() - 2.8956) < 1e-6 && std::fabs(m["h"].num() - 2.5095) < 1e-6, "scene wall keeps its size");
    check(std::fabs(m["pose"]["p"][0].num() - 1.0) < 1e-6, "scene wall keeps its pose");
    check(std::fabs(m["fit"]["w"].num() - 0.90) < 1e-6 && m["fit"]["samples"].num() == 1200, "the fit is recorded under fit");
    // Our depth wall: grows, never shrinks, pose kept once set.
    json::Value d = house::obj(); d.o["id"] = house::str("w-d123456"); d.o["source"] = house::str("depth"); d.o["w"] = house::num(3.0); d.o["h"] = house::num(2.5); d.o["pose"] = pose(2.0);
    json::Value g = wallmerge::merge(d, false, pose(2.4), 1.2f, 2.7f, 0.02f, 900, "t");
    check(std::fabs(g["w"].num() - 3.0) < 1e-6 && std::fabs(g["h"].num() - 2.7) < 1e-6, "depth wall: width kept, height grows");
    check(std::fabs(g["pose"]["p"][0].num() - 2.0) < 1e-6, "depth wall pose kept once set");
    // A brand-new depth wall takes its fit.
    json::Value n = wallmerge::merge(json::Value{}, false, pose(5.0), 1.5f, 2.2f, 0.02f, 500, "t");
    check(std::fabs(n["w"].num() - 1.5) < 1e-6 && std::fabs(n["pose"]["p"][0].num() - 5.0) < 1e-6, "a new depth wall takes the fit");
    printf("wall merge: %s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
