# Content sources: one renderer, many sources

User direction (2026-11-03): Mr. PinPin is one single-GPU video game. We choose how we render, with full control of
virtual texturing. macOS screen recording is just one data source, like 3D models; our own rendering (WebGPU) is
another source we own. Benchmarks are synthetic and not bounded by macOS recording speed.

| Source | Rendered where | Reaches the headset as | Bounded by | Benchmark |
|---|---|---|---|---|
| Mac screens (apps, desktop) | macOS WindowServer | atlas video (ScreenCaptureKit -> AtlasSender) | SCK capture: ~29.4 fps with 1-4 displays, ~8.6 fps per display with 10 animated (2026-11-03) | "macOS screen source" benchmark, gated on its own terms |
| Synthetic screens | AtlasSender itself (work-09: frames straight into the encoder path at 60 Hz) | atlas video | the encoder and the network only | the pipeline benchmark (stress.py --source synth, default) |
| Our own 2D/2.5D content (WebGPU) | the Mac or the Mini (see below) | atlas video, like a synthetic screen | our renderer + encoder + network | same pipeline benchmark, with real content |
| 3D models | the headset (room_lab MODELS: glTF + KTX2/ASTC) | assets, streamed once (docs/3d-content.md) | headset GPU, asset budget | model_test offline; headset runs |
| Game / full desktop stream | Sunshine on the laptop or Mini | Moonlight (HEVC/H.264) | Sunshine encode, Wi-Fi | the gaming gate (bench/gate.py) |

## Headset side: how the compositor takes content we render ourselves

The headset app (room_lab) is a native OpenXR app on OpenGL ES 3 (EGL binding). Everything it shows reaches it in
one of three ways, and all three already exist:

1. **As video in the atlas** (extra screens): a decoded H.264 frame (MediaCodec -> AImageReader -> external
   texture), cut into per-screen OpenXR quad layers (PanelLayer), with virtual texturing (VT flag: levels, tiles,
   mip tails, hold-until-sharp; docs/virtual-texturing.md). **Any surface we render ourselves on the Mac/Mini,
   WebGPU included, plugs in here unchanged**: the sender encodes it like a synthetic screen, the headset neither
   knows nor cares how it was drawn. This is the recommended path for "our own rendering" today.
2. **As a game stream** (Moonlight): full-view content rendered remotely (Sunshine), for heavy scenes.
3. **Rendered on the headset**: walls, UI, cursor and 3D models, drawn by room_lab in GLES into the eye buffers,
   composited above the screen quads (depth-tested holes; docs/3d-content.md).

## Conflict to flag: "rendering is obviously WebGPU" vs the native GLES headset app

- **WebGPU on the Mac/Mini, streamed as video: no conflict.** It is just another atlas source.
- **WebGPU on the headset: conflicts with the current app**, in one of two ways:
  - *In the browser (WebXR + WebGPU in Quest Browser)*: a different app. The native room_lab (passthrough, scene
    walls, quad layers with compositor super-sampling, hand input, Moonlight, VT) would have to be rebuilt as a web
    app, or the two would run separately (only one immersive app holds the compositor at a time). Quad/compositor
    layers, needed for sharp screens, are only partly exposed to WebXR (WebXR Layers).
  - *Native WebGPU (Dawn or wgpu-native) inside room_lab*: WebGPU on Android runs on Vulkan, while room_lab's
    OpenXR session uses the OpenGL ES graphics binding. It would need either a switch of room_lab to the Vulkan
    binding (rewrite the GLES renderer, PanelLayer copies, mip builds, video external textures), or GL<->Vulkan
    interop through AHardwareBuffer per frame (possible, but a second GPU API in one frame loop).
- **Nothing is being rewritten.** Recommendation until the user decides: render our own surfaces with WebGPU on the
  Mac/Mini as atlas sources (path 1), keep room_lab native for compositing, layers and 3D; revisit on-headset
  WebGPU only if a feature needs on-headset rendering that GLES can't do.

## Benchmarks per source

- Pipeline (synthetic source, default): encoder + network + headset decode/composite; screens judged against the
  synthetic change rate (60 Hz per animated screen), gap bounds as in bench/gate.py.
- macOS screen source: the same gate, against the measured SCK capture rate; its own pass/fail, reported separately.
- Gaming: the gaming gate (unchanged). Models: bench/model_test.cpp + headset GPU time (GPUTIME).

## Sender side

The sender is one renderer with several picture sources, like a game with several kinds of assets. Every source
delivers frames for a *screen* (an id the headset knows) into the same pipeline:

```
source -> deliver(frame) -> atlas cells (layout, VT levels/tiles/mip tails) -> H.264 encode -> paced send -> headset
```

`CellCapture.deliver(_:)` is the one entry point. Everything after it (damage compare, stash while hidden, blit into
tiles, prifps, VT sizing, the encoder, pacing, the network) is shared, so a new source only has to produce frames at
the size the layout asks for (`resize(w, h, fps)`) and stop when its screen leaves view (`stop()`).

| Source | Selected by | Produces | Cost / limits | Use |
|---|---|---|---|---|
| **macOS screen** (`CellCapture`, ScreenCaptureKit) | displays helper `/displays`, or `--test S:<displayID>:WxH` | a Mac display's pixels, scaled by SCK to the cell size | WindowServer must drive the display: asleep display = no frames; ~8.6 fps per display with 10 virtual 1920x1200 displays animating, ~30 fps with 1-4 (2026-11-03); every frame is marked dirty | the user's real Mac screens |
| **synthetic** (`SynthSource`) | `--synth "S:WxH,..."` [`--synth-animate "S,..."` or `none`], or `--test S:synth:WxH` | 8 precomputed IOSurface pictures per screen cycled at the capture rate (animated), or one still picture (static) | ~0: no SCK, no WindowServer | pipeline benchmarks (default for stress.py `--source synth`); measures packing, VT, encode, send |
| **owned renderer** (proposed) | `--surface S:<name>` | frames we render ourselves (WebGPU) into a shared IOSurface | GPU time of our own renderer only | our own UI/scenes rendered on the Mac at exactly the needed level |
| **video** (proposed) | `--video S:<file or URL>` | decoded frames (AVPlayerItemVideoOutput, IOSurface-backed) | one hardware decode | cinema content without screen capture |

Heavy 3D content is not a sender source: it is rendered on the headset from streamed assets (docs/3d-content.md).

### Adapting to the macOS screen source

SCK is one source with known costs, adapted around rather than treated as the system's ceiling:
- capture only screens in view (visibility-only capture, default since 4d28e69);
- background screens at lower rates (`prifps`), mip tails for hidden ones (`vt`);
- keep the display awake while a headset is connected (default since a4709fe; `nowake` turns it off).

### Where a WebGPU renderer plugs in (proposal)

Cleanest path: a separate renderer process using **Dawn** (Chrome's WebGPU implementation, C++) or **wgpu-native**,
rendering each screen into a texture backed by an **IOSurface**, shared with the sender by IOSurface ID — zero copies:

1. Renderer creates `IOSurface`s (BGRA, the cell's size for the wanted VT level) and wraps them as `MTLTexture`s;
   with Dawn, `SharedTextureMemory` imports an IOSurface as a `wgpu::Texture`, so WebGPU code renders into it.
2. It announces `{screen, iosurfaceID, w, h, seq}` over a local socket (or a small shared-memory ring of seq numbers)
   after each finished frame (after its command buffer completes).
3. The sender's `SurfaceSource` looks up the IOSurface (`IOSurfaceLookup(id)`), wraps it as a `CVPixelBuffer`
   (`CVPixelBufferCreateWithIOSurface`) and calls `deliver(_:)` — the same path as SCK frames.
4. The sender tells the renderer the wanted size per screen (the VT level from demand), so the renderer draws exactly
   the detail the headset can show — virtual texturing driving a renderer we own.

Why out of process: the renderer can be a Chrome/Dawn build or a web page's WebGPU (through a small native host) and
crash or restart without taking the sender down; IOSurfaces are designed for cross-process sharing. In-process
(linking wgpu into the sender) is simpler to start but couples the sender to the renderer's dependencies.

Not via SCK: capturing a WebGPU page's window through ScreenCaptureKit would put WindowServer back in the loop,
the very cost this source avoids.

## Roadmap: owning the rendering, game-engine style (user direction 2026-11-03), ordered by value

Gaming pairs on the real headset and the router-channel before/after come first; then, in this order:

1. **LOD by viewer pose (headset, next build; feeds the sender's VT).** Each screen's detail follows its
   projected size from the headset pose: distance, viewing angle (a screen seen edge-on needs fewer texels across)
   and field of view, both axes, with hysteresis (up at once, down after 3 s). Today `demandFor()` already uses
   distance and gaze angle for `px` and the level rule has hysteresis, but it ignores obliquity and the vertical
   extent; the new rule projects the screen's corners into the view and takes the larger texel span. Pure function in
   room_lab/vt.h with an offline test that walks a pose path (approach, orbit, turn away) and checks levels change
   monotonically, never flicker, and match the analytic projection.
2. **Video LOD ladder (sender + headset).** Per screen, resolution and bitrate from the projected size: VT levels
   already pick the resolution; add the bitrate rung per level (and per gaze priority) so a distant screen costs
   proportionally fewer bits, inside the BUDGET arbiter's atlas share.
3. **How far SCK goes on this Air (benchmark, cheap).** The macOS screen source, honestly: the most screens that
   capture at near 30 fps (1-4 displays ~29 fps measured; 10 animated ~8.6 fps). Fine if macOS can't do 10: SCK is
   for 1-2 real Mac screens; synthetic and our own rendering carry the rest.
4. **Cached video at several LODs (sender, with work-09).** For video content (a film, YouTube): keep the stream
   pre-encoded or cached at a few resolutions, so a level change switches rungs instead of re-encoding (no keyframe
   storm, no encoder load). Needs a per-screen segment source next to the atlas, and the headset switching rungs on
   a keyframe boundary with the hold rule.
5. **Impostors for 3D models (headset, MODELS flag).** Octahedral impostors (a baked atlas of views) at distance,
   the real mesh up close, chosen by the same projected-size rule as 1; baked offline by assets/make_asset.py.
