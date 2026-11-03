# Working in this repo (agents and humans)

- **Before deploying anything** to the laptop (atlas sender, PinPin Host plugins) or the headset (room_lab APK),
  run `bash bench/run_offline_tests.sh` on the VPS and deploy only if it ends with "all offline tests passed".
  It covers the budget arbiter, the headset VT logic against every recorded sender layout, the layout checker
  (recorded + synthetic packer model), the gate self-test and the bench scripts. Add new hardware-free tests there.
- **Every deploy to the Air ends with `bash bench/air_smoke.sh`** inside the same bench-lock slot (a helper display
  with the stress pattern must capture at >= 25 fps); roll back if it fails. The Air's display must be awake for
  capture: asleep, virtual displays drop out of ScreenCaptureKit (stress.py and air_smoke.sh hold `caffeinate -u`).
- **Laptop tests take the shared bench lock** (`~/.local/share/pinpin-bench.lock` on pinpin-air, see
  bench/stress.py) so two sessions never test at once.
- **Headset:** never ask the user to wear it for a test; `bench/headset_hold.py` holds it awake unworn and lets go
  the moment the real sensor sees a wearer. Every headset change ships as a settings flag, off until proven.
- **No rm on the laptop** (it prompts the user): move things aside, `launchctl bootout`, or `"enabled": false`.
- **PinPin Host:** never change its bundle id or signing identity (every permission would need new clicks).
- Commit as "Mr. PinPin" <miguelemosreverte@gmail.com>; `git pull --rebase --autostash` before pushing.
