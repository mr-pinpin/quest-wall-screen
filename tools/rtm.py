#!/usr/bin/env python3
"""Realtek/boa (Electra F30AX, 192.168.0.1) router helper: login + read/modify Wi-Fi forms.

Password is NEVER stored here. It is read at runtime from:
  env RT_PW, or the first line of stdin.
Login algorithm (postSecurityFlag = 16-bit one's-complement checksum of the body
prefix) reverse-engineered by work-09 2026-11-02; cross-checked against /tmp/rtm.py.

Usage (run on a host that reaches 192.168.0.1, e.g. the wired Mini):
  echo "$PW" | python3 rtm_tool.py dump <wlan_idx>
  echo "$PW" | python3 rtm_tool.py apply5g <wlan_idx> --set name=value [--set ...] [--commit]
Without --commit, apply5g prints the exact POST body and the before/after diff and does NOT post.
"""
import re, subprocess, urllib.parse, base64, sys, os, time

G = 'http://192.168.0.1'

def enc(v):
    return urllib.parse.quote(str(v), safe="-_.*").replace('%20', '+')

def to32(x):
    x &= 0xffffffff
    return x - (1 << 32) if x & 0x80000000 else x

def csum(s):
    c = 0; i = 0; n = len(s)
    while i < n:
        if i + 4 > n:
            if i < n:     c += to32(ord(s[i]) << 24)
            if i + 1 < n: c += to32(ord(s[i+1]) << 16)
            if i + 2 < n: c += to32(ord(s[i+2]) << 8)
            break
        c += to32(ord(s[i]) << 24) + to32(ord(s[i+1]) << 16) + to32(ord(s[i+2]) << 8) + ord(s[i+3]); i += 4
    c32 = to32(int(c))
    c = (c32 & 0xffff) + (c32 >> 16)
    c = to32(c) & 0xffff
    return (~c) & 0xffff

def body_str(fields, token):
    s = ''.join(f'{k}={enc(v)}&' for k, v in fields)
    return s + f'postSecurityFlag={csum(s)}&csrftoken={token}'

def get(p):
    return subprocess.run(['curl', '-s', '--max-time', '8', G + p],
                          capture_output=True).stdout.decode('utf8', 'replace')

def post(path, fields, token):
    body = body_str(fields, token)
    r = subprocess.run(['curl', '-s', '-i', '--max-time', '20', '-e', G + '/',
                        '-H', 'Origin: ' + G, '--data-binary', '@-', G + path],
                       input=body.encode(), capture_output=True)
    return body, r.stdout.decode('utf8', 'replace')

def compute_rates(band_value, band2g5gsupport, wifitest=0):
    """Replicate wlbasic.asp's checkFirst(): basic/oper rate masks from the band.
    Posting the static basicrates=0/operrates=0 makes hostapd fail 'prepare rates
    table' and the radio stays down. band 203 -> (0x1f0=496, 0xff0=4080)."""
    band = int(band_value) + 1
    b2g = int(band2g5gsupport or 2)
    basicRate = 0
    operRate = 0
    if band & 1:
        basicRate |= 0xf; operRate |= 0xf
    if band & 2:
        operRate |= 0xff0
        if not (band & 1):
            basicRate = 0x15f if wifitest else 0xf
    if band & 4:
        operRate |= 0xff0
        basicRate = 0x15f if wifitest else 0x1f0
    if band & 8:
        if not (band & 3):
            operRate |= 0xff0
        if (band & 1) or (band & 2):
            basicRate = 0xf
        elif band & 4:
            basicRate = 0x1f0
        elif b2g == 1:
            basicRate = 0xf
        else:
            basicRate = 0x1f0
    if (band & 64) or (band & 128):
        basicRate = 0xf if b2g == 1 else 0x1f0
        operRate |= 0xff0
    operRate |= basicRate
    return basicRate, operRate

def js_disabled_fields(html):
    """Names that page JS disables at load (document.wlanSetup.X.disabled = true);
    the browser does not submit these, so neither should we (e.g. wl_stanum)."""
    return set(re.findall(r'document\.wlanSetup\.(\w+)\.disabled\s*=\s*true', html))

def token_of(html):
    m = re.search(r"csrftoken'\s*value='(\w+)", html) or re.search(r'csrftoken"\s*value="(\w+)', html)
    return m.group(1) if m else None

def login(pw):
    h = get('/admin/login.asp'); t = token_of(h)
    if not t:
        return False, 'no csrftoken on login page'
    time.sleep(1)
    ep = base64.b64encode(pw.encode()).decode()
    _, r = post('/boaform/admin/formLogin',
                [('challenge', ''), ('username', 'admin'), ('save', 'Вход'),
                 ('encodePassword', ep), ('submit-url', '/admin/login.asp')], t)
    time.sleep(1.5)
    return True, r.split('\n', 1)[0].strip()

def parse_form(html):
    """Return (form_action, ordered list of field dicts) for the first form with inputs.
    Scripts are stripped first so fields that exist only inside document.write() strings
    (e.g. ssidpri, written only when isCMCCSupport==1) are never treated as real."""
    html = re.sub(r'<script\b.*?</script>', '', html, flags=re.S | re.I)
    best = None
    for m in re.finditer(r'<form\b([^>]*)>(.*?)</form>', html, re.S | re.I):
        attrs, inner = m.group(1), m.group(2)
        action = (re.search(r'action=["\']?([^"\'\s>]+)', attrs, re.I) or [None, ''])[1]
        fields = []
        # Walk inputs and selects in DOM order.
        for e in re.finditer(r'<input\b[^>]*>|<select\b[^>]*>.*?</select>', inner, re.S | re.I):
            tag = e.group(0)
            head = tag.split('>', 1)[0]
            nm = re.search(r'name=["\']?([\w\[\].\-]+)', head, re.I)
            if not nm:
                continue
            name = nm.group(1)
            disabled = 'disabled' in head.lower()
            if tag.lower().startswith('<select'):
                opts = []
                sel = None
                for om in re.finditer(r'<option\b([^>]*)>(.*?)</option>', tag, re.S | re.I):
                    oa = om.group(1)
                    ov = re.search(r'value=["\']?([^"\'>\s]*)', oa, re.I)
                    ov = ov.group(1) if ov else om.group(2).strip()
                    label = re.sub(r'\s+', ' ', om.group(2)).strip()
                    is_sel = 'selected' in oa.lower()
                    opts.append((ov, label, is_sel))
                    if is_sel and sel is None:
                        sel = ov
                if sel is None and opts:
                    sel = opts[0][0]
                fields.append(dict(name=name, kind='select', value=(sel or ''),
                                   disabled=disabled, options=opts))
            else:
                ty = (re.search(r'type=["\']?(\w+)', head, re.I) or [None, 'text'])[1].lower()
                vm = re.search(r'value=(?:"([^"]*)"|\'([^\']*)\'|([^\s>]+))', head, re.I)
                val = (vm.group(1) or vm.group(2) or vm.group(3)) if vm else ''
                checked = 'checked' in head.lower()
                fields.append(dict(name=name, kind=ty, value=val,
                                   disabled=disabled, checked=checked))
        if fields and (best is None or len(fields) > len(best[1])):
            best = (action, fields)
    return best if best else ('', [])

def build_fields(fields, overrides, skip=()):
    """Build ordered (name,value) list per the firmware's rules; apply overrides by name."""
    out = []
    for f in fields:
        if f['disabled']:
            continue
        name = f['name']
        if name in skip or name in ('postSecurityFlag', 'csrftoken'):
            continue  # meta fields are appended by body_str(), never from the form
        k = f['kind']
        if k in ('checkbox', 'radio'):
            if not f.get('checked'):
                continue
            val = f['value'] or 'on'
        elif k in ('submit', 'button', 'reset', 'image'):
            if name != 'save':
                continue
            val = f['value']
        else:
            val = f['value']
        if name in overrides:
            val = overrides[name]
        out.append((name, val))
    return out

def cmd_dump(idx):
    get(f'/boaform/admin/formWlanRedirect?redirect-url=/admin/wlbasic.asp&wlan_idx={idx}')
    time.sleep(0.5)
    html = get(f'/admin/wlbasic.asp')
    open(f'/tmp/wlbasic-{idx}.html', 'w').write(html)
    final_login = 'login' in html.lower() and 'wlan' not in html.lower()
    print('csrftoken:', token_of(html), '| saved /tmp/wlbasic-%s.html len %d' % (idx, len(html)))
    if 'login.asp' in html and len(html) < 4000:
        print('!! looks like NOT logged in')
    action, fields = parse_form(html)
    print('form action:', action, '| fields:', len(fields))
    for f in fields:
        tag = f['kind'] + ('/disabled' if f['disabled'] else '') + ('/checked' if f.get('checked') else '')
        print(f"  {f['name']:<22} [{tag:<16}] = {f['value']!r}")
        if f['kind'] == 'select' and re.search(r'chan|width|band|wid|freq|side|mode', f['name'], re.I):
            for ov, lab, s in f['options']:
                print(f"        option value={ov!r} label={lab!r}{' <SELECTED' if s else ''}")

def cmd_reboot(commit):
    html = get('/admin/reboot.asp')
    tok = token_of(html)
    if not tok:
        print('ERROR: no token on reboot.asp (not logged in?)'); return 2
    fields = [('reboot', 'Reboot'), ('submit-url', '/reboot.asp')]  # plain Reboot button; NOT reset
    body = body_str(fields, tok)
    print('reboot POST body:', body)
    if not commit:
        print('[dry-run] not rebooting. add --commit.'); return 0
    print('[commit] POST /boaform/admin/formReboot ...')
    _, resp = post('/boaform/admin/formReboot', fields, tok)
    print('resp head:', resp.split(chr(10), 1)[0].strip())
    return 0

def cmd_apply5g(idx, sets, commit, skip=()):
    get(f'/boaform/admin/formWlanRedirect?redirect-url=/admin/wlbasic.asp&wlan_idx={idx}')
    time.sleep(0.5)
    html = get('/admin/wlbasic.asp')
    tok = token_of(html)
    action, fields = parse_form(html)
    if not tok or not fields:
        print('ERROR: not logged in or no form (token=%r fields=%d)' % (tok, len(fields))); return 2
    names = {f['name'] for f in fields}
    for k in sets:
        if k not in names:
            print('ERROR: field %r not present in form; aborting' % k); return 2
    skip = set(skip) | js_disabled_fields(html)   # browser omits JS-disabled inputs (wl_stanum, ...)
    after = build_fields(fields, sets, skip)
    # Recompute the hostapd rate masks from the band, exactly like the page JS.
    d = dict(after)
    band_val = d.get('band')
    if band_val is not None:
        br, orr = compute_rates(band_val, d.get('Band2G5GSupport', '2'))
        after = [(k, (str(br) if k == 'basicrates' else str(orr) if k == 'operrates' else v))
                 for k, v in after]
        print(f'rates: band={band_val} -> basicrates={br} operrates={orr}')
    before = build_fields(fields, {}, skip)
    print('csrftoken:', tok, '| action:', action, '| auto-skip:', sorted(js_disabled_fields(html)))
    print('--- changed fields (before -> after) ---')
    bdict = dict(before)
    for k, v in sets.items():
        print(f'  {k}: {bdict.get(k)!r} -> {v!r}')
    body = body_str(after, tok)
    print('--- POST body ---'); print(body)
    if not commit:
        print('[dry-run] not posting. add --commit to apply.'); return 0
    print('[commit] posting to /boaform/admin/formWlanSetup ...')
    posted, resp = post('/boaform/admin/formWlanSetup', after, tok)
    print('--- response head ---'); print('\n'.join(resp.splitlines()[:12]))
    return 0

def cmd_postcheck(idx):
    """Read hostapd errors from syslog and the driver radio state after an apply."""
    syslog = get('/admin/syslog.asp')
    errs = [l.strip() for l in re.findall(r'[^\n<>]*hostapd[^\n<>]*', syslog, re.I)][-12:]
    print('--- hostapd syslog lines ---')
    print('\n'.join(errs) if errs else '(none found)')
    if re.search(r'prepare rates table|Failed to|rates table', syslog, re.I):
        print('!! rates-table / hostapd failure present in syslog')
    get(f'/boaform/admin/formWlanRedirect?redirect-url=/admin/wlstatus.asp&wlan_idx={idx}')
    st = get(f'/admin/wlstatus.asp?wlan_idx={idx}')
    for m in re.findall(r'(state_drv|channel_drv|ssid_drv)\[\d+\]\s*=\s*[^\n;]+', st):
        print('wlstatus:', m.strip())
    return 0

def main():
    pw = os.environ.get('RT_PW') or (sys.stdin.readline().strip() if not sys.stdin.isatty() else '')
    if not pw:
        print('no password (set RT_PW or pipe on stdin)'); return 2
    argv = sys.argv[1:]
    if not argv:
        print(__doc__); return 2
    ok, msg = login(pw)
    print('login:', ok, msg)
    cmd = argv[0]
    if cmd == 'dump':
        cmd_dump(argv[1] if len(argv) > 1 else '0')
    elif cmd == 'reboot':
        return cmd_reboot('--commit' in argv)
    elif cmd == 'postcheck':
        cmd_postcheck(argv[1] if len(argv) > 1 else '0')
    elif cmd == 'apply5g':
        idx = argv[1]
        sets = {}
        skip = []
        commit = False
        i = 2
        while i < len(argv):
            if argv[i] == '--set':
                k, v = argv[i+1].split('=', 1); sets[k] = v; i += 2
            elif argv[i] == '--skip':
                skip.append(argv[i+1]); i += 2
            elif argv[i] == '--commit':
                commit = True; i += 1
            else:
                i += 1
        return cmd_apply5g(idx, sets, commit, skip)
    else:
        print('unknown cmd', cmd); return 2
    return 0

if __name__ == '__main__':
    sys.exit(main())
