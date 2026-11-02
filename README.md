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

## Input: aim is never disturbed

Hand input follows one rule: the act of clicking must not move the aim.

- **Aim** runs from an estimated shoulder through the index and middle knuckles, joints that stay still
  while the thumb and fingertips move.
- **Press** is the finger gun: point, then lower the thumb onto the side of the middle finger. Pinch still
  works as a fallback.
- **Rewind and lock**: a press uses the aim from ~0.1 s earlier (before the gesture started).
- **Clicks only**: every press is a single click at that spot (button down, up 40 ms later). The button is
  never held while the hand moves, so there is no accidental text selection or window drag.
- **Smoothing** is adaptive: heavy when the hand is still (precision), light when it moves fast (no lag).
- **Feedback** (draw, loose): the cursor ring tightens and turns gold as the thumb closes; a solid dot marks
  the click.
- Screens stay put. Move one by holding a press on the bar under it; resize by a corner, or with both hands.
- **Settings**: turn the back of the left wrist toward your face and hold ("check your watch"; a ring fills)
  to open or close a floating panel: source (MINI, LAPTOP), SMALL/MEDIUM/LARGE, LOCK (hides the move bar and
  corners) and CLOSE. Palm toward the face is left alone: that is Meta's system menu gesture.
  Unlocked, the panel has its own grab bar: hold, drag, and it sticks flat to the wall you point at.
- **Dictation**: raise a fist above the chest and hold to talk; open the hand to finish. The headset mic is
  cut at pauses into short segments, each sent once to the Mini's speech service (`~/asr-service`, Parakeet
  TDT 0.6B on MLX, port 8765), and the text is typed into the streamed Mac where the cursor is.
- **Send**: finish with a throw (push the arm forward and open the hand) to press Enter after the last words.
- **Undo**: within 15 s of dictating, sweep an open hand to the left to erase the last sentence (one per swipe).

## Next

1. **Onboarding calibration.** A guided first run that asks the user to aim and "shoot" at targets
   (finger gun, pinch, hold, drag) and records labelled joint traces. The labelled set gives per-user
   thresholds for the thumb press, the lock angle and the smoothing, replacing today's fixed values
   (see the `thumb*`, `pinchPresses` and `lockBreaks` fields in `status.json`). Onboarding will matter
   for everyone who uses the platform, so build it as its own reusable stage.
2. **Settings panel**: first version shipped; add sharpness, recenter and per-screen options.
3. **Hand occlusion halo.** A hand in front of a screen cuts a blocky, pixelated hole around itself that
   shows the wall instead of the hand over the screen. Environment depth is low resolution, so its edges
   step. Fix with soft (feathered) depth occlusion and, for hands, the tracked hand mesh as the occluder.
4. **Free-standing screens** anchored anywhere in the apartment, not only on walls from the room scan.
5. **Sharper panel**: hand the video to the compositor as its own layer instead of resampling it through
   the eye buffers.
6. Deferred: **several screens at once** (moonlight-common-c allows one connection per process, so the
   second screen uses the Mac relay) and **sound** (the headset side is done; the Macs need a virtual
   audio device and `stream_audio = true` in Sunshine).
