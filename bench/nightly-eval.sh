#!/bin/bash
# Nightly assistant eval (VPS cron, a quiet-Air window). Runs the eval against the scratch house on display 60,
# appends a summary to bench/results/assistant-eval-nightly.md, and posts a /status line. Self-cleaning: eval_run.sh
# reverts the house + releases the lock; this also kills warm procs after. Skips if the user is using the assistant
# or the Air is busy, so it never disrupts the user. Disable by removing the crontab line.
set -u
REPO=/root/work/quest-wall-screen
NIGHTLY="$REPO/bench/results/assistant-eval-nightly.md"
SSH="ssh -o BatchMode=yes -o ConnectTimeout=10"
log(){ printf '%s work-42 nightly-eval[cron] %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$1" >> "$REPO/bench/activity.log"; }

# don't disrupt the user: skip if the assistant was used in the last 30 min
if [ -n "$($SSH pinpin-air 'find ~/.local/share/pinpin-assistant/screens -name session.json -mmin -30 2>/dev/null | head -1' 2>/dev/null)" ]; then
  log "SKIP: recent assistant activity"; exit 0
fi
# skip if the Air is busy (benchmarks or load)
L1=$($SSH pinpin-air "uptime | sed 's/.*averages*:* *//' | awk '{print \$1}'" 2>/dev/null)
if [ -z "$L1" ] || awk "BEGIN{exit !($L1 > 4)}"; then log "SKIP: Air load=${L1:-?}"; exit 0; fi

log "START (Air load $L1)"
# fresh scratch house
# scratch house is a Mini LaunchAgent (io.mr-pinpin.house-scratch, one owner): bootout, refresh data, bootstrap (work-ad)
$SSH pinpin-mini 'U=gui/$(id -u); launchctl bootout $U/io.mr-pinpin.house-scratch 2>/dev/null; pkill -f "server.py --data.*pinpin-house-scratch" 2>/dev/null; sleep 1; rm -rf ~/pinpin-house-scratch; cp -R ~/pinpin-house ~/pinpin-house-scratch; launchctl bootstrap $U ~/Library/LaunchAgents/io.mr-pinpin.house-scratch.plist; for i in 1 2 3 4 5 6 7 8; do curl -s -m2 127.0.0.1:57801/health >/dev/null && break; sleep 1; done' 2>/dev/null
scp -q -o BatchMode=yes "$REPO/bench/assistant_eval.py" pinpin-air:/tmp/assistant_eval.py 2>/dev/null
OUT=$($SSH pinpin-air 'bash -s' < "$REPO/bench/eval_run.sh" 2>&1)
SUMMARY=$(echo "$OUT" | grep -m1 '^PASS [0-9]')
{ echo "## $(date -u +%Y-%m-%dT%H:%M:%SZ)"; echo "${SUMMARY:-(no summary — run may have failed)}"; echo '```';
  echo "$OUT" | grep -E '^(PASS|FAIL) '; echo '```'; echo; } >> "$NIGHTLY"
$SSH pinpin-air 'pkill -f "input-format stream-json" 2>/dev/null; true' 2>/dev/null   # leave the Air clean

# /status line (work-ad): POST /api/status/component; latest per component replaces the previous.
FAILS=$(echo "$OUT" | grep -c '^FAIL ' || true)
OKVAL=$([ "${FAILS:-1}" = "0" ] && echo true || echo false)
TEXT=$(echo "${SUMMARY:-no result}" | sed 's/^PASS //')   # e.g. "17/18 (94%) first-word p50=10.7s ..."
TOKEN=$(cat /root/.config/pinpin-apps/token 2>/dev/null)
curl -s -m8 -X POST "http://192.168.0.50:57830/api/status/component" -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d "{\"component\":\"assistant-eval\",\"text\":\"${TEXT}\",\"ok\":${OKVAL},\"link\":\"bench/results/assistant-eval-nightly.md\"}" >/dev/null 2>&1 || true
log "END ${SUMMARY:-no-result}"
