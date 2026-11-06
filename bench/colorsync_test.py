#!/usr/bin/env python3
"""Self-test for bench/colorsync.py: ps time parsing, the % over a window, a restart inside the window, the gate."""
import pathlib, subprocess, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import colorsync as cs

fails = 0
def check(cond, msg):
    global fails
    if not cond: fails += 1; print('FAIL', msg)

check(cs.seconds('0:23.43') == 23.43, 'mm:ss.cc')
check(cs.seconds('1:02:03.50') == 3723.5, 'hh:mm:ss.cc')
check(cs.seconds('2-01:00:00') == 2 * 86400 + 3600, 'dd-hh:mm:ss')
s = 'T 1000\n   1:00.00\n   0:30.00\nT 1010\n   1:03.00\n   0:31.50\n'  # +4.5 s CPU over 10 s
check(abs(cs.pct(s) - 45.0) < 1e-9, f'pct 45, got {cs.pct(s)}')
check(cs.pct('T 1000\n 0:10.00\n') is None, 'one reading: unknown')
check(cs.pct('T 1000\n 9:00.00\nT 1010\n 0:01.00\n') is None, 'restart inside the window: unknown')
check(cs.pct('') is None, 'Air unreachable: unknown')
check(cs.parse('T 5\nps: no such process\nT 6\n') == [(5, 0.0), (6, 0.0)], 'junk lines ignored')
run = lambda args, text: subprocess.run([sys.executable, cs.__file__, *args], input=text, capture_output=True, text=True)
r = run(['over', '60'], 'T 0\n 0:00.00\nT 10\n 0:10.00\n')  # 100%
check(r.returncode == 0 and r.stdout.strip() == '100', f'over 60 at 100%: {r.returncode} {r.stdout!r}')
r = run(['over', '60'], s)
check(r.returncode == 1 and r.stdout.strip() == '45', f'not over 60 at 45%: {r.returncode} {r.stdout!r}')
check(run(['over', '60'], '').returncode == 2, 'unreadable: exit 2')
r = run(['pct'], s)
check(r.returncode == 0 and r.stdout.strip() == '45', 'pct prints 45')
check('sleep 7' in cs.remote(7) and cs.remote().count('ps -o time=') == 2, 'remote sampler')
# the sampler really runs and parses here (no colorsync on the VPS: two empty readings, 0%)
out = subprocess.run(['bash', '-c', cs.remote(1)], capture_output=True, text=True).stdout
check(cs.pct(out) is not None and len(cs.parse(out)) == 2, f'sampler output parses: {out!r}')
# air_smoke's gate, with a stand-in ssh that answers the colorsync sampler with a fixed sample
import os, tempfile
def smoke(sample, gate):
    d = tempfile.mkdtemp()
    with open(os.path.join(d, 'ssh'), 'w') as f:
        f.write('#!/bin/bash\ncase "$*" in *colorsync*) printf "%s" "$SAMPLE";; esac\n')
    os.chmod(os.path.join(d, 'ssh'), 0o755)
    env = dict(os.environ, PATH=d + ':' + os.environ['PATH'], SAMPLE=sample, COLORSYNC_GATE=gate)
    return subprocess.run(['bash', str(pathlib.Path(cs.__file__).parent / 'air_smoke.sh')], env=env,
                          capture_output=True, text=True, timeout=60)
r = smoke('T 0\n 0:00.00\nT 10\n 0:10.00\n', '60')
check(r.returncode == 1 and 'colorsync at 100%' in r.stdout, f'gate fails at 100%: {r.stdout!r}')
r = smoke(s, '60')
check('colorsync 45% <= 60%: ok' in r.stdout and 'SMOKE FAIL: median' in r.stdout, f'gate passes at 45%: {r.stdout!r}')
r = smoke('T 0\n 0:00.00\nT 10\n 0:10.00\n', '')
check('colorsync' not in r.stdout, f'gate off: {r.stdout!r}')
# colorsync_reset.sh with a stand-in ssh: SAMPLES is a list of samples (one per measure), KILL the killall result
def reset(args, samples, kill_err=''):
    d = tempfile.mkdtemp()
    with open(os.path.join(d, 'ssh'), 'w') as f:
        f.write('#!/bin/bash\nn=$(cat "$D/n" 2>/dev/null || echo 0)\n'
                'case "$*" in *killall*) [ -n "$KILL" ] && { echo "$KILL" >&2; exit 1; }; touch "$D/killed"; exit 0;; '
                '*colorsync*) python3 -c "import json,os,sys; print(json.loads(os.environ[\'SAMPLES\'])[$n])"; '
                'echo $((n+1)) > "$D/n";; esac\n')
    os.chmod(os.path.join(d, 'ssh'), 0o755)
    import json
    env = dict(os.environ, PATH=d + ':' + os.environ['PATH'], D=d, SAMPLES=json.dumps(samples), KILL=kill_err,
               COLORSYNC_LOG=os.path.join(d, 'activity.log'), COLORSYNC_WAIT='0', PINPIN_WHO='test')
    r = subprocess.run(['bash', str(pathlib.Path(cs.__file__).parent / 'colorsync_reset.sh'), *args], env=env,
                       capture_output=True, text=True, timeout=60)
    return r, os.path.exists(os.path.join(d, 'killed')), open(os.path.join(d, 'activity.log')).read()
hot, warm = 'T 0\n 0:00.00\nT 10\n 0:10.00\n', s  # 100%, 45%
cool = 'T 0\n 0:00.00\nT 10\n 0:03.00\n'  # 30%
r, killed, log = reset([], [hot, cool])
check(r.returncode == 0 and killed and '100% -> 30%' in log and 'test colorsync_reset' in log, f'reset: {r.stdout!r} {log!r}')
r, killed, log = reset(['--if-above', '40'], [cool])
check(r.returncode == 0 and not killed and 'no restart needed' in r.stdout, f'if-above, cool: {r.stdout!r}')
r, killed, log = reset(['--if-above', '40'], [warm, hot, hot, hot, hot])
check(r.returncode == 1 and killed and 'still >= 50%' in r.stdout, f'stays hot: {r.stdout!r}')
r, killed, log = reset([], [hot], 'killall: Operation not permitted')
check(r.returncode == 3 and 'sudo killall colorsyncd colorsync.displayservices' in r.stdout, f'needs root: {r.stdout!r}')
r, killed, log = reset([], [''])
check(r.returncode == 2 and not killed, f'unreachable: {r.stdout!r}')
print('colorsync: all checks passed' if not fails else f'colorsync: {fails} failed')
sys.exit(1 if fails else 0)
