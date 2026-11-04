#!/usr/bin/env python3
"""Self-test of ssbu_lobby.py without the game: two fake players on 127.0.0.1 (host creates a room, the other lists it,
joins it, both exchange a game datagram through the lobby, both receive PEERS for direct paths), and the HTTP status
endpoint (--status-port: counts only, CORS header, no addresses or names).
O1 room browser: players of two builds (another game version, another mod set) and an older game in one lobby code:
the older game sees only its own build's rooms; LIST flag 1 lists every build with the host's build at the end; ROOMS
names host, players, build and mods; a join across builds never reaches the other host.
O2 matchmaking: only players of the same build and lobby code are paired, the one that waited longer (or the one with an
open room) hosts, the guest is told when the host's room opens and sees it first in its list, both are told when they
are in one room; cancel, leaving, timeout and expiry; an older game is never matched; PONG counts the searching players.
Rollback matchmaking (1.4, MATCH op 2): paired only with another rollback search, the one that waited longer is player 1,
RB between the pair is relayed and makes both TOGETHER, on the public code a stranger's RB never reaches them.
Routers that change ports (1.5): PONG says where the lobby sees a game and where its PROBE to the NAT port came from;
a room's PEERS give a player behind another router its mapped port (PING 'N' 'P') at its public address, never its LAN
address, and end with 'N' '2' and its PROBE port.
    python test_ssbu_lobby.py        (starts the lobby itself on a free port; standard library only)
    LOBBY_EXE=<minilobby.exe> python test_ssbu_lobby.py   (the same against net_minilobby.c built standalone:
        clang -DMINILOBBY_MAIN -x c net_minilobby.c -o minilobby.exe -lws2_32 -ladvapi32; no status endpoint there)"""
import json
import os
import socket
import struct
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ssbu_lobby as L  # noqa: E402


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def free_tcp_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def get_status(port):
    with urllib.request.urlopen("http://127.0.0.1:%d/status" % port, timeout=3) as r:
        return r.status, r.headers, r.read().decode()


class Fake:
    def __init__(self, lobby, name, build=b"build-x", mods=None, code=b"test", bind="127.0.0.1"):
        self.build, self.mods, self.code = build, mods, code
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.bind((bind, 0))
        self.s.settimeout(0.2)
        self.lobby, self.name, self.nonce = lobby, name, os.urandom(8)
        self.got = []

    def tx(self, kind, body):
        self.s.sendto(L.hdr(kind) + body, self.lobby)

    def rx(self, want, secs=2.0):
        end = time.time() + secs
        while time.time() < end:
            for i, (k, b) in enumerate(self.got):
                if k == want:
                    del self.got[i]
                    return b
            try:
                p, _ = self.s.recvfrom(65536)
                self.got.append((p[3], p[4:]))
            except socket.timeout:
                pass
        raise AssertionError("%s: no packet of type %d" % (self.name, want))

    def join(self):
        port = self.s.getsockname()[1]
        self.tx(L.JOIN, self.nonce + self.code.ljust(32, b"\0") + self.name.encode().ljust(32, b"\0") + self.build.ljust(40, b"\0")
                + struct.pack(">IHB", 0x7F000001, port, 1) + (b"MS" + self.mods if self.mods is not None else b""))
        b = self.rx(L.WELCOME)
        assert b[:8] == self.nonce
        self.ip, self.cookie = struct.unpack_from(">IQ", b, 8)
        return self.ip

    def match(self, op, sid):
        self.tx(L.MATCH, struct.pack(">QBI", self.cookie, op, sid))

    def state(self, secs=2.0):
        """the next MATCH_STATE: (id, state, role, flags, peer, peer name, searching, players, waited)"""
        b = self.rx(L.MATCH_STATE, secs)
        assert len(b) == 49, len(b)
        sid, st, role, flags, peer = struct.unpack_from(">IBBBI", b)
        return (sid, st, role, flags, peer, b[11:43].rstrip(b"\0")) + struct.unpack_from(">HHH", b, 43)

    def until(self, want, secs=3.0):
        """MATCH_STATEs until one has state want (earlier ones skipped)"""
        end = time.time() + secs
        while time.time() < end:
            try:
                st = self.state(0.3)
            except AssertionError as e:
                if "no packet" not in str(e):
                    raise
                continue
            if st[1] == want:
                return st
        raise AssertionError("%s: no MATCH_STATE %d" % (self.name, want))

    def ping(self):
        self.tx(L.PING, struct.pack(">QQB", self.cookie, 1234, 0))
        return self.rx(L.PONG)

    def ldn(self, dst, kind, payload):
        self.tx(L.DATA, struct.pack(">IIHH", self.ip, dst, L.LDN_PORT, L.LDN_PORT) + bytes((ord("L"), kind, 0, 0)) + payload)


def network_info(host_ip, session, station_ip=None):
    n = bytearray(L.NI_SIZE)
    n[L.NI_SESSION:L.NI_SESSION + 16] = session
    n[L.NI_NODE_MAX] = 8
    n[L.NI_NODE_COUNT] = 1
    struct.pack_into("<I", n, L.NI_NODES, host_ip)
    n[L.NI_NODES + 0xB] = 1
    n[L.NI_NODES + 0xC:L.NI_NODES + 0xC + 4] = b"Host"
    if station_ip:
        o = L.NI_NODES + L.NODE_LEN
        struct.pack_into("<I", n, o, station_ip)
        n[o + 0xB] = 1
        n[L.NI_NODE_COUNT] = 2
    return bytes(n)


def mods(*entries):
    return bytes((len(entries),)) + b"".join(struct.pack(">IB", h, len(n)) + n for n, h in entries)


def none_of(f, kind, secs=0.6):
    try:
        f.rx(kind, secs)
    except AssertionError:
        return True
    return False


def many_rooms(lob):
    """More rooms than the game's list holds (LIST_MAX): the browser gets them all, the game's list a page, or the picked one first."""
    B = b"ssbu-recomp-ldn1-13.0.5"
    hosts = [Fake(lob, "H%02d" % i, B, mods(), b"many") for i in range(30)]
    ips = [h.join() for h in hosts]
    for h, ip in zip(hosts, ips):
        h.ldn(L.BROADCASTS[0], L.M_ADVERTISE, network_info(ip, os.urandom(16)))
    me = Fake(lob, "Me", B, mods(), b"many")
    me.join()
    time.sleep(0.3)

    def listing(body):
        me.tx(L.LIST, struct.pack(">QI", me.cookie, 7) + body)
        got = []
        while True:
            k = me.rx(L.LIST_END, 0.05) if any(k == L.LIST_END for k, _ in me.got) else None
            if k is not None:
                return got, k[4]
            try:
                got.append(struct.unpack_from(">I", me.rx(L.LISTING, 0.5), 4)[0])
            except AssertionError:
                pass

    first, n = listing(b"")
    assert n == L.LIST_MAX == len(first), (n, len(first))
    second, n = listing(struct.pack(">BIH", 1 | 4, 0, 1))
    assert n == 30 - L.LIST_MAX and not set(first) & set(second) and set(first) | set(second) == set(ips), (n, len(second))
    picked = [ip for ip in ips if ip not in first][0]
    top, n = listing(struct.pack(">BI", 1 | 2, picked))
    assert top[0] == picked and n == L.LIST_MAX and top.count(picked) == 1, (top[:3], n)
    me.tx(L.ROOMS, struct.pack(">QI", me.cookie, 8))
    infos = [me.rx(L.ROOM_INFO) for _ in range(30)]
    end = me.rx(L.ROOMS_END)
    assert len({struct.unpack_from(">I", b, 4)[0] for b in infos}) == 30 and end[4] == 30 and struct.unpack_from(">H", end, 5)[0] == 30, end
    for x in hosts + [me]:
        x.tx(L.LEAVE, struct.pack(">Q", x.cookie))
    time.sleep(0.3)


def room_browser(lob):
    """O1: three builds in the lobby code "test"; an older game, two newer ones (another version, another mod set)."""
    v4 = Fake(lob, "V4", b"ssbu-recomp-ldn1-13.0.4", mods())
    v5m = Fake(lob, "V5m", b"ssbu-recomp-ldn1-13.0.5+m1234abcd", mods((b"Alpha", 0x11), (b"Beta", 0x22)))
    old = Fake(lob, "Old", b"ssbu-recomp-ldn1-13.0.5")             # an older game: no mod block, no flag
    v5 = Fake(lob, "V5", b"ssbu-recomp-ldn1-13.0.5", mods())
    i4, i5m, iold, i5 = v4.join(), v5m.join(), old.join(), v5.join()
    s4, s5m = os.urandom(16), os.urandom(16)
    v4.ldn(L.BROADCASTS[0], L.M_ADVERTISE, network_info(i4, s4))
    v5m.ldn(L.BROADCASTS[0], L.M_ADVERTISE, network_info(i5m, s5m))
    time.sleep(0.2)
    # the older game: LIST without the flag -> only its own build (none of the two rooms), 0 rooms
    old.tx(L.LIST, struct.pack(">QI", old.cookie, 1))
    assert old.rx(L.LIST_END)[4] == 0 and none_of(old, L.LISTING, 0.2), "an older game saw a room of another build"
    # a newer game: LIST flag 1 -> both rooms, each with its host's build at the end
    v5.tx(L.LIST, struct.pack(">QIB", v5.cookie, 2, 1))
    got = {}
    for _ in range(2):
        b = v5.rx(L.LISTING)
        assert len(b) == 8 + L.NI_SIZE + 40, len(b)
        got[struct.unpack_from(">I", b, 4)[0]] = b[8 + L.NI_SIZE:].rstrip(b"\0")
    assert v5.rx(L.LIST_END)[4] == 2
    assert got == {i4: b"ssbu-recomp-ldn1-13.0.4", i5m: b"ssbu-recomp-ldn1-13.0.5+m1234abcd"}, got
    # ROOMS: host, players, build, mods; the room's own host sees it flagged as its own
    v5.tx(L.ROOMS, struct.pack(">QI", v5.cookie, 3))
    info = {}
    for _ in range(2):
        b = v5.rx(L.ROOM_INFO)
        seq, host, players, most, flags = struct.unpack_from(">IIBBB", b)
        name = b[11:43].rstrip(b"\0")
        build = b[43:83].rstrip(b"\0")
        info[name] = (seq, host, players, most, flags, build, b[83:])
    assert v5.rx(L.ROOMS_END)[4] == 2
    assert info[b"V4"] == (3, i4, 1, 8, 1 | 4, b"ssbu-recomp-ldn1-13.0.4", b"\0"), info[b"V4"]
    assert info[b"V5m"][:6] == (3, i5m, 1, 8, 1 | 4, b"ssbu-recomp-ldn1-13.0.5+m1234abcd") and info[b"V5m"][6] == mods((b"Alpha", 0x11), (b"Beta", 0x22)), info[b"V5m"]
    v4.tx(L.ROOMS, struct.pack(">QI", v4.cookie, 4))
    mine = [v4.rx(L.ROOM_INFO) for _ in range(2)]
    assert any(struct.unpack_from(">I", b, 4)[0] == i4 and b[10] & 2 for b in mine), "the host's own room is not flagged as its own"
    v4.rx(L.ROOMS_END)
    # an older game's room: listed with flag 4 off (its mods are not known)
    so = os.urandom(16)
    old.ldn(L.BROADCASTS[0], L.M_ADVERTISE, network_info(iold, so))
    time.sleep(0.2)
    v5.tx(L.ROOMS, struct.pack(">QI", v5.cookie, 5))
    seen = {}
    for _ in range(3):
        b = v5.rx(L.ROOM_INFO)
        seen[b[11:43].rstrip(b"\0")] = b[10]
    v5.rx(L.ROOMS_END)
    assert seen[b"Old"] & 4 == 0 and seen[b"Old"] & 1, seen
    # a join across builds never reaches the other host (the lobby parts LDN messages by build)
    v5.ldn(i4, L.M_JOIN, s4 + b"V5".ljust(32, b"\0") + b"\0\0")
    assert none_of(v4, L.DATA), "a join of another build reached the host"
    # the same build joins: the older game's room, from the newer game of the same build without mods
    v5.ldn(iold, L.M_JOIN, so + b"V5".ljust(32, b"\0") + b"\0\0")
    j = old.rx(L.DATA)
    assert j[12:14] == bytes((ord("L"), L.M_JOIN)), j[:16]
    # a broken mod block is dropped (the player stays, its mods unknown)
    bad = Fake(lob, "Bad", b"ssbu-recomp-ldn1-13.0.5+m0", bytes((3, 0, 0, 0, 1, 40)) + b"x" * 40)
    ib = bad.join()
    bad.ldn(L.BROADCASTS[0], L.M_ADVERTISE, network_info(ib, os.urandom(16)))
    time.sleep(0.2)
    v5.tx(L.ROOMS, struct.pack(">QI", v5.cookie, 6))
    seen = {}
    for _ in range(4):
        b = v5.rx(L.ROOM_INFO)
        seen[b[11:43].rstrip(b"\0")] = (b[10], b[83:])
    v5.rx(L.ROOMS_END)
    assert seen[b"Bad"][0] & 4 == 0, seen[b"Bad"]
    for f in (v4, v5m, old, v5, bad):
        f.tx(L.LEAVE, struct.pack(">Q", f.cookie))
    time.sleep(0.2)


def matchmaking(lob):
    """O2: the lobby runs with a 3 s search limit and a 5 s match limit (--search-secs / SSBU_LOBBY_SEARCH_SECS)."""
    X, Y = b"ssbu-recomp-ldn1-13.0.5", b"ssbu-recomp-ldn1-13.0.4"
    a, b, y, e, old = Fake(lob, "MA", X, mods()), Fake(lob, "MB", X, mods()), Fake(lob, "MY", Y, mods()), Fake(lob, "ME", X, mods(), b"other"), Fake(lob, "MOld", X)
    ia, ib, iy, ie, iold = a.join(), b.join(), y.join(), e.join(), old.join()
    a.match(1, 101)
    st = a.state()
    assert st[:5] == (101, L.MS_SEARCHING, 0, 0, 0) and st[6] == 1 and st[7] == 3, st      # searching 1, players of X/test: MA, MB, MOld
    y.match(1, 201)                                  # another version: never paired with MA
    assert y.state()[1] == L.MS_SEARCHING
    e.match(1, 301)                                  # another lobby code: never paired with MA
    assert e.state()[1] == L.MS_SEARCHING
    pong = a.ping()
    assert len(pong) >= 16 and struct.unpack_from(">HHHH", pong, 8) == (3, 0, 1, 4), pong.hex()   # players, rooms, searching, all builds of "test"
    a.match(1, 101)                                  # the same search asked again: still searching, no new search
    assert a.state()[1] == L.MS_SEARCHING
    time.sleep(0.3)
    b.match(1, 102)
    sa, sb = a.until(L.MS_MATCHED), b.until(L.MS_MATCHED)
    assert sa[:6] == (101, L.MS_MATCHED, L.ROLE_HOST, 0, ib, b"MB"), sa       # MA waited longer: MA hosts
    assert sb[:6] == (102, L.MS_MATCHED, L.ROLE_JOIN, 0, ia, b"MA"), sb
    b.match(1, 102)                                  # asked again (a lost answer): the same match
    assert b.state()[:3] == (102, L.MS_MATCHED, L.ROLE_JOIN)
    assert none_of(old, L.MATCH_STATE, 0.3) and none_of(y, L.MATCH_STATE, 0.3), "an older game or another build heard of the match"
    # another room of the same build already open: the guest's list still has the matched host's room first
    o = Fake(lob, "MOther", X, mods())
    io = o.join()
    o.ldn(L.BROADCASTS[0], L.M_ADVERTISE, network_info(io, os.urandom(16)))
    time.sleep(0.2)
    session = os.urandom(16)
    a.ldn(L.BROADCASTS[0], L.M_ADVERTISE, network_info(ia, session))
    sb = b.until(L.MS_MATCHED)
    assert sb[3] & 1, ("the guest was not told the host's room is open", sb)
    b.tx(L.LIST, struct.pack(">QIB", b.cookie, 9, 1))
    first = b.rx(L.LISTING)
    assert struct.unpack_from(">I", first, 4)[0] == ia, "the matched host's room is not first in the guest's list"
    b.rx(L.LISTING)
    assert b.rx(L.LIST_END)[4] == 2
    # the guest joins, the host accepts: both are told they are together; asking again repeats it (no new search)
    b.ldn(ia, L.M_JOIN, session + b"MB".ljust(32, b"\0") + b"\0\0")
    a.rx(L.DATA)
    a.ldn(L.BROADCASTS[0], L.M_ADVERTISE, network_info(ia, session, ib))
    ta, tb = a.until(L.MS_TOGETHER), b.until(L.MS_TOGETHER)
    assert ta[:6] == (101, L.MS_TOGETHER, L.ROLE_HOST, 0, ib, b"MB") and tb[:6] == (102, L.MS_TOGETHER, L.ROLE_JOIN, 0, ia, b"MA"), (ta, tb)
    b.match(1, 102)
    assert b.state()[:2] == (102, L.MS_TOGETHER)
    # cancel: the other player is told
    f, g = Fake(lob, "MF", X, mods()), Fake(lob, "MG", X, mods())
    f.join(), g.join()
    f.match(1, 1)
    f.state()
    g.match(1, 2)
    f.until(L.MS_MATCHED), g.until(L.MS_MATCHED)
    g.match(0, 2)
    assert g.until(L.MS_IDLE)[0] == 2
    sf = f.until(L.MS_PEER_LEFT)
    assert sf[5] == b"MG", sf
    # leaving the lobby: the other player is told
    h, i = Fake(lob, "MH", X, mods()), Fake(lob, "MI", X, mods())
    h.join(), i.join()
    h.match(1, 5)
    h.state()
    i.match(1, 6)
    h.until(L.MS_MATCHED), i.until(L.MS_MATCHED)
    h.tx(L.LEAVE, struct.pack(">Q", h.cookie))
    assert i.until(L.MS_PEER_LEFT)[5] == b"MH"
    # the one with an open room hosts, even when it searched second
    k, m = Fake(lob, "MK", X, mods()), Fake(lob, "MM", X, mods())
    ik, im = k.join(), m.join()
    k.ldn(L.BROADCASTS[0], L.M_ADVERTISE, network_info(ik, os.urandom(16)))
    time.sleep(0.2)
    m.match(1, 7)
    m.state()
    k.match(1, 8)
    sk, sm = k.until(L.MS_MATCHED), m.until(L.MS_MATCHED)
    assert sk[2] == L.ROLE_HOST and sm[2] == L.ROLE_JOIN and sm[3] & 1 and sm[4] == ik, (sk, sm)
    # no opponent: the searches of another build / another code end with TIMEOUT; a match nobody follows EXPIRES
    assert y.until(L.MS_TIMEOUT, 6.0)[0] == 201 and e.until(L.MS_TIMEOUT, 3.0)[0] == 301
    for x in (k, m, o, y, e, old):                   # (keep them in the lobby: no PING otherwise in this test)
        x.ping()
    assert k.until(L.MS_EXPIRED, 7.0)[4] == im and m.until(L.MS_EXPIRED, 3.0)[4] == ik
    y.match(1, 201)                                  # the ended search asked again: its end again, not a new search
    assert y.state()[:2] == (201, L.MS_TIMEOUT)
    y.match(1, 202)                                  # a new search
    assert y.state()[:2] == (202, L.MS_SEARCHING)
    y.match(0, 202)
    assert y.until(L.MS_IDLE)[0] == 202
    assert none_of(old, L.MATCH_STATE, 0.2), "an older game got a MATCH_STATE"
    for x in (a, b, y, e, old, o, f, g, i, k, m):
        x.tx(L.LEAVE, struct.pack(">Q", x.cookie))
    time.sleep(0.2)


def rb(f, dst, body):
    f.tx(L.RB, struct.pack(">QI", f.cookie, dst) + body)


def rollback_matchmaking(lob):
    """1.4: MATCH op 2 (a rollback match from Smash versus, rb_net.c), on the public lobby code."""
    X, P = b"ssbu-recomp-ldn1-13.0.5", b"public"
    r1, r2, r3, s = Fake(lob, "R1", X, mods(), P), Fake(lob, "R2", X, mods(), P), Fake(lob, "R3", X, mods(), P), Fake(lob, "RS", X, mods(), P)
    i1, i2, i3, i_s = r1.join(), r2.join(), r3.join(), s.join()
    r1.match(2, 401)
    assert r1.state()[:2] == (401, L.MS_SEARCHING)
    time.sleep(0.2)
    r2.match(1, 402)                                 # a Local Wireless search: never paired with a rollback search
    assert r2.state()[:2] == (402, L.MS_SEARCHING) and none_of(r1, L.MATCH_STATE, 0.3)
    r3.match(2, 403)
    a, b = r1.until(L.MS_MATCHED), r3.until(L.MS_MATCHED)
    assert a[:6] == (401, L.MS_MATCHED, L.ROLE_HOST, 0, i3, b"R3") and b[:6] == (403, L.MS_MATCHED, L.ROLE_JOIN, 0, i1, b"R1"), (a, b)
    r3.match(2, 403)                                 # asked again: the same match
    assert r3.state()[:3] == (403, L.MS_MATCHED, L.ROLE_JOIN)
    if not os.environ.get("LOBBY_EXE"):              # (the C lobby inside a host's game has no public-code rule: its players came on purpose)
        rb(s, i1, b"Hstranger")                   # a stranger on the public code: dropped
        rb(r2, 0, b"Hall")                        # destination 0 on the public code: dropped
        assert none_of(r1, L.RB, 0.3), "a stranger's rollback message reached a matched player"
    rb(r1, i3, b"Hchoice-from-r1")
    got = r3.rx(L.RB)
    assert struct.unpack_from(">I", got)[0] == i1 and got[4:] == b"Hchoice-from-r1", got
    assert none_of(s, L.RB, 0.2) and none_of(r2, L.RB, 0.2)
    rb(r3, i1, b"Ipropose-from-r3")
    assert r1.rx(L.RB)[4:] == b"Ipropose-from-r3"
    ta, tb = r1.until(L.MS_TOGETHER), r3.until(L.MS_TOGETHER)
    assert ta[:5] == (401, L.MS_TOGETHER, L.ROLE_HOST, 0, i3) and tb[:5] == (403, L.MS_TOGETHER, L.ROLE_JOIN, 0, i1), (ta, tb)
    rb(r1, i3, b"pads")                          # after TOGETHER: still relayed (the match goes on)
    assert r3.rx(L.RB)[4:] == b"pads"
    r2.match(0, 402)
    assert r2.until(L.MS_IDLE)[0] == 402
    for x in (r1, r2, r3, s):
        x.tx(L.LEAVE, struct.pack(">Q", x.cookie))
    time.sleep(0.2)


def nat_ports(lob, nat_port):
    """1.5: PONG's 'N' 'A' (the port seen, the PROBE's, the NAT port); PEERS' (0, mapped port) and 'N' '2'."""
    a = Fake(lob, "Nia")
    try:
        b = Fake(lob, "Nib", bind="127.0.0.2")
    except OSError:
        b = None   # (no 127.0.0.2 here: the PONG part only)
    ia = a.join()
    a.tx(L.PING, struct.pack(">QQB", a.cookie, 1, 0) + b"NP" + struct.pack(">H", 4444))   # (its router maps port 4444 to it)
    pong = a.rx(L.PONG)
    assert pong[16:18] == b"NA" and struct.unpack_from(">HHH", pong, 18) == (a.s.getsockname()[1], 0, nat_port), pong.hex()
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    probe.bind(("127.0.0.1", 0))
    probe.sendto(L.hdr(L.PROBE) + struct.pack(">Q", a.cookie ^ 1), ("127.0.0.1", nat_port))   # (another cookie: nobody's)
    probe.sendto(L.hdr(L.PROBE) + struct.pack(">Q", a.cookie), ("127.0.0.1", nat_port))
    second = probe.getsockname()[1]
    deadline = time.time() + 3
    while True:
        a.tx(L.PING, struct.pack(">QQB", a.cookie, 2, 0) + b"NP" + struct.pack(">H", 4444))
        got = struct.unpack_from(">H", a.rx(L.PONG), 20)[0]
        if got == second or time.time() > deadline:
            break
        time.sleep(0.1)
    assert got == second, (got, second)
    probe.close()
    if b is not None:
        ib = b.join()
        session = os.urandom(16)
        a.ldn(L.BROADCASTS[0], L.M_ADVERTISE, network_info(ia, session))
        b.ldn(ia, L.M_JOIN, session + b"Nib".ljust(32, b"\0") + b"\0\0")
        a.rx(L.DATA)
        a.ldn(ib, L.M_ACCEPT, network_info(ia, session, ib))
        a.ldn(L.BROADCASTS[0], L.M_ADVERTISE, network_info(ia, session, ib))
        pb = b.rx(L.PEERS)
        assert pb[4] == 1, pb[:5]
        _, _, pub, pport, lan, lport, _ = struct.unpack_from(">I32sIHIHQ", pb, 5)
        assert (pub, pport, lan, lport) == (0x7F000001, a.s.getsockname()[1], 0, 4444), (pub, pport, lan, lport)   # (never its LAN address)
        assert pb[5 + 56:5 + 58] == b"N2" and struct.unpack_from(">H", pb, 5 + 58)[0] == second, pb.hex()
        pa = a.rx(L.PEERS)
        _, _, pub, pport, lan, lport, _ = struct.unpack_from(">I32sIHIHQ", pa, 5)
        assert (lan, lport) == (0, 0) and pa[5 + 56:] == b"", pa.hex()   # (b keeps its port, maps none and sent no PROBE)
        b.tx(L.LEAVE, struct.pack(">Q", b.cookie))
    a.tx(L.LEAVE, struct.pack(">Q", a.cookie))
    time.sleep(0.2)


def main():
    port, sport, nat_port = free_port(), free_tcp_port(), free_port()
    exe = os.environ.get("LOBBY_EXE")
    if exe:
        proc = subprocess.Popen([exe, str(port)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                env=dict(os.environ, SSBU_LOBBY_SEARCH_SECS="3", SSBU_LOBBY_MATCH_SECS="5"))
    else:
        proc = subprocess.Popen([sys.executable, "-I", os.path.join(HERE, "ssbu_lobby.py"), "--bind", "127.0.0.1", "--port", str(port), "--stats", "0",
                                 "--status-port", str(sport), "--search-secs", "3", "--match-secs", "5", "--nat-port", str(nat_port)],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    lines = []          # the lobby's output, read all along (a full pipe would stop the lobby on its next line)
    reader = threading.Thread(target=lambda: lines.extend(iter(proc.stdout.readline, "")), daemon=True)
    reader.start()
    try:
        deadline = time.time() + 10
        while time.time() < deadline and not any("UDP" in l for l in lines) and proc.poll() is None:
            time.sleep(0.05)
        time.sleep(0.2)
        lob = ("127.0.0.1", port)
        room_browser(lob)
        many_rooms(lob)
        matchmaking(lob)
        rollback_matchmaking(lob)
        if exe:
            print("lobby self-test (%s): ok (room browser: builds parted, LIST flag, ROOMS, older game, cross-build join dropped; "
                  "matchmaking: same build and code only, host choice, room open, list order, together, cancel, leave, timeout, expiry, older game; rollback matchmaking: op 2 only with op 2, roles, RB relay, together)" % os.path.basename(exe))
            return 0
        nat_ports(lob, nat_port)
        code, _, body = get_status(sport)
        st = json.loads(body)
        assert code == 200 and st["up"] is True and st["players"] == 0 and st["rooms"] == 0, body
        a, b = Fake(lob, "Alice"), Fake(lob, "Bob")
        ia, ib = a.join(), b.join()
        assert ia != ib and ia >> 16 == 0x0A0D, (hex(ia), hex(ib))
        session = os.urandom(16)
        a.ldn(L.BROADCASTS[0], L.M_ADVERTISE, network_info(ia, session))
        time.sleep(0.2)
        b.tx(L.LIST, struct.pack(">QI", b.cookie, 7))
        lst = b.rx(L.LISTING)
        seq, owner = struct.unpack_from(">II", lst)
        assert seq == 7 and owner == ia and lst[8 + L.NI_SESSION:8 + L.NI_SESSION + 16] == session
        assert b.rx(L.LIST_END)[4] == 1
        b.ldn(ia, L.M_JOIN, session + b"Bob".ljust(32, b"\0") + b"\0\0")
        j = a.rx(L.DATA)
        assert j[12:14] == bytes((ord("L"), L.M_JOIN)), j[:16]
        a.ldn(ib, L.M_ACCEPT, network_info(ia, session, ib))
        assert b.rx(L.DATA)[13] == L.M_ACCEPT
        a.ldn(L.BROADCASTS[0], L.M_ADVERTISE, network_info(ia, session, ib))
        assert b.rx(L.DATA)[13] == L.M_ADVERTISE
        pa, pb = a.rx(L.PEERS), b.rx(L.PEERS)
        assert pa[4] == 1 and pb[4] == 1, (pa[:5], pb[:5])
        assert struct.unpack_from(">I", pa, 5)[0] == ib and struct.unpack_from(">Q", pa, 5 + 4 + 32 + 12)[0] == struct.unpack_from(">Q", pb, 5 + 4 + 32 + 12)[0]
        a.tx(L.DATA, struct.pack(">IIHH", ia, L.BROADCASTS[0], 49152, 49152) + b"game-broadcast")
        assert b.rx(L.DATA).endswith(b"game-broadcast")
        b.tx(L.DATA, struct.pack(">IIHH", ib, ia, 49152, 49152) + b"game-unicast")
        assert a.rx(L.DATA).endswith(b"game-unicast")
        c = Fake(lob, "Carol")
        ic = c.join()
        c.tx(L.DATA, struct.pack(">IIHH", ic, ia, 49152, 49152) + b"outsider")
        try:
            a.rx(L.DATA, 0.5)
            raise AssertionError("an outsider's datagram reached the room")
        except AssertionError as e:
            if "outsider" in str(e):
                raise
        code, headers, body = get_status(sport)
        st = json.loads(body)
        assert code == 200 and headers.get("Access-Control-Allow-Origin") == "*", (code, dict(headers))
        assert headers.get("Content-Type", "").startswith("application/json")
        assert st["players"] == 3 and st["rooms"] == 1 and st["up"] is True, body
        assert isinstance(st["uptime_s"], int) and st["uptime_s"] >= 0 and st["version"], body
        assert set(st) == {"up", "players", "rooms", "searching", "version", "uptime_s"}, body
        for leak in ("127.0.0.1", "10.13.", "Alice", "Bob", "Carol", "Host", "test", "build-x"):
            assert leak not in body, ("status leaks", leak, body)
        try:
            urllib.request.urlopen("http://127.0.0.1:%d/other" % sport, timeout=3)
            raise AssertionError("an unknown path answered 200")
        except urllib.error.HTTPError as e:
            assert e.code == 404
        a.ldn(L.BROADCASTS[0], L.M_GONE, session)
        assert b.rx(L.DATA)[13] == L.M_GONE
        time.sleep(0.2)
        assert json.loads(get_status(sport)[2])["rooms"] == 0
        print("lobby self-test: ok (room browser: builds parted, LIST flag, ROOMS, older game, cross-build join dropped; 30 rooms: pages, picked first; "
              "matchmaking: same build and code only, host choice, room open, list order, together, cancel, leave, timeout, expiry, older game; "
              "rollback matchmaking: op 2 only with op 2, roles, RB relay, together, stranger dropped; join, list, room join, PEERS, "
              "broadcast + unicast relay, outsider refused, room closed, status endpoint; NAT port: PONG's ports, PEERS' mapped port and PROBE port)")
        return 0
    finally:
        proc.terminate()
        proc.wait(5)
        reader.join(2)
        if os.environ.get("LOBBY_LOG"):
            print("".join(lines))


if __name__ == "__main__":
    sys.exit(main())
