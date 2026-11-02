#!/usr/bin/env python3
"""Port relay on the Mini: headset network (192.168.2.1) -> a Sunshine host on the home LAN.

The headset's PinPin-Quest-Offline network has no route beyond the Mini (IP forwarding stays off so the
headset remains offline). This relays exactly one Sunshine port range, TCP and UDP, 1:1 to the target.
Stdlib only (the Mini's /usr/bin/python3).
"""
import argparse
import asyncio
import socket

TCP = [-5, 0, 21]     # https, http, rtsp relative to the Sunshine base port
UDP = [9, 10, 11]     # video, control (ENet), audio

async def pipe(r, w):
    try:
        while data := await r.read(65536):
            w.write(data); await w.drain()
    except (ConnectionError, OSError):
        pass
    finally:
        w.close()

async def tcp(listen, target, port):
    async def handle(cr, cw):
        try:
            tr, tw = await asyncio.open_connection(target, port)
        except OSError:
            cw.close(); return
        for s in (cw, tw):
            sock = s.get_extra_info('socket')
            if sock: sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        await asyncio.gather(pipe(cr, tw), pipe(tr, cw))
    await (await asyncio.start_server(handle, listen, port)).serve_forever()

BUF = 4 << 20

def tune(transport):
    """Large kernel buffers (macOS UDP defaults are ~9 KB, far below one keyframe burst) and no user-space queue:
    if the kernel cannot take a datagram now, dropping it beats delaying every later frame (Moonlight has FEC)."""
    sock = transport.get_extra_info('socket')
    for opt in (socket.SO_SNDBUF, socket.SO_RCVBUF):
        try: sock.setsockopt(socket.SOL_SOCKET, opt, BUF)
        except OSError: sock.setsockopt(socket.SOL_SOCKET, opt, 1 << 20)
    return sock

def send(sock, data, addr=None):
    try:
        if addr: sock.sendto(data, addr)
        else: sock.send(data)
    except (BlockingIOError, InterruptedError, OSError):
        pass

class Upstream(asyncio.DatagramProtocol):
    def __init__(self, front, client): self.front, self.client = front, client
    def connection_made(self, transport): self.sock = tune(transport)
    def datagram_received(self, data, addr): send(self.front.sock, data, self.client)

class Front(asyncio.DatagramProtocol):
    def __init__(self, target, port): self.target, self.port, self.peers = target, port, {}
    def connection_made(self, transport): self.transport = transport; self.sock = tune(transport)
    def datagram_received(self, data, addr):
        up = self.peers.get(addr)
        if up is None:
            loop = asyncio.get_running_loop()
            self.peers[addr] = up = asyncio.ensure_future(
                loop.create_datagram_endpoint(lambda: Upstream(self, addr), remote_addr=(self.target, self.port)))
        if isinstance(up, asyncio.Future):
            if not up.done():
                up.add_done_callback(lambda f: send(f.result()[1].sock, data)); return
            self.peers[addr] = up = up.result()[1]
        send(up.sock, data)

async def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--listen', default='192.168.2.1'); p.add_argument('--target', required=True)
    p.add_argument('--base', type=int, default=57989)
    a = p.parse_args()
    loop = asyncio.get_running_loop()
    for off in UDP:
        await loop.create_datagram_endpoint(lambda o=off: Front(a.target, a.base + o), local_addr=(a.listen, a.base + off))
    await asyncio.gather(*(tcp(a.listen, a.target, a.base + off) for off in TCP))

if __name__ == '__main__':
    asyncio.run(main())
