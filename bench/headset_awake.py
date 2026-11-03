#!/usr/bin/env python3
"""Keep the Quest awake and reachable while it is on the charger, so tests never wait for a wake window.
A persistent, reversible headset setting (headset flags rule): `apply` records the original values first,
`restore` puts them back.

    python3 bench/headset_awake.py apply   [--watch-min 35]   # wait for adb, probe, record, apply, then watch
    python3 bench/headset_awake.py restore                    # wait for adb, put the recorded values back
    python3 bench/headset_awake.py probe                      # wait for adb, print every sleep-related setting

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
              "dumpsys vrpowermanager | grep -m1 '^State:'", 60)


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
