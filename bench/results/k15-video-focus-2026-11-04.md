# K15 video focus — CPU before/after

work-42, 2026-11-04. Spare display 60 (off-headset), one muted YouTube /watch video (lofi live
stream), via work-ad's POST /api/screens/60/video. Summed %CPU of VTDecoderXPCServer + all
Google Chrome processes, `top -l` interval sample. Bench lock held + START/END logged.

| State | VTDecoder+Chrome CPU | vs full |
|---|---|---|
| **full** (playing, default quality) | **~79%** | — |
| **low** (state:low, YouTube lowest quality) | **~16%** | −63 pts (~80% off) |
| **paused** (state:pause) | **~15%** | −64 pts |

## Findings

- One full-quality video the user isn't watching costs ~63% of a core (VTDecoder+Chrome summed)
  over its lowered/paused state. That's the per-unseen-video win.
- **`low` captures almost all of it** (~80%); **pause** adds only a little more beyond low
  (the decode is already tiny at lowest quality). So the policy is well-founded:
  - unseen & audible, or unseen <60 s → **low** (keeps audio, kills most decode),
  - unseen & muted ≥60 s → **pause** (stops decode entirely).
- Matches the earlier per-screen profile (video decode was the one real Chrome cost); a real
  /watch video is far heavier than screen 4's 0×0 hover-preview.

## Caveats

- Single pass; measured on the **colorsync/aerial-degraded Air** (air_smoke 9.1 fps at the time,
  per work-ad), so absolute CPU is inflated by the wallpaper load — the **delta** is the signal,
  not the absolute. A static wallpaper (K12) would lower the whole baseline.
- YouTube quality API only means something on a /watch page; on a results/search page (the
  user's screen 4) there's no playing video to cap, so the policy no-ops there.
