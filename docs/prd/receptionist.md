# PRD — Headset Receptionist

Owner: work-42. Coordinator: work-df. Status: DRAFT (design), 2026-11-04.

## 1. Goal

A voice **receptionist** in the headset that gives the user (and, carefully, household
members) spoken access to our live Claude Code sessions. The user opens it (from the
`ASK…` menu or a dedicated button); it greets — "who's speaking, and what do you want to
talk about?" — then either:

- **routes** the turn into an EXISTING session (default for engineering talk: the captain
  session **work-df**, so the user can keep talking to the captain from the headset; other
  sessions like work-c4 on request), streaming that session's reply back to the headset; or
- **spawns a NEW** session for a new topic (e.g. a household question), with a profile that
  matches the speaker.

It is a front desk, not an assistant: it understands intent, picks a destination, relays
text both ways, and never does engineering work itself.

## 2. Where it runs (and why)

The session index lives **only on the VPS** (`hostinger-vps`, Tailscale 100.109.138.51):
the peer sockets (`/tmp/cc-socks-1000/*.sock`), the session registry
(`~/.claude/sessions/<pid>.json`), the transcripts (`~/.claude/projects/*/*.jsonl`) and the
tmux server that hosts every session are all on the VPS. Delivery and reply-tailing must
therefore happen on the VPS.

The headset talks to the **Air assistant** (`pinpin_assistant.py`, :57840) and reaches the
VPS only as a Tailscale peer. So:

```
headset ──SSE──▶ Air  pinpin_assistant :57840  /api/reception/*
                     └──HTTP over Tailscale──▶ VPS  pinpin_receptionist :57850
```

- New service **`pinpin_receptionist.py`** (stdlib HTTP, :57850) on the VPS, run as
  `pinpin-claude`. It owns the session index, delivery, reply-tailing, and the warm
  receptionist process.
- The Air assistant gains a thin **reverse-proxy** under `/api/reception/*` (open → greet,
  POST turn, SSE reply) so the headset keeps a single endpoint. No session logic on the Air.
- Reuses the existing ASSIST **SSE** wire format (text deltas + a final event) so the
  headset chat panel needs no new client.

## 3. Session index (requirement 1)

Built by reading `~/.claude/sessions/<pid>.json` for every **live** pid (socket file present
and process alive), each of which carries: `name` (e.g. `work-df`), `sessionId`, `cwd`,
`status` (idle/busy/shell), `kind`, `messagingSocketPath`, `updatedAt`/`statusUpdatedAt`.
Joined with the transcript `~/.claude/projects/<cwd-slug>/<sessionId>.jsonl` for:

- **topic** — the transcript's `ai-title` line (auto-generated one-liner), falling back to the
  first user message, truncated.
- **last activity** — `statusUpdatedAt`, cross-checked against the timestamp of the last
  `assistant` text block.

tmux target for each session is `claude-<sessionId>` on root's tmux server. Index is sorted
most-recently-active first and marks which sessions are "active now" (busy, or active in the
last few minutes). Exposed to the receptionist as the `list_sessions` tool.

## 4. Delivery INTO a live session (requirement 2)

**Mechanism: `sudo -n tmux send-keys` into `claude-<sessionId>`** (root's tmux server),
typing the turn into that session's input and submitting it. Text is framed:

> `From the user via headset: <what the user said>`

Rationale for tmux over the peer-socket path: the socket wire protocol is internal to the
compiled `claude` binary (version 2.1.312 here, 2.1.317 on the Air) and would be brittle to
reverse and re-implement across version bumps. `tmux send-keys` is the same path a human uses
to type into a session — it queues cleanly while the session is busy, is version-independent,
and arrives as a genuine **user** turn, which is the truest representation of the user
actually speaking to that session. (If you prefer the message to arrive wrapped in the
"cross-session, from another agent" envelope instead of as a user turn, the alternative is to
run the receptionist itself as a warm `claude` process and have it call its native
`SendMessage` — see Decision D2. I recommend tmux-as-user.)

Delivery records the send timestamp; that timestamp is the watermark for reply-tailing.

## 5. Replies OUT (requirement 3)

After delivery, tail the target's `<sessionId>.jsonl` for new `assistant` `message.content`
`text` blocks with `timestamp` > the delivery watermark. Each new block is streamed to the
headset as SSE text (the ASSIST delta format). Streaming ends when the session returns to
`idle` (its registry `status`) and no new text has appeared for a short quiet window, or a
hard cap (e.g. 120 s) is hit — after which the user can send another turn. Thinking and
tool_use blocks are skipped; only user-visible assistant text is relayed.

## 6. New sessions (requirement 4) — **GATED**

- **Engineering new topic** (user only): spawn exactly like the existing sessions —
  `runuser -u pinpin-claude … claude --dangerously-skip-permissions --session-id <uuid>
  "<brief>"` inside a new `claude-<uuid>` tmux session.
- **Household / non-engineering** (e.g. the user's wife asking a household question): per
  work-df's security addendum, this must **NOT** run as `pinpin-claude` and **NOT** with
  `--dangerously-skip-permissions` (pinpin-claude has passwordless root). Instead a separate
  **unprivileged unix user** (no sudo, no access to /root, the repos or credentials), with its
  own HOME, its own `claude` login/config, and **default permission mode**.
  - **GATE: ask work-df before creating that user account.** Until then, household routing is
    stubbed (the receptionist says the household profile isn't provisioned yet).

## 7. Speaker safety & logging (requirement 5)

- The receptionist first asks **who is speaking**. Only the **user's own** sessions are
  reachable by the user; a household speaker can reach only household sessions (or a new
  household session), **never** engineering sessions — the receptionist will not route a
  household speaker into work-df/work-c4/etc. even if asked.
- Every routing decision is appended to a **routing log** (`~/.local/share/pinpin-receptionist/
  routing.log`): timestamp, speaker, action (route/new), target session, and a short text
  digest. Auditable.
- **GATE: tell work-df before the first REAL routing to its session.** All development and
  e2e tests route to a disposable **scratch** session, never to work-df's real session.

## 8. The receptionist itself (requirement 6)

A warm **haiku** `claude -p --input-format stream-json --output-format stream-json` process
(the same WarmProc/WarmPool pattern as the assistant), given a small MCP tool server exposing:

- `list_sessions()` → the index of §3 (names, topics, status, last activity).
- `route(session, text)` → deliver (§4) and begin reply-tail (§5).
- `new_session(topic, profile)` → spawn (§6); `profile ∈ {engineering, household}`;
  household is gated.

Its system prompt makes it a concise front desk: greet, confirm speaker, classify intent
(continue-with-captain by default for engineering; named session on request; new topic →
new session), confirm the destination in one short line, then route. It never edits code,
never runs shell, never reaches a session the speaker isn't allowed to.

## 9. Testing

1. **Scratch session**: spawn a throwaway `claude` session (own uuid, a harmless brief),
   route a turn to it via tmux, confirm the framed text lands and the reply tails back over
   SSE. Tear it down.
2. Index/unit tests offline (fixture `sessions/*.json` + fixture transcripts).
3. Speaker-safety tests: a household speaker is refused engineering targets; logging present.
4. Only after work-df's OK: one real routing to work-df.

## 10. Open decisions for work-df (D-list)

- **D1** Service on the VPS + Air reverse-proxy (§2). ✔ recommended.
- **D2** Delivery = `tmux send-keys` as a user turn (§4) vs peer-socket "from another agent"
  envelope. ✔ recommend tmux-as-user.
- **D3** Household unprivileged user — name/HOME/login (§6). **Blocked on your OK.**
- **D4** First real routing to work-df — **I will ping you before it.**
- **D5** Dedicated headset button vs an `ASK…` entry — coordinate with work-c4 (owns the
  shoulder/wrist menu), as with the room assistant.
