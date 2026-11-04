// wallfit.h: planes (walls, floor, ceiling) fitted to the headset's environment depth, fused across frames, matched
// to the house file's walls (house/README.md), plus the per-screen depth gap (how far a screen sits in front of or
// behind the real surface). Header-only C++17, no dependencies; tested on the VPS with synthetic depth
// (bench/wallfit_test.cpp).
//
// Input is what XR_META_environment_depth gives room_lab: one depth view (a layer of the depth texture array) as raw
// texture values d in [0,1], rows in GL order (row 0 = bottom, as glReadPixels returns them), with that view's fov,
// pose, nearZ and farZ (farZ may be infinite). Depth is linearised exactly like room_lab's occlusion shader:
// zn = 2d-1, z = 2fn/((f+n) - zn(f-n)), or z = 2n/(1-zn) when far is infinite. All poses share one space (the space
// the depth views are located in); house walls must be given in that same space.
//
// Per frame: a grid of up to ~40k points (any frame size is subsampled to that), normals from a local PCA over a
// 5x5 grid window (depth edges excluded), then planes one at a time by normal-aided RANSAC (one point + its normal
// is a hypothesis) and a least-squares refit. Frames fuse into tracked planes (same orientation, normal within
// fuseDeg, offset within fuseDist, overlapping): the tracked plane is the least-squares plane of every point that
// ever supported it, and its extent is a 5 cm occupancy grid unioned over frames, so an occluder seen past from
// another viewpoint shrinks nothing.
//
// Wall frame (same as Placement and house.json): origin at the rect centre, +z = the normal into the room (toward the
// viewer), +y up, +x right as seen facing the wall.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <unordered_map>
#include <vector>

namespace wallfit {

struct V2 { float x = 0, y = 0; };
struct V3 { float x = 0, y = 0, z = 0; };
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float len(V3 a) { return std::sqrt(dot(a, a)); }
inline V3 norm(V3 a) { float l = len(a); return l > 1e-12f ? a * (1.0f / l) : V3{0, 0, 0}; }

struct Quat { float x = 0, y = 0, z = 0, w = 1; };
struct Pose { Quat q; V3 p; };
inline Quat conj(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }
inline V3 rotate(Quat q, V3 v) {
    V3 u{q.x, q.y, q.z}; V3 t = cross(u, v) * 2.0f;
    return v + t * q.w + cross(u, t);
}
// Rotation whose columns are the orthonormal axes x, y, z.
inline Quat fromAxes(V3 x, V3 y, V3 z) {
    float m00 = x.x, m11 = y.y, m22 = z.z, tr = m00 + m11 + m22; Quat q;
    if (tr > 0) { float s = std::sqrt(tr + 1.0f) * 2; q = {(y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, 0.25f * s}; }
    else if (m00 > m11 && m00 > m22) { float s = std::sqrt(1.0f + m00 - m11 - m22) * 2; q = {0.25f * s, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s}; }
    else if (m11 > m22) { float s = std::sqrt(1.0f + m11 - m00 - m22) * 2; q = {(y.x + x.y) / s, 0.25f * s, (z.y + y.z) / s, (z.x - x.z) / s}; }
    else { float s = std::sqrt(1.0f + m22 - m00 - m11) * 2; q = {(z.x + x.z) / s, (z.y + y.z) / s, 0.25f * s, (x.y - y.x) / s}; }
    float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w); return {q.x / l, q.y / l, q.z / l, q.w / l};
}
inline float angleDeg(V3 a, V3 b) { return std::acos(std::clamp(dot(norm(a), norm(b)), -1.0f, 1.0f)) * 57.29578f; }

// Field of view as XrFovf: angles in radians, left and down negative.
struct Fov { float left = -0.8f, right = 0.8f, up = 0.8f, down = -0.8f; };

struct DepthFrame {
    int w = 0, h = 0;
    const uint16_t* d16 = nullptr;  // raw depth (d * 65535), or
    const float* d01 = nullptr;     // raw depth in [0,1]
    Fov fov; Pose pose; float nearZ = 0.1f, farZ = std::numeric_limits<float>::infinity();
    int64_t timeNs = 0;             // when the depth was captured (XrTime); 0 = unknown, never dropped as stale

    float raw(int x, int y) const { size_t i = (size_t)y * w + x; return d16 ? d16[i] * (1.0f / 65535.0f) : d01[i]; }
    // Metres along the view axis at pixel (x, y), 0 where there is no depth.
    float viewZ(int x, int y) const {
        float d = raw(x, y);
        if (!(d > 0.0f && d < 1.0f)) return 0;
        float zn = 2 * d - 1, n = nearZ, f = farZ;
        float z = (!std::isfinite(f) || f <= 0) ? 2 * n / std::max(1 - zn, 1e-7f) : 2 * f * n / ((f + n) - zn * (f - n));
        return z > n && z < 30.0f ? z : 0;
    }
    // The world point at pixel (x, y) (its centre) and view depth z.
    V3 point(float x, float y, float z) const {
        float tl = std::tan(fov.left), tr = std::tan(fov.right), td = std::tan(fov.down), tu = std::tan(fov.up);
        V3 v{(tl + (x + 0.5f) / w * (tr - tl)) * z, (td + (y + 0.5f) / h * (tu - td)) * z, -z};
        return pose.p + rotate(pose.q, v);
    }
    // World point -> pixel (integer, in range) and its view depth; false when behind the eye or outside the image.
    bool project(V3 P, int& px, int& py, float& z) const {
        V3 v = rotate(conj(pose.q), P - pose.p);
        if (v.z > -1e-3f) return false;
        z = -v.z;
        float tl = std::tan(fov.left), tr = std::tan(fov.right), td = std::tan(fov.down), tu = std::tan(fov.up);
        float u = (v.x / z - tl) / (tr - tl) * w, t = (v.y / z - td) / (tu - td) * h;
        if (!(u >= 0 && u < w && t >= 0 && t < h)) return false;
        px = (int)u; py = (int)t; return true;
    }
};

// Symmetric 3x3 eigen decomposition (Jacobi): values ascending, vecs[i] the unit eigenvector of values[i].
inline void eigen3(const double a_[6], double values[3], V3 vecs[3]) {
    double a[3][3] = {{a_[0], a_[1], a_[2]}, {a_[1], a_[3], a_[4]}, {a_[2], a_[4], a_[5]}};
    double v[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int sweep = 0; sweep < 24; sweep++) {
        double off = std::fabs(a[0][1]) + std::fabs(a[0][2]) + std::fabs(a[1][2]);
        if (off < 1e-18) break;
        for (int p = 0; p < 2; p++)
            for (int q = p + 1; q < 3; q++) {
                if (std::fabs(a[p][q]) < 1e-30) continue;
                double th = (a[q][q] - a[p][p]) / (2 * a[p][q]);
                double t = (th >= 0 ? 1 : -1) / (std::fabs(th) + std::sqrt(th * th + 1)), c = 1 / std::sqrt(t * t + 1), s = t * c;
                for (int k = 0; k < 3; k++) { double akp = a[k][p], akq = a[k][q]; a[k][p] = c * akp - s * akq; a[k][q] = s * akp + c * akq; }
                for (int k = 0; k < 3; k++) { double apk = a[p][k], aqk = a[q][k]; a[p][k] = c * apk - s * aqk; a[q][k] = s * apk + c * aqk; }
                for (int k = 0; k < 3; k++) { double vkp = v[k][p], vkq = v[k][q]; v[k][p] = c * vkp - s * vkq; v[k][q] = s * vkp + c * vkq; }
            }
    }
    int idx[3] = {0, 1, 2};
    std::sort(idx, idx + 3, [&](int i, int j) { return a[i][i] < a[j][j]; });
    for (int i = 0; i < 3; i++) { values[i] = a[idx[i]][idx[i]]; vecs[i] = {(float)v[0][idx[i]], (float)v[1][idx[i]], (float)v[2][idx[i]]}; }
}

// Running scatter of points (sums in doubles, relative to a fixed origin for precision): the least-squares plane.
struct Scatter {
    V3 o; bool hasOrigin = false; double n = 0, s[3] = {0, 0, 0}, ss[6] = {0, 0, 0, 0, 0, 0};
    void add(V3 p) {
        if (!hasOrigin) { o = p; hasOrigin = true; }
        double x = p.x - o.x, y = p.y - o.y, z = p.z - o.z;
        n++; s[0] += x; s[1] += y; s[2] += z;
        ss[0] += x * x; ss[1] += x * y; ss[2] += x * z; ss[3] += y * y; ss[4] += y * z; ss[5] += z * z;
    }
    void merge(const Scatter& b) {
        if (!b.n) return;
        if (!hasOrigin) { *this = b; return; }
        // shift b's sums to this origin
        double dx = b.o.x - o.x, dy = b.o.y - o.y, dz = b.o.z - o.z, m = b.n;
        double bs[3] = {b.s[0] + m * dx, b.s[1] + m * dy, b.s[2] + m * dz};
        ss[0] += b.ss[0] + 2 * dx * b.s[0] + m * dx * dx; ss[1] += b.ss[1] + dx * b.s[1] + dy * b.s[0] + m * dx * dy;
        ss[2] += b.ss[2] + dx * b.s[2] + dz * b.s[0] + m * dx * dz; ss[3] += b.ss[3] + 2 * dy * b.s[1] + m * dy * dy;
        ss[4] += b.ss[4] + dy * b.s[2] + dz * b.s[1] + m * dy * dz; ss[5] += b.ss[5] + 2 * dz * b.s[2] + m * dz * dz;
        s[0] += bs[0]; s[1] += bs[1]; s[2] += bs[2]; n += m;
    }
    V3 centroid() const { return n ? V3{(float)(o.x + s[0] / n), (float)(o.y + s[1] / n), (float)(o.z + s[2] / n)} : o; }
    // plane normal (smallest-variance axis) and rms distance of the points to the plane
    void plane(V3& normal, float& rms) const {
        double m[3] = {s[0] / n, s[1] / n, s[2] / n};
        double c[6] = {ss[0] / n - m[0] * m[0], ss[1] / n - m[0] * m[1], ss[2] / n - m[0] * m[2],
                       ss[3] / n - m[1] * m[1], ss[4] / n - m[1] * m[2], ss[5] / n - m[2] * m[2]};
        double val[3]; V3 vec[3]; eigen3(c, val, vec);
        normal = vec[0]; rms = (float)std::sqrt(std::max(0.0, val[0]));
    }
};

enum class Kind { wall, floor, ceiling, plane };
inline const char* kindName(Kind k) { return k == Kind::wall ? "wall" : k == Kind::floor ? "floor" : k == Kind::ceiling ? "ceiling" : "plane"; }
inline Kind classify(V3 n) {
    if (n.y > 0.966f) return Kind::floor;     // within 15 degrees of up
    if (n.y < -0.966f) return Kind::ceiling;
    if (std::fabs(n.y) < 0.259f) return Kind::wall;
    return Kind::plane;
}
// Wall-frame axes for a normal: +y up for walls; for floors and ceilings +y points along world -z (any fixed choice).
inline void axesFor(V3 n, V3& right, V3& up) {
    V3 ref = std::fabs(n.y) < 0.9f ? V3{0, 1, 0} : V3{0, 0, -1};
    up = norm(ref - n * dot(ref, n)); right = cross(up, n);
}

// Defaults tuned on real Quest 3 environment depth (house/fixtures/depth-47041720: ~1.5% depth noise, a wall at 2.6 m
// fits with rms 25-40 mm): a 7x7 normal window, curvature up to 0.3, 25 degree normal gate, inlier band 1.2 cm + 3% of
// depth. With them both eyes of the real frame agree on that wall within 1 degree / 9 mm; the synthetic suite passes at
// 0.5%, 1% and 1.5% noise.
struct Params {
    int targetPoints = 20000;    // grid points per frame (the frame is subsampled to about this many)
    int normalHalf = 3;          // normal window: (2*normalHalf+1)^2 grid points
    float edgeRel = 0.04f;       // a neighbour further than this fraction of depth (+ 2 cm) is across a depth edge
    float maxCurvature = 0.3f;   // smallest / total eigenvalue of the normal window: above this, not a flat spot
    float inlier = 0.012f;       // point-to-plane distance for an inlier: inlier + inlierRel * depth (m); depth noise
    float inlierRel = 0.03f;     // grows with distance, so the band does too
    float normalDeg = 25;        // point normal vs plane normal for an inlier
    int iterations = 60;         // RANSAC hypotheses per plane
    int scoreSample = 1000;      // points scored per hypothesis
    int maxPlanes = 12;          // per frame
    int minInliers = 400;        // per plane per frame
    float minArea = 0.15f;       // m^2 of occupied cells per plane per frame
    float fuseDeg = 4, fuseDist = 0.04f, fuseMargin = 0.5f;  // a frame's plane joins a tracked one within these
    int64_t maxAgeNs = 500000000;  // Fuser::add drops frames older than this (depth lags the head pose)
    float cell = 0.05f;          // occupancy cell (m)
    int cellMinHits = 2;         // points in a cell before it counts as part of the surface
    unsigned seed = 12345;
};

// One plane seen in one frame.
struct Observation { V3 n; float d = 0; V3 centroid; Scatter sc; std::vector<V3> pts; int inliers = 0; };

// The planes in one frame. Points/normals are kept for inspection (tests).
struct FrameFit {
    std::vector<Observation> planes; int gridW = 0, gridH = 0, stride = 1, validPoints = 0;
};

inline FrameFit fitFrame(const DepthFrame& f, const Params& P = {}) {
    FrameFit out;
    if (f.w <= 0 || f.h <= 0 || (!f.d16 && !f.d01)) return out;
    int s = std::max(1, (int)std::ceil(std::sqrt((double)f.w * f.h / std::max(1, P.targetPoints)) - 1e-9));
    int gw = f.w / s, gh = f.h / s; out.stride = s; out.gridW = gw; out.gridH = gh;
    std::vector<V3> pt(gw * gh); std::vector<float> zs(gw * gh, 0);
    for (int j = 0; j < gh; j++)
        for (int i = 0; i < gw; i++) {
            int x = i * s + s / 2, y = j * s + s / 2; float z = f.viewZ(x, y);
            zs[j * gw + i] = z; if (z > 0) pt[j * gw + i] = f.point((float)x, (float)y, z);
        }
    // normals by local PCA, oriented toward the eye
    std::vector<V3> nr(gw * gh); std::vector<char> ok(gw * gh, 0); std::vector<int> cand;
    int k = P.normalHalf;
    for (int j = 0; j < gh; j++)
        for (int i = 0; i < gw; i++) {
            float zc = zs[j * gw + i]; if (zc <= 0) continue;
            Scatter sc; int cnt = 0;
            for (int b = -k; b <= k; b++)
                for (int a = -k; a <= k; a++) {
                    int ii = i + a, jj = j + b; if (ii < 0 || jj < 0 || ii >= gw || jj >= gh) continue;
                    float z = zs[jj * gw + ii]; if (z <= 0 || std::fabs(z - zc) > zc * P.edgeRel + 0.02f) continue;
                    sc.add(pt[jj * gw + ii]); cnt++;
                }
            if (cnt < (2 * k + 1) * (2 * k + 1) * 2 / 3) continue;
            double m[3] = {sc.s[0] / sc.n, sc.s[1] / sc.n, sc.s[2] / sc.n};
            double c[6] = {sc.ss[0] / sc.n - m[0] * m[0], sc.ss[1] / sc.n - m[0] * m[1], sc.ss[2] / sc.n - m[0] * m[2],
                           sc.ss[3] / sc.n - m[1] * m[1], sc.ss[4] / sc.n - m[1] * m[2], sc.ss[5] / sc.n - m[2] * m[2]};
            double val[3]; V3 vec[3]; eigen3(c, val, vec);
            double tot = val[0] + val[1] + val[2]; if (tot <= 0 || val[0] / tot > P.maxCurvature) continue;
            V3 n = vec[0]; if (dot(n, f.pose.p - pt[j * gw + i]) < 0) n = n * -1.0f;
            nr[j * gw + i] = n; ok[j * gw + i] = 1; cand.push_back(j * gw + i);
        }
    out.validPoints = (int)cand.size();
    std::vector<float> tol(gw * gh);
    for (int c : cand) tol[c] = P.inlier + P.inlierRel * zs[c];
    std::mt19937 rng(P.seed);
    float cosN = std::cos(P.normalDeg / 57.29578f);
    std::vector<char> used(gw * gh, 0);
    for (int plane = 0; plane < P.maxPlanes; plane++) {
        std::vector<int> rem; rem.reserve(cand.size());
        for (int c : cand) if (!used[c]) rem.push_back(c);
        if ((int)rem.size() < P.minInliers) break;
        int bestScore = 0; V3 bn; float bd = 0;
        std::uniform_int_distribution<size_t> pick(0, rem.size() - 1);
        int sample = std::min<int>(P.scoreSample, (int)rem.size());
        for (int it = 0; it < P.iterations; it++) {
            int c = rem[pick(rng)]; V3 n = nr[c]; float d = -dot(n, pt[c]); int score = 0;
            for (int t = 0; t < sample; t++) {
                int q = rem[pick(rng)];
                if (std::fabs(dot(n, pt[q]) + d) < tol[q] && dot(n, nr[q]) > cosN) score++;
            }
            if (score > bestScore) { bestScore = score; bn = n; bd = d; }
        }
        if (!bestScore) break;
        // refit twice: inliers of the current plane -> least squares plane
        std::vector<int> in;
        for (int pass = 0; pass < 3; pass++) {
            in.clear();
            for (int q : rem) if (std::fabs(dot(bn, pt[q]) + bd) < tol[q] && dot(bn, nr[q]) > cosN) in.push_back(q);
            if ((int)in.size() < 3) break;
            Scatter sc; for (int q : in) sc.add(pt[q]);
            V3 n; float rms; sc.plane(n, rms); if (dot(n, bn) < 0) n = n * -1.0f;
            bn = n; bd = -dot(n, sc.centroid());
        }
        if ((int)in.size() < P.minInliers) {
            for (int q : in) used[q] = 1;  // too small to report; don't let it seed again
            if (bestScore * (int)rem.size() / std::max(1, sample) < P.minInliers) break;
            continue;
        }
        Observation o; o.n = bn; o.d = bd; o.inliers = (int)in.size();
        for (int q : in) { used[q] = 1; o.sc.add(pt[q]); o.pts.push_back(pt[q]); }
        o.centroid = o.sc.centroid();
        // area: occupied cells in the plane
        V3 r, u; axesFor(bn, r, u);
        std::unordered_map<int64_t, int> cells;
        for (auto& p : o.pts) {
            V3 v = p - o.centroid;
            int64_t cx = (int64_t)std::floor(dot(v, r) / P.cell), cy = (int64_t)std::floor(dot(v, u) / P.cell);
            cells[(cx << 32) ^ (cy & 0xffffffff)]++;
        }
        int occ = 0; for (auto& kv : cells) occ += kv.second >= P.cellMinHits;
        if (occ * P.cell * P.cell < P.minArea) continue;
        out.planes.push_back(std::move(o));
    }
    return out;
}

// A fused plane in the wall frame convention.
struct Wall {
    V3 n; float d = 0;              // plane: dot(n, x) + d = 0, n into the room (toward where it was seen from)
    V3 centre, right, up;           // rect centre (on the plane) and axes; +z = n
    float w = 0, h = 0;             // rect extents (m)
    float rms_m = 0;                // rms distance of all supporting points to the plane
    long samples = 0; int frames = 0;
    float confidence = 0;           // 0..1: samples, frames, flatness and area together
    Kind kind = Kind::plane;
    int id = 0;                     // stable per Fuser
    Pose pose() const { return {fromAxes(right, up, n), centre}; }
};

struct Fuser {
    Params P;
    struct Track {
        int id; Scatter sc; V3 n; float d; V3 origin, bx, by; int frames = 0;
        std::unordered_map<int64_t, int> cells;
    };
    std::vector<Track> tracks; int nextId = 1; long dropped = 0;
    explicit Fuser(Params p = {}) : P(p) {}

    static int64_t key(int64_t x, int64_t y) { return (x << 32) ^ (y & 0xffffffff); }

    // Fit one frame and fuse it; returns the frame's own planes. nowNs (XrTime now, 0 = don't check): a frame older
    // than maxAgeNs is dropped unfitted (counted in dropped).
    FrameFit add(const DepthFrame& f, int64_t nowNs = 0) {
        if (nowNs && f.timeNs && nowNs - f.timeNs > P.maxAgeNs) { dropped++; return {}; }
        FrameFit ff = fitFrame(f, P); for (auto& o : ff.planes) fuse(o);
        mergeTracks(); return ff;
    }

    // Two tracks of one surface (e.g. both started from partial first views) become one once their fits agree.
    void mergeTracks() {
        float cosF = std::cos(P.fuseDeg / 57.29578f);
        for (bool merged = true; merged;) {
            merged = false;
            for (size_t i = 0; i < tracks.size() && !merged; i++)
                for (size_t j = i + 1; j < tracks.size() && !merged; j++) {
                    Track &a = tracks[i], &b = tracks[j];
                    if (dot(a.n, b.n) < cosF) continue;
                    V3 cb = b.sc.centroid(), ca = a.sc.centroid();
                    if (0.5f * (std::fabs(dot(a.n, cb) + a.d) + std::fabs(dot(b.n, ca) + b.d)) > P.fuseDist) continue;
                    float ax0, ax1, ay0, ay1, bx0, bx1, by0, by1;
                    if (!box(a, ax0, ax1, ay0, ay1) || !box(b, bx0, bx1, by0, by1)) continue;
                    // b's box corners in a's basis must come within the margin of a's box
                    float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
                    for (float u : {bx0, bx1})
                        for (float v : {by0, by1}) {
                            V3 c = b.origin + b.bx * u + b.by * v - a.origin; float x = dot(c, a.bx), y = dot(c, a.by);
                            lo[0] = std::min(lo[0], x); hi[0] = std::max(hi[0], x); lo[1] = std::min(lo[1], y); hi[1] = std::max(hi[1], y);
                        }
                    float m = P.fuseMargin;
                    if (lo[0] > ax1 + m || hi[0] < ax0 - m || lo[1] > ay1 + m || hi[1] < ay0 - m) continue;
                    absorb(a, b); tracks.erase(tracks.begin() + j); merged = true;
                }
        }
    }

    void absorb(Track& a, const Track& b) {
        for (auto& kv : b.cells) {
            int64_t cx = kv.first >> 32, cy = (int32_t)(kv.first & 0xffffffff);
            V3 c = b.origin + b.bx * ((cx + 0.5f) * P.cell) + b.by * ((cy + 0.5f) * P.cell) - a.origin;
            a.cells[key((int64_t)std::floor(dot(c, a.bx) / P.cell), (int64_t)std::floor(dot(c, a.by) / P.cell))] += kv.second;
        }
        a.sc.merge(b.sc); a.frames += b.frames;
        V3 n; float rms; a.sc.plane(n, rms); if (dot(n, a.n) < 0) n = n * -1.0f;
        a.n = n; a.d = -dot(n, a.sc.centroid());
    }

    void fuse(const Observation& o) {
        float cosF = std::cos(P.fuseDeg / 57.29578f);
        Track* best = nullptr; float bestScore = 1e9f;
        for (auto& t : tracks) {
            if (dot(o.n, t.n) < cosF) continue;
            float off = std::fabs(dot(t.n, o.centroid) + t.d); if (off > P.fuseDist) continue;
            // overlap: the observation's centroid within the track's occupied box plus a margin
            float x0, x1, y0, y1; if (!box(t, x0, x1, y0, y1)) continue;
            V3 v = o.centroid - t.origin; float cx = dot(v, t.bx), cy = dot(v, t.by);
            float mx = P.fuseMargin, half = 0;
            for (auto& p : o.pts) { V3 q = p - o.centroid; half = std::max(half, std::max(std::fabs(dot(q, t.bx)), std::fabs(dot(q, t.by)))); }
            if (cx < x0 - half - mx || cx > x1 + half + mx || cy < y0 - half - mx || cy > y1 + half + mx) continue;
            float score = off / P.fuseDist + (1 - dot(o.n, t.n)); if (score < bestScore) { bestScore = score; best = &t; }
        }
        if (!best) {
            Track t; t.id = nextId++; t.n = o.n; t.d = o.d; t.origin = o.centroid; axesFor(o.n, t.bx, t.by);
            tracks.push_back(t); best = &tracks.back();
        }
        best->sc.merge(o.sc); best->frames++;
        V3 n; float rms; best->sc.plane(n, rms); if (dot(n, best->n) < 0) n = n * -1.0f;
        best->n = n; best->d = -dot(n, best->sc.centroid());
        for (auto& p : o.pts) {
            V3 v = p - best->origin;
            best->cells[key((int64_t)std::floor(dot(v, best->bx) / P.cell), (int64_t)std::floor(dot(v, best->by) / P.cell))]++;
        }
    }

    bool box(const Track& t, float& x0, float& x1, float& y0, float& y1) const {
        bool any = false; x0 = y0 = 1e9f; x1 = y1 = -1e9f;
        for (auto& kv : t.cells) {
            if (kv.second < P.cellMinHits) continue;
            int64_t cx = kv.first >> 32, cy = (int32_t)(kv.first & 0xffffffff);  // undo key()
            x0 = std::min(x0, cx * P.cell); x1 = std::max(x1, (cx + 1) * P.cell);
            y0 = std::min(y0, cy * P.cell); y1 = std::max(y1, (cy + 1) * P.cell); any = true;
        }
        return any;
    }

    Wall wallOf(const Track& t) const {
        Wall w; w.id = t.id; w.n = t.n; w.d = t.d; w.frames = t.frames; w.samples = (long)t.sc.n; w.kind = classify(t.n);
        V3 n; t.sc.plane(n, w.rms_m);
        // axes: the track's basis made orthogonal to the current normal (the box was measured in it)
        w.up = norm(t.by - t.n * dot(t.by, t.n)); w.right = cross(w.up, t.n);
        float x0, x1, y0, y1;
        if (box(t, x0, x1, y0, y1)) {
            V3 c = t.origin + t.bx * ((x0 + x1) / 2) + t.by * ((y0 + y1) / 2);
            w.centre = c - t.n * (dot(t.n, c) + t.d); w.w = x1 - x0; w.h = y1 - y0;
        } else w.centre = t.sc.centroid();
        int occ = 0; for (auto& kv : t.cells) occ += kv.second >= P.cellMinHits;
        float area = occ * P.cell * P.cell;
        // heuristic: enough points, seen in several frames, flat (rms includes sensor noise, ~1 cm at 3 m), big enough
        w.confidence = (1 - std::exp(-w.samples / 3000.0f)) * (1 - std::exp(-w.frames / 2.0f)) *
                       std::clamp(1 - w.rms_m / 0.06f, 0.0f, 1.0f) * std::min(1.0f, area / 0.5f);
        return w;
    }

    std::vector<Wall> walls() const { std::vector<Wall> v; for (auto& t : tracks) v.push_back(wallOf(t)); return v; }
};

// ---- the house file

// A wall of house.json in the depth views' space: pose (rect centre, +z into the room) and extents (0 = unknown).
struct HouseWall { Pose pose; float w = 0, h = 0; };
struct Match { int index = -1; float angleDeg = 0, offset = 0; };

// The house wall this fitted plane is: same facing, normal within maxDeg, each centre within maxDist of the other's
// plane, and the rects overlapping (with margin). -1: none, i.e. a wall the house does not have yet.
inline Match match(const Wall& f, const std::vector<HouseWall>& house, float maxDist = 0.03f, float maxDeg = 2.0f, float margin = 0.25f) {
    Match best; float bestScore = 1e9f;
    for (size_t i = 0; i < house.size(); i++) {
        const HouseWall& h = house[i];
        V3 hn = rotate(h.pose.q, {0, 0, 1}), hx = rotate(h.pose.q, {1, 0, 0}), hy = rotate(h.pose.q, {0, 1, 0});
        float ang = angleDeg(f.n, hn); if (ang > maxDeg) continue;
        float off = 0.5f * (std::fabs(dot(hn, f.centre - h.pose.p)) + std::fabs(dot(f.n, h.pose.p) + f.d));
        if (off > maxDist) continue;
        if (h.w > 0 && h.h > 0) {
            V3 v = f.centre - h.pose.p;
            if (std::fabs(dot(v, hx)) > (h.w + f.w) / 2 + margin || std::fabs(dot(v, hy)) > (h.h + f.h) / 2 + margin) continue;
        }
        float score = off / maxDist + ang / maxDeg;
        if (score < bestScore) { bestScore = score; best = {(int)i, ang, off}; }
    }
    return best;
}

// ---- screens: how far a screen sits from the real surface behind it

struct Gap {
    float median = 0, p10 = 0, p90 = 0, valid = 0; int n = 0;
    // corner medians (top-left, top-right, bottom-left, bottom-right; NAN when unseen): one side sunk and the other
    // floating is a yaw / relocalisation error (re-anchor); all four moved together is an offset (the plane is off)
    float tl = NAN, tr = NAN, bl = NAN, br = NAN;
};
inline Gap summarize(std::vector<float> g, int asked) {
    Gap r; r.n = (int)g.size(); r.valid = asked ? (float)g.size() / asked : 0;
    if (g.empty()) return r;
    std::sort(g.begin(), g.end());
    auto at = [&](float q) { return g[std::min(g.size() - 1, (size_t)(q * (g.size() - 1) + 0.5f))]; };
    r.median = at(0.5f); r.p10 = at(0.1f); r.p90 = at(0.9f); return r;
}

// Screen at wall-2D (x, y) (its centre), size w x h, on the wall with pose wallPose. Samples a 5x5 grid over the rect
// (inset 10%), reads the depth there, and returns the signed distance along the wall normal from the screen plane to
// the measured surface: > 0 the real surface is in front of the screen (the screen is sunk into the wall), < 0 the
// screen floats in front of it. Points the depth view does not see are skipped (valid = the fraction seen).
inline Gap screenGap(const DepthFrame& f, const Pose& wallPose, float x, float y, float w, float h) {
    V3 n = rotate(wallPose.q, {0, 0, 1}); std::vector<float> g, c[4]; int asked = 0;
    for (int b = 0; b < 5; b++)
        for (int a = 0; a < 5; a++) {
            float sx = x + (a / 4.0f - 0.5f) * 0.8f * w, sy = y + (b / 4.0f - 0.5f) * 0.8f * h;
            V3 P = wallPose.p + rotate(wallPose.q, {sx, sy, 0}); asked++;
            int px, py; float zp; if (!f.project(P, px, py, zp)) continue;
            float zm = f.viewZ(px, py); if (zm <= 0) continue;
            V3 Q = f.pose.p + (P - f.pose.p) * (zm / zp);  // the measured surface along the eye -> P ray
            float gap = dot(n, Q - P); g.push_back(gap);
            if (a != 2 && b != 2) c[(b > 2 ? 0 : 2) + (a > 2 ? 1 : 0)].push_back(gap);  // 2x2 corner sub-grids
        }
    Gap r = summarize(g, asked);
    float* out[4] = {&r.tl, &r.tr, &r.bl, &r.br};
    for (int i = 0; i < 4; i++) if (!c[i].empty()) *out[i] = summarize(c[i], 4).median;
    return r;
}

// The same against a fused plane: signed distance along the plane normal from the screen's points to the plane.
inline Gap screenGapPlane(const Wall& fit, const Pose& wallPose, float x, float y, float w, float h) {
    std::vector<float> g; float corner[4] = {NAN, NAN, NAN, NAN};
    for (int b = 0; b < 3; b++)
        for (int a = 0; a < 3; a++) {
            V3 P = wallPose.p + rotate(wallPose.q, {x + (a / 2.0f - 0.5f) * w, y + (b / 2.0f - 0.5f) * h, 0});
            float gap = -(dot(fit.n, P) + fit.d); g.push_back(gap);
            if (a != 1 && b != 1) corner[(b == 2 ? 0 : 2) + (a == 2 ? 1 : 0)] = gap;
        }
    Gap r = summarize(g, 9); r.tl = corner[0]; r.tr = corner[1]; r.bl = corner[2]; r.br = corner[3];
    return r;
}

}  // namespace wallfit
