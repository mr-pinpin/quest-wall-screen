#!/usr/bin/env python3
"""room_tools.py — room actions for the captain (work-df) to run from the VPS.

Usage:
  room_tools.py house_info
  room_tools.py move <id> <left|right|up|down> [amount_m]     # small nudge, default 0.12 m
  room_tools.py select <id> | room_tools.py clear             # gold outline (needs work-c4 renderer)
  room_tools.py add_card "<title>" "<details>"                # append to docs/captain/KANBAN.md

House server: PINPIN_HOUSE_BASE (default the Mini over Tailscale). Writes use If-Match + SHA256.
TEST writes against the SCRATCH house (:57801), never the real house, until the user asks.
"""
import json, os, sys, hashlib, urllib.request, subprocess, time

HOUSE = os.environ.get("PINPIN_HOUSE_BASE", "http://100.64.243.30:57800")   # Mini, Tailscale
KANBAN = os.environ.get("PINPIN_KANBAN", "/root/work/quest-wall-screen/docs/captain/KANBAN.md")
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


def select(sid):
    doc, rev = _get()
    if not any(str(x.get("id")) == str(sid) for x in doc.get("screens", [])):
        sys.exit("no screen %s" % sid)
    doc["selected"] = {"id": int(sid), "ts": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
    res = _put(doc, rev)
    print(json.dumps({"ok": True, "selected": sid, "rev": res.get("rev"),
                      "note": "work-c4 renderer draws the gold outline + number from house.selected"}))


def clear():
    doc, rev = _get()
    doc.pop("selected", None)
    res = _put(doc, rev)
    print(json.dumps({"ok": True, "selected": None, "rev": res.get("rev")}))


def add_card(title, details=""):
    line = "- [ ] **%s** — %s  _(via headset %s)_\n" % (
        title, details, time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))
    # the repo is root-owned; append with sudo (work-df runs this as pinpin-claude with sudo)
    p = subprocess.run(["sudo", "-n", "tee", "-a", KANBAN], input=line.encode(),
                       capture_output=True, timeout=10)
    if p.returncode != 0:
        sys.exit("could not append to %s: %s" % (KANBAN, p.stderr.decode()))
    print(json.dumps({"ok": True, "kanban": KANBAN, "card": title}))


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
        clear()
    elif cmd == "add_card":
        add_card(rest[0], rest[1] if len(rest) > 1 else "")
    else:
        sys.exit("unknown command: %s" % cmd)


if __name__ == "__main__":
    main(sys.argv[1:])
