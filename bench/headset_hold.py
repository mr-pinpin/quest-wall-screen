#!/usr/bin/env python3
"""Catch the headset's wake window and keep it awake while nobody wears it, for installs and headset benchmarks.

    python3 bench/headset_hold.py [--install APK] [--minutes 45] [--wait-hours 6] [--then 'CMD' ...]

Waits until adb answers (the Quest drops off Wi-Fi when it sleeps unworn), then reads the real proximity sensor.
Only when nobody wears it: simulates wear (prox_close; unworn it then renders exactly as worn, 72 fps), wakes the
screen, optionally installs an APK when the installed version differs, runs each --then command (e.g. a
`bench/stress.py --headset ...` run), and holds the headset awake until --minutes have passed. Every 20 s it
re-reads the sensor; HEADSET_MOUNTED appears only for a real wearer, so the moment someone puts it on, it lets go
(automation_disable restores the real sensor) and stops. It always restores the sensor on exit.
"""
import argparse, pathlib, re, subprocess, sys, time
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import stress  # ssh/adb helpers and the real-sensor check

ADB = stress.ADB.split(' -s ')[0]
ROOT = pathlib.Path(__file__).resolve().parent.parent


def log(s): print(time.strftime('%H:%M:%S'), s, flush=True)


def reachable():
    try: out = stress.mini(f'{ADB} connect 127.0.0.1:26351 >/dev/null 2>&1; {stress.ADB} shell dumpsys vrpowermanager 2>/dev/null | grep -m1 "^State:"', timeout=40)
    except subprocess.TimeoutExpired: return ''
    return out.strip()


def worn_now():  # without dropping the override: HEADSET_MOUNTED is the real wearer
    return 'HEADSET_MOUNTED' in stress.headset('dumpsys vrpowermanager | grep -m1 "^State:"', timeout=30)


def hold_on():
    stress.headset('am broadcast -a com.oculus.vrpowermanager.prox_close >/dev/null; input keyevent KEYCODE_WAKEUP', timeout=30)


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
    hold_on(); log('holding awake (simulated wear)')
    t0 = time.time()
    try:
        if a.install and installed_version() != apk_version():
            log(f'installing {a.install} (installed {installed_version()}, apk {apk_version()})')
            r = subprocess.run(['python3', str(ROOT / 'room_lab' / 'deploy.py'), 'install', a.install], capture_output=True, text=True, timeout=600)
            log(r.stdout.strip()[-600:] or r.stderr.strip()[-600:])
            hold_on()
            log(f'installed version now {installed_version()}')
        for cmd in a.then:
            if worn_now(): log('worn: letting go'); return
            log(f'run: {cmd}')
            r = subprocess.run(cmd, shell=True, cwd=ROOT, capture_output=True, text=True, timeout=1800)
            log(f'exit {r.returncode}: {(r.stdout + r.stderr).strip()[-1500:]}')
            hold_on()  # stress.py --headset restores the sensor at its end
        while time.time() - t0 < a.minutes * 60:
            if worn_now(): log('worn: letting go'); return
            hold_on(); time.sleep(20)
        log('hold time over')
    finally:
        stress.headset('am broadcast -a com.oculus.vrpowermanager.automation_disable >/dev/null', timeout=30)
        log('real sensor restored')


if __name__ == '__main__':
    main()
