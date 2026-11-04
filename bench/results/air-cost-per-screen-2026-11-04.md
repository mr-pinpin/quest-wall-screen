# Air cost per extra screen (K12)

work-42, 2026-11-04. Short profiling pass on the Air (6 virtual displays live, 1 Chrome --app
playing video on screen 4). Read-only, under the bench lock; released. No Mac sudo here, so
`top`/`ps` instantaneous CPU (no powermetrics/GPU-energy — hand that to work-df if wanted).

## Where the CPU goes (top -l interval sample, %CPU = per-core, machine has several cores)

| Component | ~CPU | Process count | Scales with #screens? | Cheap win | Owner |
|---|---|---|---|---|---|
| **colorsync** (aerial color-managing every animated frame) | ~30–72% (top consumer) | 1 daemon (+agent) | YES — one aerial surface per display | **static wallpaper → ~0** | user (System Settings) |
| **aerial WallpaperVideoExtension** | ~22% | 1 (serves all displays) | YES | **static wallpaper → ~0** | user |
| WindowServer (compositing N displays) | ~18% | 1 | partly | unavoidable; fewer/smaller displays | — |
| video decode (VTDecoderXPCServer, the 1 playing --app) | ~9% | per playing video | per playing screen | pause / cap fps on non-focused video | work-ad/c4 |
| **pinpin-atlas capture+encode** | **~5% for ALL 6** | 1 | small per screen | damage flag on static screens | sender |
| Chrome --app idle | ~1.5% | 1 window | small | background-throttle flags | runner |

## Headline

The fps drop with extra screens is **not our capture/encode** — pinpin-atlas is ~5% total for
all six displays. It is the macOS **aerial (video) wallpaper + colorsync**, which together are
the clear top of the profile (~50–94% of identified non-kernel load depending on sample) and
scale with the number of displays (each display is another animated, color-managed surface).
This is the same root cause as the pegged-colorsync finding earlier today.

## Cheap wins, ranked

1. **Static wallpaper (System Settings → Wallpaper → a still picture).** By far the biggest win:
   turns off WallpaperVideoExtension and settles colorsync to ~0 system-wide. This is the fps
   fix. Can't be set from the CLI (confirmed earlier: NSWorkspace + editing the wallpaper plist
   both fail to stop the aerial). Needs one click from the user — already on the morning list,
   now with data: it should recover most of the 36→60 fps gap on its own.
2. **Pause / fps-cap non-focused video screens.** The one playing --app costs ~9% (VTDecoder);
   idle Chrome is ~1.5%. Capping or pausing video on screens the user isn't looking at is the
   next win. Sender/runner side (work-ad runner + work-c4 focus signal).
3. **pinpin-atlas damage flag for static screens.** Atlas is already cheap (~5%), so the ceiling
   is small, but sending fewer frames for unchanging screens trims encode/WindowServer a little.
4. Chrome background-throttling flags: marginal (idle Chrome is already ~1.5%).

## What I could and couldn't test

- Measured the live 6-display state. Could not A/B add/remove displays (that's the user's live
  content, headset worn) nor set the wallpaper (UI-only). So (1) is a strong causal inference
  from the profile + the earlier CLI experiments, not an A/B on this box.
- (2)/(3)/(4) are sender-side and need work-ad/work-c4 to wire the focus/fps/damage signals; I
  can help measure once they do.
