# Screen controls are never occluded (flag CTLTOP, proposed)

Kanban 2026-11-04: "The white control under each screen is always visible, even where the screen meets the floor."
Written by work-k5 for work-c4 to integrate after v0.65. Together with the STEADY and select-outline headers, the
integration sketches are at the end.

## Why the bar disappears today (room_lab.cpp at 0e4d260)

1. **It hangs below the wall.** The move bar sits at `p.y - p.h/2 - kBarGap` (45 mm under the screen) and the code
   allows that to be outside the scanned wall rect ("the bar may hang below the scanned rect", hit test near
   line 3694). A screen placed low puts its bar under the floor line.
2. **It is in the occluded batch.** `grabBar(occ, …)`, `buttons(occ)`, `corners(occ, …)` and the extras' bars in
   `drawExtras(occ)` are drawn with `uOcclude = 1`. The fragment shader fades anything more than 5 cm behind the
   environment depth. Under the floor line the real floor is in front of the wall plane, so the bar fades out. A low
   bar near a sofa or a desk edge is hidden the same way.
3. **The room mesh draws after it.** `drawRoomMesh` (GEOMETRY / scanning) runs after the UI with depth test on and
   depth writes off. Its premultiplied glow blends over the bar wherever the floor quad is in front.

The screen picture itself is a compositor layer and is not affected. Only the eye-buffer controls are.

## Rule

Controls that belong to a screen (the move bar, its hold-progress fill, the source buttons, the resize corners and
the close X) are **always drawn and always reachable**:

- **Lift:** the bar never goes below the wall's bottom edge plus `kFloorClear` (6 cm). If there is no room under
  the screen, the bar moves *onto* the screen's bottom edge, as a tab inside the frame at z 0.024. The source
  buttons already fall back below the screen when there is no room above (`buttonCenter`). They use the same clamp.
- **On top:** these controls go in a third batch, `ctl`, drawn **last**: after the model, the chat panels and the
  room mesh. The depth test is off and `uOcclude = 0`. They are small, sit on the screen's own plane and are only
  drawn for placed screens, so drawing them on top cannot cover anything the user needs. The pointer ring and click
  mark are in `over` and stay drawn after `ctl`, so the cursor stays above the bar.
- The **hit test uses the same `barY()`** as the drawing, so what you see is what you pinch.

Rejected: keeping env-depth occlusion with a bigger bias (for example 25 cm). That still loses the bar when the
floor is more than the bias in front, which happens at any low placement viewed from standing height. It would
also make bias tuning per room a new knob. Hand occlusion of the bar (your real hand in front of it) is lost with
the on-top pass. That is acceptable for a 36 x 3.5 cm strip, and the pointer ring already behaves this way.

## Patch sketch (room_lab.cpp, behind CTLTOP, default off)

```cpp
// flags: {"CTLTOP", ctlTopMenu} in the saved-toggles table, ctlTopMenu = false; flags.json entry state "off",
// category "visual-taste", menu DISPLAY.
static constexpr float kFloorClear = 0.06f;
// One place for the bar's height, used by every draw and hit test (grabBar, drawExtras, the two hover tests).
float barY(const Placement& p, const Entity* w) const {
    float y = p.y - p.h / 2 - kBarGap;
    if (ctlTopMenu && w && y - kBarH / 2 < w->r2.offset.y + kFloorClear)
        y = p.y - p.h / 2 + kBarH / 2 + 0.012f;              // no room under the screen: a tab on its bottom edge
    return y;
}
// buttonCenter(): its fallback below the screen becomes barY(placed, w) - kBarH / 2 - 0.07f, then the same clamp
// (if it would go under the floor too, keep the buttons above the screen even past the wall top: walls end at the
// ceiling, so in practice there is always room above or below).

// build(): a third batch.
void build(Batch& occ, Batch& over, Batch& ctl) {
    Batch& bars = ctlTopMenu ? ctl : occ;
    ... buttons(bars); if (!editLocked || allBars()) grabBar(bars, *w); if (!editLocked) corners(bars, *w);
    drawExtras(occ, bars);   // screens' quads stay in occ; their bar and close X go to bars
}

// render, after drawRoomMesh(...) and before glBindVertexArray(0):
if (!ctl.v.empty()) {
    glUseProgram(program); glBindVertexArray(vao); glUniformMatrix4fv(uViewProj, 1, GL_FALSE, vp.m);
    glUniform1i(uOcclude, 0); glDisable(GL_DEPTH_TEST);
    glDrawArrays(GL_TRIANGLES, ctlFirst, ctlCount);         // uploaded after occ + over in the same VBO
    glEnable(GL_DEPTH_TEST);
}
// then redraw the pointer ring (it is in over) after ctl, or move the ring/click mark into a tiny 'top' batch drawn
// last, so the cursor stays above the bar.
```

Upload: `all = occ + over + ctl` (line ~4062), with `ctlFirst = occ.size() + over.size()`.

## Tests

- Offline: a pure helper `barYFor(screenBottom, wallBottom, on)` moved into a header (for example `ui_grid.h`) with
  checks: a high screen keeps 45 mm under; a screen whose bar would cross `wallBottom + 6 cm` gets the tab. The hit
  test and the drawing call the same function, which can be checked with a grep in `flags_check.py`-style code.
- Headset (unworn, `headset_hold.py`): place a screen with its bottom 2 cm above the floor line, with GEOMETRY on.
  Take a screencap with CTLTOP off (bar faded or covered by the floor glow) and with it on (the tab is visible).
  Pinch-hold the tab and confirm the move starts.

## Integration sketches for the other two pieces

**STEADY (`room_lab/steady.h`, `bench/steady_test.cpp`).** Add `steadyMenu = false` and a strength (`steadyK`, default
0.7, a slider or 3 steps 0.4/0.7/1.0) to the saved settings. Keep `steady::Hand steadyHand[2]`. In `updateHands`,
right after `ro`/`rd` are computed and before they go into `histO`/`histD`:

```cpp
if (steadyMenu) {
    steady::Input si; si.t = seconds(t); si.tracked = hr.valid; si.pinch = hr.pinch;
    si.o = {ro.x, ro.y, ro.z}; si.d = {rd.x, rd.y, rd.z}; si.tip = {hr.tip.x, hr.tip.y, hr.tip.z};
    steadyHand[h].strength = steadyK; auto so = steadyHand[h].update(si);
    ro = {so.o.x, so.o.y, so.o.z}; rd = {so.d.x, so.d.y, so.d.z}; hr.tip = {so.tip.x, so.tip.y, so.tip.z};
} else steadyHand[h].reset();
```

On every early `continue` (hand lost), call `steadyHand[h].reset()`. The existing rewind-and-lock then works on
filtered rays, so clicks land on the filtered pose from before the press. That is what the test checks with
`Out.click*`. When STEADY is on, the extras' cursor smoothing (`eu += (u - eu) * a`, line ~3170) should become
`a = 1`, so the cursor is not smoothed twice. The shoulder menu and wrist UI read `ray[1]` and `tip`, so they get the
same filtering for free.

**Select outline (`room_lab/select_outline.h`, `bench/select_outline_test.cpp`).** Keep `selout::Selection sel`. The
assistant's `select` and `clear` tools (and the room chat) call `sel.select(id, now)` and `sel.clear(now)`. In `build`, for each
placed screen (main = id 1 and the extras):

```cpp
sel.update(now); auto lk = sel.look(id, now); if (lk.visible) {
    selout::Rect r; r.c = P(p.x, p.y, 0); r.right = ax; r.up = ay; r.normal = n; r.w = p.w; r.h = p.h;
    r.topRoom = w->r2.offset.y + w->r2.extent.height - (p.y + p.h / 2);
    selout::Style st; float g[4] = {st.gold[0] * lk.bright, st.gold[1] * lk.bright, st.gold[2] * lk.bright, lk.alpha};
    auto f = selout::frame(r, st, lk.thickScale); for (int i = 0; i < 4; i++) { selout::V q[4]; f.quad(i, q); ctl.quad(v(q[0]), v(q[1]), v(q[2]), v(q[3]), g); }
    auto L = selout::label(r, st, lk.number); ctl.quad(v(L.badge[0]), v(L.badge[1]), v(L.badge[2]), v(L.badge[3]), g);
    // digits: the 5x7 glyphs at L.px around L.centre along L.right/L.up, in st.ink (a text() overload taking axes)
}
```

Draw it in `ctl` (on top) so the outline is never hidden by the floor either. Without CTLTOP, use `occ`.
