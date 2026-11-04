// flags.h: the flag registry (room_lab/flags.json) for the menus. Flags stay in code; the registry says, per flag,
// its state (on / off / hidden), category (perf / visual-taste / experimental) and the plain-sentence reason, with
// evidence. A hidden flag is greyed in the menu and its hint becomes the reason (registry "hint" when given, else
// the reason's first sentence, upper-cased like the menu's own hints). GL-free; bench/flags_test.cpp.
#pragma once
#include "json.h"
#include <cctype>
#include <map>
#include <string>

namespace flagreg {

struct Info { std::string name, where, state, category, reason, hint, menu; bool def = false; };

struct Registry {
    std::map<std::string, Info> byName;  // headset flags by menu label (sender flags are kept too, prefixed "sender:")
    bool load(const std::string& text) {
        bool ok = false; json::Value d = json::parse(text, &ok);
        if (!ok || d["flags"].type != json::Value::Arr) return false;
        byName.clear();
        for (auto& f : d["flags"].a) {
            Info i; i.name = f["name"].str(); i.where = f["where"].str(); i.state = f["state"].str(); i.category = f["category"].str();
            i.reason = f["reason"].str(); i.hint = f["hint"].str(); i.menu = f["menu"].str(); i.def = f["default"].type == json::Value::Bool && f["default"].b;
            if (i.name.empty()) continue;
            byName[(i.where == "sender" ? "sender:" : "") + i.name] = i;
        }
        return true;
    }
    const Info* find(const std::string& label) const { auto it = byName.find(label); return it == byName.end() ? nullptr : &it->second; }
    bool hidden(const std::string& label) const { auto* i = find(label); return i && i->state == "hidden"; }
    // The menu hint for a label: unchanged unless the flag is hidden.
    std::string hint(const std::string& label, const std::string& menuHint) const {
        auto* i = find(label); if (!i || i->state != "hidden") return menuHint;
        std::string h = !i->hint.empty() ? i->hint : i->reason.substr(0, i->reason.find(". "));
        for (auto& c : h) c = (char)std::toupper((unsigned char)c);
        if (h.size() > 48) h = h.substr(0, 45) + "...";
        return h;
    }
};

}  // namespace flagreg
