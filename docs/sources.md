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

(work-09: synthetic source, SCK source, where a WebGPU renderer plugs into AtlasSender.)
