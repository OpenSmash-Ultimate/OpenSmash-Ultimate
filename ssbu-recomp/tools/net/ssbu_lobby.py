#!/usr/bin/env python3
"""Online lobby for the SSBU native port: the game's own Local Wireless mode carried over the internet.

Every player's game (SSBU_ONLINE=<this host>:<port>, see recomp-study/net_relay.c) joins this lobby at start and gets an
address of a virtual network 10.13.0.0/16. A room a player creates in Local Wireless is listed to every other player
with the same game build and lobby code, who joins it from the game's own Rooms List. The lobby relays each room's
datagrams between that room's players only, and introduces the players of a room to each other so that their games
can send to each other directly (UDP hole punching); the relay is the fallback.

Design adapted from SNO2207/WindowSmash (tools/ssbu_relay.py), used with the author's permission. Our own code and our
own wire protocol ("SU" version 1, described at the top of recomp-study/net_relay.c); not compatible with theirs.

    python ssbu_lobby.py [--bind 127.0.0.1] [--port 39500] [--verbose] [--status-port 8080 [--status-bind 127.0.0.1]]

--status-port starts a small HTTP endpoint, GET /status, that answers with counts only (players, rooms, searching,
version, uptime_s, up) and Access-Control-Allow-Origin: * so a web page (the project site) can show the lobby's state. It never
returns addresses, names or codes. It listens on 127.0.0.1 by default, to sit behind a TLS reverse proxy (Caddy, see
deploy_lobby.sh); --status-bind 0.0.0.0 serves it directly over plain HTTP.

Room browser (O1, lobby 1.2): every room stays listed to every player of the same lobby code whatever its game version
or mod set (ROOMS, and LIST with flag 1); joining stays within one build (the build id carries the game version and a
hash of the mods). An older game keeps its own partition exactly as before.

Matchmaking (O2, lobby 1.3): "Find an opponent" (F1 > Online). A game sends MATCH; the lobby pairs two searching players
of the same build and lobby code (the same partition as the rooms), picks the host (the one that already hosts an open
room, else the one that waited longer) and tells both (MATCH_STATE); the host creates the Local Wireless room, the other
finds it first in its Rooms List. A search ends after --search-secs without an opponent (TIMEOUT); a match ends when
both are in the same room (TOGETHER), when one cancels or leaves (PEER_LEFT), or after --match-secs (EXPIRED). PONG ends
with the number of players searching and the players of every build of this lobby code. An older game never sends
MATCH: it is never matched.

Rollback matchmaking (R3 net3, lobby 1.4): MATCH op 2 is a search for a ROLLBACK match (Smash versus with online
rollback, rb_net.c): such searches are paired only with each other (never with a Local Wireless search, op 1), the one
that waited longer is player 1 (ROLE_HOST); there is no room: both games are held at their match set-up and agree on the
match over RB messages (16) addressed to each other. The lobby counts the pair TOGETHER once it relayed RB both ways
between them. On the public lobby code an RB message to one player is relayed only to the player matched with the
sender (or a player of its Local Wireless room). An older lobby ignores op 2 (the game then says "older lobby").

Routers that change ports (lobby 1.5): a router that gives every destination its own port (a "symmetric" NAT: some ISP
routers, mobile networks, carrier-grade NAT) shows the lobby one port and the other player another, so hole punching to
the port the lobby saw fails and the room's traffic goes through this lobby (twice the distance). The lobby helps the games
(net_relay.c, "Routers that change ports"): a second UDP port, --nat-port (default --port + 10; open it in the firewall
too), where each game sends a PROBE (type 30, cookie u64) so the lobby sees its router's port for a second destination;
PONG ends with 'N' 'A', the port the lobby sees, the port the PROBE came from (0 none yet) and the NAT port; PEERS end
with 'N' '2' and each entry's PROBE port (two ports close together: the router hands them out in turn, and the other
game tries the next ones); a player behind another router is given a LAN address of 0 and, as port, its router's mapping
of its game's port (PING's 'N' 'P': PCP, NAT-PMP, UPnP) or else its own port - never its LAN address. An older game
ignores all of it.

Python 3.8+, standard library only. No authentication or encryption yet: run it for people you know."""
import argparse
import json
import os
import secrets
import select
import socket
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PROTO = 1
LOBBY_VERSION = "1.5"
(JOIN, WELCOME, REJECT, PING, PONG, DATA, LEAVE, LIST, LISTING, LIST_END,
 PEERS, PUNCH, DIRECT, BCAST) = range(1, 15)
PROBE = 30   # 1.5: to the NAT port (--nat-port): cookie u64
RB = 16          # online rollback messages between two games (recomp-study/net_relay.c T_RB, rb_net.c)
ROOMS, ROOM_INFO, ROOMS_END = 17, 18, 19     # O1 room browser (net_relay.c): every room of this lobby code, any build
MATCH, MATCH_STATE = 20, 21                  # O2 matchmaking (net_relay.c): find an opponent of the same build and code
(MS_IDLE, MS_SEARCHING, MS_MATCHED, MS_TIMEOUT, MS_PEER_LEFT, MS_TOGETHER, MS_EXPIRED) = range(7)
ROLE_HOST, ROLE_JOIN = 1, 2
KIND_ROOM, KIND_ROLLBACK = 1, 2              # MATCH op 1 / op 2: a Local Wireless search, a rollback-match search (1.4)
SEARCH_FOR, MATCH_FOR = 120.0, 180.0         # seconds (--search-secs, --match-secs)
MODS_MAX, MOD_NAME, ROOMS_MAX = 32, 31, 1000   # (ROOMS: the browser pages them; ROOMS_END says how many in all)
NAME_LEN, BUILD_LEN = 32, 40
JOIN_LEN = 4 + 8 + 2 * NAME_LEN + BUILD_LEN + 4 + 2 + 1
SUBNET, BROADCASTS = 0x0A0D0000, (0x0A0DFFFF, 0xFFFFFFFF)
LDN_PORT = 65511
M_ADVERTISE, M_JOIN, M_ACCEPT, M_REJECT, M_KEEP, M_LEAVE, M_GONE, M_KICK = range(1, 9)
NI_SIZE, NI_SESSION, NI_POLICY, NI_NODE_MAX, NI_NODE_COUNT, NI_NODES, NODE_LEN = 0x480, 0x10, 0x62, 0x66, 0x67, 0x68, 0x40
LIST_MAX = 24
CLIENT_TIMEOUT, ROOM_TIMEOUT, LISTED_FOR = 10.0, 10.0, 4.0
MAX_DATA = 1500


def hdr(kind):
    return b"SU" + bytes((PROTO, kind))


def text(raw):
    return raw.split(b"\0", 1)[0].decode("utf-8", "replace")


def vip_text(ip):
    return "10.13.%d.%d" % (ip >> 8 & 255, ip & 255)


def node_name(info, i):
    o = NI_NODES + NODE_LEN * i + 0xC
    return text(info[o:o + NAME_LEN])


def mods_block(raw):
    """A game's mod-set block (count u8, count x (hash u32, length u8, name)) checked and cut to its own length, or None."""
    if not raw or raw[0] > MODS_MAX:
        return None
    o = 1
    for _ in range(raw[0]):
        if o + 5 > len(raw) or raw[o + 4] > MOD_NAME or o + 5 + raw[o + 4] > len(raw):
            return None
        o += 5 + raw[o + 4]
    return bytes(raw[:o])


def room_nodes(info, owner):
    """The station addresses of a NetworkInfo whose node 0 is owner, or None if it is not one a game makes."""
    count, most = info[NI_NODE_COUNT], info[NI_NODE_MAX]
    if not 1 <= count <= most <= 8:
        return None
    ips = []
    for i in range(8):
        o = NI_NODES + NODE_LEN * i
        if not info[o + 0xB]:
            if i == 0:
                return None
            continue
        ip = struct.unpack_from("<I", info, o)[0]
        if ip & 0xFFFF0000 != SUBNET or (i == 0 and ip != owner) or ip in ips:
            return None
        ips.append(ip)
    return frozenset(ips[1:])


class Client:
    def __init__(self, at, nonce, ip, name, part, lan, direct, now):
        self.at, self.nonce, self.ip, self.name, self.part = at, nonce, ip, name, part
        self.lan, self.direct = lan, direct
        self.mapped = 0           # 1.5: the outside port its router maps to its game's (PING 'N' 'P'; 0 none)
        self.probe_seen = 0       # 1.5: the port its PROBE came from to the NAT port (0 none yet)
        self.cookie = secrets.randbits(64)
        self.seen = now
        self.room = None          # the Room this player hosts
        self.claim = None         # the session id of the room it asked to join
        self.peers_version = 0
        self.peers_sent = None
        self.mods = None          # its mod-set block (None: an older game that does not send it)
        self.search_id = None     # O2: the id of its current search (MATCH), None = never searched
        self.search_since = None  # searching since (None: not searching)
        self.match = None         # the Match it is in
        self.final = None         # (search id, state, role, peer ip, peer name): how its last search ended (sent again if asked)
        self.kind = KIND_ROOM     # 1.4: what it searches (KIND_ROOM / KIND_ROLLBACK)
        self.rb_peer = 0          # 1.4: the player of its last rollback match (RB to it is relayed on the public code)


class Match:
    def __init__(self, host, guest, now):
        self.host, self.guest, self.at = host, guest, now
        self.room_said = False    # the guest was told the host's room is open
        self.rb_seen = set()      # (rollback matches) the players whose RB to the other one the lobby relayed

    def other(self, c):
        return self.guest if c is self.host else self.host


class Room:
    def __init__(self, owner, session, now):
        self.owner, self.session, self.created = owner, session, now
        self.info, self.nodes, self.updated = b"", frozenset(), now
        self.secrets = {}         # frozenset({a, b}) -> pair secret

    def is_open(self):
        return self.info[NI_POLICY] != 1 and self.info[NI_NODE_COUNT] < self.info[NI_NODE_MAX]


class Lobby:
    def __init__(self, sock, verbose, search_for=SEARCH_FOR, match_for=MATCH_FOR, nat_sock=None):
        self.sock, self.verbose = sock, verbose
        self.nat_sock = nat_sock   # 1.5: the NAT port's socket (None: none)
        self.nat_port = nat_sock.getsockname()[1] if nat_sock is not None else 0
        self.search_for, self.match_for = search_for, match_for
        self.by_at, self.by_ip, self.rooms = {}, {}, {}
        self.next_ip = SUBNET + 2
        self.stats = {"relayed game datagrams": 0, "ldn messages": 0, "dropped": 0}

    def say(self, *what):
        print(time.strftime("%H:%M:%S"), *what, flush=True)

    def log(self, *what):
        if self.verbose:
            self.say(*what)

    def send(self, data, at):
        try:
            self.sock.sendto(data, at)
        except OSError:
            pass

    # ---- who is where
    def members(self, room):
        out = [room.owner]
        for ip in room.nodes:
            c = self.by_ip.get(ip)
            if c is not None and c.claim == room.session:
                out.append(c)
        return out

    def room_of(self, c):
        if c.room is not None:
            return c.room
        r = self.rooms.get(c.claim) if c.claim else None
        return r if r is not None and c.ip in r.nodes else None

    def new_ip(self):
        for _ in range(0x10000):
            ip = self.next_ip
            self.next_ip = SUBNET + 2 if ip >= SUBNET + 0xFFFE else ip + 1
            if ip & 0xFF not in (0, 255) and ip not in self.by_ip:
                return ip
        return None

    def close_room(self, room, why):
        gone = hdr(DATA) + struct.pack(">IIHH", room.owner.ip, BROADCASTS[0], LDN_PORT, LDN_PORT) + bytes((ord("L"), M_GONE, 0, 0)) + room.session
        for m in self.members(room):
            if m is not room.owner:
                self.send(gone, m.at)
                m.claim = None
                self.send_peers(m)
        self.rooms.pop(room.session, None)
        if room.owner.room is room:
            room.owner.room = None
            self.send_peers(room.owner)
        self.say("room of %s closed (%s)" % (room.owner.name, why))

    def drop(self, c, why):
        if c.match is not None:
            other = c.match.other(c)
            c.match = None
            self.match_end(other, MS_PEER_LEFT, "%s left the lobby" % c.name)
        c.search_since = None
        if c.room is not None:
            self.close_room(c.room, "host left")
        if c.claim:
            r = self.rooms.get(c.claim)
            if r is not None and c.ip in r.nodes:   # tell its host at once
                self.send(hdr(DATA) + struct.pack(">IIHH", c.ip, r.owner.ip, LDN_PORT, LDN_PORT) + bytes((ord("L"), M_LEAVE, 0, 0)) + r.session, r.owner.at)
        self.by_at.pop(c.at, None)
        self.by_ip.pop(c.ip, None)
        self.say("%s (%s) left: %s" % (c.name, vip_text(c.ip), why))

    # ---- direct paths: each member of a room learns the others' addresses and a pair secret
    def send_peers(self, c):
        room = self.room_of(c)
        entries, second = [], []
        if room is not None and c.direct:
            for m in self.members(room):
                if m is c or not m.direct:
                    continue
                pair = frozenset((c.ip, m.ip))
                secret = room.secrets.setdefault(pair, secrets.randbits(64) | 1)
                pub_ip = struct.unpack(">I", socket.inet_aton(m.at[0]))[0]
                lan = m.lan
                if m.at[0] != c.at[0]:   # (behind another router: its mapped or own port at its public address, never its LAN address)
                    own = m.mapped or m.lan[1]
                    lan = (0, own if own != m.at[1] else 0)
                entries.append(struct.pack(">I32sIHIHQ", m.ip, m.name.encode()[:NAME_LEN], pub_ip, m.at[1], lan[0], lan[1], secret))
                second.append(m.probe_seen)
        body = b"".join(entries)
        if any(second):   # (1.5: 'N' '2', each one's PROBE port)
            body += b"N2" + struct.pack(">%dH" % len(second), *second)
        if body == c.peers_sent:
            return
        c.peers_sent = body
        c.peers_version += 1
        self.send(hdr(PEERS) + struct.pack(">IB", c.peers_version, len(entries)) + body, c.at)
        c.peers_entries = len(entries)
        if entries:
            self.log("%s introduced to %d player(s) of its room" % (c.name, len(entries)))

    def changed(self, c):
        """c's addresses changed: the players of its room get new PEERS."""
        room = self.room_of(c)
        if room is not None:
            self.room_changed(room)

    def on_probe(self, p, at):
        """1.5: a PROBE to the NAT port: the port the game's router uses for a second destination."""
        if len(p) < 12 or p[:2] != b"SU" or p[2] != PROTO or p[3] != PROBE:
            return
        cookie = struct.unpack_from(">Q", p, 4)[0]
        for c in self.by_at.values():
            if c.at[0] == at[0] and c.cookie == cookie:
                if c.probe_seen != at[1]:
                    c.probe_seen = at[1]
                    self.changed(c)
                return

    def room_changed(self, room):
        for m in self.members(room):
            self.send_peers(m)

    # ---- packets
    def packet(self, p, at, now):
        if len(p) < 4 or p[:2] != b"SU" or p[2] != PROTO:
            return
        kind = p[3]
        c = self.by_at.get(at)
        if kind == JOIN:
            return self.on_join(p, at, c, now)
        if c is None:
            return
        c.seen = now
        if kind == PING and len(p) >= 21 and struct.unpack_from(">Q", p, 4)[0] == c.cookie:
            same = [x for x in self.by_at.values() if x.part == c.part]
            rooms = [r for r in self.rooms.values() if r.owner.part == c.part]
            searching = sum(1 for x in same if x.search_since is not None)
            code = sum(1 for x in self.by_at.values() if x.part[1] == c.part[1])
            mapped = struct.unpack_from(">H", p, 23)[0] if len(p) >= 25 and p[21:23] == b"NP" else 0   # (1.5: its router's mapping)
            if mapped != c.mapped:
                c.mapped = mapped
                self.changed(c)
            self.send(hdr(PONG) + p[12:20] + struct.pack(">HHHH", len(same), len(rooms), searching, code)   # (O2: the last two)
                      + b"NA" + struct.pack(">HHH", at[1], c.probe_seen, self.nat_port), at)   # (1.5: what its router does)
        elif kind == LEAVE and len(p) >= 12 and struct.unpack_from(">Q", p, 4)[0] == c.cookie:
            self.drop(c, "left")
        elif kind == LIST and len(p) >= 16 and struct.unpack_from(">Q", p, 4)[0] == c.cookie:
            # (flags 2: a host this game picked in its browser, first, u32 at 17; 4: a page of LIST_MAX, u16 at 21)
            flags = p[16] if len(p) >= 17 else 0
            first = struct.unpack_from(">I", p, 17)[0] if flags & 2 and len(p) >= 21 else 0
            page = struct.unpack_from(">H", p, 21)[0] if flags & 4 and len(p) >= 23 else 0
            self.on_list(c, p[12:16], now, flags & 1, first, page)
        elif kind == ROOMS and len(p) >= 16 and struct.unpack_from(">Q", p, 4)[0] == c.cookie:
            self.on_rooms(c, p[12:16], now)
        elif kind == MATCH and len(p) >= 17 and struct.unpack_from(">Q", p, 4)[0] == c.cookie:
            self.on_match(c, p[12], struct.unpack_from(">I", p, 13)[0], now)
        elif kind == RB and 16 < len(p) <= 1500 and struct.unpack_from(">Q", p, 4)[0] == c.cookie:
            # online rollback message: to one player of the same lobby code (destination 0 = all others, only with a private code)
            # (1.4) on the public code only to its matched rollback opponent or a player of its Local Wireless room
            dst = struct.unpack_from(">I", p, 12)[0]
            if dst == 0 and c.part[1] == "public":
                return
            if dst and c.part[1] == "public" and dst != c.rb_peer:
                room = self.room_of(c)
                if room is None or dst not in [m.ip for m in self.members(room)]:
                    self.stats["dropped"] += 1
                    return
            out = hdr(RB) + struct.pack(">I", c.ip) + p[16:]
            for x in list(self.by_at.values()):
                if x is not c and x.part == c.part and (dst == 0 or x.ip == dst):
                    self.send(out, x.at)
            m = c.match
            if dst and m is not None and c.kind == KIND_ROLLBACK and m.other(c).ip == dst:
                m.rb_seen.add(c.ip)
                if len(m.rb_seen) == 2:
                    h, g = m.host, m.guest
                    self.match_end(h, MS_TOGETHER, "playing a rollback match with %s" % g.name)
                    self.match_end(g, MS_TOGETHER, "playing a rollback match with %s" % h.name)
        elif kind == DATA and len(p) >= 16:
            self.on_data(c, p, 4, now, ())
        elif kind == BCAST and len(p) >= 5:
            n = p[4]
            skip = set(struct.unpack_from(">%dI" % n, p, 5)) if len(p) >= 5 + 4 * n else set()
            body = 5 + 4 * n
            if len(p) >= body + 12:
                self.on_data(c, hdr(DATA) + p[body:], 4, now, skip)

    def on_join(self, p, at, c, now):
        if len(p) < JOIN_LEN:
            return
        nonce = p[4:12]
        code, name = text(p[12:44]), text(p[44:76]) or "Player"
        build = p[76:116].rstrip(b"\0")
        lan = struct.unpack_from(">IH", p, 116)
        direct = bool(p[122] & 1)
        part = (build, code)
        if c is not None and c.nonce != nonce:
            self.drop(c, "restarted")
            c = None
        if c is None:
            for other in list(self.by_at.values()):   # the same game from a new port (its router remapped it)?
                if other.nonce == nonce and other.part == part:
                    del self.by_at[other.at]
                    other.at = at
                    self.by_at[at] = c = other
                    break
        if c is None:
            ip = self.new_ip()
            if ip is None:
                self.send(hdr(REJECT) + nonce + b"the lobby is full".ljust(64, b"\0"), at)
                return
            c = Client(at, nonce, ip, name, part, lan, direct, now)
            self.by_at[at], self.by_ip[ip] = c, c
            self.say("%s joined (code %r, build %s) as %s from %s:%d, LAN %s:%d, direct %s" % (
                name, code, build.decode("ascii", "replace"), vip_text(ip), at[0], at[1], socket.inet_ntoa(struct.pack(">I", lan[0])), lan[1],
                "yes" if direct else "no"))
        c.seen, c.lan, c.direct = now, lan, direct
        if len(p) > JOIN_LEN + 2 and p[JOIN_LEN:JOIN_LEN + 2] == b"MS":      # O1: the game's mods (an older game sends none)
            c.mods = mods_block(p[JOIN_LEN + 2:])
        self.send(hdr(WELCOME) + nonce + struct.pack(">IQ", c.ip, c.cookie), at)

    def on_list(self, c, seq, now, other=False, first=0, page=0):
        """The game's Rooms List. other (flag 1, a newer game): the rooms of every build of this code, each LISTING ending
        with its host's build[40] (that game refuses to join a room of another build and says why). The game shows
        LIST_MAX rooms: first (flag 2) is the room of the host this player picked in its browser, put first; page (flag 4)
        the LIST_MAX rooms after page x LIST_MAX of the others, so that every room can be reached however many there are."""
        def mine(r):
            return r.owner.part == c.part if not other else r.owner.part[1] == c.part[1]
        mhost = c.match.host if c.match is not None and c.match.guest is c else None     # O2: the matched host's room first
        rooms = sorted((r for r in self.rooms.values() if mine(r) and r.owner is not c and now - r.updated <= LISTED_FOR),
                       key=lambda r: (r.owner is not mhost, r.owner.part != c.part, not r.is_open(), -r.info[NI_NODE_COUNT], r.created))
        picked = [r for r in rooms if r.owner is mhost or (first and r.owner.ip == first)]   # (the matched host, then the pick)
        rest = [r for r in rooms if r not in picked]
        start = page * LIST_MAX if page * LIST_MAX < len(rest) else 0
        rooms = (picked + rest[start:])[:LIST_MAX]
        for r in rooms:
            self.send(hdr(LISTING) + seq + struct.pack(">I", r.owner.ip) + r.info + (r.owner.part[0][:BUILD_LEN].ljust(BUILD_LEN, b"\0") if other else b""), c.at)
        self.send(hdr(LIST_END) + seq + bytes((len(rooms),)), c.at)
        if rooms:
            self.log("%s scanned: %d room(s)" % (c.name, len(rooms)))

    def on_rooms(self, c, seq, now):
        """The room browser (F1 > Online rooms): every room of this lobby code, any build, joinable ones first."""
        rooms = sorted((r for r in self.rooms.values() if r.owner.part[1] == c.part[1] and now - r.updated <= LISTED_FOR),
                       key=lambda r: (r.owner.part != c.part, not r.is_open(), -r.info[NI_NODE_COUNT], r.created))[:ROOMS_MAX]
        for r in rooms:
            o = r.owner
            flags = (1 if r.is_open() else 0) | (2 if o is c else 0) | (4 if o.mods is not None else 0)
            self.send(hdr(ROOM_INFO) + seq + struct.pack(">IBBB", o.ip, r.info[NI_NODE_COUNT], r.info[NI_NODE_MAX], flags)
                      + o.name.encode("utf-8", "replace")[:NAME_LEN].ljust(NAME_LEN, b"\0") + o.part[0][:BUILD_LEN].ljust(BUILD_LEN, b"\0")
                      + (o.mods or b"\0"), c.at)
        self.send(hdr(ROOMS_END) + seq + bytes((min(len(rooms), 255),)) + struct.pack(">H", len(rooms)), c.at)

    def on_data(self, c, p, off, now, skip):
        if len(p) - off - 12 > MAX_DATA + NI_SIZE:
            self.stats["dropped"] += 1
            return
        src, dst, sport, dport = struct.unpack_from(">IIHH", p, off)
        if src != c.ip:
            self.stats["dropped"] += 1
            return
        if sport == LDN_PORT or dport == LDN_PORT:
            return self.on_ldn(c, p, dst, sport, dport, now)
        room = self.room_of(c)
        if room is None:
            self.stats["dropped"] += 1
            return
        members = self.members(room)
        sent = 0
        for m in members:
            if m is c or m.ip in skip:
                continue
            if dst in BROADCASTS or m.ip == dst:
                self.send(p, m.at)
                sent += 1
        self.stats["relayed game datagrams"] += sent
        if not sent and dst not in BROADCASTS:
            self.stats["dropped"] += 1

    def on_ldn(self, c, p, dst, sport, dport, now):
        if sport != LDN_PORT or dport != LDN_PORT or len(p) < 20 or p[16] != ord("L"):
            self.stats["dropped"] += 1
            return
        self.stats["ldn messages"] += 1
        kind, body = p[17], p[20:]
        if dst in BROADCASTS:
            if kind == M_ADVERTISE and len(body) >= NI_SIZE:
                self.on_advertise(c, p, bytes(body[:NI_SIZE]), now)
            elif kind == M_GONE and c.room is not None and body[:16] == c.room.session:
                self.close_room(c.room, "closed by its host")
            return
        b = self.by_ip.get(dst)
        if b is None or b is c or b.part != c.part:
            self.stats["dropped"] += 1
            return
        if kind == M_JOIN:
            if b.room is None or body[:16] != b.room.session:
                self.stats["dropped"] += 1
                return
            if c.room is not None:
                self.close_room(c.room, "its host joins another room")
            c.claim = b.room.session
            self.say("%s asks to join %s's room" % (c.name, b.name))
        elif kind == M_LEAVE:
            if c.claim and b.room is not None and b.room.session == c.claim:
                c.claim = None
                self.send(p, b.at)
                self.send_peers(c)
                return
        self.send(p, b.at)

    def on_advertise(self, c, p, info, now):
        nodes = room_nodes(info, c.ip)
        if nodes is None:
            self.stats["dropped"] += 1
            return
        session = info[NI_SESSION:NI_SESSION + 16]
        room = self.rooms.get(session)
        if room is not None and room.owner is not c:
            self.stats["dropped"] += 1
            return
        if room is None:
            if c.room is not None:
                self.close_room(c.room, "replaced by a new room")
            c.claim = None
            room = c.room = self.rooms[session] = Room(c, session, now)
            self.say("%s opened a room (%d players at most)" % (c.name, info[NI_NODE_MAX]))
        before = self.members(room)
        changed = nodes != room.nodes
        room.info, room.nodes, room.updated = info, nodes, now
        after = self.members(room)
        if changed:
            names = ", ".join(node_name(info, i) or "?" for i in range(8) if info[NI_NODES + NODE_LEN * i + 0xB])
            self.say("room of %s: %d player(s): %s" % (c.name, info[NI_NODE_COUNT], names))
            self.room_changed(room)
            for m in before:
                if m not in after:
                    self.send_peers(m)
        for m in set(before) | set(after):
            if m is not c:
                self.send(p, m.at)
        if c.match is not None and c.match.host is c and c.kind == KIND_ROOM:
            self.match_room(c.match, room, now)

    # ---- O2 matchmaking
    def match_send(self, c, now, state=None):
        """MATCH_STATE to c: search id u32, state u8, role u8, flags u8 (1: the host's room is open), peer address u32,
        peer name[32], players searching u16, players u16 (both of c's build and code), seconds waited u16."""
        m, role, flags, peer, name, sid = c.match, 0, 0, 0, b"", c.search_id or 0
        if state is None and m is None and c.search_since is None and c.final is not None:
            sid, state, role, peer, name = c.final
        elif state is None:
            state = MS_MATCHED if m is not None else MS_SEARCHING if c.search_since is not None else MS_IDLE
        if m is not None:
            o = m.other(c)
            role, peer, name = (ROLE_HOST if m.host is c else ROLE_JOIN), o.ip, o.name.encode("utf-8", "replace")
            r = m.host.room
            if r is not None and r.is_open() and now - r.updated <= LISTED_FOR:
                flags |= 1
        since = m.at if m is not None else c.search_since
        same = [x for x in self.by_at.values() if x.part == c.part]
        self.send(hdr(MATCH_STATE) + struct.pack(">IBBBI", sid & 0xFFFFFFFF, state, role, flags, peer) + name[:NAME_LEN].ljust(NAME_LEN, b"\0")
                  + struct.pack(">HHH", sum(1 for x in same if x.search_since is not None), len(same),
                                min(int(now - since), 65535) if since is not None else 0), c.at)

    def match_end(self, c, state, why):
        """c's search or match is over (state: MS_TIMEOUT, MS_PEER_LEFT, MS_TOGETHER, MS_EXPIRED)."""
        m = c.match
        role, peer, name = 0, 0, b""
        if m is not None:
            o = m.other(c)
            role, peer, name = (ROLE_HOST if m.host is c else ROLE_JOIN), o.ip, o.name.encode("utf-8", "replace")
        c.match, c.search_since = None, None
        c.final = (c.search_id or 0, state, role, peer, name)
        self.match_send(c, time.monotonic())
        self.say("match: %s: %s" % (c.name, why))

    def on_match(self, c, op, sid, now):
        """MATCH from a game: op 1 = search (sent again every 2 s while it waits: the answer says how it stands), 0 = cancel,
        2 = (1.4) search for a rollback match (paired only with another op 2 search; no room: the one that waited longer is
        player 1, ROLE_HOST)."""
        if op == 0:
            if c.match is not None:
                other = c.match.other(c)
                c.match = None
                self.match_end(other, MS_PEER_LEFT, "%s cancelled the match" % c.name)
            if c.search_since is not None:
                self.say("match: %s stopped searching" % c.name)
            c.search_since, c.search_id, c.final = None, sid, (sid, MS_IDLE, 0, 0, b"")
            return self.match_send(c, now)
        if op not in (KIND_ROOM, KIND_ROLLBACK):
            return
        if c.search_id == sid and (c.match is not None or c.search_since is not None or c.final is not None):
            return self.match_send(c, now)          # the same search: how it stands (or how it ended)
        if c.match is not None:                     # a new search while matched: the old match is over
            other = c.match.other(c)
            c.match = None
            self.match_end(other, MS_PEER_LEFT, "%s searches again" % c.name)
        c.search_id, c.search_since, c.final, c.kind = sid, now, None, op
        self.say("match: %s searches an opponent%s (build %s)" % (c.name, " for a rollback match" if op == KIND_ROLLBACK else "", c.part[0].decode("ascii", "replace")))
        waiting = [x for x in self.by_at.values() if x is not c and x.part == c.part and x.search_since is not None and x.match is None and x.kind == op]
        if not waiting:
            return self.match_send(c, now)
        o = min(waiting, key=lambda x: x.search_since)

        def hosts_open(x):
            return op == KIND_ROOM and x.room is not None and x.room.is_open()
        host = c if hosts_open(c) and not hosts_open(o) else o   # the one with an open room hosts, else the one that waited longer
        m = Match(host, o if host is c else c, now)
        for x in (c, o):
            x.match, x.search_since = m, None
        if op == KIND_ROLLBACK:
            c.rb_peer, o.rb_peer = o.ip, c.ip
        self.say("match: %s hosts, %s joins%s" % (m.host.name, m.guest.name, " (rollback match)" if op == KIND_ROLLBACK else ""))
        self.match_send(m.host, now)
        self.match_send(m.guest, now)
        if m.host.room is not None and op == KIND_ROOM:
            self.match_room(m, m.host.room, now)

    def match_room(self, m, room, now):
        """(the host advertised its room) both in it: done; the room open: the guest is told once (it polls as well)."""
        if m.guest.ip in room.nodes and m.guest.claim == room.session:
            h, g = m.host, m.guest
            self.match_end(h, MS_TOGETHER, "in a room with %s" % g.name)
            self.match_end(g, MS_TOGETHER, "in %s's room" % h.name)
            return
        if room.is_open() and not m.room_said:
            m.room_said = True
            self.match_send(m.guest, now)
            self.match_send(m.host, now)

    def sweep(self, now):
        for c in list(self.by_at.values()):
            if now - c.seen > CLIENT_TIMEOUT:
                self.drop(c, "silent for %d s" % CLIENT_TIMEOUT)
        for c in list(self.by_at.values()):
            if c.search_since is not None and c.match is None and now - c.search_since > self.search_for:
                self.match_end(c, MS_TIMEOUT, "no opponent in %d s" % self.search_for)
            m = c.match
            if m is not None and m.host is c and now - m.at > self.match_for:
                g = m.guest
                self.match_end(c, MS_EXPIRED, "the match with %s expired" % g.name)
                self.match_end(g, MS_EXPIRED, "the match with %s expired" % c.name)
        for r in list(self.rooms.values()):
            if now - r.updated > ROOM_TIMEOUT:
                self.close_room(r, "no advertisement for %d s" % ROOM_TIMEOUT)


def status_json(lobby, started, now):
    """The /status document: counts only. Never addresses, names, lobby codes or builds."""
    return json.dumps({"up": True, "players": len(lobby.by_at), "rooms": len(lobby.rooms),
                       "searching": sum(1 for c in lobby.by_at.values() if c.search_since is not None),
                       "version": "%s (protocol SU %d)" % (LOBBY_VERSION, PROTO), "uptime_s": int(now - started)})


def start_status_server(lobby, bind, port, started):
    """Serve GET /status on a daemon thread. Returns the server (its .server_address has the real port)."""
    class Handler(BaseHTTPRequestHandler):
        server_version = "ssbu-lobby-status"
        sys_version = ""

        def head(self, code, body, ctype):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Access-Control-Allow-Methods", "GET, OPTIONS")
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.end_headers()

        def do_GET(self):
            if self.path.split("?", 1)[0].rstrip("/") == "/status":
                body = status_json(lobby, started, time.monotonic()).encode()
                self.head(200, body, "application/json")
            else:
                body = b'{"error":"not found"}'
                self.head(404, body, "application/json")
            self.wfile.write(body)

        def do_HEAD(self):
            ok = self.path.split("?", 1)[0].rstrip("/") == "/status"
            self.head(200 if ok else 404, b"", "application/json")

        def do_OPTIONS(self):
            self.head(204, b"", "text/plain")

        def log_message(self, *args):   # no request log: it would hold the callers' addresses
            pass

    srv = ThreadingHTTPServer((bind, port), Handler)
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, name="status-http", daemon=True).start()
    return srv


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=39500)
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--stats", type=float, default=10.0, help="seconds between statistics lines (0: none)")
    ap.add_argument("--status-port", type=int, default=0, help="TCP port of the HTTP status endpoint GET /status (0: off)")
    ap.add_argument("--status-bind", default="127.0.0.1", help="address of the status endpoint (default 127.0.0.1, behind a proxy)")
    ap.add_argument("--search-secs", type=float, default=SEARCH_FOR, help="matchmaking: a search without an opponent ends after this (default 120)")
    ap.add_argument("--nat-port", type=int, default=-1, help="UDP port where games show their routers' second port, for direct paths "
                    "through routers that change ports; open it in the firewall too (0: none; default --port + 10)")
    ap.add_argument("--match-secs", type=float, default=MATCH_FOR, help="matchmaking: matched players not in one room by then are told it expired (default 180)")
    o = ap.parse_args()
    started = time.monotonic()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    if os.name == "nt":   # (no ICMP "port unreachable" errors on receive)
        try:
            sock.ioctl(socket.SIO_UDP_CONNRESET, False)
        except (AttributeError, OSError):
            pass
    sock.bind((o.bind, o.port))
    nat = None
    nat_port = o.port + 10 if o.nat_port < 0 else o.nat_port
    if nat_port:
        nat = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        if os.name == "nt":
            try:
                nat.ioctl(socket.SIO_UDP_CONNRESET, False)
            except (AttributeError, OSError):
                pass
        nat.bind((o.bind, nat_port))
    lobby = Lobby(sock, o.verbose, o.search_secs, o.match_secs, nat)
    lobby.say("lobby on UDP %s:%d (protocol SU %d)" % (o.bind, o.port, PROTO))
    if nat is not None:
        lobby.say("NAT port on UDP %s:%d (open it in the firewall too)" % (o.bind, lobby.nat_port))
    if o.status_port:
        srv = start_status_server(lobby, o.status_bind, o.status_port, started)
        lobby.say("status endpoint on http://%s:%d/status" % srv.server_address[:2])
    last_sweep = last_stats = time.monotonic()
    while True:
        r, _, _ = select.select([sock] + ([nat] if nat is not None else []), [], [], 0.5)
        now = time.monotonic()
        if nat is not None and nat in r:
            try:
                p, at = nat.recvfrom(2048)
                lobby.on_probe(p, at)
            except OSError:
                pass
            except Exception as e:   # (as below)
                lobby.say("error on a NAT probe from %s: %r" % (at[0], e))
        if sock in r:
            try:
                p, at = sock.recvfrom(65536)
            except OSError:
                continue
            try:
                lobby.packet(p, at, now)
            except Exception as e:   # one bad packet never stops the lobby
                lobby.say("error on a packet from %s: %r" % (at[0], e))
        if now - last_sweep >= 1.0:
            last_sweep = now
            lobby.sweep(now)
        if o.stats and now - last_stats >= o.stats:
            last_stats = now
            if lobby.by_at:
                lobby.say("stats: %d player(s), %d room(s), %s" % (len(lobby.by_at), len(lobby.rooms),
                                                                   ", ".join("%s %d" % kv for kv in lobby.stats.items())))


if __name__ == "__main__":
    sys.exit(main())
