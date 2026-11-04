# Mini screen-tools contract (K20)

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

## GET /shot   (Bearer)
`200 image/png` — ScreenCaptureKit capture of the target display, long edge ~900 px, cursor hidden.
`500` on capture failure; `503` if the named display isn't found.

## POST /input   (Bearer)   body `{"op": ..., ...}` → `200 {"ok": true, "op": ...}`
Every op WARPS the cursor into the target display first (libvirtualhid / the click path drop a click
when the cursor is outside it), then acts at the current cursor.
| op | fields | effect |
|---|---|---|
| `move` | `u,v` | warp + move the pointer to u,v |
| `click` | `u,v` | warp to u,v, hover, then left down/up (single click) |
| `scroll` | `u,v,dy,dx` | warp to u,v, then pixel scroll. **dy>0 scrolls DOWN (reveals content below), dx>0 scrolls right** (sign to be confirmed on the first on-screen test, verify-after-act) |
| `type` | `text` | type the unicode text into the focused element |
| `key` | `combo` | key combo, e.g. `"return"`, `"cmd+l"`, `"cmd+t"` |

`400` on unknown op. The VPS brain screenshots after each input (verify-after-act).

## Notes
- The MINI screen's front app is the resident's Chrome (chrome-atlas-profile). This helper does PIXEL
  input only; it does NOT drive that Chrome over CDP (that Chrome is work-c4 / Atlas's). For web
  content, CDP there may beat pixel clicks — coordinate with work-c4 before any such use.
- Grant: the Mini has no PinPin Host and no stable signing identity, so the helper takes its own
  one-time Accessibility + Screen-Recording grant. No-re-prompt-on-rebuild is a follow-up (self-signed
  persistent cert, or a minimal Mini PinPin Host).
