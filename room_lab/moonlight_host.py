#!/usr/bin/env python3
"""Pair the headset with the Mini's Sunshine and start/resume a stream for the room lab.

Runs on the VPS: tunnels to the Mini's Sunshine over SSH, speaks the GameStream HTTP(S) protocol,
and approves the pairing PIN itself through Sunshine's admin API (credentials stay on the Mini).
The client identity is kept in PRIVATE and copied into the app's private storage on the headset.

  moonlight_host.py pair                 create identity, pair, push identity to the headset
  moonlight_host.py launch [--app NAME]  resume/launch and push session.json (the app connects on its own)
  moonlight_host.py apps | status | cancel
"""
import argparse
import base64
import contextlib
import datetime
import hashlib
import json
import os
import pathlib
import secrets
import shlex
import socket
import ssl
import subprocess
import sys
import threading
import time
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding, rsa
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.x509.oid import NameOID

PRIVATE = pathlib.Path('/root/.local/share/pinpin-wall-screen/private/moonlight')
SSH = ['ssh', '-o', 'BatchMode=yes', '-o', 'ProxyJump=pinpin-air', '-o', 'ConnectTimeout=20', 'pinpin-mini-lan']
LAN_HOST = '192.168.2.1'  # the Mini as seen from the headset's PinPin-Quest-Offline network
PORTS = {'http': 47989, 'https': 47984, 'admin': 47990}
LOCAL = {'http': 57989, 'https': 57984, 'admin': 57990}
PKG = 'com.pinpin.wallscreen.roomlab'

@contextlib.contextmanager
def tunnel():
    args = SSH[:-1] + ['-N', '-o', 'ExitOnForwardFailure=yes']
    for k in PORTS: args += ['-L', f'127.0.0.1:{LOCAL[k]}:127.0.0.1:{PORTS[k]}']
    p = subprocess.Popen(args + [SSH[-1]])
    try:
        for _ in range(100):
            with contextlib.suppress(OSError):
                socket.create_connection(('127.0.0.1', LOCAL['http']), 0.3).close(); break
            time.sleep(0.2)
        yield
    finally:
        p.terminate(); p.wait()

def identity():
    PRIVATE.mkdir(parents=True, exist_ok=True); os.chmod(PRIVATE, 0o700)
    kp, cp, up = PRIVATE / 'client.key', PRIVATE / 'client.pem', PRIVATE / 'uniqueid'
    if not kp.exists():
        key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'PinPin Quest Room Lab')])
        now = datetime.datetime.now(datetime.timezone.utc)
        cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
                .serial_number(x509.random_serial_number()).not_valid_before(now - datetime.timedelta(days=1))
                .not_valid_after(now + datetime.timedelta(days=365 * 20)).sign(key, hashes.SHA256()))
        kp.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.TraditionalOpenSSL, serialization.NoEncryption()))
        cp.write_bytes(cert.public_bytes(serialization.Encoding.PEM)); up.write_text(secrets.token_hex(8).upper())
        for f in (kp, cp, up): os.chmod(f, 0o600)
    key = serialization.load_pem_private_key(kp.read_bytes(), None)
    return key, x509.load_pem_x509_certificate(cp.read_bytes()), up.read_text().strip()

def query(path, params, https=False, timeout=30):
    _, _, uid = identity()
    q = {'uniqueid': uid, 'uuid': secrets.token_hex(16)} | params
    url = f"{'https' if https else 'http'}://127.0.0.1:{LOCAL['https' if https else 'http']}/{path}?" + urllib.parse.urlencode(q)
    ctx = None
    if https:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT); ctx.check_hostname = False; ctx.verify_mode = ssl.CERT_NONE
        ctx.load_cert_chain(PRIVATE / 'client.pem', PRIVATE / 'client.key')
    with urllib.request.urlopen(url, context=ctx, timeout=timeout) as r:
        root = ET.fromstring(r.read())
    if root.get('status_code') != '200': raise SystemExit(f'{path}: {root.get("status_code")} {root.get("status_message")}')
    return root

REMOTE_HTTPS = r'''
import sys,json,ssl,os,tempfile,socket,http.client,urllib.parse
d=json.load(sys.stdin)
fd,path=tempfile.mkstemp();os.write(fd,(d['cert']+d['key']).encode());os.close(fd)
try:
    ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT);ctx.check_hostname=False;ctx.verify_mode=ssl.CERT_NONE;ctx.load_cert_chain(path)
    c=http.client.HTTPSConnection(d['bind'],47984,context=ctx,timeout=60,source_address=(d['bind'],0))
    c.request('GET','/'+d['path']+'?'+d['query']);sys.stdout.write(c.getresponse().read().decode())
finally:
    os.unlink(path)
'''

def query_lan(path, params):
    """HTTPS GameStream request issued on the Mini from its headset-facing address, as a LAN client would."""
    _, _, uid = identity()
    q = urllib.parse.urlencode({'uniqueid': uid, 'uuid': secrets.token_hex(16)} | params)
    body = json.dumps({'cert': (PRIVATE / 'client.pem').read_text(), 'key': (PRIVATE / 'client.key').read_text(), 'bind': LAN_HOST, 'path': path, 'query': q})
    r = subprocess.run(SSH + ['/usr/bin/python3 -c ' + shlex.quote(REMOTE_HTTPS)], input=body, capture_output=True, text=True, timeout=90)
    if r.returncode: raise SystemExit(r.stderr[-1500:])
    root = ET.fromstring(r.stdout)
    if root.get('status_code') != '200': raise SystemExit(f'{path}: {root.get("status_code")} {root.get("status_message")}')
    return root

def admin(path, body=None):
    cred = json.loads(subprocess.run(SSH + ['cat ~/.local/share/pinpin-connect/sunshine/admin.json'], capture_output=True, text=True, check=True).stdout)
    auth = 'Basic ' + base64.b64encode(f"{cred['username']}:{cred['password']}".encode()).decode()
    base = f"https://127.0.0.1:{LOCAL['admin']}"
    req = urllib.request.Request(base + path, data=json.dumps(body).encode() if body is not None else None,
                                 headers={'Authorization': auth, 'Content-Type': 'application/json', 'Origin': base})
    with urllib.request.urlopen(req, context=ssl._create_unverified_context(), timeout=20) as r: return json.loads(r.read())

def aes(key, data, decrypt=False):
    c = Cipher(algorithms.AES(key), modes.ECB()); op = c.decryptor() if decrypt else c.encryptor()
    return op.update(data) + op.finalize()

def pair():
    key, cert, uid = identity()
    pin = f'{secrets.randbelow(10000):04d}'; salt = secrets.token_bytes(16)
    aes_key = hashlib.sha256(salt + pin.encode()).digest()[:16]
    pem = cert.public_bytes(serialization.Encoding.PEM)
    result = {}
    def first():
        try: result['root'] = query('pair', {'devicename': 'PinPinQuest', 'updateState': 1, 'phrase': 'getservercert',
                                             'salt': salt.hex(), 'clientcert': pem.hex()}, timeout=90)
        except Exception as e: result['error'] = e
    t = threading.Thread(target=first); t.start()
    approval = {}
    def approve(pairing_id):  # this Sunshine holds the approval call open until every pairing phase completes
        try: approval['status'] = admin('/api/pin', {'pairing_id': pairing_id, 'pin': pin, 'name': 'PinPin Quest Room Lab'}).get('status')
        except Exception as e: approval['status'] = repr(e)
    approver = None
    for _ in range(60):  # approve our own pending request with the PIN we chose
        time.sleep(0.5)
        pending = admin('/api/pin').get('pairings', [])
        if pending:
            approver = threading.Thread(target=approve, args=(pending[-1].get('id'),), daemon=True); approver.start()
            break
    t.join()
    if 'error' in result: raise result['error']
    root = result['root']
    if root.findtext('paired') != '1': raise SystemExit('server refused getservercert')
    server_pem = bytes.fromhex(root.findtext('plaincert')); (PRIVATE / 'server.pem').write_bytes(server_pem)
    server = x509.load_pem_x509_certificate(server_pem)
    challenge = secrets.token_bytes(16)
    root = query('pair', {'devicename': 'PinPinQuest', 'updateState': 1, 'clientchallenge': aes(aes_key, challenge).hex()})
    dec = aes(aes_key, bytes.fromhex(root.findtext('challengeresponse')), decrypt=True)
    server_response, server_challenge = dec[:32], dec[32:48]
    client_secret = secrets.token_bytes(16)
    h = hashlib.sha256(server_challenge + cert.signature + client_secret).digest()
    root = query('pair', {'devicename': 'PinPinQuest', 'updateState': 1, 'serverchallengeresp': aes(aes_key, h).hex()})
    ps = bytes.fromhex(root.findtext('pairingsecret')); server_secret, server_sig = ps[:16], ps[16:]
    server.public_key().verify(server_sig, server_secret, padding.PKCS1v15(), hashes.SHA256())  # the PIN-holder is the real server
    if hashlib.sha256(challenge + server.signature + server_secret).digest() != server_response: raise SystemExit('server failed challenge (wrong PIN?)')
    sig = key.sign(client_secret, padding.PKCS1v15(), hashes.SHA256())
    root = query('pair', {'devicename': 'PinPinQuest', 'updateState': 1, 'clientpairingsecret': (client_secret + sig).hex()})
    if root.findtext('paired') != '1': raise SystemExit('client pairing secret rejected')
    root = query('pair', {'devicename': 'PinPinQuest', 'updateState': 1, 'phrase': 'pairchallenge'}, https=True)
    if approver: approver.join(15)
    print('PAIRED', root.findtext('paired') == '1', 'approval', approval.get('status'), 'client', uid)
    push({'client.pem': PRIVATE / 'client.pem', 'client.key': PRIVATE / 'client.key', 'server.pem': PRIVATE / 'server.pem'})

def push(files):
    """Copy files into the app's private storage (files/moonlight/) through the Mini's ADB."""
    payload = {n: base64.b64encode(pathlib.Path(p).read_bytes() if isinstance(p, pathlib.Path) else p.encode()).decode() for n, p in files.items()}
    code = r'''
import sys,json,base64,pathlib,subprocess
adb=[str(pathlib.Path.home()/'.local/share/pinpin-quest/platform-tools/adb'),'-s','127.0.0.1:26351']
for n,b in json.load(sys.stdin).items():
    t=pathlib.Path.home()/'.local/share/pinpin-quest/wall-screen'/('ml-'+n);t.write_bytes(base64.b64decode(b));t.chmod(0o600)
    subprocess.run(adb+['push',str(t),'/data/local/tmp/ml-'+n],check=True,capture_output=True);t.unlink()
    r=subprocess.run(adb+['shell',"run-as %s sh -c 'mkdir -p files/moonlight && cat /data/local/tmp/ml-%s > files/moonlight/.%s && mv files/moonlight/.%s files/moonlight/%s' && rm /data/local/tmp/ml-%s"%(sys.argv[1],n,n,n,n,n)],capture_output=True,text=True)
    print('PUSHED',n,r.returncode,r.stderr.strip())
'''
    r = subprocess.run(SSH[:-1] + [SSH[-1], '/usr/bin/python3 -c ' + shlex.quote(code) + ' ' + PKG], input=json.dumps(payload), capture_output=True, text=True)
    print(r.stdout.strip(), r.stderr[-800:])

def serverinfo():
    root = query('serverinfo', {}, https=True)
    return {k: root.findtext(k) for k in ['hostname', 'appversion', 'GfeVersion', 'ServerCodecModeSupport', 'currentgame', 'state', 'PairStatus']}

def apps():
    return [(a.findtext('AppTitle'), a.findtext('ID')) for a in query('applist', {}, https=True).findall('App')]

def launch(app, width, height, fps, bitrate, hevc):
    info = serverinfo(); print('SERVER', info)
    ids = dict(apps()); appid = ids.get(app) or next(iter(ids.values()))
    rikey = secrets.token_bytes(16); rikeyid = secrets.randbelow(2**31)
    params = {'appid': appid, 'mode': f'{width}x{height}x{fps}', 'additionalStates': 1, 'sops': 0, 'rikey': rikey.hex(), 'rikeyid': rikeyid,
              'localAudioPlayMode': 0, 'surroundAudioInfo': 196610, 'remoteControllersBitmap': 0, 'gcmap': 0, 'gcpersist': 0, 'corever': 1}
    verb = 'resume' if info['currentgame'] == str(appid) else 'launch'
    if info['currentgame'] not in ('0', str(appid)): query_lan('cancel', {})
    root = query_lan(verb, params)
    url = root.findtext('sessionUrl0') or f'rtsp://{LAN_HOST}:48010'
    u = urllib.parse.urlsplit(url); url = urllib.parse.urlunsplit((u.scheme, f'{LAN_HOST}:{u.port or 48010}', u.path, u.query, u.fragment))
    session = {'host': LAN_HOST, 'rtspSessionUrl': url, 'appVersion': info['appversion'], 'gfeVersion': info['GfeVersion'],
               'serverCodecModeSupport': int(info['ServerCodecModeSupport'] or 1), 'width': width, 'height': height, 'fps': fps,
               'bitrateKbps': bitrate, 'hevc': hevc, 'rikey': rikey.hex(), 'rikeyid': rikeyid, 'app': app, 'appid': int(appid),
               'issued': time.time()}
    print(verb.upper(), json.dumps({k: v for k, v in session.items() if k != 'rikey'}))
    push({'session.json': json.dumps(session)})

def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('action', choices=['pair', 'launch', 'apps', 'status', 'cancel'])
    p.add_argument('--app', default='Desktop'); p.add_argument('--width', type=int, default=2560); p.add_argument('--height', type=int, default=1440)
    p.add_argument('--fps', type=int, default=72); p.add_argument('--bitrate', type=int, default=60000); p.add_argument('--h264', action='store_true')
    a = p.parse_args()
    with tunnel():
        if a.action == 'pair': pair()
        elif a.action == 'apps': print(apps())
        elif a.action == 'status': print(serverinfo())
        elif a.action == 'cancel': print(ET.tostring(query('cancel', {}, https=True)).decode())
        else: launch(a.app, a.width, a.height, a.fps, a.bitrate, not a.h264)

if __name__ == '__main__':
    main()
