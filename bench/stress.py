#!/usr/bin/env python3
"""N-screen stress test for the house network (docs/PERFORMANCE.md, "Stress test").

  sudo -n python3 bench/stress.py --n 10 --secs 120 [--wan 2] [--keep] [--no-headset]

What it does, from the VPS (root's ssh aliases pinpin-air and pinpin-mini):
  1. Creates the missing virtual displays 2..N+1 on the laptop (pinpin-displays :57710, 1920x1200 at 1x) and
     fills the ones it created with a moving test pattern (PinPin Stress Pattern.app; the user's own screens
     keep what they show).
  2. Writes files/stress.json on the headset: it then reports synthetic demand for screens 2..N+1 on a ring,
     with a virtual gaze turning one screen every few seconds (room_lab.cpp, stressDemand). --no-headset
     skips this and leaves the sender at its no-demand fallback (everything full size).
  3. Optionally loads the internet line with --wan K parallel downloads from the laptop (what YouTube did).
  4. Measures for --secs: ping from the Mini (the gateway) to the internet over en1 and to the headset over
     Wi-Fi, the headset's status every 2 s (fps, atlas bytes/latency/drops), the laptop's Wi-Fi bytes and the
     sender's log.
  5. Tears down everything it created and writes bench/results/stress-<time>-n<N>.json plus a line in
     bench/results/stress.md.

Pass: the gateway's internet ping p95 < 50 ms, the headset holds >= 70 fps, the atlas bytes stay inside the
demand budget whatever N is, and no screen is dropped from the atlas.
"""
import argparse, base64, json, math, pathlib, statistics, subprocess, sys, time

ROOT = pathlib.Path(__file__).resolve().parent.parent
PKG = 'com.pinpin.wallscreen.roomlab'
ADB = '~/.local/share/pinpin-quest/platform-tools/adb -s 127.0.0.1:26351'
APP = '~/Applications/PinPin Stress Pattern.app'
HEADSET_IP = '192.168.0.3'


def ssh(host, cmd, timeout=60, check=False):
    r = subprocess.run(['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10', host, cmd],
                       capture_output=True, text=True, timeout=timeout)
    if check and r.returncode:
        sys.exit(f'{host}: {cmd[:80]}... failed: {r.stderr.strip()[-400:]}')
    return r.stdout


def air(cmd, **k): return ssh('pinpin-air', cmd, **k)
def mini(cmd, **k): return ssh('pinpin-mini', cmd, **k)
def headset(cmd, **k): return mini(f"{ADB} shell {json.dumps(cmd)}", **k)


def displays():
    return json.loads(air('curl -s -m5 localhost:57710/displays', check=True) or '[]')


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


def wifi_bytes():
    # netstat -ib: Name Mtu Network Address Ipkts Ierrs Ibytes Opkts Oerrs Obytes
    for line in air('netstat -ib -I en0').splitlines():
        f = line.split()
        if len(f) >= 10 and f[0] == 'en0' and f[2].startswith('<Link'):
            return int(f[6]), int(f[9])
    return 0, 0


def longest_gap(series):
    # longest stretch (s) in which the atlas carried under 0.2 Mbit/s: a frozen picture (heartbeats only)
    worst = run = 0.0
    for a, b in zip(series, series[1:]):
        dt = max(0.1, b['t'] - a['t'])
        run = run + dt if (b['bytes'] - a['bytes']) * 8 / 1e6 / dt < 0.2 else 0.0
        worst = max(worst, run)
    return round(worst, 1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--n', type=int, default=10); ap.add_argument('--secs', type=int, default=120)
    ap.add_argument('--sweep', type=float, default=3.0, help='seconds the virtual gaze stays on each screen')
    ap.add_argument('--wan', type=int, default=0, help='parallel internet downloads from the laptop')
    ap.add_argument('--keep', action='store_true', help='leave the created displays in place')
    ap.add_argument('--no-headset', action='store_true')
    a = ap.parse_args()
    ids = list(range(2, a.n + 2))
    t0 = time.strftime('%Y%m%d-%H%M%S')
    print(f'stress: {a.n} screens for {a.secs} s, wan={a.wan}, headset={"no" if a.no_headset else "synthetic demand"}')

    # 1. displays
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

    log0 = air('cat ~/.local/share/pinpin-atlas/atlas.log')
    try:
        # 2. headset demand
        if not a.no_headset:
            spec = base64.b64encode(json.dumps({'n': a.n, 'sweep': a.sweep}).encode()).decode()
            headset(f"run-as {PKG} sh -c 'echo {spec} | base64 -d > files/stress.json'")
        time.sleep(6)  # demand + sender resize settle
        # 3. internet load
        if a.wan:
            air(f"for i in $(seq {a.wan}); do (curl -s -o /dev/null -m {a.secs + 5} "
                f"'https://speed.cloudflare.com/__down?bytes=2000000000' &) ; done")
        # 4. measure
        rx0, tx0 = wifi_bytes(); st0 = time.time()
        probe = (f"(ping -b en1 -i 0.2 -t {a.secs} 1.1.1.1 > /tmp/stress-wan.txt 2>&1 &);"
                 f"(ping -i 0.2 -t {a.secs} {HEADSET_IP} > /tmp/stress-lan.txt 2>&1 &);"
                 f"end=$(( $(date +%s) + {a.secs} )); while [ $(date +%s) -lt $end ]; do "
                 f"{ADB} shell run-as {PKG} cat files/status.json; echo; sleep 2; done")
        status_text = mini(probe, timeout=a.secs + 60)
        rx1, tx1 = wifi_bytes(); dt = time.time() - st0
        wan, lan = pings(mini('cat /tmp/stress-wan.txt')), pings(mini('cat /tmp/stress-lan.txt'))
    finally:
        # 5. teardown
        if not a.no_headset: headset(f'run-as {PKG} rm -f files/stress.json')
        if a.wan: air("pkill -f 'speed.cloudflare.com/__down' || true")
        if pattern: air('pkill -f stresspattern || true')
        if not a.keep:
            for i in created: air(f"curl -s -m10 -X POST 'localhost:57710/remove?id={i}'")

    # crash reports the sender left during the run (macOS DiagnosticReports, newer than the start)
    crashes = air(f"find ~/Library/Logs/DiagnosticReports -name 'pinpin-atlas-*.ips' -mmin -{math.ceil((time.time() - st0) / 60) + 1} 2>/dev/null").split()
    samples = []
    dec = json.JSONDecoder()
    for line in status_text.splitlines():
        line = line.strip()
        if line.startswith('{'):
            try: samples.append(dec.raw_decode(line)[0])
            except ValueError: pass
    fps = [s['fps'] for s in samples]
    series = [{'t': round(s['uptime'], 1), 'fps': s['fps'], 'stressN': s['spell'].get('stressN'),
               'bytes': s['atlas']['video']['bytes'], 'packets': s['atlas']['video']['packets'],
               'size': s['atlas']['video']['size'], 'lat': s['atlas']['video']['latencyMs'], 'live': s['atlas']['live']} for s in samples]
    engaged = a.no_headset or any(x['stressN'] == a.n for x in series)
    av = [s['atlas']['video'] for s in samples]
    lat = [v['latencyMs'] for v in av if v['latencyMs'] > 0]
    atlas_mbps = ((av[-1]['bytes'] - av[0]['bytes']) * 8 / 1e6 / max(1, samples[-1]['uptime'] - samples[0]['uptime'])) if len(av) > 1 else None
    dropped = (av[-1]['dropped'] - av[0]['dropped']) if len(av) > 1 else None
    log1 = air('cat ~/.local/share/pinpin-atlas/atlas.log')
    log = (log1[len(log0):] if log1.startswith(log0) else log1).splitlines()
    skipped = sorted({l.split('screen ')[1].split()[0] for l in log if 'atlas full' in l})
    bitrates = [int(l.split('bitrate ')[1].split()[0]) for l in log if 'bitrate ' in l]
    layouts = [l for l in log if 'layout ' in l]
    r = {
        'when': t0, 'n': a.n, 'secs': a.secs, 'wan': a.wan, 'headset': not a.no_headset,
        'wanPingMs': {'p50': pct(wan, 50), 'p95': pct(wan, 95), 'p99': pct(wan, 99), 'max': max(wan, default=None), 'count': len(wan)},
        'lanPingMs': {'p50': pct(lan, 50), 'p95': pct(lan, 95), 'p99': pct(lan, 99), 'count': len(lan)},
        'headsetFps': {'min': min(fps, default=None), 'mean': round(statistics.mean(fps), 1) if fps else None, 'samples': len(fps)},
        'atlas': {'mbps': round(atlas_mbps, 1) if atlas_mbps is not None else None, 'latencyP50': pct(lat, 50), 'latencyP99': pct(lat, 99),
                  'dropped': dropped, 'size': av[-1]['size'] if av else None, 'state': av[-1]['state'] if av else None},
        'laptopWifiMbps': {'tx': round((tx1 - tx0) * 8 / 1e6 / dt, 1), 'rx': round((rx1 - rx0) * 8 / 1e6 / dt, 1)},
        'sender': {'bitrateKbps': bitrates[-8:], 'layouts': len(layouts), 'lastLayout': layouts[-1].split('layout ')[1] if layouts else None, 'skipped': skipped},
    }
    checks = {
        'internet ping p95 < 50 ms': r['wanPingMs']['p95'] is not None and r['wanPingMs']['p95'] < 50,
        'headset >= 70 fps': r['headsetFps']['min'] is not None and r['headsetFps']['min'] >= 70,
        'atlas within 40 Mbit/s': r['atlas']['mbps'] is not None and r['atlas']['mbps'] <= 40,
        'no screen skipped': not skipped,
        'headset stress mode engaged': engaged,
        'no atlas freeze over 5 s': longest_gap(series) <= 5,
        'sender never crashed': not crashes,
    }
    r['series'] = series; r['atlas']['longestGapS'] = longest_gap(series); r['sender']['crashes'] = crashes
    r['checks'] = checks; r['pass'] = all(checks.values())
    out = ROOT / 'bench' / 'results'; out.mkdir(parents=True, exist_ok=True)
    (out / f'stress-{t0}-n{a.n}.json').write_text(json.dumps(r, indent=1))
    md = out / 'stress.md'
    if not md.exists():
        md.write_text('# Stress runs\n\n| when | N | wan | internet ping p50/p95 ms | headset ping p50/p95 | fps min | atlas Mbit/s | atlas latency p50/p99 | laptop tx Mbit/s | skipped | pass |\n|---|---|---|---|---|---|---|---|---|---|---|\n')
    with md.open('a') as f:
        f.write(f"| {t0} | {a.n} | {a.wan} | {r['wanPingMs']['p50']}/{r['wanPingMs']['p95']} | {r['lanPingMs']['p50']}/{r['lanPingMs']['p95']} | "
                f"{r['headsetFps']['min']} | {r['atlas']['mbps']} | {r['atlas']['latencyP50']}/{r['atlas']['latencyP99']} | "
                f"{r['laptopWifiMbps']['tx']} | {','.join(skipped) or '-'} | {'yes' if r['pass'] else 'NO'} |\n")
    print(json.dumps({k: v for k, v in r.items() if k != 'series'}, indent=1))


if __name__ == '__main__':
    main()
