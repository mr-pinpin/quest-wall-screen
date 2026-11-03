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


FIRST_WORN = [None]  # time of an unconfirmed MOUNTED reading


def owned_by_user():
    return stress.headset_owner() == 'user'


def worn_now():
    # The real sensor (drops the override for ~2 s). A person counts only when two probes at least 60 s apart both read
    # MOUNTED (user rule 2026-11-03: a single false reading stopped the session at 16:55); a single reading is logged,
    # the hold continues, and the probe repeats after 60 s. The result goes to stress.HOLD_STATUS for other scripts.
    if stress.headset_owner() == 'agents':  # the agreement decides, not the sensor: a reading is only a note
        if stress.headset_worn(): stress.activity('headset: sensor read MOUNTED (owner=agents: noted, not stopping)')
        hold_on(); return False
    worn = stress.headset_worn()
    if worn and FIRST_WORN[0] is None:
        FIRST_WORN[0] = time.time(); log('real sensor read MOUNTED once: confirming in 60 s (hold continues)')
        stress.activity('headset: single MOUNTED reading, re-probing in 60 s (not stopping)')
        hold_on(); time.sleep(62); worn = stress.headset_worn()
        if not worn: FIRST_WORN[0] = None; log('second probe: not worn, a false reading'); stress.activity('headset: second probe not worn, continuing')
    elif not worn: FIRST_WORN[0] = None
    status(worn)
    if worn: return True
    hold_on(); return False


def status(worn=False):
    # Tells every other script (stress.py busy/mid-run checks) that this hold probes the real sensor itself, so they
    # must not drop the override (2026-11-03: their probes every ~25 s let the headset fall asleep under the hold).
    pathlib.Path(stress.HOLD_STATUS).write_text(json.dumps({'t': time.time(), 'worn': worn, 'pid': os.getpid()}))


def hold_on():
    with stress.prox_lock():
        stress.prox('prox_close', 'headset_hold')
        stress.headset('input keyevent KEYCODE_WAKEUP', timeout=30)
    status()


def on_charger():
    # True / False when the headset answers; None when it can't be asked (asleep, adb gap): unknown is not "unplugged".
    out = stress.headset("dumpsys battery | grep -E 'AC powered|USB powered|Wireless powered'", timeout=30)
    if 'powered' not in out: return None
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
    if owned_by_user():
        log('the user owns the headset (bench/headset_owner): not touching it'); sys.exit(5)
    if stress.headset_owner() != 'agents' and stress.headset_worn():  # the strict check, confirmed after 60 s like worn_now()
        log('real sensor read MOUNTED: confirming in 60 s'); time.sleep(62)
        if stress.headset_worn(): log('someone is wearing it (two readings 60 s apart): not touching it'); sys.exit(3)
        log('second reading: not worn, a false reading; going on')
    if on_charger() is False:  # user policy: simulated wear only on the charger
        log('not on the charger: no simulated wear (user policy)'); sys.exit(4)
    hold_on(); log('holding awake (simulated wear)')
    import threading
    keep = threading.Event()
    def keeper():  # re-apply the hold and refresh the status every 20 s, also while a queued run waits or runs
        while not keep.wait(20):
            if owned_by_user():  # owner=user: release now, whatever is running
                with stress.prox_lock(): stress.prox('automation_disable', 'headset_hold owner=user')
                log('the user took the headset: released'); keep.set(); return
            try: hold_on()
            except Exception: pass
    threading.Thread(target=keeper, daemon=True).start()
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
            if owned_by_user(): log('the user took the headset: releasing'); return
            if worn_now(): log('worn: letting go'); return
            if on_charger() is False: log('taken off the charger: letting go (user policy)'); return
            log(f'run: {cmd}')
            r = subprocess.run(cmd, shell=True, cwd=ROOT, capture_output=True, text=True, timeout=1800)
            log(f'exit {r.returncode}: {(r.stdout + r.stderr).strip()[-1500:]}')
            hold_on()  # stress.py --headset restores the sensor at its end
        if a.then:  # user policy: hold only for the queued runs, release right after the last one
            log('queued runs done: releasing'); return
        while time.time() - t0 < a.minutes * 60:  # a bare hold (no runs) keeps the old timed behaviour
            if worn_now(): log('worn: letting go'); return
            if on_charger() is False: log('taken off the charger: letting go (user policy)'); return
            for _ in range(3): hold_on(); time.sleep(20)
        log('hold time over')
    finally:
        keep.set()
        try: os.unlink(stress.HOLD_STATUS)
        except OSError: pass
        with stress.prox_lock(): stress.prox('automation_disable', 'headset_hold exit')
        log('real sensor restored')
        stress.activity('END headset hold (real sensor restored)')


if __name__ == '__main__':
    main()
