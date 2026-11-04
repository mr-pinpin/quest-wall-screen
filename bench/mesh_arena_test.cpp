// Room mesh in one buffer (room_lab/mesh_arena.h, flag MESHBATCH). g++ -std=c++17 -O1 -I room_lab bench/mesh_arena_test.cpp && ./a.out
// A simulated GPU buffer receives exactly the uploads room_lab would make; "drawing" the runs must give exactly the
// triangles of the blocks asked for (no stale, missing or extra ones) through hundreds of scan-like updates.
#include "mesh_arena.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <vector>
using namespace mesharena;
static int fails = 0;
static void check(bool c, const char* m) { if (!c) { printf("FAIL %s\n", m); fails++; } }

struct Gpu {  // what glBufferData / glBufferSubData would hold
    std::vector<float> buf;
    void sync(Arena& a, const std::map<int64_t, std::vector<float>>& blocks) {
        if (a.rebuilt || buf.size() != a.cap) {  // glBufferData of the new size, then every block
            buf.assign(a.cap, 0.0f);
            for (auto& [k, r] : a.allocs) std::copy(blocks.at(k).begin(), blocks.at(k).begin() + r.len, buf.begin() + r.off);
            a.rebuilt = false; a.dirty.clear(); return;
        }
        for (auto& d : a.dirty) {  // a written block, or a freed range (zeros)
            bool written = false;
            for (auto& [k, r] : a.allocs) if (r.off == d.off && r.len == d.len) { std::copy(blocks.at(k).begin(), blocks.at(k).begin() + r.len, buf.begin() + r.off); written = true; break; }
            if (!written) std::fill(buf.begin() + d.off, buf.begin() + d.off + d.len, 0.0f);
        }
        a.dirty.clear();
    }
};
// Triangles (as their first float, a unique id) drawn by the runs; degenerate all-zero triangles are skipped.
static std::multiset<float> drawn(const Gpu& g, const std::vector<Range>& runs, size_t stride) {
    std::multiset<float> s;
    for (auto& r : runs) for (size_t t = r.off; t + 3 <= r.off + r.len; t += 3) {
        bool zero = true; for (size_t f = t * stride; f < (t + 3) * stride; f++) if (g.buf[f] != 0) zero = false;
        if (!zero) s.insert(g.buf[t * stride]);
    }
    return s;
}

int main() {
    const size_t S = 10; srand(3);
    Arena a(30 * 2000, S); Gpu g; std::map<int64_t, std::vector<float>> blocks; float nextId = 1;
    auto make = [&](int tris) { std::vector<float> v(tris * 3 * S); for (int t = 0; t < tris; t++) for (int k = 0; k < 3 * (int)S; k++) v[t * 3 * S + k] = (k == 0) ? nextId : 0.5f; nextId++; return v; };
    long perBlockDraws = 0, runDraws = 0, frames = 0; bool exact = true; size_t maxRuns = 0;
    for (int frame = 0; frame < 600; frame++) {
        int ops = 1 + rand() % 6;  // a scan: blocks appear, grow, shrink, vanish
        for (int o = 0; o < ops; o++) {
            int64_t k = rand() % 400; int r = rand() % 10;
            if (r < 6) { blocks[k] = make(1 + rand() % 120); a.put(k, blocks[k].size()); }
            else if (r < 8 && blocks.count(k)) { auto& v = blocks[k]; v.resize((v.size() / (3 * S)) / 2 * 3 * S); a.put(k, v.size()); if (v.empty()) blocks.erase(k); }
            else if (blocks.count(k)) { blocks.erase(k); a.erase(k); }
        }
        a.maybePack();
        g.sync(a, blocks);
        std::set<int64_t> draw, skip;  // the nearest ~80% under the budget; a few still pulsing
        for (auto& [k, v] : blocks) if (k < 330) draw.insert(k);  // the triangle budget drops the far blocks (keys >= 330 here)
        for (auto k : draw) if (rand() % 25 == 0) skip.insert(k);
        auto runs = a.runs(draw, skip);
        std::multiset<float> want; for (auto k : draw) if (!skip.count(k)) for (size_t t = 0; t < blocks[k].size(); t += 3 * S) want.insert(blocks[k][t]);
        if (drawn(g, runs, S) != want) exact = false;
        perBlockDraws += draw.size(); runDraws += runs.size() + skip.size(); maxRuns = std::max(maxRuns, runs.size()); frames++;
    }
    {   // bridging: runs may draw small undrawn blocks to join; the drawn set is a superset, never a skipped block
        std::set<int64_t> draw, skip; for (auto& [k, v] : blocks) if (k < 330) draw.insert(k);
        for (auto k : draw) if (k % 40 == 0) skip.insert(k);
        auto exactRuns = a.runs(draw, skip), bridged = a.runs(draw, skip, 30 * 60);
        std::multiset<float> got = drawn(g, bridged, S), must; bool super = true, noSkip = true;
        for (auto k : draw) if (!skip.count(k)) for (size_t t = 0; t < blocks[k].size(); t += 3 * S) must.insert(blocks[k][t]);
        for (float id : must) if (!got.count(id)) super = false;
        for (auto k : skip) for (size_t t = 0; t < blocks[k].size(); t += 3 * S) if (got.count(blocks[k][t])) noSkip = false;
        printf("  bridging up to 60 triangles: %zu runs instead of %zu\n", bridged.size(), exactRuns.size());
        check(super && noSkip && bridged.size() <= exactRuns.size(), "bridged runs: every wanted triangle, never a pulsing block, no more draws");
    }
    printf("  600 scan frames, %zu blocks at the end: %.0f draws per frame per block vs %.1f with the arena (worst %zu runs); %ld packs, buffer %zu floats\n",
           blocks.size(), perBlockDraws / (double)frames, runDraws / (double)frames, maxRuns, a.packs, a.cap);
    check(exact, "every frame draws exactly the asked-for blocks' triangles: nothing stale, missing or extra");
    check(runDraws * 3 < perBlockDraws, "at least 3x fewer draws");
    // Everything drawn, nothing skipped, after a pack: one draw.
    {   std::set<int64_t> all; for (auto& [k, v] : blocks) all.insert(k);
        a.grow(a.cap); g.sync(a, blocks); auto runs = a.runs(all);
        check(runs.size() == 1, "all blocks after a pack: one draw"); }
    // An update that still fits keeps its place (only that range uploads); a bigger one moves and zeroes the old range.
    {   Arena b(1000, S); Range r1 = b.put(1, 300); b.put(2, 300); b.dirty.clear();
        Range r2 = b.put(1, 150); check(r2.off == r1.off && b.dirty.size() == 2, "shrink in place: the block and its freed tail upload");
        b.dirty.clear(); Range r3 = b.put(1, 350); check(r3.off != r1.off && b.dirty.size() == 2 && b.packs == 0, "grow: moves into a free range, the old range is zeroed");
        check(b.cap == 1000 || b.packs == 1, "no needless resize");
        Range r4 = b.put(3, 2000); check(b.packs >= 1 && b.cap >= b.used() && r4.len == 2000, "no room: a bigger buffer, packed");
    }
    printf("mesh arena: %s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
