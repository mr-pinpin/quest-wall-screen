// Unit tests for the headset budget arbiter (room_lab/budget.h).
//   g++ -std=c++17 -I room_lab bench/budget_test.cpp -o /tmp/budget_test && /tmp/budget_test
#include "budget.h"
#include <cstdio>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
using namespace budget;

static int share(const Output& o, int id) { for (auto& [i, k] : o.screenKbps) if (i == id) return k; return 0; }

int main() {
    // Cinema: one big screen in focus, nine hidden.
    { Arbiter a; Input in; in.screens.push_back({2, true, 3000, 1.0f});
      for (int i = 3; i < 12; i++) in.screens.push_back({i, false, 0, 0});
      Output o = a.split(in);
      CHECK(o.mode == Mode::Cinema, "cinema mode, got %s", name(o.mode));
      CHECK(o.atlasKbps >= 15000 && o.atlasKbps <= 25000, "cinema atlas %d (target <= 25 Mbit/s, enough for 3000 px)", o.atlasKbps);
      CHECK(o.gameKbps == 0 && o.assetKbps > 0 && o.assetKbps <= in.linkKbps / 5, "cinema: no game, assets %d", o.assetKbps);
      CHECK(o.screenKbps.size() == 1 && share(o, 2) == o.atlasKbps, "only the focused screen gets bits"); }

    // Wall: ten visible screens, gaze on one; the cap holds and the focused one gets the most.
    { Arbiter a; Input in; for (int i = 2; i < 12; i++) in.screens.push_back({i, true, 1400, i == 5 ? 1.0f : 0.3f});
      Output o = a.split(in);
      CHECK(o.mode == Mode::Wall, "wall mode");
      CHECK(o.atlasKbps <= Arbiter::kAtlasCap, "wall atlas %d within the 40 Mbit/s cap", o.atlasKbps);
      int top = share(o, 5), other = share(o, 6);
      CHECK(top > 2 * other, "focused screen %d vs unfocused %d", top, other);
      int sum = 0; for (auto& [i, k] : o.screenKbps) sum += k;
      CHECK(sum <= o.atlasKbps + 10, "shares %d add up to at most the atlas %d", sum, o.atlasKbps); }

    // Game: the stream gets its ask, screens get leftovers, no asset downloads.
    { Arbiter a; Input in; in.gameActive = true; in.gameWantKbps = 60000;
      for (int i = 2; i < 12; i++) in.screens.push_back({i, i < 4, 1200, 0.2f});
      Output o = a.split(in);
      CHECK(o.mode == Mode::Game, "game mode");
      CHECK(o.gameKbps == 60000, "game gets its 60 Mbit/s, got %d", o.gameKbps);
      CHECK(o.atlasKbps <= in.linkKbps / 5, "atlas %d at most a fifth of the link around a game", o.atlasKbps);
      CHECK(o.assetKbps == 0, "assets paused during a game");
      CHECK(o.gameKbps + o.atlasKbps + o.assetKbps <= in.linkKbps, "never over the link");
      in.gameWantKbps = 500000; o = a.split(in);
      CHECK(o.gameKbps == in.linkKbps * 3 / 5, "game capped at 60%% of the link, got %d", o.gameKbps); }

    // VR game with every screen hidden: the atlas falls to the tail trickle.
    { Arbiter a; Input in; in.gameActive = true; for (int i = 2; i < 12; i++) in.screens.push_back({i, false, 0, 0});
      Output o = a.split(in);
      CHECK(o.atlasKbps == Arbiter::kTailKbps, "all hidden: atlas %d is the tail trickle", o.atlasKbps); }

    // Reallocation: hiding a screen frees its share at once, and the others keep theirs (no cap pressure here).
    { Arbiter a; Input in; in.screens = {{2, true, 1600, 1.0f}, {3, true, 1600, 0.5f}, {4, true, 1600, 0.5f}};
      Output before = a.split(in);
      in.screens[2].vis = false;
      Output after = a.split(in);
      CHECK(after.atlasKbps < before.atlasKbps, "hiding screen 4 lowers the atlas %d -> %d", before.atlasKbps, after.atlasKbps);
      CHECK(share(after, 4) == 0 && share(after, 2) == share(before, 2), "screen 4 gets nothing, screen 2 unchanged (%d -> %d)", share(before, 2), share(after, 2));
      CHECK(after.assetKbps >= before.assetKbps, "freed bits can go to assets");
      // Under cap pressure, hiding one gives the rest more.
      Arbiter b; Input crowd; for (int i = 2; i < 22; i++) crowd.screens.push_back({i, true, 2400, 1.0f});
      Output c0 = b.split(crowd); for (int i = 10; i < 20; i++) crowd.screens[i].vis = false;
      Output c1 = b.split(crowd);
      CHECK(c0.atlasKbps == Arbiter::kAtlasCap && share(c1, 2) > share(c0, 2), "capped: hiding half raises each share %d -> %d", share(c0, 2), share(c1, 2)); }

    // Link smaller than the cap: everything scales into it.
    { Arbiter a; Input in; in.linkKbps = 30000; for (int i = 2; i < 6; i++) in.screens.push_back({i, true, 1920, 1.0f});
      Output o = a.split(in);
      CHECK(o.atlasKbps <= 15000 && o.atlasKbps + o.assetKbps <= in.linkKbps, "slow link: atlas %d assets %d", o.atlasKbps, o.assetKbps); }

    // GPU pressure: bias up after 1 s over 12 ms, max 2, back down after 3 s under 10 ms each step.
    { Arbiter a; Input in; in.screens = {{2, true, 1000, 1.0f}};
      auto at = [&](double t, double ms) { in.now = t; in.frameMs = ms; return a.split(in).lvlBias; };
      CHECK(at(0, 13) == 0 && at(0.5, 13) == 0 && at(1.0, 13) == 1, "bias 1 after 1 s hot");
      CHECK(at(2.0, 13) == 2 && at(3.0, 13) == 2, "bias capped at 2");
      CHECK(at(3.5, 11) == 2, "11 ms is neither hot nor cool");
      CHECK(at(4, 9) == 2 && at(6, 9) == 2 && at(7, 9) == 1 && at(10, 9) == 0, "bias returns one step per 3 s cool"); }

    printf("%d failures\n", fails);
    return fails ? 1 : 0;
}
