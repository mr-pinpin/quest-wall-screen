#!/usr/bin/env python3
"""Screen-scoped computer-use tools, as an MCP stdio server for the claude CLI.

The assistant backend launches `claude -p ... --mcp-config screen-mcp.json --allowedTools mcp__screen__*`. This
server is that MCP server. It is pinned to ONE screen via the PINPIN_SCREEN env var, so the model can only ever
act on that screen's CGVirtualDisplay — never another display, never the main/built-in one (LiveScreenController
rejects it). Transport: newline-delimited JSON-RPC 2.0 over stdio (Claude Code's stdio MCP transport).

Python stdlib only. Runs on the Mac as a child of PinPin Host (inherits Screen Recording + Accessibility).
"""
import os, sys, json, base64, pathlib, time, urllib.request, urllib.error

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import pinpin_assistant as pa

SCREEN = os.environ.get("PINPIN_SCREEN")
CTL = pa.LiveScreenController()
APPS = pa.APPS_BASE  # mini-apps server (work-ad)


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
