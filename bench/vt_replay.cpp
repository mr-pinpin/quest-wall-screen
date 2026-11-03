// Replays recorded sender sessions (layout + the demand in force, one JSON line each: bench/results/*layouts*.jsonl)
// through the headset's VT residency logic (room_lab/vt.h vt::Resident) and reports what the user would see.
//   g++ -std=c++17 -I room_lab bench/vt_replay.cpp -o /tmp/vt_replay && /tmp/vt_replay [--step 0.4] FILE...
// Per visible screen and layout step: "blurry" = shows a cell coarser than the sharp image the headset already
// had (a mip tail right after a reveal). The old rule (copy whatever is visible) is replayed next to vt::Resident.
// Exit status 1 if vt::Resident ever shows a revealed screen blurrier than its held copy within the grace time.
#include "vt.h"
#include "json.h"
#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
    double step = 0.4; std::vector<std::string> files;
    for (int i = 1; i < argc; i++) { std::string a = argv[i]; if (a == "--step" && i + 1 < argc) step = atof(argv[++i]); else files.push_back(a); }
    long steps = 0, oldBlurry = 0, newBlurry = 0, kept = 0, reveals = 0;
    for (auto& f : files) {
        std::ifstream in(f); std::string line; double t = 0;
        std::map<int, vt::Resident> R; std::map<int, int> oldShown; std::map<int, bool> wasVis; std::map<int, double> oldReveal;
        while (std::getline(in, line)) {
            bool ok; json::Value r = json::parse(line, &ok); if (!ok || r["layout"].type != json::Value::Obj) continue;
            t += step; steps++;
            std::map<int, std::vector<vt::Cell>> cells;
            for (auto& c : r["layout"]["cells"].a) {
                vt::Cell x; x.w = c["w"].integer(0); x.h = c["h"].integer(0); x.lvl = c["lvl"].integer(-1);
                if (c["src"].size() == 4) { x.sx = c["src"][0].integer(0); x.sy = c["src"][1].integer(0); x.sw = c["src"][2].integer(0); x.sh = c["src"][3].integer(0); }
                cells[c["screen"].integer()].push_back(x);
            }
            for (auto& s : r["demand"]["screens"].a) {
                int id = s["id"].integer(); bool vis = s["vis"].integer(1) != 0;
                auto it = cells.find(id); if (it == cells.end()) continue;
                int lvl = vt::levelOf(it->second[0], vt::srcW(it->second));
                if (vis && wasVis.count(id) && !wasVis[id]) { reveals++; oldReveal[id] = t; }
                wasVis[id] = vis;
                // Old rule: hidden screens keep their image, visible ones always take the cell.
                if (vis) { if (oldShown.count(id) && lvl > oldShown[id] && oldReveal.count(id) && t - oldReveal[id] < 1.0) oldBlurry++; oldShown[id] = lvl; }
                else if (!oldShown.count(id)) oldShown[id] = lvl;
                vt::Resident& res = R[id]; int before = res.shownLvl; long keptBefore = res.keptSharp;
                if (res.shouldCopy(vis, lvl, t)) { if (vis && before >= 0 && lvl > before && t - res.revealedAt < 1.0) newBlurry++; res.copied(lvl); }
                kept += res.keptSharp - keptBefore;
            }
        }
    }
    printf("%ld layout steps, %ld reveals: old rule showed a blurrier cell %ld times after a reveal; vt::Resident %ld times (kept the sharp copy %ld times)\n",
           steps, reveals, oldBlurry, newBlurry, kept);
    return newBlurry ? 1 : 0;
}
