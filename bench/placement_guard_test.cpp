// Placements never disappear (room_lab/placement_guard.h): simulated app launches against a scene that arrives late,
// empty, or with changed uuids. Pass = every placed screen shown within 3 s at its saved spot (within 3 cm) whenever a
// cache exists, and the real walls taken over as soon as the scene has them.
// g++ -std=c++17 -I room_lab bench/placement_guard_test.cpp && ./a.out
#include "placement_guard.h"
#include <cstdio>
#include <random>
using namespace pguard;
static int fails = 0;
static void check(bool c, const char* m) { if (!c) { printf("FAIL %s\n", m); fails++; } }

struct World {  // the real room: walls in STAGE (fixed to the physical space)
    std::map<std::string, Pose> wallsStage = {{"aaaa", {anchors::yawQuat(0.3f), {1, 1.2f, -2}}}, {"bbbb", {anchors::yawQuat(-1.4f), {-2, 1.2f, 0.5f}}}};
};
struct Result { double firstShown = -1, allReal = -1; float maxErr = 0; int queries = 0; bool everNone = false; };

// One app launch: LOCAL is a fresh random frame; the scene answers queries started after relocAt with sceneUuids.
static Result launch(const World& W, std::map<std::string, CachedWall>& cache, double relocAt, const std::map<std::string, std::string>& rename, std::mt19937& rng,
                     double focusAt = -1, double secs = 60) {
    std::uniform_real_distribution<float> u(-3, 3);
    Pose stageInLocal{anchors::yawQuat(u(rng)), {u(rng), 0, u(rng)}};
    std::set<std::string> need = {"aaaa", "bbbb"};
    Requery rq; Result r; std::set<std::string> haveReal; double replyAt = -1; bool replyFull = false;
    std::map<std::string, Pose> realLocal;
    for (double t = 0; t < secs; t += 1.0 / 72) {
        if (focusAt >= 0 && t >= focusAt && t < focusAt + 1.0 / 72) rq.forced = true;
        if (rq.due(t, need, haveReal)) { rq.started(t); replyAt = t + 0.2; replyFull = t >= relocAt; }
        if (replyAt >= 0 && t >= replyAt) {  // the query's results arrive
            replyAt = -1; rq.finished();
            if (replyFull) for (auto& [id, p] : W.wallsStage) {
                std::string uid = rename.count(id) ? rename.at(id) : id;
                realLocal[uid] = anchors::compose(stageInLocal, p); haveReal.insert(uid);
            }
        }
        // What each screen is drawn against this frame.
        int shown = 0; float err = 0;
        auto si = standIns(need, haveReal, cache);
        for (auto& id : need) {
            Pose drawn; bool ok = false;
            if (haveReal.count(id)) { drawn = realLocal[id]; ok = true; }
            else for (auto& s : si) if (s == id) { drawn = toLocal(stageInLocal, cache.at(id)); ok = true; }
            if (!ok) continue;
            shown++;
            Pose truth = anchors::compose(stageInLocal, W.wallsStage.at(id));
            err = std::max(err, wallfit::len(drawn.p - truth.p));
        }
        if (shown == (int)need.size() && r.firstShown < 0) r.firstShown = t;
        if (shown < (int)need.size() && r.firstShown >= 0) r.everNone = true;
        if (shown) r.maxErr = std::max(r.maxErr, err);
        bool allReal = true; for (auto& id : need) allReal = allReal && haveReal.count(id);
        if (allReal && r.allReal < 0) r.allReal = t;
        // The app keeps the cache fresh from real walls.
        for (auto& id : need) if (haveReal.count(id)) {
            CachedWall c = fromLocal(id, stageInLocal, realLocal[id], -1, -1, 2, 2.5f);
            if (!cache.count(id) || moved(cache[id], c)) cache[id] = c;
        }
    }
    r.queries = rq.queries;
    return r;
}

int main() {
    std::mt19937 rng(11); World W; std::map<std::string, CachedWall> cache;
    // First ever launch, no cache, scene ready: real walls in well under 3 s.
    Result r = launch(W, cache, 0, {}, rng);
    check(r.allReal >= 0 && r.allReal < 1.0, "first launch: real walls within 1 s");
    check(cache.size() == 2, "cache filled from the real walls");
    // App restart, relocalisation pending 15 s (the 2026-11-04 bug): stand-ins from the first frame, real by ~17 s.
    r = launch(W, cache, 15, {}, rng);
    check(r.firstShown >= 0 && r.firstShown <= 3.0, "restart while relocalising: screens shown within 3 s");
    check(r.maxErr <= 0.03f, "...at the saved spot within 3 cm");
    check(r.allReal >= 15 && r.allReal <= 17.5, "...and the real walls taken over within 2.5 s of relocalisation");
    check(!r.everNone, "...never blank in between");
    check(r.queries <= 12, "...without a query storm");
    // The old behaviour (no cache): blank until relocalisation, then the re-query still recovers within 2.5 s.
    std::map<std::string, CachedWall> none;
    r = launch(W, none, 15, {}, rng);
    check(r.firstShown >= 15 && r.firstShown <= 17.5, "no cache: the re-query brings the screens back within 2.5 s of relocalisation");
    // Meta uuids changed (Space Setup redone): the old ids never come back; the stand-ins keep the screens at their spot.
    std::map<std::string, CachedWall> keep = cache;
    r = launch(W, keep, 0, {{"aaaa", "zzzz"}, {"bbbb", "yyyy"}}, rng, -1, 90);
    check(r.firstShown >= 0 && r.firstShown <= 3.0 && r.maxErr <= 0.03f && !r.everNone, "uuids changed: screens stay at their saved spot");
    check(r.allReal < 0, "...(the real walls under the old ids never arrive)");
    check(r.queries >= 19 && r.queries <= 23, "...re-queried every 2 s for 30 s, then every 10 s (90 s: 15 + 6)");
    // Sleep and wake: focus regained forces a query within 0.5 s even when nothing is missing.
    Requery rq; std::set<std::string> need = {"a"}, have = {"a"};
    rq.started(0); rq.finished();
    check(!rq.due(100, need, have), "nothing missing: no query");
    rq.forced = true; check(rq.due(100, need, have), "focus regained: query now");
    rq.started(100); check(!rq.due(100.2, need, have) && rq.inFlight, "one query in flight");
    // Cache: round trip through JSON, writes only on a real move.
    auto txt = dump(cache); auto back = load(txt);
    check(back.size() == cache.size() && !moved(back["aaaa"], cache["aaaa"]), "cache round trip");
    CachedWall m = cache["aaaa"]; m.stage.p.x += 0.005f; check(!moved(m, cache["aaaa"]), "5 mm is not a move");
    m.stage.p.x += 0.01f; check(moved(m, cache["aaaa"]), "15 mm is a move");
    check(load("{garbage").empty(), "a corrupt cache is ignored");
    printf("placement guard: %s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
