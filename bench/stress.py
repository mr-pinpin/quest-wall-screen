#!/usr/bin/env python3
"""N-screen stress test for the house network (docs/PERFORMANCE.md, "Stress test").

  sudo -n python3 bench/stress.py --n 10 --secs 120 [--wan K] [--keep]     # stand-in headset (default)
  sudo -n python3 bench/stress.py --headset ...                             # the real Quest, only while unworn

Nobody has to wear anything, and the test never runs while someone is using the gear:
  - It refuses to start, and aborts mid-run, when the laptop has been used in the last 10 minutes (HID idle time)
    or when someone is wearing the headset (app in VISIBLE/FOCUSED). --force skips the laptop check only.
  - The default mode uses a stand-in headset (bench/stress/headset_sim.py on the Mini). It receives the atlas
    stream and sends the same demand as the Quest's stress mode, from a separate test copy of the sender on
    ports 57730/57731, run as the PinPin Host plugin "pinpin-atlas-test". So the live sender and the headset are untouched.
  - --headset uses the Quest itself, unworn, for headset-only numbers (fps, decode latency). It simulates wear
    (prox_close; the Quest then renders exactly as when worn), starts the app if needed and puts it in stress mode
    (files/stress.json → room_lab.cpp stressDemand) on the live sender. Afterwards it restores everything and hands
    the headset back to its real proximity sensor. While wear is simulated, the sensor can't be read, so a 2-minute
    --headset run can't notice someone putting it on midway.

Steps:
  1. Create the missing laptop displays 2..N+1 (pinpin-displays :57710, 1920x1200 at 1x) and fill them with
     a moving GPU-drawn pattern (bench/stress/pattern.swift).
  2. Start the demand source (stand-in or headset), and optionally --wan K internet downloads from the laptop.
  3. Measure for --secs: ping from the Mini (the gateway) to the internet over en1 and to the headset over Wi-Fi,
     the house internet traffic at the gateway, the laptop's Wi-Fi bytes, the stream as received, and the
     sender's log and crash reports.
  4. Tear down everything it created and write bench/results/stress-<time>-n<N>.json plus a row in
     bench/results/stress.md.
"""
import argparse, atexit, base64, contextlib, fcntl, json, math, os, pathlib, shlex, statistics, subprocess, sys, time

ROOT = pathlib.Path(__file__).resolve().parent.parent
HERE = pathlib.Path(__file__).resolve().parent / 'stress'
PKG = 'com.pinpin.wallscreen.roomlab'
ADB = '~/.local/share/pinpin-quest/platform-tools/adb -s 127.0.0.1:26351'
APP = '~/Applications/PinPin Stress Pattern.app'
HEADSET_IP = '192.168.0.3'
LAPTOP_IP = '192.168.0.102'  # fallback only: the laptop's DHCP address changes (it was .104 after the 2026-11-03 reboot),
                             # and a stale one left the stand-in with no stream at all; laptop_ip() asks the laptop
IDLE_NEEDED = 600
# Shared bench lock on the laptop (agreed by work-c4 and work-09): a directory made with mkdir (atomic), holding an
# owner note. Every test that creates displays or runs a test sender takes it; afterwards it is moved aside.
# Older than LOCK_STALE_S means its holder died: taken over.
BENCH_LOCK = '~/.local/share/pinpin-bench.lock'
LOCK_STALE_S = 1800
HOST_PLUGINS = '"$HOME/Library/Application Support/PinPin Host/plugins"'
TEST_PLUGIN = {'exec': '~/.local/share/pinpin-atlas/pinpin-atlas-test', 'args': ['--port', '57730', '--demand-port', '57731'],
               'log': '~/.local/share/pinpin-atlas/atlas-sim.log', 'truncateLog': True, 'restartDelay': 1.5}


def ssh(host, cmd, timeout=60, check=False, stdin=None):
    r = subprocess.run(['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10', host, cmd],
                       capture_output=True, text=True, timeout=timeout, input=stdin)
    if check and r.returncode:
        sys.exit(f'{host}: {cmd[:80]}... failed: {r.stderr.strip()[-400:]}')
    return r.stdout


def air(cmd, **k): return ssh('pinpin-air', cmd, **k)
def mini(cmd, **k): return ssh('pinpin-mini', cmd, **k)
def headset(cmd, **k): return mini(f"{ADB} shell {json.dumps(cmd)}", **k)


def displays():
    # The helper serves one request at a time and creating a display takes ~9 s, so wait and retry.
    for _ in range(4):
        out = air('curl -s -m30 localhost:57710/displays', timeout=45)
        if out.strip().startswith('['): return json.loads(out)
        time.sleep(5)
    sys.exit('stress: the displays helper (:57710) did not answer')


def laptop_idle_s():
    out = air("ioreg -c IOHIDSystem | awk '/HIDIdleTime/ {print int($NF/1000000000); exit}'")
    return int(out.strip() or 0)


def headset_status():
    out = headset(f'pidof {PKG} >/dev/null && run-as {PKG} cat files/status.json')
    try: return json.JSONDecoder().raw_decode(out[out.index('{'):])[0]
    except ValueError: return None


# Our own simulated wear (prox_close) makes dumpsys report HEADSET_MOUNTED. Every script that sends it and every
# real-sensor probe takes this lock, so a probe never reads a prox_close another process sent a moment earlier
# (2026-11-03: the headset sat in the kitchen while two of our processes raced into "someone is wearing it").
# Every override we send is logged, so a MOUNTED reading can be checked against what we did.
PROX_LOCK, PROX_LOG = '/tmp/pinpin-prox.lock', '/tmp/pinpin-prox.log'


@contextlib.contextmanager
def prox_lock():
    with open(PROX_LOCK, 'a') as f:
        fcntl.flock(f, fcntl.LOCK_EX)
        try: yield
        finally: fcntl.flock(f, fcntl.LOCK_UN)


def prox(action, who=''):
    # action: 'prox_close' (simulate wear) or 'automation_disable' (real sensor); call with prox_lock held.
    with open(PROX_LOG, 'a') as f: f.write(f"{time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())} {action} {who or sys.argv[0]} pid {os.getpid()}\n")
    return headset(f'am broadcast -a com.oculus.vrpowermanager.{action} >/dev/null', timeout=30)


def headset_worn():
    # The real proximity sensor: drop our override, then a person is only someone the sensor reports as MOUNTED for
    # a full 5 s (sampled each second); any unmounted/standby sample means nobody. App state can't tell, because
    # simulated wear makes it VISIBLE/FOCUSED too. Callers that were holding wear re-apply it themselves.
    with prox_lock():
        prox('automation_disable', 'headset_worn')
        out = headset('sleep 1; for i in 1 2 3 4 5; do dumpsys vrpowermanager | grep -m1 "^State:"; sleep 1; done', timeout=40)
    states = [l.split(':', 1)[1].strip() for l in out.splitlines() if l.startswith('State:')]
    return len(states) >= 3 and all(s == 'HEADSET_MOUNTED' for s in states)


def simulate_wear(on):
    # Unworn, with prox_close the Quest renders exactly as when worn (measured: state 5, 72 fps, no throttling).
    with prox_lock(): prox('prox_close' if on else 'automation_disable', 'simulate_wear')


def pct(xs, p):
    if not xs: return None
    xs = sorted(xs); return round(xs[min(len(xs) - 1, int(math.ceil(p / 100 * len(xs))) - 1)], 1)


def pings(text):
    out = []
    for line in text.splitlines():
        if 'time=' in line:
            try: out.append(float(line.split('time=')[1].split()[0]))
            except ValueError: pass
    return out


def link_bytes(run, iface):
    # netstat -ib: Name Mtu Network Address Ipkts Ierrs Ibytes Opkts Oerrs Obytes
    for line in run(f'netstat -ib -I {iface}').splitlines():
        f = line.split()
        if len(f) >= 10 and f[0] == iface and f[2].startswith('<Link'):
            return int(f[6]), int(f[9])
    return 0, 0


def longest_gap(series):
    # headset mode: longest stretch (s) in which the atlas carried under 0.2 Mbit/s (a frozen picture)
    worst = run = 0.0
    for a, b in zip(series, series[1:]):
        dt = max(0.1, b['t'] - a['t'])
        run = run + dt if (b['bytes'] - a['bytes']) * 8 / 1e6 / dt < 0.2 else 0.0
        worst = max(worst, run)
    return round(worst, 1)


def lock_take(owner):
    """'' when this run now holds the bench lock, else who holds it."""
    out = air(f"L={BENCH_LOCK}; if [ -d $L ] && [ $(( $(date +%s) - $(stat -f %m $L) )) -gt {LOCK_STALE_S} ]; then "
              f"mv $L $L.stale-$(date +%s); echo stale; fi; mkdir $L 2>/dev/null && echo {json.dumps(owner)} > $L/owner && echo taken "
              f"|| cat $L/owner")
    return '' if 'taken' in out.split() else (out.strip().splitlines() or ['?'])[-1]


def lock_release():
    air(f"L={BENCH_LOCK}; [ -d $L ] && mv $L $L.done-$(date +%s); ls -dt $L.done-* 2>/dev/null | tail -n +21 | xargs rm -rf")


def laptop_ip():
    ip = air('ipconfig getifaddr en0', timeout=20).strip()
    return ip if ip.count('.') == 3 else LAPTOP_IP


def plugin_override(name, extra_args):
    """Append args to a PinPin Host plugin for one run and restart it; returns the original spec for plugin_restore."""
    path = f'{HOST_PLUGINS}/{name}.json'
    orig = air(f'cat {path}').strip()
    spec = json.loads(orig); spec['args'] = list(spec.get('args', [])) + list(extra_args)
    air(f"printf %s {shlex.quote(json.dumps(spec))} > {path}; curl -s -m10 -X POST '127.0.0.1:57790/restart?name={name}'")
    return orig


def plugin_restore(name, orig):
    air(f"printf %s {shlex.quote(orig)} > {HOST_PLUGINS}/{name}.json; curl -s -m10 -X POST '127.0.0.1:57790/restart?name={name}'")


HOLD_STATUS = '/tmp/pinpin-headset-hold.json'  # written by bench/headset_hold.py while it holds the headset awake


def hold_says_unworn():
    # While headset_hold.py simulates wear it probes the real sensor itself once a minute and records the result.
    # Dropping its override again here (headset_worn) would fight it, and our own simulated wear must never read as
    # a person: trust a fresh hold probe instead.
    try:
        st = json.loads(pathlib.Path(HOLD_STATUS).read_text())
        return time.time() - st['t'] < 150 and not st['worn']
    except (OSError, ValueError, KeyError):
        return False


def busy(force, sim=False):
    """Why the gear is in use right now, or '' when the test may run."""
    if not (sim and hold_says_unworn()) and headset_worn(): return 'someone is wearing the headset'
    idle = laptop_idle_s()
    if not force and idle < IDLE_NEEDED: return f'the laptop was used {idle} s ago (needs {IDLE_NEEDED} s idle)'
    return ''


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--n', type=int, default=10); ap.add_argument('--secs', type=int, default=120)
    ap.add_argument('--sweep', type=float, default=3.0, help='seconds the virtual gaze stays on each screen')
    ap.add_argument('--wan', type=int, default=0, help='parallel internet downloads from the laptop')
    ap.add_argument('--keep', action='store_true', help='leave the created displays in place')
    ap.add_argument('--scenario', default='ring', choices=['ring', 'cinema', 'vr', 'wall'], help='stand-in demand pattern (see headset_sim.py)')
    ap.add_argument('--game', choices=['LAPTOP', 'MINI'], help='--headset only: also run that main game stream and measure it')
    ap.add_argument('--headset', action='store_true', help='use the real Quest, unworn with simulated wear, instead of the stand-in')
    ap.add_argument('--force', action='store_true', help='skip the laptop-idle check (never the headset check)')
    ap.add_argument('--flags', default='', help='sender flags, e.g. bpcut (AtlasSender --flags): on the test copy in stand-in mode, '
                    'on the live sender (restarted, restored after) with --headset')
    ap.add_argument('--headset-flag', action='append', default=[], metavar='NAME',
                    help='--headset only: force a room_lab flag for the run via its files/NAME-on file (vt, budget, gputime, '
                         'wifill); removed in teardown. Repeatable.')
    ap.add_argument('--static', type=int, default=0, help='leave the last K created screens without the moving pattern (still desktops)')
    ap.add_argument('--codec', choices=['H264', 'HEVC'], help='--headset only: the game stream codec (room_lab CODEC flag, '
                    'files/codec-h264, v0.39+); default: as the headset is set')
    ap.add_argument('--sunshine', action='append', default=[], metavar='NAME=VALUE',
                    help='Sunshine config override for this run, e.g. vt_realtime=enabled (appended to the sunshine plugin args, '
                         'restored after); repeatable')
    ap.add_argument('--allow-over-20', action='store_true', help='allow more than 20 screens (30 crashed replayd and wedged WindowServer)')
    ap.add_argument('--cinema-size', default='', help='WxH for screen 2 (the cinema screen), e.g. 2560x1600: wider than one '
                    '1920 cell, so vt tiles it (3840x2160 was not capturable on the Air). Pair runs with the same size')
    a = ap.parse_args()
    cinema = tuple(int(v) for v in a.cinema_size.split('x')) if a.cinema_size else None
    if a.n > 20 and not a.allow_over_20:
        sys.exit('refusing --n %d: 30 concurrent captures wedged the laptop on 2026-11-03; pass --allow-over-20 once visibility-only capture is proven' % a.n)
    sim = not a.headset
    test_plugin = dict(TEST_PLUGIN, args=TEST_PLUGIN['args'] + (['--flags', a.flags] if a.flags else []))
    why = busy(a.force, sim)
    if why: sys.exit(f'stress: not running, {why}')
    holder = lock_take(f'stress.py {" ".join(sys.argv[1:])} since {time.strftime("%H:%M:%S")}')
    if holder: sys.exit(f'stress: not running, the bench lock is held ({holder})')
    atexit.register(lock_release)
    lap_ip = laptop_ip()
    ids = list(range(2, a.n + 2))
    t0 = time.strftime('%Y%m%d-%H%M%S')
    print(f'stress: {a.n} screens for {a.secs} s, wan={a.wan}, demand from {"stand-in on the Mini" if sim else "the headset"}')

    # 1. displays and pattern
    have = {d['screen']: (d.get('w'), d.get('h')) for d in displays()}
    if cinema and 2 in have and have[2] != cinema:  # a leftover screen 2 of another size: recreate it
        air("curl -s -m40 -X POST 'localhost:57710/remove?id=2'", timeout=60); del have[2]
    created = [i for i in ids if i not in have]
    for i in created:
        w, h = cinema if i == 2 and cinema else (1920, 1200)
        air(f"curl -s -m40 -X POST 'localhost:57710/displays?id={i}&w={w}&h={h}&scale=1'", timeout=60)
    time.sleep(3)
    ds = {d['screen']: d['display'] for d in displays()}
    missing = [i for i in ids if i not in ds]
    if missing: print('  warning: displays not created:', missing)
    # --static K: the last K created screens keep a still desktop (no pattern), like real idle screens
    moving = created[:max(0, len(created) - a.static)]
    pattern = [str(ds[i]) for i in moving if i in ds]
    if pattern:
        # Run the binary directly: through `open` it got no arguments. Windows still reach the GUI session.
        air(f"(nohup {APP.replace(' ', chr(92) + ' ')}/Contents/MacOS/stresspattern {' '.join(pattern)} "
            f"> ~/.local/share/pinpin-stress/pattern.log 2>&1 &); sleep 2; cat ~/.local/share/pinpin-stress/pattern.log")
    print(f'  displays: {sorted(ds)} (created {created}, pattern on {len(pattern)})')

    log_path = '~/.local/share/pinpin-atlas/' + ('atlas-sim.log' if sim else 'atlas.log')
    crash_glob = 'pinpin-atlas-test-*.ips' if sim else 'pinpin-atlas-2*.ips'
    log0 = '' if sim else air(f'cat {log_path}')
    aborted = ''; st0 = time.time(); proc = None; app_was_running = True; live_paused = False; overrides = {}; cpu = []
    try:
        # 2. per-run overrides of the live plugins (restored in `finally`), before any game session starts
        if a.sunshine:
            overrides['sunshine'] = plugin_override('sunshine', a.sunshine); time.sleep(6)  # Sunshine re-probes its encoders
        if a.flags and not sim:
            overrides['pinpin-atlas'] = plugin_override('pinpin-atlas', ['--flags', a.flags]); time.sleep(3)
        # demand source
        if sim:
            # Pause the live sender so only the test copy captures (agreed with work-09); restarted in `finally`.
            air("curl -s -m5 -X POST '127.0.0.1:57790/stop?name=pinpin-atlas'"); live_paused = True
            # The test copy runs as a PinPin Host plugin, so it inherits the Host's Screen Recording grant
            # (no Terminal, no AppleScript). Its own binary name keeps the live sender's pkill away from it.
            air('cp -f ~/.local/share/pinpin-atlas/pinpin-atlas ~/.local/share/pinpin-atlas/pinpin-atlas-test && '
                f'printf %s {json.dumps(json.dumps(test_plugin))} > {HOST_PLUGINS}/pinpin-atlas-test.json')
        else:
            # Nobody is wearing it (checked above): simulate wear, start the app if needed, put it in stress mode.
            app_was_running = bool(headset(f'pidof {PKG}').strip())
            simulate_wear(True)
            if not app_was_running:
                headset(f'input keyevent KEYCODE_WAKEUP; am start -n {PKG}/.MainActivity --ez capture false >/dev/null'); time.sleep(8)
            for fl in a.headset_flag:  # polled ~1/s by the app; removed in teardown
                headset(f"run-as {PKG} touch files/{fl}-on")
            if a.headset_flag: time.sleep(2)
            if a.codec:  # before the launch below, which reads it; removed in teardown
                headset(f"run-as {PKG} sh -c '{'touch' if a.codec == 'H264' else 'rm -f'} files/codec-h264'"); time.sleep(2)  # polled ~1/s
            spec = base64.b64encode(json.dumps({'n': a.n, 'sweep': a.sweep}).encode()).decode()
            headset(f"run-as {PKG} sh -c 'echo {spec} | base64 -d > files/stress.json'")
            if a.game:
                headset(f"run-as {PKG} sh -c 'echo {a.game} > files/launch'"); time.sleep(12)  # session setup
        time.sleep(6)  # sender start, demand and resize settle
        if a.wan:
            # nohup: plain background jobs died with the ssh session, so the earlier --wan runs loaded nothing
            air(f"for i in $(seq {a.wan}); do nohup curl -s -o /dev/null -m {a.secs + 5} "
                f"'https://speed.cloudflare.com/__down?bytes=2000000000' > /dev/null 2>&1 & done")

        # 3. measure (in the background, while watching that nobody starts using the gear)
        rx0, tx0 = link_bytes(air, 'en0'); w0 = link_bytes(mini, 'en1'); st0 = time.time()
        probe = (f"(ping -b en1 -i 0.2 -t {a.secs} 1.1.1.1 > /tmp/stress-wan.txt 2>&1 &);"
                 f"(ping -i 0.2 -t {a.secs} 192.168.1.1 > /tmp/stress-zyxel.txt 2>&1 &);"
                 f"(ping -i 0.2 -t {a.secs} {HEADSET_IP} > /tmp/stress-lan.txt 2>&1 &);"
                 f"(ping -i 0.2 -t {a.secs} {lap_ip} > /tmp/stress-laptop.txt 2>&1 &);")
        if sim:
            probe += f"python3 - {lap_ip} --port 57730 --demand-port 57731 --n {a.n} --secs {a.secs} --sweep {a.sweep} --scenario {a.scenario}"
            if 'vt' in a.flags.split(','): probe += ' --v2'  # the vt flag only acts on v2 demand
            stdin = (HERE / 'headset_sim.py').read_text()
        else:
            probe += (f"end=$(( $(date +%s) + {a.secs} )); while [ $(date +%s) -lt $end ]; do "
                      f"LC_ALL=C LANG=C perl -e 'alarm 5; exec @ARGV' {ADB} shell run-as {PKG} cat files/status.json; echo; sleep 2; done")  # 5 s cap: a sleeping headset hangs adb
            stdin = None
        proc = subprocess.Popen(['ssh', '-o', 'BatchMode=yes', 'pinpin-mini', probe], stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if stdin: proc.stdin.write(stdin)
        proc.stdin.close()
        idle_before = laptop_idle_s()
        sender = 'pinpin-atlas-test' if sim else 'pinpin-atlas'
        probe_deadline = time.time() + a.secs + 90  # a probe that overruns never ends on its own (2026-11-03: 23 min)
        while proc.poll() is None:
            time.sleep(10)
            if proc.poll() is not None: break
            if time.time() > probe_deadline:
                aborted = 'the probe overran (headset or laptop unreachable)'
                proc.kill(); mini('pkill -f "ping -b en1 -i 0.2" ; pkill -f "ping -i 0.2"'); break
            # the sender's CPU (one sample per 10 s; ps %cpu is a decaying average, enough to compare flags)
            try: cpu.append(float(air(f"ps -Ao %cpu,comm | awk '$2 ~ /{sender}$/ {{print $1; exit}}'", timeout=15).strip()))
            except ValueError: pass
            idle = laptop_idle_s()
            if not a.force and idle < idle_before: aborted = 'the laptop was touched'
            elif sim and not hold_says_unworn() and headset_worn(): aborted = 'someone put the headset on'
            idle_before = idle
            if aborted:
                proc.kill(); mini('pkill -f "ping -b en1 -i 0.2" ; pkill -f "ping -i 0.2"'); break
        out = proc.stdout.read() if not aborted else ''
        rx1, tx1 = link_bytes(air, 'en0'); w1 = link_bytes(mini, 'en1'); dt = time.time() - st0
        wan, lan = pings(mini('cat /tmp/stress-wan.txt')), pings(mini('cat /tmp/stress-lan.txt'))
        lap = pings(mini('cat /tmp/stress-laptop.txt'))  # the stand-in's own link (the headset may be asleep: lanPingMs)
        zyxel = pings(mini('cat /tmp/stress-zyxel.txt'))  # the Mini's 2.4 GHz hop alone: link jitter vs the internet
    finally:
        # 4. teardown
        for name, orig in overrides.items(): plugin_restore(name, orig)
        if proc and proc.poll() is None: proc.kill()
        if sim: air(f'printf %s {json.dumps(json.dumps(dict(TEST_PLUGIN, enabled=False)))} > {HOST_PLUGINS}/pinpin-atlas-test.json')
        if live_paused: air("curl -s -m5 -X POST '127.0.0.1:57790/start?name=pinpin-atlas'")
        else:
            headset(f'run-as {PKG} rm -f files/stress.json' + (' files/codec-h264' if a.codec else '') + ''.join(f' files/{fl}-on' for fl in a.headset_flag))
            if not app_was_running: headset(f'am force-stop {PKG}')
            simulate_wear(False)  # back to the real sensor: an unworn headset goes to sleep as usual
        if a.wan: air("pkill -f 'speed.cloudflare.com/__down' || true")
        if pattern: air('pkill -f stresspattern || true')
        if not a.keep:
            for i in created: air(f"curl -s -m40 -X POST 'localhost:57710/remove?id={i}'", timeout=60)
    if aborted:
        sys.exit(f'stress: aborted after {int(time.time() - st0)} s, {aborted}; everything was cleaned up')

    crashes = air(f"find ~/Library/Logs/DiagnosticReports -name '{crash_glob}' -mmin -{math.ceil((time.time() - st0) / 60) + 1} 2>/dev/null").split()
    log1 = air(f'cat {log_path}')
    log = (log1[len(log0):] if log1.startswith(log0) else log1).splitlines()
    skipped = sorted({l.split('screen ')[1].split()[0] for l in log if 'atlas full' in l})
    bitrates = [int(l.split('bitrate ')[1].split()[0]) for l in log if 'bitrate ' in l]
    layouts = [l for l in log if 'layout ' in l]
    r = {
        'when': t0, 'n': a.n, 'secs': a.secs, 'wan': a.wan, 'mode': 'sim' if sim else 'headset', 'scenario': (a.scenario if sim else 'ring') + (f'@{a.cinema_size}' if a.cinema_size else '') + (f'~static{a.static}' if a.static else '') + (f'+{a.flags}' if a.flags else '') + ''.join(f'+sunshine:{o}' for o in a.sunshine) + (f'+codec:{a.codec}' if a.codec else '') + ''.join(f'+headset:{fl}' for fl in a.headset_flag),
        'wanPingMs': {'p50': pct(wan, 50), 'p95': pct(wan, 95), 'p99': pct(wan, 99), 'max': max(wan, default=None), 'count': len(wan)},
        'zyxelPingMs': {'p50': pct(zyxel, 50), 'p95': pct(zyxel, 95), 'p99': pct(zyxel, 99), 'count': len(zyxel)},
        'lanPingMs': {'p50': pct(lan, 50), 'p95': pct(lan, 95), 'p99': pct(lan, 99), 'count': len(lan)},  # Mini -> headset
        'laptopPingMs': {'p50': pct(lap, 50), 'p95': pct(lap, 95), 'p99': pct(lap, 99), 'count': len(lap), 'ip': lap_ip},  # Mini -> laptop
        'houseInternetMbps': {'down': round((w1[0] - w0[0]) * 8 / 1e6 / dt, 1), 'up': round((w1[1] - w0[1]) * 8 / 1e6 / dt, 1)},
        'laptopWifiMbps': {'tx': round((tx1 - tx0) * 8 / 1e6 / dt, 1), 'rx': round((rx1 - rx0) * 8 / 1e6 / dt, 1)},
        'sender': {'bitrateKbps': bitrates[-8:], 'layouts': len(layouts), 'lastLayout': layouts[-1].split('layout ')[1] if layouts else None,
                   'skipped': skipped, 'crashes': crashes},
    }
    r['senderCpuPct'] = round(statistics.mean(cpu), 1) if cpu else None  # the atlas sender, sampled every 10 s
    checks = {
        'internet ping p95 < 50 ms': r['wanPingMs']['p95'] is not None and r['wanPingMs']['p95'] < 50,
        'no screen skipped': not skipped,
        'sender never crashed': not crashes,
    }
    if sim:
        s = json.loads(out.strip().splitlines()[-1]) if out.strip() else {}
        r['atlas'] = {k: s.get(k) for k in ('mbps', 'mbpsMax', 'freshFps', 'freshGapMs', 'receiverCpuPct', 'longestSilenceS', 'emptySeconds', 'keyframes', 'layouts', 'connects', 'sizes')}
        r['series'] = s.get('perSecondMbps', [])
        if 'v2Layouts' in s:
            # vt: every v2 layout the stand-in saw, checked offline (bench/vt_check.py)
            sys.path.insert(0, str(ROOT / 'bench')); import vt_check
            vt_file = ROOT / 'bench' / 'results' / f'stress-{t0}-{a.scenario}+{a.flags}-n{a.n}-layouts.jsonl'
            vt_file.parent.mkdir(parents=True, exist_ok=True)
            vt_file.write_text(''.join(json.dumps(x) + '\n' for x in s['v2Layouts']))
            r['vt'] = {'v2Layouts': len(s['v2Layouts']), 'belowWantedPct': s.get('belowWantedPct'), 'layoutsFile': vt_file.name}
            checks['vt layouts valid (vt_check.py)'] = vt_check.recorded(str(vt_file))
        checks['stand-in reached the sender'] = (s.get('connects') or 0) > 0  # else every other figure is void
        checks['atlas within 40 Mbit/s'] = s.get('mbps') is not None and s['mbps'] <= 40
        checks['no atlas freeze over 5 s'] = s.get('longestSilenceS') is not None and s['longestSilenceS'] <= 5
        if a.scenario == 'vr':
            checks['VR: atlas under 1 Mbit/s'] = s.get('mbps') is not None and s['mbps'] < 1
            del checks['no atlas freeze over 5 s']  # nothing to show: silence is correct
        else:
            checks['stream kept 30+ fresh frames/s (median)'] = (s.get('freshFps') or {}).get('p50', 0) >= 30
            checks['fresh-frame gap p99 < 50 ms'] = ((s.get('freshGapMs') or {}).get('p99') or 1e9) < 50
        fps_txt, mbps, lat_txt, gap = '-', s.get('mbps'), '-', s.get('longestSilenceS')
    else:
        samples, dec = [], json.JSONDecoder()
        for line in out.splitlines():
            line = line.strip()
            if line.startswith('{'):
                try: samples.append(dec.raw_decode(line)[0])
                except ValueError: pass
        fps = [x['fps'] for x in samples]
        series = [{'t': round(x['uptime'], 1), 'fps': x['fps'], 'stressN': x['spell'].get('stressN'),
                   'bytes': x['atlas']['video']['bytes'], 'packets': x['atlas']['video']['packets'],
                   'size': x['atlas']['video']['size'], 'lat': x['atlas']['video']['latencyMs'],
                   'game': {'stage': x['moonlight']['stage'], 'lat': x['video']['latencyMs'], 'rtt': x['video']['rttMs'],
                            'displayed': x['video']['displayed'], 'dropped': x['video']['dropped'],
                            # v0.38 per-frame counters, cumulative; bench/gate.py diffs them for exact p50/p99 and stalls
                            'latHist': x['video'].get('latHist'), 'stalls50': x['video'].get('stalls50'), 'stalls100': x['video'].get('stalls100'),
                            'gapMaxMs': x['video'].get('gapMaxMs'), 'hostMs': x['video'].get('hostMs')}} for x in samples]
        lat = [x['lat'] for x in series if x['lat'] > 0]
        mbps = round((series[-1]['bytes'] - series[0]['bytes']) * 8 / 1e6 / max(1, series[-1]['t'] - series[0]['t']), 1) if len(series) > 1 else None
        gap = longest_gap(series)
        r['headsetFps'] = {'min': min(fps, default=None), 'mean': round(statistics.mean(fps), 1) if fps else None}
        r['atlas'] = {'mbps': mbps, 'latencyP50': pct(lat, 50), 'latencyP99': pct(lat, 99), 'longestGapS': gap}
        r['series'] = series
        if a.game:
            g = [x['game'] for x in series]; up = [x for x in g if x['lat'] > 0]
            span = max(1, series[-1]['t'] - series[0]['t'])
            r['game'] = {'host': a.game, 'stages': sorted({x['stage'] for x in g}),
                         'latencyP50': pct([x['lat'] for x in up], 50), 'latencyP99': pct([x['lat'] for x in up], 99),
                         'rttP50': pct([x['rtt'] for x in up], 50), 'fps': round((g[-1]['displayed'] - g[0]['displayed']) / span, 1),
                         'dropped': g[-1]['dropped'] - g[0]['dropped']}
            checks['game stream ran'] = bool(up)
            checks['game latency p99 < 60 ms'] = (r['game']['latencyP99'] or 1e9) < 60
            checks['game fps >= 55'] = r['game']['fps'] >= 55
        checks['headset >= 70 fps'] = r['headsetFps']['min'] is not None and r['headsetFps']['min'] >= 70
        checks['headset stress mode engaged'] = any(x['stressN'] == a.n for x in series)
        checks['atlas within 40 Mbit/s'] = mbps is not None and mbps <= 40
        checks['no atlas freeze over 5 s'] = gap <= 5
        fps_txt, lat_txt = r['headsetFps']['min'], f"{r['atlas']['latencyP50']}/{r['atlas']['latencyP99']}"
    r['checks'] = checks; r['pass'] = all(checks.values())

    out_dir = ROOT / 'bench' / 'results'; out_dir.mkdir(parents=True, exist_ok=True)
    result = out_dir / f'stress-{t0}-{r["scenario"]}-n{a.n}.json'
    result.write_text(json.dumps(r, indent=1))
    print(f'stress: result {result}')  # suite.sh runs bench/gate.py on it
    md = out_dir / 'stress.md'
    if not md.exists():
        md.write_text('# Stress runs\n\n')
    with md.open('a') as f:
        f.write(f"| {t0} | {r['mode']}/{r['scenario']} | {a.n} | {a.wan} | {r['wanPingMs']['p50']}/{r['wanPingMs']['p95']} | {r['lanPingMs']['p50']}/{r['lanPingMs']['p95']} | "
                f"{r['houseInternetMbps']['down']} | {fps_txt} | {mbps} | {gap} | {lat_txt} | {r['laptopWifiMbps']['tx']} | "
                f"{len(crashes)} | {'yes' if r['pass'] else 'NO'} |\n")
    print(json.dumps({k: v for k, v in r.items() if k != 'series'}, indent=1))


if __name__ == '__main__':
    main()
