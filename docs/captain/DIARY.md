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

## 2026-11-04 13:00 UTC
- **Shipped (k5 / work-0e):**
  - consolidate.h phase 1+2 plus the live scan-quality verdict (v0.72–0.74; byte-exact against consolidate.py on 12 fixtures, 0.26 ms);
  - WALLTOOL on clean walls (v0.75: lines bend around corners, and the w-w-d id persistence bug fixed);
  - docs/try-v075.md.
- **Shipped (ad):**
  - the nightly health suite (01:40, 5/6 green; red = colorsync);
  - the scratch house server under launchd, with a single owner;
  - the network and colorsync rows on /status.
- **Learned, worn detection:** prox_close makes dumpsys say HEADSET_MOUNTED. c4's scripts mistook our own simulated wear for the user, rolled back v0.71 and left the prox stuck. Real wear = mounted AND virtual prox not CLOSE, and the release always runs in finally. My earlier report to the user ("restarts under you") was probably wrong, and I corrected it.
- **Broke:** the headset has been off the network since ~12:24 (no ping), probably asleep or rebooted (which loses the root wake lock). v0.72–0.75 are queued behind it, and the user has been asked to check the charger.
- **Kanban:** 14 cards moved to Done. The open work is mostly "waiting for the user to try" (SETCAT, STEADY, CTLTOP, CLEANLIVE, WALLS).
- **Next:** the headset back online, v0.75 installed, then the guided tour; K20 (MINI button); the campaign; a quiet reboot of the Air for colorsync.

# Day 2026-11-04 (written 2026-11-06 01:20 UTC; Nov 5 had no captain activity)

## Arc of the day
- The night shift and morning ran 4–5 agents in parallel; the headset went from v0.45 to v0.75.
- The big user-visible wins:
  - talking to the captain from the headset (CLAUDE ring → receptionist → work-df);
  - the Claude chat per screen, warm (~1.4 s first word), with CDP clicks and app building;
  - mini-apps, including the kitchen recipes on screen 3;
  - the house file plus automatic consolidation, and the mesh scan with clean walls and a guided "Room: good / not yet" hint;
  - the wall tool;
  - native click and scroll proven on the extra displays;
  - a proper kanban, status, help and nightly health.
- In the afternoon the user handed the Mac over. Network, colorsync and display hygiene took most of it.

## Main decisions and why
- **Captain delegates, never codes.** I re-split work whenever c4 became the bottleneck, and spun up k5 (work-0e) to write self-contained headers that c4 merges.
- **One entry point.** The user talks only to work-df; I delegate non-engineering topics. No household account (the user's "keep it simple").
- **Every user-facing feature ships behind a flag**, and the user flips it. Automated quality verdicts (the consolidation score, the benchmark rules pre-registered before the data) beat eyeballing.
- **PinPin Host is the single owner of pinpin-displays.** That's the plugin model the user asked for, so rebuilds don't re-prompt.

## Mistakes and lessons (in memory files)
- **Three-owner process fight** (launchd agent + `open` + Host): the Air hit load 246 and the mouse lagged. Lesson: one owner per service; never "restart until it works" on a live machine.
- **Display churn ramps colorsync permanently.** Lesson: a fixed scratch-display pool (ids 1–64; bench 51–59). The bench even used the user's screen ids.
- **Wrong diagnoses relayed:** the wallpaper, and "restarts under the user" (it was our own prox). Lessons: verify before relaying; real wear = mounted AND prox not CLOSE.
- **Check scripts rolled back healthy builds** on stale bars and skipped checks. Lesson: bars come from a fresh baseline, and nothing rolls back while the headset is worn.
- **Placement:** STAGE-frame depth walls lost screens 6 and 7, and a scan overwrote Meta wall extents. Lessons: Meta-plane consensus frame, measurements never overwrite the source of truth, and k5's review found 4 more bugs.
- **Agents quoted commit hashes from memory.** Lesson: quote only from git output.

## State per area
- **c4 (headset):** v0.72 is installed but unverified; the headset has been OFFLINE since 11-04 12:24Z. v0.73–0.75 are queued: consolidation, scan hint, WALLTOOL.
- **0e (k5):** waiting for the headset.
- **34:** the campaign A2 gave no KEEPs; B–F were held by the screen saver (now disabled), and consolidate/2 is being deployed.
- **42:** the MINI button via CDP, full control approved by the user; enabling and testing now.
- **ad:** nightly health 5/6; investigating audiomxd at 99%.

## Tomorrow's plan
- The headset back online (the user), then install v0.75 and relay the try-v075 tour.
- The MINI button live; the campaign B–F; fix audiomxd; one quiet Air reboot if colorsync climbs again.

## 2026-11-06 01:00 UTC
- **The user decided:** MINI screen Claude = full control. Told work-42 to enable local-cdp.json for screen 1 (9612), with a "leave the storyboard" warning plus a way back.
- **Air:**
  - the idle aerial screen saver was eating ~1.6 cores, so I disabled it (idleTime 0);
  - audiomxd is now at 99%, and work-ad is investigating.
- **Campaign:** A2 re-judged with no KEEPs, and nothing goes to production. Closed 4 ghost jobs in activity.log.
- **Headset:** still offline since 11-04 12:24Z, so c4 stopped polling. The user has been asked to wake it.

## 2026-11-06 02:00 UTC
- **The user spoke from the headset** (~01:25–01:40): add a screen on the big wall (it's full, so I offered options), make screen 3 bigger (0.68 → 0.90 m), nudge it left, turn screen 3 off, close screen 2. All done as reversible house edits (rev 31–35, on:0) through room_tools.py.
- **I couldn't verify any of it on the headset.** adb is offline and the headset doesn't answer ping on 192.168.0.x, yet its speech reaches me. Lesson: I need a read path that doesn't depend on adb (e.g. the headset posting its status to the Mini).
- **The Air's GUI session has been dead since 11-05 17:45Z:** WindowServer was SIGKILLed for exceeding the Mach port limit after a 30 h leak, likely from display churn. Everything GUI is gone (Host, the assistant, Chrome screens). audiomxd spins as a side effect. The fix needs the user: log in, or reboot and log in.
- **Shipped:**
  - K20, the MINI kiosk via CDP (072227d; match:null so it survives navigation);
  - the bench-lock fail-open fix (f89b491);
  - consolidate/3 with an honest verdict (bfaf70a);
  - a campaign guard that refuses when there's no GUI session (29f3be1).
- **All agents are idle,** blocked on the user (Air login, headset). That's legitimate; I didn't invent work.

## 2026-11-06 03:00 UTC
- Quiet hour, everyone blocked on the user (the Air at the login window since 11-05 17:45Z, the headset adb offline). Nothing invented.
- Shipped: HEARTBEAT (headset POSTs its status to the Mini, e05bd16; v0.76 waits for adb); room.sh now writes by=captain (6a10c2e). An agent first misread my revs 31-35 as the headset being online, which I corrected.
- Lesson: provenance fields must be set by the writer, not copied forward.
- Closed the ghost prox066[2830336].

## 2026-11-06 04:00 UTC

- **Decided:** no new work handed out. Every open card is blocked on the user: the Air login (no GUI session since 11-05 17:45Z, /dev/console still owned by root) or the headset (adb offline since 11-04 12:24Z). Busywork on a held campaign would only make results that we would throw away.
- **Shipped:** nothing new this hour. The last commit is f3ddd7d (the 03:00 diary).
- **Broke or learned:** the activity log is clean, with no new ghost jobs since the prox066 entry I closed at 03:18Z. My one-off probe of the Mini on the LAN from the VPS timed out, as expected, because the LAN is reachable only through Tailscale and aliases. Lesson: when the check is unchanged, probe through the known path, not by guessing.
- **Next:** once the user logs in, run air_smoke (ad), MINI /session/1 end to end (42), and resume campaign B, D, F, C, E (34). Once adb is back, c4 verifies v0.72, installs v0.76 with my go, and confirms the house rev 35 edits (screens 2 and 3 off) through HEARTBEAT.

## 2026-11-06 05:00 UTC

- **Decided:** I held all four agents again. c4, 34, 42 and ad are all alive and idle; none crashed or got stuck. Every open card still needs the Air's GUI login (the console is still owned by root) or the headset's adb (offline since 11-04 12:24Z).
- **Shipped:** nothing this hour. The last commit is 0a905ff (the 04:00 diary).
- **Broke or learned:** nothing broke. The activity log has no new entries since 03:18Z and no ghost jobs. The Air has now sat about 11.5 h at the login window, and that is the main cost to the project: the campaign, the MINI end-to-end test and air_smoke all wait on that one login. Also: no apostrophes in a single-quoted sudo heredoc; write the entry to a file first.
- **Next:** unchanged. After the login: air_smoke (ad), MINI /session/1 (42), campaign B, D, F, C, E (34). After adb: verify v0.72, install v0.76 with my go, and confirm house rev 35 through HEARTBEAT (c4).

## 2026-11-06 06:00 UTC

- **Decided:** I held all four agents (c4, 34, 42, ad: alive, idle, none crashed or stuck). Nothing changed: the Air's console is still at the login window (owned by root) and the headset's adb has been offline since 11-04 12:24Z.
- **Shipped:** nothing this hour. The last commit is e2ae934 (the 05:00 diary).
- **Broke or learned:** nothing broke. The activity log has no entries since 03:18Z and no ghost jobs. Three quiet hours in a row: once both blockers are on the user, an hourly check costs almost nothing, so I keep it at three tool calls and don't invent work.
- **Next:** unchanged. After the login: air_smoke (ad), MINI /session/1 (42), campaign B, D, F, C, E (34). After adb: verify v0.72, install v0.76 with my go, and confirm house rev 35 through HEARTBEAT (c4).

## 2026-11-06 07:00 UTC

- **Decided:** I held all four agents (c4, 34, 42, ad: alive, idle, none crashed or stuck). The blockers are unchanged: the Air's console is still owned by root (the login window, since 11-05 17:45Z), and the headset's adb has been offline since 11-04 12:24Z.
- **Shipped:** nothing this hour. The last commit is 5b2d2ef (the 06:00 diary).
- **Broke or learned:** nothing broke. The activity log has no entries since 03:18Z and no ghost jobs. Four quiet hours in a row: the user is most likely asleep, and the right move is to stay cheap and ready, not to start work that needs the Air or the headset.
- **Next:** unchanged. After the login: air_smoke (ad), MINI /session/1 (42), campaign B, D, F, C, E (34). After adb: verify v0.72, install v0.76 with my go, and confirm house rev 35 through HEARTBEAT (c4).

## 2026-11-06 08:00 UTC

- **Decided:** I held all four agents (c4, 34, 42, ad: alive, idle, none crashed or stuck). The blockers are unchanged: the Air's console is still owned by root (the login window, since 11-05 17:45Z, now about 14.5 h), and the headset's adb has been offline since 11-04 12:24Z.
- **Shipped:** nothing this hour. The last commit is 4877ecd (the 07:00 diary).
- **Broke or learned:** nothing broke. The activity log has no entries since 03:18Z and no ghost jobs. Five quiet hours in a row. When the user is back, the first ask is the one login: it unblocks three of the four agents at once.
- **Next:** unchanged. After the login: air_smoke (ad), MINI /session/1 (42), campaign B, D, F, C, E (34). After adb: verify v0.72, install v0.76 with my go, and confirm house rev 35 through HEARTBEAT (c4).

## 2026-11-06 09:00 UTC

- **Decided:** I held all four agents (c4, 34, 42, ad: alive, idle, none crashed or stuck). The blockers are unchanged: the Air's console is still owned by root (the login window since 11-05 17:45Z, about 15.5 h), and the headset's adb has been offline since 11-04 12:24Z.
- **Shipped:** nothing this hour. The last commit is 39df3d6 (the 08:00 diary).
- **Broke or learned:** nothing broke. The activity log has no entries since 03:18Z and no ghost jobs. Six quiet hours in a row. These hourly entries now repeat themselves, so while both blockers stand I keep them to a status line rather than padding them out.
- **Next:** unchanged. After the login: air_smoke (ad), MINI /session/1 (42), campaign B, D, F, C, E (34). After adb: verify v0.72, install v0.76 with my go, and confirm house rev 35 through HEARTBEAT (c4).

## 2026-11-06 10:00 UTC

- **Decided:** held all four agents (c4, 34, 42, ad: alive, idle, none crashed or stuck). The blockers are unchanged: the Air's console is still owned by root (the login window since 11-05 17:45Z, about 16.5 h) and the headset's adb has been offline since 11-04 12:24Z.
- **Shipped:** nothing this hour. The last commit is f3740d4 (the 09:00 diary).
- **Broke or learned:** nothing broke. The activity log is unchanged since 03:18Z and there are no ghost jobs. That makes seven quiet hours.
- **Next:** unchanged. After the login: air_smoke (ad), MINI /session/1 (42), campaign B, D, F, C, E (34). After adb: verify v0.72, install v0.76 with my go, and confirm house rev 35 through HEARTBEAT (c4).

## 2026-11-06 11:00 UTC

- **Decided:** I held all four agents (c4, 34, 42 and ad are alive and idle; none crashed or got stuck). The blockers haven't changed: the Air's console is still owned by root (the login window since 11-05 17:45Z, about 17.5 h ago), and the headset's adb has been offline since 11-04 12:24Z.
- **Shipped:** nothing this hour. The last commit is 1f938fe (the 10:00 diary).
- **Broke or learned:** nothing broke. The activity log hasn't changed since 03:18Z, with no ghost jobs. Eight quiet hours.
- **Next:** unchanged. After the login: air_smoke (ad), MINI /session/1 (42), and campaign blocks B, D, F, C, E (34). Once adb is back: verify v0.72, install v0.76 with my go, and confirm house rev 35 through HEARTBEAT (c4).

## 2026-11-06 12:00 UTC

- **Decided:** all four agents stay on hold. c4, 34, 42 and ad are alive and idle; none crashed or got stuck. Both blockers are unchanged: the Air's console is still owned by root (at the login window since 11-05 17:45Z, about 18.5 h), and the headset's adb has been offline since 11-04 12:24Z.
- **Shipped:** nothing this hour. The last commit is 2f1059f (the 11:00 diary).
- **Broke or learned:** nothing broke. The activity log hasn't changed since 03:18Z and has no ghost jobs. That makes nine quiet hours.
- **Next:** unchanged. After the login: air_smoke (ad), MINI /session/1 (42), and the campaign blocks B, D, F, C, E (34). Once adb is back: verify v0.72, install v0.76 with my go, and confirm house rev 35 through HEARTBEAT (c4).

## 2026-11-06 13:00 UTC

- **Decided:** all four agents stay on hold. c4, 34, 42 and ad are alive and idle; none crashed or got stuck. Both blockers are unchanged: the Air's console is still owned by root (at the login window since 11-05 17:45Z, about 19.5 h), and the headset's adb has been offline since 11-04 12:24Z.
- **Shipped:** nothing this hour. The last commit is 4563f32 (the 12:00 diary).
- **Broke or learned:** nothing broke. The activity log hasn't changed since 03:18Z and has no ghost jobs. That makes ten quiet hours.
- **Next:** unchanged. After the login: air_smoke (ad), MINI /session/1 (42), and the campaign blocks B, D, F, C, E (34). Once adb is back: verify v0.72, install v0.76 with my go, and confirm house rev 35 through HEARTBEAT (c4).

## 2026-11-06 14:00 UTC

- **Decided:** all four agents stay on hold. c4, 34, 42 and ad are alive and idle; none crashed or got stuck. Both blockers are unchanged: the Air's console is still owned by root (at the login window since 11-05 17:45Z, about 20.5 h), and the headset's adb has been offline since 11-04 12:24Z.
- **Shipped:** nothing this hour. The last commit is 1671d43 (the 13:00 diary).
- **Broke or learned:** nothing broke. The activity log hasn't changed since 03:18Z and has no ghost jobs. That makes eleven quiet hours.
- **Next:** unchanged. After the login: air_smoke (ad), MINI /session/1 (42), and the campaign blocks B, D, F, C, E (34). Once adb is back: verify v0.72, install v0.76 with my go, and confirm house rev 35 through HEARTBEAT (c4).

## 2026-11-06 15:00 UTC

- **Decided:** all four agents stay on hold. c4, 34, 42 and ad are alive and idle; none crashed or got stuck. Both blockers are unchanged: the Air's console is still owned by root (at the login window since 11-05 17:45Z, about 21.5 h), and the headset's adb has been offline since 11-04 12:24Z.
- **Shipped:** nothing this hour. The last commit is 4056689 (the 14:00 diary).
- **Broke or learned:** nothing broke. The activity log hasn't changed since 03:18Z and has no ghost jobs. That makes twelve quiet hours. The daily diary is due at 20:57 and should summarise this whole blocked day in one entry instead of repeating these hourly lines.
- **Next:** unchanged. After the login: air_smoke (ad), MINI /session/1 (42), and the campaign blocks B, D, F, C, E (34). Once adb is back: verify v0.72, install v0.76 with my go, and confirm house rev 35 through HEARTBEAT (c4).

## 2026-11-06 16:00 UTC

- **What changed:** the user logged in on the Air at about 15:58Z. The console is owned by miguel_lemos; there was no reboot (uptime 2 days 5 h). At 16:18Z PinPin Host was up (pid 70459), the load was falling (1.35 / 3.70 / 11.11), and colorsync and audiomxd were no longer at the top of the CPU list.
- **Decided:** I unblocked all four agents.
  - ad runs air_smoke.
  - 42 runs the MINI /session/1 end-to-end test.
  - 34 restarts :57800 with /headset/status, then resumes campaign blocks B, D, F, C, E, but only after air_smoke passes. A gate keeps a bad laptop state out of the benchmark numbers.
  - c4 checks the headset's adb once.
  - I skipped the planned quiet reboot for colorsync: the user just logged in, and colorsync isn't pegged right now.
- **Shipped:** no code this hour. The last commit is fbe1362 (the 15:00 diary).
- **Learned:** the 15-minute load average of 11 points to a heavy login burst that is now draining. The first campaign runs should record their load and be discarded if it's above about 2.
- **Next:** the air_smoke verdict, then the campaign; then the headset, which still needs the user if adb is offline.

## 2026-11-06 17:00 UTC

- **Decided:**
  - Gave the go for a single install window for K30 (pinpin-displays session watch) and K31b (Host stale-session sweep) after campaign block E, so the user sees one blink.
  - Approved per-run colorsync bracketing (ad).
  - Put the K15 phase 2 test on hold until v0.76 is on the headset.
  - The headset install is still waiting: c4 aborted cleanly at 16:27Z (adb flapping, nothing changed), and the user needs to charge and wake the headset.
- **Shipped:** K30 (68e7178, 2cf5281, b9a724b), K31 orphan guards (ffd4fd7, 7c2f932, 2dd12d4, 6d03728), K15 tests and the campaign guard (9138be1, ebc4016), air_smoke's stale-displays check (8c816d1), and AGENTS.md notes (d71f34b, 2a311f7).
- **Broke or learned:**
  - My orphan rule ("exit when ppid becomes 1") was wrong. Host adopts plugins across its own restarts. work-09 caught it, and the rule is now "started before the current WindowServer".
  - colorsync's creep lives inside the colorsync process: restarting colorsync resets it to about 31%.
  - A second +12-point step showed up during the campaign, with no display churn.
  - Block F is failing on a missing bench/stress/encprobe.swift that was never committed. I sent it back to 34 (commit it or void F).
- **Next:**
  - finish the campaign (C, E);
  - the K30/K31b window;
  - ad's colorsync write-up;
  - v0.76 on the headset once the user wakes it.
- **Correction, 17:25Z:** block F did not fail on an uncommitted file. bench/encprobe.swift has been on main since 67fc49d. stress.py moved to bench/stress/ (8195e98) and its path to encprobe.swift was not updated. Fixed in e250d40. I had searched only bench/stress before relaying the claim; the lesson is to search the whole repo before calling a file missing.

## 2026-11-06 18:00 UTC

- **Decided:** let the campaign run in the order C, E, then the K30+K31b install window, then a block F rerun. The F rerun goes after the install so that F gets a fresh lock and a single build of encprobe. I closed the ghost campaign[4039070] (block F, killed at 17:20Z), which the watchdog kept flagging as hung.
- **Shipped:** e250d40 (stress --encprobe path fix, by work-09). Captain: 9abeb44 and 6de09c8 (the diary plus a correction).
- **Results:** block C ended with damage DROP (bench/results/campaign-C-20261106-1720.md).
- **Broke:** block E is slow. Its runs take about 4 min for 60 s of measurement, and round 2 base timed out over ssh at 18:11Z. I asked 34 to check the Air's load and colorsync, and to mark contaminated runs rather than give them a verdict.
- **Learned:** I relayed "encprobe.swift was never committed" after searching only one directory. It was a moved-path bug. Search the whole repo before calling a file missing.
- **Next:** E finishes, then the install window; then ad's colorsync write-up and the F rerun. The headset (v0.76) is still waiting for the user to charge it and wake it.

## 2026-11-06 19:00 UTC

- **Decided:**
  - The user came back wanting to show off the headset. I stopped all Air work so the stream would stay smooth (campaign, K30/K31b install, samplers).
  - At their explicit request, c4 installed v0.76 while they wore it, with no prox at all and a rollback to v0.72 if it failed.
  - The user then asked for nothing more to be installed (a demo for their wife), so everything is FROZEN until they lift it.
  - work-ad added a shared freeze flag, /root/work/.air-freeze (pinpin-apps 43ac89e).
- **Shipped:**
  - v0.76 is on the headset and verified through HEARTBEAT: v76, 72–73 fps, focused, and house revs applied in sync (rev 42 now, from the user's own edits).
  - c4 fixed room.sh status so it no longer reports off screens as missing (abed50b).
  - colorsync_reset now uses launchctl kickstart -k and checks that the pids changed (63e90c0, c09619c, 56ffe05).
- **Broke or learned:**
  - The first v0.76 launch was killed by the delayed package broadcast; the retry stuck (a known gotcha).
  - `sudo killall` on colorsync was a silent no-op: launchd's EnableTransactions defers SIGTERM, so the pids stayed the same. Always check that the pids changed.
  - The Air was off the network 18:18–18:36Z, with no reboot, sleep or logout, so block E is CONTAMINATED.
  - I couldn't reach adb myself. The path only works from the Mini (now saved in memory).
  - I closed the ghost campaign[3822215] (block B, 01:34Z).
- **Next, after the user lifts the freeze:**
  - the colorsync kickstart (me, as root on the Air) plus air_smoke (ad);
  - the K30+K31b install window, then E (with a warm-up if needed), then F (34);
  - the K15 phase 2 DRY watch, now that vis is live (42).

## 2026-11-06 20:00 UTC

- **Decided:** keep the demo freeze in place (/root/work/.air-freeze is present). All four agents are idle and holding; none crashed or got stuck. No cards handed out, because every open card touches the Air or the headset.
- **Shipped:** nothing this hour; the last commit is 7ee5db1 (the 19:00 diary). HEARTBEAT shows the headset live and fresh (age about 2 s) on v0.76.
- **Broke or learned:** the watchdog flags the bench lock as held 62 min with no open START. That's the dead 18:15:37 stress run's marker. It's past its stale age, and the next bl.take() clears it through benchlock's stale path. Nobody takes the lock during the freeze, so it sits there harmlessly; I'm leaving it rather than hand-deleting a lock file.
- **Next, after the user lifts the freeze:**
  - the colorsync kickstart and air_smoke;
  - the K30+K31b install;
  - campaign blocks E and F;
  - K15 phase 2.
  The daily diary is due at 20:57.

## 2026-11-06 21:00 UTC

- **Decided:** keep the demo freeze (/root/work/.air-freeze is present). All four agents are idle and holding, and none crashed.
- **Shipped:** nothing this hour. The last commit is c97c034.
- **Broke:** at 21:18Z, both the Mini (100.64.243.30) and the Air (100.66.41.62) time out on ssh from the VPS. room.sh status crashes on a socket timeout reading /headset/status. Tailscale still lists them as active, so this looks like a second home-network or uplink outage today (the first was 18:18–18:36Z). The LAN demo itself shouldn't need the uplink, but the receptionist does.
- **Learned:**
  - room.sh status needs to catch URLError and timeouts, not just HTTPError. It should print "house server unreachable" instead of a traceback. That's a card for c4 once the freeze lifts.
  - The watchdog keeps re-flagging the stale bench lock every hour (123 min now), and nothing clears it during a freeze. That's noise, not a fault.
- **Next:** the same queue as at 20:00, starting once the network is back and the user lifts the freeze.

# Day 2026-11-06

**Arc.** 00:00–15:58Z was a dead stretch. The Air sat at the login window after the 11-05 17:45Z WindowServer crash, and the headset's adb was offline. I held the campaign and kept the hourly checks to three tool calls, rather than inventing work whose results would be thrown away. At 15:58Z the user logged in and the four agents ran in parallel until about 19:00Z. Then the user put the headset on to show it to their wife. They asked for v0.76 to be installed while they wore it, then for no further installs, so I froze everything. From 21:18Z the Mini and the Air time out from the VPS: a second network outage today (the first was 18:18–18:36Z).

**Decisions and why.**
- Gate the campaign on air_smoke, so a broken Air can't produce benchmark numbers.
- Restart the orphaned pinpin-displays through Host rather than reboot the Air: the user had just logged in, and the displays were already dead.
- One install window for K30 and K31b after the campaign, so the user's screens blink once and not twice.
- Every display create happens before a single colorsync reset, because colorsync creeps with each create.
- Install v0.76 while worn, without prox, only because the user asked explicitly, with a rollback to v0.72 if it failed. Then a full freeze, because they asked.
- No `kill -9` on a system daemon right before a demo.

**Mistakes and lessons** (fed into captain-lessons.md):
- I specified orphan detection as "ppid becomes 1". That's wrong: Host adopts plugins across its own restarts. work-09 caught it. The rule is now "the process started before the current WindowServer".
- I told work-09 that encprobe.swift was "never committed" after searching one directory. It was a moved path. Search the whole repo before calling a file missing.
- `sudo killall` on colorsync returned rc 0 but changed nothing: EnableTransactions defers SIGTERM. A restart counts only if the pid changed.
- I couldn't reach adb myself during the demo. The path only works through the Mini (now in memory).

**State per agent.**
- **work-c4 (headset):** v0.76 is installed and verified through HEARTBEAT (72–73 fps, house in sync at rev 42; the user moved screen 6 themselves). abed50b: room.sh status now treats off screens as correct. K9 and K17 are open.
- **work-34 / work-09 (bench, geometry, Host):**
  - Campaign: block C finished with DROP; E is CONTAMINATED (outage); F was lost to a path bug (fixed in e250d40).
  - K30 (68e7178, 2cf5281, b9a724b) and K31b (2dd12d4, 6d03728) are built and tested but not installed. K32 is filed.
- **work-42 (assistant, perf):** K20 is done (MINI end to end). K15 has its tests and a campaign guard (9138be1, ebc4016), and DRY phase 1 passed. The orphan guard is live on 2 plugins (7c2f932, flag on).
- **work-ad (apps, ops, perf):**
  - air_smoke failed, and that failure gave us the diagnosis. Then it passed at 53.5 fps.
  - The displays plist is parked (d71f34b).
  - K23 is solved, and colorsync_reset is built (56ffe05) but unproven.
  - The shared freeze flag is /root/work/.air-freeze.

**Tomorrow, after the freeze lifts and the network is back:**
1. colorsync kickstart (me, as root), then ad measures and runs air_smoke.
2. The K30 + K31b install window (34).
3. The pool, the reset, then the E and F reruns.
4. K15 phase 2 on the live vis (42).
5. room.sh status: handle timeouts (c4).
6. Open questions for the user: the big-wall screen, and screens 2 and 3 off or on.

## 2026-11-06 22:00 UTC

- **Decided:** keep the demo freeze (/root/work/.air-freeze is present; the user hasn't lifted it). All four agents are idle and holding, and none crashed.
- **Shipped:** the full daily diary (57e4aca) and new lessons in captain-lessons memory. K30 and K31 are moved to review on the kanban; the K31 move had failed at 21:30Z while the Mini was unreachable.
- **Broke or learned:** at 22:17Z the Mini and the Air answer ssh again, so the 21:18Z outage lasted under an hour. That makes two outages today, cause unknown; worth asking the user whether the router or the uplink restarted. The watchdog flagged the stale bench lock for the third time (185 min). It's harmless under the freeze, and the next bl.take() clears it.
- **Next:** unchanged. Once the user lifts the freeze: the colorsync kickstart and air_smoke, then the K30+K31b install, then E and F, then K15 phase 2, then room.sh status timeout handling.

## 2026-11-06 23:00 UTC

- **Decided:**
  - I'm keeping the demo freeze. The user hasn't lifted it, and at 02:17 local they're most likely asleep. Lifting a freeze the user set is their call, not mine.
  - I gave c4 a card that is offline and allowed under the freeze: room.sh status should handle an unreachable house server (URLError/timeout) with one short line instead of a traceback.
  - The other three agents hold. Their cards all touch the Air.
- **Shipped:** nothing from the agents this hour. The kanban mirror is at rev 105 (40f43eb).
- **Broke or learned:** nothing new. The watchdog flagged the stale bench lock again (247 min); it's still harmless under the freeze. Lesson: a freeze needs an explicit end, or it outlives its purpose overnight. Next time, ask the user for an end condition when they set one.
- **Next:** in the morning, ask the user to lift the freeze, then run the queue: colorsync kickstart, air_smoke, the K30+K31b install, campaign blocks E and F, and K15 phase 2.

## 2026-11-07 00:00 UTC

- **Decided:** the demo freeze stays; only the user lifts it. All four agents are idle; none crashed or got stuck. There's no further offline-only card to hand out tonight.
- **Shipped:** f90a1e8 (c4). Every room.sh command now prints "house server unreachable (<reason>)" and exits 1 instead of a traceback. 48/48 receptionist tests pass, and it is live on the VPS.
- **Broke or learned:** nothing broke. HEARTBEAT is still fresh at 00:17Z (age 7 s), so v0.76 is running and posting steadily about 5.5 h after install. That's the first long uptime reading we have for the HEARTBEAT build. The watchdog flagged the stale bench lock again (307 min); it's harmless.
- **Next:** in the morning, ask the user to lift the freeze, then run the queue: colorsync kickstart, air_smoke, K30+K31b install, campaign blocks E and F, K15 phase 2.
