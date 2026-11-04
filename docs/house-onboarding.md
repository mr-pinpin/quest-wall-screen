# House onboarding and setup (room_lab)

Status: design, 2026-11-04. Owner: headset side work-c4, house file / Mini server work-09, coordination work-df.
Everything here ships behind flags that are OFF by default (SHOULDER, HOUSE, WALLTOOL). With them off, room_lab is v0.44:
v0.37 behaviour plus today's flags.

## Why

Today a placement is `wall UUID + 2D offset` on a Meta Space Setup wall (placement.json, screens.json). That has three
failure modes:
- a rescan gives new UUIDs, and the screens vanish;
- duplicate rooms once hid scene data;
- if the Quest relocalises the space wrong, the wall plane moves, and the screen moves with it ("floats into the wall").

Space Setup is also one room at a time, and the user can't edit it from our app. The user wants:
- a whole-apartment scan that can be re-run at any time;
- wall drawing tools;
- one persistent, never-corrupted house file that they can edit.

## 1. Menu model (flag SHOULDER)

- **No always-armed gestures.** The spell circle stays, behind its existing SPELL toggle.
- A small persistent button rides near the shoulder: head-locked yaw only, about 0.35 m out, 0.25 m down and 0.25 m to the
  side (side is configurable). It is pressed by poke or pinch-ray, with the same ray and cursor ring as today.
- Pressing it opens the root panel about 1.0 m out, facing the user.
- Choosing a submenu spawns the next panel another `step_m` (about 1.0 m) further out along the same gaze ray, slightly
  offset, so the parent stays visible behind it: a cascading depth stack.
  - Back pops one level.
  - The button closes the whole stack.
  - Only the top panel takes input.
- Panels reuse ui_grid.h, so every button keeps v0.44's one-line explanation. Hit areas and legibility are covered by the
  existing ui_v044 tests.
- **Root:** Screens, Walls & shapes, House scan, Settings (the flags, the same items and hints as the wrist menu), Help.
- **Screens:** list, then per screen: on/off, size (S/M/L plus a fine slider), move (the existing snap/move bar), source
  (MINI / LAPTOP / Mac display N), delete. New screen: pick a wall by ray.
- The wrist settings menu, cursor ring, snap bar, AMBI and spell are unchanged. The shoulder menu drives the same state
  functions (`choose(label)`), so there is no second code path for the flags.

## 2. Wall tool (flag WALLTOOL)

All shapes live in the wall-2D frame, which is the Placement convention:
- metres from the wall pose origin;
- +x to the right as seen from inside the room;
- +y up;
- +z the normal into the room.

The steps:
1. Line: "Draw line", ray-pinch point A on a wall, then point B. While B is moving, a dashed preview is drawn 5 mm off the
   wall; both points snap to the wall plane, so there is no depth guess.
2. Confirm: the dashed line turns solid and is saved as `shape{type:line}`.
3. Expand: pull along the wall normal and the line extrudes into a perpendicular surface ("skin", `depth`). A slider or
   the pull distance sets depth; confirm saves `type:skin`.
4. Circle: centre plus radius, dashed preview, then confirm. Also "fill": a disc on the wall.
5. Edit: select a shape by ray, move its points, delete it. All saves go through the house file (section 4).

The geometry is GL-free in `room_lab/wallshape.h`, with desktop tests: snap to plane, dashed segmentation, extrusion mesh,
hit-testing.

## 3. Geometry and relocalisation (flag HOUSE)

Own the geometry; don't depend on Space Setup UUIDs. Space Setup planes are a hint only.

- **Source.** XR_META_environment_depth, which already runs for occlusion.
  - A GPU pass downsamples one eye to about 80x80 view-space points.
  - An async PBO readback, every few frames, gets them to the CPU without stalling.
- **Extraction (CPU, `room_lab/planes.h`, GL-free).**
  - Per-frame RANSAC finds plane candidates. Normals that are near horizontal give walls; near vertical give floor,
    ceiling and tables.
  - Candidates merge into persistent planes in the room frame: normal within 5°, offset within 3 cm, overlapping extent.
  - Each plane keeps an inlier-count weighted fit (rms, samples) and a 2D occupancy grid (10 cm cells) that gives the
    outline polygon and the bounding rect.
- **Anchors (XR_FB_spatial_entity + storage).**
  - Each room gets 2 to 4 of our own spatial anchors, created near walls at 1 to 3 m spacing and saved locally.
  - The house file stores each anchor's pose in the room frame. On launch we query by UUID; any one located anchor gives
    the room frame. Two or more also give a drift check, and we reject anchors that disagree by more than 5 cm or 2°.
  - Walking between rooms: when anchors of rooms A and B are located in the same session, we record the A to B link, so
    the flat shares one frame.
- **Re-scan is a button** (House scan → Re-scan this room / Walk the house). It fuses new depth into the existing planes and
  never replaces ids. A wall keeps its id, and a new Space Setup UUID is only appended to `scene[]`.
- **Placements** become `wall id + x/y` in our wall frame, with a last-known world pose as a fallback.
- **Migration.** Today's wall UUID becomes an imported wall: source "scene", pose captured at migration, scene=[uuid].
  Nothing the user has is lost.

Measure before trusting it (stand-in first, then the headset when handed back):
- depth resolution and rate as delivered; the status already reports the provider at 2000x2000, so check the real
  per-eye size;
- readback cost in ms, and plane-fit noise at 1, 2 and 4 m;
- how many persisted anchors locate after an app restart, after a reboot, and after walking to another room and back;
- anchor locate latency;
- whether anchors survive a Space Setup redo;
- the anchor count limit per app.

## 4. House file (schema agreed with work-09)

`house.json` schema 1 is served by the Mini on :57800. Writes are If-Match rev plus a SHA-256; the server does an atomic
write plus a git commit, and keeps history and rollback. The headset:
- keeps the last good doc in files/house.json (write to tmp, fsync, rename), so it works offline;
- PUTs whole objects by id on change;
- on a 409, re-applies the local op to the returned doc.

The sections are rooms (with anchors[] and links), walls (pose, w/h, outline, fit, scene[]), shapes, screens (wall, x, y,
w, h, on, source, world), menu (panel, shoulder) and settings (toggles). work-09 owns the field list; this note follows it.

## 5. Order and checks

1. Diagnosis of the v0.37 drift (read-only pull).
2. This note.
3. SHOULDER menu with Screens and Settings submenus.
4. planes.h plus anchors, measured.
5. WALLTOOL line, skin and circle.

Each step: desktop tests in bench/run_offline_tests.sh, then the v0.44 regression checklist with screenshots before the
user sees it. Flags stay OFF until the user has seen them working.
