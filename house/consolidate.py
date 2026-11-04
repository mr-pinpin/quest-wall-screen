"""House consolidation: the raw surfaces of house.json (Meta scene walls + our depth fits from every SCAN) -> a clean
floor plan, plus a score that says whether it is good enough. A pure function (doc in, plan out); the raw walls are
never changed. house/server.py runs it on every write that changes walls or rooms and stores the result as the
house's "plan" layer; GET /house/plan.svg?layer=clean draws it.

  0. Frame record (consolidate/2). A depth wall committed without a "frame" field predates v0.65, whose SCAN
     records the frame it solved (Meta's planes, else anchors): such a wall may sit in a frame that jumped between
     sessions (STAGE moved ~3.6 m on 2026-11-04), so it is frame-suspect: kept in the raw layer, left out of the plan.
  1. Frame hygiene. Each SCAN commits its surfaces in the room frame as solved at that moment. A commit (one batch:
     fit.updated) whose floor sits off the house floor (Meta's, else the biggest batch's) is moved onto it when the
     offset is small (<= yShiftMax: anchor drift) and dropped as another frame when it is large; a batch rotated
     against the room's directions (> batchDeg) is dropped too. Unlocated poses (exact identity) are dropped.
  2. Manhattan prior. The room's two directions are the peak of the walls' yaw histogram (mod 90 deg, weighted by
     width x confidence). Walls within snapDeg are snapped onto them. Off-axis Meta walls are kept as they are;
     off-axis depth walls are dropped (rotated clusters, furniture at an angle).
  3. Merge. Depth walls below minConf go first. Colinear walls (same facing, offsets within mergeOff) are united
     where their extents touch (gap <= mergeGap). A stack of parallel same-facing walls a little apart (<= stackOff,
     overlapping) is one wall seen several times or a furniture face in front of it: the plan keeps the outermost
     well-supported one (or the Meta wall). A depth wall running past the corners it makes with perpendicular walls
     is cut there (the ~10 m lines across the room); short leftovers (< minLen) go.
  4. Floor plan. The walls' lines cut the floor into cells; a cell is inside a room when the nearest wall in each
     of the four directions faces it (walls extended by doorMax/2 so doorways close). Connected cells, not split by
     a wall, form a room: its outline polygon, floor and ceiling heights (horizontal surfaces inside it), doorways
     (unsupported stretches of doorMin..doorMax between supported ones) and open sides.
  5. Score: duplicates before/after, closure of every room, Meta-vs-depth wall distance, the fraction of the depth
     evidence the plan explains, frames dropped -> "satisfactory" or "not yet", with the reasons.

Coordinates: the room frame (x right, y up, z), a wall's pose +z is its normal into the room, +x along its width.
Stdlib only (runs on the Mini). CLI: python3 house/consolidate.py house.json [--json] [--svg raw|clean|both > out.svg]
"""
import hashlib
import json
import math

ALGO = 'consolidate/2'
PARAMS = {
    'snapDeg': 7.0, 'mergeOff': 0.05, 'mergeDeg': 3.0, 'mergeGap': 0.10, 'minLen': 0.4, 'minConf': 0.4,
    'stackOff': 0.35, 'stackOverlap': 0.5, 'stackKeep': 0.25, 'cornerReach': 0.7, 'overshoot': 0.3,
    'doorMin': 0.7, 'doorMax': 1.2, 'yShiftMax': 0.3, 'batchDeg': 3.0, 'cellMerge': 0.05, 'minRoomArea': 1.0,
    # verdict thresholds (pre-registered with the algorithm; changing them is a new ALGO version)
    'okClosure': 0.9, 'okExplained': 0.6, 'okSceneFit': 0.05,
}


# ---- small geometry

def qrot(q, v):
    x, y, z, w = q
    tx, ty, tz = 2 * (y * v[2] - z * v[1]), 2 * (z * v[0] - x * v[2]), 2 * (x * v[1] - y * v[0])
    return (v[0] + w * tx + (y * tz - z * ty), v[1] + w * ty + (z * tx - x * tz), v[2] + w * tz + (x * ty - y * tx))


def yaw_q(yaw):
    """Rotation about +y by yaw: pose +z -> (sin yaw, 0, cos yaw), +x -> (cos yaw, 0, -sin yaw)."""
    return [0.0, math.sin(yaw / 2), 0.0, math.cos(yaw / 2)]


def wrap(a, period):
    """a into [-period/2, period/2)."""
    return (a + period / 2) % period - period / 2


def wmedian(vals):
    """Weighted median of [(value, weight)]."""
    vals = sorted(vals)
    tot = sum(w for _, w in vals)
    acc = 0
    for v, w in vals:
        acc += w
        if acc >= tot / 2:
            return v
    return vals[-1][0] if vals else 0.0


def r3(v):
    return round(v, 3)


# ---- 0. surfaces

def _conf(w):
    if w.get('source') in ('scene', 'manual'):
        return 1.0
    f = w.get('fit') or {}
    for v in (w.get('confidence'), f.get('conf'), f.get('confidence')):
        if isinstance(v, (int, float)) and not isinstance(v, bool):
            return max(0.0, min(1.0, float(v)))
    # no stored confidence: support (samples) x flatness (rms; real Quest depth fits 10-25 mm on a good wall)
    s = min(1.0, f.get('samples', 0) / 15000.0)
    flat = max(0.0, min(1.0, (0.04 - f.get('rms_m', 0.04)) / 0.03))
    return s * flat


def surfaces(doc, dropped):
    out = []
    for w in doc.get('walls', []):
        if not isinstance(w, dict):
            continue
        wid = w.get('id')
        pose = w.get('pose')
        if not isinstance(pose, dict) or not w.get('w') or not w.get('h'):
            dropped.append({'id': wid, 'why': 'no pose or size yet'}); continue
        p, q = [float(v) for v in pose['p']], [float(v) for v in pose['q']]
        if w.get('source', 'depth') == 'depth' and not isinstance(w.get('frame'), dict):
            dropped.append({'id': wid, 'why': 'frame-suspect: no frame record (committed before v0.65); kept in raw, not in the plan'}); continue
        if (w.get('frame') or {}).get('located') is False or (p == [0, 0, 0] and q == [0, 0, 0, 1]):
            dropped.append({'id': wid, 'why': 'frame: unlocated pose'}); continue
        r, u, n = qrot(q, (1, 0, 0)), qrot(q, (0, 1, 0)), qrot(q, (0, 0, 1))
        f = w.get('fit') or {}
        s = {'id': wid, 'src': w.get('source', 'depth'), 'kind': w.get('kind', 'plane'), 'p': p, 'r': r, 'u': u, 'n': n,
             'w': float(w['w']), 'h': float(w['h']), 'conf': _conf(w), 'samples': int(f.get('samples', 0) or 0),
             'batch': (w.get('frame') or {}).get('id') or f.get('updated') or ('scene' if w.get('source') != 'depth' else '?')}
        if abs(n[1]) < 0.26:
            s['orient'] = 'v'; s['yaw'] = math.atan2(n[0], n[2])
            s['y0'] = p[1] - abs(u[1]) * s['h'] / 2 - abs(r[1]) * s['w'] / 2
            s['y1'] = p[1] + abs(u[1]) * s['h'] / 2 + abs(r[1]) * s['w'] / 2
        elif abs(n[1]) > 0.9:
            s['orient'] = 'up' if n[1] > 0 else 'down'
            s['corners'] = [(p[0] + r[0] * a * s['w'] / 2 + u[0] * b * s['h'] / 2, p[2] + r[2] * a * s['w'] / 2 + u[2] * b * s['h'] / 2)
                            for a, b in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
        else:
            tilt = math.degrees(math.asin(min(1.0, abs(n[1]))))
            dropped.append({'id': wid, 'why': 'slanted (normal %.0f deg above horizontal): neither wall nor floor' % tilt})
            continue
        out.append(s)
    return out


# ---- 1. frame hygiene + 2. Manhattan axes

def axes_peak(surfs, P):
    """The room's direction (radians, in [0, pi/2)) and the weight share within snapDeg of it."""
    bins = [0.0] * 180  # 0.5 deg over 90 deg
    tot = 0.0
    for s in surfs:
        if s['orient'] != 'v':
            continue
        a = math.degrees(s['yaw']) % 90.0
        wgt = s['w'] * max(s['conf'], 0.05)
        bins[int(a * 2) % 180] += wgt; tot += wgt
    if tot <= 0:
        return 0.0, 0.0
    sm = [sum(bins[(i + k) % 180] for k in range(-4, 5)) for i in range(180)]
    peak = max(range(180), key=lambda i: sm[i]) / 2.0 + 0.25
    # refine: weighted circular mean (period 90) of the yaws within 3 deg of the peak
    cs = sn = 0.0
    for s in surfs:
        if s['orient'] == 'v':
            d = wrap(math.degrees(s['yaw']) - peak, 90)
            if abs(d) <= 3:
                wgt = s['w'] * max(s['conf'], 0.05); a4 = math.radians(4 * (peak + d))
                cs += wgt * math.cos(a4); sn += wgt * math.sin(a4)
    th = (math.degrees(math.atan2(sn, cs)) / 4.0) % 90.0 if cs or sn else peak
    share = sum(s['w'] * max(s['conf'], 0.05) for s in surfs if s['orient'] == 'v'
                and abs(wrap(math.degrees(s['yaw']) - th, 90)) <= P['snapDeg']) / tot
    return math.radians(th), share


def frames(surfs, P, dropped):
    """Per-commit checks against the house floor and the room's directions. Moves small floor offsets, drops batches
    in another frame. Returns the batch report."""
    ups = [s for s in surfs if s['orient'] == 'up' and s['w'] * s['h'] >= 1.0]
    downs = [s for s in surfs if s['orient'] == 'down' and s['w'] * s['h'] >= 1.0]
    batches = {}
    for s in surfs:
        if s['src'] == 'depth':
            batches.setdefault(s['batch'], []).append(s)
    big = lambda ss: sum(s['samples'] for s in ss)
    scene_up = [s for s in ups if s['src'] != 'depth']
    scene_dn = [s for s in downs if s['src'] != 'depth']
    lowest = lambda ss: min(s['p'][1] for s in ss)
    if scene_up:
        ref_floor, ref_from = lowest(scene_up), 'meta floor'
    else:
        best = max(batches, key=lambda b: big(batches[b]), default=None)
        bu = [s for s in batches.get(best, []) if s in ups]
        ref_floor, ref_from = (lowest(bu), f'floor of commit {best}') if bu else (None, None)
    ref_ceil = max(s['p'][1] for s in scene_dn) if scene_dn else None
    th0, _ = axes_peak(surfs, P)
    report = []
    for b, ss in sorted(batches.items()):
        rep = {'batch': b, 'surfaces': len(ss), 'samples': big(ss), 'dy': 0.0, 'yawDeg': None, 'status': 'ok'}
        verts = [s for s in ss if s['orient'] == 'v']
        if len(verts) >= 3:
            th, share = axes_peak(verts, P)
            dyaw = wrap(math.degrees(th - th0), 90)
            rep['yawDeg'] = round(dyaw, 2)
            if abs(dyaw) > P['batchDeg'] and share > 0.5:
                rep['status'] = f'dropped: rotated {dyaw:+.1f} deg against the room'
        if rep['status'] == 'ok':
            bu = [s for s in ss if s in ups]
            bd = [s for s in ss if s in downs]
            dy, how = None, ''
            if ref_floor is not None and bu:
                dy, how = ref_floor - lowest(bu), ref_from
            elif ref_ceil is not None and bd:
                dy, how = ref_ceil - max(s['p'][1] for s in bd), 'meta ceiling'
            if dy is not None:
                rep['dy'] = round(dy, 3)
                if abs(dy) > P['yShiftMax']:
                    rep['status'] = f'dropped: floor {dy:+.2f} m off the {how} (another frame)'
                elif abs(dy) > 0.02:
                    rep['status'] = f'moved {dy:+.2f} m onto the {how}'
                    for s in ss:
                        s['p'] = [s['p'][0], s['p'][1] + dy, s['p'][2]]
                        if 'y0' in s:
                            s['y0'] += dy; s['y1'] += dy
            else:
                rep['status'] = 'ok (no floor or ceiling to check)'
        if rep['status'].startswith('dropped'):
            for s in ss:
                s['drop'] = 'frame: commit ' + rep['status'][9:]
        report.append(rep)
    for s in surfs:
        if s.get('drop'):
            dropped.append({'id': s['id'], 'why': s['drop']})
    return [s for s in surfs if not s.get('drop')], report, ref_floor, ref_ceil


# ---- 3. segments in the axis frame

class Frame2:
    """(x, z) <-> (A, B): B along the normal of facing 0 (yaw th), A along the normal of facing 1 (yaw th + 90)."""
    def __init__(self, th):
        self.th = th
        self.eb = (math.sin(th), math.cos(th))
        self.ea = (math.cos(th), -math.sin(th))

    def to(self, x, z):
        return (x * self.ea[0] + z * self.ea[1], x * self.eb[0] + z * self.eb[1])

    def back(self, a, b):
        return (a * self.ea[0] + b * self.eb[0], a * self.ea[1] + b * self.eb[1])


SIGN = (1, 1, -1, -1)  # facing k: normal +B, +A, -B, -A


def seg_line(k):
    """For facing k: (index of the offset coordinate, index of the tangent coordinate) in (A, B)."""
    return (1, 0) if k % 2 == 0 else (0, 1)


def make_segments(surfs, F, P, dropped):
    segs, offaxis = [], []
    for s in surfs:
        if s['orient'] != 'v':
            continue
        d = wrap(math.degrees(s['yaw'] - F.th), 360)
        k = int(round(d / 90.0)) % 4
        dev = wrap(d - 90 * k, 360)
        c = F.to(s['p'][0], s['p'][2])
        if abs(dev) > P['snapDeg']:
            if s['src'] == 'depth':
                dropped.append({'id': s['id'], 'why': f'off-axis: {abs(dev):.0f} deg from the room directions'}); continue
            offaxis.append(s); continue
        if s['src'] == 'depth' and s['conf'] < P['minConf']:
            dropped.append({'id': s['id'], 'why': f'low confidence {s["conf"]:.2f}'}); continue
        oi, ti = seg_line(k)
        half = s['w'] / 2 * math.hypot(s['r'][0], s['r'][2])
        segs.append({'k': k, 'off': c[oi], 't0': c[ti] - half, 't1': c[ti] + half, 'y0': s['y0'], 'y1': s['y1'],
                     'members': [s], 'ext': [0.0, 0.0]})
    return segs, offaxis


def _weight(s):
    return s['conf'] * s['w'] * (5.0 if s['src'] != 'depth' else 1.0)


def _scene(seg):
    return [m for m in seg['members'] if m['src'] != 'depth']


def _offset_of(seg, F):
    sc = _scene(seg)
    mem = sc or seg['members']
    oi, _ = seg_line(seg['k'])
    vals = [(F.to(m['p'][0], m['p'][2])[oi], _weight(m) + 1e-9) for m in mem]
    return sum(v * w for v, w in vals) / sum(w for _, w in vals)


def merge_colinear(segs, F, P):
    out = []
    for k in range(4):
        ss = sorted((s for s in segs if s['k'] == k), key=lambda s: s['off'])
        groups, cur = [], []
        for s in ss:
            if cur and (s['off'] - cur[-1]['off'] > P['mergeOff'] or s['off'] - cur[0]['off'] > 2 * P['mergeOff']):
                groups.append(cur); cur = []
            cur.append(s)
        if cur:
            groups.append(cur)
        for g in groups:
            g.sort(key=lambda s: s['t0'])
            run = None
            for s in g:
                # Meta walls stay as Meta made them: never joined to each other, and a depth piece joins one only
                # where they overlap (an exterior wall running on past a partition is the next room's wall)
                sa, sb = bool(_scene(run)) if run else False, bool(_scene(s))
                reach = P['mergeGap'] if not (sa or sb) else -0.05
                if run and not (sa and sb) and s['t0'] <= run['t1'] + reach:
                    run['t1'] = max(run['t1'], s['t1']); run['members'] += s['members']
                    run['y0'] = min(run['y0'], s['y0']); run['y1'] = max(run['y1'], s['y1'])
                else:
                    if run:
                        out.append(run)
                    run = dict(s, members=list(s['members']))
            if run:
                out.append(run)
    for s in out:
        s['off'] = _offset_of(s, F)
    return out


def _overlap(a, b):
    return max(0.0, min(a['t1'], b['t1']) - max(a['t0'], b['t0']))


def suppress_stacks(segs, P, dropped, F):
    """Parallel same-facing walls a little apart and overlapping: keep one (the Meta wall, else the outermost well
    supported one); others are noted (furniture face or the same wall seen again)."""
    segs = sorted(segs, key=lambda s: -sum(_weight(m) for m in s['members']))
    alive = [True] * len(segs)
    for i, a in enumerate(segs):
        if not alive[i]:
            continue
        stack = [i]
        for j in range(len(segs)):
            b = segs[j]
            if j != i and alive[j] and b['k'] == a['k'] and abs(b['off'] - a['off']) <= P['stackOff'] \
                    and _overlap(a, b) >= P['stackOverlap'] * min(a['t1'] - a['t0'], b['t1'] - b['t0']):
                stack.append(j)
        if len(stack) == 1:
            continue
        wmax = max(sum(_weight(m) for m in segs[j]['members']) for j in stack)
        sc = [j for j in stack if _scene(segs[j])]
        if sc:
            keep = sc[0]
        else:
            strong = [j for j in stack if sum(_weight(m) for m in segs[j]['members']) >= P['stackKeep'] * wmax]
            keep = min(strong, key=lambda j: SIGN[a['k']] * segs[j]['off'])  # outermost: furthest behind the room side
        kp = segs[keep]
        for j in stack:
            if j == keep:
                continue
            b = segs[j]
            gap = abs(b['off'] - kp['off'])
            if gap <= 0.12 and not _scene(b):  # the same wall again: its extent counts
                kp['t0'] = min(kp['t0'], b['t0']); kp['t1'] = max(kp['t1'], b['t1']); kp['members'] += b['members']
                why = 'duplicate'
            elif _scene(b):
                continue  # two Meta walls are never merged away
            else:
                why = 'in front'
            for m in b['members']:
                kp.setdefault('stacked', []).append({'id': m['id'], 'gap': r3(gap), 'src': m['src'], 'samples': m['samples']})
                if why == 'in front':
                    dropped.append({'id': m['id'], 'why': f'{gap:.2f} m in front of a wall it overlaps (furniture face or a second look)'})
            alive[j] = False
        if not sc:
            kp['off'] = kp['off']  # the outermost keeps its own offset
    return [s for i, s in enumerate(segs) if alive[i]]


def clip_corners(segs, P, dropped):
    """Corners: perpendicular walls whose lines meet within cornerReach of both. A perpendicular wall faces into its
    room, so it says on which side of the corner this wall belongs: a depth wall running more than overshoot past
    that corner (out of the room) is cut there, and one ending short of it by <= cornerReach is extended to it
    (marked as inferred). Meta walls are left as they are."""
    R = P['cornerReach']
    cuts = []
    for s in segs:
        t0, t1, e0, e1 = s['t0'], s['t1'], 0.0, 0.0
        if not _scene(s):
            _, ti = seg_line(s['k'])
            lo_side, hi_side = [], []   # corners whose room lies above (constrain t0) / below (constrain t1)
            for t in segs:
                if t is s or t['k'] % 2 == s['k'] % 2:
                    continue
                if s['t0'] - R <= t['off'] <= s['t1'] + R and t['t0'] - R <= s['off'] <= t['t1'] + R:
                    # same room only: t lies in front of s, and some of s lies in front of t (t's normal runs
                    # along s's tangent axis); a wall of the next room met back to back is no corner of s's room
                    if SIGN[s['k']] * ((t['t0'] + t['t1']) / 2 - s['off']) <= 0:
                        continue
                    if (s['t1'] if SIGN[t['k']] > 0 else -s['t0']) <= SIGN[t['k']] * t['off']:
                        continue  # none of s lies on t's room side
                    (lo_side if SIGN[t['k']] > 0 else hi_side).append(t['off'])
            if lo_side:
                c = min(lo_side)
                if t0 < c - P['overshoot'] or (c - P['overshoot'] <= t0 < c):
                    t0 = c
                elif c < t0 <= c + R:
                    e0 = t0 - c; t0 = c
            if hi_side:
                c = max(hi_side)
                if t1 > c + P['overshoot'] or (c < t1 <= c + P['overshoot']):
                    t1 = c
                elif c - R <= t1 < c:
                    e1 = c - t1; t1 = c
        cuts.append((t0, t1, e0, e1))
    out = []
    for s, (t0, t1, e0, e1) in zip(segs, cuts):
        if (t1 - t0) < (s['t1'] - s['t0']) - 0.31:
            s['clipped'] = r3(s['t1'] - s['t0'])
        s['t0'], s['t1'], s['ext'] = t0, t1, [e0, e1]
        if t1 - t0 < P['minLen'] and not _scene(s):
            for m in s['members']:
                dropped.append({'id': m['id'], 'why': f'short ({max(0.0, t1 - t0):.2f} m after merging and corners)'})
            continue
        out.append(s)
    return out


# ---- 4. rooms

def _cluster(vals, eps):
    vals = sorted(vals)
    out, cur = [], []
    for v in vals:
        if cur and v - cur[0] > eps:
            out.append(cur[len(cur) // 2]); cur = []
        cur.append(v)
    if cur:
        out.append(cur[len(cur) // 2])
    return out


def _segments2d(segs, offaxis, F, ext):
    """Every wall as a 2D segment in (A, B) with its normal and the real part of it (parameter range; the rest is
    the doorway-closing extension), and whether it is a Meta wall (a real wall: its back is the next room's wall):
    (p, q, n, (u0, u1), meta)."""
    out = []
    for s in segs:
        oi, ti = seg_line(s['k'])
        a, b = [0, 0], [0, 0]
        a[oi] = b[oi] = s['off']; a[ti] = s['t0'] - ext; b[ti] = s['t1'] + ext
        n = [0, 0]; n[oi] = SIGN[s['k']]
        L = s['t1'] - s['t0'] + 2 * ext
        out.append((tuple(a), tuple(b), tuple(n), (ext / L, 1 - ext / L), bool(_scene(s))))
    for s in offaxis:
        c = F.to(s['p'][0], s['p'][2]); r = F.to(s['r'][0], s['r'][2]); n = F.to(s['n'][0], s['n'][2])
        hw = s['w'] / 2 + ext
        out.append(((c[0] - r[0] * hw, c[1] - r[1] * hw), (c[0] + r[0] * hw, c[1] + r[1] * hw), n, (ext / (2 * hw), 1 - ext / (2 * hw)), True))
    return out


def _ray(c, d, walls):
    """Nearest wall hit from c along unit axis d: (distance, facing c?, meta back?) or None."""
    best = None
    for p, q, n, (u0, u1), meta in walls:
        ex, ey = q[0] - p[0], q[1] - p[1]
        den = d[0] * ey - d[1] * ex
        if abs(den) < 1e-12:
            continue
        wx, wy = p[0] - c[0], p[1] - c[1]
        t = (wx * ey - wy * ex) / den
        u = (wx * d[1] - wy * d[0]) / den
        if t > 1e-6 and -1e-9 <= u <= 1 + 1e-9 and (best is None or t < best[0]):
            facing = n[0] * d[0] + n[1] * d[1] < 0
            if not facing and not (u0 <= u <= u1):
                continue  # the back of a doorway extension (it pokes through into the next room): not a wall
            best = (t, facing, meta and not facing and u0 <= u <= u1)
    return best


def _crosses(c1, c2, walls):
    for p, q, _, _, _ in walls:
        d1 = (q[0] - p[0]) * (c1[1] - p[1]) - (q[1] - p[1]) * (c1[0] - p[0])
        d2 = (q[0] - p[0]) * (c2[1] - p[1]) - (q[1] - p[1]) * (c2[0] - p[0])
        d3 = (c2[0] - c1[0]) * (p[1] - c1[1]) - (c2[1] - c1[1]) * (p[0] - c1[0])
        d4 = (c2[0] - c1[0]) * (q[1] - c1[1]) - (c2[1] - c1[1]) * (q[0] - c1[0])
        if d1 * d2 < 0 and d3 * d4 <= 0:
            return True
    return False


def _outline(cells, xs, ys):
    """Boundary loops of a set of grid cells (i, j) -> list of loops of (A, B), counter-clockwise outer first."""
    edges = {}
    for (i, j) in cells:
        x0, x1, y0, y1 = xs[i], xs[i + 1], ys[j], ys[j + 1]
        for (ni, nj), a, b in (((i, j - 1), (x0, y0), (x1, y0)), ((i + 1, j), (x1, y0), (x1, y1)),
                               ((i, j + 1), (x1, y1), (x0, y1)), ((i - 1, j), (x0, y1), (x0, y0))):
            if (ni, nj) not in cells:
                edges.setdefault(a, []).append(b)
    loops = []
    while edges:
        start = next(iter(edges))
        loop, cur = [start], start
        while True:
            nxt = edges[cur].pop()
            if not edges[cur]:
                del edges[cur]
            if nxt == start:
                break
            loop.append(nxt); cur = nxt
            if cur not in edges:
                break
        # drop colinear vertices
        clean = []
        for idx, v in enumerate(loop):
            a, b = loop[idx - 1], loop[(idx + 1) % len(loop)]
            if abs((v[0] - a[0]) * (b[1] - v[1]) - (v[1] - a[1]) * (b[0] - v[0])) > 1e-12:
                clean.append(v)
        if len(clean) >= 3:
            loops.append(clean)
    area = lambda L: sum(L[i][0] * L[(i + 1) % len(L)][1] - L[(i + 1) % len(L)][0] * L[i][1] for i in range(len(L))) / 2
    loops.sort(key=lambda L: -abs(area(L)))
    return loops, area


def rooms(segs, offaxis, F, P, exclude=()):
    """Rooms from the walls; cells whose centre lies in an exclude polygon ((A, B) loops) are left out."""
    if not segs:
        return []
    A, B = [], []
    for s in segs:
        oi, ti = seg_line(s['k'])
        (A if oi == 0 else B).append(s['off'])
        e = P['doorMax'] / 2  # where the doorway extensions end too: rays change there
        (B if oi == 0 else A).extend([s['t0'], s['t1'], s['t0'] - e, s['t1'] + e])
    for s in offaxis:
        c = F.to(s['p'][0], s['p'][2]); r = F.to(s['r'][0], s['r'][2]); hw = s['w'] / 2
        for e in (-1, 1):
            A.append(c[0] + r[0] * hw * e); B.append(c[1] + r[1] * hw * e)
    xs = _cluster(A, P['cellMerge']); ys = _cluster(B, P['cellMerge'])
    m = 0.5
    xs = [xs[0] - m] + xs + [xs[-1] + m]; ys = [ys[0] - m] + ys + [ys[-1] + m]
    closed = _segments2d(segs, offaxis, F, P['doorMax'] / 2)
    status = {}
    for i in range(len(xs) - 1):
        for j in range(len(ys) - 1):
            c = ((xs[i] + xs[i + 1]) / 2, (ys[j] + ys[j + 1]) / 2)
            if any(_inside(c, poly) for poly in exclude):
                continue
            hits = [_ray(c, d, closed) for d in ((1, 0), (-1, 0), (0, 1), (0, -1))]
            if any(h and not h[1] and not h[2] for h in hits):
                continue  # the back of one of our depth walls (single-sided evidence): outside this room
            n, front = sum(1 for h in hits if h), sum(1 for h in hits if h and h[1])
            # inside: a wall all round (a Meta wall's back counts: it is the next room's wall too, if 2 face us);
            # open: three walls facing us and nothing at all the fourth way (an unscanned side)
            if (n == 4 and front >= 2) or (n == 3 and front == 3):
                status[(i, j)] = n
    out, seen = [], set()
    centre = lambda i, j: ((xs[i] + xs[i + 1]) / 2, (ys[j] + ys[j + 1]) / 2)
    for cell in status:
        if cell in seen:
            continue
        comp, todo = set(), [cell]
        seen.add(cell)
        while todo:
            i, j = todo.pop(); comp.add((i, j))
            for nb in ((i + 1, j), (i - 1, j), (i, j + 1), (i, j - 1)):
                if nb in status and nb not in seen and not _crosses(centre(i, j), centre(*nb), closed):
                    seen.add(nb); todo.append(nb)
        cellarea = sum((xs[i + 1] - xs[i]) * (ys[j + 1] - ys[j]) for i, j in comp)
        full = sum((xs[i + 1] - xs[i]) * (ys[j + 1] - ys[j]) for i, j in comp if status[(i, j)] == 4)
        if cellarea < P['minRoomArea'] or full < 0.3 * cellarea:
            continue
        loops, area = _outline(comp, xs, ys)
        out.append({'cells': comp, 'loops': loops, 'area': cellarea, 'open': full < cellarea - 1e-6})
    out.sort(key=lambda r: -r['area'])
    return out


def meta_cycles(surfs, tol=0.15):
    """Meta's Space Setup outlines a room as a closed chain of walls, end to end. Chains of Meta walls whose ends
    meet (within tol) and close: [(polygon [(x, z)], {wall ids})]."""
    S = [s for s in surfs if s['orient'] == 'v' and s['src'] != 'depth']
    ends = []
    for s in S:
        hw = s['w'] / 2
        ends.append(((s['p'][0] - s['r'][0] * hw, s['p'][2] - s['r'][2] * hw), (s['p'][0] + s['r'][0] * hw, s['p'][2] + s['r'][2] * hw)))
    node = []  # endpoint -> node id
    pos = []
    for a, b in ends:
        ids = []
        for e in (a, b):
            k = next((i for i, q in enumerate(pos) if abs(q[0] - e[0]) <= tol and abs(q[1] - e[1]) <= tol), None)
            if k is None:
                pos.append(e); k = len(pos) - 1
            ids.append(k)
        node.append(ids)
    adj = {}
    for w, (u, v) in enumerate(node):
        if u != v:
            adj.setdefault(u, []).append((v, w)); adj.setdefault(v, []).append((u, w))
    out, used = [], set()
    for start in list(adj):
        if start in used or len(adj[start]) != 2:
            continue
        loop, walls, cur, prev_w, ok = [start], [], start, None, True
        while True:
            nxt = [(v, w) for v, w in adj[cur] if w != prev_w]
            if len(adj[cur]) != 2 or not nxt:
                ok = False; break
            v, w = nxt[0]; walls.append(w); prev_w = w
            if v == start:
                break
            if v in loop:
                ok = False; break
            loop.append(v); cur = v
        used.update(loop)
        if ok and len(walls) >= 3:
            out.append(([pos[i] for i in loop], {S[w]['id'] for w in walls}))
    return out


def outside_scene_rooms(segs, offaxis, F, P, dropped):
    """Meta's rooms (from its walls alone) are authoritative: they are the plan's rooms there, and the part of a
    depth wall lying inside one (more than 10 cm from its outline) is not a wall of the house (an overgrown extent,
    furniture); it is cut away. Returns (segments, Meta's rooms)."""
    scene = [s for s in segs if _scene(s)]
    surfs = [m for s in scene for m in s['members']] + list(offaxis)
    found = []
    for poly, ids in meta_cycles(surfs):
        loop = [F.to(x, z) for x, z in poly]
        ar = sum(loop[i][0] * loop[(i + 1) % len(loop)][1] - loop[(i + 1) % len(loop)][0] * loop[i][1] for i in range(len(loop))) / 2
        if ar < 0:
            loop.reverse()
        found.append({'loops': [loop], 'area': abs(ar), 'open': False, 'meta': ids})
    if not found and scene:  # no closed chain: Meta's rooms from its walls by the rays, as for depth walls
        found = rooms(scene, offaxis, F, P)
    inner = [[F.back(a, b) for a, b in r['loops'][0]] for r in found]
    if not inner or len(scene) == len(segs):
        return segs, found
    def inside(a, b):
        x, z = F.back(a, b)
        return any(_inside((x, z), poly) for poly in inner)
    out = []
    for s in segs:
        if _scene(s):
            out.append(s); continue
        oi, ti = seg_line(s['k'])
        step, n = 0.05, max(2, int((s['t1'] - s['t0']) / 0.05) + 1)
        keep = []
        for i in range(n):
            t = s['t0'] + (s['t1'] - s['t0']) * i / (n - 1)
            pts = []
            for dn in (-0.1, 0.0, 0.1):
                c = [0, 0]; c[oi] = s['off'] + dn; c[ti] = t
                pts.append(inside(*c))
            keep.append(not all(pts))
        if all(keep):
            out.append(s); continue
        pieces, start = [], None
        for i, k in enumerate(keep + [False]):
            if k and start is None:
                start = i
            elif not k and start is not None:
                pieces.append((start, i - 1)); start = None
        cut = 0.0
        for i0, i1 in pieces:
            t0 = s['t0'] + (s['t1'] - s['t0']) * i0 / (n - 1); t1 = s['t0'] + (s['t1'] - s['t0']) * i1 / (n - 1)
            if t1 - t0 >= P['minLen']:
                piece = dict(s, t0=t0, t1=t1, ext=[0.0, 0.0])
                out.append(piece); cut += t1 - t0
        if cut == 0.0:
            for m in s['members']:
                dropped.append({'id': m['id'], 'why': 'inside a Meta room (not a wall of the house)'})
        else:
            for q in out[-len(pieces):]:
                if q.get('members') is s['members']:
                    q['insideCut'] = r3(s['t1'] - s['t0'] - cut)
    return out, found


def support(room, segs, P):
    """Per outline edge: which stretches a real wall (not the doorway extension) covers, facing into the room ->
    supported length, doorways, holes, open stretches; and the walls that bound the room."""
    real = []
    for s in segs:
        oi, ti = seg_line(s['k'])
        real.append((s, oi, ti, s['t0'] + s['ext'][0], s['t1'] - s['ext'][1]))
    per = sup = 0.0
    doors, holes, opens, bounding = [], [], [], set()
    ends = []
    for s, oi, ti, t0, t1 in real:
        for t in (t0, t1):
            e = [0, 0]; e[oi] = s['off']; e[ti] = t; ends.append(tuple(e))
    for L in room['loops']:
        for idx in range(len(L)):
            a, b = L[idx], L[(idx + 1) % len(L)]
            horiz = abs(a[1] - b[1]) < 1e-9
            oi, ti = (1, 0) if horiz else (0, 1)
            lo, hi = sorted((a[ti], b[ti]))
            length = hi - lo; per += length
            # interior side: loops are CCW (outer) in (A, B), so the interior is to the left of a -> b
            d = (b[0] - a[0], b[1] - a[1]); inward = (-d[1], d[0])
            iv = []
            for s, soi, sti, t0, t1 in real:
                if soi != oi or abs(s['off'] - a[oi]) > 0.08:
                    continue
                nrm = [0, 0]; nrm[soi] = SIGN[s['k']]
                if nrm[0] * inward[0] + nrm[1] * inward[1] <= 0 and not _scene(s):
                    continue
                x0, x1 = max(lo, t0), min(hi, t1)
                if x1 > x0:
                    iv.append((x0, x1)); bounding.add(id(s))
            iv.sort(); merged = []
            for x0, x1 in iv:
                if merged and x0 <= merged[-1][1] + 1e-6:
                    merged[-1][1] = max(merged[-1][1], x1)
                else:
                    merged.append([x0, x1])
            sup += sum(x1 - x0 for x0, x1 in merged)
            cur = lo
            gaps = []
            for x0, x1 in merged + [[hi, hi]]:
                if x0 - cur > 1e-6:
                    gaps.append((cur, x0, cur > lo + 1e-6 and x0 < hi - 1e-6))
                cur = max(cur, x1)
            for g0, g1, inner in gaps:
                gl = g1 - g0
                p0 = [0, 0]; p1 = [0, 0]; p0[oi] = p1[oi] = a[oi]; p0[ti], p1[ti] = g0, g1
                rec = {'a': tuple(p0), 'b': tuple(p1), 'w': gl}
                if P['doorMin'] <= gl <= P['doorMax'] + 0.02 and _at_wall_end(p0, ends) and _at_wall_end(p1, ends):
                    doors.append(rec)  # a doorway: real walls end on both sides of it
                elif gl < P['doorMin']:
                    holes.append(rec)
                else:
                    opens.append(rec)
    open_len = sum(g['w'] for g in opens) + (sum(g['w'] for g in doors if False))
    return {'perimeter': per, 'supported': sup, 'doors': doors, 'holes': holes, 'opens': opens,
            'closure': 1.0 - open_len / per if per else 0.0, 'bounding': bounding}


def _at_wall_end(p, ends, tol=0.15):
    return any(abs(p[0] - e[0]) <= tol and abs(p[1] - e[1]) <= tol for e in ends)


def _inside(pt, poly):
    x, z = pt; inside = False
    for i in range(len(poly)):
        (x0, z0), (x1, z1) = poly[i - 1], poly[i]
        if (z0 > z) != (z1 > z) and x < x0 + (z - z0) * (x1 - x0) / (z1 - z0):
            inside = not inside
    return inside


# ---- 5. score helpers

def duplicates(walls):
    """Pairs of vertical walls that are the same wall twice: same facing (within 10 deg), each centre within
    stackOff of the other's plane, overlapping by half the shorter."""
    v = [w for w in walls if w.get('orient', 'v') == 'v']
    n = 0
    for i in range(len(v)):
        for j in range(i + 1, len(v)):
            a, b = v[i], v[j]
            if a['n'][0] * b['n'][0] + a['n'][2] * b['n'][2] < math.cos(math.radians(10)):
                continue
            dist = abs((b['p'][0] - a['p'][0]) * a['n'][0] + (b['p'][2] - a['p'][2]) * a['n'][2])
            if dist > PARAMS['stackOff']:
                continue
            ta = a['p'][0] * a['r'][0] + a['p'][2] * a['r'][2]
            tb = b['p'][0] * a['r'][0] + b['p'][2] * a['r'][2]
            ov = min(ta + a['w'] / 2, tb + b['w'] / 2) - max(ta - a['w'] / 2, tb - b['w'] / 2)
            if ov >= 0.5 * min(a['w'], b['w']):
                n += 1
    return n


def src_hash(doc):
    """What the plan depends on: walls and rooms, without their mesh blobs (a new room mesh every few minutes must
    not recompute the plan)."""
    strip = lambda items: [{k: v for k, v in it.items() if k != 'mesh'} if isinstance(it, dict) else it for it in items]
    return hashlib.sha256(json.dumps({'walls': strip(doc.get('walls', [])), 'rooms': strip(doc.get('rooms', [])), 'algo': ALGO},
                                     sort_keys=True, separators=(',', ':')).encode()).hexdigest()


# ---- the pass

def consolidate(doc, params=None):
    P = dict(PARAMS, **(params or {}))
    dropped = []
    raw = surfaces(doc, dropped)
    raw_vert = [dict(s) for s in raw if s['orient'] == 'v']
    surfs, batches, ref_floor, ref_ceil = frames(raw, P, dropped)
    th, share = axes_peak(surfs, P)
    F = Frame2(th)
    segs, offaxis = make_segments(surfs, F, P, dropped)
    segs = merge_colinear(segs, F, P)
    segs = suppress_stacks(segs, P, dropped, F)
    segs = clip_corners(segs, P, dropped)
    segs, meta_rooms = outside_scene_rooms(segs, offaxis, F, P, dropped)
    rm = _rooms_and_walls(segs, offaxis, surfs, F, P, ref_floor, ref_ceil, dropped, meta_rooms)
    plan_walls, plan_rooms = rm
    # ---- score
    depth_v = [s for s in raw_vert if s['src'] == 'depth']
    depth_total = sum(s['samples'] for s in depth_v) or 0
    kept_ids = {m['id'] for s in segs for m in s['members']}
    explained = (sum(s['samples'] for s in depth_v if s['id'] in kept_ids) / depth_total) if depth_total else None
    # Meta vs depth: for each Meta wall, the nearest depth wall on the same face (same facing within snapDeg,
    # overlapping, within stackOff), all depth walls counted (also those dropped later: this checks the frame)
    fits = []
    for m in surfs:
        if m['orient'] != 'v' or m['src'] == 'depth':
            continue
        best = None
        for s in surfs:
            if s['orient'] != 'v' or s['src'] != 'depth' or s.get('drop'):
                continue
            if m['n'][0] * s['n'][0] + m['n'][2] * s['n'][2] < math.cos(math.radians(P['snapDeg'])):
                continue
            dist = abs((s['p'][0] - m['p'][0]) * m['n'][0] + (s['p'][2] - m['p'][2]) * m['n'][2])
            ta = m['p'][0] * m['r'][0] + m['p'][2] * m['r'][2]; tb = s['p'][0] * m['r'][0] + s['p'][2] * m['r'][2]
            ov = min(ta + m['w'] / 2, tb + s['w'] / 2) - max(ta - m['w'] / 2, tb - s['w'] / 2)
            if dist <= P['stackOff'] and ov >= 0.3 and (best is None or dist < best):
                best = dist
        if best is not None:
            fits.append(best)
    fits.sort()
    scene_fit = {'pairs': len(fits), 'median_m': r3(fits[len(fits) // 2]) if fits else None, 'max_m': r3(fits[-1]) if fits else None}
    dup_before = duplicates(raw_vert)
    dup_after = duplicates([w['_s'] for w in plan_walls])
    reasons, ok = [], True
    if not plan_rooms:
        ok = False; reasons.append('no closed room: walls do not enclose any floor yet')
    for r in plan_rooms:
        if not r['closed'] and r['closure'] >= P['okClosure']:
            ok = False
            reasons.append(f'{r["id"]}: not closed (seen from inside, a side has no wall: scan the rest of it)')
        if r['closure'] < P['okClosure']:
            ok = False
            reasons.append(f'{r["id"]}: open {r["openM"]:.1f} m of {r["perimeter"]:.1f} m (closure {r["closure"]:.2f} < {P["okClosure"]})')
    if dup_after:
        ok = False; reasons.append(f'{dup_after} duplicate wall pairs left')
    if explained is not None and explained < P['okExplained']:
        ok = False; reasons.append(f'the plan explains {explained:.0%} of the depth evidence (< {P["okExplained"]:.0%})')
    if scene_fit['median_m'] is not None and scene_fit['median_m'] > P['okSceneFit']:
        ok = False; reasons.append(f'depth walls sit {scene_fit["median_m"] * 100:.0f} cm from the Meta walls (median, > {P["okSceneFit"] * 100:.0f} cm)')
    # back to back: a depth wall facing away from a Meta wall right behind it (the next room's side of the same wall)
    b2b = []
    for s in segs:
        if _scene(s):
            continue
        for t in segs:
            if _scene(t) and t['k'] == (s['k'] + 2) % 4 and _overlap(s, t) >= 0.5:
                gap = SIGN[s['k']] * (s['off'] - t['off'])  # >= 0: s lies behind t (each faces its own room)
                if -0.05 <= gap <= 0.3:
                    b2b.append({'depth': [m['id'] for m in s['members']][0], 'meta': _scene(t)[0]['id'], 'thickness_m': r3(gap)})
    scene_fit['backToBack'] = b2b
    both = any(s['src'] == 'depth' for s in raw_vert) and any(s['src'] != 'depth' for s in raw_vert)
    if both and not fits and not b2b:
        ok = False; reasons.append('no depth wall meets a Meta wall (same face or back to back): the two cannot be cross-checked (another frame?)')
    thin = [b for b in b2b if b['thickness_m'] < 0.06]
    if thin:
        ok = False
        reasons.append('; '.join(f'{b["depth"]} is {b["thickness_m"] * 100:.0f} cm behind Meta {b["meta"]}' for b in thin)
                       + ': thinner than a real wall (6+ cm), so the scan and Meta disagree by a few cm (frame offset?)')
    lone = [w['id'] for w in plan_walls if not w.get('room') and not w.get('offAxis') and w['w'] >= 1.0]
    if lone:
        ok = False
        reasons.append(f'{len(lone)} wall(s) of 1 m or more bound no closed room (a room is open or unscanned): ' + ', '.join(lone))
    bad = [b for b in batches if b['status'].startswith('dropped')]
    if bad:
        ok = False; reasons.append(f'{len(bad)} scan commit(s) in another frame: ' + '; '.join(f'{b["batch"]}: {b["status"]}' for b in bad))
    for w in plan_walls:
        del w['_s']
    score = {'verdict': 'satisfactory' if ok else 'not yet', 'reasons': reasons,
             'duplicates': {'raw': dup_before, 'plan': dup_after}, 'rooms': len(plan_rooms),
             'closure': r3(min((r['closure'] for r in plan_rooms), default=0.0)),
             'sceneFit': scene_fit, 'explained': r3(explained) if explained is not None else None,
             'walls': {'raw': len(raw_vert), 'plan': len(plan_walls)}, 'axesShare': r3(share),
             'frameSuspect': sum(1 for d in dropped if d['why'].startswith('frame-suspect'))}
    return {'algo': ALGO, 'src': src_hash(doc), 'rev': doc.get('rev'), 'axesDeg': round(math.degrees(th), 2),
            'floorY': r3(ref_floor) if ref_floor is not None else None, 'frames': batches,
            'walls': plan_walls, 'rooms': plan_rooms, 'dropped': dropped, 'score': score}


def _rooms_and_walls(segs, offaxis, surfs, F, P, ref_floor, ref_ceil, dropped, meta_rooms=()):
    # Meta's rooms as Meta's walls make them; the depth walls make rooms only on the floor outside those
    found = list(meta_rooms) + rooms(segs, offaxis, F, P, exclude=[r['loops'][0] for r in meta_rooms])
    found.sort(key=lambda r: -r['area'])
    horiz = [s for s in surfs if s['orient'] in ('up', 'down')]
    plan_rooms, owner = [], {}
    for n, r in enumerate(found):
        if r.get('meta'):  # a closed chain of Meta walls: supported all round by construction
            L = r['loops'][0]
            per = sum(math.hypot(L[i][0] - L[i - 1][0], L[i][1] - L[i - 1][1]) for i in range(len(L)))
            sp = {'perimeter': per, 'supported': per, 'doors': [], 'holes': [], 'opens': [], 'closure': 1.0,
                  'bounding': {id(s) for s in segs if any(m['id'] in r['meta'] for m in s['members'])}}
            for s in offaxis:
                if s['id'] in r['meta']:
                    sp['bounding'].add(id(s))
        else:
            sp = support(r, segs, P)
        polys = [[F.back(a, b) for a, b in L] for L in r['loops']]
        poly = polys[0]
        inside = [s for s in horiz if _inside((s['p'][0], s['p'][2]), poly)]
        ups = [s for s in inside if s['orient'] == 'up']
        dns = [s for s in inside if s['orient'] == 'down']
        if ups:
            lo = min(s['p'][1] for s in ups)
            fy = wmedian([(s['p'][1], s['w'] * s['h']) for s in ups if s['p'][1] <= lo + 0.08])
        else:
            fy = ref_floor
        cy = max(s['p'][1] for s in dns) if dns else ref_ceil
        rid = f'pr{n + 1}'
        tops = [{'id': s['id'], 'y': r3(s['p'][1]), 'area': r3(s['w'] * s['h'])} for s in ups if fy is not None and s['p'][1] > fy + 0.2]
        for s in list(segs) + list(offaxis):
            if id(s) in sp['bounding']:
                owner.setdefault(id(s), rid)
        pt = lambda v: [r3(v[0]), r3(v[1])]
        plan_rooms.append({
            'id': rid, 'floor': [pt(v) for v in poly], 'holes': [[pt(v) for v in h] for h in polys[1:]],
            'area': r3(r['area']), 'floorY': r3(fy) if fy is not None else None, 'ceilingY': r3(cy) if cy is not None else None,
            'closed': not r['open'] and not sp['opens'], 'closure': r3(sp['closure']), 'support': r3(sp['supported'] / sp['perimeter']),
            'perimeter': r3(sp['perimeter']), 'openM': r3(sum(g['w'] for g in sp['opens'])),
            'doors': [{'a': pt(F.back(*g['a'])), 'b': pt(F.back(*g['b'])), 'w': r3(g['w'])} for g in sp['doors']],
            'opens': [{'a': pt(F.back(*g['a'])), 'b': pt(F.back(*g['b'])), 'w': r3(g['w'])} for g in sp['opens']],
            'tops': tops})
    plan_walls = []
    used = set()
    for s in segs:
        oi, ti = seg_line(s['k'])
        ids = [m['id'] for m in s['members']]
        best = max(s['members'], key=_weight)
        wid = 'p-' + str(best['id'])[2:] if str(best['id']).startswith('w-') else 'p-' + str(best['id'])
        while wid in used:
            wid += 'x'
        used.add(wid)
        rid = owner.get(id(s))
        room = next((r for r in plan_rooms if r['id'] == rid), None)
        y0, y1 = s['y0'], s['y1']
        if room and room['floorY'] is not None and room['ceilingY'] is not None:
            y0, y1 = room['floorY'], room['ceilingY']
        c = [0, 0]; c[oi] = s['off']; c[ti] = (s['t0'] + s['t1']) / 2
        x, z = F.back(*c)
        yaw = F.th + s['k'] * math.pi / 2
        sc = _scene(s)
        src = 'scene' if sc and len(sc) == len(s['members']) else 'scene+depth' if sc else 'depth'
        w = s['t1'] - s['t0']
        nx, nz = math.sin(yaw), math.cos(yaw)
        rec = {'id': wid, 'kind': 'wall', 'pose': {'p': [r3(x), r3((y0 + y1) / 2), r3(z)], 'q': [round(v, 6) for v in yaw_q(yaw)]},
               'w': r3(w), 'h': r3(y1 - y0), 'from': ids, 'src': src, 'conf': r3(max(m['conf'] for m in s['members'])),
               'room': rid}
        if s.get('stacked'):
            rec['stacked'] = s['stacked']
        if s.get('clipped'):
            rec['clippedFrom'] = s['clipped']
        if any(s['ext']):
            rec['inferred'] = [r3(v) for v in s['ext']]
        rec['_s'] = {'p': [x, 0, z], 'n': (nx, 0, nz), 'r': (math.cos(yaw), 0, -math.sin(yaw)), 'w': w}
        plan_walls.append(rec)
    for s in offaxis:
        rec = {'id': 'p-' + str(s['id'])[2:], 'kind': 'wall', 'pose': {'p': [r3(v) for v in s['p']], 'q': None},
               'w': r3(s['w']), 'h': r3(s['h']), 'from': [s['id']], 'src': s['src'], 'conf': 1.0, 'room': owner.get(id(s)), 'offAxis': True}
        rec['pose']['q'] = [round(v, 6) for v in yaw_q(s['yaw'])]
        rec['_s'] = {'p': s['p'], 'n': s['n'], 'r': s['r'], 'w': s['w']}
        plan_walls.append(rec)
    return plan_walls, plan_rooms


# ---- drawing (house/floorplan.py draws the raw layer; this adds the clean one)

def svg(doc, plan, layer='both', size=800, margin=40):
    import floorplan
    return floorplan.plan(doc, size, margin, layer=layer, clean=plan)


if __name__ == '__main__':
    import argparse
    import os
    import sys
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument('house')
    ap.add_argument('--json', action='store_true', help='print the whole plan')
    ap.add_argument('--svg', choices=('raw', 'clean', 'both'))
    a = ap.parse_args()
    d = json.load(open(a.house))
    pl = consolidate(d)
    if a.svg:
        sys.stdout.write(svg(d, pl, a.svg))
    elif a.json:
        print(json.dumps(pl, indent=1))
    else:
        sc = pl['score']
        print(f'{sc["verdict"]}: {sc["walls"]["raw"]} raw walls -> {sc["walls"]["plan"]} plan walls, {sc["rooms"]} rooms, '
              f'duplicates {sc["duplicates"]["raw"]} -> {sc["duplicates"]["plan"]}, closure {sc["closure"]}, '
              f'scene fit {sc["sceneFit"]}, explained {sc["explained"]}, axes {pl["axesDeg"]} deg ({sc["axesShare"]})')
        for r in sc['reasons']:
            print('  -', r)
        for b in pl['frames']:
            print('  frame', b)
        for r in pl['rooms']:
            print('  room', r['id'], 'area', r['area'], 'closure', r['closure'], 'support', r['support'], 'doors', [d['w'] for d in r['doors']],
                  'open', [o['w'] for o in r['opens']], 'floor', r['floorY'], 'ceil', r['ceilingY'], 'corners', len(r['floor']))
        for w in pl['walls']:
            print('  wall', w['id'], w['src'], w['room'], 'w %.2f' % w['w'], 'from', len(w['from']), w.get('clippedFrom', ''), w.get('inferred', ''))
        for x in pl['dropped']:
            print('  drop', x['id'], x['why'])
