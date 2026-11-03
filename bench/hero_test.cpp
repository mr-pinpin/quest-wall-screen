// Hero selection (room_lab/vt.h vt::Heroes; docs/virtual-texturing.md v3).
//   g++ -std=c++17 -I room_lab bench/hero_test.cpp -o /tmp/hero_test && /tmp/hero_test
#include "vt.h"
#include <cstdio>
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
using namespace vt;
int main() {
    Heroes H;
    // Cinema: screen 2 big and in gaze; 3 and 11 peek at the edge (small, low pri): only 2.
    auto r = H.pick({{2, true, 3000, 1.0f}, {3, true, 300, 0.1f}, {11, true, 300, 0.1f}}, 0);
    CHECK(r.size() == 1 && r[0] == 2, "cinema: hero 2 only (%zu)", r.size());
    // Glance away for 2 s: 2 stays (hysteresis); after 3 s it returns to the atlas.
    r = H.pick({{2, true, 3000, 0.3f}}, 1.0); CHECK(r.size() == 1, "2 stays while failing < 3 s");
    r = H.pick({{2, true, 3000, 0.3f}}, 2.9); CHECK(r.size() == 1, "still at 2.9 s");
    r = H.pick({{2, true, 3000, 0.3f}}, 4.1); CHECK(r.empty(), "leaves after 3 s failing");
    // Back in gaze: joins at once.
    r = H.pick({{2, true, 3000, 1.0f}}, 4.2); CHECK(r.size() == 1, "joins at once");
    // A recovered hero resets its timer.
    H.pick({{2, true, 3000, 0.3f}}, 5.0); H.pick({{2, true, 3000, 1.0f}}, 7.0);
    r = H.pick({{2, true, 3000, 0.3f}}, 8.5); CHECK(r.size() == 1, "timer reset after recovering");
    // Wall of five big screens all in gaze: at most 3, the largest first.
    Heroes W; r = W.pick({{2, true, 1300, 1.0f}, {3, true, 2000, 1.0f}, {4, true, 1800, 0.9f}, {5, true, 1500, 1.0f}, {6, true, 2500, 0.85f}}, 0);
    CHECK(r.size() == 3 && std::find(r.begin(), r.end(), 2) == r.end(), "max 3, smallest left out");
    // Hidden or small screens are never heroes; a vanished hero (not in demand) leaves at once.
    Heroes V; V.pick({{2, true, 3000, 1.0f}}, 0); r = V.pick({{3, false, 3000, 1.0f}, {4, true, 900, 1.0f}}, 0.2);
    CHECK(r.empty(), "screen gone from demand leaves at once; hidden/small never join (%zu)", r.size());
    printf("%d failures\n", fails); return fails ? 1 : 0;
}
