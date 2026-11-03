#!/usr/bin/env python3
"""Read-only network snapshots from the headset at every wake window (no wear, no state changes).

    python3 bench/headset_net.py [--hours 6] [--every 300]

Whenever adb answers, records one JSON line in bench/results/headset-net-<date>.jsonl:
  wifi      SSID, band/frequency, RSSI, link rate (cmd wifi status / dumpsys wifi)
  power     Wi-Fi power-save state (iw, via the headset's su) and the power state (awake/standby/worn-simulated)
  ping      from the headset: 25 pings at 200 ms to the laptop, the Mini and the router
  path      the headset's route and ARP entries: is the laptop reached directly (its own MAC on the LAN) or through
            a gateway; Wi-Fi client-to-client traffic always relays through the access point, so both hops share air
  peer      the laptop's Wi-Fi channel/rate (from the laptop's own system_profiler), for same-channel airtime
Then sleeps --every seconds and repeats until --hours have passed.
"""
import argparse, datetime, json, pathlib, re, statistics, subprocess, sys, time
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import stress

SU = '/debug_ramdisk/su -c'
TARGETS = {'router': '192.168.0.1'}


def sh(cmd, t=60):
    try: return stress.headset(cmd, timeout=t)
    except subprocess.TimeoutExpired: return ''


def ping_stats(out):
    ms = [float(x) for x in re.findall(r'time=([\d.]+)', out)]
    sent = re.search(r'(\d+) packets transmitted, (\d+) received', out)
    s = sorted(ms)
    return {'n': len(ms), 'sent': int(sent.group(1)) if sent else None, 'p50': s[len(s) // 2] if s else None,
            'p95': s[min(len(s) - 1, int(0.95 * len(s)))] if s else None, 'max': max(s) if s else None,
            'mean': round(statistics.mean(s), 1) if s else None}


def snapshot():
    r = {'t': datetime.datetime.now().isoformat(timespec='seconds')}
    st = sh('cmd wifi status 2>/dev/null | head -20; dumpsys wifi | grep -m1 "mWifiInfo"')
    r['wifiRaw'] = st.strip()[-900:]
    for k, pat in [('ssid', r'SSID: "?([^",]+)'), ('freqMHz', r'Frequency: (\d+)'), ('rssi', r'RSSI: (-?\d+)'),
                   ('linkMbps', r'Link speed: (\d+)'), ('txMbps', r'Tx Link speed: (\d+)'), ('rxMbps', r'Rx Link speed: (\d+)'),
                   ('standard', r'Wi-Fi standard: (\S+)')]:
        m = re.search(pat, st)
        if m: r[k] = int(m.group(1)) if m.group(1).lstrip('-').isdigit() else m.group(1)
    r['powerSave'] = sh(f"{SU} 'iw dev wlan0 get power_save' 2>&1").strip()[:80]
    r['powerState'] = sh('dumpsys vrpowermanager | grep -m1 "^State:"').strip()
    r['lowLatency'] = sh('dumpsys wifi | grep -iE -m3 "low.?latency"').strip()[:300]
    route = sh('ip route; ip neigh show dev wlan0')
    r['route'] = route.strip()[:600]
    gw = re.search(r'default via (\S+)', route)
    targets = dict(TARGETS); targets['gateway'] = gw.group(1) if gw else None
    targets['laptop'] = stress.laptop_ip()  # DHCP: asked each time
    targets['mini'] = '192.168.0.50'      # the Mini on the house LAN (en0); en1 is its 2.4 GHz uplink
    r['retries'] = sh('dumpsys wifi | grep -E -m3 "^(successfulTxPackets|retriedTxPackets|lostTxPackets):"').strip()
    r['ping'] = {}
    for name, ip in targets.items():
        if ip: r['ping'][name] = dict(ping_stats(sh(f'ping -c 25 -i 0.2 -W 1 {ip}', 40)), ip=ip)
    r['laptopDirect'] = bool(re.search(re.escape(targets['laptop']) + r' .*lladdr', route))
    peer = stress.air("system_profiler SPAirPortDataType 2>/dev/null | grep -A8 'Current Network Information' | grep -E 'Channel|PHY|Transmit Rate|Signal' ", timeout=40)
    r['laptopWifi'] = ' | '.join(l.strip() for l in peer.splitlines())[:300]
    return r


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--hours', type=float, default=6); ap.add_argument('--every', type=float, default=300)
    a = ap.parse_args()
    out = pathlib.Path(__file__).resolve().parent / 'results' / f'headset-net-{datetime.date.today()}.jsonl'
    end = time.time() + a.hours * 3600; n = 0
    while time.time() < end:
        try:
            up = stress.mini(f"{stress.ADB.split(' -s ')[0]} connect 127.0.0.1:26351 >/dev/null 2>&1; {stress.ADB} get-state 2>/dev/null", timeout=40).strip() == 'device'
        except subprocess.TimeoutExpired: up = False
        state = sh('dumpsys vrpowermanager | grep -m1 "^State:"', 20).strip() if up else ''
        # Only an awake headset says anything about the network (asleep, its Wi-Fi power save adds 50-100 ms).
        if state and 'STANDBY' not in state and 'SLEEP' not in state:
            r = snapshot(); r['complete'] = bool(r.get('powerState')) and all(v['n'] for v in r['ping'].values()); n += 1
            with out.open('a') as f: f.write(json.dumps(r) + '\n')
            print(json.dumps({k: r.get(k) for k in ('t', 'ssid', 'freqMHz', 'rssi', 'linkMbps', 'powerSave', 'powerState', 'laptopDirect')}),
                  {k: (v['p50'], v['p95'], v['n']) for k, v in r['ping'].items()}, flush=True)
            time.sleep(a.every)
        else:
            time.sleep(30)
    print(f'{n} snapshots in {out}')


if __name__ == '__main__':
    main()
