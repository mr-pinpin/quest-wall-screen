# House consolidation, 2026-11-04 (house/consolidate.py, consolidate/1)

Raw (`*-raw`) and clean (`*-clean`) floor plans, the same pictures as `GET /house/plan.svg?layer=raw|clean`, in the room
frame, top-down. In the clean plan:
- Blue walls are Meta's; dashed blue walls are Meta walls off the room's two directions.
- Orange walls are our depth walls.
- Rooms are filled green when closed and beige when open.
- Doorways are green dashes and open sides are red dashes.

| house | raw walls -> plan | duplicate pairs | rooms (area, closure) | depth explained | verdict |
|---|---|---|---|---|---|
| rev 25 (scans 02:15, 03:03 UTC) | 31 -> 15 | 35 -> 0 | Meta room 15.9 m² (1.00); east room 7.4 m² (0.90) | 74% | not yet |
| rev 26 (+ scan 09:32 UTC) | 39 -> 15 | 70 -> 0 | Meta room 15.9 m² (1.00); east room 7.6 m² (0.84) | 71% | not yet |
| synthetic flat (test_consolidate.py) | 26 -> 9 | 25 -> 0 | 14.0 m² (1.00) and 9.1 m² (1.00), 0.9 m doorway | 92% | satisfactory |

Why the real house is "not yet" (the reasons the plan itself gives):

- **The scanned room is east of the TV wall, not the TV room.** All depth walls lie behind Meta's wall w-851ff933 (the
  wall with the main screen), 2-5 m east of it. Its west wall w-dde1888 faces away from the TV room, 2-3 cm behind
  w-851ff933. That gap is thinner than any real wall, so either the scan and Meta disagree by a few cm, or those scans
  were taken from the room behind the TV wall. No depth wall coincides with a Meta wall, so the two cannot be checked
  against each other directly.
- **The floor drifts from one commit to the next.** Each commit's floor sits above Meta's: by 6 cm (02:15 UTC), 16 cm
  (03:03 UTC) and 26 cm (09:32 UTC). The pass moves each commit down onto Meta's floor. At 30 cm or more it would treat
  the commit as another frame and drop it; 09:32 was 4 cm short of that. This is the stage/anchor drift work-df saw as
  "frames mixing".
- **The east room is still open.** About 1.2-1.9 m of its south side was never scanned.

What the pass removed on rev 25 (`rev25-plan.json` `dropped` lists them all):
- Four rotated walls 25-43° off the room's directions.
- Five stacked faces 14-29 cm in front of a wall they overlap: the 9.25 m line at the origin, a wardrobe front and
  repeated looks.
- Weak fits, and two slanted planes.

The 7.8 m line was cut at its corners to 2.7 m.

A real dump, 2026-11-04 08:44 UTC, from the user's seat facing the TV wall (`house/fixtures/depth-72504684`, LOCAL frame):
both eyes fused by `wallfit.h` put the wall 1.3 cm from Meta's w-851ff933. The fitting is sound. A disagreement in the
house file comes from the frame each SCAN commits in, not from the depth fit.
