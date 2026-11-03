#!/bin/bash
# Gaming-latency A/B pairs on the real headset (docs/gaming-latency-laptop.md, verdict rules in docs/bench-scoreboard.md).
# Each pair runs back to back so both halves see the same house network; each run goes through bench/gate.py.
#   [APK=room-lab.apk] bash bench/gaming_pairs.sh   # (install APK if its version differs, then) wait for the headset's wake window (bench/headset_hold.py), run all pairs
#   bash bench/gaming_pairs.sh run ARGS   # one stress.py --headset --game LAPTOP run with ARGS, then its gate
# Nobody wears anything: headset_hold.py simulates wear only while the real sensor says unworn and lets go the moment
# someone puts it on; stress.py takes the shared bench lock and logs to bench/activity.log (no laptop idle gate).
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
  "--flags gameyield"                       #    atlas yields encoder slots while "game":1 (headset v0.39+)
  "--codec HEVC"                            # 3. HEVC (OFF half of codec-h264)
  "--codec H264"                            #    H.264, low-latency rate control (headset v0.39+ CODEC flag)
  ""                                        # 5. baseline again
  "--flags damage,prifps"                   #    atlas share of the media engine: still screens skipped, background at 10-15 fps
  ""                                        # 6. baseline again
  "--flags gameyield,gamecap"               #    while "game":1 the atlas fits 1920x1200 and encodes 1 tick in 2-4
                                            #    (encprobe: game encode 40 fps/277 ms -> 60 fps/13 ms, bench/results/encshare-2026-11-03.md)
  ""                                        # 4. baseline again
  "--headset-flag wifill"                   #    Wi-Fi low-latency lock while streaming (headset v0.40+ WIFILL flag)
)
# Verdicts: python3 bench/verdict.py vt_realtime|gameyield|codec-h264|encshare OFF.json ON.json (docs/bench-scoreboard.md)
THEN=()
for p in "${PAIRS[@]}"; do THEN+=(--then "bash bench/gaming_pairs.sh run $p"); done
exec python3 bench/headset_hold.py ${APK:+--install "$APK"} --minutes $(( ${#PAIRS[@]} * (SECS + 90) / 60 + 5 )) "${THEN[@]}"
