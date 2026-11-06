// HEARTBEAT (flag, default on; read-only telemetry): a read path that does not need adb. Every ~10 s the app POSTs a
// compact status (version, state, fps, battery, house rev applied, every screen with on/wall/x/y/w/h/source, the last
// errors and events) to the house server, POST /headset/status (house/server.py), so room.sh and /status can confirm
// what the headset shows even when adb and ping are gone. Targets: files/heartbeat-hosts ("host:port" per line, tried
// in order, the last one that worked first), else the house host. files/heartbeat-token, if present, is sent as
// X-Headset-Token. Nothing is ever read back or applied. GL-free; bench/heartbeat_test.cpp.
#pragma once
#include "house.h"
#include "json.h"
#include <string>
#include <vector>

namespace heartbeat {

struct Target { std::string host; int port = 57800; };

// "host" or "host:port" per line; blank lines and # comments skipped. None: the default.
inline std::vector<Target> parseTargets(const std::string& text, const Target& dflt) {
    std::vector<Target> out; size_t i = 0;
    while (i <= text.size()) {
        size_t e = text.find('\n', i); if (e == std::string::npos) e = text.size();
        std::string l = text.substr(i, e - i); i = e + 1;
        size_t h = l.find('#'); if (h != std::string::npos) l = l.substr(0, h);
        while (!l.empty() && isspace((unsigned char)l.back())) l.pop_back();
        while (!l.empty() && isspace((unsigned char)l[0])) l.erase(0, 1);
        if (l.empty()) continue;
        Target t; size_t c = l.find(':'); t.host = l.substr(0, c);
        if (c != std::string::npos) { int p = atoi(l.c_str() + c + 1); if (p <= 0 || p > 65535) continue; t.port = p; }
        if (!t.host.empty()) out.push_back(t);
    }
    if (out.empty()) out.push_back(dflt);
    return out;
}

// The compact view of the full status.json (room_lab status()). Only what a remote reader needs to confirm a change:
// a screen's on/size/position, the house rev applied, whether the app is running and healthy.
inline json::Value compact(const json::Value& st, int version, long seq, const std::vector<std::string>& events) {
    using house::num; using house::str; using house::obj; using house::arr;
    json::Value o = obj();
    o.o["v"] = num(version); o.o["seq"] = num((double)seq);
    for (const char* k : {"uptime", "state", "running", "fps"}) if (st.has(k)) o.o[k] = st[k];
    if (st.has("battery")) { json::Value b = obj(); b.o["pct"] = st["battery"]["pct"]; b.o["charging"] = st["battery"]["charging"]; o.o["battery"] = b; }
    if (st.has("house")) {
        json::Value h = obj();
        for (const char* k : {"on", "rev", "applied", "pending", "error"}) if (st["house"].has(k)) h.o[k] = st["house"][k];
        o.o["house"] = h;
    }
    json::Value sc = arr();
    for (auto& s : st["placement"]["screens"].a) {
        json::Value e = obj();
        for (const char* k : {"id", "on", "wall", "source", "x", "y", "w", "h", "stage"}) if (s.has(k)) e.o[k] = s[k];
        sc.a.push_back(e);
    }
    o.o["screens"] = sc;
    if (st.has("anchors")) o.o["roomOk"] = st["anchors"]["roomOk"];
    if (st.has("houseFrame")) o.o["frame"] = st["houseFrame"]["source"];
    if (st.has("laptop")) o.o["laptop"] = st["laptop"]["ip"];
    if (st.has("reception")) { json::Value r = obj(); r.o["open"] = st["reception"]["open"]; r.o["sent"] = st["reception"]["sent"]; o.o["reception"] = r; }
    json::Value er = arr(); const auto& all = st["errors"].a;
    for (size_t i = all.size() > 5 ? all.size() - 5 : 0; i < all.size(); i++) er.a.push_back(all[i]);
    o.o["errors"] = er;
    json::Value ev = arr(); for (auto& e : events) ev.a.push_back(str(e));
    o.o["events"] = ev;
    return o;
}

// Sends one body to the first target that answers 200, starting with the one that worked last. Blocking (5 s
// timeouts per target in house::http): call it off the render thread.
struct Sender {
    long sent = 0, failed = 0; int good = -1; std::string lastError, lastTarget;
    bool post(const std::vector<Target>& ts, const std::string& body, const std::string& token = "") {
        if (ts.empty()) { failed++; lastError = "no target"; return false; }
        std::vector<std::string> hd = {"Content-Type: application/json"};
        if (!token.empty()) hd.push_back("X-Headset-Token: " + token);
        std::vector<int> order; if (good >= 0 && good < (int)ts.size()) order.push_back(good);
        for (int i = 0; i < (int)ts.size(); i++) if (i != good) order.push_back(i);
        std::string err;
        for (int i : order) {
            house::Response r = house::http(ts[i].host, ts[i].port, "POST", "/headset/status", body, hd);
            if (r.status == 200) { sent++; good = i; lastError.clear(); lastTarget = ts[i].host + ":" + std::to_string(ts[i].port); return true; }
            err += (err.empty() ? "" : "; ") + ts[i].host + ": " + (r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error);
        }
        failed++; lastError = err; return false;
    }
};

}  // namespace heartbeat
