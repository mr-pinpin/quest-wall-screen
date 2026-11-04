"""Import today's headset files (room_lab files/placement.json, screens.json, settings.json) into a house.json.

    python3 house/migrate.py DIR [--walls walls.json] [--out house.json]

DIR holds the pulled files (any may be missing). --walls: optional scene wall list from the headset,
[{"uuid": 32-hex, "label": "WALL_FACE", "w": m, "h": m}] or {"walls": [...]}, to fill wall extents and kinds; every
scene plane in it becomes a wall of the house (GLOBAL_MESH, a mesh rather than a plane, is skipped). Without it a wall is known
only by its scene uuid, which is all today's files carry; the headset fills in the rest on its first sync.
Nothing is dropped: every value the old files hold has a place in the house (checked by house/test_house.py).
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import schema  # noqa: E402

KIND = {'WALL_FACE': 'wall', 'INVISIBLE_WALL_FACE': 'wall', 'FLOOR': 'floor', 'CEILING': 'ceiling', 'TABLE': 'table'}


def _read(path):
    try:
        with open(path) as f:
            return json.load(f)
    except FileNotFoundError:
        return None


def build(placement=None, screens=None, settings=None, walls=None, when=''):
    doc = schema.empty()
    doc['updated'], doc['by'] = when, 'migrate'
    doc['rooms'] = [{'id': 'r1', 'name': 'home', 'anchors': []}]
    if isinstance(walls, dict):
        walls = walls.get('walls', [])
    scene = {w['uuid']: w for w in (walls or []) if w.get('label') != 'GLOBAL_MESH'}
    wall_of = {}

    def wall(uuid):
        if not uuid:
            return None
        if uuid not in wall_of:
            wid = 'w-' + uuid[:8]
            w = {'id': wid, 'room': 'r1', 'kind': 'wall', 'source': 'scene', 'scene': [uuid]}
            info = scene.get(uuid)
            if info:
                w['kind'] = KIND.get(info.get('label', ''), 'plane')
                for k in ('w', 'h'):
                    if info.get(k):
                        w[k] = info[k]
                if info.get('pose'):
                    w['pose'] = info['pose']
            doc['walls'].append(w)
            wall_of[uuid] = wid
        return wall_of[uuid]

    st = settings or {}
    src = st.get('source', '')
    main_src = {'kind': 'game', 'host': src} if src in schema.GAME_HOSTS else {'kind': 'atlas'}
    if placement and placement.get('wall'):
        doc['screens'].append({'id': schema.MAIN_ID, 'role': 'main', 'on': 1, 'wall': wall(placement['wall']),
                               'x': placement['x'], 'y': placement['y'], 'w': placement['w'], 'h': placement['h'],
                               'source': main_src})
    nxt = 2
    for s in (screens or {}).get('screens', []):
        doc['screens'].append({'id': s['id'], 'role': 'extra', 'on': 1, 'wall': wall(s['wall']),
                               'x': s['x'], 'y': s['y'], 'w': s['w'], 'h': s['h'],
                               'source': {'kind': 'mac-display', 'screen': s['id']}})
        nxt = max(nxt, s['id'] + 1)
    doc['nextScreenId'] = max(nxt, (screens or {}).get('next', 2))

    if st.get('menuSaved') and st.get('menuWall') and isinstance(st.get('menu'), list) and len(st['menu']) == 7:
        doc['menu']['panel'] = {'wall': wall(st['menuWall']), 'pose': st['menu']}
    out = {}
    for k in ('locked', 'resume', 'saveset'):
        if k in st:
            out[k] = bool(st[k])
    if 'source' in st:
        out['source'] = st['source']
    if isinstance(st.get('toggles'), dict):
        out['toggles'] = {k: bool(v) for k, v in st['toggles'].items()}
    doc['settings'] = out
    for uuid in scene:  # the rest of the house: planes nothing references yet
        wall(uuid)
    return doc


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__); sys.exit(2)
    d = a[0]
    walls = _read(a[a.index('--walls') + 1]) if '--walls' in a else None
    out = a[a.index('--out') + 1] if '--out' in a else None
    import datetime
    doc = build(_read(os.path.join(d, 'placement.json')), _read(os.path.join(d, 'screens.json')),
                _read(os.path.join(d, 'settings.json')), walls,
                datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'))
    problems = schema.validate(doc)
    if problems:
        print('\n'.join(problems), file=sys.stderr); sys.exit(1)
    text = json.dumps(doc, indent=1) + '\n'
    if out:
        with open(out, 'w') as f:
            f.write(text)
    else:
        sys.stdout.write(text)


if __name__ == '__main__':
    main()
