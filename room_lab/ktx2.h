// KTX2 textures, read progressively (docs/3d-content.md). KTX2 stores mip levels smallest first, so a plain
// sequential download makes the coarse levels usable as soon as their bytes have arrived: the headset shows the
// coarsest level at once and swaps in finer ones as they complete (the screens' "hold until fine" rule). Only
// uncompressed / GPU-native formats (RGBA8, ETC2, ASTC 4x4/6x6/8x8 sRGB) without supercompression are accepted
// (no Basis transcoder on the headset yet; ASTC is native on the Quest's Adreno).
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ktx2 {

static const uint8_t kId[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};
enum : uint32_t { VK_R8G8B8A8_UNORM = 37, VK_R8G8B8A8_SRGB = 43, VK_ETC2_R8G8B8_SRGB = 148, VK_ETC2_R8G8B8A8_SRGB = 152,
                  VK_ASTC_4x4_SRGB = 158, VK_ASTC_6x6_SRGB = 166, VK_ASTC_8x8_SRGB = 172 };

struct Level { uint64_t offset = 0, length = 0; };
struct Header {
    uint32_t vkFormat = 0, width = 0, height = 0, levels = 0, scheme = 0;
    std::vector<Level> level;  // level[0] is full size
    size_t headerBytes = 0;    // bytes needed before any level can be read
    std::string error;
};

inline uint32_t u32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
inline uint64_t u64(const uint8_t* p) { return u32(p) | ((uint64_t)u32(p + 4) << 32); }

// Parses the header from a prefix of the file. Returns false (with error empty) while more bytes are needed.
inline bool parse(const uint8_t* d, size_t n, Header& h) {
    h = Header{};
    if (n < 80) return false;
    if (memcmp(d, kId, 12)) { h.error = "not KTX2"; return false; }
    h.vkFormat = u32(d + 12); h.width = u32(d + 20); h.height = u32(d + 24);
    uint32_t depth = u32(d + 28), layers = u32(d + 32), faces = u32(d + 36);
    h.levels = std::max(1u, u32(d + 40)); h.scheme = u32(d + 44);
    if (depth > 1 || layers > 1 || faces != 1) { h.error = "only 2D textures"; return false; }
    if (h.scheme != 0) { h.error = "supercompressed KTX2 needs a transcoder"; return false; }
    if (h.levels > 16 || !h.width || !h.height) { h.error = "bad dimensions"; return false; }
    h.headerBytes = 80 + 24 * (size_t)h.levels;
    if (n < h.headerBytes) return false;
    for (uint32_t i = 0; i < h.levels; i++) h.level.push_back({u64(d + 80 + 24 * i), u64(d + 80 + 24 * i + 8)});
    return true;
}

// The finest level i such that levels i..levels-1 are all complete within the first `received` bytes; -1 if none.
inline int finestComplete(const Header& h, uint64_t received) {
    int best = -1;
    for (int i = (int)h.levels - 1; i >= 0; i--) {
        const Level& L = h.level[i];
        if (L.offset + L.length > received) break;
        best = i;
    }
    return best;
}

// ASTC block edge in texels (1 otherwise), for byte-size checks.
inline uint32_t blockSize(uint32_t f) { return f == VK_ASTC_4x4_SRGB ? 4 : f == VK_ASTC_6x6_SRGB ? 6 : f == VK_ASTC_8x8_SRGB ? 8 : 1; }
inline uint32_t levelWidth(const Header& h, int i) { return std::max(1u, h.width >> i); }
inline uint32_t levelHeight(const Header& h, int i) { return std::max(1u, h.height >> i); }
inline bool supported(uint32_t f) { return f == VK_R8G8B8A8_UNORM || f == VK_R8G8B8A8_SRGB || f == VK_ETC2_R8G8B8_SRGB || f == VK_ETC2_R8G8B8A8_SRGB || blockSize(f) > 1; }

}  // namespace ktx2
