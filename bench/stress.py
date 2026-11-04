#!/usr/bin/env python3
"""N-screen stress test for the house network (docs/PERFORMANCE.md, "Stress test").

  sudo -n python3 bench/stress.py --n 10 --secs 120 [--wan K] [--keep]     # stand-in headset (default)
  sudo -n python3 bench/stress.py --headset ...                             # the real Quest, only while unworn

Nobody has to wear anything, and the test never runs while someone is using the gear:
  - It refuses to start, and aborts mid-run, when the laptop has been used in the last 10 minutes (HID idle time)
    or when someone is wearing the headset (app in VISIBLE/FOCUSED). --force skips the laptop check only.
  - The default mode uses a stand-in headset (bench/stress/headset_sim.py on the Mini). It receives the atlas
    stream and sends the same demand as the Quest's stress mode, from a separate test copy of the sender on
    ports 57730/57731, run as the PinPin Host plugin "pinpin-atlas-test". So the live sender and the headset are untouched.
  - --headset uses the Quest itself, unworn, for headset-only numbers (fps, decode latency). It simulates wear
    (prox_close; the Quest then renders exactly as when worn), starts the app if needed and puts it in stress mode
    (files/stress.json → room_lab.cpp stressDemand) on the live sender. Afterwards it restores everything and hands
    the headset back to its real proximity sensor. While wear is simulated, the sensor can't be read, so a 2-minute
    --headset run can't notice someone putting it on midway.

Steps:
  1. Create the missing laptop displays 2..N+1 (pinpin-displays :57710, 1920x1200 at 1x) and fill them with
     a moving GPU-drawn pattern (bench/stress/pattern.swift).
  2. Start the demand source (stand-in or headset), and optionally --wan K internet downloads from the laptop.
  3. Measure for --secs: ping from the Mini (the gateway) to the internet over en1 and to the headset over Wi-Fi,
     the house internet traffic at the gateway, the laptop's Wi-Fi bytes, the stream as received, and the
     sender's log and crash reports.
  4. Tear down everything it created and write bench/results/stress-<time>-n<N>.json plus a row in
     bench/results/stress.md.
"""
import benchlock  # noqa: E402  (the Air lock: exclusive for benchmarks, shared for functional tests)
import socket
import argparse, atexit, base64, calendar, contextlib, re, fcntl, json, math, os, pathlib, shlex, statistics, subprocess, sys, time

ROOT = pathlib.Path(__file__).resolve().parent.parent
HERE = pathlib.Path(__file__).resolve().parent / 'stress'
PKG = 'com.pinpin.wallscreen.roomlab'
ADB = '~/.local/share/pinpin-quest/platform-tools/adb -s 127.0.0.1:26351'
APP = '~/Applications/PinPin Stress Pattern.app'
HEADSET_IP = '192.168.0.3'
LAPTOP_IP = '192.168.0.102'  # fallback only: the laptop's DHCP address changes (it was .104 after the 2026-11-03 reboot),
                             # and a stale one left the stand-in with no stream at all; laptop_ip() asks the laptop
IDLE_NEEDED = 600
BENCH_CHIP, BENCH_MODEL = 'Apple M1', 'MacBookAir10,1'  # the pinned benchmark host (user direction 2026-11-03)
# Shared bench lock on the laptop (agreed by work-c4 and work-09): a directory made with mkdir (atomic), holding an
# owner note. Every test that creates displays or runs a test sender takes it; afterwards it is moved aside.
# Older than LOCK_STALE_S means its holder died: taken over.
BENCH_LOCK = '~/.local/share/pinpin-bench.lock'
LOCK_STALE_S = 1800
HOST_PLUGINS = '"$HOME/Library/Application Support/PinPin Host/plugins"'
TEST_PLUGIN = {'exec': '~/.local/share/pinpin-atlas/pinpin-atlas-test', 'args': ['--port', '57730', '--demand-port', '57731', '--stat-secs', '5'],
               'log': '~/.local/share/pinpin-atlas/atlas-sim.log', 'truncateLog': True, 'restartDelay': 1.5}


def ssh(host, cmd, timeout=60, check=False, stdin=None):
    r = subprocess.run(['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10', host, cmd],
                       capture_output=True, text=True, timeout=timeout, input=stdin)
    if check and r.returncode:
        sys.exit(f'{host}: {cmd[:80]}... failed: {r.stderr.strip()[-400:]}')
    return r.stdout


def air(cmd, **k): return ssh('pinpin-air', cmd, **k)
def mini(cmd, **k): return ssh('pinpin-mini', cmd, **k)
# If the Mini's adb server is not running, the first adb call forks one that inherits the ssh session's stdout, and
# that ssh never returns (2026-11-03: a game run's probe overran by minutes). Start it detached first.
ADB_SERVER = f"{ADB.split(' -s ')[0]} start-server </dev/null >/dev/null 2>&1; "


# Commands that change what the person in the headset sees: installs, launches, stops, simulated wear, wake keys.
# 2026-11-04 00:48 UTC: two install paths with different locks both restarted the app the user was about to wear.
# headset() refuses these unless bench/headset_owner says "agents" (the release, automation_disable, is always allowed),
# and every install/launch takes INSTALL_LOCK, the one lock shared with work-df's installer.
MUTATING = re.compile(r'\b(am\s+(start|force-stop|kill|stack)|pm\s+(install|uninstall|clear|grant|revoke)|cmd\s+package\s+install|input\s+keyevent|'
                      r'vrpowermanager\.(prox_close|unmounted_wake)|reboot)\b')
INSTALL_LOCK = '/root/work/.quest-install.lock'


class OwnerRefused(RuntimeError): pass


def require_green_gate(commit=None, max_age_s=6 * 3600):
    # Installs need a green offline gate (AGENTS.md). bench/run_offline_tests.sh leaves "PASS|FAIL <commit> <unix time>"
    # in /tmp/pinpin-offline-gate.txt; refuse anything else, an old verdict, or a different commit when one is given.
    # (2026-11-04: an install went out while the gate's last line said FAILED.)
    path = '/tmp/pinpin-offline-gate.txt'  # the latest run; with a commit, that commit's own verdict (other agents run the gate too)
    if commit:
        import glob
        hits = glob.glob(f'/tmp/pinpin-offline-gate/{commit}*')
        if hits: path = hits[0]
    try: verdict, sha, t = open(path).read().split()[:3]
    except (OSError, ValueError): raise OwnerRefused('no offline gate verdict: run bench/run_offline_tests.sh first')
    if verdict != 'PASS': raise OwnerRefused(f'offline gate FAILED at {sha[:7]}: fix it before installing')
    if time.time() - int(t) > max_age_s: raise OwnerRefused('offline gate verdict older than 6 h: run it again')
    if commit and not sha.startswith(commit): raise OwnerRefused(f'offline gate ran on {sha[:7]}, not {commit}')
    return sha


def headset(cmd, allow_user=False, **k):
    if MUTATING.search(cmd) and not allow_user and headset_owner() not in ('agents', ''):
        raise OwnerRefused(f'headset_owner={headset_owner()}: refusing {cmd[:80]!r}')
    return mini(f"{ADB_SERVER}{ADB} shell {json.dumps(cmd)}", **k)


@contextlib.contextmanager
def install_lock(who='', wait_s=0):
    # The shared install lock: an O_EXCL file holding "pid host who time" (work-df's installer uses the same file),
    # plus a flock on it while held. A lock whose pid is gone on this host is stale and taken over.
    me = f"{os.getpid()} {socket.gethostname()} {who or sys.argv[0]} {time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}\n"
    t0 = time.time()
    while True:
        try:
            fd = os.open(INSTALL_LOCK, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644); os.write(fd, me.encode()); break
        except FileExistsError:
            if os.path.isdir(INSTALL_LOCK):  # the old mkdir-style lock: stale once empty and its owner pid is gone
                try: owner = open(os.path.join(os.path.dirname(INSTALL_LOCK), '.quest-install.owner')).read().split()
                except OSError: owner = []
                pid = next((w for w in owner if w.isdigit()), '')
                if not os.listdir(INSTALL_LOCK) and not (pid and os.path.exists(f'/proc/{pid}')):
                    try: os.rmdir(INSTALL_LOCK)
                    except OSError: pass
                    continue
                if time.time() - t0 >= wait_s: raise OwnerRefused(f'old-style install lock held by pid {pid or "?"}')
                time.sleep(5); continue
            try: held = open(INSTALL_LOCK).read().split()
            except OSError: held = []
            alive = bool(held) and held[0].isdigit() and len(held) > 1 and held[1] == socket.gethostname() and os.path.exists(f'/proc/{held[0]}')
            if held and not alive and len(held) > 1 and held[1] == socket.gethostname():
                os.replace(INSTALL_LOCK, INSTALL_LOCK + '.stale'); continue
            if time.time() - t0 >= wait_s: raise OwnerRefused(f'install lock held: {" ".join(held)[:120]}')
            time.sleep(5)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX)
        if headset_owner() not in ('agents', ''): raise OwnerRefused(f'headset_owner={headset_owner()}')
        yield
    finally:
        os.close(fd)
        try:
            if open(INSTALL_LOCK).read() == me: os.remove(INSTALL_LOCK)
        except OSError: pass


def displays():
    # The helper serves one request at a time and creating a display takes ~9 s, so wait and retry.
    for _ in range(4):
        out = air('curl -s -m30 localhost:57710/displays', timeout=45)
        if out.strip().startswith('['): return json.loads(out)
        time.sleep(5)
    sys.exit('stress: the displays helper (:57710) did not answer')


def laptop_idle_s():
    out = air("ioreg -c IOHIDSystem | awk '/HIDIdleTime/ {print int($NF/1000000000); exit}'")
    return int(out.strip() or 0)


def headset_status():
    out = headset(f'pidof {PKG} >/dev/null && run-as {PKG} cat files/status.json')
    try: return json.JSONDecoder().raw_decode(out[out.index('{'):])[0]
    except ValueError: return None


# Our own simulated wear (prox_close) makes dumpsys report HEADSET_MOUNTED. Every script that sends it and every
# real-sensor probe takes this lock, so a probe never reads a prox_close another process sent a moment earlier
# (2026-11-03: the headset sat in the kitchen while two of our processes raced into "someone is wearing it").
# Every override we send is logged, so a MOUNTED reading can be checked against what we did.
PROX_LOCK, PROX_LOG = '/tmp/pinpin-prox.lock', '/tmp/pinpin-prox.log'


@contextlib.contextmanager
def prox_lock():
    with open(PROX_LOCK, 'a') as f:
        fcntl.flock(f, fcntl.LOCK_EX)
        try: yield
        finally: fcntl.flock(f, fcntl.LOCK_UN)


def prox(action, who=''):
    # action: 'prox_close' (simulate wear) or 'automation_disable' (real sensor); call with prox_lock held.
    # prox_close wakes the headset's tracking. 2026-11-04 night: wake-ups in the dark (01:39, 02:06 UTC) left Meta's
    # Space Setup room mis-relocalised ("everything in a room far away" until the light went on); that self-corrects with
    # the light, and RELOCCHECK + the placement guard cover it, so night windows are allowed again (work-df, same night).
    # prox_close is refused off the charger; keep windows short and always release (automation_disable).
    if action == 'prox_close':
        bat = headset('dumpsys battery', timeout=30)
        if not any(l.strip().endswith('true') for l in bat.splitlines() if 'powered' in l):
            raise OwnerRefused('prox_close refused: the headset is not on the charger')
    with open(PROX_LOG, 'a') as f: f.write(f"{time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())} {action} {who or sys.argv[0]} pid {os.getpid()}\n")
    return headset(f'am broadcast -a com.oculus.vrpowermanager.{action} >/dev/null', allow_user=(action == 'automation_disable'), timeout=30)


def headset_worn():
    # The real proximity sensor: drop our override, then a person is only someone the sensor reports as MOUNTED for
    # a full 5 s (sampled each second); any unmounted/standby sample means nobody. App state can't tell, because
    # simulated wear makes it VISIBLE/FOCUSED too. Callers that were holding wear re-apply it themselves.
    with prox_lock():
        prox('automation_disable', 'headset_worn')
        out = headset('sleep 1; for i in 1 2 3 4 5; do dumpsys vrpowermanager | grep -m1 "^State:"; sleep 1; done', timeout=40)
    states = [l.split(':', 1)[1].strip() for l in out.splitlines() if l.startswith('State:')]
    return len(states) >= 3 and all(s == 'HEADSET_MOUNTED' for s in states)


IN_USE_FILE = pathlib.Path('/tmp/pinpin-headset-inuse.json')  # when the headset was last seen in real use
IN_USE_QUIET_S = 120  # the live atlas is touched again only after the app has been unfocused this long


def our_wear_active():
    # Simulated wear of ours (prox_close) is on when the last override any of our processes sent was prox_close.
    try: lines = pathlib.Path(PROX_LOG).read_text().splitlines()
    except OSError: return False
    for l in reversed(lines):
        if ' prox_close ' in l: return True
        if ' automation_disable ' in l: return False
    return False


def headset_in_use():
    # Real use: room_lab FOCUSED (XrSessionState 5) in a fresh status.json (written every second while it renders),
    # with no simulated wear of ours making it so. owner=agents no longer means nobody is in it (the user wears the
    # headset while agents install and restart), so runs that pause the LIVE atlas check this before and during.
    try:
        out = headset(f"pidof {PKG} >/dev/null && run-as {PKG} sh -c 'echo $(( $(date +%s) - $(stat -c %Y files/status.json) )); cat files/status.json'",
                      allow_user=True, timeout=15)
        lines = out.strip().splitlines(); age = int(lines[0]); st = json.loads(lines[-1]).get('state')
    except Exception:
        return False  # app not running, headset asleep or unreachable: nobody is looking at the screens
    used = age <= 5 and st == 5 and not our_wear_active()
    if used: IN_USE_FILE.write_text(json.dumps({'t': time.time()}))
    return used


def headset_recently_used():
    if headset_in_use(): return True
    try: return time.time() - json.loads(IN_USE_FILE.read_text())['t'] < IN_USE_QUIET_S
    except (OSError, ValueError, KeyError): return False


def note_in_use_wait():
    # one activity line per waiting spell (not one per 12 s retry)
    mark = pathlib.Path('/tmp/pinpin-inuse-wait.logged')
    try: last = mark.stat().st_mtime
    except OSError: last = 0
    if time.time() - last > 600:
        activity('WAIT live atlas untouched: the headset is in use (room_lab focused); sim runs resume 2 min after it is unfocused')
    mark.touch()


def simulate_wear(on):
    # Unworn, with prox_close the Quest renders exactly as when worn (measured: state 5, 72 fps, no throttling).
    with prox_lock(): prox('prox_close' if on else 'automation_disable', 'simulate_wear')


def pct(xs, p):
    if not xs: return None
    xs = sorted(xs); return round(xs[min(len(xs) - 1, int(math.ceil(p / 100 * len(xs))) - 1)], 1)


def pings(text):
    out = []
    for line in text.splitlines():
        if 'time=' in line:
            try: out.append(float(line.split('time=')[1].split()[0]))
            except ValueError: pass
    return out


def link_bytes(run, iface):
    # netstat -ib: Name Mtu Network Address Ipkts Ierrs Ibytes Opkts Oerrs Obytes
    for line in run(f'netstat -ib -I {iface}').splitlines():
        f = line.split()
        if len(f) >= 10 and f[0] == iface and f[2].startswith('<Link'):
            return int(f[6]), int(f[9])
    return 0, 0


def longest_gap(series):
    # headset mode: longest stretch (s) in which the atlas carried under 0.2 Mbit/s (a frozen picture)
    worst = run = 0.0
    for a, b in zip(series, series[1:]):
        dt = max(0.1, b['t'] - a['t'])
        run = run + dt if (b['bytes'] - a['bytes']) * 8 / 1e6 / dt < 0.2 else 0.0
        worst = max(worst, run)
    return round(worst, 1)


def lock_take(owner):
    """'' when this run now holds the bench lock (EXCLUSIVE, bench/benchlock.py), else who holds it. A refusal leaves an
    intent marker (refreshed on each retry, expiring after a minute), so shared holders drain and none join meanwhile."""
    global _LOCK_TID
    if '_LOCK_TID' not in globals(): _LOCK_TID = benchlock._id('exclusive', 'stress')
    r = benchlock.try_exclusive(_LOCK_TID, owner, LOCK_STALE_S)
    return '' if r == 'taken' else r.replace('busy ', '', 1)


def lock_release():
    if '_LOCK_TID' in globals(): benchlock.release('exclusive:' + _LOCK_TID)


def laptop_ip():
    ip = air('ipconfig getifaddr en0', timeout=20).strip()
    return ip if ip.count('.') == 3 else LAPTOP_IP


def plugin_override(name, extra_args):
    """Append args to a PinPin Host plugin for one run and restart it; returns the original spec for plugin_restore."""
    path = f'{HOST_PLUGINS}/{name}.json'
    orig = air(f'cat {path}').strip()
    spec = json.loads(orig); spec['args'] = list(spec.get('args', [])) + list(extra_args)
    air(f"printf %s {shlex.quote(json.dumps(spec))} > {path}; curl -s -m10 -X POST '127.0.0.1:57790/restart?name={name}'")
    return orig


def plugin_restore(name, orig):
    air(f"printf %s {shlex.quote(orig)} > {HOST_PLUGINS}/{name}.json; curl -s -m10 -X POST '127.0.0.1:57790/restart?name={name}'")


HOLD_STATUS = '/tmp/pinpin-headset-hold.json'
_SHARED_OWNER = pathlib.Path('/root/work/quest-wall-screen/bench/headset_owner')  # the runtime copy, also from a worktree
OWNER_FILE = _SHARED_OWNER if _SHARED_OWNER.exists() else pathlib.Path(__file__).resolve().parent / 'headset_owner'


def headset_owner():
    # The explicit agreement (user, 2026-11-03: "I'm not using the headset. Please develop. When I want to try
    # something, I'll say so"): bench/headset_owner says "agents" or "user" (first word), written only by work-df on
    # the user's word. agents: run freely, worn readings are notes, not stops. user: hands off the headset entirely.
    # Missing file: the older sensor-based rules.
    try: return OWNER_FILE.read_text().split()[0].lower()
    except (OSError, IndexError): return ''  # written by bench/headset_hold.py while it holds the headset awake


def hold_says_unworn():
    # While headset_hold.py simulates wear it probes the real sensor itself once a minute and records the result.
    # Dropping its override again here (headset_worn) would fight it, and our own simulated wear must never read as
    # a person: trust a fresh hold probe instead.
    try:
        st = json.loads(pathlib.Path(HOLD_STATUS).read_text())
        return time.time() - st['t'] < 150 and not st['worn']
    except (OSError, ValueError, KeyError):
        return False


def busy(force, sim=False):
    """Why the gear is in use right now, or '' when the test may run.
    The laptop has no idle gate (user decision 2026-11-03: agents use the Air freely; HID idle was false anyway,
    since our own Sunshine's input tickle resets it). Agents coordinate through the bench lock and bench/activity.log.
    The headset still never runs while a person really wears it."""
    own = headset_owner()
    # sim runs too: they pause the LIVE sender for the run, so the user's screens in the headset would freeze
    if own == 'user': return 'the bench lock is held (the user owns the headset: bench/headset_owner)'  # worded so retry loops wait
    if sim and headset_recently_used():  # a sim run pauses the LIVE atlas: never while someone is in the headset
        note_in_use_wait(); return 'the bench lock is held (the headset is in use: room_lab focused, or was within 2 min)'
    if own == '' and not hold_says_unworn() and headset_worn(): return 'someone is wearing the headset'  # a live hold probes itself
    net = network_change()
    if net: return f'the bench lock is held ({net})'  # worded as a held lock so every retry loop waits it out
    return ''


NET_AGENTS = ('work-42',)  # router/network sessions: a channel change drops Wi-Fi for ~30 s


def network_epoch():
    # Which network a result belongs to (work-df, 2026-11-03): the 5 GHz channel changes from 36 to 157 at
    # work-42's END line; every network change starts a new epoch, so pairs and baselines never mix networks.
    ends = []
    try:
        for l in ACTIVITY_LOG.read_text().splitlines():
            p = l.split()
            if len(p) > 3 and p[1] in NET_AGENTS and 'END' in p[3]: ends.append(p[0])
    except OSError: pass
    # Informational only: verdicts and summaries use the live laptopChannel, since a change can be rolled back
    # (2026-11-03 17:55: ch157 was tried and reverted to ch36).
    return {'networkChanges': len(ends), 'lastChange': ends[-1] if ends else None}


def network_change(now=None):
    # A run must not straddle a router change: refuse while a network agent has an open START in activity.log, and
    # for 2 minutes after its END (work-df, 2026-11-03).
    now = now or time.time(); open_, last_end = {}, 0
    try: lines = ACTIVITY_LOG.read_text().splitlines()[-400:]
    except OSError: return ''
    for l in lines:
        p = l.split()
        if len(p) < 4 or p[1] not in NET_AGENTS: continue
        try: t = calendar.timegm(time.strptime(p[0], '%Y-%m-%dT%H:%M:%SZ'))
        except ValueError: continue
        if 'START' in p[3] and 'END' not in p[3]: open_[p[2]] = t
        elif 'END' in p[3]: open_.pop(p[2], None); last_end = max(last_end, t)
    if open_: return f'network change in progress by {", ".join(sorted({x.split("[")[0] for x in open_}))}'
    if now - last_end < 120: return f'network change ended {int(now - last_end)} s ago (2 min settle)'
    return ''


# One shared log for every checkout (a run started from another checkout logged into its own copy, unseen: 2026-11-03)
_SHARED_LOG = pathlib.Path('/root/work/quest-wall-screen/bench/activity.log')
ACTIVITY_LOG = _SHARED_LOG if _SHARED_LOG.parent.is_dir() else pathlib.Path(__file__).resolve().parent / 'activity.log'


def activity(what, who=None):
    # The shared, append-only action log (bench/activity.log): every script and agent records what it does to the
    # Air, the headset or the Mini, start and end, in UTC. Check its tail and the bench lock before acting on a device.
    who = who or os.environ.get('PINPIN_WHO', 'work-c4')
    with ACTIVITY_LOG.open('a') as f:
        f.write(f"{time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())} {who} {pathlib.Path(sys.argv[0]).name}[{os.getpid()}] {what}\n")


# Per-phase timestamps (UTC): printed as the run goes and saved in the result, so a slow or hung run shows which step
# took the time (setup, overrides, demand, measure, teardown) instead of only its START and END in activity.log.
PHASES = []


def _stamp(t):
    return {'utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime(t)),
            'sinceStartS': round(t - PHASES[0]['t'], 1) if PHASES else 0.0}


# Background load on the Air (2026-11-04: Spotify's renderer at ~186% CPU and load ~8-9 all night): sampled at the start
# and end of each measurement; a run is "contaminated" when the load average is over 4 or a process that is not ours
# takes more than half a core. campaign.py leaves contaminated pairs out of the verdicts. Ours: the sender, the displays
# helper, the stand-ins, and what capture itself drives (WindowServer, replayd).
BENCH_PROCS = re.compile(r'^(pinpin-|encprobe|stresspattern|python3?$|sshd|WindowServer$|replayd$|ps$)')


def air_load():
    try:
        out = air('sysctl -n vm.loadavg; ps -Arco pcpu,comm | head -6', timeout=20)
        lines = out.strip().splitlines(); load = [float(x) for x in lines[0].strip('{} ').split()[:3]]
        top = []
        for l in lines[2:]:
            parts = l.strip().split(None, 1)
            if len(parts) == 2: top.append([float(parts[0]), parts[1]])
        return {'load1': load[0], 'load5': load[1], 'top': top[:3], 'topAll': top}
    except Exception as ex:
        return {'error': str(ex)[:100]}


def contamination(samples):
    why = []
    for when, smp in samples.items():
        if 'load1' not in smp: continue
        if smp['load1'] > 4: why.append(f'{when}: load {smp["load1"]}')
        why += [f'{when}: {c} {cpu:.0f}%' for cpu, c in smp.get('topAll', []) if cpu > 50 and not BENCH_PROCS.match(c)]
    return '; '.join(why)


def phase(name):
    t = time.time()
    PHASES.append(dict(phase=name, t=t, **_stamp(t)))
    print(f"  phase {name} {PHASES[-1]['utc']} (+{PHASES[-1]['sinceStartS']} s)", flush=True)


def phases():
    return [{k: v for k, v in x.items() if k != 't'} for x in PHASES] + [dict(phase='result', **_stamp(time.time()))]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--n', type=int, default=10); ap.add_argument('--secs', type=int, default=120)
    ap.add_argument('--sweep', type=float, default=3.0, help='seconds the virtual gaze stays on each screen')
    ap.add_argument('--wan', type=int, default=0, help='parallel internet downloads from the laptop')
    ap.add_argument('--keep', action='store_true', help='leave the created displays in place')
    ap.add_argument('--scenario', default='ring', choices=['ring', 'cinema', 'vr', 'wall'], help='stand-in demand pattern (see headset_sim.py)')
    ap.add_argument('--encprobe', type=int, default=0, help='run bench/encprobe.swift (a game encode) on the Air for this many seconds during the measurement')
    ap.add_argument('--sim-extra', default='', help='extra stand-in options (headset_sim.py), e.g. "--v2 --hero --keyreq 15"')
    ap.add_argument('--game', choices=['LAPTOP', 'MINI'], help='--headset only: also run that main game stream and measure it')
    ap.add_argument('--headset', action='store_true', help='use the real Quest, unworn with simulated wear, instead of the stand-in')
    ap.add_argument('--force', action='store_true', help='skip the laptop-idle check (never the headset check)')
    ap.add_argument('--flags', default='', help='sender flags, e.g. bpcut (AtlasSender --flags): on the test copy in stand-in mode, '
                    'on the live sender (restarted, restored after) with --headset')
    ap.add_argument('--headset-flag', action='append', default=[], metavar='NAME',
                    help='--headset only: force a room_lab flag for the run via its files/NAME-on file (vt, budget, gputime, '
                         'wifill); removed in teardown. Repeatable.')
    ap.add_argument('--sim-on', choices=['mini', 'air'], default='mini',
                    help='where the stand-in headset runs: mini (over the house Wi-Fi, default) or air (on the laptop itself, '
                         'receiving from 127.0.0.1: no Wi-Fi in the path; the control that separates sender from network)')
    ap.add_argument('--source', choices=['sck', 'synth'], default=None,
                    help='synth (default for stand-in runs: the pipeline benchmark, the sender renders the screens itself) or sck (the macOS screen source: virtual displays + ScreenCaptureKit; default for --headset runs)')
    ap.add_argument('--cover-idle', action='store_true', help='cover the non-animated test screens with a static window (hides the aerial wallpaper)')
    ap.add_argument('--static', type=int, default=0, help='leave the last K created screens without the moving pattern (still desktops)')
    ap.add_argument('--codec', choices=['H264', 'HEVC'], help='--headset only: the game stream codec (room_lab CODEC flag, '
                    'files/codec-h264, v0.39+); default: as the headset is set')
    ap.add_argument('--sunshine', action='append', default=[], metavar='NAME=VALUE',
                    help='Sunshine config override for this run, e.g. vt_realtime=enabled (appended to the sunshine plugin args, '
                         'restored after); repeatable')
    ap.add_argument('--allow-over-20', action='store_true', help='allow more than 20 screens (30 crashed replayd and wedged WindowServer)')
    ap.add_argument('--cinema-size', default='', help='WxH for screen 2 (the cinema screen), e.g. 2560x1600: wider than one '
                    '1920 cell, so vt tiles it (3840x2160 was not capturable on the Air). Pair runs with the same size')
    a = ap.parse_args()
    cinema = tuple(int(v) for v in a.cinema_size.split('x')) if a.cinema_size else None
    if a.n > 20 and not a.allow_over_20:
        sys.exit('refusing --n %d: 30 concurrent captures wedged the laptop on 2026-11-03; pass --allow-over-20 once visibility-only capture is proven' % a.n)
    sim = not a.headset
    if a.source is None: a.source = 'synth' if sim else 'sck'  # 2026-11-03: synth is the pipeline default (work-09 deployed it)
    test_plugin = dict(TEST_PLUGIN, args=TEST_PLUGIN['args'] + (['--flags', a.flags] if a.flags else []))
    why = busy(a.force, sim)
    if why: sys.exit(f'stress: not running, {why}')
    # Benchmarks are pinned to the M1 Air for stability (user direction 2026-11-03): the sender host is recorded in
    # every result and anything else is refused; verdict.py never compares runs from different hosts.
    for attempt in range(3):  # a transient empty ssh read (seen 2026-11-03) is not a wrong host
        try: hw = air('sysctl -n machdep.cpu.brand_string hw.model; sw_vers -productVersion', timeout=20).split('\n')
        except subprocess.TimeoutExpired: hw = []
        if hw and hw[0].strip(): break
        time.sleep(5)
    host = {'name': 'pinpin-air', 'chip': hw[0].strip() if hw else '', 'model': hw[1].strip() if len(hw) > 1 else '',
            'macos': hw[2].strip() if len(hw) > 2 else ''}
    if host['chip'] != BENCH_CHIP or host['model'] != BENCH_MODEL:
        sys.exit(f'stress: not running, the benchmark host must be the {BENCH_CHIP} {BENCH_MODEL}; pinpin-air reports {host}')
    holder = lock_take(f'stress.py {" ".join(sys.argv[1:])} since {time.strftime("%H:%M:%S")}')
    if holder: sys.exit(f'stress: not running, the bench lock is held ({holder})')
    run_desc = f'{"headset" if a.headset else "sim"} run: ' + ' '.join(sys.argv[1:])
    activity(f'START {run_desc} (bench lock taken; Air: test displays + sender' + (', headset: simulated wear' if a.headset else '') + ')')
    atexit.register(lambda: activity(f'END {run_desc} (bench lock released)'))
    atexit.register(lock_release)
    lap_ip = laptop_ip()
    # The laptop's actual Wi-Fi channel/band at run start (a router change once left it on 2.4 GHz ch13).
    try: laptop_channel = air("system_profiler SPAirPortDataType 2>/dev/null | grep -m1 -E '^ +Channel:' | sed 's/^ *Channel: *//'", timeout=30).strip()
    except subprocess.TimeoutExpired: laptop_channel = ''
    if laptop_channel and '5GHz' not in laptop_channel:  # the benchmark network is 5 GHz: wait, do not measure the wrong band
        sys.exit(f'stress: not running, the bench lock is held (the laptop is on {laptop_channel}, not 5 GHz: waiting for it to rejoin)')
    ids = list(range(2, a.n + 2))
    t0 = time.strftime('%Y%m%d-%H%M%S')
    phase('setup')
    print(f'stress: {a.n} screens for {a.secs} s, wan={a.wan}, demand from {"stand-in on the Mini" if sim else "the headset"}')

    shown = {'cinema': [2, 3, a.n + 1], 'vr': []}.get(a.scenario, ids)
    if a.source == 'synth':
        # The pipeline benchmark (docs/sources.md): the test sender renders the screens itself at 60 Hz, no virtual
        # displays and no ScreenCaptureKit, so the result measures encoder + network + receiver, not macOS capture.
        if not sim: sys.exit('stress: --source synth needs the stand-in (sim mode)')
        size = lambda i: f'{cinema[0]}x{cinema[1]}' if i == 2 and cinema else '1920x1200'
        moving = [i for i in ids if i in shown]; moving = moving[:max(0, len(moving) - a.static)]
        test_plugin['args'] += ['--synth', ','.join(f'{i}:{size(i)}' for i in ids), '--synth-animate', ','.join(map(str, moving)) or 'none']
        ds, created, pattern, cover = {}, [], [], []
        print(f'  synthetic source: screens {ids}, animated {moving}')
    else:
        moving = None
    if a.source == 'sck':  # the macOS screen source: virtual displays captured by ScreenCaptureKit
        # 0. keep the Air's display awake for the run: when its built-in display sleeps, WindowServer stops driving the
        #    virtual displays and they drop out of ScreenCaptureKit entirely (measured 2026-11-03 14:38 UTC: 29.5 fps
        #    awake vs "display not found" asleep). Released in teardown. -u wakes a display that already slept.
        air(f"(nohup caffeinate -u -d -t {a.secs + 900} >/dev/null 2>&1 &)")
        atexit.register(lambda: air("pkill -f 'caffeinate -u -d -t' || true"))
        # 1. displays and pattern
        have = {d['screen']: (d.get('w'), d.get('h')) for d in displays()}
        if cinema and 2 in have and have[2] != cinema:  # a leftover screen 2 of another size: recreate it
            air("curl -s -m40 -X POST 'localhost:57710/remove?id=2'", timeout=60); del have[2]
        created = [i for i in ids if i not in have]
        for i in created:
            w, h = cinema if i == 2 and cinema else (1920, 1200)
            air(f"curl -s -m40 -X POST 'localhost:57710/displays?id={i}&w={w}&h={h}&scale=1'", timeout=60)
        time.sleep(3)
        ds = {d['screen']: d['display'] for d in displays()}
        missing = [i for i in ids if i not in ds]
        if missing: print('  warning: displays not created:', missing)
        # The pattern animates only the screens the scenario shows (cinema: the focused screen and its two neighbours;
        # vr: none; wall/ring: all). With all 10 animated, WindowServer redrew each at only ~8.6 fps (work-09,
        # 2026-11-03), so earlier runs measured an overloaded source, not the sender. Hidden screens stay still, like a
        # real desktop. --static K: the last K of those keep a still desktop too.
        moving = [i for i in ids if i in shown]
        moving = moving[:max(0, len(moving) - a.static)]
        pattern = [str(ds[i]) for i in moving if i in ds]
        # --cover-idle: every other test screen gets a static full-screen window, hiding the animated aerial wallpaper.
        cover = [str(ds[i]) for i in ids if i in ds and i not in moving] if a.cover_idle else []
        if pattern or cover:
            # Run the binary directly: through `open` it got no arguments. Windows still reach the GUI session.
            air(f"(nohup {APP.replace(' ', chr(92) + ' ')}/Contents/MacOS/stresspattern {' '.join(pattern)} {('--cover ' + ','.join(cover)) if cover else ''} "
                f"> ~/.local/share/pinpin-stress/pattern.log 2>&1 &); sleep 2; cat ~/.local/share/pinpin-stress/pattern.log")
    if a.source == 'sck': print(f'  displays: {sorted(ds)} (created {created}, pattern on {len(pattern)})')

    log_path = '~/.local/share/pinpin-atlas/' + ('atlas-sim.log' if sim else 'atlas.log')
    crash_glob = 'pinpin-atlas-test-*.ips' if sim else 'pinpin-atlas-2*.ips'
    log0 = '' if sim else air(f'cat {log_path}')
    aborted = ''; st0 = time.time(); proc = None; app_was_running = True; live_paused = False; overrides = {}; cpu = []; air_proc = None
    try:
        phase('overrides')
        # 2. per-run overrides of the live plugins (restored in `finally`), before any game session starts
        if a.sunshine:
            overrides['sunshine'] = plugin_override('sunshine', a.sunshine); time.sleep(6)  # Sunshine re-probes its encoders
        if a.flags and not sim:
            overrides['pinpin-atlas'] = plugin_override('pinpin-atlas', ['--flags', a.flags]); time.sleep(3)
        phase('demand')
        # demand source
        if sim:
            # Pause the live sender so only the test copy captures (agreed with work-09); restarted in `finally`.
            air("curl -s -m5 -X POST '127.0.0.1:57790/stop?name=pinpin-atlas'"); live_paused = True
            # The test copy runs as a PinPin Host plugin, so it inherits the Host's Screen Recording grant
            # (no Terminal, no AppleScript). Its own binary name keeps the live sender's pkill away from it.
            air('cp -f ~/.local/share/pinpin-atlas/pinpin-atlas ~/.local/share/pinpin-atlas/pinpin-atlas-test && '
                f'printf %s {json.dumps(json.dumps(test_plugin))} > {HOST_PLUGINS}/pinpin-atlas-test.json')
        else:
            # Nobody is wearing it (checked above): simulate wear, start the app if needed, put it in stress mode.
            app_was_running = bool(headset(f'pidof {PKG}').strip())
            simulate_wear(True)
            if not app_was_running:
                headset(f'input keyevent KEYCODE_WAKEUP; am start -n {PKG}/.MainActivity --ez capture false >/dev/null'); time.sleep(8)
            for fl in a.headset_flag:  # polled ~1/s by the app; removed in teardown
                headset(f"run-as {PKG} touch files/{fl}-on")
            if a.headset_flag: time.sleep(2)
            if a.codec:  # before the launch below, which reads it; removed in teardown
                headset(f"run-as {PKG} sh -c '{'touch' if a.codec == 'H264' else 'rm -f'} files/codec-h264'"); time.sleep(2)  # polled ~1/s
            spec = base64.b64encode(json.dumps({'n': a.n, 'sweep': a.sweep, 'scenario': a.scenario}).encode()).decode()  # app v0.41+ reads scenario
            headset(f"run-as {PKG} sh -c 'echo {spec} | base64 -d > files/stress.json'")
            if a.game:
                headset(f"run-as {PKG} sh -c 'echo {a.game} > files/launch'"); time.sleep(12)  # session setup
        time.sleep(6)  # sender start, demand and resize settle
        if a.wan:
            # nohup: plain background jobs died with the ssh session, so the earlier --wan runs loaded nothing
            air(f"for i in $(seq {a.wan}); do nohup curl -s -o /dev/null -m {a.secs + 5} "
                f"'https://speed.cloudflare.com/__down?bytes=2000000000' > /dev/null 2>&1 & done")

        if a.encprobe:  # the game-encode stand-in (bench/encprobe.swift) on the Air: built once, inside this run's lock
            src = (HERE / 'encprobe.swift').read_text()
            built = subprocess.run(['ssh', '-o', 'BatchMode=yes', 'pinpin-air', 'B=~/.local/share/pinpin-atlas/encprobe; [ -x $B ] && echo have || '
                                    '{ cat > /tmp/encprobe.swift && swiftc -O -o $B /tmp/encprobe.swift && echo built; }'],
                                   input=src, capture_output=True, text=True, timeout=300)
            if 'have' not in built.stdout and 'built' not in built.stdout: raise SystemExit(f'encprobe build failed: {built.stderr[-300:]}')
        phase('measure')
        load_samples = {'start': air_load()}
        # 3. measure (in the background, while watching that nobody starts using the gear)
        rx0, tx0 = link_bytes(air, 'en0'); w0 = link_bytes(mini, 'en1'); st0 = time.time()
        probe = (f"(ping -b en1 -i 0.2 -t {a.secs} 1.1.1.1 > /tmp/stress-wan.txt 2>&1 &);"
                 f"(ping -i 0.2 -t {a.secs} 192.168.1.1 > /tmp/stress-zyxel.txt 2>&1 &);"
                 f"(ping -i 0.2 -t {a.secs} {HEADSET_IP} > /tmp/stress-lan.txt 2>&1 &);"
                 f"(ping -i 0.2 -t {a.secs} {lap_ip} > /tmp/stress-laptop.txt 2>&1 &);")
        air_proc = None
        if sim:
            sim_cmd = (f"python3 - {'127.0.0.1' if a.sim_on == 'air' else lap_ip} --port 57730 --demand-port 57731 --n {a.n} "
                       f"--secs {a.secs} --sweep {a.sweep} --scenario {a.scenario}")
            if 'vt' in a.flags.split(','): sim_cmd += ' --v2'  # the vt flag only acts on v2 demand
            if a.sim_extra: sim_cmd += ' ' + a.sim_extra  # e.g. "--v2 --hero --keyreq 15": the same demand for every config of an A/B
            stdin = (HERE / 'headset_sim.py').read_text()
            if a.sim_on == 'air':  # the stand-in on the laptop itself; the Mini keeps only its pings
                air_proc = subprocess.Popen(['ssh', '-o', 'BatchMode=yes', 'pinpin-air', sim_cmd], stdin=subprocess.PIPE,
                                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                air_proc.stdin.write(stdin); air_proc.stdin.close()
                probe += f"sleep {a.secs}"; stdin = None
            else:
                probe += sim_cmd
        else:
            probe += (ADB_SERVER + f"end=$(( $(date +%s) + {a.secs} )); while [ $(date +%s) -lt $end ]; do "
                      f"LC_ALL=C LANG=C perl -e 'alarm 5; exec @ARGV' {ADB} shell run-as {PKG} cat files/status.json; echo; sleep 2; done")  # 5 s cap: a sleeping headset hangs adb
            stdin = None
        proc = subprocess.Popen(['ssh', '-o', 'BatchMode=yes', 'pinpin-mini', probe], stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if stdin: proc.stdin.write(stdin)
        proc.stdin.close()
        # Drain the probe's output while it runs: a headset probe prints the app's status JSON every 2 s (kilobytes
        # each since v0.41), and an unread pipe fills at ~64 KB, blocks the remote side and the probe never ends
        # (2026-11-03: two gaming runs aborted with "probe overran").
        import threading
        drained = {}
        def drain(name, stream): drained[name] = stream.read()
        readers = [threading.Thread(target=drain, args=(n, st), daemon=True) for n, st in (('out', proc.stdout), ('err', proc.stderr))]
        for t in readers: t.start()
        idle_before = laptop_idle_s()  # logged only: no gate (see busy)
        sender = 'pinpin-atlas-test' if sim else 'pinpin-atlas'
        probe_deadline = time.time() + a.secs + 90  # a probe that overruns never ends on its own (2026-11-03: 23 min)
        enc_proc = None
        if a.encprobe:  # starts 10 s into the measurement (the atlas settled), runs encprobe seconds
            enc_proc = subprocess.Popen(['ssh', '-o', 'BatchMode=yes', 'pinpin-air', f'sleep 10; ~/.local/share/pinpin-atlas/encprobe {a.encprobe}'],
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        while proc.poll() is None:
            time.sleep(10)
            if proc.poll() is not None: break
            if time.time() > probe_deadline:
                aborted = 'the probe overran (headset or laptop unreachable)'
                proc.kill(); mini('pkill -f "ping -b en1 -i 0.2" ; pkill -f "ping -i 0.2"'); break
            # the sender's CPU (one sample per 10 s; ps %cpu is a decaying average, enough to compare flags)
            try: cpu.append(float(air(f"ps -Ao %cpu,comm | awk '$2 ~ /{sender}$/ {{print $1; exit}}'", timeout=15).strip()))
            except ValueError: pass
            idle = laptop_idle_s()
            if headset_owner() == 'user': aborted = 'the user took the headset (bench/headset_owner)'  # sim too: the live sender is paused
            elif sim and headset_in_use(): aborted = 'the headset came into use (room_lab focused); requeued'; note_in_use_wait()
            elif sim and headset_owner() == '' and not hold_says_unworn() and headset_worn(): aborted = 'someone put the headset on'
            idle_before = idle
            if aborted:
                proc.kill(); mini('pkill -f "ping -b en1 -i 0.2" ; pkill -f "ping -i 0.2"'); break
        if air_proc:
            if aborted: air_proc.kill()
            else:
                try: air_proc.wait(timeout=60)
                except subprocess.TimeoutExpired: air_proc.kill()
        for t in readers: t.join(timeout=10)
        out = (air_proc.stdout.read() if air_proc else drained.get('out', '')) if not aborted else ''
        rx1, tx1 = link_bytes(air, 'en0'); w1 = link_bytes(mini, 'en1'); dt = time.time() - st0
        wan, lan = pings(mini('cat /tmp/stress-wan.txt')), pings(mini('cat /tmp/stress-lan.txt'))
        lap = pings(mini('cat /tmp/stress-laptop.txt'))  # the stand-in's own link (the headset may be asleep: lanPingMs)
        zyxel = pings(mini('cat /tmp/stress-zyxel.txt'))  # the Mini's 2.4 GHz hop alone: link jitter vs the internet
        load_samples['end'] = air_load()
    finally:
        phase('teardown')
        # 4. teardown
        for name, orig in overrides.items(): plugin_restore(name, orig)
        if proc and proc.poll() is None: proc.kill()
        if air_proc and air_proc.poll() is None: air_proc.kill()
        if sim: air(f'printf %s {json.dumps(json.dumps(dict(TEST_PLUGIN, enabled=False)))} > {HOST_PLUGINS}/pinpin-atlas-test.json')
        if live_paused: air("curl -s -m5 -X POST '127.0.0.1:57790/start?name=pinpin-atlas'")
        else:
            headset(f'run-as {PKG} rm -f files/stress.json' + (' files/codec-h264' if a.codec else '') + ''.join(f' files/{fl}-on' for fl in a.headset_flag))
            if not app_was_running: headset(f'am force-stop {PKG}')
            simulate_wear(False)  # back to the real sensor: an unworn headset goes to sleep as usual
        if a.wan: air("pkill -f 'speed.cloudflare.com/__down' || true")
        if pattern or cover: air('pkill -f stresspattern || true')
        if not a.keep:
            for i in created: air(f"curl -s -m40 -X POST 'localhost:57710/remove?id={i}'", timeout=60)
    if aborted:
        sys.exit(f'stress: aborted after {int(time.time() - st0)} s, {aborted}; everything was cleaned up')

    crashes = air(f"find ~/Library/Logs/DiagnosticReports -name '{crash_glob}' -mmin -{math.ceil((time.time() - st0) / 60) + 1} 2>/dev/null").split()
    log1 = air(f'cat {log_path}')
    log = (log1[len(log0):] if log1.startswith(log0) else log1).splitlines()
    skipped = sorted({l.split('screen ')[1].split()[0] for l in log if 'atlas full' in l})
    bitrates = [int(l.split('bitrate ')[1].split()[0]) for l in log if 'bitrate ' in l]
    layouts = [l for l in log if 'layout ' in l]
    # The source rate: what ScreenCaptureKit delivered per screen (the sender's --stat-secs lines "capture: S F fps").
    # gate.py judges the stream relative to it: SCK delivers ~29.4 fps per display, so an absolute 30 fps is impossible.
    cap = {}
    for l in log:  # one line lists every screen: "capture: 2 10.8 fps @60, 3 10.8 fps @60, ..."
        if 'capture:' not in l: continue
        for sid, fps in re.findall(r'(?:capture: |, )(\d+) ([\d.]+) fps', l):
            if float(fps) > 0: cap.setdefault(sid, []).append(float(fps))
    capture_fps = {s: round(statistics.median(v), 1) for s, v in cap.items()}
    r = {
        'when': t0, 'n': a.n, 'secs': a.secs, 'wan': a.wan, 'mode': 'sim' if sim else 'headset', 'scenario': a.scenario + (f'@{a.cinema_size}' if a.cinema_size else '') + (f'~static{a.static}' if a.static else '') + ('~covered' if a.cover_idle else '') + ('~synth' if a.source == 'synth' else '') + ('~simair' if sim and a.sim_on == 'air' else '') + (f'+{a.flags}' if a.flags else '') + ''.join(f'+sunshine:{o}' for o in a.sunshine) + (f'+codec:{a.codec}' if a.codec else '') + ''.join(f'+headset:{fl}' for fl in a.headset_flag),
        'wanPingMs': {'p50': pct(wan, 50), 'p95': pct(wan, 95), 'p99': pct(wan, 99), 'max': max(wan, default=None), 'count': len(wan)},
        'zyxelPingMs': {'p50': pct(zyxel, 50), 'p95': pct(zyxel, 95), 'p99': pct(zyxel, 99), 'count': len(zyxel)},
        'lanPingMs': {'p50': pct(lan, 50), 'p95': pct(lan, 95), 'p99': pct(lan, 99), 'count': len(lan)},  # Mini -> headset
        'laptopPingMs': {'p50': pct(lap, 50), 'p95': pct(lap, 95), 'p99': pct(lap, 99), 'count': len(lap), 'ip': lap_ip},  # Mini -> laptop
        'houseInternetMbps': {'down': round((w1[0] - w0[0]) * 8 / 1e6 / dt, 1), 'up': round((w1[1] - w0[1]) * 8 / 1e6 / dt, 1)},
        'laptopWifiMbps': {'tx': round((tx1 - tx0) * 8 / 1e6 / dt, 1), 'rx': round((rx1 - rx0) * 8 / 1e6 / dt, 1)},
        'sender': {'bitrateKbps': bitrates[-8:], 'layouts': len(layouts), 'lastLayout': layouts[-1].split('layout ')[1] if layouts else None,
                   'skipped': skipped, 'crashes': crashes},
        'captureFps': capture_fps,
        'network': dict(network_epoch(), laptopChannel=laptop_channel),
        'phases': phases(),
        'host': dict(host, standIn=('pinpin-air' if a.sim_on == 'air' else 'pinpin-mini') if sim else 'quest'),
        # The content rate: the stress pattern redraws at the virtual displays' 60 Hz; still screens have none.
        'contentHz': 60 if (pattern or (a.source == 'synth' and moving)) else None,
        'patternScreens': moving,  # the screens animated (the source); gate.py judges fresh fps against them
        'airLoad': {k: {kk: vv for kk, vv in v.items() if kk != 'topAll'} for k, v in load_samples.items()},
        'contaminated': contamination(load_samples),  # '' = clean
    }
    r['senderCpuPct'] = round(statistics.mean(cpu), 1) if cpu else None  # the atlas sender, sampled every 10 s
    checks = {
        'internet ping p95 < 50 ms': r['wanPingMs']['p95'] is not None and r['wanPingMs']['p95'] < 50,
        'no screen skipped': not skipped,
        'sender never crashed': not crashes,
    }
    if sim:
        s = json.loads(out.strip().splitlines()[-1]) if out.strip() else {}
        r['atlas'] = {k: s.get(k) for k in ('mbps', 'mbpsMax', 'freshFps', 'freshGapMs', 'receiverCpuPct', 'longestSilenceS', 'emptySeconds', 'keyframes', 'layouts', 'connects', 'sizes', 'worstGaps')}
        if enc_proc is not None:  # the game encode next to the atlas: fps and latency p50/p99 (ms)
            try:
                eo, _ = enc_proc.communicate(timeout=a.encprobe + 60); r['encprobe'] = json.loads(eo.strip().splitlines()[-1])
            except (subprocess.TimeoutExpired, ValueError, IndexError) as ex:
                enc_proc.kill(); r['encprobe'] = {'error': str(ex)[:120]}
        if 'hero' in s: r['hero'] = s['hero']  # per hero screen: fps, mbps, keyframes, gap p99 (stand-in --hero)
        if 'keyReqs' in s: r['keyReqs'] = s['keyReqs']
        if a.sim_extra: r['simExtra'] = a.sim_extra
        r['series'] = s.get('perSecondMbps', [])
        if 'v2Layouts' in s:
            # vt: every v2 layout the stand-in saw, checked offline (bench/vt_check.py)
            sys.path.insert(0, str(ROOT / 'bench')); import vt_check
            vt_file = ROOT / 'bench' / 'results' / f'stress-{t0}-{a.scenario}+{a.flags}-n{a.n}-layouts.jsonl'
            vt_file.parent.mkdir(parents=True, exist_ok=True)
            vt_file.write_text(''.join(json.dumps(x) + '\n' for x in s['v2Layouts']))
            r['vt'] = {'v2Layouts': len(s['v2Layouts']), 'belowWantedPct': s.get('belowWantedPct'), 'layoutsFile': vt_file.name}
            checks['vt layouts valid (vt_check.py)'] = vt_check.recorded(str(vt_file))
        checks['stand-in reached the sender'] = (s.get('connects') or 0) > 0  # else every other figure is void
        checks['atlas within 40 Mbit/s'] = s.get('mbps') is not None and s['mbps'] <= 40
        checks['no atlas freeze over 5 s'] = s.get('longestSilenceS') is not None and s['longestSilenceS'] <= 5
        if a.scenario == 'vr':
            checks['VR: atlas under 1 Mbit/s'] = s.get('mbps') is not None and s['mbps'] < 1
            del checks['no atlas freeze over 5 s']  # nothing to show: silence is correct
        else:
            checks['stream kept 30+ fresh frames/s (median)'] = (s.get('freshFps') or {}).get('p50', 0) >= 30
            checks['fresh-frame gap p99 < 50 ms'] = ((s.get('freshGapMs') or {}).get('p99') or 1e9) < 50
        fps_txt, mbps, lat_txt, gap = '-', s.get('mbps'), '-', s.get('longestSilenceS')
    else:
        samples, dec = [], json.JSONDecoder()
        for line in out.splitlines():
            line = line.strip()
            if line.startswith('{'):
                try: samples.append(dec.raw_decode(line)[0])
                except ValueError: pass
        fps = [x['fps'] for x in samples]
        series = [{'t': round(x['uptime'], 1), 'fps': x['fps'], 'stressN': x['spell'].get('stressN'),
                   'bytes': x['atlas']['video']['bytes'], 'packets': x['atlas']['video']['packets'],
                   'size': x['atlas']['video']['size'], 'lat': x['atlas']['video']['latencyMs'],
                   'game': {'stage': x['moonlight']['stage'], 'lat': x['video']['latencyMs'], 'rtt': x['video']['rttMs'],
                            'displayed': x['video']['displayed'], 'dropped': x['video']['dropped'],
                            # v0.38 per-frame counters, cumulative; bench/gate.py diffs them for exact p50/p99 and stalls
                            'latHist': x['video'].get('latHist'), 'stalls50': x['video'].get('stalls50'), 'stalls100': x['video'].get('stalls100'),
                            'gapMaxMs': x['video'].get('gapMaxMs'), 'hostMs': x['video'].get('hostMs')}} for x in samples]
        lat = [x['lat'] for x in series if x['lat'] > 0]
        mbps = round((series[-1]['bytes'] - series[0]['bytes']) * 8 / 1e6 / max(1, series[-1]['t'] - series[0]['t']), 1) if len(series) > 1 else None
        gap = longest_gap(series)
        r['headsetFps'] = {'min': min(fps, default=None), 'mean': round(statistics.mean(fps), 1) if fps else None}
        r['atlas'] = {'mbps': mbps, 'latencyP50': pct(lat, 50), 'latencyP99': pct(lat, 99), 'longestGapS': gap}
        r['series'] = series
        if a.game:
            g = [x['game'] for x in series]; up = [x for x in g if x['lat'] > 0]
            span = max(1, series[-1]['t'] - series[0]['t'])
            r['game'] = {'host': a.game, 'stages': sorted({x['stage'] for x in g}),
                         'latencyP50': pct([x['lat'] for x in up], 50), 'latencyP99': pct([x['lat'] for x in up], 99),
                         'rttP50': pct([x['rtt'] for x in up], 50), 'fps': round((g[-1]['displayed'] - g[0]['displayed']) / span, 1),
                         'dropped': g[-1]['dropped'] - g[0]['dropped']}
            checks['game stream ran'] = bool(up)
            checks['game latency p99 < 60 ms'] = (r['game']['latencyP99'] or 1e9) < 60
            checks['game fps >= 55'] = r['game']['fps'] >= 55
        checks['headset >= 70 fps'] = r['headsetFps']['min'] is not None and r['headsetFps']['min'] >= 70
        checks['headset stress mode engaged'] = any(x['stressN'] == a.n for x in series)
        checks['atlas within 40 Mbit/s'] = mbps is not None and mbps <= 40
        checks['no atlas freeze over 5 s'] = gap <= 5
        fps_txt, lat_txt = r['headsetFps']['min'], f"{r['atlas']['latencyP50']}/{r['atlas']['latencyP99']}"
    r['checks'] = checks; r['pass'] = all(checks.values())

    out_dir = ROOT / 'bench' / 'results'; out_dir.mkdir(parents=True, exist_ok=True)
    result = out_dir / f'stress-{t0}-{r["scenario"]}-n{a.n}.json'
    result.write_text(json.dumps(r, indent=1))
    print(f'stress: result {result}')  # suite.sh runs bench/gate.py on it
    md = out_dir / 'stress.md'
    if not md.exists():
        md.write_text('# Stress runs\n\n')
    with md.open('a') as f:
        f.write(f"| {t0} | {r['mode']}/{r['scenario']} | {a.n} | {a.wan} | {r['wanPingMs']['p50']}/{r['wanPingMs']['p95']} | {r['lanPingMs']['p50']}/{r['lanPingMs']['p95']} | "
                f"{r['houseInternetMbps']['down']} | {fps_txt} | {mbps} | {gap} | {lat_txt} | {r['laptopWifiMbps']['tx']} | "
                f"{len(crashes)} | {'yes' if r['pass'] else 'NO'} |\n")
    print(json.dumps({k: v for k, v in r.items() if k != 'series'}, indent=1))


if __name__ == '__main__':
    main()
