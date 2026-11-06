# Working in this repo (agents and humans)

- **Before deploying anything** to the laptop (atlas sender, PinPin Host plugins) or the headset (room_lab APK),
  run `bash bench/run_offline_tests.sh` on the VPS and deploy only if it ends with "all offline tests passed".
  It covers the budget arbiter, the headset VT logic against every recorded sender layout, the layout checker
  (recorded + synthetic packer model), the gate self-test and the bench scripts. Add new hardware-free tests there.
- **Every deploy to the Air ends with `bash bench/air_smoke.sh`** while you still hold the bench lock (release it only after the smoke and any rollback; a smoke run after release can overlap another agent's test and fail falsely, 2026-11-03 15:27) (a helper display
  with the stress pattern must capture at >= 25 fps); roll back if it fails. The Air's display must be awake for
  capture: asleep, virtual displays drop out of ScreenCaptureKit (stress.py and air_smoke.sh hold `caffeinate -u`).
- **Laptop tests take the bench lock** (`bench/benchlock.py`; on pinpin-air under `~/.local/share/`), in one of two modes:
  EXCLUSIVE for benchmark measurement windows (stress.py runs, encprobe, deploy + air_smoke): nothing else on the Air;
  SHARED for functional tests that each use their own scratch display (apps runner, assistant end-to-end): any number at
  once, never during an exclusive hold. Writer priority: an exclusive request leaves an intent marker and waits for the
  shared holders to drain, and no new shared holder joins meanwhile, so benchmarks never starve. Wrap the test:
  `python3 bench/benchlock.py shared --who apps --max 600 -- CMD ...` (or `exclusive`); or `tok=$(python3
  bench/benchlock.py take shared --who apps --max 600)` ... `python3 bench/benchlock.py release "$tok"`;
  `python3 bench/benchlock.py status` shows who holds what. --max is your longest hold: a crashed holder expires then.
  The exclusive lock is still the `pinpin-bench.lock` directory, so an older script that takes it with mkdir still
  excludes everyone, but it does not wait for shared holders: use benchlock.py.
- **Headset:** never ask the user to wear it for a test; `bench/headset_hold.py` holds it awake unworn and lets go
  the moment the real sensor sees a wearer. Every headset change ships as a settings flag, off until proven.
- **No rm on the laptop** (it prompts the user): move things aside, `launchctl bootout`, or `"enabled": false`.
- **Never execute a third-party binary to probe it** (`--version`, `--help` included): read its strings or source, or its
  logs. On 2026-11-04, `sunshine --version` on the Mini booted a second Sunshine with the default config and truncated
  the live sunshine.log.
- **Scratch virtual displays come from a FIXED POOL: created once, NEVER removed** (K24, 2026-11-04). Every virtual
  display created and then removed raises macOS colorsync's display-info polling for good (+11–18 requests/min per
  cycle; each request rebuilds every display's profile): colorsync crept to a full core over hours of test churn and
  failed air_smoke, and only a reboot reset it. So a test never calls `POST /remove`; it reuses its pool display and
  clears its content instead (close its windows, unassign its app). `POST /displays` always RECREATES (even at the same
  size), so call it only when `GET /displays` lacks the id at the size you need. One id per user, one fixed size each:

  | id | owner | size (points, scale) | used by |
  |---|---|---|---|
  | 47 | work-ad (apps) | 1920x1200 @1 | tools/air_test.sh, tools/input_test.sh |
  | 48 | everyone | 1920x1200 @1 | bench/air_smoke.sh |
  | 50 | work-c4 (headset) | 1920x1200 @1 | headset/runtime tests that need a spare Air screen |
  | 60 | work-42 (assistant) | 1600x1000 @2 | bench/eval_run.sh and verification runs (63 is retired) |
  | 51–58 | work-34 / work-09 (bench) | 1920x1200 @1 | stress.py --source sck test displays (POOL0 = 51) |
  | 59 | work-34 / work-09 (bench) | the cinema size, its own id (never a resize) | stress.py cinema (CINEMA_ID = 59) |

  pinpin-displays accepts screen ids 1–64 only (POST /displays answers 400 "id 1-64" above that): the pool lives inside it.
  Ids 2–39 are the house's real screens (house.json nextScreenId keeps growing), so tests never create in that range.
  A new need gets a new row here, not an ad-hoc id. /status shows colorsync's CPU and request rate: red above 40%.
- **PinPin Host:** never change its bundle id or signing identity (every permission would need new clicks).
- **pinpin-displays is a PinPin Host plugin, and the Host is its SOLE owner.** Never create or
  `launchctl bootstrap` the `io.mr-pinpin.displays` launchd agent, and never `open` the app: that
  adds a 2nd/3rd owner fighting over :57710, which crash-loops and churns the virtual displays
  (incident 2026-11-04 — three owners at once spiked the Air to load 246). Because it runs as a
  Host child, its Accessibility/Screen-Recording grant is charged to the Host's permanent identity,
  so **rebuilding it does NOT re-prompt** (the user's point: no further permissions, thanks to the
  plugin system). To ship a new pinpin-displays binary: build it (`mac/install-pinpin-displays.sh`)
  and let the Host restart the plugin — `POST http://127.0.0.1:57790/restart?name=<displays-plugin>`
  (clean, host backoff), or replace-the-file-then-pkill-the-child. Same pattern for any Host plugin
  (atlas, assistant): one owner, upgrade = replace binary + Host restart.
  The old agent plist is parked as `~/Library/LaunchAgents/io.mr-pinpin.displays.plist.disabled`
  (2026-11-06): a RunAtLoad+KeepAlive copy had started an orphan at boot (ppid 1) that outlived the
  2026-11-05 17:45Z WindowServer crash and kept listing 8 dead displays. Leave it disabled. After a
  WindowServer crash or re-login: `kill -TERM` any pinpin-displays whose ppid is not the Host, check
  exactly one remains with ppid = Host; it re-creates its saved set (displays.json) by itself.
- **Editing a plugin's JSON spec bounces that plugin within ~3 s** — the Host re-reads the plugins
  folder and restarts any plugin whose spec (e.g. an `env` var) changed. There is no "edit now, apply
  on next natural restart": changing the file IS the restart.
- **The Host adopts its plugins across its OWN restarts** (`build-host.sh` uses `launchctl kickstart -k`,
  SIGTERM to the Host only; the next Host re-adopts the running children via their pid files), so a
  plugin at **ppid=1 is normal during a Host rebuild**. NEVER key orphan detection on the parent pid —
  it would kill+restart every plugin on each rebuild and break adoption. The real orphan test is the
  **login session**: a process that started before the current WindowServer (which only restarts with a
  new session) is stale (`PINPIN_ORPHAN_EXIT` guard: pinpin-displays `dw_stale` K30, `parent_watch.py`).
- Commit as "Mr. PinPin" <miguelemosreverte@gmail.com>; `git pull --rebase --autostash` before pushing.
