# Headset never sleeps (while on the charger)

The user's blocker: off-head, the Quest drops off Wi-Fi adb within minutes, so agents wait for wake windows.

## What sleeps it

- **Android stay_on** (`stay_on_while_plugged_in=7`, 2026-11-03): does not hold.
- **The Quest's power manager** (oculus-system-services, `vrpowermanager`): puts the device to sleep 15 s after
  unmount (AutoSleepTime). Raising `autosleep_time` to 14400 s (`oculuspreferences --setc`) did not hold either:
  the 2026-11-03 watch shows `mWakefulness=Asleep`, VR state STANDBY, and adb lost at 15:07 and 15:11 UTC.
- **Conclusion:** the SoC suspends. While it is suspended, adbd and the Wi-Fi link stop answering.
- `unmounted_wake` (the power manager's own wake lock) cannot be taken from adb.

## The fix (`bench/headset_awake.py nosleep`, 2026-11-04)

Root is available (`/debug_ramdisk/su`). The fix has four parts:

1. **Kernel wake lock while powered.** `/data/local/tmp/pinpin-nosleep.sh` runs under su.
   - Every 30 s it checks `/sys/class/power_supply/*/online`.
   - It holds `/sys/power/wake_lock` = `pinpin` only while the headset is powered, and releases it off the charger, so the
     battery is never drained.
   - It logs to `/data/local/tmp/pinpin-nosleep.log`.
   - It runs while `/data/local/tmp/pinpin-nosleep.run` exists.
2. **Doze off and our app whitelisted:** `dumpsys deviceidle disable all`, `dumpsys deviceidle whitelist +com.pinpin.wallscreen.roomlab`.
3. **Wi-Fi power save off:** `iw dev wlan0 set power_save off`, `cmd wifi force-hi-perf-mode enabled`.
4. **The display is never touched.** There is no simulated wear (prox_close) and no screen-on. The display still follows
   the mount sensor, so there is no burn-in and no light at night.

**Without root.** Magisk root is lost on every reboot, and nobody re-roots without the user's say-so.
- Without root, only parts 2 and 3 apply (doze and Wi-Fi). The kernel wake lock, `iw`, and the guard need root.
- The watch logs `"root": false, "note": "no root, wake lock unavailable"`.
- The guard is restarted after a reboot only if root is back.
- Whether the non-root parts alone keep adb up is exactly what the watch log shows.

The watch logs once a minute: adb reachable, powered, battery level, wakefulness, VR state, whether the wake lock is held
and whether the guard is running. It goes to `bench/results/headset-awake-<date>.jsonl`. After a reboot the guard is gone; the next watch
round restarts it only if root is back.

## Undo

`python3 bench/headset_awake.py restore-nosleep`. This removes the run file, so the guard exits and drops the lock. It also
re-enables doze, removes the whitelist entry, and restores Wi-Fi power save from
`bench/results/headset-nosleep-originals.json`. That file is recorded at the first apply and is never overwritten.

## Status

- Applied: 2026-11-04, overnight watch (5 h), while the user slept.
- Result: see the jsonl and the END line in bench/activity.log.
