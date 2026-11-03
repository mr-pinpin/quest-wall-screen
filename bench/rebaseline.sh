#!/bin/bash
# Rebaseline after the still-wallpaper fix (2026-11-03 14:22 UTC): the four 10-screen baselines, then the flag
# pairs, ordered by the encoder-vs-network control (work-09, cinema-10 on localhost vs Wi-Fi).
#   bash bench/rebaseline.sh network   # Wi-Fi-bound: flags that cut airtime first (vt, longgop, bpcut)
#   bash bench/rebaseline.sh encoder   # encoder-bound: flags that cut capture/encode work first (vt, damage, prifps)
#   bash bench/rebaseline.sh synth     # the pipeline scoreboard on the synthetic source, over Wi-Fi and on localhost
# Every run takes the bench lock (waiting while another agent holds it), is logged to bench/activity.log by
# stress.py and judged by bench/gate.py; each flag run pairs with the baseline of its scenario.
cd "$(dirname "$0")/.."
SECS=${SECS:-60}
BASE=("--scenario vr --n 10" "--scenario cinema --n 10" "--scenario wall --n 10" "--scenario ring --n 10")
case "$1" in
  network) PAIRS=("--scenario cinema --n 10 --flags vt,longgop" "--scenario ring --n 10 --flags vt,longgop" "--scenario wall --n 10 --flags vt,longgop"
                  "--scenario cinema --n 10 --wan 3" "--scenario cinema --n 10 --wan 3 --flags bpcut,vt,longgop");;
  encoder) PAIRS=("--scenario cinema --n 10 --flags vt" "--scenario wall --n 10 --flags vt" "--scenario wall --n 10 --static 7"
                  "--scenario wall --n 10 --static 7 --flags damage" "--scenario ring --n 10 --flags prifps" "--scenario ring --n 10 --flags vt,longgop");;
  synth)  # the pipeline scoreboard on the synthetic source (default since 2026-11-03), each over Wi-Fi and on localhost
    BASE=(); PAIRS=()
    for on in mini air; do
      BASE+=("--scenario vr --n 10 --sim-on $on" "--scenario cinema --n 10 --sim-on $on" "--scenario wall --n 10 --sim-on $on" "--scenario ring --n 10 --sim-on $on")
      PAIRS+=("--scenario cinema --n 10 --sim-on $on --flags vt" "--scenario wall --n 10 --sim-on $on --flags vt" "--scenario ring --n 10 --sim-on $on --flags vt"
              "--scenario cinema --n 10 --sim-on $on --flags longgop" "--scenario cinema --n 10 --sim-on $on --flags vt,longgop"
              "--scenario wall --n 10 --sim-on $on --static 7" "--scenario wall --n 10 --sim-on $on --static 7 --flags damage"
              "--scenario ring --n 10 --sim-on $on --flags prifps")
    done
    PAIRS+=("--scenario cinema --n 10 --sim-on mini --wan 3" "--scenario cinema --n 10 --sim-on mini --wan 3 --flags bpcut");;
  *) echo "usage: $0 network|encoder|synth"; exit 2;;
esac
for r in "${BASE[@]}" "${PAIRS[@]}"; do
  for attempt in $(seq 1 120); do
    out=$(timeout 600 python3 bench/stress.py $r --secs $SECS 2>&1)
    case "$out" in *"lock is held"*) sleep 30; continue;; esac
    break
  done
  res=$(echo "$out" | sed -n "s/^stress: result //p" | tail -1)
  echo "=== $r -> ${res:-no result}"; echo "$out" | grep -E "not running|aborted" | tail -1
  [ -n "$res" ] && python3 bench/gate.py "$res" | head -1
  sleep ${YIELD:-60}   # leave the lock free a moment so another agent's queued slot (profiling) can take it
done
echo "rebaseline ($1) done"
