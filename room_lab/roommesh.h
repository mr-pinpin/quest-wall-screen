// roommesh.h: a progressive room mesh from environment depth, like a room scan that grows and refines as you look
// around. Header-only C++17, no dependencies beyond wallfit.h (its DepthFrame, V3, Pose); bench/roommesh_test.cpp.
//
// Depth frames are fused into a sparse TSDF (truncated signed distance) volume: voxels of `voxel` metres in hashed
// blocks of 8x8x8, allocated only near observed surfaces. Each block records whether it changed; update() re-meshes
// changed blocks within a time budget (surface nets: one vertex per voxel cube the surface crosses, one quad per
// crossed grid edge; no lookup tables, about two triangles per surface cell, and crack-free across blocks because a
// cube's vertex depends only on its 8 corner values) and returns which blocks changed, so the renderer re-uploads only those. Walls stay wallfit's job; this mesh
// is the visual (wireframe glow while scanning, the GEOMETRY flag) and the geometry the house keeps (save()/load(),
// a blob for house.json rooms[].mesh). Feed poses in the frame you want the mesh in (the room frame for the house).
//
// Threading: integrate() and update() on one worker thread; copy the changed blocks' meshes out under your own lock.
#pragma once
#include "wallfit.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace roommesh {

using wallfit::DepthFrame;
using wallfit::V3;

constexpr int B = 8;  // voxels per block side

struct Key {
    int16_t x = 0, y = 0, z = 0;
    bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct KeyHash { size_t operator()(const Key& k) const { return ((size_t)(uint16_t)k.x * 73856093u) ^ ((size_t)(uint16_t)k.y * 19349663u) ^ ((size_t)(uint16_t)k.z * 83492791u); } };

struct BlockMesh {
    std::vector<float> pos, nrm;  // xyz per vertex
    std::vector<uint32_t> idx;    // triangles
    uint32_t version = 0;         // bumped at every re-mesh
    float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};  // bounds of the vertices (for culling and near-first ordering)
    size_t triangles() const { return idx.size() / 3; }
    V3 centre() const { return {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2}; }
    float radius() const { return 0.5f * std::sqrt((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) + (hi[2] - lo[2]) * (hi[2] - lo[2])); }
};

struct Block {
    int16_t tsdf[B * B * B];   // signed distance / trunc, scaled to +-32767 (+ = free space in front of a surface)
    uint8_t weight[B * B * B]; // 0 = never observed
    bool dirty = false;
    float drift = 0;          // largest change of any voxel since the block was last meshed (in tsdf units)
    BlockMesh mesh;
    Block() { std::fill(tsdf, tsdf + B * B * B, (int16_t)32767); std::fill(weight, weight + B * B * B, (uint8_t)0); }
};

struct Params {
    float voxel = 0.04f;        // metres
    float truncVoxels = 3;      // truncation band, in voxels
    float maxDepth = 4.0f;      // depth beyond this is not fused (too noisy)
    int pixelStride = 2;        // allocation samples every N pixels
    int maxWeight = 32;         // running average cap: newer frames keep refining
    size_t maxBlocks = 20000;   // memory cap (~30 MB); new blocks beyond it are refused (counted)
    float remeshDelta = 0.05f;  // re-mesh a block once its voxels moved this much (x trunc: 6 mm) or a new voxel appeared,
                                // so a well-seen area stops costing meshing while new areas appear at once
    int minWeight = 1;          // observations a voxel needs before it is meshed (1: the mesh appears from the first frame)
};

class Volume {
public:
    Params P;
    long refusedBlocks = 0, framesFused = 0;

    explicit Volume(Params p = {}) : P(p) {}
    size_t blocks() const { return blocks_.size(); }

    // Fuse one depth frame. Returns the number of blocks it touched.
    int integrate(const DepthFrame& f) {
        if (f.w <= 0 || f.h <= 0 || (!f.d16 && !f.d01)) return 0;
        // linear depth once per frame
        depth_.assign((size_t)f.w * f.h, 0.0f);
        for (int y = 0; y < f.h; y++) for (int x = 0; x < f.w; x++) { float z = f.viewZ(x, y); depth_[(size_t)y * f.w + x] = z > P.maxDepth ? 0 : z; }
        float trunc = P.truncVoxels * P.voxel, bs = B * P.voxel;
        // allocate blocks around each sampled surface point (the band in front of and behind it, along the ray)
        std::unordered_set<Key, KeyHash> touch;
        for (int y = P.pixelStride / 2; y < f.h; y += P.pixelStride)
            for (int x = P.pixelStride / 2; x < f.w; x += P.pixelStride) {
                float z = depth_[(size_t)y * f.w + x]; if (z <= 0) continue;
                V3 p = f.point((float)x, (float)y, z), d = wallfit::norm(p - f.pose.p);
                for (float s : {-trunc, 0.0f, trunc}) {
                    V3 q = p + d * s;
                    touch.insert({(int16_t)std::floor(q.x / bs), (int16_t)std::floor(q.y / bs), (int16_t)std::floor(q.z / bs)});
                }
            }
        // camera for projection: view-space position of voxel (vx,vy,vz) = v0 + ax*vx + ay*vy + az*vz
        wallfit::Quat qi = wallfit::conj(f.pose.q);
        V3 ax = wallfit::rotate(qi, {P.voxel, 0, 0}), ay = wallfit::rotate(qi, {0, P.voxel, 0}), az = wallfit::rotate(qi, {0, 0, P.voxel});
        float tl = std::tan(f.fov.left), tr = std::tan(f.fov.right), td = std::tan(f.fov.down), tu = std::tan(f.fov.up);
        float su = f.w / (tr - tl), sv = f.h / (tu - td);
        int touched = 0;
        for (const Key& k : touch) {
            auto it = blocks_.find(k);
            if (it == blocks_.end()) {
                if (blocks_.size() >= P.maxBlocks) { refusedBlocks++; continue; }
                it = blocks_.emplace(k, Block()).first;
            }
            Block& b = it->second; bool fresh = false; float drift = 0;
            V3 v0 = wallfit::rotate(qi, V3{(k.x * B + 0.5f) * P.voxel, (k.y * B + 0.5f) * P.voxel, (k.z * B + 0.5f) * P.voxel} - f.pose.p);
            for (int i = 0; i < B * B * B; i++) {
                int vx = i % B, vy = (i / B) % B, vz = i / (B * B);
                V3 v = v0 + ax * (float)vx + ay * (float)vy + az * (float)vz;
                if (v.z > -f.nearZ) continue;
                float zc = -v.z, iz = 1.0f / zc, u = (v.x * iz - tl) * su, t = (v.y * iz - td) * sv;
                if (!(u >= 0 && u < f.w && t >= 0 && t < f.h)) continue;
                float zm = depth_[(size_t)(int)t * f.w + (int)u]; if (zm <= 0) continue;
                float sdf = zm - zc; if (sdf < -trunc) continue;  // far behind the surface: not seen
                float tsdf = std::min(1.0f, sdf / trunc);
                int w = b.weight[i]; float old = b.tsdf[i] * (1.0f / 32767.0f);
                float nv = std::clamp((old * w + tsdf) / (w + 1), -1.0f, 1.0f);
                b.tsdf[i] = (int16_t)(nv * 32767.0f + (nv >= 0 ? 0.5f : -0.5f)); b.weight[i] = (uint8_t)std::min(w + 1, P.maxWeight);
                fresh |= w == 0 && std::fabs(nv) < 0.9f; drift = std::max(drift, std::fabs(nv - old) * (w > 0));
            }
            b.drift = std::max(b.drift, drift);
            if (fresh || b.drift > P.remeshDelta) { markDirty(k); touched++; }
        }
        framesFused++;
        return touched;
    }

    // Re-mesh dirty blocks for up to budgetMs (0 = all). Returns the blocks whose mesh changed (an emptied block is in
    // the list with an empty mesh). Blocks left dirty are done on the next call.
    std::vector<Key> update(double budgetMs = 0) {
        auto t0 = std::chrono::steady_clock::now(); std::vector<Key> changed;
        std::vector<Key> todo; for (auto& [k, b] : blocks_) if (b.dirty) todo.push_back(k);
        for (auto& k : todo) {
            if (budgetMs > 0 && std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() > budgetMs) break;
            Block& b = blocks_.at(k); b.dirty = false; b.drift = 0;
            size_t before = b.mesh.idx.size();
            meshBlock(k, b.mesh);
            if (b.mesh.idx.size() || before) { b.mesh.version++; changed.push_back(k); }
        }
        return changed;
    }
    size_t dirtyBlocks() const { size_t n = 0; for (auto& [k, b] : blocks_) n += b.dirty; return n; }

    const BlockMesh* mesh(const Key& k) const { auto it = blocks_.find(k); return it == blocks_.end() ? nullptr : &it->second.mesh; }
    template <class F> void forEachMesh(F f) const { for (auto& [k, b] : blocks_) if (!b.mesh.idx.empty()) f(k, b.mesh); }
    size_t triangles() const { size_t n = 0; for (auto& [k, b] : blocks_) n += b.mesh.triangles(); return n; }

    // The TSDF (not the triangles: they are rebuilt from it, and fusing can continue after a load), little-endian:
    // "PPRM" u16 version=1, f32 voxel, f32 trunc, u32 blocks, then per block i16 x,y,z + 512 x i16 tsdf + 512 x u8
    // weight (only blocks with any observation), then u32 CRC-32 of everything before it.
    std::string save() const {
        std::string o = "PPRM"; put16(o, 1); putf(o, P.voxel); putf(o, P.truncVoxels);
        uint32_t n = 0; for (auto& [k, b] : blocks_) n += observed(b);
        put32(o, n);
        for (auto& [k, b] : blocks_) {
            if (!observed(b)) continue;
            put16(o, (uint16_t)k.x); put16(o, (uint16_t)k.y); put16(o, (uint16_t)k.z);
            for (int i = 0; i < B * B * B; i++) put16(o, (uint16_t)b.tsdf[i]);
            o.append((const char*)b.weight, B * B * B);
        }
        put32(o, crc32(o)); return o;
    }
    // Replaces this volume with the blob's (every block dirty, so update() rebuilds the mesh). False (volume untouched)
    // on a bad magic, version, size or checksum.
    bool load(const std::string& s) {
        if (s.size() < 22 || s.compare(0, 4, "PPRM") != 0) return false;
        if (get32(s, s.size() - 4) != crc32(s.substr(0, s.size() - 4))) return false;
        size_t at = 4; if (get16(s, at) != 1) return false; at += 2;
        float voxel = getf(s, at), trunc = getf(s, at + 4); at += 8;
        uint32_t n = get32(s, at); at += 4;
        size_t per = 6 + B * B * B * 3;
        if (s.size() != at + (size_t)n * per + 4 || !(voxel > 0) || !(trunc > 0)) return false;
        std::unordered_map<Key, Block, KeyHash> nb; nb.reserve(n);
        for (uint32_t i = 0; i < n; i++, at += per) {
            Key k{(int16_t)get16(s, at), (int16_t)get16(s, at + 2), (int16_t)get16(s, at + 4)};
            Block& b = nb[k];
            for (int v = 0; v < B * B * B; v++) b.tsdf[v] = (int16_t)get16(s, at + 6 + v * 2);
            memcpy(b.weight, s.data() + at + 6 + B * B * B * 2, B * B * B); b.dirty = true;
        }
        blocks_.swap(nb); P.voxel = voxel; P.truncVoxels = trunc; return true;
    }

private:
    std::unordered_map<Key, Block, KeyHash> blocks_;
    std::vector<float> depth_;

    static bool observed(const Block& b) { for (int i = 0; i < B * B * B; i++) if (b.weight[i]) return true; return false; }
    // A block's cubes reach one voxel into its +x/+y/+z neighbours, so a change also re-meshes the -x/-y/-z neighbours.
    void markDirty(const Key& k) {
        for (int dz = -1; dz <= 0; dz++) for (int dy = -1; dy <= 0; dy++) for (int dx = -1; dx <= 0; dx++) {
            auto it = blocks_.find({(int16_t)(k.x + dx), (int16_t)(k.y + dy), (int16_t)(k.z + dz)});
            if (it != blocks_.end()) it->second.dirty = true;
        }
    }

    // The block and a one-voxel rim around it (indices -1..8 per axis), copied once so meshing does no hashing per
    // corner. NAN = not observed (or under minWeight).
    static constexpr int R = B + 2;
    void gather(const Key& k, float* c) const {
        const Block* nb[3][3][3];
        for (int dz = -1; dz <= 1; dz++) for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
            auto it = blocks_.find({(int16_t)(k.x + dx), (int16_t)(k.y + dy), (int16_t)(k.z + dz)});
            nb[dz + 1][dy + 1][dx + 1] = it == blocks_.end() ? nullptr : &it->second;
        }
        for (int z = -1; z <= B; z++) for (int y = -1; y <= B; y++) for (int x = -1; x <= B; x++) {
            int bx = x < 0 ? 0 : x >= B ? 2 : 1, by = y < 0 ? 0 : y >= B ? 2 : 1, bz = z < 0 ? 0 : z >= B ? 2 : 1;
            const Block* b = nb[bz][by][bx]; float v = NAN;
            if (b) { int i = (((z + B) % B) * B + ((y + B) % B)) * B + ((x + B) % B); if (b->weight[i] >= P.minWeight) v = b->tsdf[i] / 32767.0f; }
            c[((z + 1) * R + (y + 1)) * R + (x + 1)] = v;
        }
    }

    void meshBlock(const Key& k, BlockMesh& m) const {
        m.pos.clear(); m.nrm.clear(); m.idx.clear();
        float c[R * R * R]; gather(k, c);
        auto at = [&](int x, int y, int z) { return c[((z + 1) * R + (y + 1)) * R + (x + 1)]; };
        // cube (x,y,z) has corners x..x+1 etc., for x in -1..7; its vertex index, or -1
        constexpr int CR = B + 1; int vid[CR * CR * CR];
        auto cube = [&](int x, int y, int z) -> int& { return vid[((z + 1) * CR + (y + 1)) * CR + (x + 1)]; };
        static const int E[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (int z = -1; z < B; z++) for (int y = -1; y < B; y++) for (int x = -1; x < B; x++) {
            int& id = cube(x, y, z); id = -1;
            float f[8]; bool ok = true, neg = false, pos = false;
            for (int q = 0; q < 8 && ok; q++) { f[q] = at(x + (q & 1), y + (q >> 1 & 1), z + (q >> 2 & 1)); ok = !std::isnan(f[q]); if (ok) { neg |= f[q] < 0; pos |= f[q] >= 0; } }
            if (!ok || !neg || !pos) continue;
            float lo = *std::min_element(f, f + 8), hi = *std::max_element(f, f + 8);
            if (lo < -0.999f && hi > 0.999f) continue;  // a jump across the truncation band, not a surface
            V3 sum{0, 0, 0}; int n = 0;
            for (auto& e : E) {
                float a = f[e[0]], b = f[e[1]]; if ((a < 0) == (b < 0)) continue;
                float t = a / (a - b); V3 pa{(float)(e[0] & 1), (float)(e[0] >> 1 & 1), (float)(e[0] >> 2 & 1)}, pb{(float)(e[1] & 1), (float)(e[1] >> 1 & 1), (float)(e[1] >> 2 & 1)};
                sum = sum + pa + (pb - pa) * t; n++;
            }
            V3 p = sum * (1.0f / n);
            // gradient of the distance (toward free space): the vertex normal
            V3 g{(f[1] - f[0]) + (f[3] - f[2]) + (f[5] - f[4]) + (f[7] - f[6]), (f[2] - f[0]) + (f[3] - f[1]) + (f[6] - f[4]) + (f[7] - f[5]),
                 (f[4] - f[0]) + (f[5] - f[1]) + (f[6] - f[2]) + (f[7] - f[3])};
            g = wallfit::norm(g);
            float wx = (k.x * B + x + 0.5f + p.x) * P.voxel, wy = (k.y * B + y + 0.5f + p.y) * P.voxel, wz = (k.z * B + z + 0.5f + p.z) * P.voxel;
            id = (int)(m.pos.size() / 3);
            m.pos.insert(m.pos.end(), {wx, wy, wz}); m.nrm.insert(m.nrm.end(), {g.x, g.y, g.z});
        }
        // a quad for every crossed grid edge starting at a voxel of this block, between the 4 cubes around it
        for (int z = 0; z < B; z++) for (int y = 0; y < B; y++) for (int x = 0; x < B; x++) {
            float f0 = at(x, y, z); if (std::isnan(f0)) continue;
            for (int axis = 0; axis < 3; axis++) {
                int dx = axis == 0, dy = axis == 1, dz = axis == 2;
                float f1 = at(x + dx, y + dy, z + dz); if (std::isnan(f1) || (f0 < 0) == (f1 < 0)) continue;
                // the 4 cubes sharing this edge: offsets 0/-1 in the two other axes
                int q[4];
                if (axis == 0) { q[0] = cube(x, y, z); q[1] = cube(x, y - 1, z); q[2] = cube(x, y - 1, z - 1); q[3] = cube(x, y, z - 1); }
                else if (axis == 1) { q[0] = cube(x, y, z); q[1] = cube(x, y, z - 1); q[2] = cube(x - 1, y, z - 1); q[3] = cube(x - 1, y, z); }
                else { q[0] = cube(x, y, z); q[1] = cube(x - 1, y, z); q[2] = cube(x - 1, y - 1, z); q[3] = cube(x, y - 1, z); }
                if (q[0] < 0 || q[1] < 0 || q[2] < 0 || q[3] < 0) continue;
                // face the free side: the edge runs from f0 to f1; flip when the inside is at the start
                if (f0 >= 0) std::swap(q[1], q[3]);
                m.idx.insert(m.idx.end(), {(uint32_t)q[0], (uint32_t)q[1], (uint32_t)q[2], (uint32_t)q[0], (uint32_t)q[2], (uint32_t)q[3]});
            }
        }
        // drop vertices no quad uses (cubes on the rim)
        std::vector<int> remap(m.pos.size() / 3, -1); std::vector<float> P2, N2; P2.reserve(m.pos.size()); N2.reserve(m.nrm.size());
        for (auto& i : m.idx) {
            if (remap[i] < 0) { remap[i] = (int)(P2.size() / 3); P2.insert(P2.end(), m.pos.begin() + i * 3, m.pos.begin() + i * 3 + 3); N2.insert(N2.end(), m.nrm.begin() + i * 3, m.nrm.begin() + i * 3 + 3); }
            i = (uint32_t)remap[i];
        }
        m.pos.swap(P2); m.nrm.swap(N2);
        for (int j = 0; j < 3; j++) { m.lo[j] = 1e30f; m.hi[j] = -1e30f; }
        for (size_t v = 0; v < m.pos.size(); v += 3)
            for (int j = 0; j < 3; j++) { m.lo[j] = std::min(m.lo[j], m.pos[v + j]); m.hi[j] = std::max(m.hi[j], m.pos[v + j]); }
        if (m.pos.empty()) for (int j = 0; j < 3; j++) m.lo[j] = m.hi[j] = 0;
    }

    static void put16(std::string& o, uint16_t v) { o += (char)(v & 255); o += (char)(v >> 8); }
    static void put32(std::string& o, uint32_t v) { for (int i = 0; i < 4; i++) o += (char)((v >> (8 * i)) & 255); }
    static void putf(std::string& o, float f) { uint32_t v; memcpy(&v, &f, 4); put32(o, v); }
    static uint16_t get16(const std::string& s, size_t at) { return (uint16_t)((uint8_t)s[at] | (uint8_t)s[at + 1] << 8); }
    static uint32_t get32(const std::string& s, size_t at) { uint32_t v = 0; for (int i = 0; i < 4; i++) v |= (uint32_t)(uint8_t)s[at + i] << (8 * i); return v; }
    static float getf(const std::string& s, size_t at) { uint32_t v = get32(s, at); float f; memcpy(&f, &v, 4); return f; }
    static uint32_t crc32(const std::string& s) {
        uint32_t c = 0xffffffffu;
        for (unsigned char ch : s) { c ^= ch; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1))); }
        return ~c;
    }
};

}  // namespace roommesh
