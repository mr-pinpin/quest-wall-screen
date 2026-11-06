# Mini screen-tools contract (K20)

> **LIVE 2026-11-06 (user: "Full control").** The MINI Claude button drives the Atlas kiosk over CDP.
> On the Mini, `~/pinpin-apps-data/local-cdp.json` enables screen 1:
> `{"1": {"port": 9612, "match": null, "allow": ["shot","eval","click","move","scroll","type","key"]}}`.
> `match` is **null on purpose** (not `"atlas-webgpu.html"`): the runner picks the CDP target by the first
> page whose URL contains `match` (apps_runner.py:62), there is exactly one page target on 9612, and a
> non-null match would stop matching the moment the brain navigates off the storyboard — so it could
> neither drive nor return. `null` always selects the one kiosk tab and survives navigate + return.
> **`open`/navigate is hard-403'd** on a kiosk screen (apps_server.py:1565, "the kiosk page IS the
> screen") — the screen-1 assistant prompt navigates via `eval("location.assign('…')")` instead,
> records the original storyboard URL at session start, announces before leaving it, and can always
> return. The screen-1 kiosk prompt lives in `assistant/pinpin_assistant.py` (`KIOSK_SCREENS`,
> default `{1}`, env `PINPIN_KIOSK_SCREENS`). Verified at the apps-server level (shot 200 PNG
> 1920x1080, eval, harmless click, open→403); full headset→/session/1 e2e pending the Air GUI login
> (PinPin Host is a GUI login item; the Air's WindowServer died 17:45Z 2026-11-05).

> **SHELVED 2026-11-04 (work-df + work-c4).** The MINI screen turned out to be the Atlas Chrome
> kiosk (chrome-atlas-profile, atlas-webgpu.html) with remote debugging on 127.0.0.1:9612 — not a
> native-desktop display. So K20 is done the way the Air web screens work: **CDP** —
> `Page.captureScreenshot` for /shot, `Input.dispatchMouseEvent` + `insertText` for input, reusing
> work-ad's runner CDP code. That needs **no Screen Recording or Accessibility grant** (the user's
> "no further permissions"). The native helper below (built, compiles on the Air targeting macos14,
> runs on the Mini) is KEPT for a future native-desktop case but is NOT used for K20. The VPS brain
> is built against the CDP path instead. Button entry unchanged: the MINI Claude button posts to
> /session/1.

---


Token-protected HTTP on the Mini, served by `pinpin-mini-screen` (a small helper with its own
Screen-Recording + Accessibility grant). It exposes screenshot + input of ONE display — the MINI
wall screen, the virtual display named **"PinPin Atlas Owned"** (the one Sunshine streams). The
MINI screen's Claude brain runs on the VPS and drives the screen through this API. The helper never
touches Sunshine, never changes displays, and only ever acts on that one named display.

- Bind: `:57860`, source-restricted to loopback / 192.168.x / 100.x (Tailscale).
- Auth: `Authorization: Bearer <token>` (`~/.local/share/pinpin-mini-screen/token`, 600) on everything
  except `/health`.
- Target resolved BY NAME every call (CGDirectDisplayID changes when displays are re-created —
  1975 → 1984 today). Coordinates `u,v` are `0..10000` across the display, `0,0` = top-left, like the
  Air assistant's screen tools.

## GET /health   (no auth — the apps-server /status row polls this)
```
200 {"ok": true, "display": 1984, "name": "PinPin Atlas Owned",
     "bounds": {"x": 1920, "y": 0, "w": 1920, "h": 1080}}
200 {"ok": false, "error": "display 'PinPin Atlas Owned' not found"}   // helper up, display missing
```

## GET /shot   (Bearer)   `?max=<px>` (default 900, long edge)
`200 image/png` — ScreenCaptureKit capture of the target display, uniform scale so **u,v map 1:1 onto
the image** (no crop/letterbox), cursor hidden. Headers `X-Display-Id` and `X-Display-Bounds`
(`x,y,w,h`) so a shot and a click always refer to the same display generation. `500` capture failure;
`503` if the named display isn't found.

## POST /input   (Bearer)   body `{"op": ..., ...}`
→ `200 {"ok": true, "op": ..., "x": <global px>, "y": <global px>, "display": <id>}` (echoes where it
acted, so the brain can spot a stale display or a clamp). Every op WARPS the cursor into the target
display first (libvirtualhid / the click path drop a click when the cursor is outside it).

**Shared cursor — the brain must NOT act while the user is pointing.** That display is what the headset
streams; the user's Moonlight pointer and the helper's warps move the SAME system cursor. Before each
input the helper checks for REAL user HID input (excluding its own posted events) in the last ~3 s and,
if found, returns `409 {"error": "user_active", "idle_s": <s>}` — unless the body sets `"force": true`.
Inputs are serialised (one at a time) so callers can't interleave move/click.

| op | fields | effect |
|---|---|---|
| `move` | `u,v` | warp + move the pointer to u,v |
| `click` | `u,v`, `button` (left\|right\|middle, default left), `count` (2 = double) | warp to u,v, hover, then button down/up |
| `scroll` | `u,v,dy,dx` | warp to u,v, then pixel scroll. **dy>0 scrolls DOWN (reveals content below), dx>0 scrolls right** (matches the Air w-handler; sign confirmed on the first on-screen test) |
| `type` | `text` | type the unicode text (capped 5000) into the focused element |
| `key` | `combo` | case-insensitive combo, e.g. `"Return"`, `"Cmd+L"`, `"cmd+t"` |

`400` unknown op; `503` if the display vanished (re-resolve). The VPS brain screenshots after each input
(verify-after-act). Every input is appended to a small rotating audit log
(`~/.local/share/pinpin-mini-screen/input.log`: time, op, u, v, caller IP) so our actions are
distinguishable from the click bug / work-ad's cursor watcher.

## Notes
- The MINI screen's front app is the resident's Chrome (chrome-atlas-profile). This helper does PIXEL
  input only; it does NOT drive that Chrome over CDP (that Chrome is work-c4 / Atlas's). For web
  content, CDP there may beat pixel clicks — coordinate with work-c4 before any such use.
- Grant: the Mini has no PinPin Host and no stable signing identity, so the helper takes its own
  one-time Accessibility + Screen-Recording grant. No-re-prompt-on-rebuild is a follow-up (self-signed
  persistent cert, or a minimal Mini PinPin Host).
