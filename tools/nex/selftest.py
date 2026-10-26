"""Offline check of the NEX server: clients written to the same rules log in, register, get
matched by AutoMatchmake and play a ranked round.   python -m nex.selftest   (from tools/)"""
import secrets
import struct
import sys

from . import rmc
from .crypto import RC4, kerberos_decrypt, kerberos_encrypt
from .matchmaking import Session
from .prudp import (CONNECT, DATA, F_ACK, F_HAS_SIZE, F_NEED_ACK, F_RELIABLE, O_FRAGMENT, O_INITIAL_SEQ,
                    O_MAX_SUBSTREAM, O_SIGNATURE, O_SUPPORT, SYN, Packet, Signer, _Clear, decode,
                    decode_lite, encode_lite)
from .server import ACCESS_KEY, AUTH_PORT, SECURE_PORT, make_server
from .streams import StreamIn, StreamOut


class Net:
    def __init__(self):
        self.boxes = {}
        self.server = make_server(lambda addr, port, data, src=None: self.boxes.setdefault(addr, []).append(data),
                                  verbose=1, log=print)


LITE = False   # (main runs everything twice: UDP PRUDP V1, then PRUDP Lite as over TCP)


class Client:
    def __init__(self, net, addr, port, session_key=b""):
        self.net, self.addr, self.port = net, addr, port
        self.lite = LITE
        if self.lite:
            box = net.boxes.setdefault(addr, [])
            net.server.stream_open(addr, port, box.append)
        self.signer = Signer(ACCESS_KEY)
        self.session_key = session_key
        self.server_sig = b""
        self.seq = 2      # (the CONNECT is seq 1, as the game sends it)
        self.call = 1
        self.received = []       # RMC messages from the server, in order

    def send(self, p, key, sig):
        p.src, p.dst, p.session = 0xAF, 0xA1, 7
        if self.lite:
            p.src, p.dst = 0x1F, 0x01
            if O_FRAGMENT in p.options:
                p.fragment = p.options.pop(O_FRAGMENT)[0]
            self.net.server.stream_packet(self.addr, self.port, encode_lite(p), 0.0)
        else:
            self.net.server.datagram(self.addr, self.port, self.signer.encode(p, key, sig), 0.0)
        return self.collect()

    def collect(self):
        box = self.net.boxes.get(self.addr, [])
        out = [decode_lite(d) if self.lite else decode(d) for d in box]
        del box[:]
        for q in out:
            if q.type == DATA and not q.flags & F_ACK:
                self.check(q, self.session_key, self.my_sig)
                self.received.append(rmc.decode(self.rc4_in.crypt(q.payload)))
        return out

    def check(self, p, key, sig):
        if self.lite:
            return
        opts = b"".join(bytes((k, len(v))) + v for k, v in sorted(p.options.items()))
        assert p.signature == self.signer.sign(p, opts, key, sig), f"bad signature on {p.describe()}"

    def connect(self, payload=b""):
        syn = Packet(SYN, F_NEED_ACK | F_HAS_SIZE, options={O_SUPPORT: b"\4\0\0\0", O_SIGNATURE: bytes(16),
                                                             O_MAX_SUBSTREAM: b"\0"})
        (ack,) = self.send(syn, b"", b"")
        assert ack.type == SYN and ack.flags & F_ACK
        self.check(ack, b"", b"")
        self.server_sig = ack.options[O_SIGNATURE]
        self.my_sig = secrets.token_bytes(16)
        con = Packet(CONNECT, F_RELIABLE | F_NEED_ACK | F_HAS_SIZE, seq=1, payload=payload,
                     options={O_SUPPORT: b"\4\0\0\0", O_SIGNATURE: self.my_sig, O_INITIAL_SEQ: b"\x34\x12",
                              O_MAX_SUBSTREAM: b"\0"})
        key = self.session_key or b"CD&ML"
        self.rc4_out, self.rc4_in = (_Clear(), _Clear()) if self.lite else (RC4(key), RC4(key))
        (ack,) = self.send(con, b"", self.server_sig)
        assert ack.type == CONNECT and ack.flags & F_ACK, ack.describe()
        self.check(ack, b"", self.my_sig)
        return ack.payload

    def rpc(self, protocol, method, body):
        cid = self.call
        self.call += 1
        p = Packet(DATA, F_RELIABLE | F_NEED_ACK | F_HAS_SIZE, seq=self.seq, options={O_FRAGMENT: b"\0"},
                   payload=self.rc4_out.crypt(rmc.request(protocol, cid, method, body)))
        self.seq += 1
        self.send(p, self.session_key, self.server_sig)
        for m in self.received:
            if not m.request and m.call_id == cid:
                self.received.remove(m)
                assert m.success, m
                return StreamIn(m.body)
        raise AssertionError(f"no answer to {rmc.name_of(protocol, method)}")


def authenticate(net, addr, nsa, name):
    """Logs in on the authentication server: (its Client, pid, ticket, source key)."""
    auth = Client(net, addr, AUTH_PORT)
    auth.connect()
    info = StreamOut()
    info.string(f"ssbu-port.1.{nsa:016x}.{name}")
    info.u32(4)
    info.u8(2)
    info.u32(0)
    param = StreamOut()

    def p(o):
        o.u32(3)
        o.string(str(nsa))
        o.anydata("AuthenticationInfo", [(0, lambda d: None), (0, lambda d: d.write(info.get()))])
        o.bool(False)
        o.u32(40603)
        o.u32(0)
    param.struct(p)
    r = StreamIn(auth.rpc(10, 6, param.get()).struct()[1])
    pid, ticket, _url, _t, _build, source_key = r.pid(), r.buffer(), r.string(), r.datetime(), r.string(), r.string()
    return auth, pid, ticket, source_key


def login(net, addr, nsa, name):
    _auth, pid, ticket, source_key = authenticate(net, addr, nsa, name)
    return connect_secure(net, addr, pid, ticket, source_key, name)


def connect_secure(net, addr, pid, ticket, source_key, name):
    body = kerberos_decrypt(bytes.fromhex(source_key), ticket)
    session_key = body[:32]
    inner = StreamIn(body, 40).buffer()
    check = secrets.randbits(32)
    connect = StreamOut()
    connect.buffer(inner)
    connect.buffer(kerberos_encrypt(session_key, struct.pack("<QII", pid, 1, check)))
    sec = Client(net, addr, SECURE_PORT, session_key)
    assert struct.unpack("<I", StreamIn(sec.connect(connect.get())).buffer())[0] == (check + 1) & 0xFFFFFFFF
    reg = StreamOut()
    reg.list([f"prudp:/address={addr[0]};port=12345;natf=0;natm=0;pmp=0;sid=15;type=2;upnp=0"], StreamOut.string)
    reg.anydata("NintendoLoginData", [(0, lambda d: None), (0, lambda d: d.string("token"))])
    s = sec.rpc(11, 4, reg.get())
    assert s.u32() == rmc.SUCCESS
    sec.pid = pid
    print(f"{name}: pid {pid}, cid {s.u32()}, public {s.string()}")
    return sec


def stats(c, category):
    q = StreamOut()
    q.struct(lambda o: (o.u32(category), o.u32(1000)))
    r = StreamIn(c.rpc(120, 12, q.get()).struct()[1])
    r.u64(), r.u32(), r.pid()
    for _ in range(12):
        r.u32()
    return r.u32()


def auto_matchmake(c, app_buffer=b"rules"):
    ms = Session()
    ms.data_level = 0
    ms.min_participants, ms.max_participants = 2, 2
    ms.game_mode, ms.attribs, ms.app_buffer = 3, [1, 0, 0, 0, 0, 0], app_buffer
    param = StreamOut()

    def p(o):
        ms.encode(o, 0)
        o.list([], StreamOut.pid)
        o.u32(0)
        o.u32(0)
        o.string("")
        o.u16(1)

        def crit(q):
            q.list(["", "", "", "", "", ""], StreamOut.string)
            q.string("3")
            q.string("")
            q.string("")
            q.string("")
            q.bool(True)
            q.bool(True)
            q.bool(False)
            q.u32(0)
            q.u16(1)
        o.u32(1)
        o.struct(crit)
        o.list([], StreamOut.u32)
    param.struct(p)
    got = Session.parse(c.rpc(109, 40, param.get()))
    return got


def main():
    net = Net()
    a = login(net, ("10.13.0.2", 49152), 0x1111, "Alpha")
    b = login(net, ("10.13.0.3", 49152), 0x2222, "Bravo")
    print("ratings", stats(a, 7), stats(b, 7))
    ga = auto_matchmake(a)
    gb = auto_matchmake(b)
    print("gatherings", ga.gid, gb.gid, "owner", gb.owner)
    assert ga.gid == gb.gid and gb.owner == a.pid
    a.collect()
    notes = [m for m in a.received if m.request and m.protocol == 14]
    assert notes, "the owner was not told"

    evs = [StreamIn(StreamIn(n.body).struct()[1]) for n in notes]
    assert any((e.pid(), e.u32()) == (b.pid, 3001) for e in evs)
    assert net.server.may_talk("10.13.0.2", "10.13.0.3")
    urls = StreamOut()
    urls.u32(ga.gid)
    print("session URLs", b.rpc(21, 41, urls.get()).list(StreamIn.string))
    q = StreamOut()
    q.struct(lambda o: (o.u32(7), o.u32(ga.gid), o.list([a.pid, b.pid], StreamOut.pid)))
    rid = a.rpc(120, 1, q.get()).u64()
    end = StreamOut()

    def er(o):
        o.u64(rid)

        def res(pid, wl):
            def f(x):
                x.struct(lambda y: (y.pid(pid), y.u32(0), y.u32(wl), y.s32(0), y.qbuffer(b"")))
            return f
        o.u32(2)
        res(a.pid, 1)(o)
        res(b.pid, 2)(o)
    end.struct(er)
    a.rpc(120, 3, end.get())
    ra, rb = stats(a, 7), stats(b, 7)
    print("after the round", ra, rb)
    assert ra > 1000 > rb
    print("ok")
    return 0


if __name__ == "__main__":
    main()
    LITE = True
    print("---- the same over PRUDP Lite (streams)")
    main()
    print("---- what untrusted clients may not do (nex/test_hardening.py)")
    from . import test_hardening
    sys.exit(test_hardening.run())
