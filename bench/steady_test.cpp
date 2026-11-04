// Pointer stabilization (room_lab/steady.h, flag STEADY). g++ -std=c++17 -I room_lab bench/steady_test.cpp && ./a.out
// Synthetic 72 Hz hand: tremor 4-8 Hz at 2-6 mm must drop >= 70% at strength 0.7 (ray origin, direction, pinch point),
// deliberate moves lag < 60 ms, a still or stopped hand never drifts, and a click lands where the cursor was.
#include "steady.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace steady;
static int fails = 0;
static void check(bool c, const char* m) { if (!c) { printf("FAIL %s\n", m); fails++; } }
static const float kDt = 1.0f / 72;

// Tremor: a sinusoid at f Hz, amplitude A (m) along a fixed oblique axis, plus a quarter-amplitude orthogonal part.
static V tremor(float t, float f, float A) {
    float w = 6.2831853f * f;
    return V{0.6f, 0.48f, 0.64f} * (A * std::sin(w * t)) + V{0.8f, -0.6f, 0} * (0.25f * A * std::cos(w * t + 0.7f));
}
static float rms(const std::vector<float>& e) { double s = 0; for (float x : e) s += x * x; return e.empty() ? 0 : (float)std::sqrt(s / e.size()); }

int main() {
    const V O{0.1f, 1.2f, -0.3f}, D = norm(V{0.1f, 0.05f, -1}), TIP{0.12f, 1.15f, -0.35f};
    // 1. tremor at rest, every frequency and amplitude in the band: RMS error vs the true pose
    float worst = 1;
    for (float f : {4.0f, 5.0f, 6.0f, 8.0f}) for (float A : {0.002f, 0.004f, 0.006f}) {
        Hand h; h.strength = 0.7f;
        std::vector<float> rawO, outO, rawD, outD, rawT, outT;
        for (int i = 0; i < 72 * 6; i++) {
            float t = i * kDt; V tr = tremor(t, f, A);
            Input in; in.t = t; in.tracked = true;
            in.o = O + tr; in.tip = TIP + tr * 1.1f;
            in.d = norm(D * h.leverM + tr);  // knuckle tremor with the shoulder still: the direction shakes too
            Out o = h.update(in);
            if (t < 1) continue;
            rawO.push_back(len(in.o - O)); outO.push_back(len(o.o - O));
            rawD.push_back(len(in.d - D) * h.leverM); outD.push_back(len(o.d - D) * h.leverM);
            rawT.push_back(len(in.tip - TIP)); outT.push_back(len(o.tip - TIP));
        }
        float rO = 1 - rms(outO) / rms(rawO), rD = 1 - rms(outD) / rms(rawD), rT = 1 - rms(outT) / rms(rawT);
        float r = std::fmin(rO, std::fmin(rD, rT)); worst = std::fmin(worst, r);
        if (r < 0.7f) printf("  %.0f Hz %.0f mm: origin -%.0f%% direction -%.0f%% tip -%.0f%%\n", f, A * 1000, rO * 100, rD * 100, rT * 100);
    }
    printf("  tremor 4-8 Hz, 2-6 mm at strength 0.7: worst reduction %.0f%%\n", worst * 100);
    check(worst >= 0.7f, "tremor reduced >= 70% at strength 0.7");

    // 2. deliberate moves: constant-speed ramps (steady-state lag) and a 20 cm minimum-jerk reach, with 3 mm tremor
    float worstLag = 0;
    for (float v : {0.15f, 0.3f, 0.6f, 1.0f}) {
        Hand h; h.strength = 0.7f; V dir = norm(V{1, 0.3f, 0}); float lag = 0;
        for (int i = 0; i < 72; i++) {
            float t = i * kDt; Input in; in.t = t; in.tracked = true; in.o = O + dir * (v * t); in.d = D; in.tip = in.o;
            Out o = h.update(in);
            if (t > 0.5f) lag = std::fmax(lag, dot(in.o - o.o, dir) / v);
        }
        if (lag > 0.06f) printf("  ramp %.2f m/s: lag %.0f ms\n", v, lag * 1000);
        worstLag = std::fmax(worstLag, lag);
    }
    printf("  ramps 0.15-1 m/s: worst lag %.0f ms\n", worstLag * 1000);
    check(worstLag < 0.06f, "ramp lag < 60 ms");
    for (float A : {0.0f, 0.003f}) {
        Hand h; h.strength = 0.7f; V dir = norm(V{1, -0.2f, 0.1f}); const float L = 0.2f, T = 0.5f, t0 = 0.5f;
        float inAt = -1, outAt = -1;
        for (int i = 0; i < 72 * 2; i++) {
            float t = i * kDt, s = std::fmin(std::fmax((t - t0) / T, 0.0f), 1.0f), mj = s * s * s * (10 - 15 * s + 6 * s * s);
            Input in; in.t = t; in.tracked = true; V clean = O + dir * (L * mj); in.o = clean + tremor(t, 6, A); in.d = D; in.tip = in.o;
            Out o = h.update(in);
            if (inAt < 0 && len(clean - (O + dir * L)) < 0.01f) inAt = t;
            if (outAt < 0 && len(o.o - (O + dir * L)) < 0.01f + A) outAt = t;
        }
        printf("  20 cm reach in 0.5 s (tremor %.0f mm): output arrives %.0f ms after the hand\n", A * 1000, (outAt - inAt) * 1000);
        check(outAt > 0 && outAt - inAt < 0.06f, "reach lag < 60 ms");
    }

    // 3. no drift: 20 s of tremor at rest, the mean output stays on the true pose; a move then a stop settles on target
    {
        Hand h; h.strength = 0.7f; V sum{}; int n = 0; float far = 0;
        for (int i = 0; i < 72 * 20; i++) {
            float t = i * kDt; Input in; in.t = t; in.tracked = true; in.o = O + tremor(t, 5, 0.004f); in.d = D; in.tip = in.o;
            Out o = h.update(in); if (t > 1) { sum = sum + (o.o - O); n++; } if (t > 19) far = std::fmax(far, len(o.o - O));
        }
        float mean = len(sum * (1.0f / n));
        printf("  20 s at rest with 4 mm tremor: mean offset %.2f mm, last second max %.2f mm\n", mean * 1000, far * 1000);
        check(mean < 0.0005f, "no drift at rest (mean within 0.5 mm)");
        check(far < 0.002f, "no wander after 20 s (within 2 mm)");
    }
    {
        Hand h; h.strength = 0.7f; V tgt = O + V{0.15f, 0, 0}; float settle = -1;
        for (int i = 0; i < 72 * 3; i++) {
            float t = i * kDt; Input in; in.t = t; in.tracked = true; in.o = t < 0.5f ? O + V{0.3f * t, 0, 0} : tgt; in.d = D; in.tip = in.o;
            Out o = h.update(in); if (t >= 0.5f && settle < 0 && len(o.o - tgt) < 0.0005f) settle = t - 0.5f;
            if (t > 2.5f) check(len(o.o - tgt) < 1e-4f, "stopped hand: output exactly on target");
        }
        printf("  stop after a move: within 0.5 mm of the target in %.0f ms\n", settle * 1000);
        check(settle >= 0 && settle < 0.5f, "stopped hand settles within 0.5 s (dead zone does not park off target)");
    }

    // 4. clicks: aim at a target with tremor, then the press itself jerks the knuckles 1 cm down over 60 ms, detected
    // at the end of that jerk. The click pose is the cursor the user saw before the jerk, not where the hand went.
    {
        Hand h; h.strength = 0.7f; const float tPress = 2.0f, jerkS = 0.06f; Out before, atEdge; bool held = true; V firstClick; int pressFrames = 0;
        for (int i = 0; i < 72 * 3; i++) {
            float t = i * kDt, j = std::fmin(std::fmax((t - (tPress - jerkS)) / jerkS, 0.0f), 1.0f);
            Input in; in.t = t; in.tracked = true; in.o = O + tremor(t, 6, 0.004f) + V{0, -0.01f * j, 0}; in.d = norm(D * 0.5f + V{0, -0.01f * j, 0});
            in.tip = TIP + tremor(t, 6, 0.004f) + V{0, -0.01f * j, 0}; in.pinch = t >= tPress && t < 2.5f;
            Out o = h.update(in);
            if (t < tPress - jerkS && t >= tPress - jerkS - kDt) before = o;
            if (o.edge) { atEdge = o; firstClick = o.clickO; }
            if (o.press && !o.edge) { pressFrames++; if (len(o.clickO - firstClick) > 0) held = false; }
        }
        float errClick = len(atEdge.clickO - O), errRaw = 0.01f, jump = len(atEdge.clickO - before.o), errTip = len(atEdge.clickTip - TIP);
        printf("  click: %.1f mm from the target (raw at the edge: ~10 mm), %.1f mm from the cursor shown before the press\n", errClick * 1000, jump * 1000);
        check(errClick < 0.003f && errClick < errRaw / 3, "click pose within 3 mm of the target");
        check(jump < 0.003f, "click lands where the cursor was (< 3 mm)");
        check(errTip < 0.003f, "click pinch point within 3 mm");
        check(len(atEdge.clickD - D) * 0.5f < 0.003f, "click direction within 3 mm at the lever");
        check(held && pressFrames > 30, "click pose fixed for the whole press");
    }

    // 5. strength 0 is a pass-through (the press pose is the input at the edge); tracking loss restarts cleanly
    {
        Hand h; h.strength = 0; bool same = true; Out e;
        for (int i = 0; i < 200; i++) {
            float t = i * kDt; Input in; in.t = t; in.tracked = true; in.o = O + tremor(t, 6, 0.005f); in.d = norm(D + tremor(t, 5, 0.01f)); in.tip = in.o; in.pinch = i == 100;
            Out o = h.update(in); if (len(o.o - in.o) > 0 || len(o.d - in.d) > 0 || len(o.tip - in.tip) > 0) same = false; if (o.edge) e = o;
            if (i == 100 && len(e.clickO - in.o) > 0) same = false;
        }
        check(same, "strength 0: exact pass-through");
        Hand g; g.strength = 0.7f;
        for (int i = 0; i < 100; i++) { Input in; in.t = i * kDt; in.tracked = true; in.o = O; in.d = D; in.tip = O; g.update(in); }
        Input lost; lost.t = 100 * kDt; g.update(lost);
        Input back; back.t = 101 * kDt; back.tracked = true; back.o = O + V{0.3f, 0, 0}; back.d = D; back.tip = back.o;
        check(len(g.update(back).o - back.o) < 1e-6f, "after a tracking loss the ray restarts at the hand, no glide from the old spot");
    }
    printf("steady pointer: %s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
