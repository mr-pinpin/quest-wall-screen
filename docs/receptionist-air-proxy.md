# Staged: Air-side reverse-proxy for the receptionist

Status: STAGED 2026-11-04, not deployed. Owner: work-42. Needs an Air plugin redeploy (a
pure HTTP addition — no new TCC grant). Keep off the headset path until the first-real-routing
gate is cleared with work-df.

The headset talks to the Air assistant (`pinpin_assistant.py`, :57840). The receptionist runs
on the VPS (:57850). The Air just forwards `/api/reception/*` to the VPS over Tailscale and
streams the SSE straight back — no session logic on the Air.

## 1. Token

The Air needs the same bearer token as the VPS receptionist. Copy it to a 600 file on the Air:

```sh
# on the VPS
scp ~/.local/share/pinpin-receptionist/token \
    pinpin-air:~/.local/share/pinpin-assistant/receptionist-token   # chmod 600
```

## 2. Handler (add to pinpin_assistant.py, in the request router)

```python
RECEPT_BASE = os.environ.get("PINPIN_RECEPT_BASE", "http://100.109.138.51:57850")  # VPS Tailscale IP

def _recept_token():
    try:
        return open(os.path.expanduser("~/.local/share/pinpin-assistant/receptionist-token")).read().strip()
    except OSError:
        return ""

def _proxy_reception(self, suffix, body):
    """Forward POST /api/reception/<suffix> to the VPS receptionist, stream SSE back verbatim."""
    import urllib.request
    url = RECEPT_BASE + "/api/" + suffix            # turn | sessions | propose | route | new
    req = urllib.request.Request(url, data=body, method=self.command,
                                 headers={"Authorization": "Bearer " + _recept_token(),
                                          "Content-Type": "application/json"})
    up = urllib.request.urlopen(req, timeout=130)
    ctype = up.headers.get("Content-Type", "application/json")
    self.send_response(200); self.send_header("Content-Type", ctype)
    self.send_header("Cache-Control", "no-cache"); self.send_header("Connection", "close"); self.end_headers()
    while True:
        chunk = up.read(1024)
        if not chunk: break
        self.wfile.write(chunk); self.wfile.flush()
```

Route `self.path.startswith("/api/reception/")` → `_proxy_reception(path[len("/api/reception/"):], raw_body)`.
The headset opens an SSE on `POST /api/reception/turn {conversation_id, text}`; the receptionist
relays the text verbatim to work-df (or a switched destination) and streams the reply back
through the same pipe. `GET /api/reception/sessions` is also proxied (for an optional roster view).

## 3. Entry point (coordinate with work-c4)

Add a "Receptionist" item to the shoulder/wrist `ASK…` menu (or a dedicated button) that opens
the chat panel bound to `/api/reception/turn`, same SSE client as ASSIST. (D5 in the PRD.)
