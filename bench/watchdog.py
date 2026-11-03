#!/usr/bin/env python3
"""Bench watchdog: reports (never fixes) the stalls that cost hours on 2026-11-03. Once a minute it checks
  hung run      the bench lock is held more than 10 min past its run's expected end (START time + --secs + 4 min
                setup/teardown), or for more than 30 min when no open START explains it
  stalled queue a queue script (suite.sh, rebaseline.sh, gaming_pairs.sh, headset_vt_pairs.sh) is running, the lock
                is free, and nothing has STARTed or ENDed for more than 5 min
  ghost job     an activity.log START with a VPS pid and no END, whose process no longer exists
  long hold     simulated wear (prox_close) not followed by automation_disable for more than 45 min, or a headset
                hold START with no END for more than 45 min
and appends one `WATCHDOG <kind>: <detail>` line to bench/activity.log per finding (each repeated at most hourly).
An agent tails the log for WATCHDOG lines and relays them (scripts cannot message sessions).

    python3 bench/watchdog.py            # loop (systemd: pinpin-bench-watchdog)
    python3 bench/watchdog.py --once     # one pass, findings printed
Tests: bench/watchdog_test.py (fake logs, offline).
"""
import calendar, json, pathlib, re, subprocess, sys, time

HERE = pathlib.Path(__file__).resolve().parent
ACTIVITY, PROX_LOG, STATE = HERE / 'activity.log', pathlib.Path('/tmp/pinpin-prox.log'), pathlib.Path('/tmp/pinpin-watchdog.json')
QUEUES = ('suite.sh', 'rebaseline.sh', 'gaming_pairs.sh', 'headset_vt_pairs.sh', 'runner.sh', 'synth2.sh', 'synth3.sh', 'synth4.sh')
NET_AGENTS = ('work-42',)  # network changes (stress.py NET_AGENTS): runs wait while one is open
# headset_queue.sh is not here: waiting for a headset wake window is its normal state
LINE = re.compile(r'^(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ) (\S+) (\S+?)\[([^\]]*)\] (START\+END|START|END|WATCHDOG)\b ?(.*)$')


def ts(s): return calendar.timegm(time.strptime(s, '%Y-%m-%dT%H:%M:%SZ'))


def parse(lines):
    """Activity lines -> dicts {t, who, script, pid, kind, text}; other lines ignored."""
    out = []
    for l in lines:
        m = LINE.match(l.strip())
        if m: out.append({'t': ts(m.group(1)), 'who': m.group(2), 'script': m.group(3), 'pid': m.group(4), 'kind': m.group(5), 'text': m.group(6)})
    return out


def open_starts(events):
    """STARTs without a later END from the same script[pid] (manual entries pair by script and who)."""
    opened = {}
    for e in events:
        key = (e['who'], e['script'], e['pid'])
        if e['kind'] == 'START': opened[key] = e
        elif e['kind'] in ('END', 'START+END'): opened.pop(key, None)
    return list(opened.values())


def expected_end(start):
    m = re.search(r'--secs (\d+)', start['text'])
    return start['t'] + (int(m.group(1)) if m else 120) + 240


def check(now, events, lock, procs, prox_lines):
    """Pure: findings as (kind, key, detail). lock: None or {'owner': str, 'age_s': int}. procs: {pid: cmdline}."""
    found, opened = [], open_starts(events)
    if lock:
        runs = [s for s in opened if s['script'].startswith('stress.py') or 'lock' in s['text']]
        if runs:
            for s in runs:
                late = now - expected_end(s)
                if late > 600: found.append(('hung run', f"{s['script']}[{s['pid']}]@{s['t']}", f"{s['who']} {s['script']}[{s['pid']}] started {time.strftime('%H:%M:%SZ', time.gmtime(s['t']))}, {late // 60} min past its expected end; lock owner: {lock['owner']}"))
        elif lock['age_s'] > 1800:
            found.append(('hung run', f"lock:{lock['owner']}", f"bench lock held {lock['age_s'] // 60} min with no open START explaining it; owner: {lock['owner']}"))
    queues = [p for p, c in procs.items() if any(q in c for q in QUEUES)]
    # a queue of ours stalls when it runs but logs nothing of its own: other agents' slots must not hide that
    # (2026-11-03: a self-matching pgrep wait kept the synth queue idle 17:32-17:50 while work-09's slots ran)
    last = max((e['t'] for e in events if e['kind'] in ('START', 'END', 'START+END') and e['who'] == 'work-c4'), default=0)
    # stress.py holds new runs while a network change is open and for 2 min after its END (network_change()): that
    # wait is the queue working as designed, not a stall (2026-11-03 21:24: a false "stalled queue" during the ch36
    # rollback). Count the stall from the end of that settle instead.
    net = [e for e in events if e['who'] in NET_AGENTS]
    net_open = any(e['who'] in NET_AGENTS for e in open_starts(events))
    last = max([last] + [e['t'] + 120 for e in net if e['kind'] in ('END', 'START+END')])
    if queues and not lock and not net_open and now - last > 600:
        found.append(('stalled queue', f"queue@{last}", f"queue running ({', '.join(sorted({q for q in QUEUES for c in procs.values() if q in c}))}), lock free, nothing of ours started for {(now - last) // 60} min"))
    for s in opened:
        # a process is in its exit window for a few seconds (lock release, then its END line): only a START older than
        # 2 minutes counts, and report() needs it on two consecutive passes
        if s['pid'].isdigit() and s['pid'] not in procs and s['who'] == 'work-c4' and now - s['t'] > 120:
            found.append(('ghost job', f"{s['script']}[{s['pid']}]@{s['t']}", f"{s['script']}[{s['pid']}] START at {time.strftime('%H:%M:%SZ', time.gmtime(s['t']))} has no END and its process is gone"))
    for s in opened:
        if 'headset hold' in s['text'] and now - s['t'] > 2700:
            found.append(('long hold', f"hold@{s['t']}", f"headset hold open for {(now - s['t']) // 60} min ({s['who']} {s['script']}[{s['pid']}])"))
    last_close = None
    for l in prox_lines:
        m = re.match(r'^(\S+Z) (prox_close|automation_disable)', l)
        if not m: continue
        last_close = ts(m.group(1)) if m.group(2) == 'prox_close' else None
    if last_close and now - last_close > 2700:
        found.append(('long hold', f"prox@{last_close}", f"simulated wear (prox_close) on for {(now - last_close) // 60} min with no automation_disable"))
    return found


def gather():
    events = parse(ACTIVITY.read_text().splitlines()) if ACTIVITY.exists() else []
    try:
        out = subprocess.run(['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10', 'pinpin-air',
                              'L=~/.local/share/pinpin-bench.lock; [ -d $L ] && echo $(( $(date +%s) - $(stat -f %m $L) )) && cat $L/owner'],
                             capture_output=True, text=True, timeout=30).stdout.splitlines()
        lock = {'age_s': int(out[0]), 'owner': ' '.join(out[1:]).strip()} if out and out[0].isdigit() else None
    except (subprocess.TimeoutExpired, ValueError):
        lock = None
    procs = {}
    for l in subprocess.run(['ps', '-eo', 'pid=,args='], capture_output=True, text=True).stdout.splitlines():
        p = l.strip().split(None, 1)
        if len(p) == 2: procs[p[0]] = p[1]
    prox = PROX_LOG.read_text().splitlines() if PROX_LOG.exists() else []
    return events, lock, procs, prox


def report(findings, now):
    seen = json.loads(STATE.read_text()) if STATE.exists() else {}
    pending = seen.pop('_pending', {})  # ghost jobs need two consecutive passes
    new, still = [], {}
    for kind, key, detail in findings:
        if kind == 'ghost job' and key not in pending: still[key] = now; continue
        if now - seen.get(key, 0) >= 3600:
            seen[key] = now; new.append((kind, detail))
            with ACTIVITY.open('a') as f: f.write(f"{time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime(now))} watchdog watchdog.py[{__import__('os').getpid()}] WATCHDOG {kind}: {detail}\n")
    seen['_pending'] = still
    STATE.write_text(json.dumps(seen))
    return new


def main():
    once = '--once' in sys.argv
    while True:
        now = int(time.time())
        events, lock, procs, prox = gather()
        f = check(now, events, lock, procs, prox)
        if once: print(json.dumps(f, indent=1)); return
        report(f, now)
        time.sleep(60)


if __name__ == '__main__':
    main()
