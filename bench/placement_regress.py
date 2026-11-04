#!/usr/bin/env python3
"""Placement regression on the headset: where the user placed things, they stay there, no matter what.

    python3 bench/placement_regress.py [--cases restart,pending,hidden] [--shots]

For each case the app is restarted (force-stop + start; no prox_close) and status.json is polled every 0.5 s:
  restart  plain app restart
  pending  files/scene-test {"ignoreFirstS": 12}: the scene answers nothing for 12 s (relocalisation pending)
  hidden   files/scene-test {"hide": [main wall]}: Meta no longer has the main screen's wall (uuids changed)
Pass = every placed screen drawn (source scene or cache) within 3 s of the app being up, each within 3 cm of its
baseline STAGE spot, and still there at the end (pending: the real wall takes over by ~14 s; hidden: the main screen
stays on its stand-in). --shots adds a screencap at +3 s (the display must be on: worn, or on the charger by day).
Needs headset_owner=agents and takes the install lock. Results: bench/results/placement-regress-<ts>.json.
"""
import json, math, os, subprocess, sys, time
sys.path.insert(0, '/root/work/quest-wall-screen/bench')  # runtime files (headset_owner, activity.log) live there
import stress
P = stress.PKG
H = lambda c, t=40: stress.headset(c, timeout=t)
RES = '/root/work/quest-wall-screen/bench/results'

def status():
    try: return json.loads(H(f'run-as {P} cat files/status.json'))
    except ValueError: return {}

def screens(s):
    return {x['id']: x for x in (s.get('placement') or {}).get('screens', [])}

def restart(scene_test=None):
    if scene_test is None: H(f'run-as {P} rm -f files/scene-test')
    else: H(f"run-as {P} sh -c 'echo {json.dumps(json.dumps(scene_test))[1:-1]} > files/scene-test'")
    H(f'am force-stop {P}'); H(f'am start -n {P}/.MainActivity --ez capture false')
    t0 = time.time()
    while time.time() - t0 < 20:  # the new process is up when its uptime restarts
        s = status()
        if s and s.get('uptime', 99) < 15 and s.get('state') in (4, 5): return s.get('uptime', 0), time.time()
        time.sleep(0.3)
    return None, time.time()

def run_case(name, base, scene_test, expect_final, shots, out):
    up, t_up = restart(scene_test)
    r = {'case': name, 'appUp': up is not None, 'samples': []}
    shown_at, worst, ok_final = None, 0.0, False
    shot_done = False
    while time.time() - t_up < (18 if name == 'pending' else 10):
        s = status(); sc = screens(s); t = time.time() - t_up
        drawn = {i: x for i, x in sc.items() if x['source'] in ('scene', 'cache') and 'stage' in x}
        errs = {i: math.dist(drawn[i]['stage'], base[i]['stage']) for i in drawn if i in base}
        r['samples'].append({'t': round(t, 2), 'sources': {i: x['source'] for i, x in sc.items()}, 'err': {i: round(e, 3) for i, e in errs.items()}})
        if shown_at is None and set(base) <= set(drawn): shown_at = t
        if errs: worst = max(worst, max(errs.values()))
        if shots and not shot_done and t >= 3:
            stress.mini(f'{stress.ADB} exec-out screencap -p > /tmp/pp-reg-{name}.png', timeout=60)
            subprocess.run(['scp', '-q', f'pinpin-mini:/tmp/pp-reg-{name}.png', f'{out}-{name}.png'], timeout=60); shot_done = True
        time.sleep(0.5)
    final = screens(status())
    ok_final = all(final.get(i, {}).get('source') == expect_final.get(i, 'scene') for i in base)
    r.update({'shownAt': shown_at, 'worstErrM': round(worst, 3), 'final': {i: x['source'] for i, x in final.items()},
              'pass': up is not None and shown_at is not None and shown_at <= 3.0 and worst <= 0.03 and ok_final})
    print(f"{name:8} {'PASS' if r['pass'] else 'FAIL'}  shown at {shown_at}s  worst {worst*100:.1f} cm  final {r['final']}")
    return r

def main():
    cases = (sys.argv[sys.argv.index('--cases') + 1] if '--cases' in sys.argv else 'restart,pending,hidden').split(',')
    shots = '--shots' in sys.argv
    if stress.headset_owner() != 'agents': sys.exit('headset_owner is not agents')
    out = f"{RES}/placement-regress-{time.strftime('%Y%m%dT%H%M%SZ', time.gmtime())}"
    with stress.install_lock('placement_regress'):
        stress.activity(f'START placement regression ({",".join(cases)}; app restarts, no prox)')
        try:
            s = status(); base = {i: x for i, x in screens(s).items() if x['source'] == 'scene' and 'stage' in x}
            if not base:  # not settled yet: one plain restart to get a baseline from real walls
                restart(None); time.sleep(6); base = {i: x for i, x in screens(status()).items() if x['source'] == 'scene' and 'stage' in x}
            print('baseline', {i: x['stage'] for i, x in base.items()})
            if not base: sys.exit('no screen on a located real wall: no baseline')
            time.sleep(3)  # let the app write files/lastwalls.json from the real walls
            main_wall = next((x['wall'] for i, x in base.items() if i == 1), None)
            results = []
            for c in cases:
                if c == 'restart': results.append(run_case(c, base, None, {}, shots, out))
                elif c == 'pending': results.append(run_case(c, base, {'ignoreFirstS': 12}, {}, shots, out))
                elif c == 'hidden':
                    full = next((e['uuid'] for e in (status().get('scene') or {}).get('entities', []) if main_wall and e['uuid'].startswith(main_wall)), None)
                    results.append(run_case(c, base, {'hide': [full] if full else []}, {1: 'cache'}, shots, out))
        finally:
            restart(None)  # leave the app clean: no scene-test
            stress.activity('END placement regression')
    json.dump({'baseline': base, 'results': results}, open(out + '.json', 'w'), indent=1)
    allpass = all(r['pass'] for r in results)
    print('ALL PASS' if allpass else 'FAILURES', '->', out + '.json')
    sys.exit(0 if allpass else 1)

if __name__ == '__main__':
    main()
