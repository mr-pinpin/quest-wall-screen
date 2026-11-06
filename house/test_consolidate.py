"""House consolidation tests (house/consolidate.py): a synthetic two-room flat with every kind of scan noise, the
real house at rev 25 (as the user's scans left it, 2026-11-04) and a real depth dump (wall-facing, 2026-11-04 08:44
UTC) next to Meta's walls in the same frame; plus the server storing the plan layer. Hardware-free; part of
bench/run_offline_tests.sh.   python3 house/test_consolidate.py
"""
import copy
import json
import math
import os
import random
import sys
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import consolidate as C  # noqa: E402
import floorplan  # noqa: E402
import schema  # noqa: E402

TH = math.radians(23)  # the flat's directions in the room frame


def world(u, v):
    return (u * math.cos(TH) + v * math.sin(TH), -u * math.sin(TH) + v * math.cos(TH))


def wall(wid, src, a, b, n, y=-0.15, h=2.5, dy=0.0, dang=0.0, doff=0.0, batch='t1', samples=30000, rms=0.015, kind='wall'):
    """A vertical wall from (u, v) a to b, facing n (unit (u, v)), as house.json stores it."""
    cu, cv = (a[0] + b[0]) / 2 + n[0] * doff, (a[1] + b[1]) / 2 + n[1] * doff
    x, z = world(cu, cv)
    nx, nz = world(*n)
    yaw = math.atan2(nx, nz) + math.radians(dang)
    w = {'id': wid, 'room': 'r1', 'kind': kind, 'source': src, 'w': math.hypot(b[0] - a[0], b[1] - a[1]), 'h': h,
         'pose': {'p': [x, y + dy, z], 'q': C.yaw_q(yaw)}}
    if src == 'depth':
        w['fit'] = {'rms_m': rms, 'samples': samples, 'updated': batch}
        w['frame'] = {'source': 'scene', 'id': batch}
    return w


def plane(wid, src, u, v, w, h, y, up=True, batch='t1', samples=80000):
    x, z = world(u, v)
    s = math.sqrt(0.5)
    q = [-s, 0, 0, s] if up else [s, 0, 0, s]
    d = {'id': wid, 'room': 'r1', 'kind': 'floor' if up else 'ceiling', 'source': src, 'w': w, 'h': h, 'pose': {'p': [x, y, z], 'q': q}}
    if src == 'depth':
        d['fit'] = {'rms_m': 0.012, 'samples': samples, 'updated': batch}
        d['frame'] = {'source': 'scene', 'id': batch}
    return d


def flat(seed=1, rotated_batch=False, drop_east=False):
    """Room A (Meta): u 0..4, v 0..3.5; room B (scanned): u 4.1..7.1, v 0..3, behind A's east wall (10 cm thick);
    a 0.9 m doorway between them at v 1.0..1.9. Scan noise on B: three looks at every wall (2 commits, the second
    10 cm high), a furniture face, a 9 m line along a wall, a rotated cluster, weak bits, a slanted plane."""
    R = random.Random(seed)
    W = []
    E, N, Wn, S = (1, 0), (0, 1), (-1, 0), (0, -1)
    W += [wall('w-ma-s', 'scene', (0, 0), (4, 0), N), wall('w-ma-n', 'scene', (0, 3.5), (4, 3.5), S),
          wall('w-ma-w', 'scene', (0, 0), (0, 3.5), E), wall('w-ma-e1', 'scene', (4, 0), (4, 1.0), Wn),
          wall('w-ma-e2', 'scene', (4, 1.9), (4, 3.5), Wn)]
    W += [plane('w-ma-floor', 'scene', 2, 1.75, 4, 3.5, -1.40), plane('w-ma-ceil', 'scene', 2, 1.75, 4, 3.5, 1.10, up=False)]
    B = [((4.1, 0), (7.1, 0), N), ((4.1, 3), (7.1, 3), S), ((4.1, 0), (4.1, 1.0), E), ((4.1, 1.9), (4.1, 3), E)]
    if not drop_east:
        B.append(((7.1, 0), (7.1, 3), Wn))
    k = 0
    for a, b, n in B:
        for look in range(3):
            batch = 't1' if look < 2 else 't2'
            L = math.hypot(b[0] - a[0], b[1] - a[1]); t = ((b[0] - a[0]) / L, (b[1] - a[1]) / L)
            s0, s1 = R.uniform(-0.05, 0.15), R.uniform(-0.05, 0.15)
            aa, bb = (a[0] + t[0] * s0, a[1] + t[1] * s0), (b[0] - t[0] * s1, b[1] - t[1] * s1)
            W.append(wall(f'w-d{k:02d}', 'depth', aa, bb, n, doff=R.gauss(0, 0.01), dang=R.uniform(-1.5, 1.5), batch=batch,
                          dy=0.10 if batch == 't2' else 0.0, samples=R.randint(20000, 60000), rms=R.uniform(0.01, 0.02)))
            k += 1
    W.append(wall('w-dfurn', 'depth', (6.85, 0.8), (6.85, 2.0), Wn, h=0.8, y=-1.0, samples=9000))       # wardrobe front
    W.append(wall('w-dlong', 'depth', (4.1, -3), (4.1, 6), E, samples=40000, rms=0.025, batch='t2', dy=0.1))  # 9 m line
    W.append(wall('w-drot1', 'depth', (5, 0.8), (6.5, 2.2), (0.68, -0.73), samples=12000))              # rotated
    W.append(wall('w-drot2', 'depth', (5.2, 0.6), (6.6, 2.0), (0.68, -0.73), samples=11000))
    W.append(wall('w-dweak', 'depth', (5, 1.5), (6, 1.5), N, samples=1500, rms=0.03))                    # weak
    W.append(wall('w-dtiny', 'depth', (5.4, 1.2), (5.65, 1.2), S, samples=30000))                        # short
    sl = plane('w-dslant', 'depth', 5.5, 1.5, 1.0, 1.0, -0.9)
    sl['pose']['q'] = [-0.38268, 0, 0, 0.92388]; sl['kind'] = 'plane'; W.append(sl)                     # 45 deg
    W += [plane('w-dfloor1', 'depth', 5.6, 1.5, 2.8, 2.8, -1.40, batch='t1'), plane('w-dfloor2', 'depth', 5.6, 1.5, 2.6, 2.6, -1.30, batch='t2'),
          plane('w-dbed', 'depth', 6.0, 2.0, 1.4, 1.9, -0.95, batch='t1', samples=20000)]
    if rotated_batch:  # a third commit in a frame rotated by 6 deg (a STAGE jump)
        for i, (a, b, n) in enumerate(B[:4]):
            ww = wall(f'w-dj{i}', 'depth', a, b, n, dang=6.0, batch='t3', samples=30000)
            ang = math.radians(6.0); x, z = ww['pose']['p'][0], ww['pose']['p'][2]
            ww['pose']['p'][0], ww['pose']['p'][2] = x * math.cos(ang) + z * math.sin(ang), -x * math.sin(ang) + z * math.cos(ang)
            W.append(ww)
    d = schema.empty(); d['by'] = 'test'; d['rev'] = 7; d['rooms'] = [{'id': 'r1', 'name': 'flat'}]; d['walls'] = W
    return d


def with_frames(doc):
    """The doc with a frame record on every depth wall: the fixtures predate v0.65 and test the geometry."""
    for w in doc['walls']:
        if w.get('source', 'depth') == 'depth':
            w.setdefault('frame', {'source': 'test'})
    return doc


def area(poly):
    return abs(sum(poly[i][0] * poly[(i + 1) % len(poly)][1] - poly[(i + 1) % len(poly)][0] * poly[i][1] for i in range(len(poly)))) / 2


class Synthetic(unittest.TestCase):
    def test_noisy_flat_is_cleaned(self):
        d = flat()
        self.assertEqual(schema.validate(d), [])
        before = copy.deepcopy(d)
        pl = C.consolidate(d)
        self.assertEqual(d, before, 'pure: the raw house is not touched')
        sc = pl['score']
        self.assertEqual(sc['verdict'], 'satisfactory', sc['reasons'])
        self.assertAlmostEqual(pl['axesDeg'], 23.0, delta=0.5)
        self.assertGreaterEqual(sc['duplicates']['raw'], 10)
        self.assertEqual(sc['duplicates']['plan'], 0)
        self.assertEqual(len(pl['rooms']), 2)
        areas = sorted(area(r['floor']) for r in pl['rooms'])
        self.assertAlmostEqual(areas[0], 9.0, delta=0.45)    # room B 3 x 3
        self.assertAlmostEqual(areas[1], 14.0, delta=0.7)    # room A 4 x 3.5
        for r in pl['rooms']:
            self.assertTrue(r['closed'], r)
            self.assertAlmostEqual(r['floorY'], -1.40, delta=0.03)
        doors = [dd['w'] for r in pl['rooms'] for dd in r['doors']]
        self.assertTrue(any(abs(w - 0.9) < 0.12 for w in doors), doors)
        self.assertLessEqual(max(w['w'] for w in pl['walls']), 4.2, 'the 9 m line is cut at its corners')
        why = {x['id']: x['why'] for x in pl['dropped']}
        for wid, word in (('w-drot1', 'off-axis'), ('w-drot2', 'off-axis'), ('w-dweak', 'confidence'), ('w-dslant', 'slanted'),
                          ('w-dfurn', 'in front'), ('w-dtiny', '')):
            self.assertIn(wid, why); self.assertIn(word, why[wid])
        moved = {f['batch']: f for f in pl['frames']}
        self.assertAlmostEqual(moved['t2']['dy'], -0.10, delta=0.01); self.assertTrue(moved['t2']['status'].startswith('moved'))
        b2b = sc['sceneFit']['backToBack']
        self.assertTrue(b2b and all(abs(b['thickness_m'] - 0.10) < 0.04 for b in b2b), b2b)
        self.assertEqual(C.consolidate(d), pl, 'deterministic')
        self.assertEqual(schema.validate(dict(d, plan=pl)), [])

    def test_seeds(self):
        for seed in range(2, 8):
            pl = C.consolidate(flat(seed))
            self.assertEqual(pl['score']['verdict'], 'satisfactory', (seed, pl['score']['reasons']))
            self.assertEqual(len(pl['rooms']), 2, seed)

    def test_rotated_commit_is_dropped(self):
        pl = C.consolidate(flat(rotated_batch=True))
        f = {x['batch']: x for x in pl['frames']}
        self.assertTrue(f['t3']['status'].startswith('dropped: rotated'), f['t3'])
        self.assertEqual(pl['score']['verdict'], 'not yet')
        self.assertTrue(any('another frame' in r for r in pl['score']['reasons']))
        self.assertEqual(len(pl['rooms']), 2, 'the rest still makes the plan')

    def test_open_room_is_not_yet(self):
        pl = C.consolidate(flat(drop_east=True))
        self.assertEqual(pl['score']['verdict'], 'not yet')
        self.assertTrue(any('open' in r or 'bound no room' in r for r in pl['score']['reasons']), pl['score']['reasons'])

    def test_empty_and_odd(self):
        d = schema.empty()
        self.assertEqual(C.consolidate(d)['score']['verdict'], 'not yet')
        d['walls'] = [{'id': 'w-x', 'kind': 'wall', 'source': 'depth'}, {'id': 'w-o', 'kind': 'wall', 'source': 'depth', 'w': 1, 'h': 1,
                                                                         'pose': {'p': [0, 0, 0], 'q': [0, 0, 0, 1]}, 'frame': {'source': 'anchors'}}]
        why = {x['id']: x['why'] for x in C.consolidate(d)['dropped']}
        self.assertIn('no pose', why['w-x']); self.assertIn('unlocated', why['w-o'])


class Real(unittest.TestCase):
    def test_house_rev25(self):
        """The house after the 02:15 and 03:03 UTC scans: 20 noisy depth walls next to Meta's 11."""
        d = with_frames(json.load(open(os.path.join(HERE, 'fixtures', 'house-rev25.json'))))
        t = time.time(); pl = C.consolidate(d); dt = time.time() - t
        print(f'timing consolidate rev 25: {dt * 1000:.0f} ms (info; the bound only catches a runaway)')
        self.assertLess(dt, 15.0)
        sc = pl['score']
        self.assertGreaterEqual(sc['duplicates']['raw'], 30)
        self.assertEqual(sc['duplicates']['plan'], 0)
        self.assertAlmostEqual(pl['axesDeg'], 17.5, delta=1.0)
        self.assertLessEqual(max(w['w'] for w in pl['walls'] if w['src'] == 'depth'), 6.0, 'no 9 m lines')
        self.assertGreaterEqual(len(pl['rooms']), 1)
        self.assertTrue(all(f['status'].startswith('moved') for f in pl['frames']), pl['frames'])  # 6 and 16 cm high
        self.assertEqual(sc['verdict'], 'not yet')  # the scanned room backs onto the TV wall 2 cm away: a frame offset
        self.assertTrue(any('thinner than a real wall' in r for r in sc['reasons']), sc['reasons'])
        self.assertEqual(schema.validate(dict(d, plan=pl)), [])
        svg = floorplan.plan(d, layer='both', clean=pl)
        self.assertIn('plan-wall', svg); self.assertIn('class="wall"', svg)

    def test_frame_suspect(self):
        """consolidate/2: rev 25 as stored (pre-v0.65 depth walls, no frame record): every depth wall is kept in the
        raw layer but left out of the plan, which is Meta's walls alone."""
        d = json.load(open(os.path.join(HERE, 'fixtures', 'house-rev25.json')))
        raw = json.dumps(d, sort_keys=True)
        pl = C.consolidate(d)
        depth = [w['id'] for w in d['walls'] if w.get('source', 'depth') == 'depth']
        self.assertGreaterEqual(len(depth), 20)
        self.assertEqual(pl['score']['frameSuspect'], len(depth))
        self.assertEqual(json.dumps(d, sort_keys=True), raw, 'raw layer untouched')
        why = {x['id']: x['why'] for x in pl['dropped']}
        self.assertTrue(all(why.get(i, '').startswith('frame-suspect') for i in depth))
        self.assertTrue(pl['walls'] and all(w['src'] != 'depth' for w in pl['walls']))
        self.assertIsNone(pl['score']['explained'])
        self.assertEqual(pl['score']['verdict'], 'not yet')  # consolidate/3: unverified space blocks the verdict
        self.assertTrue(any('rescan with v0.65+' in r for r in pl['score']['reasons']), pl['score']['reasons'])
        self.assertEqual(C.consolidate(with_frames(d))['score']['frameSuspect'], 0)

    def test_depth_dump_meets_meta(self):
        """A wall-facing depth dump (both eyes fused by room_lab/wallfit.h, bench/wallfit_replay.cpp --house) next to
        Meta's walls in the same LOCAL frame: the fitted wall lies on Meta's within 2 cm."""
        d = json.load(open(os.path.join(HERE, 'fixtures', 'depth-72504684', 'house.json')))
        pl = C.consolidate(d)
        fit = pl['score']['sceneFit']
        self.assertGreaterEqual(fit['pairs'], 1)
        self.assertLessEqual(fit['median_m'], 0.02)
        self.assertTrue(any(r['area'] > 8 for r in pl['rooms']), 'Meta room found')


if __name__ == '__main__':
    unittest.main(verbosity=1)
