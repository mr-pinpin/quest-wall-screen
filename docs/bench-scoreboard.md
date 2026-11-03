# Bench scoreboard: every performance flag, kept, dropped or pending

The rule (headset flags rule, docs/PERFORMANCE.md): every change ships behind a flag, off until a benchmark
pair proves it, and only what is proven stays. Each row cites its evidence: the result files in bench/results/
(stress-*.json, one row each in stress.md) and the gate verdict (`python3 bench/gate.py FILE`).

Verdicts: **kept** (default on or recommended), **dropped** (removed or left off for good), **pending** (no
pair yet), **kept-verified** (correctness change, verified once, no A/B needed).

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
| vt | protocol v2: levels, tiles, mip tails | cinema-10 @2560x1600 and wall-20, off vs on | layouts: 0 errors (vt_check, vt_headset_test); suite pairs pending | pending |
| gameyield | halve the atlas encode rate while the headset plays a game | needs demand "game":1 | built, untested | pending |

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

## Platform

| Change | What | Evidence | Verdict |
|---|---|---|---|
| PinPin Host plugin adoption | Host restarts leave plugins running | kickstart -k kept pids; adopted atlas still captures (06147ca) | kept-verified |
| Displays helper as Host plugin | one app holds every permission | /displays works under the Host; old LaunchAgent retired (06147ca) | kept-verified |
| 20-screen cap | stress.py refuses > 20 without --allow-over-20 | 30 screens crashed replayd and wedged WindowServer (2026-11-03) | kept until visibility-only capture is proven at 30 |

## Results log

(One line per suite run as results land: date, run, gate verdict, key figures.)
