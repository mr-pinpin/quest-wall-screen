"""Cross-check fixtures for room_lab/consolidate.h (K27): runs work-34's house/consolidate.py on each fixture and writes,
per case, the input house doc and what the Python gives: the segments and dropped walls after phase 3 (with Meta's
rooms), and consolidate()'s plan walls and rooms. bench/consolidate_test.cpp then runs the C++ and compares.
    python3 bench/consolidate_xcheck.py OUT"""
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'house'))
import consolidate as C  # noqa: E402
import test_consolidate as T  # noqa: E402

assert C.ALGO == 'consolidate/1', 'consolidate.py moved to %s: port room_lab/consolidate.h first' % C.ALGO


def stage(doc):
    P = dict(C.PARAMS); dropped = []
    raw = C.surfaces(doc, dropped)
    surfs, batches, rf, rc = C.frames(raw, P, dropped)
    th, share = C.axes_peak(surfs, P)
    F = C.Frame2(th)
    segs, off = C.make_segments(surfs, F, P, dropped)
    segs = C.merge_colinear(segs, F, P)
    segs = C.suppress_stacks(segs, P, dropped, F)
    segs = C.clip_corners(segs, P, dropped)
    scene = [s for s in segs if C._scene(s)]
    needs = bool(scene) and not C.meta_cycles([m for s in scene for m in s['members']] + list(off))
    segs, _ = C.outside_scene_rooms(segs, off, F, P, dropped)
    plan = C.consolidate(json.loads(json.dumps(doc)))
    return {'walls': [{k: w.get(k) for k in ('id', 'pose', 'w', 'h', 'room', 'src', 'from', 'offAxis')} for w in plan['walls']],
            'rooms': [{k: r.get(k) for k in ('id', 'floor', 'area', 'floorY', 'ceilingY', 'closed', 'closure', 'perimeter', 'openM', 'doors')} for r in plan['rooms']],
            'th': th, 'share': share, 'needsRooms': needs,
            'segs': [{'k': s['k'], 'off': s['off'], 't0': s['t0'], 't1': s['t1'], 'y0': s['y0'], 'y1': s['y1'],
                      'ids': [m['id'] for m in s['members']]} for s in segs],
            'offaxis': [s['id'] for s in off], 'dropped': [[d['id'], d['why']] for d in dropped]}


def main():
    out = sys.argv[1]; os.makedirs(out, exist_ok=True)
    cases = {'house-rev25': json.load(open(os.path.join(ROOT, 'house/fixtures/house-rev25.json'))),
             'depth-72504684': json.load(open(os.path.join(ROOT, 'house/fixtures/depth-72504684/house.json')))}
    for seed in range(1, 9):
        cases['flat-%d' % seed] = T.flat(seed)
    cases['flat-rotated'] = T.flat(3, rotated_batch=True)
    cases['flat-open'] = T.flat(4, drop_east=True)
    names = []
    for name, doc in cases.items():
        json.dump(doc, open(os.path.join(out, name + '.doc.json'), 'w'))
        json.dump(stage(json.loads(json.dumps(doc))), open(os.path.join(out, name + '.want.json'), 'w'))
        names.append(name)
    open(os.path.join(out, 'cases.txt'), 'w').write('\n'.join(names) + '\n')
    print('%d cases' % len(names))


main()
