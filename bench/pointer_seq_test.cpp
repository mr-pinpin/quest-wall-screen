// Main-screen pointer sequencing (room_lab/pointer_seq.h). g++ -std=c++17 -I room_lab bench/pointer_seq_test.cpp && ./a.out
#include "pointer_seq.h"
#include <cstdio>
#include <string>
#include <vector>
static int fails = 0;
static void check(bool c, const char* m) { if (!c) { printf("FAIL %s\n", m); fails++; } }
int main() {
    std::vector<std::string> log; pointerseq::Seq s;
    auto pos = [&](int x, int y) { log.push_back("pos " + std::to_string(x) + "," + std::to_string(y)); };
    auto press = [&](int x, int y) { s.anchor(x, y, pos); log.push_back("press"); s.anchor(x, y, pos); log.push_back("release"); };
    s.move(100, 100, pos); s.move(100, 100, pos); s.move(101, 100, pos);
    check(log.size() == 2, "moves deduped");
    // Something else moved the Mini's cursor; our pointer did not move; the user pinches: the position goes first anyway.
    log.clear(); press(101, 100);
    check(log.size() == 4 && log[0] == "pos 101,100" && log[1] == "press" && log[2] == "pos 101,100" && log[3] == "release", "press and release each preceded by the position");
    // Every press in a long run is preceded by a position event, whatever the dedupe state.
    log.clear();
    for (int i = 0; i < 50; i++) { if (i % 3) s.move(200 + i % 2, 300, pos); press(200 + i % 2, 300); }
    bool all = true; for (size_t i = 0; i < log.size(); i++) if ((log[i] == "press" || log[i] == "release") && (i == 0 || log[i - 1].rfind("pos ", 0) != 0)) all = false;
    check(all, "never a button without a position right before it");
    // Reconnect: the next move is sent even at the same spot.
    log.clear(); s.reset(); s.move(201, 300, pos); check(log.size() == 1, "reset: the next move goes out");
    printf("pointer sequence: %s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
