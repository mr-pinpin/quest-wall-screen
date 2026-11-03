#!/usr/bin/env python3
"""Offline checker for virtual-texturing layouts (docs/virtual-texturing.md, protocol v2). No hardware needed.

  python3 bench/vt_check.py recorded FILE      # check layouts the sender really sent (JSON lines, see below)
  python3 bench/vt_check.py synthetic [--runs 2000] [--seed 1]   # random v2 demand through a model of the packer

Recorded input, one JSON object per line: {"layout": {...}, "displays": {"2": [1920, 1200], ...},
"demand": {...v2 demand in force...}}. `displays` and `demand` are optional: without them the display size is
inferred from the src rects and demand checks are skipped. headset_sim.py --v2 writes this format (vtLog).

Checks, per layout:
  fit      atlas within 3840x2400 and H.264 level 5.1 (36,864 macroblocks); every cell inside the atlas;
           no two cells overlap; every cell at most 1920x1200.
  cover    each screen's src rects tile its display exactly: no gaps, no overlaps, to the pixel.
  level    all cells of a screen share one lvl; lvl = round(log2(srcW / frameW)); cell aspect matches src aspect
           (within the 2 px of rounding frames to even sizes).
  demand   (notes, not errors: the sender follows demand with delays by design) a hidden screen still wider than a
           mip tail, a visible one with more detail than asked (shrinks wait for the refit gate) or less (atlas full).
and across layouts: gen strictly increases whenever the layout changes.

Synthetic mode runs a line-for-line Python model of AtlasSender.layoutVT (keep the two in step), so it checks the
packing algorithm, not the Swift build; recorded mode checks the real sender.
"""
import argparse, json, math, random, sys

MAX_W, MAX_H, TILE_W, TILE_H, TAIL = 3840, 2400, 1920, 1200, 240


def check_layout(L, displays=None, demand=None):
    errs, info = [], []
    W, H = L['atlas']; cells = L['cells']
    if W > MAX_W or H > MAX_H: errs.append(f'atlas {W}x{H} over {MAX_W}x{MAX_H}')
    if math.ceil(W / 16) * math.ceil(H / 16) > 36864: errs.append(f'atlas {W}x{H} over H.264 level 5.1')
    for i, c in enumerate(cells):
        if c['x'] < 0 or c['y'] < 0 or c['x'] + c['w'] > W or c['y'] + c['h'] > H: errs.append(f'cell {i} (screen {c["screen"]}) outside the atlas')
        if c['w'] > TILE_W or c['h'] > TILE_H: errs.append(f'cell {i} (screen {c["screen"]}) {c["w"]}x{c["h"]} over the tile cap')
        if c['w'] <= 0 or c['h'] <= 0: errs.append(f'cell {i} empty')
    # overlaps: sweep by x
    order = sorted(range(len(cells)), key=lambda i: cells[i]['x'])
    for a_i, a in enumerate(order):
        A = cells[a]
        for b in order[a_i + 1:]:
            B = cells[b]
            if B['x'] >= A['x'] + A['w']: break
            if B['y'] < A['y'] + A['h'] and A['y'] < B['y'] + B['h']: errs.append(f'cells {a} and {b} overlap (screens {A["screen"]}, {B["screen"]})')
    by = {}
    for c in cells: by.setdefault(c['screen'], []).append(c)
    want = {s['id']: s for s in (demand or {}).get('screens', [])}
    for sid, cs in by.items():
        if any('src' not in c or 'lvl' not in c for c in cs): errs.append(f'screen {sid}: cell without src/lvl'); continue
        if len({c['lvl'] for c in cs}) != 1: errs.append(f'screen {sid}: mixed levels {sorted({c["lvl"] for c in cs})}')
        if displays and str(sid) in displays: sw, sh = displays[str(sid)]
        else: sw = max(c['src'][0] + c['src'][2] for c in cs); sh = max(c['src'][1] + c['src'][3] for c in cs)
        k = cs[0]['src'][2] / cs[0]['w']                     # display px per frame px
        tol = math.ceil(k) + 1
        frame_w = round(sw / k)
        lvl = max(0, round(math.log2(sw / frame_w))) if frame_w else 0
        if cs[0]['lvl'] != lvl: errs.append(f'screen {sid}: lvl {cs[0]["lvl"]} but frame {frame_w} of {sw} means {lvl}')
        for c in cs:
            # frames round to even pixels, so a cell may stretch by up to 2 px per axis
            kx, ky = c['src'][2] / c['w'], c['src'][3] / c['h']
            if abs(kx / ky - 1) > 2.5 / min(c['w'], c['h']): errs.append(f'screen {sid}: cell aspect {c["w"]}x{c["h"]} vs src {c["src"][2]}x{c["src"][3]}')
        # coverage: area adds up, every rect inside, no pairwise overlap beyond rounding
        area = sum(c['src'][2] * c['src'][3] for c in cs)
        if area != sw * sh: errs.append(f'screen {sid}: src rects cover {area} px of {sw}x{sh} ({sw * sh - area:+d})')
        for c in cs:
            x, y, w, h = c['src']
            if x < 0 or y < 0 or x + w > sw + tol or y + h > sh + tol: errs.append(f'screen {sid}: src {c["src"]} outside {sw}x{sh}')
        for i in range(len(cs)):
            for j in range(i + 1, len(cs)):
                a, b = cs[i]['src'], cs[j]['src']
                ox = min(a[0] + a[2], b[0] + b[2]) - max(a[0], b[0]); oy = min(a[1] + a[3], b[1] + b[3]) - max(a[1], b[1])
                if ox > 0 and oy > 0: errs.append(f'screen {sid}: src rects {a} and {b} overlap')
        if sid in want:
            d = want[sid]
            if not d.get('vis', 1):
                if frame_w > TAIL: info.append(f'screen {sid} hidden but {frame_w} px wide (capture not paused yet?)')
            elif 'lvl' in d and frame_w > (sw >> d['lvl']) + 1: info.append(f'screen {sid}: {frame_w} px, more than lvl {d["lvl"]} asks (shrinks wait for the 5 s refit gate)')
            elif 'lvl' in d and frame_w < (sw >> d['lvl']) - 2: info.append(f'screen {sid}: shrunk to {frame_w} px of wanted {sw >> d["lvl"]} (atlas full)')
    return errs, info


# ---- model of AtlasSender.layoutVT (Swift), for synthetic runs
def layout_vt(entries, tiers, live, pri, cell_w=TILE_W, cell_h=TILE_H):
    def tail(e):
        w = e['w']
        while w > TAIL: w //= 2
        return max(16, w // 2 * 2)
    def height(e, w): return max(16, w * e['h'] // max(1, e['w']) // 2 * 2)
    width = {e['screen']: (max(tail(e), min(e['w'], tiers.get(e['screen'], min(e['w'], cell_w))) // 2 * 2) if e['screen'] in live else tail(e)) for e in entries}
    def pack():
        tiles = []
        for e in entries:
            fw = width[e['screen']]; fh = height(e, fw)
            for ty in range(0, fh, cell_h):
                for tx in range(0, fw, cell_w):
                    tiles.append(dict(e=e, w=min(cell_w, fw - tx), h=min(cell_h, fh - ty), sx=tx, sy=ty, fw=fw, fh=fh))
        tiles = [t for _, t in sorted(enumerate(tiles), key=lambda p: (-p[1]['h'], p[0]))]
        out, x, y, row_h, W = [], 0, 0, 0, 0
        for t in tiles:
            if x + t['w'] > MAX_W: x = 0; y += row_h; row_h = 0
            if y + t['h'] > MAX_H: return None
            out.append(dict(t, x=x, y=y)); x += t['w']; row_h = max(row_h, t['h']); W = max(W, x)
        return max(16, min(MAX_W, (W + 15) // 16 * 16)), max(16, min(MAX_H, (y + row_h + 15) // 16 * 16)), out
    while True:
        p = pack()
        if p: break
        cand = [e for e in entries if width[e['screen']] > tail(e)]
        if not cand: return None
        def cost(e): w = width[e['screen']]; return w * height(e, w) / pri.get(e['screen'], 1)
        e = max(cand, key=cost)   # like Swift's max(by:), keeps the first of equal maxima
        width[e['screen']] = max(tail(e), int(width[e['screen']] * 0.9) // 2 * 2)
    W, H, out = p
    cells = []
    for c in out:
        e = c['e']; k = e['w'] / c['fw']
        def edge(v, frame, disp): return disp if v >= frame else int(math.floor(v * disp / frame + 0.5))
        x0, x1 = edge(c['sx'], c['fw'], e['w']), edge(c['sx'] + c['w'], c['fw'], e['w'])
        y0, y1 = edge(c['sy'], c['fh'], e['h']), edge(c['sy'] + c['h'], c['fh'], e['h'])
        cells.append({'screen': e['screen'], 'x': c['x'], 'y': c['y'], 'w': c['w'], 'h': c['h'], 'lvl': max(0, round(math.log2(k))),
                      'src': [x0, y0, x1 - x0, y1 - y0]})
    return {'atlas': [W, H], 'cells': cells}


def synthetic(runs, seed):
    rnd = random.Random(seed); bad = 0; shrunk = 0
    sizes = [(1920, 1200), (3840, 2160), (2560, 1600), (1920, 1080), (3840, 2400), (1280, 800)]
    for r in range(runs):
        n = rnd.randint(1, 30)
        entries = [dict(screen=i + 2, w=sw, h=sh) for i, (sw, sh) in enumerate(rnd.choice(sizes) for _ in range(n))]
        demand = {'v': 2, 'screens': []}; tiers = {}; live = set(); pri = {}
        for e in entries:
            vis = rnd.random() < 0.6; lvl = rnd.choice([0, 0, 1, 1, 2, 3]); p = round(rnd.uniform(0.05, 1), 2)
            demand['screens'].append({'id': e['screen'], 'vis': int(vis), 'lvl': lvl, 'pri': p})
            if vis: live.add(e['screen']); tiers[e['screen']] = max(16, e['w'] >> lvl); pri[e['screen']] = p
            else: pri[e['screen']] = 0.05
        L = layout_vt(entries, tiers, live, pri)
        if L is None: print(f'run {r}: {n} screens: even mip tails do not fit (sender falls back to v1)'); continue
        errs, info = check_layout(L, {str(e['screen']): [e['w'], e['h']] for e in entries}, demand)
        shrunk += any('shrunk' in i for i in info)
        if errs:
            bad += 1
            if bad <= 5: print(f'run {r}: {n} screens:', *errs[:6], sep='\n  ')
    print(f'synthetic: {runs} layouts, {bad} with errors, {shrunk} with a screen shrunk to fit')
    return bad == 0


def recorded(path):
    bad = 0; last_gen = None; last = None; n = 0
    for line_no, line in enumerate(open(path), 1):
        line = line.strip()
        if not line: continue
        rec = json.loads(line); L = rec.get('layout', rec)
        if 'gen' not in L: continue  # v1 layout
        n += 1
        errs, info = check_layout(L, rec.get('displays'), rec.get('demand'))
        body = json.dumps({k: v for k, v in L.items() if k != 'gen'}, sort_keys=True)
        if last is not None and body != last and L['gen'] <= last_gen: errs.append(f'gen {L["gen"]} did not increase (was {last_gen})')
        if last is not None and body == last and L['gen'] != last_gen: errs.append(f'gen changed {last_gen} -> {L["gen"]} for the same layout')
        last, last_gen = body, L['gen']
        for i in info: print(f'line {line_no} gen {L["gen"]}: note: {i}')
        if errs: bad += 1; print(f'line {line_no} gen {L["gen"]}:', *errs, sep='\n  ')
    print(f'recorded: {n} v2 layouts, {bad} with errors')
    return bad == 0 and n > 0


if __name__ == '__main__':
    ap = argparse.ArgumentParser(); sub = ap.add_subparsers(dest='mode', required=True)
    r = sub.add_parser('recorded'); r.add_argument('file')
    s = sub.add_parser('synthetic'); s.add_argument('--runs', type=int, default=2000); s.add_argument('--seed', type=int, default=1)
    a = ap.parse_args()
    sys.exit(0 if (recorded(a.file) if a.mode == 'recorded' else synthetic(a.runs, a.seed)) else 1)
