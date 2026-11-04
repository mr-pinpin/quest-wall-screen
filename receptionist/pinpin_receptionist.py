#!/usr/bin/env python3
"""pinpin_receptionist.py — the headset receptionist service (VPS side).

Simple front desk: everything the user says is relayed, VERBATIM, to the captain session
(work-df) by default. work-df is the single entry point and delegates non-engineering topics
itself (spawn_session.sh). The user can say "talk to work-c4" to switch the destination for
this chat; otherwise there is no "who's speaking" and no per-message confirm.

Security (per work-df):
  - Bound to loopback + the Tailscale interface only, NEVER 0.0.0.0.
  - Every request needs the receptionist bearer token.
  - Relays the user's words VERBATIM, prefix "[via headset] " — never rewritten.
  - Append-only audit log of every relay/switch: time, session, text hash.
"""
import json, os, re, sys, time, hashlib, threading, socket
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import recept_core as rc

STATE = os.path.expanduser("~/.local/share/pinpin-receptionist")
TOKEN_FILE = os.path.join(STATE, "token")
AUDIT_LOG = os.path.join(STATE, "routing.log")
PORT = int(os.environ.get("PINPIN_RECEPT_PORT", "57850"))
DEFAULT_TARGET = os.environ.get("PINPIN_RECEPT_DEFAULT", "work-df")   # the captain, single entry point

# Hard block-list of session ids/names that must never receive traffic (used in testing to make
# work-df unreachable until the first-real-message gate is cleared).
BLOCKED = set(x.strip() for x in os.environ.get("PINPIN_RECEPT_BLOCK", "").split(",") if x.strip())

_target = {}            # conversation_id -> current destination session name
_target_lock = threading.Lock()

# "talk to / switch to / connect me to / go to <name>"
_SWITCH_RE = re.compile(
    r'\b(?:talk|speak|switch|connect(?:\s+me)?|change|go)\s+(?:to|with)\s+(.{1,40})', re.I)


def _token():
    try:
        return open(TOKEN_FILE).read().strip()
    except OSError:
        return None


def _norm(s):
    return re.sub(r'[^a-z0-9]', '', (s or '').lower())


def _is_blocked(s):
    return bool(s) and (s.get("session_id") in BLOCKED or s.get("name") in BLOCKED)


def audit(action, session, text):
    rec = {"t": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
           "action": action, "session": session,
           "text_sha256": hashlib.sha256((text or "").encode()).hexdigest()[:16],
           "len": len(text or "")}
    with open(AUDIT_LOG, "a") as f:
        f.write(json.dumps(rec) + "\n")
    return rec


def detect_switch(text):
    """If the user asked to switch destination and names a LIVE session, return that name, else None."""
    m = _SWITCH_RE.search(text or "")
    if not m:
        return None
    tail = _norm(m.group(1))
    if not tail:
        return None
    # longest session-name match that appears in the tail (normalised)
    best = None
    for s in rc.list_sessions():
        n = s.get("name")
        if n and _norm(n) in tail:
            if best is None or len(_norm(n)) > len(_norm(best)):
                best = n
    return best


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def _auth(self):
        want = _token()
        got = self.headers.get("Authorization", "")
        if got.startswith("Bearer "):
            got = got[7:]
        if not want or got != want:
            self._json(401, {"error": "unauthorized"})
            return False
        return True

    def _json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _sse_open(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "close")
        self.end_headers()

    def _sse(self, event, data):
        self.wfile.write(("event: %s\ndata: %s\n\n" % (event, json.dumps(data))).encode())
        self.wfile.flush()

    def _body(self):
        n = int(self.headers.get("Content-Length", "0") or "0")
        if not n:
            return {}
        try:
            return json.loads(self.rfile.read(n).decode())
        except ValueError:
            return {}

    def do_GET(self):
        if self.path == "/health":
            return self._json(200, {"ok": True, "service": "pinpin-receptionist",
                                    "default": DEFAULT_TARGET, "sessions": len(rc.list_sessions())})
        if not self._auth():
            return
        if self.path == "/api/sessions":
            return self._json(200, {"sessions": rc.list_sessions(include_self="work-42")})
        self._json(404, {"error": "not found"})

    def do_POST(self):
        if not self._auth():
            return
        b = self._body()
        if self.path in ("/api/reception/turn", "/api/turn"):
            return self._turn(b)
        if self.path == "/api/spawn":
            return self._spawn(b)
        self._json(404, {"error": "not found"})

    def _turn(self, b):
        """One spoken turn. Switch destination if asked; otherwise relay VERBATIM to the current
        target (work-df by default) and stream the reply back."""
        cid = (b.get("conversation_id") or "default").strip()
        text = b.get("text") or ""
        if not text.strip():
            return self._json(400, {"error": "empty text"})

        switch = detect_switch(text)
        if switch:
            s = rc.resolve(switch)
            if s and not _is_blocked(s):
                with _target_lock:
                    _target[cid] = switch
                audit("switch", s["session_id"], "")
                self._sse_open()
                self._sse("switched", {"name": switch, "topic": s.get("topic")})
                self._sse("delta", {"text": "Okay — you're now talking to %s." % switch})
                self._sse("done", {"name": switch})
                return

        with _target_lock:
            name = _target.get(cid, DEFAULT_TARGET)
        s = rc.resolve(name)
        self._sse_open()
        if not s:
            self._sse("error", {"error": "destination %s is not live" % name})
            return
        if _is_blocked(s):
            audit("blocked", s["session_id"], text)
            self._sse("error", {"error": "that session is not reachable right now"})
            return
        audit("route", s["session_id"], text)
        try:
            self._sse("routed", {"session": s["session_id"], "name": s["name"]})
            wm = rc.deliver(s["session_id"], text)          # VERBATIM, prefix "[via headset] "
            for chunk in rc.tail_reply(s["session_id"], wm):
                self._sse("delta", {"text": chunk})
            self._sse("done", {"session": s["session_id"]})
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as e:  # noqa
            try:
                self._sse("error", {"error": str(e)})
            except Exception:
                pass

    def _spawn(self, b):
        """Start a new engineering session (used by work-df / spawn_session.sh)."""
        topic = (b.get("topic") or "").strip()
        brief = (b.get("brief") or topic or "New session from the receptionist.").strip()
        sid = rc.spawn_engineering(brief)
        audit("spawn", sid, topic)
        return self._json(200, {"session": sid})


def _serve_on(addr):
    ThreadingHTTPServer((addr, PORT), H).serve_forever()


def _tailscale_ip():
    try:
        import subprocess
        out = subprocess.run(["tailscale", "ip", "-4"], capture_output=True, timeout=5)
        ip = out.stdout.decode().strip().splitlines()[0].strip()
        socket.inet_aton(ip)
        return ip
    except Exception:
        return None


def main():
    binds = ["127.0.0.1"]
    ts = os.environ.get("PINPIN_RECEPT_TAILSCALE_IP") or _tailscale_ip()
    if ts and ts not in binds:
        binds.append(ts)
    threads = []
    for addr in binds:
        t = threading.Thread(target=_serve_on, args=(addr,), daemon=True)
        t.start(); threads.append(t)
        sys.stderr.write("receptionist listening on %s:%d (default -> %s)\n" % (addr, PORT, DEFAULT_TARGET))
    sys.stderr.flush()
    for t in threads:
        t.join()


if __name__ == "__main__":
    main()
