// LOD by viewer pose (room_lab/vt.h projectedPx + Level): pose paths through a room, checked offline.
//   g++ -std=c++17 -I room_lab bench/lod_test.cpp -o /tmp/lod_test && /tmp/lod_test
#include "vt.h"
#include <cstdio>
#include <random>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
using namespace vt;

int main() {
    // A 1.6 x 1.0 m screen on the wall z=0, centred at (0, 1.5, 0), facing +z; a 3840x2400 Mac display.
    Vec3 c{0, 1.5f, 0}, right{1, 0, 0}, up{0, 1, 0}; const float W = 1.6f, H = 1.0f; const int SW = 3840, SH = 2400;
    auto px = [&](Vec3 eye) { return projectedPx(eye, c, right, up, W, H, SW, SH); };
    auto lvl = [&](int p) { return levelFor(SW, p); };

    // Analytic check head-on at 2 m: width spans 2*atan(0.4) rad.
    int p2 = px({0, 1.5f, 2});
    float want = std::max(2 * std::atan(0.4f) * 1260, 2 * std::atan(0.25f) * 1260 * SW / SH);  // both axes, the larger
    CHECK(std::abs(p2 - (int)std::lround(want)) <= 2, "head-on 2 m: %d px, analytic %.0f", p2, want);

    // Approach from 6 m to 0.6 m: px grows monotonically, level falls monotonically to 0.
    int last = -1, lastL = 99;
    for (float d = 6; d >= 0.6f; d -= 0.1f) {
        int p = px({0, 1.5f, d}), L = lvl(p);
        CHECK(p >= last, "approach: px fell at %.1f m (%d < %d)", d, p, last);
        CHECK(L <= lastL, "approach: level rose at %.1f m", d);
        last = p; lastL = L;
    }
    CHECK(lastL == 0, "up close the full level 0 (got %d)", lastL);
    CHECK(lvl(px({0, 1.5f, 6})) >= 1, "at 6 m a coarser level than up close");

    // Horizontal orbit at 2 m: the height still spans the same angle, so a uniform level stays the same (honest:
    // only per-axis levels could exploit horizontal foreshortening).
    int head = px({0, 1.5f, 2}), edge = px({2 * std::sin(1.4835f), 1.5f, 2 * std::cos(1.4835f)});
    CHECK(std::abs(edge - head) <= head / 50 && lvl(edge) == lvl(head), "horizontal orbit keeps the level (%d vs %d px)", edge, head);
    // Oblique in both axes (from below and to the side, 60 deg each): both spans shrink, the level drops.
    float a = 1.0472f; int both = px({2 * std::sin(a) * std::cos(a), 1.5f - 2 * std::sin(a) * std::sin(a), 2 * std::cos(a)});
    CHECK(both < head, "oblique in both axes needs fewer texels (%d vs %d)", both, head);
    // A tall portrait screen: the vertical span decides.
    int portrait = projectedPx({0, 1.5f, 2}, c, right, up, 0.6f, 1.2f, 1200, 2400);
    CHECK(portrait > 1200 * 0.25f, "portrait: vertical extent counts (%d)", portrait);

    // Head jitter (+-2 cm, +-1 deg) while standing at 3 m for 30 s at 5 Hz: hysteresis means no level flicker.
    std::mt19937 rng(1); std::uniform_real_distribution<float> j(-0.02f, 0.02f);
    Level L; int changes = 0, prev = -1;
    for (int i = 0; i < 150; i++) {
        int p = px({j(rng), 1.5f + j(rng), 3 + j(rng)}), l = L.want(SW, p, true, i * 0.2);
        if (prev >= 0 && l != prev) { changes++; }
        prev = l;
    }
    CHECK(changes <= 1, "jitter at 3 m: %d level changes in 30 s", changes);
    // Walk-away / walk-back path: shrinking waits 3 s, growing is immediate.
    Level W2; double t = 0; int atNear = levelFor(SW, px({0, 1.5f, 1})); W2.lvl = atNear;
    for (int i = 0; i < 10; i++) { t += 0.2; W2.want(SW, px({0, 1.5f, 5}), true, t); }
    CHECK(W2.lvl == atNear, "walking away: no shrink within 2 s");
    for (int i = 0; i < 10; i++) { t += 0.2; W2.want(SW, px({0, 1.5f, 5}), true, t); }
    CHECK(W2.lvl > atNear, "walking away: shrinks after 3 s");
    t += 0.2; CHECK(W2.want(SW, px({0, 1.5f, 1}), true, t) == atNear, "walking back: full detail at once");

    printf("head-on 2 m %d px (lvl %d), 6 m %d px (lvl %d), 85 deg side %d px, 60/60 deg %d px (lvl %d)\n", head, lvl(head), px({0, 1.5f, 6}), lvl(px({0, 1.5f, 6})), edge, both, lvl(both));
    printf("%d failures\n", fails);
    return fails ? 1 : 0;
}
