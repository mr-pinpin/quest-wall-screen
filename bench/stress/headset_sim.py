#!/usr/bin/env python3
"""Headset stand-in for the stress test: receives the atlas stream the way the Quest does and sends the same
demand datagrams, so the sender and the network can be tested with nobody wearing the headset.

  python3 headset_sim.py HOST --port 57730 --demand-port 57731 --n 10 --secs 120 [--sweep 3]

Demand matches room_lab.cpp stressDemand(): screens 2..N+1 on a ring at 1.5/2.5/3.5 m, a virtual gaze turning
one screen every `sweep` seconds. Stream: MacScreenSender framing, u32 big-endian length, then a packet whose
first byte is flags (bit 0 keyframe, bit 1 repeat of the previous image, 0x80 = text/layout).
It does not decode. It measures what the network delivers: per-second bytes and fresh frames, the longest
silence, keyframes and layout changes. It prints one JSON object at the end. Python 3.9 standard library only
(it runs on the Mini).
"""
import argparse, json, math, socket, struct, threading, time

PX_PER_RAD = 1260.0


def demand(n, sweep, t):
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
        out.append({'id': k + 2, 'vis': int(vis), 'px': int(2 * half * PX_PER_RAD), 'pri': round(pri, 2)})
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('host'); ap.add_argument('--port', type=int, default=57730); ap.add_argument('--demand-port', type=int, default=57731)
    ap.add_argument('--n', type=int, default=10); ap.add_argument('--secs', type=float, default=120); ap.add_argument('--sweep', type=float, default=3)
    ap.add_argument('--budget', type=int, default=60000)
    a = ap.parse_args()
    t0 = time.time(); stop = threading.Event()

    def send_demand():
        u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        while not stop.is_set():
            t = time.time() - t0
            msg = {'t': int(t * 1000), 'budgetKbps': a.budget, 'screens': demand(a.n, a.sweep, t)}
            u.sendto(json.dumps(msg, separators=(',', ':')).encode(), (a.host, a.demand_port))
            stop.wait(0.2)
    threading.Thread(target=send_demand, daemon=True).start()

    per_sec = {}  # second -> [bytes, fresh frames, repeats]
    keyframes = layouts = connects = 0; last_layout = None; sizes = set()
    last_rx = time.time(); longest_silence = 0.0; first_rx = None
    while time.time() - t0 < a.secs:
        try:
            s = socket.create_connection((a.host, a.port), timeout=3)
        except OSError:
            time.sleep(1); continue
        connects += 1
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1); s.settimeout(1.0)
        buf = b''
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
                while len(buf) >= 4:
                    n = struct.unpack('>I', buf[:4])[0]
                    if len(buf) < 4 + n: break
                    pkt, buf = buf[4:4 + n], buf[4 + n:]
                    sec = int(now - t0); row = per_sec.setdefault(sec, [0, 0, 0]); row[0] += n + 4
                    if not pkt: continue
                    f = pkt[0]
                    if f & 0x80:
                        txt = pkt[1:].decode('utf-8', 'replace')
                        if txt != last_layout: layouts += 1; last_layout = txt
                        continue
                    if f & 1: keyframes += 1
                    if f & 2: row[2] += 1
                    else: row[1] += 1
                    if len(pkt) >= 13: sizes.add(struct.unpack('>HH', pkt[9:13]))
        finally:
            s.close()
    stop.set()
    secs = sorted(per_sec)[1:-1]  # drop the partial first and last seconds
    mbps = [per_sec[s][0] * 8 / 1e6 for s in secs]
    fresh = sorted(per_sec[s][1] for s in secs)
    gaps = [s for s in range(secs[0], secs[-1] + 1) if s not in per_sec] if secs else []
    print(json.dumps({
        'connects': connects, 'secondsMeasured': len(secs),
        'mbps': round(sum(mbps) / len(mbps), 1) if mbps else 0, 'mbpsMax': round(max(mbps), 1) if mbps else 0,
        'freshFps': {'p5': fresh[len(fresh) // 20] if fresh else 0, 'p50': fresh[len(fresh) // 2] if fresh else 0},
        'longestSilenceS': round(longest_silence, 2), 'emptySeconds': len(gaps),
        'keyframes': keyframes, 'layouts': layouts, 'lastLayout': last_layout,
        'sizes': sorted(sizes)[-4:], 'perSecondMbps': [round(x, 1) for x in mbps],
    }))


if __name__ == '__main__':
    main()
