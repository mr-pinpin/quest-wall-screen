// wallfit.h on synthetic environment depth: a ray-cast room (4 walls, floor, ceiling) with a sofa in front of one
// wall, rendered as room_lab receives depth (uint16 d*65535, GL row order, far = infinity), with depth noise.
//   g++ -std=c++17 -O2 -I room_lab bench/wallfit_test.cpp -o /tmp/wallfit && /tmp/wallfit
#include "wallfit.h"
#include <chrono>
#include <cstdio>
#include <string>

using namespace wallfit;
static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// The room: x in [-2, 2], y in [0, 2.5], z in [-3, 1.5]; a sofa box in front of the front wall (z = -3).
struct Box { V3 lo, hi; };
static const Box ROOM{{-2, 0, -3}, {2, 2.5f, 1.5f}}, SOFA{{-1, 0, -2.8f}, {1, 0.8f, -2.2f}};

static Quat yawPitch(float yawDeg, float pitchDeg) {
    float y = yawDeg / 57.29578f / 2, p = pitchDeg / 57.29578f / 2;
    Quat qy{0, std::sin(y), 0, std::cos(y)}, qp{std::sin(p), 0, 0, std::cos(p)};
    return {qy.w * qp.x + qy.x * qp.w + qy.y * qp.z - qy.z * qp.y, qy.w * qp.y - qy.x * qp.z + qy.y * qp.w + qy.z * qp.x,
            qy.w * qp.z + qy.x * qp.y - qy.y * qp.x + qy.z * qp.w, qy.w * qp.w - qy.x * qp.x - qy.y * qp.y - qy.z * qp.z};
}

struct Synth {
    int w = 256, h = 256; float noise = 0.005f; bool sofa = true; unsigned seed = 7;
    std::vector<uint16_t> d16; std::vector<float> d01; DepthFrame f;
    DepthFrame render(V3 eye, Quat q) {
        f = DepthFrame{}; f.w = w; f.h = h; f.fov = {-0.785f, 0.785f, 0.785f, -0.785f}; f.pose = {q, eye}; f.nearZ = 0.1f;
        d16.assign(w * h, 0); d01.assign(w * h, 0);
        std::mt19937 rng(seed); std::normal_distribution<float> N(0, 1);
        float tl = std::tan(f.fov.left), tr = std::tan(f.fov.right), td = std::tan(f.fov.down), tu = std::tan(f.fov.up);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                V3 v{tl + (x + 0.5f) / w * (tr - tl), td + (y + 0.5f) / h * (tu - td), -1};
                V3 D = rotate(q, v);  // eye + D*t has view depth t
                float t = 1e9f; const float o[3] = {eye.x, eye.y, eye.z}, d[3] = {D.x, D.y, D.z};
                const float lo[3] = {ROOM.lo.x, ROOM.lo.y, ROOM.lo.z}, hi[3] = {ROOM.hi.x, ROOM.hi.y, ROOM.hi.z};
                for (int a = 0; a < 3; a++) if (std::fabs(d[a]) > 1e-9f) t = std::min(t, ((d[a] > 0 ? hi[a] : lo[a]) - o[a]) / d[a]);
                if (sofa) {  // slab test
                    const float slo[3] = {SOFA.lo.x, SOFA.lo.y, SOFA.lo.z}, shi[3] = {SOFA.hi.x, SOFA.hi.y, SOFA.hi.z};
                    float t0 = 0, t1 = 1e9f; bool hit = true;
                    for (int a = 0; a < 3 && hit; a++) {
                        if (std::fabs(d[a]) < 1e-9f) { if (o[a] < slo[a] || o[a] > shi[a]) hit = false; continue; }
                        float ta = (slo[a] - o[a]) / d[a], tb = (shi[a] - o[a]) / d[a]; if (ta > tb) std::swap(ta, tb);
                        t0 = std::max(t0, ta); t1 = std::min(t1, tb); if (t0 > t1) hit = false;
                    }
                    if (hit && t0 > 0 && t0 < t) t = t0;
                }
                float z = t * (1 + noise * N(rng));
                float dd = 1 - f.nearZ / z;  // far = infinity: zn = 1 - 2n/z, d = (zn + 1) / 2
                d01[y * w + x] = dd; d16[y * w + x] = (uint16_t)std::lround(std::clamp(dd, 0.0f, 1.0f) * 65535);
            }
        f.d16 = d16.data(); return f;
    }
};

struct Truth { const char* name; V3 n; V3 c; float w, h; };
static std::vector<Truth> truths() {
    return {{"front", {0, 0, 1}, {0, 1.25f, -3}, 4, 2.5f}, {"back", {0, 0, -1}, {0, 1.25f, 1.5f}, 4, 2.5f},
            {"left", {1, 0, 0}, {-2, 1.25f, -0.75f}, 4.5f, 2.5f}, {"right", {-1, 0, 0}, {2, 1.25f, -0.75f}, 4.5f, 2.5f},
            {"floor", {0, 1, 0}, {0, 0, -0.75f}, 4, 4.5f}, {"ceiling", {0, -1, 0}, {0, 2.5f, -0.75f}, 4, 4.5f}};
}
static HouseWall houseOf(const Truth& t, V3 shift = {}, float yawDeg = 0) {
    V3 r, u; axesFor(t.n, r, u);
    Quat q = fromAxes(r, u, t.n);
    if (yawDeg) { Quat y = yawPitch(yawDeg, 0); q = {y.w * q.x + y.x * q.w + y.y * q.z - y.z * q.y, y.w * q.y - y.x * q.z + y.y * q.w + y.z * q.x,
                                                     y.w * q.z + y.x * q.y - y.y * q.x + y.z * q.w, y.w * q.w - y.x * q.x - y.y * q.y - y.z * q.z}; }
    return {{q, t.c + shift}, t.w, t.h};
}

static const V3 EYE{0.3f, 1.6f, -0.5f};

static Fuser survey(float noise, int size = 256) {
    Fuser fz; Synth s; s.noise = noise; s.w = s.h = size;
    const float views[][2] = {{0, -10}, {45, -10}, {-45, -10}, {90, -10}, {-90, -10}, {180, -10}, {135, -10}, {-135, -10},
                              {0, -70}, {180, -70}, {0, 70}, {180, 70}, {30, -30}, {-30, -30}};
    for (auto& v : views) { s.seed++; fz.add(s.render(EYE, yawPitch(v[0], v[1]))); }
    return fz;
}

int main() {
    // single frame facing the front wall
    for (float noise : {0.005f, 0.01f}) {
        Synth s; s.noise = noise;
        FrameFit ff = fitFrame(s.render(EYE, yawPitch(0, -10)));
        const Observation* fw = nullptr;
        for (auto& o : ff.planes) if (angleDeg(o.n, {0, 0, 1}) < 3 && std::fabs(o.centroid.z + 3) < 0.1f) fw = &o;
        CHECK(fw, "noise %.3f: front wall not found in a single frame (%zu planes)", noise, ff.planes.size());
        if (fw) {
            V3 n; float rms; fw->sc.plane(n, rms);
            CHECK(angleDeg(fw->n, {0, 0, 1}) < 1.0f, "noise %.3f: single-frame normal off by %.2f deg", noise, angleDeg(fw->n, {0, 0, 1}));
            CHECK(std::fabs(fw->d - 3) < 0.015f, "noise %.3f: single-frame offset off by %.1f mm", noise, (fw->d - 3) * 1000);
            CHECK(rms < 3.0f * noise * 3.2f, "noise %.3f: rms %.1f mm", noise, rms * 1000);
        }
    }

    // fused survey: every surface found once, matched to its house wall, right size
    for (float noise : {0.005f, 0.01f}) {
        Fuser fz = survey(noise);
        auto walls = fz.walls(); auto T = truths();
        std::vector<HouseWall> house; for (auto& t : T) house.push_back(houseOf(t));
        std::vector<int> hits(T.size(), 0);
        for (auto& w : walls) {
            Match m = match(w, house);
            if (m.index < 0) continue;
            hits[m.index]++;
            const Truth& t = T[m.index];
            CHECK(std::fabs(w.w - t.w) < 0.2f && std::fabs(w.h - t.h) < 0.2f, "noise %.3f %s: extent %.2f x %.2f, want %.2f x %.2f", noise, t.name, w.w, w.h, t.w, t.h);
            CHECK(len(w.centre - t.c) < 0.12f, "noise %.3f %s: centre off by %.2f m", noise, t.name, len(w.centre - t.c));
            CHECK(std::string(kindName(w.kind)) == (m.index == 4 ? "floor" : m.index == 5 ? "ceiling" : "wall"), "%s: kind %s", t.name, kindName(w.kind));
            CHECK(w.confidence > 0.5f, "noise %.3f %s: confidence %.2f", noise, t.name, w.confidence);
            CHECK(w.rms_m < noise * 3 * 3.2f, "noise %.3f %s: fused rms %.1f mm", noise, t.name, w.rms_m * 1000);
            CHECK(m.angleDeg < 1.0f && m.offset < 0.015f, "noise %.3f %s: fused plane off %.2f deg / %.1f mm", noise, t.name, m.angleDeg, m.offset * 1000);
        }
        for (size_t i = 0; i < T.size(); i++) CHECK(hits[i] == 1, "noise %.3f: %s matched by %d fused planes (want 1)", noise, T[i].name, hits[i]);
        // the sofa front (z = -2.2) is a plane of its own, never the front wall
        bool sofaSeen = false;
        for (auto& w : walls) if (angleDeg(w.n, {0, 0, 1}) < 3 && std::fabs(w.centre.z + 2.2f) < 0.05f) {
            sofaSeen = true; CHECK(match(w, house).index < 0, "sofa front matched a house wall");
        }
        CHECK(sofaSeen, "noise %.3f: sofa front not fitted", noise);
        // the front wall's extent is not cut by the sofa (other viewpoints see past it; the box spans the wall)
        if (noise == 0.005f) {
            printf("survey (noise 0.5%%): %zu fused planes\n", walls.size());
            for (auto& w : walls) printf("  #%d %-7s n (%5.2f %5.2f %5.2f) c (%5.2f %5.2f %5.2f) %4.2f x %4.2f rms %4.1f mm  %ld pts %d frames conf %.2f\n",
                                         w.id, kindName(w.kind), w.n.x, w.n.y, w.n.z, w.centre.x, w.centre.y, w.centre.z, w.w, w.h, w.rms_m * 1000, w.samples, w.frames, w.confidence);
        }

        // matching tolerances: 5 cm off or 3 degrees off is a different wall
        Wall front; bool have = false;
        for (auto& w : walls) if (match(w, house).index == 0) { front = w; have = true; }
        CHECK(have, "no fused front wall");
        if (have) {
            CHECK(match(front, {houseOf(T[0], {0, 0, 0.05f})}).index < 0, "front wall matched a house wall 5 cm away");
            CHECK(match(front, {houseOf(T[0], {0, 0, 0.02f})}).index == 0, "front wall did not match a house wall 2 cm away");
            CHECK(match(front, {houseOf(T[0], {}, 3)}).index < 0, "front wall matched a house wall rotated 3 deg");
            CHECK(match(front, {houseOf(T[0], {6, 0, 0})}).index < 0, "front wall matched a coplanar wall 6 m along");
            // house file entry
            Pose p = front.pose(); V3 z = rotate(p.q, {0, 0, 1}), xr = rotate(p.q, {1, 0, 0});
            CHECK(angleDeg(z, front.n) < 0.01f && angleDeg(xr, {1, 0, 0}) < 1.0f, "front wall pose axes wrong");

            // screens on the front wall (wall-2D x, y from the wall centre (0, 1.25, -3))
            Synth s; s.noise = noise; DepthFrame f = s.render(EYE, yawPitch(0, -10));
            HouseWall hw = houseOf(T[0]);
            struct Case { const char* name; float sunk; float yaw; };
            for (Case c : {Case{"on the wall", 0, 0}, Case{"sunk 5 cm", 0.05f, 0}, Case{"floating 10 cm", -0.10f, 0}}) {
                Pose wp = hw.pose; wp.p = wp.p - V3{0, 0, c.sunk};
                Gap g = screenGap(f, wp, 1.0f, 0.4f, 1.2f, 0.75f);
                CHECK(g.valid > 0.9f && std::fabs(g.median - c.sunk) < 0.012f + 2 * noise, "screen %s: frame gap %.1f mm (valid %.2f), want %.0f", c.name, g.median * 1000, g.valid, c.sunk * 1000);
                for (float k : {g.tl, g.tr, g.bl, g.br}) CHECK(std::fabs(k - c.sunk) < 0.02f + 3 * noise, "screen %s: corner %.1f mm", c.name, k * 1000);
                Gap gp = screenGapPlane(front, wp, 1.0f, 0.4f, 1.2f, 0.75f);
                CHECK(std::fabs(gp.median - c.sunk) < 0.01f, "screen %s: plane gap %.1f mm, want %.0f", c.name, gp.median * 1000, c.sunk * 1000);
            }
            // a yawed screen (relocalisation error): left and right corners on opposite sides of the wall
            Pose wp = hw.pose; wp.p = wp.p + V3{1.0f, 0.4f, 0};  // pivot at the screen centre
            wp.q = yawPitch(5, 0);
            Gap g = screenGap(f, wp, 0, 0, 1.2f, 0.75f);
            CHECK(g.tl * g.tr < 0 && g.bl * g.br < 0 && std::fabs(g.tl - g.tr) > 0.04f, "yawed screen: corners tl %.1f tr %.1f bl %.1f br %.1f mm",
                  g.tl * 1000, g.tr * 1000, g.bl * 1000, g.br * 1000);
            Gap gp = screenGapPlane(front, wp, 0, 0, 1.2f, 0.75f);
            CHECK(gp.tl * gp.tr < 0, "yawed screen vs plane: tl %.1f tr %.1f", gp.tl * 1000, gp.tr * 1000);
            if (noise == 0.005f) printf("yawed 5 deg screen: corners tl %.0f tr %.0f bl %.0f br %.0f mm, median %.0f\n", g.tl * 1000, g.tr * 1000, g.bl * 1000, g.br * 1000, g.median * 1000);
        }
    }

    // not one lucky seed: other RANSAC and noise seeds give the same six surfaces, once each, right size
    {
        int bad = 0, runs = 0;
        for (float noise : {0.005f, 0.01f})
            for (unsigned seed = 1; seed <= 6; seed++) {
                Params P; P.seed = seed * 7919; Fuser fz(P); Synth s; s.noise = noise; s.seed = seed * 101;
                const float views[][2] = {{0, -10}, {45, -10}, {-45, -10}, {90, -10}, {-90, -10}, {180, -10}, {135, -10}, {-135, -10}, {0, -70}, {180, -70}, {0, 70}, {180, 70}};
                for (auto& v : views) { s.seed++; fz.add(s.render(EYE, yawPitch(v[0], v[1]))); }
                auto T = truths(); std::vector<HouseWall> house; for (auto& t : T) house.push_back(houseOf(t));
                std::vector<int> hits(T.size(), 0); bool ok = true;
                for (auto& w : fz.walls()) {
                    Match m = match(w, house); if (m.index < 0) continue; hits[m.index]++;
                    ok &= std::fabs(w.w - T[m.index].w) < 0.2f && std::fabs(w.h - T[m.index].h) < 0.2f;
                }
                for (int h : hits) ok &= h == 1;
                runs++; if (!ok) bad++;
            }
        CHECK(bad == 0, "%d of %d seeded surveys wrong", bad, runs);
        printf("seeded surveys: %d of %d right\n", runs - bad, runs);
    }

    // stale frames are dropped; empty depth fits nothing; d01 and d16 agree
    {
        Fuser fz; Synth s; DepthFrame f = s.render(EYE, yawPitch(0, -10)); f.timeNs = 1000000000;
        fz.add(f, 1700000000);
        CHECK(fz.dropped == 1 && fz.tracks.empty(), "a 0.7 s old frame was fused");
        fz.add(f, 1200000000);
        CHECK(fz.dropped == 1 && !fz.tracks.empty(), "a 0.2 s old frame was dropped");
        std::vector<uint16_t> none(s.w * s.h, 65535), zero(s.w * s.h, 0);
        DepthFrame e = f; e.d16 = none.data(); CHECK(fitFrame(e).planes.empty(), "planes from an all-far frame");
        e.d16 = zero.data(); CHECK(fitFrame(e).planes.empty(), "planes from an all-zero frame");
        DepthFrame g = f; g.d16 = nullptr; g.d01 = s.d01.data();
        CHECK(fitFrame(g).planes.size() == fitFrame(f).planes.size(), "d01 and d16 disagree");
    }

    // cost per frame (VPS CPU; the headset will be a few times slower)
    for (int size : {256, 512, 2000}) {
        Synth s; s.w = s.h = size; DepthFrame f = s.render(EYE, yawPitch(30, -20));
        auto t0 = std::chrono::steady_clock::now(); FrameFit ff; int reps = size > 512 ? 2 : 10;
        for (int i = 0; i < reps; i++) ff = fitFrame(f);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / reps;
        printf("fitFrame %dx%d: %.1f ms (%d grid points, %zu planes)\n", size, size, ms, ff.validPoints, ff.planes.size());
        if (size == 256) CHECK(ms < 100, "256x256 frame took %.1f ms", ms);
    }
    printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
