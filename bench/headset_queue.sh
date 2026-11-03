#!/bin/bash
# The headset run queue, never left stopped (user rule 2026-11-03: "Nobody is giving permission for the game to become
# stopped or paused"). bench/headset-queue.txt holds one stress.py --headset argument line per run, in order. A run
# leaves the queue only when it produced a result. The main loop holds the headset (bench/headset_hold.py: charger
# only, never a confirmed wearer) for everything that remains, and whenever a hold ends for any reason (worn,
# unplugged, unreachable, done) it waits for the next wake window and goes on until the queue is empty.
#   [APK=...] bash bench/headset_queue.sh          # run the queue
#   bash bench/headset_queue.sh run "ARGS"         # one run (used by the hold)
cd "$(dirname "$0")/.."
Q=bench/headset-queue.txt; SECS=${SECS:-120}
if [ "$1" = run ]; then
  args="$2"
  while [ -f /tmp/pinpin-yield-until ] && [ "$(date +%s)" -lt "$(cat /tmp/pinpin-yield-until)" ]; do sleep 10; done
  for attempt in $(seq 1 80); do
    out=$(python3 bench/stress.py --headset --secs $SECS $args 2>&1)
    case "$out" in *"lock is held"*) sleep 15;; *) break;; esac
  done
  f=/tmp/headset-run-$(date -u +%H%M%S).log; echo "$out" > $f
  res=$(echo "$out" | sed -n "s/^stress: result //p" | tail -1)
  echo "=== $args -> ${res:-no result} (log $f)"
  if [ -n "$res" ]; then
    python3 bench/gate.py "$res" | grep -E "^(PASS|FAIL)|gaming" | head -9
    python3 - "$Q" "$args" <<"PY"
import fcntl, sys
q, line = sys.argv[1], sys.argv[2]
with open(q, "r+") as f:
    fcntl.flock(f, fcntl.LOCK_EX); lines = f.read().splitlines()
    if line in lines: lines.remove(line)
    f.seek(0); f.truncate(); f.write("".join(l + "\n" for l in lines))
PY
  fi
  exit 0
fi
while [ -s $Q ]; do
  THEN=(); while IFS= read -r l; do [ -n "$l" ] && THEN+=(--then "bash bench/headset_queue.sh run \"$l\""); done < $Q
  echo "$(date -u +%H:%M:%SZ) hold for ${#THEN[@]} queued runs"
  python3 bench/headset_hold.py ${APK:+--install "$APK"} --minutes 600 "${THEN[@]}"; rc=$?
  echo "$(date -u +%H:%M:%SZ) hold ended (rc $rc); $(wc -l < $Q) runs left; re-arming"
  sleep 60
done
echo "headset queue empty"
