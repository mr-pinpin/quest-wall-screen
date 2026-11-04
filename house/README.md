# The house file

One document, `house.json`, holds the whole house setup after onboarding: rooms and their spatial anchors, walls
(Meta scene walls and our own depth-extracted ones), the shapes the user draws on them, every screen's placement and
source, the menus and the settings. The headset reads it at start, edits it at any time, and keeps working offline
from its cached copy.

| file | what |
|---|---|
| `schema.py` | validation, schema conversion (old readers and writers), today's headset file formats |
| `server.py` | the sync server on the Mini (LAN, no internet): atomic writes, one git commit per change, conflicts refused |
| `migrate.py` | imports today's `files/placement.json`, `screens.json`, `settings.json` (+ optional scene wall list) |
| `test_house.py` | round trip, corrupt and partial writes, conflicts, disk corruption, rollback, blobs + their garbage collection, old schemas |
| `consolidate.py` | raw walls -> clean floor plan + score (the `plan` layer, below); `test_consolidate.py` |
| `floorplan.py` | `GET /house/plan.svg?layer=raw|clean|both` |

## Document (schema 1)

```
{"schema": 1, "rev": 7, "updated": "2026-11-04T00:00:00Z", "by": "headset", "nextScreenId": 4,
 "rooms":   [{"id": "r1", "name": "home", "height": 2.6, "floor": [[x, z], ...], "mesh": "<sha256 of a roommesh.h blob>",
              "anchors": [{"uuid": "<32 hex>", "pose": {"p": [x,y,z], "q": [x,y,z,w]}}]}],
 "links":   [{"a": "r1", "b": "r2", "pose": {"p": [...], "q": [...]}}],
 "walls":   [{"id": "w-851ff933", "room": "r1", "kind": "wall", "source": "scene|depth|manual",
              "scene": ["<Meta Space Setup uuid, newest first>"], "anchors": [...], "w": 4.1, "h": 2.6,
              "pose": {"p": [...], "q": [...]}, "outline": [[x, y], ...],
              "fit": {"rms_m": 0.004, "samples": 1800, "updated": "...", "w": 3.9, "h": 2.4}, "mesh": "<sha256 of a blob>"}],
 "shapes":  [{"id": "s1", "wall": "w-851ff933", "type": "line|circle|skin", "pts": [[x, y], ...],
              "dashed": true, "closed": false, "r": 0.25, "depth": 0.05, "color": "#ffcc00"}],
 "screens": [{"id": 1, "role": "main", "on": 1, "wall": "w-851ff933", "x": -0.47, "y": 0.24, "w": 1.96, "h": 1.23,
              "world": {"p": [...], "q": [...]}, "source": {"kind": "game", "host": "LAPTOP"}},
             {"id": 2, "role": "extra", "on": 1, "wall": "w-df0486f4", "x": ..., "source": {"kind": "mac-display", "screen": 2}}],
 "menu":    {"panel": {"wall": "w-851ff933", "pose": [px,py,pz,qx,qy,qz,qw]}, "shoulder": {"side": "left", "reach_m": 1.0, "step_m": 1.0}},
 "settings": {"locked": true, "resume": true, "saveset": true, "source": "LAPTOP", "toggles": {"VT": true, "SHOULDER": false}}}
```

Frames:
- Room frame: rooms locate through any one of their own spatial anchors; two or more also check for drift.
- Wall poses: relative to the room frame.
- Wall-2D coordinates (screens' x/y, shapes' pts): metres from the wall pose. +x is right as seen from inside the room, +y is up, +z points into the room; a skin's depth extrudes along +z.
- Screen ids: the main screen is id 1, and an extra screen's id is the laptop screen it shows.
- Wall extents: `w`/`h` are authoritative; `fit.w`/`fit.h` are what the last depth fit measured (a scan never shrinks a
  wall, it records what it saw).
- Ids: walls use our own ids, which stay stable across rescans. `scene` lists the Meta uuids that have meant that wall and is only a hint for matching.

## The plan layer (consolidate.py)

Every scan adds raw surfaces: Meta's walls, our depth fits, duplicates, furniture faces, lines that ran on too long, and
commits in a slightly different frame. `consolidate.py` is a pure function of `walls` + `rooms` that turns them into a
clean plan. It never edits the raw walls. The server runs it in the same revision as every write that changes walls or
rooms (about 0.1 s), and stores the result as the top-level `plan`:

```
"plan": {"algo": "consolidate/1", "src": "<sha256 of walls+rooms+algo>", "rev": 26, "updated": "...", "axesDeg": 17.3,
         "frames": [{"batch": "<fit.updated>", "dy": -0.16, "yawDeg": 0.4, "status": "moved -0.16 m onto the meta floor"}],
         "walls": [{"id": "p-851ff933", "pose": {...}, "w": 3.59, "h": 2.51, "from": ["w-851ff933", ...], "src": "scene|depth|scene+depth",
                    "room": "pr1", "conf": 1.0, "stacked": [...], "clippedFrom": 7.8, "inferred": [0, 0.39], "offAxis": true}],
         "rooms": [{"id": "pr1", "floor": [[x, z], ...], "area": 15.9, "floorY": -1.42, "ceilingY": 1.09, "closed": true,
                    "closure": 1.0, "support": 1.0, "doors": [{"a": [x, z], "b": [x, z], "w": 0.9}], "opens": [...], "tops": [...]}],
         "dropped": [{"id": "w-d528b76", "why": "0.29 m in front of a wall it overlaps (furniture face or a second look)"}],
         "score": {"verdict": "satisfactory | not yet", "reasons": [...], "duplicates": {"raw": 70, "plan": 0},
                   "closure": 0.84, "sceneFit": {"pairs": 0, "median_m": null, "backToBack": [...]}, "explained": 0.71}}}
```

The steps are listed at the top of `consolidate.py`:
1. Frame hygiene for each scan commit.
2. A Manhattan snap.
3. Merging, stack suppression and corner cuts.
4. Rooms: Meta's closed wall chains, plus our walls by ray casting on the rest of the floor.
5. The score.

Plan wall ids follow the raw wall that dominates them, so they stay stable across rescans. The headset can place screens
on `plan.walls`.

## Sync protocol (server.py, port 57800)

- `GET /house[?schema=K]`
  - Returns: the house, plus `ETag`/`X-House-Rev`, `X-House-Schema` and `X-House-SHA256` headers.
  - Older reader: asking for an older schema K converts the house down to it.
- `PUT /house`
  - Request: the whole house, with `If-Match: <rev it was based on>`, `X-House-SHA256: <sha256 of body>` and `Content-Length`.
  - Accepted: 200 `{"rev": N}`. The rev goes up by one, the file is written atomically, and one git commit is made.
  - Refused:
    - 400: truncated, hash mismatch, invalid JSON or invalid house, with the list of problems.
    - 409: stale, with the current house in the writer's schema.
    - 428: no If-Match.
  - Older writer: its body is converted up, and every field it could not see is kept from the current house.
- `GET /house/history`, `GET /house/rev/N`, `POST /house/rollback?rev=N` (If-Match: current rev; makes a new revision).
- `PUT|GET /blob/<sha256>`: meshes and other binaries. The body must hash to the name, and a wall's `mesh` must name a stored blob.
  - Blobs are not in git: `blobs/` is ignored, so history stays small. They are garbage-collected on every write.
  - Kept: what the house uses now; the newest 5 meshes per room or wall (from `meshlog.jsonl`, rebuilt from git if lost);
    what any tagged revision uses; and anything uploaded in the last 15 min, since the headset PUTs the blob before
    the house write that names it.
  - A rollback to a revision whose mesh was collected is refused (400).
- `POST /house/tag?rev=N&name=X` pins a milestone (git tag `house/X`, and its blobs); `GET /house/tags`; `GET /blobs`.
- `GET /legacy/placement.json|screens.json|settings.json`: the house in today's headset formats.
- `GET /house/plan` (the plan layer, computed on the fly if missing or stale), `GET /house/plan.svg?layer=raw|clean|both`.

Headset client:
- Caches the last good house and works offline.
- Edits whole objects by id.
- On reconnect, PUTs with its old If-Match. On a 409 it re-applies its edits to the returned house and PUTs again.

## Safety

- Writes: temp file, fsync, rename, then commit. If the commit fails, the old file is put back.
- Startup: if `house.json` is missing or invalid, the server restores the newest valid revision from git. It never serves a broken house.
- History: the data directory (`--data`, on the Mini `~/pinpin-house`) is a git repository, so `git log` is the history and any revision can be restored.
- Repo copy: `bench/house_mirror.sh` copies the Mini's house into this repo as `house/house.json`.
