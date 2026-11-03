// Minimal binary glTF 2.0 (GLB) reader for headset-rendered models (docs/3d-content.md): the first mesh's first
// triangle primitive (POSITION, NORMAL, TEXCOORD_0, indices), the node transform that places it, and the URI of
// its base-colour texture (a .ktx2 next to the model, streamed separately, coarse mips first). No GL here, so it
// is unit-tested on a desktop (bench/model_test.cpp).
#pragma once
#include "json.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace gltf {

struct Mesh {
    std::vector<float> pos, nrm, uv;   // 3, 3, 2 floats per vertex
    std::vector<uint32_t> idx;
    std::string baseColorUri;          // "" without a texture
    float min[3] = {0, 0, 0}, max[3] = {0, 0, 0};
    std::string error;
    size_t vertices() const { return pos.size() / 3; }
};

inline uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

// An accessor's elements as floats (FLOAT only) or as indices (UNSIGNED_BYTE/SHORT/INT). Bounds-checked.
inline bool readAccessor(const json::Value& doc, const uint8_t* bin, size_t binLen, int ai, int comps, std::vector<float>* f, std::vector<uint32_t>* u) {
    const json::Value& a = doc["accessors"][(size_t)ai];
    if (a.type != json::Value::Obj) return false;
    const json::Value& bv = doc["bufferViews"][(size_t)a["bufferView"].integer(-1)];
    if (bv.type != json::Value::Obj || bv["buffer"].integer(0) != 0) return false;
    int ct = a["componentType"].integer(), count = a["count"].integer(0);
    int csize = ct == 5126 || ct == 5125 ? 4 : ct == 5123 ? 2 : ct == 5121 ? 1 : 0;
    if (!csize || count <= 0) return false;
    size_t off = (size_t)bv["byteOffset"].integer(0) + (size_t)a["byteOffset"].integer(0);
    size_t stride = bv["byteStride"].integer(0) > 0 ? (size_t)bv["byteStride"].integer(0) : (size_t)csize * comps;
    size_t viewEnd = (size_t)bv["byteOffset"].integer(0) + (size_t)bv["byteLength"].integer(0);
    if (viewEnd > binLen || off + stride * (count - 1) + (size_t)csize * comps > viewEnd) return false;
    for (int i = 0; i < count; i++) {
        const uint8_t* p = bin + off + stride * i;
        for (int c = 0; c < comps; c++) {
            const uint8_t* q = p + c * csize;
            if (f) { if (ct != 5126) return false; float v; memcpy(&v, q, 4); f->push_back(v); }
            else { uint32_t v = csize == 4 ? rd32(q) : csize == 2 ? (uint32_t)(q[0] | (q[1] << 8)) : q[0]; u->push_back(v); }
        }
    }
    return true;
}

// Column-major 4x4 from a node's matrix or TRS.
inline void nodeMatrix(const json::Value& n, float m[16]) {
    for (int i = 0; i < 16; i++) m[i] = (i % 5 == 0) ? 1.f : 0.f;
    if (n["matrix"].size() == 16) { for (int i = 0; i < 16; i++) m[i] = (float)n["matrix"][(size_t)i].num(); return; }
    float t[3] = {0, 0, 0}, s[3] = {1, 1, 1}, q[4] = {0, 0, 0, 1};
    for (int i = 0; i < 3; i++) { if (n["translation"].size() == 3) t[i] = (float)n["translation"][(size_t)i].num(); if (n["scale"].size() == 3) s[i] = (float)n["scale"][(size_t)i].num(); }
    if (n["rotation"].size() == 4) for (int i = 0; i < 4; i++) q[i] = (float)n["rotation"][(size_t)i].num();
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float r[9] = {1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w),
                  2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
                  2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)};
    for (int c = 0; c < 3; c++) for (int rr = 0; rr < 3; rr++) m[c * 4 + rr] = r[c * 3 + rr] * s[c];
    m[12] = t[0]; m[13] = t[1]; m[14] = t[2];
}

inline Mesh load(const uint8_t* d, size_t n) {
    Mesh M;
    if (n < 20 || rd32(d) != 0x46546C67 || rd32(d + 4) != 2) { M.error = "not a GLB v2"; return M; }
    size_t total = std::min<size_t>(rd32(d + 8), n), p = 12;
    std::string js; const uint8_t* bin = nullptr; size_t binLen = 0;
    while (p + 8 <= total) {
        uint32_t len = rd32(d + p), type = rd32(d + p + 4);
        if (p + 8 + len > total) { M.error = "truncated chunk"; return M; }
        if (type == 0x4E4F534A) js.assign((const char*)d + p + 8, len);
        else if (type == 0x004E4942) { bin = d + p + 8; binLen = len; }
        p += 8 + ((len + 3) & ~3u);
    }
    bool ok = false; json::Value doc = json::parse(js, &ok);
    if (!ok) { M.error = "bad JSON chunk"; return M; }
    const json::Value& prim = doc["meshes"][0]["primitives"][0];
    if (prim.type != json::Value::Obj || prim["mode"].integer(4) != 4) { M.error = "no triangle primitive"; return M; }
    const json::Value& at = prim["attributes"];
    if (!readAccessor(doc, bin, binLen, at["POSITION"].integer(), 3, &M.pos, nullptr)) { M.error = "bad POSITION"; return M; }
    if (at.has("NORMAL") && !readAccessor(doc, bin, binLen, at["NORMAL"].integer(), 3, &M.nrm, nullptr)) { M.error = "bad NORMAL"; return M; }
    if (at.has("TEXCOORD_0") && !readAccessor(doc, bin, binLen, at["TEXCOORD_0"].integer(), 2, &M.uv, nullptr)) { M.error = "bad TEXCOORD_0"; return M; }
    if (prim.has("indices")) { if (!readAccessor(doc, bin, binLen, prim["indices"].integer(), 1, nullptr, &M.idx)) { M.error = "bad indices"; return M; } }
    else for (uint32_t i = 0; i < M.vertices(); i++) M.idx.push_back(i);
    size_t nv = M.vertices();
    if ((!M.nrm.empty() && M.nrm.size() != nv * 3) || (!M.uv.empty() && M.uv.size() != nv * 2)) { M.error = "attribute counts differ"; return M; }
    for (uint32_t i : M.idx) if (i >= nv) { M.error = "index out of range"; return M; }
    // The node that uses mesh 0 places it (one level: enough for single-mesh sample assets).
    for (size_t i = 0; i < doc["nodes"].size(); i++) if (doc["nodes"][i]["mesh"].integer(-1) == 0) {
        float m[16]; nodeMatrix(doc["nodes"][i], m);
        for (size_t v = 0; v < nv; v++) {
            float* P = &M.pos[v * 3]; float x = P[0], y = P[1], z = P[2];
            for (int r = 0; r < 3; r++) P[r] = m[r] * x + m[4 + r] * y + m[8 + r] * z + m[12 + r];
            if (!M.nrm.empty()) {
                float* N = &M.nrm[v * 3]; float a = N[0], b = N[1], c = N[2], o[3];
                for (int r = 0; r < 3; r++) o[r] = m[r] * a + m[4 + r] * b + m[8 + r] * c;
                float l = std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]); if (l > 0) for (int r = 0; r < 3; r++) N[r] = o[r] / l;
            }
        }
        break;
    }
    for (int c = 0; c < 3; c++) { M.min[c] = 1e30f; M.max[c] = -1e30f; }
    for (size_t v = 0; v < nv; v++) for (int c = 0; c < 3; c++) { M.min[c] = std::min(M.min[c], M.pos[v * 3 + c]); M.max[c] = std::max(M.max[c], M.pos[v * 3 + c]); }
    int tex = doc["materials"][(size_t)prim["material"].integer(0)]["pbrMetallicRoughness"]["baseColorTexture"]["index"].integer(-1);
    if (tex >= 0) {
        const json::Value& t = doc["textures"][(size_t)tex];
        int src = t["extensions"]["KHR_texture_basisu"]["source"].integer(t["source"].integer(-1));
        if (src >= 0) M.baseColorUri = doc["images"][(size_t)src]["uri"].str();
    }
    return M;
}

}  // namespace gltf
