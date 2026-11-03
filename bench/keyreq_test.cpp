// Keyframe requests (room_lab/vt.h vt::KeyReq), pairing with the sender's noperiodic flag.
//   g++ -std=c++17 -I room_lab bench/keyreq_test.cpp -o /tmp/keyreq_test && /tmp/keyreq_test
#include "vt.h"
#include <cstdio>
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
int main() {
    vt::KeyReq k;
    // Healthy stream at 60 fps for 10 s: no requests.
    for (int i = 0; i < 600; i++) { double t = i / 60.0; k.packet(t); k.shown(t); k.tick(t); }
    CHECK(k.count == 0, "healthy stream asks for nothing (%ld)", k.count);
    // A decode error asks once; a burst of errors within 0.5 s still asks once.
    k.decodeError(10.0); k.decodeError(10.1); k.decodeError(10.3);
    CHECK(k.count == 1, "error burst -> 1 request (%ld)", k.count);
    k.formatChanged(10.7); CHECK(k.count == 2, "format change after 0.5 s -> request");
    // Stall: packets keep arriving, nothing shown from t=11: a request after 1.5 s, then every 1.5 s, not more often.
    long c0 = k.count;
    for (int i = 0; i < 300; i++) { double t = 11 + i / 60.0; k.packet(t); k.tick(t); }   // 5 s of stall
    CHECK(k.count - c0 >= 2 && k.count - c0 <= 4, "5 s stall -> 2-4 requests (%ld)", k.count - c0);
    // Sender gone (no packets): no requests (nothing to key).
    long c1 = k.count; for (int i = 0; i < 300; i++) k.tick(20 + i / 60.0);
    CHECK(k.count == c1, "no packets -> no requests");
    // Recovery: frames shown again -> requests stop.
    long c2 = k.count; for (int i = 0; i < 300; i++) { double t = 30 + i / 60.0; k.packet(t); k.shown(t); k.tick(t); }
    CHECK(k.count == c2, "recovered -> no more requests");
    printf("%d failures\n", fails); return fails ? 1 : 0;
}
