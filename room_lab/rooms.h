// MULTIROOM (flag, default off; roadmap "Next"): several rooms, each with its own anchors and frame, and the links
// between them. A link is the pose of room b's frame in room a's frame plus the doorway (a point in a's frame). It is
// learned whenever anchors of both rooms locate in one session (Learner) and saved as house.json links[] (schema:
// a, b, pose of b in a; we add id "a~b", door, samples, updated). place() solves every room from its own located anchors
// (anchors::solve) and then places the rooms with none located through the links, so one room's anchors are enough
// for the whole flat. Tracker says which room the head is in (inside its box from walls/floor/anchors, with a margin
// so it does not flicker at a doorway) and records the crossing point as a doorway sample. GL-free;
// bench/rooms_test.cpp.
#pragma once
#include "anchors.h"
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace rooms {
using anchors::compose; using anchors::inverse; using wallfit::Pose; using wallfit::Quat; using wallfit::V3;

struct Room {
    std::string id;
    std::vector<anchors::Stored> anchors;             // rooms[].anchors, poses in this room's frame
    bool hasBox = false; float x0 = 0, x1 = 0, z0 = 0, z1 = 0;  // floor extent in the room frame (x, z)
};
// Extent from points on the room's walls or floor (room frame). Without any: the anchors grown by pad.
inline void boxFrom(Room& r, const std::vector<V3>& pts, float pad = 1.5f) {
    std::vector<V3> p = pts; float g = 0;
    if (p.empty()) { for (auto& a : r.anchors) p.push_back(a.inRoom.p); g = pad; }
    if (p.empty()) { r.hasBox = false; return; }
    r.x0 = r.x1 = p[0].x; r.z0 = r.z1 = p[0].z;
    for (auto& v : p) { r.x0 = std::min(r.x0, v.x); r.x1 = std::max(r.x1, v.x); r.z0 = std::min(r.z0, v.z); r.z1 = std::max(r.z1, v.z); }
    r.x0 -= g; r.x1 += g; r.z0 -= g; r.z1 += g; r.hasBox = true;
}

struct Link {
    std::string a, b; Pose bInA;
    bool hasDoor = false; V3 doorInA; int samples = 0, doorSamples = 0;
    std::string id() const { return a + "~" + b; }
};

struct Placed {
    std::string id; bool ok = false, viaLink = false;
    Pose inLocal;            // the room frame in LOCAL this session
    anchors::Frame frame;    // the direct solve (frame.ok false when placed through a link or not at all)
    std::string via;         // the room it was placed from, when viaLink
};

inline const Placed* find(const std::vector<Placed>& ps, const std::string& id) { for (auto& p : ps) if (p.id == id) return &p; return nullptr; }
inline const Room* find(const std::vector<Room>& rs, const std::string& id) { for (auto& r : rs) if (r.id == id) return &r; return nullptr; }

inline std::vector<Placed> place(const std::vector<Room>& rs, const std::vector<Link>& links, const std::vector<anchors::Seen>& seen) {
    std::vector<Placed> out;
    for (auto& r : rs) {
        Placed p; p.id = r.id; p.frame = anchors::solve(r.anchors, seen);
        if (p.frame.ok) { p.ok = true; p.inLocal = p.frame.roomInLocal; }
        out.push_back(p);
    }
    // Through the links, breadth first: a room is placed from a neighbour already placed (direct ones first).
    for (bool grew = true; grew;) {
        grew = false;
        for (auto& l : links) {
            Placed *a = nullptr, *b = nullptr;
            for (auto& p : out) { if (p.id == l.a) a = &p; if (p.id == l.b) b = &p; }
            if (!a || !b || a->ok == b->ok) continue;
            if (a->ok) { b->inLocal = compose(a->inLocal, l.bInA); b->ok = b->viaLink = true; b->via = a->id; }
            else { a->inLocal = compose(b->inLocal, inverse(l.bInA)); a->ok = a->viaLink = true; a->via = b->id; }
            grew = true;
        }
    }
    return out;
}

// How far the head (LOCAL) is outside the room's floor box, in metres (0 inside, grown by margin). -1: room unknown.
inline float outside(const Room& r, const Placed& p, V3 head, float margin = 0) {
    if (!p.ok || !r.hasBox) return -1;
    V3 h = compose(inverse(p.inLocal), Pose{{0, 0, 0, 1}, head}).p;
    float dx = std::max({r.x0 - margin - h.x, 0.0f, h.x - r.x1 - margin}), dz = std::max({r.z0 - margin - h.z, 0.0f, h.z - r.z1 - margin});
    return std::sqrt(dx * dx + dz * dz);
}

struct Crossing { std::string from, to; V3 headLocal; };

// The room the head is in. It stays in the current room while within margin of its box; it moves to a room whose
// box holds the head, preferring rooms placed by their own anchors, then the nearest box.
struct Tracker {
    std::string current; float margin = 0.3f;
    std::vector<Crossing> crossings;   // drained by the caller
    bool update(const std::vector<Room>& rs, const std::vector<Placed>& ps, V3 head) {
        if (!current.empty()) {
            const Room* r = find(rs, current); const Placed* p = find(ps, current);
            if (r && p) { float d = outside(*r, *p, head, margin); if (d == 0) return false; }
        }
        std::string best; float bestD = 1e9f; bool bestDirect = false;
        for (auto& r : rs) {
            const Placed* p = find(ps, r.id); if (!p) continue;
            float d = outside(r, *p, head); if (d < 0) continue;
            bool direct = p->frame.ok;
            if (d < bestD - 1e-4f || (std::fabs(d - bestD) <= 1e-4f && direct && !bestDirect)) { best = r.id; bestD = d; bestDirect = direct; }
        }
        if (best.empty() || best == current) return false;
        if (bestD > 0 && !current.empty()) {           // in no box: keep the room we were in unless it is gone
            const Placed* p = find(ps, current); if (p && p->ok) return false;
        }
        if (!current.empty()) crossings.push_back({current, best, head});
        current = best; return true;
    }
};

// Learns links from rooms placed directly at the same time, and doorways from crossings.
struct Learner {
    float tolM = 0.05f, tolDeg = 2.0f; size_t cap = 60; int minSamples = 5;
    std::map<std::pair<std::string, std::string>, std::vector<Pose>> poses;  // (a, b) with a < b: b in a
    struct Door { V3 sum; int n = 0; };
    std::map<std::pair<std::string, std::string>, Door> doors;               // door in a's frame
    void sample(const std::vector<Placed>& ps) {
        for (size_t i = 0; i < ps.size(); i++) for (size_t j = 0; j < ps.size(); j++) {
            const Placed &a = ps[i], &b = ps[j];
            if (!(a.id < b.id) || !a.frame.ok || !b.frame.ok) continue;
            auto& v = poses[{a.id, b.id}]; v.push_back(compose(inverse(a.inLocal), b.inLocal));
            if (v.size() > cap) v.erase(v.begin());
        }
    }
    void crossing(const Crossing& c, const std::vector<Placed>& ps) {
        std::string a = std::min(c.from, c.to), b = std::max(c.from, c.to);
        const Placed* pa = find(ps, a); if (!pa || !pa->ok) return;
        V3 d = compose(inverse(pa->inLocal), Pose{{0, 0, 0, 1}, c.headLocal}).p;
        Door& D = doors[{a, b}]; D.sum = D.sum + d; D.n++;
    }
    // The links to keep: the learned ones (consensus of the samples, like anchors::solve) replace the old pose of the
    // same pair; old links without new samples stay as they were. changed lists the ids whose pose moved more than
    // tolM / tolDeg (or are new) or got a new doorway.
    std::vector<Link> merge(const std::vector<Link>& old, std::vector<std::string>* changed = nullptr) const {
        std::vector<Link> out;
        auto oldOf = [&](const std::string& a, const std::string& b, bool* flip) -> const Link* {
            for (auto& l : old) { if (l.a == a && l.b == b) { *flip = false; return &l; } if (l.a == b && l.b == a) { *flip = true; return &l; } }
            return nullptr;
        };
        std::vector<std::pair<std::string, std::string>> pairs;
        for (auto& [k, v] : poses) if ((int)v.size() >= minSamples) pairs.push_back(k);
        for (auto& [k, d] : doors) if (d.n > 0 && std::find(pairs.begin(), pairs.end(), k) == pairs.end()) pairs.push_back(k);
        for (auto& l : old) {
            auto k = std::make_pair(std::min(l.a, l.b), std::max(l.a, l.b));
            if (std::find(pairs.begin(), pairs.end(), k) == pairs.end()) out.push_back(l);
        }
        for (auto& k : pairs) {
            bool flip = false; const Link* o = oldOf(k.first, k.second, &flip);
            Link L; L.a = k.first; L.b = k.second; bool have = false;
            if (o) { L = *o; if (flip) { L.a = k.first; L.b = k.second; L.bInA = inverse(o->bInA);
                     if (o->hasDoor) L.doorInA = compose(L.bInA, Pose{{0, 0, 0, 1}, o->doorInA}).p; } have = true; }
            bool moved = false;
            auto it = poses.find(k);
            if (it != poses.end() && (int)it->second.size() >= minSamples) {
                std::vector<anchors::Stored> st{{"x", Pose{{0, 0, 0, 1}, {0, 0, 0}}}}; std::vector<anchors::Seen> se;
                for (auto& p : it->second) se.push_back({"x", p});
                // anchors::solve with one stored anchor at the origin: the consensus of the samples.
                anchors::Frame f = anchors::solve(st, se, tolM, tolDeg);
                if (f.ok && f.used >= minSamples) {
                    moved = !have || wallfit::len(f.roomInLocal.p - L.bInA.p) > tolM || anchors::angleDeg(f.roomInLocal.q, L.bInA.q) > tolDeg;
                    if (moved || !have) L.bInA = f.roomInLocal;
                    L.samples = std::max(L.samples, f.used); have = true;
                }
            }
            auto dt = doors.find(k);
            if (dt != doors.end() && dt->second.n > 0) {
                V3 d = dt->second.sum * (1.0f / dt->second.n);
                if (!L.hasDoor || wallfit::len(d - L.doorInA) > 0.1f) moved = true;
                L.doorInA = d; L.hasDoor = true; L.doorSamples = dt->second.n;
            }
            if (!have) continue;   // a doorway alone, without any pose for the pair: nothing to save yet
            if (moved && changed) changed->push_back(L.id());
            out.push_back(L);
        }
        return out;
    }
};

// The next free room id "rN".
inline std::string nextId(const std::vector<Room>& rs) {
    for (int n = 1;; n++) { std::string id = "r" + std::to_string(n); if (!find(rs, id)) return id; }
}

}  // namespace rooms
