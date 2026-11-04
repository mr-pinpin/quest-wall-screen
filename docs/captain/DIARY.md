# Captain's technical diary (work-df)

Times are UTC. Hourly entries are short; a daily entry is written at about 20:57.

## 2026-11-04, the night shift, 01:30–09:55

- **What went right:**
  - Four agents ran in parallel all night: c4 (headset), 09→34 (geometry, benchmarks), 42 (assistant), and ad (apps, new tonight).
  - The headset went from v0.45 to v0.64.
  - Shipped: the Claude chat per screen, now warm at ~1.4 s to first word; computer use through CDP; apps with a sandbox; house.json plus consolidation; the mesh scan; staying awake on the charger (219/221 min).
- **What went wrong, and the lessons:**
  - Two agents installed at once, because the locks didn't match. Now one lock, and an owner guard in code.
  - The "night prox ban" I imposed left the agents waiting. The user said wake the headset, don't wait, so don't over-correct with blanket bans.
  - The user's screens "moved to another room" twice. The first time was Meta relocalising in the dark (lights fixed it). The second was a scene query racing relocalisation. Make placement robust in code (re-query, cache), not by telling the user what to do.
  - An agent scan shrank real walls, because it overwrote scene extents. Measurements must never overwrite the source of truth: keep a separate "fit" field.
  - The scan commit frame drifts (the floor at +6/+16/+26 cm). Found by an automatic consolidation score, not by eye. Automated quality verdicts pay off.
  - A forced debug flag (GEOMETRY) was left on for the user. Every force file must lose to the user's own toggle.
  - The laptop's animated wallpaper plus colorsync ate ~1.3 cores and quietly skewed the benchmarks. Record the environment's load per run.
- **On coordination:**
  - The user wants a captain who delegates rather than codes, who is never idle, and who is honest about failures.
  - They are their own single entry point to me, and I delegate non-engineering topics to spawned sessions.
- **Next:**
  - the receptionist, so the user's headset voice reaches me (by 10:50);
  - a proper kanban;
  - the user's kanban: dictation always on, pointer smoothing, controls always visible, the select outline.

## 2026-11-04 10:00 UTC
- **Decided:**
  - The user wants to talk only to me. I dropped the household account (overcomplication) and had work-42 bridge ASK… to work-df through the VPS receptionist (tmux delivery, transcript tail, verbatim, token, audit).
  - Re-split work for parallelism: c4 takes only the CLAUDE entry; 34 takes the frame-drift fix; a new agent, k5 (work-0e), writes self-contained headers so c4 isn't the bottleneck.
- **Shipped:**
  - k5: steady.h, select_outline.h, dictation.h, panel_texture.h, select_house.h (4626f40, e6b6b82, 351a4b2, f960dbc).
  - 34: the frame fix through Meta-plane consensus (8db4341, 960d7ff), load-tolerant timing tests (96b07d1).
  - ad: a kanban board and CLI (0ec2f9b).
  - 42: room.sh, the reply cleaner, the queued server, and the Air cost profile.
- **Broke, and lessons:**
  - Twice an install rolled back on a stale check bar (fps ≥70, then scene count = 14). Pass/fail bars must come from a fresh baseline measurement, not constants.
  - The user's room chat went to the room assistant, not to me, and that confused them. An entry point needs to say who you are talking to.
- **Learned:** the Air's slowdown per extra screen is the aerial wallpaper plus colorsync, not our capture (~5%). Only the user can switch it.
- **Next:** v0.65 confirmed on the headset; k5 integrates v0.66; c4 does HOUSE sync (K9) and the headset fps profile; the user returns about 10:50.

## 2026-11-04 11:00 UTC
- **Decided:**
  - The user now talks to me from the headset: CLAUDE in the ring (v0.65) → receptionist → work-df.
  - Shipped v0.66/0.67: HOUSE sync with a local-first push, and the 6/7 repair attempt.
  - A 5th agent (work-0e) builds headers and integrates them, so c4 isn't the bottleneck.
- **Broke, the big one:** work-42's PinPin Displays deploy left THREE owners (PinPin Host child, a launchd KeepAlive agent, an `open` copy). They fought over :57710, displays were re-created every few seconds, the Air hit load 246, and the user couldn't use their Mac. I fixed it by hand: booted out the agent, killed the extras, so only the Host child runs.
- **Lesson:** each service has one owner, and on a live machine never "restart until it works". Now in AGENTS.md (b296d20).
- **Broke:** screens 6/7 couldn't be recovered automatically, because depth walls lived in STAGE and wrong rebases corrupted the cache. The user re-places them. v0.67 has the Meta-only check.
- **Broke:** colorsync was pegged since 02:04. It was NOT the wallpaper; I relayed a wrong diagnosis and sent the user on a detour. A reboot fixed most of it (100% → 30%).
- **Broke:** after the reboot the Air joined the ZyXEL with Tailscale off. Rejoined it to RT-5WiFi (.105), removed the other home networks, started Tailscale.
- **Next:** work-0e's combined v0.68 (dictation, STEADY, select, panels, chat history); the campaign resumes; a network-health check on /status.

## 2026-11-04 12:00 UTC
- **Decided:**
  - The user handed the Mac to the agents ("don't make it sluggish"). Guards went on every bench run: start only at load < 4; stand down if load > 6 or WindowServer +60 for 30 s.
  - Kitchen = screen 3 (recipes), found from wall geometry. Spotify quit, plus its autostart off.
- **Shipped:**
  - the native click (TextEdit closed) and scroll (scrollY 0→1200) both proven on the extra displays;
  - placement-review fixes (4 bugs, plus MULTIROOM doorway pollution, plus visible setup-change and repair hints) as v0.71;
  - consolidate.h, live (v0.72, off);
  - the network and colorsync rows on /status.
- **Learned, the big one:** every virtual-display create/remove permanently ramps colorsync's polling (92→203 req/min), and only a reboot resets it. Today's slowness came from our own churn, not the wallpaper; I relayed a wrong diagnosis earlier.
  - Rule: a fixed scratch-display pool (ids 1–64 only; bench 51–59).
  - The old sck bench runs even used the user's own screen ids 2..N.
- **Broke:** check scripts rolled back or restarted under the user 3 times today (stale bars, skipped checks counted as failures). Now: never roll back while worn; install only at STANDBY.
- **Broke:** agents quoted commit hashes from memory, twice wrong. Quote only from git output.
- **Next:** v0.71 auto-installs at STANDBY; the MINI-screen Claude button (VPS brain + Mini helper); the campaign A2→F; a quiet reboot later to reset colorsync.
