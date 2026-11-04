"""house.json: the persistent house file (rooms, walls, user shapes, screens, menu, settings).

One document holds the whole house setup. It is versioned twice:
  schema  the layout of the document. A reader asks for the schema it understands; the server converts down
          (dropping what that schema cannot express) and converts an older-schema write up again, keeping from the
          base revision every field the writer could not see, so an old headset never erases newer data.
  rev     the revision of the content: every accepted write is rev + 1 and one git commit (house/server.py).

Stdlib only: this runs on the Mini (python3) and on the VPS (tests).
"""
import copy
import re

CURRENT = 1
UUID = re.compile(r'^[0-9a-f]{32}$')
SHA = re.compile(r'^[0-9a-f]{64}$')
ID = re.compile(r'^[A-Za-z0-9_.-]{1,64}$')
COLOR = re.compile(r'^#[0-9a-fA-F]{6}$')
WALL_KINDS = {'wall', 'floor', 'ceiling', 'table', 'plane'}
WALL_SOURCES = {'scene', 'depth', 'manual'}
SHAPE_TYPES = {'line', 'circle', 'skin'}
ROLES = {'main', 'extra'}
SOURCE_KINDS = {'mac-display', 'game', 'atlas', 'surface', 'video'}
GAME_HOSTS = {'MINI', 'LAPTOP'}
MAIN_ID = 1
BY = {'headset', 'vps', 'migrate', 'rollback', 'test', 'apps'}
MAX_ITEMS = 2000  # per list; a house has tens of walls and screens, not thousands


def empty():
    return {'schema': CURRENT, 'rev': 0, 'updated': '', 'by': 'migrate', 'nextScreenId': 2,
            'rooms': [], 'links': [], 'walls': [], 'shapes': [], 'screens': [], 'menu': {}, 'settings': {}}


# ---- validation: returns a list of problems ("path: what"), empty when the document is valid

def _num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool) and v == v and abs(v) < 1e6


def _vec(v, n):
    return isinstance(v, list) and len(v) == n and all(_num(x) for x in v)


def _pts(v, lo=1):
    return isinstance(v, list) and lo <= len(v) <= 4096 and all(_vec(p, 2) for p in v)


def _pose(v):
    return isinstance(v, dict) and _vec(v.get('p'), 3) and _vec(v.get('q'), 4)


def _anchors(v):
    # our own persisted spatial anchors (XR_FB_spatial_entity_storage): uuid + pose in the room frame
    return isinstance(v, list) and all(isinstance(a, dict) and isinstance(a.get('uuid'), str) and UUID.match(a['uuid'])
                                       and _pose(a.get('pose')) for a in v)


def validate(doc, blobs=None):
    """Problems with doc as a CURRENT-schema house; blobs: the set of stored blob hashes (None: not checked)."""
    e = []
    if not isinstance(doc, dict):
        return ['$: not an object']
    if doc.get('schema') != CURRENT:
        return [f'schema: {doc.get("schema")!r}, this server stores schema {CURRENT}']
    for k, t in (('rev', int), ('updated', str), ('by', str), ('nextScreenId', int), ('rooms', list), ('links', list), ('walls', list),
                 ('shapes', list), ('screens', list), ('menu', dict), ('settings', dict)):
        if not isinstance(doc.get(k), t) or isinstance(doc.get(k), bool):
            e.append(f'{k}: missing or not {t.__name__}')
    if e:
        return e
    if doc['rev'] < 0:
        e.append('rev: negative')
    if doc['by'] not in BY:
        e.append(f'by: {doc["by"]!r} not one of {sorted(BY)}')
    for k in ('rooms', 'walls', 'shapes', 'screens'):
        if len(doc[k]) > MAX_ITEMS:
            e.append(f'{k}: more than {MAX_ITEMS} items')

    def ids(kind, items):
        seen = set()
        for i, it in enumerate(items):
            p = f'{kind}[{i}]'
            if not isinstance(it, dict):
                e.append(f'{p}: not an object'); continue
            v = it.get('id')
            if kind == 'screens':
                if not isinstance(v, int) or isinstance(v, bool) or v < 0:
                    e.append(f'{p}.id: not a non-negative integer')
            elif not isinstance(v, str) or not ID.match(v):
                e.append(f'{p}.id: {v!r} is not a valid id')
            if v in seen:
                e.append(f'{p}.id: duplicate {v!r}')
            seen.add(v)
        return seen

    rooms = ids('rooms', doc['rooms'])
    walls = ids('walls', doc['walls'])
    ids('shapes', doc['shapes'])
    screens = ids('screens', doc['screens'])

    for i, r in enumerate(doc['rooms']):
        if not isinstance(r, dict): continue
        p = f'rooms[{i}]'
        if 'name' in r and not isinstance(r['name'], str): e.append(f'{p}.name: not a string')
        if 'anchors' in r and not _anchors(r['anchors']): e.append(f'{p}.anchors: needs [{{uuid: 32-hex, pose: {{p, q}}}}, ..]')
        if 'height' in r and not (_num(r['height']) and r['height'] > 0): e.append(f'{p}.height: not a positive number')
        if 'floor' in r and not _pts(r['floor'], 3): e.append(f'{p}.floor: not a list of >= 3 [x,z] points')
        if 'mesh' in r:  # the room's fused geometry (room_lab/roommesh.h save(), "PPRM" blob) in the room frame
            if not (isinstance(r['mesh'], str) and SHA.match(r['mesh'])): e.append(f'{p}.mesh: not a sha256 blob id')
            elif blobs is not None and r['mesh'] not in blobs: e.append(f'{p}.mesh: blob {r["mesh"][:12]}.. not stored (PUT /blob first)')

    for i, l in enumerate(doc['links']):
        p = f'links[{i}]'
        if not (isinstance(l, dict) and l.get('a') in rooms and l.get('b') in rooms and l.get('a') != l.get('b') and _pose(l.get('pose'))):
            e.append(f'{p}: needs a, b (two different known rooms) and pose {{p, q}} of b in a')

    anchor_owner = {}
    for i, w in enumerate(doc['walls']):
        if not isinstance(w, dict): continue
        p = f'walls[{i}]'
        if w.get('room') is not None and w.get('room') not in rooms: e.append(f'{p}.room: unknown room {w.get("room")!r}')
        if w.get('kind') not in WALL_KINDS: e.append(f'{p}.kind: {w.get("kind")!r} not one of {sorted(WALL_KINDS)}')
        if w.get('source') not in WALL_SOURCES: e.append(f'{p}.source: {w.get("source")!r} not one of {sorted(WALL_SOURCES)}')
        a = w.get('scene', [])  # Meta Space Setup uuids that have meant this wall, newest first (a hint for matching)
        if not isinstance(a, list) or not all(isinstance(x, str) and UUID.match(x) for x in a):
            e.append(f'{p}.scene: not a list of 32-hex uuids')
        else:
            for x in a:
                if x in anchor_owner and anchor_owner[x] != w.get('id'):
                    e.append(f'{p}.scene: {x} also belongs to wall {anchor_owner[x]!r}')
                anchor_owner[x] = w.get('id')
        if 'anchors' in w and not _anchors(w['anchors']): e.append(f'{p}.anchors: needs [{{uuid: 32-hex, pose: {{p, q}}}}, ..]')
        for k in ('w', 'h'):  # optional until known: a migrated wall has only its scene uuid until the headset reports it
            if k in w and not (_num(w[k]) and w[k] > 0): e.append(f'{p}.{k}: not a positive number')
        if 'pose' in w and not _pose(w['pose']): e.append(f'{p}.pose: needs p:[x,y,z] and q:[x,y,z,w]')
        if 'fit' in w:
            f = w['fit']
            if not (isinstance(f, dict) and _num(f.get('rms_m')) and f['rms_m'] >= 0 and isinstance(f.get('samples'), int)
                    and isinstance(f.get('updated', ''), str)):
                e.append(f'{p}.fit: needs rms_m >= 0, samples (int), updated (ISO string)')
            # fit.w / fit.h (optional): the extent the depth fit measured; the wall's own w/h stay authoritative
            # (work-c4's wallmerge: a scan never shrinks a wall, it only records what it saw)
            elif any(k in f and not (_num(f[k]) and f[k] > 0) for k in ('w', 'h')):
                e.append(f'{p}.fit.w/h: the measured extent must be positive numbers')
        if 'outline' in w and not _pts(w['outline'], 3): e.append(f'{p}.outline: not a list of >= 3 [x,y] points')
        if 'mesh' in w:
            if not (isinstance(w['mesh'], str) and SHA.match(w['mesh'])): e.append(f'{p}.mesh: not a sha256 blob id')
            elif blobs is not None and w['mesh'] not in blobs: e.append(f'{p}.mesh: blob {w["mesh"][:12]}.. not stored (PUT /blob first)')

    for i, s in enumerate(doc['shapes']):
        if not isinstance(s, dict): continue
        p = f'shapes[{i}]'
        if s.get('wall') not in walls: e.append(f'{p}.wall: unknown wall {s.get("wall")!r}')
        t = s.get('type')
        if t not in SHAPE_TYPES: e.append(f'{p}.type: {t!r} not one of {sorted(SHAPE_TYPES)}'); continue
        if t == 'circle':
            if not _pts(s.get('pts'), 1) or len(s['pts']) != 1: e.append(f'{p}.pts: a circle has one centre point')
            if not (_num(s.get('r')) and s.get('r') > 0): e.append(f'{p}.r: not a positive radius')
        else:
            if not _pts(s.get('pts'), 2): e.append(f'{p}.pts: needs >= 2 [x,y] points')
        if t == 'skin' and not (_num(s.get('depth')) and s.get('depth') >= 0): e.append(f'{p}.depth: a skin needs depth >= 0')
        for k in ('dashed', 'closed', 'fill'):
            if k in s and not isinstance(s[k], bool): e.append(f'{p}.{k}: not a boolean')
        # wall tool extras (room_lab/wallshape.h), all optional: a corner-bent line's pieces share a group; dash/gap/width
        # in metres; thick turns a skin's quad into a box
        if 'group' in s and not (isinstance(s['group'], str) and ID.match(s['group'])): e.append(f'{p}.group: not a valid id')
        for k in ('dash', 'gap', 'width'):
            if k in s and not (_num(s[k]) and s[k] > 0): e.append(f'{p}.{k}: not a positive number')
        if 'thick' in s and not (_num(s['thick']) and s['thick'] >= 0): e.append(f'{p}.thick: not >= 0')
        if 'color' in s and not (isinstance(s['color'], str) and COLOR.match(s['color'])): e.append(f'{p}.color: not #rrggbb')

    mains = 0
    for i, s in enumerate(doc['screens']):
        if not isinstance(s, dict): continue
        p = f'screens[{i}]'
        if s.get('role') not in ROLES: e.append(f'{p}.role: {s.get("role")!r} not one of {sorted(ROLES)}')
        mains += s.get('role') == 'main'
        if s.get('wall') not in walls: e.append(f'{p}.wall: unknown wall {s.get("wall")!r}')
        for k in ('x', 'y'):
            if not _num(s.get(k)): e.append(f'{p}.{k}: not a number')
        for k in ('w', 'h'):
            if not (_num(s.get(k)) and s.get(k) > 0): e.append(f'{p}.{k}: not a positive number')
        if 'on' in s and s['on'] not in (0, 1) or isinstance(s.get('on'), bool): e.append(f'{p}.on: 0 or 1')
        if 'world' in s and not _pose(s['world']): e.append(f'{p}.world: needs p:[x,y,z] and q:[x,y,z,w]')
        src = s.get('source')
        if not (isinstance(src, dict) and src.get('kind') in SOURCE_KINDS):
            e.append(f'{p}.source: needs kind in {sorted(SOURCE_KINDS)}')
        elif src['kind'] == 'mac-display' and not (isinstance(src.get('screen'), int) and src['screen'] >= 0):
            e.append(f'{p}.source.screen: a mac-display source needs the laptop screen id')
        elif src['kind'] == 'game' and src.get('host') not in GAME_HOSTS:
            e.append(f'{p}.source.host: a game source needs host in {sorted(GAME_HOSTS)}')
        if s.get('role') == 'main' and s.get('id') != MAIN_ID:
            e.append(f'{p}.id: the main screen is id {MAIN_ID} (the demand protocol uses it for the main panel)')
    if mains > 1:
        e.append(f'screens: {mains} screens with role "main", at most one')
    ints = [s for s in screens if isinstance(s, int)]
    if ints and doc['nextScreenId'] <= max(ints):
        e.append('nextScreenId: not above every screen id')

    # apps (optional, written only by the pinpin-apps server): which HTML mini-app each screen shows. A top-level
    # section, not a screen field, so a headset re-applying a queued screen edit after a 409 never undoes it.
    if 'apps' in doc:
        if not isinstance(doc['apps'], list) or len(doc['apps']) > MAX_ITEMS:
            e.append('apps: not a list of at most %d items' % MAX_ITEMS)
        else:
            seen_app = set()
            for i, a in enumerate(doc['apps']):
                p = f'apps[{i}]'
                if not isinstance(a, dict): e.append(f'{p}: not an object'); continue
                sc = a.get('screen')
                if not isinstance(sc, int) or isinstance(sc, bool) or sc < 0: e.append(f'{p}.screen: not a non-negative integer')
                elif sc in seen_app: e.append(f'{p}.screen: duplicate {sc}')
                seen_app.add(sc)
                if not (isinstance(a.get('app'), str) and ID.match(a['app'])): e.append(f'{p}.app: {a.get("app")!r} is not a valid app id')

    m = doc['menu']
    pn = m.get('panel')
    if pn is not None:
        if not isinstance(pn, dict) or pn.get('wall') not in walls: e.append('menu.panel.wall: unknown wall')
        elif not _vec(pn.get('pose'), 7): e.append('menu.panel.pose: needs [px,py,pz,qx,qy,qz,qw] relative to the wall')
    sh = m.get('shoulder')
    if sh is not None:
        if not (isinstance(sh, dict) and sh.get('side') in ('left', 'right') and _num(sh.get('reach_m', 1)) and _num(sh.get('step_m', 1))):
            e.append('menu.shoulder: needs side left|right, reach_m, step_m')
    if 'plan' in doc:  # derived (house/consolidate.py, written by the server): shape-checked only
        pl = doc['plan']
        if not (isinstance(pl, dict) and isinstance(pl.get('algo'), str) and isinstance(pl.get('src'), str)
                and isinstance(pl.get('walls'), list) and isinstance(pl.get('rooms'), list) and isinstance(pl.get('score'), dict)):
            e.append('plan: needs algo, src, walls, rooms, score')
        else:
            for i, w in enumerate(pl['walls']):
                if not (isinstance(w, dict) and isinstance(w.get('id'), str) and _pose(w.get('pose')) and _num(w.get('w')) and _num(w.get('h'))):
                    e.append(f'plan.walls[{i}]: needs id, pose, w, h')
            for i, r in enumerate(pl['rooms']):
                if not (isinstance(r, dict) and isinstance(r.get('id'), str) and _pts(r.get('floor'), 3)):
                    e.append(f'plan.rooms[{i}]: needs id and floor (>= 3 [x,z] points)')
    st = doc['settings']
    if 'toggles' in st and not (isinstance(st['toggles'], dict) and all(isinstance(v, bool) for v in st['toggles'].values())):
        e.append('settings.toggles: values must be booleans')
    return e


# ---- schema conversion. UP[n](doc) converts schema n to n+1; DOWN[n](doc) converts schema n+1 to n and drops what
# schema n cannot hold. Empty while only schema 1 exists; the tests register a schema 2 to exercise the machinery.
UP, DOWN = {}, {}


def convert(doc, to):
    d = copy.deepcopy(doc)
    while d['schema'] < to:
        n = d['schema']; d = UP[n](d); d['schema'] = n + 1
    while d['schema'] > to:
        n = d['schema']; d = DOWN[n - 1](d); d['schema'] = n - 1
    return d


def oldest_readable():
    s = CURRENT
    while s - 1 in DOWN:
        s -= 1
    return s


def preserve(new, base, k):
    """new: a write from a schema-k writer, already converted up; base: the current house it was based on.

    Whatever the writer could not see (present in base, absent from base as converted down to schema k) is taken
    from base, so up-conversion defaults never replace newer data. Everything the writer could see is the writer's.
    Lists of objects are matched by id."""
    return _keep(copy.deepcopy(new), base, convert(base, k))


def _keep(new, base, seen):
    if isinstance(new, dict) and isinstance(base, dict):
        seen = seen if isinstance(seen, dict) else {}
        for key, bv in base.items():
            if key not in seen:
                new[key] = copy.deepcopy(bv)
            elif key in new:
                new[key] = _keep(new[key], bv, seen[key])
        return new
    if isinstance(new, list) and isinstance(base, list):
        by = lambda lst: {it.get('id'): it for it in lst if isinstance(it, dict) and 'id' in it}
        b, sv = by(base), by(seen if isinstance(seen, list) else [])
        return [_keep(it, b[it['id']], sv.get(it['id'], {})) if isinstance(it, dict) and it.get('id') in b else it for it in new]
    return new


# ---- the formats today's headset builds read (room_lab.cpp: placement.json, screens.json, settings.json), byte
# layout as room_lab writes them (its loaders use sscanf on that exact order), so a build without the house client
# can still be given the house: GET /legacy/<name>.

def _anchor(doc, wall_id):
    for w in doc['walls']:
        if w['id'] == wall_id and w.get('scene'):
            return w['scene'][0]
    return ''


def _f(v):
    v = float(v)
    return str(int(v)) if v == int(v) else ('%.6g' % v)


def legacy_placement(doc):
    for s in doc['screens']:
        if s['role'] == 'main':
            return '{"wall":"%s","x":%s,"y":%s,"w":%s,"h":%s}' % (_anchor(doc, s['wall']), _f(s['x']), _f(s['y']), _f(s['w']), _f(s['h']))
    return None


def legacy_screens(doc):
    items = ['{"id":%d,"wall":"%s","x":%s,"y":%s,"w":%s,"h":%s}' % (s['id'], _anchor(doc, s['wall']), _f(s['x']), _f(s['y']), _f(s['w']), _f(s['h']))
             for s in doc['screens'] if s['role'] == 'extra']
    return '{"next":%d,"screens":[%s]}' % (doc['nextScreenId'], ','.join(items))


def legacy_settings(doc):
    st, m = doc['settings'], doc['menu'].get('panel') or {}
    pose = m.get('pose') or [0, 0, 0, 0, 0, 0, 1]
    j = '{"locked":%d,"menuSaved":%d,"menuWall":"%s","menu":[%s]' % (
        1 if st.get('locked', True) else 0, 1 if m else 0, _anchor(doc, m['wall']) if m else '', ','.join(_f(v) for v in pose))
    j += ',"saveset":%d,"resume":%d,"source":"%s"' % (1 if st.get('saveset') else 0, 1 if st.get('resume') else 0, st.get('source', ''))
    if st.get('saveset') and st.get('toggles'):
        j += ',"toggles":{%s}' % ','.join('"%s":%d' % (k, 1 if v else 0) for k, v in st['toggles'].items())
    return j + '}'
