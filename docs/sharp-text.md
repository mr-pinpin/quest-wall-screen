# Sharp text: build-to-lossless refinement for the wall panel

Goal: streamed desktop text in the headset is as sharp as the source pixels,
for any content, with no change to how motion, video or games look.

## Why the video alone stays blurry
Low-latency H.264/HEVC spends a fixed budget per frame. When text changes, it
gets one frame of bits; afterwards the region is coded as "unchanged", so the
first, soft rendition stays on screen. Real-time encoders never spend idle
bandwidth refining static regions.

## Design: two layers
1. **Video layer** (unchanged): Sunshine -> Moonlight -> MediaCodec. Handles
   motion with low latency.
2. **Refinement layer** (new): a sender on the source Mac captures the same
   screen at the stream resolution, splits it into 64x64 tiles and tracks
   change per tile.
   - When a tile changes, it sends `INVALIDATE` immediately (a few bytes), and
     the headset shows the video for that tile.
   - When a tile has been unchanged for `settle` ms (200), it sends the tile
     **losslessly** (RGB, deflate) and the headset draws it over the video.
   The headset composites the tiles into the panel texture before mipmaps
   are built, so the panel shader and mip filtering are unchanged.

Nothing about the refinement can stop or delay the video: if the sender is
missing, slow or mismatched, the panel simply shows the video as today.

## Protocol (TCP, little endian)
Hello (sender -> headset, once): `"PPSH"`, u8 version=1, u16 width,
u16 height, u16 tile.
Then frames: u8 type, u32 payload length, payload.
- `1 INVALIDATE`: u16 count, u16 tile index[count]
- `2 TILE`: u16 index, u32 rawLength, deflate(zlib) RGB24 rows of the tile
  (edge tiles are clipped to the frame)
- `3 PING`: empty, every 2 s
Tile index = row * tilesX + column, tilesX = ceil(width / tile).

## Components
- `sharp/SharpSender.swift`: ScreenCaptureKit capture (stream size, cursor on),
  uses SCK dirty rects to find candidate tiles, compares against the last
  frame, owns the tile state machine, TCP server on port 57700. Packaged as
  `PinPin Sharp.app` signed with a stable local identity, so Screen Recording
  is granted once and survives rebuilds. Runs as a LaunchAgent.
- `sharp/sharp_probe.py`: test client; reconstructs the refined image and
  reports coverage, bytes and invalidations.
- `room_lab/sharp.h`: headset client (network thread + inflate), tile texture
  and per-tile mask; applied in `VideoStream::resolveMips()`. Active only when
  the hello size equals the decoded video size and the selected source
  matches `files/sharp.json`.

## Phases
0. Quick win: laptop Sunshine `vt_realtime = disabled` (done 2026-11-02);
   LAPTOP bitrate 40 -> 80 Mbps with the first headset build.
1. Sender + probe, verified on the laptop: lossless reconstruction, change
   invalidation, bandwidth.
2. Headset compositing, verified with a debug tint and status counters.
3. Panel as an OpenXR quad composition layer (sampled once by the
   compositor instead of twice).
4. Same refinement for the Mini source (Atlas on the TV/headset).
