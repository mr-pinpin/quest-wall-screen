#!/usr/bin/env python3
"""room_tools.py — room actions for the captain (work-df) to run from the VPS.

Usage:
  room_tools.py house_info
  room_tools.py move <id> <left|right|up|down> [amount_m]     # small nudge, default 0.12 m
  room_tools.py select <id> | room_tools.py clear [id]         # gold outline list (work-0e renderer, v0.66)
  room_tools.py label <screen> <x> <y> <text> [color]         # a label mark above/near a screen (metres)
  room_tools.py arrow <screen> <x> <y> <dx> <dy> [color]      # an arrow mark from (x,y) to (x+dx,y+dy)
  room_tools.py draw '<json mark>'                            # any mark (label|arrow|rect|circle); full control
  room_tools.py unmark [id]                                   # drop one mark, or all. marks drawn by the headset
  room_tools.py add_card "<title>" "<details>"                # -> work-ad kanban API
  room_tools.py status [screen_id]                            # what the headset SHOWS (HEARTBEAT, no adb): age,
                                                              # version, house rev applied, each screen vs the house

House server: PINPIN_HOUSE_BASE (default the Mini over Tailscale). Writes use If-Match + SHA256.
TEST writes against the SCRATCH house (:57801), never the real house, until the user asks.
"""
import json, os, sys, hashlib, urllib.request, urllib.error, subprocess, time

HOUSE = os.environ.get("PINPIN_HOUSE_BASE", "http://100.64.243.30:57800")   # Mini, Tailscale
KANBAN_API = os.environ.get("PINPIN_KANBAN_API", "http://100.64.243.30:57830/api/kanban")  # work-ad's board
NUDGE_DEFAULT = 0.12      # a nudge is a small fixed step
NUDGE_MAX = 0.30
_DIRS = {"left": ("x", -1), "right": ("x", 1), "up": ("y", 1), "down": ("y", -1)}


def _get():
    with urllib.request.urlopen(HOUSE + "/house", timeout=10) as r:
        return json.load(r), r.headers.get("X-House-Rev")


def _put(doc, rev):
    doc["by"] = "captain"   # the house history names the writer; without this a room.sh write kept the last writer's "headset"
    body = json.dumps(doc).encode()
    req = urllib.request.Request(HOUSE + "/house", data=body, method="PUT",
                                 headers={"Content-Type": "application/json", "If-Match": str(rev),
                                          "X-House-SHA256": hashlib.sha256(body).hexdigest()})
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.load(r)


def _wall(doc, wid):
    return next((w for w in doc.get("walls", []) if w.get("id") == wid), None)


def house_info():
    doc, rev = _get()
    walls = [{"id": w["id"], "w": w.get("w"), "h": w.get("h")}
             for w in doc.get("walls", []) if w.get("kind") == "wall" and w.get("scene")]
    screens = [{"id": s["id"], "wall": s.get("wall"),
                "x": s.get("x"), "y": s.get("y"), "w": s.get("w"), "h": s.get("h"),
                "on": s.get("on"), "source": s.get("source")} for s in doc.get("screens", [])]
    out = {"rev": rev, "nextScreenId": doc.get("nextScreenId"),
           "selected": doc.get("selected"), "marks": doc.get("marks"),
           "placeable_walls": walls, "screens": screens}
    print(json.dumps(out, indent=2))


# --- HEARTBEAT read path: the headset posts its compact status to the house server every ~10 s (room_lab/heartbeat.h)
STALE_S = 30              # older than this: the headset is not reporting (off, asleep, app closed, off the LAN)


def compare(beat, doc, tol=0.01):
    """Each house screen against what the headset reports: {id: {"ok": bool, "diff": {...}}}. on, x, y, w, h must
    agree within tol metres. A screen the headset does not report is not ok."""
    seen = {s.get("id"): s for s in beat.get("screens", [])}
    out = {}
    for hs in doc.get("screens", []):
        sid = hs.get("id"); got = seen.get(sid)
        if got is None:
            out[sid] = {"ok": False, "diff": {"missing": "not reported by the headset"}}
            continue
        diff = {}
        if int(bool(hs.get("on", 1))) != int(bool(got.get("on", 1))):
            diff["on"] = [hs.get("on", 1), got.get("on")]
        for k in ("x", "y", "w", "h"):
            a, b = hs.get(k), got.get(k)
            if isinstance(a, (int, float)) and isinstance(b, (int, float)) and abs(a - b) > tol:
                diff[k] = [round(a, 3), round(b, 3)]
        out[sid] = {"ok": not diff, "diff": diff}
    return out


def status(sid=None):
    try:
        with urllib.request.urlopen(HOUSE + "/headset/status", timeout=10) as r:
            beat = json.load(r)
    except urllib.error.HTTPError as e:
        sys.exit("no headset status on the house server yet (HTTP %d): the headset has not posted since the server "
                 "started, or HEARTBEAT is off / not installed (v0.76+)" % e.code)
    doc, rev = _get()
    cmp_ = compare(beat, doc)
    screens = beat.get("screens", [])
    if sid is not None:
        screens = [s for s in screens if str(s.get("id")) == str(sid)]
        cmp_ = {k: v for k, v in cmp_.items() if str(k) == str(sid)}
    applied = (beat.get("house") or {}).get("applied")
    out = {"age_s": beat.get("ageS"), "fresh": (beat.get("ageS") or 1e9) <= STALE_S, "received": beat.get("received"),
           "version": beat.get("v"), "fps": beat.get("fps"), "battery": beat.get("battery"),
           "house_rev": int(rev) if rev and str(rev).isdigit() else rev, "headset_applied_rev": applied,
           "in_sync": str(applied) == str(rev), "screens": screens, "vs_house": cmp_,
           "errors": beat.get("errors"), "events": beat.get("events")}
    print(json.dumps(out, indent=2))


# --- DRAW primitive: house.marks[] drawn by the headset over everything (work-0e contract, K1 pt2) ---
def _marks(doc):
    m = doc.get("marks")
    return m if isinstance(m, list) else []


def _now():
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def draw(spec):
    """Add or replace one mark. spec is a dict matching work-0e's house.marks[] contract; a draw with
    an existing id replaces it, and a fresh ts re-pops it."""
    import uuid
    doc, rev = _get()
    spec.setdefault("id", "m-" + uuid.uuid4().hex[:8])
    spec.setdefault("kind", "label")
    spec["ts"] = _now()
    marks = [m for m in _marks(doc) if m.get("id") != spec["id"]]   # replace same id
    marks.append(spec)
    doc["marks"] = marks
    res = _put(doc, rev)
    print(json.dumps({"ok": True, "id": spec["id"], "kind": spec["kind"],
                      "marks": [m["id"] for m in marks], "rev": res.get("rev")}))


def unmark(mid=None):
    doc, rev = _get()
    marks = [m for m in _marks(doc) if m.get("id") != mid] if mid else []   # drop one, or all
    doc["marks"] = marks
    res = _put(doc, rev)
    print(json.dumps({"ok": True, "marks": [m["id"] for m in marks], "rev": res.get("rev")}))


def label(screen, x, y, text, color="gold"):
    draw({"kind": "label", "screen": int(screen), "x": float(x), "y": float(y),
          "text": text, "color": color})


def arrow(screen, x, y, dx, dy, color="gold"):
    draw({"kind": "arrow", "screen": int(screen), "x": float(x), "y": float(y),
          "dx": float(dx), "dy": float(dy), "color": color})


def move(sid, direction, amount=None):
    axis, sign = _DIRS.get((direction or "").lower(), (None, None))
    if not axis:
        sys.exit("direction must be left|right|up|down")
    step = min(float(amount) if amount else NUDGE_DEFAULT, NUDGE_MAX)   # cap: a nudge stays small
    doc, rev = _get()
    s = next((x for x in doc.get("screens", []) if str(x.get("id")) == str(sid)), None)
    if not s:
        sys.exit("no screen %s" % sid)
    before = (s.get("x"), s.get("y"))
    s[axis] = round((s.get(axis) or 0.0) + sign * step, 3)             # ONLY the one axis moves
    w = _wall(doc, s.get("wall"))
    if axis == "x" and w and w.get("w"):                               # clamp x to the wall
        lim = w["w"] / 2 - (s.get("w") or 0) / 2 - 0.03
        s["x"] = 0.0 if lim <= 0 else max(-lim, min(lim, s["x"]))
    res = _put(doc, rev)
    print(json.dumps({"ok": True, "screen": sid, "axis": axis, "step": sign * step,
                      "from": before, "to": (s.get("x"), s.get("y")), "rev": res.get("rev")}))


def _sel_list(doc):
    sel = doc.get("selected")
    if isinstance(sel, dict):          # legacy single object -> list
        return [sel]
    return sel if isinstance(sel, list) else []


def select(sid):
    doc, rev = _get()
    if not any(str(x.get("id")) == str(sid) for x in doc.get("screens", [])):
        sys.exit("no screen %s" % sid)
    sel = _sel_list(doc)
    now = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    for e in sel:                      # already selected -> refresh ts (re-flash the outline)
        if str(e.get("id")) == str(sid):
            e["ts"] = now
            break
    else:
        sel.append({"id": int(sid), "ts": now})   # new -> append in selection order
    doc["selected"] = sel
    res = _put(doc, rev)
    print(json.dumps({"ok": True, "selected": [e["id"] for e in sel], "rev": res.get("rev")}))


def clear(sid=None):
    doc, rev = _get()
    sel = _sel_list(doc)
    sel = [e for e in sel if str(e.get("id")) != str(sid)] if sid else []   # clear one, or all
    doc["selected"] = sel
    res = _put(doc, rev)
    print(json.dumps({"ok": True, "selected": [e["id"] for e in sel], "rev": res.get("rev")}))


def _apps_token():
    p = os.path.expanduser("~/.local/share/pinpin-apps/token")
    try:
        return open(p).read().strip()
    except OSError:
        pass
    r = subprocess.run(["sudo", "-n", "cat", "/root/.config/pinpin-apps/token"],
                       capture_output=True, timeout=5)
    return r.stdout.decode().strip() if r.returncode == 0 else ""


def add_card(title, details=""):
    # Append to work-ad's live board (docs/captain/KANBAN.md is generated from it, so file appends
    # would be overwritten). Cards spoken via the headset are attributed to the user.
    body = json.dumps({"title": title, "details": details, "by": "user (headset)"}).encode()
    req = urllib.request.Request(KANBAN_API, data=body, method="POST",
                                 headers={"Authorization": "Bearer " + _apps_token(),
                                          "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=10) as r:
        card = json.load(r)
    print(json.dumps({"ok": True, "card": card.get("id"), "title": title}))


def main(argv):
    if not argv:
        sys.exit(__doc__)
    cmd, rest = argv[0], argv[1:]
    if cmd == "house_info":
        house_info()
    elif cmd == "move":
        move(rest[0], rest[1], rest[2] if len(rest) > 2 else None)
    elif cmd == "select":
        select(rest[0])
    elif cmd == "clear":
        clear(rest[0] if rest else None)   # clear <id> drops one; clear drops all
    elif cmd == "status":               # status [screen_id]
        status(rest[0] if rest else None)
    elif cmd == "add_card":
        add_card(rest[0], rest[1] if len(rest) > 1 else "")
    elif cmd == "draw":                 # draw '<json mark>'  (full control, all kinds)
        draw(json.loads(rest[0]))
    elif cmd == "label":                # label <screen> <x> <y> <text> [color]
        label(rest[0], rest[1], rest[2], rest[3], rest[4] if len(rest) > 4 else "gold")
    elif cmd == "arrow":                # arrow <screen> <x> <y> <dx> <dy> [color]
        arrow(rest[0], rest[1], rest[2], rest[3], rest[4], rest[5] if len(rest) > 5 else "gold")
    elif cmd == "unmark":               # unmark <id> drops one; unmark drops all
        unmark(rest[0] if rest else None)
    else:
        sys.exit("unknown command: %s" % cmd)


if __name__ == "__main__":
    main(sys.argv[1:])
