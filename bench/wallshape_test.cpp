// wallshape.h driven like room_lab drives it (controller ray + pinch per frame) in a 4 x 4.5 x 2.5 m room.
//   g++ -std=c++17 -O1 -I room_lab bench/wallshape_test.cpp -o /tmp/wallshape && /tmp/wallshape [shapes.json]
// With an argument it also writes the shapes it made, as a house.json fragment, for house/schema.py to validate.
#include "house.h"
#include "wallshape.h"
#include <cstdio>
#include <fstream>

using namespace wallshape;
static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static bool near(float a, float b, float tol = 1e-3f) { return std::fabs(a - b) <= tol; }

static WallRef wallOf(const char* id, V3 n, V3 c, float w, float h) {
    V3 r, u; wallfit::axesFor(n, r, u); return {id, {wallfit::fromAxes(r, u, n), c}, w, h};
}
static std::vector<WallRef> room() {
    return {wallOf("front", {0, 0, 1}, {0, 1.25f, -3}, 4, 2.5f), wallOf("left", {1, 0, 0}, {-2, 1.25f, -0.75f}, 4.5f, 2.5f),
            wallOf("right", {-1, 0, 0}, {2, 1.25f, -0.75f}, 4.5f, 2.5f), wallOf("back", {0, 0, -1}, {0, 1.25f, 1.5f}, 4, 2.5f),
            wallOf("floor", {0, 1, 0}, {0, 0, -0.75f}, 4, 4.5f)};
}
static const V3 EYE{0.2f, 1.6f, -0.4f};
static Input at(V3 p, bool pinch = false) { return {EYE, p - EYE, pinch}; }
static void hover(Tool& t, V3 p) { t.update(at(p)); }
static void press(Tool& t, V3 p) { t.update(at(p)); t.update(at(p, true)); t.update(at(p)); }
static V3 onWall(const Tool& t, int w, float x, float y) { return t.walls()[w].to3D({x, y}); }
static float meshLength(const Mesh& m) {  // total length of a line mesh's quads (long side)
    float L = 0;
    for (size_t i = 0; i + 3 < m.pos.size(); i += 4) L += std::max(wallfit::len(m.pos[i + 1] - m.pos[i]), wallfit::len(m.pos[i + 3] - m.pos[i]));
    return L;
}
static std::string dumpAll(const Tool& t) {
    json::Value a; a.type = json::Value::Arr;
    for (auto& [id, s] : t.shapes()) a.a.push_back(toJson(s));
    return house::dump(a);
}

int main(int argc, char** argv) {
    // ---- ray hits: clamped onto the rect within the grace, rejected beyond it, snapped to edges and corners
    {
        Tool t; t.setWalls(room());
        Hit h = t.raycast(EYE, onWall(t, 0, 0.5f, 0.3f) - EYE);
        CHECK(h.ok() && h.wall == 0 && near(h.p.x, 0.5f) && near(h.p.y, 0.3f), "plain hit %d (%.3f %.3f)", h.wall, h.p.x, h.p.y);
        h = t.raycast(EYE, onWall(t, 0, 1.0f, 1.25f + 0.02f) - EYE);
        CHECK(h.wall == 0 && near(h.p.y, 1.25f) && h.snapY, "2 cm above the top: clamped onto the edge (%.3f)", h.p.y);
        h = t.raycast(EYE, onWall(t, 0, 1.0f, 1.25f - 0.02f) - EYE);
        CHECK(near(h.p.y, 1.25f) && h.snapY, "2 cm below the top edge: snapped (%.3f)", h.p.y);
        h = t.raycast(EYE, onWall(t, 0, 1.0f, 1.25f - 0.04f) - EYE);
        CHECK(near(h.p.y, 1.21f) && !h.snapY, "4 cm below the top edge: not snapped (%.3f)", h.p.y);
        h = t.raycast(EYE, onWall(t, 0, -1.98f, -1.23f) - EYE);
        CHECK(h.wall == 0 && near(h.p.x, -2) && near(h.p.y, -1.25f) && h.snapX && h.snapY, "corner snap (%.3f %.3f)", h.p.x, h.p.y);
        CHECK(t.raycast(EYE, onWall(t, 3, 0, 0) - EYE).wall == 3, "back wall from inside");
        CHECK(!t.raycast({0, 1.25f, -5}, {0, 0, 1}).ok() || t.raycast({0, 1.25f, -5}, {0, 0, 1}).wall != 0, "front wall hit from behind");
    }

    // ---- line: A, B, preview (dashed), confirm, expand into a skin, handles, undo
    Tool t; t.setWalls(room());
    {
        t.begin(Mode::Line);
        CHECK(t.state() == State::PlacingA, "begin: %s", stateName(t.state()));
        t.update(at(EYE + V3{0, 5, 0})); CHECK(!t.hovering(), "placingA: pointing at no wall is not hovering");
        press(t, onWall(t, 0, -1, -0.25f));
        CHECK(t.state() == State::PlacingB, "after A: %s", stateName(t.state()));
        hover(t, onWall(t, 0, 0.5f, -0.25f));
        auto pv = t.previewMeshes();
        CHECK(pv.size() == 1 && pv[0].dashed && pv[0].preview, "dashed preview while placing B");
        if (!pv.empty()) {
            float L = meshLength(pv[0]), want = 1.5f * t.P.dash / (t.P.dash + t.P.gap);
            CHECK(std::fabs(L - want) < t.P.dash, "dashes cover %.3f m of 1.5 m (want ~%.3f)", L, want);
            float lift = 1e9f; for (auto& p : pv[0].pos) lift = std::min(lift, p.z + 3); CHECK(near(lift, t.P.lift), "preview lift %.4f", lift);
        }
        press(t, onWall(t, 0, 0.5f, -0.25f));
        CHECK(t.state() == State::Preview, "after B: %s", stateName(t.state()));
        CHECK(t.takeEdits().empty(), "edits before confirming");
        press(t, onWall(t, 0, 0.5f, -0.25f));
        CHECK(t.state() == State::Confirmed, "after confirm: %s", stateName(t.state()));
        auto e = t.takeEdits();
        CHECK(e.size() == 1 && e[0].obj["type"].str() == "line" && e[0].obj["wall"].str() == "front", "confirm saves one line");
        const Shape& s = t.shapes().begin()->second;
        CHECK(s.pts.size() == 2 && s.pts[0].first == -1 && s.pts[0].second == -0.25 && s.pts[1].first == 0.5, "line points (%.4f %.4f) (%.4f)", s.pts[0].first, s.pts[0].second, s.pts[1].first);
        CHECK(t.handles().size() == 2, "a line has 2 handles, got %zu", t.handles().size());
        hover(t, onWall(t, 0, 1.5f, 1.0f)); CHECK(!t.hovering(), "confirmed: a bare wall is not the tool's");
        hover(t, onWall(t, 0, 0.5f, -0.25f)); CHECK(t.hovering(), "confirmed: on a handle the tool hovers");
        auto lm = t.meshes(); CHECK(lm.size() == 1 && lm[0].segs.size() == 1 && near(lm[0].width, t.P.width) && near(lm[0].rgba[0], 1) && near(lm[0].rgba[1], 0.8f), "line strokes, width, rgba");

        t.expand();
        CHECK(t.state() == State::Expanding, "expand: %s", stateName(t.state()));
        V3 mid = onWall(t, 0, -0.25f, -0.25f), n{0, 0, 1};
        hover(t, mid + n * 0.3f + V3{0.4f, 0.2f, 0});  // aim off the normal line: depth is the nearest point on it
        float d = t.depth();
        hover(t, mid + n * 0.3f);
        CHECK(near(t.depth(), 0.3f, 0.002f), "depth from the ray %.4f (off-line aim gave %.3f)", t.depth(), d);
        auto sp = t.previewMeshes(); CHECK(sp.size() == 1 && sp[0].pos.size() == 4, "skin preview quad");
        press(t, mid + n * 0.3f);
        CHECK(t.state() == State::Done, "after depth: %s", stateName(t.state()));
        e = t.takeEdits();
        CHECK(e.size() == 1 && e[0].obj["type"].str() == "skin" && near((float)e[0].obj["depth"].num(), 0.3f, 0.002f), "expand saves a skin");
        auto ms = t.meshes(); CHECK(ms.size() == 1 && ms[0].pos.size() == 4 && ms[0].idx.size() == 12, "skin mesh: two-sided quad");
        CHECK(t.handles().size() == 3, "skin handles: 2 ends + depth, got %zu", t.handles().size());

        // drag the depth handle out to 0.5 m, then the B end to x = 1.98 (snaps to the wall's edge at 2.0)
        Handle dh{}; for (auto& h : t.handles()) if (h.kind == Handle::Depth) dh = h;
        t.update(at(dh.pos)); t.update(at(dh.pos, true));
        CHECK(t.state() == State::Dragging, "grab depth handle: %s", stateName(t.state()));
        t.update(at(mid + n * 0.5f, true)); t.update(at(mid + n * 0.5f));
        CHECK(near((float)t.shapes().begin()->second.depth, 0.5f, 0.002f), "dragged depth %.4f", t.shapes().begin()->second.depth);
        Handle bh{}; for (auto& h : t.handles()) if (h.kind == Handle::EndB) bh = h;
        t.update(at(bh.pos)); t.update(at(bh.pos, true));
        V3 to = onWall(t, 0, 1.98f, 0.1f);  // off the line: the end moves along the segment only
        t.update(at(to, true)); t.update(at(to));
        const Shape& s2 = t.shapes().begin()->second;
        CHECK(s2.pts[1].first == 2.0 && s2.pts[1].second == -0.25, "dragged end (%.4f %.4f), want (2, -0.25)", s2.pts[1].first, s2.pts[1].second);
        CHECK(t.takeEdits().size() == 2, "one edit per drag");

        // undo the drags and the expand, one at a time
        t.undo(); CHECK(t.shapes().begin()->second.pts[1].first == 0.5, "undo end drag");
        t.undo(); CHECK(near((float)t.shapes().begin()->second.depth, 0.3f, 0.002f), "undo depth drag");
        t.undo(); CHECK(t.shapes().begin()->second.type == Type::Line, "undo expand -> line");
        e = t.takeEdits(); CHECK(e.size() == 3 && e[2].obj["type"].str() == "line", "undo emits the restoring edits");
        t.undo(); CHECK(t.shapes().empty() && t.state() == State::Idle, "undo the line itself (%zu left, %s)", t.shapes().size(), stateName(t.state()));
        e = t.takeEdits(); CHECK(e.size() == 1 && e[0].obj.type == json::Value::Null, "deleting edit");
    }

    // ---- cancel and undo while placing
    {
        t.begin(Mode::Line); press(t, onWall(t, 0, 0, 0));
        t.undo(); CHECK(t.state() == State::PlacingA, "undo in placingB -> placingA");
        press(t, onWall(t, 0, 0, 0)); press(t, onWall(t, 0, 1, 0));
        t.undo(); CHECK(t.state() == State::PlacingB, "undo in preview -> placingB");
        t.cancel(); CHECK(t.state() == State::Idle && t.takeEdits().empty() && t.shapes().empty(), "cancel saves nothing");
        t.begin(Mode::Line); press(t, onWall(t, 0, 0, 0)); hover(t, onWall(t, 0, 0.01f, 0));
        CHECK(t.previewMeshes().empty(), "a 1 cm line is no line");
        press(t, onWall(t, 0, 0.01f, 0)); CHECK(t.state() == State::PlacingB, "1 cm B refused");
        t.cancel();
    }

    // ---- across the corner: front wall to left wall, bent where the unfolded straight line meets the corner
    {
        t.begin(Mode::Line);
        press(t, {-1, 1.0f, -3}); press(t, {-2, 1.5f, -2}); press(t, {-2, 1.5f, -2});
        CHECK(t.state() == State::Confirmed && t.shapes().size() == 2, "bent line: %zu shapes (%s)", t.shapes().size(), stateName(t.state()));
        auto e = t.takeEdits(); CHECK(e.size() == 2, "two pieces saved");
        const Shape *f = nullptr, *l = nullptr;
        for (auto& [id, s] : t.shapes()) (s.wall == "front" ? f : l) = &s;
        if (f && l) {
            CHECK(!f->group.empty() && f->group == l->group, "pieces share a group");
            V3 cf = t.walls()[0].to3D(f->pt(1)), cl = t.walls()[1].to3D(l->pt(0));
            CHECK(wallfit::len(cf - V3{-2, 1.25f, -3}) < 0.002f && wallfit::len(cl - cf) < 0.002f, "bend point (%.3f %.3f %.3f), want (-2, 1.25, -3)", cf.x, cf.y, cf.z);
            CHECK(f->pts[1].first == -2.0 && l->pts[0].first == 2.25, "bend point on both walls' edges exactly");
        }
        CHECK(t.handles().size() == 2, "bent line: the 2 outer ends only, got %zu", t.handles().size());
        t.expand(); hover(t, V3{-1.5f, 1.125f, -3} + V3{0, 0, 0.25f}); press(t, V3{-1.5f, 1.125f, -3} + V3{0, 0, 0.25f});
        int skins = 0; for (auto& [id, s] : t.shapes()) skins += s.type == Type::Skin && near((float)s.depth, 0.25f, 0.003f);
        CHECK(skins == 2, "bent line expands into 2 skins of one depth (%d)", skins);
        // the left piece's skin stands out of the left wall (+x), the front piece's out of the front wall (+z)
        for (auto& m : t.meshes()) {
            float maxX = -9, maxZ = -9; for (auto& p : m.pos) { maxX = std::max(maxX, p.x); maxZ = std::max(maxZ, p.z); }
            bool left = t.shapes().at(m.shape).wall == "left";
            CHECK(left ? near(maxX, -2 + 0.25f, 0.004f) : near(maxZ, -3 + 0.25f, 0.004f), "skin extrudes along its own wall's normal");
        }
        // a pick on either piece selects the group; remove deletes both, undo brings both back
        std::string picked = t.pick(EYE, V3{-1, 1.0f, -3} - EYE);
        CHECK(!picked.empty() && t.current().size() == 2, "pick selects the bent group");
        t.takeEdits(); t.remove(picked);
        CHECK(t.shapes().empty() && t.takeEdits().size() == 2, "remove deletes the group");
        t.undo(); CHECK(t.shapes().size() == 2, "undo remove");
        t.takeEdits();
        // parallel walls never bend
        V2 ca, cb; CHECK(!t.bend(t.walls()[0], {0, 0}, t.walls()[3], {0, 0}, ca, cb), "front/back bend");
    }

    // ---- circle: centre + radius, outline and filled disc; radius handle
    {
        t.begin(Mode::Circle); t.setFill(true);
        press(t, onWall(t, 0, 0.5f, 0.5f)); hover(t, onWall(t, 0, 0.75f, 0.5f));
        CHECK(near(t.radius(), 0.25f), "radius %.4f", t.radius());
        auto pv = t.previewMeshes(); CHECK(pv.size() == 1 && pv[0].dashed, "dashed circle preview");
        press(t, onWall(t, 0, 0.75f, 0.5f)); press(t, onWall(t, 0, 0.75f, 0.5f));
        CHECK(t.state() == State::Confirmed, "circle confirmed: %s", stateName(t.state()));
        auto e = t.takeEdits();
        CHECK(e.size() == 1 && e[0].obj["type"].str() == "circle" && e[0].obj["r"].num() == 0.25 && e[0].obj["fill"].b, "circle saved");
        t.expand(); CHECK(t.state() == State::Confirmed, "a circle does not expand");
        Mesh m; for (auto& x : t.meshes()) if (x.shape == e[0].id) m = x;
        int n = circleSegments(0.25f); bool inside = true; V3 c = onWall(t, 0, 0.5f, 0.5f);
        for (auto& p : m.pos) inside &= wallfit::len(p - c) < 0.25f + t.P.width;
        CHECK(inside && m.idx.size() >= (size_t)n * 3, "disc + outline within the radius (%zu idx)", m.idx.size());
        Handle rh{}; for (auto& h : t.handles()) if (h.kind == Handle::Radius) rh = h;
        t.update(at(rh.pos)); t.update(at(rh.pos, true)); t.update(at(onWall(t, 0, 0.9f, 0.5f), true)); t.update(at(onWall(t, 0, 0.9f, 0.5f)));
        CHECK(t.shapes().at(e[0].id).r == 0.4, "radius drag %.4f", t.shapes().at(e[0].id).r);
        t.finish(); t.takeEdits();
    }

    // ---- runaway guards: degenerate dash patterns, NaN, absurd lengths stay bounded (a float-stalled dash loop once
    // grew to 3 GB and was OOM-killed, 2026-11-04)
    {
        WallRef w = room()[0]; Mesh m;
        addLine(m, w, {{-1, 0}, {1, 0}}, 0.01f, 0.005f, true, 0.05f, 0.0f);   // gap 0: one long dash run, still finite
        CHECK(m.segs.size() == 40, "gap 0: %zu dashes, want 40", m.segs.size());
        m = {}; addLine(m, w, {{-1, 0}, {1, 0}}, 0.01f, 0.005f, true, 1e-7f, 1e-7f);
        CHECK(m.segs.size() == 1, "sub-0.1 mm dash draws solid (%zu)", m.segs.size());
        m = {}; addLine(m, w, {{0, 0}, {1e6f, 0}}, 0.01f, 0.005f, true, 0.05f, 0.03f);
        CHECK(m.segs.size() == (size_t)maxDashes, "1000 km dashed line capped at %d (%zu)", maxDashes, m.segs.size());
        m = {}; addLine(m, w, {{0, 0}, {NAN, 0}}, 0.01f, 0.005f, true, 0.05f, 0.03f);
        CHECK(m.segs.empty(), "NaN point draws nothing");
        m = {}; addLine(m, w, {{0, 0}, {1, 0}}, 0.01f, 0.005f, true, INFINITY, 0.03f);
        CHECK(m.segs.empty(), "infinite dash draws nothing");
        // the float-stall case: a long run of short segments far from the origin with a non-representable dash
        std::vector<V2> many; for (int i = 0; i < 2000; i++) many.push_back({1000.0f + i * 0.0137f, 0});
        m = {}; addLine(m, w, many, 0.01f, 0.005f, true, 0.0501f, 0.0299f);
        float stroke = 0; for (auto& sg : m.segs) stroke += wallfit::len(sg.second - sg.first);
        float want = 1999 * 0.0137f * 0.0501f / 0.08f;  // a dash spanning several segments is several strokes
        CHECK(std::fabs(stroke - want) < 0.1f && m.segs.size() < 3000, "many short segments: %.2f m of strokes (want %.2f), %zu strokes", stroke, want, m.segs.size());
        m = {}; addCircle(m, w, {0, 0}, NAN, 0.01f, 0.005f, true, 0.05f, 0.03f, true);
        CHECK(m.pos.empty(), "NaN radius draws nothing");
        m = {}; addSkin(m, w, {{0, 0}, {1, 0}}, NAN, 0);
        CHECK(m.pos.empty(), "NaN depth draws nothing");
    }

    // ---- lossless JSON: unknown keys and explicit defaults survive load -> save, text round-trips exactly
    {
        std::string in = R"([{"id":"x1","wall":"front","type":"line","pts":[[-0.4651,0.2437],[1,0]],"dashed":false,"note":"by hand","color":"#00ff88"},
                            {"id":"x2","wall":"left","type":"skin","pts":[[0,0],[0.5,0]],"depth":0.1234,"thick":0.02,"group":"g7"},
                            {"id":"x3","wall":"front","type":"circle","pts":[[0,0]],"r":0.3,"fill":true,"future":{"a":[1,2]}}])";
        Tool u; u.setWalls(room());
        CHECK(u.load(json::parse(in)) == 3, "load 3 shapes");
        std::string out = dumpAll(u), want = house::dump(json::parse(in));
        CHECK(out == want, "load -> save changed the JSON:\n  in  %s\n  out %s", want.c_str(), out.c_str());
        std::string made = dumpAll(t);
        Tool v; v.setWalls(room()); v.load(json::parse(made));
        CHECK(dumpAll(v) == made, "tool-made shapes do not round-trip through text");
        CHECK(u.meshes().size() == 3, "meshes for loaded shapes");
        if (argc > 1) {
            std::ofstream f(argv[1]); f << "{\"walls\":[\"front\",\"left\",\"right\",\"back\",\"floor\"],\"shapes\":" << made.substr(0, made.size() - 1)
                                        << "," << out.substr(1) << "}";
        }
    }

    printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
