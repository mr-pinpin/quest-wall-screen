"""Top-down floor plan of the house (SVG) from the walls' poses, in the room frame: x to the right, z down the page.

Walls (vertical planes) are drawn as segments from pose.p -/+ right*w/2, with a tick along the normal (+z of the
pose, into the room); floors and ceilings as faint rectangles; screens as thick bars on their wall at x, with their
id. Walls without a pose yet (orientation unknown, e.g. migrated from files that carried none) are counted in the
caption. Served by house/server.py as GET /house/plan.svg.
"""
import html


def rotate(q, v):
    x, y, z, w = q
    ux, uy, uz = x, y, z
    tx, ty, tz = 2 * (uy * v[2] - uz * v[1]), 2 * (uz * v[0] - ux * v[2]), 2 * (ux * v[1] - uy * v[0])
    return (v[0] + w * tx + (uy * tz - uz * ty), v[1] + w * ty + (uz * tx - ux * tz), v[2] + w * tz + (ux * ty - uy * tx))


def axes(pose):
    q = pose['q']
    return rotate(q, (1, 0, 0)), rotate(q, (0, 1, 0)), rotate(q, (0, 0, 1))


def plan(doc, size=800, margin=40):
    walls = {w['id']: w for w in doc.get('walls', []) if isinstance(w, dict)}
    posed = {i: w for i, w in walls.items() if isinstance(w.get('pose'), dict)}
    segs, rects, ticks, bars, labels = [], [], [], [], []
    for i, w in posed.items():
        p = w['pose']['p']; r, u, n = axes(w['pose']); hw, hh = w.get('w', 1.0) / 2, w.get('h', 1.0) / 2
        if w.get('kind') in ('floor', 'ceiling') or abs(n[1]) > 0.7:
            corners = [(p[0] + r[0] * a * hw + u[0] * b * hh, p[2] + r[2] * a * hw + u[2] * b * hh) for a, b in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
            rects.append((w.get('kind', 'plane'), corners))
            continue
        a = (p[0] - r[0] * hw, p[2] - r[2] * hw); b = (p[0] + r[0] * hw, p[2] + r[2] * hw)
        segs.append((i, a, b, 'w' not in w))
        ticks.append(((p[0], p[2]), (p[0] + n[0] * 0.25, p[2] + n[2] * 0.25)))
        labels.append((i, (p[0] + n[0] * 0.4, p[2] + n[2] * 0.4)))
    for s in doc.get('screens', []):
        w = posed.get(s.get('wall'))
        if not w:
            continue
        p = w['pose']['p']; r, u, n = axes(w['pose']); c = [p[k] + r[k] * s['x'] + u[k] * s['y'] + n[k] * 0.06 for k in range(3)]
        bars.append((s['id'], (c[0] - r[0] * s['w'] / 2, c[2] - r[2] * s['w'] / 2), (c[0] + r[0] * s['w'] / 2, c[2] + r[2] * s['w'] / 2), s.get('on', 1)))
    pts = [q for _, a, b, _ in segs for q in (a, b)] + [q for _, c in rects for q in c] + [q for _, a, b, _ in bars for q in (a, b)]
    if not pts:
        pts = [(-1, -1), (1, 1)]
    x0, x1 = min(q[0] for q in pts), max(q[0] for q in pts); z0, z1 = min(q[1] for q in pts), max(q[1] for q in pts)
    s = (size - 2 * margin) / max(x1 - x0, z1 - z0, 0.5)
    X = lambda q: f'{margin + (q[0] - x0) * s:.1f}'
    Z = lambda q: f'{margin + (q[1] - z0) * s:.1f}'
    W, H = int(2 * margin + (x1 - x0) * s) + 1, int(2 * margin + (z1 - z0) * s) + 40
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" font-family="sans-serif" font-size="11">',
         '<rect width="100%" height="100%" fill="#fbfaf7"/>']
    for kind, c in rects:
        o.append(f'<polygon class="{kind}" points="{" ".join(X(q) + "," + Z(q) for q in c)}" fill="{"#e8eef6" if kind == "floor" else "none"}" '
                 f'stroke="#9aa9bd" stroke-dasharray="{"" if kind == "floor" else "4 4"}"/>')
    for i, a, b, unknown in segs:
        o.append(f'<line class="wall" x1="{X(a)}" y1="{Z(a)}" x2="{X(b)}" y2="{Z(b)}" stroke="#2b2f36" stroke-width="4" stroke-linecap="round"'
                 + (' stroke-dasharray="6 4"' if unknown else '') + '/>')
    for a, b in ticks:
        o.append(f'<line class="normal" x1="{X(a)}" y1="{Z(a)}" x2="{X(b)}" y2="{Z(b)}" stroke="#c0392b" stroke-width="1.5"/>')
    for i, q in labels:
        o.append(f'<text x="{X(q)}" y="{Z(q)}" fill="#555" text-anchor="middle">{html.escape(i)}</text>')
    for sid, a, b, on in bars:
        o.append(f'<line class="screen" x1="{X(a)}" y1="{Z(a)}" x2="{X(b)}" y2="{Z(b)}" stroke="{"#1f77b4" if on else "#a9c4dd"}" stroke-width="7"/>')
        o.append(f'<text x="{X(((a[0] + b[0]) / 2, (a[1] + b[1]) / 2))}" y="{float(Z(((a[0] + b[0]) / 2, (a[1] + b[1]) / 2))) - 8:.1f}" fill="#1f77b4" text-anchor="middle">screen {sid}</text>')
    missing = len(walls) - len(posed)
    cap = f'house rev {doc.get("rev")}: {len(posed)} walls with orientation' + (f', {missing} without (no pose yet)' if missing else '') + '; 1 m = ' + f'{s:.0f} px'
    o.append(f'<text x="{margin}" y="{H - 14}" fill="#333">{html.escape(cap)}</text></svg>')
    return '\n'.join(o) + '\n'
