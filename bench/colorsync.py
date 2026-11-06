#!/usr/bin/env python3
"""colorsync load on the Air (K23/K24): CPU of colorsyncd + colorsync.displayservices as % of one core, measured over a
window from two `ps -o time=` readings, and the decisions built on it (bench/colorsync_reset.sh, the air_smoke gate).

Why: every virtual display CREATED into a running colorsync raises its polling for good (6 pool creates at 2026-11-06
17:21Z took it from 45% to 100% of a core, flat through every later run); runs themselves do not move it. Restarting
the colorsync processes resets it (16:20Z: 31%, the post-boot level). So: create the pool, then restart colorsync once
before a campaign, and again between blocks only when it is above 40%.

  python3 bench/colorsync.py remote [WINDOW]    # the read-only sampler to run on the Air (~WINDOW s, default 10)
  python3 bench/colorsync.py pct < sample       # sample = that sampler's output -> prints the % ("?" if unreadable)
  python3 bench/colorsync.py over MAX < sample  # exit 0 if above MAX, 1 if not, 2 if unreadable (prints the %)
"""
import re
import sys

PROCS = 'colorsyncd|colorsync.displayservices'


def remote(window=10):
    one = f'p=$(pgrep -d, -f "{PROCS}"); echo T $(date +%s); [ -n "$p" ] && ps -o time= -p "$p"'
    return f'{one}; sleep {int(window)}; {one}; true'


def seconds(t):
    """ps time: [[dd-]hh:]mm:ss[.cc] -> seconds"""
    t = t.strip()
    days = 0
    if '-' in t:
        d, t = t.split('-', 1)
        days = int(d)
    s = 0.0
    for x in t.split(':'):
        s = s * 60 + float(x)
    return days * 86400 + s


def parse(text):
    """-> [(epoch, total CPU seconds of the colorsync processes)], one per "T" block"""
    out = []
    for line in text.splitlines():
        line = line.strip()
        m = re.match(r'^T (\d+)$', line)
        if m:
            out.append([int(m.group(1)), 0.0])
        elif out and re.match(r'^[\d:.\-]+$', line):
            out[-1][1] += seconds(line)
    return [tuple(x) for x in out]


def pct(text):
    """CPU % of one core between the first and the last reading, or None. A restart inside the window (the total
    falls) is None too: the window straddles two processes."""
    r = parse(text)
    if len(r) < 2 or r[-1][0] <= r[0][0] or r[-1][1] < r[0][1]:
        return None
    return (r[-1][1] - r[0][1]) * 100.0 / (r[-1][0] - r[0][0])


def main(argv):
    if argv[:1] == ['remote']:
        print(remote(argv[1] if len(argv) > 1 else 10))
        return 0
    if argv[:1] not in (['pct'], ['over']) or (argv[0] == 'over' and len(argv) != 2):
        print(__doc__)
        return 2
    v = pct(sys.stdin.read())
    print('?' if v is None else f'{v:.0f}')
    if v is None:
        return 2
    return 0 if argv[0] == 'pct' or v > float(argv[1]) else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
