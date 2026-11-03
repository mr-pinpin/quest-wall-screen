#!/bin/bash
# The benchmark suite (bench/suite.sh): cinema, VR, wall and moving-gaze scenarios at 10 and 20 screens (30 wedged macOS screen capture on 2026-11-03; re-add only after visibility-only capture is proven),
# plus one with heavy internet load. Every run is guarded by stress.py itself: it starts only while the laptop
# has been idle 10 min and nobody wears the headset, and aborts (cleaning up) when either is picked up.
# Waits for an idle window (checks every 10 min, up to 10 h), then runs what is left; an aborted run is retried.
cd "$(dirname "$0")/.."
RUNS=(
  "--scenario vr --n 10"
  "--scenario cinema --n 10"
  "--scenario wall --n 10"
  "--scenario ring --n 10"
  "--scenario cinema --n 20"
  "--scenario wall --n 20"
  "--scenario cinema --n 10 --wan 3"
  "--scenario cinema --n 10 --wan 3 --flags bpcut"
)
SECS=${SECS:-60}
EXTRA=${EXTRA:-}   # e.g. EXTRA=--force to run while the laptop is in use
i=0
for attempt in $(seq 1 60); do
  while [ $i -lt ${#RUNS[@]} ]; do
    out=$(timeout 600 python3 bench/stress.py ${RUNS[$i]} --secs $SECS $EXTRA 2>&1)
    case "$out" in
      *"not running"*|*"aborted"*) echo "$(date +%T) waiting: $(echo "$out" | tail -1)"; break;;
      *) echo "=== ${RUNS[$i]}"; echo "$out" | grep -E '"(pass|mbps|p95|p99|longestSilenceS)"|warning' | head -12; i=$((i+1));;
    esac
  done
  [ $i -ge ${#RUNS[@]} ] && { echo "suite done"; tail -12 bench/results/stress.md; exit 0; }
  sleep 600
done
echo "suite incomplete: $i of ${#RUNS[@]} runs"; exit 1
