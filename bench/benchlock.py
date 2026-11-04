#!/usr/bin/env python3
"""The Air's bench lock with two modes (AGENTS.md):

  EXCLUSIVE  benchmark measurement windows (stress.py runs, encprobe, deploy + air_smoke): nothing else runs on the Air.
  SHARED     functional tests that each use their own scratch display (apps runner, assistant end-to-end): any number
             at once, never during an exclusive hold.

Writer priority: an exclusive request first leaves an intent marker, then waits for the shared holders to drain;
new shared requests wait while any intent is pending, so benchmarks never starve.

    python3 bench/benchlock.py shared --who apps --max 600 -- CMD ARGS...      # wait, run CMD holding the lock, release
    python3 bench/benchlock.py exclusive --who work-09 --max 900 -- CMD ...
    tok=$(python3 bench/benchlock.py take shared --who apps --max 600)  ...  python3 bench/benchlock.py release "$tok"
    python3 bench/benchlock.py status

On the Air, under ~/.local/share/:
  pinpin-bench.lock/        EXCLUSIVE: a directory made with mkdir (atomic), as before, so every older client that
                            takes it with mkdir still excludes everyone; its owner file names the holder
  pinpin-bench.shared/<id>  one file per SHARED holder: "who\\nexpiry-epoch"
  pinpin-bench.wantx/<id>   one file per waiting EXCLUSIVE request: "who\\nexpiry-epoch" (refreshed while it waits)
Each side writes its own marker before checking for the other's, so a shared holder and an exclusive one can never
both get in. Expired markers (a crashed holder) are removed by whoever looks next; an exclusive lock older than
STALE_S is moved aside as before (stress.py's rule).
Set BENCHLOCK_LOCAL=/some/dir to run against a local directory instead of the Air (the tests do).
"""
import argparse, hashlib, os, re, shlex, subprocess, sys, time

STALE_S = 1800        # an exclusive lock older than this is stale (stress.py LOCK_STALE_S)
WANT_TTL = 60         # an exclusive intent lives this long unless refreshed (each poll refreshes it)
POLL = 5


def _run(script):
    local = os.environ.get('BENCHLOCK_LOCAL')
    if local:
        return subprocess.run(['bash', '-c', f'D={shlex.quote(local)}\n' + script], capture_output=True, text=True, timeout=60).stdout
    err = ''
    for attempt in range(3):  # a loaded Air drops some ssh connections (2026-11-04 05:00, load 8-9): retry before giving up
        try:
            p = subprocess.run(['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10', 'pinpin-air', 'bash -s'],
                               input='D=~/.local/share\n' + script, capture_output=True, text=True, timeout=60)
            if p.stdout.strip() or p.returncode == 0: return p.stdout
            err = p.stderr.strip().splitlines()[-1] if p.stderr.strip() else f'ssh exit {p.returncode}'
        except subprocess.TimeoutExpired:
            err = 'ssh timed out'
        time.sleep(2 * (attempt + 1))
    return f'busy unreachable ({err})\n'


# shell helpers: expire old markers; count live ones; is the exclusive lock held (dropping a stale one)
_PRE = r'''
L=$D/pinpin-bench.lock; S=$D/pinpin-bench.shared; W=$D/pinpin-bench.wantx; now=$(date +%s); mkdir -p $S $W
for f in $S/* $W/*; do [ -f "$f" ] || continue; e=$(sed -n 2p "$f"); [ -n "$e" ] && [ "$e" -lt $now ] && rm -f "$f"; done
xheld() {
  [ -d $L ] || return 1
  m=$(stat -c %Y $L 2>/dev/null || stat -f %m $L 2>/dev/null)
  case "$m" in ''|*[!0-9]*) [ -d $L ]; return; esac   # vanished between the checks: never treat a failed stat as stale
  if [ $(( now - m )) -gt %(stale)d ]; then mv $L $L.stale-$now-$$; return 1; fi
  return 0
}
live() { ls -A "$1" 2>/dev/null | grep -v "^$2\$" | wc -l | tr -d ' '; }
'''


def _pre():
    return _PRE.replace('%(stale)d', str(STALE_S))


def _id(mode, who):
    # the token goes into remote shell commands and file names: only [A-Za-z0-9_.-] (a "who" with parentheses broke it)
    return re.sub(r'[^A-Za-z0-9_.-]', '_', f'{mode[0]}-{who}-{os.getpid()}-{hashlib.sha1(os.urandom(8)).hexdigest()[:6]}')[:120]


def try_shared(tid, who, max_s):
    """One attempt: 'taken', or why not ('exclusive: <owner>' / 'exclusive pending')."""
    out = _run(_pre() + f'''
if xheld; then echo "busy exclusive: $(cat $L/owner 2>/dev/null)"; exit; fi
if [ $(live $W) -gt 0 ]; then echo "busy exclusive pending"; exit; fi
printf '%s\\n%s\\n' {shlex.quote(who)} $(( now + {int(max_s)} )) > $S/{tid}
if xheld || [ $(live $W) -gt 0 ]; then rm -f $S/{tid}; echo "busy exclusive pending"; exit; fi
echo taken
''')
    return out.strip().splitlines()[-1] if out.strip() else 'busy unreachable'


def try_exclusive(tid, who, max_s):
    """One attempt: 'taken', or why not. Leaves (refreshes) the intent marker when it has to wait."""
    out = _run(_pre() + f'''
printf '%s\\n%s\\n' {shlex.quote(who)} $(( now + {WANT_TTL} )) > $W/{tid}
if [ $(live $S) -gt 0 ]; then echo "busy shared: $(for f in $S/*; do head -1 "$f"; done | tr '\\n' ' ')"; exit; fi
if xheld; then echo "busy exclusive: $(cat $L/owner 2>/dev/null)"; exit; fi
if mkdir $L 2>/dev/null; then printf '%s\\n' {tid} > $L/id; printf '%s\\n' {shlex.quote(who)} > $L/owner; rm -f $W/{tid}; echo taken; else echo "busy exclusive: $(cat $L/owner 2>/dev/null)"; fi
''')
    return out.strip().splitlines()[-1] if out.strip() else 'busy unreachable'


def withdraw(tid):
    _run(f'rm -f $D/pinpin-bench.wantx/{tid}')


def take(mode, who, max_s=900, wait_s=3600):
    """Block until held; returns the token for release(). Raises TimeoutError after wait_s."""
    tid = _id(mode, who); end = time.time() + wait_s
    attempt = try_shared if mode == 'shared' else try_exclusive
    while True:
        r = attempt(tid, who, max_s)
        if r == 'taken':
            return f'{mode}:{tid}'
        if time.time() > end:
            if mode == 'exclusive': withdraw(tid)
            raise TimeoutError(r)
        time.sleep(POLL)


def release(token):
    mode, tid = token.split(':', 1)
    if mode == 'shared':
        _run(f'rm -f $D/pinpin-bench.shared/{tid}')
    else:
        # only our own lock: a release that runs late must never free the next holder's (found by the randomised test)
        _run(f'L=$D/pinpin-bench.lock; rm -f $D/pinpin-bench.wantx/{tid}; [ -d $L ] && [ "$(cat $L/id 2>/dev/null)" = {tid} ] && mv $L $L.done-$(date +%s)-$$; '
             f'ls -dt $L.done-* 2>/dev/null | tail -n +21 | xargs rm -rf')


def status():
    return _run(_pre() + '''
echo "exclusive: $(xheld && cat $L/owner || echo none)"
echo "pending exclusive: $(for f in $W/*; do [ -f "$f" ] && head -1 "$f"; done | tr '\\n' ' ')"
echo "shared: $(for f in $S/*; do [ -f "$f" ] && head -1 "$f"; done | tr '\\n' ' ')"
''')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    for m in ('shared', 'exclusive'):
        p = sub.add_parser(m); p.add_argument('--who', required=True); p.add_argument('--max', type=int, default=900)
        p.add_argument('--wait', type=int, default=3600); p.add_argument('command', nargs=argparse.REMAINDER)
    t = sub.add_parser('take'); t.add_argument('mode', choices=['shared', 'exclusive']); t.add_argument('--who', required=True)
    t.add_argument('--max', type=int, default=900); t.add_argument('--wait', type=int, default=3600)
    r = sub.add_parser('release'); r.add_argument('token')
    sub.add_parser('status')
    a = ap.parse_args()
    if a.cmd == 'status':
        print(status(), end=''); return
    if a.cmd == 'release':
        release(a.token); return
    if a.cmd == 'take':
        print(take(a.mode, a.who, a.max, a.wait)); return
    cmd = a.command[1:] if a.command[:1] == ['--'] else a.command
    if not cmd: sys.exit('nothing to run (put the command after --)')
    tok = take(a.cmd, a.who, a.max, a.wait)
    try:
        rc = subprocess.call(cmd)
    finally:
        release(tok)
    sys.exit(rc)


if __name__ == '__main__':
    main()
