#!/usr/bin/env python3
"""Home-only desktop relay. TLS client certificates protect the Mac endpoint."""
import argparse
import base64
import hashlib
import http.server
import json
import pathlib
import socket
import socketserver
import ssl
import struct
import threading

MAX_PACKET = 8 * 1024 * 1024
ROOT = pathlib.Path(__file__).resolve().parent

def read_exact(stream, length):
    data = bytearray()
    while len(data) < length:
        part = stream.recv(length - len(data))
        if not part:
            raise EOFError('Stream ended')
        data.extend(part)
    return bytes(data)

def read_packet(stream):
    length = struct.unpack('!I', read_exact(stream, 4))[0]
    if not 1 <= length <= MAX_PACKET:
        raise ValueError('Invalid video packet length')
    return read_exact(stream, length)

def websocket_packet(data):
    n = len(data)
    if n < 126:
        return bytes((0x82, n)) + data
    if n < 65536:
        return b'\x82\x7e' + struct.pack('!H', n) + data
    return b'\x82\x7f' + struct.pack('!Q', n) + data

def tls_context(config, server=False):
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER) if server else ssl.create_default_context(cafile=config['ca'])
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.load_cert_chain(config['certificate'], config['key'])
    if server:
        context.load_verify_locations(cafile=config['ca'])
        context.verify_mode = ssl.CERT_REQUIRED
    return context

def serve_sender(config):
    context = tls_context(config, server=True)
    class Handler(socketserver.BaseRequestHandler):
        def handle(self):
            self.request.settimeout(10)
            try:
                with context.wrap_socket(self.request, server_side=True) as client:
                    with socket.create_connection(('127.0.0.1', 9234), timeout=15) as source:
                        source.settimeout(None)
                        client.settimeout(None)
                        def watch_disconnect():
                            try:
                                client.recv(1)
                            except OSError:
                                pass
                            try:
                                source.shutdown(socket.SHUT_RDWR)
                            except OSError:
                                pass
                        threading.Thread(target=watch_disconnect, daemon=True).start()
                        while True:
                            packet = read_packet(source)
                            client.sendall(struct.pack('!I', len(packet)) + packet)
            except (OSError, EOFError, ValueError):
                pass
    class Server(socketserver.ThreadingTCPServer):
        allow_reuse_address = True
        daemon_threads = True
    Server((config.get('bind', '0.0.0.0'), config.get('port', 9235)), Handler).serve_forever()

def serve_viewer(config, port, state):
    context = tls_context(config)
    state.mkdir(parents=True, exist_ok=True)
    allowed_origins = {'http://127.0.0.1:' + str(port), 'http://localhost:' + str(port)}
    routes = {'/': ('index.html', 'text/html; charset=utf-8'),
              '/index.html': ('index.html', 'text/html; charset=utf-8'),
              '/viewer.js': ('viewer.js', 'text/javascript'),
              '/wall_math.mjs': ('wall_math.mjs', 'text/javascript'),
              '/probe.html': ('probe.html', 'text/html; charset=utf-8')}
    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'
        def do_GET(self):
            if self.path == '/stream':
                self.stream()
                return
            if self.path not in routes:
                self.send_error(404)
                return
            name, mime = routes[self.path]
            data = (ROOT / name).read_bytes()
            self.send_response(200)
            self.send_header('Content-Type', mime)
            self.send_header('Content-Length', str(len(data)))
            self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            self.wfile.write(data)

        def do_POST(self):
            if self.path != '/report' or self.headers.get('Origin') not in allowed_origins:
                self.send_error(403)
                return
            length = int(self.headers.get('Content-Length', '0'))
            if not 0 < length <= 16384:
                self.send_error(413)
                return
            try:
                report = json.loads(self.rfile.read(length))
                if not isinstance(report, dict):
                    raise ValueError()
            except (ValueError, UnicodeDecodeError):
                self.send_error(400)
                return
            p = state / 'viewer-report.json'
            p.write_text(json.dumps(report, indent=2) + '\n')
            p.chmod(0o600)
            self.send_response(204)
            self.end_headers()

        def stream(self):
            if self.headers.get('Origin') not in allowed_origins or self.headers.get('Upgrade', '').lower() != 'websocket':
                self.send_error(403)
                return
            key = self.headers.get('Sec-WebSocket-Key', '')
            try:
                if len(base64.b64decode(key, validate=True)) != 16:
                    raise ValueError()
            except ValueError:
                self.send_error(400)
                return
            accept = base64.b64encode(hashlib.sha1((key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
            self.send_response(101)
            self.send_header('Upgrade', 'websocket')
            self.send_header('Connection', 'Upgrade')
            self.send_header('Sec-WebSocket-Accept', accept)
            self.end_headers()
            self.close_connection = True
            try:
                raw = socket.create_connection((config['host'], config.get('port', 9235)), timeout=10)
                with context.wrap_socket(raw, server_hostname='pinpin-wall-screen') as source:
                    source.settimeout(None)
                    def watch_close():
                        try:
                            self.connection.recv(1024)
                        except OSError:
                            pass
                        try:
                            source.shutdown(socket.SHUT_RDWR)
                        except OSError:
                            pass
                    threading.Thread(target=watch_close, daemon=True).start()
                    while True:
                        self.wfile.write(websocket_packet(read_packet(source)))
                        self.wfile.flush()
            except (OSError, EOFError, ValueError):
                try:
                    self.wfile.write(websocket_packet(b'\x80Mac screen is not sharing yet. Open Wall Screen on your Mac.'))
                    self.wfile.flush()
                except OSError:
                    pass
        def log_message(self, *args):
            pass
    http.server.ThreadingHTTPServer(('127.0.0.1', port), Handler).serve_forever()

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('role', choices=['sender', 'viewer'])
    p.add_argument('--config', type=pathlib.Path, required=True)
    p.add_argument('--port', type=int, default=8088)
    p.add_argument('--state', type=pathlib.Path)
    args = p.parse_args()
    config = json.loads(args.config.read_text())
    if args.role == 'sender':
        serve_sender(config)
    else:
        if args.state is None:
            p.error('viewer needs --state outside the source checkout')
        serve_viewer(config, args.port, args.state)

if __name__ == '__main__':
    main()
