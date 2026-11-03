// The headset's budget arbiter (docs/3d-content.md, "One budget, split by mode"): one place that splits the
// network between the screen atlas, the game stream and asset downloads, and sheds screen detail before the
// headset ever drops a frame. No GL or OpenXR here, so it is unit-tested on a desktop (bench/budget_test.cpp).
#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace budget {

enum class Mode { Wall, Cinema, Game };
inline const char* name(Mode m) { return m == Mode::Cinema ? "cinema" : m == Mode::Game ? "game" : "wall"; }

struct Screen { int id; bool vis; int px; float pri; };  // the demand's per-screen fields

struct Input {
    std::vector<Screen> screens;  // extra screens only (not the main panel)
    bool gameActive = false;      // a game/desktop stream fills the main view
    int gameWantKbps = 50000;     // what the game stream asks for
    int linkKbps = 200000;        // usable Wi-Fi throughput to the headset (conservative default, to measure)
    double frameMs = 0;           // headset GPU frame time, smoothed
    double now = 0;               // seconds
};

struct Output {
    Mode mode = Mode::Wall;
    int atlasKbps = 0, gameKbps = 0, assetKbps = 0;
    int lvlBias = 0;              // extra levels of coarseness for unfocused screens (pri < 0.5) under GPU pressure
    std::vector<std::pair<int, int>> screenKbps;  // per visible screen: its share of the atlas
};

// Kbps a visible screen is worth at its on-headset size: ~8 Mbit/s for a screen spanning 1920 px at 60 fps,
// scaling with area (px squared), at least 300 kbps so small screens still move.
inline int screenWorth(int px) { double k = std::min(px, 3840) / 1920.0; return std::max(300, (int)(8000 * k * k)); }

struct Arbiter {
    static constexpr int kAtlasCap = 40000;    // the sender's own ceiling (bench check: atlas within 40 Mbit/s)
    static constexpr int kTailKbps = 150;       // all-tails trickle: VR runs measured ~0.1 Mbit/s
    int bias = 0; double hotSince = -1, coolSince = -1;

    static Mode modeOf(const Input& in) {
        if (in.gameActive) return Mode::Game;
        int vis = 0; bool big = false;
        for (auto& s : in.screens) if (s.vis) { vis++; big |= s.pri >= 0.9f && s.px >= 1500; }
        return vis == 1 && big ? Mode::Cinema : Mode::Wall;
    }

    Output split(const Input& in) {
        Output o; o.mode = modeOf(in);
        // Atlas: what the visible screens are worth, weighted by gaze, capped; hidden screens cost only the tail
        // trickle, so hiding one frees its share at once.
        double want = 0; std::vector<std::pair<int, double>> w;
        for (auto& s : in.screens) if (s.vis) {
            double v = screenWorth(s.px) * (0.25 + 0.75 * std::clamp(s.pri, 0.0f, 1.0f));
            w.push_back({s.id, v}); want += v;
        }
        int cap = std::min(kAtlasCap, in.linkKbps / 2);
        if (o.mode == Mode::Game) cap = std::min(cap, in.linkKbps / 5);  // screens around a game get the leftovers
        o.atlasKbps = std::max(kTailKbps, (int)std::min<double>(want, cap));
        double k = want > 0 ? std::min(1.0, cap / want) : 0;
        for (auto& [id, v] : w) o.screenKbps.push_back({id, (int)(v * k)});
        // Game: first claim on the link in game mode, never more than 60% of it.
        o.gameKbps = o.mode == Mode::Game ? std::min(in.gameWantKbps, in.linkKbps * 3 / 5) : 0;
        // Assets: whatever is left, at most a fifth of the link, nothing while a game streams (they'd add jitter).
        int left = in.linkKbps - o.atlasKbps - o.gameKbps;
        o.assetKbps = o.mode == Mode::Game ? 0 : std::max(0, std::min(left, in.linkKbps / 5));
        // GPU: above 12 ms (72 Hz leaves 13.9) for 1 s, unfocused screens drop a level (up to 2); back after 3 s
        // under 10 ms. Detail goes before frames do.
        if (in.frameMs > 12) {
            coolSince = -1;
            if (hotSince < 0) hotSince = in.now;
            else if (in.now - hotSince >= 1 && bias < 2) { bias++; hotSince = in.now; }
        } else if (in.frameMs > 0 && in.frameMs < 10) {
            hotSince = -1;
            if (coolSince < 0) coolSince = in.now;
            else if (in.now - coolSince >= 3 && bias > 0) { bias--; coolSince = in.now; }
        } else { hotSince = coolSince = -1; }
        o.lvlBias = bias;
        return o;
    }
};

}  // namespace budget
