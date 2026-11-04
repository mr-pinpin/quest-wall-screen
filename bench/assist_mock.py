"""A tiny SSE stand-in for the per-screen assistant (docs/prd/assistant-protocol.md), for bench/assist_test.sh."""
import json, sys, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
class H(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *a): pass
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        turn = body.get('turn', ''); screen = self.path.split('/')[2]
        self.send_response(200); self.send_header('Content-Type', 'text/event-stream'); self.send_header('Transfer-Encoding', 'chunked'); self.end_headers()
        def ev(o):
            d = (': keepalive\n\n' if o is None else 'data: ' + json.dumps(o) + '\n\n').encode()
            self.wfile.write(b'%x\r\n%s\r\n' % (len(d), d)); self.wfile.flush()
        ev({'type': 'start', 'turnId': turn, 'screen': screen}); ev(None)
        if body['text'] == 'die': ev({'type': 'text', 'delta': 'Starting', 'turnId': turn}); self.wfile.flush(); self.connection.close(); return
        for part in ('Hi ', 'from screen ', screen + '.'): ev({'type': 'text', 'delta': part, 'turnId': turn}); time.sleep(0.01)
        ev({'type': 'action', 'action': 'screenshot', 'status': 'begin', 'turnId': turn}); ev({'type': 'action', 'action': 'screenshot', 'status': 'ok', 'turnId': turn})
        ev({'type': 'done', 'turnId': turn, 'stopReason': 'end'}); self.wfile.write(b'0\r\n\r\n')
ThreadingHTTPServer(('127.0.0.1', int(sys.argv[1])), H).serve_forever()
