# VT and HERO: 3-minute visual check in the headset

VT ("sharp only where you look") and HERO ("own stream for a big screen") passed the stand-in benchmarks: on the
laptop's test copy, VT lifted the screens from 49 to 62 fps and cut the worst stutter from 111 to 28 ms, inside the
42 Mbit/s budget; HERO kept the big screen at 60 fps within the same budget (bench/results/campaign-A-*). Neither has
been seen working in the headset yet. VT broke the 3 November demo, so it stays off until this check passes.

## Before you put it on (an agent does this)

1. The laptop's sender runs with `--flags vt,hero` (they only work together with the headset's switches), deployed
   with air_smoke and the previous spec kept for instant rollback. Logged in bench/activity.log.
2. The headset runs the current build with VT and HERO **off**, as you left it. Four screens placed as usual.

## The check (about 3 minutes)

| Step | Do | Pass | Fail (stop, switch it off) |
|---|---|---|---|
| 1. Baseline (30 s) | Settings → EXPERIMENTAL, VT and HERO off. Look at each screen. | Your normal setup: 4 screens where you put them. | (nothing to judge yet) |
| 2. VT on (60 s) | Turn **VT** on. Read small text on the main screen from where you sit; walk 2 m back and read it again; look away and back at another screen. | Text as sharp as before; the screen you look back at is sharp within a moment; all 4 screens stay where they were and keep drawing. | Any screen moves, vanishes or is drawn inside a wall; screens only appear near the 3D model; text blurry for more than a second after you look at it; the cursor ring or the snap bar is gone. |
| 3. HERO on (60 s) | Turn **HERO** on as well. Watch a moving video on the main screen; glance at the other screens. | The video is smooth; the other screens keep updating; nothing moves. | The main screen freezes or blanks; the others stop; anything from step 2's fail list. |
| 4. Off again (15 s) | Turn HERO, then VT off. | Back to step 1 at once, nothing lost. | Anything stays changed. |

Say "VT pass" / "VT fail: what you saw" (and the same for HERO). On a fail, an agent puts the laptop's sender back to
its previous spec and the switches stay off; nothing else changes on the headset.

## Known failure signs from 3 November (catch them immediately)

Screens visible only near the 3D model or drawn into the walls; the cursor ring missing; the move bar no longer
snapping; AMBI (the glowing frame) gone; clicks on the Mini screen landing in the wrong place.
