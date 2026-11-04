// VT bezel strips cut on tile boundaries (room_lab/vt_strip.h). g++ -std=c++17 -I room_lab bench/vt_strip_test.cpp && ./a.out
#include "vt_strip.h"
#include <cmath>
#include <cstdio>
static int fails = 0;
static void check(bool c, const char* m) { if (!c) { printf("FAIL %s\n", m); fails++; } }
// The cell atlasAt picks for panel fraction (u, v-from-top).
static int cellAt(const std::vector<vt::Cell>& cells, float u, float v) {
    float ax, ay; vt::atlasAt(cells, u, v, ax, ay);
    for (size_t i = 0; i < cells.size(); i++) if (ax >= cells[i].x && ax <= cells[i].x + cells[i].w && ay >= cells[i].y && ay <= cells[i].y + cells[i].h) return (int)i;
    return -1;
}
int main() {
    // A 1920x1200 screen as a 2x2 grid of tiles at different atlas spots and levels; the panel is 1.6 x 1.0 m at (0,0).
    std::vector<vt::Cell> cells = {{0, 0, 960, 600, 0, 0, 0, 960, 600}, {1000, 0, 480, 300, 1, 960, 0, 960, 600},
                                   {0, 700, 960, 600, 0, 0, 600, 960, 600}, {1000, 400, 480, 300, 1, 960, 600, 960, 600}};
    const float l = -0.8f, w = 1.6f, bt = -0.5f, h = 1.0f;
    auto top = vtstrip::cuts(cells, true, l - 0.03f, l + w + 0.03f, l, w);   // the top strip overhangs both corners
    check(top.size() == 3 && std::fabs(top[1]) < 1e-5f, "top strip: one cut, at the tile boundary x = 0");
    auto left = vtstrip::cuts(cells, false, bt - 0.03f, bt + h + 0.03f, bt, h);
    check(left.size() == 3 && std::fabs(left[1]) < 1e-5f, "left strip: one cut at y = 0 (tiles counted from the top)");
    // Every piece samples a single tile, ends included (sampled a hair inside).
    bool single = true;
    for (int strip = 0; strip < 2; strip++) {
        auto& c = strip ? left : top;
        for (size_t i = 0; i + 1 < c.size(); i++) {
            float a = vtstrip::inside(c[i], c[i], c[i + 1]), b = vtstrip::inside(c[i + 1], c[i], c[i + 1]);
            for (float edge : {0.02f, 0.98f}) {  // near the panel edge the strip runs along
                float ua = strip ? edge : (a - l) / w, va = strip ? 1 - (a - bt) / h : edge, ub = strip ? edge : (b - l) / w, vb = strip ? 1 - (b - bt) / h : edge;
                if (cellAt(cells, ua, va) != cellAt(cells, ub, vb)) single = false;
            }
        }
    }
    check(single, "each piece lies in one tile: its ends sample the same tile");
    check(vtstrip::cuts({cells[0]}, true, l, l + w, l, w).size() == 2, "one tile: no cuts");
    int pieces = 0; for (auto* c : {&top, &left}) pieces += (int)c->size() - 1;
    printf("  2x2 tiles: %d pieces for a top + a left strip (16 + 16 before); 7 rings x 4 strips x 7 screens: %d quads (3136 before)\n", pieces, 7 * 2 * pieces * 7);
    printf("vt strip: %s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
