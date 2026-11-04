// Claude on every screen (flag ASSIST; docs/prd/assistant.md, protocol agreed with work-42). GL-free parts:
//  - Sse: a Server-Sent Events parser that accepts plain or chunked HTTP bodies, fed in arbitrary pieces.
//  - Chat: one screen's conversation. A user turn, then assistant events {type:"text",delta} appended,
//    {type:"action",label} listed under the bubble, {type:"done"} / {type:"error",message}; every event may carry
//    "turn" and events of an older turn are ignored, so a late stream never bleeds into the next answer.
//  - wrap(): Markdown-lite to display lines (paragraphs, # headings, - bullets, ``` code, **bold** / `code` / links
//    shown as plain text), word-wrapped to a column count in code points.
//  - send(): POST /session/<id>/message and stream the reply into a Chat (blocking; room_lab runs it on a thread).
// bench/assist_test.cpp covers the parser, the model, wrapping and send() against a mock server.
#pragma once
#include "house.h"
#include "font_unifont.h"
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace assist {

// ---- SSE over HTTP (headers, then a plain or chunked body).
struct Sse {
    std::string head, body, chunkBuf, ev; bool inBody = false, chunked = false; size_t chunkLeft = 0; bool chunkCrlf = false;
    int status = 0; bool ended = false;
    std::function<void(const json::Value&)> onEvent;
    void feed(const char* p, size_t n) {
        std::string in(p, n);
        if (!inBody) {
            head += in; size_t e = head.find("\r\n\r\n"); if (e == std::string::npos) return;
            in = head.substr(e + 4); head.resize(e); inBody = true;
            sscanf(head.c_str(), "HTTP/%*s %d", &status);
            std::string low = head; for (auto& c : low) c = (char)tolower(c);
            chunked = low.find("transfer-encoding: chunked") != std::string::npos;
        }
        if (!chunked) { lines(in); return; }
        chunkBuf += in;
        for (;;) {
            if (chunkCrlf) { if (chunkBuf.size() < 2) return; chunkBuf.erase(0, 2); chunkCrlf = false; }
            if (chunkLeft == 0) {
                size_t e = chunkBuf.find("\r\n"); if (e == std::string::npos) return;
                chunkLeft = strtoul(chunkBuf.substr(0, e).c_str(), nullptr, 16); chunkBuf.erase(0, e + 2);
                if (chunkLeft == 0) { ended = true; return; }
            }
            size_t take = std::min(chunkLeft, chunkBuf.size()); if (!take) return;
            lines(chunkBuf.substr(0, take)); chunkBuf.erase(0, take); chunkLeft -= take;
            if (chunkLeft == 0) chunkCrlf = true;
        }
    }
    void lines(const std::string& s) {
        body += s;
        for (size_t e; (e = body.find('\n')) != std::string::npos;) {
            std::string l = body.substr(0, e); body.erase(0, e + 1);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            if (l.empty()) { if (!ev.empty() && onEvent) { bool ok = false; json::Value v = json::parse(ev, &ok); if (ok) onEvent(v); } ev.clear(); continue; }
            if (l[0] == ':') continue;  // keepalive comment
            if (l.rfind("data:", 0) == 0) { std::string d = l.substr(5); if (!d.empty() && d[0] == ' ') d.erase(0, 1); ev += d; }
        }
    }
};

// ---- One screen's conversation.
struct Turn { std::string role, text, turn; std::vector<std::string> actions; bool done = false, error = false; };
struct Chat {
    std::vector<Turn> turns; std::string state = "idle";  // idle, sending, streaming, error
    long version = 0;                                      // bumps on every change (re-layout when it moves)
    void user(const std::string& text, const std::string& turn) {
        turns.push_back({"user", text, turn, {}, true, false});
        turns.push_back({"assistant", "", turn, {}, false, false});
        state = "sending"; version++;
    }
    Turn* current(const std::string& turn) {
        for (auto it = turns.rbegin(); it != turns.rend(); ++it) if (it->role == "assistant") return (turn.empty() || it->turn == turn) ? &*it : nullptr;
        return nullptr;
    }
    void apply(const json::Value& e) {
        std::string tid = e.has("turn") ? e["turn"].str() : e["turnId"].str();
        Turn* t = current(tid); if (!t || t->done) return;
        std::string ty = e["type"].str();
        if (ty == "start") { state = "streaming"; return; }
        if (ty == "text") { t->text += e["delta"].str(); state = "streaming"; }
        else if (ty == "action") {  // {label} or {action, detail, status: begin|ok|error}: one line per action, marked on error
            std::string st = e["status"].str(), label = e.has("label") ? e["label"].str() : e["action"].str() + (e["detail"].str().empty() ? "" : " " + e["detail"].str());
            if (st == "ok") return;
            t->actions.push_back(st == "error" ? label + " (failed)" : label);
        }
        else if (ty == "done") { t->done = true; state = "idle"; }
        else if (ty == "error") { t->done = t->error = true; t->text += (t->text.empty() ? "" : "\n") + std::string("Error: ") + e["message"].str(); state = "error"; }
        else return;
        version++;
    }
    // History from GET /session/<id>: its "messages" [{role, content (or text), actions:[...]}].
    void load(const json::Value& arr) {
        turns.clear();
        for (auto& h : arr.a) { Turn t{h["role"].str(), h.has("text") ? h["text"].str() : h["content"].str(), h["turn"].str(), {}, true, false}; for (auto& a : h["actions"].a) t.actions.push_back(a.str()); turns.push_back(t); }
        version++;
    }
};

// ---- Markdown-lite to lines.
struct Line { std::string text; int style = 0; bool user = false; };  // style: 0 text, 1 heading, 2 code, 3 action
inline std::string inlinePlain(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s.compare(i, 2, "**") == 0 || s.compare(i, 2, "__") == 0) { i++; continue; }
        if (s[i] == '`') continue;
        if (s[i] == '[') {  // [text](url) -> text
            size_t close = s.find("](", i), end = close == std::string::npos ? close : s.find(')', close);
            if (close != std::string::npos && end != std::string::npos) { o += s.substr(i + 1, close - i - 1); i = end; continue; }
        }
        o += s[i];
    }
    return o;
}
inline void wrapInto(std::vector<Line>& out, const std::string& text, size_t cols, int style, bool user, const std::string& indent = "", const std::string& first = "") {
    std::string line = first; size_t len = unifont::length(first);
    size_t i = 0;
    auto flush = [&]() { out.push_back({line, style, user}); line = indent; len = unifont::length(indent); };
    while (i < text.size()) {
        size_t j = text.find(' ', i); if (j == std::string::npos) j = text.size();
        std::string word = text.substr(i, j - i); size_t wl = unifont::length(word);
        bool lead = len == unifont::length(line.empty() ? first : indent) && (line == first || line == indent);
        if (!lead && len + 1 + wl > cols) flush();
        lead = (line == first && len == unifont::length(first)) || (line == indent && len == unifont::length(indent));
        while (wl > cols - std::min(len, cols - 1)) {  // a word longer than a line: hard break
            size_t room = cols - len, k = 0, b = 0; while (k < room && b < word.size()) { unifont::next(word, b); k++; }
            line += word.substr(0, b); word.erase(0, b); wl -= k; flush(); lead = true;
        }
        if (!lead) { line += ' '; len++; }
        line += word; len += wl; i = j + 1;
    }
    if (line != indent || out.empty() || !first.empty()) out.push_back({line, style, user});
}
inline std::vector<Line> wrap(const Chat& c, size_t cols) {
    std::vector<Line> out;
    for (auto& t : c.turns) {
        bool u = t.role == "user"; bool code = false;
        size_t start = 0;
        std::string txt = t.text.empty() && !u && !t.done ? "..." : t.text;
        while (start <= txt.size()) {
            size_t e = txt.find('\n', start); if (e == std::string::npos) e = txt.size();
            std::string l = txt.substr(start, e - start); start = e + 1;
            if (l.rfind("```", 0) == 0) { code = !code; if (e >= txt.size()) break; continue; }
            if (code) { std::string s = l; while (unifont::length(s) > cols) { size_t b = 0, k = 0; while (k < cols) { unifont::next(s, b); k++; } out.push_back({s.substr(0, b), 2, u}); s.erase(0, b); } out.push_back({s, 2, u}); }
            else if (l.empty()) out.push_back({"", 0, u});
            else if (l[0] == '#') { size_t h = l.find_first_not_of('#'); wrapInto(out, inlinePlain(l.substr(h == std::string::npos ? l.size() : h + (l[h] == ' '))), cols, 1, u); }
            else if (l.rfind("- ", 0) == 0 || l.rfind("* ", 0) == 0) wrapInto(out, inlinePlain(l.substr(2)), cols, 0, u, "  ", "\xE2\x80\xA2 ");
            else wrapInto(out, inlinePlain(l), cols, 0, u);
            if (e >= txt.size()) break;
        }
        for (auto& a : t.actions) out.push_back({"> " + a, 3, u});
        out.push_back({"", 0, u});
    }
    return out;
}

// ---- POST /session/<id>/message, streaming the reply into chat (under lock). Returns "" or an error.
inline std::string send(const std::string& host, int port, int screen, const std::string& text, const std::string& turn, Chat& chat, std::mutex& lock, int timeoutS = 60) {
    json::Value b = house::obj(); b.o["text"] = house::str(text); b.o["turn"] = house::str(turn);
    std::string body = house::dump(b);
    addrinfo hints{}, *ai = nullptr; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &ai) != 0 || !ai) return "resolve";
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    timeval tv{timeoutS, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (fd < 0 || connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) { freeaddrinfo(ai); if (fd >= 0) close(fd); return "connect"; }
    freeaddrinfo(ai);
    std::string req = "POST /session/" + std::to_string(screen) + "/message HTTP/1.1\r\nHost: " + host + "\r\nAccept: text/event-stream\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
                      std::to_string(body.size()) + "\r\n\r\n" + body;
    if (::send(fd, req.data(), req.size(), 0) != (ssize_t)req.size()) { close(fd); return "send"; }
    Sse sse; std::string err;
    sse.onEvent = [&](const json::Value& e) { std::lock_guard<std::mutex> g(lock); chat.apply(e); };
    char buf[4096];
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n <= 0) { if (n < 0) err = "timeout"; break; }
        sse.feed(buf, (size_t)n);
        if (sse.inBody && sse.status != 200) { err = "HTTP " + std::to_string(sse.status); break; }
        if (sse.ended) break;
        { std::lock_guard<std::mutex> g(lock); Turn* t = chat.current(turn); if (t && t->done) break; }
    }
    close(fd);
    std::lock_guard<std::mutex> g(lock);
    Turn* t = chat.current(turn);
    if (t && !t->done) { if (err.empty()) err = "stream ended early"; json::Value e = house::obj(); e.o["type"] = house::str("error"); e.o["message"] = house::str(err); e.o["turn"] = house::str(turn); chat.apply(e); }
    return err;
}

}  // namespace assist
