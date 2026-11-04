// House client (room_lab/house.h) against the real house/server.py. Run through bench/house_client_test.sh, which
// starts a throwaway server on 127.0.0.1 with a copy of house/house.json:  house_client_test <port> <dir>
#include "house.h"
#include <cstdio>
#include <sys/stat.h>
static int fails = 0;
static void check(bool c, const char* m) { if (!c) { printf("FAIL %s\n", m); fails++; } }
static json::Value screen(const json::Value& doc, int id) { for (auto& s : doc["screens"].a) if (s["id"].integer() == id) return s; return {}; }
static house::Edit moveScreen(const house::Store& st, int id, const char* k, double v) {
    json::Value s = screen(st.doc, id); s.o[k] = house::num(v); return {"screens", std::to_string(id), s};
}
int main(int argc, char** argv) {
    if (argc < 3) { printf("usage: house_client_test <port> <dir>\n"); return 2; }
    int port = atoi(argv[1]); std::string dir = argv[2];
    // SHA-256 test vectors.
    check(house::sha256("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256 empty");
    check(house::sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256 abc");
    check(house::sha256(std::string(1000, 'a')).size() == 64, "sha256 multi-block");
    // Writer round trip.
    std::string t = "{\"a\":[1,2.5,-3e-07,true,null,\"x\\\"y\\n\"],\"b\":{\"c\":0}}";
    json::Value v = json::parse(t); std::string d1 = house::dump(v), d2 = house::dump(json::parse(d1));
    check(d1 == d2, "writer round trip"); check(d1.find("2.5") != std::string::npos && d1.find("[1,") != std::string::npos, "numbers kept");

    std::string a = dir + "/a", b = dir + "/b"; mkdir(a.c_str(), 0700); mkdir(b.c_str(), 0700);
    house::Store A(a); A.host = "127.0.0.1"; A.port = port;
    check(!A.loadCache(), "no cache at first");
    check(A.fetch(), "fetch rev 1"); check(A.rev == 1, "rev 1");
    check(A.doc["screens"].size() == 3 && screen(A.doc, 2)["wall"].str() == "w-df0486f4", "migrated screens");
    // An edit goes up as one PUT and comes back after a restart.
    A.edit(moveScreen(A, 2, "x", -0.5));
    check(std::fabs(screen(A.doc, 2)["x"].num() + 0.5) < 1e-9, "edit applied locally at once");
    check(A.push("test move screen 2"), "push"); check(A.rev == 2 && A.pending.empty(), "rev 2, nothing pending");
    house::Store A2(a); A2.host = "127.0.0.1"; A2.port = port;
    check(A2.loadCache() && A2.rev == 2 && std::fabs(screen(A2.doc, 2)["x"].num() + 0.5) < 1e-9, "cache survives a restart");
    // Two writers on rev 2: the second gets 409, re-applies its edit on top and both edits survive.
    house::Store B(b); B.host = "127.0.0.1"; B.port = port; check(B.fetch() && B.rev == 2, "B on rev 2");
    A.edit(moveScreen(A, 3, "y", 0.11)); check(A.push(), "A pushes rev 3");
    B.edit(moveScreen(B, 2, "w", 1.2)); check(B.push(), "B pushes after a conflict");
    check(B.conflicts == 1 && B.rev == 4, "one conflict, rev 4");
    check(A.fetch() && A.rev == 4, "A sees rev 4");
    check(std::fabs(screen(A.doc, 3)["y"].num() - 0.11) < 1e-9 && std::fabs(screen(A.doc, 2)["w"].num() - 1.2) < 1e-9 &&
          std::fabs(screen(A.doc, 2)["x"].num() + 0.5) < 1e-9, "both edits kept");
    // Offline: edits queue on disk, survive a restart, and go up when the server is back.
    A.port = 1; A.edit(moveScreen(A, 2, "h", 0.75));
    check(!A.push() && A.pending.size() == 1, "offline push fails, edit queued");
    house::Store A3(a); A3.host = "127.0.0.1"; A3.port = port;
    check(A3.loadCache() && A3.pending.size() == 1 && std::fabs(screen(A3.doc, 2)["h"].num() - 0.75) < 1e-9, "queued edit survives a restart");
    check(A3.push() && A3.rev == 5 && A3.pending.empty(), "queued edit sent when back online");
    // A whole-object section and a delete.
    json::Value set = A3.doc["settings"]; set.o["locked"].type = json::Value::Bool; set.o["locked"].b = true;
    A3.edit({"settings", "", set});
    A3.edit({"screens", "3", json::Value{}});
    check(A3.push() && A3.rev == 6, "settings + delete pushed");
    house::Store C(dir); C.host = "127.0.0.1"; C.port = port;
    check(C.fetch() && C.doc["screens"].size() == 2 && C.doc["settings"]["locked"].b, "server has both");
    // Damaged inputs: a corrupt cache is ignored; a body that doesn't match its hash is refused.
    house::writeAtomic(a + "/house.json", "{\"schema\":1,\"rev\":");
    house::Store A4(a); check(!A4.loadCache(), "corrupt cache ignored");
    check(!A4.adopt("{\"schema\":1,\"rev\":9}", house::sha256("something else")) && A4.lastError == "sha mismatch", "hash mismatch refused");
    // The server refuses a PUT whose body doesn't match its hash (sent by hand).
    std::string body = house::dump(C.doc);
    house::Response r = house::http("127.0.0.1", port, "PUT", "/house", body, {"If-Match: 6", "X-House-SHA256: " + house::sha256(body + "x")});
    check(r.status == 400, "server refuses a damaged PUT");
    printf("house client: %s (%ld GET, %ld PUT, %ld conflicts)\n", fails ? "FAILED" : "all checks passed", A.gets + B.gets, A.puts + B.puts + A3.puts, B.conflicts);
    return fails ? 1 : 0;
}
