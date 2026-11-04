// house["selected"] (work-42's contract, receptionist/room_tools.py) -> the outline state (select_outline.h).
// The field is a list [{"id": n, "ts": "<iso8601Z>"}, ...] in selection order; [] (or the key absent, or null)
// means nothing is selected; the older single object {"id": n, "ts": ...} is read as a one-entry list. Ids are
// screen ids (main = 1) and the badge shows the id. A new ts on an id already selected pops its outline again
// ("show me screen 3" twice flashes it twice). Call apply() on every house sync. bench/select_outline_test.cpp.
#pragma once
#include <map>
#include <string>
#include "json.h"
#include "select_outline.h"

namespace selout {

struct HouseSelect {
    std::map<int, std::string> ts;  // what we last applied, by screen id
    long applied = 0;
    void apply(const json::Value& house, Selection& sel, float now) {
        const json::Value& f = house["selected"];
        std::map<int, std::string> next;
        auto add = [&](const json::Value& e) { int id = e["id"].integer(-1); if (id >= 0) next[id] = e["ts"].str(); };
        if (f.type == json::Value::Arr) for (auto& e : f.a) add(e);
        else if (f.type == json::Value::Obj) add(f);
        for (auto& [id, t] : ts) if (!next.count(id)) sel.clear(id, now);
        for (auto& [id, t] : next) { auto it = ts.find(id); if (it == ts.end() || it->second != t) { sel.select(id, now); applied++; } }
        ts.swap(next);
    }
};

}  // namespace selout
