// room_lab/consolidate.h against work-34's house/consolidate.py (K27). bench/consolidate_xcheck.py DIR writes, per case,
// the house doc and what the Python gives up to the same stage; this runs the C++ on each and compares: the same
// segments (facing, offset, extent, heights within 1 mm; the same member walls), the same off-axis and dropped walls
// (id and reason), the same room direction (0.01 deg). Then the 2 ms budget on the largest case.
//   python3 bench/consolidate_xcheck.py /tmp/cx && g++ -std=c++17 -O2 -I room_lab bench/consolidate_test.cpp && ./a.out /tmp/cx
#include "consolidate.h"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
static int fails = 0;
static void check(bool c, const std::string& m) { if (!c) { printf("FAIL %s\n", m.c_str()); fails++; } }
static json::Value load(const std::string& p) { std::ifstream f(p); std::stringstream ss; ss << f.rdbuf(); bool ok = false; json::Value v = json::parse(ss.str(), &ok); return v; }

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : "/tmp/cx";
    std::ifstream cf(dir + "/cases.txt"); std::string name; int cases = 0; double worstMm = 0, worstDeg = 0;
    double bestUs = 1e18, planUs = 0; std::string slowCase; size_t planWalls = 0, planRooms = 0; int verdicts = 0, okRooms = 0, suspects = 0;
    auto r3c = [](double v) { return std::round(v * 1000.0) / 1000.0; };
    while (cf >> name) {
        json::Value doc = load(dir + "/" + name + ".doc.json"), want = load(dir + "/" + name + ".want.json");
        consolidate::Result R; consolidate::run(doc, R, {}, true); cases++;
        double dth = std::fabs(consolidate::wrap(consolidate::deg(R.th - want["th"].n), 90));
        worstDeg = std::max(worstDeg, dth);
        check(dth < 0.01, name + ": room direction");
        check(R.needsRooms == want["needsRooms"].b, name + ": needsRooms");
        const auto& ws = want["segs"].a;
        check(R.segs.size() == ws.size(), name + ": " + std::to_string(R.segs.size()) + " segments, the Python has " + std::to_string(ws.size()));
        for (size_t i = 0; i < std::min(R.segs.size(), ws.size()); i++) {
            const auto& s = R.segs[i]; const auto& w = ws[i];
            double e = std::max({std::fabs(s.off - w["off"].n), std::fabs(s.t0 - w["t0"].n), std::fabs(s.t1 - w["t1"].n), std::fabs(s.y0 - w["y0"].n), std::fabs(s.y1 - w["y1"].n)});
            worstMm = std::max(worstMm, e * 1000);
            bool ids = s.members.size() == w["ids"].a.size();
            for (size_t m = 0; ids && m < s.members.size(); m++) ids = s.members[m]->id == w["ids"][m].str();
            check(s.k == w["k"].integer() && e < 0.001 && ids, name + ": segment " + std::to_string(i));
        }
        bool off = R.offaxis.size() == want["offaxis"].a.size();
        for (size_t i = 0; off && i < R.offaxis.size(); i++) off = R.offaxis[i]->id == want["offaxis"][i].str();
        check(off, name + ": off-axis walls");
        bool dr = R.dropped.size() == want["dropped"].a.size();
        for (size_t i = 0; dr && i < R.dropped.size(); i++) dr = R.dropped[i].id == want["dropped"][i][0].str() && R.dropped[i].why == want["dropped"][i][1].str();
        if (!dr) for (size_t i = 0; i < std::max(R.dropped.size(), want["dropped"].a.size()); i++)
            printf("    dropped %zu: c++ [%s] %s | py [%s] %s\n", i, i < R.dropped.size() ? R.dropped[i].id.c_str() : "-", i < R.dropped.size() ? R.dropped[i].why.c_str() : "",
                   want["dropped"][i][0].str().c_str(), want["dropped"][i][1].str().c_str());
        check(dr, name + ": dropped walls and reasons");
        // phase 2: the plan's walls and rooms, as consolidate() returns them (rounded to mm there)
        auto near = [](double a, double b) { return std::fabs(a - b) <= 0.0015; };
        const auto& pw = want["walls"].a;
        check(R.walls.size() == pw.size(), name + ": " + std::to_string(R.walls.size()) + " plan walls, the Python has " + std::to_string(pw.size()));
        for (size_t i = 0; i < std::min(R.walls.size(), pw.size()); i++) {
            const auto& w = R.walls[i]; const auto& v = pw[i];
            double qy = v["pose"]["q"][1].n, qw = v["pose"]["q"][3].n, yaw = 2 * std::atan2(qy, qw);
            double dyaw = std::fabs(consolidate::wrap(consolidate::deg(w.yaw - yaw), 360));
            bool from = w.from.size() == v["from"].a.size(); for (size_t m = 0; from && m < w.from.size(); m++) from = w.from[m] == v["from"][m].str();
            std::string room = v["room"].type == json::Value::Str ? v["room"].str() : "";
            bool ok = w.id == v["id"].str() && near(w.x, v["pose"]["p"][0].n) && near(w.y, v["pose"]["p"][1].n) && near(w.z, v["pose"]["p"][2].n) && dyaw < 0.01 &&
                      near(w.w, v["w"].n) && near(w.h, v["h"].n) && w.room == room && w.src == v["src"].str() && from;
            if (!ok) printf("    wall %zu: c++ %s %.3f %.3f %.3f w %.3f h %.3f room %s | py %s %.3f %.3f %.3f w %.3f h %.3f room %s\n", i, w.id.c_str(), w.x, w.y, w.z, w.w, w.h, w.room.c_str(),
                            v["id"].str().c_str(), v["pose"]["p"][0].n, v["pose"]["p"][1].n, v["pose"]["p"][2].n, v["w"].n, v["h"].n, room.c_str());
            check(ok, name + ": plan wall " + std::to_string(i));
        }
        const auto& pr = want["rooms"].a;
        check(R.rooms.size() == pr.size(), name + ": " + std::to_string(R.rooms.size()) + " rooms, the Python has " + std::to_string(pr.size()));
        for (size_t i = 0; i < std::min(R.rooms.size(), pr.size()); i++) {
            const auto& r = R.rooms[i]; const auto& v = pr[i];
            auto optEq = [&](bool have, double val, const json::Value& j) { return j.type == json::Value::Null ? !have : have && near(val, j.n); };
            // the outline up to rotation (its first vertex follows CPython's set order)
            const auto& fl = v["floor"].a; bool poly = r.floor.size() == fl.size();
            if (poly) { bool any = false;
                for (size_t sft = 0; sft < fl.size() && !any; sft++) { bool all = true; for (size_t k = 0; k < fl.size() && all; k++) all = near(r.floor[(k + sft) % fl.size()].first, fl[k][0].n) && near(r.floor[(k + sft) % fl.size()].second, fl[k][1].n); any = all; }
                poly = any; }
            bool ok = r.id == v["id"].str() && near(r.area, v["area"].n) && optEq(r.haveFloor, r.floorY, v["floorY"]) && optEq(r.haveCeil, r.ceilingY, v["ceilingY"]) &&
                      r.closed == v["closed"].b && near(r.closure, v["closure"].n) && near(r.perimeter, v["perimeter"].n) && near(r.openM, v["openM"].n) && r.doors == (int)v["doors"].a.size() && poly;
            if (!ok) printf("    room %zu: c++ %s area %.3f closed %d closure %.3f per %.3f doors %d verts %zu | py %s area %.3f closed %d closure %.3f per %.3f doors %zu verts %zu\n", i, r.id.c_str(), r.area, r.closed, r.closure, r.perimeter, r.doors, r.floor.size(),
                            v["id"].str().c_str(), v["area"].n, v["closed"].b, v["closure"].n, v["perimeter"].n, v["doors"].a.size(), fl.size());
            check(ok, name + ": room " + std::to_string(i));
        }
        planWalls += R.walls.size(); planRooms += R.rooms.size();
        {   const auto& sc = want["score"]; bool ok = R.score.ok == (sc["verdict"].str() == "satisfactory");
            bool rs = R.score.reasons.size() == sc["reasons"].a.size();
            for (size_t i = 0; rs && i < R.score.reasons.size(); i++) rs = R.score.reasons[i] == sc["reasons"][i].str();
            if (!rs) { for (auto& r : R.score.reasons) printf("    c++: %s\n", r.c_str()); for (auto& r : sc["reasons"].a) printf("    py:  %s\n", r.str().c_str()); }
            bool nums = R.score.dupRaw == sc["duplicates"]["raw"].integer() && R.score.dupPlan == sc["duplicates"]["plan"].integer() &&
                        (sc["explained"].type == json::Value::Null ? !R.score.haveExplained : std::fabs(r3c(R.score.explained) - sc["explained"].n) < 1e-9) &&
                        R.score.pairs == sc["sceneFit"]["pairs"].integer() && R.score.frameSuspect == sc["frameSuspect"].integer();
            suspects += R.score.frameSuspect;
            check(ok && rs && nums, name + ": score (verdict, reasons word for word, duplicates, explained, Meta pairs)");
            verdicts += ok && rs; okRooms += R.score.ok; }
        // budget: best of 20 runs (a worker thread, but not slower than 2 ms)
        double best = 1e18;
        for (int r = 0; r < 20; r++) { auto t0 = std::chrono::steady_clock::now(); consolidate::Result Q; consolidate::run(doc, Q); best = std::min(best, std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count()); }
        if (name == "house-rev25-framed") { bestUs = best; slowCase = name;
            double bp = 1e18; for (int r = 0; r < 10; r++) { auto t0 = std::chrono::steady_clock::now(); consolidate::Result Q; consolidate::run(doc, Q, {}, true); bp = std::min(bp, std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count()); }
            planUs = bp; }
    }
    printf("  %d cases match the Python: worst %.4f mm, %.5f deg; %zu plan walls, %zu rooms; house-rev25 (44 walls) in %.0f us, with the floor plan %.0f us\n", cases, worstMm, worstDeg, planWalls, planRooms, bestUs, planUs);
    // The live hint: where-words from the head, and the reason order on a real case.
    check(consolidate::where(0, 0, 0, -1, 0, -2) == "AHEAD" && consolidate::where(0, 0, 0, -1, 0, 2) == "BEHIND YOU" &&
          consolidate::where(0, 0, 0, -1, 2, 0) == "TO YOUR RIGHT" && consolidate::where(0, 0, 0, -1, -2, 0) == "TO YOUR LEFT", "hint directions (forward -z, right +x)");
    {   json::Value doc = load(dir + "/flat-open.doc.json"); consolidate::Result R; consolidate::run(doc, R, {}, true);
        auto h = consolidate::hint(R, 0, 0, 0, -1);
        printf("  flat-open hint: \"%s\"\n", h.text.c_str());
        check(h.text == "NOT YET: WALL TO YOUR RIGHT UNSEEN", "the flat without its east wall, looking down -z: the wall to the right is unseen");
        json::Value d2 = load(dir + "/house-rev25-framed.doc.json"); consolidate::Result R2; consolidate::run(d2, R2, {}, true);
        auto h2 = consolidate::hint(R2, 0, 0, 0, -1); printf("  house-rev25 hint: \"%s\" (%s)\n", h2.text.c_str(), R2.score.ok ? "satisfactory" : "not yet");
        check(h2.good == R2.score.ok, "the hint says GOOD exactly when the verdict is satisfactory"); }
    printf("  scores: %d of %d verdicts with every reason word for word (%d satisfactory); %d frame-suspect walls (consolidate/2)\n", verdicts, cases, okRooms, suspects);
    check(suspects > 0, "consolidate/2: the stored rev 25 has frame-suspect depth walls");
    check(cases >= 10, "fixtures present");
    check(bestUs < 2000, "under 2 ms on house-rev25");
    check(planUs < 20000, "the floor plan under 20 ms on house-rev25 (it runs at ~1 Hz on a worker)");
    printf("consolidate: %s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
