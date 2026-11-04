// Per-frame UI geometry, estimated from the real menus, font and layout (no headset): how many quads each part of
// room_lab.cpp's build() pushes every frame with 7 screens, and what PANELTEX / SETCAT change. A quad = 6 vertices of
// 36 bytes, copied and uploaded every frame (render(): all = occ + over + ctl, glBufferData). The measured numbers come
// from status.json "frameCost" on the headset. g++ -std=c++17 -I room_lab bench/frame_cost_estimate.cpp && ./a.out room_lab/room_lab.cpp room_lab/flags.json
#include "panel_texture.h"
#include "settings_menu.h"
#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
static uint8_t F[26][7], D[10][7];
static const uint8_t* glyph(char c) { return c >= 'A' && c <= 'Z' ? F[c - 'A'] : c >= '0' && c <= '9' ? D[c - '0'] : nullptr; }
static std::string slurp(const char* p) { std::ifstream f(p); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
static long textQuads(const std::string& s) { long q = 0; for (char c : s) if (auto* g = glyph(c)) for (int r = 0; r < 7; r++) for (int k = 0; k < 5; k++) q += (g[r] >> k) & 1; return q; }
static long levelQuads(const shoulder::Level& L) {  // drawShoulder: bg + title + per item box, label, hint lines
    UiGrid g; g.cols = L.grid.cols; g.layout(shoulder::Menu::toGrid(L.items));
    long q = 1 + textQuads(L.title);
    for (size_t i = 0; i < L.items.size(); i++) q += 1 + textQuads(L.items[i].label) + textQuads(g.cells[i].hint1) + textQuads(g.cells[i].hint2);
    return q;
}
int main(int argc, char** argv) {
    std::string src = slurp(argc > 1 ? argv[1] : "room_lab/room_lab.cpp");
    auto table = [&](const char* name, uint8_t* out, int n) { size_t a = src.find(name); a = src.find('{', a); size_t b = src.find("};", a); std::string body = src.substr(a, b - a);
        std::regex num("\\d+"); int k = 0; for (std::sregex_iterator it(body.begin(), body.end(), num), e; it != e && k < n; ++it) out[k++] = (uint8_t)std::stoi(it->str()); };
    table("static const uint8_t F[26][7]", &F[0][0], 182); table("static const uint8_t D[10][7]", &D[0][0], 70);
    setmenu::Tree t; flagreg::Registry R; R.load(slurp(argc > 2 ? argv[2] : "room_lab/flags.json")); t.reg = &R;
    std::vector<UiGrid::Item> items; std::vector<std::string> labels;
    { size_t a = src.find("const MenuItem menuItems[kMenuItems] = {"), b = src.find("};", a); std::string body = src.substr(a, b - a);
      std::regex it("\\{\"([A-Z0-9]+)\", \"([A-Z ]*)\", \"([^\"]*)\"\\}");
      for (std::sregex_iterator i(body.begin(), body.end(), it), e; i != e; ++i) { items.push_back({(*i)[1], (*i)[2], (*i)[3]}); labels.push_back((*i)[1]); t.entries.push_back({(*i)[1], (*i)[2], (*i)[3]}); } }
    UiGrid g; g.layout(items); long flat = paneltex::Panel::quadsPerFrame(paneltex::fromGrid(g, labels, std::vector<bool>(labels.size()), "SETTINGS"), glyph);
    shoulder::Level old; old.title = "SETTINGS"; old.grid.cols = 5; for (auto& e : t.entries) if (e.label != "CLOSE") old.items.push_back({e.label, e.hint, "set:" + e.label}); old.items.push_back({"BACK", "ONE MENU BACK", "back"});
    long oldCascade = levelQuads(old), root = levelQuads(t.level("SETTINGS")), worstCat = 0;
    for (auto& c : setmenu::categories()) worstCat = std::max(worstCat, levelQuads(t.level("SET " + c.name + " 1")));
    const int extras = 6;
    long ambi = 7 * 4, ambiVt = 7 * 4 * 16, assistBtn = 20 + 8, assistDots = 3 * 8, closeX = 16 + 2, bar = 2, cursor = 40, title = textQuads("SCREEN 5") + textQuads("MAC DISPLAY COMING") + 1 + 4;
    printf("Per-frame UI quads (x6 vertices x36 B, copied and uploaded every frame), 7 screens = main + %d extras\n", extras);
    printf("  %-58s %7s\n", "part", "quads");
    printf("  %-58s %7ld\n", "SETTINGS panel open, flat (PANELTEX off)", flat);
    printf("  %-58s %7d\n", "SETTINGS panel open, PANELTEX on (hover + bar)", 3);
    printf("  %-58s %7ld\n", "shoulder/wrist SETTINGS level, flat 5 columns (SETCAT off)", oldCascade);
    printf("  %-58s %7ld\n", "  SETCAT root panel (PANELTEX off)", root);
    printf("  %-58s %7ld\n", "  SETCAT biggest category panel (PANELTEX off)", worstCat);
    printf("  %-58s %7d\n", "  any cascade panel with PANELTEX on (hover only)", 1);
    printf("  %-58s %7ld\n", "AMBI bezel, 7 screens", 7 * ambi);
    printf("  %-58s %7ld\n", "AMBI bezel, 7 screens under VT (16 cuts per strip)", 7 * ambiVt);
    printf("  %-58s %7ld\n", "extra screens not live yet (frame + 'MAC DISPLAY COMING')", extras * title);
    printf("  %-58s %7ld\n", "ASSIST buttons, 7 screens (+ dots on the listening one)", 7 * assistBtn + assistDots);
    printf("  %-58s %7ld\n", "close X + move bars, 6 extras unlocked", extras * (closeX + bar));
    printf("  %-58s %7ld\n", "cursor ring on one screen", cursor);
    printf("  -> the flat SETTINGS (panel %ld or cascade %ld quads = %.1f / %.1f MB a frame) dwarfs everything else.\n", flat, oldCascade, flat * 6 * 36 / 1e6, oldCascade * 6 * 36 / 1e6);
    return 0;
}
