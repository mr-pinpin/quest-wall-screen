#!/usr/bin/env python3
"""Loopback-only WebXR capability probe; room reports stay outside the checkout."""
import argparse
import http.server
import json
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port', type=int, default=8087)
    p.add_argument('--state', type=pathlib.Path, required=True)
    args = p.parse_args()
    args.state.mkdir(parents=True, exist_ok=True)

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path == '/report':
                file = args.state / 'probe-report.json'
                data = file.read_bytes() if file.exists() else b'{}'
                mime = 'application/json'
            elif self.path in ('/', '/probe.html'):
                data = (ROOT / 'probe.html').read_bytes()
                mime = 'text/html; charset=utf-8'
            else:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header('Content-Type', mime)
            self.send_header('Cache-Control', 'no-store')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_POST(self):
            if self.path != '/report':
                self.send_error(404)
                return
            length = int(self.headers.get('Content-Length', '0'))
            if not 0 < length <= 16384:
                self.send_error(413)
                return
            try:
                report = json.loads(self.rfile.read(length))
            except (ValueError, UnicodeDecodeError):
                self.send_error(400)
                return
            if not isinstance(report, dict):
                self.send_error(400)
                return
            file = args.state / 'probe-report.json'
            file.write_text(json.dumps(report, indent=2) + '\n')
            file.chmod(0o600)
            self.send_response(204)
            self.end_headers()

        def log_message(self, *args):
            pass

    http.server.ThreadingHTTPServer(('127.0.0.1', args.port), Handler).serve_forever()

if __name__ == '__main__':
    main()
