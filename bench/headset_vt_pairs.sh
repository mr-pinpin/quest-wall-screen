#!/bin/bash
# Real-headset VT pair (docs/virtual-texturing.md): cinema with 10 screens, VT off vs on (sender --flags vt + headset
# files/vt-on), back to back under one headset hold. Installs APK first (v0.41+: cinema demand in the app).
#   APK=/root/.cache/pinpin-wall-screen/room-lab-041/room-lab.apk bash bench/headset_vt_pairs.sh
# Nobody wears anything: headset_hold.py simulates wear only while the real sensor says unworn.
cd "$(dirname "$0")/.."
SECS=${SECS:-60}
RUNS=("--scenario cinema --n 10" "--scenario cinema --n 10 --flags vt --headset-flag vt")
THEN=()
for r in "${RUNS[@]}"; do THEN+=(--then "out=\$(python3 bench/stress.py --headset --secs $SECS $r 2>&1); echo \"=== $r\"; echo \"\$out\" | tail -3; res=\$(echo \"\$out\" | sed -n \"s/^stress: result //p\" | tail -1); [ -n \"\$res\" ] && python3 bench/gate.py \"\$res\"; true"); done
exec python3 bench/headset_hold.py ${APK:+--install "$APK"} --minutes $(( ${#RUNS[@]} * (SECS + 120) / 60 + 3 )) "${THEN[@]}"
