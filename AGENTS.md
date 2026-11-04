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
- Commit as "Mr. PinPin" <miguelemosreverte@gmail.com>; `git pull --rebase --autostash` before pushing.
