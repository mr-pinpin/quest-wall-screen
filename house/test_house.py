"""House file tests: schema, migration, and the sync server end to end (a real server on a free port, in a temp git
repo). Hardware-free; part of bench/run_offline_tests.sh.   python3 house/test_house.py
"""
import copy
import hashlib
import http.client
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import migrate  # noqa: E402
import schema  # noqa: E402
import server  # noqa: E402

# Pulled from the headset at 2026-11-03 22:11 UTC (work-c4): test fixtures only.
PLACEMENT = {"wall": "851ff933da164cab35f7293e373d2105", "x": -0.465022, "y": 0.243729, "w": 1.96431, "h": 1.2277}
SCREENS = {"next": 4, "screens": [
    {"id": 2, "wall": "df0486f4f8c1f48e728b8a1aac628737", "x": -0.229414, "y": 0.335872, "w": 1.05222, "h": 0.657638},
    {"id": 3, "wall": "488e679d78667a5f018f7cfefad3e0e2", "x": -0.40869, "y": -0.11862, "w": 0.680491, "h": 0.425307}]}
SETTINGS = {"locked": 0, "menuSaved": 1, "menuWall": "851ff933da164cab35f7293e373d2105",
            "menu": [0.947889, 0.156052, 5.09987, -1.00972e-09, -0.915635, -1.0401e-08, 0.402011]}


def fixture():
    d = migrate.build(PLACEMENT, SCREENS, SETTINGS, when='2026-11-04T00:00:00Z')
    d['by'] = 'test'
    return d


class Schema(unittest.TestCase):
    def test_empty_and_fixture_valid(self):
        self.assertEqual(schema.validate(schema.empty()), [])
        self.assertEqual(schema.validate(fixture()), [])

    def test_rejects(self):
        cases = {
            'dangling wall': lambda d: d['screens'][0].__setitem__('wall', 'nope'),
            'two mains': lambda d: d['screens'][1].__setitem__('role', 'main'),
            'main not id 1': lambda d: d['screens'][0].__setitem__('id', 7),
            'dup screen id': lambda d: d['screens'][2].__setitem__('id', 2),
            'nextScreenId low': lambda d: d.__setitem__('nextScreenId', 3),
            'NaN': lambda d: d['screens'][0].__setitem__('x', float('nan')),
            'negative size': lambda d: d['screens'][0].__setitem__('w', -1),
            'bad uuid': lambda d: d['walls'][0]['scene'].__setitem__(0, 'XYZ'),
            'uuid on two walls': lambda d: d['walls'][1]['scene'].append(d['walls'][0]['scene'][0]),
            'circle without r': lambda d: d['shapes'].append({'id': 's1', 'wall': d['walls'][0]['id'], 'type': 'circle', 'pts': [[0, 0]]}),
            'skin without depth': lambda d: d['shapes'].append({'id': 's1', 'wall': d['walls'][0]['id'], 'type': 'skin', 'pts': [[0, 0], [1, 0]]}),
            'game host': lambda d: d['screens'][0].__setitem__('source', {'kind': 'game', 'host': 'TV'}),
            'on bool': lambda d: d['screens'][0].__setitem__('on', True),
            'room anchor no pose': lambda d: d['rooms'][0].__setitem__('anchors', [{'uuid': '0' * 32}]),
            'link unknown room': lambda d: d['links'].append({'a': 'r1', 'b': 'r9', 'pose': {'p': [0, 0, 0], 'q': [0, 0, 0, 1]}}),
            'shoulder side': lambda d: d['menu'].__setitem__('shoulder', {'side': 'up'}),
            'toggle not bool': lambda d: d['settings'].__setitem__('toggles', {'VT': 1}),
            'schema 99': lambda d: d.__setitem__('schema', 99),
            'rooms not list': lambda d: d.__setitem__('rooms', {}),
            'apps not list': lambda d: d.__setitem__('apps', {}),
            'app screen str': lambda d: d.__setitem__('apps', [{'screen': '2', 'app': 'recipes'}]),
            'app id bad': lambda d: d.__setitem__('apps', [{'screen': 2, 'app': '../x'}]),
            'app screen dup': lambda d: d.__setitem__('apps', [{'screen': 2, 'app': 'a'}, {'screen': 2, 'app': 'b'}]),
        }
        for name, f in cases.items():
            d = fixture(); f(d)
            self.assertTrue(schema.validate(d), f'{name}: accepted')

    def test_full_featured_valid(self):
        d = fixture()
        w0 = d['walls'][0]['id']
        d['rooms'][0]['anchors'] = [{'uuid': 'a' * 32, 'pose': {'p': [0, 0, 0], 'q': [0, 0, 0, 1]}}]
        d['rooms'].append({'id': 'r2', 'name': 'kitchen', 'height': 2.6, 'floor': [[0, 0], [3, 0], [3, 2]]})
        d['links'].append({'a': 'r1', 'b': 'r2', 'pose': {'p': [4, 0, 0], 'q': [0, 0.7071, 0, 0.7071]}})
        d['walls'].append({'id': 'w-depth1', 'room': 'r2', 'kind': 'wall', 'source': 'depth', 'scene': [], 'w': 3, 'h': 2.5,
                           'pose': {'p': [1, 1.2, -1], 'q': [0, 0, 0, 1]}, 'outline': [[-1.5, -1.25], [1.5, -1.25], [1.5, 1.25]],
                           'fit': {'rms_m': 0.004, 'samples': 1800, 'updated': '2026-11-04T00:00:00Z'},
                           'anchors': [{'uuid': 'b' * 32, 'pose': {'p': [0, 0, 0], 'q': [0, 0, 0, 1]}}]})
        d['shapes'] += [{'id': 's1', 'wall': w0, 'type': 'line', 'pts': [[0, 0], [1, 0], [1, 1]], 'dashed': True, 'color': '#ffcc00'},
                        {'id': 's2', 'wall': w0, 'type': 'circle', 'pts': [[0.2, 0.3]], 'r': 0.25},
                        {'id': 's3', 'wall': 'w-depth1', 'type': 'skin', 'pts': [[0, 0], [1, 0], [1, 1]], 'closed': True, 'depth': 0.05}]
        d['screens'][1]['on'] = 0
        d['screens'][1]['world'] = {'p': [1, 1.5, -2], 'q': [0, 0, 0, 1]}
        d['menu']['shoulder'] = {'side': 'left', 'reach_m': 1.0, 'step_m': 1.0}
        d['settings']['toggles'] = {'VT': True, 'SHOULDER': False}
        d['apps'] = [{'screen': 2, 'app': 'recipes'}, {'screen': 9, 'app': 'weather'}]  # a screen not placed yet is fine
        d['by'] = 'apps'
        self.assertEqual(schema.validate(d), [])


class Migration(unittest.TestCase):
    def test_nothing_lost(self):
        d = fixture()
        self.assertEqual(json.loads(schema.legacy_placement(d)), PLACEMENT)
        self.assertEqual(json.loads(schema.legacy_screens(d)), SCREENS)
        st = json.loads(schema.legacy_settings(d))
        for k in ('locked', 'menuSaved', 'menuWall'):
            self.assertEqual(st[k], SETTINGS[k], k)
        for a, b in zip(st['menu'], SETTINGS['menu']):
            self.assertAlmostEqual(a, b, places=12)
        self.assertEqual(len(d['walls']), 3)  # menu wall = main screen wall: one wall, not two
        self.assertEqual([s['source'] for s in d['screens'][1:]], [{'kind': 'mac-display', 'screen': 2}, {'kind': 'mac-display', 'screen': 3}])

    def test_legacy_parses_like_room_lab(self):
        # room_lab's loaders are sscanf on a fixed key order; check the exact prefixes it scans
        d = fixture()
        self.assertTrue(schema.legacy_placement(d).startswith('{"wall":"851ff933da164cab35f7293e373d2105","x":'))
        self.assertTrue(schema.legacy_screens(d).startswith('{"next":4,"screens":[{"id":2,"wall":"'))

    def test_scene_walls_fill_extents(self):
        walls = [{'uuid': PLACEMENT['wall'], 'label': 'WALL_FACE', 'w': 4.1, 'h': 2.6}]
        d = migrate.build(PLACEMENT, SCREENS, SETTINGS, walls)
        w = [w for w in d['walls'] if w['scene'] == [PLACEMENT['wall']]][0]
        self.assertEqual((w['kind'], w['w'], w['h']), ('wall', 4.1, 2.6))

    def test_all_scene_planes_become_walls(self):
        walls = {'walls': [{'uuid': PLACEMENT['wall'], 'label': 'WALL_FACE', 'w': 4.1, 'h': 2.6},
                           {'uuid': 'c' * 32, 'label': 'FLOOR', 'w': 5, 'h': 4},
                           {'uuid': 'd' * 32, 'label': 'GLOBAL_MESH'}]}
        d = migrate.build(PLACEMENT, SCREENS, SETTINGS, walls)
        self.assertEqual(schema.validate(d), [])
        kinds = {w['scene'][0]: w['kind'] for w in d['walls']}
        self.assertEqual(kinds.get('c' * 32), 'floor')
        self.assertNotIn('d' * 32, kinds)
        self.assertEqual(len(d['walls']), 4)

    def test_missing_files(self):
        self.assertEqual(schema.validate(migrate.build()), [])


class FloorPlan(unittest.TestCase):
    def test_box_room(self):
        import floorplan
        d = fixture()
        # four walls of a 4 x 4.5 m room facing inward, pose = rect centre; one wall keeps no pose
        def q_yaw(deg):
            import math
            return [0, math.sin(math.radians(deg) / 2), 0, math.cos(math.radians(deg) / 2)]
        poses = [([0, 1.25, -2.25], q_yaw(0)), ([0, 1.25, 2.25], q_yaw(180)), ([-2, 1.25, 0], q_yaw(90))]
        for w, (p, q) in zip(d['walls'], poses):
            w['pose'] = {'p': p, 'q': q}; w['w'], w['h'] = 4.0, 2.5
        d['walls'].append({'id': 'w-nopose', 'room': 'r1', 'kind': 'wall', 'source': 'scene', 'scene': ['e' * 32]})
        self.assertEqual(schema.validate(d), [])
        svg = floorplan.plan(d)
        self.assertEqual(svg.count('class="wall"'), 3)
        self.assertEqual(svg.count('class="normal"'), 3)
        self.assertEqual(svg.count('class="screen"'), 3)
        self.assertIn('1 without (no pose yet)', svg)
        # the left wall (yaw 90: normal +x) is drawn vertical on the page, its normal tick pointing right
        import re
        lines = re.findall(r'class="(wall|normal)" x1="([\d.]+)" y1="([\d.]+)" x2="([\d.]+)" y2="([\d.]+)"', svg)
        walls = [l for l in lines if l[0] == 'wall']; normals = [l for l in lines if l[0] == 'normal']
        self.assertAlmostEqual(float(walls[2][1]), float(walls[2][3]), delta=0.5)
        self.assertGreater(float(normals[2][3]), float(normals[2][1]))
        self.assertTrue(svg.lstrip().startswith('<svg') and svg.rstrip().endswith('</svg>'))


class Server(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix='house-test-')
        self.start()

    def start(self):
        self.srv = server.serve(self.dir, 0, '127.0.0.1')
        self.port = self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def stop(self):
        self.srv.shutdown(); self.srv.server_close()

    def tearDown(self):
        self.stop()
        shutil.rmtree(self.dir, ignore_errors=True)

    def req(self, method, path, body=None, headers=None):
        c = http.client.HTTPConnection('127.0.0.1', self.port, timeout=10)
        c.request(method, path, body=body, headers=headers or {})
        r = c.getresponse(); data = r.read(); c.close()
        return r.status, dict(r.getheaders()), data

    def get(self, path='/house'):
        st, h, data = self.req('GET', path)
        self.assertEqual(st, 200, data)
        return json.loads(data), h, data

    def put(self, doc, rev, raw=None, digest=None, extra=None):
        body = raw if raw is not None else json.dumps(doc).encode()
        h = {'If-Match': str(rev), 'X-House-SHA256': digest or hashlib.sha256(body).hexdigest(), 'Content-Type': 'application/json'}
        h.update(extra or {})
        st, hh, data = self.req('PUT', '/house', body, h)
        return st, json.loads(data) if data else None

    def commits(self):
        return int(subprocess.run(['git', 'rev-list', '--count', 'HEAD'], cwd=self.dir, capture_output=True, text=True).stdout or 0)

    def seed(self):
        st, r = self.put(fixture(), 0)
        self.assertEqual(st, 200, r)
        return r['rev']

    def test_round_trip(self):
        doc, h, raw = self.get()
        self.assertEqual(doc['rev'], 0)
        self.assertEqual(h['X-House-SHA256'], hashlib.sha256(raw).hexdigest())
        self.seed()
        got, h, _ = self.get()
        want = fixture()
        for k in ('rev', 'updated'):
            got.pop(k); want.pop(k)
        self.assertEqual(got, want)
        self.assertEqual(h['ETag'], '"1"')
        # a GET-edit-PUT cycle
        doc, _, _ = self.get()
        doc['screens'][1]['x'] = 0.5
        st, r = self.put(doc, 1)
        self.assertEqual((st, r['rev']), (200, 2))
        self.assertEqual(self.get()[0]['screens'][1]['x'], 0.5)

    def test_every_write_is_a_commit_and_history(self):
        n0 = self.commits()
        self.seed()
        doc = self.get()[0]; doc['settings']['resume'] = True
        self.assertEqual(self.put(doc, 1, extra={'X-House-Note': 'resume on'})[0], 200)
        self.assertEqual(self.commits(), n0 + 2)
        hist = json.loads(self.req('GET', '/house/history')[2])
        self.assertEqual([h['rev'] for h in hist[:3]], [2, 1, 0])
        self.assertEqual(hist[0]['note'], 'resume on')
        self.assertEqual(json.loads(self.req('GET', '/house/rev/1')[2])['settings'].get('resume'), None)

    def test_corrupt_writes_rejected(self):
        self.seed()
        before, n0 = self.get()[2], self.commits()
        body = json.dumps(fixture()).encode()
        # wrong digest, truncated JSON with its own correct digest, invalid house, no digest, no If-Match
        self.assertEqual(self.put(None, 1, raw=body, digest='0' * 64)[0], 400)
        self.assertEqual(self.put(None, 1, raw=body[:len(body) // 2])[0], 400)
        bad = fixture(); bad['screens'][0]['wall'] = 'nope'
        st, r = self.put(bad, 1)
        self.assertEqual(st, 400); self.assertIn('problems', r)
        self.assertEqual(self.req('PUT', '/house', body, {'If-Match': '1'})[0], 400)
        self.assertEqual(self.req('PUT', '/house', body, {'X-House-SHA256': hashlib.sha256(body).hexdigest()})[0], 428)
        self.assertEqual(self.put(None, 1, raw=b'\xff\xfe not json')[0], 400)
        # a body cut off mid-upload (connection closed after half the declared bytes)
        s = socket.create_connection(('127.0.0.1', self.port))
        s.sendall(b'PUT /house HTTP/1.1\r\nHost: x\r\nIf-Match: 1\r\nX-House-SHA256: ' + hashlib.sha256(body).hexdigest().encode() +
                  b'\r\nContent-Length: ' + str(len(body)).encode() + b'\r\n\r\n' + body[:len(body) // 2])
        s.shutdown(socket.SHUT_WR)
        reply = s.recv(4096); s.close()
        self.assertIn(b' 400 ', reply.split(b'\r\n')[0] + b' ')
        self.assertEqual(self.get()[2], before)
        self.assertEqual(self.commits(), n0)

    def test_conflict_never_overwrites(self):
        self.seed()
        a = self.get()[0]; b = self.get()[0]
        a['screens'][1]['x'] = 1.0
        self.assertEqual(self.put(a, 1)[0], 200)
        b['screens'][2]['x'] = 2.0
        st, cur = self.put(b, 1)  # stale: based on 1, current is 2
        self.assertEqual(st, 409)
        self.assertEqual(cur['rev'], 2); self.assertEqual(cur['screens'][1]['x'], 1.0)
        cur['screens'][2]['x'] = 2.0  # the client re-applies its edit to the current house
        self.assertEqual(self.put(cur, 2)[0], 200)
        d = self.get()[0]
        self.assertEqual((d['screens'][1]['x'], d['screens'][2]['x']), (1.0, 2.0))

    def test_disk_corruption_restores_last_good(self):
        self.seed()
        good = self.get()[0]
        self.stop()
        with open(os.path.join(self.dir, 'house.json'), 'wb') as f:
            f.write(b'{"schema":1,"rev":1,"walls":[{"id"')  # e.g. power cut mid-write by something else
        self.start()
        got = self.get()[0]
        self.assertEqual(got['screens'], good['screens'])
        with open(os.path.join(self.dir, 'house.json')) as f:
            self.assertEqual(json.load(f)['screens'], good['screens'])
        self.stop()
        os.remove(os.path.join(self.dir, 'house.json'))
        self.start()
        self.assertEqual(self.get()[0]['screens'], good['screens'])

    def test_no_temp_files_left(self):
        self.seed()
        self.assertEqual([f for f in os.listdir(self.dir) if '.tmp-' in f], [])

    def test_rollback(self):
        self.seed()
        d = self.get()[0]; d['screens'] = d['screens'][:1]; d['nextScreenId'] = 4
        self.assertEqual(self.put(d, 1)[0], 200)
        self.assertEqual(self.req('POST', '/house/rollback?rev=1', headers={'If-Match': '1'})[0], 409)
        st, _, data = self.req('POST', '/house/rollback?rev=1', headers={'If-Match': '2'})
        self.assertEqual((st, json.loads(data)['rev']), (200, 3))
        got = self.get()[0]
        self.assertEqual((got['by'], len(got['screens'])), ('rollback', 3))

    def test_blobs(self):
        self.seed()
        mesh = os.urandom(5000); h = hashlib.sha256(mesh).hexdigest()
        d = self.get()[0]; d['walls'][0]['mesh'] = h
        self.assertEqual(self.put(d, 1)[0], 400)  # mesh not uploaded yet
        self.assertEqual(self.req('PUT', '/blob/' + '0' * 64, mesh)[0], 400)
        self.assertEqual(self.req('PUT', '/blob/' + h, mesh)[0], 200)
        self.assertEqual(self.put(d, 1)[0], 200)
        self.assertEqual(self.req('GET', '/blob/' + h)[2], mesh)

    def test_legacy_endpoints(self):
        self.seed()
        st, h, svg = self.req('GET', '/house/plan.svg')
        self.assertEqual((st, h['Content-Type']), (200, 'image/svg+xml'))
        self.assertIn(b'</svg>', svg)
        self.assertEqual(json.loads(self.req('GET', '/legacy/placement.json')[2]), PLACEMENT)
        self.assertEqual(json.loads(self.req('GET', '/legacy/screens.json')[2]), SCREENS)

    def test_old_schema_reads_and_writes(self):
        # A schema 2 that adds wall "labels" and a top-level "notes"; schema-1 readers must still work, and a
        # schema-1 write must not erase what only schema 2 holds.
        def up(d):
            d = copy.deepcopy(d); d['notes'] = ''
            for w in d['walls']:
                w['labels'] = []
            return d

        def down(d):
            d = copy.deepcopy(d); d.pop('notes', None)
            for w in d['walls']:
                w.pop('labels', None)
            return d
        old_validate = schema.validate

        def validate2(doc, blobs=None):
            if not isinstance(doc, dict) or doc.get('schema') != 2:
                return ['schema: not 2']
            schema.CURRENT = 1  # the schema-1 rules for everything schema 2 shares with it
            try:
                return old_validate(dict(down(doc), schema=1), blobs)
            finally:
                schema.CURRENT = 2
        self.seed()
        schema.UP[1], schema.DOWN[1], schema.CURRENT, schema.validate = up, down, 2, validate2
        try:
            d = self.get()[0]
            d = up(d); d['schema'] = 2; d['notes'] = 'hall light'; d['walls'][0]['labels'] = ['tv wall']
            self.assertEqual(self.put(d, 1)[0], 200)
            # old reader
            v1, h, _ = self.get('/house?schema=1')
            self.assertEqual((v1['schema'], h['X-House-Schema']), (1, '1'))
            self.assertNotIn('notes', v1); self.assertNotIn('labels', v1['walls'][0])
            self.assertEqual(self.req('GET', '/house?schema=0')[0], 406)
            self.assertEqual(self.req('GET', '/house?schema=3')[0], 406)
            # old writer edits a screen; schema-2 data survives
            v1['screens'][1]['x'] = 0.75
            st, r = self.put(v1, 2)
            self.assertEqual(st, 200, r)
            v2 = self.get()[0]
            self.assertEqual((v2['schema'], v2['notes'], v2['walls'][0]['labels'], v2['screens'][1]['x']), (2, 'hall light', ['tv wall'], 0.75))
            # a stale old writer gets the 409 in its own schema
            st, cur = self.put(v1, 2)
            self.assertEqual((st, cur['schema']), (409, 1))
        finally:
            schema.UP.clear(); schema.DOWN.clear(); schema.CURRENT = 1; schema.validate = old_validate


if __name__ == '__main__':
    r = unittest.main(exit=False, verbosity=1).result
    n = r.testsRun
    print(f'{n} house tests, {len(r.failures) + len(r.errors)} failed')
    sys.exit(0 if r.wasSuccessful() else 1)
