#!/usr/bin/env python3
"""Screen-scoped computer-use tools, as an MCP stdio server for the claude CLI.

The assistant backend launches `claude -p ... --mcp-config screen-mcp.json --allowedTools mcp__screen__*`. This
server is that MCP server. It is pinned to ONE screen via the PINPIN_SCREEN env var, so the model can only ever
act on that screen's CGVirtualDisplay — never another display, never the main/built-in one (LiveScreenController
rejects it). Transport: newline-delimited JSON-RPC 2.0 over stdio (Claude Code's stdio MCP transport).

Python stdlib only. Runs on the Mac as a child of PinPin Host (inherits Screen Recording + Accessibility).
"""
import os, sys, json, base64, pathlib, time, hashlib, urllib.request, urllib.error

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import pinpin_assistant as pa

SCREEN = os.environ.get("PINPIN_SCREEN")
CTL = pa.LiveScreenController()
APPS = pa.APPS_BASE  # mini-apps server (work-ad)
HOUSE = os.environ.get("PINPIN_HOUSE_BASE", "http://192.168.0.50:57800")  # Mini house server (work-c4 contract)


def _house_get():
    with urllib.request.urlopen(HOUSE + "/house", timeout=10) as r:
        return json.load(r), r.headers.get("X-House-Rev")


def _house_put(doc, rev):
    body = json.dumps(doc).encode()
    req = urllib.request.Request(HOUSE + "/house", data=body, method="PUT",
                                 headers={"Content-Type": "application/json", "If-Match": str(rev),
                                          "X-House-SHA256": hashlib.sha256(body).hexdigest()})
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.load(r)


def _place_next_to(doc, cur, new_w):
    # work-c4's rule: same wall, x = cur.x +/- (cur.w/2 + 0.15 + new_w/2), clamp to wall extent (3 cm margin),
    # try right then left; return (x, y) or (None, None) if it doesn't fit.
    wall = next((w for w in doc["walls"] if w.get("id") == cur.get("wall")), None)
    half = (wall.get("w", 0) / 2.0) if wall and wall.get("w") else 1e9
    for sign in (1, -1):
        x = cur["x"] + sign * (cur["w"] / 2 + 0.15 + new_w / 2)
        if abs(x) + new_w / 2 <= half - 0.03:
            return round(x, 3), round(cur.get("y", 0), 3)
    return None, None


def _new_screen(args):
    doc, rev = _house_get()
    cur = next((s for s in doc.get("screens", []) if str(s.get("id")) == str(SCREEN)), None)
    new_w = float(args.get("width_m") or (min(1.2, cur["w"]) if cur else 1.0))
    if args.get("wall"):
        if not any(w.get("id") == args["wall"] and w.get("scene") for w in doc["walls"]):
            return {"error": f"wall {args['wall']} is not a scene-located wall; the headset can only place on those"}
        wall_id, x, y = args["wall"], 0.0, 0.0
    elif cur:
        wall_id = cur["wall"]
        x, y = _place_next_to(doc, cur, new_w)
        if x is None:
            return {"error": "no room on this wall next to the current screen"}
    else:
        return {"error": "no current screen to place next to; name a wall (see house_info)"}
    did = int(doc["nextScreenId"])
    # create the Mac virtual display for this screen on the Air (source.screen == the pinpin-displays id)
    urllib.request.urlopen(urllib.request.Request(f"{pa.DISPLAYS_HTTP}/displays?id={did}&w=1600&h=1000&scale=2",
                                                   data=b"", method="POST"), timeout=8).read()
    h = round(new_w * (1000.0 / 1600.0), 3)  # display is 1600x1000 points
    screen = {"id": did, "role": "extra", "on": 1, "wall": wall_id, "x": x, "y": y,
              "w": round(new_w, 3), "h": h, "source": {"kind": "mac-display", "screen": did}}
    doc["screens"].append(screen)
    doc["nextScreenId"] = did + 1
    try:
        res = _house_put(doc, rev)
    except urllib.error.HTTPError as e:
        urllib.request.urlopen(urllib.request.Request(f"{pa.DISPLAYS_HTTP}/remove?id={did}", data=b"", method="POST"), timeout=5).read()
        if e.code == 409:
            return {"error": "the house changed during the write (409) — try again"}
        return {"error": f"house PUT failed {e.code}: {e.read().decode('utf-8','replace')[:200]}"}
    if args.get("app"):
        _apps("POST", f"/api/screens/{did}/app", {"app": args["app"]})
    elif args.get("url"):
        _apps("POST", f"/api/screens/{did}/open", {"url": args["url"]})
    return {"ok": True, "screen": did, "wall": wall_id, "x": x, "w": round(new_w, 3), "h": h, "rev": res.get("rev")}


def _close_screen(args):
    doc, rev = _house_get()
    sid = args.get("screen", SCREEN)
    s = next((s for s in doc.get("screens", []) if str(s.get("id")) == str(sid)), None)
    if not s:
        return {"error": f"no screen {sid} in the house"}
    s["on"] = 0
    res = _house_put(doc, rev)
    return {"ok": True, "screen": sid, "closed": True, "rev": res.get("rev")}


def _house_info():
    doc, rev = _house_get()
    walls = [{"id": w["id"], "w": w.get("w"), "h": w.get("h")}
             for w in doc.get("walls", []) if w.get("kind") == "wall" and w.get("scene")]
    screens = [{"id": s["id"], "wall": s.get("wall"), "x": s.get("x"), "w": s.get("w"),
                "on": s.get("on"), "source": s.get("source")} for s in doc.get("screens", [])]
    return {"rev": rev, "nextScreenId": doc.get("nextScreenId"), "placeable_walls": walls, "screens": screens}


def _apps(method, path, body=None, timeout=90):
    data = json.dumps(body).encode() if body is not None else None
    headers = {"Content-Type": "application/json"} if data is not None else {}
    req = urllib.request.Request(APPS + path, data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.load(r)
    except urllib.error.HTTPError as e:
        return {"error": f"HTTP {e.code}", "body": e.read().decode("utf-8", "replace")[:800]}
    except Exception as e:
        return {"error": str(e)}


def _authoring_guide():
    g = _apps("GET", "/api/authoring", timeout=10)   # live copy if the apps server serves it
    if isinstance(g, dict) and g.get("text"):
        return g["text"]
    if isinstance(g, str):
        return g
    return _AUTHORING_FALLBACK


_AUTHORING_FALLBACK = r"""Writing a mini-app (build via the apps tools):
- put_app(id, files, message, say?): id is [a-z0-9-_] up to 48 chars and must equal manifest.id and the folder.
  files is {"index.html":"...","manifest.json":"..."}. It commits, then renders in headless Chrome and returns
  check.errors (JS/console errors) and check.text (what's on screen). If check.errors is non-empty, fix and
  put_app again. If check.text isn't what the user asked for, change the app and put_app again.
- assign_app(app): put the finished app on THIS screen.
manifest.json: {"id":"timer","name":"Kitchen timer","description":"one line",
  "size":{"aspect":"16:10","preferred_px":[1920,1200],"distance_m":1.5},
  "permissions":{"fetch":[],"voice":true,"ask":false,"storage":true},"config":{}}
index.html rules: one file, plain HTML/CSS/JS, no build, no CDNs, must work offline. DON'T include the SDK
(pinpin.css/pinpin.js are injected); start logic with pinpin.ready.then(...). Screen is 1-3 m away: FEW WORDS,
HUGE TYPE (rem ~ 1/30 width; body 1rem, nothing < 0.6rem, key number uses class pp-huge). <= ~12 lines on screen,
nothing scrolls (pinpin.fit(el)). No mouse/keyboard: input is voice via pinpin.onVoice(text => ...); match loosely;
always show a .pp-hint of what can be said. State via pinpin.store.set/get (survives reloads). Dark bg; colours from
--pp-accent/--pp-good/--pp-bad/--pp-muted. Escape injected text with pinpin.escape. pinpin.ask takes 10-30 s, so
show a fallback first. Example timer:
<!doctype html><html><head><meta charset="utf-8"><title>Timer</title>
<style>#t{font-size:7rem;font-weight:800;text-align:center}</style></head><body class="pp-center">
<div id="t">0:00</div><div class="pp-hint">Say <b>5 minutes</b> | <b>stop</b></div>
<script>let end=0;
pinpin.onVoice(t=>{const m=t.match(/(\d+)\s*(min|minute)/i);
 if(m){end=Date.now()+m[1]*60000;pinpin.store.set('end',end);}
 else if(/stop|cancel/i.test(t)){end=0;pinpin.store.set('end',0);}});
pinpin.ready.then(()=>{end=pinpin.store.get('end',0);
 setInterval(()=>{const s=Math.max(0,Math.round((end-Date.now())/1000));
  document.getElementById('t').textContent=Math.floor(s/60)+':'+String(s%60).padStart(2,'0');},250);});
</script></body></html>"""

TOOLS = [
    {"name": "screenshot", "description": "Capture the current screen as a PNG image so you can see it.",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "move", "description": "Move the pointer to u,v (0..10000 across this screen, 0,0 = top-left).",
     "inputSchema": {"type": "object", "properties": {"u": {"type": "number"}, "v": {"type": "number"}},
                     "required": ["u", "v"]}},
    {"name": "click", "description": "Click at u,v (0..10000 across this screen).",
     "inputSchema": {"type": "object", "properties": {"u": {"type": "number"}, "v": {"type": "number"}},
                     "required": ["u", "v"]}},
    {"name": "scroll", "description": "Scroll at u,v. dy>0 reveals content below (scrolls down); dx>0 scrolls right. ~120 per notch.",
     "inputSchema": {"type": "object", "properties": {"u": {"type": "number"}, "v": {"type": "number"},
                     "dy": {"type": "number"}, "dx": {"type": "number"}}, "required": ["u", "v", "dy"]}},
    {"name": "type", "description": "Type text into the focused window on this screen.",
     "inputSchema": {"type": "object", "properties": {"text": {"type": "string"}}, "required": ["text"]}},
    {"name": "key", "description": "Press a key combo, e.g. 'return', 'cmd+l', 'cmd+t'.",
     "inputSchema": {"type": "object", "properties": {"combo": {"type": "string"}}, "required": ["combo"]}},
    {"name": "open_url", "description": "Navigate this screen's browser window to a URL (it lands on this screen).",
     "inputSchema": {"type": "object", "properties": {"url": {"type": "string"}}, "required": ["url"]}},
    # App building (make this screen a custom mini-app). Read authoring_guide first.
    {"name": "authoring_guide", "description": "How to write a PinPin mini-app. Read this before building one.",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "list_apps", "description": "List existing mini-apps (id, name).",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "get_app", "description": "Read an app's files and manifest.",
     "inputSchema": {"type": "object", "properties": {"id": {"type": "string"}}, "required": ["id"]}},
    {"name": "put_app",
     "description": "Create/update an app and render-check it. Returns check.errors and check.text; if errors are "
                    "non-empty or the text is wrong, fix and call again. files is {filename: content}. For a NEW "
                    "app pass create:true (409 if the id is taken). For an EDIT pass base:<the app's current "
                    "commit from get_app> (409 if it changed since; then get_app again and retry).",
     "inputSchema": {"type": "object", "properties": {
         "id": {"type": "string"}, "files": {"type": "object"}, "message": {"type": "string"},
         "say": {"type": "array", "items": {"type": "string"}},
         "create": {"type": "boolean"}, "base": {"type": "string"}}, "required": ["id", "files", "message"]}},
    {"name": "assign_app", "description": "Show an app on THIS screen.",
     "inputSchema": {"type": "object", "properties": {"app": {"type": "string"}}, "required": ["app"]}},
    # House tools: add / place / close wall screens in the house (so "put a weather screen next to this one" works).
    {"name": "house_info", "description": "List the house's placeable (scene-located) walls and existing screens.",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "new_screen",
     "description": "Create a new wall screen. By default it goes next to THIS screen on the same wall; or pass a "
                    "wall id (from house_info). Optionally put an app or url on it. Returns the new screen id.",
     "inputSchema": {"type": "object", "properties": {
         "wall": {"type": "string"}, "width_m": {"type": "number"},
         "app": {"type": "string"}, "url": {"type": "string"}}}},
    {"name": "close_screen", "description": "Turn a screen off (reversible). Defaults to THIS screen.",
     "inputSchema": {"type": "object", "properties": {"screen": {"type": "number"}}}},
]


def _shot(s):
    """A screenshot content block for screen s, or an error note if capture failed."""
    try:
        png, w, h = CTL.screenshot(s)
        return [{"type": "text", "text": f"[screen {s} now, {w}x{h} backing px]"},
                {"type": "image", "data": base64.b64encode(png).decode(), "mimeType": "image/png"}]
    except Exception as e:
        return [{"type": "text", "text": f"[screenshot after action failed: {e}]"}]


def call(name, args):
    s = SCREEN
    if s is None:
        raise RuntimeError("PINPIN_SCREEN not set")
    if name == "screenshot":
        return {"content": _shot(s)}
    # App-building tools (no screen capture; they return JSON the model reads to iterate).
    if name == "authoring_guide":
        return {"content": [{"type": "text", "text": _authoring_guide()}]}
    if name == "list_apps":
        return {"content": [{"type": "text", "text": json.dumps(_apps("GET", "/api/apps"))}]}
    if name == "get_app":
        return {"content": [{"type": "text", "text": json.dumps(_apps("GET", "/api/apps/" + args["id"]))}]}
    if name == "put_app":
        body = {"files": args["files"], "message": args.get("message", "assistant edit"),
                "by": "assistant", "check": True}
        if args.get("say"):
            body["say"] = args["say"]
        if args.get("create"):
            body["create"] = True
        if args.get("base"):
            body["base"] = args["base"]
        res = _apps("PUT", "/api/apps/" + args["id"], body)
        return {"content": [{"type": "text", "text": json.dumps(res)}]}
    if name == "assign_app":
        res = _apps("POST", f"/api/screens/{s}/app", {"app": args["app"]})
        return {"content": [{"type": "text", "text": json.dumps(res)}]}
    if name == "house_info":
        return {"content": [{"type": "text", "text": json.dumps(_house_info())}]}
    if name == "new_screen":
        return {"content": [{"type": "text", "text": json.dumps(_new_screen(args))}]}
    if name == "close_screen":
        return {"content": [{"type": "text", "text": json.dumps(_close_screen(args))}]}
    # Mutating actions. verify-after-act (work-df): every action returns a fresh screenshot of the TARGET screen,
    # so the model confirms the real result from the pixels instead of trusting the call returned.
    if name == "move":
        CTL.move(s, args["u"], args["v"]); msg = f"moved to {args['u']},{args['v']}"
    elif name == "click":
        CTL.click(s, args["u"], args["v"]); msg = f"clicked {args['u']},{args['v']}"
    elif name == "scroll":
        CTL.scroll(s, args["u"], args["v"], args["dy"], args.get("dx", 0)); msg = "scrolled"
    elif name == "type":
        CTL.type_text(s, args["text"]); msg = "typed"
    elif name == "key":
        CTL.key(s, args["combo"]); msg = "key " + args["combo"]
    elif name == "open_app":
        CTL.open_app(s, args["bundle"]); msg = "opened " + args["bundle"]
    elif name == "open_url":
        CTL.open_url(s, args["url"]); msg = "opened " + args["url"]
    elif name == "place_window":
        CTL.place_window(s, args["app"]); msg = "placed " + args["app"]
    else:
        raise RuntimeError("unknown tool " + name)
    time.sleep(0.6)  # let the UI settle before the confirming screenshot
    return {"content": [{"type": "text", "text": msg + " — verify from the screenshot:"}] + _shot(s)}


def _ok(msg):
    return {"content": [{"type": "text", "text": msg}]}


def handle(msg):
    mid = msg.get("id")
    method = msg.get("method")
    if method == "initialize":
        return {"jsonrpc": "2.0", "id": mid, "result": {
            "protocolVersion": "2024-11-05",
            "capabilities": {"tools": {}},
            "serverInfo": {"name": "pinpin-screen", "version": "1.0"}}}
    if method == "notifications/initialized":
        return None
    if method == "tools/list":
        return {"jsonrpc": "2.0", "id": mid, "result": {"tools": TOOLS}}
    if method == "tools/call":
        p = msg.get("params", {})
        try:
            res = call(p.get("name"), p.get("arguments", {}) or {})
            return {"jsonrpc": "2.0", "id": mid, "result": res}
        except Exception as e:
            return {"jsonrpc": "2.0", "id": mid,
                    "result": {"isError": True, "content": [{"type": "text", "text": "error: " + str(e)}]}}
    if mid is not None:
        return {"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": "method not found"}}
    return None


def main():
    out = sys.stdout
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            continue
        resp = handle(msg)
        if resp is not None:
            out.write(json.dumps(resp) + "\n"); out.flush()


if __name__ == "__main__":
    main()
