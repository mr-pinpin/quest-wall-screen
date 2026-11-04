// The CLAUDE (reception) chat survives app restarts (kanban K18). GL- and network-free pieces:
//  - History: the receptionist's GET history (work-42) and a local cache of the same shape (files/reception-history.json,
//    written whenever the chat settles), so a restart shows the conversation even when the server is unreachable.
//    Either way only the last N exchanges are kept (an exchange starts at a user message).
//  - Reopen: files/reception-open holds "1" while the panel is open; at start a "1" reopens it in front of the user.
//  - MicCheck: whether the mic is really listening. Recording but no audio callbacks for stallS = stalled (the user's
//    last speech did not arrive, 2026-11-04); the panel shows LISTENING / NOT LISTENING - PINCH TO RESTART, and a stall
//    restarts the mic by itself at most every retryS.
// bench/reception_persist_test.cpp.
#pragma once
#include <string>
#include <vector>
#include "json.h"

namespace recpersist {

struct Msg { std::string role, text; };

inline std::string esc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c == '\n') o += "\\n"; else if (c == '\r') o += "\\r"; else if (c == '\t') o += "\\t";
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o;
}
// The last n exchanges: everything from the n-th last user message on (all of it if there are fewer).
inline std::vector<Msg> lastExchanges(const std::vector<Msg>& m, int n) {
    if (n <= 0) return {};
    int seen = 0;
    for (int i = (int)m.size() - 1; i >= 0; i--) if (m[i].role == "user" && ++seen == n) return std::vector<Msg>(m.begin() + i, m.end());
    return m;
}
inline std::string toJson(const std::vector<Msg>& m, const std::string& conv) {
    std::string j = "{\"conversation_id\":\"" + esc(conv) + "\",\"messages\":[";
    for (size_t i = 0; i < m.size(); i++) j += std::string(i ? "," : "") + "{\"role\":\"" + esc(m[i].role) + "\",\"text\":\"" + esc(m[i].text) + "\"}";
    return j + "]}";
}
// Messages from a history document: {"messages":[{role, text|content}, ...]} or a bare array; empty texts dropped.
inline std::vector<Msg> fromJson(const json::Value& v) {
    const json::Value& a = v.type == json::Value::Arr ? v : v["messages"];
    std::vector<Msg> out;
    for (auto& h : a.a) {
        std::string role = h["role"].str(), text = h.has("text") ? h["text"].str() : h["content"].str();
        if (role.empty() || text.empty()) continue;
        out.push_back({role == "user" ? "user" : "assistant", text});
    }
    return out;
}
// Server history wins when it has anything; the local cache otherwise.
inline std::vector<Msg> pick(const std::vector<Msg>& server, const std::vector<Msg>& local, int n) {
    return lastExchanges(server.empty() ? local : server, n);
}

struct MicCheck {
    float stallS = 2.0f, retryS = 5.0f;
    enum State { Off, Listening, Stalled } state = Off;
    long lastFrames = -1; float framesAt = 0, restartAt = -100; long autoRestarts = 0;
    // Per frame: recording = the app thinks the mic is on; frames = the audio callback's running count.
    // Returns true when the mic should be restarted now (a stall, rate-limited).
    bool update(float t, bool recording, long frames) {
        if (frames != lastFrames) { lastFrames = frames; framesAt = t; }
        if (!recording) { state = Off; return false; }
        state = t - framesAt > stallS ? Stalled : Listening;
        if (state == Stalled && t - restartAt >= retryS) { restartAt = t; autoRestarts++; return true; }  // stays Stalled until audio flows
        return false;
    }
    // A restart the user asked for (pinch on the strip): counts like an automatic one for the rate limit.
    void restarted(float t) { restartAt = t; }
    const char* label() const { return state == Listening ? "LISTENING" : state == Stalled ? "NOT LISTENING - PINCH TO RESTART" : "MIC OFF - PINCH TO START"; }
};

}  // namespace recpersist
