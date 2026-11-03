// Unit tests for the headset's laptop discovery (room_lab/hosts.h).
//   g++ -std=c++17 -I room_lab bench/hosts_test.cpp -o /tmp/hosts_test && /tmp/hosts_test [NAME.local [IFADDR]]
// With a name argument it also sends a real mDNS query (run on a machine on the house LAN, e.g. the Mini).
#include "hosts.h"
#include <cstdio>
#include <cstdlib>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void put16(std::vector<uint8_t>& v, int x) { v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x)); }
static void label(std::vector<uint8_t>& v, const char* s) { v.push_back(uint8_t(strlen(s))); v.insert(v.end(), s, s + strlen(s)); }

int main(int argc, char** argv) {
    // Query encoding.
    auto q = hosts::buildQuery("Miguels-MacBook-Air.local", 0x1234);
    const uint8_t want[] = {0x12, 0x34, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 19};
    CHECK(q.size() == 12 + 1 + 19 + 1 + 5 + 1 + 4 && !memcmp(q.data(), want, sizeof want), "query header/first label (size %zu)", q.size());
    CHECK(q[q.size() - 4] == 0 && q[q.size() - 3] == 1 && q[q.size() - 2] == 0x80 && q[q.size() - 1] == 1, "type A, class IN + QU");
    CHECK(hosts::buildQuery("bad..name", 1).empty() && hosts::buildQuery(std::string(64, 'a') + ".local", 1).empty(), "invalid names rejected");

    // A macOS-style answer: question echoed, answer name compressed to the question, plus an AAAA first.
    std::vector<uint8_t> r = {0x12, 0x34, 0x84, 0x00};
    put16(r, 1); put16(r, 2); put16(r, 0); put16(r, 0);
    label(r, "Miguels-MacBook-Air"); label(r, "local"); r.push_back(0); put16(r, 1); put16(r, 0x8001);
    r.insert(r.end(), {0xC0, 12}); put16(r, 28); put16(r, 0x8001); put16(r, 0); put16(r, 120); put16(r, 16); for (int i = 0; i < 16; i++) r.push_back(uint8_t(i));
    r.insert(r.end(), {0xC0, 12}); put16(r, 1); put16(r, 0x8001); put16(r, 0); put16(r, 120); put16(r, 4); r.insert(r.end(), {192, 168, 0, 104});
    CHECK(hosts::parseA(r.data(), r.size(), "miguels-macbook-air.local") == "192.168.0.104", "A after AAAA, compressed, case-insensitive: got '%s'", hosts::parseA(r.data(), r.size(), "miguels-macbook-air.local").c_str());
    CHECK(hosts::parseA(r.data(), r.size(), "other.local").empty(), "another host's record is ignored");
    std::vector<uint8_t> asQuery = r; asQuery[2] = 0;
    CHECK(hosts::parseA(asQuery.data(), asQuery.size(), "miguels-macbook-air.local").empty(), "queries are not answers");
    for (size_t cut = 0; cut < r.size(); cut++) hosts::parseA(r.data(), cut, "miguels-macbook-air.local");  // truncations must not crash
    std::vector<uint8_t> loop = {0, 0, 0x84, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0xC0, 12};
    CHECK(hosts::parseA(loop.data(), loop.size(), "x.local").empty(), "compression loop rejected");

    // The fallback chain, with files in a temp dir.
    char dir[] = "/tmp/hosts_testXXXXXX"; mkdtemp(dir); std::string d = dir;
    auto res = hosts::laptop(d, false, "192.168.0.102");
    CHECK(res.ip == "192.168.0.102" && res.via == "default", "default when nothing is known: %s via %s", res.ip.c_str(), res.via.c_str());
    std::ofstream(d + "/hosts-last.json") << "{\"LAPTOP\": \"192.168.0.104\"}";
    res = hosts::laptop(d, false, "192.168.0.102");
    CHECK(res.ip == "192.168.0.104" && res.via == "last known", "last known beats the default: %s via %s", res.ip.c_str(), res.via.c_str());
    std::ofstream(d + "/hosts.json") << "{\"LAPTOP\": \"192.168.0.150\", \"laptopName\": \"x\"}";
    res = hosts::laptop(d, true, "192.168.0.102");
    CHECK(res.ip == "192.168.0.150" && res.via == "hosts.json", "override beats everything: %s via %s", res.ip.c_str(), res.via.c_str());
    std::ofstream(d + "/hosts.json") << "{\"laptopName\": \"no-such-host-pinpin\"}";
    res = hosts::laptop(d, true, "192.168.0.102");
    CHECK(res.ip == "192.168.0.104" && res.via == "last known", "mDNS miss falls back to last known: %s via %s", res.ip.c_str(), res.via.c_str());

    if (argc > 1) { std::string ip = hosts::resolve(argv[1], 1500, argc > 2 ? argv[2] : nullptr); printf("live mDNS %s -> %s\n", argv[1], ip.empty() ? "(no answer)" : ip.c_str()); CHECK(!ip.empty(), "live query answered"); }
    printf("%d failures\n", fails);
    return fails ? 1 : 0;
}
