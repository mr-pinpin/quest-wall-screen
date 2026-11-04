// The frame SCAN commits and MESHSAVE use (2026-11-04, work-34's consolidation): commits drifted against Meta's floor,
// +6 cm (02:15), +16 cm (03:03), +26 cm (09:32). The walls were fitted well (1.3 cm in LOCAL); the frame was the
// problem. Each commit used the frame solved from our own anchors, while Meta's scene walls keep the poses stored
// at the first commit. Our anchors and Meta's scene do not move together across relocalisations, so every later
// commit landed in a frame shifted against the stored scene. Now the frame comes from Meta's scene walls and floor
// themselves, wherever the house already has their poses: each located scene plane with a stored pose is one estimate
// (anchors::solve, the same consensus as our anchors; at least 2 must agree). Our anchors are the fallback (first
// commit, or no stored scene planes located). floorCheck() refuses a commit whose fitted floor is more than 5 cm off
// Meta's floor in the same frame. GL-free; bench/roomframe_test.cpp.
#pragma once
#include "anchors.h"
#include <cmath>
#include <string>
#include <vector>

namespace roomframe {
using wallfit::Pose; using wallfit::V3;

struct Result {
    bool ok = false; Pose roomInLocal; std::string source = "none";   // "scene", "anchors" or "none"
    int used = 0;                       // planes (or anchors) that agreed
    float anchorsVsSceneM = -1, anchorsVsSceneDeg = -1;   // drift between the two frames when both exist
};

// scene: stored = the house's pose of each Meta plane (centre pose, room frame), seen = the same planes located now
// (centre pose, LOCAL); anchors: the anchors' own solve.
inline Result choose(const std::vector<anchors::Stored>& sceneStored, const std::vector<anchors::Seen>& sceneSeen, const anchors::Frame& anchorFrame) {
    Result r;
    anchors::Frame s = anchors::solve(sceneStored, sceneSeen, 0.05f, 2.0f);
    if (s.ok && s.used >= 2) { r.ok = true; r.roomInLocal = s.roomInLocal; r.source = "scene"; r.used = s.used; }
    else if (anchorFrame.ok) { r.ok = true; r.roomInLocal = anchorFrame.roomInLocal; r.source = "anchors"; r.used = anchorFrame.used; }
    if (s.ok && s.used >= 2 && anchorFrame.ok) {
        r.anchorsVsSceneM = wallfit::len(s.roomInLocal.p - anchorFrame.roomInLocal.p); r.anchorsVsSceneDeg = anchors::angleDeg(s.roomInLocal.q, anchorFrame.roomInLocal.q);
    }
    return r;
}

// A LOCAL pose expressed in the room frame: what gets written to the house.
inline Pose toRoom(const Pose& roomInLocal, const Pose& local) { return anchors::compose(anchors::inverse(roomInLocal), local); }

// The fitted floor height against Meta's, both in LOCAL (y up): ok when within tolM, or when either is unknown.
struct FloorCheck { bool ok = true; float offM = 0; bool known = false; };
inline FloorCheck floorCheck(const std::vector<float>& fittedFloorY, float metaFloorY, bool metaKnown, float tolM = 0.05f) {
    FloorCheck c; if (!metaKnown || fittedFloorY.empty()) return c;
    // The floor is the largest, lowest flat area; the fitter can also report low furniture tops: take the lowest.
    float y = fittedFloorY[0]; for (float v : fittedFloorY) y = std::min(y, v);
    c.known = true; c.offM = y - metaFloorY; c.ok = std::fabs(c.offM) <= tolM; return c;
}
}  // namespace roomframe
