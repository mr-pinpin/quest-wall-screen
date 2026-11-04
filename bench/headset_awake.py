#!/usr/bin/env python3
"""Keep the Quest awake and reachable while it is on the charger, so tests never wait for a wake window.
A persistent, reversible headset setting (headset flags rule): `apply` records the original values first,
`restore` puts them back.

    python3 bench/headset_awake.py apply   [--watch-min 35]   # wait for adb, probe, record, apply, then watch
    python3 bench/headset_awake.py restore                    # wait for adb, put the recorded values back
    python3 bench/headset_awake.py probe                      # wait for adb, print every sleep-related setting
    python3 bench/headset_awake.py unmounted [--watch-min 40] # Quest-level: hold the power manager's unmounted wake lock
    python3 bench/headset_awake.py release                    # drop that wake lock again
    python3 bench/headset_awake.py autosleep [--seconds 14400] [--watch-min 40]  # the Quest auto-sleep delay
    python3 bench/headset_awake.py restore-autosleep          # put the recorded delay back
    python3 bench/headset_awake.py nosleep [--watch-min 300]  # the fix that holds (2026-11-04): see docs/headset-never-sleep.md
    python3 bench/headset_awake.py restore-nosleep            # undo nosleep, originals from bench/results/headset-nosleep-originals.json

The working knob (2026-11-03): `oculuspreferences --getc/--setc autosleep_time` (seconds; was 15), read by the
power manager as AutoSleepTime. unmounted_wake does not take from adb (lock count stays 0; system callers only).

Android's stay_on (apply) does NOT hold on the Quest: its own power manager (oculus-system-services.jar,
service vrpowermanager) sleeps 15 s after unmount (AutoSleepTime). Its controls, read from the jar 2026-11-03:
broadcasts com.oculus.vrpowermanager.unmounted_wake / release_unmounted_wake (UnmountedWakeLock count in
dumpsys), the autosleep_time setting (mAutosleepTimeObserver, AUTOSLEEP_OFF_VALUE) and a disable_autosleep extra.
`unmounted` uses the wake lock: awake off-head with the display off (the display follows the mount state).

What `apply` sets (only "awake while charging"; nothing that lights the display unworn or off the charger):
  settings global stay_on_while_plugged_in = 7   Android: stay awake on AC/USB/wireless power
  settings global wifi_sleep_policy        = 2   Android: never turn Wi-Fi off while asleep
No simulated wear (prox_close) here: that keeps the display lit, which off-head means battery drain and burn-in.
The watch logs, once a minute, whether adb answers, the power/display state and charging, to
bench/results/headset-awake-<date>.jsonl; on-charger reachability for 30+ minutes is the pass.
Originals: bench/results/headset-awake-originals.json (and /data/local/tmp/pinpin-awake-orig.json on the headset).
"""
import datetime, json, pathlib, re, subprocess, sys, time
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import stress

RES = pathlib.Path(__file__).resolve().parent / 'results'
ORIG = RES / 'headset-awake-originals.json'
SETTINGS = [('global', 'stay_on_while_plugged_in', '7'), ('global', 'wifi_sleep_policy', '2')]


def log(s): print(time.strftime('%H:%M:%S'), s, flush=True)


def sh(cmd, t=40):
    try: return stress.headset(cmd, timeout=t)
    except subprocess.TimeoutExpired: return ''


def reachable():
    try:
        return stress.mini(f"{stress.ADB.split(' -s ')[0]} connect 127.0.0.1:26351 >/dev/null 2>&1; {stress.ADB} get-state 2>/dev/null", timeout=40).strip() == 'device' \
            and sh('echo ok', 20).strip() == 'ok'
    except subprocess.TimeoutExpired:
        return False


def wait_adb(hours=8):
    end = time.time() + hours * 3600
    while time.time() < end:
        if reachable(): return True
        time.sleep(30)
    return False


def probe():
    return sh("for ns in global secure system; do settings list $ns | grep -iE 'sleep|stay_on|timeout|screen_off|doze|wifi_.*(idle|sleep)|power' | sed \"s/^/$ns: /\"; done; "
              "getprop | grep -iE 'sleep|proximity|power|stay' | head -30; dumpsys battery | grep -E 'powered|level|status'; "
              "dumpsys power | grep -E 'mWakefulness=|mStayOn|Display Power|mHoldingWakeLockSuspendBlocker' | head -8; "
              "dumpsys vrpowermanager | grep -m1 '^State:'; echo '--- vrpowermanager (full)'; dumpsys vrpowermanager | head -120; "
              "echo '--- autosleep mentions'; dumpsys vrpowermanager | grep -iE 'autosleep|sleep.?time|timeout|idle' | head -30; "
              "echo '--- receivers'; dumpsys package | grep -oE 'com\\.oculus\\.vrpowermanager\\.[A-Za-z_.]+' | sort -u | head -60; "
              "echo '--- oculus settings'; for ns in global secure system; do settings list $ns | grep -iE 'oculus|vr_|sleep|proximity' | sed \"s/^/$ns: /\"; done", 90)


def state():
    out = sh("dumpsys battery | grep -E 'AC powered|USB powered|Wireless powered|level'; dumpsys power | grep -E 'mWakefulness=|Display Power: state'; "
             "dumpsys vrpowermanager | grep -m1 '^State:'", 40)
    powered = any(re.search(rf'{k} powered: true', out) for k in ('AC', 'USB', 'Wireless'))
    lvl = re.search(r'level: (\d+)', out)
    return {'powered': powered, 'level': int(lvl.group(1)) if lvl else None,
            'wake': (re.search(r'mWakefulness=(\w+)', out) or [None, None])[1],
            'display': (re.search(r'Display Power: state=(\w+)', out) or [None, None])[1],
            'vr': (re.search(r'State: (\w+)', out) or [None, None])[1]}


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else 'probe'
    watch = float(sys.argv[sys.argv.index('--watch-min') + 1]) if '--watch-min' in sys.argv else 35
    log(f'{mode}: waiting for adb'); 
    if not wait_adb(): log('headset never reachable'); sys.exit(2)
    log('reachable')
    if mode == 'probe':
        print(probe()); return
    if mode in ('autosleep', 'restore-autosleep'):
        OP = "/debug_ramdisk/su -c 'oculuspreferences {}'"
        origf = RES / 'headset-autosleep-original.txt'
        if mode == 'restore-autosleep':
            v = origf.read_text().strip() if origf.exists() else '15'
            log('restore: ' + sh(OP.format(f'--setc autosleep_time {v}')).strip() + ' | ' + sh("dumpsys vrpowermanager | grep -m1 AutoSleepTime").strip()); return
        secs = sys.argv[sys.argv.index('--seconds') + 1] if '--seconds' in sys.argv else '14400'
        cur = re.search(r'autosleep_time\s*:\s*(-?\d+)', sh(OP.format('--getc autosleep_time')))
        if cur and not origf.exists(): RES.mkdir(exist_ok=True); origf.write_text(cur.group(1))
        log(f'original autosleep_time: {origf.read_text().strip() if origf.exists() else "?"} s; setting {secs} s')
        log('set: ' + sh(OP.format(f'--setc autosleep_time {secs}')).strip())
        time.sleep(2)
        log('power manager now: ' + sh("dumpsys vrpowermanager | head -4").strip().replace('\n', ' | '))
        stress.activity(f'START headset autosleep_time {secs} s (was {origf.read_text().strip() if origf.exists() else "?"}); restore: headset_awake.py restore-autosleep')
        out = RES / f'headset-awake-{datetime.date.today()}.jsonl'
        t0, up, down = time.time(), 0, 0
        while time.time() - t0 < watch * 60:
            ok = reachable()
            r = {'t': datetime.datetime.now().isoformat(timespec='seconds'), 'mode': f'autosleep {secs}', 'adb': ok, **(state() if ok else {})}
            up += ok; down += not ok
            with out.open('a') as f: f.write(json.dumps(r) + '\n')
            log(json.dumps(r)); time.sleep(60)
        log(f'watch over (autosleep {secs}): reachable {up} of {up + down} minutes')
        stress.activity(f'END headset autosleep watch: reachable {up} of {up + down} minutes (setting stays; restore-autosleep reverts)'); return
    if mode in ('nosleep', 'restore-nosleep'):
        # The Quest's power manager suspends the SoC off-head whatever autosleep_time says (2026-11-03 watch: "Asleep",
        # adb dropping every few minutes). A kernel wake lock stops the suspend itself. An on-headset root loop holds it
        # only while the headset is powered and drops it off the charger (never drain the battery); doze is disabled and
        # Wi-Fi power save turned off so the link stays up. Everything is recorded first and undone by restore-nosleep.
        SU = "/debug_ramdisk/su -c '{}'"
        origf = RES / 'headset-nosleep-originals.json'
        loop = ('#!/system/bin/sh\n'
                '# pinpin nosleep guard: hold a kernel wake lock while powered. Stop: rm /data/local/tmp/pinpin-nosleep.run\n'
                'L=/data/local/tmp/pinpin-nosleep.log; held=0\n'
                'while [ -f /data/local/tmp/pinpin-nosleep.run ]; do\n'
                '  p=0; for f in /sys/class/power_supply/*/online; do [ "$(cat $f 2>/dev/null)" = 1 ] && p=1; done\n'
                '  if [ $p = 1 ] && [ $held = 0 ]; then echo pinpin > /sys/power/wake_lock; held=1; echo "$(date -u +%FT%TZ) powered: wake lock held" >> $L; fi\n'
                '  if [ $p = 0 ] && [ $held = 1 ]; then echo pinpin > /sys/power/wake_unlock; held=0; echo "$(date -u +%FT%TZ) unpowered: wake lock released" >> $L; fi\n'
                '  sleep 30\n'
                'done\n'
                '[ $held = 1 ] && echo pinpin > /sys/power/wake_unlock; echo "$(date -u +%FT%TZ) guard stopped" >> $L\n')
        if mode == 'restore-nosleep':
            orig = json.loads(origf.read_text()) if origf.exists() else {}
            sh('rm -f /data/local/tmp/pinpin-nosleep.run'); time.sleep(1)
            sh(SU.format('echo pinpin > /sys/power/wake_unlock'))
            if orig.get('deviceidle', 'enabled') != 'disabled': sh('dumpsys deviceidle enable all')
            if not orig.get('whitelisted'): sh(f'dumpsys deviceidle whitelist -{stress.PKG}')
            if orig.get('power_save'): sh(SU.format(f"iw dev wlan0 set power_save {orig['power_save']}"))
            sh('cmd wifi force-hi-perf-mode disabled')
            log('restored nosleep: ' + json.dumps(orig) + ' | wake_lock now: ' + sh(SU.format('cat /sys/power/wake_lock')).strip())
            stress.activity('END headset nosleep (restored)'); return
        if not origf.exists():  # the first apply's values are the originals
            idle = sh('dumpsys deviceidle enabled').strip()
            orig = {'deviceidle': 'enabled' if '1' in idle.split() else 'disabled' if idle else '?', 'deviceidleRaw': idle,
                    'whitelisted': stress.PKG in sh('dumpsys deviceidle whitelist'),
                    'power_save': (re.search(r'Power save: (\w+)', sh(SU.format('iw dev wlan0 get power_save'))) or [None, ''])[1],
                    'wake_lock': sh(SU.format('cat /sys/power/wake_lock')).strip(), 'autosleep': sh(SU.format('oculuspreferences --getc autosleep_time')).strip()}
            RES.mkdir(exist_ok=True); origf.write_text(json.dumps(orig, indent=1))
        log('originals: ' + origf.read_text().replace('\n', ' '))
        import base64
        b64 = base64.b64encode(loop.encode()).decode()
        sh(f'echo {b64} | base64 -d > /data/local/tmp/pinpin-nosleep.sh && chmod 755 /data/local/tmp/pinpin-nosleep.sh && touch /data/local/tmp/pinpin-nosleep.run')
        if 'pinpin-nosleep.sh' not in sh('ps -A -o ARGS | grep pinpin-nosleep | grep -v grep'):
            sh(SU.format('nohup /data/local/tmp/pinpin-nosleep.sh >/dev/null 2>&1 &'))
        sh('dumpsys deviceidle disable all'); sh(f'dumpsys deviceidle whitelist +{stress.PKG}')
        sh(SU.format('iw dev wlan0 set power_save off')); r = sh('cmd wifi force-hi-perf-mode enabled').strip()
        time.sleep(3)
        log('applied: wake_lock=' + sh(SU.format('cat /sys/power/wake_lock')).strip() + ' | guard=' + sh('ps -A -o ARGS | grep pinpin-nosleep.sh | grep -v grep').strip()
            + ' | deviceidle=' + sh('dumpsys deviceidle enabled').strip().replace('\n', ' ') + ' | power_save=' + sh(SU.format('iw dev wlan0 get power_save')).strip()
            + ' | hi-perf: ' + (r or 'ok') + ' | ' + sh('tail -2 /data/local/tmp/pinpin-nosleep.log').strip().replace('\n', ' / '))
        stress.activity('START headset nosleep (kernel wake lock while powered, doze off, wifi power save off); restore: headset_awake.py restore-nosleep')
        out = RES / f'headset-awake-{datetime.date.today()}.jsonl'
        t0, up, down = time.time(), 0, 0
        while time.time() - t0 < watch * 60:
            ok = reachable()
            r = {'t': datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds'), 'mode': 'nosleep', 'adb': ok, **(state() if ok else {})}
            if ok:
                r['lock'] = 'pinpin' in sh(SU.format('cat /sys/power/wake_lock'))
                r['guard'] = 'pinpin-nosleep.sh' in sh('ps -A -o ARGS | grep pinpin-nosleep.sh | grep -v grep')
                if not r['guard'] and r.get('powered'):  # e.g. after a reboot: start it again
                    sh('touch /data/local/tmp/pinpin-nosleep.run'); sh(SU.format('nohup /data/local/tmp/pinpin-nosleep.sh >/dev/null 2>&1 &')); r['guardRestarted'] = True
            up += ok; down += not ok
            with out.open('a') as f: f.write(json.dumps(r) + '\n')
            log(json.dumps(r)); time.sleep(60)
        log(f'watch over (nosleep): reachable {up} of {up + down} minutes')
        stress.activity(f'END headset nosleep watch: reachable {up} of {up + down} minutes (nosleep stays; restore-nosleep reverts)'); return
    if mode == 'release':
        sh('am broadcast -a com.oculus.vrpowermanager.release_unmounted_wake >/dev/null')
        log('released: ' + sh("dumpsys vrpowermanager | grep -E 'UnmountedWakeLock|^State:'").strip().replace('\n', ' | ')); return
    if mode == 'unmounted':
        RES.mkdir(exist_ok=True)
        info = sh("for ns in global secure system; do echo \"$ns autosleep_time=$(settings get $ns autosleep_time)\"; done; "
                  "/debug_ramdisk/su -c 'oculuspreferences --getc autosleep_time' 2>&1 | head -3; dumpsys vrpowermanager | head -6", 60)
        log('before: ' + info.strip().replace('\n', ' | '))
        sh('am broadcast -a com.oculus.vrpowermanager.unmounted_wake >/dev/null'); time.sleep(3)
        log('after unmounted_wake: ' + sh("dumpsys vrpowermanager | head -6").strip().replace('\n', ' | '))
        out = RES / f'headset-awake-{datetime.date.today()}.jsonl'
        t0, up, down = time.time(), 0, 0
        while time.time() - t0 < watch * 60:
            ok = reachable()
            r = {'t': datetime.datetime.now().isoformat(timespec='seconds'), 'mode': 'unmounted_wake', 'adb': ok, **(state() if ok else {})}
            if ok: r['unmountedWake'] = sh("dumpsys vrpowermanager | grep -m1 UnmountedWakeLock").strip()
            up += ok; down += not ok
            with out.open('a') as f: f.write(json.dumps(r) + '\n')
            log(json.dumps(r)); time.sleep(60)
        log(f'watch over (unmounted_wake): reachable {up} of {up + down} minutes'); return
    if mode == 'restore':
        orig = json.loads(ORIG.read_text())
        for ns, key, _ in SETTINGS:
            v = orig.get(f'{ns}/{key}')
            sh(f'settings delete {ns} {key}' if v in (None, 'null', '') else f'settings put {ns} {key} {v}')
        log('restored: ' + json.dumps(orig)); return
    # apply
    RES.mkdir(exist_ok=True)
    (RES / f'headset-awake-probe-{datetime.date.today()}.txt').write_text(probe())
    if not ORIG.exists():  # the first apply's values are the originals; a re-apply must not overwrite them
        orig = {f'{ns}/{key}': sh(f'settings get {ns} {key}').strip() for ns, key, _ in SETTINGS}
        ORIG.write_text(json.dumps(orig, indent=1))
        sh(f"echo '{json.dumps(orig)}' > /data/local/tmp/pinpin-awake-orig.json")
    log('originals: ' + ORIG.read_text().replace('\n', ' '))
    for ns, key, val in SETTINGS: sh(f'settings put {ns} {key} {val}')
    log('applied: ' + ', '.join(f'{ns}/{key}={sh(f"settings get {ns} {key}").strip()}' for ns, key, _ in SETTINGS))
    out = RES / f'headset-awake-{datetime.date.today()}.jsonl'
    t0, up, down = time.time(), 0, 0
    while time.time() - t0 < watch * 60:
        ok = reachable()
        r = {'t': datetime.datetime.now().isoformat(timespec='seconds'), 'adb': ok, **(state() if ok else {})}
        up += ok; down += not ok
        with out.open('a') as f: f.write(json.dumps(r) + '\n')
        log(json.dumps(r))
        time.sleep(60)
    log(f'watch over: reachable {up} of {up + down} minutes')


if __name__ == '__main__':
    main()
