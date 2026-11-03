// Video LOD ladder, offline (docs/sources.md roadmap 2/4): the headset's level per screen (room_lab/vt.h projectedPx +
// Level hysteresis, demand at 5 Hz) drives a rung switcher that changes rung only on segment boundaries (1 s, where
// every rung has an IDR: pre-encoded or cached video, work-09's prototype on the Mini). Counts switches per minute
// and time spent below the wanted detail along viewer paths.
//   g++ -std=c++17 -O1 -I room_lab bench/ladder_sim.cpp -o /tmp/ladder_sim && /tmp/ladder_sim
#include "vt.h"
#include <cstdio>
#include <functional>
#include <random>

using namespace vt;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct Result { double switchesPerMin, levelChangesPerMin, belowPct; };

// rungs: widths of the cached rungs, finest first; the rung for a level is the coarsest rung still >= srcW >> lvl
static Result run(std::function<Vec3(double)> path, double secs, std::vector<int> rungs) {
    Vec3 c{0, 1.5f, 0}, right{1, 0, 0}, up{0, 1, 0}; const float W = 1.6f, H = 1.0f; const int SW = 1920, SH = 1080;
    Level L; int prevL = -1, levelChanges = 0, rung = 0, switches = 0; double below = 0;
    auto rungFor = [&](int lvl) { int w = SW >> lvl, r = 0; for (int i = 0; i < (int)rungs.size(); i++) if (rungs[i] >= w) r = i; return r; };
    for (int i = 0; i * 0.2 < secs; i++) {
        double t = i * 0.2; int px = projectedPx(path(t), c, right, up, W, H, SW, SH);
        if (i == 0) { L.lvl = levelFor(SW, px); rung = rungFor(L.lvl); }  // start where the viewer starts: no startup switch
        int lvl = L.want(SW, px, true, t);
        if (prevL >= 0 && lvl != prevL) levelChanges++;
        prevL = lvl;
        if (i % 5 == 0) {  // a segment boundary every second: switch to the rung the current level asks for
            int want = rungFor(lvl);
            if (want != rung) { rung = want; switches++; }
        }
        if (rungs[rung] < std::min(SW, px)) below += 0.2;  // showing fewer pixels than the view resolves
    }
    return {switches * 60 / secs, levelChanges * 60 / secs, 100 * below / secs};
}

int main() {
    std::vector<int> rungs{1920, 960, 480, 240};
    std::mt19937 rng(7); std::normal_distribution<float> n(0, 1);
    auto walk = [](double t) -> Vec3 {  // approach 6 m -> 1 m, orbit at 1 m, walk away
        if (t < 20) return {0, 1.5f, (float)(6 - 5 * t / 20)};
        if (t < 40) { float a = (float)((t - 20) / 20 * 1.2 - 0.6); return {std::sin(a), 1.5f, std::cos(a)}; }
        return {0, 1.5f, (float)(1 + 5 * (t - 40) / 20)};
    };
    std::vector<Vec3> jit(1000); for (auto& v : jit) v = {0.03f * n(rng), 1.5f + 0.03f * n(rng), 3 + 0.03f * n(rng)};
    auto jitter = [&](double t) { return jit[(int)(t * 5) % jit.size()]; };
    std::vector<Vec3> rw(1501); Vec3 p{0, 1.6f, 3}, vel{0, 0, 0};
    for (auto& v : rw) { vel = {0.9f * vel.x + 0.08f * n(rng), 0, 0.9f * vel.z + 0.08f * n(rng)}; p.x = std::clamp(p.x + vel.x * 0.2f, -2.5f, 2.5f);
        p.z = std::clamp(p.z + vel.z * 0.2f, 0.7f, 6.0f); v = p; }
    auto room = [&](double t) { return rw[std::min<size_t>((size_t)(t * 5), rw.size() - 1)]; };
    auto cinema = [&](double t) { return Vec3{0.02f * std::sin((float)t * 0.7f), 1.2f + 0.02f * std::sin((float)t * 1.3f), 2.0f + 0.03f * std::sin((float)t * 0.4f)}; };

    struct P { const char* name; std::function<Vec3(double)> f; double secs; } paths[] = {
        {"walk (approach, orbit, away)", walk, 60}, {"standing at 3 m, head jitter", jitter, 60},
        {"room random walk", room, 300}, {"cinema seat at 2 m", cinema, 300}};
    for (auto& p : paths) {
        Result r = run(p.f, p.secs, rungs);
        printf("%-30s rung switches %.1f/min  headset level changes %.1f/min  below wanted %.1f%% of the time\n", p.name, r.switchesPerMin, r.levelChangesPerMin, r.belowPct);
        if (std::string(p.name).rfind("standing", 0) == 0 || std::string(p.name).rfind("cinema", 0) == 0)
            CHECK(r.switchesPerMin <= 1, "%s: no flicker (%.1f switches/min)", p.name, r.switchesPerMin);
        CHECK(r.switchesPerMin <= 12, "%s: at most 12 switches/min (%.1f)", p.name, r.switchesPerMin);
        CHECK(r.belowPct <= 15, "%s: below wanted detail at most 15%% of the time (%.1f%%)", p.name, r.belowPct);
    }
    printf("%d failures\n", fails);
    return fails ? 1 : 0;
}
