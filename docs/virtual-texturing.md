# Virtual texturing for the extra screens (protocol v2)

Owners: headset compositor = work-c4 (room_lab), sender = work-09 (atlas/AtlasSender.swift).
Status: spec agreed by message 2026-11-03; v2 fields are additive, so either side can ship first.

## Why

Tens of screens can't all be streamed at full detail; the user mostly looks at one (cinema) or none (VR game).
Like a game engine's virtual texturing, only what is seen is fetched at the detail it is seen at, everything
else stays resident at whatever detail it last had, and a coarse fallback is always available instantly.

What v1 already does (keep it):

| v1 piece | VT term |
|---|---|
| demand at 5 Hz: `vis`, `px`, `pri` per screen | feedback pass |
| width tiers 480/640/960/1280/1920 from `px` | mip selection |
| hidden screens lose their SCStream after 2 s, cell keeps the last frame (`stash`) | eviction to resident copy |
| bitrate split by `pri` | streaming priority |

What v1 lacks (this spec):

1. **A ceiling.** One cell is at most 1920x1200, so a cinema-sized screen that spans ~3800 headset px is capped
   at half its detail. Fix: a screen can occupy several cells (tiles) at one level.
2. **Pop-in / blank on level change.** A tier change gives the screen a new cell size; the headset recreates its
   layer swapchain and shows the placeholder until the next frame. Fix: the headset keeps showing the old
   image (any level) until the new one has arrived ("show the low mip until the high one is in").
3. **No residency knowledge.** The headset can't tell which level it shows. Fix: cells carry their level and
   source rectangle.
4. **Hidden screens waste atlas area and decode time.** Fix: hidden cells may shrink to a mip-tail thumbnail;
   the headset stops copying hidden screens and keeps their last sharp frame locally.

## Terms

- `srcW x srcH`: the screen's Mac display in backing pixels (what `/displays` reports).
- Level `L`: the screen downscaled to `srcW >> L` wide (`L = 0` is full detail). Levels are octaves; the
  sender may still pick a finer width between octaves (v1 tiers) and reports the exact width in `src`/`w`.
- Mip tail: a thumbnail at most 240 px wide, the coarsest level, cheap enough to keep for every screen.

## Demand (headset -> laptop, UDP :57721, 5 Hz), v2

```json
{"v":2,"t":123,"budgetKbps":60000,
 "screens":[{"id":2,"vis":1,"px":2400,"pri":1.0,"lvl":0,"have":1}, ...]}
```

- `v`: 2. Absent = v1.
- `vis`, `px`, `pri`: unchanged.
- `lvl`: wanted level, chosen by the headset (below). Senders without v2 ignore it and keep using `px`.
- `have`: level the headset currently shows for that screen (-1 = nothing yet). Lets the sender prefer sending
  a screen the headset has nothing for (mip tail first), and tells benchmarks how much is shown below wanted.

Headset level choice (per screen, every demand tick):

```
want = clamp(floor(log2(srcW / max(px, 1))), 0, tailLevel)   // coarsest level still >= px wide; srcW from src, else 1920
up   : apply at once when want < current                      // detail is visible immediately
down : apply after 3 s with want > current                    // shrinking saves bits but costs a keyframe
hidden (vis 0): lvl = tailLevel                               // sender may shrink it; headset keeps its sharp copy
```

## Layout (laptop -> headset, text packet 0x80 before every keyframe), v2

```json
{"atlas":[3840,2304],"gen":17,
 "cells":[{"screen":2,"x":0,"y":0,"w":1920,"h":1200,"lvl":0,"src":[0,0,1920,1200]},
          {"screen":2,"x":1920,"y":0,"w":1920,"h":1200,"lvl":0,"src":[1920,0,1920,1200]},
          {"screen":3,"x":0,"y":1200,"w":240,"h":150,"lvl":4,"src":[0,0,3840,2400]}]}
```

- `gen`: increases with every layout change; restarts at 0 with the sender, so compare for change, not order.
- Per cell: `lvl` and `src` (the part of the display, in display pixels, this cell covers; default the whole
  display). One screen may have several cells at the same level (tiles); together their `src` rects cover
  the display. v1 headsets read the first cell per screen only, so the sender sends tiles only to `v:2` demand.
- Tile size: at most 1920x1200 per cell; the atlas stays within 3840x2304: H.264 level 5.1 (36,864 macroblocks)
  for the Quest's decoder, and the Mac's hardware encoder tops out at 4096x2304 (at 3840x2400 it produced no
  frames at all, 2026-11-03).
- `src` edges: tile edges map to display pixels per axis (rounded), and a frame's far edges map to the display's
  edges, so one screen's `src` rects tile its display exactly. The cell itself may be up to 2 px off the display's
  aspect (frames are rounded to even sizes). `bench/vt_check.py` checks all of this offline.
- Cinema rule (sender): when one screen has `pri` 1 and `lvl` 0 and the rest are hidden, that screen may take
  the whole atlas as tiles while the others are mip-tail cells.

## Headset compositor (room_lab, flag VT, off by default until proven)

Each extra screen has one quad layer (`PanelLayer`) whose swapchain is the resident copy.

1. **Hold:** when a screen's cell size or level changes, the new swapchain is built next to the old one; the old
   layer stays submitted until the new one has its first image. No blank frame, no placeholder flash.
2. **Hidden screens are not copied.** Today every atlas frame is blitted and mip-mapped into every screen's
   layer (10 screens = 10 blits + 10 mip builds per frame). Under VT only screens with `vis` 1 are updated; a
   hidden screen keeps its last sharp image, which is what the user sees first when turning back, until the
   fresh capture arrives (~1 frame after the sender restarts its capture).
3. **Tiles:** cells of one screen are blitted into that screen's layer at their `src` rect scaled to the level
   (one blit each), then one mip build.
4. **Coarse never overwrites sharp while hidden:** a mip-tail cell is used only for a screen with nothing better
   (first appearance, or after its layer was dropped for memory).
5. Status (`files/status.json` -> `atlas`): per screen `lvl` wanted, `have`, holds, skipped copies, so the bench
   can report time-to-sharp after a reveal and frames shown below the wanted level.

## Fallback and compatibility

- v1 sender + v2 headset: `lvl`/`have` ignored; holds and hidden-skip still help.
- v2 sender + v1 headset: no `v:2`, so no tiles and no tail shrink; sender behaves as v1.
- Stale demand (> 2 s): sender returns to everything visible, full size (unchanged).

## Benchmarks this enables (bench/stress.py)

- time-to-sharp after a reveal (headset `have` reaching `lvl`), target < 150 ms with the held copy shown at once;
- atlas bitrate in cinema-10/20 with hidden screens as mip tails, target: cinema screen at level 0 under 25 Mbit/s;
- headset GPU time for layer copies, before/after hidden-skip.

## 3D content (not this protocol)

3D models are rendered on the headset from streamed assets (glTF with LODs, KTX2/Basis textures), the same
VT idea applied to meshes and textures; the video atlas stays for Mac screens and remote-rendered heavy scenes.

## v3: hero streams (video LOD ladder; docs/sources.md roadmap 2 and 4)

Agreed 2026-11-03 (work-c4 headset/spec, work-09 sender). Inside the atlas a cell's size is its level, and every level
change costs a whole-frame IDR, so they stay rare (3 s headset hysteresis, 5 s sender refit gate). A screen that
deserves it (the focused cinema screen, a video) leaves the atlas and becomes a **hero stream**: its own encoder
session (or forwarded cached segments for video, no encoding), where a rung switch is an IDR on that stream only.

- **Transport: multiplexed on the atlas TCP connection** (one connection, the existing pacing). Packet type
  `0x81`: `u32 length, 0x81, u16 screen, then flags, pts u64, w u16, h u16, 3 codec bytes, Annex B` (the v1 video
  packet with a screen id). SPS/PPS in-band with every IDR, so a rung switch needs nothing else.
- **Layout**: v2 layout plus `"hero": [ids]`: those screens have no atlas cell; the headset decodes their 0x81
  packets. A screen moves atlas <-> hero only at a layout change (keyframe on both).
- **Rungs**: the hero's width follows the same level rule (`vt::levelFor` from `projectedPx`, hysteresis in
  `vt::Level`); switches only at a closed-GOP boundary (1 s GOPs for cached video). Bitrate per rung comes out of the
  BUDGET arbiter's atlas share (per-screen shares already exist in `budget::Output.screenKbps`).
- **Headset decoders**: one MediaCodec per hero, configured with `max-width`/`max-height` (adaptive playback; all
  Quest c2.qti avc/hevc decoders declare it), so a rung switch is an IDR at the new size on the same decoder with
  an `OUTPUT_FORMAT_CHANGED` (new size -> the screen's PanelLayer; the VT hold rule shows the old image until the
  new size has a frame). Budget (media_codecs_anorak.xml): 16 instances; low-latency throughput 3,916,800
  macroblocks/s (~4K@120). With the game stream (2560x1440@60, ~0.86M) and the atlas (2400x1200@60, ~0.68M), about
  3-4 heroes at 1080p60 (0.49M each) or one 4K60 hero (1.94M); to be measured on the headset.
- **Prototype evidence** (work-09, bench/ladder_proto.sh, bench/ladder_sim.cpp): 3 rungs, 1 s closed GOPs, walk-up /
  orbit / walk-away: 4 switches/min, 0 decode errors across resolution changes, 62% of the always-full bytes;
  offline over vt.h: walk 4.0/min, random walk 2.8/min, standing jitter 0, cinema 0, < 1% of time below wanted.
- **Fallback**: a sender without hero support never sends 0x81 or "hero"; a headset without it sends no
  `"hero":1` capability in demand (`"v":3` demand adds `"heroOk":1`), so the sender keeps everything in the atlas.
- **Who is a hero (decided by the headset)**: v3 demand per screen adds `"hero":1` when the screen is visible, its
  projected width (`projectedPx`) is >= 1280 and its gaze priority >= 0.8, at most 3 heroes (the decoder budget
  above), with the level hysteresis (becomes hero at once, stops being one after 3 s). The sender follows demand;
  with stale demand everything returns to the atlas.
- **Codec**: HEVC for heroes by default (the 2026-11-03 headset pair dropped H.264 for the game stream: 36 vs 43 fps,
  p99 95 vs 60 ms); v3 demand may carry `"heroCodec":"h264"|"hevc"`.
- **Sender side** (work-09, `--flags hero`): the hero's frames (any source) feed its own VTCompressionSession at the
  level size, zero copy, no atlas blit; a level change recreates only that session (one IDR on that stream, none in
  the atlas); pacing and pend1 apply per stream on the shared connection.
- **0x81 packet, exactly** (work-09): `[0x81][u16 BE screen][flags: bit0 key, bit2 HEVC][pts u64][w u16][h u16]`
  `[3 codec bytes: H.264 profile/compat/level, zeros for HEVC][Annex B, VPS/SPS/PPS before every IDR]`. The codec is
  per packet because the atlas path assumes H.264. `"v":3` demand also means vt on. Hero bitrate for now:
  min(20 Mbit/s, applied atlas bitrate / (heroes + 1)); the BUDGET per-screen split comes later. A peer connect
  forces a keyframe on every hero; a screen moving into or out of the atlas is an atlas layout change plus a fresh
  hero encoder.
- **Cell width (2026-11-03 finding)**: octave levels are too coarse for small screens. A wall screen spanning ~500 px
  gets level 1 = 960 px from `floor(log2(1920/500))`, where v1's tier is 640, and synth wall-10 vt over Wi-Fi sent
  3840x1808 instead of 3840x800 (fresh 62 -> 39 fps, gap p99 32 -> 114 ms). The sender sizes each cell as the
  smallest of its finer widths (480, 640, 960, 1280, 1920, ..., srcW) that covers the demand's `px`; `lvl` stays
  the rounded log2 level for the headset's residency and hold rules.
