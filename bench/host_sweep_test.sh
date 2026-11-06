#!/bin/bash
# PinPin Host's stale-session sweep (K31b), on the Air, against a scratch build (never installed, never signed with the
# Host identity) and a scratch support dir (PINPIN_HOST_SUPPORT): the real Host, its plugins and the user's processes
# are not touched. Run from the VPS:  bash bench/host_sweep_test.sh   (copies mac/host/PinPinHost.swift to the Air)
#
# Fake plugin processes (a compiled sleep(600)), orphaned to launchd (ppid 1) by a double fork:
#   stale   exact plugin path, ppid 1, started before "WindowServer"  -> swept, logged
#   decoy   same binary NAME at another path, ppid 1, started before   -> kept (never a name match)
#   fresh   exact plugin path, ppid 1, started after WindowServer      -> kept (a host restart's copy: adopted)
#   child   exact plugin path, parent alive (not ppid 1), started before -> kept
# PINPIN_HOST_WS_START stands in for the WindowServer start time; the flag file decides whether anything happens.
set -e
cd "$(dirname "$0")/.."
ssh -o BatchMode=yes pinpin-air 'mkdir -p /tmp/k31-build'
scp -q mac/host/PinPinHost.swift pinpin-air:/tmp/k31-build/
ssh -o BatchMode=yes pinpin-air 'bash -s' <<'AIR'
set -e
cd /tmp/k31-build
swiftc -O -swift-version 5 PinPinHost.swift -o host-k31 2>&1 | grep -v warning | grep . && exit 1 || true
T=$(mktemp -d /tmp/k31-sweep.XXXX); S=$T/support; mkdir -p $S/plugins $T/bin $T/elsewhere
# a real binary of our own (copies of /bin/sleep are SIGKILLed by code signing); linker ad-hoc signing survives a cp
printf '#include <unistd.h>\nint main(void) { sleep(600); return 0; }\n' > $T/fake.c
cc $T/fake.c -o $T/bin/fakeplugin; cp $T/bin/fakeplugin $T/elsewhere/fakeplugin
printf '{"exec": "%s"}\n' $T/bin/fakeplugin > $S/plugins/fake.json
orphan() { ( "$@" >/dev/null 2>&1 & echo $! > $T/last ) ; cat $T/last; }   # the subshell exits: ppid becomes 1
STALE=$(orphan $T/bin/fakeplugin); DECOY=$(orphan $T/elsewhere/fakeplugin)
$T/bin/fakeplugin & CHILD=$!          # its parent (this shell) stays alive
sleep 4; WS=$(( $(date +%s) - 1 )); sleep 3
FRESH=$(orphan $T/bin/fakeplugin)
fails=0
alive() { kill -0 $1 2>/dev/null; }
check() { if eval "$1"; then echo "ok    $2"; else echo "FAIL  $2"; fails=$((fails+1)); fi; }
check "[ \$(ps -o ppid= -p $STALE | tr -d ' ') = 1 ] && [ \$(ps -o ppid= -p $FRESH | tr -d ' ') = 1 ]" "fakes are orphans (ppid 1)"
# 1. flag off: nothing
PINPIN_HOST_SUPPORT=$S PINPIN_HOST_WS_START=$WS ./host-k31 --sweep-only > $T/out1 2> $T/log1
check "alive $STALE && alive $DECOY && alive $FRESH && alive $CHILD && [ -z \"\$(cat $T/out1)\" ]" "flag off: nothing killed"
# 2. flag on: only the stale exact-path orphan
touch $S/sweep-stale
PINPIN_HOST_SUPPORT=$S PINPIN_HOST_WS_START=$WS ./host-k31 --sweep-only > $T/out2 2> $T/log2
sleep 1
check "! alive $STALE" "stale orphan at the plugin's exact path: killed"
check "grep -q \"sweep: killed stale fake pid $STALE /private$T/bin/fakeplugin\" $T/log2" "the kill is logged with pid and real path"
check "alive $DECOY" "same name at another path: kept (no name match)"
check "alive $FRESH" "orphan started after WindowServer (a host restart's copy): kept for adoption"
check "alive $CHILD" "parent alive (not ppid 1): kept"
check "[ \"\$(cat $T/out2)\" = $STALE ]" "exactly one pid swept"
# 3. unknown WindowServer start: never stale
PINPIN_HOST_SUPPORT=$S PINPIN_HOST_WS_START=0 ./host-k31 --sweep-only > $T/out3 2> $T/log3 || true
check "[ -z \"\$(cat $T/out3)\" ] && alive $DECOY && alive $FRESH" "unknown WindowServer start: nothing killed"
# 4. the real WindowServer (no override): its start time is read, and nothing started in this session is stale
PINPIN_HOST_SUPPORT=$S ./host-k31 --sweep-only > $T/out4 2> $T/log4
check "grep -Eq 'WindowServer started [1-9][0-9]{9}' $T/log4 && [ -z \"\$(cat $T/out4)\" ] && alive $DECOY && alive $FRESH && alive $CHILD" "real WindowServer start read; this session's processes kept"
sed 's/^/      log: /' $T/log2
sed 's/^/      log: /' $T/log4
kill $DECOY $FRESH $CHILD 2>/dev/null || true
echo "host sweep: $fails failures"
exit $fails
AIR
