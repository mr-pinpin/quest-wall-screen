# Captain's roadmap (work-df)

The coordinator's living checklist. Times are UTC. Owners: **c4** = work-c4 (headset app), **09** = work-09
(house file, geometry, benchmarks), **42** = work-42 (assistant backend), **apps** = the mini-apps agent.

## Standing rules
- `bench/headset_owner`: `agents` by default. It is `user` only while the user says they are using the headset,
  and then nothing is installed or restarted.
- New features ship behind flags, OFF by default. Every old feature is kept. Every setting gets a one-line explanation.
- One install lock: `stress.install_lock()` / `deploy.py`. Nothing touches the headset outside `stress.headset()`.
- No AskUserQuestion in workers; decisions go to work-df. Nobody idles.

## Done (2026-11-04)
- [x] v0.37 restored; diagnosis of screen 2 "in the wall" (scene plane vs real wall; to be confirmed by DEPTHGAP)
- [x] house.json v1 + Mini sync server :57800, git history, migration rev 1 (09)
- [x] wallfit.h depth wall fitting (09)
- [x] v0.45 SHOULDER cascading menu, installed for the user ("very intuitive")
- [x] one install lock + owner guard in stress.headset() (c4)
- [x] v0.46 built: HOUSE, DEPTHGAP, depth dump, MODELS default placement, WRIST radial (c4)

## Now
- [~] P0 headset never sleeps (active since 01:50, 5 h watch running) on the charger, proven by an overnight watch (c4) — PRD: prd/headset-awake.md
- [x] v0.46 session: install, DEPTHGAP reading on screen 2, depth dump, screenshots (c4)
- [x] wallshape.h line/circle/extrude state machine (09), then WALLTOOL wiring (c4)
- [x] flag registry flags.json (b9a70e9; benchmark campaign pending) with reasons and evidence, plus the benchmark campaign deciding the defaults (09) — PRD: prd/flags.md
- [ ] assistant backend, per-screen computer use (42) — PRD: prd/assistant.md
- [ ] mini-apps platform, apps repo, kitchen recipes and weather (apps) — PRD: prd/mini-apps.md
- [x] SCROLL global wave gesture (c4) — PRD: prd/assistant.md §Scroll
- [x] ASSIST button and chat bubble on every screen (c4, 2bd2e26; backend e2e pending)


## User feedback 2026-11-04 ~02:20 UTC
- [x] P0 placements never disappear (v0.47 e3f5487; headset regress deferred to day)
- [x] v0.47 installed with ASSIST on (02:26; not yet seen running)
- [ ] screens made from the menu get the move bar like the main one (c4)
- [ ] screens can be created on walls in rooms we scanned (multi-room placement) (c4)
- [ ] close button on unlocked screens with a double-press confirm (c4)
- [ ] progressive tessellated scan mesh like Meta (TSDF + marching cubes) (34 core, c4 render)
- [ ] GEOMETRY flag to show the room mesh and walls (c4 + 34)
- [ ] assistant: per-screen Chrome window lands on its display; verify-after-act (42 + ad)

## Next
- [ ] SCAN on demand: own persisted anchors, sweep progress cue, commit to house.json (c4 + 09)
- [ ] Multi-room: room links, relocalisation across rooms
- [ ] DEPTHFIT fallback only if DEPTHGAP shows it is needed
- [ ] Hand checks with the user: ring scaling, snap drop, MINI click
- [ ] Re-baseline network on ch36, the 400 ms VT gap

## Later (user will say when)
- [ ] Capture: passthrough camera texturing / splats on the Mini, night navigation
- [ ] Cinema mode
- [ ] Mr. PinPin House game

## Morning list for the user (needs a click or a decision)
- [ ] Accessibility re-grant on the Air after the PinPin Displays rebuild (scroll on extra screens), batched with other TCC prompts (42)
- [ ] Permanent root on the headset? (the never-sleep wake lock needs root; Magisk is lost on reboot)
- [ ] Move screen 2 to a solid wall (DEPTHGAP: hangs over a 38 cm recess)
- [ ] Hand checks: ring scaling, snap drop, MINI click
- [ ] Re-authenticate claude on the Mini (OAuth expired; the Air login works) (42)
- [ ] Close 2 stray Safari windows on the Air main display (example.com, YouTube search) from the first assistant test
