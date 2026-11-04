// Selection outline (kanban 2026-11-04: "select + draw primitives: a gold outline + number on a screen"): a gold
// frame around a screen's rectangle on its wall plus a big number badge, for the assistant and the user to say
// "screen 2". Geometry only: the frame is a closed quad strip (outer/inner pairs, mitred corners) a margin outside
// the screen, lifted off the wall so it never z-fights the wall mesh or the screen; the badge sits above the
// top-left corner (inside the frame when the wall has no room above). Select/clear state with a pulse: a pop on
// select (thicker, brighter, decaying in ~0.2 s), then a slow breathe; a cleared outline fades out in fadeS.
// The caller draws frame.quad(i) and the badge with its Batch, and the digits with its 5x7 font at Label.px.
// GL-free; bench/select_outline_test.cpp.
#pragma once
#include <cmath>
#include <string>
#include <vector>

namespace selout {

struct V { float x = 0, y = 0, z = 0; };
inline V operator+(V a, V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V operator-(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V operator*(V a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V cross(V a, V b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

// A screen on a wall: centre and the wall's axes in world space (right, up, normal toward the room), size in m.
// topRoom: how far the wall extends above the screen's top edge (m); < 0 means unknown (plenty).
struct Rect { V c, right{1, 0, 0}, up{0, 1, 0}, normal{0, 0, 1}; float w = 1, h = 0.6f, topRoom = -1; };

struct Style {
    float margin = 0.01f;  // gap between the screen edge and the frame's inner edge
    float thick = 0.011f;  // frame width at rest (x1.5 at the pop: still clear of the move bar, 27.5 mm below)
    float lift = 0.035f;   // off the wall, in front of the screen (screens sit at 0.02, buttons/bars at 0.02-0.024)
    float labelH = 0.11f;  // digit height
    float pad = 0.018f;    // badge padding around the digits
    float gold[4] = {1.0f, 0.78f, 0.22f, 1.0f}, ink[4] = {0.10f, 0.08f, 0.04f, 0.92f};  // frame and badge / digits
};

struct Frame {
    V strip[10];  // outer0, inner0, outer1, inner1, ... outer0, inner0: closed strip, corners bottom-left counter-clockwise
    // Quad i (0..3: bottom, right, top, left) as 4 corners counter-clockwise seen from the room.
    void quad(int i, V q[4]) const { q[0] = strip[2 * i + 1]; q[1] = strip[2 * i]; q[2] = strip[2 * i + 2]; q[3] = strip[2 * i + 3]; }
};
struct Label {
    std::string text; V centre, right, up; float px = 0;  // digits centred at centre, pixel size px for a 5x7 font
    V badge[4];                                            // badge corners, counter-clockwise from the room
    bool inside = false;                                   // no room above the screen: drawn inside its top-left corner
};

inline Frame frame(const Rect& r, const Style& s, float thickScale = 1) {
    Frame f; float t = s.thick * thickScale, ix = r.w / 2 + s.margin, iy = r.h / 2 + s.margin, ox = ix + t, oy = iy + t;
    const float cx[4] = {-1, 1, 1, -1}, cy[4] = {-1, -1, 1, 1};
    V base = r.c + r.normal * s.lift;
    for (int k = 0; k <= 4; k++) {
        int c = k % 4;
        f.strip[2 * k] = base + r.right * (cx[c] * ox) + r.up * (cy[c] * oy);
        f.strip[2 * k + 1] = base + r.right * (cx[c] * ix) + r.up * (cy[c] * iy);
    }
    return f;
}

inline Label label(const Rect& r, const Style& s, int number) {
    Label L; L.text = std::to_string(number); L.right = r.right; L.up = r.up;
    L.px = s.labelH / 7;
    float tw = ((int)L.text.size() * 6 - 1) * L.px, bw = std::fmax(tw, s.labelH) + 2 * s.pad, bh = s.labelH + 2 * s.pad;
    float outer = s.margin + s.thick;
    L.inside = r.topRoom >= 0 && r.topRoom < outer + bh + 0.01f;
    float x0 = -r.w / 2 - outer, y0 = L.inside ? r.h / 2 - bh - 0.01f : r.h / 2 + outer + 0.01f;  // badge bottom-left
    if (L.inside) x0 = -r.w / 2 + 0.01f;
    V base = r.c + r.normal * (s.lift + 0.002f);
    auto P = [&](float x, float y) { return base + r.right * x + r.up * y; };
    L.badge[0] = P(x0, y0); L.badge[1] = P(x0 + bw, y0); L.badge[2] = P(x0 + bw, y0 + bh); L.badge[3] = P(x0, y0 + bh);
    L.centre = P(x0 + bw / 2, y0 + bh / 2) + r.normal * 0.002f;
    return L;
}

// Selection state with the pulse. Times in seconds on any monotonic clock.
struct Selection {
    float popS = 0.2f, breatheHz = 0.7f, fadeS = 0.25f;
    struct Entry { int id, number; float since, cleared; };  // cleared < 0 while selected
    std::vector<Entry> items;

    void select(int id, int number, float now) {
        for (auto& e : items) if (e.id == id) { e.number = number; e.since = now; e.cleared = -1; return; }  // again: pop again
        items.push_back({id, number, now, -1});
    }
    void select(int id, float now) { select(id, id, now); }
    void clear(int id, float now) { for (auto& e : items) if (e.id == id && e.cleared < 0) e.cleared = now; }
    void clear(float now) { for (auto& e : items) if (e.cleared < 0) e.cleared = now; }
    bool selected(int id) const { for (auto& e : items) if (e.id == id) return e.cleared < 0; return false; }
    int count() const { int n = 0; for (auto& e : items) n += e.cleared < 0; return n; }

    // Drop faded entries; call once a frame before drawing.
    void update(float now) {
        std::vector<Entry> keep;
        for (auto& e : items) if (e.cleared < 0 || now - e.cleared < fadeS) keep.push_back(e);
        items.swap(keep);
    }
    struct Look { bool visible = false; float thickScale = 1, alpha = 0, bright = 1; int number = 0; };
    Look look(int id, float now) const {
        Look l;
        for (auto& e : items) if (e.id == id) {
            float a = now - e.since, pop = std::exp(-std::fmax(a, 0.0f) / (popS / 3));     // 1 at select, ~0.05 after popS
            float breathe = 0.5f - 0.5f * std::cos(6.2831853f * breatheHz * std::fmax(a - popS, 0.0f));  // 0..1, starts at 0
            l.visible = true; l.number = e.number;
            l.thickScale = 1 + 0.5f * pop + 0.2f * breathe * (1 - pop);
            l.bright = 1 + 0.5f * pop;
            l.alpha = 0.85f + 0.15f * std::fmax(pop, breathe);
            if (e.cleared >= 0) l.alpha *= std::fmax(0.0f, 1 - (now - e.cleared) / fadeS);
            if (l.alpha <= 0) l.visible = false;
        }
        return l;
    }
};

}  // namespace selout
