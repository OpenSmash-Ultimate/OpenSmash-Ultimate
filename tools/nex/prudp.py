"""PRUDP V1 (NEX's reliable UDP), server side, transport-agnostic: datagrams come in through
PrudpServer.datagram() and go out through the send callback, so the server can sit behind the
netplay lobby's virtual LAN as well as on a real socket.

Packet: EA D0 | header (12) | signature (16) | options | payload, little-endian.
  header: version 1, options size, payload size, source vport, destination vport,
          type | flags << 4 (u16), session id, substream id, sequence id (u16)
  signature: HMAC-MD5, key MD5(access key), over header[4:12] + session key + u32(sum of the
             access key's bytes) + the peer's connection signature + options + payload
DATA payloads are RC4: key "CD&ML" on the authentication server, the Kerberos session key on
the secure server; one stream per direction, in sequence order.

PRUDP Lite, NEX's form for streams (TCP / WebSocket, which already carry it reliably and, on a
console, encrypted): 80 | options size | payload size (u16) | stream types (src << 4 | dst) |
source port | destination port | fragment id | type | flags << 4 (u16) | sequence id (u16) |
options | payload - no signature, no RC4, no session id. Options: 0 support (SYN, CONNECT), 1 the
server's connection signature (SYN-ACK), 0x80 the client's Lite signature (CONNECT)."""
import hashlib
import hmac
import secrets
import struct

from .crypto import RC4

SYN, CONNECT, DATA, DISCONNECT, PING = range(5)
LITE_PING = 8                    # (the keep-alive SSBU's NEX sends over a stream)
TYPE_NAMES = {SYN: "SYN", CONNECT: "CONNECT", DATA: "DATA", DISCONNECT: "DISCONNECT", PING: "PING", LITE_PING: "PING8"}
F_ACK, F_RELIABLE, F_NEED_ACK, F_HAS_SIZE, F_MULTI_ACK = 0x1, 0x2, 0x4, 0x8, 0x200
O_SUPPORT, O_SIGNATURE, O_FRAGMENT, O_INITIAL_SEQ, O_MAX_SUBSTREAM = 0, 1, 2, 3, 4
FRAGMENT = 1300
RESEND_AFTER = 0.5
RESEND_LIMIT = 12
IDLE_TIMEOUT = 40.0
# What one client may make the server keep (clients are not trusted):
MAX_MESSAGE = 64 * 1024          # a message's bytes while it comes in fragments (NEX's requests are small)
HOLD_WINDOW = 64                 # how far ahead of the next one a packet may come and be held
MAX_HELD = 64 * 1024             # payload bytes held out of order
MAX_UNACKED = 1024               # our packets the client has not acknowledged (UDP)
MAX_CONNS_PER_HOST = 4           # connections from one address (a new one takes the place of the
                                 # oldest: a game that reconnects is not locked out by its old ones)
MAX_CONNS = 1024                 # connections in all


def host_of(addr):
    """The address part of a transport's address (a virtual address and a port)."""
    return addr[0] if isinstance(addr, tuple) else addr


class Packet:
    __slots__ = ("src", "dst", "type", "flags", "session", "substream", "seq", "signature", "options", "payload",
                 "raw_options", "streams", "fragment")

    def __init__(self, type_=DATA, flags=0, src=0, dst=0, session=0, substream=0, seq=0, options=None, payload=b""):
        self.type, self.flags, self.src, self.dst = type_, flags, src, dst
        self.session, self.substream, self.seq = session, substream, seq
        self.options = options if options is not None else {}
        self.payload = payload
        self.signature = b"\0" * 16
        self.raw_options = b""
        self.streams = 0xAA      # (Lite) source and destination stream types
        self.fragment = 0        # (Lite) fragment id; V1 keeps it in option 2

    def describe(self):
        f = [n for n, b in (("ACK", F_ACK), ("REL", F_RELIABLE), ("NEED", F_NEED_ACK), ("SIZE", F_HAS_SIZE),
                            ("MULTI", F_MULTI_ACK)) if self.flags & b]
        return (f"{TYPE_NAMES.get(self.type, self.type)}[{'|'.join(f)}] {self.src:02x}->{self.dst:02x} "
                f"sess {self.session} sub {self.substream} seq {self.seq} opts {sorted(self.options)} {len(self.payload)} B")


def encode_options(p):
    out = bytearray()
    for oid in sorted(p.options):
        v = p.options[oid]
        out += bytes((oid, len(v))) + v
    return bytes(out)


def decode(data):
    if len(data) < 30 or data[:2] != b"\xEA\xD0":
        return None
    version, optsize, size, src, dst, tf, session, substream, seq = struct.unpack_from("<BBHBBHBBH", data, 2)
    if version != 1 or 30 + optsize + size > len(data):
        return None
    p = Packet(tf & 0xF, tf >> 4, src, dst, session, substream, seq)
    p.signature = data[14:30]
    p.raw_options = data[30:30 + optsize]
    i, opts = 0, {}
    while i + 2 <= optsize:
        oid, n = p.raw_options[i], p.raw_options[i + 1]
        opts[oid] = p.raw_options[i + 2:i + 2 + n]
        i += 2 + n
    p.options = opts
    p.payload = data[30 + optsize:30 + optsize + size]
    return p


LITE_HEADER = 12


def lite_size(data, pos=0):
    """The size of the Lite packet at pos, or 0 if its header is not all there yet; -1 if it is
    not a Lite packet."""
    if len(data) - pos < LITE_HEADER:
        return 0
    if data[pos] != 0x80:
        return -1
    optsize, size = data[pos + 1], struct.unpack_from("<H", data, pos + 2)[0]
    return LITE_HEADER + optsize + size


def decode_lite(data):
    n = lite_size(data)
    if n <= 0 or n > len(data):
        return None
    optsize, size, streams, src, dst, frag, tf, seq = struct.unpack_from("<BHBBBBHH", data, 1)
    p = Packet(tf & 0xF, tf >> 4, src, dst, 0, 0, seq)
    p.streams, p.fragment = streams, frag
    p.raw_options = data[LITE_HEADER:LITE_HEADER + optsize]
    i, opts = 0, {}
    while i + 2 <= optsize:
        oid, m = p.raw_options[i], p.raw_options[i + 1]
        opts[oid] = p.raw_options[i + 2:i + 2 + m]
        i += 2 + m
    p.options = opts
    p.payload = data[LITE_HEADER + optsize:LITE_HEADER + optsize + size]
    return p


def encode_lite(p):
    options = encode_options(p)
    return struct.pack("<BBHBBBBHH", 0x80, len(options), len(p.payload), p.streams, p.src, p.dst, p.fragment,
                       p.type | p.flags << 4, p.seq) + options + p.payload


class _Clear:
    """(Lite: no PRUDP encryption.)"""
    @staticmethod
    def crypt(data):
        return data


class Signer:
    def __init__(self, access_key):
        key = access_key.encode()
        self.mac_key = hashlib.md5(key).digest()
        self.key_sum = struct.pack("<I", sum(key))

    def header(self, p, optsize):
        return struct.pack("<BBHBBHBBH", 1, optsize, len(p.payload), p.src, p.dst, p.type | p.flags << 4,
                           p.session, p.substream, p.seq)

    def sign(self, p, options, session_key, connection_signature):
        h = self.header(p, len(options))
        m = hmac.new(self.mac_key, digestmod=hashlib.md5)
        m.update(h[4:])
        m.update(session_key)
        m.update(self.key_sum)
        m.update(connection_signature)
        m.update(options)
        m.update(p.payload)
        return m.digest()

    def encode(self, p, session_key, connection_signature):
        options = encode_options(p)
        p.signature = self.sign(p, options, session_key, connection_signature)
        return b"\xEA\xD0" + self.header(p, len(options)) + p.signature + options + p.payload


class Connection:
    """One client of a PRUDP server."""

    def __init__(self, server, addr, vport, now):
        self.server, self.addr, self.vport = server, addr, vport   # addr: the transport's (anything hashable)
        self.local_vport = None
        self.state = "syn"
        self.streams = 0xAA
        self.connect_ack = None
        self.client_session = 0
        self.session = secrets.randbits(8)
        self.server_signature = b""
        self.client_signature = b""
        self.support = b"\0\0\0\0"
        self.session_key = b""
        self.rc4_in = self.rc4_out = None
        self.in_seq = 1          # next reliable sequence id expected from the client (substream 0)
        self.held = {}           # out-of-order reliable packets, by sequence id
        self.held_bytes = 0
        self.fragments = []
        self.fragment_bytes = 0
        self.out_seq = 1
        self.ping_seq = 1
        self.unacked = {}        # our reliable packets: seq -> [bytes, last sent, tries]
        self.seen = now
        self.pid = 0             # (set by the service once the client is known)
        self.user = {}           # service state

    def __repr__(self):
        return f"<{self.server.name} {self.addr} pid {self.pid}>"


class PrudpServer:
    """send(addr, bytes) puts a datagram out; on_connect(conn, payload) -> CONNECT-ACK payload, or
    None to refuse; on_message(conn, bytes) gets each reassembled DATA message; on_close(conn)."""

    def __init__(self, name, access_key, send, on_connect, on_message, on_close, secure, log=None, lite=False):
        self.name, self.secure, self.lite = name, secure, lite
        self.signer = Signer(access_key)
        self.send_raw, self.on_connect, self.on_message, self.on_close = send, on_connect, on_message, on_close
        self.log = log or (lambda *a: None)
        self.conns = {}          # (addr, client vport) -> Connection
        self.hosts = {}          # host_of(addr) -> its Connections
        self.sig_key = secrets.token_bytes(16)

    # ---- in ------------------------------------------------------------------------
    def datagram(self, addr, data, now):
        """One packet (a UDP datagram, or one Lite packet cut from a stream)."""
        p = decode_lite(data) if self.lite else decode(data)
        if p is None:
            self.log(f"{self.name}: not a PRUDP {'Lite' if self.lite else 'V1'} packet from {addr}: {data[:32].hex()}")
            return
        self.log(f"{self.name}: <- {addr} {p.describe()}")
        key = (addr, p.src)
        c = self.conns.get(key)
        if p.type == SYN:
            if c is not None:
                self.close(c, "new SYN")
            mine = self.hosts.setdefault(host_of(addr), set())
            if len(mine) >= MAX_CONNS_PER_HOST:
                self.close(min(mine, key=lambda o: o.seen), "too many connections from there")
            if len(self.conns) >= MAX_CONNS:
                self.log(f"{self.name}: SYN from {addr} turned away: {len(self.conns)} connections")
                return
            c = self.conns[key] = Connection(self, addr, p.src, now)
            self.hosts.setdefault(host_of(addr), set()).add(c)
            c.local_vport = p.dst
            c.streams = (p.streams & 0xF) << 4 | p.streams >> 4
            self.on_syn(c, p)
            return
        if c is None:
            self.log(f"{self.name}: {p.describe()} from {addr} without a connection")
            return
        c.seen = now
        if p.type == CONNECT:
            self.on_connect_packet(c, p)
        elif p.type == DATA:
            self.on_data(c, p, now)
        elif p.type in (PING, LITE_PING):
            if p.flags & F_NEED_ACK:
                self.send(c, Packet(p.type, F_ACK | (p.flags & F_HAS_SIZE), seq=p.seq, substream=p.substream))
        elif p.type == DISCONNECT:
            ack = Packet(DISCONNECT, F_ACK | (p.flags & F_HAS_SIZE), seq=p.seq, substream=p.substream)
            for _ in range(3):
                self.send(c, ack)
            self.close(c, "client disconnected")

    def on_syn(self, c, p):
        c.support = p.options.get(O_SUPPORT, b"\4\0\0\0")
        c.server_signature = hmac.new(self.sig_key, repr(c.addr).encode(), hashlib.md5).digest()
        ack = Packet(SYN, F_ACK | (p.flags & F_HAS_SIZE), seq=p.seq, substream=p.substream)
        if self.lite:
            ack.options = {O_SUPPORT: c.support, O_SIGNATURE: c.server_signature}
        else:
            ack.options = {O_SUPPORT: c.support, O_SIGNATURE: c.server_signature, O_MAX_SUBSTREAM: bytes((0,))}
        self.send(c, ack, session_key=b"", signature=b"", session=0)
        self.log(f"{self.name}: SYN from {c.addr} vport {c.vport:02x}, support {c.support.hex()}")

    def on_connect_packet(self, c, p):
        if c.state == "open":   # (our CONNECT-ACK got lost: send it again)
            if c.connect_ack:
                self.send_raw(c.addr, c.connect_ack)
            return
        c.client_signature = p.options.get(O_SIGNATURE, b"")
        c.client_session = p.session
        c.in_seq = (p.seq + 1) & 0xFFFF   # (the CONNECT is the reliable stream's first packet)
        answer = self.on_connect(c, p.payload)
        if answer is None:
            self.log(f"{self.name}: CONNECT from {c.addr} refused")
            self.close(c, "refused")
            return
        ack = Packet(CONNECT, F_ACK | (p.flags & F_HAS_SIZE), seq=p.seq, substream=p.substream, payload=answer)
        if self.lite:
            ack.options = {O_SUPPORT: c.support}
        else:
            ack.options = {O_SUPPORT: c.support, O_SIGNATURE: bytes(16),
                           O_INITIAL_SEQ: struct.pack("<H", 0), O_MAX_SUBSTREAM: bytes((0,))}
        c.connect_ack = self.send(c, ack, session_key=b"", signature=c.client_signature)
        if self.lite:
            c.rc4_in = c.rc4_out = _Clear()
        else:
            key = c.session_key if self.secure else b"CD&ML"
            c.rc4_in, c.rc4_out = RC4(key), RC4(key)
        c.state = "open"
        self.log(f"{self.name}: connected {c.addr} (session {c.client_session})")

    def on_data(self, c, p, now):
        if p.flags & F_ACK:
            if p.flags & F_MULTI_ACK:
                self.on_multi_ack(c, p)
            else:
                c.unacked.pop(p.seq, None)
            return
        if p.flags & F_MULTI_ACK:   # (sent without FLAG_ACK by some versions)
            self.on_multi_ack(c, p)
            return
        if not p.flags & F_RELIABLE:
            self.log(f"{self.name}: unreliable DATA ignored: {p.describe()}")
            return
        taken = c.state == "open" and p.substream == 0
        ahead = (p.seq - c.in_seq) & 0xFFFF   # (>= 0x8000: old, a resend of something already taken)
        if taken and 0 < ahead < 0x8000 and (ahead >= HOLD_WINDOW or c.held_bytes + len(p.payload) > MAX_HELD):
            self.log(f"{self.name}: {p.describe()} from {c.addr} too far ahead of {c.in_seq}: dropped")
            return   # (not acknowledged: a client sends it again once it has caught up)
        if p.flags & F_NEED_ACK:
            self.send(c, Packet(DATA, F_ACK | (p.flags & F_HAS_SIZE), seq=p.seq, substream=p.substream))
        if not taken or ahead >= 0x8000 or p.seq in c.held:
            return
        c.held[p.seq] = p
        c.held_bytes += len(p.payload)
        while c.in_seq in c.held:
            q = c.held.pop(c.in_seq)
            c.held_bytes -= len(q.payload)
            c.in_seq = (c.in_seq + 1) & 0xFFFF
            part = c.rc4_in.crypt(q.payload)
            c.fragments.append(part)
            c.fragment_bytes += len(part)
            if c.fragment_bytes > MAX_MESSAGE:
                self.close(c, f"a message of more than {MAX_MESSAGE} bytes")
                return
            frag = q.fragment if self.lite else q.options.get(O_FRAGMENT, b"\0")[0]
            if frag == 0:
                message = b"".join(c.fragments)
                c.fragments, c.fragment_bytes = [], 0
                if message:
                    self.on_message(c, message)
                    if self.conns.get((c.addr, c.vport)) is not c:   # (the message closed it)
                        return

    def on_multi_ack(self, c, p):
        pl = p.payload
        if p.seq == 0 and len(pl) >= 4:   # new form: substream, count, everything up to, extras
            count, upto = pl[1], struct.unpack_from("<H", pl, 2)[0]
            extra = [struct.unpack_from("<H", pl, 4 + 2 * i)[0] for i in range(count) if 6 + 2 * i <= len(pl)]
        else:                             # old form: everything up to the packet's own seq, extras
            upto = p.seq
            extra = [struct.unpack_from("<H", pl, 2 * i)[0] for i in range(len(pl) // 2)]
        for seq in list(c.unacked):
            if (upto - seq) & 0xFFFF < 0x8000 or seq in extra:
                del c.unacked[seq]

    # ---- out -----------------------------------------------------------------------
    def send(self, c, p, session_key=None, signature=None, session=None):
        p.src, p.dst = c.local_vport, c.vport
        if self.lite:
            p.streams = c.streams
            if O_FRAGMENT in p.options:
                p.fragment = p.options.pop(O_FRAGMENT)[0]
            data = encode_lite(p)
        else:
            p.session = c.session if session is None else session
            data = self.signer.encode(p, c.session_key if session_key is None else session_key,
                                      c.client_signature if signature is None else signature)
        self.log(f"{self.name}: -> {c.addr} {p.describe()}")
        self.send_raw(c.addr, data)
        return data

    def send_message(self, c, message, now):
        """One RMC message to the client, reliably (fragmented as needed). Fragment ids are a byte:
        1, 2, ... 255, 1, ... for all but the last, which is 0 (NexServer keeps its answers under
        255 fragments; this only never fails half-way)."""
        if c.state != "open":
            return
        parts = [message[i:i + FRAGMENT] for i in range(0, len(message), FRAGMENT)] or [b""]
        for n, part in enumerate(parts):
            frag = 0 if n == len(parts) - 1 else n % 255 + 1
            p = Packet(DATA, F_RELIABLE | F_NEED_ACK | F_HAS_SIZE, seq=c.out_seq, substream=0,
                       options={O_FRAGMENT: bytes((frag,))}, payload=c.rc4_out.crypt(part))
            c.out_seq = (c.out_seq + 1) & 0xFFFF or 1
            data = self.send(c, p)
            if not self.lite:   # (a stream already delivers: nothing to send again)
                c.unacked[p.seq] = [data, now, 1]

    def tick(self, now):
        for c in list(self.conns.values()):
            if now - c.seen > IDLE_TIMEOUT:
                self.close(c, "timed out")
                continue
            if len(c.unacked) > MAX_UNACKED:
                self.close(c, f"{len(c.unacked)} packets unacknowledged")
                continue
            for seq, entry in list(c.unacked.items()):
                if now - entry[1] >= RESEND_AFTER * min(entry[2], 4):
                    if entry[2] >= RESEND_LIMIT:
                        self.close(c, f"packet {seq} never acknowledged")
                        break
                    self.send_raw(c.addr, entry[0])
                    entry[1], entry[2] = now, entry[2] + 1

    def close(self, c, why):
        if self.conns.get((c.addr, c.vport)) is c:
            del self.conns[(c.addr, c.vport)]
            mine = self.hosts.get(host_of(c.addr))
            if mine is not None:
                mine.discard(c)
                if not mine:
                    del self.hosts[host_of(c.addr)]
            c.state = "closed"
            c.why = why
            self.log(f"{self.name}: {c.addr} gone ({why})")
            self.on_close(c)
