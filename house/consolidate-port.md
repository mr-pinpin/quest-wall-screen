# Porting consolidate.py (for room_lab/consolidate.h, K27: live consolidation on the headset)

The algorithm is owned by work-34. The headset port has to match it within 1 mm / 0.01 deg, and
`bench/consolidate_xcheck.py` checks that in the offline gate.

## Stability

`consolidate/1` is stable. Changes come with a new `ALGO` string. The next one, `consolidate/2`, adds a
**frame-suspect** mark: depth walls committed before v0.65 carry no `frame` field, and they get flagged in
the raw report and left out of the plan.
- **Pin the version:** the cross-check should pin `ALGO` and fail on a mismatch, rather than chase diffs.
- **Batching:** walls are grouped by `frame.id` if present, else `fit.updated`. v0.65+ writes `frame`
  without an `id`, so today the key is `fit.updated`.
- **Confidence:** read from `fit.conf` when present.

## Live input

Feed the house's existing walls plus the current SCAN's fits as one extra batch:
- use a fixed key (e.g. `"live"`) and `conf = Wall::confidence`;
- don't feed the current scan alone: merging, stacks, corner cuts and Meta's room chain (`meta_cycles`) need
  the neighbours.

What to expect from it:
- **Height:** the live fits are already in the scene-solved house frame, so `frames()` should give `dy ≈ 0`.
  A nonzero `dy` is worth showing as a warning.
- **Weak fits:** early fits sit below `minConf` 0.4 and stay out until they firm up.
- **Old drifted walls:** the 02:15 / 03:03 / 09:32 UTC depth walls in the house are shifted against Meta. If
  they clutter the live view, filter out depth walls that have no `frame` field (the same rule as
  frame-suspect).

## Matching Python to 1 mm

**Numbers**
- Use `double` throughout.
- `wrap(a, p) = (a + p/2) mod p - p/2` uses Python's floored modulo: `fmod` plus a sign correction.

**Ordering** (Python's sorts are stable)
- `suppress_stacks` sorts by `-weight`, with ties left in input order (the doc order of walls).
- `merge_colinear` sorts by `off`, then by `t0`.
- Use `std::stable_sort` throughout.

**`axes_peak`**
- The bin is `int(a*2) % 180`, where `a = deg(yaw) mod 90` is always >= 0 (add 90 after `fmod`).
- Smooth over +/-4 bins.
- The peak is the **first** maximum, then add 0.25.

**Other details**
- `_cluster` takes `cur[len//2]` (the upper median), not the mean.
- `frames()` iterates batches in sorted key order. "Lowest floor" means the minimum `y` over up-facing planes
  of at least 1 m².
- Compare `dropped` by id plus reason **prefix** (`off-axis`, `low confidence`, `slanted`, `short`,
  `in front`, `inside a Meta room`, `frame:`), not by the formatted numbers.

## Budget

- **Phase 1** (everything up to `outside_scene_rooms`) is cheap: tens of walls, O(n²) at most.
- **Phase 2** (`rooms()` ray casting: cells x 4 rays x walls) is most of the ~24 ms Python takes on rev 25.
  Keep it off the 2 ms path, at about 1 Hz on a worker.
