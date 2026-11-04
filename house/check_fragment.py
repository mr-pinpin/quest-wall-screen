"""Validate shapes written by bench/wallshape_test.cpp against house/schema.py (C++ output, Python validator).
    python3 house/check_fragment.py shapes.json"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import schema  # noqa: E402

frag = json.load(open(sys.argv[1]))
d = schema.empty(); d['by'] = 'test'; d['rooms'] = [{'id': 'r1'}]
d['walls'] = [{'id': w, 'room': 'r1', 'kind': 'floor' if w == 'floor' else 'wall', 'source': 'manual', 'scene': []} for w in frag['walls']]
d['shapes'] = frag['shapes']
problems = schema.validate(d)
print('\n'.join(problems) if problems else f'{len(d["shapes"])} shapes from wallshape.h valid')
sys.exit(1 if problems else 0)
