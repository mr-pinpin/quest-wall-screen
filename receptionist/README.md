# Headset Receptionist

A simple voice front desk in the headset. Everything the user says is relayed **verbatim** to
the captain session **work-df**, and work-df's reply is streamed back. work-df is the single
entry point and delegates non-engineering topics itself (recipes, household questions) by
spawning sessions with `spawn_session.sh`. The user can say "talk to work-c4" to switch the
destination for a chat; otherwise there is no "who's speaking" and no per-message confirm.
Design: `docs/prd/receptionist.md`.

## Pieces

| File | Role |
|---|---|
| `recept_core.py` | Session index (`~/.claude/sessions/*.json` + transcripts), `deliver()` via `sudo tmux send-keys`, `tail_reply()` from the transcript, `spawn_engineering()` (launches in the TRUSTED workspace so a fresh session doesn't stall on the trust prompt). |
| `pinpin_receptionist.py` | HTTP service (:57850): token auth, dual-bind loopback+Tailscale, `/health`, `/api/sessions`, `/api/turn` (relay, with "talk to X" switching), `/api/spawn`, audit log, block-list. |
| `spawn_session.sh` | `spawn_session.sh "<topic>" "<brief>"` — start a new session like the existing ones; prints `<name> <id>` once it registers. work-df's hand-off helper. |
| `test_receptionist.py` | Offline tests (fixtures; no live sessions/network/tmux). 21 checks. |
| `run.sh` | Launch the service. |

## Topology

Index (sockets, `~/.claude/sessions`, transcripts, root's tmux) is local to the **VPS**, so the
service runs there as `pinpin-claude`. The headset reaches it via the Air assistant's
`/api/reception/*` reverse-proxy over Tailscale — see `docs/receptionist-air-proxy.md`.

```
headset ──SSE──▶ Air :57840 /api/reception/{turn,sessions} ──Tailscale──▶ VPS :57850
```

## /api/turn

`POST /api/turn {conversation_id, text}` → SSE. If `text` says "talk/switch/connect to <a live
session name>", the destination for that chat switches (event `switched` + a spoken ack).
Otherwise `text` is delivered VERBATIM (`[via headset] ` prefix, `-l` literal) to the current
destination (work-df by default) and the reply streams back as `delta` events.

## Security

- Bearer token (`~/.local/share/pinpin-receptionist/token`, 600) on every request.
- Bound to 127.0.0.1 and the Tailscale IP only, never 0.0.0.0.
- Relays words unchanged; the service never rewrites routed text.
- Append-only `routing.log`: time, action (route/switch/spawn/blocked), session, text hash.
- `PINPIN_RECEPT_BLOCK=<names/ids>` hard-refuses sessions (used in testing to keep work-df
  unreachable until the first-real-message gate is cleared).
- `PINPIN_RECEPT_DEFAULT=<name>` overrides the default destination (default `work-df`; set to a
  scratch session for testing).

## Run / test

```sh
python3 receptionist/test_receptionist.py                 # offline
python3 receptionist/pinpin_receptionist.py               # production (default -> work-df)
PINPIN_RECEPT_DEFAULT=work-d5 PINPIN_RECEPT_BLOCK=work-df \
    python3 receptionist/pinpin_receptionist.py            # testing against a scratch session
```

Proven end-to-end against disposable scratch sessions: verbatim relay (exact text in the
target's transcript), multi-turn straight-through with no confirm, "talk to X" switching,
and audit. Gate held: work-df received no test traffic (structurally blocked throughout).
