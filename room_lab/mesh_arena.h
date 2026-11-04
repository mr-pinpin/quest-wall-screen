// Room mesh draw batching (flag MESHBATCH; work-df 2026-11-04: "hundreds of draws per eye with GEOMETRY on"). Every
// mesh block lives in ONE vertex buffer, sub-allocated here: a block keeps its range while it fits (an update rewrites
// just that range), a freed range is zero-filled (zero vertices = degenerate triangles, nothing drawn), and the blocks
// to draw this frame merge into runs: consecutive in the buffer, free gaps included, broken only by an allocated block
// that is not drawn. So a frame is a few draws instead of one per block. A block can be excluded from the runs (the
// ones still pulsing after a change, drawn on their own with the pulse uniform). When the buffer has no room the arena
// asks for a bigger one and packs everything (a full upload). Units: floats (the caller's vertex is `stride` floats).
// GL-free: room_lab.cpp does the glBufferSubData / glDrawArrays; bench/mesh_arena_test.cpp.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace mesharena {

struct Range { size_t off = 0, len = 0; };

struct Arena {
    size_t cap = 0, stride = 10;            // floats in the buffer; floats per vertex
    std::map<int64_t, Range> allocs;        // by block key
    std::map<size_t, size_t> freeList;      // offset -> length, coalesced
    std::vector<Range> dirty;               // ranges to (re)upload: written blocks and zeroed frees
    bool rebuilt = false;                   // the buffer was resized and packed: upload all of it
    long packs = 0;

    explicit Arena(size_t capFloats = 0, size_t strideFloats = 10) : cap(capFloats), stride(strideFloats) { if (cap) freeList[0] = cap; }
    size_t used() const { size_t u = 0; for (auto& [k, r] : allocs) u += r.len; return u; }
    size_t high() const { size_t h = 0; for (auto& [k, r] : allocs) h = std::max(h, r.off + r.len); return h; }

    void release(size_t off, size_t len) {
        if (!len) return;
        dirty.push_back({off, len});  // zero it
        auto it = freeList.emplace(off, len).first;
        auto nx = std::next(it);
        if (nx != freeList.end() && it->first + it->second == nx->first) { it->second += nx->second; freeList.erase(nx); }
        if (it != freeList.begin()) { auto pv = std::prev(it); if (pv->first + pv->second == it->first) { pv->second += it->second; freeList.erase(it); } }
    }
    bool take(size_t len, size_t& off) {
        for (auto it = freeList.begin(); it != freeList.end(); ++it) if (it->second >= len) {
            off = it->first; size_t rest = it->second - len; freeList.erase(it);
            if (rest) freeList[off + len] = rest;
            return true;
        }
        return false;
    }
    void erase(int64_t key) { auto it = allocs.find(key); if (it == allocs.end()) return; Range r = it->second; allocs.erase(it); release(r.off, r.len); }
    // Place `len` floats for `key` (an update keeps its range when it still fits). Returns where to write them.
    Range put(int64_t key, size_t len) {
        len -= len % stride;
        auto it = allocs.find(key);
        if (it != allocs.end() && len <= it->second.len) {
            Range r = it->second; release(r.off + len, r.len - len); r.len = len; it->second = r;
            if (!len) { allocs.erase(it); return {}; }
            dirty.push_back(r); return r;
        }
        if (it != allocs.end()) erase(key);
        if (!len) return {};
        size_t off = 0;
        if (!take(len, off)) { grow(std::max(cap * 2, used() + len) + len); take(len, off); }
        allocs[key] = {off, len}; dirty.push_back({off, len});
        return {off, len};
    }
    // A bigger buffer, everything packed from offset 0 in key order (the caller re-uploads all of it).
    void grow(size_t newCap) {
        cap = std::max(newCap, used());
        size_t off = 0; for (auto& [k, r] : allocs) { r.off = off; off += r.len; }
        freeList.clear(); if (cap > off) freeList[off] = cap - off;
        dirty.clear(); rebuilt = true; packs++;
    }
    // Pack in place when more than half of the used span is holes (keeps runs long); a full upload.
    bool maybePack() { size_t h = high(), u = used(); if (h > 4096 && u * 2 < h) { grow(cap); return true; } return false; }

    // Draw runs (in vertices) for the blocks in `draw`, skipping those in `skip` (drawn separately). `bridge`: up to
    // that many floats of blocks that are neither drawn nor skipped may be drawn anyway to join two runs (a few extra
    // far triangles instead of another draw call); 0 = exactly the asked-for blocks.
    std::vector<Range> runs(const std::set<int64_t>& draw, const std::set<int64_t>& skip = {}, size_t bridge = 0) const {
        std::vector<std::pair<size_t, int64_t>> byOff; for (auto& [k, r] : allocs) byOff.push_back({r.off, k});
        std::sort(byOff.begin(), byOff.end());
        std::vector<Range> out; bool open = false; size_t pending = 0, pendingEnd = 0;
        for (auto& [off, k] : byOff) {
            const Range& r = allocs.at(k);
            if (skip.count(k)) { open = false; pending = 0; continue; }
            if (!draw.count(k)) {
                if (open && pending + r.len <= bridge) { pending += r.len; pendingEnd = r.off + r.len; continue; }
                open = false; pending = 0; continue;
            }
            (void)pendingEnd;
            if (open) out.back().len = (r.off + r.len) / stride - out.back().off;  // gaps in between: zeros or bridged blocks
            else { out.push_back({r.off / stride, r.len / stride}); open = true; }
            pending = 0;
        }
        return out;
    }
};

}  // namespace mesharena
