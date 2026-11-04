// Pointer stabilization for tremor (flag STEADY, off by default; kanban 2026-11-04). Each hand's ray origin, ray
// direction and pinch point go through a One-Euro filter (a low-pass whose cutoff rises with the hand's speed:
// still hands are smoothed hard, deliberate moves pass with little lag) and then a soft dead zone (the output holds
// until the filtered point leaves a small sphere around it, then is dragged along its edge, so nothing jumps; inside
// the sphere it creeps to the filtered point, so it never parks off target). One strength 0..1 drives both:
// 0 is a pass-through, 0.7 cuts 4-8 Hz, 2-6 mm tremor by >= 70% with < 60 ms lag on deliberate moves.
// Clicks: on the press edge, Out.click* is the filtered pose from rewindS ago (before the thumb's own jerk moved
// the knuckles) and it stays put for the whole press, so the click lands where the cursor was, not where it jumped.
// The caller feeds the filtered o/d into its ray history, so its rewind-and-lock (room_lab.cpp kRewind) also sees
// filtered values. The direction is filtered as the point d * leverM, so the same mm settings apply to angles.
// GL-free; bench/steady_test.cpp.
#pragma once
#include <cmath>

namespace steady {

struct V { float x = 0, y = 0, z = 0; };
inline V operator+(V a, V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V operator-(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V operator*(V a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float len(V a) { return std::sqrt(dot(a, a)); }
inline V norm(V a) { float l = len(a); return l > 1e-9f ? a * (1 / l) : V{0, 0, 1}; }

// Parameters from one strength: s = 0 is off; s = 1 is the strongest (about 0.3 Hz still cutoff, 5 mm dead zone).
struct Params {
    float minCutoffHz = 1e9f, beta = 0, dCutoffHz = 1.0f, deadM = 0, creepS = 0.2f;
    static Params of(float s) {
        Params p; s = std::fmin(std::fmax(s, 0.0f), 1.0f);
        if (s <= 0) return p;
        p.minCutoffHz = 6.0f * std::pow(0.05f, s);  // 6 Hz at 0+, 0.73 Hz at 0.7, 0.3 Hz at 1
        p.beta = 40.0f;                             // + 1 Hz per 2.5 cm/s of (low-passed) hand speed
        p.deadM = 0.005f * s;                       // 3.5 mm at 0.7
        return p;
    }
};

// One-Euro filter on a 3D point (Casiez et al. 2012), speed from the low-passed derivative's magnitude.
struct Euro3 {
    bool init = false; V x, dx; float t = 0;
    static float alpha(float hz, float dt) { float tau = 1.0f / (6.2831853f * hz); return 1.0f / (1.0f + tau / dt); }
    V update(V in, float tNow, const Params& p) {
        if (!init || tNow - t > 0.25f || tNow <= t) { init = true; x = in; dx = {}; t = tNow; return x; }  // first sample or a gap: restart
        float dt = tNow - t; t = tNow;
        V d = (in - x) * (1 / dt);
        dx = dx + (d - dx) * alpha(p.dCutoffHz, dt);
        float hz = p.minCutoffHz + p.beta * len(dx);
        x = x + (in - x) * alpha(hz, dt);
        return x;
    }
};

// Soft dead zone with creep: holds inside deadM, drags on the edge outside it, creeps to the input in creepS.
struct Dead3 {
    bool init = false; V y; float t = 0;
    V update(V in, float tNow, const Params& p) {
        if (!init || tNow - t > 0.25f || tNow <= t) { init = true; y = in; t = tNow; return y; }
        float dt = tNow - t; t = tNow;
        V e = in - y; float L = len(e);
        if (L > p.deadM) y = y + e * ((L - p.deadM) / L);   // pulled along by the edge of the zone
        e = in - y;
        y = y + e * std::fmin(dt / p.creepS, 1.0f);        // and slowly onto the input: no residual offset
        return y;
    }
};

struct Input {
    float t = 0;          // seconds
    bool tracked = false; // hand ray valid this frame
    bool pinch = false;   // press state (pinch or finger gun) as the app decides it
    V o, d, tip;          // ray origin, unit direction, pinch point
};
struct Out {
    V o, d, tip;                 // filtered, for drawing the ray and the cursor
    bool press = false, edge = false;
    V clickO, clickD, clickTip;  // valid while press: the filtered pose rewindS before the press edge
};

struct Hand {
    float strength = 0;
    float leverM = 0.5f;   // shoulder -> knuckles: direction tremor in mm at the knuckles
    float rewindS = 0.1f;  // like room_lab.cpp's kRewind (7 frames at 72 Hz)
    static constexpr int kHist = 32;
    Euro3 eo, ed, et; Dead3 zo, zd, zt;
    struct Sample { float t; V o, d, tip; } hist[kHist]; int histN = 0;
    bool pressed = false; Out last;

    void reset() { eo = ed = et = Euro3{}; zo = zd = zt = Dead3{}; histN = 0; pressed = false; }

    Out update(const Input& in) {
        Out out;
        if (!in.tracked) { reset(); last = out; return out; }
        Params p = Params::of(strength);
        if (strength <= 0) {  // off: exactly the input, the press pose is the input at the edge
            out.o = in.o; out.d = in.d; out.tip = in.tip;
        } else {
            out.o = zo.update(eo.update(in.o, in.t, p), in.t, p);
            out.d = norm(zd.update(ed.update(in.d * leverM, in.t, p), in.t, p));
            out.tip = zt.update(et.update(in.tip, in.t, p), in.t, p);
        }
        hist[histN % kHist] = {in.t, out.o, out.d, out.tip}; histN++;
        out.press = in.pinch; out.edge = in.pinch && !pressed;
        if (out.edge) {
            const Sample* s = &hist[(histN - 1) % kHist];
            if (strength > 0)  // the newest sample at least rewindS old (or the oldest kept)
                for (int k = 1; k < histN && k < kHist; k++) {
                    const Sample& c = hist[(histN - 1 - k) % kHist];
                    s = &c; if (in.t - c.t >= rewindS) break;
                }
            out.clickO = s->o; out.clickD = s->d; out.clickTip = s->tip;
        } else if (in.pinch) { out.clickO = last.clickO; out.clickD = last.clickD; out.clickTip = last.clickTip; }
        pressed = in.pinch; last = out;
        return out;
    }
};

}  // namespace steady
