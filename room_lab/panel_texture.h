// Text panels as one texture (the SETTINGS panel cost ~40k CPU quads a frame, fps 43-53, 2026-11-04): the panel's
// static content (background, buttons in their on/off colours, labels, hints, headings, title) is described as boxes
// and texts in panel-local metres, rasterised into an RGBA image only when that description changes, and drawn as
// one textured quad. Hover, the pointer ring and the grab bar stay as a few live quads on top, so pointing never
// re-renders. Text uses the app's 5x7 font through a glyph callback (room_lab.cpp's glyph(), bit 16 = left column)
// at the same metrics as its text(): (6n - 1) px wide, 7 px tall, centred; glyph pixels are area-sampled, so sizes
// that are not whole texels stay even. Pixels are 0xAABBGGRR (assist.h's rgba()), row 0 at the top, straight alpha.
// Hit-testing reads the same boxes (Box.hit >= 0), so what is drawn is what is pinched.
//
// Drop-in (room_lab.cpp, drawMenu): paneltex::Panel menuTex; each frame
//   if (menuTex.set(paneltex::fromGrid(menuGrid, labels, on, "SETTINGS"))) {}  // cheap compare; dirty on change
//   if (menuTex.dirty()) { auto& img = menuTex.render(glyph); glBindTexture(GL_TEXTURE_2D, tex);
//       glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, menuTex.w, menuTex.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img.data());
//       glGenerateMipmap(GL_TEXTURE_2D); }
//   then one quad at the panel's corners with the chat panels' textured program (drawChatPanels), plus the hover
//   highlight (one translucent quad over menuGrid.cells[k]) and the bar as today. GL-free; bench/panel_texture_test.cpp.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include "ui_grid.h"

namespace paneltex {

using GlyphFn = const uint8_t* (*)(char);
inline uint32_t rgba(int r, int g, int b, int a) { return (uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16 | (uint32_t)a << 24; }
inline uint32_t rgbaf(const float* c) {
    auto q = [](float v) { return (int)std::lround(std::fmin(std::fmax(v, 0.0f), 1.0f) * 255); };
    return rgba(q(c[0]), q(c[1]), q(c[2]), q(c[3]));
}

struct Box { float x, y, w, h; uint32_t col; int hit = -1; };   // centre and size (m); hit id for hit(), -1 = none
struct Text { float cx, cy, px; std::string s; uint32_t col; }; // centre, glyph pixel size (m)
struct Content {
    float halfW = 0.1f, halfH = 0.1f; uint32_t bg = 0;
    std::vector<Box> boxes; std::vector<Text> texts;              // drawn in order: boxes, then texts
    std::string key() const {                                     // everything that changes pixels
        std::string k; auto f = [&](float v) { k += std::to_string(std::lround(v * 1e5)); k += ','; };
        f(halfW); f(halfH); k += std::to_string(bg) + ';';
        for (auto& b : boxes) { f(b.x); f(b.y); f(b.w); f(b.h); k += std::to_string(b.col) + ';'; }
        for (auto& t : texts) { f(t.cx); f(t.cy); f(t.px); k += t.s; k += '\x1f'; k += std::to_string(t.col) + ';'; }
        return k;
    }
};

struct Panel {
    int maxTexels = 8192;         // either side (Quest 3's GL_MAX_TEXTURE_SIZE is 16384; the full 49-item panel is 3.7 m tall)
    float maxPixels = 8e6f;       // and at most 32 MB of RGBA
    float texelsPerGlyphPx = 2.5f;// the smallest text gets at least this many texels per font pixel
    float minTexelsPerM = 600;
    Content c; std::string ckey; bool dirty_ = true; long version = 0, renders = 0;
    int w = 0, h = 0; float tpm = 0; std::vector<uint32_t> img;

    // New content: returns true (and marks dirty) only if it changes the pixels.
    bool set(const Content& n) {
        std::string k = n.key(); if (k == ckey && !img.empty()) return false;
        c = n; ckey = k; dirty_ = true; return true;
    }
    bool dirty() const { return dirty_; }
    void size() {
        float minPx = 1; for (auto& t : c.texts) if (t.px > 0) minPx = std::min(minPx, t.px);
        tpm = std::max(minTexelsPerM, texelsPerGlyphPx / minPx);
        tpm = std::min({tpm, maxTexels / (2 * c.halfW), maxTexels / (2 * c.halfH), std::sqrt(maxPixels / (4 * c.halfW * c.halfH))});
        w = std::max(1, (int)std::ceil(2 * c.halfW * tpm)); h = std::max(1, (int)std::ceil(2 * c.halfH * tpm));
    }
    // Panel metres -> texel coordinates (x right, y down from the top edge).
    float tx(float x) const { return (x + c.halfW) * tpm; }
    float ty(float y) const { return (c.halfH - y) * tpm; }
    // Panel metres -> texture uv (v down, like the image rows).
    void uv(float x, float y, float& u, float& v) const { u = (x + c.halfW) / (2 * c.halfW); v = (c.halfH - y) / (2 * c.halfH); }

    static void blend(uint32_t& d, uint32_t s, float cov) {
        float a = ((s >> 24) & 255) / 255.0f * cov; if (a <= 0) return;
        float da = ((d >> 24) & 255) / 255.0f, oa = a + da * (1 - a); uint32_t out = 0;
        for (int k = 0; k < 3; k++) {
            float sc = ((s >> (8 * k)) & 255), dc = ((d >> (8 * k)) & 255);
            float oc = oa > 0 ? (sc * a + dc * da * (1 - a)) / oa : 0;
            out |= (uint32_t)std::lround(std::fmin(oc, 255.0f)) << (8 * k);
        }
        d = out | (uint32_t)std::lround(oa * 255) << 24;
    }
    // Area coverage of [x0,x1) x [y0,y1) (texels): blended with col, or added into a coverage window (ox, oy, cw x ch).
    struct Cov { std::vector<float> v; int ox = 0, oy = 0, cw = 0, ch = 0; };
    void rect(float x0, float y0, float x1, float y1, Cov* cov, uint32_t col) {
        int lx = cov ? cov->ox : 0, ly = cov ? cov->oy : 0, hx = cov ? cov->ox + cov->cw : w, hy = cov ? cov->oy + cov->ch : h;
        int ix0 = std::max(lx, (int)std::floor(x0)), ix1 = std::min(hx, (int)std::ceil(x1));
        int iy0 = std::max(ly, (int)std::floor(y0)), iy1 = std::min(hy, (int)std::ceil(y1));
        for (int y = iy0; y < iy1; y++) {
            float cy = std::fmin(y + 1.0f, y1) - std::fmax((float)y, y0); if (cy <= 0) continue;
            for (int x = ix0; x < ix1; x++) {
                float cx = std::fmin(x + 1.0f, x1) - std::fmax((float)x, x0); if (cx <= 0) continue;
                if (cov) cov->v[(size_t)(y - cov->oy) * cov->cw + (x - cov->ox)] += cx * cy; else blend(img[(size_t)y * w + x], col, cx * cy);
            }
        }
    }
    // Rasterise if dirty; returns the image (w x h).
    const std::vector<uint32_t>& render(GlyphFn glyph) {
        if (!dirty_) return img;
        size(); img.assign((size_t)w * h, c.bg);
        for (auto& b : c.boxes) rect(tx(b.x - b.w / 2), ty(b.y + b.h / 2), tx(b.x + b.w / 2), ty(b.y - b.h / 2), nullptr, b.col);
        Cov cov;
        for (auto& t : c.texts) {
            int n = (int)t.s.size(); if (!n || !glyph) continue;
            float x0 = t.cx - (n * 6 - 1) * t.px / 2, y0 = t.cy + 3.5f * t.px;  // as room_lab.cpp text()
            int bx0 = std::max(0, (int)std::floor(tx(x0))), bx1 = std::min(w, (int)std::ceil(tx(x0 + n * 6 * t.px)));
            int by0 = std::max(0, (int)std::floor(ty(y0))), by1 = std::min(h, (int)std::ceil(ty(y0 - 7 * t.px)));
            if (bx0 >= bx1 || by0 >= by1) continue;
            cov.ox = bx0; cov.oy = by0; cov.cw = bx1 - bx0; cov.ch = by1 - by0;
            cov.v.assign((size_t)cov.cw * cov.ch, 0);  // per string, so pixels that touch add up instead of over-blending
            for (int i = 0; i < n; i++) if (auto* g = glyph(t.s[i])) for (int r = 0; r < 7; r++) for (int cc = 0; cc < 5; cc++) if (g[r] & (16 >> cc)) {
                float x = x0 + (i * 6 + cc) * t.px, y = y0 - r * t.px;
                rect(tx(x), ty(y), tx(x + t.px), ty(y - t.px), &cov, 0);
            }
            for (int y = by0; y < by1; y++) for (int x = bx0; x < bx1; x++) { float v = cov.v[(size_t)(y - by0) * cov.cw + (x - bx0)]; if (v > 0) blend(img[(size_t)y * w + x], t.col, std::fmin(v, 1.0f)); }
        }
        dirty_ = false; version++; renders++;
        return img;
    }
    // Topmost hit-testable box under (x, y) panel metres: its hit id, or -1.
    int hit(float x, float y) const {
        for (int i = (int)c.boxes.size() - 1; i >= 0; i--) {
            const Box& b = c.boxes[i];
            if (b.hit >= 0 && std::fabs(x - b.x) < b.w / 2 && std::fabs(y - b.y) < b.h / 2) return b.hit;
        }
        return -1;
    }
    // What the per-frame quad path would draw for the same content (one quad per lit glyph pixel, per box).
    static long quadsPerFrame(const Content& c, GlyphFn glyph) {
        long q = 1 + (long)c.boxes.size();
        for (auto& t : c.texts) for (char ch : t.s) if (auto* g = glyph(ch)) for (int r = 0; r < 7; r++) for (int k = 0; k < 5; k++) q += (g[r] >> k) & 1;
        return q;
    }
};

// The SETTINGS panel from its UiGrid layout: background, title, section headings, one box per cell (hit id = the
// cell's index) in its on/off colour, its label and hint lines. Colours as in room_lab.cpp drawMenu().
struct GridStyle {
    uint32_t bg = rgba(13, 15, 20, 204), idle = rgba(41, 46, 61, 242), on = rgba(26, 115, 56, 242);
    uint32_t white = rgba(242, 247, 255, 255), dim = rgba(158, 173, 199, 255);
};
inline Content fromGrid(const UiGrid& g, const std::vector<std::string>& labels, const std::vector<bool>& on,
                        const std::string& title, const GridStyle& st = {}) {
    Content c; c.halfW = g.halfW; c.halfH = g.halfH; c.bg = st.bg;
    c.texts.push_back({0, g.titleY, UiGrid::fit(title, g.titlePx, 2 * g.halfW - 2 * g.pad, g.titleH * 0.6f), title, st.white});
    for (auto& hd : g.headings) c.texts.push_back({hd.x, hd.y, hd.px, hd.text, st.dim});
    for (size_t k = 0; k < g.cells.size(); k++) {
        const UiGrid::Cell& cl = g.cells[k];
        c.boxes.push_back({cl.x, cl.y, cl.w, cl.h, k < on.size() && on[k] ? st.on : st.idle, (int)k});
        if (k < labels.size()) c.texts.push_back({cl.x, cl.labelY, cl.px, labels[k], st.white});
        if (!cl.hint1.empty()) c.texts.push_back({cl.x, cl.hint1Y, cl.hintPx, cl.hint1, st.dim});
        if (!cl.hint2.empty()) c.texts.push_back({cl.x, cl.hint2Y, cl.hintPx, cl.hint2, st.dim});
    }
    return c;
}

}  // namespace paneltex
