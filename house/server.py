"""House sync server: the one canonical house.json, on the Mini, over the LAN (no internet needed).

    python3 house/server.py --data ~/pinpin-house [--port 57800] [--bind 0.0.0.0]

--data is a git repository (created if missing) holding house.json and blobs/<sha256>. Every accepted change is
written atomically (temp file, fsync, rename) and committed, so the history is `git log` and any revision can be
restored. Nothing is ever overwritten silently: a write must name the revision it was based on, and the server
refuses truncated, corrupt, invalid or stale writes.

  GET  /house[?schema=K]       the house (converted down to schema K if asked); headers ETag/X-House-Rev = rev,
                               X-House-Schema, X-House-SHA256 (of the body)
  PUT  /house                  body: the whole house. Needs If-Match: <rev it was based on>, X-House-SHA256: <sha256 of
                               the body>, Content-Length. 200 {"rev": N}; 400 corrupt/invalid (+ problems);
                               409 stale (+ the current house); 428 no If-Match. An older-schema body is converted up
                               and keeps every field its writer could not see.
  GET  /house/history[?limit=N] [{rev, commit, when, by, note}], newest first
  GET  /house/rev/N            the house as it was at revision N
  POST /house/rollback?rev=N   new revision with revision N's content (If-Match: current rev)
  PUT  /blob/<sha256>          a mesh or other binary; the body must hash to the name. GET /blob/<sha256>.
  GET  /legacy/placement.json | screens.json | settings.json   the house in today's headset file formats
  GET  /house/plan.svg         top-down floor plan from the walls' poses (house/floorplan.py)
  GET  /health
"""
import argparse
import datetime
import hashlib
import json
import os
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import schema  # noqa: E402

MAX_HOUSE = 16 << 20
MAX_BLOB = 256 << 20
AUTHOR = ['-c', 'user.name=Mr. PinPin', '-c', 'user.email=miguelemosreverte@gmail.com']


def sha(b):
    return hashlib.sha256(b).hexdigest()


def now():
    return datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')


def encode(doc):
    return (json.dumps(doc, indent=1, ensure_ascii=False) + '\n').encode()


class Store:
    """The data directory: house.json + blobs/, under git. All writes go through one lock."""

    def __init__(self, root):
        self.root = os.path.abspath(os.path.expanduser(root))
        self.path = os.path.join(self.root, 'house.json')
        self.blobdir = os.path.join(self.root, 'blobs')
        self.lock = threading.Lock()
        os.makedirs(self.blobdir, exist_ok=True)
        if not os.path.isdir(os.path.join(self.root, '.git')):
            self.git('init', '-q')
        self.doc = self.load()

    def git(self, *a, check=True):
        r = subprocess.run(['git', *AUTHOR, *a], cwd=self.root, capture_output=True, text=True)
        if check and r.returncode:
            raise RuntimeError(f'git {a[0]}: {r.stderr.strip()}')
        return r.stdout

    def blobs(self):
        return {n for n in os.listdir(self.blobdir) if schema.SHA.match(n)}

    def load(self):
        """The house on disk; if it is missing or fails validation, the last committed good one (never a broken one)."""
        try:
            with open(self.path, 'rb') as f:
                doc = json.loads(f.read())
            if not schema.validate(doc, self.blobs()):
                if self.git('status', '--porcelain', '--', 'house.json', check=False).strip():
                    self.write(doc, 'adopt house.json found on disk')  # e.g. a migration placed by hand
                return doc
            why = 'invalid'
        except (OSError, ValueError):
            why = 'missing or unreadable'
        for commit in self.git('log', '--format=%H', '--', 'house.json', check=False).split():
            try:
                doc = json.loads(self.git('show', f'{commit}:house.json'))
            except (RuntimeError, ValueError):
                continue
            if not schema.validate(doc):
                print(f'house.json {why}: restored rev {doc["rev"]} from commit {commit[:10]}', file=sys.stderr)
                self.write(doc, f'restore rev {doc["rev"]} (house.json on disk was {why})')
                return doc
        doc = schema.empty()
        doc['updated'] = now()
        print(f'house.json {why} and no good revision in git: starting an empty house', file=sys.stderr)
        self.write(doc, 'empty house')
        return doc

    def _atomic(self, path, data):
        tmp = f'{path}.tmp-{os.getpid()}-{threading.get_ident()}'
        with open(tmp, 'wb') as f:
            f.write(data); f.flush(); os.fsync(f.fileno())
        os.replace(tmp, path)
        fd = os.open(os.path.dirname(path), os.O_RDONLY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)

    def write(self, doc, note):
        """Atomic write + commit; on a failed commit the previous file is put back and the error raised."""
        prev = None
        try:
            with open(self.path, 'rb') as f:
                prev = f.read()
        except OSError:
            pass
        self._atomic(self.path, encode(doc))
        try:
            self.git('add', 'house.json')
            self.git('commit', '-q', '--allow-empty', '-m', f'house rev {doc["rev"]}: {doc["by"]}' + (f' - {note}' if note else ''))
        except RuntimeError:
            if prev is not None:
                self._atomic(self.path, prev)
            self.git('reset', '-q', check=False)
            raise

    def put(self, doc, note):
        self.write(doc, note)
        self.doc = doc

    def history(self, limit=50):
        out = []
        for line in self.git('log', f'-{limit}', '--format=%H%x09%cI%x09%s', '--', 'house.json', check=False).splitlines():
            commit, when, subject = line.split('\t', 2)
            if not subject.startswith('house rev '):
                continue
            head, _, note = subject[10:].partition(' - ')
            rev, _, by = head.partition(': ')
            out.append({'rev': int(rev), 'commit': commit, 'when': when, 'by': by, 'note': note})
        return out

    def at(self, rev):
        for h in self.history(100000):
            if h['rev'] == rev:
                return json.loads(self.git('show', f'{h["commit"]}:house.json'))
        return None

    def put_blob(self, name, data):
        path = os.path.join(self.blobdir, name)
        if not os.path.exists(path):
            self._atomic(path, data)
            self.git('add', os.path.join('blobs', name))
            self.git('commit', '-q', '-m', f'blob {name[:12]} ({len(data)} bytes)')


class Handler(BaseHTTPRequestHandler):
    store: Store = None
    protocol_version = 'HTTP/1.1'
    timeout = 30  # a sender that stops mid-body is cut off, and its partial body is never applied

    def log_message(self, fmt, *a):
        sys.stderr.write('%s %s\n' % (self.address_string(), fmt % a))

    def send(self, code, body=b'', ctype='application/json', headers=None):
        if isinstance(body, (dict, list)):
            body = (json.dumps(body, indent=1) + '\n').encode()
        elif isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        for k, v in (headers or {}).items():
            self.send_header(k, str(v))
        self.end_headers()
        if self.command != 'HEAD':
            self.wfile.write(body)

    def send_house(self, doc, code=200, extra=None):
        body = encode(doc)
        h = {'ETag': f'"{doc["rev"]}"', 'X-House-Rev': doc['rev'], 'X-House-Schema': doc['schema'], 'X-House-SHA256': sha(body)}
        h.update(extra or {})
        self.send(code, body, headers=h)

    def body(self, limit):
        """The exact request body, or None after an error reply (missing length, too big, short read)."""
        n = self.headers.get('Content-Length')
        if n is None or not n.isdigit():
            self.send(411, {'error': 'Content-Length required'}); return None
        n = int(n)
        if n > limit:
            self.send(413, {'error': f'body over {limit} bytes'}); return None
        try:
            data = self.rfile.read(n)
        except OSError:
            data = b''
        if len(data) != n:
            self.close_connection = True
            self.send(400, {'error': f'truncated body: {len(data)} of {n} bytes'}); return None
        return data

    def schema_param(self, q):
        k = q.get('schema', [str(schema.CURRENT)])[0]
        if not k.isdigit() or not schema.oldest_readable() <= int(k) <= schema.CURRENT:
            self.send(406, {'error': f'schema {k} not served', 'oldest': schema.oldest_readable(), 'current': schema.CURRENT})
            return None
        return int(k)

    def do_GET(self):
        u = urlparse(self.path); q = parse_qs(u.query); p = u.path.rstrip('/')
        st = self.store
        if p == '/health':
            return self.send(200, {'ok': True, 'rev': st.doc['rev'], 'schema': schema.CURRENT, 'oldest': schema.oldest_readable()})
        if p == '/house':
            k = self.schema_param(q)
            if k is None:
                return
            return self.send_house(schema.convert(st.doc, k))
        if p == '/house/history':
            return self.send(200, st.history(int(q.get('limit', ['50'])[0])))
        if p.startswith('/house/rev/') and p[11:].isdigit():
            doc = st.at(int(p[11:]))
            return self.send_house(doc) if doc else self.send(404, {'error': 'no such revision'})
        if p.startswith('/blob/') and schema.SHA.match(p[6:]):
            path = os.path.join(st.blobdir, p[6:])
            if not os.path.exists(path):
                return self.send(404, {'error': 'no such blob'})
            with open(path, 'rb') as f:
                return self.send(200, f.read(), 'application/octet-stream')
        if p == '/house/plan.svg':
            import floorplan
            return self.send(200, floorplan.plan(st.doc), 'image/svg+xml')
        if p.startswith('/legacy/'):
            fn = {'placement.json': schema.legacy_placement, 'screens.json': schema.legacy_screens,
                  'settings.json': schema.legacy_settings}.get(p[8:])
            if fn:
                v = fn(st.doc)
                return self.send(200, v) if v is not None else self.send(404, {'error': 'nothing to export'})
        self.send(404, {'error': 'not found'})

    do_HEAD = do_GET

    def check_match(self):
        m = self.headers.get('If-Match', '').strip().strip('"')
        if not m:
            self.send(428, {'error': 'If-Match: <rev> required (the revision this edit was based on)'}); return None
        if not m.isdigit():
            self.send(400, {'error': 'If-Match must be a revision number'}); return None
        return int(m)

    def do_PUT(self):
        u = urlparse(self.path); p = u.path.rstrip('/')
        st = self.store
        if p.startswith('/blob/') and schema.SHA.match(p[6:]):
            data = self.body(MAX_BLOB)
            if data is None:
                return
            if sha(data) != p[6:]:
                return self.send(400, {'error': 'blob body does not hash to its name'})
            with st.lock:
                st.put_blob(p[6:], data)
            return self.send(200, {'blob': p[6:], 'bytes': len(data)})
        if p != '/house':
            return self.send(404, {'error': 'not found'})
        base = self.check_match()
        if base is None:
            return
        want = self.headers.get('X-House-SHA256', '').strip().lower()
        if not schema.SHA.match(want):
            return self.send(400, {'error': 'X-House-SHA256: <sha256 of the body> required'})
        data = self.body(MAX_HOUSE)
        if data is None:
            return
        if sha(data) != want:
            return self.send(400, {'error': 'body does not match X-House-SHA256 (damaged or partial upload)'})
        try:
            doc = json.loads(data)
        except ValueError as ex:
            return self.send(400, {'error': f'not JSON: {ex}'})
        if not isinstance(doc, dict) or not isinstance(doc.get('schema'), int) or isinstance(doc.get('schema'), bool):
            return self.send(400, {'error': 'no schema number'})
        k = doc['schema']
        if not schema.oldest_readable() <= k <= schema.CURRENT:
            return self.send(400, {'error': f'schema {k} not accepted', 'oldest': schema.oldest_readable(), 'current': schema.CURRENT})
        with st.lock:
            cur = st.doc
            if base != cur['rev']:
                return self.send_house(schema.convert(cur, k), 409, {'X-House-Conflict': f'based on {base}, current {cur["rev"]}'})
            if k < schema.CURRENT:
                doc = schema.preserve(schema.convert(doc, schema.CURRENT), cur, k)
            doc['rev'], doc['updated'] = cur['rev'] + 1, now()
            problems = schema.validate(doc, st.blobs())
            if problems:
                return self.send(400, {'error': 'invalid house', 'problems': problems[:50]})
            try:
                st.put(doc, self.headers.get('X-House-Note', '')[:200].replace('\n', ' '))
            except (OSError, RuntimeError) as ex:
                return self.send(500, {'error': f'not saved: {ex}'})
        self.send(200, {'rev': doc['rev']}, headers={'ETag': f'"{doc["rev"]}"'})

    def do_POST(self):
        u = urlparse(self.path); q = parse_qs(u.query); p = u.path.rstrip('/')
        st = self.store
        if p != '/house/rollback' or not q.get('rev', [''])[0].isdigit():
            return self.send(404, {'error': 'not found'})
        base = self.check_match()
        if base is None:
            return
        self.close_connection = True  # any body is ignored
        with st.lock:
            if base != st.doc['rev']:
                return self.send_house(st.doc, 409)
            old = st.at(int(q['rev'][0]))
            if old is None:
                return self.send(404, {'error': 'no such revision'})
            old = schema.convert(old, schema.CURRENT) if old['schema'] <= schema.CURRENT else old
            old['rev'], old['updated'], old['by'] = st.doc['rev'] + 1, now(), 'rollback'
            problems = schema.validate(old, st.blobs())
            if problems:
                return self.send(400, {'error': 'that revision is not valid now', 'problems': problems[:50]})
            st.put(old, f'to rev {q["rev"][0]}')
        self.send(200, {'rev': old['rev'], 'restored': int(q['rev'][0])})


def serve(data, port=57800, bind='0.0.0.0'):
    Handler.store = Store(data)
    srv = ThreadingHTTPServer((bind, port), Handler)
    srv.daemon_threads = True
    return srv


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', required=True)
    ap.add_argument('--port', type=int, default=57800)
    ap.add_argument('--bind', default='0.0.0.0')
    a = ap.parse_args()
    s = serve(a.data, a.port, a.bind)
    print(f'house server on {a.bind}:{a.port}, data {Handler.store.root}, rev {Handler.store.doc["rev"]}', file=sys.stderr)
    s.serve_forever()
