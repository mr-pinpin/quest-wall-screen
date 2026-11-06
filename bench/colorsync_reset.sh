#!/bin/bash
# Reset the Air's colorsync load (K23/K24): restart colorsyncd + colorsync.displayservices (launchd respawns both),
# which drops the polling every display create left behind (2026-11-06: 100% -> ~31% of a core). Run it AFTER every
# virtual-display create and before measuring; runs themselves do not raise it, so between blocks use --if-above 40.
# The caller holds the bench lock (EXCLUSIVE: a campaign's window); this script takes none. Nothing visible changes.
#
#   bash bench/colorsync_reset.sh [--if-above N] [--until-below M]
#     --if-above N     measure first; restart only when above N% (default: always restart)
#     --until-below M  after the restart, re-check (4 x 30 s) until below M% (default 50)
# Exit: 0 below M (or no restart needed), 1 still at/above M, 2 Air unreachable/unreadable,
#       3 the restart needs root: prints the one command for work-df's root path (agents never type a password).
# Logs START/END to the shared bench/activity.log (as stress.py) as $PINPIN_WHO (default: apps).
cd "$(dirname "$0")/.."
ABOVE=; BELOW=50
while [ $# -gt 0 ]; do case $1 in --if-above) ABOVE=$2; shift 2;; --until-below) BELOW=$2; shift 2;; *) sed -n 2,12p "$0"; exit 2;; esac; done
WHO=${PINPIN_WHO:-apps}
# the shared log, as stress.py (_SHARED_LOG): the watchdog and /status read that one, not a worktree's copy
SHARED=/root/work/quest-wall-screen/bench/activity.log
[ -d "${SHARED%/*}" ] || SHARED=bench/activity.log
LOG=${COLORSYNC_LOG:-$SHARED}  # the tests point it elsewhere
note() { echo "$(date -u +%FT%TZ) $WHO colorsync_reset[$$] $*" >> "$LOG"; echo "$*"; }
SAMPLER=$(python3 bench/colorsync.py remote 10)
measure() { ssh -o BatchMode=yes -o ConnectTimeout=10 pinpin-air "$SAMPLER" 2>/dev/null | python3 bench/colorsync.py pct; }

p0=$(measure) || { note "NOTE Air unreachable or colorsync unreadable: nothing done"; exit 2; }
if [ -n "$ABOVE" ] && python3 -c "import sys; sys.exit(0 if $p0 <= $ABOVE else 1)"; then
  note "NOTE colorsync ${p0}% <= ${ABOVE}%: no restart needed"; exit 0
fi
note "START restart colorsync (was ${p0}% of a core)"
err=$(ssh -o BatchMode=yes -o ConnectTimeout=10 pinpin-air 'killall colorsyncd colorsync.displayservices' 2>&1)
if [ $? -ne 0 ]; then
  if echo "$err" | grep -qiE 'not permitted|permission'; then
    note "END needs root on the Air: ask work-df to run: sudo killall colorsyncd colorsync.displayservices"; exit 3
  fi
  note "END restart failed: ${err:0:160}"; exit 2
fi
p=?
for i in 1 2 3 4; do  # COLORSYNC_WAIT: the tests shorten the waits
  sleep ${COLORSYNC_WAIT:-$([ $i = 1 ] && echo 15 || echo 30)}
  p=$(measure) || p=?
  if [ "$p" != "?" ] && python3 -c "import sys; sys.exit(0 if $p < $BELOW else 1)"; then
    note "END colorsync ${p0}% -> ${p}% (< ${BELOW}%, check $i)"; exit 0
  fi
done
note "END colorsync ${p0}% -> ${p}%: still >= ${BELOW}% after 4 checks"; exit 1
