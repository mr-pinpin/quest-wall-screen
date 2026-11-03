#!/usr/bin/env python3
"""N-screen stress test for the house network (docs/PERFORMANCE.md, "Stress test").

  sudo -n python3 bench/stress.py --n 10 --secs 120 [--wan K] [--keep]     # stand-in headset (default)
  sudo -n python3 bench/stress.py --headset ...                             # the real Quest, only while unworn

Nobody has to wear anything, and the test never runs while someone is using the gear:
  - It refuses to start, and aborts mid-run, when the laptop has been used in the last 10 minutes (HID idle time)
    or when someone is wearing the headset (app in VISIBLE/FOCUSED). --force skips the laptop check only.
  - The default mode uses a stand-in headset (bench/stress/headset_sim.py on the Mini). It receives the atlas
    stream and sends the same demand as the Quest's stress mode, from a separate test copy of the sender on
    ports 57730/57731 (bench/stress/start-atlas-test.command). So the live sender and the headset are untouched.
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
import argparse, base64, json, math, pathlib, statistics, subprocess, sys, time

ROOT = pathlib.Path(__file__).resolve().parent.parent
HERE = pathlib.Path(__file__).resolve().parent / 'stress'
PKG = 'com.pinpin.wallscreen.roomlab'
ADB = '~/.local/share/pinpin-quest/platform-tools/adb -s 127.0.0.1:26351'
APP = '~/Applications/PinPin Stress Pattern.app'
HEADSET_IP = '192.168.0.3'
LAPTOP_IP = '192.168.0.102'
IDLE_NEEDED = 600


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
    return json.loads(air('curl -s -m5 localhost:57710/displays', check=True) or '[]')


def laptop_idle_s():
    out = air("ioreg -c IOHIDSystem | awk '/HIDIdleTime/ {print int($NF/1000000000); exit}'")
    return int(out.strip() or 0)


def headset_status():
    out = headset(f'pidof {PKG} >/dev/null && run-as {PKG} cat files/status.json')
    try: return json.JSONDecoder().raw_decode(out[out.index('{'):])[0]
    except ValueError: return None


def headset_worn():
    # The real proximity sensor: drop any virtual "worn" override (prox_close) and read the power state. The app's
    # own state can't tell, because a simulated wear makes it VISIBLE/FOCUSED too.
    out = headset('am broadcast -a com.oculus.vrpowermanager.automation_disable >/dev/null; sleep 1; '
                  'dumpsys vrpowermanager | grep -m1 "^State:"')
    return 'HEADSET_MOUNTED' in out


def simulate_wear(on):
    # Unworn, with prox_close the Quest renders exactly as when worn (measured: state 5, 72 fps, no throttling).
    headset('am broadcast -a com.oculus.vrpowermanager.' + ('prox_close' if on else 'automation_disable') + ' >/dev/null')


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


def busy(force):
    """Why the gear is in use right now, or '' when the test may run."""
    if headset_worn(): return 'someone is wearing the headset'
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
    a = ap.parse_args()
    sim = not a.headset
    why = busy(a.force)
    if why: sys.exit(f'stress: not running, {why}')
    ids = list(range(2, a.n + 2))
    t0 = time.strftime('%Y%m%d-%H%M%S')
    print(f'stress: {a.n} screens for {a.secs} s, wan={a.wan}, demand from {"stand-in on the Mini" if sim else "the headset"}')

    # 1. displays and pattern
    have = {d['screen'] for d in displays()}
    created = [i for i in ids if i not in have]
    for i in created:
        air(f"curl -s -m10 -X POST 'localhost:57710/displays?id={i}&w=1920&h=1200&scale=1'")
    time.sleep(3)
    ds = {d['screen']: d['display'] for d in displays()}
    missing = [i for i in ids if i not in ds]
    if missing: print('  warning: displays not created:', missing)
    pattern = [str(ds[i]) for i in created if i in ds]
    if pattern:
        # Run the binary directly: through `open` it got no arguments. Windows still reach the GUI session.
        air(f"(nohup {APP.replace(' ', chr(92) + ' ')}/Contents/MacOS/stresspattern {' '.join(pattern)} "
            f"> ~/.local/share/pinpin-stress/pattern.log 2>&1 &); sleep 2; cat ~/.local/share/pinpin-stress/pattern.log")
    print(f'  displays: {sorted(ds)} (created {created}, pattern on {len(pattern)})')

    log_path = '~/.local/share/pinpin-atlas/' + ('atlas-sim.log' if sim else 'atlas.log')
    crash_glob = 'pinpin-atlas-test-*.ips' if sim else 'pinpin-atlas-2*.ips'
    log0 = '' if sim else air(f'cat {log_path}')
    aborted = ''; st0 = time.time(); proc = None; app_was_running = True
    try:
        # 2. demand source
        if sim:
            air('mkdir -p ~/.local/share/pinpin-stress && cat > ~/.local/share/pinpin-stress/start-atlas-test.command && '
                'chmod +x ~/.local/share/pinpin-stress/start-atlas-test.command && '
                'open -g -a Terminal ~/.local/share/pinpin-stress/start-atlas-test.command',
                stdin=(HERE / 'start-atlas-test.command').read_text())
        else:
            # Nobody is wearing it (checked above): simulate wear, start the app if needed, put it in stress mode.
            app_was_running = bool(headset(f'pidof {PKG}').strip())
            simulate_wear(True)
            if not app_was_running:
                headset(f'input keyevent KEYCODE_WAKEUP; am start -n {PKG}/.MainActivity --ez capture false >/dev/null'); time.sleep(8)
            spec = base64.b64encode(json.dumps({'n': a.n, 'sweep': a.sweep}).encode()).decode()
            headset(f"run-as {PKG} sh -c 'echo {spec} | base64 -d > files/stress.json'")
            if a.game:
                headset(f"run-as {PKG} sh -c 'echo {a.game} > files/launch'"); time.sleep(12)  # session setup
        time.sleep(6)  # sender start, demand and resize settle
        if a.wan:
            air(f"for i in $(seq {a.wan}); do (curl -s -o /dev/null -m {a.secs + 5} "
                f"'https://speed.cloudflare.com/__down?bytes=2000000000' &) ; done")

        # 3. measure (in the background, while watching that nobody starts using the gear)
        rx0, tx0 = link_bytes(air, 'en0'); w0 = link_bytes(mini, 'en1'); st0 = time.time()
        probe = (f"(ping -b en1 -i 0.2 -t {a.secs} 1.1.1.1 > /tmp/stress-wan.txt 2>&1 &);"
                 f"(ping -i 0.2 -t {a.secs} {HEADSET_IP} > /tmp/stress-lan.txt 2>&1 &);")
        if sim:
            probe += f"python3 - {LAPTOP_IP} --port 57730 --demand-port 57731 --n {a.n} --secs {a.secs} --sweep {a.sweep} --scenario {a.scenario}"
            stdin = (HERE / 'headset_sim.py').read_text()
        else:
            probe += (f"end=$(( $(date +%s) + {a.secs} )); while [ $(date +%s) -lt $end ]; do "
                      f"{ADB} shell run-as {PKG} cat files/status.json; echo; sleep 2; done")
            stdin = None
        proc = subprocess.Popen(['ssh', '-o', 'BatchMode=yes', 'pinpin-mini', probe], stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if stdin: proc.stdin.write(stdin)
        proc.stdin.close()
        idle_before = laptop_idle_s()
        while proc.poll() is None:
            time.sleep(10)
            if proc.poll() is not None: break
            idle = laptop_idle_s()
            if not a.force and idle < idle_before: aborted = 'the laptop was touched'
            elif sim and headset_worn(): aborted = 'someone put the headset on'
            idle_before = idle
            if aborted:
                proc.kill(); mini('pkill -f "ping -b en1 -i 0.2" ; pkill -f "ping -i 0.2"'); break
        out = proc.stdout.read() if not aborted else ''
        rx1, tx1 = link_bytes(air, 'en0'); w1 = link_bytes(mini, 'en1'); dt = time.time() - st0
        wan, lan = pings(mini('cat /tmp/stress-wan.txt')), pings(mini('cat /tmp/stress-lan.txt'))
    finally:
        # 4. teardown
        if proc and proc.poll() is None: proc.kill()
        if sim: air('pkill -x pinpin-atlas-test || true')
        else:
            headset(f'run-as {PKG} rm -f files/stress.json')
            if not app_was_running: headset(f'am force-stop {PKG}')
            simulate_wear(False)  # back to the real sensor: an unworn headset goes to sleep as usual
        if a.wan: air("pkill -f 'speed.cloudflare.com/__down' || true")
        if pattern: air('pkill -f stresspattern || true')
        if not a.keep:
            for i in created: air(f"curl -s -m10 -X POST 'localhost:57710/remove?id={i}'")
    if aborted:
        sys.exit(f'stress: aborted after {int(time.time() - st0)} s, {aborted}; everything was cleaned up')

    crashes = air(f"find ~/Library/Logs/DiagnosticReports -name '{crash_glob}' -mmin -{math.ceil((time.time() - st0) / 60) + 1} 2>/dev/null").split()
    log1 = air(f'cat {log_path}')
    log = (log1[len(log0):] if log1.startswith(log0) else log1).splitlines()
    skipped = sorted({l.split('screen ')[1].split()[0] for l in log if 'atlas full' in l})
    bitrates = [int(l.split('bitrate ')[1].split()[0]) for l in log if 'bitrate ' in l]
    layouts = [l for l in log if 'layout ' in l]
    r = {
        'when': t0, 'n': a.n, 'secs': a.secs, 'wan': a.wan, 'mode': 'sim' if sim else 'headset', 'scenario': a.scenario if sim else 'ring',
        'wanPingMs': {'p50': pct(wan, 50), 'p95': pct(wan, 95), 'p99': pct(wan, 99), 'max': max(wan, default=None), 'count': len(wan)},
        'lanPingMs': {'p50': pct(lan, 50), 'p95': pct(lan, 95), 'p99': pct(lan, 99), 'count': len(lan)},
        'houseInternetMbps': {'down': round((w1[0] - w0[0]) * 8 / 1e6 / dt, 1), 'up': round((w1[1] - w0[1]) * 8 / 1e6 / dt, 1)},
        'laptopWifiMbps': {'tx': round((tx1 - tx0) * 8 / 1e6 / dt, 1), 'rx': round((rx1 - rx0) * 8 / 1e6 / dt, 1)},
        'sender': {'bitrateKbps': bitrates[-8:], 'layouts': len(layouts), 'lastLayout': layouts[-1].split('layout ')[1] if layouts else None,
                   'skipped': skipped, 'crashes': crashes},
    }
    checks = {
        'internet ping p95 < 50 ms': r['wanPingMs']['p95'] is not None and r['wanPingMs']['p95'] < 50,
        'no screen skipped': not skipped,
        'sender never crashed': not crashes,
    }
    if sim:
        s = json.loads(out.strip().splitlines()[-1]) if out.strip() else {}
        r['atlas'] = {k: s.get(k) for k in ('mbps', 'mbpsMax', 'freshFps', 'freshGapMs', 'longestSilenceS', 'emptySeconds', 'keyframes', 'layouts', 'connects', 'sizes')}
        r['series'] = s.get('perSecondMbps', [])
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
                            'displayed': x['video']['displayed'], 'dropped': x['video']['dropped']}} for x in samples]
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
    (out_dir / f'stress-{t0}-{r["scenario"]}-n{a.n}.json').write_text(json.dumps(r, indent=1))
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
