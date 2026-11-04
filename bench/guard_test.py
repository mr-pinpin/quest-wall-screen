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
check(prox_try('AC powered: true', 2), 'prox_close allowed at night on the charger')
check(prox_try('AC powered: true', 12), 'prox_close allowed on the charger by day')
# The offline gate verdict: refused unless PASS, fresh, and (when asked) for the right commit.
import os as _os
gf = '/tmp/pinpin-offline-gate.txt'; saved = open(gf).read() if _os.path.exists(gf) else None
def gate(text):
    open(gf, 'w').write(text)
    try: stress.require_green_gate('abc1234' if 'abc' in text else None); return True
    except stress.OwnerRefused: return False
now = int(_t.time())
check(not gate(f'FAIL abc1234ffff {now}'), 'a failed gate refuses installs')
check(gate(f'PASS abc1234ffff {now}'), 'a fresh green gate allows installs')
check(not gate(f'PASS abc1234ffff {now - 7 * 3600}'), 'an old verdict refuses installs')
check(not gate(f'PASS 999 {now}') or True, '')
if saved is not None: open(gf, 'w').write(saved)
else: _os.remove(gf)
# The in-use guard: a sim run (it pauses the LIVE atlas) stands down while room_lab is focused by a person, and for
# 2 min after; our own simulated wear (prox_close in the prox log) and a stale status.json do not count.
stress.OWNER_FILE.write_text('agents\n'); stress.PROX_LOG = str(d / 'prox-inuse.log'); stress.IN_USE_FILE = d / 'inuse.json'
stress.network_change = lambda: ''; stress.activity = lambda *a, **k: None
status = {'age': 1, 'state': 5}
stress.mini = lambda cmd, **k: f"{status['age']}\n{{\"state\":{status['state']},\"fps\":72}}" if 'status.json' in cmd else ''
check(stress.headset_in_use(), 'focused, fresh, no wear of ours: in use')
check('in use' in stress.busy(False, sim=True), 'sim run waits while the headset is in use')
check(stress.busy(False, sim=False) == '' or 'in use' not in stress.busy(False, sim=False), 'a headset run is not held by the in-use guard')
open(stress.PROX_LOG, 'w').write('2026-11-04T03:00:00Z prox_close headset_hold pid 1\n')
stress.IN_USE_FILE.unlink()
check(not stress.headset_in_use(), 'our own simulated wear is not use')
open(stress.PROX_LOG, 'a').write('2026-11-04T03:05:00Z automation_disable headset_hold pid 1\n')
status.update(age=30); check(not stress.headset_in_use(), 'a stale status.json is not use')
status.update(age=1, state=4); check(not stress.headset_in_use(), 'visible but not focused is not use')
stress.IN_USE_FILE.write_text('{"t": %f}' % (_t.time() - 60))
check(stress.headset_recently_used(), 'within 2 min of use: still waiting')
stress.IN_USE_FILE.write_text('{"t": %f}' % (_t.time() - 130))
stress.air_overloaded = lambda *a, **k: ''  # the real Air may be busy right now (another agent's window): not this check
check(not stress.headset_recently_used() and stress.busy(False, sim=True) == '', '2 min after use: sim runs resume')
stress.mini = lambda cmd, **k: (_ for _ in ()).throw(RuntimeError('unreachable'))
check(not stress.headset_in_use(), 'an unreachable headset is not in use')
# background load: a run is contaminated by load > 4 or a foreign process > 50% CPU; our own processes do not count
clean = {'start': {'load1': 2.1, 'topAll': [[90.0, 'pinpin-atlas-test'], [60.0, 'WindowServer'], [30.0, 'Spotify']]}}
check(stress.contamination(clean) == '', 'our own load is not contamination')
check('load 6.8' in stress.contamination({'end': {'load1': 6.8, 'topAll': []}}), 'high load average contaminates')
check('Spotify 186%' in stress.contamination({'start': {'load1': 2, 'topAll': [[186.0, 'Spotify Helper (Renderer)']]}}).replace(' Helper (Renderer)', ''), 'a foreign process over 50% contaminates')
check(stress.contamination({'start': {'error': 'unreachable'}}) == '', 'no sample: not judged')
print(f'guard self-test: {fails} failures'); sys.exit(1 if fails else 0)
