# Stress runs

| when | N | wan | internet ping p50/p95 ms | headset ping p50/p95 | fps min | atlas Mbit/s | atlas latency p50/p99 | laptop tx Mbit/s | skipped | pass |
|---|---|---|---|---|---|---|---|---|---|---|
| 20261103-082043 | 4 | 0 | 18.0/82.8 | 47.3/138.5 | 72 | 0.0 | 25.3/25.3 | 0.1 | - | NO |
| 20261103-082418 | 4 | 0 | 17.6/25.2 | 8.7/83.6 | 72 | 0.6 | 36.8/40.5 | 0.6 | - | yes |
| 20261103-082824 | 4 | 0 | 17.5/21.4 | 22.0/91.8 | 72 | 0.1 | 29.9/31.0 | 0.2 | - | yes |
| 20261103-083301 | 10 | 0 | 17.6/44.4 | 5.2/68.3 | 69 | 9.7 | 35.1/2226.8 | 10.4 | - | NO |
| 20261103-083851 | 10 | 0 | 17.4/32.0 | 4.5/22.5 | 70 | 13.3 | 44.3/662.0 | 16.3 | - | yes |
| 20261103-084304 | 10 | 0 | 17.6/22.6 | 5.1/67.6 | 72 | 8.5 | 44.7/31764.9 | 12.1 | - | yes |

Notes: the 4-screen runs had no visible pattern (launch bug, fixed), so their atlas numbers mean nothing. Runs before 1bbcb91 lacked the freeze and crash checks: 083851 and 084304 "passed" with 22 s freezes, and 084304 had 4 sender segfaults.
