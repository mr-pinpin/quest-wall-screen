"""bench/benchlock.py against a local directory: shared holders run together, exclusive excludes everyone, writer
priority, expiry of crashed holders, stale exclusive locks, the CLI, and a randomised overlap check.
    python3 bench/benchlock_test.py"""
import os, random, subprocess, sys, tempfile, threading, time

D = tempfile.mkdtemp(prefix='benchlock-test-')
os.environ['BENCHLOCK_LOCAL'] = D
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import benchlock as bl  # noqa: E402

bl.POLL = 0.05
fails = 0


def check(c, msg):
    global fails
    if not c: fails += 1; print('FAIL', msg)


# two shared holders at once
a = bl.take('shared', 'apps', 60, 5); b = bl.take('shared', 'assist', 60, 5)
check(a and b, 'two shared holders')
# exclusive must wait for them, and its pending intent blocks new shared holders
x_tid = bl._id('exclusive', 'bench')
check(bl.try_exclusive(x_tid, 'bench', 60).startswith('busy shared'), 'exclusive while shared held')
check(bl.try_shared('s-late', 'late', 60) == 'busy exclusive pending', 'new shared blocked by a pending exclusive')
bl.release(a)
check(bl.try_exclusive(x_tid, 'bench', 60).startswith('busy shared'), 'exclusive still waits for the second holder')
bl.release(b)
check(bl.try_exclusive(x_tid, 'bench', 60) == 'taken', 'exclusive after the shared holders drained')
check(bl.try_shared('s-x', 'apps', 60).startswith('busy exclusive'), 'shared while exclusive held')
check(bl.try_exclusive('e-other', 'other', 60).startswith('busy exclusive'), 'second exclusive while held')
bl.withdraw('e-other')
bl.release('exclusive:' + x_tid)
check(bl.try_shared('s-after', 'apps', 60) == 'taken', 'shared after the exclusive released')
bl.release('shared:s-after')

# a crashed shared holder expires
check(bl.try_shared('s-crash', 'crashed', 1) == 'taken', 'short shared hold')
time.sleep(2.1)
check(bl.try_exclusive('e-1', 'bench', 60) == 'taken', 'expired shared holder does not block')
bl.release('exclusive:e-1')
# an abandoned exclusive intent expires too (WANT_TTL), so it cannot starve shared holders forever
bl.WANT_TTL = 1
check(bl.try_exclusive('e-gone', 'gone', 60) in ('taken',), 'take, to leave intent below')
bl.release('exclusive:e-gone')
check(bl.try_shared('s-hold', 'h', 60) == 'taken', 'shared hold'); bl.try_exclusive('e-gone2', 'gone2', 60)  # leaves intent
check(bl.try_shared('s-blocked', 'b', 60) == 'busy exclusive pending', 'intent blocks shared')
time.sleep(2.1)
check(bl.try_shared('s-free', 'b', 60) == 'taken', 'abandoned intent expired')
for t in ('shared:s-hold', 'shared:s-free'): bl.release(t)
bl.WANT_TTL = 60
# a stale exclusive lock (older than STALE_S) is moved aside
os.makedirs(os.path.join(D, 'pinpin-bench.lock')); old = time.time() - bl.STALE_S - 10
os.utime(os.path.join(D, 'pinpin-bench.lock'), (old, old))
check(bl.try_shared('s-stale', 'apps', 60) == 'taken', 'stale exclusive lock ignored'); bl.release('shared:s-stale')
# an older client that takes the exclusive lock by plain mkdir still excludes shared holders
os.makedirs(os.path.join(D, 'pinpin-bench.lock'))
check(bl.try_shared('s-legacy', 'apps', 60).startswith('busy exclusive'), 'legacy mkdir lock respected')
os.rmdir(os.path.join(D, 'pinpin-bench.lock'))

# regression (2026-11-04, randomised run): a release that runs late must not free the NEXT holder's lock
t1 = bl.take('exclusive', 'first', 60, 5); bl.release(t1)
t2 = bl.take('exclusive', 'second', 60, 5)
bl.release(t1)  # the first holder's release, arriving late
check(os.path.isdir(os.path.join(D, 'pinpin-bench.lock')), 'a late release freed the next holder\'s lock')
check(bl.try_shared('s-late2', 'apps', 60).startswith('busy exclusive'), 'shared got in after a late release')
bl.release(t2)
check(not os.path.isdir(os.path.join(D, 'pinpin-bench.lock')), 'own release did not free the lock')
# regression: a failed stat (the lock vanished between the checks) must never count as stale and move a live lock
fake = tempfile.mkdtemp(prefix='fakestat-'); open(os.path.join(fake, 'stat'), 'w').write('#!/bin/sh\nexit 1\n'); os.chmod(os.path.join(fake, 'stat'), 0o755)
t3 = bl.take('exclusive', 'held', 60, 5); path = os.environ['PATH']; os.environ['PATH'] = fake + ':' + path
try:
    check(bl.try_shared('s-statfail', 'apps', 60).startswith('busy exclusive'), 'failed stat treated a live lock as stale')
finally:
    os.environ['PATH'] = path
check(os.path.isdir(os.path.join(D, 'pinpin-bench.lock')), 'failed stat moved a live lock')
bl.release(t3)

# regression: a "who" with shell characters (parentheses, quotes, $, ;) must not break the remote commands
weird = "work-09 compile check (no deploy) 'x' $HOME; rm -rf /"
tw = bl.take('exclusive', weird, 60, 5); check(os.path.isdir(os.path.join(D, 'pinpin-bench.lock')), 'exclusive with a weird who')
bl.release(tw); check(not os.path.isdir(os.path.join(D, 'pinpin-bench.lock')), 'released with a weird who')
ts = bl.take('shared', weird, 60, 5); bl.release(ts)
check(not os.listdir(os.path.join(D, 'pinpin-bench.shared')), 'shared with a weird who released')

# CLI: runs the command holding the lock, then releases
out = subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'benchlock.py'), 'shared', '--who', 'cli',
                      '--max', '30', '--', 'bash', '-c', f'ls {D}/pinpin-bench.shared | wc -l'], capture_output=True, text=True, timeout=60)
check(out.stdout.strip() == '1' and out.returncode == 0, f'CLI holds while running ({out.stdout!r} {out.stderr[-200:]!r})')
check('shared: \n' in bl.status() or bl.status().rstrip().endswith('shared:'), 'CLI released')

# heartbeat (review fix): a CLI command that runs longer than its --max keeps its hold
here = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'benchlock.py')
# (--max 4, checked at 7 s: past the first expiry with slack for a slow, loaded VPS; --max 2 flaked once in the gate,
# its whole-second expiry leaving ~1 s between heartbeats)
pr = subprocess.Popen([sys.executable, here, 'shared', '--who', 'long', '--max', '4', '--', 'sleep', '10'])
time.sleep(7)
check(bl.try_exclusive('e-hb', 'bench', 60).startswith('busy shared'), 'a shared hold expired while its command still ran')
pr.wait(timeout=30); bl.withdraw('e-hb')
check(bl.try_exclusive('e-hb2', 'bench', 60) == 'taken', 'exclusive after the long shared command ended'); bl.release('exclusive:e-hb2')

# fail closed (2026-11-06): when counting the other side's markers fails (here: wc missing, as a refused fork would
# be), a request must read the lock as busy, not as free
sh = bl.take('shared', 'apps', 60, 5)
shim = tempfile.mkdtemp(prefix='benchlock-shim-'); open(os.path.join(shim, 'wc'), 'w').write('#!/bin/sh\nexit 1\n')
os.chmod(os.path.join(shim, 'wc'), 0o755); path0 = os.environ['PATH']; os.environ['PATH'] = shim + ':' + path0
r1 = bl.try_exclusive('x-fc', 'bench', 60); r2 = bl.try_shared('s-fc', 'late', 60)
os.environ['PATH'] = path0; bl.withdraw('x-fc'); bl.release(sh); bl.release('shared:s-fc')
check(r1 != 'taken', f'exclusive with a failing holder count: busy, not taken ({r1})')
check(r2 != 'taken', f'shared with a failing intent count: busy, not taken ({r2})')
check(bl.try_exclusive('x-fc2', 'bench', 60) == 'taken', 'and the lock works again once counting works'); bl.release('exclusive:x-fc2')

# randomised: 6 workers, mixed modes; an exclusive hold never overlaps any other hold
events = []; lk = threading.Lock()


def worker(k):
    rnd = random.Random(k)
    for _ in range(6):
        mode = 'exclusive' if rnd.random() < 0.35 else 'shared'
        tok = bl.take(mode, f'w{k}', 30, 60)
        with lk: events.append((time.time(), 'in', mode, k))
        time.sleep(rnd.uniform(0.02, 0.15))
        with lk: events.append((time.time(), 'out', mode, k))
        bl.release(tok)
        time.sleep(rnd.uniform(0, 0.05))


for rnd_round in range(3):  # three rounds; each worker's choices are seeded (k), the interleaving is the OS's
    ts = [threading.Thread(target=worker, args=(k + 10 * rnd_round,)) for k in range(6)]
    for t in ts: t.start()
    for t in ts: t.join(timeout=300)
inside = {}; bad = 0; shared_peak = 0
for _, kind, mode, k in sorted(events):
    if kind == 'in':
        others = [m for m in inside.values()]
        if mode == 'exclusive' and others: bad += 1
        if mode == 'shared' and 'exclusive' in others: bad += 1
        inside[k] = mode
        shared_peak = max(shared_peak, sum(1 for m in inside.values() if m == 'shared'))
    else:
        inside.pop(k, None)
check(bad == 0 and len(events) == 216, f'randomised: {bad} overlaps, {len(events)} events')
check(shared_peak >= 2, f'shared holders did run together (peak {shared_peak})')
print(f'benchlock: {fails} failures (randomised: 108 holds, shared peak {shared_peak}, {bad} overlaps)')
sys.exit(1 if fails else 0)
