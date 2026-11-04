#!/usr/bin/env python3
"""Flag benchmark campaign: one scenario block, several configs, N rounds with the config order rotated each round
(so drift over the hour hits every config alike), then each flag judged against its baseline per round with
bench/verdict.py; the flag's verdict is the majority of its rounds (KEEP or DROP in at least 2 of 3, else
INCONCLUSIVE). Results: bench/results/campaign-<block>-<date>.md and .json. Runs on the VPS like stress.py.

    python3 bench/campaign.py --block A --rounds 3 \\
        --configs "base=,longgop=longgop,vt=vt,vthero=vt+hero" \\
        --pairs "longgop:longgop:base,vt-cinema:vt:base,hero:vthero:vt" \\
        -- --scenario cinema --n 10 --secs 60 --sim-on mini --source synth --sim-extra "--v2 --hero --keyreq 15"

Each run is one stress.py call holding the bench lock only for itself; when the lock is busy it waits (12 s steps,
up to 30 min per run) so other sessions interleave. A config with no result after one retry is reported, not guessed.
"""
import argparse, datetime, json, os, pathlib, statistics, subprocess, sys, time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import verdict  # noqa: E402

LOG = '/root/work/quest-wall-screen/bench/activity.log'


def log(msg):
    line = f"{datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')} {os.environ.get('PINPIN_WHO', 'campaign')} {msg}"
    print(line, flush=True)
    try:
        with open(LOG, 'a') as f: f.write(line + '\n')
    except OSError:
        pass


def run(flags, extra, deadline_s=1800):
    cmd = [sys.executable, str(HERE / 'stress.py')] + extra + (['--flags', flags] if flags else [])
    end = time.time() + deadline_s
    while time.time() < end:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=1200)
        out = p.stdout + p.stderr
        if 'not running' in out and 'lock' in out:
            time.sleep(12); continue
        for line in reversed(out.splitlines()):
            if line.startswith('stress: result '):
                return line.split('stress: result ', 1)[1].strip(), out
        return None, out
    return None, 'lock never free'


def figures(r):
    a = verdict.atlas(r)
    h = [x for x in (r.get('hero') or {}).values() if x.get('fps')]
    if h: a['heroFps'] = max(x['fps'] for x in h); a['heroMbps'] = round(sum(x.get('mbps') or 0 for x in h), 1)
    g = verdict.game(r)
    if g: a.update({'gameLat99': g['lat99'], 'gameHost': g['host'], 'gameFps': g['fps']})
    return a


def median(vals):
    v = [x for x in vals if isinstance(x, (int, float))]
    return round(statistics.median(v), 1) if v else None


def main():
    argv = sys.argv[1:]
    extra = argv[argv.index('--') + 1:] if '--' in argv else []
    argv = argv[:argv.index('--')] if '--' in argv else argv
    ap = argparse.ArgumentParser()
    ap.add_argument('--block', required=True); ap.add_argument('--rounds', type=int, default=3)
    ap.add_argument('--configs', required=True, help='name=flags,... (base= for no flags)')
    ap.add_argument('--pairs', required=True, help='rule:on:off,... (verdict.py rule, config names)')
    a = ap.parse_args(argv)
    # configs: "base=,longgop=longgop,vthero=vt+hero" (+ joins flags inside one config)
    configs = [(c.split('=', 1)[0], c.split('=', 1)[1].replace('+', ',')) for c in a.configs.split(',')]
    pairs = [tuple(p.split(':')) for p in a.pairs.split(',')]
    names = [n for n, _ in configs]
    for rule, on, off in pairs:
        if on not in names or off not in names: sys.exit(f'pair {rule}:{on}:{off} names an unknown config')
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%d-%H%M')
    log(f'campaign[{os.getpid()}] START block {a.block}: {a.rounds} rounds x {len(configs)} configs ({", ".join(names)}); stress {" ".join(extra)}')
    results = {n: [None] * a.rounds for n in names}
    for r in range(a.rounds):
        order = configs[r % len(configs):] + configs[:r % len(configs)]
        for name, flags in order:
            path, out = run(flags, extra)
            if not path:
                log(f'campaign block {a.block} round {r + 1} {name}: no result, retrying once ({out.strip().splitlines()[-1] if out.strip() else "no output"})')
                path, out = run(flags, extra)
            results[name][r] = path
            print(f'round {r + 1} {name}: {path}', flush=True)
    load = lambda p: json.load(open(p)) if p and os.path.exists(p) else None
    report = {'block': a.block, 'stress': extra, 'configs': dict(configs), 'runs': results, 'medians': {}, 'verdicts': {}}
    for name in names:
        figs = [figures(x) for x in map(load, results[name]) if x]
        keys = sorted({k for f in figs for k in f})
        report['medians'][name] = {k: median([f.get(k) for f in figs]) for k in keys}
    for rule, on, off in pairs:
        per = []
        for r in range(a.rounds):
            A, B = load(results[off][r]), load(results[on][r])
            per.append(verdict.verdict(rule, A, B) if A and B else ('RERUN', 'missing run'))
        votes = [v for v, _ in per]
        final = 'KEEP' if votes.count('KEEP') >= 2 else 'DROP' if votes.count('DROP') >= 2 else 'INCONCLUSIVE'
        report['verdicts'][f'{rule} ({on} vs {off})'] = {'verdict': final, 'rounds': [{'verdict': v, 'why': w} for v, w in per]}
    res = HERE / 'results'
    (res / f'campaign-{a.block}-{stamp}.json').write_text(json.dumps(report, indent=1))
    md = [f'# Flag campaign, block {a.block} ({stamp} UTC)', '', f'stress.py {" ".join(extra)}; {a.rounds} rounds, order rotated per round.', '',
          '| config | flags | ' + ' | '.join(['fresh fps', 'gap p99 ms', 'mbps', 'keyframes', 'sender cpu %', 'hero fps']) + ' |', '|---|---|---|---|---|---|---|---|']
    for name, flags in configs:
        m = report['medians'][name]
        md.append(f'| {name} | {flags or "(none)"} | ' + ' | '.join(str(m.get(k)) for k in ('fps', 'gapP99', 'mbps', 'keyframes', 'cpu', 'heroFps')) + ' |')
    md += ['', '| flag (rule) | verdict | rounds |', '|---|---|---|']
    for k, v in report['verdicts'].items():
        md.append(f'| {k} | **{v["verdict"]}** | ' + '<br>'.join(f'{x["verdict"]}: {x["why"]}' for x in v['rounds']) + ' |')
    (res / f'campaign-{a.block}-{stamp}.md').write_text('\n'.join(md) + '\n')
    log(f'campaign[{os.getpid()}] END block {a.block}: ' + ', '.join(f'{k.split(" ")[0]} {v["verdict"]}' for k, v in report['verdicts'].items())
        + f'; bench/results/campaign-{a.block}-{stamp}.md')
    print('\n'.join(md))


if __name__ == '__main__':
    main()
