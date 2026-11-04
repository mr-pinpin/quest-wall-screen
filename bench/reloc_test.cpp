// RELOCCHECK sampling (room_lab/reloc.h). g++ -std=c++17 -I room_lab bench/reloc_test.cpp && ./a.out
// Replays the 2026-11-04 08:44-08:48Z walk: steep / far views with wild gaps must not count; a real 35 cm shift seen
// from in front must; old samples age out.
#include "reloc.h"
#include "anchors.h"
#include <cstdio>
using namespace reloc;
static int fails = 0;
static void check(bool c, const char* m) { if (!c) { printf("FAIL %s\n", m); fails++; } }
int main() {
    Pose wall{anchors::yawQuat(0), {0, 1.5f, -3}};  // facing +z, screen at its centre
    check(frontal(wall, 0, 0, {0, 1.6f, -1}), "2 m straight in front");
    check(frontal(wall, 0, 0, {1.2f, 1.6f, -1}), "31 deg off: counts");
    check(!frontal(wall, 0, 0, {2.5f, 1.6f, -2}), "68 deg off (beside the wall): ignored");
    check(!frontal(wall, 0, 0, {0, 1.6f, 1.5f}), "4.5 m away: ignored");
    check(frontal(wall, 0, 0, {0, 1.6f, -5}), "behind (sign convention): by distance and angle only");
    check(frontal(wall, 1.0f, 0, {1.0f, 1.6f, -1.2f}), "screen off the wall centre: its own centre");
    // The walk: frontal and aligned (3 cm) for 60 s, then 2 min of steep views reading -7 .. +100 cm.
    Window w; double t = 0;
    for (; t < 60; t += 2) w.add(t, 0.03f, 0.95f, true);
    check(std::fabs(w.median() - 0.03f) < 1e-4f, "aligned");
    float wild[] = {-0.07f, -0.19f, 1.0f, 1.08f, -0.16f, 0.9f};
    int bad = 0;
    for (int i = 0; t < 180; t += 2, i++) { w.add(t, wild[i % 6], 0.6f + 0.3f * (i % 2), false); float m = w.median(); bad += std::isfinite(m) && std::fabs(m) > 0.2f; }
    check(bad == 0, "steep views never push the median over 20 cm");
    check(!std::isfinite(w.median()), "no frontal sample for 30 s: no verdict (not the last value)");
    // Low coverage from the front: ignored too.
    for (int i = 0; i < 10; i++, t += 2) w.add(t, 0.5f, 0.4f, true);
    check(!std::isfinite(w.median()), "valid 0.4: ignored");
    // A real shift: 35 cm seen from the front for 20 s -> the median says so within 6 samples.
    int k = 0; for (; k < 10; k++, t += 2) { w.add(t, 0.35f, 0.9f, true); if (std::isfinite(w.median()) && w.median() > 0.2f) break; }
    check(k <= 5, "real 35 cm shift detected from the front");
    printf("reloc: %s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
