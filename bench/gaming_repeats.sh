#!/bin/bash
# Gaming flags judged on repeats (work-df, 2026-11-03): single back-to-back pairs could not separate a flag from Wi-Fi
# noise (baselines alone ranged 18-72 stalls > 100 ms). Per flag: baseline/flag alternating, REPS times, under one
# headset hold (the user policy: on the charger, only while runs remain, never worn). Then bench/gaming_summary.py.
#   [APK=...] [REPS=3] bash bench/gaming_repeats.sh "--headset-flag wifill" "--flags vt,gamecap,gameyield --headset-flag vt" ...
cd "$(dirname "$0")/.."
REPS=${REPS:-3}; SECS=${SECS:-120}
[ $# -gt 0 ] || set -- "--headset-flag wifill" "--flags vt,gamecap,gameyield --headset-flag vt" "--sunshine vt_realtime=enabled" "--flags gameyield,gamecap"
THEN=()
for flag in "$@"; do for i in $(seq 1 $REPS); do THEN+=(--then "bash bench/gaming_pairs.sh run"); THEN+=(--then "bash bench/gaming_pairs.sh run $flag"); done; done
python3 bench/headset_hold.py ${APK:+--install "$APK"} --minutes $(( ${#THEN[@]} / 2 * (SECS + 180) / 60 + 5 )) "${THEN[@]}"
python3 bench/gaming_summary.py --since "$(date -u -d "-$(( ${#THEN[@]} / 2 * (SECS + 180) / 60 + 20 )) min" +%Y%m%d-%H%M%S)"
