// roommesh.h: the progressive mesh of a ray-cast room (4 walls, floor, ceiling, a sofa) from synthetic depth, and a
// replay of a real Quest depth dump.   g++ -std=c++17 -O2 -I room_lab bench/roommesh_test.cpp -o /tmp/rm && /tmp/rm [dump dir]
// Run it under a memory cap (run_offline_tests.sh does): ulimit -v 1500000; timeout 120.
#include "json.h"
#include "roommesh.h"
#include "loadtol.h"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>

using namespace roommesh;
using wallfit::Quat;
static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct Box { V3 lo, hi; };
static const Box ROOM{{-2, 0, -3}, {2, 2.5f, 1.5f}}, SOFA{{-1, 0, -2.8f}, {1, 0.8f, -2.2f}};
static Quat yawPitch(float yawDeg, float pitchDeg) {
    float y = yawDeg / 57.29578f / 2, p = pitchDeg / 57.29578f / 2;
    Quat qy{0, std::sin(y), 0, std::cos(y)}, qp{std::sin(p), 0, 0, std::cos(p)};
    return {qy.w * qp.x + qy.x * qp.w + qy.y * qp.z - qy.z * qp.y, qy.w * qp.y - qy.x * qp.z + qy.y * qp.w + qy.z * qp.x,
            qy.w * qp.z + qy.x * qp.y - qy.y * qp.x + qy.z * qp.w, qy.w * qp.w - qy.x * qp.x - qy.y * qp.y - qy.z * qp.z};
}
// distance from p to the nearest true surface (room faces and the sofa's faces)
static float surfaceDist(V3 p) {
    float d = std::min({std::fabs(p.x - ROOM.lo.x), std::fabs(p.x - ROOM.hi.x), std::fabs(p.y - ROOM.lo.y), std::fabs(p.y - ROOM.hi.y),
                        std::fabs(p.z - ROOM.lo.z), std::fabs(p.z - ROOM.hi.z)});
    V3 c{std::clamp(p.x, SOFA.lo.x, SOFA.hi.x), std::clamp(p.y, SOFA.lo.y, SOFA.hi.y), std::clamp(p.z, SOFA.lo.z, SOFA.hi.z)};
    bool inside = c.x == p.x && c.y == p.y && c.z == p.z;
    float ds = inside ? std::min({p.x - SOFA.lo.x, SOFA.hi.x - p.x, p.y - SOFA.lo.y, SOFA.hi.y - p.y, p.z - SOFA.lo.z, SOFA.hi.z - p.z}) : wallfit::len(p - c);
    return std::min(d, ds);
}
struct Synth {
    int w = 256, h = 256; float noise = 0.005f; unsigned seed = 7; std::vector<uint16_t> d16;
    DepthFrame render(V3 eye, Quat q) {
        DepthFrame f; f.w = w; f.h = h; f.fov = {-0.785f, 0.785f, 0.785f, -0.785f}; f.pose = {q, eye}; f.nearZ = 0.1f;
        d16.assign(w * h, 0); std::mt19937 rng(seed++); std::normal_distribution<float> N(0, 1);
        float tl = std::tan(f.fov.left), tr = std::tan(f.fov.right), td = std::tan(f.fov.down), tu = std::tan(f.fov.up);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            V3 D = wallfit::rotate(q, {tl + (x + 0.5f) / w * (tr - tl), td + (y + 0.5f) / h * (tu - td), -1});
            float t = 1e9f; const float o[3] = {eye.x, eye.y, eye.z}, d[3] = {D.x, D.y, D.z};
            const float lo[3] = {ROOM.lo.x, ROOM.lo.y, ROOM.lo.z}, hi[3] = {ROOM.hi.x, ROOM.hi.y, ROOM.hi.z};
            for (int a = 0; a < 3; a++) if (std::fabs(d[a]) > 1e-9f) t = std::min(t, ((d[a] > 0 ? hi[a] : lo[a]) - o[a]) / d[a]);
            const float slo[3] = {SOFA.lo.x, SOFA.lo.y, SOFA.lo.z}, shi[3] = {SOFA.hi.x, SOFA.hi.y, SOFA.hi.z};
            float t0 = 0, t1 = 1e9f; bool hit = true;
            for (int a = 0; a < 3 && hit; a++) {
                if (std::fabs(d[a]) < 1e-9f) { if (o[a] < slo[a] || o[a] > shi[a]) hit = false; continue; }
                float ta = (slo[a] - o[a]) / d[a], tb = (shi[a] - o[a]) / d[a]; if (ta > tb) std::swap(ta, tb);
                t0 = std::max(t0, ta); t1 = std::min(t1, tb); if (t0 > t1) hit = false;
            }
            if (hit && t0 > 0 && t0 < t) t = t0;
            float z = t * (1 + noise * N(rng)), dd = 1 - f.nearZ / z;
            d16[y * w + x] = (uint16_t)std::lround(std::clamp(dd, 0.0f, 1.0f) * 65535);
        }
        f.d16 = d16.data(); return f;
    }
};
static std::string slurp(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }

int main(int argc, char** argv) {
    const V3 EYE{0.3f, 1.6f, -0.5f};
    const float views[][2] = {{0, -10}, {45, -10}, {-45, -10}, {90, -10}, {-90, -10}, {180, -10}, {135, -10}, {-135, -10}, {0, -70}, {180, -70}, {0, 70}, {180, 70}};
    Volume vol; Synth s; std::vector<size_t> tris; double worstMs = 0, totalMs = 0; size_t firstChanged = 0; std::vector<double> frameMs;
    for (int i = 0; i < 12; i++) {
        DepthFrame f = s.render(EYE, yawPitch(views[i][0], views[i][1]));
        auto t0 = std::chrono::steady_clock::now();
        vol.integrate(f); auto changed = vol.update();
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        worstMs = std::max(worstMs, ms); totalMs += ms; frameMs.push_back(ms); tris.push_back(vol.triangles());
        if (i == 0) firstChanged = changed.size();
        // the changed list names exactly the blocks whose mesh version moved
        for (auto& k : changed) CHECK(vol.mesh(k) != nullptr, "changed block without a mesh");
    }
    printf("12 frames: %zu blocks, %zu triangles, integrate+update %.1f ms avg / %.1f ms worst (VPS)\n", vol.blocks(), vol.triangles(), totalMs / 12, worstMs);
    // bounds: every vertex inside its block's AABB, and the AABB within the block's cube plus one voxel
    {
        size_t bad = 0;
        vol.forEachMesh([&](const Key& k, const BlockMesh& m) {
            float bs = B * vol.P.voxel, lo[3] = {k.x * bs, k.y * bs, k.z * bs};
            for (int j = 0; j < 3; j++) bad += m.lo[j] < lo[j] - vol.P.voxel * 1.01f || m.hi[j] > lo[j] + bs + vol.P.voxel * 1.01f || m.lo[j] > m.hi[j];
            for (size_t v = 0; v < m.pos.size(); v += 3) for (int j = 0; j < 3; j++) bad += m.pos[v + j] < m.lo[j] || m.pos[v + j] > m.hi[j];
            bad += wallfit::len(m.centre() - V3{lo[0] + bs / 2, lo[1] + bs / 2, lo[2] + bs / 2}) > bs || m.radius() > bs;
        });
        CHECK(bad == 0, "%zu block bounds problems", bad);
    }
    // grows and refines: more surface after more views
    CHECK(tris[0] > 0 && tris[11] > tris[0] * 2, "mesh did not grow: %zu -> %zu triangles", tris[0], tris[11]);
    CHECK(firstChanged > 0, "first frame changed no blocks");
    CHECK(vol.update().empty(), "an update with no new depth changed blocks");
    // accuracy: mesh vertices lie on the true surfaces
    std::vector<float> err; size_t nanv = 0;
    vol.forEachMesh([&](const Key&, const BlockMesh& m) {
        for (size_t i = 0; i < m.pos.size(); i += 3) {
            V3 p{m.pos[i], m.pos[i + 1], m.pos[i + 2]};
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) { nanv++; continue; }
            err.push_back(surfaceDist(p));
        }
        for (float v : m.nrm) nanv += !std::isfinite(v);
    });
    // every triangle faces the free side (its winding agrees with the distance gradient at its corners)
    size_t agree = 0, total = 0;
    vol.forEachMesh([&](const Key&, const BlockMesh& m) {
        for (size_t i = 0; i < m.idx.size(); i += 3) {
            auto P = [&](uint32_t v) { return V3{m.pos[v * 3], m.pos[v * 3 + 1], m.pos[v * 3 + 2]}; };
            auto N = [&](uint32_t v) { return V3{m.nrm[v * 3], m.nrm[v * 3 + 1], m.nrm[v * 3 + 2]}; };
            V3 fn = wallfit::cross(P(m.idx[i + 1]) - P(m.idx[i]), P(m.idx[i + 2]) - P(m.idx[i]));
            if (wallfit::len(fn) < 1e-12f) continue;
            total++; agree += wallfit::dot(fn, N(m.idx[i]) + N(m.idx[i + 1]) + N(m.idx[i + 2])) > 0;
        }
    });
    printf("winding agrees with the normals on %zu of %zu triangles\n", agree, total);
    CHECK(agree > total * 0.97, "winding: %zu of %zu triangles face the free side", agree, total);
    std::sort(err.begin(), err.end());
    float med = err.empty() ? 1 : err[err.size() / 2], p95 = err.empty() ? 1 : err[err.size() * 95 / 100];
    printf("vertex distance to the true surfaces: median %.1f mm, p95 %.1f mm (%zu vertices)\n", med * 1000, p95 * 1000, err.size());
    CHECK(nanv == 0, "%zu non-finite vertices or normals", nanv);
    CHECK(med < 0.012f && p95 < 0.04f, "mesh off the surfaces: median %.1f mm p95 %.1f mm", med * 1000, p95 * 1000);
    // coverage: points on the walls the views saw have mesh nearby
    {
        std::unordered_map<long long, int> grid; float c = 0.05f;
        auto cell = [&](V3 p) { return ((long long)std::floor(p.x / c) * 1000003LL + (long long)std::floor(p.y / c)) * 1000003LL + (long long)std::floor(p.z / c); };
        vol.forEachMesh([&](const Key&, const BlockMesh& m) { for (size_t i = 0; i < m.pos.size(); i += 3) grid[cell({m.pos[i], m.pos[i + 1], m.pos[i + 2]})] = 1; });
        auto near = [&](V3 p) {
            for (int dx = -1; dx <= 1; dx++)
                for (int dy = -1; dy <= 1; dy++)
                    for (int dz = -1; dz <= 1; dz++)
                        if (grid.count(cell({p.x + dx * c, p.y + dy * c, p.z + dz * c}))) return true;
            return false;
        };
        std::mt19937 rng(3); std::uniform_real_distribution<float> U(0, 1); int hit = 0, n = 0;
        for (int i = 0; i < 2000; i++) {  // the four walls, away from the corners and the sofa
            float a = U(rng), b = 0.9f + U(rng) * 1.4f; V3 p;
            switch (i % 4) { case 0: p = {-1.8f + a * 3.6f, b, -3}; break; case 1: p = {-1.8f + a * 3.6f, b, 1.5f}; break;
                             case 2: p = {-2, b, -2.8f + a * 4.1f}; break; default: p = {2, b, -2.8f + a * 4.1f}; }
            n++; hit += near(p);
        }
        printf("wall coverage: %d of %d sampled points have mesh within ~5 cm\n", hit, n);
        CHECK(hit > n * 0.9, "wall coverage %.0f%%", 100.0 * hit / n);
    }
    // the per-frame budget on the headset is <= 10 ms per update at 2-4 Hz; the VPS should be well inside it.
    // Load-tolerant (loadtol.h): the median frame against a limit scaled by this run's calibration; worst is info.
    CHECK(loadtol::ok("roommesh integrate+update per frame", frameMs, 40), "integrate+update median %.1f ms on the VPS", loadtol::median(frameMs));
    // incremental: a budgeted update leaves the rest for next time
    {
        Volume v2; Synth s2; v2.integrate(s2.render(EYE, yawPitch(0, -10)));
        size_t d0 = v2.dirtyBlocks(); auto part = v2.update(0.001);
        CHECK(part.size() < d0 && v2.dirtyBlocks() > 0, "a tiny budget did all %zu blocks", d0);
        v2.update(); CHECK(v2.dirtyBlocks() == 0, "dirty blocks left after a full update");
    }
    // save / load: identical mesh after reload, corruption refused
    {
        std::string blob = vol.save(); Volume v3;
        CHECK(v3.load(blob), "load of a saved volume failed");
        v3.update(); CHECK(v3.triangles() == vol.triangles(), "reloaded mesh %zu triangles vs %zu", v3.triangles(), vol.triangles());
        printf("saved volume: %zu KB for %zu blocks\n", blob.size() / 1024, vol.blocks());
        std::string bad = blob; bad[bad.size() / 2] ^= 0x40; Volume v4;
        CHECK(!v4.load(bad) && v4.blocks() == 0, "a corrupted blob loaded");
        CHECK(!v4.load(blob.substr(0, blob.size() - 10)), "a truncated blob loaded");
        CHECK(!v4.load("PPRM"), "a stub loaded");
    }
    // memory cap: refuses blocks beyond it, never grows past it
    {
        Params p; p.maxBlocks = 50; Volume v5(p); Synth s5;
        for (int i = 0; i < 3; i++) v5.integrate(s5.render(EYE, yawPitch(views[i][0], views[i][1])));
        CHECK(v5.blocks() <= 50 && v5.refusedBlocks > 0, "cap: %zu blocks, %ld refused", v5.blocks(), v5.refusedBlocks);
    }
    // tracking loss (review fix): a NaN pose fuses nothing (no NaN to integer casts)
    {
        Volume v7; Synth s7; DepthFrame f = s7.render(EYE, yawPitch(0, -10)); f.pose.p.y = NAN;
        CHECK(v7.integrate(f) == 0 && v7.blocks() == 0, "a NaN pose was fused");
    }
    // empty and far-only depth fuse nothing
    {
        Volume v6; Synth s6; DepthFrame f = s6.render(EYE, yawPitch(0, -10));
        std::vector<uint16_t> none(f.w * f.h, 65535); f.d16 = none.data();
        CHECK(v6.integrate(f) == 0 && v6.blocks() == 0, "blocks from an empty frame");
    }
    // a real Quest depth dump (both eyes), when given
    if (argc > 1) {
        std::string dir = argv[1]; bool ok = false; json::Value m = json::parse(slurp(dir + "/meta.json"), &ok);
        CHECK(ok, "meta.json"); Volume vr; const char* eye[2] = {"left", "right"}; std::vector<uint16_t> buf[2];
        for (int e = 0; e < 2 && ok; e++) {
            int w = m["w"].integer(), h = m["h"].integer(); std::string raw = slurp(dir + "/" + eye[e] + ".d16");
            buf[e].resize(w * h); memcpy(buf[e].data(), raw.data(), std::min(raw.size(), buf[e].size() * 2));
            const json::Value& ev = m["eyes"][e]; DepthFrame f; f.w = w; f.h = h; f.d16 = buf[e].data();
            f.nearZ = (float)m["near"].num(); f.farZ = m["far"].num() < 0 ? INFINITY : (float)m["far"].num();
            f.fov = {(float)ev["fov"][0].num(), (float)ev["fov"][1].num(), (float)ev["fov"][2].num(), (float)ev["fov"][3].num()};
            f.pose = {{(float)ev["pose"]["q"][0].num(), (float)ev["pose"]["q"][1].num(), (float)ev["pose"]["q"][2].num(), (float)ev["pose"]["q"][3].num()},
                      {(float)ev["pose"]["p"][0].num(), (float)ev["pose"]["p"][1].num(), (float)ev["pose"]["p"][2].num()}};
            auto t0 = std::chrono::steady_clock::now(); vr.integrate(f); vr.update();
            printf("real depth %s eye: %zu blocks, %zu triangles, %.1f ms\n", eye[e], vr.blocks(), vr.triangles(),
                   std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }
        CHECK(vr.triangles() > 500, "real depth gave %zu triangles", vr.triangles());
    }
    printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
