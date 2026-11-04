#!/usr/bin/env python3
"""Offline test for the per-screen assistant backend (no model, no Mac).

Drives the full turn loop and the HTTP/SSE layer against a mock model and a mock screen, and checks:
  - a turn streams start -> text -> action -> done;
  - "open example.com" produces an open_url action on the TARGET screen only (scoping);
  - the main/built-in display is refused;
  - history persists per screen and survives a reload;
  - every action is logged to the per-screen action log;
  - /api/ask answers (text, and json when asked).
Hardware-free: part of bench/run_offline_tests.sh.
"""
import os, sys, json, tempfile, threading, urllib.request, pathlib

# Point data at a throwaway dir BEFORE importing the module (it binds DATA_DIR at import time).
_TMP = tempfile.mkdtemp(prefix="pinpin-assist-test-")
os.environ["PINPIN_ASSIST_DIR"] = _TMP
os.environ.setdefault("PINPIN_ASSIST_MODEL", "sonnet")

HERE = pathlib.Path(__file__).resolve().parent
for cand in (HERE, HERE.parent / "assistant"):   # scratchpad layout, then repo layout (bench/ -> ../assistant)
    if (cand / "pinpin_assistant.py").exists():
        sys.path.insert(0, str(cand)); break
import pinpin_assistant as pa  # noqa: E402

from http.server import ThreadingHTTPServer  # noqa: E402

FAILS = []
def check(cond, msg):
    print(("ok  " if cond else "FAIL") + " " + msg)
    if not cond:
        FAILS.append(msg)


class MockScreen(pa.ScreenController):
    """Records every call with its screen id; screen 1 is the forbidden main display."""
    def __init__(self):
        self.calls = []
    def resolve(self, screen):
        if str(screen) == "1":
            raise pa.ScreenError("refusing to act on the main/built-in display")
        return {"screen": str(screen), "display": 50 + int(screen), "w": 1920, "h": 1200, "scale": 2, "main": False}
    def open_url(self, screen, url):
        self.resolve(screen); self.calls.append(("open_url", str(screen), url))
    def open_app(self, screen, bundle):
        self.resolve(screen); self.calls.append(("open_app", str(screen), bundle))
    def click(self, screen, u, v):
        self.resolve(screen); self.calls.append(("click", str(screen), u, v))
    def type_text(self, screen, text):
        self.resolve(screen); self.calls.append(("type", str(screen), text))


def collect(assistant, screen, text, think=False):
    evs = []
    assistant.handle_message(screen, text, think, evs.append)
    return evs


def main():
    scr = MockScreen()
    a = pa.Assistant(pa.MockBackend(), scr)

    # 1) open on screen 7
    evs = collect(a, "7", "open example.com")
    types = [e["type"] for e in evs]
    check(types[0] == "start", "turn starts with start event")
    check(types[-1] == "done", "turn ends with done event")
    check(evs[-1]["stopReason"] == "end", "stopReason end on success")
    check(any(e["type"] == "text" for e in evs), "at least one text delta")
    acts = [e for e in evs if e["type"] == "action" and e.get("status") == "ok"]
    check(len(acts) == 1 and acts[0]["action"] == "open_url", "one successful open_url action")
    check(scr.calls == [("open_url", "7", "https://example.com")], f"open_url hit screen 7 only: {scr.calls}")
    check(all("turn" in e for e in evs), "every event carries a turn id")
    begin = [e for e in evs if e["type"] == "action" and e.get("status") == "begin"]
    check(begin and begin[0].get("label") == "opening example.com", f"action has a short label: {begin and begin[0].get('label')}")

    # 2) scoping: nothing landed on any other screen
    check(all(c[1] == "7" for c in scr.calls), "no action touched a screen other than 7")

    # 3) main display refused
    scr.calls.clear()
    evs = collect(a, "1", "open example.com")
    err = [e for e in evs if e["type"] == "action" and e.get("status") == "error"]
    check(len(err) == 1 and "main" in err[0]["detail"], "open on main display refused with error action")
    check(scr.calls == [], "no call recorded for the refused main display")

    # 4) history persists and reloads
    s = a.store.load("7")
    check(s["turns"] == 1 and len(s["messages"]) == 2, "screen 7 history has one user+one assistant turn")
    fresh = pa.SessionStore(_TMP)
    check(fresh.load("7")["turns"] == 1, "history survives a fresh store (persisted to disk)")

    # 5) action log written
    log = a.store.actions("7")
    check('"open_url"' in log and '"status": "ok"' in log, "per-screen action log records open_url ok")

    # 6) /api/ask
    r = a.ask("recipes", "7", "two dinner ideas")
    check(r.get("text", "").startswith("(mock)"), "ask returns text")
    rj = a.ask("recipes", "7", "give me json", want_json=True)
    check(isinstance(rj.get("json"), dict), "ask json:true returns a parsed object")

    # 7) HTTP + SSE end to end
    srv = ThreadingHTTPServer(("127.0.0.1", 0), pa.make_handler(a))
    port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    try:
        req = urllib.request.Request(f"http://127.0.0.1:{port}/session/9/message",
                                     data=json.dumps({"text": "open example.com"}).encode(),
                                     headers={"Content-Type": "application/json"})
        raw = urllib.request.urlopen(req, timeout=10).read().decode()
        got = [json.loads(l[6:]) for l in raw.splitlines() if l.startswith("data: ")]
        check(got and got[0]["type"] == "start" and got[-1]["type"] == "done", "SSE stream start..done over HTTP")
        check(any(e["type"] == "action" and e.get("status") == "ok" for e in got), "SSE carried the action event")
        h = json.loads(urllib.request.urlopen(f"http://127.0.0.1:{port}/health", timeout=5).read())
        check(h.get("ok") is True, "GET /health ok")
        hist = json.loads(urllib.request.urlopen(f"http://127.0.0.1:{port}/session/9", timeout=5).read())
        check(hist["turns"] == 1, "GET /session/9 shows the turn")
    finally:
        srv.shutdown()

    print()
    if FAILS:
        print(f"ASSISTANT MOCK TEST: {len(FAILS)} FAILED")
        return 1
    print("ASSISTANT MOCK TEST: all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
