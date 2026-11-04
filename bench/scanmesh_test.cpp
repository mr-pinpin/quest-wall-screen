// The scan meshes, before/after: rawsurface.h (live depth surface) and roomplanes.h (the fused mesh regularised by the
// room's planes), on the synthetic room (truth known) and on a real Quest depth dump. Prints the quality table:
// triangles per planar m^2, vertex deviation from the true surfaces, hole fraction on the walls.
//   g++ -std=c++17 -O2 -I room_lab bench/scanmesh_test.cpp -o /tmp/sm && /tmp/sm [dump dir]   (ulimit -v 1500000; timeout 120)
#define main roommesh_test_main
#include "roommesh_test.cpp"
#undef main
#include "rawsurface.h"
#include "roomplanes.h"

static Volume survey(Synth& s, wallfit::Fuser& fz) {
    const V3 EYE{0.3f, 1.6f, -0.5f}; Volume vol;
    const float views[][2] = {{0, -10}, {45, -10}, {-45, -10}, {90, -10}, {-90, -10}, {180, -10}, {135, -10}, {-135, -10}, {0, -70}, {180, -70}, {0, 70}, {180, 70}};
    for (auto& v : views) { DepthFrame f = s.render(EYE, yawPitch(v[0], v[1])); vol.integrate(f); fz.add(f); }
    vol.update(); return vol;
}

int main(int argc, char** argv) {
    const V3 EYE{0.3f, 1.6f, -0.5f};
    // ---- 1. raw depth surface: one frame, smoothed, cut at the sofa's edges
    {
        Synth s; s.noise = 0.01f; DepthFrame f = s.render(EYE, yawPitch(0, -10));
        rawsurface::Params p0; p0.radius = 0;  // no smoothing, for comparison
        std::vector<double> rawMs; rawsurface::Mesh m;
        for (int rep = 0; rep < 5; rep++) { auto t0 = std::chrono::steady_clock::now(); m = rawsurface::build(f);
            rawMs.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count()); }
        double ms = loadtol::median(rawMs);
        rawsurface::Mesh raw = rawsurface::build(f, p0);
        auto rms = [](const rawsurface::Mesh& mm) { double s2 = 0; size_t n = 0; for (size_t i = 0; i < mm.pos.size(); i += 3) { float d = surfaceDist({mm.pos[i], mm.pos[i + 1], mm.pos[i + 2]}); s2 += d * d; n++; } return n ? std::sqrt(s2 / n) : 1.0; };
        double rs = rms(m), rr = rms(raw);
        // no triangle bridges the sofa front (z -2.2) and the wall behind it (z -3)
        size_t bridges = 0;
        for (size_t i = 0; i < m.idx.size(); i += 3) {
            float zs[3]; for (int j = 0; j < 3; j++) zs[j] = m.pos[m.idx[i + j] * 3 + 2];
            bridges += *std::max_element(zs, zs + 3) - *std::min_element(zs, zs + 3) > 0.3f;
        }
        printf("raw surface: %zu triangles from a %dx%d grid in %.2f ms; rms to truth %.1f mm smoothed vs %.1f mm unsmoothed; %zu bridging triangles\n",
               m.triangles(), m.gw, m.gh, ms, rs * 1000, rr * 1000, bridges);
        CHECK(m.triangles() > 20000, "raw surface too sparse (%zu)", m.triangles());
        CHECK(rs < rr * 0.7, "bilateral smoothing did not reduce noise (%.1f vs %.1f mm)", rs * 1000, rr * 1000);
        CHECK(bridges == 0, "%zu triangles bridge a depth discontinuity", bridges);
        CHECK(loadtol::ok("raw surface", rawMs, 20), "raw surface took %.1f ms (median)", ms);
        size_t facing = 0; for (size_t i = 0; i < m.idx.size(); i += 3) {
            auto P = [&](uint32_t v) { return V3{m.pos[v * 3], m.pos[v * 3 + 1], m.pos[v * 3 + 2]}; };
            V3 n = wallfit::cross(P(m.idx[i + 1]) - P(m.idx[i]), P(m.idx[i + 2]) - P(m.idx[i]));
            facing += wallfit::dot(n, EYE - P(m.idx[i])) > 0;
        }
        CHECK(facing == m.triangles(), "%zu of %zu raw triangles face the eye", facing, m.triangles());
    }
    // ---- 2. fused mesh, before and after plane regularisation (synthetic: truth known)
    {
        Synth s; wallfit::Fuser fz; Volume vol = survey(s, fz);
        std::vector<wallfit::Wall> planes; for (auto& w : fz.walls()) if (w.confidence > 0.3f) planes.push_back(w);
        std::vector<double> regMs; roomplanes::Result R;
        for (int rep = 0; rep < 3; rep++) { auto t0 = std::chrono::steady_clock::now(); R = roomplanes::regularize(vol, planes);
            regMs.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count()); }
        double ms = loadtol::median(regMs);
        // before: triangles on planar surfaces (truth: within 2 cm of a room face) and their deviation
        size_t beforePlanarTris = 0; std::vector<float> devBefore, devAfter;
        vol.forEachMesh([&](const Key&, const BlockMesh& m) {
            for (size_t i = 0; i < m.pos.size(); i += 3) devBefore.push_back(surfaceDist({m.pos[i], m.pos[i + 1], m.pos[i + 2]}));
            for (size_t i = 0; i < m.idx.size(); i += 3) {
                V3 c{0, 0, 0}; for (int j = 0; j < 3; j++) c = c + V3{m.pos[m.idx[i + j] * 3], m.pos[m.idx[i + j] * 3 + 1], m.pos[m.idx[i + j] * 3 + 2]} * (1.0f / 3);
                float dr = std::min({std::fabs(c.x - ROOM.lo.x), std::fabs(c.x - ROOM.hi.x), std::fabs(c.y - ROOM.lo.y), std::fabs(c.y - ROOM.hi.y), std::fabs(c.z - ROOM.lo.z), std::fabs(c.z - ROOM.hi.z)});
                beforePlanarTris += dr < 0.02f;
            }
        });
        for (auto& p : R.patches) for (auto& q : p.quads) for (auto& c : q) devAfter.push_back(surfaceDist(c));
        for (size_t i = 0; i < R.pos.size(); i += 3) devAfter.push_back(surfaceDist({R.pos[i], R.pos[i + 1], R.pos[i + 2]}));
        auto pct = [](std::vector<float> v, float q) { if (v.empty()) return 0.0f; std::sort(v.begin(), v.end()); return v[std::min(v.size() - 1, (size_t)(q * v.size()))]; };
        // holes: points on the walls (as the coverage test samples them) not covered by a patch of their plane
        auto covered = [&](V3 p, V3 n) {
            for (auto& pt : R.patches) {
                if (wallfit::dot(pt.n, n) < 0.95f) continue;
                const wallfit::Wall& pl = planes[pt.plane]; if (std::fabs(wallfit::dot(pl.n, p) + pl.d) > 0.03f) continue;
                V3 v = p - pl.centre; float x = wallfit::dot(v, pl.right), y = wallfit::dot(v, pl.up);
                for (auto& q : pt.quads) {
                    V3 a = q[0] - pl.centre, c = q[2] - pl.centre;
                    if (x >= wallfit::dot(a, pl.right) - 1e-3f && x <= wallfit::dot(c, pl.right) + 1e-3f && y >= wallfit::dot(a, pl.up) - 1e-3f && y <= wallfit::dot(c, pl.up) + 1e-3f) return true;
                }
            }
            return false;
        };
        std::mt19937 rng(3); std::uniform_real_distribution<float> U(0, 1); int holes = 0, n = 0;
        for (int i = 0; i < 2000; i++) {
            float a = U(rng), b = 0.9f + U(rng) * 1.4f; V3 p, nn;
            switch (i % 4) { case 0: p = {-1.8f + a * 3.6f, b, -3}; nn = {0, 0, 1}; break; case 1: p = {-1.8f + a * 3.6f, b, 1.5f}; nn = {0, 0, -1}; break;
                             case 2: p = {-2, b, -2.8f + a * 4.1f}; nn = {1, 0, 0}; break; default: p = {2, b, -2.8f + a * 4.1f}; nn = {-1, 0, 0}; }
            n++; holes += !covered(p, nn);
        }
        float area = R.planarArea();
        printf("\nfused mesh, synthetic room (%zu planes, regularised in %.1f ms):\n", planes.size(), ms);
        printf("| | triangles | planar triangles per m^2 | vertex deviation median / p95 | wall holes |\n|---|---|---|---|---|\n");
        printf("| before (roommesh) | %zu | %.0f | %.1f / %.1f mm | - |\n", vol.triangles(), beforePlanarTris / std::max(area, 1e-3f), pct(devBefore, 0.5f) * 1000, pct(devBefore, 0.95f) * 1000);
        printf("| after (roomplanes) | %zu (%zu quads + %zu object) | %.1f | %.1f / %.1f mm | %.1f%% |\n", R.triangles(), R.quads(), R.idx.size() / 3,
               R.quads() * 2 / std::max(area, 1e-3f), pct(devAfter, 0.5f) * 1000, pct(devAfter, 0.95f) * 1000, 100.0 * holes / n);
        CHECK(R.patches.size() >= 6, "only %zu planar patches", R.patches.size());
        CHECK(R.quads() * 2 * 10 < beforePlanarTris, "planar surfaces not simplified: %zu quads vs %zu triangles", R.quads(), beforePlanarTris);
        CHECK(holes < n * 0.05, "wall holes %.1f%%", 100.0 * holes / n);
        CHECK(pct(devAfter, 0.5f) <= pct(devBefore, 0.5f) + 0.001f, "regularised mesh further from the truth");
        CHECK(R.objectTriangles > 0, "the sofa disappeared (no object triangles)");
        CHECK(loadtol::ok("regularise", regMs, 200), "regularise took %.0f ms (median)", ms);
        // every quad corner is exactly on its plane
        float worst = 0; for (auto& p : R.patches) for (auto& q : p.quads) for (auto& c : q) worst = std::max(worst, std::fabs(wallfit::dot(planes[p.plane].n, c) + planes[p.plane].d));
        CHECK(worst < 1e-4f, "quad corner %.2f mm off its plane", worst * 1000);
        std::vector<float> P, N; std::vector<uint32_t> I; std::vector<uint8_t> planar; R.toMesh(P, N, I, planar);
        CHECK(planar.size() == I.size() / 3 && I.size() / 3 == R.triangles(), "toMesh sizes");
    }
    // ---- 3. the real depth dump: before / after (no truth: size and flatness)
    if (argc > 1) {
        std::string dir = argv[1]; bool ok = false; json::Value m = json::parse(slurp(dir + "/meta.json"), &ok);
        Volume vol; wallfit::Fuser fz; std::vector<uint16_t> buf[2]; const char* eye[2] = {"left", "right"};
        for (int e = 0; e < 2 && ok; e++) {
            int w = m["w"].integer(), h = m["h"].integer(); std::string raw = slurp(dir + "/" + eye[e] + ".d16");
            buf[e].resize(w * h); memcpy(buf[e].data(), raw.data(), std::min(raw.size(), buf[e].size() * 2));
            const json::Value& ev = m["eyes"][e]; DepthFrame f; f.w = w; f.h = h; f.d16 = buf[e].data();
            f.nearZ = (float)m["near"].num(); f.farZ = m["far"].num() < 0 ? INFINITY : (float)m["far"].num();
            f.fov = {(float)ev["fov"][0].num(), (float)ev["fov"][1].num(), (float)ev["fov"][2].num(), (float)ev["fov"][3].num()};
            f.pose = {{(float)ev["pose"]["q"][0].num(), (float)ev["pose"]["q"][1].num(), (float)ev["pose"]["q"][2].num(), (float)ev["pose"]["q"][3].num()},
                      {(float)ev["pose"]["p"][0].num(), (float)ev["pose"]["p"][1].num(), (float)ev["pose"]["p"][2].num()}};
            vol.integrate(f); fz.add(f);
            if (e == 0) { auto rs = rawsurface::build(f); printf("\nreal dump, raw surface (left eye): %zu triangles\n", rs.triangles()); CHECK(rs.triangles() > 5000, "raw surface on real depth: %zu", rs.triangles()); }
        }
        vol.update();
        std::vector<wallfit::Wall> planes; for (auto& w : fz.walls()) if (w.samples > 300) planes.push_back(w);
        roomplanes::Result R = roomplanes::regularize(vol, planes);
        // flatness of what became planar: before, the fused vertices' distance to their plane; after, 0 by construction
        std::vector<float> flat;
        vol.forEachMesh([&](const Key&, const BlockMesh& bm) { for (size_t i = 0; i < bm.pos.size(); i += 3) { V3 p{bm.pos[i], bm.pos[i + 1], bm.pos[i + 2]};
            float best = 1; for (auto& pl : planes) best = std::min(best, std::fabs(wallfit::dot(pl.n, p) + pl.d)); if (best < 0.05f) flat.push_back(best); } });
        double s2 = 0; for (float d : flat) s2 += d * d;
        printf("real dump, fused: before %zu triangles; after %zu (%zu quads over %.2f m^2 on %zu planes + %zu object triangles); "
               "near-plane vertices rms %.1f mm before, 0 after (on the plane)\n", vol.triangles(), R.triangles(), R.quads(), R.planarArea(), R.patches.size(),
               R.idx.size() / 3, flat.empty() ? 0 : std::sqrt(s2 / flat.size()) * 1000);
        CHECK(R.triangles() < vol.triangles(), "regularising the real mesh did not reduce it");
    }
    printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
