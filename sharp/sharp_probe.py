#!/usr/bin/env python3
"""Test client for the sharp-text sender: rebuilds the refined image and reports coverage and traffic."""
import argparse, select, socket, struct, time, zlib


def png(path, w, h, rgb):
    raw = b''.join(b'\0' + bytes(rgb[y * w * 3:(y + 1) * w * 3]) for y in range(h))
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('host'); p.add_argument('--port', type=int, default=57700)
    p.add_argument('--seconds', type=float, default=10); p.add_argument('--png'); p.add_argument('--size', help='request a capture size, e.g. 2880x1800')
    a = p.parse_args()
    s = socket.create_connection((a.host, a.port), timeout=10); s.settimeout(None)
    if a.size: s.sendall(b'PPSZ' + struct.pack('<HH', *map(int, a.size.split('x'))))
    t0 = time.time(); buf = bytearray()

    def read(k):
        while len(buf) < k:
            left = a.seconds - (time.time() - t0)
            if left <= 0 or not select.select([s], [], [], left)[0]: return None
            chunk = s.recv(1 << 20)
            if not chunk: return None
            buf.extend(chunk)
        out = bytes(buf[:k]); del buf[:k]; return out

    magic, ver, w, h, tile = struct.unpack('<4sBHHH', read(11))
    assert magic == b'PPSH' and ver == 1, (magic, ver)
    tx, ty = -(-w // tile), -(-h // tile)
    img = bytearray(w * h * 3); valid = [False] * (tx * ty)
    n = {'tiles': 0, 'invalid': 0, 'bytes': 11, 'pings': 0, 'first_full': None}
    while True:
        head = read(5)
        if head is None: break
        kind, size = struct.unpack('<BI', head); body = read(size)
        if body is None: break
        n['bytes'] += 5 + size
        if kind == 1:
            for i in struct.unpack_from('<%dH' % struct.unpack_from('<H', body)[0], body, 2): valid[i] = False; n['invalid'] += 1
        elif kind == 2:
            idx, raw_len = struct.unpack_from('<HI', body)
            data = body[6:] if len(body) - 6 == raw_len else zlib.decompress(body[6:], -15)
            assert len(data) == raw_len, (idx, len(data), raw_len)
            x0, y0 = idx % tx * tile, idx // tx * tile; cw = min(tile, w - x0); ch = raw_len // (cw * 3)
            for y in range(ch): img[((y0 + y) * w + x0) * 3:((y0 + y) * w + x0 + cw) * 3] = data[y * cw * 3:(y + 1) * cw * 3]
            valid[idx] = True; n['tiles'] += 1
            if n['first_full'] is None and all(valid): n['first_full'] = round(time.time() - t0, 3)
        elif kind == 3: n['pings'] += 1
    cov = sum(valid) / len(valid)
    print(f'{w}x{h} tile {tile}: coverage {cov:.1%}, tiles {n["tiles"]}, invalidations {n["invalid"]}, '
          f'{n["bytes"] / 1e6:.2f} MB, full screen after {n["first_full"]} s, pings {n["pings"]}')
    if a.png: png(a.png, w, h, img)


if __name__ == '__main__':
    main()
