# Stress runs

| when | N | wan | internet ping p50/p95 ms | headset ping p50/p95 | fps min | atlas Mbit/s | atlas latency p50/p99 | laptop tx Mbit/s | skipped | pass |
|---|---|---|---|---|---|---|---|---|---|---|
| 20261103-082043 | 4 | 0 | 18.0/82.8 | 47.3/138.5 | 72 | 0.0 | 25.3/25.3 | 0.1 | - | NO | aerial wallpaper, superseded (results/aerial) |
| 20261103-082418 | 4 | 0 | 17.6/25.2 | 8.7/83.6 | 72 | 0.6 | 36.8/40.5 | 0.6 | - | yes | aerial wallpaper, superseded (results/aerial) |
| 20261103-082824 | 4 | 0 | 17.5/21.4 | 22.0/91.8 | 72 | 0.1 | 29.9/31.0 | 0.2 | - | yes | aerial wallpaper, superseded (results/aerial) |
| 20261103-083301 | 10 | 0 | 17.6/44.4 | 5.2/68.3 | 69 | 9.7 | 35.1/2226.8 | 10.4 | - | NO | aerial wallpaper, superseded (results/aerial) |
| 20261103-083851 | 10 | 0 | 17.4/32.0 | 4.5/22.5 | 70 | 13.3 | 44.3/662.0 | 16.3 | - | yes | aerial wallpaper, superseded (results/aerial) |
| 20261103-084304 | 10 | 0 | 17.6/22.6 | 5.1/67.6 | 72 | 8.5 | 44.7/31764.9 | 12.1 | - | yes | aerial wallpaper, superseded (results/aerial) |

Notes: the 4-screen runs had no visible pattern (launch bug, fixed), so their atlas numbers mean nothing. Runs before 1bbcb91 lacked the freeze and crash checks: 083851 and 084304 "passed" with 22 s freezes, and 084304 had 4 sender segfaults.
| 20261103-085153 | 10 | 0 | 18.2/76.6 | 4.7/25.5 | 71 | 24.2 | 36.6/47.0 | 27.8 | - | NO | aerial wallpaper, superseded (results/aerial) |
| 20261103-085802 | 10 | 0 | 17.4/20.8 | 54.4/172.0 | 48 | 0.0 | 28.4/28.4 | 0.4 | - | NO | aerial wallpaper, superseded (results/aerial) |

## From 2026-11-03 12:30: stand-in headset (sim) and guarded runs

| when | mode | N | wan | internet ping p50/p95 ms | headset ping p50/p95 | house internet Mbit/s down | fps min | atlas Mbit/s | longest freeze s | atlas latency p50/p99 | laptop tx Mbit/s | sender crashes | pass |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 20261103-093208 | sim/vr | 10 | 0 | 19.4/91.1 | 71.5/191.9 | 2.1 | - | 0.1 | 7.91 | - | 0.1 | 0 | NO | aerial wallpaper, superseded (results/aerial) |
| 20261103-093602 | sim/cinema | 10 | 0 | 18.7/87.8 | 97.8/202.8 | 0.9 | - | 32.1 | 0.42 | - | 27.0 | 0 | NO | aerial wallpaper, superseded (results/aerial) |
| 20261103-094009 | sim/wall | 10 | 0 | 18.1/73.7 | 47.6/167.7 | 0.9 | - | 36.9 | 0.36 | - | 30.7 | 0 | NO | aerial wallpaper, superseded (results/aerial) |
| 20261103-094415 | sim/ring | 10 | 0 | 18.2/91.7 | 57.7/160.4 | 0.8 | - | 31.6 | 0.42 | - | 26.0 | 0 | NO | aerial wallpaper, superseded (results/aerial) |
| 20261103-094805 | sim/cinema | 20 | 0 | 18.2/97.8 | 53.7/113.6 | 0.8 | - | 17.2 | 0.35 | - | 14.6 | 0 | NO | aerial wallpaper, superseded (results/aerial) |
| 20261103-095324 | sim/wall | 20 | 0 | 18.1/87.0 | 44.5/148.5 | 3.4 | - | 37.2 | 0.95 | - | 29.8 | 0 | NO | aerial wallpaper, superseded (results/aerial) |
| 20261103-095853 | sim/cinema | 30 | 0 | 19.1/83.3 | None/None | 0.9 | - | 0.0 | 1.23 | - | 0.1 | 0 | NO | aerial wallpaper, superseded (results/aerial) |
| 20261103-100552 | sim/wall | 30 | 0 | 18.3/75.0 | 42.2/139.8 | 0.9 | - | 0 | 0.0 | - | 0.1 | 0 | NO | aerial wallpaper, superseded (results/aerial) |
| 20261103-101243 | sim/cinema | 10 | 3 | 17.6/21.4 | 76.8/100.5 | 0.8 | - | 0 | 0.0 | - | 0.0 | 0 | NO | aerial wallpaper, superseded (results/aerial) |
| 20261103-101836 | sim/cinema | 10 | 0 | 17.7/57.5 | 28.3/62.8 | 0.8 | - | 0 | 0.0 | - | 0.1 | 0 | NO | aerial wallpaper, superseded (results/aerial) |
| 20261103-123611 | sim/vr | 10 | 0 | 18.1/25.1 | 51.6/78.5 | 0.9 | - | 0 | 0.0 | - | 0.1 | 0 | yes | VOID: 0 connects, laptop IP changed after the reboot (moved to results/void)
| 20261103-133125 | sim/vr | 10 | 0 | 17.7/24.0 | 48.2/160.6 | 3.6 | - | 0.1 | 3.89 | - | 0.1 | 0 | yes | aerial wallpaper, superseded (results/aerial) |
| 20261103-134108 | sim/cinema | 10 | 0 | 17.9/192.0 | 75.5/168.7 | 3.1 | - | 7.8 | 0.42 | - | 6.8 | 0 | NO | aerial wallpaper, superseded (results/aerial) |
| 20261103-134519 | sim/wall | 10 | 0 | 17.8/23.8 | 56.3/334.8 | 1.9 | - | 24.3 | 0.42 | - | 18.7 | 0 | NO | aerial wallpaper, superseded (results/aerial) |
| 20261103-135108 | sim/ring | 10 | 0 | 17.9/29.7 | 5.5/29.5 | 3.6 | - | 20.0 | 0.41 | - | 14.3 | 0 | NO | aerial wallpaper, superseded (results/aerial) |
| 20261103-135536 | sim/cinema | 20 | 0 | 17.9/29.8 | None/None | 3.6 | - | 9.7 | 0.45 | - | 8.3 | 0 | NO | aerial wallpaper, superseded (results/aerial) |
| 20261103-140337 | sim/cinema+vt,longgop | 10 | 0 | 17.9/23.8 | 62.0/67.3 | 3.4 | - | 18.1 | 0.42 | - | 14.0 | 0 | NO | aerial wallpaper, superseded (results/aerial) |
| 20261103-140814 | sim/cinema+longgop | 10 | 0 | 18.3/28.2 | None/None | 3.3 | - | 6.6 | 0.43 | - | 5.9 | 0 | NO | aerial wallpaper, superseded (results/aerial) |
| 20261103-141709 | sim/cinema+vt | 10 | 0 | 18.9/100.4 | 27.5/36.9 | 3.5 | - | 20.5 | 0.43 | - | 15.1 | 0 | NO | aerial wallpaper, superseded (results/aerial) |
| 20261103-144359 | sim/cinema~simair | 10 | 0 | 17.8/30.4 | None/None | 3.4 | - | 8.3 | 0.13 | - | 0.1 | 0 | NO |
| 20261103-144849 | sim/cinema~simair+vt | 10 | 0 | 17.9/32.6 | 30.6/50.7 | 3.4 | - | 19.5 | 0.18 | - | 0.1 | 0 | NO |
| 20261103-145348 | sim/cinema~simair+longgop | 10 | 0 | 17.8/22.9 | None/None | 2.3 | - | 6.7 | 0.17 | - | 0.1 | 0 | NO |
| 20261103-145851 | sim/cinema | 10 | 0 | 17.7/28.8 | None/None | 2.6 | - | 7.3 | 0.43 | - | 5.9 | 0 | NO |
| 20261103-155548 | sim/cinema | 10 | 0 | 16.9/20.6 | 39.6/149.8 | 0.6 | - | 7.8 | 0.13 | - | 5.6 | 0 | NO |
| 20261103-174632 | sim/cinema~synth | 10 | 0 | 17.6/32.2 | 30.9/150.5 | 0.8 | - | 31.7 | 0.08 | - | 27.2 | 0 | yes |
| 20261103-174852 | sim/cinema~synth+nodelay | 10 | 0 | 17.6/24.2 | 45.8/160.7 | 0.6 | - | 31.5 | 0.1 | - | 26.4 | 0 | yes |
| 20261103-202311 | sim/cinema~synth | 10 | 0 | 17.7/27.8 | 56.1/76.5 | 0.8 | - | 26.8 | 0.27 | - | 23.4 | 0 | yes |
| 20261103-204331 | sim/cinema | 10 | 0 | 19.0/192.9 | 47.1/83.7 | 0.8 | - | 27.3 | 0.43 | - | 23.4 | 0 | NO |
| 20261103-210217 | sim/cinema~synth | 10 | 0 | 17.4/24.6 | 24.8/119.8 | 0.8 | - | 26.8 | 0.25 | - | 23.1 | 0 | NO |
