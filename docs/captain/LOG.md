# Captain log (UTC)

- 01:03, 01:26 work-09 session OOM-killed (wallshape test 3.2 GB); resumed as work-34; tests now under ulimit+timeout
- 01:36 headset_owner=agents (user asleep); work-ad (mini-apps) started; work-42 assigned assistant backend
- 01:43 v0.46 installed (SHOULDER+WRIST on); DEPTHGAP: screen 2 hangs over a recess (38 cm gap), main on the wall
- 01:50 check: c4, 34, 42, ad all busy; closed ghost headset_hold[1858753]; NOSLEEP waits for a wake window (headset asleep)
- 02:00 work-34 campaign GO (sender flags only auto-flip after smoke); work-42 e2e tonight approved on display 90 (≤3 turns, public pages); displays rebuild deferred to morning (TCC)
- 02:10 rebase collision in the shared checkout resolved by c4/42; suggest per-agent worktrees
- 02:20 c4: v0.47 built (WALLTOOL, ASSIST UI, SCROLL; flags off); NOSLEEP active since 01:50 with root; v0.47 session GO
- 02:10 check: all four active. User woke briefly: screens misplaced = Meta relocalised in the dark (lights on fixed it); perf flags were back on in settings.json, now off. New user rule: deploy/restart any time unless the user says not to disturb
- 02:30 check: all four active. v0.47 installed (P0 fix, ASSIST on); campaign block A running; apps runner test pass; Safari cleanup put on the morning list
- 02:50 check: c4 (v0.49 chat UX installing), 34 (campaign A, benchlock races fixed, roommesh.h done), 42 (M8 app-building e2e) busy; ad idle -> app picker + 3 apps
- 03:10 check: all four busy. v0.52 + GEOMETRY installed (03:10) for the user to compare scans; work-34 on a plane-regularized mesh + live raw depth; work-42 reproducing the screen-4 click bug (not TCC); watchdog flags the NOSLEEP 5 h watch as hung (expected duration, harmless)
- 03:30 check: all four busy. v0.53 measured on the charger: 72 fps with the mesh, never-disappear path live, STAGE rebase caught a 3.7 m jump. CDP web input live (ad), assistant clicks verified (42), latency p50 3.2 s first word (warm pool in progress). Campaign block A continues
- 03:50 check: all four busy. 42: house tools + room assistant + Mini plugin staged, now an eval suite + safety + YouTube play; ad: status page /status, guide /help, cursor watcher, now cooking mode; c4: v0.55 charger session; 34: campaign block A
- 04:10 check: all busy. Air load ~5: colorsync.displayservices+colorsyncd pegged ~100% since ~02:04Z (a killall respawned them pegged again), WallpaperVideoExtension ~30%; Spotify calmer; our scratch displays gone (42 quiet). Campaign waits for load<4. v0.57 installed. Assistant eval 16/18
- 04:30 check: all four busy (running commands). The campaign retries while the work-42 eval holds the lock; Air load ~5-7 (Chrome eval, colorsync, and the user's own gaucho tier1 login item spiking). Nothing stuck
