// roomplanes.h: the fused room mesh (roommesh.h) regularised by the room's planes (wallfit.h): walls, floor and ceiling
// become a few large quads, flat and exactly on their plane (stable places for screens); only what is not planar
// (furniture, objects) keeps the full triangle tessellation. Header-only, GL-free; bench/roomplanes_test.cpp.
//
//   1. Each fused triangle whose three vertices lie within snapDist of a plane, facing the same way (within normalDeg),
//      belongs to that plane; the others are objects.
//   2. A plane's triangles are rasterised into a grid of `cell` metres on the plane (cell centres inside a triangle),
//      small notches and holes closed (morphological closing), and the grid merged greedily into the fewest
//      axis-aligned rectangles: one quad (two triangles) each, corners exactly on the plane.
//   3. Object triangles keep their tessellation; their vertices within snapDist of a plane snap onto it, so furniture
//      meets the floor and the walls without a seam.
// Unobserved parts stay holes (nothing is invented).
#pragma once
#include "roommesh.h"
#include "wallfit.h"
#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace roomplanes {

using wallfit::V3;

struct Params {
    float snapDist = 0.015f;   // vertex-to-plane distance for planar triangles and for snapping, at least; a noisier
    float snapSigma = 2.0f;    // plane widens it to snapSigma x its fit rms (real Quest depth: rms 25-40 mm on a
    float snapMax = 0.05f;     // single look), capped at snapMax so furniture near a wall is not flattened into it
    float normalDeg = 20;      // triangle normal vs plane normal
    float cell = 0.05f;        // plane grid (m)
    int close = 1;             // closing iterations (fills notches and holes up to ~2 cells)
    int minCells = 6;          // a plane needs this many occupied cells to become a patch
};

struct Patch {
    int plane = 0; V3 n; std::vector<std::array<V3, 4>> quads;  // corners counter-clockwise seen from the free side
    float area = 0; int cells = 0;
};

struct Result {
    std::vector<Patch> patches;
    std::vector<float> pos, nrm; std::vector<uint32_t> idx;  // object triangles (tessellated)
    size_t planarTriangles = 0, objectTriangles = 0, snappedVertices = 0;
    size_t quads() const { size_t n = 0; for (auto& p : patches) n += p.quads.size(); return n; }
    size_t triangles() const { return quads() * 2 + idx.size() / 3; }
    float planarArea() const { float a = 0; for (auto& p : patches) a += p.area; return a; }
    // One buffer for drawing: patches first (planar[t] = 1 for their triangles), then objects.
    void toMesh(std::vector<float>& P, std::vector<float>& N, std::vector<uint32_t>& I, std::vector<uint8_t>& planar) const {
        P.clear(); N.clear(); I.clear(); planar.clear();
        for (auto& pt : patches)
            for (auto& q : pt.quads) {
                uint32_t b = (uint32_t)(P.size() / 3);
                for (auto& c : q) { P.insert(P.end(), {c.x, c.y, c.z}); N.insert(N.end(), {pt.n.x, pt.n.y, pt.n.z}); }
                I.insert(I.end(), {b, b + 1, b + 2, b, b + 2, b + 3}); planar.insert(planar.end(), {1, 1});
            }
        uint32_t b = (uint32_t)(P.size() / 3);
        P.insert(P.end(), pos.begin(), pos.end()); N.insert(N.end(), nrm.begin(), nrm.end());
        for (auto i : idx) I.push_back(b + i);
        planar.insert(planar.end(), idx.size() / 3, 0);
    }
};

inline Result regularize(const roommesh::Volume& vol, const std::vector<wallfit::Wall>& planes, const Params& P = {}) {
    Result R;
    float cosN = std::cos(P.normalDeg / 57.29578f);
    std::vector<float> snap(planes.size());
    for (size_t k = 0; k < planes.size(); k++) snap[k] = std::clamp(P.snapSigma * planes[k].rms_m, P.snapDist, std::max(P.snapDist, P.snapMax));
    struct Grid { std::unordered_map<long long, uint8_t> cells; };
    std::vector<Grid> grids(planes.size());
    auto key = [](long long x, long long y) { return wallfit::Fuser::key(x, y); };  // unsigned packing, no UB
    auto dist = [&](size_t k, V3 p) { return wallfit::dot(planes[k].n, p) + planes[k].d; };
    auto to2 = [&](size_t k, V3 p) { V3 v = p - planes[k].centre; return std::pair<float, float>{wallfit::dot(v, planes[k].right), wallfit::dot(v, planes[k].up)}; };
    vol.forEachMesh([&](const roommesh::Key&, const roommesh::BlockMesh& m) {
        std::vector<int> remap(m.pos.size() / 3, -1);
        auto V = [&](uint32_t i) { return V3{m.pos[i * 3], m.pos[i * 3 + 1], m.pos[i * 3 + 2]}; };
        for (size_t t = 0; t < m.idx.size(); t += 3) {
            V3 a = V(m.idx[t]), b = V(m.idx[t + 1]), c = V(m.idx[t + 2]);
            V3 fn = wallfit::norm(wallfit::cross(b - a, c - a));
            int best = -1; float bestD = 1e9f;
            for (size_t k = 0; k < planes.size(); k++) {
                float da = std::fabs(dist(k, a)), db = std::fabs(dist(k, b)), dc = std::fabs(dist(k, c)), mx = std::max({da, db, dc});
                if (mx < snap[k] && wallfit::dot(fn, planes[k].n) > cosN && mx / snap[k] < bestD) { bestD = mx / snap[k]; best = (int)k; }
            }
            if (best >= 0) {  // rasterise into the plane's grid: cells whose centres fall inside the triangle, plus its corners
                R.planarTriangles++;
                auto A = to2(best, a), B2 = to2(best, b), C = to2(best, c);
                float x0 = std::min({A.first, B2.first, C.first}), x1 = std::max({A.first, B2.first, C.first});
                float y0 = std::min({A.second, B2.second, C.second}), y1 = std::max({A.second, B2.second, C.second});
                auto& g = grids[best].cells;
                for (auto& q : {A, B2, C}) g[key((long long)std::floor(q.first / P.cell), (long long)std::floor(q.second / P.cell))] = 1;
                float den = (B2.second - C.second) * (A.first - C.first) + (C.first - B2.first) * (A.second - C.second);
                if (std::fabs(den) < 1e-12f) continue;
                for (long long cy = (long long)std::floor(y0 / P.cell); cy * P.cell <= y1; cy++)
                    for (long long cx = (long long)std::floor(x0 / P.cell); cx * P.cell <= x1; cx++) {
                        float px = (cx + 0.5f) * P.cell, py = (cy + 0.5f) * P.cell;
                        float l1 = ((B2.second - C.second) * (px - C.first) + (C.first - B2.first) * (py - C.second)) / den;
                        float l2 = ((C.second - A.second) * (px - C.first) + (A.first - C.first) * (py - C.second)) / den;
                        if (l1 >= 0 && l2 >= 0 && l1 + l2 <= 1) g[key(cx, cy)] = 1;
                    }
                continue;
            }
            // an object triangle: keep it, snapping vertices that sit on a plane
            R.objectTriangles++;
            for (int j = 0; j < 3; j++) {
                uint32_t vi = m.idx[t + j];
                if (remap[vi] < 0) {
                    V3 p = V(vi), n{m.nrm[vi * 3], m.nrm[vi * 3 + 1], m.nrm[vi * 3 + 2]};
                    int sk = -1; float sd = 1;
                    for (size_t k = 0; k < planes.size(); k++) { float d = std::fabs(dist(k, p)) / snap[k]; if (d < sd) { sd = d; sk = (int)k; } }
                    if (sk >= 0) { p = p - planes[sk].n * dist(sk, p); R.snappedVertices++; }
                    remap[vi] = (int)(R.pos.size() / 3);
                    R.pos.insert(R.pos.end(), {p.x, p.y, p.z}); R.nrm.insert(R.nrm.end(), {n.x, n.y, n.z});
                }
                R.idx.push_back((uint32_t)remap[vi]);
            }
        }
    });
    // per plane: dense grid, closing, greedy rectangles
    for (size_t k = 0; k < planes.size(); k++) {
        auto& cells = grids[k].cells; if ((int)cells.size() < P.minCells) continue;
        long long X0 = LLONG_MAX, X1 = LLONG_MIN, Y0 = LLONG_MAX, Y1 = LLONG_MIN;
        for (auto& [kk, v] : cells) { long long x = wallfit::Fuser::keyX(kk), y = wallfit::Fuser::keyY(kk); X0 = std::min(X0, x); X1 = std::max(X1, x); Y0 = std::min(Y0, y); Y1 = std::max(Y1, y); }
        int pad = P.close, W = (int)(X1 - X0 + 1 + 2 * pad), H = (int)(Y1 - Y0 + 1 + 2 * pad);
        if ((long long)W * H > 4000000) continue;  // absurd extent: refuse rather than allocate
        std::vector<uint8_t> g(W * H, 0), t(W * H, 0);
        for (auto& [kk, v] : cells) { long long x = wallfit::Fuser::keyX(kk), y = wallfit::Fuser::keyY(kk); g[(y - Y0 + pad) * W + (x - X0 + pad)] = 1; }
        for (int it = 0; it < P.close; it++) {  // dilate then erode (3x3)
            for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { uint8_t v = 0; for (int b = -1; b <= 1 && !v; b++) for (int a = -1; a <= 1 && !v; a++) { int xx = x + a, yy = y + b; v = xx >= 0 && yy >= 0 && xx < W && yy < H && g[yy * W + xx]; } t[y * W + x] = v; }
            for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { uint8_t v = 1; for (int b = -1; b <= 1 && v; b++) for (int a = -1; a <= 1 && v; a++) { int xx = x + a, yy = y + b; v = xx >= 0 && yy >= 0 && xx < W && yy < H && t[yy * W + xx]; } g[y * W + x] = v; }
        }
        Patch pt; pt.plane = (int)k; pt.n = planes[k].n;
        std::vector<uint8_t> used(W * H, 0);
        const wallfit::Wall& pl = planes[k];
        auto corner = [&](float x, float y) { V3 c = pl.centre + pl.right * x + pl.up * y; return c - pl.n * (wallfit::dot(pl.n, c) + pl.d); };
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            if (!g[y * W + x] || used[y * W + x]) continue;
            int w = 1; while (x + w < W && g[y * W + x + w] && !used[y * W + x + w]) w++;
            int h = 1;
            for (;; h++) {
                if (y + h >= H) break;
                bool row = true; for (int a = 0; a < w && row; a++) row = g[(y + h) * W + x + a] && !used[(y + h) * W + x + a];
                if (!row) break;
            }
            for (int b = 0; b < h; b++) for (int a = 0; a < w; a++) used[(y + b) * W + x + a] = 1;
            float fx0 = (x + X0 - pad) * P.cell, fy0 = (y + Y0 - pad) * P.cell, fx1 = fx0 + w * P.cell, fy1 = fy0 + h * P.cell;
            pt.quads.push_back({corner(fx0, fy0), corner(fx1, fy0), corner(fx1, fy1), corner(fx0, fy1)});  // CCW seen from +n (right x up = n)
            pt.area += w * h * P.cell * P.cell; pt.cells += w * h;
        }
        R.patches.push_back(std::move(pt));
    }
    return R;
}

}  // namespace roomplanes
