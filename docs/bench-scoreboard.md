# Bench scoreboard: every performance flag, kept, dropped or pending

The rule (headset flags rule, docs/PERFORMANCE.md): every change ships behind a flag, off until a benchmark
pair proves it, and only what is proven stays. Each row cites its evidence: the result files in bench/results/
(stress-*.json, one row each in stress.md) and the gate verdict (`python3 bench/gate.py FILE`).

Verdicts: **kept** (default on or recommended), **dropped** (removed or left off for good), **pending** (no
pair yet), **kept-verified** (correctness change, verified once, no A/B needed).

> **2026-11-03 14:22 UTC: rebaseline.** Every screen run before this time ran with macOS aerial (video) wallpapers on the
> test displays, so idle and hidden screens were never still and cost a full capture + encode. Those runs are
> superseded (moved to bench/results/aerial/, marked in stress.md) and are not used for any verdict below.

## The bar

| Goal | Criterion | Status |
|---|---|---|
| Real-time gaming | gate.py gaming: fps >= 55, frame latency p50/p99 <= 25/50 ms, motion-to-photon est. <= 50 ms, no stall > 100 ms, headset >= 70 fps | pending (headset asleep) |
| Tens of screens | stress.py checks at 10 and 20 screens, all scenarios: atlas <= 40 Mbit/s, no freeze > 5 s, sender never crashed, internet ping p95 < 50 ms | pending (suite running) |
| Cinema illusion | cinema-10 with VT: focused screen at level 0, others tails, atlas <= 25 Mbit/s | pending |
| VR game | vr-10: atlas ~0.1 Mbit/s | measured 0.1 Mbit/s (earlier run); rerun pending |

## Laptop sender (atlas/AtlasSender.swift, `--flags`)

| Flag | What | Pair | Evidence | Verdict |
|---|---|---|---|---|
| visibility-only capture | capture only screens in view (crash fix after 30 screens) | correctness | Air test: hidden screen pauses, reveal resumes (4d28e69) | kept-verified |
| stuck-exit | exit when a refresh hangs, Host restarts it | correctness | validated before the reboot (4d28e69) | kept-verified |
| bpcut | bitrate x0.7 under backpressure | cinema-10 --wan 3, off vs on | work-09 single A/B: frames 148 -> 318, keyframes 23 -> 7, gap p99 583 -> 343 ms; suite pair pending | pending |
| vt | protocol v2: levels, tiles, mip tails | synth cinema-10 off vs on, Wi-Fi and localhost; judged as part of vt+longgop(+pace) with 3 repeats | synth cinema-10 Wi-Fi (stress-20261103-165829 vs -172611): fresh 48 -> 56 of 60 (encoder ceiling lifted), but gap p99 32 -> 105 ms, max 248 -> 420 ms, 29 keyframes/min (full-atlas bursts over Wi-Fi); work-09 localhost: fresh 62, p99 22.5 ms. verdict.py said DROP on the single Wi-Fi pair; held as PENDING (work-df) until the localhost pair and the vt+longgop+pace repeats | **pending** |
| longgop | periodic keyframes every 10 s instead of 2 s (forced ones unchanged) | cinema-10, off vs on (and with vt) | Air A/B, 2 moving screens, 30 s: keyframes 16 -> 4 (~490 KB each), fresh fps 40 -> 46 | pending (suite pair) |
| gamecap | while a game plays ("game":1), the atlas is laid out within 1920x1200 | --headset --game LAPTOP, baseline vs gameyield,gamecap (gaming pair 6) | encprobe: game encode 40 fps / 277 ms -> 60 fps / 14 ms alone, 13 ms with gameyield (bench/results/encshare-2026-11-03.md) | pending (headset pair) |
| damage | frames with no dirty rects are not copied (no re-encode); copies cover only changed rows | wall-10 --static 7, off vs on; gaming pair 5 with prifps | encprobe: no effect; SCK delivered still screens at full rate with dirty rects (0% clean) | pending, likely drop |
| prifps | per-screen capture rate by priority (>= 0.8 full, >= 0.3 15 fps, else 10); reduced-rate changes coalesce to 15 encodes/s | cinema-10, off vs on; gaming pair 5 with damage | encprobe: atlas 40 -> 29 fps but the game encode stays overloaded (264 ms); with gameyield,gamecap: p99 22.8 -> 18.5 ms | pending |
| gameyield | while the headset plays a game ("game":1), encode the atlas on 1 tick in 2-4 (more when its own encode time shows contention) | --headset --game LAPTOP, off vs on (bench/gaming_pairs.sh) | built; laptop test queued behind the suite | pending |

## Headset (room_lab, settings flags)

| Flag | What | Pair | Evidence | Verdict |
|---|---|---|---|---|
| VT | hold old image on resize, skip hidden copies, tiles, v2 demand | headset VT on/off with sender vt | desktop tests pass (bench/vt_headset_test.cpp); hardware pending | pending |
| BUDGET | one arbiter: atlas / game / assets by mode, detail before frames | headset --game with 10 screens, off vs on | unit tests pass (bench/budget_test.cpp); hardware pending | pending |
| GPUTIME | GPU timer queries for the budget's frame time | gpuSupported / gpuMs in status | builds; hardware pending | pending |

## Game stream (Sunshine on the laptop, Moonlight on the headset)

| Setting | What | Pair | Evidence | Verdict |
|---|---|---|---|---|
| vt_realtime | VideoToolbox real-time encoding (off since 2026-11-02 for sharp text) | --sunshine vt_realtime=enabled vs current, gate hostMs | docs/gaming-latency-laptop.md | pending |
| codec | H.264 (low-latency rate control) vs HEVC | headset --game, both | | pending |

## Pass/drop rules (mechanical)

`python3 bench/verdict.py RULE OFF.json ON.json` applies these to a pair (same scenario, run back to back) and
prints KEEP, DROP or RERUN with the figures. A second RERUN on a fresh pair is DROP: a flag must show its effect,
not hover in the noise. Every rule also drops on any gate check that passes OFF and fails ON.

| Flag (rule) | Expected effect | KEEP when ON... | DROP when ON... |
|---|---|---|---|
| bpcut (`bpcut`), cinema-10 --wan 3 | under backpressure the encoder sends smaller frames instead of stalling: fewer keyframes, smaller gaps | fresh-frame gap p99 <= 0.8x OFF and keyframes <= OFF | gap p99 >= OFF, or keyframes > 1.2x OFF |
| longgop (`longgop`), cinema-10 | no periodic whole-atlas keyframe every 2 s: fewer bursts on Wi-Fi, smaller fresh-frame gaps | same as bpcut: gap p99 <= 0.8x OFF and keyframes <= OFF | same as bpcut |
| vt at cinema-10 @2560x1600 (`vt-cinema`) | hidden screens become 240-px mip tails, the focused screen gets tiles at level 0: far fewer bits | atlas Mbit/s <= 0.8x OFF, below-wanted <= 10% | layouts invalid (vt_check), Mbit/s >= OFF, or fresh fps more than 5 below OFF |
| vt at wall-20 (`vt-wall`) | all screens visible and small: roughly neutral; vt must not cost anything | Mbit/s <= 1.1x OFF and gap p99 <= 1.2x OFF, layouts valid | otherwise |
| vt at cinema-10, latency (`vt-latency`), pre-registered 2026-11-04 ~04:55 UTC before block A2 (work-df) | real-time play + many screens: frame rate and latency inside the atlas budget | in 2 of 3 clean pairs: fresh fps >= OFF + 5, gap p99 <= 0.5x OFF, Mbit/s <= 42 | fresh fps < OFF, gap p99 >= OFF, > 42 Mbit/s, or layouts invalid |
| vt overall | | from A2 on: KEEP in vt-cinema OR vt-latency (both reported), and vt-wall; then "proven on stand-in, waiting for a headset visual check" (it broke the 11-03 demo); never in the user's APK defaults before they have seen it | DROP in both cinema rules, or in vt-wall |
| damage (`damage`), wall-10 --static 7 | still screens stop costing copies and re-encodes: lower sender CPU, same stream | sender CPU <= 0.85x OFF (stream unchanged) | CPU >= 0.95x OFF, gap p99 > 1.1x OFF, or Mbit/s > 1.05x OFF |
| prifps (`prifps`), cinema-10 | background screens at 10-15 fps: fewer bits and encodes, the focused screen keeps full rate | atlas Mbit/s <= 0.85x OFF, fresh fps within 5 of OFF | Mbit/s >= 0.95x OFF or fresh fps more than 5 below OFF |
| damage,prifps (`encshare`), --headset --game LAPTOP | the atlas's share of the media engine shrinks, so the game encode waits less | same as gameyield | same as gameyield |
| gameyield,gamecap (`gamecap`), --headset --game LAPTOP; **leading candidate for the gaming gate** | the atlas stops competing with the game encode: game hostMs and frame latency back to the no-atlas level | same as gameyield | same as gameyield |
| gameyield (`gameyield`), --headset --game LAPTOP | the atlas encodes 1 tick in 2-4 while a game plays, so the game encode waits less for the media engine: lower hostMs and frame latency; the atlas drops to 15-30 fps by design | game frame latency p99 <= 0.9x OFF, or host ms p50 >= 1 ms lower; game fps >= 55 | latency p99 and host ms both >= OFF, game fps < 55, or 2+ more stalls over 100 ms |
| vt_realtime=enabled (`vt_realtime`), --headset --game LAPTOP | VideoToolbox real-time mode: no frame queueing in the encoder, lower hostMs; costs some sharpness on still text (covered by sharp-text tiles and QUAD) | same as gameyield | same as gameyield |
| codec H.264 (`codec-h264`; OFF = HEVC), --headset --game LAPTOP | low-latency rate control (H.264 only): shorter, steadier encodes, fewer packets queued behind big frames | same as gameyield | same as gameyield; equal latency is DROP (HEVC stays, better quality per bit) |

Run order agreed with work-c4: 1 vt_realtime, 2 gameyield (needs headset v0.39 for `"game":1`), 3 codec
(v0.39 CODEC flag). Runner: `bash bench/gaming_pairs.sh`.

## Platform

| Change | What | Evidence | Verdict |
|---|---|---|---|
| PinPin Host plugin adoption | Host restarts leave plugins running | kickstart -k kept pids; adopted atlas still captures (06147ca) | kept-verified |
| Displays helper as Host plugin | one app holds every permission | /displays works under the Host; old LaunchAgent retired (06147ca) | kept-verified |
| 20-screen cap | stress.py refuses > 20 without --allow-over-20 | 30 screens crashed replayd and wedged WindowServer (2026-11-03) | kept until visibility-only capture is proven at 30 |

## Results log

(One line per suite run as results land: date, run, gate verdict, key figures.)
