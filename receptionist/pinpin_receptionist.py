#!/usr/bin/env python3
"""pinpin_receptionist.py — the headset receptionist service (VPS side).

Front desk that routes the user's spoken turns into live Claude Code sessions and streams
their replies back to the headset. Runs as pinpin-claude on the VPS.

Security (per work-df):
  - Bound to loopback + the Tailscale interface only, NEVER 0.0.0.0.
  - Every request needs the receptionist bearer token.
  - route() delivers the user's words VERBATIM (prefix "[via headset] "), nothing else.
  - Engineering destinations require a spoken confirm (two-phase: propose -> route).
  - Append-only audit log of every routing: time, speaker, session, text hash.
  - Household/new-user routing is GATED until work-df OKs the account.
"""
import json, os, sys, time, hashlib, threading, socket, subprocess, uuid, queue
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import recept_core as rc

STATE = os.path.expanduser("~/.local/share/pinpin-receptionist")
TOKEN_FILE = os.path.join(STATE, "token")
AUDIT_LOG = os.path.join(STATE, "routing.log")
PORT = int(os.environ.get("PINPIN_RECEPT_PORT", "57850"))
HOUSEHOLD_ENABLED = os.environ.get("PINPIN_RECEPT_HOUSEHOLD", "") == "1"  # gated off by default

# Engineering sessions the user owns. A household speaker may never reach these.
ENGINEERING = True  # all current VPS sessions are the user's engineering sessions

# Hard block-list of session ids (or names) that must never receive traffic — structurally
# enforces gates like "no real routing to work-df until the user/work-df OKs it".
BLOCKED = set(x.strip() for x in os.environ.get("PINPIN_RECEPT_BLOCK", "").split(",") if x.strip())


def _is_blocked(s):
    return bool(s) and (s.get("session_id") in BLOCKED or s.get("name") in BLOCKED)


def _token():
    try:
        return open(TOKEN_FILE).read().strip()
    except OSError:
        return None


def audit(speaker, action, session, text):
    rec = {
        "t": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "speaker": speaker,
        "action": action,
        "session": session,
        "text_sha256": hashlib.sha256((text or "").encode()).hexdigest()[:16],
        "len": len(text or ""),
    }
    with open(AUDIT_LOG, "a") as f:
        f.write(json.dumps(rec) + "\n")
    return rec


# ---- confirm gate: an engineering route must be preceded by a propose for the same target ----
_pending = {}           # session_id -> propose epoch
_PENDING_TTL = 120.0


def propose(session_id):
    _pending[session_id] = time.time()


def confirm_ok(session_id):
    ts = _pending.get(session_id)
    return bool(ts and (time.time() - ts) < _PENDING_TTL)


def clear_pending(session_id):
    _pending.pop(session_id, None)


# ---- the warm haiku receptionist: one persistent `claude -p` stream per conversation ----
HERE = os.path.dirname(os.path.abspath(__file__))
PROMPT_FILE = os.path.join(HERE, "receptionist-prompt.txt")
MCP_CONFIG = os.path.join(HERE, "receptionist-mcp.json")
MODEL = os.environ.get("PINPIN_RECEPT_MODEL", "haiku")
_TOOLS = ("mcp__reception__list_sessions,mcp__reception__propose_route,"
          "mcp__reception__route,mcp__reception__new_session")


class WarmReceptionist:
    """A persistent streaming claude process acting as the front desk for one conversation."""

    def __init__(self):
        self.sid = str(uuid.uuid4())
        self.lock = threading.Lock()
        prompt = open(PROMPT_FILE).read() if os.path.exists(PROMPT_FILE) else "You are a receptionist."
        self.p = subprocess.Popen(
            ["claude", "-p", "--verbose", "--input-format", "stream-json", "--output-format", "stream-json",
             "--include-partial-messages", "--model", MODEL, "--session-id", self.sid,
             "--mcp-config", MCP_CONFIG, "--allowedTools", _TOOLS,
             "--append-system-prompt", prompt],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)

    def turn(self, text):
        """Send one user turn; yield assistant text deltas until the turn completes."""
        with self.lock:
            msg = {"type": "user", "message": {"role": "user", "content": text}}
            self.p.stdin.write(json.dumps(msg) + "\n")
            self.p.stdin.flush()
            while True:
                line = self.p.stdout.readline()
                if not line:
                    return
                try:
                    ev = json.loads(line)
                except ValueError:
                    continue
                t = ev.get("type")
                if t == "stream_event":
                    se = ev.get("event", {})
                    if se.get("type") == "content_block_delta":
                        d = se.get("delta", {})
                        if d.get("type") == "text_delta" and d.get("text"):
                            yield d["text"]
                elif t == "result":
                    return

    def close(self):
        try:
            self.p.stdin.close()
            self.p.terminate()
        except Exception:
            pass


_convs = {}           # conversation_id -> WarmReceptionist
_convs_lock = threading.Lock()


def _get_conv(cid):
    with _convs_lock:
        w = _convs.get(cid)
        if w is None or w.p.poll() is not None:
            w = WarmReceptionist()
            _convs[cid] = w
        return w


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
        try:
            self.wfile.write(("event: %s\ndata: %s\n\n" % (event, json.dumps(data))).encode())
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            raise

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
                                    "sessions": len(rc.list_sessions())})
        if not self._auth():
            return
        if self.path == "/api/sessions":
            return self._json(200, {"sessions": rc.list_sessions(include_self="work-42")})
        self._json(404, {"error": "not found"})

    def do_POST(self):
        if not self._auth():
            return
        b = self._body()
        if self.path == "/api/propose":
            # declare intent to route to `session`; returns a short spoken destination line
            sid = (b.get("session") or "").strip()
            s = rc.resolve(sid)
            if not s:
                return self._json(404, {"error": "no such session", "session": sid})
            if _is_blocked(s):
                audit("user", "blocked-propose", s["session_id"], "")
                return self._json(403, {"error": "that session is not reachable right now"})
            propose(s["session_id"])
            label = ("the captain session" if s["name"] == "work-df" else "the %s session" % s["name"])
            return self._json(200, {
                "session": s["session_id"], "name": s["name"], "topic": s["topic"],
                "confirm_line": "Sending to %s (%s). Okay?" % (label, s["topic"] or s["name"]),
                "requires_yes": ENGINEERING,
            })
        if self.path == "/api/route":
            return self._route(b)
        if self.path == "/api/new":
            return self._new(b)
        if self.path in ("/api/reception/turn", "/api/turn"):
            return self._turn(b)
        self._json(404, {"error": "not found"})

    def _turn(self, b):
        """Headset entry point: one spoken turn to the receptionist brain; stream its voice back."""
        cid = (b.get("conversation_id") or "default").strip()
        text = b.get("text") or ""
        if not text.strip():
            return self._json(400, {"error": "empty text"})
        w = _get_conv(cid)
        self._sse_open()
        try:
            self._sse("conversation", {"conversation_id": cid})
            for chunk in w.turn(text):
                self._sse("delta", {"text": chunk})
            self._sse("done", {"conversation_id": cid})
        except (BrokenPipeError, ConnectionResetError):
            pass

    def _route(self, b):
        sid = (b.get("session") or "").strip()
        text = b.get("text") or ""
        speaker = (b.get("speaker") or "user").strip()
        confirmed = bool(b.get("confirmed"))
        s = rc.resolve(sid)
        if not s:
            return self._json(404, {"error": "no such session", "session": sid})
        if _is_blocked(s):
            audit(speaker, "blocked-route", s["session_id"], text)
            return self._json(403, {"error": "that session is not reachable right now"})
        # speaker safety: only the user may reach engineering sessions
        if speaker != "user":
            audit(speaker, "refused-engineering", s["session_id"], text)
            return self._json(403, {"error": "household speaker may not reach engineering sessions"})
        # confirm gate for engineering targets
        if ENGINEERING and not (confirmed and confirm_ok(s["session_id"])):
            return self._json(409, {"error": "confirm required",
                                    "hint": "POST /api/propose then confirm with a spoken yes"})
        if not text.strip():
            return self._json(400, {"error": "empty text"})
        clear_pending(s["session_id"])
        audit(speaker, "route", s["session_id"], text)
        # deliver verbatim + stream the target's reply as SSE
        self._sse_open()
        try:
            self._sse("routed", {"session": s["session_id"], "name": s["name"]})
            wm = rc.deliver(s["session_id"], text)
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

    def _new(self, b):
        profile = (b.get("profile") or "engineering").strip()
        topic = (b.get("topic") or "").strip()
        speaker = (b.get("speaker") or "user").strip()
        if profile == "household":
            if not HOUSEHOLD_ENABLED:
                audit(speaker, "new-household-gated", "-", topic)
                return self._json(423, {"error": "household profile not provisioned",
                                        "hint": "awaiting work-df approval to create the unprivileged user"})
            # (when enabled) spawn under the unprivileged household user — not reachable yet
            return self._json(501, {"error": "household spawn not implemented"})
        if speaker != "user":
            return self._json(403, {"error": "only the user may start engineering sessions"})
        sid = rc.spawn_engineering(topic)
        audit(speaker, "new-engineering", sid, topic)
        return self._json(200, {"session": sid, "profile": "engineering"})


def _serve_on(addr):
    httpd = ThreadingHTTPServer((addr, PORT), H)
    httpd.serve_forever()


def main():
    binds = ["127.0.0.1"]
    ts = os.environ.get("PINPIN_RECEPT_TAILSCALE_IP") or _tailscale_ip()
    if ts and ts not in binds:
        binds.append(ts)
    threads = []
    for addr in binds:
        t = threading.Thread(target=_serve_on, args=(addr,), daemon=True)
        t.start()
        threads.append(t)
        sys.stderr.write("receptionist listening on %s:%d\n" % (addr, PORT))
    sys.stderr.flush()
    for t in threads:
        t.join()


def _tailscale_ip():
    try:
        import subprocess
        out = subprocess.run(["tailscale", "ip", "-4"], capture_output=True, timeout=5)
        ip = out.stdout.decode().strip().splitlines()[0].strip()
        socket.inet_aton(ip)
        return ip
    except Exception:
        return None


if __name__ == "__main__":
    main()
