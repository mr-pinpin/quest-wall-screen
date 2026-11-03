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

// The coarsest level still at least px wide (pure; Level adds the hysteresis).
inline int levelFor(int srcWidth, int px, int tail = 4) {
    return std::min(tail, std::max(0, (int)std::floor(std::log2((double)srcWidth / std::max(px, 1)) + 1e-6)));
}

// Level choice per screen: detail goes up at once, down after 3 s (each change costs the sender a keyframe);
// a hidden screen wants the mip tail (the headset keeps its sharp copy anyway).
struct Level {
    int lvl = 0; double downSince = -1;
    int want(int srcWidth, int px, bool vis, double now, int tail = 4) {
        int w = !vis ? tail : levelFor(srcWidth, px, tail);
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

// Per-screen residency: whether this frame's atlas cell should be copied into the screen's layer.
//  - hidden: never, once the screen has an image (it keeps its last sharp copy; the sender may shrink it to a tail)
//  - just revealed: the sender's first layouts after a reveal can still carry the mip-tail cell (its resize waits
//    for the next refresh), so for `grace` seconds a cell coarser than the shown level is not copied: the sharp,
//    slightly stale image beats a blurry fresh one; after that, fresh content wins whatever its level.
//  - otherwise: always (level changes the user asked for, by distance or size, apply at once).
struct Resident {
    int shownLvl = -1; bool visible = false; double revealedAt = -1;
    long skippedHidden = 0, keptSharp = 0;
    bool shouldCopy(bool vis, int cellLvl, double now, double grace = 1.0) {
        if (vis && !visible) revealedAt = now;
        visible = vis;
        if (!vis) { if (shownLvl >= 0) { skippedHidden++; return false; } return true; }
        if (shownLvl >= 0 && cellLvl > shownLvl && revealedAt >= 0 && now - revealedAt < grace) { keptSharp++; return false; }
        return true;
    }
    void copied(int lvl) { shownLvl = lvl; }
};

// LOD by viewer pose (docs/sources.md roadmap 1): how many source pixels across a screen are worth sending, from
// its projection into the view. A screen (centre c, unit axes right/up, size w x h metres) seen from `eye`: the
// angles it spans horizontally and vertically (edge-on screens span little, so obliquity counts), times the
// headset's pixels per radian, give the texels it covers; the level rule needs that as a source width, so the
// vertical span is converted through the source aspect. Plain vector maths, tested along pose paths offline.
struct Vec3 { float x, y, z; };
inline Vec3 vsub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 vadd(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 vmul(Vec3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
inline float vdot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float vlen(Vec3 a) { return std::sqrt(vdot(a, a)); }
inline float angleBetween(Vec3 a, Vec3 b) {
    float c = vdot(a, b) / std::max(1e-6f, vlen(a) * vlen(b));
    return std::acos(std::fmin(1.0f, std::fmax(-1.0f, c)));
}
inline int projectedPx(Vec3 eye, Vec3 c, Vec3 right, Vec3 up, float w, float h, int srcW, int srcH, float pxPerRad = 1260.0f) {
    Vec3 l = vsub(vsub(c, vmul(right, w / 2)), eye), r = vsub(vadd(c, vmul(right, w / 2)), eye);
    Vec3 b = vsub(vsub(c, vmul(up, h / 2)), eye), t = vsub(vadd(c, vmul(up, h / 2)), eye);
    float acrossW = angleBetween(l, r) * pxPerRad, acrossH = angleBetween(b, t) * pxPerRad;
    float scale = std::max(acrossW / std::max(1, srcW), acrossH / std::max(1, srcH));  // source texels per source pixel
    return (int)std::lround(std::min(1.0f, scale) * srcW);
}
// Note: one uniform level must satisfy both axes, so turning around a screen horizontally does not lower its level
// (its height still spans the same angle); only distance, or obliquity in both axes, does. Per-axis (anisotropic)
// levels would need non-uniform tiles: a possible later step, not done.


// Hero streams (docs/virtual-texturing.md v3): which screens leave the atlas for their own stream. A candidate is
// visible, at least 1280 px across (projected) and in gaze (pri >= 0.8); at most `max` heroes, best (pri * px)
// first. A hero joins at once and stays until it has failed the rule for 3 s (each change costs keyframes).
struct HeroIn { int id; bool vis; int px; float pri; };
struct Heroes {
    std::map<int, double> failingSince; std::vector<int> cur;
    std::vector<int> pick(const std::vector<HeroIn>& in, double now, int max = 3) {
        auto ok = [](const HeroIn& h) { return h.vis && h.px >= 1280 && h.pri >= 0.8f; };
        std::vector<int> keep;
        for (int id : cur) {
            const HeroIn* h = nullptr; for (auto& x : in) if (x.id == id) h = &x;
            if (h && ok(*h)) { failingSince.erase(id); keep.push_back(id); continue; }
            double& t = failingSince.emplace(id, now).first->second;
            if (h && now - t < 3) keep.push_back(id); else failingSince.erase(id);
        }
        std::vector<HeroIn> cand; for (auto& x : in) if (ok(x) && std::find(keep.begin(), keep.end(), x.id) == keep.end()) cand.push_back(x);
        std::sort(cand.begin(), cand.end(), [](const HeroIn& a, const HeroIn& b) { return a.pri * a.px > b.pri * b.px; });
        for (auto& c : cand) if ((int)keep.size() < max) keep.push_back(c.id);
        cur = keep; return cur;
    }
};

// Keyframe requests (pairs with the sender's noperiodic flag: no periodic keyframes, so the headset asks). The demand
// carries a counter, "keyReq": n; any change makes the sender key the atlas and every hero (a counter survives lost
// datagrams and 5 Hz repeats). It goes up on a decoder error, a format change, or 1.5 s without a shown frame while
// packets keep arriving; at most one request per 0.5 s.
struct KeyReq {
    long count = 0; double last = -1e9, lastShown = -1, lastPacket = -1;
    bool request(double now) { if (now - last < 0.5) return false; last = now; count++; return true; }
    bool decodeError(double now) { return request(now); }
    bool formatChanged(double now) { return request(now); }
    void shown(double now) { lastShown = now; }
    void packet(double now) { lastPacket = now; if (lastShown < 0) lastShown = now; }
    // Call every frame: a stall (packets flowing, nothing shown for 1.5 s) asks for a keyframe, then again each 1.5 s.
    bool tick(double now) {
        if (lastPacket < 0 || now - lastPacket > 1.0 || lastShown < 0) return false;
        if (now - lastShown > 1.5 && now - last > 1.5) return request(now);
        return false;
    }
};

// Hold rule: a screen that shows an image and gets a new panel size keeps the old image until the new one exists.
inline bool needsHold(bool hasImage, int cw, int ch, int contentW, int contentH) { return hasImage && (cw != contentW || ch != contentH); }

}  // namespace vt
