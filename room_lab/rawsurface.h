// rawsurface.h: the current environment-depth frame as a mesh, for instant feedback of what the sensor sees (the
// fused room mesh, roommesh.h, takes several frames to settle). A screen-space grid (every `step` pixels), depth
// smoothed by an edge-aware bilateral filter (neighbours across a depth jump do not blur in), triangles cut at depth
// discontinuities and at grazing angles (where depth is least reliable). Header-only, GL-free; bench/rawsurface_test.cpp.
#pragma once
#include "wallfit.h"
#include <cmath>
#include <cstdint>
#include <vector>

namespace rawsurface {

using wallfit::DepthFrame;
using wallfit::V3;

struct Params {
    int step = 2;            // grid spacing in depth pixels (256^2 depth -> 128^2 vertices)
    int radius = 2;          // bilateral window radius, in grid cells
    float sigmaS = 1.5f;     // spatial sigma, in grid cells
    float sigmaR = 0.02f;    // range sigma, as a fraction of depth (2 cm at 1 m): neighbours further than ~3 sigma barely count
    float cutRel = 0.04f;    // a triangle edge whose depths differ by more than this fraction (+ cutAbs) is cut
    float cutAbs = 0.02f;
    float grazingCos = 0.17f;// triangles seen at more than ~80 degrees from their normal are dropped
    float maxDepth = 5.0f;
};

struct Mesh {
    std::vector<float> pos, nrm;  // xyz per vertex (world), normals toward the eye
    std::vector<uint32_t> idx;
    int gw = 0, gh = 0;           // grid size
    size_t triangles() const { return idx.size() / 3; }
};

inline Mesh build(const DepthFrame& f, const Params& P = {}) {
    Mesh m;
    if (f.w <= 0 || f.h <= 0 || (!f.d16 && !f.d01) || P.step < 1 || !f.sane()) return m;
    int gw = f.w / P.step, gh = f.h / P.step; m.gw = gw; m.gh = gh;
    std::vector<float> z(gw * gh, 0), zs(gw * gh, 0);
    for (int j = 0; j < gh; j++) for (int i = 0; i < gw; i++) {
        float d = f.viewZ(i * P.step + P.step / 2, j * P.step + P.step / 2); z[j * gw + i] = d > P.maxDepth ? 0 : d;
    }
    // bilateral: spatial gaussian x range gaussian on the depth difference (relative), invalid neighbours skipped
    float is2 = 1.0f / (2 * P.sigmaS * P.sigmaS);
    for (int j = 0; j < gh; j++) for (int i = 0; i < gw; i++) {
        float c = z[j * gw + i]; if (c <= 0) continue;
        float sr = P.sigmaR * c, ir2 = 1.0f / (2 * sr * sr), sw = 0, sz = 0;
        for (int b = -P.radius; b <= P.radius; b++) for (int a = -P.radius; a <= P.radius; a++) {
            int x = i + a, y = j + b; if (x < 0 || y < 0 || x >= gw || y >= gh) continue;
            float v = z[y * gw + x]; if (v <= 0) continue;
            float dz = v - c, w = std::exp(-(a * a + b * b) * is2 - dz * dz * ir2);
            sw += w; sz += w * v;
        }
        zs[j * gw + i] = sz / sw;
    }
    std::vector<int> vid(gw * gh, -1);
    for (int j = 0; j < gh; j++) for (int i = 0; i < gw; i++) {
        float d = zs[j * gw + i]; if (d <= 0) continue;
        V3 p = f.point((float)(i * P.step + P.step / 2), (float)(j * P.step + P.step / 2), d);
        vid[j * gw + i] = (int)(m.pos.size() / 3); m.pos.insert(m.pos.end(), {p.x, p.y, p.z});
    }
    m.nrm.assign(m.pos.size(), 0.0f);
    auto P3 = [&](int v) { return V3{m.pos[v * 3], m.pos[v * 3 + 1], m.pos[v * 3 + 2]}; };
    auto joined = [&](float a, float b) { return std::fabs(a - b) <= P.cutRel * std::min(a, b) + P.cutAbs; };
    auto tri = [&](int a, int b, int c) {
        V3 pa = P3(a), pb = P3(b), pc = P3(c), n = wallfit::cross(pb - pa, pc - pa);
        float ln = wallfit::len(n); if (ln < 1e-12f) return;
        V3 toEye = wallfit::norm(f.pose.p - (pa + pb + pc) * (1.0f / 3));
        float cosv = wallfit::dot(n, toEye) / ln;
        if (std::fabs(cosv) < P.grazingCos) return;      // grazing: unreliable
        if (cosv < 0) { std::swap(b, c); n = n * -1.0f; } // face the eye
        m.idx.insert(m.idx.end(), {(uint32_t)a, (uint32_t)b, (uint32_t)c});
        for (int v : {a, b, c}) { m.nrm[v * 3] += n.x; m.nrm[v * 3 + 1] += n.y; m.nrm[v * 3 + 2] += n.z; }
    };
    for (int j = 0; j + 1 < gh; j++) for (int i = 0; i + 1 < gw; i++) {
        int a = vid[j * gw + i], b = vid[j * gw + i + 1], c = vid[(j + 1) * gw + i], d = vid[(j + 1) * gw + i + 1];
        float za = zs[j * gw + i], zb = zs[j * gw + i + 1], zc = zs[(j + 1) * gw + i], zd = zs[(j + 1) * gw + i + 1];
        if (a >= 0 && b >= 0 && c >= 0 && joined(za, zb) && joined(za, zc) && joined(zb, zc)) tri(a, b, c);
        if (b >= 0 && d >= 0 && c >= 0 && joined(zb, zd) && joined(zd, zc) && joined(zb, zc)) tri(b, d, c);
    }
    for (size_t v = 0; v < m.nrm.size(); v += 3) { V3 n = wallfit::norm({m.nrm[v], m.nrm[v + 1], m.nrm[v + 2]}); m.nrm[v] = n.x; m.nrm[v + 1] = n.y; m.nrm[v + 2] = n.z; }
    return m;
}

}  // namespace rawsurface
