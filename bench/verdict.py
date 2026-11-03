#!/usr/bin/env python3
"""Mechanical keep/drop verdict for one flag A/B pair (docs/bench-scoreboard.md, "Pass/drop rules").

    python3 bench/verdict.py RULE OFF.json ON.json     # RULE: bpcut | longgop | noperiodic | vt-cinema | vt-wall | damage | prifps | gamecap | gameyield | vt_realtime | codec-h264
    python3 bench/verdict.py --self-test

OFF and ON are bench/results/stress-*.json from the same scenario, run back to back (ON = flag set; for codec-h264,
ON = H.264, OFF = HEVC). Prints KEEP, DROP or RERUN with the figures; exit 0 KEEP, 1 DROP, 2 RERUN.
A second RERUN on a fresh pair counts as DROP: the flag must show its effect, not hover in the noise.
Every rule also drops on a gate check (bench/gate.py) that passes OFF and fails ON.
"""
import json, pathlib, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import gate


def atlas(r):
    a = r.get('atlas') or {}
    return {'mbps': a.get('mbps'), 'gapP99': (a.get('freshGapMs') or {}).get('p99'), 'keyframes': a.get('keyframes'),
            'fps': (a.get('freshFps') or {}).get('p50'), 'silence': a.get('longestSilenceS'),
            'below': (r.get('vt') or {}).get('belowWantedPct'), 'cpu': r.get('senderCpuPct'), 'vtValid': (r.get('checks') or {}).get('vt layouts valid (vt_check.py)')}


def game(r):
    """Numbers behind gate.gaming(): frame latency p50/p99 (ms), host ms, fps, stalls over 100 ms."""
    series = r.get('series') or []
    g = [x.get('game') or {} for x in series]
    if not g or not any('displayed' in x for x in g): return {}
    first, last = g[0], g[-1]
    span = max(1e-6, series[-1]['t'] - series[0]['t'])
    if 'latHist' in last and 'latHist' in first:
        h = [b - a for a, b in zip(first['latHist'], last['latHist'])]; p50, p99 = gate.hist_pct(h, 50), gate.hist_pct(h, 99)
    else:
        lats = [x.get('lat') for x in g if (x.get('lat') or 0) > 0]; p50, p99 = gate.pct(lats, 50), gate.pct(lats, 99)
    return {'lat50': p50, 'lat99': p99, 'host': gate.pct([x.get('hostMs') for x in g if (x.get('hostMs') or 0) > 0], 50),
            'fps': round((last.get('displayed', 0) - first.get('displayed', 0)) / span, 1),
            'stalls100': (last.get('stalls100', 0) - first.get('stalls100', 0)) if 'stalls100' in last else None}


def new_failures(off, on):
    def flat(v): return {f'{part}: {k}': ok for part in ('screens', 'gaming') for k, (ok, _) in (v.get(part) or {}).items()}
    a, b = flat(gate.judge(off)), flat(gate.judge(on))
    return [k for k, ok in b.items() if not ok and a.get(k, False)]


def le(x, y, k=1.0): return x is not None and y is not None and x <= y * k
def ge(x, y, k=1.0): return x is not None and y is not None and x >= y * k


def latency_rule(off, on, need_fps=True):
    """Game-stream rules (gameyield, vt_realtime, codec-h264): frame latency p99 down 10%+ or host time down 1 ms+."""
    A, B = game(off), game(on)
    if not A or not B: return 'RERUN', 'no game stream in one half'
    better = le(B['lat99'], A['lat99'], 0.9) or (A['host'] is not None and B['host'] is not None and B['host'] <= A['host'] - 1)
    worse = ge(B['lat99'], A['lat99']) and (A['host'] is None or B['host'] is None or B['host'] >= A['host'])
    worse |= (B['stalls100'] or 0) > (A['stalls100'] or 0) + 1
    if need_fps and (B['fps'] or 0) < 55: worse = True
    why = f"lat p99 {A['lat99']} -> {B['lat99']} ms, p50 {A['lat50']} -> {B['lat50']}, host {A['host']} -> {B['host']} ms, fps {A['fps']} -> {B['fps']}, stalls>100 {A['stalls100']} -> {B['stalls100']}"
    return ('DROP' if worse else 'KEEP' if better else 'RERUN'), why


def rule(name, off, on):
    A, B = atlas(off), atlas(on)
    if name in ('bpcut', 'longgop', 'noperiodic'):  # bpcut: smaller frames under backpressure; longgop/noperiodic: fewer or no periodic keyframe bursts
        why = f"gap p99 {A['gapP99']} -> {B['gapP99']} ms, keyframes {A['keyframes']} -> {B['keyframes']}, mbps {A['mbps']} -> {B['mbps']}"
        if ge(B['gapP99'], A['gapP99']) or (B['keyframes'] or 0) > (A['keyframes'] or 0) * 1.2: return 'DROP', why
        return ('KEEP' if le(B['gapP99'], A['gapP99'], 0.8) and le(B['keyframes'], A['keyframes']) else 'RERUN'), why
    if name == 'vt-cinema':  # hidden screens as mip tails: fewer bits, focused screen unharmed, layouts valid
        why = f"mbps {A['mbps']} -> {B['mbps']}, fresh fps {A['fps']} -> {B['fps']}, below wanted {B['below']}%, layouts valid {B['vtValid']}"
        if B['vtValid'] is False or ge(B['mbps'], A['mbps']) or (B['fps'] or 0) < (A['fps'] or 0) - 5: return 'DROP', why
        return ('KEEP' if le(B['mbps'], A['mbps'], 0.8) and (B['below'] is None or B['below'] <= 10) else 'RERUN'), why
    if name == 'vt-wall':  # every screen visible and small: vt must not cost anything
        why = f"mbps {A['mbps']} -> {B['mbps']}, gap p99 {A['gapP99']} -> {B['gapP99']} ms, layouts valid {B['vtValid']}"
        ok = B['vtValid'] is not False and le(B['mbps'], A['mbps'], 1.1) and le(B['gapP99'], A['gapP99'], 1.2)
        return ('KEEP' if ok else 'DROP'), why
    if name == 'damage':  # still screens cost no copies: less sender CPU, same stream
        why = f"sender cpu {A['cpu']} -> {B['cpu']}%, mbps {A['mbps']} -> {B['mbps']}, gap p99 {A['gapP99']} -> {B['gapP99']} ms"
        if ge(B['cpu'], A['cpu'], 0.95) or not le(B['gapP99'], A['gapP99'], 1.1) or not le(B['mbps'], A['mbps'], 1.05): return 'DROP', why
        return ('KEEP' if le(B['cpu'], A['cpu'], 0.85) else 'RERUN'), why
    if name == 'prifps':  # background screens at 10-15 fps: fewer bits, the focused screen unharmed
        why = f"mbps {A['mbps']} -> {B['mbps']}, fresh fps {A['fps']} -> {B['fps']}, sender cpu {A['cpu']} -> {B['cpu']}%"
        if ge(B['mbps'], A['mbps'], 0.95) or (B['fps'] or 0) < (A['fps'] or 0) - 5: return 'DROP', why
        return ('KEEP' if le(B['mbps'], A['mbps'], 0.85) else 'RERUN'), why
    if name in ('gameyield', 'gamecap', 'encshare', 'vt_realtime', 'codec-h264'):
        return latency_rule(off, on)
    raise SystemExit(f'unknown rule {name}')


def host_key(r):
    h = r.get('host') or {}
    return (h.get('chip'), h.get('model')) if h else None


def verdict(name, off, on):
    # Never across hosts (benchmarks are pinned to the M1 Air): a pair from different hosts, or with an unknown host
    # on one side only, is not comparable.
    if host_key(off) != host_key(on):
        return 'RERUN', f'not comparable: hosts differ ({host_key(off)} vs {host_key(on)}); benchmarks are pinned to the M1 Air'
    v, why = rule(name, off, on)
    nf = new_failures(off, on)
    if nf: v, why = 'DROP', why + '; newly failing: ' + ', '.join(nf)
    return v, why


def self_test():
    a, b = {'host': {'chip': 'Apple M1', 'model': 'MacBookAir10,1'}}, {'host': {'chip': 'Apple M4', 'model': 'Mac16,10'}}
    if verdict('bpcut', a, b)[0] != 'RERUN': print('FAIL: cross-host pair must not be compared'); return False
    def res(mbps=20, gap=40, keys=10, fps=50, lat=None, host=8, stalls=0, below=None, valid=None, gfps=60, cpu=None):
        r = {'checks': {}, 'senderCpuPct': cpu, 'atlas': {'mbps': mbps, 'freshGapMs': {'p99': gap}, 'keyframes': keys, 'freshFps': {'p50': fps}}}
        if valid is not None: r['checks']['vt layouts valid (vt_check.py)'] = valid
        if below is not None: r['vt'] = {'belowWantedPct': below}
        if lat is not None:  # 100 s of game frames, all in the bucket holding `lat`
            b = [0] * 31; b[min(30, lat // 5)] = int(gfps * 100)
            r['series'] = [{'t': 0, 'game': {'displayed': 0, 'latHist': [0] * 31, 'hostMs': host, 'stalls100': 0}},
                           {'t': 100, 'game': {'displayed': int(gfps * 100), 'latHist': b, 'hostMs': host, 'stalls100': stalls}}]
        return r
    cases = [
        ('bpcut', res(gap=500, keys=20), res(gap=300, keys=8), 'KEEP'),
        ('bpcut', res(gap=300, keys=8), res(gap=320, keys=8), 'DROP'),
        ('bpcut', res(gap=300, keys=8), res(gap=270, keys=8), 'RERUN'),
        ('vt-cinema', res(mbps=30), res(mbps=12, valid=True, below=2), 'KEEP'),
        ('vt-cinema', res(mbps=30), res(mbps=12, valid=False), 'DROP'),
        ('vt-cinema', res(mbps=30, fps=55), res(mbps=12, valid=True, fps=40), 'DROP'),
        ('vt-wall', res(mbps=30, gap=40), res(mbps=31, gap=45, valid=True), 'KEEP'),
        ('vt-wall', res(mbps=30, gap=40), res(mbps=36, gap=45, valid=True), 'DROP'),
        ('gameyield', res(lat=40, host=9), res(lat=20, host=9), 'KEEP'),
        ('gameyield', res(lat=20, host=9), res(lat=40, host=9), 'DROP'),
        ('vt_realtime', res(lat=20, host=9), res(lat=20, host=7), 'KEEP'),
        ('codec-h264', res(lat=20, host=9), res(lat=20, host=9, gfps=50), 'DROP'),
        ('codec-h264', res(lat=20, host=9), res(lat=20, host=9), 'DROP'),
        ('damage', res(cpu=40), res(cpu=20), 'KEEP'),
        ('damage', res(cpu=40), res(cpu=39), 'DROP'),
        ('damage', res(cpu=40, gap=40), res(cpu=20, gap=60), 'DROP'),
        ('prifps', res(mbps=30, fps=58), res(mbps=18, fps=57), 'KEEP'),
        ('prifps', res(mbps=30, fps=58), res(mbps=18, fps=40), 'DROP'),
        ('encshare', res(lat=40, host=9), res(lat=25, host=9), 'KEEP'),
    ]
    bad = 0
    for name, off, on, want in cases:
        got, why = rule(name, off, on)
        if got != want: bad += 1; print(f'FAIL {name}: want {want}, got {got} ({why})')
    print(f'verdict self-test: {len(cases)} cases, {bad} failures')
    return bad == 0


if __name__ == '__main__':
    if sys.argv[1:] == ['--self-test']: sys.exit(0 if self_test() else 1)
    if len(sys.argv) != 4: sys.exit(__doc__)
    v, why = verdict(sys.argv[1], json.load(open(sys.argv[2])), json.load(open(sys.argv[3])))
    print(f'{v}  {sys.argv[1]}: {why}')
    sys.exit({'KEEP': 0, 'DROP': 1, 'RERUN': 2}[v])
