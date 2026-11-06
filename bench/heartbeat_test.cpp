// HEARTBEAT (room_lab/heartbeat.h) against the real house/server.py. Run through bench/heartbeat_test.sh, which starts a
// throwaway server on 127.0.0.1:  heartbeat_test <port> <data dir>
#include "heartbeat.h"
#include <cstdio>
#include <fstream>
static int fails = 0;
static void check(bool c, const char* m) { if (!c) { printf("FAIL %s\n", m); fails++; } }
// A full status.json as room_lab writes it (trimmed to what compact() reads, plus noise it must drop).
static const char* kStatus = R"({"uptime":31.2,"state":5,"running":1,"fps":72,"frames":2166,"head":[0,0,0],
 "hands":[{"active":1}],"scene":{"state":"loaded","entities":[{"uuid":"w-d12ed9f"}]},
 "battery":{"pct":87,"charging":1,"toasts":0},"laptop":{"ip":"192.168.0.105","via":"mdns"},
 "house":{"on":1,"rev":35,"applied":35,"pending":0,"gets":3,"puts":0,"conflicts":0,"error":""},
 "placement":{"stage":1,"screens":[{"id":1,"on":1,"wall":"851ff933","source":"scene","x":-0.374,"y":0.475,"w":2.4,"h":1.5,"stage":[-2.46,2.11,-1.13]},
   {"id":3,"on":0,"wall":"aeda1f4d","source":"scene","x":0.728,"y":0.177,"w":0.9,"h":0.5625}]},
 "anchors":{"roomOk":1,"stored":3},"houseFrame":{"source":"scene","used":13},
 "reception":{"on":1,"open":0,"sent":4,"dictq":{"on":1}},
 "errors":["e1","e2","e3","e4","e5","e6","e7"]})";
int main(int argc, char** argv) {
    if (argc < 3) { printf("usage: heartbeat_test <port> <dir>\n"); return 2; }
    int port = atoi(argv[1]); std::string dir = argv[2];
    // Targets.
    heartbeat::Target d{"192.168.0.50", 57800};
    auto t = heartbeat::parseTargets("", d); check(t.size() == 1 && t[0].host == "192.168.0.50" && t[0].port == 57800, "no file: the house host");
    t = heartbeat::parseTargets("# comment\n 10.0.0.1:9 \n\nair.local\nbad:0\n", d);
    check(t.size() == 2 && t[0].host == "10.0.0.1" && t[0].port == 9 && t[1].host == "air.local" && t[1].port == 57800, "parsed, bad port skipped");
    // Compact view.
    bool ok = false; json::Value st = json::parse(kStatus, &ok); check(ok, "fixture parses");
    json::Value c = heartbeat::compact(st, 76, 12, {"house: applied rev 35"});
    std::string body = house::dump(c);
    check(c["v"].integer() == 76 && c["seq"].integer() == 12 && c["fps"].integer() == 72, "version, seq, fps");
    check(c["house"]["applied"].integer() == 35 && !c["house"].has("gets"), "house rev applied, noise dropped");
    check(c["screens"].a.size() == 2 && c["screens"][1]["id"].integer() == 3 && c["screens"][1]["on"].integer() == 0, "screen 3 off");
    check(std::fabs(c["screens"][1]["w"].num() - 0.9) < 1e-6, "screen 3 width 0.90 m");
    check(c["screens"][0]["stage"].a.size() == 3, "stage position kept");
    check(c["errors"].a.size() == 5 && c["errors"][4].str() == "e7", "last 5 errors");
    check(c["events"][0].str() == "house: applied rev 35", "events");
    check(!c.has("scene") && !c.has("hands") && c["laptop"].str() == "192.168.0.105" && c["frame"].str() == "scene", "compact");
    check(body.size() < 2048, "under 2 KB");
    // Sender: a dead first target, then the server; the one that worked is tried first next time.
    heartbeat::Sender s; std::vector<heartbeat::Target> ts = {{"127.0.0.1", 1}, {"127.0.0.1", port}};
    check(s.post(ts, body), "posted past a dead target");
    check(s.good == 1 && s.sent == 1 && s.lastTarget == "127.0.0.1:" + std::to_string(port), "good target remembered");
    house::Response r = house::http("127.0.0.1", port, "GET", "/headset/status");
    json::Value back = json::parse(r.body, &ok);
    check(r.status == 200 && ok && back["seq"].integer() == 12 && back["screens"][1]["w"].num() > 0.89, "read back from the server");
    check(back["from"].str() == "127.0.0.1" && back["received"].str().size() == 20, "server stamps from and received");
    // All targets dead: counted, error says which.
    std::vector<heartbeat::Target> dead = {{"127.0.0.1", 1}}; heartbeat::Sender s2; check(!s2.post(dead, body) && s2.failed == 1 && s2.lastError.find("127.0.0.1") != std::string::npos, "all dead: failure");
    // Token.
    { std::ofstream f(dir + "/headset-token"); f << "tok\n"; }
    heartbeat::Sender s3; check(!s3.post(ts, body) && s3.lastError.find("HTTP 403") != std::string::npos, "token missing: 403");
    check(s3.post(ts, body, "tok"), "token sent: accepted");
    printf("heartbeat: %s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
