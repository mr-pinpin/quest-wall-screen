// flags.h against the real registry (room_lab/flags.json).   g++ -std=c++17 -I room_lab bench/flags_test.cpp
#include "flags.h"
#include <cstdio>
#include <fstream>
#include <sstream>
static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
int main(int argc, char** argv) {
    std::ifstream f(argc > 1 ? argv[1] : "room_lab/flags.json"); std::stringstream ss; ss << f.rdbuf();
    flagreg::Registry r; CHECK(r.load(ss.str()), "registry did not load");
    CHECK(r.find("QUAD") && r.find("QUAD")->state == "on" && r.find("QUAD")->def, "QUAD on by default");
    CHECK(r.find("sender:vt") && !r.find("vt"), "sender flags are prefixed");
    CHECK(r.hint("QUAD", "SHARPER MAIN SCREEN") == "SHARPER MAIN SCREEN", "a shown flag keeps its menu hint");
    flagreg::Registry h;
    h.load(R"({"flags":[{"name":"X","where":"headset","state":"hidden","category":"perf","default":false,"reason":"No measurable gain in 3 A/B pairs. Details in bench."},
                        {"name":"Y","where":"headset","state":"hidden","category":"perf","default":false,"reason":"long","hint":"no gain on wi-fi"}]})");
    CHECK(h.hidden("X") && h.hint("X", "OLD") == "NO MEASURABLE GAIN IN 3 A/B PAIRS", "hidden hint from the reason: %s", h.hint("X", "OLD").c_str());
    CHECK(h.hint("Y", "OLD") == "NO GAIN ON WI-FI", "explicit hint");
    CHECK(!h.load("{not json"), "bad json refused");
    printf("%d failures\n", failures); return failures ? 1 : 0;
}
