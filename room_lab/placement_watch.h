// Two placement failures that used to be silent (work-k5 review, 2026-11-04), as small GL-free decisions:
//  - The one-time depth-wall repair (room_lab.cpp repairDepthWalls) marked itself done even when it could not run
//    (the house copy it saw had fewer than 2 matching walls, e.g. a stale cache before the first fetch), so it never
//    retried. Now: apply when 2+ walls agree; otherwise wait and retry on the next house document, up to maxTries;
//    then give up visibly (status + a hint to scan again), never silently.
//  - Re-running Meta's Space Setup gives every scene plane a new uuid: none of the planes the house stored is located
//    any more, so roomframe::choose quietly falls back to our anchors' frame (and its drift). sceneChanged() spots it
//    (stored planes, none of them seen, while Meta walls are located) so the app says so instead of misplacing.
// bench/placement_watch_test.cpp.
#pragma once

namespace pwatch {

enum class Repair { Apply, Wait, GiveUp, Nothing };
// matches: depth walls matched to house walls with a pose; used: how many agree (anchors::solve); ok: the solve's
// verdict; tries: documents already tried (this one included).
inline Repair repairVerdict(int matches, int used, bool ok, int tries, int maxTries = 4) {
    if (ok && used >= 2) return Repair::Apply;
    if (tries >= maxTries) return matches == 0 ? Repair::Nothing : Repair::GiveUp;   // no depth wall in the house: nothing to repair
    return Repair::Wait;
}
inline const char* name(Repair r) { return r == Repair::Apply ? "applied" : r == Repair::Wait ? "waiting" : r == Repair::GiveUp ? "gave up" : "nothing to repair"; }

// storedPlanes: Meta planes whose pose the house stores; storedSeen: how many of them are located now; metaLocated:
// Meta walls located now; seconds: how long the scene has been queried (give Meta time to locate everything).
inline bool sceneChanged(int storedPlanes, int storedSeen, int metaLocated, float seconds, float waitS = 10) {
    return seconds >= waitS && storedPlanes >= 2 && storedSeen == 0 && metaLocated >= 2;
}

}  // namespace pwatch
