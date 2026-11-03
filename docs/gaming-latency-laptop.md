# Gaming latency on the laptop source (Sunshine -> headset), with the extra screens running

Goal: real-time gaming from the laptop (Sunshine, main stream) while tens of extra screens stream from the same
laptop (pinpin-atlas). This note lists where the laptop side can add latency and the flag that tests each one.
Each change ships as a flag and stays only if `bench/gate.py` on a `stress.py --headset --game LAPTOP` pair proves
it (frame latency p50/p99 from `video.latHist`, `hostMs`, stalls).

## What the laptop does today (2026-11-03)

| Piece | Setting | Source |
|---|---|---|
| Game encoder | HEVC, `hevc_videotoolbox` (hardware), chosen by the headset's Moonlight | Sunshine log: `ACTIVE_HEVC_MODE: 3` |
| Real-time mode | **`vt_realtime = disabled`** (set 2026-11-02 as a sharp-text quick win, docs/sharp-text.md phase 0) | `~/.local/share/pinpin-sunshine/sunshine.conf` |
| Low-latency rate control | not used: FFmpeg enables it only for H.264 with `AV_CODEC_FLAG_LOW_DELAY`, and the stream is HEVC | FFmpeg vtenc; string present in the build |
| B-frames, GOP | none (Sunshine sets `max_b_frames 0`), infinite GOP with IDR on request | Sunshine defaults |
| `PrioritizeEncodingSpeedOverQuality` | ignored: "not supported on this device" (MacBook Air) | Sunshine log warning |
| Atlas encoder | H.264 hardware, real-time, low-latency RC, no reordering, up to 3840x2304 at 60 fps | atlas/AtlasSender.swift |

The Air has one media engine. The game encode and the atlas encode share it, and so do Sunshine's capture and the
atlas's ScreenCaptureKit streams (replayd).

## Suspects, in expected order of impact, and the flag for each

1. **Sunshine real-time mode off.** With `RealTime` false, VideoToolbox may queue frames and spend longer per frame
   for quality. Game frames then wait in the encoder: worst under load, when the atlas also encodes.
   Flag: Sunshine `vt_realtime` per run. Sunshine takes `name=value` overrides on its command line, so `stress.py`
   can append `vt_realtime=enabled` to the `sunshine` Host plugin's args for one run and restore them after
   (proposed: `--sunshine vt_realtime=enabled`). Cost if adopted: some text sharpness on the main panel, which
   the sharp-text refinement (exact tiles) and QUAD already cover for still text.
2. **The atlas competing for the media engine.** A 3840x2304 atlas at 60 fps is a large share of one engine.
   The atlas now logs `encode ms p50/p99/max` every 30 s (submit to output). If it rises while a game runs, the
   engine is contended. Flag: `gameyield` (sender) encodes the atlas at most every other tick while demand says
   `"game":1`. Headset side: send `"game":1` in demand while the Moonlight stream is playing (room_lab, next to
   `budgetKbps`; BUDGET already lowers the atlas's bitrate share).
3. **HEVC without low-latency rate control.** H.264 gets VideoToolbox's low-latency rate control (shorter
   encode, steadier frame sizes, so fewer packets queue behind a big frame). Flag: headset Moonlight codec
   choice H264 vs HEVC for the LAPTOP host. Costs bits per quality; at 80 Mbit/s on the LAN that is affordable.
4. **Shared Wi-Fi airtime.** The laptop sends game (up to 80 Mbit/s) plus atlas (up to 40) over one Wi-Fi link,
   and the headset receives both. Big atlas keyframes delay game packets behind them. Flags already in place:
   `bpcut` (sender: lower bitrate under backpressure), BUDGET (headset: one arbiter for the link), and the
   source pacing in the sender (no keyframe spirals). The gate's stall counts show whether they suffice.

Not suspects: B-frames (none), keyframe interval (IDR only on request on the game stream; the atlas keyframes
every 2 s at most and only on change), and capture method (Sunshine's own ScreenCaptureKit path).

## How to run the A/B

```
# baseline, then each flag alone, nobody wearing the headset (bench/headset_hold.py arms it)
python3 bench/stress.py --headset --game LAPTOP --n 10 --secs 120
python3 bench/stress.py --headset --game LAPTOP --n 10 --secs 120 --flags gameyield     # needs "game":1 in demand
python3 bench/stress.py --headset --game LAPTOP --n 10 --secs 120 --sunshine vt_realtime=enabled   # proposed
python3 bench/gate.py bench/results/stress-*-headset-*.json
```

Read `hostMs` (Sunshine's own processing time, reported to Moonlight) for suspects 1-3 and stalls for 4.

Missing pieces before these runs mean anything:
- `stress.py --flags` only reaches the stand-in's test sender today; a `--headset` run uses the live sender. It
  needs the same treatment there (restart the live `pinpin-atlas` plugin with the flags, restore after).
- `--sunshine name=value` (suspect 1) is not written yet.
- The headset's demand needs `"game":1` while the main stream plays, for `gameyield`.
