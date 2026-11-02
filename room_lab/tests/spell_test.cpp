#include "../spell.h"
#include <cstdio>
#include <random>
#include <functional>
using R = SpellRecognizer;
// Feed a stroke at 72 Hz; return the first detection (found, clockwise) or not found.
// View plane: right = +x, up = +y, user looks down -z; points at z = -0.5 (arm's length).
struct Out { bool found, cw; float r, at; };
Out run(std::function<R::P3(float)> f, float dur, float noise, unsigned seed = 1) {
    R s; std::mt19937 g(seed); std::normal_distribution<float> N(0, noise);
    for (int i = 0; i <= (int)(dur * 72); i++) {
        float t = i / 72.0f; R::P3 p = f(t); p.x += N(g); p.y += N(g); p.z += N(g);
        s.add(p, t);
        auto r = s.analyze({1, 0, 0}, {0, 1, 0});
        if (r.found) return {true, r.clockwise, r.radius, t};
    }
    return {false, false, 0, dur};
}
int fails = 0;
void expect(const char* name, Out o, bool found, bool cw = false) {
    bool ok = o.found == found && (!found || o.cw == cw);
    if (!ok) fails++;
    printf("%-46s %s  found=%d cw=%d r=%.3f t=%.2f\n", name, ok ? "ok  " : "FAIL", o.found, o.cw, o.r, o.at);
}
int main() {
    const float PI = 3.14159265f;
    auto circle = [&](float r, float T, int dir, float turns = 1.15f, float wob = 0) {
        return [=](float t) { float a = dir * 2 * PI * turns * t / T + PI / 2; float rr = r * (1 + wob * std::sin(5 * a));
                              return R::P3{rr * std::cos(a), 0.1f + rr * std::sin(a), -0.5f}; };
    };
    // Clockwise in the user's view = decreasing angle with x right, y up.
    expect("clockwise 15 cm, 1.2 s", run(circle(0.15f, 1.2f, -1), 1.4f, 0.003f), true, true);
    expect("counterclockwise 15 cm, 1.2 s", run(circle(0.15f, 1.2f, +1), 1.4f, 0.003f), true, false);
    expect("small 7 cm circle (too small for the spell)", run(circle(0.07f, 0.8f, -1), 1.0f, 0.002f), false);
    expect("clockwise 10 cm, 1.0 s", run(circle(0.10f, 1.0f, -1), 1.2f, 0.003f), true, true);
    expect("clockwise big 30 cm, 2.0 s", run(circle(0.30f, 2.0f, -1, 1.1f), 2.2f, 0.005f), true, true);
    expect("clockwise wobbly hand-drawn (18%)", run(circle(0.15f, 1.4f, -1, 1.15f, 0.18f), 1.6f, 0.004f), true, true);
    expect("clockwise shaky hand (8 mm noise)", run(circle(0.15f, 1.3f, -1), 1.5f, 0.008f), true, true);
    expect("half circle only", run(circle(0.15f, 1.2f, -1, 0.5f), 1.2f, 0.003f), false);
    expect("three quarters only", run(circle(0.15f, 1.2f, -1, 0.75f), 1.2f, 0.003f), false);
    expect("too fast (0.25 s)", run(circle(0.15f, 0.25f, -1), 0.3f, 0.002f), false);
    expect("too slow (4 s)", run(circle(0.15f, 4.0f, -1), 4.2f, 0.002f), false);
    expect("tiny 2 cm jitter circle", run(circle(0.02f, 1.0f, -1), 1.2f, 0.001f), false);
    expect("straight swipe", run([](float t) { return R::P3{-0.3f + 0.5f * t, 0.1f, -0.5f}; }, 1.2f, 0.003f), false);
    expect("pointing still (cursor use)", run([](float t) { return R::P3{0.0f, 0.1f, -0.5f}; }, 2.0f, 0.004f), false);
    expect("zigzag scribble", run([](float t) { return R::P3{0.2f * std::sin(9 * t), 0.1f + 0.05f * std::sin(3 * t), -0.5f}; }, 2.0f, 0.003f), false);
    // A figure eight's lobe is a real round loop; accepted as a circle (an unusual motion in practice).
    expect("figure eight 30 cm (lobes too small)", run([&](float t) { float a = 2 * PI * t / 1.4f; return R::P3{0.15f * std::sin(a), 0.1f + 0.1f * std::sin(2 * a), -0.5f}; }, 1.6f, 0.003f), false);
    expect("clockwise ellipse 0.75 tall", run([&](float t) { auto p = circle(0.15f, 1.3f, -1)(t); p.y = 0.1f + (p.y - 0.1f) * 0.75f; return p; }, 1.5f, 0.003f), true, true);
    expect("flat ellipse 0.4 (not a circle)", run([&](float t) { auto p = circle(0.15f, 1.3f, -1)(t); p.y = 0.1f + (p.y - 0.1f) * 0.4f; return p; }, 1.5f, 0.003f), false);
    expect("circle drawn while moving hand sideways", run([&](float t) { auto p = circle(0.15f, 1.3f, -1)(t); p.x += 0.05f * t; return p; }, 1.5f, 0.003f), true, true);
    // Robustness over seeds for the main case
    int ok = 0; for (unsigned s = 1; s <= 50; s++) { auto o = run(circle(0.10f + 0.003f * s, 0.9f + 0.02f * s, -1), 0.9f + 0.02f * s + 0.3f, 0.006f, s); ok += o.found && o.cw; }
    printf("clockwise across 50 sizes/speeds/noise: %d/50\n", ok); if (ok < 48) fails++;
    int fp = 0; for (unsigned s = 1; s <= 50; s++) { std::mt19937 g(s); std::uniform_real_distribution<float> U(-1, 1);
        float a = U(g), b = U(g), c = U(g); auto o = run([=](float t) { return R::P3{0.15f * std::sin(3 * t + a) + 0.05f * b * t, 0.1f + 0.08f * std::sin(5 * t + c), -0.5f}; }, 2.5f, 0.004f, s); fp += o.found; }
    printf("false positives over 50 random wiggles: %d/50\n", fp); if (fp > 1) fails++;
    printf("%s (%d failures)\n", fails ? "FAILED" : "ALL PASS", fails);
    return fails;
}
