// wallshape.h: the wall tools' geometry and state machine (flag WALLTOOL, docs/house-onboarding.md section 2): lines
// from point A to point B on the walls, expanded into perpendicular "skins", and circles, saved as house.json shapes.
// GL-free: room_lab feeds the controller ray and pinch every frame and draws the meshes it gets back; the tests
// (bench/wallshape_test.cpp) drive it on the VPS.
//
// Frames: every shape lives on one wall, in that wall's 2D frame (the Placement convention): metres from the wall pose,
// +x right as seen from inside the room, +y up, +z the normal into the room. A line drawn across the corner between
// two walls becomes two line shapes sharing a "group", bent where the corner is (the straight line on the walls
// unfolded flat), and expanding it extrudes each piece along its own wall's normal.
//
// States: Idle -> PlacingA -> PlacingB -> Preview -> Confirmed -> Expanding -> Done (Dragging while a handle is held).
// A pinch (its press) sets A, then B, then confirms, then sets the depth. cancel() drops the gesture in progress;
// undo() steps back a point while placing, else reverts the last saved change. Every saved change comes out of
// takeEdits() as whole shapes by id (json::Value, Null = deleted), ready for house::Store::edit({"shapes", id, obj}).
#pragma once
#include "json.h"
#include "wallfit.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace wallshape {

using wallfit::Pose;
using wallfit::Quat;
using wallfit::V2;
using wallfit::V3;
using wallfit::cross;
using wallfit::dot;
using wallfit::len;
using wallfit::norm;
using wallfit::rotate;

inline double q4(double v) { return std::round(v * 1e4) / 1e4; }  // stored coordinates: 0.1 mm, exact through %.9g text

struct WallRef {
    std::string id; Pose pose; float w = 0, h = 0;  // the rect is centred on the pose
    V3 n() const { return rotate(pose.q, {0, 0, 1}); }
    V3 right() const { return rotate(pose.q, {1, 0, 0}); }
    V3 up() const { return rotate(pose.q, {0, 1, 0}); }
    V2 to2D(V3 p) const { V3 v = p - pose.p; return {dot(v, right()), dot(v, up())}; }
    V3 to3D(V2 q, float z = 0) const { return pose.p + rotate(pose.q, {q.x, q.y, z}); }
};

enum class Type { Line, Circle, Skin };
inline const char* typeName(Type t) { return t == Type::Line ? "line" : t == Type::Circle ? "circle" : "skin"; }

// One house.json shape. Keys this build does not know are kept in `other`, and keys present in the loaded JSON are
// written back even at their default value, so load -> save is lossless.
struct Shape {
    std::string id, wall, group, color; Type type = Type::Line;
    std::vector<std::pair<double, double>> pts;  // wall-2D metres
    double r = 0, depth = 0, thick = 0, dash = 0, gap = 0, width = 0;
    bool dashed = false, closed = false, fill = false;
    std::set<std::string> present; json::Value other;
    V2 pt(size_t i) const { return {(float)pts[i].first, (float)pts[i].second}; }
};

// ---- JSON
inline json::Value jnum(double n) { json::Value v; v.type = json::Value::Num; v.n = n; return v; }
inline json::Value jstr(const std::string& s) { json::Value v; v.type = json::Value::Str; v.s = s; return v; }
inline json::Value jbool(bool b) { json::Value v; v.type = json::Value::Bool; v.b = b; return v; }

inline bool fromJson(const json::Value& v, Shape& s) {
    if (v.type != json::Value::Obj) return false;
    s = Shape{}; s.other.type = json::Value::Obj;
    for (auto& [k, x] : v.o) {
        bool known = true;
        if (k == "id" && x.type == json::Value::Str) s.id = x.s;
        else if (k == "wall" && x.type == json::Value::Str) s.wall = x.s;
        else if (k == "group" && x.type == json::Value::Str) s.group = x.s;
        else if (k == "color" && x.type == json::Value::Str) s.color = x.s;
        else if (k == "type" && x.type == json::Value::Str && (x.s == "line" || x.s == "circle" || x.s == "skin"))
            s.type = x.s == "line" ? Type::Line : x.s == "circle" ? Type::Circle : Type::Skin;
        else if (k == "pts" && x.type == json::Value::Arr) {
            for (auto& p : x.a) { if (p.size() != 2) return false; s.pts.push_back({p[0].num(), p[1].num()}); }
        }
        else if (x.type == json::Value::Num && (k == "r" || k == "depth" || k == "thick" || k == "dash" || k == "gap" || k == "width"))
            (k == "r" ? s.r : k == "depth" ? s.depth : k == "thick" ? s.thick : k == "dash" ? s.dash : k == "gap" ? s.gap : s.width) = x.n;
        else if (x.type == json::Value::Bool && (k == "dashed" || k == "closed" || k == "fill"))
            (k == "dashed" ? s.dashed : k == "closed" ? s.closed : s.fill) = x.b;
        else known = false;
        if (known) s.present.insert(k); else s.other.o[k] = x;
    }
    return !s.id.empty() && !s.wall.empty() && !s.pts.empty();
}

inline json::Value toJson(const Shape& s) {
    json::Value v = s.other; v.type = json::Value::Obj;
    auto has = [&](const char* k) { return s.present.count(k) > 0; };
    v.o["id"] = jstr(s.id); v.o["wall"] = jstr(s.wall); v.o["type"] = jstr(typeName(s.type));
    json::Value pts; pts.type = json::Value::Arr;
    for (auto& p : s.pts) { json::Value a; a.type = json::Value::Arr; a.a = {jnum(p.first), jnum(p.second)}; pts.a.push_back(a); }
    v.o["pts"] = pts;
    if (!s.group.empty() || has("group")) v.o["group"] = jstr(s.group);
    if (!s.color.empty() || has("color")) v.o["color"] = jstr(s.color);
    const std::pair<const char*, double> nums[] = {{"r", s.r}, {"depth", s.depth}, {"thick", s.thick}, {"dash", s.dash}, {"gap", s.gap}, {"width", s.width}};
    for (auto& [k, x] : nums) if (x != 0 || has(k)) v.o[k] = jnum(x);
    const std::pair<const char*, bool> bools[] = {{"dashed", s.dashed}, {"closed", s.closed}, {"fill", s.fill}};
    for (auto& [k, x] : bools) if (x || has(k)) v.o[k] = jbool(x);
    return v;
}

// ---- meshes (world space, triangles)
// rgba from "#rrggbb" (alpha 1); anything else -> the default yellow
inline void rgbaOf(const std::string& c, float out[4]) {
    unsigned v = 0xffcc00;
    if (c.size() == 7 && c[0] == '#') { char* e = nullptr; unsigned x = (unsigned)strtoul(c.c_str() + 1, &e, 16); if (e && !*e) v = x; }
    out[0] = ((v >> 16) & 255) / 255.0f; out[1] = ((v >> 8) & 255) / 255.0f; out[2] = (v & 255) / 255.0f; out[3] = 1;
}

// Triangles for surfaces (skins, discs) and, for lines and outlines, the same strokes as segments with a width, for
// renderers that draw lines themselves (room_lab's Batch::line(width)).
struct Mesh {
    std::vector<V3> pos; std::vector<uint32_t> idx;
    std::vector<std::pair<V3, V3>> segs; float width = 0;  // line strokes (centre lines), width in metres
    bool preview = false, dashed = false; std::string shape, color; float rgba[4] = {1, 0.8f, 0, 1};
    void quad(V3 a, V3 b, V3 c, V3 d, bool twoSided = false) {
        uint32_t i = (uint32_t)pos.size(); pos.insert(pos.end(), {a, b, c, d});
        idx.insert(idx.end(), {i, i + 1, i + 2, i, i + 2, i + 3});
        if (twoSided) idx.insert(idx.end(), {i, i + 2, i + 1, i, i + 3, i + 2});
    }
};

// A polyline on a wall, width wide, lifted off the wall; dashed: dash on, gap off, in world metres, the pattern
// continuing across corners of the polyline. Dash k covers [k*period, k*period + dash] of the length along the whole
// polyline: an integer walk, bounded (maxDashes), so float rounding can never stall it. Non-finite input draws nothing.
constexpr int maxDashes = 10000;
inline bool finite2(V2 p) { return std::isfinite(p.x) && std::isfinite(p.y); }
inline void addLine(Mesh& m, const WallRef& w, const std::vector<V2>& pts, float width, float lift, bool dashed, float dash, float gap) {
    if (!std::isfinite(width) || !std::isfinite(lift) || !std::isfinite(dash) || !std::isfinite(gap)) return;
    for (auto& p : pts) if (!finite2(p)) return;
    bool solid = !dashed || dash < 1e-4f;
    double period = (double)dash + std::max(0.0, (double)gap), acc = 0; int dashes = 0;
    for (size_t i = 1; i < pts.size(); i++) {
        V2 a = pts[i - 1], b = pts[i]; double dx = b.x - a.x, dy = b.y - a.y, L = std::sqrt(dx * dx + dy * dy);
        if (L < 1e-6) continue;
        double ux = dx / L, uy = dy / L; float px = (float)(-uy * width / 2), py = (float)(ux * width / 2);
        auto piece = [&](double s0, double s1) {
            V2 p0{(float)(a.x + ux * s0), (float)(a.y + uy * s0)}, p1{(float)(a.x + ux * s1), (float)(a.y + uy * s1)};
            m.segs.push_back({w.to3D(p0, lift), w.to3D(p1, lift)}); m.width = width;
            m.quad(w.to3D({p0.x - px, p0.y - py}, lift), w.to3D({p1.x - px, p1.y - py}, lift), w.to3D({p1.x + px, p1.y + py}, lift),
                   w.to3D({p0.x + px, p0.y + py}, lift));
        };
        if (solid) { piece(0, L); acc += L; continue; }
        for (long long k = (long long)std::floor(acc / period); k * period < acc + L && dashes < maxDashes; k++) {
            double s0 = std::max(acc, k * period), s1 = std::min(acc + L, k * period + dash);
            if (s1 > s0 + 1e-9) { piece(s0 - acc, s1 - acc); dashes++; }
        }
        acc += L;
    }
}

inline int circleSegments(float r) { return std::clamp((int)std::ceil(2 * 3.14159265f * r / 0.02f), 24, 128); }

inline void addCircle(Mesh& m, const WallRef& w, V2 c, float r, float width, float lift, bool dashed, float dash, float gap, bool fill) {
    if (!finite2(c) || !std::isfinite(r) || r <= 0) return;
    int n = circleSegments(r);
    std::vector<V2> ring;
    for (int i = 0; i <= n; i++) { float a = 2 * 3.14159265f * i / n; ring.push_back({c.x + r * std::cos(a), c.y + r * std::sin(a)}); }
    if (fill) {
        uint32_t base = (uint32_t)m.pos.size(); m.pos.push_back(w.to3D(c, lift));
        for (int i = 0; i < n; i++) m.pos.push_back(w.to3D(ring[i], lift));
        for (int i = 0; i < n; i++) m.idx.insert(m.idx.end(), {base, base + 1 + (uint32_t)i, base + 1 + (uint32_t)((i + 1) % n)});
    }
    addLine(m, w, ring, width, lift + (fill ? 0.001f : 0), dashed, dash, gap);
}

// A skin: each segment of the polyline extruded along the wall normal by depth, as a two-sided quad, or (thick > 0) a
// closed box thick wide centred on the line.
inline void addSkin(Mesh& m, const WallRef& w, const std::vector<V2>& pts, float depth, float thick) {
    if (!std::isfinite(depth) || !std::isfinite(thick)) return;
    for (auto& p : pts) if (!finite2(p)) return;
    for (size_t i = 1; i < pts.size(); i++) {
        V2 a = pts[i - 1], b = pts[i];
        if (thick <= 0) { m.quad(w.to3D(a), w.to3D(b), w.to3D(b, depth), w.to3D(a, depth), true); continue; }
        float dx = b.x - a.x, dy = b.y - a.y, L = std::sqrt(dx * dx + dy * dy); if (L < 1e-6f) continue;
        float px = -dy / L * thick / 2, py = dx / L * thick / 2;
        V3 c[8];  // bottom (on the wall) 0-3, top (at depth) 4-7: a-, b-, b+, a+
        V2 q[4] = {{a.x - px, a.y - py}, {b.x - px, b.y - py}, {b.x + px, b.y + py}, {a.x + px, a.y + py}};
        for (int k = 0; k < 4; k++) { c[k] = w.to3D(q[k]); c[k + 4] = w.to3D(q[k], depth); }
        m.quad(c[3], c[2], c[1], c[0]); m.quad(c[4], c[5], c[6], c[7]);  // wall side, room side
        m.quad(c[0], c[1], c[5], c[4]); m.quad(c[1], c[2], c[6], c[5]); m.quad(c[2], c[3], c[7], c[6]); m.quad(c[3], c[0], c[4], c[7]);
    }
}

// ---- the tool
enum class Mode { Line, Circle };
enum class State { Idle, PlacingA, PlacingB, Preview, Confirmed, Expanding, Done, Dragging };
inline const char* stateName(State s) {
    const char* n[] = {"idle", "placingA", "placingB", "preview", "confirmed", "expanding", "done", "dragging"}; return n[(int)s];
}

struct Input { V3 origin, dir; bool pinch = false; };
struct Edit { std::string id; json::Value obj; };  // obj Null: the shape was deleted
struct Hit { int wall = -1; V2 p; V3 world; float t = 0; bool snapX = false, snapY = false; bool ok() const { return wall >= 0; } };
struct Handle { enum Kind { EndA, EndB, Depth, Radius } kind; std::string shape; V3 pos; };

struct Params {
    float snap = 0.03f;          // snap to a wall edge / corner within this (m)
    float grace = 0.05f;         // a hit this far outside a wall's rect still counts, clamped onto the rect
    float lift = 0.005f;         // lines and previews float this far off the wall
    float width = 0.01f;         // line width (m)
    float dash = 0.05f, gap = 0.03f;  // preview dash pattern (m)
    float maxDepth = 2.0f;       // skin depth limit (m)
    float minLength = 0.02f;     // shortest line / smallest radius (m)
    float handleRadius = 0.05f;  // a ray this close to a handle grabs it (m)
    float cornerTol = 0.10f;     // a bend point this close to a wall's edge snaps onto the edge
    std::string color = "#ffcc00";
};

class Tool {
public:
    Params P;

    void setWalls(std::vector<WallRef> w) { walls_ = std::move(w); }
    const std::vector<WallRef>& walls() const { return walls_; }

    // Existing shapes (the house's "shapes" array); unknown or invalid entries are skipped (and never edited).
    int load(const json::Value& arr) {
        shapes_.clear(); order_.clear(); int n = 0;
        for (auto& v : arr.a) { Shape s; if (fromJson(v, s) && !shapes_.count(s.id)) { order_.push_back(s.id); shapes_[s.id] = s; n++; } }
        return n;
    }
    const std::map<std::string, Shape>& shapes() const { return shapes_; }

    void begin(Mode m) { mode_ = m; current_.clear(); state_ = State::PlacingA; a_ = b_ = Hit{}; bValid_ = false; }
    State state() const { return state_; }
    Mode mode() const { return mode_; }
    const Hit& cursor() const { return cursor_; }
    // The ray is something the tool uses (a wall while placing, a handle when editing, any time during a gesture):
    // room_lab gives the tool the hand only then, so it never takes the pointer from screens.
    bool hovering() const {
        switch (state_) {
            case State::PlacingA: return cursor_.ok();
            case State::PlacingB: case State::Preview: case State::Expanding: case State::Dragging: return true;
            case State::Confirmed: case State::Done: return onHandle_;
            default: return false;
        }
    }
    const std::vector<std::string>& current() const { return current_; }
    void setFill(bool f) { fill_ = f; }
    void setThickness(float t) { thick_ = std::max(0.0f, t); }

    void update(const Input& in) {
        bool down = in.pinch && !pinch_, up = !in.pinch && pinch_; pinch_ = in.pinch; ray_ = in;
        cursor_ = raycast(in.origin, in.dir);
        onHandle_ = false;
        if (state_ == State::Confirmed || state_ == State::Done)
            for (auto& h : handles()) onHandle_ |= rayPointDist(in, h.pos) < P.handleRadius;
        switch (state_) {
            case State::PlacingA:
                if (down && cursor_.ok()) { a_ = cursor_; state_ = State::PlacingB; }
                break;
            case State::PlacingB:
                bValid_ = placeB(in, b_);
                if (down && bValid_) state_ = State::Preview;
                break;
            case State::Preview:
                if (down) confirm();
                break;
            case State::Expanding:
                depth_ = depthFromRay(in);
                if (down) commitExpand();
                break;
            case State::Confirmed: case State::Done:
                if (down) grab(in);
                break;
            case State::Dragging:
                drag(in);
                if (up) release();
                break;
            default: break;
        }
    }

    void confirm() {
        if (state_ != State::Preview) return;
        std::vector<Shape> made;
        if (mode_ == Mode::Circle) {
            Shape s = base(walls_[a_.wall].id, Type::Circle); s.pts = {{q4(a_.p.x), q4(a_.p.y)}}; s.r = q4(radius_); s.fill = fill_;
            made.push_back(s);
        } else if (b_.wall == a_.wall) {
            Shape s = base(walls_[a_.wall].id, Type::Line); s.pts = {{q4(a_.p.x), q4(a_.p.y)}, {q4(b_.p.x), q4(b_.p.y)}};
            made.push_back(s);
        } else {
            V2 ca, cb; if (!bend(walls_[a_.wall], a_.p, walls_[b_.wall], b_.p, ca, cb)) return;
            Shape s1 = base(walls_[a_.wall].id, Type::Line), s2 = base(walls_[b_.wall].id, Type::Line);
            s1.pts = {{q4(a_.p.x), q4(a_.p.y)}, {q4(ca.x), q4(ca.y)}}; s2.pts = {{q4(cb.x), q4(cb.y)}, {q4(b_.p.x), q4(b_.p.y)}};
            s1.group = s2.group = "g" + s1.id.substr(1);
            made = {s1, s2};
        }
        std::vector<std::pair<std::string, std::optional<Shape>>> after;
        current_.clear();
        for (auto& s : made) { after.push_back({s.id, s}); current_.push_back(s.id); }
        commit(after);
        state_ = State::Confirmed;
    }

    void expand() {
        if (state_ != State::Confirmed || current_.empty()) return;
        for (auto& id : current_) if (shapes_[id].type == Type::Circle) return;
        depth_ = 0; state_ = State::Expanding;
    }
    void finish() { if (state_ == State::Confirmed || state_ == State::Expanding) state_ = State::Done; }

    void cancel() {
        switch (state_) {
            case State::PlacingA: case State::PlacingB: case State::Preview: state_ = State::Idle; current_.clear(); break;
            case State::Expanding: state_ = State::Confirmed; break;
            case State::Dragging: for (auto& [id, s] : dragBefore_) shapes_[id] = s; state_ = dragFrom_; break;
            default: break;
        }
    }

    void undo() {
        switch (state_) {
            case State::PlacingB: state_ = State::PlacingA; return;
            case State::Preview: state_ = State::PlacingB; return;
            case State::Expanding: case State::Dragging: cancel(); return;
            default: break;
        }
        if (undo_.empty()) return;
        auto entry = undo_.back(); undo_.pop_back();
        for (auto& [id, before] : entry) apply(id, before);
        bool any = false; for (auto& id : current_) any |= shapes_.count(id) > 0;
        if (!any) current_.clear();
        state_ = current_.empty() ? State::Idle : State::Done;
    }

    // Delete a shape (and the rest of its group), undoable.
    void remove(const std::string& id) {
        auto it = shapes_.find(id); if (it == shapes_.end()) return;
        std::vector<std::pair<std::string, std::optional<Shape>>> after;
        for (auto& s : groupOf(id)) after.push_back({s, std::nullopt});
        commit(after);
        current_.erase(std::remove_if(current_.begin(), current_.end(), [&](auto& c) { return !shapes_.count(c); }), current_.end());
    }

    // The shape under a ray (its line within 3 cm, or inside / on its circle), "" if none; it becomes current.
    std::string pick(V3 origin, V3 dir) {
        Hit h = raycast(origin, dir, true); if (!h.ok()) return "";
        std::string best; float bestD = 0.03f;
        for (auto& id : order_) {
            auto it = shapes_.find(id); if (it == shapes_.end() || it->second.wall != walls_[h.wall].id) continue;
            const Shape& s = it->second; float d = 1e9f;
            if (s.type == Type::Circle) { float c = std::hypot(h.p.x - s.pt(0).x, h.p.y - s.pt(0).y); d = s.fill && c < s.r ? 0 : std::fabs(c - (float)s.r); }
            else for (size_t i = 1; i < s.pts.size(); i++) d = std::min(d, segDist(h.p, s.pt(i - 1), s.pt(i)));
            if (d < bestD) { bestD = d; best = id; }
        }
        if (!best.empty()) { current_ = groupOf(best); state_ = State::Done; }
        return best;
    }

    std::vector<Edit> takeEdits() { std::vector<Edit> e; e.swap(edits_); return e; }

    // Saved shapes, as drawn: solid lines, circles (outline or disc), skins.
    std::vector<Mesh> meshes() const {
        std::vector<Mesh> out;
        for (auto& id : order_) {
            auto it = shapes_.find(id); if (it == shapes_.end()) continue;
            const WallRef* w = wallOf(it->second.wall); if (!w) continue;
            out.push_back(meshOf(*w, it->second, it->second.depth, false));
        }
        return out;
    }

    // What the gesture in progress shows: the dashed line / circle while placing, the skin while expanding.
    std::vector<Mesh> previewMeshes() const {
        std::vector<Mesh> out;
        if ((state_ == State::PlacingB || state_ == State::Preview) && (state_ == State::Preview || bValid_)) {
            Mesh m; m.preview = m.dashed = true; m.color = P.color; rgbaOf(m.color, m.rgba);
            if (mode_ == Mode::Circle) addCircle(m, walls_[a_.wall], a_.p, radius_, P.width, P.lift, true, P.dash, P.gap, fill_);
            else if (b_.wall == a_.wall) addLine(m, walls_[a_.wall], {a_.p, b_.p}, P.width, P.lift, true, P.dash, P.gap);
            else {
                V2 ca, cb;
                if (bend(walls_[a_.wall], a_.p, walls_[b_.wall], b_.p, ca, cb)) {
                    addLine(m, walls_[a_.wall], {a_.p, ca}, P.width, P.lift, true, P.dash, P.gap);
                    addLine(m, walls_[b_.wall], {cb, b_.p}, P.width, P.lift, true, P.dash, P.gap);
                }
            }
            out.push_back(m);
        }
        if (state_ == State::Expanding)
            for (auto& id : current_) {
                auto it = shapes_.find(id); const WallRef* w = it == shapes_.end() ? nullptr : wallOf(it->second.wall); if (!w) continue;
                Mesh m = meshOf(*w, it->second, depth_, true); m.preview = true; out.push_back(m);
            }
        return out;
    }

    // Handles of the current shapes: line / skin ends (not the shared bend point), skin depth, circle radius.
    std::vector<Handle> handles() const {
        std::vector<Handle> out;
        if (state_ != State::Confirmed && state_ != State::Done && state_ != State::Dragging) return out;
        for (auto& id : current_) {
            auto it = shapes_.find(id); if (it == shapes_.end()) continue;
            const Shape& s = it->second; const WallRef* w = wallOf(s.wall); if (!w) continue;
            if (s.type == Type::Circle) { out.push_back({Handle::Radius, id, w->to3D({s.pt(0).x + (float)s.r, s.pt(0).y}, P.lift)}); continue; }
            V2 a = s.pt(0), b = s.pt(s.pts.size() - 1);
            if (!sharedEnd(s, w->to3D(a))) out.push_back({Handle::EndA, id, w->to3D(a, P.lift)});
            if (!sharedEnd(s, w->to3D(b))) out.push_back({Handle::EndB, id, w->to3D(b, P.lift)});
            if (s.type == Type::Skin) { V2 m{(s.pt(0).x + s.pt(1).x) / 2, (s.pt(0).y + s.pt(1).y) / 2}; out.push_back({Handle::Depth, id, w->to3D(m, (float)s.depth)}); }
        }
        return out;
    }

    // ---- geometry, public for tests and for the UI's own use

    // Nearest wall hit by the ray (front side), clamped onto the rect (grace), snapped to edges.
    Hit raycast(V3 o, V3 d, bool noSnap = false) const {
        Hit best; d = norm(d);
        for (size_t i = 0; i < walls_.size(); i++) {
            const WallRef& w = walls_[i]; V3 n = w.n(); float den = dot(d, n);
            if (den > -1e-6f) continue;
            float t = dot(w.pose.p - o, n) / den; if (t <= 0 || (best.ok() && t >= best.t)) continue;
            V2 q = w.to2D(o + d * t);
            if (std::fabs(q.x) > w.w / 2 + P.grace || std::fabs(q.y) > w.h / 2 + P.grace) continue;
            Hit h; h.wall = (int)i; h.t = t;
            h.p = {std::clamp(q.x, -w.w / 2, w.w / 2), std::clamp(q.y, -w.h / 2, w.h / 2)};
            if (!noSnap) snapEdges(w, h);
            h.world = w.to3D(h.p); best = h;
        }
        return best;
    }

    void snapEdges(const WallRef& w, Hit& h) const {
        for (float e : {-w.w / 2, w.w / 2}) if (std::fabs(h.p.x - e) < P.snap) { h.p.x = e; h.snapX = true; }
        for (float e : {-w.h / 2, w.h / 2}) if (std::fabs(h.p.y - e) < P.snap) { h.p.y = e; h.snapY = true; }
    }

    // A on wall a, B on wall b: where the straight line on the two walls unfolded flat crosses their shared corner, as
    // a point in each wall's frame. False when the walls are parallel or the corner is not on both rects.
    bool bend(const WallRef& a, V2 pa, const WallRef& b, V2 pb, V2& ca, V2& cb) const {
        V3 na = a.n(), nb = b.n(), u = cross(na, nb); float uu = dot(u, u);
        if (uu < 0.1f) return false;  // within ~18 degrees of parallel: no corner between them
        float ha = dot(na, a.pose.p), hb = dot(nb, b.pose.p);
        V3 x0 = (cross(nb, u) * ha + cross(u, na) * hb) * (1.0f / uu), dl = norm(u);
        V3 A = a.to3D(pa), B = b.to3D(pb);
        V3 e1 = norm(cross(na, dl)), e2 = norm(cross(nb, dl));
        if (dot(A - x0, e1) < 0) e1 = e1 * -1.0f;
        if (dot(B - x0, e2) < 0) e2 = e2 * -1.0f;
        float sa = dot(A - x0, dl), ta = dot(A - x0, e1), sb = dot(B - x0, dl), tb = dot(B - x0, e2);
        if (ta + tb < 1e-4f) return false;
        V3 C = x0 + dl * (sa + (sb - sa) * ta / (ta + tb));
        ca = a.to2D(C); cb = b.to2D(C);
        auto inside = [&](const WallRef& w, V2& c) {
            if (std::fabs(c.x) > w.w / 2 + P.cornerTol || std::fabs(c.y) > w.h / 2 + P.grace) return false;
            if (std::fabs(std::fabs(c.x) - w.w / 2) < P.cornerTol) c.x = std::copysign(w.w / 2, c.x);  // onto the edge exactly
            c.y = std::clamp(c.y, -w.h / 2, w.h / 2);
            return true;
        };
        return inside(a, ca) && inside(b, cb);
    }

    float depth() const { return depth_; }
    float radius() const { return radius_; }

private:
    std::vector<WallRef> walls_;
    std::map<std::string, Shape> shapes_; std::vector<std::string> order_;
    std::vector<std::vector<std::pair<std::string, std::optional<Shape>>>> undo_;
    std::vector<Edit> edits_;
    State state_ = State::Idle, dragFrom_ = State::Done; Mode mode_ = Mode::Line;
    Hit cursor_, a_, b_; bool bValid_ = false, pinch_ = false, fill_ = false, onHandle_ = false; float thick_ = 0, depth_ = 0, radius_ = 0;
    std::vector<std::string> current_; Input ray_;
    Handle drag_{Handle::EndA, "", {}}; std::map<std::string, Shape> dragBefore_;
    int nextId_ = 1;

    const WallRef* wallOf(const std::string& id) const { for (auto& w : walls_) if (w.id == id) return &w; return nullptr; }

    std::string newId() {
        for (;; nextId_++) { std::string id = "s" + std::to_string(nextId_); if (!shapes_.count(id)) { nextId_++; return id; } }
    }
    Shape base(const std::string& wall, Type t) { Shape s; s.id = newId(); s.wall = wall; s.type = t; s.color = P.color; s.width = P.width; return s; }

    std::vector<std::string> groupOf(const std::string& id) const {
        auto it = shapes_.find(id); if (it == shapes_.end()) return {};
        if (it->second.group.empty()) return {id};
        std::vector<std::string> g; for (auto& o : order_) { auto j = shapes_.find(o); if (j != shapes_.end() && j->second.group == it->second.group) g.push_back(o); }
        return g;
    }

    // One undoable change: the new state of each id (nullopt = delete); records the old, emits the edits.
    void commit(const std::vector<std::pair<std::string, std::optional<Shape>>>& after) {
        std::vector<std::pair<std::string, std::optional<Shape>>> before;
        for (auto& [id, s] : after) {
            auto it = shapes_.find(id);
            before.push_back({id, it == shapes_.end() ? std::nullopt : std::optional<Shape>(it->second)});
            apply(id, s);
        }
        undo_.push_back(before);
    }
    void apply(const std::string& id, const std::optional<Shape>& s) {
        if (s) { if (!shapes_.count(id)) order_.push_back(id); shapes_[id] = *s; edits_.push_back({id, toJson(*s)}); }
        else { shapes_.erase(id); order_.erase(std::remove(order_.begin(), order_.end(), id), order_.end()); edits_.push_back({id, json::Value{}}); }
    }

    bool placeB(const Input& in, Hit& b) {
        const WallRef& wa = walls_[a_.wall];
        if (mode_ == Mode::Circle) {  // radius in A's wall plane, wherever the ray meets it
            V3 n = wa.n(), d = norm(in.dir); float den = dot(d, n); if (den > -1e-6f) return false;
            float t = dot(wa.pose.p - in.origin, n) / den; if (t <= 0) return false;
            V2 q = wa.to2D(in.origin + d * t); radius_ = std::hypot(q.x - a_.p.x, q.y - a_.p.y);
            b.wall = a_.wall; b.p = q; return radius_ >= P.minLength;
        }
        if (!cursor_.ok()) return false;
        b = cursor_;
        if (b.wall == a_.wall) return std::hypot(b.p.x - a_.p.x, b.p.y - a_.p.y) >= P.minLength;
        V2 ca, cb; return bend(wa, a_.p, walls_[b.wall], b.p, ca, cb);
    }

    // Depth along the normal at the middle of the first current piece: the point on that normal line nearest the ray.
    float depthFromRay(const Input& in) const {
        if (current_.empty()) return 0;
        auto it = shapes_.find(current_[0]); const WallRef* w = it == shapes_.end() ? nullptr : wallOf(it->second.wall); if (!w) return 0;
        const Shape& s = it->second; V2 m{(s.pt(0).x + s.pt(1).x) / 2, (s.pt(0).y + s.pt(1).y) / 2};
        return std::clamp(lineParam(w->to3D(m), w->n(), in), 0.0f, P.maxDepth);
    }
    static float lineParam(V3 m, V3 n, const Input& in) {  // parameter along line m + n*t closest to the ray
        V3 d = norm(in.dir), w0 = in.origin - m; float b = dot(d, n), den = 1 - b * b;
        if (den < 1e-6f) return 0;
        return (dot(n, w0) - b * dot(d, w0)) / den;
    }

    void commitExpand() {
        std::vector<std::pair<std::string, std::optional<Shape>>> after;
        for (auto& id : current_) {
            Shape s = shapes_[id]; s.type = Type::Skin; s.depth = q4(depth_); if (thick_ > 0) s.thick = q4(thick_);
            after.push_back({id, s});
        }
        commit(after); state_ = State::Done;
    }

    bool sharedEnd(const Shape& s, V3 p) const {  // p on the wall (no lift): another piece of the group ends there
        if (s.group.empty()) return false;
        for (auto& [id, o] : shapes_) {
            if (id == s.id || o.group != s.group) continue;
            const WallRef* w = wallOf(o.wall); if (!w) continue;
            for (size_t i = 0; i < o.pts.size(); i++) if (len(w->to3D(o.pt(i)) - p) < 0.005f) return true;
        }
        return false;
    }

    static float segDist(V2 p, V2 a, V2 b) {
        float dx = b.x - a.x, dy = b.y - a.y, L2 = dx * dx + dy * dy;
        float t = L2 > 0 ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / L2, 0.0f, 1.0f) : 0;
        return std::hypot(p.x - a.x - t * dx, p.y - a.y - t * dy);
    }

    static float rayPointDist(const Input& in, V3 p) {
        V3 d = norm(in.dir), v = p - in.origin; float t = std::max(0.0f, dot(v, d)); return len(v - d * t);
    }

    void grab(const Input& in) {
        float best = P.handleRadius; bool found = false;
        for (auto& h : handles()) { float d = rayPointDist(in, h.pos); if (d < best) { best = d; drag_ = h; found = true; } }
        if (!found) return;
        dragBefore_.clear();
        for (auto& id : groupOf(drag_.shape)) dragBefore_[id] = shapes_[id];
        dragFrom_ = state_; state_ = State::Dragging;
    }

    void drag(const Input& in) {
        auto it = shapes_.find(drag_.shape); if (it == shapes_.end()) return;
        Shape& s = it->second; const WallRef* w = wallOf(s.wall); if (!w) return;
        const Shape& was = dragBefore_[s.id];
        if (drag_.kind == Handle::Depth) {
            V2 m{(was.pt(0).x + was.pt(1).x) / 2, (was.pt(0).y + was.pt(1).y) / 2};
            double d = q4(std::clamp(lineParam(w->to3D(m), w->n(), in), 0.0f, P.maxDepth));
            for (auto& [id, b] : dragBefore_) shapes_[id].depth = d;  // a bent skin keeps one depth
            return;
        }
        // the ray on this shape's wall plane (unbounded), then clamped and snapped like any hit
        V3 n = w->n(), d = norm(in.dir); float den = dot(d, n); if (den > -1e-6f) return;
        float t = dot(w->pose.p - in.origin, n) / den; if (t <= 0) return;
        Hit h; h.p = w->to2D(in.origin + d * t);
        h.p = {std::clamp(h.p.x, -w->w / 2, w->w / 2), std::clamp(h.p.y, -w->h / 2, w->h / 2)};
        if (drag_.kind == Handle::Radius) { s.r = q4(std::max(P.minLength, std::hypot(h.p.x - was.pt(0).x, h.p.y - was.pt(0).y))); return; }
        // ends move along the segment (the handle sets its length); the other end stays
        size_t moving = drag_.kind == Handle::EndA ? 0 : was.pts.size() - 1, fixed = moving ? 0 : was.pts.size() - 1;
        V2 f = was.pt(fixed), m0 = was.pt(moving); float dx = m0.x - f.x, dy = m0.y - f.y, L = std::hypot(dx, dy); if (L < 1e-6f) return;
        float ux = dx / L, uy = dy / L, along = std::max(P.minLength, (h.p.x - f.x) * ux + (h.p.y - f.y) * uy);
        Hit e; e.p = {f.x + ux * along, f.y + uy * along}; snapEdges(*w, e);
        e.p = {std::clamp(e.p.x, -w->w / 2, w->w / 2), std::clamp(e.p.y, -w->h / 2, w->h / 2)};
        s.pts[moving] = {q4(e.p.x), q4(e.p.y)};
    }

    void release() {
        std::vector<std::pair<std::string, std::optional<Shape>>> after;
        for (auto& [id, b] : dragBefore_) { after.push_back({id, shapes_[id]}); shapes_[id] = b; }  // commit records the before
        commit(after); state_ = dragFrom_;
    }

    Mesh meshOf(const WallRef& w, const Shape& s, double depth, bool asSkin) const {
        Mesh m; m.shape = s.id; m.color = s.color.empty() ? P.color : s.color; m.dashed = s.dashed; rgbaOf(m.color, m.rgba);
        std::vector<V2> pts; for (size_t i = 0; i < s.pts.size(); i++) pts.push_back(s.pt(i));
        if (s.closed && pts.size() > 2) pts.push_back(pts[0]);
        float width = s.width > 0 ? (float)s.width : P.width, dash = s.dash > 0 ? (float)s.dash : P.dash, gap = s.gap > 0 ? (float)s.gap : P.gap;
        if (s.type == Type::Circle) addCircle(m, w, s.pt(0), (float)s.r, width, P.lift, s.dashed, dash, gap, s.fill);
        else if (s.type == Type::Skin || asSkin) addSkin(m, w, pts, (float)depth, (float)(asSkin ? thick_ : s.thick));
        else addLine(m, w, pts, width, P.lift, s.dashed, dash, gap);
        return m;
    }
};

}  // namespace wallshape
