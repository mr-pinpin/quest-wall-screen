# The PinPin assistant (per-screen + room Claude)

A Claude button on every wall screen (and an "ASK…" room session) in the headset. The user speaks; the headset
sends the STT text; Claude answers in a bubble and can act on that screen or the room. Code: `assistant/`.
Protocol: `docs/prd/assistant-protocol.md`. Eval: `bench/assistant_eval.py` + `bench/results/`.

## Pieces

- **`assistant/pinpin_assistant.py`** — stdlib HTTP service, port **57840**. One session per screen id (+ a
  `room` session); `POST /session/<id>/message {text,turn}` → SSE (`text` deltas + `action` events + `done`),
  persistent history, per-screen action log, turn caps (180 s / 20 steps). Also `POST /api/ask` and
  `POST /api/build` (for the mini-apps server / control page), `GET /debug/screenshot/<id>`, `GET /health`,
  `GET /health/auth`.
- **`assistant/screen_mcp.py`** — the MCP stdio server the `claude` CLI loads, pinned to one screen via
  `PINPIN_SCREEN`. Tools: screenshot / move / click / scroll / type / key / open_url / **eval(js)** /
  **play_youtube** (web); authoring_guide / list_apps / get_app / put_app / assign_app (apps);
  house_info / new_screen / move_screen / resize_screen / close_screen (house).
- **`assistant/pinpin-screencap.swift`, `pinpin-type.swift`** — per-display capture (ScreenCaptureKit, ~900 px)
  and key injection; run as PinPin Host children so they inherit TCC.

## Model routing (speed)

`claude` CLI login is the backend (no API key; runs only as a PinPin Host child, where the Keychain login works).
Per turn: plain **chat → haiku**, action/build text → **sonnet**, "think harder" → **opus** (`pick_model`).
- **Warm pool** (`WarmProc`/`WarmPool`): a persistent `claude --input-format stream-json` process per screen for
  chat turns (no cold start) → first-word ~**1.4 s**. Cap 4, 10-min idle reap, RSS logged. Action/build/think run
  cold; cold is also the fallback if a warm proc is dead/hung.
- **Streaming**: `--include-partial-messages` streams text token-by-token.
- **Memory bridge**: warm (chat) and cold (action) processes have separate in-memory contexts. Each warm proc
  keeps a transcript watermark and is fed a compact note of the unseen cold-turn actions before its next turn, so
  mixed conversations ("open YouTube" then "did it work?") stay coherent.

## Acting on a screen

Web screens are Chrome `--app` windows owned by work-ad's runner (mini-apps server, `192.168.0.50:57830`).
- **open_url / click / scroll / type / key / eval** all go through the runner's CDP (`/api/screens/<id>/…`), not
  OS events — CGEvent clicks don't register on `--app` windows. pinpin-displays is the fallback for native apps.
- **Reliable play**: `play_youtube` opens the search, evals the first `/watch` href, opens it, nudges `play()`.
- **verify-after-act**: every mutating tool returns a fresh screenshot so the model confirms from pixels.

## House / room

house.json on the Mini house server (`:57800`). `new_screen` allocates `nextScreenId`, creates the Mac display,
writes a schema-valid screen with `If-Match` rev, optionally assigns an app/url. move/resize/close edit it;
`direction:left/right/up/down` for nudges. Only `screens[]` + `nextScreenId` are touched. Dev/test against a
**scratch** server on `:57801`; first real write is on the user's voice.

## Safety

Destructive actions (close_screen, put_app replace) need a spoken confirmation: the first tool call returns
`needs_confirmation` and does NOT act; a later call (new turn, after the user says yes) carries it out. File-backed
registry so it survives the per-turn MCP process. Reversible: close = `on:0`; apps via the apps server's git.

## Credentials & deployment

- **Apps token** (work-ad): `Authorization: Bearer` on every powerful apps call; read from a local 600 file
  (`~/.local/share/pinpin-apps/token` on the Air, `/root/.config/pinpin-apps/token` on the VPS). Never committed.
- **Air** (`pinpin-air` over Tailscale; its LAN IP rotates on MAC change — was .102, .104, now .105 — so
  reach it by Tailscale/mDNS, never a pinned LAN IP): the live instance, a PinPin Host plugin
  (`assistant/deploy.sh`) — full computer use.
- **Mini** (`192.168.0.50`): a LaunchAgent for the MINI screen; staged **disabled** until the user re-auths
  `claude` (`GET /health/auth` goes green). `docs/mini-assistant-reauth.md`.
