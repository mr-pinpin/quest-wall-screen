#!/usr/bin/env python3
"""Self-test for bench/gate.py: synthetic results that must pass or fail for the right reason."""
import copy, json, pathlib, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import gate

fails = 0
def check(cond, msg):
    global fails
    if not cond: fails += 1; print('FAIL', msg)

h0 = [0] * 31; h1 = [0] * 31; h1[3] = 3000; h1[4] = 500; h1[9] = 20
good = {'mode': 'headset', 'scenario': 'ring', 'n': 10, 'wan': 0, 'headsetFps': {'min': 72},
        'checks': {'atlas within 40 Mbit/s': True, 'game fps >= 55': True},
        'series': [{'t': 0, 'game': {'displayed': 0, 'latHist': h0, 'stalls50': 0, 'stalls100': 0, 'gapMaxMs': 20, 'rtt': 6, 'hostMs': 4, 'lat': 18}},
                   {'t': 60, 'game': {'displayed': 3550, 'latHist': h1, 'stalls50': 1, 'stalls100': 0, 'gapMaxMs': 60, 'rtt': 6, 'hostMs': 4, 'lat': 18}}]}

def variant(f):
    r = copy.deepcopy(good); f(r); return gate.judge(r)

v = gate.judge(good)
check(v['pass'], f'good run passes: {v}')
check(gate.hist_pct(h1, 50) == 20 and gate.hist_pct(h1, 99) == 25, 'histogram percentiles use bucket upper edges')
check(not variant(lambda r: r['series'][1]['game'].update(stalls100=1))['pass'], 'a 100 ms stall fails')
check(not variant(lambda r: r['series'][1]['game']['latHist'].__setitem__(30, 200))['pass'], 'a long latency tail fails p99')
check(not variant(lambda r: r['series'][1]['game'].update(displayed=2000))['pass'], '33 fps fails')
check(not variant(lambda r: r.update(headsetFps={'min': 60}))['pass'], 'headset at 60 fps fails')
check(not variant(lambda r: r['checks'].update({'atlas within 40 Mbit/s': False}))['pass'], 'a failed screen check fails')
check(variant(lambda r: r['checks'].update({'game fps >= 55': False}))['pass'], "stress.py's own game checks are superseded")
old = variant(lambda r: [x['game'].pop(k) for x in r['series'] for k in ('latHist', 'stalls50', 'stalls100')])
check(old['pass'] and any('approx' in k for k in old['gaming']), 'pre-v0.38 results pass flagged approx')
nogame = copy.deepcopy(good); [x.pop('game') for x in nogame['series']]
check('gaming' not in gate.judge(nogame), 'runs without a game stream have no gaming section')
sim = copy.deepcopy(good); sim['series'] = [1.5, 2.0, 0.3]
check('gaming' not in gate.judge(sim), 'sim runs (numeric series) judge without a gaming section')
# Relative screen rule: fresh fps >= 95% of the source, gap p99 <= 2 source frames; absolute lines informational.
scr = {'scenario': 'cinema', 'captureFps': {'2': 29.4, '3': 29.0},
       'checks': {'stream kept 30+ fresh frames/s (median)': False, 'fresh-frame gap p99 < 50 ms': False, 'sender never crashed': True},
       'atlas': {'freshFps': {'p50': 28.5}, 'freshGapMs': {'p99': 60.0}}}
v = gate.judge(scr)
check(v['pass'] and 'info' in v, f'28.5 of 29.4 fps and 60 ms pass relative; absolute lines informational: {v}')
low = copy.deepcopy(scr); low['atlas']['freshFps']['p50'] = 23
check(not gate.judge(low)['pass'], '23 of 29.4 fps fails (< 95%)')
slow = copy.deepcopy(scr); slow['atlas']['freshGapMs']['p99'] = 80
check(not gate.judge(slow)['pass'], '80 ms > 2 frames at 29.4 fps fails')
old = copy.deepcopy(scr); old.pop('captureFps')
check(any('assumed' in k for k in gate.judge(old)['screens']), 'runs without captureFps use the assumed SCK rate')
print(f'gate self-test: {fails} failures'); sys.exit(1 if fails else 0)
