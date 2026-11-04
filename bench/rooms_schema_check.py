"""MULTIROOM's house writes (rooms[], links[]) must pass house/schema.py: the doc bench/rooms_house_test.cpp leaves
after applying its link edits (C++ output, Python validator), plus a link to an unknown room that must be refused.
    python3 bench/rooms_schema_check.py out.json"""
import copy
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'house'))
import schema  # noqa: E402

d = json.load(open(sys.argv[1])); d.setdefault('by', 'test'); d.setdefault('updated', '2026-11-04T07:00:00Z')
problems = schema.validate(d)
if not d.get('links'): problems.append('no links written')
bad = copy.deepcopy(d); bad['links'][0]['b'] = 'r9'
if not schema.validate(bad): problems.append('a link to an unknown room was accepted')
print('\n'.join(problems) if problems else f'{len(d["links"])} MULTIROOM link(s) from rooms_house.h valid in house/schema.py')
sys.exit(1 if problems else 0)
