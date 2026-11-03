# Performance and the house network

## What happened on 2026-11-03

With three screens up, the whole house lost usable internet for roughly half an hour; the user had to move
their laptop to the ZyXEL to get online. Measured afterwards:

| Link | Capacity | Notes |
|---|---|---|
| House internet (Mini en1 → ZyXEL, 2.4 GHz ch 13, 20 MHz, MCS 7) | ~30 Mbps (3.8 MB/s) | the only way out for every device |
| Mini's own traffic (default route via Tailscale exit node) | ~3 Mbps, 218 ms | house NAT bypasses it (`route-to en1`), so it is not the house path |
| Laptop (Air) → strong router, 5 GHz | 1200 Mbps PHY | also carries the user's own work |
| Headset → strong router, 5 GHz | 2401 Mbps PHY | |

Load at the time:

- Main LAPTOP stream: 80 Mbps HEVC (+~20 % FEC), Air → Wi-Fi → Mini Python relay → Wi-Fi → headset.
- Extra-screen atlas: up to 40 Mbps H.264, 3840×1200, Air → headset.
- Two YouTube players on the extra screens. Those displays were HiDPI (3840×2400 backing), so YouTube
  picked 1440p/4K streams, each on its own able to fill the ~30 Mbps line. Once the uplink queue fills,
  every device's latency climbs into seconds (bufferbloat): the "network collapsed" symptom.
- The laptop's traffic also runs through its own Tailscale exit node (the VPS), adding latency and a
  ~3 Mbps-per-flow ceiling to everything it fetches.

So the trigger was internet video sized for screens far larger than the headset can show, on a thin line
with no fairness; the LAN streams added Wi-Fi airtime pressure on top.

## Fixed right away

- Extra displays are created at 1x (1920×1200), which is exactly the atlas cell size: no visible loss,
  and YouTube now picks ≤1080p. Existing displays are switched in place.
- The LAPTOP stream goes straight to the laptop (192.168.0.102); the Mini relay, a leftover from the old
  hotspot network, is out of the media path and off the house gateway.
- Proposed for the gateway (network owner): per-device fair queuing with a cap just under line rate on
  the Mini (dummynet pipe with per-source queues), so no single device can starve the house again.

## Design: one application, demand-driven pixels

The goal is video-game behaviour: ten screens should cost about what the user can actually see.

1. **One sender, one stream.** All screens, including the main one, come from a single capture-and-encode
   service on the Mac, multiplexed into one connection. One congestion controller, one bitrate.
2. **Virtual-texture style demand.** Screens are split into tiles. The headset says which screens are
   visible and how many pixels each covers in the user's view; the sender only encodes visible screens,
   only at that resolution (a screen 4 m away across the room needs a fraction of its pixels), and only
   the tiles that changed. Off-view or behind-a-wall screens cost nothing; a static desktop costs nothing.
3. **Gaze priority.** The screen the user looks at gets full frame rate and most of the bits; peripheral
   screens drop to a few fps and coarser quantisation; their last frame stays sharp on the wall.
4. **A global budget with congestion control.** One number (start: 60 Mbps on the LAN) shared by all
   screens, adapted down when delay or loss rises (GCC/BBR-like), never up into the Wi-Fi cliff.
5. **Internet stays fair.** The gateway shapes WAN per device; content on extra screens is rendered at
   the size it is seen, so internet video quality follows the headset, not a phantom 4K display.

### Interface: headset → sender (live now)

Five times a second the headset sends one UDP datagram to the laptop, port 57721:

```json
{"t": 123456, "budgetKbps": 60000,
 "screens": [{"id": 1, "vis": 1, "px": 1400, "pri": 1.00},
             {"id": 2, "vis": 0, "px": 610,  "pri": 0.00}]}
```

- `id` 1 is the main panel, extras use their screen ids.
- `vis`: inside the field of view and not behind a scanned wall.
- `px`: horizontal headset pixels the screen spans (≈22 px per degree): the most detail worth sending.
- `pri`: gaze weight, 1 within ~12° of the head direction, falling to 0.1 at the edge of view, 0 if unseen.
- `budgetKbps`: the total the sender may spend across all screens.

Sender side (owner: the video/encoder session): stop or throttle unseen screens, scale each cell to `px`,
split the budget by `pri`, encode changed tiles only.

### Sender side (pinpin-atlas, extra screens)

What `atlas/AtlasSender.swift` does with the demand datagrams (UDP :57721):

- **Unseen screens cost nothing.** A screen with `vis: 0` keeps capturing, but its frames go to a one-frame
  stash instead of the atlas, so its cell stays still and encodes as skipped blocks. When it becomes visible,
  the stashed frame is copied in immediately, so the user never sees an old picture.
- **Bitrate follows gaze.** The atlas encoder gets `budgetKbps × Σpri(visible extras) / Σpri(all visible)`,
  4–40 Mbps. It is retuned live (no keyframe) whenever the share moves by more than 15 %.
- **Resolution follows distance.** Each cell's width is the smallest tier in 480/640/960/1280/1920 covering
  its `px`. A new tier must hold for 2 s (growing) or 5 s (shrinking) before it applies, since each layout
  change costs one keyframe and a capture restart. Unseen screens keep their size. Smaller cells also let
  more screens fit the 3840×2160 atlas: ten screens at 960 px fit where only two at 1920 did.
- **Still screens are nearly free.** The sender encodes only on change, plus three flush repeats (hardware
  decoders release a frame when the next arrives) and a 1/s heartbeat. Repeats carry header bit 1, so the
  headset's latency figure leaves them out. Within a changed frame, H.264 skip-codes unchanged macroblocks.
  That is the "changed tiles only" behaviour, without a separate tile protocol.
- **Fail-safe.** Demand older than 2 s (no headset, lost packets) means everything visible, full size, full
  bitrate: the behaviour before demand existed.

Still open: the main panel (Sunshine, id 1) ignores the budget; per-screen frame-rate caps for low-`pri`
screens; congestion control from measured delay rather than a fixed budget.

### Stress test (bench/stress.py, built 2026-11-03)

`sudo -n python3 bench/stress.py --n 10 --secs 120 [--wan K] [--no-headset] [--keep]` from the VPS:
- **Screens.** Creates the missing laptop displays 2..N+1 at 1x. Fills the ones it created with a moving pattern
  (bench/stress/pattern.swift, built as ~/Applications/PinPin Stress Pattern.app on the Air): sliding bars, scrolling text,
  a noise block.
- **Headset.** Puts the headset in stress mode (files/stress.json): synthetic demand for N screens on a ring, the
  virtual gaze moving one screen every `--sweep` s.
- **Measures.** Gateway ping to the internet over en1 and to the headset, the headset status every 2 s, the laptop's
  Wi-Fi bytes and the sender log.
- **Teardown and output.** Removes everything it created and writes bench/results/stress-*.json (with the time series)
  plus a row in bench/results/stress.md.

Pass when all of these hold:
- internet ping p95 < 50 ms
- headset >= 70 fps
- atlas <= 40 Mbit/s
- no screen skipped
- stress mode engaged
- the atlas kept flowing

First 10-screen run (shaping on):
- **Held:** internet p95 44 ms, headset 69-73 fps, atlas 9.7 Mbit/s average, never over its cap.
- **Failed:** the extra screens froze in bursts. There were 10 s windows at 0 Mbit/s and atlas latency p99 was 2.2 s.
- **Cause:** every demand-driven cell resize is a new layout, which restarts every capture and sends a keyframe. A
  moving gaze turns that into a storm.
- **Next on the sender:** resize cells without restarting captures, and refit the layout rarely, with hysteresis on
  the whole layout and not just per cell.
- **Caveat:** eight animated patterns plus ten captures load the Air to ~13, so the pattern itself costs CPU. A
  GPU-drawn pattern would isolate the sender.
