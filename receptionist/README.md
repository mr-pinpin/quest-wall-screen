# Headset Receptionist

A voice front desk in the headset that connects the person speaking to one of the user's
live Claude Code sessions (or starts a new one), relaying words in and replies out. Design:
`docs/prd/receptionist.md`.

## Pieces

| File | Role |
|---|---|
| `recept_core.py` | Session index (reads `~/.claude/sessions/*.json` + transcripts), `deliver()` via `sudo tmux send-keys`, `tail_reply()` from the transcript, `spawn_engineering()`. |
| `pinpin_receptionist.py` | HTTP service (:57850). Auth, dual-bind loopback+Tailscale, `/api/sessions`, `/api/propose`, `/api/route` (SSE), `/api/new`, `/api/turn` (the warm brain), audit log, confirm gate, block-list. |
| `receptionist_mcp.py` | MCP stdio server giving the warm brain its tools (`list_sessions`, `propose_route`, `route`, `new_session`) — all through the service. |
| `receptionist-prompt.txt` | The brain's system prompt (front-desk behaviour, speaker safety, verbatim, confirm). |
| `receptionist-mcp.json` | `--mcp-config` wiring for the warm `claude -p` process. |
| `test_receptionist.py` | Offline tests (fixtures; no live sessions/network/tmux). 22 checks. |

## Topology

The index (sockets, `~/.claude/sessions`, transcripts, root's tmux server) is local to the
**VPS**, so this service runs on the VPS as `pinpin-claude`. The headset reaches it through
the Air assistant's `/api/reception/*` reverse-proxy over Tailscale — see
`docs/receptionist-air-proxy.md` (staged; needs an Air deploy).

```
headset ──SSE──▶ Air pinpin_assistant :57840 /api/reception/* ──Tailscale──▶ VPS :57850
```

## Security (per work-df)

1. **Verbatim only** — `route()` sends the user's words unchanged, prefixed `[via headset] `.
   The brain chooses the destination, never the text; `-l` literal send never interprets it.
2. **Auth + binding** — every request needs the bearer token
   (`~/.local/share/pinpin-receptionist/token`, 600); bound to 127.0.0.1 and the Tailscale IP
   only, never 0.0.0.0.
3. **Spoken confirm** — engineering routes require `/api/propose` then a spoken yes
   (`confirmed:true`). The brain reads the confirmation line aloud first.
4. **Audit** — every routing appends to `routing.log` (time, speaker, session, text hash).
5. **Speaker safety** — only `speaker:"user"` may reach engineering sessions; others are 403.
6. **Block-list** — `PINPIN_RECEPT_BLOCK=<names/ids>` hard-refuses listed sessions
   (used in testing to make work-df unreachable).

## Gates (do not cross without approval)

- **Household profile** is stubbed (423). Creating the separate unprivileged unix user needs
  work-df's OK (`PINPIN_RECEPT_HOUSEHOLD=1` only after it exists).
- **First real routing to work-df** — keep the service off the headset path / work-df blocked
  until work-df is pinged.

## Run / test

```sh
# offline tests
python3 receptionist/test_receptionist.py

# service (production: no block-list; the user WANTS the captain reachable)
python3 receptionist/pinpin_receptionist.py

# service (testing: work-df unreachable)
PINPIN_RECEPT_BLOCK="work-df" python3 receptionist/pinpin_receptionist.py
```

Proven end-to-end against a disposable scratch session: index → propose → confirm gate →
verbatim delivery → transcript reply relayed back, and the warm haiku brain driving it over
`/api/turn`.
