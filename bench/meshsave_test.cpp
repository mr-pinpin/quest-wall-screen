// MESHSAVE (room_lab/meshsave.h): two sessions in different LOCAL frames build one room mesh. Session 1 looks at half
// the room and saves; session 2 (Meta's LOCAL origin elsewhere) loads that and looks at the other half; every vertex
// must lie on the true room surfaces (no doubled or shifted walls), and both halves must be there. Fusing session 2's
// frames in LOCAL instead (what v0.63 and older did) is the control: its walls land off the true ones.
//   g++ -std=c++17 -O2 -I room_lab -I bench bench/meshsave_test.cpp && ./a.out
#include "meshsave.h"
#include "roomframe.h"
#include <cstdio>
#include <random>
using wallfit::DepthFrame; using wallfit::Quat; using wallfit::V3; using wallfit::Pose;
static int fails = 0;
static void check(bool c, const char* m) { if (!c) { printf("FAIL %s\n", m); fails++; } }
struct Box { V3 lo, hi; };
static const Box ROOM{{-2, 0, -3}, {2, 2.5f, 1.5f}};
static float surfaceDist(V3 p) {
    return std::min({std::fabs(p.x - ROOM.lo.x), std::fabs(p.x - ROOM.hi.x), std::fabs(p.y - ROOM.lo.y), std::fabs(p.y - ROOM.hi.y),
                     std::fabs(p.z - ROOM.lo.z), std::fabs(p.z - ROOM.hi.z)});
}
static Quat yaw(float deg) { float a = deg / 57.29578f / 2; return {0, std::sin(a), 0, std::cos(a)}; }
struct Synth {   // depth of the empty room box, rendered from a pose in the ROOM frame
    int w = 128, h = 128; std::vector<uint16_t> d16; unsigned seed = 3;
    DepthFrame render(Pose eyeInRoom) {
        DepthFrame f; f.w = w; f.h = h; f.fov = {-0.785f, 0.785f, 0.785f, -0.785f}; f.pose = eyeInRoom; f.nearZ = 0.1f;
        d16.assign(w * h, 0); std::mt19937 rng(seed++); std::normal_distribution<float> N(0, 1);
        float tl = std::tan(f.fov.left), tr = std::tan(f.fov.right), td = std::tan(f.fov.down), tu = std::tan(f.fov.up);
        V3 o = eyeInRoom.p;
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            V3 D = wallfit::rotate(eyeInRoom.q, {tl + (x + 0.5f) / w * (tr - tl), td + (y + 0.5f) / h * (tu - td), -1});
            float t = 1e9f; const float oo[3] = {o.x, o.y, o.z}, d[3] = {D.x, D.y, D.z};
            const float lo[3] = {ROOM.lo.x, ROOM.lo.y, ROOM.lo.z}, hi[3] = {ROOM.hi.x, ROOM.hi.y, ROOM.hi.z};
            for (int a = 0; a < 3; a++) if (std::fabs(d[a]) > 1e-9f) t = std::min(t, ((d[a] > 0 ? hi[a] : lo[a]) - oo[a]) / d[a]);
            float z = t * (1 + 0.003f * N(rng)), dd = 1 - f.nearZ / z;
            d16[y * w + x] = (uint16_t)std::lround(std::clamp(dd, 0.0f, 1.0f) * 65535);
        }
        f.d16 = d16.data(); return f;
    }
};
// Feed one session: the headset sees LOCAL poses (roomInLocal * eyeInRoom); toRoom undoes that with the solved frame.
static void session(roommesh::Volume& v, Synth& s, const Pose& roomInLocal, const Pose& solvedRoom, float yaw0, float yaw1, bool inRoom) {
    for (float a = yaw0; a <= yaw1; a += 15) {
        Pose eyeInRoom{yaw(a), {0, 1.5f, -0.7f}};
        DepthFrame f = s.render(eyeInRoom); f.pose = anchors::compose(roomInLocal, eyeInRoom);   // as the headset reports it
        v.integrate(inRoom ? meshsave::toRoom(f, solvedRoom) : f); v.update(1e9);
    }
}
static void stats(const roommesh::Volume& v, float& worst, float& meanD, size_t& verts, float& minX, float& maxX) {
    worst = 0; double sum = 0; verts = 0; minX = 1e9f; maxX = -1e9f;
    v.forEachMesh([&](const roommesh::Key&, const roommesh::BlockMesh& m) {
        for (size_t i = 0; i + 2 < m.pos.size(); i += 3) {
            V3 p{m.pos[i], m.pos[i + 1], m.pos[i + 2]}; float d = surfaceDist(p);
            worst = std::max(worst, d); sum += d; verts++; minX = std::min(minX, p.x); maxX = std::max(maxX, p.x);
        }
    });
    meanD = verts ? (float)(sum / verts) : 0;
}
int main() {
    roommesh::Params P; P.voxel = 0.05f; Synth s;
    // Session 1: Meta's LOCAL origin here; anchors solved 4 mm / 0.2 deg off the truth. Look left (+yaw = toward -x).
    Pose A{yaw(30), {1.2f, -0.3f, 0.8f}}, Asolved = A; Asolved.p = Asolved.p + V3{0.004f, 0, -0.003f};
    roommesh::Volume v1(P); session(v1, s, A, Asolved, 20, 100, true);
    std::string blob = v1.save();
    float worst, meanD, minX, maxX; size_t n1, n2;
    stats(v1, worst, meanD, n1, minX, maxX);
    check(n1 > 500 && meanD < 0.02f, "session 1 mesh on the room surfaces");
    check(minX < -1.9f && maxX < 1.5f, "session 1 saw the left side only");
    // Session 2: LOCAL origin 3 m away and turned 100 deg (a relocalised start); load, then look right.
    Pose B{yaw(-70), {-2.5f, 0.1f, 3.0f}};
    // Its frame as room_lab now picks it (roomframe.h): our anchors drifted 10 cm down against Meta's scene, so the
    // frame comes from Meta's planes stored in the house (here: the four walls and the floor of the box).
    const float s2 = std::sqrt(0.5f);
    std::vector<anchors::Stored> planes = {{"n", {yaw(0), {0, 1.25f, -3}}}, {"s", {yaw(180), {0, 1.25f, 1.5f}}}, {"e", {yaw(-90), {2, 1.25f, -0.75f}}},
                                           {"w", {yaw(90), {-2, 1.25f, -0.75f}}}, {"f", {{-s2, 0, 0, s2}, {0, 0, -0.75f}}}};
    std::vector<anchors::Seen> seenB; for (auto& pl : planes) seenB.push_back({pl.uuid, anchors::compose(B, pl.inRoom)});
    anchors::Frame drifted; drifted.ok = true; drifted.used = 3; drifted.roomInLocal = anchors::compose(B, Pose{yaw(0.5f), {0, -0.10f, 0}});
    roomframe::Result rf = roomframe::choose(planes, seenB, drifted);
    check(rf.source == "scene", "session 2 frame from Meta's planes, not the drifted anchors");
    roommesh::Volume v2(P); check(v2.load(blob), "blob loads"); v2.update(1e9);
    session(v2, s, B, rf.roomInLocal, -100, -20, true);
    stats(v2, worst, meanD, n2, minX, maxX);
    check(meanD < 0.02f && worst < 0.12f, "both sessions agree: every vertex on a true surface");
    check(minX < -1.9f && maxX > 1.9f && n2 > n1, "both halves of the room are in the mesh");
    // The floor against Meta's (y = 0 in the room frame): within 1 cm.
    double fy = 0; size_t fn = 0;
    v2.forEachMesh([&](const roommesh::Key&, const roommesh::BlockMesh& m) { for (size_t i = 0; i + 2 < m.pos.size(); i += 3) if (m.pos[i + 1] < 0.15f && std::fabs(m.pos[i]) < 1.6f && m.pos[i + 2] > -2.6f && m.pos[i + 2] < 1.1f) { fy += m.pos[i + 1]; fn++; } });
    printf("floor mean %.1f mm over %zu verts\n", fy / std::max<size_t>(fn, 1) * 1000, fn);
    check(fn > 100 && std::fabs(fy / fn) < 0.01f, "mesh floor on Meta's floor within 1 cm");
    // Control: the drifted anchors' frame would lift the second session's floor about 10 cm.
    roommesh::Volume v4(P); session(v4, s, B, drifted.roomInLocal, -100, -20, true);
    double gy = 0; size_t gn = 0;
    v4.forEachMesh([&](const roommesh::Key&, const roommesh::BlockMesh& m) { for (size_t i = 0; i + 2 < m.pos.size(); i += 3) if (m.pos[i + 1] < 0.3f && std::fabs(m.pos[i]) < 1.6f && m.pos[i + 2] > -2.6f && m.pos[i + 2] < 1.1f) { gy += m.pos[i + 1]; gn++; } });
    check(gn > 100 && gy / gn > 0.07f, "control: the drifted anchors' frame lifts the floor");
    // Control: the same session 2 fused in LOCAL (the old behaviour) lands its half 3 m away.
    roommesh::Volume v3(P); v3.load(blob); v3.update(1e9); session(v3, s, B, B, -100, -20, false);
    float w3, m3, x0, x1; size_t n3; stats(v3, w3, m3, n3, x0, x1);
    check(m3 > 0.1f, "control: LOCAL fusing would misplace the second session");
    // Policy: a minute and 20 blocks, or anything at all when leaving focus.
    meshsave::Policy po; check(!po.due(0, true), "nothing changed: no save");
    po.changed(5); check(!po.due(100, false) && po.due(100, true), "5 blocks: only on leaving focus");
    po.changed(30); check(po.due(100, false), "35 blocks, first save: due"); po.saved(100);
    po.changed(40); check(!po.due(130, false) && po.due(161, false), "then at most once a minute");
    // A fitted plane moves with the frame: the x = -2 wall seen in session 2's LOCAL comes back to x = -2.
    wallfit::Wall w; w.n = {1, 0, 0}; w.centre = {-2, 1.2f, -0.5f}; w.right = {0, 0, -1}; w.up = {0, 1, 0};
    wallfit::Wall wl = w; wl.centre = B.p + wallfit::rotate(B.q, w.centre); wl.n = wallfit::rotate(B.q, w.n); wl.right = wallfit::rotate(B.q, w.right); wl.up = wallfit::rotate(B.q, w.up);
    wallfit::Wall wr = meshsave::wallToRoom(wl, B);
    check(wallfit::len(wr.centre - w.centre) < 1e-4f && wallfit::dot(wr.n, w.n) > 0.9999f && std::fabs(wr.d - 2) < 1e-4f, "plane back in the room frame");
    // Load source.
    using meshsave::Source;
    check(meshsave::pick("", "") == Source::none && meshsave::pick("", "ab") == Source::local, "no house mesh: local copy");
    check(meshsave::pick("ab", "ab") == Source::local && meshsave::pick("ab", "cd") == Source::house, "house mesh wins when it differs");
    printf("meshsave: %s (session 1 %zu verts, both %zu, mean %.1f mm; control mean %.0f mm)\n", fails ? "FAILED" : "all checks passed", n1, n2, meanD * 1000, m3 * 1000);
    return fails ? 1 : 0;
}
