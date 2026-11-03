#!/usr/bin/env python3
"""Offline tests for bench/watchdog.py: fake activity logs, locks, process lists and prox logs."""
import pathlib, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import watchdog as w

fails = 0
def check(cond, msg):
    global fails
    if not cond: fails += 1; print('FAIL', msg)

T = w.ts('2026-11-03T13:00:00Z')
def at(m): return f"2026-11-03T13:{m:02d}:00Z"
def kinds(f): return sorted(k for k, _, _ in f)

# Today's hung run: a --headset run started 12:57 with --secs 120 holding the lock 23 min later.
ev = w.parse([f"{at(0)} work-c4 stress.py[100] START headset run: --headset --game LAPTOP --n 10 --secs 120 (bench lock taken)"])
lock = {'owner': 'stress.py --headset ... since 13:00:00', 'age_s': 23 * 60}
check(kinds(w.check(T + 23 * 60, ev, lock, {'100': 'python3 bench/stress.py --headset'}, [])) == ['hung run'], 'hung run after 23 min')
check(w.check(T + 8 * 60, ev, lock, {'100': 'python3 bench/stress.py'}, []) == [], 'no alarm within expected end + 10 min')
# Finished runs are not open.
ev2 = ev + w.parse([f"{at(5)} work-c4 stress.py[100] END headset run (bench lock released)"])
check(w.check(T + 60 * 60, ev2, None, {}, []) == [], 'ended run: nothing')
# Lock held with no explanation for 31 min (a manual test that forgot END).
check(kinds(w.check(T + 31 * 60, [], {'owner': 'someone', 'age_s': 31 * 60}, {}, [])) == ['hung run'], 'unexplained lock > 30 min')
# Stalled queue: suite running, lock free, last event 6 min ago.
ev3 = w.parse([f"{at(0)} work-c4 stress.py[200] START sim run: --scenario vr --n 10 --secs 60", f"{at(4)} work-c4 stress.py[200] END sim run"])
procs = {'300': 'bash bench/suite.sh'}
check(kinds(w.check(T + 10 * 60, ev3, None, procs, [])) == ['stalled queue'], 'stalled queue')
check(w.check(T + 8 * 60, ev3, None, procs, []) == [], 'queue waiting under 5 min: fine')
check(w.check(T + 10 * 60, ev3, {'owner': 'work-09 test', 'age_s': 60}, procs, []) == [], 'queue waiting on another holder: fine')
# Ghost job: our START, pid gone, no lock.
ev4 = w.parse([f"{at(0)} work-c4 stress.py[400] START sim run: --secs 60"])
check('ghost job' in kinds(w.check(T + 3 * 60, ev4, None, {}, [])), 'ghost job')
check('ghost job' not in kinds(w.check(T + 60, ev4, None, {}, [])), 'a run ending right now is not a ghost (exit window)')
check('ghost job' not in kinds(w.check(T + 2 * 60, ev4, None, {'400': 'python3 bench/stress.py'}, [])), 'live job is not a ghost')
check(w.check(T + 2 * 60, w.parse([f"{at(0)} work-09 ectest.sh[manual] START Air: test"]), None, {}, []) == [], 'manual entries are not checked for pids')
# Long hold: prox_close without automation_disable for 46 min; a later disable clears it.
prox = [f"{at(0)} prox_close headset_hold pid 9"]
check(kinds(w.check(T + 46 * 60, [], None, {}, prox)) == ['long hold'], 'prox override 46 min')
check(w.check(T + 46 * 60, [], None, {}, prox + [f"{at(30)} automation_disable headset_worn pid 9"]) == [], 'released override')
ev5 = w.parse([f"{at(0)} work-c4 headset_hold.py[500] START headset hold (simulated wear, real-sensor probe each minute) for up to 40 min"])
check('long hold' in kinds(w.check(T + 50 * 60, ev5, None, {'500': 'python3 bench/headset_hold.py'}, [])), 'hold START open 50 min')
# Report dedupes per key for an hour.
import tempfile, os
d = pathlib.Path(tempfile.mkdtemp()); w.ACTIVITY, w.STATE = d / 'a.log', d / 's.json'
f = [('hung run', 'k1', 'detail')]
check(len(w.report(f, T)) == 1 and len(w.report(f, T + 600)) == 0 and len(w.report(f, T + 3700)) == 1, 'hourly dedupe')
g = [('ghost job', 'g1', 'gone')]
check(len(w.report(g, T)) == 0 and len(w.report(g, T + 60)) == 1, 'ghost needs two consecutive passes')
check(w.ACTIVITY.read_text().count('WATCHDOG hung run: detail') == 2 and w.parse(w.ACTIVITY.read_text().splitlines())[0]['kind'] == 'WATCHDOG', 'WATCHDOG lines parse')
print(f'watchdog self-test: {fails} failures'); sys.exit(1 if fails else 0)
