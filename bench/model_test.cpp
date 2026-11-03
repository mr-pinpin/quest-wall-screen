// Offline tests for headset-rendered models (docs/3d-content.md): JSON, GLB mesh loading, progressive KTX2 levels,
// and the asset download budget against the BUDGET arbiter.
//   g++ -std=c++17 -I room_lab bench/model_test.cpp -o /tmp/model_test && /tmp/model_test [ASSETDIR]
// ASSETDIR (optional): a folder from assets/make_asset.py (model.glb + basecolor.ktx2) to check a real model too.
#include "gltf.h"
#include "ktx2.h"
#include "assets.h"
#include "budget.h"
#include <cstdio>
#include <fstream>
#include <iterator>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; i++) v.push_back(uint8_t(x >> (8 * i))); }
static void put64(std::vector<uint8_t>& v, uint64_t x) { put32(v, (uint32_t)x); put32(v, (uint32_t)(x >> 32)); }
static std::vector<uint8_t> readAll(const std::string& p) { std::ifstream f(p, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }

// A one-triangle GLB with a node rotation of 90 degrees about X, uint16 indices and a texture URI.
static std::vector<uint8_t> tinyGlb() {
    std::vector<uint8_t> bin;
    float pos[] = {0, 0, 0, 1, 0, 0, 0, 1, 0}, nrm[] = {0, 0, 1, 0, 0, 1, 0, 0, 1}, uv[] = {0, 0, 1, 0, 0, 1};
    auto addf = [&](const float* f, int n) { for (int i = 0; i < n; i++) { uint32_t u; memcpy(&u, &f[i], 4); put32(bin, u); } };
    addf(pos, 9); addf(nrm, 9); addf(uv, 6);
    uint16_t idx[] = {0, 1, 2, 0}; for (auto i : idx) { bin.push_back(uint8_t(i)); bin.push_back(uint8_t(i >> 8)); }
    std::string js = R"({"asset":{"version":"2.0"},"nodes":[{"mesh":0,"rotation":[0.7071068,0,0,0.7071068]}],"meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"indices":3,"material":0}]}],)"
                     R"("materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}],"textures":[{"source":0}],"images":[{"uri":"basecolor.ktx2"}],)"
                     R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"},)"
                     R"({"bufferView":2,"componentType":5126,"count":3,"type":"VEC2"},{"bufferView":3,"componentType":5123,"count":3,"type":"SCALAR"}],)"
                     R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":36},{"buffer":0,"byteOffset":72,"byteLength":24},{"buffer":0,"byteOffset":96,"byteLength":8}],"buffers":[{"byteLength":104}]})";
    while (js.size() % 4) js += ' ';
    std::vector<uint8_t> g; put32(g, 0x46546C67); put32(g, 2); put32(g, (uint32_t)(12 + 8 + js.size() + 8 + bin.size()));
    put32(g, (uint32_t)js.size()); put32(g, 0x4E4F534A); g.insert(g.end(), js.begin(), js.end());
    put32(g, (uint32_t)bin.size()); put32(g, 0x004E4942); g.insert(g.end(), bin.begin(), bin.end());
    return g;
}

// A KTX2 header + level index for an RGBA8 8x8 texture (4 levels) with data smallest first.
static std::vector<uint8_t> tinyKtx(std::vector<ktx2::Level>& lv) {
    std::vector<uint8_t> k(ktx2::kId, ktx2::kId + 12);
    for (uint32_t v : {43u, 1u, 8u, 8u, 0u, 0u, 1u, 4u, 0u}) put32(k, v);
    for (int i = 0; i < 4; i++) { put32(k, 0); }
    put64(k, 0); put64(k, 0);
    uint64_t off = 80 + 24 * 4; lv.assign(4, {});
    for (int i = 3; i >= 0; i--) { uint64_t len = (uint64_t)(8 >> i) * (8 >> i) * 4; lv[i] = {off, len}; off += len; }
    for (int i = 0; i < 4; i++) { put64(k, lv[i].offset); put64(k, lv[i].length); put64(k, lv[i].length); }
    k.resize(off, 0x7f);
    return k;
}

int main(int argc, char** argv) {
    // JSON.
    { bool ok; json::Value v = json::parse(R"({"a":[1,2.5,-3e2],"s":"x\"yé","t":true,"n":null,"o":{}})", &ok);
      CHECK(ok && v["a"].size() == 3 && v["a"][2].num() == -300 && v["s"].str() == "x\"y\xc3\xa9" && v["t"].b && v["o"].type == json::Value::Obj, "json values");
      json::parse("{\"a\":[1,2", &ok); CHECK(!ok, "truncated json rejected");
      json::parse("{\"a\":1} x", &ok); CHECK(!ok, "trailing garbage rejected");
      std::string deep(100, '['); json::parse(deep, &ok); CHECK(!ok, "depth limit"); }

    // GLB.
    { auto g = tinyGlb(); gltf::Mesh m = gltf::load(g.data(), g.size());
      CHECK(m.error.empty(), "tiny glb loads: %s", m.error.c_str());
      CHECK(m.vertices() == 3 && m.idx.size() == 3 && m.uv.size() == 6 && m.baseColorUri == "basecolor.ktx2", "mesh contents");
      CHECK(std::fabs(m.pos[7] - 0) < 1e-5 && std::fabs(m.pos[8] - 1) < 1e-5, "node rotation applied: vertex 2 (0,1,0) -> (0,0,1), got (%g,%g,%g)", m.pos[6], m.pos[7], m.pos[8]);
      CHECK(std::fabs(m.nrm[1] + 1) < 1e-5, "normals rotated too: (0,0,1) -> (0,-1,0), got %g", m.nrm[1]);
      for (size_t cut : {size_t(10), size_t(30), g.size() - 9}) { gltf::Mesh t = gltf::load(g.data(), cut); CHECK(!t.error.empty(), "truncated glb at %zu rejected", cut); }
      auto bad = g; bad[g.size() - 8] = 9;  // index 0 -> 9
      CHECK(!gltf::load(bad.data(), bad.size()).error.empty(), "out-of-range index rejected"); }

    // KTX2: levels complete smallest first.
    { std::vector<ktx2::Level> lv; auto k = tinyKtx(lv); ktx2::Header h;
      CHECK(!ktx2::parse(k.data(), 79, h) && h.error.empty(), "short prefix: need more bytes");
      CHECK(ktx2::parse(k.data(), k.size(), h) && h.levels == 4 && h.width == 8 && ktx2::supported(h.vkFormat), "header");
      CHECK(ktx2::finestComplete(h, h.headerBytes) == -1, "no level before data");
      CHECK(ktx2::finestComplete(h, lv[3].offset + lv[3].length) == 3, "1x1 first");
      CHECK(ktx2::finestComplete(h, lv[1].offset + lv[1].length - 1) == 2, "level 1 incomplete -> 2");
      CHECK(ktx2::finestComplete(h, k.size()) == 0, "all levels");
      auto bad = k; bad[0] = 0; CHECK(!ktx2::parse(bad.data(), bad.size(), h) && !h.error.empty(), "bad identifier");
      auto sc = k; sc[44] = 2; CHECK(!ktx2::parse(sc.data(), sc.size(), h) && h.error.find("transcoder") != std::string::npos, "supercompression refused"); }

    // Budget accounting: the bucket never exceeds its rate plus one burst; paused means zero.
    { assets::Bucket b; b.kbps = 8000;  // 1 MB/s
      uint64_t got = 0; for (int i = 0; i <= 2000; i++) { uint64_t a = b.allow(i * 0.001); b.take(a); got += a; }
      CHECK(got <= 1000000 * 2 + 250000 && got >= 1900000, "8 Mbit/s for 2 s gives ~2 MB, got %llu", (unsigned long long)got);
      b.kbps = 0; CHECK(b.allow(3.0) == 0, "paused bucket gives nothing"); }

    // Through the arbiter: assets get bits in cinema/wall, nothing during a game.
    { budget::Arbiter a; budget::Input in; in.screens = {{2, true, 3000, 1.0f}};
      budget::Output o = a.split(in); CHECK(o.assetKbps > 0, "cinema: assets flow (%d kbps)", o.assetKbps);
      in.gameActive = true; o = a.split(in); CHECK(o.assetKbps == 0, "game: assets paused"); }

    if (argc > 1) {
        std::string d = argv[1];
        auto g = readAll(d + "/model.glb"); gltf::Mesh m = gltf::load(g.data(), g.size());
        CHECK(m.error.empty() && m.vertices() > 1000 && !m.uv.empty() && m.baseColorUri == "basecolor.ktx2", "real model: %zu vertices, %s", m.vertices(), m.error.c_str());
        printf("model: %zu vertices, %zu triangles, bounds %.2f..%.2f x %.2f..%.2f x %.2f..%.2f\n", m.vertices(), m.idx.size() / 3, m.min[0], m.max[0], m.min[1], m.max[1], m.min[2], m.max[2]);
        auto k = readAll(d + "/basecolor.ktx2"); ktx2::Header h;
        CHECK(ktx2::parse(k.data(), k.size(), h) && ktx2::finestComplete(h, k.size()) == 0, "real ktx2: all %u levels present", h.levels);
        for (uint32_t i = 0; i + 1 < h.levels; i++) CHECK(h.level[i].offset > h.level[i + 1].offset, "level %u stored after level %u (smallest first)", i, i + 1);
        // Time to first pixels and to full detail at the cinema-mode asset share.
        budget::Arbiter a; budget::Input in; in.screens = {{2, true, 3000, 1.0f}}; int kbps = a.split(in).assetKbps;
        double bps = kbps * 125.0, first = (h.level[h.levels - 1].offset + h.level[h.levels - 1].length + g.size()) / bps, full = (k.size() + g.size()) / bps;
        int coarse64 = 0; for (uint32_t i = 0; i < h.levels; i++) if (ktx2::levelWidth(h, i) <= 64) { coarse64 = (int)i; break; }
        double at64 = (h.level[coarse64].offset + h.level[coarse64].length + g.size()) / bps;
        printf("at %d kbps: textured (1x1) after %.0f ms, 64 px after %.0f ms, full %ux%u after %.0f ms\n", kbps, first * 1000, at64 * 1000, h.width, h.height, full * 1000);
        CHECK(at64 < 0.5, "a 64 px texture within 0.5 s at the cinema asset share");
    }
    printf("%d failures\n", fails);
    return fails ? 1 : 0;
}
