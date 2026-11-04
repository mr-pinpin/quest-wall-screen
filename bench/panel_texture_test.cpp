// Text panel as one texture (room_lab/panel_texture.h). g++ -std=c++17 -O1 -I room_lab bench/panel_texture_test.cpp && ./a.out room_lab/room_lab.cpp
// Reads the real SETTINGS items and the real 5x7 font out of room_lab.cpp, lays the panel out with UiGrid as the app
// does, and checks the texture: rendered only when the content changes, the pixels where the layout says, hit-test
// identical to UiGrid::hit, and how many per-frame quads it replaces.
#include "panel_texture.h"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
using namespace paneltex;
static int fails = 0;
static void check(bool c, const char* m) { if (!c) { printf("FAIL %s\n", m); fails++; } }

static uint8_t F[26][7], D[10][7]; static bool haveFont = false;
static const uint8_t* glyph(char c) {  // as room_lab.cpp's glyph(), letters and digits
    static const uint8_t DASH[7] = {0, 0, 0, 31, 0, 0, 0}, DOT[7] = {0, 0, 0, 0, 0, 12, 12};
    if (c == '-') return DASH;
    if (c == '.') return DOT;
    return c >= 'A' && c <= 'Z' ? F[c - 'A'] : c >= '0' && c <= '9' ? D[c - '0'] : nullptr;
}
static bool loadFont(const std::string& src) {
    auto table = [&](const char* name, uint8_t* out, int n) {
        size_t a = src.find(name); if (a == std::string::npos) return false;
        a = src.find('{', a); size_t b = src.find("};", a); std::string body = src.substr(a, b - a);
        std::regex num("\\d+"); int k = 0;
        for (std::sregex_iterator it(body.begin(), body.end(), num), e; it != e && k < n; ++it) out[k++] = (uint8_t)std::stoi(it->str());
        return k == n;
    };
    return table("static const uint8_t F[26][7]", &F[0][0], 182) && table("static const uint8_t D[10][7]", &D[0][0], 70);
}
static uint8_t chan(uint32_t p, int k) { return (p >> (8 * k)) & 255; }
static bool close(uint32_t a, uint32_t b, int tol = 3) { for (int k = 0; k < 4; k++) if (std::abs(chan(a, k) - chan(b, k)) > tol) return false; return true; }

int main(int argc, char** argv) {
    std::ifstream f(argc > 1 ? argv[1] : "room_lab/room_lab.cpp"); std::stringstream ss; ss << f.rdbuf(); std::string src = ss.str();
    haveFont = loadFont(src); check(haveFont, "5x7 font read from room_lab.cpp");
    std::vector<UiGrid::Item> items; std::vector<std::string> labels;
    {   size_t a = src.find("const MenuItem menuItems[kMenuItems] = {"), b = src.find("};", a);
        std::string body = a == std::string::npos ? "" : src.substr(a, b - a);
        std::regex it("\\{\"([A-Z0-9]+)\", \"([A-Z ]*)\", \"([^\"]*)\"\\}");
        for (std::sregex_iterator i(body.begin(), body.end(), it), e; i != e; ++i) { items.push_back({(*i)[1], (*i)[2], (*i)[3]}); labels.push_back((*i)[1]); }
    }
    check(items.size() >= 30, "SETTINGS items read from room_lab.cpp");

    // 1. The real panel: one texture instead of tens of thousands of quads.
    UiGrid g; g.layout(items);
    std::vector<bool> on(items.size(), false); for (size_t k = 0; k < on.size(); k += 3) on[k] = true;
    Content c = fromGrid(g, labels, on, "SETTINGS");
    Panel P; check(P.set(c) && P.dirty(), "first content: dirty");
    auto t0 = std::chrono::steady_clock::now(); const auto& img = P.render(glyph);
    float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    long quads = Panel::quadsPerFrame(c, glyph);
    printf("  SETTINGS: %zu items, %.2f x %.2f m -> %d x %d texture (%.0f texels/m), rendered in %.1f ms; replaces %ld quads/frame\n",
           items.size(), 2 * g.halfW, 2 * g.halfH, P.w, P.h, P.tpm, ms, quads);
    check(img.size() == (size_t)P.w * P.h && P.w <= 4096 && P.h <= 4096, "texture within 4096");
    check(std::fabs((float)P.w / P.h - g.halfW / g.halfH) < 0.01f, "texture aspect = panel aspect");
    float minPx = 1; for (auto& t : c.texts) minPx = std::min(minPx, t.px);
    printf("  smallest text (hints): %.1f texels per font pixel\n", minPx * P.tpm);
    check(minPx * P.tpm >= 1.95f, "every item shown: the hints still get >= 2 texels per font pixel");
    {   std::vector<UiGrid::Item> few(items.begin(), items.begin() + 15); std::vector<std::string> fl(labels.begin(), labels.begin() + 15);
        UiGrid gf; gf.layout(few); Panel pf; Content cf = fromGrid(gf, fl, std::vector<bool>(15, false), "SETTINGS"); pf.set(cf); pf.render(glyph);
        float mp = 1; for (auto& t : cf.texts) mp = std::min(mp, t.px);
        check(mp * pf.tpm >= 2.5f - 1e-3f, "15 items: smallest text gets the full 2.5 texels per font pixel"); }
    check(quads > 5000, "the quad path it replaces is large");
    check(ms < 200, "render well under a frame budget's worth of seconds (done once per change)");

    // 2. Dirty flag: same content no; a toggled item, a changed label or hint yes; render only when dirty.
    check(!P.set(fromGrid(g, labels, on, "SETTINGS")) && !P.dirty(), "same content: not dirty");
    long r0 = P.renders; P.render(glyph); check(P.renders == r0, "not dirty: no re-render");
    on[1] = !on[1]; check(P.set(fromGrid(g, labels, on, "SETTINGS")) && P.dirty(), "an item toggled: dirty");
    P.render(glyph); check(P.renders == r0 + 1 && !P.dirty() && P.version == 2, "rendered once, version bumped");
    auto l2 = labels; l2[0] = "MINIX"; check(P.set(fromGrid(g, l2, on, "SETTINGS")), "a label changed: dirty");
    UiGrid g2 = g; g2.cells[2].hint1 = "OTHER"; check(P.set(fromGrid(g2, l2, on, "SETTINGS")), "a hint changed: dirty");
    P.set(fromGrid(g, labels, on, "SETTINGS")); P.render(glyph);

    // 3. Pixels where the layout puts them: each cell's colour at its centre-left padding, bg between cells.
    GridStyle st; int okCells = 0;
    for (size_t k = 0; k < g.cells.size(); k++) {
        const auto& cl = g.cells[k];
        int x = (int)P.tx(cl.x - cl.w / 2 + 0.004f), y = (int)P.ty(cl.y - cl.h / 2 + 0.004f);  // inside the cell, clear of text
        uint32_t want = st.bg; Panel::blend(want, on[k] ? st.on : st.idle, 1);
        okCells += close(P.img[(size_t)y * P.w + x], want);
    }
    check(okCells == (int)g.cells.size(), "every cell drawn in its on/off colour at its layout position");
    {   const auto& a = g.cells[0]; int x = (int)P.tx(a.x + a.w / 2 + g.gap / 2), y = (int)P.ty(a.y);
        if (g.cells.size() > 1 && std::fabs(g.cells[1].y - a.y) < 1e-4f) check(close(P.img[(size_t)y * P.w + x], st.bg), "gap between cells is background"); }

    // 4. Text: a synthetic panel, the letter I at 0.01 m pixels: lit pixels in the text colour, unlit in the box colour,
    // and at a fractional size the total ink equals the lit area (area sampling, no gaps or double blending).
    {   Content t; t.halfW = 0.05f; t.halfH = 0.05f; t.bg = rgba(0, 0, 0, 255);
        t.boxes.push_back({0, 0, 0.08f, 0.08f, rgba(0, 0, 255, 255), 7});
        t.texts.push_back({0, 0, 0.01f, "I", rgba(255, 255, 255, 255)});
        Panel q; q.set(t); q.render(glyph);
        const uint8_t* gi = glyph('I'); int lit = 0, good = 0;
        for (int r = 0; r < 7; r++) for (int k = 0; k < 5; k++) {
            float x = -2.5f * 0.01f + (k + 0.5f) * 0.01f, y = 3.5f * 0.01f - (r + 0.5f) * 0.01f;  // (6n - 1) = 5 px wide, centred
            uint32_t px = q.img[(size_t)(int)q.ty(y) * q.w + (int)q.tx(x)];
            bool on1 = gi[r] & (16 >> k); lit += on1;
            good += on1 ? close(px, rgba(255, 255, 255, 255)) : close(px, rgba(0, 0, 255, 255));
        }
        check(good == 35, "glyph pixels exactly where the 5x7 font puts them");
        check(q.hit(0, 0) == 7 && q.hit(0.045f, 0.045f) == -1, "synthetic hit: box id inside, -1 outside");
        Content u = t; u.texts[0].px = 0.0073f; u.boxes.clear(); u.texts[0].col = rgba(255, 255, 255, 255);
        Panel z; z.texelsPerGlyphPx = 2.3f; z.set(u); z.render(glyph);
        double ink = 0; for (auto p : z.img) ink += chan(p, 0) / 255.0;
        double want = lit * (0.0073 * z.tpm) * (0.0073 * z.tpm);
        check(std::fabs(ink - want) / want < 0.01, "fractional size: ink = lit area within 1%");
        printf("  fractional glyph size: %.2f texels per font pixel, ink %.1f vs %.1f texels\n", 0.0073 * z.tpm, ink, want);
    }

    // 5. Hit-test = UiGrid::hit everywhere on the panel; uv maps the panel corners to the texture corners.
    {   int same = 0, n = 0;
        for (float y = -g.halfH; y <= g.halfH; y += 0.0031f) for (float x = -g.halfW; x <= g.halfW; x += 0.0029f, n++) same += P.hit(x, y) == g.hit(x, y);
        check(same == n, "hit-test matches UiGrid::hit at every sampled point");
        int cells = 0; for (size_t k = 0; k < g.cells.size(); k++) cells += P.hit(g.cells[k].x, g.cells[k].y) == (int)k;
        check(cells == (int)g.cells.size(), "every cell centre hits its own index");
        float u, v; P.uv(-g.halfW, g.halfH, u, v); bool ok = std::fabs(u) < 1e-5f && std::fabs(v) < 1e-5f;
        P.uv(g.halfW, -g.halfH, u, v); ok = ok && std::fabs(u - 1) < 1e-5f && std::fabs(v - 1) < 1e-5f;
        check(ok, "uv: top-left (0,0), bottom-right (1,1)");
    }
    printf("panel texture: %s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
