// Selection outline (room_lab/select_outline.h). g++ -std=c++17 -I room_lab bench/select_outline_test.cpp && ./a.out
#include "select_outline.h"
#include <cmath>
#include <cstdio>
using namespace selout;
static int fails = 0;
static void check(bool c, const char* m) { if (!c) { printf("FAIL %s\n", m); fails++; } }
static bool near(float a, float b, float e = 1e-4f) { return std::fabs(a - b) < e; }

int main() {
    // A screen on a wall turned 30 degrees about y: local coordinates come back from the wall's axes.
    float a = 0.5236f; Rect r; r.right = {std::cos(a), 0, -std::sin(a)}; r.up = {0, 1, 0}; r.normal = cross(r.right, r.up);
    r.c = {1.0f, 1.4f, -2.0f}; r.w = 1.2f; r.h = 0.675f;
    Style s;
    auto loc = [&](V p) { V d = p - r.c; return V{dot(d, r.right), dot(d, r.up), dot(d, r.normal)}; };

    Frame f = frame(r, s);
    bool plane = true, ring = true;
    for (int k = 0; k < 10; k++) {
        V l = loc(f.strip[k]); if (!near(l.z, s.lift)) plane = false;
        float ex = std::fabs(l.x) - r.w / 2, ey = std::fabs(l.y) - r.h / 2, want = k % 2 ? s.margin : s.margin + s.thick;
        if (!near(ex, want) || !near(ey, want)) ring = false;  // every corner sits exactly margin (inner) / margin+thick (outer) out
    }
    check(plane, "frame lies on the wall plane, lifted by Style.lift");
    check(ring, "inner corners margin outside the screen, outer corners margin + thick");
    check(near(loc(f.strip[0]).x, loc(f.strip[8]).x) && near(loc(f.strip[1]).y, loc(f.strip[9]).y), "strip closes on itself");
    bool facing = true, outside = true; float area = 0;
    for (int i = 0; i < 4; i++) {
        V q[4]; f.quad(i, q);
        for (int t = 0; t < 2; t++) {  // the two triangles (q0 q1 q2) and (q0 q2 q3)
            V n = cross(q[t + 1] - q[0], q[t + 2] - q[0]); float z = dot(n, r.normal);
            if (z <= 0) facing = false;
            area += z / 2;
        }
        for (int k = 0; k < 4; k++) { V l = loc(q[k]); if (std::fabs(l.x) < r.w / 2 + s.margin - 1e-5f && std::fabs(l.y) < r.h / 2 + s.margin - 1e-5f) outside = false; }
    }
    float ox = r.w + 2 * (s.margin + s.thick), oy = r.h + 2 * (s.margin + s.thick), ix = r.w + 2 * s.margin, iy = r.h + 2 * s.margin;
    check(facing, "every triangle faces the room (counter-clockwise from the front)");
    check(outside, "the frame never covers the screen");
    check(near(area, ox * oy - ix * iy, 1e-5f), "frame area = outer minus inner rectangle (no gaps, no overlaps)");
    // At the strongest pulse the frame stays clear of the move bar (room_lab.cpp: kBarGap 0.045 - kBarH/2 0.0175 below the screen).
    Selection sel; float maxScale = 0;
    sel.select(2, 0); for (float t = 0; t < 10; t += 0.01f) maxScale = std::fmax(maxScale, sel.look(2, t).thickScale);
    check(s.margin + s.thick * maxScale < 0.045f - 0.0175f, "pulsing frame never reaches the move bar");
    printf("  frame: %.0f mm out, %.0f mm wide (x%.2f at the pop), %.0f mm off the wall\n", s.margin * 1000, s.thick * 1000, maxScale, s.lift * 1000);

    // Label: above the top-left corner, clear of the frame; inside when the wall ends just above the screen.
    Label L = label(r, s, 7);
    V b0 = loc(L.badge[0]), b2 = loc(L.badge[2]), c = loc(L.centre);
    check(L.text == "7" && !L.inside, "number 7 above the screen");
    check(b0.y >= r.h / 2 + s.margin + s.thick, "badge above the frame");
    check(near(b0.x, -r.w / 2 - s.margin - s.thick), "badge aligned with the frame's left edge");
    check(near(L.px * 7, s.labelH), "digits labelH tall");
    check(b2.x - b0.x >= L.px * 5 + 2 * s.pad - 1e-5f && b2.y - b0.y >= L.px * 7 + 2 * s.pad - 1e-5f, "badge holds the digit with padding");
    check(near(c.x, (b0.x + b2.x) / 2) && near(c.y, (b0.y + b2.y) / 2) && c.z > b0.z && b0.z > s.lift, "digits centred, in front of the badge, badge in front of the frame");
    Label L2 = label(r, s, 12); check(loc(L2.badge[2]).x - loc(L2.badge[0]).x >= (12 - 1) * L2.px + 2 * s.pad - 1e-5f, "two digits fit");
    Rect low = r; low.topRoom = 0.05f; Label Li = label(low, s, 3);
    check(Li.inside && loc(Li.badge[2]).y <= r.h / 2 && loc(Li.badge[0]).x >= -r.w / 2, "no room above: badge inside the top-left corner");

    // State: select, re-select, clear one, clear all, fade.
    Selection q;
    q.select(1, 0); q.select(4, 2, 0.1f);
    check(q.selected(1) && q.selected(4) && !q.selected(2) && q.count() == 2, "two selected");
    check(q.look(4, 0.2f).number == 2, "explicit number shown");
    auto l0 = q.look(1, 0), l1 = q.look(1, 0.3f), l2 = q.look(1, 1.0f);
    check(l0.thickScale > 1.45f && l0.bright > 1.45f, "pop on select");
    check(l1.thickScale < 1.1f && l1.bright < 1.05f, "pop gone after 0.3 s");
    check(l2.visible && l2.alpha >= 0.85f && l2.alpha <= 1, "visible and opaque while selected");
    float lo = 9, hi = 0; for (float t = 2; t < 6; t += 0.01f) { float k = q.look(1, t).thickScale; lo = std::fmin(lo, k); hi = std::fmax(hi, k); }
    check(lo < 1.02f && hi > 1.15f && hi < 1.25f, "slow breathe after the pop");
    q.select(1, 5); check(q.look(1, 5).thickScale > 1.45f, "selecting again pops again");
    q.clear(1, 6); check(!q.selected(1) && q.count() == 1, "clear(id)");
    check(q.look(1, 6.1f).visible && q.look(1, 6.1f).alpha < 0.85f, "cleared: fading out");
    q.update(6.3f); check(!q.look(1, 6.3f).visible && q.items.size() == 1, "faded and dropped");
    q.select(1, 7); q.clear(7.5f); q.update(8); check(q.count() == 0 && q.items.empty(), "clear() all");
    check(!q.look(9, 8).visible, "unknown id not drawn");
    printf("select outline: %s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
