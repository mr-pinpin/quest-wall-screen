"""stress.headset() owner guard and the shared install lock, without any device (OWNER_FILE and the lock path faked)."""
import os, pathlib, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stress
d = pathlib.Path(tempfile.mkdtemp()); stress.OWNER_FILE = d / 'owner'; stress.INSTALL_LOCK = str(d / 'install.lock')
calls = []; stress.mini = lambda cmd, **k: calls.append(cmd) or ''
fails = 0
def check(c, m):
    global fails
    if not c: print('FAIL', m); fails += 1
def refused(cmd, **k):
    try: stress.headset(cmd, **k); return False
    except stress.OwnerRefused: return True
stress.OWNER_FILE.write_text('user\n')
for c in ('am start -n p/.A', 'am force-stop p', 'pm install -r -g x.apk', 'pm grant p perm', 'input keyevent KEYCODE_WAKEUP',
          'am broadcast -a com.oculus.vrpowermanager.prox_close'):
    check(refused(c), f'user: {c} refused')
check(not refused('run-as p cat files/status.json'), 'user: reads allowed')
check(not refused('am broadcast -a com.oculus.vrpowermanager.automation_disable', allow_user=True), 'release allowed')
try:
    with stress.install_lock('t'): check(False, 'install lock while user')
except stress.OwnerRefused: pass
check(not os.path.exists(stress.INSTALL_LOCK), 'refused lock leaves no file')
stress.OWNER_FILE.write_text('agents\n')
check(not refused('am start -n p/.A'), 'agents: launch allowed')
with stress.install_lock('t'):
    check(os.path.exists(stress.INSTALL_LOCK), 'lock file while held')
    try:
        with stress.install_lock('second'): check(False, 'second holder got in')
    except stress.OwnerRefused: pass
check(not os.path.exists(stress.INSTALL_LOCK), 'released')
open(stress.INSTALL_LOCK, 'w').write(f'999999999 {stress.socket.gethostname()} dead 2026\n')
with stress.install_lock('t'): check(True, '')
check(not os.path.exists(stress.INSTALL_LOCK), 'stale lock taken over and released')
# Old mkdir-style lock: an empty dir whose owner pid is dead is stale; a live owner blocks.
os.mkdir(stress.INSTALL_LOCK); (d / '.quest-install.owner').write_text('999999999 old\n')
with stress.install_lock('t'): check(os.path.isfile(stress.INSTALL_LOCK), 'stale lock dir replaced by the file lock')
os.mkdir(stress.INSTALL_LOCK); (d / '.quest-install.owner').write_text(f'{os.getpid()} live\n')
try:
    with stress.install_lock('t'): check(False, 'live old-style lock ignored')
except stress.OwnerRefused: pass
check(os.path.isdir(stress.INSTALL_LOCK), 'live old-style lock left alone'); os.rmdir(stress.INSTALL_LOCK)
# prox_close: refused off the charger and at night (dumpsys faked through stress.mini).
stress.OWNER_FILE.write_text('agents\n')
import time as _t
real_gm = _t.gmtime
def prox_try(battery, hour):
    stress.mini = lambda cmd, **k: battery if 'dumpsys battery' in cmd else ''
    stress.time.gmtime = lambda *a: _t.struct_time((2026, 11, 4, hour, 0, 0, 2, 308, 0))
    try: stress.prox('prox_close', 'test'); return True
    except stress.OwnerRefused: return False
    finally: stress.time.gmtime = real_gm
stress.PROX_LOG = str(d / 'prox.log')
check(not prox_try('AC powered: false\nUSB powered: false', 12), 'prox_close refused off the charger')
check(not prox_try('AC powered: true', 2), 'prox_close refused at night')
check(prox_try('AC powered: true', 12), 'prox_close allowed on the charger by day')
print(f'guard self-test: {fails} failures'); sys.exit(1 if fails else 0)
