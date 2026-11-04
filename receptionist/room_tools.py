#!/usr/bin/env python3
"""room_tools.py — room actions for the captain (work-df) to run from the VPS.

Usage:
  room_tools.py house_info
  room_tools.py move <id> <left|right|up|down> [amount_m]     # small nudge, default 0.12 m
  room_tools.py select <id> | room_tools.py clear [id]         # gold outline list (work-0e renderer, v0.66)
  room_tools.py add_card "<title>" "<details>"                # append to docs/captain/KANBAN.md

House server: PINPIN_HOUSE_BASE (default the Mini over Tailscale). Writes use If-Match + SHA256.
TEST writes against the SCRATCH house (:57801), never the real house, until the user asks.
"""
import json, os, sys, hashlib, urllib.request, subprocess, time

HOUSE = os.environ.get("PINPIN_HOUSE_BASE", "http://100.64.243.30:57800")   # Mini, Tailscale
KANBAN_API = os.environ.get("PINPIN_KANBAN_API", "http://100.64.243.30:57830/api/kanban")  # work-ad's board
NUDGE_DEFAULT = 0.12      # a nudge is a small fixed step
NUDGE_MAX = 0.30
_DIRS = {"left": ("x", -1), "right": ("x", 1), "up": ("y", 1), "down": ("y", -1)}


def _get():
    with urllib.request.urlopen(HOUSE + "/house", timeout=10) as r:
        return json.load(r), r.headers.get("X-House-Rev")


def _put(doc, rev):
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
           "selected": doc.get("selected"), "placeable_walls": walls, "screens": screens}
    print(json.dumps(out, indent=2))


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
    elif cmd == "add_card":
        add_card(rest[0], rest[1] if len(rest) > 1 else "")
    else:
        sys.exit("unknown command: %s" % cmd)


if __name__ == "__main__":
    main(sys.argv[1:])
