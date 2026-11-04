// CLAUDE chat across restarts (room_lab/reception_persist.h). g++ -std=c++17 -I room_lab bench/reception_persist_test.cpp && ./a.out
#include "reception_persist.h"
#include <cstdio>
using namespace recpersist;
static int fails = 0;
static void check(bool c, const char* m) { if (!c) { printf("FAIL %s\n", m); fails++; } }

int main() {
    std::vector<Msg> m;
    for (int i = 0; i < 12; i++) { m.push_back({"user", "question " + std::to_string(i)}); m.push_back({"assistant", "answer " + std::to_string(i)}); }
    m.push_back({"assistant", "a follow-up note"});
    auto k = lastExchanges(m, 5);
    check(k.size() == 11 && k.front().text == "question 7" && k.back().text == "a follow-up note", "last 5 exchanges start at the 5th last question, trailing notes kept");
    check(lastExchanges(m, 50).size() == m.size() && lastExchanges(m, 0).empty(), "fewer exchanges than asked: all; 0: none");

    // Round trip through the cache, with text that needs escaping (quotes, newlines, UTF-8, control).
    std::vector<Msg> w = {{"user", "say \"hi\"\nand \\ more"}, {"assistant", "Привет — ok\t\x01"}};
    bool ok = false; json::Value v = json::parse(toJson(w, "headset-1234"), &ok);
    auto r = fromJson(v);
    check(ok && v["conversation_id"].str() == "headset-1234", "cache is valid JSON with the conversation id");
    check(r.size() == 2 && r[0].text == w[0].text && r[1].text == w[1].text && r[0].role == "user", "round trip keeps the text exactly");

    // Server shapes: {"messages":[...]} with text or content, a bare array; empty or roleless entries dropped.
    auto s1 = fromJson(json::parse("{\"messages\":[{\"role\":\"user\",\"text\":\"a\"},{\"role\":\"assistant\",\"content\":\"b\"},{\"role\":\"assistant\",\"text\":\"\"},{\"text\":\"x\"}]}"));
    check(s1.size() == 2 && s1[1].text == "b", "server messages: text or content, empties dropped");
    auto s2 = fromJson(json::parse("[{\"role\":\"user\",\"text\":\"q\"},{\"role\":\"system\",\"text\":\"routed\"}]"));
    check(s2.size() == 2 && s2[1].role == "assistant", "bare array; other roles shown as the assistant side");
    check(fromJson(json::parse("{\"error\":\"unknown\"}")).empty(), "unknown conversation: nothing");
    check(pick({}, w, 5).size() == 2 && pick(s1, w, 5)[0].text == "a", "server history wins, the local cache otherwise");

    // Mic self-check: listening while callbacks flow; stalled after 2 s without; auto-restart at most every 5 s.
    MicCheck mc; long frames = 0; int restarts = 0; bool sawStall = false;
    for (float t = 0; t < 30; t += 0.02f) {
        bool alive = t < 10 || t > 22;  // the audio callback stops between 10 s and 22 s
        if (alive) frames++;
        if (mc.update(t, true, frames)) restarts++;
        if (t > 5 && t < 9.9f) check(mc.state == MicCheck::Listening, "listening while audio flows");
        if (t > 12.1f && t < 21.9f && mc.state != MicCheck::Stalled) sawStall = false; else if (t > 12.1f && t < 12.2f) sawStall = true;
    }
    check(sawStall, "stalled from 2 s after the audio stops until it flows again (a restart does not fake LISTENING)");
    check(restarts >= 2 && restarts <= 3 && mc.state == MicCheck::Listening, "auto-restart at 12, 17, 22 s (every 5 s while stalled), then listening again");
    printf("  stall 10-22 s: %d automatic restarts, label now \"%s\"\n", restarts, mc.label());
    MicCheck off; off.update(0, false, 0); check(off.state == MicCheck::Off && std::string(off.label()).find("PINCH") != std::string::npos, "mic off: says pinch to start");
    printf("reception persistence: %s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
