// ASSIST headset side (room_lab/assist.h): SSE parsing (plain, chunked, byte by byte), the chat model, Markdown-lite
// wrapping, and send() against bench/assist_mock.py. Run through bench/assist_test.sh.
#include "assist.h"
#include <cstdio>
using namespace assist;
static int fails = 0;
static void check(bool c, const char* m) { if (!c) { printf("FAIL %s\n", m); fails++; } }
int main(int argc, char** argv) {
    // SSE: events split anywhere, keepalives ignored, chunked bodies decoded.
    std::string evs = ": keepalive\n\ndata: {\"type\":\"start\",\"turnId\":\"t1\"}\n\ndata: {\"type\":\"text\",\"delta\":\"Hola \\u00e9\",\"turnId\":\"t1\"}\n\n"
                      "data: {\"type\":\"done\",\"turnId\":\"t1\",\"stopReason\":\"end\"}\n\n";
    for (int mode = 0; mode < 2; mode++) {
        std::string http = std::string("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n") + (mode ? "Transfer-Encoding: chunked\r\n" : "") + "\r\n";
        if (!mode) http += evs;
        else { for (size_t i = 0; i < evs.size(); i += 7) { std::string c = evs.substr(i, 7); char h[16]; snprintf(h, sizeof h, "%zx\r\n", c.size()); http += h + c + "\r\n"; } http += "0\r\n\r\n"; }
        Sse s; std::vector<std::string> types; s.onEvent = [&](const json::Value& e) { types.push_back(e["type"].str()); };
        for (char c : http) s.feed(&c, 1);
        check(s.status == 200 && types.size() == 3 && types[1] == "text", mode ? "chunked SSE byte by byte" : "plain SSE byte by byte");
        if (mode) check(s.ended, "chunked end seen");
    }
    // Chat: deltas append, actions listed (ok skipped, error marked), a stale turn ignored, done closes the turn.
    Chat c; c.user("open youtube", "t1");
    json::Value e;
    auto ev = [&](const char* j) { c.apply(json::parse(j)); };
    ev("{\"type\":\"text\",\"delta\":\"Opening \",\"turnId\":\"t1\"}");
    ev("{\"type\":\"action\",\"action\":\"open_url\",\"detail\":\"youtube.com\",\"status\":\"begin\",\"turnId\":\"t1\"}");
    ev("{\"type\":\"action\",\"action\":\"open_url\",\"detail\":\"youtube.com\",\"status\":\"ok\",\"turnId\":\"t1\"}");
    ev("{\"type\":\"action\",\"action\":\"click\",\"status\":\"error\",\"turnId\":\"t1\"}");
    ev("{\"type\":\"text\",\"delta\":\"YouTube.\",\"turnId\":\"t0\"}");  // an older turn: ignored
    ev("{\"type\":\"text\",\"delta\":\"YouTube.\",\"turnId\":\"t1\"}");
    check(c.turns.back().text == "Opening YouTube.", "deltas append, stale turn ignored");
    check(c.turns.back().actions.size() == 2 && c.turns.back().actions[1] == "click (failed)", "actions: ok skipped, error marked");
    ev("{\"type\":\"done\",\"turnId\":\"t1\"}"); check(c.turns.back().done && c.state == "idle", "done closes the turn");
    ev("{\"type\":\"text\",\"delta\":\"late\",\"turnId\":\"t1\"}"); check(c.turns.back().text == "Opening YouTube.", "nothing after done");
    // A local greeting is a finished assistant line and does not capture the next turn's events.
    Chat g; g.greet("Hi, what do you want on this screen?"); g.user("open news", "t5");
    g.apply(json::parse("{\"type\":\"text\",\"delta\":\"Opening.\",\"turnId\":\"t5\"}"));
    check(g.turns[0].text == "Hi, what do you want on this screen?" && g.turns.back().text == "Opening.", "greeting stays, reply goes to the new turn");
    // Wrapping: headings, bullets with hanging indent, code kept verbatim, links and bold as text, UTF-8 counted by code points.
    Chat m; m.user("q", "t2"); m.turns.back().text = "# Plan\nUse **bold** and [the docs](http://x) here please now\n- a bullet that is long enough to wrap around\n```\ncode line that is long\n```\nЖЖЖЖЖЖЖЖЖЖЖЖ";
    m.turns.back().done = true;
    auto L = wrap(m, 12);
    bool heading = false, bullet = false, hang = false, code = false, plain = true, width = true;
    for (size_t i = 0; i < L.size(); i++) {
        if (L[i].style == 1 && L[i].text == "Plan") heading = true;
        if (L[i].text.rfind("\xE2\x80\xA2 ", 0) == 0) { bullet = true; if (i + 1 < L.size() && L[i + 1].text.rfind("  ", 0) == 0) hang = true; }
        if (L[i].style == 2) code = true;
        if (L[i].text.find("**") != std::string::npos || L[i].text.find("](") != std::string::npos) plain = false;
        if (unifont::length(L[i].text) > 12) width = false;
    }
    check(heading && bullet && hang && code && plain && width, "markdown-lite wrapping");
    check(L[0].user && L[0].text == "q", "user turn first");
    // The panel image: lit pixels for text, the last lines shown, scroll clamped, user lines tinted.
    Raster R; R.w = 64 * 8 + 12; R.h = 10 * lineH() + 8;
    int sc = render(L, 0, R); size_t lit = 0; for (auto p : R.px) lit += (p & 0xff) > 150;
    check(sc == 0 && lit > 200, "panel rasterised");
    int clamped = render(L, 1000, R); check(clamped == std::max(0, (int)L.size() - 10), "scroll clamped to the first line");
    Raster U; U.w = 100; U.h = 30; render({{"hi", 0, true}}, 0, U); check(U.px[(size_t)10 * 100 + 90] == rgba(28, 44, 70, 235), "user line tinted");
    // send() against the mock server: the reply streams in, and a server that dies mid-turn leaves an error.
    if (argc > 1) {
        int port = atoi(argv[1]); std::mutex lk; Chat s; s.user("hello", "T9");
        std::string err = send("127.0.0.1", port, "2", "hello", "T9", s, lk, 10);
        check(err.empty() && s.turns.back().done && s.turns.back().text == "Hi from screen 2.", "send(): streamed reply");
        check(s.turns.back().actions.size() == 1, "send(): action listed");
        Chat d; d.user("die", "T10");
        err = send("127.0.0.1", port, "2", "die", "T10", d, lk, 10);
        check(!err.empty() && d.turns.back().error, "send(): a dropped stream becomes an error in the bubble");
        Chat rm; rm.user("how many screens", "T11");
        err = send("127.0.0.1", port, "room", "how many screens", "T11", rm, lk, 10);
        check(err.empty() && rm.turns.back().text == "Hi from screen room.", "send(): the room session by name");
    }
    // The receptionist (work-42, /api/reception/turn): routed shown under the reply, keepalives ignored, switched.
    if (argc > 1) {
        std::mutex lk; Chat rc; rc.greet("Hi, this goes to work-df."); rc.user("hello captain", "h1");
        std::string err = reception("127.0.0.1", atoi(argv[1]), "headset-test", "hello captain", rc, lk);
        check(err.empty() && rc.state == "idle" && rc.dest == "work-df", "reception: routed to work-df, done");
        check(rc.turns.back().text == "Got it: hello captain (headset-test)" && rc.turns.back().actions.size() == 1 && rc.turns.back().actions[0] == "to work-df", "reception: reply text and the destination line");
        rc.user("talk to work-c4", "h2"); reception("127.0.0.1", atoi(argv[1]), "headset-test", "talk to work-c4", rc, lk);
        check(rc.dest == "work-c4" && rc.turns.back().actions[0] == "now talking to work-c4", "reception: switched");
    }
    printf("assist: %s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
