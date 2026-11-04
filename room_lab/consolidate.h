// House consolidation on the headset (kanban K27, work-0e): a port of work-34's house/consolidate.py ("consolidate/1",
// the same PARAMS), steps 0-3 and Meta's room chains, so SCAN can show clean walls live and use them as placement
// targets instead of raw fits. Stage for stage the same rules and the same arithmetic (doubles, Python's floor modulo
// and round-half-even), so the output matches the Python on the same input: bench/consolidate_xcheck.py runs the Python
// functions to the same stage and bench/consolidate_test.cpp compares, in the offline gate (a change to the Python that
// is not mirrored here fails there). Not ported yet (phase 2): rooms() by rays (used only when Meta's walls form no
// closed chain: Result.needsRooms says so and the depth walls are then not cut by Meta's rooms), the floor plan and the
// score. Input: house.json walls (same fields as the Python reads); room_lab feeds the house's walls plus the live SCAN
// fits as one batch. GL-free.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
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

struct Result {
    double th = 0, share = 0; bool needsRooms = false;
    std::vector<Seg> segs; std::vector<Surf*> offaxis; std::vector<Dropped> dropped;
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
struct Cycle { std::vector<std::pair<double, double>> poly; };
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
        if (ok && walls.size() >= 3) { Cycle c; for (int i : loop) c.poly.push_back(pos[i]); out.push_back(c); }
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
inline std::vector<Seg> outsideSceneRooms(const std::vector<Seg>& segs, const Frame2& F, const Params& P, Result& R) {
    std::vector<const Seg*> scene; for (auto& s : segs) if (hasScene(s)) scene.push_back(&s);
    std::vector<Surf*> surfs; for (auto* s : scene) for (auto* m : s->members) surfs.push_back(m);
    for (auto* s : R.offaxis) surfs.push_back(s);
    std::vector<std::vector<std::pair<double, double>>> inner;
    for (auto& c : metaCycles(surfs)) inner.push_back(c.poly);  // (x, z) already; the Python's F.to / reverse / F.back round-trips them
    if (inner.empty() && !scene.empty()) { R.needsRooms = true; return segs; }  // phase 2: rooms() by rays
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

// The whole of phases 0-3 on a house document.
inline void run(const json::Value& doc, Result& R, const Params& P = {}) {
    R = Result{};
    std::vector<Surf*> raw; surfaces(doc, R, raw);
    std::vector<Surf*> surfs = frames(raw, P, R);
    axesPeak(surfs, P, R.th, R.share);
    Frame2 F(R.th);
    std::vector<Seg> segs = makeSegments(surfs, F, P, R);
    segs = mergeColinear(segs, F, P);
    segs = suppressStacks(segs, P, R);
    segs = clipCorners(segs, P, R);
    R.segs = outsideSceneRooms(segs, F, P, R);
}

}  // namespace consolidate
