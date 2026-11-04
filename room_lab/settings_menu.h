// SETTINGS by category (flag SETCAT; work-df 2026-11-04: "49 switches in a 3.7 m panel is a UX problem"). The root
// panel holds a MOST USED row (the source buttons, LOCK, AMBI) and one button per category; a category opens its
// switches in a cascaded panel (shoulder.h) further out, paged so no panel is taller than maxH (panel units, scale
// 1). room_lab/flags.json decides what shows: a "hidden" flag is left out, an "off" flag is greyed with its reason
// as the hint (still a switch: pressing it turns it on). Labels the registry does not know (MINI, LOCK, ...) keep
// their own hint; a switch no category names lands in EXPERIMENTAL, so a new flag never disappears.
// Levels: "SETTINGS" (root) and "SET <CATEGORY> <page>"; a category button is "open:SET <CATEGORY> 1", paging is
// "swap:SET <CATEGORY> <page>" (shoulder.h replaces the top panel in place). GL-free; bench/settings_menu_test.cpp.
#pragma once
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>
#include "flags.h"
#include "shoulder.h"

namespace setmenu {

struct Entry { std::string label, section, hint; };
struct Category { std::string name, hint; std::vector<std::string> labels; };

inline const std::vector<Category>& categories() {
    static const std::vector<Category> c = {
        {"SCREENS", "SOURCES SIZES BARS", {"MINI", "LAPTOP", "SMALL", "MEDIUM", "LARGE", "LOCK", "MACDISP", "ALLBARS", "CLOSEX", "DEPTHPLACE", "CTLTOP", "RESUME", "SAVESET"}},
        {"INPUT", "HANDS AND GESTURES", {"STEADY", "SPELL", "SHOULDER", "WRIST", "SCROLL", "WALLTOOL"}},
        {"LOOK", "HOW SCREENS LOOK", {"QUAD", "CURVE", "MARGIN", "SUPER", "BEZEL", "FADE", "GLASS", "AMBI", "PANELTEX", "SETCAT"}},
        {"ASSISTANT", "CLAUDE AND THE MIC", {"ASSIST", "RECEPTION", "DICTQ"}},
        {"SCAN AND ROOM", "WALLS MESH HOUSE", {"SCAN", "GEOMETRY", "MULTIROOM", "MESHSAVE", "HOUSE", "RELOCCHECK"}},
        {"EXPERIMENTAL", "STREAMING TRIALS", {"H264", "HOSTRES", "VT", "BUDGET", "WIFILL", "MODELS", "LODPOSE", "HERO", "KEYREQ"}},
        {"DEBUG", "MEASUREMENTS", {"GPUTIME", "DEPTHGAP"}},
    };
    return c;
}
inline const std::vector<std::string>& mostUsed() { static const std::vector<std::string> m = {"MINI", "LAPTOP", "LOCK", "AMBI"}; return m; }

struct Tree {
    std::vector<Entry> entries;               // every switch, as room_lab's menuItems (CLOSE is left out)
    const flagreg::Registry* reg = nullptr;   // flags.json; null = everything shown as is
    float maxH = 1.0f; int cols = 3;

    const Entry* entry(const std::string& l) const { for (auto& e : entries) if (e.label == l) return &e; return nullptr; }
    bool shown(const std::string& l) const { return l != "CLOSE" && entry(l) && !(reg && reg->hidden(l)); }
    std::string categoryOf(const std::string& l) const {
        for (auto& c : categories()) for (auto& x : c.labels) if (x == l) return c.name;
        return "EXPERIMENTAL";
    }
    std::vector<std::string> switches(const std::string& cat) const {  // in menuItems order
        std::vector<std::string> out;
        for (auto& e : entries) if (shown(e.label) && categoryOf(e.label) == cat) out.push_back(e.label);
        return out;
    }
    static std::string upper(std::string s) { for (auto& c : s) c = (char)std::toupper((unsigned char)c); return s; }
    shoulder::Item item(const std::string& l) const {
        const Entry* e = entry(l); shoulder::Item it{l, e ? e->hint : "", "set:" + l};
        if (reg) if (auto* i = reg->find(l); i && i->state == "off") {
            it.dim = true;
            std::string h = !i->hint.empty() ? i->hint : i->reason.substr(0, i->reason.find(". "));
            h = upper(h); if (h.size() > 48) h = h.substr(0, 45) + "...";
            if (!h.empty()) it.hint = h;
        }
        return it;
    }
    // The root: MOST USED switches, then a button per non-empty category, then `tail` (BACK in the cascade, CLOSE on
    // the standalone panel).
    shoulder::Level root(const shoulder::Item& tail) const {
        shoulder::Level L; L.name = "SETTINGS"; L.title = "SETTINGS"; L.grid.cols = cols;
        for (auto& l : mostUsed()) if (shown(l)) { auto it = item(l); it.section = "MOST USED"; L.items.push_back(it); }
        for (auto& c : categories()) {
            int n = (int)switches(c.name).size(); if (!n) continue;
            L.items.push_back({c.name, std::to_string(n) + (n == 1 ? " SWITCH: " : " SWITCHES: ") + c.hint, "open:SET " + c.name + " 1", "CATEGORIES"});
        }
        // switches no category names (new flags): their own EXPERIMENTAL button already counts them
        auto t = tail; t.section = "CATEGORIES"; L.items.push_back(t);
        return L;
    }
    float height(const std::vector<shoulder::Item>& items) const {
        UiGrid g; g.cols = cols; g.layout(shoulder::Menu::toGrid(items)); return 2 * g.halfH;
    }
    // Pages of a category: as many switches as fit under maxH with the page's NEXT PAGE and BACK buttons.
    std::vector<std::vector<std::string>> pages(const std::string& cat) const {
        std::vector<std::string> all = switches(cat); std::vector<std::vector<std::string>> out;
        size_t i = 0;
        while (i < all.size()) {
            std::vector<std::string> page;
            while (i < all.size()) {
                std::vector<shoulder::Item> trial; for (auto& l : page) trial.push_back(item(l));
                trial.push_back(item(all[i])); trial.push_back({"NEXT PAGE", "", ""}); trial.push_back({"BACK", "", ""});
                if (!page.empty() && height(trial) > maxH) break;
                page.push_back(all[i++]);
            }
            out.push_back(page);
        }
        return out;
    }
    // "SETTINGS" or "SET <CATEGORY> <page>" (1-based); an unknown name gives an empty level with BACK.
    shoulder::Level level(const std::string& name) const {
        if (name == "SETTINGS") return root({"BACK", "ONE MENU BACK", "back"});
        shoulder::Level L; L.name = name; L.grid.cols = cols;
        std::string rest = name.rfind("SET ", 0) == 0 ? name.substr(4) : "";
        size_t sp = rest.find_last_of(' '); int page = 1; std::string cat = rest;
        if (sp != std::string::npos && sp + 1 < rest.size() && std::isdigit((unsigned char)rest[sp + 1])) { page = std::max(1, atoi(rest.c_str() + sp + 1)); cat = rest.substr(0, sp); }
        auto ps = pages(cat); int n = (int)ps.size();
        L.title = cat + (n > 1 ? " " + std::to_string(std::min(page, n)) + " OF " + std::to_string(n) : "");
        if (n) for (auto& l : ps[std::min(page, n) - 1]) L.items.push_back(item(l));
        if (n > 1) L.items.push_back({"NEXT PAGE", "MORE " + cat + " SWITCHES", "swap:SET " + cat + " " + std::to_string(page % n + 1)});
        L.items.push_back({"BACK", "ONE MENU BACK", "back"});
        return L;
    }
};

}  // namespace setmenu
