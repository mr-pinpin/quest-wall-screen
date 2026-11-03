// Virtual texturing, headset side (docs/virtual-texturing.md): the parts with no GL or OpenXR in them, so they are
// tested on a desktop against real sender layouts (bench/vt_headset_test.cpp).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace vt {

// One atlas cell. lvl and src (the part of the Mac display it covers, display pixels) are protocol v2; a v1
// sender leaves lvl -1 and sw 0.
struct Cell { int x = 0, y = 0, w = 0, h = 0, lvl = -1, sx = 0, sy = 0, sw = 0, sh = 0; };
// A source rectangle in the atlas (top-left origin) and where it lands in the screen's panel image.
struct Tile { int sx, sy, w, h, dx, dy, dw, dh; };

struct Layout {
    int W = 0, H = 0; long gen = -1;
    std::map<int, std::vector<Cell>> screens;  // a screen may come as several cells (tiles)
};

// {"atlas":[W,H],"gen":17,"cells":[{"screen":2,"x":0,"y":0,"w":1920,"h":1200,"lvl":0,"src":[0,0,1920,1200]},...]}
inline Layout parse(const std::string& m) {
    Layout L;
    size_t a = m.find("\"atlas\":["); if (a != std::string::npos) sscanf(m.c_str() + a + 9, "%d,%d", &L.W, &L.H);
    size_t g = m.find("\"gen\":"); if (g != std::string::npos) L.gen = atol(m.c_str() + g + 6);
    for (size_t p = m.find("{\"screen\":"); p != std::string::npos; p = m.find("{\"screen\":", p + 1)) {
        int id; Cell c;
        if (sscanf(m.c_str() + p, "{\"screen\":%d,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d", &id, &c.x, &c.y, &c.w, &c.h) != 5) continue;
        std::string cell = m.substr(p, m.find('}', p) - p);  // v2 fields, in any order
        size_t l = cell.find("\"lvl\":"); if (l != std::string::npos) c.lvl = atoi(cell.c_str() + l + 6);
        size_t s = cell.find("\"src\":["); if (s != std::string::npos) sscanf(cell.c_str() + s + 7, "%d,%d,%d,%d", &c.sx, &c.sy, &c.sw, &c.sh);
        L.screens[id].push_back(c);
    }
    return L;
}

// The Mac display's width in pixels, from v2 src rects; 1920 (the v1 cell cap) without them.
inline int srcW(const std::vector<Cell>& cells) {
    int w = 0; for (auto& c : cells) w = std::max(w, c.sx + c.sw);
    return w > 0 ? w : 1920;
}

// The tiles of one screen in its panel image, and that image's size. Tile edges come from one scale applied to
// display coordinates, so neighbouring tiles share their edge exactly: no seam, no overlap, whatever the rounding.
inline std::vector<Tile> tiles(const std::vector<Cell>& cells, int& cw, int& ch) {
    std::vector<Tile> out; cw = ch = 0;
    if (cells.empty()) return out;
    if (cells.size() == 1 && cells[0].sw <= 0) {  // v1: the cell is the whole screen
        const Cell& c = cells[0]; cw = c.w; ch = c.h; out.push_back({c.x, c.y, c.w, c.h, 0, 0, c.w, c.h}); return out;
    }
    int sw = 0, sh = 0; double ax = 0, ay = 0;
    for (auto& c : cells) { sw = std::max(sw, c.sx + c.sw); sh = std::max(sh, c.sy + c.sh); ax += c.w; ay += c.h; }
    double sumSw = 0, sumSh = 0; for (auto& c : cells) { sumSw += std::max(1, c.sw); sumSh += std::max(1, c.sh); }
    double kx = ax / sumSw, ky = ay / sumSh;  // the level's scale, averaged so per-cell rounding doesn't skew it
    auto X = [&](int s) { return (int)std::lround(s * kx); }; auto Y = [&](int s) { return (int)std::lround(s * ky); };
    cw = X(sw); ch = Y(sh);
    for (auto& c : cells) out.push_back({c.x, c.y, c.w, c.h, X(c.sx), Y(c.sy), X(c.sx + c.sw) - X(c.sx), Y(c.sy + c.sh) - Y(c.sy)});
    return out;
}

// Level choice per screen: detail goes up at once, down after 3 s (each change costs the sender a keyframe);
// a hidden screen wants the mip tail (the headset keeps its sharp copy anyway).
struct Level {
    int lvl = 0; double downSince = -1;
    int want(int srcWidth, int px, bool vis, double now, int tail = 4) {
        int w = !vis ? tail : std::min(tail, std::max(0, (int)std::floor(std::log2((double)srcWidth / std::max(px, 1)) + 1e-6)));
        if (w < lvl || !vis) { lvl = w; downSince = -1; }
        else if (w > lvl) { if (downSince < 0) downSince = now; else if (now - downSince > 3) { lvl = w; downSince = -1; } }
        else downSince = -1;
        return lvl;
    }
};

// The level a screen shows: its cell's lvl (v2), else derived from its width.
inline int levelOf(const Cell& c, int srcWidth) {
    if (c.lvl >= 0) return c.lvl;
    return std::max(0, (int)std::lround(std::log2((double)srcWidth / std::max(1, c.w))));
}

// Hold rule: a screen that shows an image and gets a new panel size keeps the old image until the new one exists.
inline bool needsHold(bool hasImage, int cw, int ch, int contentW, int contentH) { return hasImage && (cw != contentW || ch != contentH); }

}  // namespace vt
