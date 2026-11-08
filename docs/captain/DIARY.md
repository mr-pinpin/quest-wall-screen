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
