#!/bin/bash
# Gaming-latency A/B pairs on the real headset (docs/gaming-latency-laptop.md, verdict rules in docs/bench-scoreboard.md).
# Each pair runs back to back so both halves see the same house network; each run goes through bench/gate.py.
#   bash bench/gaming_pairs.sh            # wait for the headset's wake window (bench/headset_hold.py), run all pairs
#   bash bench/gaming_pairs.sh run ARGS   # one stress.py --headset --game LAPTOP run with ARGS, then its gate
# Nobody wears anything: headset_hold.py simulates wear only while the real sensor says unworn and lets go the moment
# someone puts it on; stress.py takes the shared bench lock and checks the laptop is idle.
cd "$(dirname "$0")/.."
SECS=${SECS:-120}
if [ "$1" = run ]; then
  shift
  out=$(python3 bench/stress.py --headset --game LAPTOP --n 10 --secs $SECS "$@" 2>&1)
  echo "=== $*"; echo "$out" | grep -E '"pass"|latency|warning|not running|aborted' | head -8
  res=$(echo "$out" | sed -n 's/^stress: result //p' | tail -1)
  [ -n "$res" ] && { python3 bench/gate.py "$res" || echo "gate: FAIL ($res)"; }
  exit 0
fi
PAIRS=(
  ""                                        # 1. baseline: Sunshine as configured (vt_realtime disabled, HEVC)
  "--sunshine vt_realtime=enabled"          #    Sunshine real-time encoding
  ""                                        # 2. baseline again (pairs stay back to back)
  "--flags gameyield"                       #    atlas yields encoder slots while "game":1 (needs room_lab to send it)
)
# 3. HEVC vs H264 for the LAPTOP host: add once the headset has a codec switch (work-c4), e.g. "--headset-codec H264".
THEN=()
for p in "${PAIRS[@]}"; do THEN+=(--then "bash bench/gaming_pairs.sh run $p"); done
exec python3 bench/headset_hold.py --minutes $(( ${#PAIRS[@]} * (SECS + 90) / 60 + 5 )) "${THEN[@]}"
