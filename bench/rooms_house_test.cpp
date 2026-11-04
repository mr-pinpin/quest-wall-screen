// MULTIROOM <-> house file (room_lab/rooms_house.h): rooms and links read from a house doc, learned links written back
// as edits, applied with house::apply and dumped for house/schema.py.
//   g++ -std=c++17 -I room_lab bench/rooms_house_test.cpp && ./a.out out.json && python3 bench/rooms_schema_check.py out.json
#include "rooms_house.h"
#include <cstdio>
#include <fstream>
using namespace rooms;
static int fails = 0;
static void check(bool c, const char* m) { if (!c) { printf("FAIL %s\n", m); fails++; } }
int main(int argc, char** argv) {
    const char* src = R"({"schema":1,"rev":3,"nextScreenId":1,"by":"test","updated":"2026-11-04T07:00:00Z","rooms":[
      {"id":"r1","name":"home","anchors":[{"uuid":"0123456789abcdef0123456789abcdef","pose":{"p":[0,1,-2.5],"q":[0,0,0,1]}}]},
      {"id":"r2","name":"room 2","anchors":[{"uuid":"fedcba9876543210fedcba9876543210","pose":{"p":[0,1,-1.5],"q":[0,0,0,1]}}],
       "floor":[[-1.5,-1.5],[1.5,-1.5],[1.5,1.5],[-1.5,1.5]]}],
     "links":[{"id":"r2~r1","a":"r2","b":"r1","pose":{"p":[1,0,4.5],"q":[0,-0.7071068,0,0.7071068]}}],
     "walls":[{"id":"w1","room":"r1","kind":"wall","source":"scene","scene":[],"pose":{"p":[0,1.3,-2.5],"q":[0,0,0,1]},"w":4,"h":2.6},
              {"id":"w2","room":"r1","kind":"wall","source":"scene","scene":[],"pose":{"p":[2,1.3,0],"q":[0,-0.7071068,0,0.7071068]},"w":5,"h":2.6},
              {"id":"f1","room":"r1","kind":"floor","source":"depth","scene":[],"pose":{"p":[0,0,0],"q":[-0.7071068,0,0,0.7071068]},"w":9,"h":9}],
     "shapes":[],"screens":[],"menu":{},"settings":{}})";
    bool ok = false; json::Value doc = json::parse(src, &ok); check(ok, "fixture parses");
    std::vector<Room> rs; std::vector<Link> ls; load(doc, rs, ls);
    check(rs.size() == 2 && rs[0].anchors.size() == 1 && rs[1].anchors.size() == 1, "rooms and anchors read");
    check(rs[0].hasBox && std::fabs(rs[0].x0 + 2) < 0.01f && std::fabs(rs[0].x1 - 2) < 0.01f && std::fabs(rs[0].z0 + 2.5f) < 0.01f && std::fabs(rs[0].z1 - 2.5f) < 0.01f, "r1 box from its walls (floor plane ignored)");
    check(rs[1].hasBox && std::fabs(rs[1].x1 - 1.5f) < 0.01f, "r2 box from its floor");
    check(ls.size() == 1 && ls[0].a == "r2" && ls[0].b == "r1", "stored link read");
    // A session sees both rooms' anchors: the learned r1~r2 replaces the stored r2~r1.
    Pose r1In{anchors::yawQuat(0.3f), {1, 0, 2}}, r2InR1{anchors::yawQuat(1.5708f), {4.5f, 0, 1.0f}};
    Learner L;
    for (int i = 0; i < 6; i++) {
        std::vector<anchors::Seen> seen = {{rs[0].anchors[0].uuid, compose(r1In, rs[0].anchors[0].inRoom)}, {rs[1].anchors[0].uuid, compose(compose(r1In, r2InR1), rs[1].anchors[0].inRoom)}};
        L.sample(place(rs, ls, seen));
    }
    std::vector<std::string> changed; auto merged = L.merge(ls, &changed);
    auto edits = linkEdits(merged, ls, changed, "2026-11-04T07:00:00Z");
    check(edits.size() == 2 && edits[0].obj.type == json::Value::Null && edits[0].id == "r2~r1" && edits[1].id == "r1~r2", "reverse link removed, new one written");
    for (auto& e : edits) house::apply(doc, e);
    check(doc["links"].a.size() == 1 && doc["links"][0]["id"].str() == "r1~r2", "house doc has the one link");
    std::vector<Room> rs2; std::vector<Link> ls2; load(doc, rs2, ls2);
    check(ls2.size() == 1 && wallfit::len(ls2[0].bInA.p - r2InR1.p) < 0.01f && anchors::angleDeg(ls2[0].bInA.q, r2InR1.q) < 0.5f, "round trip through the doc");
    if (argc > 1) { std::ofstream o(argv[1]); o << house::dump(doc); }
    printf("rooms_house: %s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
