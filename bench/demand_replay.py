#!/usr/bin/env python3
"""Replay the stand-in's gaze traces through models of the sender's layout logic, v1 and vt, and count what costs
keyframes: layout changes (each one a resize + keyframe on the sender) per minute, and atlas size.

    python3 bench/demand_replay.py [--scenario ring|cinema|wall|vr] [--n 10] [--secs 60] [--sweep 3]

Demand: bench/stress/headset_sim.py demand() at 5 Hz (what the suite sends). Sender model, per AtlasSender.swift:
refresh pass every 2 s; v1 tiers 480/640/960/1280/1920 from px, at most one refit per 5 s, growing after 2 s and
shrinking after 5 s of wanting it; vt: the headset's level rule (headset_sim V2), growth within 1 s, shrink at the
5 s gate, hidden-for-2-s screens as mip tails, packing by vt_check.layout_vt; its layout key also includes each
screen's live/tail state and priority quantised to 1/20 (Int(pri*20)), as the Swift key does.
"""
import argparse, math, pathlib, sys
HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE)); sys.path.insert(0, str(HERE / 'stress'))
import headset_sim, vt_check

TIERS = [480, 640, 960, 1280, 1920]


def run(scenario, n, secs, sweep, vt, pri_in_key=True, tail_after=2.0, gate_all=False):
    entries = [{'screen': k + 2, 'w': 1920, 'h': 1200} for k in range(n)]
    tier, want, hidden_at, last_refit, key, changes, areas = {}, {}, {}, -1e9, None, [], []
    v2 = headset_sim.V2()
    t = 0.0
    while t < secs:
        d = headset_sim.demand(n, sweep, t, scenario)
        if vt: v2.apply(d, t)
        for s in d:  # demand side (demand(), every datagram)
            sid = s['id']
            if s['vis']: hidden_at.pop(sid, None)
            else: hidden_at.setdefault(sid, t)
            if not s['vis']: continue
            if vt: target = max(16, 1920 >> min(s['lvl'], 8))
            else: target = min(next((x for x in TIERS if x >= s['px']), 1920), 1920)
            cur = tier.get(sid, 1920)
            if target == cur: want.pop(sid, None)
            elif want.get(sid, (None,))[0] != target: want[sid] = (target, t)
        if abs(t / 2 - round(t / 2)) < 1e-6:  # refresh pass every 2 s
            refit = False
            for sid, (w, since) in list(want.items()):
                grow = w > tier.get(sid, 1920)
                due = ((t - last_refit >= 1) if grow else (t - last_refit >= 5)) if vt else (t - last_refit >= 5 and t - since > (2 if grow else 5))
                if due: tier[sid] = w; want.pop(sid); refit = True
            if refit: last_refit = t
            live = {e['screen'] for e in entries if e['screen'] not in hidden_at or t - hidden_at[e['screen']] < tail_after}
            pri = {s['id']: (max(0.05, s['pri']) if s['vis'] else 0.05) for s in d}
            k = tuple(sorted(tier.items()))
            if vt: k += tuple(sorted((sid, sid in live, int(pri[sid] * 20) if pri_in_key else 0) for sid in pri))
            if k != key and gate_all and not refit and key is not None and t - last_refit < 5: pass  # wait for the refit gate
            elif k != key:
                if gate_all and key is not None: last_refit = t
                changes.append(round(t, 1)); key = k
                if vt:
                    L = vt_check.layout_vt(entries, tier, live, pri)
                    areas.append(L['atlas'][0] * L['atlas'][1] if L else None)
                else:
                    areas.append(sum(tier.get(e['screen'], 1920) * tier.get(e['screen'], 1920) * 1200 // 1920 for e in entries))
        t = round(t + 0.2, 3)
    return changes, areas


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--scenario', default='ring'); ap.add_argument('--n', type=int, default=10)
    ap.add_argument('--secs', type=float, default=60); ap.add_argument('--sweep', type=float, default=3)
    a = ap.parse_args()
    variants = [('v1', False, True, 2, False), ('vt', True, True, 2, False), ('vt, pri not in key', True, False, 2, False),
                ('vt, tail after 10 s', True, False, 10, False), ('vt, 5 s gate', True, False, 2, True), ('vt, 10 s tail + gate', True, False, 10, True)]
    for name, vt, pk, ta, ga in variants:
        ch, ar = run(a.scenario, a.n, a.secs, a.sweep, vt, pk, ta, ga)
        per_min = (len(ch) - 1) * 60 / a.secs  # the first is the initial layout
        mpx = [x / 1e6 for x in ar if x]
        print(f'{a.scenario}-{a.n} {name:20s} layout changes {per_min:5.1f}/min  atlas Mpx mean {sum(mpx)/max(1,len(mpx)):5.2f} max {max(mpx or [0]):5.2f}  at t={ch[:12]}{"..." if len(ch) > 12 else ""}')


if __name__ == '__main__':
    main()
