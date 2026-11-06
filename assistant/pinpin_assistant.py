#!/usr/bin/env python3
"""PinPin assistant backend: the per-screen Claude button.

The user presses a Claude button on a wall screen and speaks; the headset sends the STT text here (no Enter).
We hold one conversational session per screen id, stream a Markdown reply back as SSE, and can act on THAT
screen's Mac virtual display (open an app, click, type) via computer use. Runs as a PinPin Host plugin on each
Mac that hosts screens, so it inherits Screen Recording + Accessibility + Input.

Design: the model backend and the screen controller are injected, so the whole turn loop runs offline against
mocks in bench/assistant_mock_test.py. The live pieces (claude CLI, PinPin Displays, per-display screenshot) are
the product; the mocks are only for the hardware-free test.

Python stdlib only (matches the mini-apps server). Protocol: docs/prd/assistant-protocol.md.
"""
import json, os, sys, time, threading, subprocess, urllib.request, urllib.parse, urllib.error, socket, base64, pathlib, queue, shutil, re
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# ---- config -----------------------------------------------------------------
PORT = int(os.environ.get("PINPIN_ASSIST_PORT", "57840"))
DATA_DIR = pathlib.Path(os.path.expanduser(os.environ.get("PINPIN_ASSIST_DIR", "~/.local/share/pinpin-assistant")))
MAX_STEPS = int(os.environ.get("PINPIN_ASSIST_MAX_STEPS", "12"))
MAX_WALL_S = float(os.environ.get("PINPIN_ASSIST_MAX_WALL_S", "90"))
DISPLAYS_HTTP = os.environ.get("PINPIN_DISPLAYS_HTTP", "http://127.0.0.1:57710")
DISPLAYS_UDP = (os.environ.get("PINPIN_DISPLAYS_HOST", "127.0.0.1"), 57711)
APPS_BASE = os.environ.get("PINPIN_APPS_BASE", "http://192.168.0.50:57830")  # mini-apps server (work-ad's Chrome runner)
# Receptionist (work-42): the headset reaches the VPS receptionist through this thin reverse-proxy.
RECEPT_BASE = os.environ.get("PINPIN_RECEPT_BASE", "http://100.109.138.51:57850")  # VPS over Tailscale
RECEPT_TOKEN_FILE = os.path.expanduser(os.environ.get(
    "PINPIN_RECEPT_TOKEN_FILE", "~/.local/share/pinpin-assistant/receptionist-token"))
ROOM_TO_CAPTAIN = os.environ.get("PINPIN_ROOM_TO_CAPTAIN", "") == "1"  # ASK… room session -> receptionist -> work-df
# K20: screens whose page is a Chrome kiosk on the Mini (local-cdp.json). open_url/open is 403 there
# (the kiosk page IS the screen) — these screens navigate via eval(location.assign(...)) instead.
KIOSK_SCREENS = {int(x) for x in os.environ.get("PINPIN_KIOSK_SCREENS", "1").split(",") if x.strip().isdigit()}

_APPS_TOKEN = ""


def apps_token():
    """Shared bearer token for the apps server's powerful endpoints (work-ad). Read from a local 600 file; never
    logged or committed. Re-read until found (it may be placed after startup)."""
    global _APPS_TOKEN
    if _APPS_TOKEN:
        return _APPS_TOKEN
    for p in ("~/.local/share/pinpin-apps/token", "/root/.config/pinpin-apps/token", "~/pinpin-apps-data/token"):
        try:
            _APPS_TOKEN = open(os.path.expanduser(p)).read().strip()
            if _APPS_TOKEN:
                return _APPS_TOKEN
        except Exception:
            continue
    return ""


def apps_headers(extra=None):
    h = dict(extra or {})
    t = apps_token()
    if t:
        h["Authorization"] = "Bearer " + t
    return h
MODEL_FAST = os.environ.get("PINPIN_ASSIST_MODEL", "sonnet")      # tool/build turns
MODEL_THINK = os.environ.get("PINPIN_ASSIST_MODEL_THINK", "opus")  # "think harder"
MODEL_CHAT = os.environ.get("PINPIN_ASSIST_MODEL_CHAT", "haiku")   # plain conversation (fast first word)

# A turn that looks like it wants to act on the screen (or build) goes to the stronger tool model; plain chat goes
# to haiku for latency. The model still has the tools either way, so a missed match just uses a cheaper model.
_ACTION_RE = re.compile(
    r"\b(open|go ?to|visit|browse|search|show|play|watch|pull up|bring up|make|build|create|set up|"
    r"turn this|put a|add a|click|scroll|type|press|close|move|resize|weather|timer|youtube|google|website|app)\b"
    r"|https?://|\w+\.(com|org|net|io|tv)\b", re.I)


def pick_model(text, think):
    if think:
        return MODEL_THINK
    return MODEL_FAST if _ACTION_RE.search(text or "") else MODEL_CHAT


def context_note(msgs):
    """A compact preamble so a warm (chat) process learns what happened in cold (action/build) turns it didn't
    run — the user's words, the actions taken, and the reply. Keeps mixed conversations coherent."""
    if not msgs:
        return ""
    lines = []
    for m in msgs:
        txt = (m.get("content") or "").strip().replace("\n", " ")[:280]
        if m.get("role") == "user":
            lines.append(f"User: {txt}")
        else:
            acts = m.get("actions") or []
            did = (" [did: " + "; ".join(acts) + "]") if acts else ""
            lines.append(f"You: {txt}{did}")
    return ("[Earlier in this conversation, for context — use it, don't repeat it:\n"
            + "\n".join(lines) + "\n]\n\n")


def chat_system_prompt(screen):
    if str(screen) == "room":
        return (
            "You are the PinPin room assistant — you help with the whole room, not one screen. Call house_info to "
            "see the house's walls (by id and size) and screens (by id). You can add, move, resize and close wall "
            "screens — new_screen (on a named wall), move_screen, resize_screen, close_screen — always naming the "
            "screen by its id; and build apps (authoring_guide, put_app, assign_app). The user speaks from anywhere: "
            "\"make screen 3 bigger\", \"put a weather screen on the big wall\", \"turn off the screen by the window\". "
            "For a nudge like \"a bit to the left/right/up/down\", call move_screen with direction (and optional "
            "amount_m) — e.g. direction:\"left\" — NOT an absolute position. Name the screen by id. "
            "If a request names a wall by feel (\"by the door\") and you can't map it to a wall id, say what walls "
            "exist and ask which. You do NOT have a single screen's pointer tools (screenshot/click) here.\n"
            "CONFIRMATION: a destructive tool (close_screen, or put_app with replace) returns needs_confirmation — "
            "relay its message to the user and only call the tool again after they clearly say yes; if they say no, "
            "don't. Keep replies short and in Markdown, and confirm what changed in one line."
        )
    if str(screen).isdigit() and int(screen) in KIOSK_SCREENS:
        return (
            f"You are the PinPin assistant for the MINI wall screen (screen {screen}). This screen is a Chrome "
            f"kiosk showing the Mr. PinPin Atlas storyboard. The user has given you FULL control of this Chrome: "
            f"you may click, scroll, type, and navigate it to other web pages, exactly like a laptop screen. "
            f"Every action returns a screenshot — read it to CONFIRM what happened before you say it did.\n"
            f"NAVIGATION on this screen is NOT done with open_url (that is blocked here — the kiosk page IS the "
            f"screen). To go to another page, use eval with location.assign, e.g. "
            f"eval(\"location.assign('https://en.wikipedia.org')\"); to go back, eval(\"history.back()\") or "
            f"location.assign of a saved URL.\n"
            f"AT THE START of a session, before doing anything else, record this screen's current URL by calling "
            f"eval(\"location.href\") — this is the ORIGINAL storyboard URL. Remember it for the whole session.\n"
            f"BEFORE you navigate away from the Atlas storyboard, SAY SO first (\"this will leave the storyboard\") "
            f"so the user knows the screen will change. ALWAYS offer to return, and be able to return, to that "
            f"original URL on request — navigate back with eval(\"location.assign('<the original URL>')\").\n"
            f"To act precisely on a page (click a specific link/button), prefer eval (e.g. "
            f"[...document.querySelectorAll('a')].find(x=>/learn more/i.test(x.textContent))?.click()) over "
            f"guessing click coordinates. To play a video, use play_youtube(query).\n"
            f"Keep replies short and in Markdown, and confirm in one line what the user now sees."
        )
    return (
        f"You are the PinPin assistant for wall screen {screen}. You may ONLY act on this screen via the "
        f"mcp__screen__ tools; never touch any other display. The screen is a web view: open_url navigates "
        f"THIS screen's browser window to a page (it lands on this display), and click/scroll/type act inside "
        f"it. Every action returns a screenshot of the screen — read it to CONFIRM the result actually "
        f"happened before you say it did; if it didn't, try again or report what you see.\n"
        f"To PLAY a video, use play_youtube(query) — it finds and starts the first result. To act precisely on a "
        f"page (click a specific link/button), prefer eval (e.g. the first link: "
        f"[...document.querySelectorAll('a')].find(x=>/learn more|more info/i.test(x.textContent))?.click()) over "
        f"guessing click coordinates.\n"
        f"To turn this screen into a custom mini-app (\"make this a kitchen timer\", \"build me X here\"): call "
        f"authoring_guide first, write the app with put_app (create:true for a new app; id derived from the "
        f"name) and iterate until check.errors is empty AND check.text matches what was asked, then assign_app "
        f"to show it on this screen and confirm from a screenshot.\n"
        f"CONFIRMATION: if a tool returns needs_confirmation (a destructive action), relay its message and only "
        f"call it again after the user clearly says yes.\n"
        f"Keep replies short and in Markdown, and confirm in one line what the user now sees."
    )
NOW = lambda: time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())

_VERB = {"open_url": "opening", "open_app": "opening", "place_window": "placing",
         "click": "clicking", "move": "moving", "type": "typing", "key": "pressing",
         "scroll": "scrolling", "screenshot": "looking at the screen"}

def _label(ev):
    """A short human label for the headset status line, e.g. 'opening youtube.com'."""
    action = ev.get("action", "")
    verb = _VERB.get(action, action)
    detail = (ev.get("detail") or "").strip()
    if action == "open_url":
        detail = detail.replace("https://", "").replace("http://", "").split("/")[0]
    if action in ("screenshot",) or not detail:
        return verb
    return f"{verb} {detail}"[:60]


# ---- screen controller ------------------------------------------------------
class ScreenError(Exception):
    pass


class ScreenController:
    """Scoped computer use for one screen's CGVirtualDisplay. u,v are 0..10000 across that display."""
    def resolve(self, screen):
        """Return the display record {screen,display,w,h,scale,main}; raise ScreenError if unknown/main."""
        raise NotImplementedError

    def screenshot(self, screen):  # -> (png_bytes, w, h)
        raise NotImplementedError

    def move(self, screen, u, v): raise NotImplementedError
    def click(self, screen, u, v): raise NotImplementedError
    def scroll(self, screen, u, v, dy, dx=0): raise NotImplementedError
    def type_text(self, screen, text): raise NotImplementedError
    def key(self, screen, combo): raise NotImplementedError
    def open_app(self, screen, bundle): raise NotImplementedError
    def open_url(self, screen, url): raise NotImplementedError
    def place_window(self, screen, app): raise NotImplementedError


class LiveScreenController(ScreenController):
    """Talks to PinPin Displays (:57710 HTTP, :57711 UDP mouse) and a per-display screenshot helper.

    The screenshot helper is a PinPin Host-launched tool that captures one CGDirectDisplayID (it needs Screen
    Recording, which the host grants). Keyboard/app actions go through the host's input plugin. Only exercised on
    the Macs; never in the offline test.
    """
    def __init__(self, cap_cmd=None):
        # cap_cmd: argv taking ("<displayID>") and writing PNG to stdout. Set on the Mac.
        self.cap_cmd = cap_cmd or [os.path.expanduser("~/.local/share/pinpin-assistant/pinpin-screencap")]
        self._udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    def _displays(self):
        with urllib.request.urlopen(DISPLAYS_HTTP + "/displays", timeout=5) as r:
            return json.load(r)

    def resolve(self, screen):
        # PinPin Displays' /displays lists ONLY the virtual displays it created, never the main/built-in one, so a
        # screen that resolves here can never be the main display. An id not in the list is rejected. This is the
        # "never act on the main display" guarantee: the main display is simply unreachable through this path.
        screen = int(screen)
        for d in self._displays():
            if int(d["screen"]) == screen:
                d = dict(d); d["main"] = False
                return d
        raise ScreenError(f"no display for screen {screen} (not a PinPin virtual display)")

    def screenshot(self, screen):
        # Prefer the CDP screenshot via the apps server (/shot): it's the only way to capture a REMOTE
        # CDP screen like the MINI (the Air can't capture the Mini's display locally), and it unifies
        # once the runner has /shot everywhere. If /shot doesn't give a PNG, fall back to LOCAL display
        # capture WHEN this screen is a local display (resolve succeeds); for a remote screen with no
        # local display (the MINI), surface the /shot error instead of capturing the wrong display.
        shot_err = None
        try:
            req = urllib.request.Request(f"{APPS_BASE}/api/screens/{int(screen)}/shot?max=900",
                                         headers=apps_headers())
            with urllib.request.urlopen(req, timeout=10) as r:
                png = r.read()
                w = h = 0
                vp = r.headers.get("X-Viewport", "")
                if "x" in vp:
                    try:
                        w, h = (int(x) for x in vp.split("x")[:2])
                    except ValueError:
                        pass
                return png, w, h
        except Exception as e:
            shot_err = getattr(e, "code", type(e).__name__)
        try:
            d = self.resolve(screen)   # local display? (raises for a remote screen like the MINI)
        except ScreenError:
            raise ScreenError(f"shot unavailable ({shot_err}) and screen {screen} has no local display")
        out = subprocess.run(self.cap_cmd + [str(d["display"])], capture_output=True, timeout=15)
        if out.returncode != 0:
            raise ScreenError("screenshot failed: " + out.stderr.decode("utf-8", "replace")[:200])
        return out.stdout, d["w"], d["h"]

    def _mouse(self, verb, screen, u, v):
        self.resolve(screen)  # validates + refuses main
        msg = f"{verb} {int(screen)} {int(u)} {int(v)}".encode()
        self._udp.sendto(msg, DISPLAYS_UDP)

    def _cdp(self, screen, verb, body):
        # Route input through work-ad's CDP runner for web/runner-owned screens (reliable; pinpin-displays'
        # CGEvent clicks don't register on Chrome --app windows). Returns True if handled. On 409 (no runner
        # window = native screen), 404 (endpoint not live yet) or 502 (runner unreachable), fall back to CGEvent.
        try:
            req = urllib.request.Request(f"{APPS_BASE}/api/screens/{int(screen)}/{verb}",
                                         data=json.dumps(body).encode(),
                                         headers=apps_headers({"Content-Type": "application/json"}), method="POST")
            with urllib.request.urlopen(req, timeout=6) as r:
                json.load(r)
            return True
        except Exception:
            return False

    def move(self, screen, u, v): self._mouse("m", screen, u, v)

    def click(self, screen, u, v):
        if self._cdp(screen, "click", {"u": int(u), "v": int(v)}):
            return
        self._mouse("c", screen, u, v)

    def scroll(self, screen, u, v, dy, dx=0):
        if self._cdp(screen, "scroll", {"u": int(u), "v": int(v), "dy": int(dy), "dx": int(dx)}):
            return
        # Fallback: the "w <id> <u> <v> <dy> <dx>" datagram the headset's SCROLL also sends (pinpin-displays).
        self.resolve(screen)
        self._udp.sendto(f"w {int(screen)} {int(u)} {int(v)} {int(dy)} {int(dx)}".encode(), DISPLAYS_UDP)

    def _post(self, path, **params):
        self.resolve(params.get("screen", params.get("id", 0)))
        url = DISPLAYS_HTTP + path + "?" + urllib.parse.urlencode(params)
        with urllib.request.urlopen(url, data=b"", timeout=8) as r:
            return r.read()

    def place_window(self, screen, app):
        return self._post("/place", app=app, screen=int(screen))

    def open_app(self, screen, bundle):
        subprocess.run(["open", "-b", bundle], timeout=10, check=True)
        time.sleep(1.0)
        return self.place_window(screen, bundle)

    def open_url(self, screen, url):
        # Land the page deterministically on THIS screen via work-ad's shared Chrome runner (mini-apps server):
        # it ensures the screen's Chrome --app window exists on that virtual display and navigates it. We poll
        # shown_seq == seq to confirm the page actually landed (verify, not trust). This replaces launching Safari
        # (which opened on the user's main display and could not be moved from ssh).
        self.resolve(screen)  # never-main guarantee: must be a PinPin virtual display
        if not (url.startswith("http://") or url.startswith("https://")):
            url = "https://" + url
        body = json.dumps({"url": url}).encode()
        req = urllib.request.Request(f"{APPS_BASE}/api/screens/{int(screen)}/open", data=body,
                                     headers=apps_headers({"Content-Type": "application/json"}), method="POST")
        with urllib.request.urlopen(req, timeout=8) as r:
            resp = json.load(r)
        seq = resp.get("seq")
        deadline = time.time() + 10
        while seq is not None and time.time() < deadline:
            with urllib.request.urlopen(f"{APPS_BASE}/api/screens", timeout=5) as r:
                data = json.load(r)
            rows = data if isinstance(data, list) else data.get("screens", [])
            e = next((x for x in rows if str(x.get("screen")) == str(screen)), None)
            if e and e.get("shown_seq") is not None and e["shown_seq"] >= seq:
                return f"navigated screen {screen} to {url} (seq {seq})"
            time.sleep(0.5)
        return f"requested {url} on screen {screen} (seq {seq}; not confirmed landed within 10s)"

    def type_text(self, screen, text):
        if self._cdp(screen, "type", {"text": text}):
            return
        # Fallback (native screens): keyboard reaches the focused window via the host input tool.
        subprocess.run([os.path.expanduser("~/.local/share/pinpin-assistant/pinpin-type")],
                       input=text.encode(), timeout=15, check=True)

    def key(self, screen, combo):
        if self._cdp(screen, "key", {"combo": combo}):
            return
        subprocess.run([os.path.expanduser("~/.local/share/pinpin-assistant/pinpin-type"), "--key", combo],
                       timeout=10, check=True)


# ---- session store ----------------------------------------------------------
class SessionStore:
    def __init__(self, root=DATA_DIR):
        self.root = pathlib.Path(root)
        self.lock = threading.Lock()
        self.cache = {}

    def _dir(self, screen):
        d = self.root / "screens" / str(screen)
        d.mkdir(parents=True, exist_ok=True)
        return d

    def load(self, screen):
        with self.lock:
            if screen in self.cache:
                return self.cache[screen]
            f = self._dir(screen) / "session.json"
            if f.exists():
                s = json.loads(f.read_text())
            else:
                s = {"screen": str(screen), "claudeSessionId": None, "messages": [], "turns": 0}
            self.cache[screen] = s
            return s

    def save(self, screen):
        with self.lock:
            s = self.cache.get(screen)
            if s is None:
                return
            f = self._dir(screen) / "session.json"
            tmp = f.with_suffix(".tmp")
            tmp.write_text(json.dumps(s, ensure_ascii=False))
            tmp.rename(f)

    def reset(self, screen):
        s = self.load(screen)
        s["claudeSessionId"] = None
        s["messages"] = []
        self.save(screen)

    def log_action(self, screen, rec):
        rec = dict(rec, ts=NOW())
        with (self._dir(screen) / "actions.jsonl").open("a") as fh:
            fh.write(json.dumps(rec, ensure_ascii=False) + "\n")

    def actions(self, screen):
        f = self._dir(screen) / "actions.jsonl"
        return f.read_text() if f.exists() else ""


# ---- model backends ---------------------------------------------------------
class ModelBackend:
    def run_turn(self, session, text, think, screen, ctl, emit, deadline):
        """Drive one user turn. Call emit({...}) for SSE events. Return stopReason: end|max_steps|timeout|error."""
        raise NotImplementedError

    def ask(self, prompt, system=None, want_json=False, model=None):  # one-shot, no screen actions
        raise NotImplementedError

    def auth_ok(self):  # (authed, detail) — is the model reachable (claude login valid)?
        return True, "n/a"


class MockBackend(ModelBackend):
    """Deterministic backend for the offline test. Understands a few phrases so the turn loop + screen routing is
    exercised end to end without a model or a Mac."""
    def run_turn(self, session, text, think, screen, ctl, emit, deadline):
        low = text.lower().strip()
        steps = 0
        model = MODEL_THINK if think else MODEL_FAST
        emit({"type": "text", "delta": f"*( {model} )* "})
        if low.startswith("open ") or "open " in low:
            # pull a url or a site word
            target = low.split("open", 1)[1].strip().split(" and ")[0].strip()
            url = target if target.startswith("http") else "https://" + target.split()[0].replace(" ", "")
            emit({"type": "action", "action": "open_url", "detail": url, "status": "begin"})
            try:
                ctl.open_url(screen, url)
                emit({"type": "action", "action": "open_url", "detail": url, "status": "ok"})
                steps += 1
            except Exception as e:
                emit({"type": "action", "action": "open_url", "detail": str(e), "status": "error"})
                emit({"type": "text", "delta": f"\nCouldn't open it: {e}"})
                return "error"
            emit({"type": "text", "delta": f"\nOpened **{url}** on screen {screen}."})
            return "end"
        emit({"type": "text", "delta": "Okay. (mock) You said: " + text})
        return "end"

    def ask(self, prompt, system=None, want_json=False, model=None):
        if want_json:
            return {"text": "", "json": {"echo": prompt}}
        return {"text": "(mock) " + prompt}


class WarmProc:
    """One persistent `claude --input-format stream-json` process for a screen, so chat turns skip the ~2 s cold
    start. Context lives in the process across turns (fed over stdin). One turn at a time (lock). Only usable as a
    PinPin Host child, where the claude login (Keychain) is reachable."""
    def __init__(self, screen, claude_bin, mcp_config):
        self.screen = screen
        self.last = time.time()
        self.lock = threading.Lock()
        self.session_id = None
        self.seen = 0  # transcript messages already in this process's context (watermark)
        argv = [claude_bin, "-p", "--input-format", "stream-json", "--output-format", "stream-json",
                "--include-partial-messages", "--model", MODEL_CHAT, "--verbose",
                "--append-system-prompt", chat_system_prompt(screen),
                "--mcp-config", mcp_config, "--allowedTools", "mcp__screen__*", "--max-turns", str(MAX_STEPS)]
        self.proc = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, text=True, bufsize=1,
                                     env={**os.environ, "PINPIN_SCREEN": str(screen)})

    def alive(self):
        return self.proc.poll() is None

    def rss_mb(self):
        try:
            out = subprocess.run(["ps", "-o", "rss=", "-p", str(self.proc.pid)],
                                 capture_output=True, text=True, timeout=3).stdout.strip()
            return int(out) // 1024 if out.isdigit() else None
        except Exception:
            return None

    def kill(self):
        try:
            self.proc.kill()
        except Exception:
            pass

    def run_turn(self, text, emit, deadline):
        with self.lock:
            if not self.alive():
                raise RuntimeError("warm proc not alive")  # pre-stream failure -> caller falls back to cold
            self.last = time.time()
            msg = {"type": "user", "message": {"role": "user", "content": [{"type": "text", "text": text}]}}
            self.proc.stdin.write(json.dumps(msg) + "\n"); self.proc.stdin.flush()
            steps = 0; stop = "end"; streamed = False
            while True:
                if time.time() > deadline:
                    self.kill(); return "timeout"
                line = self.proc.stdout.readline()
                if line == "":
                    if not streamed:
                        raise RuntimeError("warm proc ended before output")  # fall back to cold
                    return "error"
                line = line.strip()
                if not line:
                    continue
                try:
                    ev = json.loads(line)
                except json.JSONDecodeError:
                    continue
                t = ev.get("type")
                if t == "system" and ev.get("session_id"):
                    self.session_id = ev["session_id"]
                elif t == "stream_event":
                    e = ev.get("event", {})
                    if e.get("type") == "content_block_delta":
                        d = e.get("delta", {})
                        if d.get("type") == "text_delta" and d.get("text"):
                            streamed = True; emit({"type": "text", "delta": d["text"]})
                elif t == "assistant":
                    for b in ev.get("message", {}).get("content", []):
                        if b.get("type") == "text" and not streamed and b.get("text"):
                            emit({"type": "text", "delta": b["text"]})
                        elif b.get("type") == "tool_use":
                            steps += 1
                            name = b.get("name", "").replace("mcp__screen__", "")
                            emit({"type": "action", "action": name,
                                  "detail": json.dumps(b.get("input", {}))[:200], "status": "begin"})
                            if steps >= MAX_STEPS:
                                stop = "max_steps"
                elif t == "result":
                    if ev.get("is_error"):
                        stop = "error"
                    self.last = time.time()
                    return stop


class WarmPool:
    """Per-screen warm processes, capped, with idle reaping. Chat turns reuse them; action/build/think stay cold."""
    def __init__(self, claude_bin, mcp_config, cap=None, idle_s=None):
        self.claude = claude_bin
        self.mcp_config = mcp_config
        self.cap = int(os.environ.get("PINPIN_ASSIST_WARM_CAP", cap or 4))
        self.idle_s = float(os.environ.get("PINPIN_ASSIST_WARM_IDLE_S", idle_s or 600))
        self.procs = {}
        self.lock = threading.Lock()
        threading.Thread(target=self._reaper, daemon=True).start()

    def get(self, screen):
        with self.lock:
            wp = self.procs.get(screen)
            if wp and wp.alive():
                return wp
            if wp:
                wp.kill(); self.procs.pop(screen, None)
            if len(self.procs) >= self.cap:
                lru = min(self.procs.values(), key=lambda p: p.last)
                lru.kill(); self.procs.pop(lru.screen, None)
                sys.stderr.write(f"{NOW()} warm pool: evicted screen {lru.screen} (cap {self.cap})\n")
            wp = WarmProc(screen, self.claude, self.mcp_config)
            self.procs[screen] = wp
            sys.stderr.write(f"{NOW()} warm pool: spawned screen {screen} rss={wp.rss_mb()}MB pool={len(self.procs)}\n")
            return wp

    def _reaper(self):
        while True:
            time.sleep(60)
            now = time.time()
            with self.lock:
                for s, wp in list(self.procs.items()):
                    if not wp.alive() or now - wp.last > self.idle_s:
                        wp.kill(); self.procs.pop(s, None)
                        sys.stderr.write(f"{NOW()} warm pool: reaped screen {s}\n")
                if self.procs:  # memory per process, logged (work-df)
                    mem = ", ".join(f"{s}={wp.rss_mb()}MB" for s, wp in self.procs.items())
                    sys.stderr.write(f"{NOW()} warm pool: {len(self.procs)} proc(s) rss {mem}\n")


class ClaudeCliBackend(ModelBackend):
    """The real backend: wraps `claude -p --output-format stream-json --resume <sid>` with a screen-scoped MCP
    server for computer use. Plain-chat turns use a WARM per-screen process (no cold start); action/build/think
    turns run a cold process (model from the router), with the warm pool as the fast path and cold as fallback."""
    def __init__(self, claude_bin="claude", mcp_config=None):
        self.claude = claude_bin
        self.mcp_config = mcp_config or os.path.expanduser("~/.local/share/pinpin-assistant/screen-mcp.json")
        self.pool = WarmPool(self.claude, self.mcp_config)

    def _argv(self, session, text, think, screen):
        model = pick_model(text, think)
        sys_prompt = chat_system_prompt(screen)
        argv = [self.claude, "-p", text, "--output-format", "stream-json", "--verbose",
                "--include-partial-messages",  # token-level text deltas -> first word streams immediately
                "--model", model, "--append-system-prompt", sys_prompt,
                "--mcp-config", self.mcp_config, "--allowedTools", "mcp__screen__*",
                "--max-turns", str(MAX_STEPS)]
        # (Tried skipping the MCP server on chat turns to shave startup; measured it neither faster nor safer — a
        # mis-routed chat turn then couldn't use a tool — so the tools stay loaded on every turn.)
        if session.get("claudeSessionId"):
            argv += ["--resume", session["claudeSessionId"]]
        return argv, {**os.environ, "PINPIN_SCREEN": str(screen)}

    def run_turn(self, session, text, think, screen, ctl, emit, deadline):
        # Plain-chat turns go to the warm per-screen process (no cold start). If the warm process can't be used
        # (dead/spawn fail before any output), fall back to the cold path below. Action/build/think use cold.
        if not think and pick_model(text, think) == MODEL_CHAT:
            try:
                wp = self.pool.get(str(screen))
                # Catch the warm process up on transcript it didn't run (cold turns, pre-spawn history). The
                # current user message is already appended at the tail, so exclude it from the preamble.
                msgs = session.get("messages", [])
                lo = max(wp.seen, len(msgs) - 1 - 10)  # at most the last ~10 unseen messages (seed/catch-up)
                preamble = context_note(msgs[lo:len(msgs) - 1]) if len(msgs) > 1 else ""
                stop = wp.run_turn(preamble + text if preamble else text, emit, deadline)
                wp.seen = len(msgs) + 1  # +1 for the assistant reply handle_message appends after this returns
                return stop
            except Exception as e:
                sys.stderr.write(f"{NOW()} warm turn failed ({e}); falling back to cold\n")
        argv, env = self._argv(session, text, think, screen)
        p = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env, bufsize=1, text=True)
        steps = 0
        stop = "end"
        streamed_text = False  # did token deltas (stream_event) carry the text? then don't re-emit the full block
        try:
            for line in p.stdout:
                if time.time() > deadline:
                    stop = "timeout"; p.kill(); break
                line = line.strip()
                if not line:
                    continue
                try:
                    ev = json.loads(line)
                except json.JSONDecodeError:
                    continue
                t = ev.get("type")
                if t == "system" and ev.get("session_id"):
                    session["claudeSessionId"] = ev["session_id"]
                elif t == "stream_event":
                    e = ev.get("event", {})
                    if e.get("type") == "content_block_delta":
                        d = e.get("delta", {})
                        if d.get("type") == "text_delta" and d.get("text"):
                            streamed_text = True
                            emit({"type": "text", "delta": d["text"]})
                elif t == "assistant":
                    for block in ev.get("message", {}).get("content", []):
                        if block.get("type") == "text" and not streamed_text:
                            emit({"type": "text", "delta": block["text"]})  # fallback when no partial streaming
                        elif block.get("type") == "tool_use":
                            steps += 1
                            name = block.get("name", "").replace("mcp__screen__", "")
                            emit({"type": "action", "action": name,
                                  "detail": json.dumps(block.get("input", {}))[:200], "status": "begin"})
                            if steps >= MAX_STEPS:
                                stop = "max_steps"
                elif t == "result":
                    if ev.get("subtype") not in (None, "success"):
                        stop = "error"
            p.wait(timeout=5)
        except Exception as e:
            emit({"type": "error", "message": str(e)})
            stop = "error"
        finally:
            if p.poll() is None:
                p.kill()
        return stop

    def ask(self, prompt, system=None, want_json=False, model=None):
        argv = [self.claude, "-p", prompt, "--output-format", "json", "--model", model or MODEL_FAST]
        if system:
            argv += ["--append-system-prompt", system]
        out = subprocess.run(argv, capture_output=True, text=True, timeout=MAX_WALL_S)
        text = ""
        try:
            text = json.loads(out.stdout).get("result", "")
        except Exception:
            text = out.stdout.strip()
        if want_json:
            try:
                start = text.index("{"); end = text.rindex("}") + 1
                return {"text": "", "json": json.loads(text[start:end])}
            except Exception:
                return {"text": text, "json": None}
        return {"text": text}

    def auth_ok(self):
        try:
            out = subprocess.run([self.claude, "-p", "ok", "--model", MODEL_CHAT, "--output-format", "json"],
                                 capture_output=True, text=True, timeout=30)
            blob = out.stdout + out.stderr
            if any(k in blob for k in ("Not logged in", "authentication_failed", "Please run /login",
                                       "OAuth", "Invalid API key")):
                return False, "claude login expired — run `claude` login on this machine"
            return True, "ok"
        except Exception as e:
            return False, str(e)[:160]


# ---- assistant --------------------------------------------------------------
class Assistant:
    def __init__(self, backend, ctl, store=None):
        self.backend = backend
        self.ctl = ctl
        self.store = store or SessionStore()

    def handle_message(self, screen, text, think, emit, turn=None):
        session = self.store.load(screen)
        turn_id = turn or f"{screen}-{int(time.time()*1000)}"
        deadline = time.time() + MAX_WALL_S
        ts = NOW()

        # Every event carries the turn id (so a late stream can't merge into the next turn).
        def out(ev):
            emit({**ev, "turn": turn_id})

        out({"type": "start", "screen": str(screen)})
        session["messages"].append({"role": "user", "content": text, "ts": ts, "actions": []})

        reply_parts = []
        action_labels = []

        def emit2(ev):
            if ev.get("type") == "text":
                reply_parts.append(ev["delta"])
            elif ev.get("type") == "action":
                ev = {**ev, "label": ev.get("label") or _label(ev)}
                if ev.get("status") in (None, "begin"):
                    action_labels.append(ev["label"])
                self.store.log_action(screen, {"turn": turn_id, **ev})
            out(ev)

        stop = "error"
        try:
            stop = self.backend.run_turn(session, text, think, screen, self.ctl, emit2, deadline)
        except Exception as e:
            out({"type": "error", "message": str(e)})
        session["messages"].append({"role": "assistant", "content": "".join(reply_parts),
                                     "ts": NOW(), "actions": action_labels})
        session["turns"] += 1
        self.store.save(screen)
        out({"type": "done", "stopReason": stop})

    def ask(self, app, screen, prompt, system=None, want_json=False, model=None):
        self.store.log_action(screen or "ask", {"type": "ask", "app": app, "prompt": prompt[:200], "model": model})
        return self.backend.ask(prompt, system=system, want_json=want_json, model=model)

    def auth_status(self):
        ok, detail = self.backend.auth_ok()
        return {"authed": ok, "detail": detail, "ts": NOW()}

    def _apps_ids(self):
        try:
            with urllib.request.urlopen(APPS_BASE + "/api/apps", timeout=10) as r:
                data = json.load(r)
            rows = data if isinstance(data, list) else data.get("apps", [])
            return {x.get("id") for x in rows if isinstance(x, dict)}
        except Exception:
            return set()

    def _app_commit(self, app):
        try:
            with urllib.request.urlopen(f"{APPS_BASE}/api/apps/{app}", timeout=10) as r:
                return json.load(r).get("commit")
        except Exception:
            return None

    def build_app(self, prompt, app=None, screen=None):
        """Control-page 'ask Claude to build an app'. Runs a build turn (sonnet + app tools) in a dedicated
        'build' session so it never touches per-screen chat; then WE assign it to the screen (if given) so the
        model stays out of screen state. Returns {text, app, commit, assigned?}."""
        before = self._apps_ids()
        text = (prompt or "").strip()
        if app:
            text += f'\n\n(Modify the existing app "{app}".)'
        text += "\n\n(Build it and make sure it render-checks clean with put_app. Do NOT assign it to a screen.)"
        self.store.log_action("build", {"type": "build", "prompt": text[:200], "app": app, "screen": screen})
        parts = []
        self.handle_message("build", text, False,
                            lambda ev: parts.append(ev["delta"]) if ev.get("type") == "text" else None,
                            turn="build")
        after = self._apps_ids()
        new_app = app or next(iter(after - before), None)
        res = {"text": "".join(parts).strip(), "app": new_app, "commit": self._app_commit(new_app) if new_app else None}
        if new_app and screen is not None and str(screen) != "":
            try:
                req = urllib.request.Request(f"{APPS_BASE}/api/screens/{screen}/app",
                                             data=json.dumps({"app": new_app}).encode(),
                                             headers=apps_headers({"Content-Type": "application/json"}), method="POST")
                urllib.request.urlopen(req, timeout=10).read()
                res["assigned"] = screen
            except Exception as e:
                res["assign_error"] = str(e)
        return res


# ---- HTTP -------------------------------------------------------------------
def make_handler(assistant):
    class H(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *a):
            pass

        def _json(self, code, obj):
            body = json.dumps(obj, ensure_ascii=False).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def _body(self):
            n = int(self.headers.get("Content-Length", "0"))
            return json.loads(self.rfile.read(n) or b"{}") if n else {}

        def do_GET(self):
            u = urllib.parse.urlparse(self.path)
            parts = [p for p in u.path.split("/") if p]
            if u.path == "/health":
                return self._json(200, {"ok": True, "port": PORT, "ts": NOW()})
            if u.path == "/health/auth":  # green once the claude login is valid (poll after re-auth)
                return self._json(200, assistant.auth_status())
            if len(parts) == 2 and parts[0] == "session":
                s = assistant.store.load(parts[1])
                return self._json(200, {
                    "screen": s["screen"], "claudeSessionId": s["claudeSessionId"], "turns": s["turns"],
                    "messages": [{"role": m["role"], "text": m["content"],
                                  "actions": m.get("actions", []), "t": m["ts"]} for m in s["messages"]]})
            if len(parts) == 3 and parts[0] == "debug" and parts[1] == "screenshot":
                # Capture one screen as PNG. Works because the service runs as a PinPin Host child (Screen
                # Recording inherited); a bare ssh-launched screencap would not have the grant.
                try:
                    png, w, h = assistant.ctl.screenshot(parts[2])
                except Exception as e:
                    return self._json(500, {"error": str(e)})
                self.send_response(200); self.send_header("Content-Type", "image/png")
                self.send_header("Content-Length", str(len(png))); self.end_headers(); self.wfile.write(png)
                return
            if len(parts) == 3 and parts[0] == "session" and parts[2] == "actions":
                self.send_response(200); self.send_header("Content-Type", "application/x-ndjson")
                b = assistant.store.actions(parts[1]).encode()
                self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
                return
            if u.path.startswith("/api/reception/"):
                # keep the query string (history?conv=&limit=): strip the prefix off the raw path
                return self._proxy_reception("GET", self.path[len("/api/reception/"):], None)
            self._json(404, {"error": "not found"})

        def do_POST(self):
            u = urllib.parse.urlparse(self.path)
            parts = [p for p in u.path.split("/") if p]
            try:
                if u.path == "/api/ask":
                    b = self._body()
                    r = assistant.ask(b.get("app"), b.get("screen"), b.get("prompt", ""),
                                      b.get("system"), bool(b.get("json")), b.get("model"))
                    return self._json(200, r)
                if u.path == "/api/build":
                    b = self._body()
                    return self._json(200, assistant.build_app(b.get("prompt", ""), b.get("app"), b.get("screen")))
                if len(parts) == 3 and parts[0] == "session" and parts[2] == "reset":
                    assistant.store.reset(parts[1]); return self._json(200, {"ok": True})
                if len(parts) == 3 and parts[0] == "session" and parts[2] == "message":
                    return self._sse_message(parts[1], self._body())
                if u.path.startswith("/api/reception/"):
                    n = int(self.headers.get("Content-Length", "0") or "0")
                    raw = self.rfile.read(n) if n else b"{}"
                    return self._proxy_reception("POST", self.path[len("/api/reception/"):], raw)
            except Exception as e:
                return self._json(500, {"error": str(e)})
            self._json(404, {"error": "not found"})

        def _proxy_reception(self, method, suffix, raw):
            """Forward /api/reception/<suffix> to the VPS receptionist and stream the response back.
            Resilient: if the VPS is unreachable, answer with one SSE error event so the headset
            client still gets a well-formed stream."""
            try:
                tok = open(RECEPT_TOKEN_FILE).read().strip()
            except OSError:
                tok = ""
            url = RECEPT_BASE + "/api/" + suffix              # turn | sessions
            req = urllib.request.Request(url, data=(raw if method == "POST" else None), method=method,
                                         headers={"Authorization": "Bearer " + tok,
                                                  "Content-Type": "application/json"})
            try:
                up = urllib.request.urlopen(req, timeout=135)
            except Exception as e:
                # well-formed SSE error so assist.h renders something instead of hanging
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Cache-Control", "no-cache"); self.send_header("Connection", "close")
                self.end_headers()
                try:
                    self.wfile.write(("data: " + json.dumps(
                        {"type": "error", "message": "receptionist offline: %s" % e}) + "\n\n").encode())
                    self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError):
                    pass
                return
            self.send_response(up.status)
            self.send_header("Content-Type", up.headers.get("Content-Type", "application/json"))
            self.send_header("Cache-Control", "no-cache"); self.send_header("Connection", "close")
            self.end_headers()
            try:
                while True:
                    chunk = up.read(1024)
                    if not chunk:
                        break
                    self.wfile.write(chunk); self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                pass

        def _sse_message(self, screen, body):
            # Bridge: the room session (ASK…) relays to the captain (work-df) via the receptionist,
            # behind a reversible flag. Per-screen chats are unaffected.
            if str(screen) == "room" and ROOM_TO_CAPTAIN:
                raw = json.dumps({"conversation_id": "room", "text": body.get("text", "")}).encode()
                return self._proxy_reception("POST", "turn", raw)
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "close")
            self.end_headers()
            q = queue.Queue()
            DONE = object()

            def run():
                try:
                    assistant.handle_message(screen, body.get("text", ""), bool(body.get("think")),
                                             q.put, turn=body.get("turn"))
                finally:
                    q.put(DONE)

            threading.Thread(target=run, daemon=True).start()
            while True:
                try:
                    ev = q.get(timeout=3)   # keepalive every 3 s keeps work-c4's short socket timeout alive
                except queue.Empty:
                    try:
                        self.wfile.write(b": keepalive\n\n"); self.wfile.flush()
                        continue
                    except (BrokenPipeError, ConnectionResetError):
                        break
                if ev is DONE:
                    break
                try:
                    self.wfile.write(("data: " + json.dumps(ev, ensure_ascii=False) + "\n\n").encode())
                    self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError):
                    break
    return H


def build(live=False):
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    if live:
        return Assistant(ClaudeCliBackend(), LiveScreenController())
    return Assistant(MockBackend(), LiveScreenController())


def main():
    live = "--mock" not in sys.argv
    backend = ClaudeCliBackend() if live else MockBackend()
    ctl = LiveScreenController()
    assistant = Assistant(backend, ctl)
    srv = ThreadingHTTPServer(("0.0.0.0", PORT), make_handler(assistant))
    sys.stderr.write(f"{NOW()} pinpin-assistant on :{PORT} backend={type(backend).__name__}\n")
    srv.serve_forever()


if __name__ == "__main__":
    main()
