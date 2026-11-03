#!/usr/bin/env python3
"""Medians per gaming arm from repeated runs (bench/gaming_repeats.sh): fps, frame latency p99, stalls > 100 ms (and
their spread), grouped by scenario label (baseline vs each flag).
    python3 bench/gaming_summary.py [--since YYYYmmdd-HHMMSS]"""
import glob, json, pathlib, statistics, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent)); import gate
since = sys.argv[sys.argv.index("--since") + 1] if "--since" in sys.argv else ""
arms = {}
for f in sorted(glob.glob(str(pathlib.Path(__file__).resolve().parent / "results" / "stress-*.json"))):
    stamp = pathlib.Path(f).name[7:22]
    if stamp < since: continue
    r = json.load(open(f))
    if r.get("mode") != "headset": continue
    g = gate.gaming(r)
    if not g: continue
    val = lambda k: next((w for name, (ok, w) in g.items() if name.startswith(k)), "")
    try:
        fps = float(val("fps").split()[0]); p99 = float(val("frame latency p99").split()[0]); st = int(val("no stall").split()[0])
    except (ValueError, IndexError): continue
    net = (r.get("network") or {}).get("label", "ch36 (no epoch recorded)")
    arms.setdefault(f"{net} | {r.get('scenario')}", []).append((fps, p99, st, stamp))
for arm, xs in arms.items():
    med = lambda i: statistics.median(x[i] for x in xs)
    print(f"{arm:50s} n={len(xs)}  fps {med(0):5.1f}  p99 {med(1):5.0f} ms  stalls>100 {med(2):5.0f} (range {min(x[2] for x in xs)}-{max(x[2] for x in xs)})  runs {[x[3] for x in xs]}")
