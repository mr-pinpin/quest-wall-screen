# Runbook: move the strong router's 5 GHz radio to channel 157 at 80 MHz

Status: **ready, not approved.** Apply only after the user approves (relayed by work-df), and only by the router
session (work-42). Prepared by work-09 on 2026-11-03.

## Why

The router (Electra F30AX, 192.168.0.1) runs its 5 GHz radio, which carries `RT-5WiFi-EDA0` and the extra SSID
`PinPin-Quest-Offline`, on channel 36 at **160 MHz**. That width spans channels 36-64 and overlaps four
neighbours, as scanned from the Air on 2026-11-03:

| Neighbour | Channel | Width | Signal at the Air |
|---|---|---|---|
| RT-5WiFi-E682 | 36 | 40 MHz | -71 dBm |
| RT-5WiFi-365F | 40 | 80 MHz | -57 dBm |
| Neighbor5G | 44 | 80 MHz | -79 dBm |
| TP-Link_794E_5G | 48 | 80 MHz | -63 dBm |

The upper band is nearly empty: only DIR-842-5G is there, on channel 149 at 80 MHz and -76 dBm.

**Symptoms.**

- Pings from the Air to the Mini show p95 90-230 ms and p99 200-340 ms, in runs of 40-110 ms lasting seconds.
- The headset's Wi-Fi transmissions retry 56% of the time (work-c4).

**Ruled out.**

- AWDL: awdl0 carried 0-2 packets/s during the spikes, and the log showed no channel-switch events.
- Power save: the spikes remain with 2 Mbit/s of steady traffic.
- Our own processes: the spikes appear even when no bench run holds the lock.

**Target.**

Channel 157 at 80 MHz (5735-5815 MHz). It is not a DFS channel, it is allowed in Russia (the router reports
country RU), and nothing nearby sits on 153-161. What we lose is the 160 MHz peak rate, which nothing of ours
needs: the game stream is at most 80 Mbit/s and the extra screens at most 40.

## Before: record the current state

1. **Log in.** Use the admin web UI's login (`/admin/login.asp`, `formLogin`). In bench scripts that is the
   `rtm.py` helper.
2. **Save the 5 GHz basic settings page as HTML.** Open `/admin/wlbasic.asp` with `wlan_idx=0` (5 GHz;
   `wlan_idx=1` is 2.4 GHz). Go there via `/boaform/admin/formWlanRedirect?redirect-url=/admin/wlbasic.asp&wlan_idx=0`
   and save the page as `router-wlbasic-5g-before-<UTC>.html`. Write down:
   - band and mode (802.11ax)
   - channel number and auto/manual
   - channel width (`chanwid` or similar)
   - control sideband
   - DFS enable
   - TX power
   - country
3. **Save the extra-SSID page.** Save `/admin/wlmultipleap.asp?wlan_idx=0` the same way, with its SSIDs, AP1
   enabled, and isolation off.
4. **Measure the baseline**, one minute, read-only, on the Air:
   ```
   ping --apple-time -i 0.2 -c 300 192.168.0.50   # Air -> Mini
   ```
   Note p50, p95, p99 and max.
5. **Note who is on 5 GHz now:**
   - the Air: `system_profiler SPAirPortDataType` shows channel 36
   - the headset: `dumpsys wifi | grep -m3 -E "SSID|freq"` over adb from the Mini
   - the LG TV, if it is on 5 GHz: the router's client list
6. **Take the bench lock** on the Air (`~/.local/share/pinpin-bench.lock`). Log START in
   `/root/work/quest-wall-screen/bench/activity.log`. Every 5 GHz client drops for a few seconds.

## Change

1. On the `wlbasic.asp` (`wlan_idx=0`) form, set **channel 157, manual** and **width 80 MHz**. Leave everything
   else exactly as recorded.
2. Post to `/boaform/admin/formWlanSetup`. Build the post from the page's own form fields, with the csrftoken read
   from the page, and change only the channel and width. Do not hand-write the field list: the firmware rejects or
   resets fields it doesn't get back.
3. Wait about 60 s for the radio to restart and the clients to rejoin.

## After: check

1. **The new channel is live.** On the Air, `system_profiler SPAirPortDataType` should show `PinPin-Quest-Offline`
   and `RT-5WiFi-EDA0` at Channel 157 (5GHz, 80MHz).
2. **The Air** is back on its SSID at 192.168.0.102 (`ipconfig getifaddr en0`), and the Mini reaches it:
   `nc -z 192.168.0.102 57720` (atlas) and `57989` (Sunshine).
3. **The headset**, if awake: `adb connect 127.0.0.1:26351` through the Mini works, and `dumpsys wifi` shows 5785
   MHz (centre 5775). It is asleep most of the time; then check at its next wake window (headset_net.py logs it).
4. **The Mini and the TV.** The Mini is wired, so it is unaffected. The TV still shows the Atlas.
5. **Re-measure** the same Air -> Mini ping for one minute. Expected: p95 and p99 well down; the spikes were
   contention, so they should mostly disappear.
6. **Release the lock** and log END. Then rerun the suite's laptopPingMs-sensitive runs, e.g. cinema-10 and the
   gaming pairs.

## Rollback

Re-post the `wlbasic.asp` form with the recorded channel (36) and width (160 MHz), built from the page's form
fields as above. Then rerun checks 1-3.

- **Clients don't come back within 2 minutes:** roll back at once.
- **The UI is unreachable:** the router is on the Mini's LAN at 192.168.0.1, wired. If it is still unreachable,
  power-cycle it. The setting persists, so roll back from the UI after it boots.

## Not part of this change

- **The ZyXEL** (192.168.1.1, it holds the Rostelecom PPPoE): do not touch.
- **2.4 GHz** (`wlan_idx=1`): unchanged.
- **DNS and Meta routing:** decided against (docs/PERFORMANCE.md, "Decided, do not re-propose").

## Related, Sunshine's "user is active" assertion (noted, nothing changed)

`pmset -g assertions` on the Air shows `UserIsActive` named
`com.apple.iohideventsystem.queue.tickle.nxevent ... process:sunshine-bin`.

- **Where it comes from:** WindowServer raises it whenever a process posts a HID (NX) event. Sunshine posts input
  when a Moonlight client sends mouse or keyboard input, and also once at startup; each Sunshine restart resets
  the Air's HIDIdleTime. Bench runs with `--sunshine` overrides restart Sunshine.
- **Settings:** Sunshine has no setting for this beyond `mouse`, `keyboard` and `controller`
  (`enabled`/`disabled`). Turning those off would also block the player's input in a game, so it is not a fix.
- **Consequence:** never judge "someone is using the laptop" from HIDIdleTime or UserIsActive alone. Rule out
  sunshine-bin first: `pmset -g assertions | grep tickle` names the process.
