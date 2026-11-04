"""room_lab/flags.json against the code: every flag in the code is registered and every registered flag exists, the
defaults match the code, states/categories are valid, and the evidence (commits, result files) exists.
    python3 bench/flags_check.py"""
import json
import os
import re
import subprocess
import sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
read = lambda p: open(os.path.join(root, p)).read()
reg = json.loads(read('room_lab/flags.json'))
cpp = read('room_lab/room_lab.cpp') + read('room_lab/panel_layer.h')
swift = read('atlas/AtlasSender.swift')
problems = []

# headset: the saved toggles table {"NAME", var} plus the menu-only items with their own variables
table = dict(re.findall(r'\{"([A-Z0-9]+)", ([A-Za-z_.]+)\}', cpp[cpp.index('const std::pair<const char*, bool> t[]'):][:2000]))
table.update({'SAVESET': 'saveSet', 'RESUME': 'resumeOn', 'MIPQ': 'mipqMenu'})
sender = set(re.findall(r'flags\.contains\("([a-z0-9]+)"\)', swift))

def code_default(var):
    m = re.search(r'\b' + re.escape(var.split('.')[-1]) + r'\s*=\s*(true|false)\b', cpp)
    return None if not m else m.group(1) == 'true'

names = set()
for f in reg['flags']:
    n, where = f['name'], f.get('where')
    names.add((where, n))
    if f.get('state') not in reg['states']: problems.append(f'{n}: state {f.get("state")!r}')
    if f.get('category') not in reg['categories']: problems.append(f'{n}: category {f.get("category")!r}')
    if not f.get('reason', '').strip(): problems.append(f'{n}: no reason')
    if (f['state'] == 'on') != bool(f.get('default')): problems.append(f'{n}: state {f["state"]} but default {f.get("default")}')
    if where == 'headset':
        if n not in table: problems.append(f'{n}: registered but not a headset toggle in room_lab.cpp')
        else:
            d = code_default(table[n])
            if d is None: problems.append(f'{n}: default of {table[n]} not found in the code')
            elif d != f['default']: problems.append(f'{n}: registry default {f["default"]}, code default {d} ({table[n]})')
    elif where == 'sender':
        if n not in sender: problems.append(f'{n}: registered but not a sender --flags flag')
        elif f['default']: problems.append(f'{n}: sender flags are opt-in (--flags), default must be false')
    else: problems.append(f'{n}: where {where!r}')
    for e in f.get('evidence', []):
        if 'commit' in e and subprocess.run(['git', 'cat-file', '-e', e['commit'] + '^{commit}'], cwd=root, capture_output=True).returncode:
            problems.append(f'{n}: evidence commit {e["commit"]} not in the repo')
        if 'result' in e and not os.path.exists(os.path.join(root, e['result'])):
            problems.append(f'{n}: evidence file {e["result"]} missing')
for n in table:
    if ('headset', n) not in names: problems.append(f'{n}: headset toggle in the code but not in flags.json')
for n in sender:
    if ('sender', n) not in names: problems.append(f'{n}: sender flag in the code but not in flags.json')
print('\n'.join(problems) if problems else f'{len(reg["flags"])} flags registered, all match the code')
sys.exit(1 if problems else 0)
