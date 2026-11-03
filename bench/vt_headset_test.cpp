// Desktop test of the headset's virtual-texturing logic (room_lab/vt.h, docs/virtual-texturing.md).
//   g++ -std=c++17 -O1 -I room_lab bench/vt_headset_test.cpp -o /tmp/vt_test && /tmp/vt_test [layouts.txt ...]
// Built-in cases, then every layout line in the given files (sender logs: lines containing {"atlas":...}).
// For each screen: its tiles cover the panel image exactly once (no seam, no overlap), stay inside the atlas,
// and don't overlap another screen's cells.
#include "vt.h"
#include <fstream>
#include <iostream>

static int fails = 0, checked = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void checkLayout(const std::string& line, const char* where) {
    vt::Layout L = vt::parse(line);
    if (L.screens.empty()) return;
    checked++;
    std::vector<unsigned char> atlas((size_t)std::max(1, L.W) * std::max(1, L.H), 0);
    for (auto& [id, cells] : L.screens) {
        for (auto& c : cells) {
            CHECK(c.x >= 0 && c.y >= 0 && c.x + c.w <= L.W && c.y + c.h <= L.H, "%s screen %d cell %d,%d %dx%d outside atlas %dx%d", where, id, c.x, c.y, c.w, c.h, L.W, L.H);
            CHECK(c.w <= 1920 && c.h <= 1200, "%s screen %d cell %dx%d over the 1920x1200 tile cap", where, id, c.w, c.h);
            for (int y = std::max(0, c.y); y < std::min(L.H, c.y + c.h); y++)
                for (int x = std::max(0, c.x); x < std::min(L.W, c.x + c.w); x++) {
                    CHECK(atlas[(size_t)y * L.W + x] == 0, "%s screen %d overlaps another cell at %d,%d", where, id, x, y);
                    if (atlas[(size_t)y * L.W + x]) goto next; atlas[(size_t)y * L.W + x] = 1;
                }
            next:;
        }
        int cw, ch; auto ts = vt::tiles(cells, cw, ch);
        CHECK(cw > 0 && ch > 0, "%s screen %d empty panel image", where, id);
        std::vector<unsigned char> cover((size_t)cw * ch, 0);
        for (auto& t : ts)
            for (int y = t.dy; y < t.dy + t.dh; y++) for (int x = t.dx; x < t.dx + t.dw; x++) {
                if (x < 0 || y < 0 || x >= cw || y >= ch) { CHECK(false, "%s screen %d tile outside its image", where, id); goto done; }
                cover[(size_t)y * cw + x]++;
            }
        {
            int gaps = 0, overlaps = 0;
            for (auto v : cover) { gaps += v == 0; overlaps += v > 1; }
            CHECK(gaps == 0 && overlaps == 0, "%s screen %d (%zu tiles, %dx%d): %d seam px, %d overlap px", where, id, ts.size(), cw, ch, gaps, overlaps);
            for (auto& t : ts) CHECK(std::abs(t.dw - t.w) <= 1 && std::abs(t.dh - t.h) <= 1, "%s screen %d tile resampled %dx%d -> %dx%d", where, id, t.w, t.h, t.dw, t.dh);
        }
        done:;
    }
}

int main(int argc, char** argv) {
    // v1 layout: one cell per screen, no src.
    checkLayout(R"({"atlas":[3840,1200],"cells":[{"screen":2,"x":0,"y":0,"w":1920,"h":1200},{"screen":3,"x":1920,"y":0,"w":960,"h":600}]})", "v1");
    // The spec's example: screen 2 at lvl 0 as two tiles, screen 3 a mip tail.
    checkLayout(R"({"atlas":[3840,2400],"gen":17,"cells":[{"screen":2,"x":0,"y":0,"w":1920,"h":1200,"lvl":0,"src":[0,0,1920,1200]},{"screen":2,"x":1920,"y":0,"w":1920,"h":1200,"lvl":0,"src":[1920,0,1920,1200]},{"screen":3,"x":0,"y":1200,"w":240,"h":150,"lvl":4,"src":[0,0,3840,2400]}]})", "spec");
    // Cinema: a 3840x2160 screen at lvl 0 as four tiles, shrunk 10% by the sender's fit loop (odd sizes).
    checkLayout(R"({"atlas":[3456,1944],"gen":3,"cells":[{"screen":5,"x":0,"y":0,"w":1728,"h":972,"lvl":0,"src":[0,0,1920,1080]},{"screen":5,"x":1728,"y":0,"w":1728,"h":972,"lvl":0,"src":[1920,0,1920,1080]},{"screen":5,"x":0,"y":972,"w":1728,"h":972,"lvl":0,"src":[0,1080,1920,1080]},{"screen":5,"x":1728,"y":972,"w":1728,"h":972,"lvl":0,"src":[1920,1080,1920,1080]}]})", "cinema-shrunk");
    // Uneven split: 2560 wide as 1920 + 640, scaled by 0.73 (rounding on both tiles).
    checkLayout(R"({"atlas":[1872,1052],"gen":4,"cells":[{"screen":6,"x":0,"y":0,"w":1402,"h":1052,"lvl":0,"src":[0,0,1920,1440]},{"screen":6,"x":1402,"y":0,"w":467,"h":1052,"lvl":0,"src":[1920,0,640,1440]}]})", "uneven");

    // v2 fields parse in any order.
    { vt::Layout L = vt::parse(R"({"gen":9,"atlas":[100,50],"cells":[{"screen":4,"x":1,"y":2,"w":30,"h":20,"src":[5,6,60,40],"lvl":1}]})");
      auto& c = L.screens[4][0];
      CHECK(L.gen == 9 && L.W == 100 && c.lvl == 1 && c.sx == 5 && c.sw == 60 && c.sh == 40, "parse order"); }

    // Levels: up at once, down only after 3 s, hidden = tail, back in view = up at once.
    { vt::Level v; int src = 3840;
      CHECK(v.want(src, 3800, true, 0) == 0, "full detail");
      CHECK(v.want(src, 900, true, 1) == 0, "no shrink before 3 s");
      CHECK(v.want(src, 900, true, 3.5) == 0, "still waiting at 2.5 s");
      CHECK(v.want(src, 900, true, 4.2) == 2, "shrinks after 3 s (3840/900 -> lvl 2, 960 wide), got %d", v.lvl);
      CHECK(v.want(src, 2000, true, 4.3) == 0, "grows at once (2000 > 1920 needs lvl 0), got %d", v.lvl);
      CHECK(v.want(src, 1900, true, 4.4) == 0 && v.want(src, 1900, true, 8) == 1, "1900 px -> lvl 1 after 3 s, got %d", v.lvl);
      CHECK(v.want(src, 2000, false, 5) == 4, "hidden -> tail");
      CHECK(v.want(src, 3840, true, 5.2) == 0, "revealed -> full at once");
      CHECK(v.want(1920, 1920, true, 6) == 0 && v.want(1920, 1921, true, 6.1) == 0, "px above src stays at lvl 0"); }

    // Hold rule.
    CHECK(vt::needsHold(true, 960, 600, 1920, 1200) && !vt::needsHold(false, 960, 600, 1920, 1200) && !vt::needsHold(true, 1920, 1200, 1920, 1200), "hold rule");
    CHECK(vt::levelOf(vt::Cell{0, 0, 960, 600}, 1920) == 1 && vt::levelOf(vt::Cell{0, 0, 240, 150, 4}, 3840) == 4, "levelOf");

    for (int i = 1; i < argc; i++) {
        std::ifstream f(argv[i]); std::string line; int n = 0;
        while (std::getline(f, line)) {
            line.erase(std::remove_if(line.begin(), line.end(), [](char c) { return c == 0x20 || c == 0x09; }), line.end());  // json.dumps spacing
            size_t p = line.find("{\"atlas\":"); if (p == std::string::npos) continue;
            n++; checkLayout(line.substr(p), (std::string(argv[i]) + ":" + std::to_string(n)).c_str());
        }
        printf("%s: %d layouts\n", argv[i], n);
    }
    printf("%d layouts checked, %d failures\n", checked, fails);
    return fails ? 1 : 0;
}
