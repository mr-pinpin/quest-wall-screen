# 5 GHz channel: ch36/80 (auto) vs ch157/80 — verdict 2026-11-03 (work-09)

Host: M1 Air (MacBookAir10,1) on PinPin-Quest-Offline, Mini wired. Router Electra F30AX.
Decision rule (work-df): weight loaded metrics; idle ping tails are dominated by macOS Wi-Fi power save.

| metric (loaded) | ch36 | ch157 |
|---|---|---|
| game latency p50, ms (work-c4 `--headset --game LAPTOP --n 3` cinema, no flags) | 26.9 / 27.3 / 30.4 / 27.1 | 34.6 / 33.4 |
| game fps | 45.8 / 45.7 / 43.1 / 48.9 | 37.9 / 39.6 |
| game rtt p50, ms | 20–22 | 23 |
| synth cinema-10 Wi-Fi fresh fps p50 | 48 / 47 | 39 / 39 |
| synth cinema-10 gap p99, ms | 27.2 / 27.7 | 30.8 / 52.6 |
| synth cinema-10 Mbit/s | 31.7 / 31.5 | 26.8 / 26.8 |
| laptop ping p99 under load, ms | 11.4 / 9.6 | 6.8 / 97.3 |
| Air PHY rate (signal -46/-47 dBm) | 1200 | 864 / 960 |
| noise floor | -87 dBm | -93 dBm |

SCK cinema-10 (BEFORE 20 fps / gap p99 128 ms vs AFTER2 41 fps / 117.5 ms) is not comparable on fps: the sender
build and test pattern changed between the runs.

Idle 10-min ping (for the record): ch36 p95 41 / p99 229 / max 385 ms; ch157 p95 78–80 / p99 243–270 / max 565–1291 ms.

**Verdict: roll back to ch36.** Every repeatable loaded metric is worse on ch157 (~+6 ms game latency, ~-7 game fps,
-20% synth cinema throughput), consistent with the lower PHY rate on UNII-3 at the same signal. The quieter noise floor
does not compensate. Raw files: `wifi-channel-{BEFORE,AFTER,AFTER2}-*.txt` and the `stress-*` JSONs listed in them.
