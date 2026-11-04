#!/usr/bin/env python3
"""Headset stand-in for the stress test: receives the atlas stream the way the Quest does and sends the same
demand datagrams, so the sender and the network can be tested with nobody wearing the headset.

  python3 headset_sim.py HOST --port 57730 --demand-port 57731 --n 10 --secs 120 [--sweep 3] [--v2]

Demand matches room_lab.cpp stressDemand(): screens 2..N+1 on a ring at 1.5/2.5/3.5 m, a virtual gaze turning
one screen every `sweep` seconds. Stream: MacScreenSender framing, u32 big-endian length, then a packet whose
first byte is flags (bit 0 keyframe, bit 1 repeat of the previous image, 0x80 = text/layout).
It does not decode. It measures what the network delivers: per-second bytes and fresh frames, the longest
silence, keyframes and layout changes. It prints one JSON object at the end. Python 3.9 standard library only
(it runs on the Mini).

--v2 speaks virtual-texturing demand (docs/virtual-texturing.md): "v":2, and per screen "lvl" by the headset's
rule (coarsest level still >= px wide, up at once, down after 3 s, hidden = mip tail) and "have" (the level of
the screen's cells in the last layout). It then also reports the v2 layouts it saw, with the demand in force,
for bench/vt_check.py, and how often a visible screen showed less detail than it asked for.

--hero (implies --v2) sends v3 demand: "v":3, "heroOk":1, and "hero":1 on every visible screen at least 1000 px wide,
as room_lab's HERO does; hero packets (0x81, u16 screen, flags: bit 0 key, bit 2 HEVC) are counted per screen apart
from the atlas. --keyreq S bumps the demand's "keyReq" counter every S seconds (from 5 s), like a headset asking for
a keyframe after a decode error; a sender without noperiodic still sends its periodic ones. Same demand for every
config of an A/B, so the flags are the only difference (a sender without vt/hero ignores the v2/v3 fields).
"""
import argparse, json, math, socket, struct, threading, time

PX_PER_RAD = 1260.0


BASE = 2  # the first screen id (--base): 2 for the user's numbering; stress.py's sck pool starts at 80


def demand(n, sweep, t, scenario='ring'):
    """Synthetic demand for screens 2..n+1.
    ring:   screens on a ring, the gaze turning one screen every `sweep` s (adapting to head motion)
    cinema: screen 2 fills the view (~3000 px across, full priority); its two neighbours peek in at the edge, the
            rest are behind you (how most time is spent: watching or working on one big screen)
    vr:     a full VR game: no screen visible at all; the atlas should cost almost nothing
    wall:   every screen visible at once as a small tile (~500 px): the worst case for the atlas"""
    if scenario == 'cinema':
        return [{'id': BASE, 'vis': 1, 'px': 3000, 'pri': 1.0}] + [
            {'id': k + BASE, 'vis': int(k in (1, n - 1)), 'px': 300 if k in (1, n - 1) else 0, 'pri': 0.1 if k in (1, n - 1) else 0.0} for k in range(1, n)]
    if scenario == 'vr':
        return [{'id': k + BASE, 'vis': 0, 'px': 0, 'pri': 0.0} for k in range(n)]
    if scenario == 'wall':
        return [{'id': k + BASE, 'vis': 1, 'px': 500, 'pri': 1.0 if k == 0 else 0.3} for k in range(n)]
    tau = 2 * math.pi
    gaze = math.fmod(t / (sweep * n), 1.0) * tau
    out = []
    for k in range(n):
        at = tau * k / n
        off = abs(math.remainder(at - gaze, tau))
        dist = 1.5 + (k % 3)
        half = math.atan(0.7 / dist)
        vis = off - half < 0.95
        pri = 0.0 if not vis else 1.0 if off < 0.21 else max(0.1, 1.0 - (off - 0.21) / 0.75)
        out.append({'id': k + BASE, 'vis': int(vis), 'px': int(2 * half * PX_PER_RAD), 'pri': round(pri, 2)})
    return out


class V2:
    """The headset's level choice (room_lab/vt.h), per screen."""
    def __init__(self): self.cur, self.down_since, self.src_w, self.have = {}, {}, {}, {}

    def layout(self, j):
        self.have = {}
        for c in j.get('cells', []):
            if 'src' in c:
                self.src_w[c['screen']] = max(self.src_w.get(c['screen'], 0), c['src'][0] + c['src'][2])
                self.have[c['screen']] = c['lvl']

    def apply(self, screens, now):
        for d in screens:
            sid, w = d['id'], self.src_w.get(d['id'], 1920)
            tail = 0
            while (w >> tail) > 240: tail += 1
            want = tail if not d['vis'] else min(tail, max(0, int(math.floor(math.log2(w / max(d['px'], 1))))))
            cur = self.cur.get(sid, want)
            if want < cur: cur = want; self.down_since.pop(sid, None)
            elif want > cur:
                since = self.down_since.setdefault(sid, now)
                if now - since >= 3: cur = want; self.down_since.pop(sid, None)
            else: self.down_since.pop(sid, None)
            self.cur[sid] = cur
            d['lvl'] = cur; d['have'] = self.have.get(sid, -1)
        return screens


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('host'); ap.add_argument('--port', type=int, default=57730); ap.add_argument('--demand-port', type=int, default=57731)
    ap.add_argument('--base', type=int, default=2, help='first screen id (stress.py passes its sck pool start)')
    ap.add_argument('--n', type=int, default=10); ap.add_argument('--secs', type=float, default=120); ap.add_argument('--sweep', type=float, default=3)
    ap.add_argument('--budget', type=int, default=60000)
    ap.add_argument('--scenario', default='ring', choices=['ring', 'cinema', 'vr', 'wall'])
    ap.add_argument('--v2', action='store_true', help='virtual-texturing demand (lvl/have) and layout report')
    ap.add_argument('--hero', action='store_true', help='v3 demand: heroOk, hero on big visible screens (implies --v2)')
    ap.add_argument('--keyreq', type=float, default=0, help='bump keyReq every this many seconds (0: never)')
    ap.add_argument('--game', action='store_true', help='demand says a game stream is playing ("game":1), like room_lab while streaming')
    a = ap.parse_args()
    if a.hero: a.v2 = True
    global BASE; BASE = a.base
    keyreqs = [0]
    t0 = time.time(); stop = threading.Event()
    v2 = V2() if a.v2 else None; last_msg = {}; v2_layouts = []; below = [0, 0]  # visible screen-ticks below wanted, total

    def send_demand():
        u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        while not stop.is_set():
            t = time.time() - t0
            msg = {'t': int(t * 1000), 'budgetKbps': a.budget, 'screens': demand(a.n, a.sweep, t, a.scenario)}
            if v2:
                msg['v'] = 2; v2.apply(msg['screens'], t)
                if a.hero:
                    msg['v'] = 3; msg['heroOk'] = 1
                    for d in msg['screens']: d['hero'] = int(bool(d['vis']) and d['px'] >= 1000)
                for d in msg['screens']:
                    if d['vis'] and t > 5: below[1] += 1; below[0] += d['have'] > d['lvl'] or d['have'] < 0
            if a.keyreq > 0 and t > 5:
                keyreqs[0] = int((t - 5) // a.keyreq) + 1
            if a.keyreq > 0: msg['keyReq'] = keyreqs[0]
            if a.game: msg['game'] = 1
            last_msg['m'] = msg
            u.sendto(json.dumps(msg, separators=(',', ':')).encode(), (a.host, a.demand_port))
            stop.wait(0.2)
    threading.Thread(target=send_demand, daemon=True).start()

    per_sec = {}  # second -> [bytes, fresh frames, repeats]
    fresh_gaps = []; last_fresh = None
    # The 5 longest fresh-frame gaps with their context, to tell a startup gap from a layout/level switch or a
    # keyframe burst (VT runs over Wi-Fi each had one ~400 ms gap, 2026-11-03; localhost max 82 ms).
    worst = []; layout_t = key_t = None
    keyframes = layouts = connects = 0; last_layout = None; sizes = set()
    heroes = {}  # screen -> {bytes, frames, keys, hevc, last, gaps}
    last_rx = time.time(); longest_silence = 0.0; first_rx = None
    while time.time() - t0 < a.secs:
        try:
            s = socket.create_connection((a.host, a.port), timeout=3)
        except OSError:
            time.sleep(1); continue
        connects += 1
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1); s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 << 20); s.settimeout(1.0)
        buf = bytearray(); pos = 0  # parse in place; compact rarely (no per-packet copies)
        try:
            while time.time() - t0 < a.secs:
                try:
                    chunk = s.recv(1 << 20)
                except socket.timeout:
                    chunk = None
                now = time.time()
                if chunk == b'': break
                if chunk is None: continue
                if first_rx is None: first_rx = last_rx = now
                longest_silence = max(longest_silence, now - last_rx); last_rx = now
                buf += chunk
                while len(buf) - pos >= 4:
                    n = struct.unpack_from('>I', buf, pos)[0]
                    if len(buf) - pos < 4 + n: break
                    pkt = buf[pos + 4:pos + 4 + 13] if n > 13 else buf[pos + 4:pos + 4 + n]; pos += 4 + n  # header only: no payload copy
                    sec = int(now - t0); row = per_sec.setdefault(sec, [0, 0, 0]); row[0] += n + 4
                    if not pkt: continue
                    f = pkt[0]
                    if f == 0x81 and len(pkt) >= 4:  # hero stream packet: its own stats, not the atlas's
                        sid, hf = pkt[1] << 8 | pkt[2], pkt[3]
                        h = heroes.setdefault(sid, {'bytes': 0, 'frames': 0, 'keys': 0, 'hevc': False, 'last': None, 'gaps': [], 'first': now})
                        h['bytes'] += n + 4; h['frames'] += 1; h['keys'] += hf & 1; h['hevc'] = bool(hf & 4)
                        if h['last'] is not None: h['gaps'].append(now - h['last'])
                        h['last'] = now
                        continue
                    if f & 0x80:
                        txt = bytes(buf[pos - n + 1:pos]).decode('utf-8', 'replace')
                        if txt != last_layout:
                            layouts += 1; last_layout = txt; layout_t = now
                            if v2:
                                try: j = json.loads(txt)
                                except ValueError: j = {}
                                v2.layout(j)
                                if 'gen' in j and len(v2_layouts) < 300: v2_layouts.append({'layout': j, 'demand': last_msg.get('m')})
                        continue
                    if f & 1: keyframes += 1
                    if f & 2: row[2] += 1
                    else:
                        row[1] += 1
                        if last_fresh is not None:
                            fresh_gaps.append(now - last_fresh)
                            worst.append((now - last_fresh, round(now - first_rx, 2), bool(f & 1),
                                          round(now - layout_t, 2) if layout_t else None, round(now - key_t, 2) if key_t else None))
                            if len(worst) > 64: worst = sorted(worst, reverse=True)[:5]
                        if f & 1: key_t = now
                        last_fresh = now
                    if n >= 13: sizes.add(struct.unpack_from('>HH', pkt, 9))
                if pos > (4 << 20) or pos == len(buf):
                    del buf[:pos]; pos = 0
        finally:
            s.close()
    stop.set()
    secs = sorted(per_sec)[1:-1]  # drop the partial first and last seconds
    mbps = [per_sec[s][0] * 8 / 1e6 for s in secs]
    fresh = sorted(per_sec[s][1] for s in secs)
    gaps = [s for s in range(secs[0], secs[-1] + 1) if s not in per_sec] if secs else []
    print(json.dumps({
        'connects': connects, 'secondsMeasured': len(secs), 'receiverCpuPct': round(100 * time.process_time() / max(1, time.time() - t0), 1),
        'mbps': round(sum(mbps) / len(mbps), 1) if mbps else 0, 'mbpsMax': round(max(mbps), 1) if mbps else 0,
        'freshFps': {'p5': fresh[len(fresh) // 20] if fresh else 0, 'p50': fresh[len(fresh) // 2] if fresh else 0},
        'longestSilenceS': round(longest_silence, 2), 'emptySeconds': len(gaps),
        'freshGapMs': {'p50': round(sorted(fresh_gaps)[len(fresh_gaps) // 2] * 1000, 1) if fresh_gaps else None,
                       'p99': round(sorted(fresh_gaps)[int(len(fresh_gaps) * 0.99)] * 1000, 1) if fresh_gaps else None,
                       'max': round(max(fresh_gaps) * 1000, 1) if fresh_gaps else None},
        'keyframes': keyframes, 'layouts': layouts, 'lastLayout': last_layout,
        'worstGaps': [{'ms': round(g * 1000, 1), 'atS': at, 'endsOnKey': k, 'sinceLayoutS': sl, 'sincePrevKeyS': sk}
                      for g, at, k, sl, sk in sorted(worst, reverse=True)[:5]],
        'sizes': sorted(sizes)[-4:], 'perSecondMbps': [round(x, 1) for x in mbps],
        **({'v2Layouts': v2_layouts, 'belowWantedPct': round(100 * below[0] / below[1], 1) if below[1] else None} if v2 else {}),
        **({'keyReqs': keyreqs[0]} if a.keyreq > 0 else {}),
        **({'hero': {str(sid): {'fps': round(h['frames'] / max(1e-3, (h['last'] or now) - h['first']), 1), 'mbps': round(h['bytes'] * 8 / 1e6 / max(1e-3, (h['last'] or now) - h['first']), 1),
                                'keyframes': h['keys'], 'hevc': h['hevc'],
                                'gapP99Ms': round(sorted(h['gaps'])[int(len(h['gaps']) * 0.99)] * 1000, 1) if h['gaps'] else None}
                    for sid, h in heroes.items()}} if a.hero else {}),
    }))


if __name__ == '__main__':
    main()
