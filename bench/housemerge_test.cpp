// HOUSE first sync and merge (room_lab/housemerge.h) on the 2026-11-04 case: the house has screens 1-3, the headset
// 1-5. The headset pushes first; then both have 1-5; a later move from room.sh is adopted; a removal elsewhere of a
// known screen is honoured; a screen added on the headset while the house was away is kept.
//   g++ -std=c++17 -I room_lab bench/housemerge_test.cpp && ./a.out
#include "housemerge.h"
#include "house.h"
#include <cstdio>
#include <map>
static int fails = 0;
static void check(bool c, const char* m) { if (!c) { printf("FAIL %s\n", m); fails++; } }
struct Spot { std::string wall; float x = 0; };
// The headset side as room_lab models it, with the house doc applied through house::apply like the real client.
struct Headset {
    std::map<int, Spot> screens; std::set<int> known; bool pushed = false;
    std::vector<house::Edit> pushAll(const json::Value& doc) {   // houseQueueScreens: every local screen, whole objects
        std::vector<house::Edit> e;
        for (auto& [id, s] : screens) { json::Value o = house::obj(); o.o["id"] = house::num(id); o.o["wall"] = house::str(s.wall); o.o["x"] = house::num(s.x); e.push_back({"screens", std::to_string(id), o}); }
        for (auto& sc : doc["screens"].a) { int id = (int)sc["id"].num(); if (!screens.count(id)) e.push_back({"screens", std::to_string(id), json::Value{}}); }
        return e;
    }
    // houseUpdate: returns the edits to push (if any).
    std::vector<house::Edit> update(const json::Value& doc) {
        if (!pushed) { pushed = true; return pushAll(doc); }   // first sync: local is the truth
        std::set<int> local, hs; std::map<int, Spot> hspot;
        for (auto& [id, s] : screens) local.insert(id);
        for (auto& sc : doc["screens"].a) { int id = (int)sc["id"].num(); hs.insert(id); hspot[id] = {sc["wall"].str(), (float)sc["x"].num()}; }
        housemerge::Decision d = housemerge::decide(local, hs, known);
        std::map<int, Spot> next;
        for (int id : d.fromHouse) next[id] = hspot[id];
        for (int id : d.keepLocal) next[id] = screens[id];
        screens = next; known = hs;
        return d.push() ? pushAll(doc) : std::vector<house::Edit>{};
    }
};
static json::Value houseWith(std::vector<int> ids) {
    json::Value d = house::obj(); d.o["screens"] = house::arr();
    for (int id : ids) { json::Value o = house::obj(); o.o["id"] = house::num(id); o.o["wall"] = house::str("w-house"); o.o["x"] = house::num(0); d.o["screens"].a.push_back(o); }
    return d;
}
static std::set<int> ids(const json::Value& d) { std::set<int> s; for (auto& sc : d["screens"].a) s.insert((int)sc["id"].num()); return s; }
int main() {
    // The exact case: house 1-3, headset 1-5 (its own spots).
    json::Value house = houseWith({1, 2, 3});
    Headset h; for (int id = 1; id <= 5; id++) h.screens[id] = {"w-local", 0.1f * id};
    auto e = h.update(house); for (auto& x : e) house::apply(house, x);
    check(ids(house) == std::set<int>({1, 2, 3, 4, 5}), "first sync: the house gets 1-5");
    check(h.screens.size() == 5 && h.screens[4].wall == "w-local" && h.screens[2].wall == "w-local", "first sync: the headset keeps 1-5 where they were");
    for (auto& sc : house["screens"].a) check(sc["wall"].str() == "w-local", "first sync: the house has the headset's spots");
    // Next rev, nothing changed: no pushes, both 1-5.
    e = h.update(house); check(e.empty() && h.screens.size() == 5, "steady: nothing to push");
    // room.sh moves screen 3: adopted.
    for (auto& sc : house.o["screens"].a) if (sc["id"].num() == 3) { sc.o["wall"] = house::str("w-moved"); sc.o["x"] = house::num(0.7); }
    e = h.update(house); check(e.empty() && h.screens[3].wall == "w-moved" && std::fabs(h.screens[3].x - 0.7f) < 1e-6f, "a move from elsewhere is adopted");
    // Screen 5 removed elsewhere (the house knew it): removed here too.
    house::apply(house, {"screens", "5", json::Value{}});
    e = h.update(house); check(!h.screens.count(5) && e.empty(), "a removal elsewhere of a known screen is honoured");
    // Screen 6 summoned on the headset while the house was away, then a rev without it: kept and pushed.
    h.screens[6] = {"w-local", 0.6f};
    e = h.update(house); for (auto& x : e) house::apply(house, x);
    check(h.screens.count(6) && ids(house).count(6), "a screen the house never knew is kept and pushed");
    // A screen added elsewhere (7) appears here.
    json::Value seven = house::obj(); seven.o["id"] = house::num(7); seven.o["wall"] = house::str("w-house"); seven.o["x"] = house::num(0);
    house::apply(house, {"screens", "7", seven});
    e = h.update(house); check(h.screens.count(7) && e.empty(), "a screen added elsewhere appears");
    // The pure rule on its own.
    housemerge::Decision d = housemerge::decide({1, 2, 3, 4, 5}, {1, 2, 3}, {});
    check(d.keepLocal == std::vector<int>({4, 5}) && d.removed.empty() && d.push(), "decide: unknown local screens are kept and pushed");
    d = housemerge::decide({1, 2, 3, 4, 5}, {1, 2, 3}, {1, 2, 3, 4, 5});
    check(d.removed == std::vector<int>({4, 5}) && !d.push(), "decide: known screens missing from the house were removed");
    printf("housemerge: %s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
