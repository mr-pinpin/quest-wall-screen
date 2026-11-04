# Per-screen assistant protocol (headset ↔ backend)

Status: draft for agreement, 2026-11-04. Owner: backend work-42. Readers: work-c4 (headset ASSIST UI),
work-ad (mini-apps server), work-df (coordination).

The user presses a **Claude button** on a wall screen and speaks. The headset already turns speech into text
(the Mini's Parakeet STT), so the headset sends **text** — there is no Enter key. Claude answers in a Markdown
chat bubble beside that screen, and can **act on that screen** ("open YouTube, search X") because each screen is
a Mac virtual display the backend can see and drive.

## 1. Where the backend runs

`pinpin-assistant` runs as a **PinPin Host plugin** on each Mac that hosts screens, so it inherits Screen
Recording + Accessibility + Input:
- **Mini** (`192.168.0.50`): the MINI screen and any Mini displays. Also the single target for app-initiated
  `/api/ask` (see §5).
- **Air**: the summoned virtual displays.

A screen id routes to the instance on the Mac that owns its CGVirtualDisplay. The headset talks to one logical
endpoint; routing by screen→machine is the backend's job. Default port: **57840**.

It never acts on the main / built-in display of either Mac. A screen whose display id is the main display is
rejected.

## 2. Session model

- One **session per screen id**. State: the screen id, the underlying `claude` session id, the message history,
  and the per-screen action log. History persists across backend restarts (one JSON file per screen).
- A session is conversational: follow-ups ("make it bigger", "no, the other video") continue the same thread.
- `POST /session/<screenId>/reset` starts a fresh thread for that screen (keeps the log).

## 3. Sending the user's speech

```
POST /session/<screenId>/message
Content-Type: application/json
{ "text": "open youtube and search for lofi", "think": false }
```
- `text`: the STT transcript. No Enter semantics — each POST is one user turn.
- `think` (optional, default false): `true` routes the turn to the stronger model (opus) for "think harder".

The response is an **SSE stream** (`Content-Type: text/event-stream`). Events (one JSON object per `data:` line):

| `type`    | fields | meaning |
|-----------|--------|---------|
| `start`   | `turnId`, `screen` | the turn began |
| `text`    | `delta` | a Markdown chunk of the reply; **append** to the bubble |
| `action`  | `action`, `detail`, `status` (`begin`\|`ok`\|`error`) | the assistant touched the screen: e.g. `{"action":"open_app","detail":"Safari","status":"begin"}`, then `open_url`, `click`, `type`, `key`, `screenshot`. Show as a quiet status line under the bubble. |
| `error`   | `message` | the turn failed; show it, keep the session |
| `done`    | `turnId`, `stopReason` (`end`\|`max_steps`\|`timeout`), `steps` | the turn is complete |

The bubble consumes `text.delta` by appending (not full re-render). Markdown is rendered once the stream ends or
at paragraph boundaries — work-c4 confirms which.

Turns are capped: **max 12 tool steps** and **90 s wall time** (configurable). On a cap, the stream ends with
`done.stopReason = max_steps | timeout` after a short apology `text`.

## 4. Reading history

```
GET /session/<screenId>            -> { "screen", "claudeSessionId", "messages":[{role,content,ts}], "turns":N }
GET /session/<screenId>/actions    -> the action log (JSONL) for that screen
```

## 5. App-initiated ask (replaces work-ad's stub)

Mini-apps (`192.168.0.50:57830`) forward their `/api/ask` to the backend so apps can ask the model without
computer use:
```
POST /api/ask
{ "app":"recipes", "screen":"<id>", "prompt":"...", "system":"...optional...", "json": false }
-> { "text":"...", "json": <object, only when json:true> }
```
This is one-shot (no stream, no screen actions) and is served by the **Mini** instance. `json:true` asks the
model for a single JSON object and returns it parsed in `json`.

## 6. Voice routing (agreed with work-c4 + work-ad)

Two paths, chosen by the headset by **which control has focus at speak time**:
1. **Claude button active** → `POST /session/<screenId>/message` (this doc). Conversational + computer use.
2. **An app's own dictation field** (e.g. recipes "eggs, tomatoes") → straight to the app via mini-apps
   `POST /api/screens/<id>/say`. The backend is **not** in this path.

The backend does not call `/say` for ordinary dictation. If the assistant decides to put text into an app it
does so as a `type` computer-use action, which the app sees as normal keyboard input.

## 7. Computer use (what `action` events can report)

Scoped to the one screen's CGVirtualDisplay, reusing PinPin Displays (`:57710` HTTP, `:57711` UDP mouse) plus a
new per-display screenshot. Tool set: `screenshot`, `move(u,v)`, `click(u,v)` (u,v in 0..10000 across that
display), `type(text)`, `key(combo)`, `open_app(bundle)`, `open_url(url)`, `place_window(app)`. Every call is
logged per screen and surfaced as an `action` event. The backend never sends input to any display but the
target screen's, and never to the main/built-in display.
