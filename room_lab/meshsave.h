// MESHSAVE (default on; the user, 2026-11-04: a whole walk-through of the flat was lost because only SCAN / DONE saved,
// and the GEOMETRY mesh lived in memory in LOCAL coordinates). "Seeing it means it's saved": the room mesh is fused in
// the room frame (r1's own anchors, anchors.h), saved by itself (roommesh save(): the TSDF, so fusing continues after a
// load) every minute while it changes and when the app leaves focus, kept in files/mesh-room.pprm and sent to the house
// as a blob (PUT /blob/<sha256>) named by rooms[].mesh. At start the saved mesh is loaded before any new depth is fused.
// GL-free; bench/meshsave_test.cpp.
#pragma once
#include "anchors.h"
#include "roommesh.h"

namespace meshsave {
using wallfit::Pose;

// A depth frame taken in LOCAL, re-expressed in the room frame (only its pose changes).
inline wallfit::DepthFrame toRoom(wallfit::DepthFrame f, const Pose& roomInLocal) {
    f.pose = anchors::compose(anchors::inverse(roomInLocal), f.pose); return f;
}

// A fitted plane (wallfit, LOCAL) in the room frame, for roomplanes over the room-frame volume.
inline wallfit::Wall wallToRoom(wallfit::Wall w, const Pose& roomInLocal) {
    Pose R = anchors::inverse(roomInLocal);
    w.centre = R.p + wallfit::rotate(R.q, w.centre); w.n = wallfit::rotate(R.q, w.n); w.right = wallfit::rotate(R.q, w.right); w.up = wallfit::rotate(R.q, w.up);
    w.d = -wallfit::dot(w.n, w.centre); return w;
}

// When to save: changed blocks since the last save, at most every `every` s (and only once `minChanged` blocks moved),
// or at once when leaving focus if anything changed at all.
struct Policy {
    double every = 60; size_t minChanged = 20;
    double lastSave = -1e18; size_t changedSince = 0;
    void changed(size_t n) { changedSince += n; }
    bool due(double now, bool leaving) const {
        if (!changedSince) return false;
        return leaving || (now - lastSave >= every && changedSince >= minChanged);
    }
    void saved(double now) { lastSave = now; changedSince = 0; }
};

// The load at start: what to load, given the house's rooms[].mesh sha and the local copy. The house one wins when they
// differ (another headset or a consolidation may have written it); the local copy covers an offline start or a save the
// house never got (no sha in the house yet).
enum class Source { none, local, house };
inline Source pick(const std::string& houseSha, const std::string& localSha) {
    if (!houseSha.empty()) return houseSha == localSha ? Source::local : Source::house;
    return localSha.empty() ? Source::none : Source::local;
}
}  // namespace meshsave
