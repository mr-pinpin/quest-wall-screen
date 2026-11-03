#!/usr/bin/env python3
"""Pass/fail gate for benchmark results (bench/results/stress-*.json): the "real-time gaming plus tens of
screens" bar, judged from a result file alone, so suites and humans read the same verdict.

    python3 bench/gate.py bench/results/stress-*.json        # one verdict block per file; exit 1 if any fails
    python3 bench/gate.py --json FILE                         # machine-readable
    python3 bench/gate.py --source content FILE               # judge screens against the content rate (60 Hz)

Gaming (runs with --game; the headset plays the main stream while the extra screens load the network):
  stream ran              frames were displayed
  fps >= 55               displayed frames per second (a 60 fps stream, minus at most ~8% loss)
  frame latency p50/p99   packet arrival -> image on the GPU, per frame (status video.latHist), <= 25 / 50 ms
  motion-to-photon p50    estimate: host processing + RTT/2 + frame latency p50 + half a 72 Hz refresh, <= 50 ms
  stalls                  gaps between displayed frames: none over 100 ms, at most 1 per minute over 50 ms
  headset >= 70 fps       the headset itself never dropped below 72 Hz pacing
Screens: every check stress.py itself recorded (atlas bitrate, freezes, crashes, internet ping, ...).
Without per-frame counters (results from before v0.38) the gaming figures fall back to stress.py's smoothed
latency, flagged "approx".
"""
import json, sys

BUCKET_MS = 5  # status video.latHist: 5 ms buckets, the last one is >= 150 ms


def hist_pct(h, p):
    n = sum(h)
    if not n: return None
    k, acc = p / 100 * n, 0
    for i, c in enumerate(h):
        acc += c
        if acc >= k: return (i + 1) * BUCKET_MS  # bucket upper edge: never flatters
    return len(h) * BUCKET_MS


def pct(xs, p):
    xs = sorted(x for x in xs if x is not None)
    return xs[min(len(xs) - 1, int(p / 100 * len(xs)))] if xs else None


def gaming(r):
    series = [x for x in (r.get('series') or []) if isinstance(x, dict)]  # sim runs store plain per-second numbers
    g = [x.get('game') or {} for x in series]
    if not g or not any('displayed' in x for x in g):
        return None
    first, last = g[0], g[-1]
    span = max(1e-6, series[-1]['t'] - series[0]['t'])
    frames = last.get('displayed', 0) - first.get('displayed', 0)
    out, approx = {}, False
    if 'latHist' in last and 'latHist' in first:
        h = [b - a for a, b in zip(first['latHist'], last['latHist'])]
        lat50, lat99 = hist_pct(h, 50), hist_pct(h, 99)
    else:  # pre-v0.38: smoothed latency samples only
        lats = [x.get('lat') for x in g if (x.get('lat') or 0) > 0]
        lat50, lat99, approx = pct(lats, 50), pct(lats, 99), True
    rtt = pct([x.get('rtt') for x in g if (x.get('rtt') or 0) > 0], 50)
    host = pct([x.get('hostMs') for x in g if (x.get('hostMs') or 0) > 0], 50)
    m2p = None if lat50 is None else round((host or 0) + (rtt or 0) / 2 + lat50 + 1000 / 72 / 2, 1)
    s50 = last.get('stalls50', 0) - first.get('stalls50', 0) if 'stalls50' in last else None
    s100 = last.get('stalls100', 0) - first.get('stalls100', 0) if 'stalls100' in last else None
    fps = round(frames / span, 1)
    hfps = (r.get('headsetFps') or {}).get('min')
    tag = ' (approx)' if approx else ''
    out['stream ran'] = (frames > 0, f'{frames} frames')
    out['fps >= 55'] = (fps >= 55, f'{fps} fps')
    out['frame latency p50 <= 25 ms' + tag] = (lat50 is not None and lat50 <= 25, f'{lat50} ms')
    out['frame latency p99 <= 50 ms' + tag] = (lat99 is not None and lat99 <= 50, f'{lat99} ms')
    out['motion-to-photon p50 <= 50 ms (estimate)' + tag] = (m2p is not None and m2p <= 50, f'{m2p} ms (host {host}, rtt {rtt})')
    if s100 is not None:
        out['no stall over 100 ms'] = (s100 == 0, f'{s100} stalls, longest {last.get("gapMaxMs")} ms')
        out['stalls over 50 ms <= 1/min'] = (s50 <= max(1, span / 60), f'{s50} in {round(span)} s')
    out['headset >= 70 fps'] = (hfps is not None and hfps >= 70, f'min {hfps}')
    return out


ABSOLUTE = ('stream kept 30+ fresh frames/s (median)', 'fresh-frame gap p99 < 50 ms', 'internet ping p95 < 50 ms')  # informational since 2026-11-03 (the ping is the Mini 2.4 GHz hop: house Wi-Fi, not our stream)
SCK_FPS = 29.4  # ScreenCaptureKit's measured per-display delivery, used when a run did not record its capture rate


SOURCE_MODE = 'capture'  # 'content' (--source content) once ScreenCaptureKit is shown to deliver the content rate


def source(r):
    # The focused screen's source rate: by default its capture rate (screen 2 in cinema, else the fastest screen;
    # measured, or assumed when a run predates captureFps); with --source content, the content rate (60 Hz pattern).
    if SOURCE_MODE == 'content' and r.get('contentHz'): return float(r['contentHz']), 'content'
    if SOURCE_MODE == 'content': return 60.0, 'assumed content'
    cap = {str(k): v for k, v in (r.get('captureFps') or {}).items()}
    if cap:
        sc = r.get('scenario', '')
        f = cap.get('2') if sc.startswith('cinema') and '2' in cap else max(cap.values())
        return f, 'measured'
    return SCK_FPS, 'assumed'


def union_rate(rates, tick=60.0):
    # Fresh frames the sender can produce: one per 60 Hz tick in which any source screen changed. With independent
    # captures at r_i fps: tick * (1 - prod(1 - r_i/tick)). Three screens at 8.6 fps -> 22.3 (work-09 measured 20-22).
    p = 1.0
    for x in rates: p *= max(0.0, 1 - min(x, tick) / tick)
    return round(tick * (1 - p), 1)


def screens_relative(r):
    # Relative to the source (user decision via work-df 2026-11-03): an absolute 30 fps can't be met when the
    # source delivers ~29.4; the stream must keep >= 95% of the source rate and lose at most 2 source frames.
    a = r.get('atlas') or {}
    fresh, gap = (a.get('freshFps') or {}).get('p50'), (a.get('freshGapMs') or {}).get('p99')
    if fresh is None and gap is None: return {}
    src, how = source(r)
    out = {}
    cap = {str(k): v for k, v in (r.get('captureFps') or {}).items()}
    shown = [str(s) for s in (r.get('patternScreens') or [])]
    if fresh is not None and cap and shown and SOURCE_MODE == 'capture':
        # Fresh fps against the source's change rate: the union of the animated screens' capture rates.
        rates = [cap[s] for s in shown if s in cap]
        u = union_rate(rates)
        out[f'fresh fps >= 90% of source change rate ({u}: union of {len(rates)}/{len(shown)} screens)'] = (fresh >= 0.9 * u and len(rates) == len(shown), f'{fresh} fps')
        if gap is not None and u:
            bound = min(2000 / u, 100)  # never looser than 100 ms, however slow the source gets
            out[f'gap p99 <= 2 source change intervals, at most 100 ms ({round(bound)} ms)'] = (gap <= bound, f'{gap} ms')
        gmax = (a.get('freshGapMs') or {}).get('max')
        if gmax is not None: out['gap max <= 250 ms'] = (gmax <= 250, f'{gmax} ms')
        return out
    elif fresh is not None and src: out[f'fresh fps >= 95% of source ({how} {src})'] = (fresh >= 0.95 * src, f'{fresh} fps')
    if gap is not None and src: out[f'gap p99 <= 2 source frames ({round(2000 / src)} ms)'] = (gap <= 2000 / src, f'{gap} ms')
    return out


def judge(r):
    checks = r.get('checks') or {}
    v = {'screens': {k: (bool(ok), '') for k, ok in checks.items()
                     if not k.startswith('game ') and k not in ABSOLUTE}}  # stress.py's own game checks are replaced by the stricter ones
    rel = screens_relative(r) if any(k in checks for k in ABSOLUTE[:2]) else {}
    v['screens'].update(rel)
    info = {k: (bool(checks[k]), 'informational') for k in ABSOLUTE if k in checks}
    cap = r.get('captureFps') or {}
    if cap and r.get('contentHz'):  # a capture-side shortfall must stay visible whatever the gate judges against
        c = max(cap.values()); info[f'capture {c} of content {r["contentHz"]} fps'] = (c >= 0.95 * r['contentHz'], 'informational')
    if info: v['info'] = info
    g = gaming(r)
    if g is not None: v['gaming'] = g
    v['pass'] = all(ok for part in ('screens', 'gaming') for ok, _ in (v.get(part) or {}).values())  # 'info' never fails a run
    return v


def main():
    global SOURCE_MODE
    if '--source' in sys.argv: SOURCE_MODE = sys.argv[sys.argv.index('--source') + 1]
    args = [a for i, a in enumerate(sys.argv[1:], 1) if a != '--json' and a != '--source' and sys.argv[i - 1] != '--source']
    as_json, failed, out = '--json' in sys.argv, False, {}
    for path in args:
        r = json.load(open(path)); v = judge(r); out[path] = v; failed |= not v['pass']
        if as_json: continue
        print(f"{'PASS' if v['pass'] else 'FAIL'}  {path}  ({r.get('mode')}/{r.get('scenario')}, n={r.get('n')}, wan={r.get('wan')})")
        for part in ('gaming', 'screens', 'info'):
            for name, (ok, why) in (v.get(part) or {}).items():
                print(f"   {'ok  ' if ok else 'FAIL'} {part}: {name}{'  - ' + why if why else ''}")
    if as_json: print(json.dumps(out, indent=1))
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
