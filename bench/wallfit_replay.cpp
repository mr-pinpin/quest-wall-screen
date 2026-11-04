#include "wallfit.h"
#include "json.h"
// Replay of a real depth dump (files/depth-<t>/ from room_lab, pulled to house/fixtures/): fits each eye alone and
// compares the eyes (same instant, 6 cm apart).   g++ -std=c++17 -O2 -I room_lab bench/wallfit_replay.cpp -o /tmp/r && /tmp/r house/fixtures/depth-47041720 [param value ...]
#include <cstdio>
#include <fstream>
#include <sstream>
using namespace wallfit;
static std::string slurp(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }
int main(int argc, char** argv) {
    std::string dir = argv[1]; bool ok = false; json::Value m = json::parse(slurp(dir + "/meta.json"), &ok);
    int w = m["w"].integer(), h = m["h"].integer(); float nearZ = (float)m["near"].num(), farZ = m["far"].num() < 0 ? INFINITY : (float)m["far"].num();
    const char* names[2] = {"left", "right"}; std::vector<uint16_t> buf[2]; std::vector<Wall> walls[2];
    for (int e = 0; e < 2; e++) {
        std::string raw = slurp(dir + "/" + names[e] + ".d16"); buf[e].resize(w * h); memcpy(buf[e].data(), raw.data(), std::min(raw.size(), buf[e].size() * 2));
        const json::Value& ev = m["eyes"][e];
        DepthFrame f; f.w = w; f.h = h; f.d16 = buf[e].data(); f.nearZ = nearZ; f.farZ = farZ;
        f.fov = {(float)ev["fov"][0].num(), (float)ev["fov"][1].num(), (float)ev["fov"][2].num(), (float)ev["fov"][3].num()};
        f.pose = {{(float)ev["pose"]["q"][0].num(), (float)ev["pose"]["q"][1].num(), (float)ev["pose"]["q"][2].num(), (float)ev["pose"]["q"][3].num()},
                  {(float)ev["pose"]["p"][0].num(), (float)ev["pose"]["p"][1].num(), (float)ev["pose"]["p"][2].num()}};
        int valid = 0; float zmin = 1e9, zmax = 0; std::vector<float> zs;
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { float z = f.viewZ(x, y); if (z > 0) { valid++; zs.push_back(z); } }
        std::sort(zs.begin(), zs.end());
        printf("%s: %d/%d valid depth, z p5 %.2f p50 %.2f p95 %.2f m\n", names[e], valid, w * h, zs.empty() ? 0 : zs[zs.size() / 20], zs.empty() ? 0 : zs[zs.size() / 2], zs.empty() ? 0 : zs[zs.size() * 19 / 20]);
        Params P;
        for (int k = 2; k + 1 < argc; k += 2) { std::string n = argv[k]; float v = atof(argv[k + 1]);
            if (n == "inlierRel") P.inlierRel = v; else if (n == "inlier") P.inlier = v; else if (n == "normalDeg") P.normalDeg = v;
            else if (n == "curv") P.maxCurvature = v; else if (n == "minInliers") P.minInliers = (int)v; else if (n == "half") P.normalHalf = (int)v;
            else if (n == "edge") P.edgeRel = v; else if (n == "minArea") P.minArea = v; }
        Fuser fz(P); FrameFit ff = fz.add(f);
        walls[e] = fz.walls();
        printf("  %zu planes (%d grid points)\n", walls[e].size(), ff.validPoints);
        for (auto& wl : walls[e]) printf("   #%d %-7s n (%5.2f %5.2f %5.2f) d %6.3f c (%5.2f %5.2f %5.2f) %4.2f x %4.2f rms %4.1f mm %ld pts conf %.2f\n",
            wl.id, kindName(wl.kind), wl.n.x, wl.n.y, wl.n.z, wl.d, wl.centre.x, wl.centre.y, wl.centre.z, wl.w, wl.h, wl.rms_m * 1000, wl.samples, wl.confidence);
    }
    // left vs right: the same surfaces, seen 6 cm apart at the same instant
    int pairs = 0, wallPairs = 0; float worstAng = 0, worstOff = 0;
    for (auto& a : walls[0]) {
        float best = 1e9; const Wall* bb = nullptr;
        for (auto& b : walls[1]) { if (dot(a.n, b.n) < 0.94f) continue; float off = std::fabs(dot(b.n, a.centre) + b.d); if (off < best) { best = off; bb = &b; } }
        if (!bb || best > 0.15f) { printf("left #%d (%s, %.2fx%.2f): no right-eye counterpart\n", a.id, kindName(a.kind), a.w, a.h); continue; }
        float ang = angleDeg(a.n, bb->n); pairs++; worstAng = std::max(worstAng, ang); worstOff = std::max(worstOff, best);
        printf("left #%d ~ right #%d (%s): %.2f deg, %.1f mm apart\n", a.id, bb->id, kindName(a.kind), ang, best * 1000);
        if (a.kind == Kind::wall && bb->kind == Kind::wall && ang <= 2.0f && best <= 0.015f) wallPairs++;
    }
    printf("%d plane pairs across eyes, worst %.2f deg / %.1f mm; %d wall pairs within 2 deg / 15 mm\n", pairs, worstAng, worstOff * 1000, wallPairs);
    // as a test (default params): the real frame's wall, seen by both eyes, must agree
    return argc == 2 && wallPairs < 1 ? 1 : 0;
}
