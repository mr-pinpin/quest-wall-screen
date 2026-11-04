// House consolidation on the headset (kanban K27, work-0e): a port of work-34's house/consolidate.py ("consolidate/1",
// the same PARAMS), steps 0-3 and Meta's room chains, so SCAN can show clean walls live and use them as placement
// targets instead of raw fits. Stage for stage the same rules and the same arithmetic (doubles, Python's floor modulo
// and round-half-even), so the output matches the Python on the same input: bench/consolidate_xcheck.py runs the Python
// functions and bench/consolidate_test.cpp compares, in the offline gate (a change to the Python that is not mirrored
// here fails there). Phase 2 (planWalls / planRooms): the floor plan by rays (rooms()), each room's outline, support
// (doors, holes, open sides, closure), floor and ceiling heights, and the plan walls with their room's heights, as
// consolidate() returns them, and the score. The one difference by construction: a room outline's first
// vertex follows CPython's set order there, so outlines compare up to rotation. Input: house.json walls (the same
// fields the Python reads); room_lab feeds the house's walls plus the live SCAN fits as one batch. score(): the verdict
// and its reasons, word for word as consolidate.py's score (the live SCAN hint is built from them). GL-free.
#pragma once
#include <algorithm>
#include <cmath>
#include <array>
#include <cstdio>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "json.h"

namespace consolidate {

struct Params {
    double snapDeg = 7.0, mergeOff = 0.05, mergeDeg = 3.0, mergeGap = 0.10, minLen = 0.4, minConf = 0.4, stackOff = 0.35,
           stackOverlap = 0.5, stackKeep = 0.25, cornerReach = 0.7, overshoot = 0.3, doorMin = 0.7, doorMax = 1.2,
           yShiftMax = 0.3, batchDeg = 3.0, cellMerge = 0.05, minRoomArea = 1.0;
};
constexpr const char* kAlgo = "consolidate/1";
constexpr double kPi = 3.14159265358979323846;

inline double deg(double r) { return r * 180.0 / kPi; }
inline double rad(double d) { return d * kPi / 180.0; }
inline double pmod(double a, double b) { double m = std::fmod(a, b); if (m != 0 && ((m < 0) != (b < 0))) m += b; return m; }  // Python %
inline double wrap(double a, double period) { return pmod(a + period / 2, period) - period / 2; }
inline long pyround(double x) { double r = std::nearbyint(x); return (long)r; }  // round half to even, like Python's round()

struct Surf {
    std::string id, src, kind, batch, drop;
    double p[3], r[3], u[3], n[3], w = 0, h = 0, conf = 0, yaw = 0, y0 = 0, y1 = 0; long samples = 0;
    int orient = 0;  // 0 vertical, 1 up, 2 down
};
struct Dropped { std::string id, why; };
struct Seg {
    int k = 0; double off = 0, t0 = 0, t1 = 0, y0 = 0, y1 = 0, ext[2] = {0, 0}, clipped = -1;
    std::vector<Surf*> members;
};
struct Frame2 {
    double th = 0, eb[2] = {0, 1}, ea[2] = {1, 0};
    explicit Frame2(double t = 0) : th(t) { eb[0] = std::sin(t); eb[1] = std::cos(t); ea[0] = std::cos(t); ea[1] = -std::sin(t); }
    void to(double x, double z, double& a, double& b) const { a = x * ea[0] + z * ea[1]; b = x * eb[0] + z * eb[1]; }
    void back(double a, double b, double& x, double& z) const { x = a * ea[0] + b * eb[0]; z = a * ea[1] + b * eb[1]; }
};
constexpr int SIGN[4] = {1, 1, -1, -1};
inline void segLine(int k, int& oi, int& ti) { if (k % 2 == 0) { oi = 1; ti = 0; } else { oi = 0; ti = 1; } }

using PT = std::pair<double, double>;   // (A, B) in the axis frame
using Loop = std::vector<PT>;
struct RoomF { std::vector<Loop> loops; double area = 0; bool open = false, meta = false; std::vector<std::string> metaIds; };
struct PlanRoom { std::string id; Loop floor; std::vector<Loop> holes; double area = 0, closure = 0, perimeter = 0, openM = 0; bool closed = false, haveFloor = false, haveCeil = false; double floorY = 0, ceilingY = 0; int doors = 0;
                  std::vector<std::pair<PT, PT>> opens; };  // open stretches (x, z) ends: where the room is not walled yet
struct B2B { std::string depth, meta; double thickness; };
struct Score {
    bool ok = false; std::vector<std::string> reasons;
    int dupRaw = 0, dupPlan = 0; bool haveExplained = false; double explained = 0;
    int pairs = 0; bool haveMedian = false; double median = 0; std::vector<B2B> b2b; std::vector<std::string> lone; int badFrames = 0;
};
struct PlanWall { std::string id, room, src; double x = 0, y = 0, z = 0, yaw = 0, w = 0, h = 0; bool offAxis = false; std::vector<std::string> from; };
struct Result {
    double th = 0, share = 0; bool needsRooms = false;   // needsRooms: Meta's walls form no closed chain (rooms by rays then)
    bool haveFloor = false, haveCeil = false; double refFloor = 0, refCeil = 0; std::vector<Surf*> surfs;  // after frames()
    std::vector<Seg> segs; std::vector<Surf*> offaxis; std::vector<Dropped> dropped;
    std::vector<RoomF> metaRooms;                            // Meta's rooms (closed chains, else by rays from Meta's walls)
    std::vector<PlanRoom> rooms; std::vector<PlanWall> walls;  // phase 2: the floor plan, as consolidate() returns it
    std::vector<Surf> rawVert;                                 // vertical surfaces as read (before frames() moved any)
    std::vector<std::pair<std::string, std::string>> batches;  // frames(): batch -> status, in key order
    Score score;
    std::deque<Surf> pool;  // the surfaces the segments point at
    // A plan wall from a segment (room frame): centre, yaw (pose +z = normal into the room), width, y range.
    void wall(const Seg& s, double& x, double& z, double& yaw, double& w) const {
        int oi, ti; segLine(s.k, oi, ti); double c[2]; c[oi] = s.off; c[ti] = (s.t0 + s.t1) / 2;
        Frame2 F(th); F.back(c[0], c[1], x, z); yaw = th + s.k * kPi / 2; w = s.t1 - s.t0;
    }
};

inline void qrot(const double* q, const double* v, double* o) {
    double x = q[0], y = q[1], z = q[2], w = q[3];
    double tx = 2 * (y * v[2] - z * v[1]), ty = 2 * (z * v[0] - x * v[2]), tz = 2 * (x * v[1] - y * v[0]);
    o[0] = v[0] + w * tx + (y * tz - z * ty); o[1] = v[1] + w * ty + (z * tx - x * tz); o[2] = v[2] + w * tz + (x * ty - y * tx);
}
inline bool isNum(const json::Value& v) { return v.type == json::Value::Num; }
inline double conf(const json::Value& w) {
    std::string src = w["source"].str();
    if (src == "scene" || src == "manual") return 1.0;
    const json::Value& f = w["fit"];
    for (const json::Value* v : {&w["confidence"], &f["conf"], &f["confidence"]}) if (isNum(*v)) return std::max(0.0, std::min(1.0, v->n));
    double s = std::min(1.0, (isNum(f["samples"]) ? f["samples"].n : 0.0) / 15000.0);
    double rms = isNum(f["rms_m"]) ? f["rms_m"].n : 0.04;
    double flat = std::max(0.0, std::min(1.0, (0.04 - rms) / 0.03));
    return s * flat;
}
inline std::string fmt(const char* f, double v) { char b[96]; snprintf(b, sizeof b, f, v); return b; }

// 0. surfaces
inline void surfaces(const json::Value& doc, Result& R, std::vector<Surf*>& out) {
    for (auto& w : doc["walls"].a) {
        if (w.type != json::Value::Obj) continue;
        std::string wid = w["id"].str();
        const json::Value& pose = w["pose"];
        bool hasW = isNum(w["w"]) && w["w"].n != 0, hasH = isNum(w["h"]) && w["h"].n != 0;
        if (pose.type != json::Value::Obj || !hasW || !hasH) { R.dropped.push_back({wid, "no pose or size yet"}); continue; }
        double p[3], q[4];
        for (int i = 0; i < 3; i++) p[i] = pose["p"][i].n;
        for (int i = 0; i < 4; i++) q[i] = pose["q"][i].n;
        const json::Value& fr = w["frame"];
        bool unloc = fr["located"].type == json::Value::Bool && !fr["located"].b;
        if (unloc || (p[0] == 0 && p[1] == 0 && p[2] == 0 && q[0] == 0 && q[1] == 0 && q[2] == 0 && q[3] == 1)) { R.dropped.push_back({wid, "frame: unlocated pose"}); continue; }
        Surf s; s.id = wid; s.src = w.has("source") ? w["source"].str() : "depth"; s.kind = w.has("kind") ? w["kind"].str() : "plane";
        const double ex[3] = {1, 0, 0}, ey[3] = {0, 1, 0}, ez[3] = {0, 0, 1};
        qrot(q, ex, s.r); qrot(q, ey, s.u); qrot(q, ez, s.n);
        for (int i = 0; i < 3; i++) s.p[i] = p[i];
        s.w = w["w"].n; s.h = w["h"].n; s.conf = conf(w);
        const json::Value& f = w["fit"]; s.samples = isNum(f["samples"]) ? (long)f["samples"].n : 0;
        if (fr["id"].type == json::Value::Str && !fr["id"].str().empty()) s.batch = fr["id"].str();
        else if (f["updated"].type == json::Value::Str && !f["updated"].str().empty()) s.batch = f["updated"].str();
        else s.batch = s.src != "depth" ? "scene" : "?";
        if (std::fabs(s.n[1]) < 0.26) {
            s.orient = 0; s.yaw = std::atan2(s.n[0], s.n[2]);
            s.y0 = s.p[1] - std::fabs(s.u[1]) * s.h / 2 - std::fabs(s.r[1]) * s.w / 2;
            s.y1 = s.p[1] + std::fabs(s.u[1]) * s.h / 2 + std::fabs(s.r[1]) * s.w / 2;
        } else if (std::fabs(s.n[1]) > 0.9) {
            s.orient = s.n[1] > 0 ? 1 : 2;
        } else {
            double tilt = deg(std::asin(std::min(1.0, std::fabs(s.n[1]))));
            R.dropped.push_back({wid, fmt("slanted (normal %.0f deg above horizontal): neither wall nor floor", tilt)}); continue;
        }
        R.pool.push_back(s); out.push_back(&R.pool.back());
    }
}

// 1-2. axes and frames
inline void axesPeak(const std::vector<Surf*>& surfs, const Params& P, double& thOut, double& share) {
    std::vector<double> bins(180, 0.0); double tot = 0;
    for (auto* s : surfs) {
        if (s->orient != 0) continue;
        double a = pmod(deg(s->yaw), 90.0), wgt = s->w * std::max(s->conf, 0.05);
        bins[(int)(a * 2) % 180] += wgt; tot += wgt;
    }
    if (tot <= 0) { thOut = 0; share = 0; return; }
    std::vector<double> sm(180, 0.0);
    for (int i = 0; i < 180; i++) { double v = 0; for (int k = -4; k <= 4; k++) v += bins[((i + k) % 180 + 180) % 180]; sm[i] = v; }
    int best = 0; for (int i = 1; i < 180; i++) if (sm[i] > sm[best]) best = i;
    double peak = best / 2.0 + 0.25, cs = 0, sn = 0;
    for (auto* s : surfs) if (s->orient == 0) {
        double d = wrap(deg(s->yaw) - peak, 90);
        if (std::fabs(d) <= 3) { double wgt = s->w * std::max(s->conf, 0.05), a4 = rad(4 * (peak + d)); cs += wgt * std::cos(a4); sn += wgt * std::sin(a4); }
    }
    double th = (cs != 0 || sn != 0) ? pmod(deg(std::atan2(sn, cs)) / 4.0, 90.0) : peak;
    double in = 0;
    for (auto* s : surfs) if (s->orient == 0 && std::fabs(wrap(deg(s->yaw) - th, 90)) <= P.snapDeg) in += s->w * std::max(s->conf, 0.05);
    thOut = rad(th); share = in / tot;
}

inline std::vector<Surf*> frames(std::vector<Surf*> surfs, const Params& P, Result& R) {
    auto isUp = [](const Surf* s) { return s->orient == 1 && s->w * s->h >= 1.0; };
    auto isDn = [](const Surf* s) { return s->orient == 2 && s->w * s->h >= 1.0; };
    std::vector<std::string> order; std::map<std::string, std::vector<Surf*>> batches;
    for (auto* s : surfs) if (s->src == "depth") { if (!batches.count(s->batch)) order.push_back(s->batch); batches[s->batch].push_back(s); }
    auto big = [](const std::vector<Surf*>& ss) { long t = 0; for (auto* s : ss) t += s->samples; return t; };
    auto lowest = [](const std::vector<Surf*>& ss) { double m = 1e300; for (auto* s : ss) m = std::min(m, s->p[1]); return m; };
    std::vector<Surf*> sceneUp, sceneDn;
    for (auto* s : surfs) { if (isUp(s) && s->src != "depth") sceneUp.push_back(s); if (isDn(s) && s->src != "depth") sceneDn.push_back(s); }
    bool haveFloor = false; double refFloor = 0; std::string refFrom;
    if (!sceneUp.empty()) { refFloor = lowest(sceneUp); refFrom = "meta floor"; haveFloor = true; }
    else if (!order.empty()) {
        std::string best = order[0]; long bb = big(batches[best]);
        for (auto& b : order) { long v = big(batches[b]); if (v > bb) { bb = v; best = b; } }
        std::vector<Surf*> bu; for (auto* s : batches[best]) if (isUp(s)) bu.push_back(s);
        if (!bu.empty()) { refFloor = lowest(bu); refFrom = "floor of commit " + best; haveFloor = true; }
    }
    bool haveCeil = !sceneDn.empty(); double refCeil = -1e300; for (auto* s : sceneDn) refCeil = std::max(refCeil, s->p[1]);
    R.haveFloor = haveFloor; R.refFloor = refFloor; R.haveCeil = haveCeil; R.refCeil = refCeil;
    double th0, sh0; axesPeak(surfs, P, th0, sh0);
    for (auto& [b, ss] : batches) {
        std::string status = "ok";
        std::vector<Surf*> verts; for (auto* s : ss) if (s->orient == 0) verts.push_back(s);
        if (verts.size() >= 3) {
            double th, share; axesPeak(verts, P, th, share);
            double dyaw = wrap(deg(th - th0), 90);
            if (std::fabs(dyaw) > P.batchDeg && share > 0.5) status = fmt("dropped: rotated %+.1f deg against the room", dyaw);
        }
        if (status == "ok") {
            std::vector<Surf*> bu, bd; for (auto* s : ss) { if (isUp(s)) bu.push_back(s); if (isDn(s)) bd.push_back(s); }
            bool have = false; double dy = 0; std::string how;
            if (haveFloor && !bu.empty()) { dy = refFloor - lowest(bu); how = refFrom; have = true; }
            else if (haveCeil && !bd.empty()) { double m = -1e300; for (auto* s : bd) m = std::max(m, s->p[1]); dy = refCeil - m; how = "meta ceiling"; have = true; }
            if (have) {
                if (std::fabs(dy) > P.yShiftMax) status = fmt("dropped: floor %+.2f m off the ", dy) + how + " (another frame)";
                else if (std::fabs(dy) > 0.02) { status = fmt("moved %+.2f m onto the ", dy) + how; for (auto* s : ss) { s->p[1] += dy; if (s->orient == 0) { s->y0 += dy; s->y1 += dy; } } }
            }
        }
        if (status.rfind("dropped", 0) == 0) for (auto* s : ss) s->drop = "frame: commit " + status.substr(9);
        R.batches.push_back({b, status});
    }
    std::vector<Surf*> keep;
    for (auto* s : surfs) { if (!s->drop.empty()) R.dropped.push_back({s->id, s->drop}); else keep.push_back(s); }
    return keep;
}

// 3. segments
inline double weight(const Surf* s) { return s->conf * s->w * (s->src != "depth" ? 5.0 : 1.0); }
inline bool hasScene(const Seg& s) { for (auto* m : s.members) if (m->src != "depth") return true; return false; }
inline double offsetOf(const Seg& seg, const Frame2& F) {
    std::vector<Surf*> mem; for (auto* m : seg.members) if (m->src != "depth") mem.push_back(m);
    if (mem.empty()) mem = seg.members;
    int oi, ti; segLine(seg.k, oi, ti); double sv = 0, sw = 0;
    for (auto* m : mem) { double c[2]; F.to(m->p[0], m->p[2], c[0], c[1]); double w = weight(m) + 1e-9; sv += c[oi] * w; sw += w; }
    return sv / sw;
}
inline std::vector<Seg> makeSegments(const std::vector<Surf*>& surfs, const Frame2& F, const Params& P, Result& R) {
    std::vector<Seg> segs;
    for (auto* s : surfs) {
        if (s->orient != 0) continue;
        double d = wrap(deg(s->yaw - F.th), 360);
        int k = (int)pmod((double)pyround(d / 90.0), 4.0);
        double dev = wrap(d - 90 * k, 360);
        double c[2]; F.to(s->p[0], s->p[2], c[0], c[1]);
        if (std::fabs(dev) > P.snapDeg) {
            if (s->src == "depth") { R.dropped.push_back({s->id, fmt("off-axis: %.0f deg from the room directions", std::fabs(dev))}); continue; }
            R.offaxis.push_back(s); continue;
        }
        if (s->src == "depth" && s->conf < P.minConf) { R.dropped.push_back({s->id, fmt("low confidence %.2f", s->conf)}); continue; }
        int oi, ti; segLine(k, oi, ti);
        double half = s->w / 2 * std::hypot(s->r[0], s->r[2]);
        Seg g; g.k = k; g.off = c[oi]; g.t0 = c[ti] - half; g.t1 = c[ti] + half; g.y0 = s->y0; g.y1 = s->y1; g.members = {s};
        segs.push_back(g);
    }
    return segs;
}
inline std::vector<Seg> mergeColinear(const std::vector<Seg>& segs, const Frame2& F, const Params& P) {
    std::vector<Seg> out;
    for (int k = 0; k < 4; k++) {
        std::vector<Seg> ss; for (auto& s : segs) if (s.k == k) ss.push_back(s);
        std::stable_sort(ss.begin(), ss.end(), [](const Seg& a, const Seg& b) { return a.off < b.off; });
        std::vector<std::vector<Seg>> groups; std::vector<Seg> cur;
        for (auto& s : ss) {
            if (!cur.empty() && (s.off - cur.back().off > P.mergeOff || s.off - cur.front().off > 2 * P.mergeOff)) { groups.push_back(cur); cur.clear(); }
            cur.push_back(s);
        }
        if (!cur.empty()) groups.push_back(cur);
        for (auto& g : groups) {
            std::stable_sort(g.begin(), g.end(), [](const Seg& a, const Seg& b) { return a.t0 < b.t0; });
            bool haveRun = false; Seg run;
            for (auto& s : g) {
                bool sa = haveRun && hasScene(run), sb = hasScene(s);
                double reach = !(sa || sb) ? P.mergeGap : -0.05;
                if (haveRun && !(sa && sb) && s.t0 <= run.t1 + reach) {
                    run.t1 = std::max(run.t1, s.t1); run.members.insert(run.members.end(), s.members.begin(), s.members.end());
                    run.y0 = std::min(run.y0, s.y0); run.y1 = std::max(run.y1, s.y1);
                } else { if (haveRun) out.push_back(run); run = s; haveRun = true; }
            }
            if (haveRun) out.push_back(run);
        }
    }
    for (auto& s : out) s.off = offsetOf(s, F);
    return out;
}
inline double overlap(const Seg& a, const Seg& b) { return std::max(0.0, std::min(a.t1, b.t1) - std::max(a.t0, b.t0)); }
inline double segWeight(const Seg& s) { double t = 0; for (auto* m : s.members) t += weight(m); return t; }
inline std::vector<Seg> suppressStacks(std::vector<Seg> segs, const Params& P, Result& R) {
    std::vector<double> key; for (auto& s : segs) key.push_back(-segWeight(s));
    std::vector<size_t> idx(segs.size()); for (size_t i = 0; i < idx.size(); i++) idx[i] = i;
    std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return key[a] < key[b]; });
    std::vector<Seg> sorted; for (size_t i : idx) sorted.push_back(segs[i]); segs.swap(sorted);
    std::vector<bool> alive(segs.size(), true);
    for (size_t i = 0; i < segs.size(); i++) {
        if (!alive[i]) continue;
        const Seg a = segs[i];
        std::vector<size_t> stack = {i};
        for (size_t j = 0; j < segs.size(); j++) {
            const Seg& b = segs[j];
            if (j != i && alive[j] && b.k == a.k && std::fabs(b.off - a.off) <= P.stackOff && overlap(a, b) >= P.stackOverlap * std::min(a.t1 - a.t0, b.t1 - b.t0)) stack.push_back(j);
        }
        if (stack.size() == 1) continue;
        double wmax = 0; for (size_t j : stack) wmax = std::max(wmax, segWeight(segs[j]));
        std::vector<size_t> sc; for (size_t j : stack) if (hasScene(segs[j])) sc.push_back(j);
        size_t keep;
        if (!sc.empty()) keep = sc[0];
        else {
            bool any = false; double bestV = 0; keep = stack[0];
            for (size_t j : stack) if (segWeight(segs[j]) >= P.stackKeep * wmax) { double v = SIGN[a.k] * segs[j].off; if (!any || v < bestV) { bestV = v; keep = j; any = true; } }
        }
        Seg& kp = segs[keep];
        for (size_t j : stack) {
            if (j == keep) continue;
            Seg& b = segs[j];
            double gap = std::fabs(b.off - kp.off); bool front;
            if (gap <= 0.12 && !hasScene(b)) { kp.t0 = std::min(kp.t0, b.t0); kp.t1 = std::max(kp.t1, b.t1); kp.members.insert(kp.members.end(), b.members.begin(), b.members.end()); front = false; }
            else if (hasScene(b)) continue;
            else front = true;
            if (front) for (auto* m : b.members) R.dropped.push_back({m->id, fmt("%.2f m in front of a wall it overlaps (furniture face or a second look)", gap)});
            alive[j] = false;
        }
    }
    std::vector<Seg> out; for (size_t i = 0; i < segs.size(); i++) if (alive[i]) out.push_back(segs[i]);
    return out;
}
inline std::vector<Seg> clipCorners(std::vector<Seg> segs, const Params& P, Result& R) {
    double Rr = P.cornerReach;
    struct Cut { double t0, t1, e0, e1; }; std::vector<Cut> cuts;
    for (auto& s : segs) {
        double t0 = s.t0, t1 = s.t1, e0 = 0, e1 = 0;
        if (!hasScene(s)) {
            std::vector<double> lo, hi;
            for (auto& t : segs) {
                if (&t == &s || t.k % 2 == s.k % 2) continue;
                if (s.t0 - Rr <= t.off && t.off <= s.t1 + Rr && t.t0 - Rr <= s.off && s.off <= t.t1 + Rr) {
                    if (SIGN[s.k] * ((t.t0 + t.t1) / 2 - s.off) <= 0) continue;
                    if ((SIGN[t.k] > 0 ? s.t1 : -s.t0) <= SIGN[t.k] * t.off) continue;
                    (SIGN[t.k] > 0 ? lo : hi).push_back(t.off);
                }
            }
            if (!lo.empty()) {
                double c = *std::min_element(lo.begin(), lo.end());
                if (t0 < c - P.overshoot || (c - P.overshoot <= t0 && t0 < c)) t0 = c;
                else if (c < t0 && t0 <= c + Rr) { e0 = t0 - c; t0 = c; }
            }
            if (!hi.empty()) {
                double c = *std::max_element(hi.begin(), hi.end());
                if (t1 > c + P.overshoot || (c < t1 && t1 <= c + P.overshoot)) t1 = c;
                else if (c - Rr <= t1 && t1 < c) { e1 = c - t1; t1 = c; }
            }
        }
        cuts.push_back({t0, t1, e0, e1});
    }
    std::vector<Seg> out;
    for (size_t i = 0; i < segs.size(); i++) {
        Seg s = segs[i]; const Cut& c = cuts[i];
        if ((c.t1 - c.t0) < (s.t1 - s.t0) - 0.31) s.clipped = s.t1 - s.t0;
        s.t0 = c.t0; s.t1 = c.t1; s.ext[0] = c.e0; s.ext[1] = c.e1;
        if (c.t1 - c.t0 < P.minLen && !hasScene(s)) {
            for (auto* m : s.members) R.dropped.push_back({m->id, fmt("short (%.2f m after merging and corners)", std::max(0.0, c.t1 - c.t0))});
            continue;
        }
        out.push_back(s);
    }
    return out;
}

// Meta's rooms: closed chains of Meta walls, end to end.
struct Cycle { std::vector<std::pair<double, double>> poly; std::vector<std::string> ids; };
inline std::vector<Cycle> metaCycles(const std::vector<Surf*>& all, double tol = 0.15) {
    std::vector<Surf*> S; for (auto* s : all) if (s->orient == 0 && s->src != "depth") S.push_back(s);
    std::vector<std::pair<double, double>> pos; std::vector<std::pair<int, int>> node;
    for (auto* s : S) {
        double hw = s->w / 2; std::pair<double, double> e[2] = {{s->p[0] - s->r[0] * hw, s->p[2] - s->r[2] * hw}, {s->p[0] + s->r[0] * hw, s->p[2] + s->r[2] * hw}};
        int ids[2];
        for (int t = 0; t < 2; t++) {
            int k = -1; for (size_t i = 0; i < pos.size(); i++) if (std::fabs(pos[i].first - e[t].first) <= tol && std::fabs(pos[i].second - e[t].second) <= tol) { k = (int)i; break; }
            if (k < 0) { pos.push_back(e[t]); k = (int)pos.size() - 1; }
            ids[t] = k;
        }
        node.push_back({ids[0], ids[1]});
    }
    std::vector<int> keys; std::map<int, std::vector<std::pair<int, int>>> adj;
    for (size_t w = 0; w < node.size(); w++) {
        int u = node[w].first, v = node[w].second; if (u == v) continue;
        if (!adj.count(u)) keys.push_back(u);
        adj[u].push_back({v, (int)w});
        if (!adj.count(v)) keys.push_back(v);
        adj[v].push_back({u, (int)w});
    }
    std::vector<Cycle> out; std::vector<bool> used(pos.size(), false);
    for (int start : keys) {
        if (used[start] || adj[start].size() != 2) continue;
        std::vector<int> loop = {start}; std::vector<int> walls; int cur = start, prevW = -1; bool ok = true;
        for (;;) {
            std::vector<std::pair<int, int>> nxt; for (auto& vw : adj[cur]) if (vw.second != prevW) nxt.push_back(vw);
            if (adj[cur].size() != 2 || nxt.empty()) { ok = false; break; }
            int v = nxt[0].first, w = nxt[0].second; walls.push_back(w); prevW = w;
            if (v == start) break;
            if (std::find(loop.begin(), loop.end(), v) != loop.end()) { ok = false; break; }
            loop.push_back(v); cur = v;
        }
        for (int v : loop) used[v] = true;
        if (ok && walls.size() >= 3) { Cycle c; for (int i : loop) c.poly.push_back(pos[i]); for (int w : walls) c.ids.push_back(S[w]->id); out.push_back(c); }
    }
    return out;
}
inline bool inside(double x, double z, const std::vector<std::pair<double, double>>& poly) {
    bool in = false; size_t n = poly.size();
    for (size_t i = 0; i < n; i++) {
        auto [x0, z0] = poly[(i + n - 1) % n]; auto [x1, z1] = poly[i];
        if ((z0 > z) != (z1 > z) && x < x0 + (z - z0) * (x1 - x0) / (z1 - z0)) in = !in;
    }
    return in;
}
// ---- phase 2: rooms by rays (consolidate.py rooms(), _segments2d, _ray, _crosses, _outline, support)
struct Seg2 { PT p, q, n; double u0, u1; bool meta; };
inline std::vector<double> cluster(std::vector<double> v, double eps) {
    std::sort(v.begin(), v.end()); std::vector<double> out, cur;
    for (double x : v) { if (!cur.empty() && x - cur[0] > eps) { out.push_back(cur[cur.size() / 2]); cur.clear(); } cur.push_back(x); }
    if (!cur.empty()) out.push_back(cur[cur.size() / 2]);
    return out;
}
inline std::vector<Seg2> segments2d(const std::vector<Seg>& segs, const std::vector<Surf*>& offaxis, const Frame2& F, double ext) {
    std::vector<Seg2> out;
    for (auto& s : segs) {
        int oi, ti; segLine(s.k, oi, ti); double a[2], b[2], n[2] = {0, 0};
        a[oi] = b[oi] = s.off; a[ti] = s.t0 - ext; b[ti] = s.t1 + ext; n[oi] = SIGN[s.k];
        double L = s.t1 - s.t0 + 2 * ext;
        out.push_back({{a[0], a[1]}, {b[0], b[1]}, {n[0], n[1]}, ext / L, 1 - ext / L, hasScene(s)});
    }
    for (auto* s : offaxis) {
        double c[2], r[2], n[2]; F.to(s->p[0], s->p[2], c[0], c[1]); F.to(s->r[0], s->r[2], r[0], r[1]); F.to(s->n[0], s->n[2], n[0], n[1]);
        double hw = s->w / 2 + ext;
        out.push_back({{c[0] - r[0] * hw, c[1] - r[1] * hw}, {c[0] + r[0] * hw, c[1] + r[1] * hw}, {n[0], n[1]}, ext / (2 * hw), 1 - ext / (2 * hw), true});
    }
    return out;
}
struct Hit { bool hit = false; double t = 0; bool facing = false, metaBack = false; };
inline Hit ray(PT c, PT d, const std::vector<Seg2>& walls) {
    Hit best;
    for (auto& w : walls) {
        double ex = w.q.first - w.p.first, ey = w.q.second - w.p.second, den = d.first * ey - d.second * ex;
        if (std::fabs(den) < 1e-12) continue;
        double wx = w.p.first - c.first, wy = w.p.second - c.second, t = (wx * ey - wy * ex) / den, u = (wx * d.second - wy * d.first) / den;
        if (t > 1e-6 && -1e-9 <= u && u <= 1 + 1e-9 && (!best.hit || t < best.t)) {
            bool facing = w.n.first * d.first + w.n.second * d.second < 0;
            if (!facing && !(w.u0 <= u && u <= w.u1)) continue;
            best = {true, t, facing, w.meta && !facing && w.u0 <= u && u <= w.u1};
        }
    }
    return best;
}
inline bool crosses(PT c1, PT c2, const std::vector<Seg2>& walls) {
    for (auto& w : walls) {
        auto [px, py] = w.p; auto [qx, qy] = w.q;
        double d1 = (qx - px) * (c1.second - py) - (qy - py) * (c1.first - px), d2 = (qx - px) * (c2.second - py) - (qy - py) * (c2.first - px);
        double d3 = (c2.first - c1.first) * (py - c1.second) - (c2.second - c1.second) * (px - c1.first), d4 = (c2.first - c1.first) * (qy - c1.second) - (c2.second - c1.second) * (qx - c1.first);
        if (d1 * d2 < 0 && d3 * d4 <= 0) return true;
    }
    return false;
}
inline double loopArea(const Loop& L) { double a = 0; for (size_t i = 0; i < L.size(); i++) a += L[i].first * L[(i + 1) % L.size()].second - L[(i + 1) % L.size()].first * L[i].second; return a / 2; }
inline bool insideAB(PT c, const Loop& poly) { std::vector<std::pair<double, double>> p(poly.begin(), poly.end()); return inside(c.first, c.second, p); }
inline std::vector<Loop> outline(const std::set<std::pair<int, int>>& cells, const std::vector<double>& xs, const std::vector<double>& ys) {
    std::vector<PT> order; std::map<PT, std::vector<PT>> edges;
    for (auto [i, j] : cells) {
        double x0 = xs[i], x1 = xs[i + 1], y0 = ys[j], y1 = ys[j + 1];
        std::pair<int, int> nb[4] = {{i, j - 1}, {i + 1, j}, {i, j + 1}, {i - 1, j}}; PT a[4] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}, b[4] = {{x1, y0}, {x1, y1}, {x0, y1}, {x0, y0}};
        for (int e = 0; e < 4; e++) if (!cells.count(nb[e])) { if (!edges.count(a[e])) order.push_back(a[e]); edges[a[e]].push_back(b[e]); }
    }
    std::vector<Loop> loops;
    for (;;) {
        PT start; bool found = false; for (auto& k : order) if (edges.count(k)) { start = k; found = true; break; }
        if (!found) break;
        Loop loop = {start}; PT cur = start;
        for (;;) {
            PT nxt = edges[cur].back(); edges[cur].pop_back();
            if (edges[cur].empty()) edges.erase(cur);
            if (nxt == start) break;
            loop.push_back(nxt); cur = nxt;
            if (!edges.count(cur)) break;
        }
        Loop clean;
        for (size_t idx = 0; idx < loop.size(); idx++) {
            PT v = loop[idx], a = loop[(idx + loop.size() - 1) % loop.size()], b = loop[(idx + 1) % loop.size()];
            if (std::fabs((v.first - a.first) * (b.second - v.second) - (v.second - a.second) * (b.first - v.first)) > 1e-12) clean.push_back(v);
        }
        if (clean.size() >= 3) loops.push_back(clean);
    }
    std::stable_sort(loops.begin(), loops.end(), [](const Loop& a, const Loop& b) { return std::fabs(loopArea(a)) > std::fabs(loopArea(b)); });
    return loops;
}
inline std::vector<RoomF> roomsByRays(const std::vector<Seg>& segs, const std::vector<Surf*>& offaxis, const Frame2& F, const Params& P, const std::vector<Loop>& exclude = {}) {
    if (segs.empty()) return {};
    std::vector<double> A, B;
    for (auto& s : segs) {
        int oi, ti; segLine(s.k, oi, ti); (oi == 0 ? A : B).push_back(s.off);
        double e = P.doorMax / 2; auto& T = oi == 0 ? B : A; T.push_back(s.t0); T.push_back(s.t1); T.push_back(s.t0 - e); T.push_back(s.t1 + e);
    }
    for (auto* s : offaxis) {
        double c[2], r[2]; F.to(s->p[0], s->p[2], c[0], c[1]); F.to(s->r[0], s->r[2], r[0], r[1]); double hw = s->w / 2;
        for (int e : {-1, 1}) { A.push_back(c[0] + r[0] * hw * e); B.push_back(c[1] + r[1] * hw * e); }
    }
    std::vector<double> xs = cluster(A, P.cellMerge), ys = cluster(B, P.cellMerge); const double m = 0.5;
    xs.insert(xs.begin(), xs.front() - m); xs.push_back(xs.back() + m); ys.insert(ys.begin(), ys.front() - m); ys.push_back(ys.back() + m);
    std::vector<Seg2> closed = segments2d(segs, offaxis, F, P.doorMax / 2);
    std::vector<std::pair<int, int>> order; std::map<std::pair<int, int>, int> status;
    for (int i = 0; i + 1 < (int)xs.size(); i++) for (int j = 0; j + 1 < (int)ys.size(); j++) {
        PT c{(xs[i] + xs[i + 1]) / 2, (ys[j] + ys[j + 1]) / 2};
        bool ex = false; for (auto& poly : exclude) if (insideAB(c, poly)) { ex = true; break; }
        if (ex) continue;
        Hit h[4] = {ray(c, {1, 0}, closed), ray(c, {-1, 0}, closed), ray(c, {0, 1}, closed), ray(c, {0, -1}, closed)};
        bool back = false; int n = 0, front = 0;
        for (auto& x : h) { if (x.hit && !x.facing && !x.metaBack) back = true; if (x.hit) n++; if (x.hit && x.facing) front++; }
        if (back) continue;
        if ((n == 4 && front >= 2) || (n == 3 && front == 3)) { status[{i, j}] = n; order.push_back({i, j}); }
    }
    auto centre = [&](int i, int j) { return PT{(xs[i] + xs[i + 1]) / 2, (ys[j] + ys[j + 1]) / 2}; };
    std::vector<RoomF> out; std::set<std::pair<int, int>> seen;
    for (auto cell : order) {
        if (seen.count(cell)) continue;
        std::set<std::pair<int, int>> comp; std::vector<std::pair<int, int>> todo = {cell}; seen.insert(cell);
        while (!todo.empty()) {
            auto [i, j] = todo.back(); todo.pop_back(); comp.insert({i, j});
            for (std::pair<int, int> nb : {std::pair<int, int>{i + 1, j}, {i - 1, j}, {i, j + 1}, {i, j - 1}})
                if (status.count(nb) && !seen.count(nb) && !crosses(centre(i, j), centre(nb.first, nb.second), closed)) { seen.insert(nb); todo.push_back(nb); }
        }
        double cellarea = 0, full = 0;
        for (auto [i, j] : comp) { double a = (xs[i + 1] - xs[i]) * (ys[j + 1] - ys[j]); cellarea += a; if (status[{i, j}] == 4) full += a; }
        if (cellarea < P.minRoomArea || full < 0.3 * cellarea) continue;
        RoomF r; r.loops = outline(comp, xs, ys); r.area = cellarea; r.open = full < cellarea - 1e-6; out.push_back(r);
    }
    std::stable_sort(out.begin(), out.end(), [](const RoomF& a, const RoomF& b) { return a.area > b.area; });
    return out;
}
struct Gap { PT a, b; double w; };
struct Support { double perimeter = 0, supported = 0, closure = 0; std::vector<Gap> doors, holes, opens; std::set<int> bounding; };  // bounding: seg indices
inline bool atWallEnd(PT p, const std::vector<PT>& ends, double tol = 0.15) { for (auto& e : ends) if (std::fabs(p.first - e.first) <= tol && std::fabs(p.second - e.second) <= tol) return true; return false; }
inline Support support(const RoomF& room, const std::vector<Seg>& segs, const Params& P) {
    Support S; std::vector<PT> ends;
    struct Real { int i, oi, ti; double t0, t1; }; std::vector<Real> real;
    for (int i = 0; i < (int)segs.size(); i++) { int oi, ti; segLine(segs[i].k, oi, ti); real.push_back({i, oi, ti, segs[i].t0 + segs[i].ext[0], segs[i].t1 - segs[i].ext[1]}); }
    for (auto& r : real) for (double t : {r.t0, r.t1}) { double e[2]; e[r.oi] = segs[r.i].off; e[r.ti] = t; ends.push_back({e[0], e[1]}); }
    for (auto& L : room.loops) for (size_t idx = 0; idx < L.size(); idx++) {
        PT a = L[idx], b = L[(idx + 1) % L.size()];
        bool horiz = std::fabs(a.second - b.second) < 1e-9; int oi = horiz ? 1 : 0, ti = horiz ? 0 : 1;
        double av[2] = {a.first, a.second}, bv[2] = {b.first, b.second};
        double lo = std::min(av[ti], bv[ti]), hi = std::max(av[ti], bv[ti]); S.perimeter += hi - lo;
        double d[2] = {bv[0] - av[0], bv[1] - av[1]}, inward[2] = {-d[1], d[0]};
        std::vector<std::pair<double, double>> iv;
        for (auto& r : real) {
            const Seg& s = segs[r.i];
            if (r.oi != oi || std::fabs(s.off - av[oi]) > 0.08) continue;
            double nrm[2] = {0, 0}; nrm[r.oi] = SIGN[s.k];
            if (nrm[0] * inward[0] + nrm[1] * inward[1] <= 0 && !hasScene(s)) continue;
            double x0 = std::max(lo, r.t0), x1 = std::min(hi, r.t1);
            if (x1 > x0) { iv.push_back({x0, x1}); S.bounding.insert(r.i); }
        }
        std::sort(iv.begin(), iv.end()); std::vector<std::pair<double, double>> merged;
        for (auto& [x0, x1] : iv) { if (!merged.empty() && x0 <= merged.back().second + 1e-6) merged.back().second = std::max(merged.back().second, x1); else merged.push_back({x0, x1}); }
        for (auto& [x0, x1] : merged) S.supported += x1 - x0;
        double cur = lo; merged.push_back({hi, hi});
        for (auto& [x0, x1] : merged) {
            if (x0 - cur > 1e-6) {
                double g0 = cur, g1 = x0, gl = g1 - g0; double p0[2], p1[2]; p0[oi] = p1[oi] = av[oi]; p0[ti] = g0; p1[ti] = g1;
                Gap g{{p0[0], p0[1]}, {p1[0], p1[1]}, gl};
                if (P.doorMin <= gl && gl <= P.doorMax + 0.02 && atWallEnd(g.a, ends) && atWallEnd(g.b, ends)) S.doors.push_back(g);
                else if (gl < P.doorMin) S.holes.push_back(g);
                else S.opens.push_back(g);
            }
            cur = std::max(cur, x1);
        }
    }
    double openLen = 0; for (auto& g : S.opens) openLen += g.w;
    S.closure = S.perimeter ? 1.0 - openLen / S.perimeter : 0.0;
    return S;
}

inline std::vector<Seg> outsideSceneRooms(const std::vector<Seg>& segs, const Frame2& F, const Params& P, Result& R) {
    std::vector<const Seg*> scene; for (auto& s : segs) if (hasScene(s)) scene.push_back(&s);
    std::vector<Surf*> surfs; for (auto* s : scene) for (auto* m : s->members) surfs.push_back(m);
    for (auto* s : R.offaxis) surfs.push_back(s);
    std::vector<std::vector<std::pair<double, double>>> inner; R.metaRooms.clear();
    for (auto& c : metaCycles(surfs)) {
        Loop loop; for (auto& [x, z] : c.poly) { double a, b; F.to(x, z, a, b); loop.push_back({a, b}); }
        double ar = loopArea(loop); if (ar < 0) std::reverse(loop.begin(), loop.end());
        RoomF r; r.loops = {loop}; r.area = std::fabs(ar); r.meta = true; r.metaIds = c.ids; R.metaRooms.push_back(r);
    }
    if (R.metaRooms.empty() && !scene.empty()) {  // no closed chain: Meta's rooms from its walls by the rays
        R.needsRooms = true; std::vector<Seg> sc; for (auto* s : scene) sc.push_back(*s);
        R.metaRooms = roomsByRays(sc, R.offaxis, F, P);
    }
    for (auto& r : R.metaRooms) { std::vector<std::pair<double, double>> poly; for (auto& [a, b] : r.loops[0]) { double x, z; F.back(a, b, x, z); poly.push_back({x, z}); } inner.push_back(poly); }
    if (inner.empty() || scene.size() == segs.size()) return segs;
    auto isIn = [&](double a, double b) { double x, z; F.back(a, b, x, z); for (auto& p : inner) if (inside(x, z, p)) return true; return false; };
    std::vector<Seg> out;
    for (auto& s : segs) {
        if (hasScene(s)) { out.push_back(s); continue; }
        int oi, ti; segLine(s.k, oi, ti);
        int n = std::max(2, (int)((s.t1 - s.t0) / 0.05) + 1);
        std::vector<bool> keep;
        for (int i = 0; i < n; i++) {
            double t = s.t0 + (s.t1 - s.t0) * i / (n - 1); bool all = true;
            for (double dn : {-0.1, 0.0, 0.1}) { double c[2]; c[oi] = s.off + dn; c[ti] = t; if (!isIn(c[0], c[1])) { all = false; } }
            keep.push_back(!all);
        }
        bool allKeep = true; for (bool k : keep) allKeep = allKeep && k;
        if (allKeep) { out.push_back(s); continue; }
        std::vector<std::pair<int, int>> pieces; int start = -1; keep.push_back(false);
        for (int i = 0; i < (int)keep.size(); i++) {
            if (keep[i] && start < 0) start = i;
            else if (!keep[i] && start >= 0) { pieces.push_back({start, i - 1}); start = -1; }
        }
        double cut = 0;
        for (auto& [i0, i1] : pieces) {
            double t0 = s.t0 + (s.t1 - s.t0) * i0 / (n - 1), t1 = s.t0 + (s.t1 - s.t0) * i1 / (n - 1);
            if (t1 - t0 >= P.minLen) { Seg piece = s; piece.t0 = t0; piece.t1 = t1; piece.ext[0] = piece.ext[1] = 0; out.push_back(piece); cut += t1 - t0; }
        }
        if (cut == 0.0) for (auto* m : s.members) R.dropped.push_back({m->id, "inside a Meta room (not a wall of the house)"});
    }
    return out;
}

// consolidate.py _rooms_and_walls: Meta's rooms plus the rooms the depth walls make outside them, each with its support,
// floor and ceiling (horizontal surfaces inside, else the house's), and the plan walls with their room's heights.
inline double wmedian(std::vector<std::pair<double, double>> v) {
    std::sort(v.begin(), v.end()); double tot = 0; for (auto& x : v) tot += x.second;
    double acc = 0; for (auto& x : v) { acc += x.second; if (acc >= tot / 2) return x.first; }
    return v.empty() ? 0.0 : v.back().first;
}
inline void roomsAndWalls(Result& R, const Frame2& F, const Params& P) {
    std::vector<Loop> excl; for (auto& r : R.metaRooms) excl.push_back(r.loops[0]);
    std::vector<RoomF> found = R.metaRooms; for (auto& r : roomsByRays(R.segs, R.offaxis, F, P, excl)) found.push_back(r);
    std::stable_sort(found.begin(), found.end(), [](const RoomF& a, const RoomF& b) { return a.area > b.area; });
    std::map<int, std::string> owner;   // seg index, or -1 - offaxis index -> room id
    R.rooms.clear(); R.walls.clear();
    for (size_t n = 0; n < found.size(); n++) {
        const RoomF& r = found[n]; Support sp; std::set<int> bounding;
        if (r.meta) {
            const Loop& L = r.loops[0]; double per = 0;
            for (size_t i = 0; i < L.size(); i++) { const PT& a = L[(i + L.size() - 1) % L.size()]; per += std::hypot(L[i].first - a.first, L[i].second - a.second); }
            sp.perimeter = sp.supported = per; sp.closure = 1.0;
            for (int i = 0; i < (int)R.segs.size(); i++) for (auto* m : R.segs[i].members) if (std::find(r.metaIds.begin(), r.metaIds.end(), m->id) != r.metaIds.end()) { bounding.insert(i); break; }
            for (int j = 0; j < (int)R.offaxis.size(); j++) if (std::find(r.metaIds.begin(), r.metaIds.end(), R.offaxis[j]->id) != r.metaIds.end()) bounding.insert(-1 - j);
        } else { sp = support(r, R.segs, P); bounding = sp.bounding; }
        std::vector<Loop> polys; for (auto& L : r.loops) { Loop q; for (auto& [a, b] : L) { double x, z; F.back(a, b, x, z); q.push_back({x, z}); } polys.push_back(q); }
        std::vector<std::pair<double, double>> poly(polys[0].begin(), polys[0].end());
        std::vector<Surf*> ups, dns;
        for (auto* s : R.surfs) if ((s->orient == 1 || s->orient == 2) && inside(s->p[0], s->p[2], poly)) (s->orient == 1 ? ups : dns).push_back(s);
        PlanRoom pr; pr.id = "pr" + std::to_string(n + 1);
        if (!ups.empty()) {
            double lo = 1e300; for (auto* s : ups) lo = std::min(lo, s->p[1]);
            std::vector<std::pair<double, double>> v; for (auto* s : ups) if (s->p[1] <= lo + 0.08) v.push_back({s->p[1], s->w * s->h});
            pr.floorY = wmedian(v); pr.haveFloor = true;
        } else if (R.haveFloor) { pr.floorY = R.refFloor; pr.haveFloor = true; }
        if (!dns.empty()) { double m = -1e300; for (auto* s : dns) m = std::max(m, s->p[1]); pr.ceilingY = m; pr.haveCeil = true; }
        else if (R.haveCeil) { pr.ceilingY = R.refCeil; pr.haveCeil = true; }
        // owner.setdefault in segs + offaxis order
        for (int i = 0; i < (int)R.segs.size(); i++) if (bounding.count(i) && !owner.count(i)) owner[i] = pr.id;
        for (int j = 0; j < (int)R.offaxis.size(); j++) if (bounding.count(-1 - j) && !owner.count(-1 - j)) owner[-1 - j] = pr.id;
        pr.floor = polys[0]; for (size_t i = 1; i < polys.size(); i++) pr.holes.push_back(polys[i]);
        pr.area = r.area; pr.closed = !r.open && sp.opens.empty(); pr.closure = sp.closure; pr.perimeter = sp.perimeter;
        for (auto& g : sp.opens) pr.openM += g.w;
        pr.doors = (int)sp.doors.size();
        for (auto& g : sp.opens) { double ax, az, bx, bz; F.back(g.a.first, g.a.second, ax, az); F.back(g.b.first, g.b.second, bx, bz); pr.opens.push_back({{ax, az}, {bx, bz}}); }
        R.rooms.push_back(pr);
    }
    std::set<std::string> used;
    for (int i = 0; i < (int)R.segs.size(); i++) {
        const Seg& s = R.segs[i]; PlanWall w;
        Surf* best = s.members[0]; for (auto* m : s.members) if (weight(m) > weight(best)) best = m;
        w.id = best->id.rfind("w-", 0) == 0 ? "p-" + best->id.substr(2) : "p-" + best->id;
        while (used.count(w.id)) w.id += "x";
        used.insert(w.id);
        auto o = owner.find(i); if (o != owner.end()) w.room = o->second;
        double y0 = s.y0, y1 = s.y1;
        for (auto& pr : R.rooms) if (pr.id == w.room && pr.haveFloor && pr.haveCeil) { y0 = pr.floorY; y1 = pr.ceilingY; }
        double x, z, yaw, wd; R.wall(s, x, z, yaw, wd);
        w.x = x; w.y = (y0 + y1) / 2; w.z = z; w.yaw = yaw; w.w = wd; w.h = y1 - y0;
        int sc = 0; for (auto* m : s.members) { w.from.push_back(m->id); sc += m->src != "depth"; }
        w.src = sc && sc == (int)s.members.size() ? "scene" : sc ? "scene+depth" : "depth";
        R.walls.push_back(w);
    }
    for (int j = 0; j < (int)R.offaxis.size(); j++) {
        Surf* s = R.offaxis[j]; PlanWall w; w.id = "p-" + (s->id.size() > 2 ? s->id.substr(2) : std::string());
        w.x = s->p[0]; w.y = s->p[1]; w.z = s->p[2]; w.yaw = s->yaw; w.w = s->w; w.h = s->h; w.offAxis = true; w.from = {s->id}; w.src = s->src;
        auto o = owner.find(-1 - j); if (o != owner.end()) w.room = o->second;
        R.walls.push_back(w);
    }
}

// consolidate.py's score: duplicates, closure, the depth evidence explained, Meta-vs-depth distance, back-to-back
// walls, lone walls, frames dropped -> the verdict and its reasons (the same strings).
inline double r3(double v) { return std::round(v * 1000.0) / 1000.0; }
inline int duplicates(const std::vector<std::array<double, 7>>& v) {  // p.x, p.z, n.x, n.z, r.x, r.z, w
    int n = 0; double c10 = std::cos(rad(10));
    for (size_t i = 0; i < v.size(); i++) for (size_t j = i + 1; j < v.size(); j++) {
        const auto &a = v[i], &b = v[j];
        if (a[2] * b[2] + a[3] * b[3] < c10) continue;
        double dist = std::fabs((b[0] - a[0]) * a[2] + (b[1] - a[1]) * a[3]); if (dist > 0.35) continue;
        double ta = a[0] * a[4] + a[1] * a[5], tb = b[0] * a[4] + b[1] * a[5];
        double ov = std::min(ta + a[6] / 2, tb + b[6] / 2) - std::max(ta - a[6] / 2, tb - b[6] / 2);
        if (ov >= 0.5 * std::min(a[6], b[6])) n++;
    }
    return n;
}
inline void score(Result& R, const Params& P) {
    Score& S = R.score; S = Score{};
    const double okClosure = 0.9, okExplained = 0.6, okSceneFit = 0.05;
    long depthTotal = 0; std::set<std::string> kept;
    for (auto& s : R.segs) for (auto* m : s.members) kept.insert(m->id);
    long expl = 0; for (auto& s : R.rawVert) if (s.src == "depth") { depthTotal += s.samples; if (kept.count(s.id)) expl += s.samples; }
    if (depthTotal) { S.haveExplained = true; S.explained = (double)expl / depthTotal; }
    std::vector<double> fits; double cs = std::cos(rad(P.snapDeg));
    for (auto* m : R.surfs) {
        if (m->orient != 0 || m->src == "depth") continue;
        bool have = false; double best = 0;
        for (auto* s : R.surfs) {
            if (s->orient != 0 || s->src != "depth" || !s->drop.empty()) continue;
            if (m->n[0] * s->n[0] + m->n[2] * s->n[2] < cs) continue;
            double dist = std::fabs((s->p[0] - m->p[0]) * m->n[0] + (s->p[2] - m->p[2]) * m->n[2]);
            double ta = m->p[0] * m->r[0] + m->p[2] * m->r[2], tb = s->p[0] * m->r[0] + s->p[2] * m->r[2];
            double ov = std::min(ta + m->w / 2, tb + s->w / 2) - std::max(ta - m->w / 2, tb - s->w / 2);
            if (dist <= P.stackOff && ov >= 0.3 && (!have || dist < best)) { best = dist; have = true; }
        }
        if (have) fits.push_back(best);
    }
    std::sort(fits.begin(), fits.end()); S.pairs = (int)fits.size();
    if (!fits.empty()) { S.haveMedian = true; S.median = r3(fits[fits.size() / 2]); }
    std::vector<std::array<double, 7>> rawV, planV;
    for (auto& s : R.rawVert) rawV.push_back({s.p[0], s.p[2], s.n[0], s.n[2], s.r[0], s.r[2], s.w});
    for (auto& w : R.walls) {
        if (w.offAxis) { for (auto* s : R.offaxis) if ("p-" + (s->id.size() > 2 ? s->id.substr(2) : std::string()) == w.id) { planV.push_back({s->p[0], s->p[2], s->n[0], s->n[2], s->r[0], s->r[2], s->w}); break; } }
        else planV.push_back({w.x, w.z, std::sin(w.yaw), std::cos(w.yaw), std::cos(w.yaw), -std::sin(w.yaw), w.w});
    }
    S.dupRaw = duplicates(rawV); S.dupPlan = duplicates(planV);
    bool ok = true; auto& rs = S.reasons; char b[512];
    if (R.rooms.empty()) { ok = false; rs.push_back("no closed room: walls do not enclose any floor yet"); }
    for (auto& r : R.rooms) {
        double closure = r3(r.closure), per = r3(r.perimeter), openM = r3(r.openM);
        if (!r.closed && closure >= okClosure) { ok = false; rs.push_back(r.id + ": not closed (seen from inside, a side has no wall: scan the rest of it)"); }
        if (closure < okClosure) { ok = false; snprintf(b, sizeof b, ": open %.1f m of %.1f m (closure %.2f < %g)", openM, per, closure, okClosure); rs.push_back(r.id + b); }
    }
    if (S.dupPlan) { ok = false; rs.push_back(std::to_string(S.dupPlan) + " duplicate wall pairs left"); }
    if (S.haveExplained && S.explained < okExplained) { ok = false; snprintf(b, sizeof b, "the plan explains %.0f%% of the depth evidence (< %.0f%%)", S.explained * 100, okExplained * 100); rs.push_back(b); }
    if (S.haveMedian && S.median > okSceneFit) { ok = false; snprintf(b, sizeof b, "depth walls sit %.0f cm from the Meta walls (median, > %.0f cm)", S.median * 100, okSceneFit * 100); rs.push_back(b); }
    for (auto& s : R.segs) {
        if (hasScene(s)) continue;
        for (auto& t : R.segs) if (hasScene(t) && t.k == (s.k + 2) % 4 && overlap(s, t) >= 0.5) {
            double gap = SIGN[s.k] * (s.off - t.off);
            if (-0.05 <= gap && gap <= 0.3) { std::string meta; for (auto* m : t.members) if (m->src != "depth") { meta = m->id; break; } S.b2b.push_back({s.members[0]->id, meta, r3(gap)}); }
        }
    }
    bool anyD = false, anyM = false; for (auto& s : R.rawVert) (s.src == "depth" ? anyD : anyM) = true;
    if (anyD && anyM && fits.empty() && S.b2b.empty()) { ok = false; rs.push_back("no depth wall meets a Meta wall (same face or back to back): the two cannot be cross-checked (another frame?)"); }
    std::string thin;
    for (auto& x : S.b2b) if (x.thickness < 0.06) { snprintf(b, sizeof b, "%s is %.0f cm behind Meta %s", x.depth.c_str(), x.thickness * 100, x.meta.c_str()); thin += (thin.empty() ? "" : "; ") + std::string(b); }
    if (!thin.empty()) { ok = false; rs.push_back(thin + ": thinner than a real wall (6+ cm), so the scan and Meta disagree by a few cm (frame offset?)"); }
    for (auto& w : R.walls) if (w.room.empty() && !w.offAxis && r3(w.w) >= 1.0) S.lone.push_back(w.id);
    if (!S.lone.empty()) {
        ok = false; std::string l; for (auto& id : S.lone) l += (l.empty() ? "" : ", ") + id;
        rs.push_back(std::to_string(S.lone.size()) + " wall(s) of 1 m or more bound no closed room (a room is open or unscanned): " + l);
    }
    std::string bad; int nb = 0;
    for (auto& [bt, st] : R.batches) if (st.rfind("dropped", 0) == 0) { bad += (nb ? "; " : "") + bt + ": " + st; nb++; }
    S.badFrames = nb;
    if (nb) { ok = false; rs.push_back(std::to_string(nb) + " scan commit(s) in another frame: " + bad); }
    S.ok = ok;
}

// The live SCAN hint (work-df: guided capture): the verdict as one short line that says where to look, from the head
// (room frame: position and forward). "ROOM: GOOD", or "NOT YET: ..." from the first reason that applies, in the
// order a user can fix them: no room at all, an open side (a door-sized gap: "DOORWAY ... UNCLEAR", else "WALL ...
// UNSEEN"), a wall bounding no room, double walls, mostly furniture, scan vs Meta, an old scan in another frame.
inline std::string where(double hx, double hz, double fx, double fz, double tx, double tz) {
    double dx = tx - hx, dz = tz - hz, ahead = dx * fx + dz * fz, right = dx * -fz + dz * fx;
    double a = deg(std::atan2(right, ahead));
    return std::fabs(a) <= 45 ? "AHEAD" : std::fabs(a) >= 135 ? "BEHIND YOU" : a > 0 ? "TO YOUR RIGHT" : "TO YOUR LEFT";
}
struct Hint { bool good = false; std::string text; };
inline Hint hint(const Result& R, double hx, double hz, double fx, double fz, const Params& P = {}) {
    Hint h; const Score& S = R.score;
    if (S.ok) { h.good = true; h.text = "ROOM: GOOD"; return h; }
    if (R.rooms.empty()) { h.text = "NOT YET: LOOK AROUND, NO ROOM IS CLOSED YET"; return h; }
    // The room with the most open length: a missing wall comes as several short stretches along the cell outline, so
    // their total says doorway (up to doorMax + 0.5 m) or wall, and their length-weighted centre says where.
    const PlanRoom* worst = nullptr; double worstOpen = 0;
    for (auto& r : R.rooms) { double o = 0; for (auto& [a, b] : r.opens) o += std::hypot(b.first - a.first, b.second - a.second); if (o > worstOpen) { worstOpen = o; worst = &r; } }
    if (worst) {
        double cx = 0, cz = 0;
        for (auto& [a, b] : worst->opens) { double w = std::hypot(b.first - a.first, b.second - a.second); cx += w * (a.first + b.first) / 2; cz += w * (a.second + b.second) / 2; }
        std::string dir = where(hx, hz, fx, fz, cx / worstOpen, cz / worstOpen);
        h.text = worstOpen <= P.doorMax + 0.5 ? "NOT YET: DOORWAY " + dir + " UNCLEAR" : "NOT YET: WALL " + dir + " UNSEEN"; return h;
    }
    if (!S.lone.empty()) for (auto& w : R.walls) if (w.id == S.lone[0]) { h.text = "NOT YET: THE WALL " + where(hx, hz, fx, fz, w.x, w.z) + " CLOSES NO ROOM, LOOK PAST IT"; return h; }
    if (S.dupPlan) { h.text = "NOT YET: DOUBLE WALLS, SWEEP THEM AGAIN SLOWLY"; return h; }
    if (S.haveExplained && S.explained < 0.6) { h.text = "NOT YET: MOSTLY FURNITURE, AIM AT THE WALLS"; return h; }
    if (S.badFrames) { h.text = "NOT YET: AN OLD SCAN IS IN ANOTHER FRAME"; return h; }
    h.text = "NOT YET: SCAN AND ROOM SETUP DISAGREE"; return h;
}

// Phases 0-3 on a house document; withPlan: phase 2 too (the floor plan: rooms and plan walls) and the score.
inline void run(const json::Value& doc, Result& R, const Params& P = {}, bool withPlan = false) {
    R = Result{};
    std::vector<Surf*> raw; surfaces(doc, R, raw);
    for (auto* s : raw) if (s->orient == 0) R.rawVert.push_back(*s);
    std::vector<Surf*> surfs = frames(raw, P, R); R.surfs = surfs;
    axesPeak(surfs, P, R.th, R.share);
    Frame2 F(R.th);
    std::vector<Seg> segs = makeSegments(surfs, F, P, R);
    segs = mergeColinear(segs, F, P);
    segs = suppressStacks(segs, P, R);
    segs = clipCorners(segs, P, R);
    R.segs = outsideSceneRooms(segs, F, P, R);
    if (withPlan) { roomsAndWalls(R, F, P); score(R, P); }
}

}  // namespace consolidate
