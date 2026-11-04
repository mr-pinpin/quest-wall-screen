// SETTINGS by category (room_lab/settings_menu.h, flag SETCAT). g++ -std=c++17 -I room_lab bench/settings_menu_test.cpp && ./a.out room_lab/room_lab.cpp room_lab/flags.json
// The real switches (room_lab.cpp menuItems) and the real registry (flags.json): every switch in exactly one category
// page, hidden flags left out, off flags greyed with their reason, no panel taller than maxH, paging that cycles, and
// the cascade's hit-test landing on the item drawn there (from the shoulder root and beside the standalone panel).
#include "settings_menu.h"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
using namespace setmenu;
static int fails = 0;
static void check(bool c, const char* m) { if (!c) { printf("FAIL %s\n", m); fails++; } }
static std::string slurp(const char* p) { std::ifstream f(p); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }

// Ray at the centre of item i on the top panel: from the eye through the item's world point.
static bool hitsItem(const shoulder::Menu& m, shoulder::P3 eye, int i) {
    const shoulder::Level& L = m.stack.back(); const UiGrid::Cell& c = L.grid.cells[i];
    shoulder::P3 p = L.toWorld(c.x, c.y), d = shoulder::sub(p, eye); float l = shoulder::len(d); d = shoulder::mul(d, 1 / l);
    shoulder::Hit h = m.hit(eye, d);
    return h.level == (int)m.stack.size() - 1 && h.item == i;
}

int main(int argc, char** argv) {
    std::string src = slurp(argc > 1 ? argv[1] : "room_lab/room_lab.cpp"), reg = slurp(argc > 2 ? argv[2] : "room_lab/flags.json");
    Tree t; flagreg::Registry R; check(R.load(reg), "flags.json loads"); t.reg = &R;
    {   size_t a = src.find("const MenuItem menuItems[kMenuItems] = {"), b = src.find("};", a);
        std::string body = a == std::string::npos ? "" : src.substr(a, b - a);
        std::regex it("\\{\"([A-Z0-9]+)\", \"([A-Z ]*)\", \"([^\"]*)\"\\}");
        for (std::sregex_iterator i(body.begin(), body.end(), it), e; i != e; ++i) t.entries.push_back({(*i)[1], (*i)[2], (*i)[3]});
    }
    check(t.entries.size() >= 40, "switches read from room_lab.cpp");

    // 1. Every shown switch is on exactly one page of exactly one category; CLOSE is not a switch.
    std::map<std::string, int> seen; int pagesN = 0; float tallest = 0; std::string tallestName;
    for (auto& c : categories()) {
        auto ps = t.pages(c.name);
        for (size_t p = 0; p < ps.size(); p++) {
            shoulder::Level L = t.level("SET " + c.name + " " + std::to_string(p + 1)); pagesN++;
            float h = t.height(L.items); if (h > tallest) { tallest = h; tallestName = L.title; }
            for (auto& it : L.items) if (it.action.rfind("set:", 0) == 0) seen[it.label]++;
        }
    }
    int shown = 0, once = 0; for (auto& e : t.entries) if (t.shown(e.label)) { shown++; once += seen[e.label] == 1; }
    check(once == shown && !seen.count("CLOSE"), "every switch exactly once, CLOSE not among them");
    printf("  %d switches in %zu categories, %d pages; tallest panel %.2f m (%s)\n", shown, categories().size(), pagesN, tallest, tallestName.c_str());
    check(tallest <= t.maxH + 1e-4f, "no category page taller than maxH");
    shoulder::Level root = t.root({"CLOSE", "HIDE THIS PANEL", "close"});
    float rootH = t.height(root.items);
    printf("  root: %zu buttons, %.2f m tall (the flat panel was 3.68 m)\n", root.items.size(), rootH);
    check(rootH <= 1.2f, "root panel under 1.2 m");
    check(root.items.size() >= 4 && root.items[0].label == "MINI" && root.items[0].section == "MOST USED" && root.items[3].label == "AMBI", "MOST USED row first: MINI LAPTOP LOCK AMBI");
    {   UiGrid g; g.cols = t.cols; g.layout(shoulder::Menu::toGrid(root.items));
        check(g.headings.size() == 2 && g.headings[0].text == "MOST USED" && g.headings[1].text == "CATEGORIES", "root headings: MOST USED, CATEGORIES"); }
    bool catsOk = true; for (auto& it : root.items) if (it.section == "CATEGORIES" && it.label != "CLOSE") catsOk = catsOk && it.action == "open:SET " + it.label + " 1" && it.hint.find("SWITCH") != std::string::npos;
    check(catsOk, "category buttons open their first page and say how many switches");

    // 2. The registry: off flags greyed with the reason; on flags and unknown labels keep their own hint; hidden left out.
    {   auto vt = t.item("VT"), quad = t.item("QUAD"), mini = t.item("MINI");
        check(vt.dim && vt.hint != t.entry("VT")->hint && vt.hint.size() <= 48, "VT (off): greyed, reason as hint");
        check(!quad.dim && quad.hint == t.entry("QUAD")->hint, "QUAD (on): normal, its own hint");
        check(!mini.dim && mini.hint == t.entry("MINI")->hint, "MINI (not a flag): normal");
        std::string r2 = reg; size_t k = r2.find("\"name\": \"BUDGET\""); size_t s = r2.find("\"state\": \"off\"", k);
        if (k != std::string::npos && s != std::string::npos) r2.replace(s, 14, "\"state\": \"hidden\"");
        flagreg::Registry R2; R2.load(r2); Tree t2 = t; t2.reg = &R2;
        bool gone = true; for (auto& c : categories()) for (auto& l : t2.switches(c.name)) gone = gone && l != "BUDGET";
        check(R2.hidden("BUDGET") && gone, "a hidden flag is left out of every page");
        Tree t3 = t; t3.entries.push_back({"NEWFLAG", "EXPERIMENTAL", "SOMETHING NEW"});
        auto ex = t3.switches("EXPERIMENTAL"); check(std::find(ex.begin(), ex.end(), "NEWFLAG") != ex.end(), "a switch no category names lands in EXPERIMENTAL");
    }

    // 3. Paging: a small maxH forces pages; NEXT PAGE cycles 1 -> 2 -> ... -> 1, titles say "N OF M", all switches kept.
    {   Tree s = t; s.maxH = 0.55f; auto ps = s.pages("SCREENS"); int n = (int)ps.size();
        check(n >= 2, "SCREENS pages at maxH 0.55");
        size_t total = 0; bool fit = true, cyc = true;
        for (int p = 1; p <= n; p++) {
            shoulder::Level L = s.level("SET SCREENS " + std::to_string(p)); total += ps[p - 1].size();
            fit = fit && s.height(L.items) <= s.maxH + 1e-4f;
            auto& nx = L.items[L.items.size() - 2];
            cyc = cyc && nx.label == "NEXT PAGE" && nx.action == "swap:SET SCREENS " + std::to_string(p % n + 1) && L.title == "SCREENS " + std::to_string(p) + " OF " + std::to_string(n);
        }
        check(fit && cyc && total == s.switches("SCREENS").size(), "pages fit, NEXT PAGE cycles, titles N OF M, nothing lost");
        printf("  SCREENS at maxH 0.55 m: %d pages\n", n);
    }

    // 4. Cascade from the shoulder root: SETTINGS -> a category one step further; the hit-test finds every item.
    {   shoulder::Menu m; m.build = [&](const std::string& n) { return t.level(n); };
        shoulder::P3 eye{0, 1.6f, 0}; m.eye0 = eye; m.yaw0 = 0; m.push("SETTINGS");
        bool all = true; for (size_t i = 0; i < m.stack.back().items.size(); i++) all = all && hitsItem(m, eye, (int)i);
        check(all, "root: a ray at every button hits that button");
        int look = -1; for (size_t i = 0; i < m.stack[0].items.size(); i++) if (m.stack[0].items[i].label == "LOOK") look = (int)i;
        check(m.press({0, look}).empty() && m.stack.size() == 2 && m.stack[1].name == "SET LOOK 1", "LOOK opens its page");
        check(std::fabs(m.stack[1].dist - m.stack[0].dist - m.step) < 1e-4f, "the category panel is one step further");
        all = true; for (size_t i = 0; i < m.stack.back().items.size(); i++) all = all && hitsItem(m, eye, (int)i);
        check(all, "category: a ray at every switch hits that switch");
        int amb = -1; for (size_t i = 0; i < m.stack[1].items.size(); i++) if (m.stack[1].items[i].label == "AMBI") amb = (int)i;
        check(amb >= 0 && m.press({1, amb}) == "set:AMBI", "a switch returns set:<LABEL> for room_lab");
        m.press({1, (int)m.stack[1].items.size() - 1}); check(m.stack.size() == 1, "BACK returns to the root");
    }

    // 5. Beside the standalone panel (0.8 m away, 0.4 m half-width): the category opens a step further, clear of it.
    {   shoulder::Menu m; m.side = 1;
        shoulder::P3 eye{0, 1.6f, 0}, panel{0, 1.5f, -0.8f};
        Tree s = t; s.maxH = 0.55f; m.build = [&](const std::string& n) { return s.level(n); };
        m.openBeside(eye, panel, 0.4f, "SET SCREENS 1");
        const auto& L = m.stack[0];
        check(L.anchored && std::fabs(L.dist - (0.8f + m.step)) < 1e-4f && std::fabs(L.centre.y - panel.y) < 1e-5f, "anchored a step beyond the panel, at its height");
        float gap = std::fabs(L.yaw) - (0.4f / 0.8f + L.grid.halfW * L.scale / L.dist);
        check(gap > 0.02f, "turned until it clears the panel");
        bool all = true; for (size_t i = 0; i < L.items.size(); i++) all = all && hitsItem(m, eye, (int)i);
        check(all, "beside the panel: every switch is hit where drawn");
        int nx = (int)L.items.size() - 2; std::string before = m.stack[0].name; shoulder::P3 c0 = m.stack[0].centre;
        m.press({0, nx});
        check(m.stack.size() == 1 && m.stack[0].name == "SET SCREENS 2" && m.stack[0].anchored && std::fabs(m.stack[0].centre.x - c0.x) < 1e-5f, "NEXT PAGE swaps in place");
        all = true; for (size_t i = 0; i < m.stack[0].items.size(); i++) all = all && hitsItem(m, eye, (int)i);
        check(all, "page 2: every switch is hit where drawn");
        m.press({0, (int)m.stack[0].items.size() - 1}); check(!m.open(), "BACK closes the cascade: the standalone panel is the root");
    }
    printf("settings menu: %s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
