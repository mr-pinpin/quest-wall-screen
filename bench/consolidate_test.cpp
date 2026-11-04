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
    double bestUs = 1e18; std::string slowCase;
    while (cf >> name) {
        json::Value doc = load(dir + "/" + name + ".doc.json"), want = load(dir + "/" + name + ".want.json");
        consolidate::Result R; consolidate::run(doc, R); cases++;
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
        // budget: best of 20 runs (a worker thread, but not slower than 2 ms)
        double best = 1e18;
        for (int r = 0; r < 20; r++) { auto t0 = std::chrono::steady_clock::now(); consolidate::Result Q; consolidate::run(doc, Q); best = std::min(best, std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count()); }
        if (name == "house-rev25") { bestUs = best; slowCase = name; }
    }
    printf("  %d cases match the Python: worst %.4f mm, %.5f deg; house-rev25 (44 walls) in %.0f us\n", cases, worstMm, worstDeg, bestUs);
    check(cases >= 10, "fixtures present");
    check(bestUs < 2000, "under 2 ms on house-rev25");
    printf("consolidate: %s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
