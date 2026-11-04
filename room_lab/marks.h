// DRAW primitive (kanban K1, flag MARKS): house["marks"] (written by work-42's room.sh draw/clear) are labels, arrows,
// rectangles and circles on the walls so work-df can point at things for the user ("this wall", "here"), composed
// with select. A mark sits at a wall position (house wall id + x, y in that wall's frame, the frame of screens[].x/y)
// or relative to a screen (screen id + x, y offset from its centre). Colour (#rrggbb or a name), optional expiry
// (ttl seconds after ts, or until). This header parses, expires and turns each mark into flat geometry in its wall's
// frame (quads + text runs); room_lab.cpp maps that onto the wall and draws it on top (the CTLTOP batch).
// GL-free; bench/marks_test.cpp.
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <string>
#include <vector>
#include "json.h"

namespace marks {

struct P { float x = 0, y = 0; };
struct Quad { P c[4]; float col[4]; };           // wall-frame corners, counter-clockwise from the room
struct TextRun { float x, y, px; std::string s; float col[4]; };  // centred, 5x7 font pixel size px

enum class Kind { Label, Arrow, Rect, Circle };
struct Mark {
    std::string id, wall, text, ts; int screen = -1; Kind kind = Kind::Label;
    float x = 0, y = 0, w = 0.3f, h = 0.2f, r = 0.15f, dx = 0, dy = -0.3f; float col[4] = {1.0f, 0.78f, 0.22f, 1};
    double until = 0;  // unix seconds; 0 = no expiry
};

// "2026-11-04T10:15:00Z" (fractional seconds allowed) -> unix seconds; 0 if unparsable.
inline double isoTime(const std::string& s) {
    int Y, M, D, h, m; double sec;
    if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%lf", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
    // days from civil (Howard Hinnant), no timegm/locale needed
    Y -= M <= 2; long era = (Y >= 0 ? Y : Y - 399) / 400; unsigned yoe = (unsigned)(Y - era * 400);
    unsigned doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097 + (long)doe - 719468;
    return days * 86400.0 + h * 3600 + m * 60 + sec;
}
inline bool color(const std::string& s, float* c) {
    struct N { const char* n; float r, g, b; };
    static const N names[] = {{"gold", 1.0f, 0.78f, 0.22f}, {"red", 1.0f, 0.3f, 0.25f}, {"green", 0.3f, 0.9f, 0.45f}, {"blue", 0.35f, 0.7f, 1.0f}, {"white", 0.95f, 0.97f, 1.0f}};
    std::string l = s; for (auto& ch : l) ch = (char)std::tolower((unsigned char)ch);
    for (auto& n : names) if (l == n.n) { c[0] = n.r; c[1] = n.g; c[2] = n.b; c[3] = 1; return true; }
    unsigned v; if (l.size() == 7 && l[0] == '#' && sscanf(l.c_str() + 1, "%6x", &v) == 1) { c[0] = (v >> 16 & 255) / 255.0f; c[1] = (v >> 8 & 255) / 255.0f; c[2] = (v & 255) / 255.0f; c[3] = 1; return true; }
    return false;
}
inline float clampSize(float v) { return std::min(5.0f, std::max(0.02f, v)); }
inline std::string upper(const std::string& s) { std::string o; for (unsigned char c : s) o += (char)std::toupper(c); if (o.size() > 40) o = o.substr(0, 40); return o; }

// house["marks"] -> marks (at most 64; unknown kinds, missing anchors and bad numbers skipped).
inline std::vector<Mark> parse(const json::Value& house) {
    std::vector<Mark> out; const json::Value& a = house["marks"];
    if (a.type != json::Value::Arr) return out;
    for (auto& e : a.a) {
        if (out.size() >= 64) break;
        if (e.type != json::Value::Obj) continue;
        Mark m; m.id = e["id"].str(); std::string k = e["kind"].str();
        if (k == "label") m.kind = Kind::Label; else if (k == "arrow") m.kind = Kind::Arrow; else if (k == "rect") m.kind = Kind::Rect; else if (k == "circle") m.kind = Kind::Circle; else continue;
        if (e["wall"].type == json::Value::Str && !e["wall"].str().empty()) m.wall = e["wall"].str();
        else if (e["screen"].type == json::Value::Num) m.screen = e["screen"].integer(-1);
        else continue;
        if (e["x"].type != json::Value::Num || e["y"].type != json::Value::Num) continue;
        m.x = (float)e["x"].n; m.y = (float)e["y"].n;
        if (e["w"].type == json::Value::Num) m.w = clampSize((float)e["w"].n);
        if (e["h"].type == json::Value::Num) m.h = clampSize((float)e["h"].n);
        if (e["r"].type == json::Value::Num) m.r = clampSize((float)e["r"].n);
        if (e["dx"].type == json::Value::Num) m.dx = std::max(-5.0f, std::min(5.0f, (float)e["dx"].n));
        if (e["dy"].type == json::Value::Num) m.dy = std::max(-5.0f, std::min(5.0f, (float)e["dy"].n));
        m.text = upper(e["text"].str()); m.ts = e["ts"].str();
        if (e["color"].type == json::Value::Str) color(e["color"].str(), m.col);
        double ts = isoTime(m.ts);
        if (e["ttl"].type == json::Value::Num && ts > 0) m.until = ts + e["ttl"].n;
        else if (e["until"].type == json::Value::Str) m.until = isoTime(e["until"].str());
        if (m.kind == Kind::Label && m.text.empty()) continue;
        out.push_back(m);
    }
    return out;
}
inline bool expired(const Mark& m, double nowUnix) { return m.until > 0 && nowUnix >= m.until; }

// Geometry in the wall frame. pop: 1 right after the mark appears, decaying to 0 (thicker lines, like select's pop).
struct Geo { std::vector<Quad> quads; std::vector<TextRun> texts; };
inline void seg(Geo& g, P a, P b, float wdt, const float* col) {
    float dx = b.x - a.x, dy = b.y - a.y, L = std::sqrt(dx * dx + dy * dy); if (L < 1e-6f) return;
    float nx = -dy / L * wdt / 2, ny = dx / L * wdt / 2;
    Quad q{{{a.x + nx, a.y + ny}, {a.x - nx, a.y - ny}, {b.x - nx, b.y - ny}, {b.x + nx, b.y + ny}}, {col[0], col[1], col[2], col[3]}};
    // counter-clockwise from the room (+z): flip if needed
    float cr = (q.c[1].x - q.c[0].x) * (q.c[2].y - q.c[0].y) - (q.c[1].y - q.c[0].y) * (q.c[2].x - q.c[0].x);
    if (cr < 0) std::swap(q.c[1], q.c[3]);
    g.quads.push_back(q);
}
inline void caption(Geo& g, const std::string& text, float x, float y, const float* col) {
    if (text.empty()) return;
    float px = 0.012f; float tw = (6.0f * text.size() - 1) * px, th = 7 * px, pad = 0.015f;
    const float bg[4] = {0.06f, 0.06f, 0.08f, 0.85f * col[3]};
    g.quads.push_back({{{x - tw / 2 - pad, y - th / 2 - pad}, {x + tw / 2 + pad, y - th / 2 - pad}, {x + tw / 2 + pad, y + th / 2 + pad}, {x - tw / 2 - pad, y + th / 2 + pad}}, {bg[0], bg[1], bg[2], bg[3]}});
    g.texts.push_back({x, y, px, text, {col[0], col[1], col[2], col[3]}});
}
// The mark at wall-frame position (x, y) (the caller resolved a screen anchor to its wall position already).
inline Geo geometry(const Mark& m, float x, float y, float pop = 0, float alpha = 1) {
    Geo g; float col[4] = {m.col[0], m.col[1], m.col[2], m.col[3] * alpha}; float lw = 0.014f * (1 + 0.6f * pop);
    switch (m.kind) {
        case Kind::Label: caption(g, m.text, x, y, col); break;
        case Kind::Rect: {
            float hw = m.w / 2, hh = m.h / 2; P a{x - hw, y - hh}, b{x + hw, y - hh}, c{x + hw, y + hh}, d{x - hw, y + hh};
            seg(g, a, b, lw, col); seg(g, b, c, lw, col); seg(g, c, d, lw, col); seg(g, d, a, lw, col);
            caption(g, m.text, x, y + hh + 0.06f, col); break;
        }
        case Kind::Circle: {
            const int n = 32;
            for (int i = 0; i < n; i++) { float a0 = 6.2831853f * i / n, a1 = 6.2831853f * (i + 1) / n;
                seg(g, {x + m.r * std::cos(a0), y + m.r * std::sin(a0)}, {x + m.r * std::cos(a1), y + m.r * std::sin(a1)}, lw, col); }
            caption(g, m.text, x, y + m.r + 0.06f, col); break;
        }
        case Kind::Arrow: {
            P a{x, y}, b{x + m.dx, y + m.dy}; float L = std::sqrt(m.dx * m.dx + m.dy * m.dy); if (L < 1e-4f) break;
            float ux = m.dx / L, uy = m.dy / L, head = std::min(0.12f, L * 0.4f);
            P base{b.x - ux * head, b.y - uy * head};
            seg(g, a, base, lw * 1.4f, col);
            P l{base.x - uy * head * 0.6f, base.y + ux * head * 0.6f}, r{base.x + uy * head * 0.6f, base.y - ux * head * 0.6f};
            Quad tri{{b, l, base, r}, {col[0], col[1], col[2], col[3]}};  // a kite: the head as one quad
            float cr = (tri.c[1].x - tri.c[0].x) * (tri.c[2].y - tri.c[0].y) - (tri.c[1].y - tri.c[0].y) * (tri.c[2].x - tri.c[0].x);
            if (cr < 0) std::swap(tri.c[1], tri.c[3]);
            g.quads.push_back(tri);
            caption(g, m.text, a.x - ux * 0.08f, a.y - uy * 0.08f, col); break;  // the caption at the tail
        }
    }
    return g;
}

// Appearance over time: a pop when a mark (id + ts) first shows, a 0.5 s fade before it expires.
struct Life {
    struct Seen { std::string id, ts; float since; };
    std::vector<Seen> seen;
    float pop(const Mark& m, float now) {
        for (auto& s : seen) if (s.id == m.id) { if (s.ts != m.ts) { s.ts = m.ts; s.since = now; } return std::exp(-std::max(0.0f, now - s.since) / 0.07f); }
        seen.push_back({m.id, m.ts, now}); return 1;
    }
    static float alpha(const Mark& m, double nowUnix) { return m.until > 0 ? (float)std::min(1.0, std::max(0.0, (m.until - nowUnix) / 0.5)) : 1.0f; }
};

}  // namespace marks
