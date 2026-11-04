// AMBI bezel under VT (flag BEZELFEW): a bezel strip samples the screen's edge colours from the atlas, and with VT a
// screen arrives as several tiles, so the strip must be cut wherever the tile under it changes. bezelFrame used to
// cut every strip into 16 equal pieces (7 rings x 4 strips x 16 x 7 screens = 3136 quads a frame); the cuts only have
// to fall on the tile boundaries, usually 1 to 3 pieces. Also: the uv of a piece end is sampled a hair inside the piece,
// so an end on a boundary maps into its own tile, not the neighbour's. GL-free; bench/vt_strip_test.cpp.
#pragma once
#include <algorithm>
#include <vector>
#include "vt.h"

namespace vtstrip {

// Cut positions (panel metres, sorted, including a and b) for a strip from a to b along x (alongX) or y. The panel
// spans [lo, lo + size] on that axis; tiles are in source pixels, x from the left, y from the TOP (so flipY for
// the panel's y-up axis).
inline std::vector<float> cuts(const std::vector<vt::Cell>& cells, bool alongX, float a, float b, float lo, float size) {
    std::vector<float> c = {a, b};
    if (cells.size() > 1 && cells[0].sw > 0 && size > 0) {
        int sw = 0, sh = 0; for (auto& t : cells) { sw = std::max(sw, t.sx + t.sw); sh = std::max(sh, t.sy + t.sh); }
        for (auto& t : cells) for (int e : {alongX ? t.sx : t.sy, alongX ? t.sx + t.sw : t.sy + t.sh}) {
            if (e <= 0 || e >= (alongX ? sw : sh)) continue;  // the panel's own edges: past them the uv clamps to the same tile
            float f = alongX ? (float)e / sw : 1.0f - (float)e / sh;  // fraction of the panel from lo
            float p = lo + f * size;
            if (p > a + 1e-5f && p < b - 1e-5f) c.push_back(p);
        }
    }
    std::sort(c.begin(), c.end()); c.erase(std::unique(c.begin(), c.end(), [](float x, float y) { return y - x < 1e-5f; }), c.end());
    return c;
}
// Where to sample the uv for a piece end at p in a piece [p0, p1]: a hair inside it.
inline float inside(float p, float p0, float p1) { float e = std::min(1e-4f, (p1 - p0) * 0.01f); return p <= p0 ? p0 + e : p >= p1 ? p1 - e : p; }

}  // namespace vtstrip
