# Quest Wall Screen

A Mac desktop on a real wall, seen through a Meta Quest 3: passthrough, room-aware placement,
depth occlusion and hand input, streamed live over the home network. Part of the Mr. PinPin home
platform (render or capture at home, stream over the router, display on the TV or the headset).

## Layout

| Path | What it is |
|---|---|
| `room_lab/` | **Native OpenXR app** (`com.pinpin.wallscreen.roomlab`): GLES passthrough app with Meta scene walls, environment-depth occlusion, joint-derived hand ray and pinch, wall placement persisted by anchor UUID, and the hardware-decoded desktop panel. |
| `room_lab/moonlight.h` | Moonlight source: moonlight-common-c (UDP + FEC, encrypted control, HEVC) from the Mini's Sunshine into the same decoder. The app connects as soon as a new `files/moonlight/session.json` appears and falls back to the Mac relay when the stream ends. |
| `room_lab/moonlight_host.py` | Pairs the headset with Sunshine (approving the PIN via the admin API) and launches/resumes a session from the Mini's headset-facing address. Identity stays in a private directory on the VPS and in the app's private storage. |
| `room_lab/third_party/build_moonlight.py` | Builds moonlight-common-c (pinned commit) and mbedTLS 3.6 (pinned SHA-256) for arm64 with the NDK; nothing vendored. |
| `room_lab/video.h` | WebSocket client → MediaCodec H.264 (Qualcomm picture-order low latency) → AImageReader → EGLImage → mipmapped sRGB texture, sampled with CAS-style sharpening. |
| `MacScreenSender.swift` | ScreenCaptureKit + VideoToolbox H.264 sender on the Mac. |
| `transport.py` | Home relay: TLS (client certificates) from the Mac to the Mini, WebSocket to the headset. |
| `index.html`, `viewer.js`, `wall_math.mjs` | First WebXR browser prototype (kept for reference). |
| `native_probe/` | OpenXR capability probe used to confirm the headset firmware's extensions. |
| `spec.json` | Requirements and what has been validated. |

## Room lab

Build (Linux, NDK r30, Android platform 23 jar, `aapt`, `dalvik-exchange`, `apksigner`):

    python3 room_lab/build.py --ndk ~/android-ndk-r30 --out ~/.cache/room-lab \
        --key-store homedev.p12 --key-pass key-password

The builder downloads the Khronos OpenXR loader for Android (Apache-2.0) from Maven Central into the
output directory and pins its SHA-256; it is not vendored here.

Deploy and inspect through the Mini's wireless ADB (`deploy.py install APK [--capture] | status | stop | shell CMD`).
The app writes `files/status.json` every second: frame rate, depth frames, scene entities, hands,
aim/placement, and video (packets, decoded, displayed, decode-to-display latency).

Using it: point the right hand at a wall (palm away). The dashed outline turns green where the
panel fits, red where it is blocked; pinch to place. Left pinch cycles 1.0 / 1.44 / 2.0 m.
Four head-locked dots show passthrough, room model, depth and hands.

## Headset notes (Quest 3, developer mode)

- Scene data needs exactly one Space Setup room; duplicate rooms make the runtime hide all anchors.
- Scene queries need the storage-location filter chained behind a component filter.
- Hand tracking without controllers: preferences `hand_tracking_opt_in` and `hand_tracking_enabled`.
- The Qualcomm decoder holds ~14 frames unless `vendor.qti-ext-dec-picture-order.enable=1`.

## Mini → headset (Moonlight)

    python3 room_lab/moonlight_host.py pair                       # once
    python3 room_lab/moonlight_host.py launch --app "PinPin Atlas" --width 2560 --height 1440 --fps 72

Measured 2026-11-02 on the Mini-hosted PinPin-Quest-Offline Wi-Fi: HEVC 2560x1440 at 57 fps (the app's own rate),
6 ms RTT, ~25 ms from arrival to the panel (41 ms worst), no drops.

## Next

Game-grade transport like the TV path (Sunshine → Moonlight: UDP, HEVC, high bitrate, 60–120 fps,
input), direct LAN instead of the ADB tunnel, Retina capture on the Mac.
