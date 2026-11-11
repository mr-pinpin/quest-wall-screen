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

## 2026-11-07 01:00 UTC

- **Decided:** the freeze stays. work-42's nightly-eval cron started at 01:17:07Z on the Air despite /root/work/.air-freeze. I let the run finish, because killing it mid-run could leave screens in an odd state. I asked 42 to confirm the screens are untouched and to make the cron skip under the freeze flag, as work-ad's collector already does.
- **Shipped:** no new code. The last commit is c1bf4ab.
- **Broke or learned:**
  - A freeze flag is only as good as its coverage. I announced it to the agents, but scheduled jobs (crons) don't read messages. Every recurring job that touches the Air has to check the file.
  - HEARTBEAT is still fresh (age 3 s, about 6.5 h after install).
  - The stale bench lock was flagged again (367 min).
- **Next:** the nightly-eval freeze check (42), then the morning queue once the user lifts the freeze.

## 2026-11-07 02:00 UTC

- **Decided:** the freeze stays. I gave work-ad a read-only ops card on why the Mini's Tailscale path from the VPS fails. At 02:18Z the house server was unreachable from the VPS, but from the Air over the LAN the Mini answered fine (8 ms ping, ports 57800 and 22 open). On the VPS, Tailscale shows the Mini on `relay "hel"`, while the Air connects directly. My guess is that today's 18:18Z and 21:18Z "outages" were this relay path too, not the home network; to be checked.
- **Shipped:** 008a0ae (nightly-eval skips while the freeze flag exists) and b374cde (eval_run.sh takes the lock through benchlock, so a dead holder is taken over instead of blocking forever). Tonight's nightly run had LOCK FAILed on the stale lock and changed nothing.
- **Learned:** "unreachable from the VPS" is not the same as "down". Probe from a LAN peer before calling an outage. I had told the user their internet might be down; that may have been wrong.
- **Next:** ad's Tailscale diagnosis; the morning queue once the freeze lifts. Clearing the stale bench lock is on my list.
- **Correction, 02:25Z (work-ad):** the `relay "hel"` row belongs to another Air (macbook-air-anastasiia, offline). The Mini connects directly. It uses the VPS (in Brazil, about 250 ms away) as its Tailscale exit node, so all its internet traffic goes through Brazil. The VPS logs show stalls where the link is up but new connections time out: the Air at 18:10–18:40Z, the Mini at 21:00–22:20Z and 02:17Z. Suspect: the home uplink saturating behind the exit-node path. The fixes (Mini off the exit node; UPnP on the RT router) are the user's call. I misread a status row; the lesson is to match the hostname, not just the line.

## 2026-11-07 03:00 UTC

- **Decided:** the freeze stays. All four agents are idle and holding, and none crashed. I set a boundary for work-ad: an agent read the Mini's browser-history DBs (host counts only) to answer a network question. Personal data is off-limits without the user's OK. ad saved that as a standing rule, and I added it to captain-lessons.
- **Shipped:** pinpin-apps 3eb0b5c (ad): /status has a per-peer counter of "VPS → peer stalls" (link online, yet TCP connections time out). The first reading was 176 for the Mini and 66 for the Air in 24 h. The Anastasiia-Air entries are inbound attempts from that machine to the VPS, not ours; I left them.
- **Broke or learned:** HEARTBEAT went stale at 03:17Z (age 63 s, after more than 8 h fresh), most likely because the headset went to sleep or was taken off. That's not a fault. The stale bench lock was flagged again (488 min).
- **Next:** the morning questions for the user:
  1. lift the freeze;
  2. take the Mini off the exit node (only if nothing in its browsers needs Facebook or WhatsApp);
  3. UPnP on the RT router;
  4. the big-wall screen;
  5. screens 2 and 3.
  Then the queue: colorsync kickstart, air_smoke, K30+K31b, E and F, K15 phase 2.

## 2026-11-07 04:00 UTC

- **Decided:** the freeze stays. All four agents are idle and holding, none crashed, and there were no new cards. Every remaining card touches the Air or the headset, or needs a decision from the user.
- **Shipped:** nothing. The last commit is 07f02ff.
- **Broke or learned:** HEARTBEAT age is 186 s, not about 3700 s. So the headset posted 3 min ago and is waking intermittently, not fully asleep. It's worth checking once the freeze lifts: the headset may be dozing and waking, rather than a posting gap in v0.76. The stale bench lock was flagged again (549 min).
- **Next:** the morning questions and the queue are as in the 03:00 entry.

## 2026-11-07 05:00 UTC

- **Decided:** the freeze stays. All four agents are idle and holding; none crashed, and there were no new cards.
- **Shipped:** nothing. The last commit is 60f7a71.
- **Learned:** I re-read HEARTBEAT. The last post was received at 05:14:46Z, and age_s has stayed at about 186–193 s for two hours. So the headset still posts while idle, every 3 min or so, which looks like a slower idle cadence or periodic wakes. It hasn't stopped. The 04:00 worry was overstated; c4 can confirm the idle cadence from the v0.76 code. The stale bench lock was flagged again (610 min).
- **Next:** the morning questions and the queue are the same as in the 03:00 entry.

## 2026-11-07 06:00 UTC

- **Decided:** the freeze stays. All four agents are idle and holding, none crashed, and there are no new cards.
- **Shipped:** nothing. The last commit is 039d81d.
- **Broke or learned:** nothing broke. HEARTBEAT was last received at 06:16:47Z, so the headset is still posting during idle about 11.5 h after install. The stale bench lock was flagged again (672 min). It's the first item on the post-freeze list.
- **Next:** the morning questions and the queue are as in the 03:00 entry.

## 2026-11-07 07:00 UTC

- **Decided:** keep the demo freeze (/root/work/.air-freeze is still present). All four agents are idle and holding, and none crashed. No cards handed out, because every open card touches the Air or the headset.
- **Shipped:** nothing this hour. The last commit is 1c25747 (the 06:00 diary).
- **Observed:** the headset is on the charger at 98%, with the app backgrounded (fps 0, session state 1). The house is at rev 43, applied 43, pending 0, so the user made one more edit since rev 42. No ghost jobs. The watchdog flagged the stale 18:15:37 bench lock again (732 min); that is noise until the first post-freeze take clears it.
- **Learned:** bench/headset-queue.txt shows uncommitted changes in the shared checkout. It is a runtime file, so I am leaving it alone.
- **Next:** unchanged, waiting on the user. Lift the freeze, then the colorsync kickstart and air_smoke, the K30/K31b install, blocks E and F, and K15 phase 2.

## 2026-11-07 08:00 UTC

- **Decided:** keep the demo freeze (/root/work/.air-freeze is present). All four agents are idle and holding, and none crashed. No cards handed out: every open card touches the Air or the headset.
- **Shipped:** nothing this hour. The last commit is 1a3014c (the 07:00 diary).
- **Observed:** HEARTBEAT is fresh (ageS 4). The headset is on the charger, holding at 98% for over an hour, likely the charge cap. The house is at rev 43, all applied. App uptime is about 66 min, so the app process started around 07:11Z. That is probably the system restarting a backgrounded app while it charges; it is not an install, and nobody touched it. No ghost jobs. The watchdog flagged the stale bench lock again (793 min).
- **Learned:** when using HEARTBEAT as a liveness signal, read ageS rather than trusting values that didn't change.
- **Next:** unchanged, waiting on the user. Lift the freeze, then the colorsync kickstart and air_smoke, the K30/K31b install, blocks E and F, and K15 phase 2.

## 2026-11-07 09:00 UTC

- **Decided:** keep the demo freeze (/root/work/.air-freeze is present). All four agents are idle and holding, and none crashed. No cards handed out, and no remote action.
- **Shipped:** nothing from agents this hour. On origin, after 699d919 (the 08:00 diary), there is the Mini's automatic house mirror (3b74894, rev 43).
- **Broke:** around 09:09Z the Mini (100.64.243.30) and the Air (100.66.41.62) both went offline from the VPS. Tailscale shows them offline, last seen 8 min before 09:18Z, with rx 0. ssh times out, tailscale ping gets no reply, and :57800/headset/status times out. The phone-side macbook-air-anastasiia is offline too (last seen 28m). Three home devices dropping together points at the home uplink or power, not at our services. This is the third drop in about 15 h (after 18:18Z and 21:18Z).
- **Learned:** without a probe that runs from inside the house I can't tell a power cut from a broken uplink. A LAN-side marker (Mini uptime and last-online, logged locally) would settle it after the fact. That is a card for ad once the freeze lifts.
- **Next:** check once more at 10:00. When the house is back, read the Mini's uptime to rule a power cut in or out. The queue after the freeze is unchanged.

## 2026-11-07 10:00 UTC

- **Decided:** keep the freeze. All four agents are idle and none crashed. No cards handed out; with the house unreachable there is nothing they could act on.
- **Shipped:** nothing. The last commit is 83977e2 (the 09:00 diary).
- **Broke:** the Mini and the Air have now been offline for about 1 h. Tailscale has them last seen at about 09:09Z with rx 0, and :57800 times out.
  - macbook-air-anastasiia was seen 1 min ago with real traffic (rx 2.9 MB). So some device with a route to the internet is up, while both of ours stay dark.
  - That weakens the uplink-only theory from 09:00. It fits a power cut that left the Mini/Air waiting at a FileVault or login screen, or a dead RT router while that MacBook used another network.
  - The watchdog's last line is 08:29Z, so its hourly check presumably depends on reaching the house too.
- **Correction:** at 09:00 I called macbook-air-anastasiia "phone-side". It is a MacBook.
- **Learned:** one other peer coming back is the cheapest discriminator between an uplink failure and a host failure. It points at host or LAN power, which only the user can see.
- **Next:** ask the user to look at the Mini and Air (powered? at the login screen?) and at the RT router. When they're back, read the uptime to settle whether it was a power cut. The queue after the freeze is unchanged.

## 2026-11-07 11:00 UTC

- **Decided:** keep the freeze, and keep agents idle. With the house unreachable, any card would be blind work. No remote action is possible.
- **Shipped:** nothing. The last commit is d76e2f4 (the 10:00 diary).
- **Broke:** the Mini and the Air are still offline, about 2 h now (last seen around 09:09Z, rx 0). macbook-air-anastasiia was seen about 10:40Z and has been offline since. The watchdog has been silent since 08:29Z.
- **Learned:** nothing new can be learned from the VPS side. The user was asked at 10:18Z to check the power, the login screens and the RT router.
- **Next:** wait for the user. When the house is back: check the Mini and Air uptimes (power cut or not), confirm that Host and the receptionist restarted, and confirm HEARTBEAT. Then the queue after the freeze.

## 2026-11-07 12:00 UTC

- **Decided:** keep the freeze and keep the agents idle. The house has been unreachable for about 3 h. I'm not repeating the question to the user beyond this one-line reminder.
- **Shipped:** nothing. The last commit is 77b3735 (the 11:00 diary).
- **Broke:** the Mini and the Air are still offline (last seen around 09:09Z, rx 0). macbook-air-anastasiia was last seen about 1 h ago. The watchdog has been silent since 08:29Z. There is no headset status.
- **Learned:** nothing new from the VPS side. This is the longest of the three drops (18:18Z, 21:18Z, 09:09Z), and the only one that hasn't recovered by itself. That suggests a different cause, such as power or a machine at a login screen, rather than the short TCP stalls.
- **Next:** wait for the user. On return: check the uptimes, the Host and receptionist pids, and HEARTBEAT. Then the queue after the freeze.

## 2026-11-07 13:00 UTC

- **Decided:** keep the freeze and keep the agents idle. The house has been unreachable for about 4 h. A new session, work-ff, appeared about 41 min ago and is idle. I did not start it, and I'm leaving it alone, because it may be the user's own.
- **Shipped:** nothing. The last commit is 1b7b1ee (the 12:00 diary).
- **Broke:** the Mini and the Air are still offline (last seen around 09:09Z, rx 0). macbook-air-anastasiia is active again, seen 1 min ago with heavy traffic (tx 224 MB) via relay hel. So that machine has internet, while both of ours stay dark for 4 h. That points further toward the Mini and the Air themselves being down (power, a login screen after a reboot, or sleep), not the uplink.
- **Learned:** that MacBook's activity is now a good probe of the house uplink. When it's online and ours aren't, the fault is local to our machines or their LAN segment.
- **Next:** wait for the user to look at the Mini and the Air. On return: check the uptimes, the Host and receptionist pids, and HEARTBEAT, then the queue after the freeze.

## 2026-11-07 14:00 UTC

- **Decided:** keep the freeze and keep the agents idle. Nothing in the house is reachable, so there is no useful card to hand out. work-ff is still idle and untouched.
- **Shipped:** nothing. The last commit is e740ef6 (the 13:00 diary).
- **Broke:** the Mini and the Air have been offline for about 5 h (last seen around 09:09Z, rx 0). macbook-air-anastasiia is online right now (relay hel, traffic both ways). No headset status, and no watchdog lines since 08:29Z.
- **Learned:** nothing new. The pattern still points at our two machines (power, sleep, or a login screen after a reboot), not at the uplink. I've asked the user three times, so from here I'll only say something when the state changes.
- **Next:** wait for the user. On return: check the uptimes, the Host and receptionist pids, and HEARTBEAT, then the queue after the freeze.

## 2026-11-07 15:00 UTC

- **Decided:** no change. The freeze holds, the agents stay idle, and work-ff is untouched. I'm not sending the user another nudge, because nothing has changed since 14:00.
- **Shipped:** nothing. The last commit is 3ba66d9 (the 14:00 diary).
- **Broke:** the Mini and the Air are still offline, about 6 h now (last seen around 09:09Z, rx 0). macbook-air-anastasiia was last seen 7 min ago. No headset status, and no watchdog lines since 08:29Z.
- **Learned:** nothing new from here.
- **Next:** wait for the user. On return: check the uptimes, the Host and receptionist pids, and HEARTBEAT, then the queue after the freeze. The daily diary is due around 20:57Z.

## 2026-11-07 16:00 UTC

- **Decided:** I looked at the Air (read only) and changed nothing. The freeze holds, the agents stay idle, and work-ff is untouched.
- **Observed:**
  - The Air came back. It booted at 15:56Z (kern.boottime), with WindowServer at 15:56:11Z. Host pid 873 is up after the reboot.
  - The 7 h gap ends in a cold boot, which fits a power cut or a forced restart rather than an uplink drop.
  - The Air is now on 192.168.1.35 with gateway 192.168.1.1, not the RT LAN 192.168.0.x where it normally lives. It cannot ping the Mini at 192.168.0.50.
  - The Mini is still offline in Tailscale (7 h).
  - The watchdog came back at 16:01Z, reporting the stale lock at 1306 min.
  - The Air's load is about 15: mds at 94%, plus colorsync.displayservices at 62% and colorsyncd at 38% right after the displays were re-created.
- **Learned:** this all points at the RT router or its LAN being down. The Air fell back to the 192.168.1.x network (likely the ZyXEL's), and the Mini, on the RT LAN, has no path out. colorsync starts warm after a boot (100% between the two daemons), which fits the creep model: display creates into a running colorsync.
- **Next:**
  - The user checks the RT router and the Mini; I don't touch the routers.
  - Once the Mini is back: check its uptime and confirm HEARTBEAT.
  - When the freeze lifts: the colorsync kickstart first (it is already warm), then the usual queue.

## 2026-11-07 17:00 UTC

- **Decided:** no action. The freeze holds and the agents stay idle. work-34's card (K30/K31b) and ad's (colorsync, air_smoke) both need the freeze lifted and the Mini back. work-ff is untouched.
- **Shipped:** nothing. The last commit is a6c13e6 (the 16:00 diary).
- **Observed:**
  - The Air is up 1 h 22 m, still on 192.168.1.35 with the 192.168.1.1 gateway. Its load is down to 2.3 now that mds has settled.
  - colorsync is holding at about 100% between the two daemons (displayservices 63%, colorsyncd 38%), unchanged from 16:00. So it is not settling by itself after the boot.
  - The Mini has been offline 8 h, and HEARTBEAT is absent, since it posts to the Mini.
  - The watchdog reported the stale lock at 1366 min.
- **Learned:** colorsync stays pegged at a steady 100% after a fresh boot. That supports kickstarting it first when the freeze lifts, rather than hoping it decays.
- **Next:** the user checks the RT router and the Mini (asked at 16:18). Once the Mini is back: check its uptime and HEARTBEAT, move the Air back to the RT network, then the colorsync kickstart and the queue.

## 2026-11-07 18:00 UTC

- **Decided:** no action. The freeze holds, the agents stay idle, and work-ff is untouched. Nothing has changed that the user needs to hear beyond the 16:18 request.
- **Shipped:** nothing. The last commit is 1fe3225 (the 17:00 diary).
- **Observed:**
  - The Air is up 2 h 22 m, still on 192.168.1.35, with load about 3.
  - colorsync is a steady ~100% (61% + 38%).
  - ANECompilerService has sat at about 96% for over an hour. That is presumably macOS compiling Neural Engine models after the boot. It's not ours, and I'm leaving it alone.
  - The Mini has been offline 9 h. The watchdog reported the stale lock at 1427 min.
- **Learned:** after this boot the Air carries about 2 cores of system load that isn't ours (ANE compile plus colorsync). Any benchmark run before the colorsync kickstart and an idle check would be contaminated. air_smoke must gate on both.
- **Next:** wait for the user on the RT router and the Mini. The daily diary is due around 20:57Z.

## 2026-11-07 19:00 UTC

- **Decided:** no action. The freeze holds, the agents stay idle, and work-ff is untouched.
- **Shipped:** nothing. The last commit is ca71fba (the 18:00 diary).
- **Observed:**
  - The Air is up 3 h 22 m on 192.168.1.35, with load about 3.
  - colorsync is a steady ~100%.
  - ANECompilerService has been at about 96% for more than 2 h. That is long for a one-off compile after a boot, so it may be stuck in a loop. It's an Apple process, not ours.
  - The Mini has been offline 10 h. The watchdog reported the stale lock at 1487 min.
- **Learned:** if ANECompilerService is still pegged once the freeze lifts, add it to the air_smoke idle gate next to colorsync. A stray core of system load would skew every benchmark.
- **Next:** wait for the user on the RT router and the Mini. The daily diary is due around 20:57Z.

## 2026-11-07 20:00 UTC

- **Decided:** no action. The freeze holds, the agents stay idle, and work-ff is untouched.
- **Shipped:** nothing. The last commit is d70f3dd (the 19:00 diary).
- **Observed:**
  - My read-only ssh to the Air timed out after 15 s this hour.
  - Tailscale still lists the Air as active and direct, and its byte counters are climbing (tx is up about 135 MB in the hour). So this looks like one of the known VPS-to-peer TCP stalls with the link up, not the Air going down. I did not retry in a loop.
  - The Mini has been offline 11 h. The watchdog reported the stale lock at 1547 min.
- **Learned:** stalls hit the Air even on the 192.168.1.x network, without the Mini's exit node in the path. That weakens the theory that the Mini's exit node saturated the uplink, so the stall cause is still open.
- **Next:** the daily diary at about 20:57Z. Waiting for the user on the RT router and the Mini.

## 2026-11-07 21:00 UTC

- **Decided:** no action. The freeze holds, the agents stay idle, and work-ff is untouched. I confirmed both crons are alive: hourly, plus the daily one, which actually fires at about 21:27Z because of the scheduler offset, not 20:57.
- **Shipped:** nothing. The last commit is 648c20a (the 20:00 diary).
- **Observed:**
  - ssh to the Air works again (the 20:00 timeout was a transient stall). It is up 5 h 22 m on 192.168.1.35, with load about 3.2.
  - ANECompilerService has been pegged at 94% for about 4 h, and colorsync is a steady ~99%.
  - The Mini has been offline 12 h. The watchdog reported the stale lock at 1608 min.
- **Learned:** ANECompilerService running 4 h is not a normal compile after a boot. When the freeze lifts, ad should look at it (what is asking the ANE to compile?) before any benchmark.
- **Next:** the daily diary at about 21:27Z. Waiting for the user on the RT router and the Mini.

# Day 2026-11-07

**Arc.** The demo freeze from 11-06 held all day, and the user never lifted it. Overnight was quiet: v0.76 posted HEARTBEAT steadily for more than 8 h after install, the first long run of that build. At about 09:09Z the Mini and the Air both dropped off Tailscale. The Air came back at 15:56Z after a cold boot, but on 192.168.1.35 (gateway 192.168.1.1) instead of the RT LAN 192.168.0.x. The Mini has been dark for 12 h and counting. So today was a holding day; the only code was freeze hygiene.

**Shipped.**
- f90a1e8 (c4, just before midnight): room.sh prints "house server unreachable" instead of a traceback.
- 008a0ae (nightly-eval skips under the freeze flag) and b374cde (eval_run.sh takes the bench lock through benchlock).
- pinpin-apps 3eb0b5c (ad): /status counts VPS → peer stalls (176 for the Mini and 66 for the Air in 24 h).
- 3b74894 (the Mini's mirror of house rev 43, the user's own edit).

**Decisions and why.**
- Let the 01:17Z nightly-eval run finish instead of killing it mid-run. Then make the crons respect the freeze file. Killing it could have left the screens half-changed.
- Read only on the Air after it rebooted: no colorsync kickstart and no network change. The freeze covers the machine, not just installs.
- Ask the user about the outage once, plainly, then stay quiet until something changes. Repeating an unchanged state is noise.

**Mistakes and lessons** (fed into captain-lessons.md):
- At 02:00 I blamed a relay path on a misread Tailscale row; it belonged to another machine. Match the hostname, not the line. Unreachable from the VPS doesn't mean down.
- The freeze flag first missed a cron. A freeze only covers what checks the file, so every scheduled job touching the Air must read it.
- I wrote "phone-side" for a MacBook (corrected at 10:00).
- Treating a single surviving peer as an uplink probe worked. macbook-air-anastasiia online while ours were dark ruled out the uplink, and the Air's 192.168.1.x address after the reboot then pointed at the RT router or its LAN.

**State per agent.** Everyone was idle all day, held by the freeze. No crashes.
- **work-c4:** v0.76 is fine; K9 and K17 are open.
- **work-34:** K30 and K31b are built but not installed; E and F are to rerun. K32 is in the backlog.
- **work-42:** K15 phase 2 DRY watch is pending.
- **work-ad:** the stall counter is live. Next for ad: why ANECompilerService has been pegged at about 95% for more than 5 h since the Air's boot.
- **work-ff** appeared at about 12:36Z. I did not start it, and I left it alone.

**Air after the boot:** colorsync is a steady ~100%, and ANECompilerService about 95%. That is about 2 cores of foreign load, so no benchmark until both are cleared.

**Tomorrow.**
1. The user looks at the RT router and the Mini. When it's back: the Mini's uptime, HEARTBEAT, and the Air back on the RT network.
2. Lift the freeze, then the colorsync kickstart and the ANE investigation, then air_smoke.
3. The K30 + K31b install, the pool, then the E and F reruns.
4. K15 phase 2. Clear the stale bench lock.
5. Open questions: the Mini off the exit node, UPnP on the RT router, the big-wall screen, screens 2 and 3.

## 2026-11-07 22:00 UTC

- **Decided:** no action. The freeze holds, the agents stay idle, and work-ff is untouched.
- **Shipped:** 198a77f (the Day 2026-11-07 entry). captain-lessons memory updated: crons must read the freeze flag; unreachable is not down; surviving-peer probe; ask once.
- **Observed:** the Air went offline again around 21:30Z (Tailscale last seen 47 min before 22:18Z, ssh times out), after about 5.5 h up on 192.168.1.x. The watchdog skipped its 22:0x line, which fits. The Mini has been offline 13 h. Nothing is reachable at home now.
- **Learned:** nothing distinguishing. A sleep, a lid close or another power event would all look the same from here. The Air's uptime on return will tell a reboot from a sleep.
- **Next:** wait for the user. On return: the uptimes of both machines, the RT router state, HEARTBEAT, then lift the freeze and run the queue.

## 2026-11-07 23:00 UTC

- **Decided:** no action. The freeze holds, the agents stay idle, and work-ff is untouched.
- **Shipped:** nothing. The last commit is 37c4730 (the 22:00 diary).
- **Observed:** nothing at home is reachable. The Air has been offline for about 1 h 50 m (since about 21:30Z) and the Mini for 14 h. There is no headset status, and the watchdog has been silent since 21:04Z.
- **Learned:** nothing new from the VPS side.
- **Next:** wait for the user. On return: the uptimes of both machines, the RT router state, HEARTBEAT, then lift the freeze and run the queue.

## 2026-11-08 00:00 UTC

- **Decided:** no action. The freeze holds, the agents stay idle, and work-ff is untouched.
- **Shipped:** nothing. The last commit is bb8de67 (the 23:00 diary).
- **Observed:** nothing at home is reachable. The Air has been offline about 2 h 50 m (since about 21:30Z) and the Mini about 15 h. The watchdog has been silent since 21:04Z.
- **Learned:** nothing new; the state is unchanged.
- **Next:** wait for the user. On return: the uptimes of both machines, the RT router state, HEARTBEAT, then lift the freeze and run the queue.

## 2026-11-08 01:00 UTC

- **Decided:** no action. The freeze holds, the agents stay idle, and work-ff is untouched.
- **Shipped:** nothing. The last commit is 1d49475 (the 00:00 diary).
- **Verified:** at 01:17:01Z the nightly-eval cron logged "NOTE SKIP: air-freeze present" and made no Air or Mini actions. That's the first live proof of 008a0ae; last night the same cron ran straight through the freeze.
- **Observed:** nothing at home is reachable. The Air has been offline about 3 h 50 m and the Mini about 16 h.
- **Next:** wait for the user. On return: the uptimes of both machines, the RT router state, HEARTBEAT, then lift the freeze and run the queue.

## 2026-11-08 02:00 UTC

- **Decided:** no action. The freeze holds, the agents stay idle, and work-ff is untouched.
- **Shipped:** nothing. The last commit is 2cadb2c (the 01:00 diary).
- **Observed:** nothing at home is reachable. The Air has been offline about 4 h 50 m and the Mini about 17 h. No new log lines since the 01:17Z nightly-eval skip.
- **Learned:** nothing new; it is night in Moscow (05:17 MSK), so I don't expect anything to change before the user is up.
- **Next:** wait for the user. On return: the uptimes of both machines, the RT router state, HEARTBEAT, then lift the freeze and run the queue.

## 2026-11-08 03:00 UTC

- **Decided:** no action. The freeze holds, the agents stay idle, and work-ff is untouched.
- **Shipped:** nothing. The last commit is 177b373 (the 02:00 diary).
- **Observed:** nothing at home is reachable. The Air has been offline about 5 h 50 m and the Mini about 18 h. No new log lines.
- **Learned:** nothing new.
- **Next:** wait for the user. On return: the uptimes of both machines, the RT router state, HEARTBEAT, then lift the freeze and run the queue.

## 2026-11-08 04:00 UTC

- **Decided:** no action. The freeze holds, the agents stay idle, and work-ff is untouched.
- **Shipped:** nothing. The last commit is 8d02a14 (the 03:00 diary).
- **Observed:** nothing at home is reachable. The Air has been offline about 6 h 50 m and the Mini about 19 h. No new log lines.
- **Learned:** nothing new.
- **Next:** wait for the user. On return: the uptimes of both machines, the RT router state, HEARTBEAT, then lift the freeze and run the queue.

## 2026-11-08 05:00 UTC

- **Decided:** no action. The freeze holds, the agents stay idle, and work-ff is untouched.
- **Shipped:** nothing. The last commit is bddbfb4 (the 04:00 diary).
- **Observed:** nothing at home is reachable. The Air has been offline about 7 h 50 m and the Mini about 20 h. No new log lines.
- **Learned:** nothing new.
- **Next:** wait for the user. On return: the uptimes of both machines, the RT router state, HEARTBEAT, then lift the freeze and run the queue.

## 2026-11-08 06:00 UTC

- **Decided:** no action. The freeze holds, the agents stay idle, and work-ff is untouched.
- **Shipped:** nothing. The last commit is 0643676 (the 05:00 diary).
- **Observed:** nothing at home is reachable. The Air has been offline about 8 h 50 m and the Mini about 21 h. No new log lines.
- **Learned:** nothing new.
- **Next:** wait for the user. On return: the uptimes of both machines, the RT router state, HEARTBEAT, then lift the freeze and run the queue.

## 2026-11-08 07:00 UTC

- **Decided:** no action. The freeze holds, the agents stay idle, and work-ff is untouched.
- **Shipped:** nothing. The last commit is de15001 (the 06:00 diary).
- **Observed:** nothing at home is reachable. The Air has been offline about 9 h 50 m and the Mini about 22 h. No new log lines.
- **Learned:** nothing new.
- **Next:** wait for the user (it is about 10:17 MSK). On return: the uptimes of both machines, the RT router state, HEARTBEAT, then lift the freeze and run the queue.

## 2026-11-08 08:00 UTC

- **Decided:** no action. The freeze holds, the agents stay idle, and work-ff is untouched.
- **Shipped:** nothing. The last commit is e419e03 (the 07:00 diary).
- **Observed:** nothing at home is reachable. The Air has been offline about 10 h 50 m and the Mini about 23 h. No new log lines.
- **Learned:** nothing new.
- **Next:** wait for the user. On return: the uptimes of both machines, the RT router state, HEARTBEAT, then lift the freeze and run the queue.

## 2026-11-08 09:00 UTC

- **Decided:** no action. The freeze holds, the agents stay idle, and work-ff is untouched.
- **Shipped:** nothing. The last commit is 9a84fd5 (the 08:00 diary).
- **Observed:** nothing at home is reachable. The Mini has now been offline 24 h (since about 09:09Z yesterday), and the Air about 11 h 50 m. No new log lines.
- **Learned:** nothing new.
- **Next:** wait for the user. On return: the uptimes of both machines, the RT router state, HEARTBEAT, then lift the freeze and run the queue.

## 2026-11-08 10:00 UTC

- **Decided:** no action. The freeze holds, the agents stay idle, and work-ff is untouched.
- **Shipped:** nothing. The last commit is 8030e7e (the 09:00 diary).
- **Observed:** nothing at home is reachable. The Mini has been offline about 25 h and the Air about 12 h 50 m. No new log lines.
- **Learned:** nothing new.
- **Next:** wait for the user. On return: the uptimes of both machines, the RT router state, HEARTBEAT, then lift the freeze and run the queue.

## 2026-11-08 11:00 UTC

- **Decided:** no action. The freeze holds, the agents stay idle, and work-ff is untouched.
- **Shipped:** nothing. The last commit is 39d8196 (the 10:00 diary).
- **Observed:** nothing at home is reachable. The Mini has been offline about 26 h and the Air about 13 h 50 m. No new log lines.
- **Learned:** nothing new.
- **Next:** wait for the user. On return: the uptimes of both machines, the RT router state, HEARTBEAT, then lift the freeze and run the queue.

## 2026-11-08 12:00 UTC

- **Decided:** no action. The freeze holds, the agents stay idle, and work-ff is untouched.
- **Shipped:** nothing. The last commit is 477c849 (the 11:00 diary).
- **Observed:** nothing at home is reachable. The Mini has been offline about 27 h and the Air about 14 h 50 m. No new log lines.
- **Learned:** nothing new.
- **Next:** wait for the user. On return: the uptimes of both machines, the RT router state, HEARTBEAT, then lift the freeze and run the queue.

## 2026-11-08 13:00 UTC

- **Decided:** no action. The freeze holds, the agents stay idle, and work-ff is untouched.
- **Shipped:** nothing. The last commit is b4b889b (the 12:00 diary).
- **Observed:**
  - macbook-air-anastasiia is online and talking directly from 136.169.211.98, the same public IP the Air used yesterday afternoon on 192.168.1.x. So the home uplink is up right now, and someone is home using that MacBook.
  - The Mini has been offline about 28 h and the Air about 15 h 50 m. No new log lines.
- **Learned:** this confirms it from the VPS side. The house has internet; only our two machines are dark. They are off, asleep, or (for the Mini) cut off on a dead RT LAN.
- **Next:** wait for the user. On return: the uptimes of both machines, the RT router state, HEARTBEAT, then lift the freeze and run the queue.

## 2026-11-08 14:00 UTC

- **Decided:** no action. The freeze holds, the agents stay idle, and work-ff is untouched.
- **Shipped:** nothing. The last commit is 3321b1a (the 13:00 diary).
- **Observed:** the home uplink is still up: macbook-air-anastasiia is connected directly from 136.169.211.98. The Mini has been offline about 29 h and the Air about 16 h 50 m. No new log lines.
- **Learned:** nothing new. The user was told at 13:18Z that the house has internet and only our two machines are dark.
- **Next:** wait for the user. On return: the uptimes of both machines, the RT router state, HEARTBEAT, then lift the freeze and run the queue.

## 2026-11-08 15:00 UTC

- **Decided:** no action. The freeze holds, the agents stay idle, and work-ff is untouched.
- **Shipped:** nothing. The last commit is 9dd9c3d (the 14:00 diary).
- **Observed:** the home uplink is still up (macbook-air-anastasiia connected directly from 136.169.211.98). The Mini has been offline about 30 h and the Air about 17 h 50 m. No new log lines.
- **Learned:** nothing new.
- **Next:** wait for the user. On return: the uptimes of both machines, the RT router state, HEARTBEAT, then lift the freeze and run the queue.

## 2026-11-08 16:00 UTC

- **Decided:** no action. The freeze holds, the agents stay idle, and work-ff is untouched.
- **Shipped:** nothing. The last commit is d6f4ab7 (the 15:00 diary).
- **Observed:** the home uplink is up (macbook-air-anastasiia connected directly). The Mini has been offline about 31 h and the Air about 18 h 50 m. No new log lines.
- **Learned:** nothing new.
- **Next:** wait for the user. On return: the uptimes of both machines, the RT router state, HEARTBEAT, then lift the freeze and run the queue.

## 2026-11-08 17:00 UTC

- **Decided:** no action. The freeze holds, the agents stay idle, and work-ff is untouched.
- **Shipped:** nothing. The last commit is 268a3d7 (the 16:00 diary).
- **Observed:** the home uplink is up (macbook-air-anastasiia connected directly). The Mini has been offline about 32 h and the Air about 19 h 50 m. No new log lines.
- **Learned:** nothing new.
- **Next:** wait for the user. On return: the uptimes of both machines, the RT router state, HEARTBEAT, then lift the freeze and run the queue.

## 2026-11-08 18:00 UTC

- **Decided:** I looked at the Air (read only) and changed nothing. The freeze holds, the agents stay idle, and work-ff is untouched.
- **Shipped:** nothing. The last commit is 015d8ff (the 17:00 diary).
- **Observed:**
  - The Air came back from a cold boot at about 17:32Z (up 46 min at 18:18Z). So its 21:30Z dropout yesterday ended in a reboot, not a wake: it was off or out of power.
  - It is again on 192.168.1.35, not the RT LAN.
  - ANECompilerService is no longer in the top 3, so yesterday's 4-h ANE peg did not survive the reboot.
  - colorsync is again a steady ~99% (61% + 38%) right after boot.
  - The watchdog is back (17:38Z, stale lock at 2842 min).
  - The Mini has been offline about 33 h.
- **Learned:** two cold boots in two days, each after hours dark. The Air is losing power: battery run-down, or shut down by hand. colorsync pegs at boot every time, so the kickstart has to become part of the start-up path, not a one-off.
- **Next:** wait for the user on the Mini and the RT router. When the freeze lifts: the colorsync kickstart, and a card for ad to run colorsync_reset automatically after boot (behind a flag).

## 2026-11-08 19:00 UTC

- **Decided:** read only on the Air, and I am telling the user to plug it in. The freeze holds, the agents stay idle, and work-ff is untouched.
- **Shipped:** nothing. The last commit is 06494f5 (the 18:00 diary).
- **Found:** `pmset -g batt` on the Air shows InternalBattery 53%, discharging, 1:26 remaining. The Air is running on battery. That explains both the 21:30Z dropout yesterday and the two cold boots: the battery runs flat, then someone powers it back on, still unplugged. At this rate it dies again around 20:45Z.
- **Learned:** yesterday I blamed "power cut or sleep". I should have run `pmset -g batt` at 16:18Z yesterday when it first came back; one cheap command would have saved a day. It goes into the check: after any unexpected boot, read the power source first.
- **Other:** colorsync is ~99% again, and a Chrome renderer is at 38%. The Mini has been offline about 34 h; the watchdog is back (stale lock 2903 min).
- **Next:** the user plugs in the Air charger and checks the Mini and the RT router. Then the queue after the freeze.

## 2026-11-08 20:00 UTC

- **Decided:** no action. The freeze holds, the agents stay idle, and work-ff is untouched.
- **Shipped:** nothing. The last commit is 127f6d6 (the 19:00 diary).
- **Observed:**
  - The Air is plugged in: 33% and charging (it was 53% and discharging at 19:18Z). So someone acted on the 19:18Z note, and it should not die tonight.
  - macOS estimates 17 h to full. That points to a weak charger or USB source, roughly matching the load. Worth checking it's the proper charger.
  - mediaanalysisd at 58% and colorsync at ~99% after the boot.
  - The Mini has been offline about 35 h. The watchdog reported the stale lock at 2964 min.
- **Learned:** 17 h to full under about 3 load means the charger barely outpaces the drain. A benchmark load could make it discharge while plugged in. Check `pmset -g batt` before any long Air run.
- **Next:** the Mini and the RT router (the user). When the freeze lifts: the colorsync kickstart, the charger check, then the queue.

## 2026-11-08 21:00 UTC

- **Decided:** read only, and I am warning the user again: the Air's charger is losing. The freeze holds, the agents stay idle, and work-ff is untouched.
- **Shipped:** nothing. The last commit is d6f82f2 (the 20:00 diary).
- **Broke:** the Air battery fell from 33% to 18% in the hour, although pmset says "charging". The source is too weak for the load (about 3.3: colorsync ~97%, plus a Chrome renderer at 40%). At about -15%/h it dies around 22:30Z, a third cold boot.
- **Learned:** "charging" in pmset does not mean net positive; compare the percentage across checks. I misread the 20:00 reading ("should not die tonight") from one sample. The colorsync peg directly costs battery here; it is not only a benchmark problem.
- **Other:** the Mini has been offline about 36 h. The watchdog reported the stale lock at 3024 min.
- **Next:** the user swaps to the proper MacBook charger. I'm not doing the colorsync kickstart to save power, because the freeze still holds. The daily diary is due at about 21:27Z.

# Day 2026-11-08

**Arc.** The third frozen day; the freeze from 11-06 still stands.
- The Mini has been dark the whole day: about 36 h since 11-07 09:09Z.
- The Air was dark from 11-07 21:30Z until a cold boot at about 17:32Z, again on 192.168.1.35 rather than the RT LAN.
- From 13:00Z macbook-air-anastasiia was online directly from the home IP (136.169.211.98). That proved the house uplink was up; only our machines were down.
- At 19:18Z `pmset -g batt` finally explained the Air: it was running unplugged (53%, discharging). Both cold boots were a flat battery.
- The user plugged it in by 20:18Z, but the charger loses to the load: 33% fell to 18% in an hour while "charging". The Air will die again around 22:30Z unless the proper charger goes in.
- No code shipped today, only hourly diaries.

**Decisions and why.**
- Read only on the Air after every boot. The freeze covers the machine, so no colorsync kickstart, even though the peg now costs battery.
- No cards for the agents. Every open card needs the Air, the Mini or the headset, and blind work would be thrown away.
- Ask the user once per new fact (house uplink up; Air unplugged; charger too weak) and stay quiet when nothing changes.

**Verified.** The nightly-eval freeze skip (008a0ae) fired live at 01:17Z ("NOTE SKIP: air-freeze present"), the first time this guard has been seen working.

**Mistakes and lessons** (in captain-lessons.md):
- I spent about 26 h guessing "power cut, sleep, or RT router" for the Air. One `pmset -g batt` at the 11-07 16:18Z boot would have said "unplugged". After any unexpected boot, read the power source first.
- At 20:00 I concluded "it should not die tonight" from one "charging" sample. A trend needs two readings: compare the percentage across checks.
- The colorsync peg (~100% after every boot, measured three boots running) is not just benchmark noise. On battery it shortens uptime, so the kickstart belongs in the start-up path.

**State per agent.** All idle and held by the freeze; none crashed. work-ff (not mine) is untouched.
- **work-c4:** v0.76 on the headset; K9 and K17 open. No HEARTBEAT since the Mini died.
- **work-34:** K30 and K31b built, not installed; the E and F reruns pending. K32 in backlog.
- **work-42:** K15 phase 2 DRY watch pending; the nightly-eval freeze guard proven.
- **work-ad:** stall counter live. The ANE peg cleared on reboot, so that card is dropped. New card: a flagged colorsync_reset after boot.

**Tomorrow.**
1. The user: the proper charger on the Air; the Mini and the RT router checked.
2. Once the Mini is back: its uptime and HEARTBEAT; the Air back onto the RT LAN.
3. Lift the freeze, then the colorsync kickstart, then air_smoke (with a battery or charger check).
4. The K30 + K31b install, the pool, the E and F reruns, K15 phase 2; clear the stale bench lock.
5. Open questions: the Mini off the exit node, UPnP on the RT router, the big-wall screen, screens 2 and 3.

## 2026-11-08 22:00 UTC

- **Decided:** read only, and I am telling the user the Air is about to die. I'm not kickstarting colorsync to stretch the battery: the freeze stands, and 30 min would barely be gained.
- **Shipped:** 4db8279 (the Day 2026-11-08 entry). captain-lessons memory updated: pmset "charging" is not net positive; the colorsync peg at every boot.
- **Broke:** the Air is at 14% and discharging, with 0:34 remaining. At 21:18Z it said "charging" at 18%, so the weak charger has dropped out or been unplugged. It will shut down at about 22:50Z, the third cold boot in three days. The Mini has been offline about 37 h.
- **Learned:** nothing new: the same flat-battery cause, seen live this time.
- **Next:** the proper charger in, then the Air boots and comes back on its own. Then the Mini and RT router check, and the queue after the freeze.

## 2026-11-08 23:00 UTC

- **Decided:** no action. The freeze holds, the agents stay idle, and work-ff is untouched.
- **Shipped:** nothing. The last commit is 960efdc (the 22:00 diary).
- **Fixed by the user:** the Air went from 14% discharging (22:18Z) to 57% charging, 1:31 to full. That is a proper charger: about +43% in under an hour. Uptime is 5 h 46 m, so it did not die; it was saved before the 22:50Z cutoff.
- **Observed:** mediaanalysisd at 92% and colorsync at ~100% (load about 4.3). The Mini has been offline about 38 h. The watchdog reported the stale lock at 3145 min.
- **Learned:** the 21:18Z warning, with a concrete deadline and action ("proper charger by 22:30"), got acted on within the hour. Specific asks work.
- **Next:** the Mini and RT router check (the user). When the freeze lifts: the colorsync kickstart, then the queue.

## 2026-11-09 00:00 UTC

- **Decided:** no action. The freeze holds, the agents stay idle, and work-ff is untouched.
- **Shipped:** nothing. The last commit is 58e31c5 (the 23:00 diary).
- **Observed:**
  - The Air is at 97% and charging, up 6 h 46 m, so the power problem is solved.
  - The load is about 4.8: mds_stores at 96% (Spotlight catching up after the boots) plus colorsync at ~100%.
  - The Mini has been offline about 39 h. The watchdog reported the stale lock at 3205 min.
- **Learned:** nothing new.
- **Next:** the Mini and RT router check (the user). When the freeze lifts: the colorsync kickstart, then the queue.

## 2026-11-09 01:00 UTC

- **Decided:** no action. The freeze holds, the agents stay idle, and work-ff is untouched.
- **Shipped:** nothing. The last commit is 4d3519c (the 00:00 diary).
- **Observed:**
  - The Air is at 100% and charged.
  - ANECompilerService is back at 92%. After this boot the order was mds, then mediaanalysisd (58–92%), then ANECompilerService. After the 11-07 boot it was the same compiler for 5 h or more.
  - The nightly-eval freeze skip fired again at 01:17Z.
  - The Mini has been offline about 40 h.
- **Learned:** the ANE peg is likely macOS's own photo and media analysis after a boot, compiling models on the ANE, rather than anything of ours. It is a fixed cost after every boot. air_smoke should wait until mds, mediaanalysisd and ANECompilerService are all quiet, or the numbers carry 2–3 cores of foreign load.
- **Next:** the Mini and RT router check (the user). When the freeze lifts: the colorsync kickstart, an idle gate for those three processes in air_smoke (ad), then the queue.

## 2026-11-09 02:00 UTC

- **Decided:** no action. The freeze holds, the agents stay idle, and work-ff is untouched.
- **Shipped:** nothing. The last commit is 0c6d2cb (the 01:00 diary).
- **Observed:** the Air is charged at 100%, up 8 h 46 m. ANECompilerService is still at 88%, plus colorsync at ~96% (load 2.7). The Mini has been offline about 41 h. The watchdog reported the stale lock at 3326 min.
- **Learned:** nothing new.
- **Next:** the Mini and RT router check (the user). When the freeze lifts: the colorsync kickstart, the idle gate in air_smoke, then the queue.

## 2026-11-09 03:00 UTC

- **Decided:** no action. The freeze holds, the agents stay idle, and work-ff is untouched.
- **Shipped:** nothing. The last commit is 4805130 (the 02:00 diary).
- **Observed:** the Air is charged at 100%, up 9 h 46 m. ANECompilerService is at 98%, about 3 h pegged so far this boot, plus colorsync at ~100%. The Mini has been offline about 42 h. The watchdog reported the stale lock at 3386 min.
- **Learned:** nothing new.
- **Next:** the Mini and RT router check (the user). When the freeze lifts: the colorsync kickstart, the idle gate in air_smoke, then the queue.

## 2026-11-09 04:00 UTC

- **Decided:** no action. The freeze holds, the agents stay idle, and work-ff is untouched.
- **Shipped:** nothing. The last commit is 8dcae5c (the 03:00 diary).
- **Observed:**
  - ANECompilerService has gone quiet, after about 3–4 h pegged following its start around 00:30–01:00Z. With mds and mediaanalysisd also done, the Air's only foreign load is now colorsync at ~100% (62% + 38%). The third process is replayd at 4%.
  - The Air is at 100%, up 10 h 46 m.
  - The Mini has been offline about 43 h. The watchdog reported the stale lock at 3447 min.
- **Learned:** the system work after a boot (mds, then mediaanalysisd, then the ANE compile) takes about 10 h to settle on this Air. colorsync never settles by itself. So the idle gate is worth it, and colorsync needs the kickstart.
- **Next:** the Mini and RT router check (the user). When the freeze lifts: the colorsync kickstart, then air_smoke, then the queue.

## 2026-11-09 05:00 UTC

- **Decided:** no action. The freeze holds, the agents stay idle, and work-ff is untouched.
- **Shipped:** nothing. The last commit is 4594496 (the 04:00 diary).
- **Correction:** at 04:00 I wrote that the Air's system work after the boot "settled in about 10 h". It hasn't: mds_stores is at 83% and mediaanalysisd at 65% again, with load 6.5, at 11 h 46 m up. The quiet hour was a lull, not the end.
- **Learned:** the same lesson as the pmset one: one quiet sample is not a settled state. The air_smoke idle gate must require a sustained quiet window (say 10 min under a CPU threshold), not a single snapshot.
- **Other:** the Air is at 100%, charged. The Mini has been offline about 44 h. The watchdog reported the stale lock at 3507 min.
- **Next:** the Mini and RT router check (the user). When the freeze lifts: the colorsync kickstart, the idle gate with a sustained window (ad), then the queue.

## 2026-11-09 06:00 UTC

- **Decided:** no action. The freeze holds, the agents stay idle, and work-ff is untouched.
- **Shipped:** nothing. The last commit is 76a9c08 (the 05:00 diary).
- **Observed:** the Air is at load 2.0, with only colorsync at ~99%; mds and mediaanalysisd are quiet again. It is at 100% charge, up 12 h 46 m. The Mini has been offline about 45 h. The watchdog reported the stale lock at 3567 min.
- **Learned:** nothing new. Per 05:00, I'm not calling this settled from one sample.
- **Next:** the Mini and RT router check (the user). When the freeze lifts: the colorsync kickstart, the sustained idle gate, then the queue.

## 2026-11-09 07:00 UTC

- **Decided:** no action. The freeze holds, the agents stay idle, and work-ff is untouched.
- **Shipped:** nothing. The last commit is c97396c (the 06:00 diary).
- **Observed:** the Air is at load 1.3, with colorsync at ~100% and PinPin Host at 7.5%. It has been quiet for two checks in a row now. It is at 100% charge, up 13 h 46 m. The Mini has been offline about 46 h. The watchdog reported the stale lock at 3627 min.
- **Learned:** nothing new. With the post-boot work done, colorsync is now nearly all of the Air's idle load, so the kickstart alone should bring it close to clean.
- **Next:** the Mini and RT router check (the user, morning in Moscow now). When the freeze lifts: the colorsync kickstart, air_smoke, then the queue.

## 2026-11-09 08:00 UTC

- **Decided:** hold the freeze (/root/work/.air-freeze is present). All four agents are idle and none crashed; no cards handed out, since every open card touches the Air or the headset. work-ff is untouched.
- **Shipped:** nothing this hour; the last commit is 4e1a8af (the 07:00 diary).
- **Broke or learned:**
  - The Mini is still offline in tailscale ("last seen 1d ago", about 47 h), and /headset/status doesn't answer. Still the user's to check (power, RT router).
  - The Air is up 14 h 46 m on 192.168.1.35 at 100% charged, load 2.1, which is almost all colorsync (displayservices 61% + colorsyncd 38%). The probe macbook-air-anastasiia went offline 7 min before the check: a peer sleeping, not a fault.
  - The watchdog flagged the stale 18:15:37 bench lock again (3688 min). There are no ghost jobs to close.
- **Next:** unchanged. Wait for the Mini and the freeze lift, then the colorsync kickstart, air_smoke, the K30+K31b install, the E and F reruns, and the K15 phase 2 DRY.

## 2026-11-09 09:00 UTC

- **Decided:** hold the freeze (/root/work/.air-freeze is present). All four agents are idle and none crashed; no cards handed out, since every open card touches the Air or the headset. work-ff is untouched.
- **Shipped:** nothing this hour; the last commit is 65a505e (the 08:00 diary).
- **Broke or learned:**
  - tailscale now lists the Mini as "last seen 2d ago" (about 48 h dark), and /headset/status gets no answer.
  - The Air is up 15 h 46 m on 192.168.1.35 at 100% charged, load 2.0, which is still almost all colorsync (62% + 38%).
  - The probe macbook-air-anastasiia has been offline 37 min: a sleeping peer, not a fault.
  - The watchdog re-flagged the stale bench lock (3748 min). There are no ghost jobs to close.
- **Next:** unchanged. The user checks the Mini and the RT router and lifts the freeze; then the colorsync kickstart, air_smoke, the K30+K31b install, the E and F reruns, and the K15 phase 2 DRY.

## 2026-11-09 10:00 UTC

- **Decided:** hold the freeze (/root/work/.air-freeze is present). All four agents are idle and none crashed; no cards handed out, since every open card touches the Air or the headset. work-ff is untouched.
- **Shipped:** nothing this hour; the last commit is a4cdde7 (the 09:00 diary).
- **Broke or learned:**
  - The Mini is still "last seen 2d ago" (about 49 h), and /headset/status gets no answer.
  - The Air is up 16 h 46 m on 192.168.1.35 at 100% charged, load 1.8–2.2. Of that, colorsync (62% + 38%) is the only real load: a steady ~1 core burned for 16 h.
  - The probe macbook-air-anastasiia was seen 17 min ago: it comes and goes.
  - The watchdog re-flagged the stale bench lock (3808 min). There are no ghost jobs to close.
- **Next:** unchanged. The user checks the Mini and the RT router and lifts the freeze; then the colorsync kickstart, air_smoke, the K30+K31b install, the E and F reruns, and the K15 phase 2 DRY.

## 2026-11-09 11:00 UTC

- **Decided:** hold the freeze (/root/work/.air-freeze is present). All four agents are idle and none crashed; no cards handed out, since every open card touches the Air or the headset. work-ff is untouched.
- **Shipped:** nothing this hour; the last commit is 9284bdc (the 10:00 diary).
- **Broke or learned:**
  - The Mini is still "last seen 2d ago" (about 50 h), and /headset/status gets no answer.
  - The Air is up 17 h 46 m on 192.168.1.35 at 100% charged, load 1.8–2.2, with colorsync at 62% + 38% as before.
  - A Python process (5.8%) shows up third on the Air: probably the apps runner or a plugin, given its Homebrew path. It's small and doesn't touch the freeze, so I'm not chasing it.
  - The probe macbook-air-anastasiia shows the same "17m ago" as last hour, so that field looks stale; I'll read it as on and off.
  - The watchdog re-flagged the stale bench lock (3869 min). There are no ghost jobs to close.
- **Next:** unchanged. The user checks the Mini and the RT router and lifts the freeze; then the colorsync kickstart, air_smoke, the K30+K31b install, the E and F reruns, and the K15 phase 2 DRY.

## 2026-11-09 12:00 UTC

- **Decided:** hold the freeze (/root/work/.air-freeze is present). All four agents are idle and none crashed; no cards handed out, since every open card touches the Air or the headset. work-ff is untouched.
- **Shipped:** nothing this hour; the last commit is df115c0 (the 11:00 diary).
- **Broke or learned:**
  - The Mini is still "last seen 2d ago" (about 51 h), and /headset/status gets no answer.
  - The Air is up 18 h 46 m on 192.168.1.35 at 100% charged, load 1.8–2.1, with colorsync at 61% + 38%. Last hour's Python blip has gone.
  - The probe's "17m ago" has now been frozen for three hours, which confirms that field is stale (the tailscale peer cache). I'm not reading it as liveness anymore.
  - The watchdog re-flagged the stale bench lock (3930 min). There are no ghost jobs to close.
- **Next:** unchanged. The user checks the Mini and the RT router and lifts the freeze; then the colorsync kickstart, air_smoke, the K30+K31b install, the E and F reruns, and the K15 phase 2 DRY.

## 2026-11-09 13:00 UTC

- **Decided:** hold the freeze (/root/work/.air-freeze is present). All four agents are idle and none crashed; no cards handed out, since every open card touches the Air or the headset. work-ff is untouched.
- **Shipped:** nothing this hour; the last commit is 93aaf74 (the 12:00 diary).
- **Broke or learned:**
  - The Mini is still "last seen 2d ago" (about 52 h), and /headset/status gets no answer.
  - The Air is up 19 h 46 m on 192.168.1.35 at 100% charged, load 2.2, with colorsync at 62% + 38%.
  - **Correction to 12:00:** the probe's "last seen" moved to 37m ago, so the field isn't stale. The repeated "17m" was more likely the peer waking at a regular interval that lines up with my :17 checks. I called it stale after three samples with no second explanation, which is the one-sample mistake again, just slower.
  - The watchdog re-flagged the stale bench lock (3990 min). There are no ghost jobs to close.
- **Next:** unchanged. The user checks the Mini and the RT router and lifts the freeze; then the colorsync kickstart, air_smoke, the K30+K31b install, the E and F reruns, and the K15 phase 2 DRY.

## 2026-11-09 14:00 UTC

- **Decided:** hold the freeze (/root/work/.air-freeze is present). All four agents are idle and none crashed; no cards handed out, since every open card touches the Air or the headset. work-ff is untouched.
- **Shipped:** nothing this hour; the last commit is 750965d (the 13:00 diary).
- **Broke or learned:**
  - The Mini is still "last seen 2d ago" (about 53 h), and /headset/status gets no answer.
  - The Air is up 20 h 46 m on 192.168.1.35 at 100% charged, load 1.8–2.3, with colorsync at 62% + 38%. Nothing else is above 5%.
  - The probe was last seen 7m ago, which matches the 13:00 correction: it wakes on its own schedule.
  - The watchdog re-flagged the stale bench lock (4051 min). There are no ghost jobs to close.
- **Next:** unchanged. The user checks the Mini and the RT router and lifts the freeze; then the colorsync kickstart, air_smoke, the K30+K31b install, the E and F reruns, and the K15 phase 2 DRY.

## 2026-11-09 15:00 UTC

- **Decided:** hold the freeze (/root/work/.air-freeze is present). All four agents are idle and none crashed; no cards handed out. work-ff is untouched. I'm telling the user one line about the Air's power (below), because that's a change and it has a deadline.
- **Shipped:** nothing this hour; the last commit is 3c9dd16 (the 14:00 diary).
- **Broke or learned:**
  - **The Air is off the charger.** pmset reads "100%; discharging; 6:34 remaining" (it was charged at 14:18Z). colorsync is still burning about one core (62% + 38%, load 2.1). At this rate the Air runs flat around 21:45Z, the third flat-battery boot in four days, unless it goes back on the charger. The kickstart would stretch that, but it's frozen.
  - The Mini is still "last seen 2d ago" (about 54 h).
  - The probe macbook-air-anastasiia is moving real traffic (tx 5.9 MB), so someone is using it: the home uplink works.
  - The watchdog re-flagged the stale bench lock (4111 min). There are no ghost jobs to close.
- **Next:** at 16:00, a second pmset reading to confirm the trend. The post-freeze queue is unchanged.

## 2026-11-09 16:00 UTC

- **Decided:** hold the freeze (/root/work/.air-freeze is present). All four agents are idle and none crashed; no cards handed out. work-ff is untouched. I won't send the user another line about the Air: I already asked at 15:00, and the next change worth reporting is it coming back.
- **Shipped:** nothing this hour; the last commit is 37448a1 (the 15:00 diary).
- **Broke or learned:**
  - **The Air went dark around 15:50Z** (tailscale shows "last seen 27m ago", and ssh times out). That's six hours before my 21:45Z battery estimate, so it's probably not a flat battery. The likelier causes are lid-close sleep after unplugging, or the Air being carried somewhere. I can't tell which without pmset, so I'm not calling a cause.
  - The Mini is still "last seen 2d ago" (about 55 h). The probe macbook-air-anastasiia is active (1m ago), so the uplink works.
  - The watchdog (pid 1887348, alive for 5 d 18 h) skipped its ~15:47 stale-lock line; the last one is 14:47. I haven't checked whether it skips when the Air is unreachable or whether it's just drift. It's harmless either way. There are no ghost jobs to close.
- **Next:** when the Air returns, run `pmset -g batt` and uptime first: a fresh boot means it went flat, an old uptime means it slept. The post-freeze queue is unchanged.

## 2026-11-09 17:00 UTC

- **Decided:** hold the freeze (/root/work/.air-freeze is present). All four agents are idle and none crashed; no cards handed out. work-ff is untouched. No new line to the user: the Air going dark was already reported at 16:00.
- **Shipped:** nothing this hour; the last commit is 3511145 (the 16:00 diary).
- **Broke or learned:**
  - The Air is still dark: tailscale says "last seen 1h ago" and ssh times out. The cause is still unknown, and I'll check with pmset and uptime when it's back.
  - The Mini is still "last seen 2d ago" (about 56 h). The probe macbook-air-anastasiia is active, so the uplink works.
  - The watchdog has now skipped two stale-lock lines (the last is 14:47), and both gaps line up with the Air being dark. That points to it skipping its check when the Air is unreachable, not to drift, though I haven't read the code to confirm. Its process is alive. There are no ghost jobs to close.
- **Next:** unchanged. When the Air returns, pmset and uptime first; then the post-freeze queue once the user lifts the freeze.

## 2026-11-09 18:00 UTC

- **Decided:** hold the freeze (/root/work/.air-freeze is present). All four agents are idle and none crashed; no cards handed out. work-ff is untouched. No new line to the user: nothing has changed since 16:00.
- **Shipped:** nothing this hour; the last commit is 92fb9a9 (the 17:00 diary).
- **Broke or learned:**
  - The Air has been dark about 2.5 h ("last seen 2h ago", and ssh times out); the cause is still unknown. If it is asleep on battery, the 6:34 estimate from 14:18Z no longer applies, because sleep draws far less.
  - The Mini is still "last seen 2d ago" (about 57 h). The probe macbook-air-anastasiia is active, so the uplink works.
  - The watchdog has been silent since 14:47, which still lines up with the Air being dark. There are no ghost jobs to close.
- **Next:** unchanged. pmset and uptime first when the Air is back; the daily diary is at ~21:27Z.

## 2026-11-09 19:00 UTC

- **Decided:** hold the freeze (/root/work/.air-freeze is present). All four agents are idle and none crashed; no cards handed out. work-ff is untouched. I'm sending the user one line about the Air's battery, because it has a deadline about 2.5 h away.
- **Shipped:** nothing this hour; the last commit is e84da48 (the 18:00 diary).
- **Broke or learned:**
  - **The Air is back without a reboot** (up 1 d 1 h 46 m), so the 15:50Z drop was sleep or a move, not a flat battery. It now reaches us from a different public IP, 77.40.62.77 (home is 136.169.211.98), with LAN address 192.168.0.14, so it's on another network away from home. I won't assume that's the RT LAN: the subnet only looks familiar.
  - **Battery is at 41%, discharging, 2:36 left** (it was 100% at 14:18Z), so it goes flat around 21:50Z. colorsync is still burning about one core (61% + 38%) and accounts for most of the drain. This is the case for putting the kickstart in the start-up path once the freeze lifts.
  - The first ssh hung at 15 s on the new path and the retry worked: a slow first connection, not a fault.
  - The watchdog came back at 18:55Z (4359 min), which fits the Air returning; its silence was the Air gate, not drift. The Mini is still "2d ago" (about 58 h). There are no ghost jobs to close.
- **Next:** the daily diary at ~21:27Z, with a pmset reading.

## 2026-11-09 20:00 UTC

- **Decided:** hold the freeze (/root/work/.air-freeze is present). All four agents are idle and none crashed; no cards handed out. work-ff is untouched. One short reminder to the user, because the battery deadline is now under 2 h.
- **Shipped:** nothing this hour; the last commit is 1856d42 (the 19:00 diary).
- **Broke or learned:**
  - **The Air is still unplugged:** 41% to 25% in an hour (16%/h), with 1:43 left, so it goes flat around 22:00Z. It's still away from home (77.40.62.77, 192.168.0.14), and load is up to 2.6.
  - A Python 3.9 process showed 24.6% CPU in one sample. By the follow-up it had dropped out, and the only Pythons left are the long-lived apps_runner, the hf-ssh relay and pinpin_assistant (all up 1 d 2 h, near 0%). So it was a transient burst, most likely the assistant. I'm not chasing it during the freeze.
  - The watchdog is back on its hourly line (19:58Z, 4422 min). The Mini is still "2d ago" (about 59 h). There are no ghost jobs to close.
- **Next:** the daily diary at ~21:27Z. If the Air is dark by then, the pmset readings at 14:18, 19:18 and 20:18Z already explain why.

## 2026-11-09 21:00 UTC

- **Decided:** hold the freeze (/root/work/.air-freeze is present). All four agents are idle and none crashed; no cards handed out. work-ff is untouched.
- **Shipped:** nothing this hour; the last commit is 8b455f4 (the 20:00 diary).
- **Broke or learned:**
  - **The Air is at 12%, discharging, 0:57 left,** so it goes flat around 22:15Z. The rate is 13%/h, down from 16%/h, with load steady at 2.1 and colorsync at 62% + 38%. Still away from home (77.40.62.77). If it dies, the next boot shows up as a fresh uptime, and that's already explained: no mystery this time.
  - The watchdog's hourly line is regular again (20:58Z, 4483 min). The Mini is still "2d ago" (about 60 h). There are no ghost jobs to close.
- **Learned:** with colorsync pegged, a full charge lasts about 7.5 h unplugged (100% at 14:18Z to about 22:15Z). That's the number to put in front of the user when we argue for the start-up kickstart.
- **Next:** the daily diary at ~21:27Z.

# Day 2026-11-09

**Arc.** The fourth day of the demo freeze (/root/work/.air-freeze, set 11-06 at 19:00Z), and nothing shipped except the 22 hourly diary commits (4d3519c through 3a670c4). The kanban hasn't moved since rev 105 (11-06 22:18Z), and LOG.md last changed on 11-04. The Mini was dark all day ("last seen 2d ago", about 60 h by 21:00Z), so the house server, HEARTBEAT and adb were all out of reach. The Air told the day's story:
- 00:00–14:18Z: plugged in at home, at 100% from 01:18Z.
- About 15:00Z: unplugged.
- About 15:50Z: dark, without rebooting.
- 18:55Z: back from a different public IP (77.40.62.77, LAN 192.168.0.14), so it had been moved off the home network.
- 21:18Z: draining at 12% with 0:57 left, flat around 22:15Z.

**Decisions and why.**
- Kept every agent idle and handed out no cards. Every open card (K30/K31b install, the E and F reruns, K15 phase 2, the colorsync kickstart) touches the Air or the headset, and the user froze both.
- Told the user only when something changed and had a deadline: the Air unplugged at 15:00Z, at 41% at 19:00Z, at 25% at 20:00Z, at 12% at 21:00Z. Otherwise one line saying nothing needed them, and I didn't repeat the Mini request each hour.
- Didn't chase a Python burst (24.6%) on the Air during the freeze; a second sample showed only the long-lived runner, relay and assistant near 0%.

**Mistakes and lessons** (fed into captain-lessons.md):
- **04:00Z:** "post-boot work settled in ~10 h". mds and mediaanalysisd were back at 05:00Z. One quiet sample isn't settled, so air_smoke's idle gate needs a sustained quiet window.
- **12:00Z:** I called the probe's "last seen 17m ago" a stale field after three identical readings. At 13:00Z it moved. More likely the peer wakes at an interval that lines up with my :17 checks. A fixed-cadence check can alias a periodic signal, so identical readings need a second explanation ruled out before calling a field stale.
- **16:00Z:** I correctly refused to guess between a flat battery and sleep, and uptime at 19:00Z settled it (no reboot). Uptime plus pmset tells "slept or moved" from "died".
- The watchdog went silent from 14:47 to 18:55Z, exactly while the Air was dark. A silent watchdog means an unreachable Air, not a dead watchdog (its pid was alive).

**Learned:** with colorsync pegged at one core, a full charge lasts about 7.5 h unplugged (100% at 14:18Z to about 22:15Z). That's the concrete case for the start-up kickstart.

**State per agent** (all idle, none crashed; work-ff untouched):
- **work-c4 (headset):** v0.76 is on the headset (unverified since the Mini went dark); K9 and K17 are open.
- **work-34 / work-09 (bench, geometry, Host):** K30 and K31b are built and not installed; K32 is in the backlog; E and F reruns are pending; the stale bench lock from 11-06 18:15:37 is still there (4483 min).
- **work-42 (assistant, perf):** K15 phase 2 DRY waits on the freeze lift and a live vis.
- **work-ad (apps, ops, perf):** air_smoke with a sustained idle gate, a battery check and a flagged colorsync_reset at boot are queued.

**Tomorrow:**
1. When the Air comes back after dying, run pmset and uptime first (a fresh boot is expected and already explained).
2. The user checks the Mini and the RT router.
3. On the freeze lift: the colorsync kickstart (me), air_smoke (ad), the K30+K31b install (34), the E and F reruns, K15 phase 2 DRY (42), and clearing the stale lock.
4. Open questions for the user: the big-wall screen, screens 2 and 3, and UPnP.

## 2026-11-09 22:00 UTC

- **Decided:** hold the freeze (/root/work/.air-freeze is present). All four agents are idle and none crashed; no cards handed out. work-ff is untouched. One last line to the user: the Air is about to die.
- **Shipped:** 5eeb6d4 (the 11-09 daily diary). In memory, captain-lessons gained a 11-09 section: one sample isn't settled, a fixed cadence can alias a periodic signal, uptime tells died from slept, and a silent watchdog means the Air is unreachable.
- **Broke or learned:**
  - **The Air reports "Battery Warning: Final"** at 22:18Z, still on battery and still away from home (77.40.62.77). It's up 1 d 4 h 46 m with load 1.8–2.2 and colorsync at 62% + 38%, on track for the 22:15Z estimate. My `tail -1` on pmset picked up the warning line instead of the percentage. The check should grep for the `%` line.
  - The Python 3.9 burst came back (28.7%), probably the pinpin-assistant. Not chased during the freeze.
  - The watchdog is still regular (21:58Z, 4543 min). The Mini is still "2d ago". There are no ghost jobs to close.
- **Next:** expect the Air dark by 23:00Z. When it returns, a fresh uptime is expected; check pmset first.

## 2026-11-09 23:00 UTC

- **Decided:** hold the freeze (/root/work/.air-freeze is present). All four agents are idle and none crashed; no cards handed out. work-ff is untouched. No new line to the user: the Air going dark was predicted and reported at 22:00.
- **Shipped:** nothing this hour; the last commit is 82fbc71 (the 22:00 diary).
- **Broke or learned:**
  - **The Air went dark around 22:38Z** ("last seen 39m ago", and ssh times out), 23 min after the 22:15Z estimate and 20 min after "Battery Warning: Final". Almost certainly a flat battery, but I'll confirm with uptime when it returns rather than assume.
  - The Mini is still "2d ago" (about 62 h). The probe macbook-air-anastasiia was seen 7m ago.
  - The watchdog has gone silent again (last line 21:58Z), which fits the Air being unreachable. There are no ghost jobs to close.
- **Next:** when the Air returns, uptime and pmset first. A fresh boot means colorsync is pegged again and the post-boot mds, mediaanalysisd and ANE waves restart. The post-freeze queue is unchanged.

## 2026-11-10 00:00 UTC

- **Decided:** hold the freeze (/root/work/.air-freeze is present). All four agents are idle and none crashed; no cards handed out. work-ff is untouched. No line to the user: nothing has changed since 23:00.
- **Shipped:** nothing this hour; the last commit is c524a92 (the 23:00 diary).
- **Broke or learned:**
  - The Air is still dark ("last seen 1h ago", and ssh times out) since about 22:38Z, presumably flat. The Mini is still "2d ago" (about 63 h).
  - The watchdog is silent (last line 21:58Z) while the Air is unreachable, as expected. There are no ghost jobs to close.
  - The nightly-eval cron (01:17Z) should skip on the freeze flag again. I'll check its line at 01:00.
- **Next:** unchanged. Uptime and pmset first when the Air returns; the post-freeze queue once the user lifts the freeze.

## 2026-11-10 01:00 UTC

- **Decided:** hold the freeze (/root/work/.air-freeze is present). All four agents are idle and none crashed; no cards handed out. work-ff is untouched. No line to the user: nothing has changed.
- **Shipped:** nothing this hour; the last commit is 8709225 (the 00:00 diary).
- **Verified:** the nightly-eval cron skipped on the freeze flag at 01:17:01Z (work-42 "NOTE SKIP: air-freeze present"). That's the third night running the 008a0ae guard has held.
- **Broke or learned:**
  - The Air is still dark ("last seen 2h ago", and ssh times out) since about 22:38Z. The Mini is still "2d ago" (about 64 h).
  - The watchdog is silent since 21:58Z, which fits the Air being unreachable. There are no ghost jobs to close.
- **Next:** unchanged. Uptime and pmset first when the Air returns; the post-freeze queue once the user lifts the freeze.

## 2026-11-10 02:00 UTC

- **Decided:** hold the freeze (/root/work/.air-freeze is present). All four agents are idle and none crashed; no cards handed out. work-ff is untouched. No line to the user: nothing has changed.
- **Shipped:** nothing this hour; the last commit is cc58164 (the 01:00 diary).
- **Broke or learned:**
  - The Air is still dark ("last seen 3h ago", and ssh times out) since about 22:38Z. The Mini is still "2d ago" (about 65 h).
  - The probe macbook-air-anastasiia was seen 1m ago.
  - The watchdog is silent since 21:58Z, which fits the Air being unreachable. There are no ghost jobs to close.
- **Next:** unchanged. Uptime and pmset first when the Air returns; the post-freeze queue once the user lifts the freeze.

## 2026-11-10 03:00 UTC

- **Decided:** hold the freeze (/root/work/.air-freeze is present). All four agents are idle and none crashed; no cards handed out. work-ff is untouched. No line to the user: nothing has changed, and it's night at home.
- **Shipped:** nothing this hour; the last commit is 6c1b006 (the 02:00 diary).
- **Broke or learned:**
  - The Air is still dark ("last seen 4h ago", and ssh times out) since about 22:38Z. The Mini is still "2d ago" (about 66 h).
  - The watchdog is silent since 21:58Z, which fits the Air being unreachable. There are no ghost jobs to close.
- **Learned:** nothing new.
- **Next:** unchanged. Uptime and pmset first when the Air returns; the post-freeze queue once the user lifts the freeze.

## 2026-11-10 04:00 UTC

- **Decided:** hold the freeze (/root/work/.air-freeze is present). All four agents are idle and none crashed; no cards handed out. work-ff is untouched. No line to the user: nothing has changed.
- **Shipped:** nothing this hour; the last commit is 4a0eb64 (the 03:00 diary).
- **Broke or learned:**
  - The Air is still dark ("last seen 5h ago", and ssh times out) since about 22:38Z. The Mini is still "2d ago" (about 67 h).
  - The watchdog is silent since 21:58Z, which fits the Air being unreachable. There are no ghost jobs to close.
- **Learned:** nothing new.
- **Next:** unchanged. Uptime and pmset first when the Air returns; the post-freeze queue once the user lifts the freeze.

## 2026-11-10 05:00 UTC

- **Decided:** hold the freeze (/root/work/.air-freeze is present). All four agents are idle and none crashed; no cards handed out. work-ff is untouched. No line to the user: nothing has changed.
- **Shipped:** nothing this hour; the last commit is ba1af90 (the 04:00 diary).
- **Broke or learned:**
  - The Air is still dark ("last seen 6h ago", and ssh times out) since about 22:38Z. The Mini is still "2d ago" (about 68 h).
  - The watchdog is silent since 21:58Z, which fits the Air being unreachable. There are no ghost jobs to close.
- **Learned:** nothing new.
- **Next:** unchanged. Uptime and pmset first when the Air returns; the post-freeze queue once the user lifts the freeze.

## 2026-11-10 06:00 UTC

- **Decided:** hold the freeze (/root/work/.air-freeze is present). All four agents are idle and none crashed; no cards handed out. work-ff is untouched. No line to the user: nothing has changed for them.
- **Shipped:** nothing this hour; the last commit is 629b4f1 (the 05:00 diary).
- **Broke or learned:**
  - The probe macbook-air-anastasiia is now **direct from the home public IP 136.169.211.98** (it was on the "hel" relay before). The home uplink works and that peer is at home right now, so the Mini being dark (about 69 h, "2d ago") is local to the Mini or its LAN, not the uplink. That's what the user needs to look at.
  - The Air is still dark ("last seen 7h ago") since about 22:38Z, presumably flat and still away.
  - The watchdog is silent since 21:58Z, which fits the Air being unreachable. There are no ghost jobs to close.
- **Next:** unchanged. Uptime and pmset first when the Air returns; the post-freeze queue once the user lifts the freeze.

## 2026-11-10 07:00 UTC

- **Decided:** hold the freeze (/root/work/.air-freeze is present). All four agents are idle and none crashed; no cards handed out. work-ff is untouched. No line to the user: nothing has changed.
- **Shipped:** nothing this hour; the last commit is f00a17f (the 06:00 diary).
- **Broke or learned:**
  - The Air is still dark ("last seen 8h ago") since about 22:38Z. The Mini is still "2d ago" (about 70 h).
  - The probe macbook-air-anastasiia is back on the "hel" relay (it was direct at 06:00). The relay-or-direct state flips with the NAT path, so a relay reading doesn't mean the device left home. The 06:00 direct sighting is still the evidence that the uplink works.
  - The watchdog is silent since 21:58Z, which fits the Air being unreachable. There are no ghost jobs to close.
- **Next:** unchanged. Uptime and pmset first when the Air returns; the post-freeze queue once the user lifts the freeze.

## 2026-11-10 08:00 UTC

- **Decided:** hold the freeze (/root/work/.air-freeze is present). All four agents are idle and none crashed; no cards handed out. work-ff is untouched. No line to the user: nothing has changed.
- **Shipped:** nothing this hour; the last commit is 21f8b5d (the 07:00 diary).
- **Broke or learned:**
  - The Air is still dark ("last seen 9h ago") since about 22:38Z. The Mini is still "2d ago" (about 71 h).
  - The probe is direct from the home IP again (136.169.211.98), so the home uplink is up.
  - The watchdog is silent since 21:58Z, which fits the Air being unreachable. There are no ghost jobs to close.
- **Learned:** nothing new.
- **Next:** unchanged. Uptime and pmset first when the Air returns; the post-freeze queue once the user lifts the freeze.

## 2026-11-10 09:00 UTC

- **Decided:** hold the freeze (/root/work/.air-freeze is present). All four agents are idle and none crashed; no cards handed out. work-ff is untouched. No line to the user: nothing has changed.
- **Shipped:** nothing this hour; the last commit is 7ec4b1d (the 08:00 diary).
- **Broke or learned:**
  - tailscale now shows the Mini as "last seen 3d ago" (about 72 h dark).
  - The Air is still dark ("last seen 10h ago") since about 22:38Z.
  - The watchdog is silent since 21:58Z, which fits the Air being unreachable. There are no ghost jobs to close.
- **Learned:** nothing new.
- **Next:** unchanged. Uptime and pmset first when the Air returns; the post-freeze queue once the user lifts the freeze.

## 2026-11-10 10:00 UTC

- **Decided:** hold the freeze (/root/work/.air-freeze is present). All four agents are idle and none crashed; no cards handed out. work-ff is untouched. No line to the user: nothing has changed.
- **Shipped:** nothing this hour; the last commit is 0f9a462 (the 09:00 diary).
- **Broke or learned:**
  - The Air is still dark ("last seen 11h ago") since about 22:38Z. The Mini is still "3d ago" (about 73 h).
  - The watchdog is silent since 21:58Z, which fits the Air being unreachable. There are no ghost jobs to close.
- **Learned:** nothing new.
- **Next:** unchanged. Uptime and pmset first when the Air returns; the post-freeze queue once the user lifts the freeze.

## 2026-11-10 11:00 UTC

- **Decided:** hold the freeze (/root/work/.air-freeze is present). All four agents are idle and none crashed; no cards handed out. work-ff is untouched. No line to the user: nothing has changed.
- **Shipped:** nothing this hour; the last commit is 834351e (the 10:00 diary).
- **Broke or learned:**
  - The Air is still dark ("last seen 12h ago") since about 22:38Z. The Mini is still "3d ago" (about 74 h).
  - The watchdog is silent since 21:58Z, which fits the Air being unreachable. There are no ghost jobs to close.
- **Learned:** nothing new.
- **Next:** unchanged. Uptime and pmset first when the Air returns; the post-freeze queue once the user lifts the freeze.

## 2026-11-10 12:00 UTC

- **Decided:** hold the freeze (/root/work/.air-freeze is present). All four agents are idle and none crashed; no cards handed out. work-ff is untouched. No line to the user: nothing has changed.
- **Shipped:** nothing this hour; the last commit is a99db6e (the 11:00 diary).
- **Broke or learned:**
  - The Air is still dark ("last seen 13h ago") since about 22:38Z. The Mini is still "3d ago" (about 75 h).
  - The watchdog is silent since 21:58Z, which fits the Air being unreachable. There are no ghost jobs to close.
- **Learned:** nothing new.
- **Next:** unchanged. Uptime and pmset first when the Air returns; the post-freeze queue once the user lifts the freeze.

## 2026-11-10 13:00 UTC

- **Decided:** hold the freeze (/root/work/.air-freeze is present). All four agents are idle and none crashed; no cards handed out. work-ff is untouched. I'm telling the user that the Mini is half back: it's a change, and they may be at it right now.
- **Shipped:** nothing this hour; the last commit is 26e1ae7 (the 12:00 diary).
- **Broke or learned:**
  - **The Mini is back on tailscale** after about 76 h. It shows active on the "hel" relay with real traffic (tx 13.8 MB, rx 6.3 MB), and `tailscale ping` answers via DERP(hel) in 0.45–1.5 s, with no direct path.
  - **But TCP to it fails:** ssh :22 and the house server :57800 both time out, even with 40 s timeouts. So the tailscale node is up and the services or the TCP path are not. It may still be booting, its sshd and house server may be down, or the relay path may be too slow. I can't see which without a shell. The traffic counters may be its exit-node traffic through this VPS, so they don't prove the services are up.
  - Lesson already on file: "active" with traffic is not "reachable". Test the actual service port.
  - The Air is still dark ("last seen 14h ago"). The watchdog is silent. There are no ghost jobs to close.
- **Next:** retry ssh and :57800 at 14:00. If they're up: uptime, the LAN IP (is it back on 192.168.0.x?), HEARTBEAT ageS. If they're still down, ask the user whether the Mini is at the login screen.

## 2026-11-10 14:00 UTC

- **Decided:** hold the freeze (/root/work/.air-freeze is present). All four agents are idle and none crashed; no cards handed out. work-ff is untouched. No new line to the user: I asked at 13:00, and the Mini vanishing again doesn't change what they'd do.
- **Shipped:** nothing this hour; the last commit is d160dad (the 13:00 diary).
- **Broke or learned:**
  - **The Mini dropped off again around 13:40Z** ("last seen 37m ago"). It was on tailscale for roughly 13:00–13:40Z, through the DERP relay only, while ssh and :57800 never answered. That pattern fits a machine that woke briefly (a power nap or Wake for network access) or a boot that never reached its services, more than a healthy machine. I can't tell which without a shell, and I'm not guessing in the user line.
  - The probe macbook-air-anastasiia is direct from the home IP with real traffic (tx 124 MB), so the home uplink is fine. The Mini's problem is local.
  - The Air is still dark ("last seen 15h ago"). The watchdog is silent. There are no ghost jobs to close.
- **Next:** the same checks at 15:00. If the Mini reappears, test the service ports first, not the tailscale state.

## 2026-11-10 15:00 UTC

- **Decided:** hold the freeze (/root/work/.air-freeze is present). All four agents are idle and none crashed; no cards handed out. work-ff is untouched. No line to the user: nothing has changed since 14:00.
- **Shipped:** nothing this hour; the last commit is 22d06ef (the 14:00 diary).
- **Broke or learned:**
  - The Mini is dark again ("last seen 1h ago", since about 13:40Z), and ssh and :57800 time out. The Air is still dark ("last seen 16h ago").
  - The probe macbook-air-anastasiia is direct from home and busy (its tailscale counters are up to 837 MB), so the home uplink and the tailscale path are fine. Its traffic isn't ours to look into.
  - The watchdog is silent since 21:58Z. There are no ghost jobs to close.
- **Learned:** nothing new.
- **Next:** unchanged. Service ports first if the Mini reappears; uptime and pmset first for the Air; the post-freeze queue once the user lifts the freeze.

## 2026-11-10 16:00 UTC

- **Decided:** hold the freeze (/root/work/.air-freeze is present). All four agents are idle and none crashed; no cards handed out. work-ff is untouched. No line to the user: nothing has changed.
- **Shipped:** nothing this hour; the last commit is 3a6a63d (the 15:00 diary).
- **Broke or learned:**
  - The Mini is dark ("last seen 2h ago", since about 13:40Z), and ssh and :57800 time out. The Air is still dark ("last seen 17h ago").
  - The probe macbook-air-anastasiia is direct from home, so the uplink is fine.
  - The watchdog is silent since 21:58Z. There are no ghost jobs to close.
- **Learned:** nothing new.
- **Next:** unchanged. Service ports first if the Mini reappears; uptime and pmset first for the Air; the post-freeze queue once the user lifts the freeze.

## 2026-11-10 17:00 UTC

- **Decided:** hold the freeze (/root/work/.air-freeze is present). All four agents are idle and none crashed; no cards handed out. work-ff is untouched. No line to the user: nothing has changed.
- **Shipped:** nothing this hour; the last commit is 040761e (the 16:00 diary).
- **Broke or learned:**
  - The Mini is dark ("last seen 3h ago", since about 13:40Z), and ssh and :57800 time out. The Air is still dark ("last seen 18h ago").
  - The watchdog is silent since 21:58Z. There are no ghost jobs to close.
- **Learned:** nothing new.
- **Next:** unchanged. Service ports first if the Mini reappears; uptime and pmset first for the Air; the post-freeze queue once the user lifts the freeze.

## 2026-11-10 18:00 UTC

- **Decided:** hold the freeze (/root/work/.air-freeze is present). All four agents are idle and none crashed; no cards handed out. work-ff is untouched. No line to the user: nothing has changed.
- **Shipped:** nothing this hour; the last commit is dd4dd2c (the 17:00 diary).
- **Broke or learned:**
  - The Mini is dark ("last seen 4h ago", since about 13:40Z), and ssh and :57800 time out. The Air is still dark ("last seen 19h ago").
  - The probe is direct from home, so the uplink is fine.
  - The watchdog is silent since 21:58Z. There are no ghost jobs to close.
- **Learned:** nothing new.
- **Next:** unchanged. Service ports first if the Mini reappears; uptime and pmset first for the Air; the daily diary at ~21:27Z.

## 2026-11-10 19:00 UTC

- **Decided:** hold the freeze (/root/work/.air-freeze is present; the Mini coming back doesn't lift it). My four agents are idle and none crashed; no cards handed out. A new session, **work-77**, has appeared (busy, started about 19:00Z), probably started by the user. I sent it a read-only FYI (the freeze, the Mini's state) instead of assuming what it's doing.
- **Shipped:** nothing this hour; the last commit is d7fab98 (the 18:00 diary).
- **Broke or learned:**
  - **The Mini is back:** booted about 19:10Z (up 7 min at 19:17Z), direct from the home IP, with a console login and Chrome running, so someone is at it.
  - **Its network has changed:** en0 is 192.168.2.1 (it looks like it's serving a 192.168.2.x network) and en1 is 192.168.1.37. It's no longer at 192.168.0.50, so anything hard-coded to .0.50 (the kanban URL, the pinpin-mini-lan alias, the Air's house-server address) will miss it. I won't fix that during the freeze or while the user is at the machine.
  - The house server on :57800 answers (its root returns `{"error":"not found"}`), but /headset/status gives 404 rather than the old JSON. That may just mean no headset has reported since the boot. Unconfirmed.
  - Post-boot load is high (5–8): mds_stores at 105%, apfsd at 68%, WallpaperVideoExtension at 36%. The same wave pattern as the Air's boots.
  - The Air is still dark ("last seen 20h ago"). There are no ghost jobs to close.
- **Next:** at 20:00, the HEARTBEAT path and whether the Air reappears. Leave the Mini alone while it's in use.

## 2026-11-10 20:00 UTC

- **Decided:**
  - Hold the freeze (/root/work/.air-freeze is present). My four agents are idle and none crashed; no cards handed out.
  - work-77 (the user's session) is reworking the Mini's network for the Ufanet line. It asked me not to touch the Mini's network config, and I acked. I'm keeping my Mini checks to tailscale status plus one HTTP probe: no ssh while they work.
- **Shipped:** nothing from my side; the last commit is d6ee4ed (the 19:00 diary).
- **What work-77 reported:**
  - The Mini's gateway role is gone: bootpd is disabled (it was rogue DHCP on the ZyXEL LAN), en0 is on DHCP (192.168.1.35), RT-admin 192.168.0.50 is disabled, forwarding is off, dummynet shaping is removed, and the pf anchor is now a WAN guard on en0.
  - Next, the Ufanet cable goes into the Mini's en0, then house Wi-Fi.
  - Every 192.168.0.50 target (TV, Moonlight, the kanban URL, pinpin-mini-lan) is stale until the new layout settles. work-77 has recorded the details in the home-network-routers memory.
- **Status:** the Mini is direct from home (136.169.211.98), and :57800 answers. The Air is still dark ("last seen 21h ago"). The watchdog is silent, and there are no ghost jobs to close.
- **Learned:** a second session working on shared hardware is best handled with one FYI and one ack, then hands off. The freeze flag still holds for my agents.
- **Next:** after the network settles and the freeze lifts, a card to re-point every 192.168.0.x target (c4: room.sh/HEARTBEAT; ad: apps/kanban URLs; 34: bench sim-on Mini). The daily diary is at ~21:27Z.

## 2026-11-10 21:00 UTC

- **Decided:** hold the freeze (/root/work/.air-freeze is present). My four agents are idle and none crashed; no cards handed out. work-77 is idle now and has sent nothing new. I'm still hands off the Mini's network (tailscale status and one HTTP probe only).
- **Shipped:** nothing this hour; the last commit is bd1fbad (the 20:00 diary).
- **Broke or learned:**
  - The Mini is stable since the rework: direct from home and active, with :57800 answering. Its post-boot state isn't checked, by choice: no ssh while the user's session owns its network.
  - The Air is still dark ("last seen 22h ago", since 11-09 about 22:38Z). The watchdog is silent, and there are no ghost jobs to close.
- **Learned:** nothing new.
- **Next:** the daily diary at ~21:27Z. After the freeze lifts: re-point the 192.168.0.x targets, the colorsync kickstart, air_smoke, then the queue.

# Day 2026-11-10

**Arc.** The fifth day of the demo freeze (/root/work/.air-freeze). Nothing shipped in quest-wall-screen except the 22 hourly diary commits (8709225 through b49d117); the kanban is still at rev 105 (11-06) and LOG.md hasn't changed since 11-04.
- **00:00–13:00Z:** the Air was dark all day (it went flat on 11-09 around 22:38Z while away from home) and the Mini had been dark since 11-07.
- **About 13:00–13:40Z:** the Mini showed up on tailscale via DERP only (pings answered), while ssh and :57800 never answered, then dropped again.
- **About 19:10Z:** the Mini booted for real with someone at the console. A new session, work-77 (the user's), was reworking its network for the Ufanet line.
- **work-77's changes:** Mini bootpd disabled (it was rogue DHCP on the ZyXEL LAN), en0 on DHCP (192.168.1.35), RT-admin 192.168.0.50 disabled, forwarding and dummynet shaping off, pf anchor turned into a WAN guard. Next, the Ufanet cable goes into the Mini's en0, then house Wi-Fi.
- **Uplink:** fine all day. The probe macbook-air-anastasiia was direct from the home IP 136.169.211.98, so the Mini's outage was local.

**Decisions and why.**
- Kept my four agents idle. Every open card touches the Air, the Mini or the headset, and the freeze still holds; the Mini returning doesn't lift it.
- Told the user only on change: Mini half-back (13:00), Mini gone again (14:00), Mini back with a new LAN (19:00). Hourly "nothing new" lines otherwise.
- When work-77 appeared, I sent one FYI (freeze, Mini state). After its request I acked and went hands off the Mini's network: checks cut to tailscale status plus one HTTP probe, no ssh. I passed two read-only observations: en0 and en1 differed from its plan at 19:17Z, and the Air's old 192.168.1.35 lease could conflict.

**Mistakes and lessons** (fed into captain-lessons.md):
- At 13:00 the Mini was "active" with traffic, but no service answered. The node's traffic counters can be exit-node traffic through this VPS, so they don't prove its services are up. Test the service ports, not the tailscale state. I did that right; it's worth writing down.
- At 19:00 I assumed nothing about work-77 and asked instead. The right move with a second session on shared hardware: one FYI, honour its "don't touch", hand off. No mistakes of my own today worth a correction entry.

**State per agent** (all idle, none crashed; work-ff untouched):
- **work-c4 (headset):** v0.76 is unverified since 11-07. HEARTBEAT (/headset/status) gives 404 on the rebooted Mini; maybe no headset since boot. K9 and K17 are open.
- **work-34 / work-09 (bench, geometry, Host):** K30 and K31b are built and not installed; K32 is in the backlog; the E and F reruns are pending; the stale bench lock from 11-06 18:15:37 is still there.
- **work-42 (assistant, perf):** K15 phase 2 DRY waits. The nightly-eval freeze skip held for a third night (01:17Z).
- **work-ad (apps, ops, perf):** air_smoke (sustained idle gate plus a battery check) and the flagged colorsync_reset at boot are queued. The apps and kanban URLs point at the stale 192.168.0.50.
- **work-77 (the user's network session):** owns the Mini's network until it says done.

**Tomorrow:**
1. Wait for work-77 to say the network is settled.
2. When the Air returns: uptime and pmset first (a fresh boot is expected).
3. When the user lifts the freeze: a re-point card for every 192.168.0.x target (c4: room.sh/HEARTBEAT, ad: apps/kanban URLs, 34: bench sim-on Mini), then the colorsync kickstart, air_smoke, the K30+K31b install, the E and F reruns, K15 phase 2, and clearing the stale lock.
4. Open questions for the user: the big-wall screen, screens 2 and 3, and UPnP (likely moot on the new line).

## 2026-11-10 22:00 UTC

- **Decided:** hold the freeze (/root/work/.air-freeze is present). My four agents are idle and none crashed; no cards handed out. work-77 is idle with nothing new from it. Still hands off the Mini's network.
- **Shipped:** b47cc7b (the 11-10 daily diary). In memory, captain-lessons gained a 11-10 section: "active" isn't the same as services being up, and one FYI then hands off when a peer session owns shared hardware.
- **Broke or learned:**
  - The Mini is stable (direct from home, :57800 answering) for about 3 h since the rework.
  - The Air is still dark ("last seen 23h ago").
  - The watchdog is silent, and there are no ghost jobs to close.
- **Learned:** nothing new.
- **Next:** unchanged. Wait for work-77's "network settled", then uptime and pmset when the Air returns, then the post-freeze queue starting with the 192.168.0.x re-point.

## 2026-11-10 23:00 UTC

- **Decided:** hold the freeze (/root/work/.air-freeze is present). My four agents are idle and none crashed; no cards handed out. work-77 is idle. Still hands off the Mini's network.
- **Shipped:** nothing this hour; the last commit is 36b48bb (the 22:00 diary).
- **Broke or learned:**
  - **The home public IP has changed,** from 136.169.211.98 to 95.105.86.155. The Mini and the probe macbook-air-anastasiia both show it, so it's the house's uplink that changed, not one device. It fits work-77's plan (the Ufanet cable into the Mini's en0, or a re-lease), but I haven't confirmed which, and it's theirs to say.
  - Both machines reconnected direct within the hour, and the Mini's :57800 answers.
  - The Air is still dark ("last seen 1d ago"). The watchdog is silent, and there are no ghost jobs to close.
- **Learned:** anything keyed on the home's public IP (allowlists, the Air's "is it home" test I used on 11-09) needs updating. That's for the re-point card, after work-77 says the network is settled.
- **Next:** unchanged. Wait for work-77, then the Air's uptime and pmset when it returns, then the post-freeze queue.

## 2026-11-11 00:00 UTC

- **Decided:** hold the freeze (/root/work/.air-freeze is present). My four agents are idle and none crashed; no cards handed out. work-77 is idle with nothing new. Still hands off the Mini's network.
- **Shipped:** nothing this hour; the last commit is 2018b8c (the 23:00 diary).
- **Broke or learned:**
  - The new home IP 95.105.86.155 is holding: the Mini and the probe are both direct on it, and :57800 answers.
  - The Air is still dark ("last seen 1d ago").
  - The watchdog is silent, and there are no ghost jobs to close.
- **Learned:** nothing new.
- **Next:** check the nightly-eval freeze skip at 01:17Z. Otherwise unchanged: work-77's "settled", the Air's uptime and pmset, then the post-freeze queue.

## 2026-11-11 01:00 UTC

- **Decided:** hold the freeze (/root/work/.air-freeze is present). My four agents are idle and none crashed; no cards handed out. work-77 is idle. Still hands off the Mini's network.
- **Shipped:** nothing this hour; the last commit is df73a3f (the 00:00 diary).
- **Verified:** the nightly-eval cron skipped on the freeze flag at 01:17:02Z ("NOTE SKIP: air-freeze present"). That's the fourth night running.
- **Broke or learned:**
  - The Mini and the probe are stable and direct on 95.105.86.155, and :57800 answers.
  - The Air is still dark ("last seen 1d ago", since 11-09 about 22:38Z).
  - The watchdog is silent, and there are no ghost jobs to close.
- **Next:** unchanged. work-77's "settled", the Air's uptime and pmset, then the post-freeze queue.
