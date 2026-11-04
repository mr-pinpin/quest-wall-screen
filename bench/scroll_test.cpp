// Wave-to-scroll gesture (room_lab/scroll.h). g++ -std=c++17 -I room_lab bench/scroll_test.cpp && ./a.out
#include "scroll.h"
#include <cstdio>
#include <cstdlib>
using namespace scrollg;
static int fails = 0;
static void check(bool c, const char* m) { if (!c) { printf("FAIL %s\n", m); fails++; } }
static Input hand(float t, float x, float y, bool open = true, bool pinch = false, float nz = -1) {
    Input in; in.t = t; in.tracked = true; in.open = open; in.pinch = pinch;
    in.head = {0, 1.6f, 0}; in.right = {1, 0, 0}; in.up = {0, 1, 0};
    in.palm = {x, 1.4f + y, -0.4f}; in.palmNormal = {0, 0, nz};  // nz -1: palm faces away from the face
    return in;
}
int main() {
    // A 15 cm upward wave over 0.5 s: about 600 units (5 notches), vertical only.
    Wave w; int sy = 0, sx = 0; float t = 0;
    for (int i = 0; i <= 10; i++, t += 1 / 72.0f) w.update(hand(t, 0, 0));  // hold still to engage
    check(w.engaged && !w.active, "engaged, not yet scrolling");
    for (int i = 1; i <= 36; i++, t += 1 / 72.0f) { Out o = w.update(hand(t, 0.002f * std::sin(i * 0.7f), 0.15f * i / 36)); sy += o.dy; sx += o.dx; }
    check(std::abs(sy - 600) <= 2, "15 cm -> 600 units");
    check(sx == 0, "axis locked: horizontal jitter ignored");
    // Small jitter under 2 cm never scrolls.
    Wave j; int js = 0; t = 0;
    for (int i = 0; i < 200; i++, t += 1 / 72.0f) { Out o = j.update(hand(t, 0.008f * std::sin(i * 0.3f), 0.008f * std::cos(i * 0.2f))); js += o.dx + o.dy; }
    check(js == 0, "jitter under 2 cm sends nothing");
    // Pinching, a closed hand or a palm facing the user: nothing.
    Wave p; int ps = 0; t = 0;
    for (int i = 0; i < 80; i++, t += 1 / 72.0f) { Out o = p.update(hand(t, 0, 0.003f * i, true, true)); ps += o.dy; }
    check(ps == 0, "pinch never scrolls");
    Wave c; int cs = 0; t = 0;
    for (int i = 0; i < 80; i++, t += 1 / 72.0f) { Out o = c.update(hand(t, 0, 0.003f * i, false)); cs += o.dy; }
    check(cs == 0, "closed hand never scrolls");
    Wave f; int fs = 0; t = 0;
    for (int i = 0; i < 80; i++, t += 1 / 72.0f) { Out o = f.update(hand(t, 0, 0.003f * i, true, false, 1)); fs += o.dy; }
    check(fs == 0, "palm toward the face never scrolls");
    // Sideways wave: horizontal units, sign follows the hand; down wave negative.
    Wave h; int hx = 0, hy = 0; t = 0;
    for (int i = 0; i < 10; i++, t += 1 / 72.0f) h.update(hand(t, 0, 0));
    for (int i = 1; i <= 30; i++, t += 1 / 72.0f) { Out o = h.update(hand(t, -0.1f * i / 30, 0.004f)); hx += o.dx; hy += o.dy; }
    check(hx < -380 && hx > -420 && hy == 0, "left wave: horizontal, negative");
    // Losing tracking resets: a new wave must engage again.
    h.update(Input{}); check(!h.engaged && !h.active, "tracking lost resets");
    printf("scroll gesture: %s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
