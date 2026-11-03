#!/usr/bin/env python3
"""User policy (2026-11-03, approved via work-df): simulated wear only while the headset is on the charger (AC, USB
or wireless power), only for the duration of queued runs (released right after the last one), never while a person
really wears it, every hold logged in bench/activity.log.

Catch the headset's wake window and keep it awake while nobody wears it, for installs and headset benchmarks.

    python3 bench/headset_hold.py [--install APK] [--minutes 45] [--wait-hours 6] [--then 'CMD' ...]

Waits until adb answers (the Quest drops off Wi-Fi when it sleeps unworn), then reads the real proximity sensor.
Only when nobody wears it: simulates wear (prox_close; unworn it then renders exactly as worn, 72 fps), wakes the
screen, optionally installs an APK when the installed version differs, runs each --then command (e.g. a
`bench/stress.py --headset ...` run), and holds the headset awake until --minutes have passed. Simulated wear
makes dumpsys report HEADSET_MOUNTED too (checked 2026-11-03), so a real wearer can only be seen with the
override dropped: once a minute, and before each command (never during one), it drops it for about a second
(automation_disable), reads the real state, and re-applies it unless someone is wearing the headset; then it lets
go and stops. Unworn, the Quest waits ~15 s before sleeping, so the probe never loses the window. It always
restores the real sensor on exit.
"""
import argparse, json, os, pathlib, re, subprocess, sys, time
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import stress  # ssh/adb helpers and the real-sensor check

ADB = stress.ADB.split(' -s ')[0]
ROOT = pathlib.Path(__file__).resolve().parent.parent


def log(s): print(time.strftime('%H:%M:%S'), s, flush=True)


def reachable():
    try: out = stress.mini(f'{ADB} connect 127.0.0.1:26351 >/dev/null 2>&1; {stress.ADB} shell dumpsys vrpowermanager 2>/dev/null | grep -m1 "^State:"', timeout=40)
    except subprocess.TimeoutExpired: return ''
    return out.strip()


def worn_now():
    # The real sensor (drops the override for ~2 s); re-apply the hold only if nobody is wearing it. The result goes
    # to stress.HOLD_STATUS so sim runs trust it instead of dropping the override themselves.
    worn = stress.headset_worn()
    pathlib.Path(stress.HOLD_STATUS).write_text(json.dumps({'t': time.time(), 'worn': worn, 'pid': os.getpid()}))
    if worn: return True
    hold_on(); return False


def hold_on():
    with stress.prox_lock():
        stress.prox('prox_close', 'headset_hold')
        stress.headset('input keyevent KEYCODE_WAKEUP', timeout=30)


def on_charger():
    out = stress.headset("dumpsys battery | grep -E 'AC powered|USB powered|Wireless powered'", timeout=30)
    return 'true' in out


def installed_version():
    m = re.search(r'versionCode:(\d+)', stress.headset(f'pm list packages --show-versioncode {stress.PKG}', timeout=30))
    return int(m.group(1)) if m else None


def apk_version():
    m = re.search(r'versionCode="(\d+)"', (ROOT / 'room_lab' / 'AndroidManifest.xml').read_text())
    return int(m.group(1)) if m else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--install', help='APK to install if its version is not the installed one')
    ap.add_argument('--minutes', type=float, default=45)
    ap.add_argument('--wait-hours', type=float, default=6)
    ap.add_argument('--then', action='append', default=[], help='shell command to run once held (repeatable)')
    a = ap.parse_args()
    deadline = time.time() + a.wait_hours * 3600
    while True:
        st = reachable()
        if st: break
        if time.time() > deadline: log('headset never reachable'); sys.exit(2)
        time.sleep(30)
    log(f'reachable: {st}')
    if stress.headset_worn():  # the strict check: drops any override first
        log('someone is wearing it: not touching it'); sys.exit(3)
    if not on_charger():  # user policy: simulated wear only on the charger
        log('not on the charger: no simulated wear (user policy)'); sys.exit(4)
    hold_on(); log('holding awake (simulated wear)')
    stress.activity(f'START headset hold (simulated wear, real-sensor probe each minute) for up to {a.minutes} min')
    t0 = time.time()
    try:
        if a.install and installed_version() != apk_version():
            log(f'installing {a.install} (installed {installed_version()}, apk {apk_version()})')
            with stress.prox_lock():  # deploy.py's launch sends prox_close too: no probe may read it as a person
                with open(stress.PROX_LOG, 'a') as f: f.write(f"{time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())} prox_close deploy.py install (launch) pid {os.getpid()}\n")
                stress.activity(f'START headset install {a.install}')
                r = subprocess.run(['python3', str(ROOT / 'room_lab' / 'deploy.py'), 'install', a.install], capture_output=True, text=True, timeout=600)
                stress.activity(f'END headset install (exit {r.returncode})')
            log(r.stdout.strip()[-600:] or r.stderr.strip()[-600:])
            hold_on()
            log(f'installed version now {installed_version()}')
        for cmd in a.then:
            if worn_now(): log('worn: letting go'); return
            if not on_charger(): log('taken off the charger: letting go (user policy)'); return
            log(f'run: {cmd}')
            r = subprocess.run(cmd, shell=True, cwd=ROOT, capture_output=True, text=True, timeout=1800)
            log(f'exit {r.returncode}: {(r.stdout + r.stderr).strip()[-1500:]}')
            hold_on()  # stress.py --headset restores the sensor at its end
        if a.then:  # user policy: hold only for the queued runs, release right after the last one
            log('queued runs done: releasing'); return
        while time.time() - t0 < a.minutes * 60:  # a bare hold (no runs) keeps the old timed behaviour
            if worn_now(): log('worn: letting go'); return
            if not on_charger(): log('taken off the charger: letting go (user policy)'); return
            for _ in range(3): hold_on(); time.sleep(20)
        log('hold time over')
    finally:
        try: os.unlink(stress.HOLD_STATUS)
        except OSError: pass
        with stress.prox_lock(): stress.prox('automation_disable', 'headset_hold exit')
        log('real sensor restored')
        stress.activity('END headset hold (real sensor restored)')


if __name__ == '__main__':
    main()
