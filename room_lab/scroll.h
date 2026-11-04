// Global scroll gesture (flag SCROLL, docs/prd/assistant.md): a flat open hand, palm away from the face, moved up or
// down scrolls the screen being pointed at; left/right scrolls sideways. The page follows the hand, like dragging
// paper: Out.dy > 0 means the hand went up (content should move up), Out.dx > 0 right; the caller maps that to each
// source's wheel sign. Pointing and pinching never scroll (the hand must be open), and nothing is sent until the
// hand has travelled 2 cm, after which the dominant axis locks for the rest of the wave.
// GL-free; bench/scroll_test.cpp.
#pragma once
#include <cmath>

namespace scrollg {

struct V { float x = 0, y = 0, z = 0; };
inline V sub(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline float dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float len(V a) { return std::sqrt(dot(a, a)); }

struct Input {
    float t = 0;          // seconds
    bool tracked = false; // hand tracked this frame
    bool open = false;    // four fingers extended (not pointing, not a fist)
    bool pinch = false;
    V palm, palmNormal;   // palm centre; normal out of the palm (the side the palm faces)
    V head, right, up;    // head position and its right/up axes
};
struct Out { int dx = 0, dy = 0; bool engaged = false, active = false; };  // wheel units (120 = one notch)

struct Wave {
    float engageS = 0.12f, startM = 0.02f, unitsPerM = 4000.0f;  // 3 cm of hand = one notch
    float facing = 0.4f;                                           // palm normal . (palm - head) direction
    bool engaged = false, active = false; int axis = -1;           // 0 horizontal, 1 vertical
    float since = -1; V start, last; float accX = 0, accY = 0;

    Out update(const Input& in) {
        Out o;
        V toPalm = sub(in.palm, in.head); float L = len(toPalm);
        bool ok = in.tracked && in.open && !in.pinch && L > 0.15f && dot(in.palmNormal, toPalm) / L > facing;
        if (!ok) { reset(); return o; }
        if (!engaged) {
            if (since < 0) since = in.t;
            if (in.t - since < engageS) return o;
            engaged = true; start = last = in.palm; accX = accY = 0; axis = -1;
        }
        o.engaged = true;
        V d = sub(in.palm, last); last = in.palm;
        float mx = dot(d, in.right), my = dot(d, in.up);
        if (!active) {
            V tot = sub(in.palm, start); float tx = dot(tot, in.right), ty = dot(tot, in.up);
            if (std::hypot(tx, ty) < startM) return o;
            active = true; axis = std::fabs(ty) >= std::fabs(tx) ? 1 : 0;
            mx = tx; my = ty;  // the first 2 cm count too
        }
        o.active = true;
        if (axis == 1) accY += my * unitsPerM; else accX += mx * unitsPerM;
        o.dy = (int)accY; accY -= o.dy; o.dx = (int)accX; accX -= o.dx;
        return o;
    }
    void reset() { engaged = active = false; since = -1; axis = -1; accX = accY = 0; }
};

}  // namespace scrollg
