# PRD: the headset never sleeps (P0)

**Problem.** When the Quest sleeps, its Wi-Fi goes off and adb is unreachable. Agents then wait for the user to wake
it. The user called this a blocker (2026-11-04).

**Goal.** On the charger, the Quest stays reachable over Wi-Fi/adb indefinitely, with no simulated wear and the
display off.

## Requirements
- [ ] Find what actually sleeps it: vrpowermanager AutoSleepTime, doze/deviceidle, Wi-Fi power save, app standby
- [ ] Persistent across reboots where possible; re-applied automatically after a headset reboot
- [ ] Reversible: originals recorded (bench/results/headset-awake-originals.json)
- [ ] Never prox_close off the charger; the display does not stay lit when unworn
- [ ] An overnight watch log, adb reachable every minute for ≥4 h, is the pass criterion
- [ ] Documented in docs/ and in the quest-room-lab memory

Off the charger, normal sleep is fine and expected.
