#!/usr/bin/env python3
"""Screen-scoped computer-use tools, as an MCP stdio server for the claude CLI.

The assistant backend launches `claude -p ... --mcp-config screen-mcp.json --allowedTools mcp__screen__*`. This
server is that MCP server. It is pinned to ONE screen via the PINPIN_SCREEN env var, so the model can only ever
act on that screen's CGVirtualDisplay — never another display, never the main/built-in one (LiveScreenController
rejects it). Transport: newline-delimited JSON-RPC 2.0 over stdio (Claude Code's stdio MCP transport).

Python stdlib only. Runs on the Mac as a child of PinPin Host (inherits Screen Recording + Accessibility).
"""
import os, sys, json, base64, pathlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import pinpin_assistant as pa

SCREEN = os.environ.get("PINPIN_SCREEN")
CTL = pa.LiveScreenController()

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
