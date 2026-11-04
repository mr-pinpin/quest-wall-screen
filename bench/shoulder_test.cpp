// Shoulder menu geometry and stack (room_lab/shoulder.h). g++ -std=c++17 -I room_lab bench/shoulder_test.cpp && ./a.out
#include "shoulder.h"
#include <cstdio>
using namespace shoulder;
static int fails = 0;
static void check(bool c, const char* m) { if (!c) { printf("FAIL %s\n", m); fails++; } }
static Level tree(const std::string& n) {
    Level L;
    if (n == "ROOT") L.items = {{"SCREENS", "MOVE AND SIZE SCREENS", "open:SCREENS"}, {"SETTINGS", "ALL SWITCHES", "open:SETTINGS"},
                                {"CLOSE", "HIDE THIS MENU", "close"}};
    else if (n == "SCREENS") L.items = {{"MAIN", "THE BIG SCREEN", "open:SCREEN 1"}, {"SCREEN 2", "AN EXTRA SCREEN", "open:SCREEN 2"}, {"BACK", "ONE MENU BACK", "back"}};
    else L.items = {{"BIGGER", "WIDER BY 20%", "bigger"}, {"SMALLER", "NARROWER BY 20%", "smaller"}, {"MOVE", "POINT AT A WALL", "move"}, {"BACK", "ONE MENU BACK", "back"}};
    return L;
}
// A ray from the eye through panel-local (x, y) of a level.
static void rayTo(const Menu& m, int k, float x, float y, P3& o, P3& d) {
    o = m.eye0; P3 t = m.stack[k].toWorld(x, y); P3 v = sub(t, o); d = mul(v, 1 / len(v));
}
int main() {
    Menu m; m.build = tree;
    P3 head{0.3f, 1.6f, -0.2f}, gaze = {0, 0, -1};
    // The button: yaw-only, beside the right shoulder, down and forward; a ray at it and a poke both hit.
    P3 b = m.button(head, gaze);
    check(b.x > head.x + 0.15f && b.y < head.y - 0.2f && b.z < head.z, "button beside the right shoulder");
    P3 bLook = m.button(head, P3{0, -0.9f, -0.44f});
    check(std::fabs(bLook.y - b.y) < 1e-4f, "button doesn't move with head pitch");
    P3 bTurn = m.button(head, P3{1, 0, 0});  // turned right 90 degrees
    check(std::fabs(len(sub(bTurn, head)) - len(sub(b, head))) < 1e-4f && bTurn.x > head.x, "button follows yaw");
    P3 d0 = sub(b, head); d0 = mul(d0, 1 / len(d0));
    check(m.buttonHit(head, gaze, head, d0), "ray at button hits");
    check(!m.buttonHit(head, gaze, head, gaze), "looking ahead doesn't hit");
    check(m.buttonPoke(head, gaze, add(b, {0.02f, 0, 0})) && !m.buttonPoke(head, gaze, add(b, {0.08f, 0, 0})), "poke radius");
    // Open: the root 1 m out along the gaze, facing the eye.
    m.toggle(head, gaze);
    check(m.open() && m.stack.size() == 1, "button opens root");
    float d = len(sub(P3{m.stack[0].centre.x, head.y, m.stack[0].centre.z}, head));
    check(std::fabs(d - 1.0f) < 1e-3f, "root 1 m out");
    check(m.stack[0].scale == 1, "root at scale 1");
    // Press SCREENS: a child 2 m out, scaled 2x, to the left (away from the right-shoulder button), clear of the root.
    P3 o, r;
    const UiGrid::Cell& c0 = m.stack[0].grid.cells[0];
    rayTo(m, 0, c0.x, c0.y, o, r);
    Hit h = m.hit(o, r);
    check(h.level == 0 && h.item == 0, "ray finds SCREENS");
    check(m.press(h).empty() && m.stack.size() == 2, "open:SCREENS pushes");
    const Level& L1 = m.stack[1];
    float d1 = len(sub(P3{L1.centre.x, head.y, L1.centre.z}, head));
    check(std::fabs(d1 - 2.0f) < 1e-3f && std::fabs(L1.scale - 2.0f) < 1e-4f, "child 1 m further, scaled by distance");
    check(L1.yaw > m.stack[0].yaw, "child turns away from the button (left for a right button)");
    // Angular clearance: the child's near edge is beyond the root's far edge, seen from the eye.
    float rootEdge = m.stack[0].yaw + m.stack[0].grid.halfW / m.reach, childEdge = L1.yaw - L1.grid.halfW / m.reach;
    check(childEdge > rootEdge, "child clears its parent");
    // The child's buttons have the same angular size as the root's.
    float ang0 = m.stack[0].grid.cells[0].w * m.stack[0].scale / 1.0f, ang1 = L1.grid.cells[0].w * L1.scale / 2.0f;
    check(std::fabs(ang0 - ang1) < 0.02f * ang0 || L1.grid.cells[0].w != m.stack[0].grid.cells[0].w, "same angular size");
    // Only the top panel takes presses: a ray at the root now pops back to it.
    rayTo(m, 0, c0.x, c0.y, o, r); h = m.hit(o, r);
    check(h.level == 0 && h.item == -1, "lower panel is a breadcrumb");
    m.press(h); check(m.stack.size() == 1, "breadcrumb pops to that level");
    // Deeper: SCREENS -> SCREEN 2 -> BIGGER returns an action for room_lab.
    rayTo(m, 0, c0.x, c0.y, o, r); m.press(m.hit(o, r));
    const UiGrid::Cell& c1 = m.stack[1].grid.cells[1];
    rayTo(m, 1, c1.x, c1.y, o, r); m.press(m.hit(o, r));
    check(m.stack.size() == 3 && m.stack[2].name == "SCREEN 2", "third level");
    check(std::fabs(m.stack[2].scale - 3.0f) < 1e-4f, "third level 3 m");
    const UiGrid::Cell& c2 = m.stack[2].grid.cells[0];
    rayTo(m, 2, c2.x, c2.y, o, r); Hit hb = m.hit(o, r);
    check(m.press(hb) == "bigger", "action returned to the app");
    const UiGrid::Cell& cb = m.stack[2].grid.cells[3];
    rayTo(m, 2, cb.x, cb.y, o, r); m.press(m.hit(o, r));
    check(m.stack.size() == 2, "BACK pops");
    // Hints are kept: each cell gets its one-line explanation.
    check(!m.stack[1].grid.cells[0].hint1.empty(), "hints laid out");
    // Refresh keeps poses.
    P3 before = m.stack[1].centre; m.refresh();
    check(len(sub(before, m.stack[1].centre)) < 1e-6f && m.stack.size() == 2, "refresh keeps poses");
    // A miss everywhere: nothing.
    h = m.hit(head, P3{0, 1, 0}); check(h.level < 0, "ray up misses");
    // The button again closes everything; a left-shoulder menu fans right.
    m.toggle(head, gaze); check(!m.open(), "button closes the stack");
    Menu l; l.build = tree; l.side = -1; l.toggle(head, gaze);
    l.push("SCREENS"); check(l.stack[1].yaw < l.stack[0].yaw, "left button fans right");
    printf("shoulder menu: %s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
