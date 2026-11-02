// The summoning spell: a circle drawn in the air with the index fingertip.
// Clockwise (as the user sees it) summons, counterclockwise cancels or dismisses.
// Fingertip samples are kept in 3D and projected onto the view plane at analysis time, so a turn of the head
// while drawing doesn't bend the circle. A window of recent samples counts as a circle when it sweeps at least
// kMinSweep around its own centroid, keeps a fairly even radius, is big (18 cm across or more, up to arm-sized,
// as the user asked for a big deliberate invocation), takes 0.4-2.2 s,
// ends near where it began, and is round (its narrowest spread at least kMinRound of its widest, which rules out
// the lobes and teardrops a scribble makes).
#pragma once
#include <cmath>
#include <deque>
#include <vector>

struct SpellRecognizer {
    struct P3 { float x, y, z; };
    struct Sample { P3 p; float t; };
    struct Result {
        bool found = false, clockwise = false;
        P3 centre{0, 0, 0};     // 3D centroid of the circle
        float radius = 0;       // metres, in the view plane
        float progress = 0;     // 0..1 of a full turn in the current stroke, for the trail's glow
        int turn = 0;           // +1 counterclockwise, -1 clockwise, 0 unknown (direction of the stroke so far)
    };
    static constexpr float kMinSweep = 5.6f;  // ~320 degrees
    static constexpr float kMinR = 0.09f, kMaxR = 0.35f, kMaxSpread = 0.20f, kMaxGap = 0.85f, kMinRound = 0.72f;
    static constexpr float kMinT = 0.4f, kMaxT = 2.2f;
    std::deque<Sample> pts;

    void add(P3 p, float t) {
        pts.push_back({p, t});
        while (!pts.empty() && t - pts.front().t > kMaxT) pts.pop_front();
    }
    void clear() { pts.clear(); }

    static float dot(P3 a, P3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    // right/up: unit view-plane axes (from the head pose).
    Result analyze(P3 right, P3 up) const {
        Result r;
        int n = (int)pts.size();
        if (n < 12) return r;
        std::vector<float> X(n), Y(n);
        for (int i = 0; i < n; i++) { X[i] = dot(pts[i].p, right); Y[i] = dot(pts[i].p, up); }
        // Progress of the whole stroke, for feedback.
        {
            float sweep = sweepOf(X, Y, 0, n);
            r.progress = std::fmin(std::fabs(sweep) / 6.2832f, 1.0f);
            r.turn = sweep > 0.3f ? 1 : sweep < -0.3f ? -1 : 0;
        }
        // Shortest qualifying window ending now.
        float tEnd = pts[n - 1].t;
        for (int i = n - 12; i >= 0; i--) {
            float dur = tEnd - pts[i].t;
            if (dur < kMinT) continue;
            if (dur > kMaxT) break;
            float cx = 0, cy = 0; for (int k = i; k < n; k++) { cx += X[k]; cy += Y[k]; }
            int m = n - i; cx /= m; cy /= m;
            float mr = 0, mr2 = 0;
            for (int k = i; k < n; k++) { float d = std::hypot(X[k] - cx, Y[k] - cy); mr += d; mr2 += d * d; }
            mr /= m; float sd = std::sqrt(std::fmax(mr2 / m - mr * mr, 0.0f));
            if (mr < kMinR || mr > kMaxR || sd / mr > kMaxSpread) continue;
            float sxx = 0, syy = 0, sxy = 0;
            for (int k = i; k < n; k++) { float dx = X[k] - cx, dy = Y[k] - cy; sxx += dx * dx; syy += dy * dy; sxy += dx * dy; }
            float tr = (sxx + syy) / 2, det = std::sqrt(std::fmax((sxx - syy) * (sxx - syy) / 4 + sxy * sxy, 0.0f));
            if (std::sqrt(std::fmax(tr - det, 0.0f) / std::fmax(tr + det, 1e-9f)) < kMinRound) continue;
            if (std::hypot(X[n - 1] - X[i], Y[n - 1] - Y[i]) / mr > kMaxGap) continue;
            float sweep = 0, px = X[i] - cx, py = Y[i] - cy;
            for (int k = i + 1; k < n; k++) {
                float qx = X[k] - cx, qy = Y[k] - cy;
                sweep += std::atan2(px * qy - py * qx, px * qx + py * qy); px = qx; py = qy;
            }
            if (std::fabs(sweep) < kMinSweep) continue;
            r.found = true; r.clockwise = sweep < 0; r.radius = mr;
            P3 c{0, 0, 0}; for (int k = i; k < n; k++) { c.x += pts[k].p.x; c.y += pts[k].p.y; c.z += pts[k].p.z; }
            r.centre = {c.x / m, c.y / m, c.z / m};
            return r;
        }
        return r;
    }
    static float sweepOf(const std::vector<float>& X, const std::vector<float>& Y, int a, int b) {
        float cx = 0, cy = 0; for (int k = a; k < b; k++) { cx += X[k]; cy += Y[k]; }
        cx /= (b - a); cy /= (b - a);
        float s = 0, px = X[a] - cx, py = Y[a] - cy;
        for (int k = a + 1; k < b; k++) { float qx = X[k] - cx, qy = Y[k] - cy; s += std::atan2(px * qy - py * qx, px * qx + py * qy); px = qx; py = qy; }
        return s;
    }
};
