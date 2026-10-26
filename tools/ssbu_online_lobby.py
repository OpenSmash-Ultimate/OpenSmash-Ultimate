#!/usr/bin/env python3
"""The netplay lobby with SSBU's own Online mode: tools/ssbu_relay.py's lobby, plus the NEX game
server of tools/nex at the lobby's own virtual address, 10.13.0.1.

Players' games log in to it when they pick Online (runtime/hle_online.c sends NEX there); the
lobby carries the datagrams to and from it like any other, and lets players the game server
has put in one match talk to each other.

    python3 ssbu_online_lobby.py [the lobby's options] [--state DIR] [--nex-log 0|1|2]

ssbu-recomp's games are served only with --su-port (none by default: ssbu_relay.py's lobby on the
same machine has SU's usual port), and without the game server: it is WindowSmash's games' only.

Python 3.8+, standard library only."""
import argparse
import asyncio
import base64
import collections
import hashlib
import os
import signal
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ssbu_relay as relay   # noqa: E402
from nex.prudp import lite_size   # noqa: E402
from nex.server import NAT_CHECK_SECOND_IP, make_server   # noqa: E402

GATEWAY = 0x0A0D0001
NAT_CHECK_SECOND = struct.unpack(">I", socket.inet_aton(NAT_CHECK_SECOND_IP))[0]   # nncs2
PREFACE = b"SSBUNEX1"
STREAMS_PER_PLAYER = 4           # NEX streams a player may have open (authentication, secure, a retry)
MAX_STREAMS = 2048               # NEX streams open in all (the service's LimitNOFILE is 4096)
STREAM_RATE = 64 * 1024          # bytes a second a stream may send (NEX's own traffic is far less)
STREAM_BUFFER = 256 * 1024       # unparsed bytes a stream may hold
STREAM_PACKET = 16 * 1024        # largest PRUDP Lite packet (NEX fragments at 1300)
STREAM_BACKLOG = 1024 * 1024     # bytes waiting to go out to a stream: more, and its game is not reading
FRAME_COST = 16                  # (what an empty WebSocket frame takes from the stream's rate)
HANDSHAKE_TIME = 5               # seconds a new TCP connection has to say whose stream it is...
MAX_HANDSHAKES = 64              # ...how many may be at that at once...
HANDSHAKES_PER_HOST = 8          # ...and from one address
DRAIN_TIME = 30                  # seconds a stream's game may take to read what it has been sent
WS_CONTROL = 125                 # largest WebSocket control frame (ping, pong, close: RFC 6455)
WS_GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def ip_int(text):
    return struct.unpack(">I", socket.inet_aton(text))[0]


def ws_unmask(data, mask):
    n = len(data)
    key = int.from_bytes((mask * (n // 4 + 1))[:n], "little")
    return (int.from_bytes(data, "little") ^ key).to_bytes(n, "little")


def ws_frame(data, opcode=2):
    n = len(data)
    if n < 126:
        head = struct.pack(">BB", 0x80 | opcode, n)
    elif n < 65536:
        head = struct.pack(">BBH", 0x80 | opcode, 126, n)
    else:
        head = struct.pack(">BBQ", 0x80 | opcode, 127, n)
    return head + data


async def ws_read(reader):
    """One WebSocket frame: (opcode, unmasked payload)."""
    b0, b1 = await reader.readexactly(2)
    n = b1 & 0x7F
    if n == 126:
        n = struct.unpack(">H", await reader.readexactly(2))[0]
    elif n == 127:
        n = struct.unpack(">Q", await reader.readexactly(8))[0]
    if n > STREAM_PACKET * 4 or b0 & 0x08 and n > WS_CONTROL:
        raise ValueError(f"WebSocket frame of {n} bytes (opcode {b0 & 0x0F})")
    mask = await reader.readexactly(4) if b1 & 0x80 else None
    data = await reader.readexactly(n)
    if mask:
        data = ws_unmask(data, mask)
    return b0 & 0x0F, data


class OnlineLobby(relay.Lobby):
    def __init__(self, o):
        super().__init__(o)
        self.nex = make_server(self.nex_send, host="10.13.0.1", state_dir=o.state, log=self.say, verbose=o.nex_log)
        self.nex.part_of = self.part_of
        self.streams = {}        # virtual address -> the player's open NEX streams (writers)
        self.handshakes = collections.Counter()   # address -> its TCP connections not known yet

    def part_of(self, ip):
        c = self.by_ip.get(ip_int(ip))
        return c.part if c is not None else None

    def nex_send(self, addr, sport, data, src_ip=None):
        ip, port = addr
        c = self.by_ip.get(ip_int(ip))
        if c is not None:
            src = ip_int(src_ip) if src_ip else GATEWAY
            self.to(c, self.data_header(c) + struct.pack(">IIHH", src, c.ip, sport, port) + data)   # (in its version)

    def new_ip(self):
        ip = super().new_ip()
        return super().new_ip() if ip == NAT_CHECK_SECOND else ip   # (nncs2's address is the server's)

    def on_game(self, c, p, dst):
        if c.wire is self.su:   # (the game server is WindowSmash's games' only)
            super().on_game(c, p, dst)
            return
        if dst in (GATEWAY, NAT_CHECK_SECOND):
            _src, _dst, sport, dport = struct.unpack_from(">IIHH", p, 4)
            self.nex.datagram((relay.ip_text(c.ip), sport), dport, p[16:], time.monotonic(),
                              None if dst == GATEWAY else relay.ip_text(dst))
            return
        if dst not in relay.BROADCASTS:
            m = self.by_ip.get(dst)
            if m is not None and m is not c and m.wire is not self.su and self.nex.may_talk(relay.ip_text(c.ip), relay.ip_text(dst)):
                self.to(m, p)
                return
        super().on_game(c, p, dst)

    def drop(self, c, why):
        super().drop(c, why)
        self.nex.player_gone(relay.ip_text(c.ip))
        for writer in self.streams.pop(c.ip, ()):   # (its NEX streams go with it, unsent data or not)
            writer.transport.abort()

    # ---- NEX's streams: TCP from the player's game (runtime/hle_online.c) ---------------
    def stream_writer(self, writer, ws, addr):
        """What the game server writes a stream with: dropped, rather than kept in memory, when
        its game does not read what it is sent."""
        def write(data):
            if writer.is_closing():
                return
            if writer.transport.get_write_buffer_size() > STREAM_BACKLOG:
                self.say(f"NEX stream from {addr}: its game does not read (more than {STREAM_BACKLOG} B waiting)")
                writer.transport.abort()
                return
            writer.write(ws_frame(data) if ws else data)
        return write

    async def stream(self, reader, writer):
        """One NEX stream: "SSBUNEX1", the player's virtual address and port and the server port it
        connected to (big-endian u32, u16, u16), then PRUDP Lite - raw, or in WebSocket frames.

        Limits: HANDSHAKE_TIME to say whose it is (MAX_HANDSHAKES at once, HANDSHAKES_PER_HOST from
        one address), STREAMS_PER_PLAYER and MAX_STREAMS, STREAM_RATE for everything a stream
        sends (WebSocket pings too), STREAM_BACKLOG of answers waiting to go out to it. Whose
        stream it is, is told by the player's address alone (players behind one address can
        claim each other's: the preface has no secret), but never takes an open stream's place."""
        peer = writer.get_extra_info("peername") or ("?", 0)
        addr = port = None
        opened = False
        if sum(self.handshakes.values()) >= MAX_HANDSHAKES or self.handshakes[peer[0]] >= HANDSHAKES_PER_HOST:
            self.drops["NEX stream handshakes"] += 1
            writer.transport.abort()
            return
        self.handshakes[peer[0]] += 1
        shaking = True

        def shaken():
            nonlocal shaking
            if shaking:
                shaking = False
                self.handshakes[peer[0]] -= 1
                if self.handshakes[peer[0]] <= 0:
                    del self.handshakes[peer[0]]
        try:
            head = await asyncio.wait_for(reader.readexactly(16), HANDSHAKE_TIME)
            if head[:8] != PREFACE:
                return
            vip, vport, port = struct.unpack(">IHH", head[8:])
            c = self.by_ip.get(vip)
            if c is None or not c.verified or c.at[0] != peer[0] or c.wire is self.su:
                self.say(f"NEX stream from {peer[0]} for {relay.ip_text(vip)}: not that player's")
                return
            mine = self.streams.setdefault(vip, set())
            if len(mine) >= STREAMS_PER_PLAYER or sum(len(s) for s in self.streams.values()) >= MAX_STREAMS:
                self.say(f"NEX stream from {relay.ip_text(vip)}: too many")
                return
            mine.add(writer)
            budget = relay.Bucket(STREAM_RATE, STREAM_RATE * 2, time.monotonic())
            addr = (relay.ip_text(vip), vport)
            first = await asyncio.wait_for(reader.readexactly(1), HANDSHAKE_TIME)
            ws = first == b"G"
            if ws:
                request = first + await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), HANDSHAKE_TIME)
                key = next((line.split(b":", 1)[1].strip() for line in request.split(b"\r\n")
                            if line.lower().startswith(b"sec-websocket-key:")), b"")
                accept = base64.b64encode(hashlib.sha1(key + WS_GUID).digest())
                line = request.split(b"\r\n", 1)[0].decode("latin-1")
                self.nex.debug(f"WebSocket from {addr}: {line[:200]!r}")
                writer.write(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                             b"Sec-WebSocket-Accept: " + accept + b"\r\nSec-WebSocket-Protocol: NEX\r\n\r\n")
            elif first != b"\x80":
                self.say(f"NEX stream from {addr}: neither PRUDP Lite nor a WebSocket ({first.hex()}"
                         f"{(await asyncio.wait_for(reader.read(15), HANDSHAKE_TIME)).hex()}: TLS?)")
                return
            write = self.stream_writer(writer, ws, addr)
            if not self.nex.stream_open(addr, port, write):
                return
            opened = True
            shaken()
            buf = bytearray() if ws else bytearray(first)
            while True:
                if ws:
                    opcode, payload = await ws_read(reader)
                    got = FRAME_COST + len(payload)
                else:
                    payload = await reader.read(65536)
                    if not payload:
                        break
                    opcode, got = 2, len(payload)
                if not budget.take(time.monotonic(), got):
                    raise ValueError("over its rate")
                if opcode == 8:
                    break
                if opcode == 9 and not writer.is_closing():
                    writer.write(ws_frame(payload, 10))
                if opcode not in (0, 2):
                    continue
                buf += payload
                if len(buf) > STREAM_BUFFER:
                    raise ValueError(f"{len(buf)} B not PRUDP Lite packets yet")
                while True:
                    n = lite_size(buf)
                    if n < 0 or n > STREAM_PACKET:
                        raise ValueError(f"not PRUDP Lite: {bytes(buf[:16]).hex()}")
                    if n == 0 or n > len(buf):
                        break
                    self.nex.stream_packet(addr, port, bytes(buf[:n]), time.monotonic())
                    del buf[:n]
                # (a game that does not read what it gets is not read either, and not for long)
                await asyncio.wait_for(writer.drain(), DRAIN_TIME)
        except (asyncio.IncompleteReadError, asyncio.TimeoutError, ConnectionError, OSError):
            pass
        except Exception as e:
            self.say(f"NEX stream from {peer}: {e!r}")
        finally:
            shaken()
            if opened:
                self.nex.stream_closed(addr, port)
            for vip_, mine in list(self.streams.items()):
                mine.discard(writer)
                if not mine:
                    del self.streams[vip_]
            if writer.transport.get_write_buffer_size():
                writer.transport.abort()
            else:
                writer.close()

    async def run(self):
        async def nex_ticks():
            while True:
                await asyncio.sleep(0.1)
                self.nex.tick(time.monotonic())
        ticks = asyncio.ensure_future(nex_ticks())
        try:
            await super().run()
        finally:
            ticks.cancel()


async def main(o):
    loop = asyncio.get_running_loop()
    transport, lobby = await relay.serve(o, OnlineLobby)
    port = transport.get_extra_info('sockname')[1]
    streams = await asyncio.start_server(lobby.stream, o.bind, port)   # NEX's TCP, on the same port number
    relay.Lobby.say(f"SSBU netplay lobby with Online on UDP and TCP {o.bind}:{port} "
                    f"(protocol {relay.PROTOCOL}; NEX at 10.13.0.1)")
    relay.say_more(o, lobby)
    task = asyncio.ensure_future(lobby.run())
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, task.cancel)
        except (NotImplementedError, AttributeError):
            pass
    try:
        await task
    except asyncio.CancelledError:
        pass
    finally:
        lobby.nex.flush(time.monotonic(), force=True)   # (accounts and ratings not written yet)
        streams.close()
        lobby.close_ports()
        relay.Lobby.say("stopped")


def parse_args(argv=None):
    a = argparse.ArgumentParser(add_help=False)
    a.add_argument("--state", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "nex", "state"),
                   help="where the game server keeps accounts and ratings")
    a.add_argument("--nex-log", type=int, default=1, help="game server log: 0 quiet, 1 calls, 2 packets")
    mine, rest = a.parse_known_args(argv)
    o = relay.parse_args(rest)
    o.state, o.nex_log = mine.state, mine.nex_log
    os.makedirs(o.state, exist_ok=True)
    return o


if __name__ == "__main__":
    try:
        asyncio.run(main(parse_args()))
    except KeyboardInterrupt:
        pass
