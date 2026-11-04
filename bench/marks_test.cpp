// DRAW primitive (room_lab/marks.h, flag MARKS). g++ -std=c++17 -I room_lab bench/marks_test.cpp && ./a.out
#include "marks.h"
#include <cstdio>
using namespace marks;
static int fails = 0;
static void check(bool c, const char* m) { if (!c) { printf("FAIL %s\n", m); fails++; } }
static bool near(float a, float b, float e = 1e-4f) { return std::fabs(a - b) < e; }
static bool ccw(const Quad& q) { return (q.c[1].x - q.c[0].x) * (q.c[2].y - q.c[0].y) - (q.c[1].y - q.c[0].y) * (q.c[2].x - q.c[0].x) > 0; }

int main() {
    // Time: ISO 8601 Z to unix seconds (checked against known values).
    check(isoTime("1970-01-01T00:00:00Z") == 0 && isoTime("2026-11-04T10:15:00Z") == 1793787300.0 && isoTime("2000-03-01T00:00:00.5Z") == 951868800.5, "iso8601 Z -> unix seconds");
    check(isoTime("yesterday") == 0, "unparsable time: 0");

    const char* doc = R"({"marks":[
      {"id":"a","kind":"label","wall":"w-1","x":0.5,"y":1.2,"text":"this wall","color":"red","ts":"2026-11-04T10:00:00Z","ttl":60},
      {"id":"b","kind":"arrow","screen":3,"x":0,"y":0.8,"dx":0,"dy":-0.4,"text":"here"},
      {"id":"c","kind":"rect","wall":"w-2","x":1,"y":1,"w":0.6,"h":0.4,"color":"#3366ff","until":"2026-11-04T09:00:00Z"},
      {"id":"d","kind":"circle","wall":"w-2","x":2,"y":1,"r":99},
      {"id":"e","kind":"star","wall":"w-2","x":0,"y":0},
      {"id":"f","kind":"rect","x":0,"y":0},
      {"id":"g","kind":"label","wall":"w-3","x":0,"y":0},
      {"id":"h","kind":"circle","wall":"w-2","x":"1","y":0}]})";
    auto ms = parse(json::parse(doc));
    check(ms.size() == 4, "4 valid marks; an unknown kind, no anchor, an empty label and a string coordinate skipped");
    check(ms[0].text == "THIS WALL" && near(ms[0].col[0], 1.0f) && near(ms[0].col[1], 0.3f) && ms[0].until == isoTime("2026-11-04T10:01:00Z"), "label: upper-cased, named colour, ttl after ts");
    check(ms[1].screen == 3 && ms[1].wall.empty() && near(ms[1].dy, -0.4f), "arrow anchored to screen 3");
    check(near(ms[2].col[2], 1.0f) && near(ms[2].col[0], 0.2f) && ms[2].until == isoTime("2026-11-04T09:00:00Z"), "rect: #rrggbb colour, until");
    check(near(ms[3].r, 5.0f) && ms[3].until == 0, "circle radius clamped to 5 m; no expiry");
    double now = isoTime("2026-11-04T10:00:30Z");
    check(!expired(ms[0], now) && expired(ms[0], now + 31) && expired(ms[2], now) && !expired(ms[3], now + 1e6), "expiry: ttl, until, none");
    check(near(Life::alpha(ms[0], isoTime("2026-11-04T10:00:59.75Z")), 0.5f, 0.01f), "fades over its last 0.5 s");

    // Geometry: every quad faces the room; rect corners where asked; arrow tip exactly at (x+dx, y+dy); captions.
    auto r = geometry(ms[2], 1, 1);
    bool allCcw = true; for (auto& q : r.quads) allCcw = allCcw && ccw(q);
    float minx = 9, maxx = -9, miny = 9, maxy = -9; for (int i = 0; i < 4; i++) for (auto& p : r.quads[i].c) { minx = std::min(minx, p.x); maxx = std::max(maxx, p.x); miny = std::min(miny, p.y); maxy = std::max(maxy, p.y); }
    check(r.quads.size() == 4 && allCcw && near(minx, 0.7f - 0.007f) && near(maxx, 1.3f + 0.007f) && near(miny, 0.8f - 0.007f) && near(maxy, 1.2f + 0.007f), "rect: 4 sides around 0.6 x 0.4 m, facing the room");
    auto a = geometry(ms[1], 0, 0.8f);
    bool tip = false; for (auto& q : a.quads) for (auto& p : q.c) if (near(p.x, 0) && near(p.y, 0.4f)) tip = true;
    allCcw = true; for (auto& q : a.quads) allCcw = allCcw && ccw(q);
    check(tip && allCcw && a.texts.size() == 1 && a.texts[0].s == "HERE" && a.texts[0].y > 0.8f, "arrow: tip at (x+dx, y+dy), caption at the tail");
    auto c = geometry(ms[3], 2, 1); check(c.quads.size() == 32 && c.texts.empty(), "circle: 32 segments, no caption without text");
    auto l = geometry(ms[0], 0.5f, 1.2f); check(l.quads.size() == 1 && l.texts.size() == 1 && near(l.texts[0].x, 0.5f) && near(l.texts[0].y, 1.2f), "label: a badge and its text centred on the point");
    auto p1 = geometry(ms[2], 1, 1, 1), p0 = geometry(ms[2], 1, 1, 0);
    float w1 = std::fabs(p1.quads[0].c[0].y - p1.quads[0].c[1].y), w0 = std::fabs(p0.quads[0].c[0].y - p0.quads[0].c[1].y);
    check(w1 > w0 * 1.5f, "the pop thickens the lines");
    Life life; float a0 = life.pop(ms[0], 10), a1 = life.pop(ms[0], 11); check(a0 == 1 && a1 < 0.01f, "pop decays");
    Mark again = ms[0]; again.ts = "2026-11-04T10:05:00Z"; check(life.pop(again, 12) == 1, "drawn again (new ts): pops again");
    printf("marks: %s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
