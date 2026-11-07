#!/bin/bash
# Offline test: eval_run.sh takes the bench lock through benchlock.py (EXCLUSIVE) instead of raw `mkdir`, so a lock
# left behind by a CRASHED holder (older than STALE_S) is TAKEN OVER rather than blocking the nightly forever (the
# old `mkdir` loop just LOCK-FAILed on any existing dir). It also must NOT steal a lock from a LIVE holder.
# Hardware-free: benchlock.py runs against a temp dir via BENCHLOCK_LOCAL, using the exact CLI eval_run.sh issues.
set -u
cd "$(dirname "$0")/.."
BL="$PWD/bench/benchlock.py"
T=$(mktemp -d); fail=0
trap 'rm -rf "$T"' EXIT
export BENCHLOCK_LOCAL="$T"
LOCK="$T/pinpin-bench.lock"
check() { if [ "$2" = "$3" ]; then echo "ok    $1"; else echo "FAIL  $1: expected [$3] got [$2]"; fail=1; fi; }

# the command eval_run.sh runs to acquire the lock (keep in sync with eval_run.sh); $1 = --wait seconds
take() { python3 "$BL" take exclusive --who "work-42 eval" --max 1800 --wait "$1"; }

STALE_S=$(python3 -c 'import sys;sys.path.insert(0,"bench");import benchlock;print(benchlock.STALE_S)')

# --- 1) a STALE lock (crashed holder, mtime older than STALE_S) is taken over, NOT LOCK FAIL ---
mkdir -p "$LOCK"; echo "dead stress.py --scenario cinema" > "$LOCK/owner"
old=$(( $(date +%s) - STALE_S - 10 ))
touch -d "@$old" "$LOCK" 2>/dev/null || touch -t "$(date -d @"$old" +%Y%m%d%H%M.%S)" "$LOCK"
tok=$(take 0 2>/dev/null); rc=$?
check "stale lock: take exits 0 (taken over, not LOCK FAIL)" "$rc" "0"
case "$tok" in exclusive:*) got=token;; *) got="[$tok]";; esac
check "stale lock: a release token is returned" "$got" "token"
[ -d "$LOCK" ] && held=yes || held=no
check "stale lock: a fresh lock is now held" "$held" "yes"
ls -d "$T"/pinpin-bench.lock.stale-* >/dev/null 2>&1 && aside=yes || aside=no
check "stale lock: the crashed holder's dir was moved aside" "$aside" "yes"
python3 "$BL" release "$tok"
[ -d "$LOCK" ] && still=yes || still=no
check "release frees our own lock" "$still" "no"

# --- 2) a LIVE lock (fresh mtime) is NOT stolen: the eval waits then fails, leaving the live holder in place ---
rm -rf "$T"/pinpin-bench.lock.* 2>/dev/null
mkdir -p "$LOCK"; echo "work-09 campaign" > "$LOCK/owner"; printf 'e-live\n' > "$LOCK/id"   # fresh mtime = alive
tok2=$(take 0 2>/dev/null); rc2=$?
check "live lock: take fails (does not steal a live hold)" "$([ "$rc2" -ne 0 ] && echo nonzero || echo zero)" "nonzero"
case "$tok2" in exclusive:*) stole=yes;; *) stole=no;; esac
check "live lock: no token was issued" "$stole" "no"
grep -q "work-09 campaign" "$LOCK/owner" 2>/dev/null && kept=yes || kept=no
check "live lock: the live holder still owns the lock" "$kept" "yes"

[ $fail = 0 ] && echo "eval lock test passed" || { echo "OFFLINE TEST FAILED"; exit 1; }
