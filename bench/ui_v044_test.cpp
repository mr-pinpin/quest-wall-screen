// v0.44 offline checks: settings button explanations (room_lab/ui_grid.h hints) and the AMBI border's atlas lookup
// under VT (room_lab/vt.h atlasAt). g++ -std=c++17 -I room_lab bench/ui_v044_test.cpp && ./a.out
#include "ui_grid.h"
#include "vt.h"
#include <cstdio>
#include <cmath>
static int fails = 0;
static void check(bool c, const char* m) { if (!c) { printf("FAIL %s\n", m); fails++; } }
static bool fontOk(const std::string& s) {  // the app's 5x7 font: A-Z 0-9 % - . : ! and space
    for (char c : s) if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '%' || c == '-' || c == '.' || c == ':' || c == '!')) return false;
    return true;
}
int main() {
    // The real settings list (kept in sync by hand with room_lab.cpp menuItems).
    std::vector<UiGrid::Item> items = {
        {"MINI", "SOURCE", "SHOW THE MAC MINI"}, {"LAPTOP", "SOURCE", "SHOW THE LAPTOP"}, {"H264", "SOURCE", "GAME IN H264 NOT HEVC"},
        {"HOSTRES", "SOURCE", "FIND THE LAPTOP BY NAME"}, {"SMALL", "SIZE", "MAIN SCREEN 1 M WIDE"}, {"MEDIUM", "SIZE", "MAIN SCREEN 1.6 M WIDE"},
        {"LARGE", "SIZE", "MAIN SCREEN 2.4 M WIDE"}, {"LOCK", "PANEL", "PIN SCREENS AND THIS PANEL"}, {"QUAD", "PANEL", "SHARPER MAIN SCREEN"},
        {"CLOSE", "PANEL", "HIDE THIS PANEL"}, {"CURVE", "DISPLAY", "BEND THE SCREEN AROUND YOU"}, {"MARGIN", "BORDER", "SOFT CLEAR EDGE"},
        {"SUPER", "BORDER", "SMOOTHER EDGES MORE GPU"}, {"BEZEL", "BORDER", "FRAME AROUND THE SCREEN"}, {"FADE", "BEZEL", "DARK FRAME THAT FADES"},
        {"GLASS", "BEZEL", "FROSTED GLASS FRAME"}, {"AMBI", "BEZEL", "FRAME GLOWS WITH SCREEN COLOURS"}, {"SPELL", "GESTURES", "DRAW A CIRCLE FOR A NEW SCREEN"},
        {"MACDISP", "SCREENS", "EXTRA SCREENS ARE MAC DISPLAYS"}, {"SAVESET", "MEMORY", "REMEMBER THESE SETTINGS"},
        {"RESUME", "MEMORY", "REOPEN LAST SOURCE AT START"}, {"VT", "EXPERIMENTAL", "SHARP ONLY WHERE YOU LOOK"},
        {"BUDGET", "EXPERIMENTAL", "SHARE WI-FI BETWEEN STREAMS"}, {"GPUTIME", "EXPERIMENTAL", "MEASURE GPU TIME"},
        {"WIFILL", "EXPERIMENTAL", "LOW LATENCY WI-FI"}, {"MODELS", "EXPERIMENTAL", "SHOW A 3D MODEL"}, {"LODPOSE", "EXPERIMENTAL", "DETAIL BY VIEWING ANGLE"},
        {"HERO", "EXPERIMENTAL", "OWN STREAM FOR A BIG SCREEN"}, {"KEYREQ", "EXPERIMENTAL", "ASK FOR KEYFRAMES"},
        {"SHOULDER", "EXPERIMENTAL", "MENU BUTTON BY YOUR SHOULDER"}, {"HOUSE", "EXPERIMENTAL", "SYNC LAYOUT TO THE HOUSE FILE"},
        {"DEPTHGAP", "EXPERIMENTAL", "MEASURE SCREENS VS REAL WALLS"}, {"WRIST", "EXPERIMENTAL", "TAP YOUR WRIST FOR A RING MENU"},
        {"SCAN", "EXPERIMENTAL", "SCAN BUTTON IN THE MENUS"}, {"SCROLL", "EXPERIMENTAL", "WAVE AN OPEN HAND TO SCROLL"},
        {"WALLTOOL", "EXPERIMENTAL", "DRAW LINES AND SHAPES ON WALLS"}};
    UiGrid g; g.layout(items);
    float minHint = 1;
    for (size_t i = 0; i < items.size(); i++) {
        const auto& c = g.cells[i];
        check(!items[i].hint.empty() && fontOk(items[i].hint), "every button has an explanation in the panel font");
        check(!c.hint1.empty(), "the explanation is laid out");
        check((c.hint2.empty() ? c.hint1 : c.hint1 + " " + c.hint2) == items[i].hint, "wrapping keeps every word");
        for (auto* l : {&c.hint1, &c.hint2}) check(UiGrid::textW(*l, c.hintPx) <= c.w + 1e-6f, "explanation fits the button width");
        check(c.labelY > c.hint1Y && c.hint1Y >= c.hint2Y - 1e-6f, "label above its explanation");
        check(c.labelY + 3.5f * c.px <= c.y + c.h / 2 + 1e-6f, "label inside the button (top)");
        float low = (c.hint2.empty() ? c.hint1Y : c.hint2Y) - 3.5f * c.hintPx;
        check(low >= c.y - c.h / 2 - 1e-6f, "explanation inside the button (bottom)");
        check(std::fabs(c.labelY - (c.hint2.empty() ? c.hint1Y : c.hint2Y)) > 3.5f * (c.px + c.hintPx) - 1e-6f, "label and explanation do not overlap");
        check(g.hit(c.x, c.hint1Y) == (int)i, "the explanation is part of the button (pointing there clicks it)");
        minHint = std::min(minHint, c.hintPx);
    }
    // legible: glyphs at least ~6 mm tall (7 px) on the panel
    check(minHint * 7 >= 0.006f, "explanations at least 6 mm tall");
    // no hints: the old layout exactly (v0.37 panel)
    std::vector<UiGrid::Item> plain; for (auto it : items) { it.hint.clear(); plain.push_back(it); }
    UiGrid p; p.layout(plain);
    for (auto& c : p.cells) check(c.h == p.cellH && c.labelY == c.y && c.hint1.empty(), "without explanations: the old cells");

    // atlasAt: v1 (one cell, whole screen)
    std::vector<vt::Cell> v1 = {{100, 50, 960, 600}};
    float ax, ay;
    check(vt::atlasAt(v1, 0, 0, ax, ay) && ax == 100 && ay == 50, "v1 top-left");
    check(vt::atlasAt(v1, 1, 1, ax, ay) && ax == 1060 && ay == 650, "v1 bottom-right");
    // v2: a 1920x1200 display in four tiles placed anywhere in the atlas, at level 0
    vt::Cell t[4] = {{2000, 0, 960, 600, 0, 0, 0, 960, 600}, {0, 0, 960, 600, 0, 960, 0, 960, 600},
                     {0, 1000, 960, 600, 0, 0, 600, 960, 600}, {3000, 3000, 960, 600, 0, 960, 600, 960, 600}};
    std::vector<vt::Cell> v2(t, t + 4);
    check(vt::atlasAt(v2, 0.0f, 0.0f, ax, ay) && ax == 2000 && ay == 0, "tiled: top-left in its own tile");
    check(vt::atlasAt(v2, 0.75f, 0.25f, ax, ay) && std::fabs(ax - 480) < 1 && std::fabs(ay - 300) < 1, "tiled: top-right quadrant");
    check(vt::atlasAt(v2, 0.25f, 0.75f, ax, ay) && std::fabs(ax - 480) < 1 && std::fabs(ay - 1300) < 1, "tiled: bottom-left quadrant");
    check(vt::atlasAt(v2, 1.0f, 1.0f, ax, ay) && std::fabs(ax - 3960) < 1 && std::fabs(ay - 3600) < 1, "tiled: bottom-right corner");
    // the old first-cell rule put every border point in tile 0: the bug this replaces
    check(vt::atlasAt(v2, 0.9f, 0.9f, ax, ay) && ax > 2900, "tiled: far corner not taken from the first tile");
    std::vector<vt::Cell> none; check(!vt::atlasAt(none, 0.5f, 0.5f, ax, ay), "no cells: no lookup");
    printf("%s: %d failures\n", fails ? "FAIL" : "ui v0.44 (hints, AMBI under VT)", fails);
    return fails ? 1 : 0;
}
