# Sender profile: cinema-10 on localhost (2026-11-03, ~15:15 UTC)

Test sender (atlas-vis) with stage counters, 10 helper displays 1920x1200 with the stress pattern on all of them,
the stand-in headset (bench/stress/headset_sim.py --scenario cinema) on the Air receiving from 127.0.0.1, 40 s each.

| Variant | Fresh fps p50/p5 | Gap p50/p99/max ms | Atlas | Capture per visible screen | Encode p50 | Atlas copy | Held | Frame copies/s |
|---|---|---|---|---|---|---|---|---|
| v1 | 20/16 | 38/141/201 | 3680x2128 | 8.6 fps | 36 ms | 3.4 ms | 0 | 25.8 |
| vt | 22/19 | 32/129/256 | 2400x1200 | 8.6 fps | 9.7 ms | 1.3 ms | 0 | 25.7 |
| vt, --vt-max-w 1280 | 21/16 | 33/144/172 | 2000x800 | 8.5 fps | 6.4 ms | 0.7 ms | 0 | 25.7 |
| v1, capfast | 22/18 | 31/136/185 | 3680x2128 | 8.6 fps | 37 ms | 3.5 ms | 0 | 25.8 |
| vt, capfast | 19/16 | 33/131/202 | 2400x1200 | 8.6 fps | 9.8 ms | 1.3 ms | 0 | 25.8 |

Reading:
- The source caps the frame rate: with the pattern on all 10 displays, WindowServer updates each display only
  ~8.6 times a second (one display: ~30, four: ~29). The 3 visible cinema screens give ~26 frame copies/s; the
  sender turns them into 20-22 encodes/s (changes landing in the same 16 ms tick share one encode).
- The sender pipeline has no bottleneck here: no backpressure holds, lock waits ~0, copies 1-3 ms.
- vt cuts encoder time per frame 3.7x (9.7 vs 36 ms), the part that matters next to a game encode.
- capfast (capture interval 1/120 s) changes nothing: capture is limited by redraws, not by the interval.
- The Wi-Fi adds the gap tail: localhost gap p99 80 ms (stress control) vs 140-220 ms over Wi-Fi.
