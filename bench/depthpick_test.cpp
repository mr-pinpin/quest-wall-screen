// DEPTHPLACE: the solid fitted wall a placement ray hits (room_lab/depthpick.h). g++ -std=c++17 -I room_lab bench/depthpick_test.cpp && ./a.out
#include "depthpick.h"
#include <cstdio>
using namespace depthpick;
using wallfit::Kind;
static int fails = 0;
static void check(bool c, const char* m) { if (!c) { printf("FAIL %s\n", m); fails++; } }
static wallfit::Wall wall(V3 c, V3 n, V3 right, float w, float h, float conf, Kind k = Kind::wall) {
    wallfit::Wall x; x.centre = c; x.n = n; x.right = right; x.up = {0, 1, 0}; x.d = -wallfit::dot(n, c); x.w = w; x.h = h; x.confidence = conf; x.kind = k; return x;
}
int main() {
    // The TV room: a wall 3 m ahead (facing us, n = +z), a nearer cupboard front 1.5 m ahead but only 0.4 m wide,
    // a weak (unconfirmed) wall at 2 m, the floor, and a wall to the left.
    std::vector<wallfit::Wall> ws = {
        wall({0, 1.3f, -3}, {0, 0, 1}, {1, 0, 0}, 3.0f, 2.4f, 0.8f),                    // 0 TV wall
        wall({0.1f, 1.0f, -1.5f}, {0, 0, 1}, {1, 0, 0}, 0.4f, 1.0f, 0.9f),              // 1 narrow cupboard: < 0.6 m
        wall({0.3f, 1.3f, -2}, {0, 0, 1}, {1, 0, 0}, 2.0f, 2.0f, 0.2f),                 // 2 weak
        wall({0, 0, -1.5f}, {0, 1, 0}, {1, 0, 0}, 4.0f, 3.0f, 0.9f, Kind::floor),       // 3 floor
        wall({-2, 1.3f, -1.5f}, {1, 0, 0}, {0, 0, -1}, 3.0f, 2.4f, 0.7f)};              // 4 left wall
    V3 eye{0, 1.6f, 0};
    float t = 0;
    check(pick(ws, eye, wallfit::norm(V3{0, -0.1f, -1}), {}, 0.5f, 8, &t) == 0 && std::fabs(t - 3.0f) < 0.05f, "ray ahead: the TV wall, skipping narrow / weak / floor");
    check(pick(ws, eye, wallfit::norm(V3{-1, 0, -0.75f}), {}) == 4, "ray left: the left wall");
    check(pick(ws, eye, wallfit::norm(V3{0, 0, 1}), {}) == -1, "behind: nothing");
    check(pick(ws, eye, wallfit::norm(V3{1.2f, 0, -1}), {}) == -1, "past the TV wall's edge: nothing");
    check(pick(ws, eye, wallfit::norm(V3{0, -0.1f, -1}), {}, 0.5f, 2.5f) == -1, "beyond range: nothing");
    // Meta already has the TV wall (Space Setup): leave it to the scene wall.
    wallfit::HouseWall meta{ws[0].pose(), 3.0f, 2.4f};
    check(pick(ws, eye, wallfit::norm(V3{0, -0.1f, -1}), {meta}) == -1, "a wall Meta has is not duplicated");
    // A stronger weak wall: once confirmed (conf 0.6) it is nearer and wins.
    ws[2].confidence = 0.6f;
    check(pick(ws, eye, wallfit::norm(V3{0.1f, -0.1f, -1}), {}) == 2, "nearest solid wall wins");
    printf("depthpick: %s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
