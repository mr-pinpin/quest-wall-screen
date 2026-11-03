# 3D content next to the screens: plan

How locally rendered 3D models coexist with the streamed screens (docs/virtual-texturing.md) and the game
stream on the Quest 3. Figures marked *to measure* are estimates until the bench has them.

## Where things are rendered

| Content | Rendered on | Delivered as |
|---|---|---|
| Mac screens (extras) | Mac, captured | one H.264 atlas, virtual-textured (levels, tiles, mip tails) |
| Main game / desktop stream | Mac or Mini | Moonlight (HEVC) |
| Heavy 3D scenes (full VR games, path tracing) | Mini | the game stream, full view |
| 3D models in the room (objects, avatars, props) | **the headset** | streamed assets: glTF 2.0, meshopt meshes with an LOD chain, KTX2 (Basis) textures with mips |

Rendering models on the headset keeps them at 72-120 Hz with zero network latency on head motion; only
assets cross the network, once. Remote rendering stays for what the XR2 Gen 2 cannot draw.

## The same virtual-texturing idea, applied to assets

- Feedback: each frame the headset already knows every object's screen-space size (as it does for screens, `px`).
- Selection: mesh LOD by screen-space error; texture level by texel density, the same floor(log2) rule as screens.
- Residency: the coarsest LOD and the KTX2 mip tail load first (a few KB) so an object is never missing;
  finer levels stream in by priority (gaze, size, distance), and the coarse level shows until they arrive (the
  screens' hold rule).
- Transport: HTTP range requests to the Mini (KTX2 levels and glTF buffers are addressable ranges), cached on
  the headset's storage.

## Compositor order (back to front)

1. Passthrough.
2. Quad layers: the main panel and each extra screen (compositor-sampled, super-sampled, sharp text).
3. Projection layer (eye buffers): walls, UI, cursor, **and 3D models**. Wherever a screen is visible the eye
   buffers write alpha 0, depth-tested; a model in front of a screen writes alpha 1 and covers it. This is
   the wall-occlusion path that exists today (uOcclude), extended to models.
4. A model *behind* a screen needs nothing: the screen's hole is only cut where the screen is nearest.

Limit: quad layers cost compositor time each. Past ~8 visible quads (*to measure*), the farthest screens
are drawn into the eye buffers from the atlas instead (lower quality is invisible at that size).

## One budget, split by mode

Per frame at 72 Hz the GPU has 13.9 ms; the network gives the headset ~400 Mbit/s on 5 GHz (*to measure*);
the hardware decoder must run the game stream and the atlas together (*to measure*: the --game bench run).

| Mode | Game stream | Atlas | Assets | Headset GPU |
|---|---|---|---|---|
| Cinema (one big screen) | off | target <= 25 Mbit/s, one screen at level 0, the rest mip tails | idle | screens only |
| Desk (several screens) | off | <= 40 Mbit/s by gaze | background | screens + light 3D |
| VR game (remote) | 30-60 Mbit/s | ~0.1 Mbit/s (measured: all tails) | paused | game layer only |
| Mixed room (models + screens) | optional | by gaze | burst, rate-limited | 3D <= 8 ms, copies <= 2 ms, UI <= 1 ms |

The arbiter is the headset, as now: `budgetKbps` in demand already caps the atlas; it extends to the game
stream's requested bitrate and the asset fetch rate (assets yield first: they pause while the game stream's
RTT rises). On the GPU side, when frame time passes ~12 ms the headset lowers levels of unfocused screens
and LODs of unfocused models before it ever drops a frame.

## Steps

1. Measure: per-quad compositor cost, decoder headroom with game + atlas + a 4K video, Wi-Fi throughput to the headset.
2. A glTF + KTX2 loader in room_lab (or StereoKit as the renderer, which has both) behind a flag; one model.
3. LOD/mip residency with the VT rules above; depth-tested holes in the eye buffers for screens behind models.
4. Bench: the gaming gate (bench/gate.py) with N models on screen; pass = no change in game latency/stalls.
