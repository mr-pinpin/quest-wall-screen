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

### Stress test (to build)

A test mode that summons N screens (target 10) showing a moving test pattern plus a static desktop, and
records for 2 minutes: total LAN Mbps, headset decode fps and latency p50/p99, ping from a third device to
the router and to the internet. Pass: the third device stays under 50 ms, the headset holds 72 fps, total
LAN traffic stays within the budget whatever N is.
