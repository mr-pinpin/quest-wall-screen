// Shoulder menu (flag SHOULDER, docs/house-onboarding.md section 1): a small button that rides near the shoulder and
// opens a cascading depth stack of panels. No always-armed gesture: the button is the only way in.
//  - The button sits head-relative in yaw only (it doesn't tilt with the head): `side` * 0.22 m out to the side,
//    0.30 m down, 0.30 m forward. A pinch-ray or a fingertip poke presses it.
//  - Pressing opens the root panel `reach` m out along the gaze at that moment, world-locked from then on.
//  - Choosing a submenu spawns the child `step` m further out. Every level is scaled by its distance, so all panels
//    look the same size, and each child is turned sideways, away from the button, until it clears its parent: the
//    parents stay in view, dimmed, as breadcrumbs. Only the top panel takes button presses; a ray on a lower panel
//    pops back to that level.
// Radial root (flag WRIST): the same stack can open from a point instead (the wrist button): the first level is a
// ring of round buttons around that point, at its real distance and scale 1; deeper levels cascade from there, each
// `step` m further and scaled by distance like the shoulder's.
// GL-free: room_lab draws the levels and dispatches item actions; bench/shoulder_test.cpp covers the geometry.
// Units: metres, world frame of the app (+y up). Panel-local frame: origin at the panel centre, +x right, +y up,
// +z toward the viewer, at scale 1 (UiGrid units); world = centre + R(yaw) * (local * scale).
#pragma once
#include "ui_grid.h"
#include <cmath>
#include <functional>
#include <string>
#include <vector>

namespace shoulder {

struct P3 { float x = 0, y = 0, z = 0; };
inline P3 add(P3 a, P3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline P3 sub(P3 a, P3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline P3 mul(P3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(P3 a, P3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float len(P3 a) { return std::sqrt(dot(a, a)); }
// Rotation about +y by yaw; yaw 0 looks down -z (OpenXR forward), positive yaw turns left.
inline P3 rotY(float yaw, P3 v) { float c = std::cos(yaw), s = std::sin(yaw); return {c * v.x + s * v.z, v.y, -s * v.x + c * v.z}; }
inline P3 forward(float yaw) { return rotY(yaw, {0, 0, -1}); }
// The yaw of a gaze direction, ignoring pitch (straight up/down: 0).
inline float yawOf(P3 d) { return (std::fabs(d.x) + std::fabs(d.z) < 1e-6f) ? 0.0f : std::atan2(-d.x, -d.z); }

// action: "open:<NAME>", "swap:<NAME>" (replace the top panel in place: paging), "back", "close", or anything room_lab
// handles. section: a UiGrid heading over the items that share it. dim: greyed (an off flag, its reason as the hint).
struct Item { std::string label, hint, action, section; bool dim = false; };
struct Level {
    std::string name, title; std::vector<Item> items;
    UiGrid grid; P3 centre; float yaw = 0, scale = 1, dist = 1;  // dist: from the eye, horizontally
    bool radial = false;                                          // a ring of round buttons (grid.cells hold them)
    bool anchored = false;                                        // opened beside another panel (openBeside)
    float ringR = 0, buttonR = 0;
    P3 toWorld(float x, float y, float z = 0) const { return add(centre, rotY(yaw, mul(P3{x, y, z}, scale))); }
    // Ray against the panel plane: true with panel-local (x, y) when it lands inside the panel.
    bool local(P3 o, P3 d, float& x, float& y) const {
        P3 lo = rotY(-yaw, sub(o, centre)), ld = rotY(-yaw, d);
        if (std::fabs(ld.z) < 1e-5f) return false;
        float t = -lo.z / ld.z; if (t <= 0) return false;
        x = (lo.x + ld.x * t) / scale; y = (lo.y + ld.y * t) / scale;
        return std::fabs(x) <= grid.halfW && std::fabs(y) <= grid.halfH;
    }
};

struct Hit { int level = -1, item = -1; bool button = false; };

struct Menu {
    float side = 1;               // +1 right shoulder, -1 left
    float reach = 1.0f, step = 1.0f;
    float buttonR = 0.045f;       // generous: it is the only way in
    std::vector<Level> stack;     // [0] root ... back() top
    std::function<Level(const std::string&)> build;  // the room_lab tree: name -> title and items
    P3 eye0; float yaw0 = 0;      // head when the stack opened

    bool open() const { return !stack.empty(); }
    P3 button(P3 head, P3 gaze) const { return add(head, rotY(yawOf(gaze), {side * 0.22f, -0.30f, -0.30f})); }
    // Ray-sphere and poke: either one hits the button.
    bool buttonHit(P3 head, P3 gaze, P3 o, P3 d) const {
        P3 c = button(head, gaze), oc = sub(c, o); float t = dot(oc, d);
        if (t > 0 && len(sub(oc, mul(d, t))) < buttonR) return true;
        return false;
    }
    bool buttonPoke(P3 head, P3 gaze, P3 tip) const { return len(sub(tip, button(head, gaze))) < buttonR; }

    // A radial level: n round buttons on a ring, the first at the top, clockwise; the centre is left for the hint.
    static void layoutRadial(Level& L) {
        int n = (int)L.items.size();
        L.buttonR = 0.024f; L.ringR = std::fmax(0.07f, n * (2 * L.buttonR + 0.008f) / 6.2832f);
        L.grid.cells.assign(n, {});
        for (int i = 0; i < n; i++) {
            float a = 6.2832f * i / n; UiGrid::Cell c{};
            c.x = L.ringR * std::sin(a); c.y = L.ringR * std::cos(a); c.w = c.h = 2 * L.buttonR;
            c.px = UiGrid::fit(L.items[i].label, 0.0022f, 1.7f * L.buttonR, 0.5f * L.buttonR); c.labelY = c.y;
            L.grid.cells[i] = c;
        }
        L.grid.halfW = L.grid.halfH = L.ringR + L.buttonR + 0.01f; L.grid.titleY = 0;
    }
    float angularHalf(const Level& L) const { return L.grid.halfW * L.scale / std::fmax(L.dist, 0.05f); }
    void place(Level& L, int depth) const {
        L.grid.layout(toGrid(L.items));
        float base = (depth > 0 && (stack[0].radial || stack[0].anchored)) ? stack[0].dist : reach;
        float d = depth == 0 ? reach : base + depth * step;
        L.scale = d / reach; L.dist = d;
        // Turn each child away from the shoulder button until it clears its parent (plus a 2 degree gap): the parent is
        // nearer, so any overlap would hide the child. Scaled by distance, a panel's angular half-width is halfW / reach.
        float yaw = depth == 0 ? yaw0 : stack[depth - 1].yaw + side * (angularHalf(stack[depth - 1]) + L.grid.halfW / reach + 0.035f);
        L.yaw = yaw;
        L.centre = add(eye0, mul(forward(yaw), d));
        L.centre.y = eye0.y - 0.12f * L.scale;
    }
    static std::vector<UiGrid::Item> toGrid(const std::vector<Item>& items) {
        std::vector<UiGrid::Item> g; for (auto& it : items) g.push_back({it.label, it.section, it.hint}); return g;
    }
    void toggle(P3 head, P3 gaze) {
        if (open()) { stack.clear(); return; }
        eye0 = head; yaw0 = yawOf(gaze);
        push("ROOT");
    }
    // Open a radial root around `at` (e.g. just above the wrist button), facing the eye.
    void openRadial(P3 eye, P3 at, const std::string& name = "WRIST") {
        if (!build) return;
        stack.clear(); eye0 = eye;
        P3 d = sub(at, eye); yaw0 = yawOf(d);
        Level L = build(name); L.name = name; L.radial = true; layoutRadial(L);
        L.centre = at; L.yaw = yaw0; L.scale = 1; L.dist = std::fmax(std::sqrt(d.x * d.x + d.z * d.z), 0.1f);
        stack.push_back(L);
    }
    // Open `name` as the root of a cascade beside a panel that is not part of the stack (the standalone SETTINGS):
    // `step` m further out than that panel, turned away by `side` until it clears it, at the panel's height.
    void openBeside(P3 eye, P3 panelCentre, float panelHalfW, const std::string& name) {
        if (!build) return;
        stack.clear(); eye0 = eye;
        P3 d = sub(panelCentre, eye); float pd = std::fmax(std::sqrt(d.x * d.x + d.z * d.z), 0.1f);
        Level L = build(name); L.name = name; L.anchored = true; L.grid.layout(toGrid(L.items));
        L.dist = pd + step; L.scale = L.dist / reach; yaw0 = yawOf(d);
        L.yaw = yaw0 + side * (panelHalfW / pd + L.grid.halfW / reach + 0.035f);
        L.centre = add(eye, mul(forward(L.yaw), L.dist)); L.centre.y = panelCentre.y;
        stack.push_back(L);
    }
    // Replace the top panel with `name`, keeping its place (a page of the same list).
    void swap(const std::string& name) {
        if (!build || stack.empty()) return;
        Level& T = stack.back(); Level L = build(name); L.name = name; L.radial = T.radial; L.anchored = T.anchored;
        if (L.radial) layoutRadial(L); else L.grid.layout(toGrid(L.items));
        L.centre = T.centre; L.yaw = T.yaw; L.scale = T.scale; L.dist = T.dist; T = L;
    }
    void push(const std::string& name) {
        if (!build) return;
        Level L = build(name); L.name = name;
        place(L, (int)stack.size());
        stack.push_back(L);
    }
    void pop() { if (!stack.empty()) stack.pop_back(); }
    // Re-run the builder for every open level (labels that show state, a screen list that changed), keeping poses.
    void refresh() {
        for (size_t k = 0; k < stack.size(); k++) {
            Level L = build(stack[k].name); L.name = stack[k].name; L.radial = stack[k].radial; L.anchored = stack[k].anchored;
            if (L.radial) layoutRadial(L); else L.grid.layout(toGrid(L.items));
            L.centre = stack[k].centre; L.yaw = stack[k].yaw; L.scale = stack[k].scale; L.dist = stack[k].dist;
            stack[k] = L;
        }
    }
    // What the ray points at: the top panel's item, or a lower panel (item -1) when the top doesn't cover that spot.
    Hit hit(P3 o, P3 d) const {
        Hit h;
        for (int k = (int)stack.size() - 1; k >= 0; k--) {
            float x, y;
            if (!stack[k].local(o, d, x, y)) continue;
            h.level = k; h.item = -1;
            if (k == (int)stack.size() - 1) {
                if (!stack[k].radial) h.item = stack[k].grid.hit(x, y);
                else for (size_t i = 0; i < stack[k].grid.cells.size(); i++) {
                    const UiGrid::Cell& c = stack[k].grid.cells[i];
                    if ((x - c.x) * (x - c.x) + (y - c.y) * (y - c.y) < stack[k].buttonR * stack[k].buttonR * 1.21f) { h.item = (int)i; break; }
                }
            }
            return h;
        }
        return h;
    }
    // A pinch on a hit: open/back/close are handled here; any other action is returned for room_lab to run.
    std::string press(const Hit& h) {
        if (h.level < 0) return "";
        if (h.level < (int)stack.size() - 1) { stack.resize(h.level + 1); return ""; }
        if (h.item < 0) return "";
        std::string a = stack[h.level].items[h.item].action;
        if (a.rfind("open:", 0) == 0) { push(a.substr(5)); return ""; }
        if (a.rfind("swap:", 0) == 0) { swap(a.substr(5)); return ""; }
        if (a == "back") { pop(); return ""; }
        if (a == "close") { stack.clear(); return ""; }
        return a;
    }
};

}  // namespace shoulder
