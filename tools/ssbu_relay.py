#!/usr/bin/env python3
"""Netplay lobby for the SSBU native port: the server every player's game joins to play online.

Players' games join it at start (Play SSBU.bat). A game that isn't version 13.0.5 is refused, with a
message the game shows (WindowSmash plays 13.0.5 only: REQUIRED_BUILD). A room anyone creates in
Local Wireless is listed to every player of the same lobby code, who join it from the game's Rooms List;
the lobby then carries the room's traffic between its players only. Players only send to the
lobby, so nobody needs port forwarding; the lobby's machine needs its UDP port open. Players in
the same room whose games can do it are also introduced to each other, so their games can send the
room's traffic straight to each other (UDP hole punching) and use the lobby only when that fails.
The protocol is runtime/net_relay.c's (version 2); the host player's game can run a smaller relay
of its own for a private room (launchers\\Host SSBU Netplay.bat). Version 3, on the same port, is
version 2 with a room browser, mod sets, matchmaking and a rollback relay, for games that say
their simulation version and mods: "Protocol SN" below has both, message by message.

ssbu-recomp's games join the same lobby on a port of their own (--su-port, 39500): their protocol
is "SU" version 1 (ssbu-recomp/runtime/net_relay.c), with its own numbers and layouts, and the
same rooms, limits, rates and timeouts (but datagrams as large as ssbu-recomp's lobby took:
--su-max-data). Players of the two protocols never share a room. ssbu-recomp's games also have a
room browser, matchmaking and a rollback relay here. --status-port serves GET /status
(counts only: players, rooms, players searching) for the project's site.

    python3 ssbu_relay.py [--port 39420] [--su-port 39500] [--bind 0.0.0.0] [--verbose] [--banlist FILE]
                          [--status-port 8080 [--status-bind 127.0.0.1]]
    python3 ssbu_relay.py --probe HOST[:PORT] [--room CODE] [--build ID]   (is a lobby there?)

Python 3.8+, standard library only."""

# ---- Protocol SN, versions 2 and 3 ---------------------------------------------------------------
# UDP, every field big-endian; each packet starts 'S' 'N' version type. Version 2 is runtime/net_relay.c's
# (its comment says it too). Version 3 is version 2 and more, on the same port: the lobby answers a
# game in the version of its JOIN, and two games of version 3 talk to each other in version 3 (a game
# of version 2 drops packets of another version, but a REJECT). "3:" marks what version 3 adds.
#
#  1 JOIN        game:  nonce u64, lobby code[32], name[32], build[40]         (repeated until answered)
#                       3: then sim u32, mod set: count u8 (32 at most), count x (hash u32, length u8 (31 at
#                       most), name[length]): 121 bytes at least; bytes after the mod set are ignored (room
#                       for later additions)
#  2 WELCOME     lobby: nonce u64, address u32, cookie u64, flags u8, members u8 (0: a lobby lists none)
#                       flags 1 a lobby, 2 it introduces players; 3: 4 ROOMS, 8 MATCH, 16 RB (it has them)
#  3 REJECT      lobby: nonce u64, reason u8, text[64]                         (in the JOIN's version)
#                       reasons 1 full or too many from that address, 2 another netplay version, 4 not
#                       13.0.5 (version 2 only); 3: 5 the mod set is not one (too many, a name too long, cut short)
#  4 PING        game:  nonce u64, time u64, cookie u64, ping u16              (every second)
#                       then, from a game that can connect directly: peers version u32, LAN address u32,
#                       port u16, flags u8 (1 direct connections allowed), direct peers u8
#  5 PONG        lobby: time u64, players u16, rooms u16 (of its key), flags u8 (1)
#                       3: then players searching u16 (of its key), players u16 (of its lobby code, every key)
#  6 DATA        both:  source u32, destination u32, source port u16, destination port u16, payload
#                       (payload 1500 bytes at most: --max-data)
#  7 MEMBERS     a player's relay only: count u8, count x (address u32, name[32])
#  8 LEAVE       game:  nonce u64
#  9 LIST        game:  cookie u64, seq u32                                    (nn::ldn's Scan)
#                       3: then flags u8 (1: every key's rooms of the lobby code, its own key's first)
# 10 LISTING     lobby: seq u32, host address u32, NetworkInfo[0x480]          (24 at most, best first)
#                       3, LIST flag 1: then the host's build[40], sim u32, mod set
# 11 LIST_END    lobby: seq u32, count u8
# 12 PEERS       lobby: cookie u64, version u32, count u8, count x (address u32, name[32], flags u8, public
#                       address u32, port u16, LAN address u32, port u16, secret u64)   (its room's players;
#                       flags 1 introduced, 2 that player turned direct connections off, 4 its game can't)
# 13 BCAST       game:  count u8, count x address u32 (players it reaches directly), then as DATA
# 14 PUNCH       game to game: secret u64, from u32, to u32, ack u8, nonce u64
# 15 DIRECT      game to game: secret u64, then as DATA
# Version 3 only, in SU's numbers and layouts (ssbu-recomp/runtime/net_relay.c); a lobby or game of
# version 2 never sends them, and the lobby ignores them from one:
# 16 RB          game:  cookie u64, destination u32 (0: every player of its key, not on the public code),
#                       message                                                (16 < packet length <= 1500)
#                lobby: source u32, message
#                       (on the public code "smash", only to its rollback opponent or a player of its room)
# 17 ROOMS       game:  cookie u64, seq u32                                    (the room browser)
# 18 ROOM_INFO   lobby: seq u32, host address u32, players u8, most u8, flags u8 (1 open, 2 yours, 4 mods
#                       known, 8 of your key: you can join it), host name[32], build[40], sim u32, mod set
#                       (every key's rooms of the lobby code, yours first; 32 at most)
# 19 ROOMS_END   lobby: seq u32, count u8
# 20 MATCH       game:  cookie u64, op u8 (1 a search for a room, 2 for a rollback match, 0 cancel), search
#                       id u32                                (again every 2 s while it searches or is matched)
# 21 MATCH_STATE lobby: search id u32, state u8 (0 idle, 1 searching, 2 matched, 3 none found in time, 4 the
#                       other cancelled or left, 5 together, 6 expired), role u8 (1 you host or are player 1, 2 you join),
#                       flags u8 (1 the host's room is open), the other's address u32, its name[32],
#                       players searching u16, players u16 (both of your key), seconds waited u16
#
# Version 3's key: two games meet (rooms, matchmaking, rollback messages) when all four are the same.
#   build  the game's main module build id, as version 2's (13.0.5's for WindowSmash; the lobby lets any in)
#   sim    the simulation version: a number in the game's source, bumped whenever the lifter or the runtime
#          changes what the game computes (anything that can make two games' matches differ); never a
#          per-OS or per-CPU number, so Windows, Linux and macOS, avx2 and x86-64-v2 builds meet
#   mods   the mods' hashes, in any order (names are shown, not compared); each the same on every OS
#   code   the lobby code ("smash", the default, is the public lobby)
# Players of version 2 are a pool of their own (their games say no sim or mods), SU's another (their port).
#
# For the C client (runtime/net_relay.c), from ssbu-recomp/runtime/net_relay.c (SU), message by message:
#   JOIN         send_join's tail: the mod set is mods_scan's g_mods_wire, the hashes mod_add's; after
#                build put sim. tree_hash is Win32 (FindFirstFile, '\\'): its rule (lower-case path with '/',
#                FNV-1a 64 of path then size) ports, but a mod's native code (a .dll, .so, .dylib) differs
#                per OS: hash what is the same on every OS, or two OSes never meet
#   WELCOME      its flags, in place of SU's guesses (mm_tick's 6.5 s "older lobby")
#   REJECT 2     from a lobby or a player's relay of version 2: JOIN again in version 2 (no ROOMS, MATCH, RB)
#   PONG         SU's T_PONG tail (n >= 20), 1 byte later (after flags)
#   LIST         net_list_rooms with flags 1; LISTING's tail: listing_build, then sim and the mod set
#   ROOMS..      rooms_ask, rooms_info (sim at body offset 83, the mod set at 87; flag 8), rooms_end,
#                net_rooms_get, room_why (build, then sim, then mods), net_room_refusal; mods_read as is
#   MATCH..      mm_find, mm_send, mm_tick, mm_state, net_match_*: as they are, in this header
#   RB           net_rb_send and its T_RB case: as they are; the public code is "smash", not "public"
#   the rest     WindowSmash's own, as now; PUNCH and DIRECT between two games of version 3 in version 3
import argparse
import asyncio
import collections
import email.utils
import ipaddress
import json
import os
import secrets
import signal
import socket
import struct
import sys
import time

PROTOCOL = 2
JOIN, WELCOME, REJECT, PING, PONG, DATA, MEMBERS, LEAVE, LIST, LISTING, LIST_END = range(1, 12)
PEERS, BCAST, PUNCH, DIRECT = range(12, 16)   # (direct connections; PUNCH and DIRECT go between players)
NAME_LEN = 32
BUILD_LEN = 40
# 13.0.5's main module build id (tools/game_version.py MAIN_BUILD_ID; this file is deployed on its own)
REQUIRED_BUILD = b"21450C647B8C5940ED5B68218EA128FD3C5D597D"
NOT_13 = "WindowSmash needs game 13.0.5: run the installer again"   # (a REJECT's text: 63 bytes at most)
JOIN_SIZE = 4 + 8 + 2 * NAME_LEN + BUILD_LEN
V1_JOIN_SIZE = 4 + 8 + 2 * NAME_LEN
PING_SIZE = 4 + 8 + 8 + 8 + 2
PING_EXT_SIZE = PING_SIZE + 4 + 4 + 2 + 1 + 1   # (a game that can connect directly says so in its PINGs)
PEER = struct.Struct(">I32sBIHIHQ")   # a PEERS entry
ROOM_MAX = 8
INTRO_ROOMS = 20      # rooms a player is introduced in an hour, at most
PEERS_GAP = 0.25      # seconds between two PEERS to a player, at least
SUBNET = 0x0A0D0000
BROADCASTS = (0x0A0DFFFF, 0xFFFFFFFF)
DEFAULT_PORT = 39420
LIST_MAX = 24
# Version 3, on the same port: version 2's messages, and ssbu-recomp's room browser, mod sets,
# matchmaking and rollback relay in SU's numbers and layouts. Its players are parted by a key of
# their own: build, simulation version, mod set and lobby code (see "Protocol SN" above).
V3 = 3
RB, ROOMS, ROOM_INFO, ROOMS_END, MATCH, MATCH_STATE = range(16, 22)
V3_JOIN_SIZE = JOIN_SIZE + 4 + 1   # (a JOIN of version 2's, the simulation version, a mod set of none)
V3_PUBLIC = "smash"                # (the public lobby's code, where rollback messages go to opponents only)
LOBBY, INTRODUCES, HAS_ROOMS, HAS_MATCH, HAS_RB = 1, 2, 4, 8, 16   # WELCOME's flags

# Local wireless's own messages (runtime/hle_net.c), carried in DATA between port 65511s:
# 'L', type, 0, 0, payload.
LDN_PORT = 65511
LM_ADVERTISE, LM_JOIN, LM_ACCEPT, LM_REJECT, LM_KEEP, LM_LEAVE, LM_GONE, LM_KICK = range(1, 9)
# A network's NetworkInfo, as the console lays it out (little-endian).
NI_SIZE = 0x480
NI_SESSION = 0x10
NI_POLICY = 0x62
NI_NODE_MAX = 0x66
NI_NODE_COUNT = 0x67
NI_NODES = 0x68
NI_ADV_SIZE = 0x26A
NODE_SIZE = 0x40

# ssbu-recomp's games speak a protocol of their own, "SU" version 1 (ssbu-recomp/runtime/net_relay.c
# describes it), on a port of its own: the same lobby, other numbers and layouts.
SU_PROTOCOL = 1
SU_PORT = 39500
(SU_JOIN, SU_WELCOME, SU_REJECT, SU_PING, SU_PONG, SU_DATA, SU_LEAVE, SU_LIST, SU_LISTING, SU_LIST_END,
 SU_PEERS, SU_PUNCH, SU_DIRECT, SU_BCAST) = range(1, 15)
SU_JOIN_SIZE = 4 + 8 + 2 * NAME_LEN + BUILD_LEN + 4 + 2 + 1
SU_PING_SIZE = 4 + 8 + 8 + 1
SU_PEER = struct.Struct(">I32sIHIHQ")   # a PEERS entry (only the players introduced)
SU_PEERS_AGAIN = 5.0   # seconds: its PINGs don't say which PEERS came last, so the last is sent again this often
# The room browser: every room of the lobby code, whatever its game's version or mods. A game's build
# id has its version and a hash of its mods, so players of another version or mod set never share a
# room; their rooms are listed all the same, with the host's build and mods, and the game says why
# it can't join one (ssbu-recomp/runtime/net_relay.c room_why).
SU_ROOMS, SU_ROOM_INFO, SU_ROOMS_END = ROOMS, ROOM_INFO, ROOMS_END
ROOMS_MAX = 32        # rooms in a ROOMS answer, at most
MODS_MAX = 32         # a game's mod set (its JOIN ends with "MS" and it): mods at most...
MOD_NAME = 31         # ...and a mod's name, bytes at most
# Matchmaking ("Find an opponent"): a game searching sends MATCH (again every 2 s) and hears how it
# stands in MATCH_STATE. Two players of one build and lobby code are paired: the host (the one
# with an open room, else the one that waited longer) makes the Local Wireless room, the other
# finds it first in its Rooms List; or, searching for a rollback match, the two games agree on the
# match in rollback messages (SU_RB) and the one that waited longer is player 1.
SU_MATCH, SU_MATCH_STATE = MATCH, MATCH_STATE
MS_IDLE, MS_SEARCHING, MS_MATCHED, MS_TIMEOUT, MS_PEER_LEFT, MS_TOGETHER, MS_EXPIRED = range(7)
ROLE_HOST, ROLE_JOIN = 1, 2
KIND_ROOM, KIND_ROLLBACK = 1, 2   # MATCH's op: a search for a Local Wireless room, for a rollback match (0: cancel)
# Online rollback (Smash versus with rollback, ssbu-recomp's rb_net.c): its games' messages to each
# other through the lobby. From a game: cookie u64, destination u32 (0: every other player of its
# build and lobby code), message; to a game: source u32, message.
SU_RB = RB
SU_PUBLIC = "public"   # ssbu-recomp's public lobby code: rollback messages go only to an opponent or a player of one's room
RB_MAX = 1500          # largest rollback packet
# The status page (--status-port): GET /status, counts only, for ssbu-recomp's site.
LOBBY_VERSION = "1.4"   # ssbu-recomp's lobby's version, which /status reports
STATUS_REQUEST = 8192   # bytes of a request (its line and headers), at most
STATUS_TIME = 5.0       # seconds a request has to come, and its answer to go
STATUS_OPEN = 32        # requests at once, at most


def header(kind, version=PROTOCOL):
    return b"SN" + bytes((version, kind))


def su_header(kind):
    return b"SU" + bytes((SU_PROTOCOL, kind))


def mods_block(raw):
    """A game's mod set (count u8, count x (hash u32, name length u8, name)) cut to its own length,
    or None when it is not one."""
    if not raw or raw[0] > MODS_MAX:
        return None
    o = 1
    for _ in range(raw[0]):
        if o + 5 > len(raw) or raw[o + 4] > MOD_NAME or o + 5 + raw[o + 4] > len(raw):
            return None
        o += 5 + raw[o + 4]
    return bytes(raw[:o])


def mods_key(block):
    """What of a mod set parts players (SN 3): its mods' hashes, in order (not their names, nor the
    order the game lists them in)."""
    out, o = [], 1
    for _ in range(block[0]):
        out.append(struct.unpack_from(">I", block, o)[0])
        o += 5 + block[o + 4]
    return tuple(sorted(out))


class Wire:
    """A protocol the lobby speaks (a version of one), on a UDP port of its own or of another's (port:
    the wire that has the port)."""
    def __init__(self, name, version, data, bcast, magic=None, port=None):
        self.name, self.magic, self.version = name, magic or name.encode(), version
        self.data, self.bcast = data, bcast   # (its DATA and BCAST types)
        self.port = port or self
        self.transport = None

    def header(self, kind, version=None):
        return self.magic + bytes((self.version if version is None else version, kind))


class Endpoint(asyncio.DatagramProtocol):
    """A wire's UDP port: what comes in goes to the lobby."""
    def __init__(self, lobby, wire):
        self.lobby, self.wire = lobby, wire

    def connection_made(self, transport):
        self.wire.transport = transport

    def error_received(self, exc):
        self.lobby.drops["socket error"] += 1

    def datagram_received(self, p, at):
        self.lobby.received(self.wire, p, at)


def text(raw):
    return raw.split(b"\0", 1)[0].decode("utf-8", "replace")


def printable(s):
    return "".join(ch if ch.isprintable() else "?" for ch in s)


def ip_text(ip):
    return f"10.13.{ip >> 8 & 255}.{ip & 255}"


def address_number(host):
    return struct.unpack(">I", socket.inet_aton(host))[0]


def ldn_packet(src, dst, kind, payload, head=None):
    """A local wireless message as a player's game would send it (head: the DATA header of its wire)."""
    return ((head or header(DATA)) + struct.pack(">IIHH", src, dst, LDN_PORT, LDN_PORT) +
            bytes((ord("L"), kind, 0, 0)) + payload)


def node_name(info, i=0):
    o = NI_NODES + NODE_SIZE * i + 0xC
    return printable(text(info[o:o + NAME_LEN]))


def network_nodes(info, owner_ip):
    """The addresses of a network's stations (node 0, the host, left out), or None when the
    NetworkInfo is not one a game makes or its host is not owner_ip."""
    count, most = info[NI_NODE_COUNT], info[NI_NODE_MAX]
    if not 1 <= count <= most <= 8 or struct.unpack_from("<H", info, NI_ADV_SIZE)[0] > 0x180:
        return None
    nodes = []
    for i in range(8):
        o = NI_NODES + NODE_SIZE * i
        if not info[o + 0xB]:
            if i == 0:
                return None
            continue
        ip = struct.unpack_from("<I", info, o)[0]
        if ip & 0xFFFF0000 != SUBNET or (i == 0 and ip != owner_ip) or ip in nodes:
            return None
        nodes.append(ip)
    return frozenset(nodes[1:]) if len(nodes) == count else None


class Bucket:
    """A token bucket: rate a second, up to burst saved."""
    __slots__ = ("rate", "burst", "level", "at")

    def __init__(self, rate, burst, now):
        self.rate, self.burst, self.level, self.at = rate, burst, burst, now

    def take(self, now, n=1.0):
        self.level = min(self.burst, self.level + (now - self.at) * self.rate)
        self.at = now
        if self.level < n:
            return False
        self.level -= n
        return True


class Client:
    def __init__(self, at, nonce, ip, name, part, now, o, wire=None):
        self.at, self.nonce, self.ip, self.name, self.part = at, nonce, ip, name, part
        self.wire = wire       # the protocol it speaks (None: SN)
        self.build, self.code = b"", ""   # (its part: wire, build and lobby code)
        self.code_key = None   # (wire, lobby code): its part and the parts of other builds with that code
        self.mods = None       # (SU, SN 3) its mod set (None: its game does not say)
        self.sim = 0           # (SN 3) its simulation version
        # matchmaking (SU)
        self.search_id = None     # its search's id (None: it never searched)
        self.search_since = None  # searching since (None: not searching)
        self.kind = KIND_ROOM     # what it searches for
        self.match = None         # the Match it is in
        self.final = None         # how its last search ended: (id, state, role, the other's address and name)
        self.rb_peer = None       # the player of its last rollback match
        self.cookie = secrets.randbits(64)   # proves the address is real (it came back in a PING)
        self.verified = False
        self.seen = now
        self.ping = 0xFFFF
        self.claim = None      # the session this player asked to join (its LM_JOIN)
        self.listing = None    # the network this player hosts
        # direct connections (from its extended PINGs)
        self.ext = False           # its game can connect directly
        self.direct_ok = False     # ...and its player allows it (it shows the player's address)
        self.lan = None            # (address, port) on its own network
        self.direct_count = 0      # the players it reaches directly now
        self.peers_version = 0     # of the last PEERS it was sent
        self.peers_sent = b""      # ...and its entries
        self.peers_at = -1e9
        self.introduced_count = 0
        self.intro_rooms = {}      # session -> when it was first introduced in that room (the last hour's)
        self.packets = Bucket(o.packet_rate, o.packet_rate * 2, now)
        self.bytes = Bucket(o.byte_rate, o.byte_rate * 2, now)
        self.joins = Bucket(2, 8, now)
        self.lists = Bucket(4, 8, now)
        self.match_asks = Bucket(2, 8, now)


class Match:
    """Two players matchmaking paired: the host makes the room (or is player 1)."""
    def __init__(self, host, guest, now):
        self.host, self.guest, self.at = host, guest, now
        self.room_said = False   # the guest was told the host's room is open
        self.rb_seen = set()     # (a rollback match) the players whose rollback message to the other was carried

    def other(self, c):
        return self.guest if c is self.host else self.host


class Listing:
    """A network a player hosts, from its latest advert."""
    def __init__(self, owner, session, now):
        self.owner, self.session, self.created = owner, session, now
        self.info, self.nodes, self.updated = b"", frozenset(), 0.0
        self.distance = None   # the host's ping to the lobby in 100 ms steps, when the room opened
        self.pair_secrets = {}   # frozenset of two Clients -> their secret for direct packets
        self.talked = set()      # (sender, receiver): players that sent each other game packets here
        self.introduced = set()  # the players last told about each other

    @property
    def count(self):
        return self.info[NI_NODE_COUNT]

    @property
    def open(self):
        return self.info[NI_POLICY] != 1 and self.info[NI_NODE_COUNT] < self.info[NI_NODE_MAX]


def options(**overrides):
    """The lobby's settings (main's command line; tests override some). su_port, status_port: SU's
    port, the status page's (None: not served, 0: any free port)."""
    o = argparse.Namespace(port=DEFAULT_PORT, su_port=None, bind="0.0.0.0", verbose=False, banlist=None,
                           max_clients=2000, max_pending=1000, per_ip=8, gate_rate=5.0, gate_burst=20,
                           packet_rate=600.0, byte_rate=400_000.0, max_data=1500, su_max_data=1500 + NI_SIZE,
                           client_timeout=10.0, pending_timeout=3.0, listed_for=3.5, listing_timeout=10.0,
                           search_for=120.0, match_for=180.0, status_port=None, status_bind="127.0.0.1",
                           tick=1.0, stats=60.0)
    for k, v in overrides.items():
        setattr(o, k, v)
    return o


class Lobby:
    def __init__(self, o):
        self.o = o
        self.sn = Wire("SN", PROTOCOL, DATA, BCAST)
        self.sn3 = Wire("SN3", V3, DATA, BCAST, b"SN", self.sn)
        self.su = Wire("SU", SU_PROTOCOL, SU_DATA, SU_BCAST)
        self.by_addr = {}       # (wire of the port, (host, port)) -> Client
        self.by_ip = {}         # virtual address -> Client
        self.parts = {}         # (wire name, build, lobby code), SN 3: (..., build, simulation, mods, code) -> set of Client
        self.part_rooms = {}    # part -> set of Listing
        self.code_rooms = {}    # (wire name, lobby code) -> set of Listing, every build's
        self.code_players = collections.Counter()   # (wire name, lobby code) -> players, every build's
        self.searching = {}     # part -> {Client: None}: its players searching, the first first
        self.matches = set()    # Match
        self.listings = {}      # session id -> Listing
        self.per_ip = collections.Counter()   # host -> verified players from there
        self.pending = 0        # players not verified yet
        self.gates = {}         # host -> Bucket: answers to strangers' JOINs
        self.next_ip = SUBNET + 2
        self.dirty = set()           # rooms whose players may need a new PEERS
        self.dirty_clients = set()   # ...and players
        self.banned, self.ban_stamp, self.ban_checked = [], None, 0.0
        self.counts = collections.Counter()
        self.drops = collections.Counter()
        self.max_data = 0
        self.started = time.monotonic()
        self.status_server = None
        self.status_open = 0

    # ---- plumbing ---------------------------------------------------------------------
    def close_ports(self):
        for wire in (self.sn, self.su):
            if wire.transport is not None:
                wire.transport.close()
        if self.status_server is not None:
            self.status_server.close()

    def log(self, *what):
        if self.o.verbose:
            self.say(*what)

    @staticmethod
    def say(*what):
        print(time.strftime("%Y-%m-%d %H:%M:%S"), *what, flush=True)

    def send(self, data, at, wire=None):
        """data to the address at, from the port of wire (SN's by default)."""
        self.counts["out packets"] += 1
        self.counts["out bytes"] += len(data)
        (wire or self.sn).port.transport.sendto(data, at)

    def to(self, c, data):
        self.send(data, c.at, c.wire)

    def data_header(self, c):
        """The header of a DATA to c."""
        wire = c.wire or self.sn
        return wire.header(wire.data)

    def reject(self, wire, at, nonce, reason, why, version=None):
        """A REJECT (SU's has no reason byte)."""
        reason = b"" if wire is self.su else bytes((reason,))
        self.send(wire.header(REJECT, version) + nonce + reason + why.encode()[:63].ljust(64, b"\0"), at, wire)

    def gate(self, at, now):
        """Whether a stranger at this address may get an answer now (rate-limited: a lobby must not
        answer floods from spoofed addresses)."""
        b = self.gates.get(at[0])
        if b is None:
            b = self.gates[at[0]] = Bucket(self.o.gate_rate, self.o.gate_burst, now)
        return b.take(now)

    def is_banned(self, host):
        if not self.banned:
            return False
        try:
            a = ipaddress.ip_address(host)
        except ValueError:
            return False
        return any(a in net for net in self.banned)

    def load_banlist(self, now):
        self.ban_checked = now
        path = self.o.banlist
        if not path:
            return
        try:
            stamp = os.stat(path).st_mtime
        except OSError:
            stamp = None
        if stamp == self.ban_stamp:
            return
        self.ban_stamp, banned = stamp, []
        if stamp is not None:
            with open(path, encoding="utf-8", errors="replace") as f:
                for line in f:
                    line = line.split("#", 1)[0].strip()
                    if line:
                        try:
                            banned.append(ipaddress.ip_network(line, strict=False))
                        except ValueError:
                            self.say(f"banlist: not an address: {printable(line)}")
        self.banned = banned
        self.say(f"banlist: {len(banned)} entr{'y' if len(banned) == 1 else 'ies'}")
        for c in list(self.by_addr.values()):
            if self.is_banned(c.at[0]):
                self.drop(c, "banned")

    def new_ip(self):
        """A free virtual address, handed out in turn (a freed one is not reused at once)."""
        for _ in range(0x10000):
            ip = self.next_ip
            self.next_ip = SUBNET + 2 if ip >= SUBNET + 0xFFFE else ip + 1
            if ip & 0xFF not in (0, 255) and ip not in self.by_ip:
                return ip
        return None

    # ---- who is in which network -----------------------------------------------------
    def members(self, listing):
        """The players of a network: its host, and every station it lists that asked to join it."""
        out = [listing.owner]
        for ip in listing.nodes:
            c = self.by_ip.get(ip)
            if c is not None and c.claim == listing.session:
                out.append(c)
        return out

    def network_of(self, c):
        if c.listing is not None:
            return c.listing
        listing = self.listings.get(c.claim) if c.claim else None
        return listing if listing is not None and c.ip in listing.nodes else None

    def close(self, listing, packet=None):
        """A network is gone: tell its players (with the host's own LM_GONE if there is one)."""
        if packet is None:
            packet = ldn_packet(listing.owner.ip, BROADCASTS[0], LM_GONE, listing.session, self.data_header(listing.owner))
        self.dirty_clients.update(self.members(listing), listing.introduced)
        self.dirty.discard(listing)
        for m in self.members(listing):
            if m is not listing.owner:
                self.to(m, packet)
                m.claim = None
        self.listings.pop(listing.session, None)
        for index, key in ((self.part_rooms, listing.owner.part), (self.code_rooms, listing.owner.code_key)):
            rooms = index.get(key)
            if rooms is not None:
                rooms.discard(listing)
                if not rooms:
                    del index[key]
        if listing.owner.listing is listing:
            listing.owner.listing = None
        self.log(f"room of {printable(listing.owner.name)} closed")

    def drop(self, c, why):
        if c.match is not None:   # (the other player is told at once)
            other = c.match.other(c)
            c.match = None
            self.match_end(other, MS_PEER_LEFT, f"{printable(c.name)} left the lobby", time.monotonic())
        self.unsearch(c)
        if c.listing is not None:
            self.close(c.listing)
        if c.claim:   # tell its host at once rather than after the host's timeout
            listing = self.listings.get(c.claim)
            if listing is not None and c.ip in listing.nodes:
                self.to(listing.owner, ldn_packet(c.ip, listing.owner.ip, LM_LEAVE, c.claim, self.data_header(listing.owner)))
                self.dirty.add(listing)
        self.dirty_clients.discard(c)
        self.by_addr.pop(((c.wire or self.sn).port, c.at), None)
        self.by_ip.pop(c.ip, None)
        players = self.parts.get(c.part)
        if players is not None:
            players.discard(c)
            if not players:
                del self.parts[c.part]
        if self.code_players[c.code_key] > 1:
            self.code_players[c.code_key] -= 1
        else:
            del self.code_players[c.code_key]
        if c.verified:
            self.per_ip[c.at[0]] -= 1
            if self.per_ip[c.at[0]] <= 0:
                del self.per_ip[c.at[0]]
        else:
            self.pending -= 1
        self.log(f"{printable(c.name)} ({ip_text(c.ip)}) left: {why}")

    def verify(self, c):
        if not c.verified:
            c.verified = True
            self.pending -= 1
            self.per_ip[c.at[0]] += 1

    # ---- packets ------------------------------------------------------------------------
    def received(self, wire, p, at):
        self.counts["in packets"] += 1
        self.counts["in bytes"] += len(p)
        if len(p) < 4 or p[:2] != wire.magic:
            return
        try:
            now = time.monotonic()
            (self.packet if wire is self.sn else self.su_packet)(p, at, now)
            if self.dirty or self.dirty_clients:
                self.flush(now)
        except Exception as e:   # (one bad packet never stops the lobby)
            self.drops["error"] += 1
            self.say(f"error on a packet from {at[0]}: {e!r}")

    def packet(self, p, at, now):
        """A packet of WindowSmash's games (SN, version 2 or 3)."""
        kind = p[3]
        if p[2] not in (PROTOCOL, V3):   # another version of the game's netplay: say so
            if kind == JOIN and len(p) >= V1_JOIN_SIZE and self.gate(at, now):
                self.reject(self.sn, at, p[4:12], 2, f"different netplay version (lobby {PROTOCOL} or {V3}, you {p[2]})", p[2])
            return
        wire = self.sn if p[2] == PROTOCOL else self.sn3
        c = self.by_addr.get((self.sn, at))
        if kind == JOIN:
            self.on_join(wire, p, at, c, now)
        elif c is None or (c.wire or self.sn) is not wire:   # (none, or a packet of the other version from there)
            return
        elif kind == PING:
            if len(p) < PING_SIZE or p[4:12] != c.nonce or struct.unpack_from(">Q", p, 20)[0] != c.cookie:
                return
            self.verify(c)
            c.seen = now
            c.ping = struct.unpack_from(">H", p, 28)[0]
            if len(p) >= PING_EXT_SIZE:
                self.on_ping_ext(c, p, now)
            pong = wire.header(PONG) + p[12:20] + struct.pack(">HHB", min(len(self.parts.get(c.part, ())), 0xFFFF),
                                                              min(len(self.part_rooms.get(c.part, ())), 0xFFFF), 1)
            if wire is self.sn3:   # (and players searching of its key, players of its lobby code)
                pong += struct.pack(">HH", min(len(self.searching.get(c.part, ())), 0xFFFF), min(self.code_players[c.code_key], 0xFFFF))
            self.to(c, pong)
        elif kind == LEAVE:
            if len(p) >= 12 and p[4:12] == c.nonce:
                self.drop(c, "left")
        elif kind == LIST:
            if len(p) < 16 or struct.unpack_from(">Q", p, 4)[0] != c.cookie:
                return
            self.verify(c)
            c.seen = now
            if not c.lists.take(now):
                self.drops["list rate"] += 1
                return
            self.on_list(c, p[12:16], now, wire is self.sn3 and len(p) > 16 and bool(p[16] & 1))   # (3: flags 1, every key's)
        elif kind == DATA:
            self.on_data(c, p, now)
        elif kind == BCAST:
            self.on_bcast(c, p, now)
        elif wire is self.sn3 and kind in (ROOMS, MATCH, RB):   # (version 3's own: they start with the cookie)
            if len(p) < 12 or struct.unpack_from(">Q", p, 4)[0] != c.cookie:
                return
            self.verify(c)
            c.seen = now
            self.on_extra(c, kind, p, now)

    def on_extra(self, c, kind, p, now):
        """ROOMS, MATCH and RB, of SU and SN 3 (the same numbers and layouts; the cookie checked)."""
        if kind == ROOMS:
            if len(p) < 16:
                return
            if not c.lists.take(now):
                self.drops["list rate"] += 1
                return
            self.on_rooms(c, p[12:16], now)
        elif kind == MATCH:
            if len(p) < 17:
                return
            if not c.match_asks.take(now):
                self.drops["match rate"] += 1
                return
            self.on_match(c, p[12], struct.unpack_from(">I", p, 13)[0], now)
        elif kind == RB:
            self.on_rb(c, p, now)

    def on_data(self, c, p, now):
        if len(p) < 16 or not self.admit(c, p, 4, now):
            return
        src, dst, sport, dport = struct.unpack_from(">IIHH", p, 4)
        if src != c.ip:
            self.drops["not its address"] += 1
        elif sport == LDN_PORT or dport == LDN_PORT:
            self.on_ldn(c, p, dst, sport, dport, now)
        else:
            self.on_game(c, p, dst)

    def admit(self, c, p, at, now):
        """Whether a verified player's datagram (its DATA body at offset at) may be carried."""
        if not c.verified:
            return False
        c.seen = now
        size = len(p) - at - 12
        if size > (self.o.su_max_data if c.wire is self.su else self.o.max_data):
            self.drops["too big"] += 1
            return False
        self.max_data = max(self.max_data, size)
        if not c.packets.take(now) or not c.bytes.take(now, len(p)):
            self.drops["rate"] += 1
            return False
        return True

    def on_join(self, wire, p, at, c, now):
        """JOIN: nonce, lobby code, name, build; version 3's then its simulation version and mod set."""
        if len(p) < (JOIN_SIZE if wire is self.sn else V3_JOIN_SIZE):
            return
        nonce = p[4:12]
        room, name = text(p[12:12 + NAME_LEN]), text(p[12 + NAME_LEN:12 + 2 * NAME_LEN])
        build = p[12 + 2 * NAME_LEN:JOIN_SIZE].rstrip(b"\0")
        part = mods = None
        if wire is self.sn3:
            sim, mods = struct.unpack_from(">I", p, JOIN_SIZE)[0], mods_block(p[JOIN_SIZE + 4:])
            if mods is None:
                if not self.is_banned(at[0]) and self.gate(at, now):
                    self.reject(wire, at, nonce, 5, "the lobby cannot read your game's mod list")
                return
            part = (wire.name, build.upper(), sim, mods_key(mods), room)
        c = self.join(wire, at, c, nonce, room, name, build, now, part)
        if c is None:
            return
        flags = LOBBY | INTRODUCES
        if wire is self.sn3:
            c.sim, c.mods = sim, mods
            flags |= HAS_ROOMS | HAS_MATCH | HAS_RB
        self.to(c, wire.header(WELCOME) + nonce + struct.pack(">IQB", c.ip, c.cookie, flags) + b"\0")

    def join(self, wire, at, c, nonce, room, name, build, now, part=None):
        """The player a JOIN comes from (c: the one at its address, if any): a new one, one back
        from a new port, or the same again; None when it is turned away. part: its key (by default
        the wire's name, its build and lobby code)."""
        if self.is_banned(at[0]):
            self.drops["banned"] += 1
            return None
        part = part or (wire.name, build, room)
        if c is not None and (c.nonce != nonce or (c.wire or self.sn) is not wire or c.part != part):
            self.drop(c, "restarted")   # (a new game from the same place, or another version or key)
            c = None
        if c is None:   # a player back from a new place (its router changed its port)?
            for other in self.parts.get(part, ()):
                if other.nonce == nonce and other.verified:
                    del self.by_addr[wire.port, other.at]
                    self.per_ip[other.at[0]] -= 1
                    if self.per_ip[other.at[0]] <= 0:
                        del self.per_ip[other.at[0]]
                    self.per_ip[at[0]] += 1
                    other.at = at
                    self.by_addr[wire.port, at] = c = other
                    self.mark(c)   # (the players it was introduced to need its new port)
                    break
        if c is None:   # a new player
            if not self.gate(at, now):
                self.drops["join rate"] += 1
                return None
            if wire is self.sn and build.upper() != REQUIRED_BUILD:   # (another game version: refused, saying why)
                self.drops["game version"] += 1
                self.log(f"refused {printable(name)} from {at[0]}:{at[1]}: build {build[:8].decode('ascii', 'replace')}, "
                         "not 13.0.5's")
                self.reject(wire, at, nonce, 4, NOT_13)
                return None
            why = ("the lobby is full" if len(self.by_addr) >= self.o.max_clients or self.pending >= self.o.max_pending
                   else f"too many players from your address ({self.o.per_ip})" if self.per_ip[at[0]] >= self.o.per_ip
                   else None)
            ip = self.new_ip() if why is None else None
            if ip is None:
                self.reject(wire, at, nonce, 1, why or "the lobby is full")
                return None
            c = Client(at, nonce, ip, name, part, now, self.o, wire)
            c.build, c.code, c.code_key = build, room, (wire.name, room)
            self.by_addr[wire.port, at], self.by_ip[ip] = c, c
            self.parts.setdefault(part, set()).add(c)
            self.code_players[c.code_key] += 1
            self.pending += 1
            self.log(f"{printable(name)} joined lobby {printable(room)!r} ({wire.name}, build "
                     f"{build[:8].decode('ascii', 'replace')}) as {ip_text(ip)} from {at[0]}:{at[1]}")
        c.seen = now
        return c

    # ---- ssbu-recomp's games (SU) -------------------------------------------------------
    # The same lobby in SU's numbers and layouts, with WindowSmash's limits, rates and timeouts:
    # a player is verified (its cookie came back) before anything of its is carried, and players
    # are introduced to each other only once their games talk.
    def su_packet(self, p, at, now):
        """A packet of ssbu-recomp's games (SU)."""
        if p[2] != SU_PROTOCOL:   # (another version: nothing it says is known)
            return
        kind = p[3]
        c = self.by_addr.get((self.su, at))
        if kind == SU_JOIN:
            self.su_join(p, at, c, now)
            return
        if c is None:
            return
        if kind == SU_DATA:
            self.on_data(c, p, now)
            return
        if kind == SU_BCAST:
            self.on_bcast(c, p, now)
            return
        if len(p) < 12 or struct.unpack_from(">Q", p, 4)[0] != c.cookie:   # (the rest starts with the cookie)
            return
        self.verify(c)
        c.seen = now
        if kind == SU_PING:
            if len(p) < SU_PING_SIZE:
                return
            c.direct_count = min(p[20], ROOM_MAX - 1)
            counts = (len(self.parts.get(c.part, ())), len(self.part_rooms.get(c.part, ())),
                      len(self.searching.get(c.part, ())), self.code_players[c.code_key])   # (the last: every build's)
            self.to(c, su_header(SU_PONG) + p[12:20] + struct.pack(">HHHH", *(min(n, 0xFFFF) for n in counts)))
            if c.peers_version and now - c.peers_at >= SU_PEERS_AGAIN:   # (in case the last was lost)
                self.sync(c, now, again=True)
        elif kind == SU_LEAVE:
            self.drop(c, "left")
        elif kind == SU_LIST:
            if len(p) < 16:
                return
            if not c.lists.take(now):
                self.drops["list rate"] += 1
                return
            self.on_list(c, p[12:16], now, len(p) > 16 and bool(p[16] & 1))   # (flags 1: the rooms of every build of its lobby code)
        elif kind in (SU_ROOMS, SU_MATCH, SU_RB):
            self.on_extra(c, kind, p, now)

    def su_join(self, p, at, c, now):
        """JOIN: nonce, lobby code, name, build, LAN address and port, flags (1: direct connections),
        and from a newer game "MS" and its mod set."""
        if len(p) < SU_JOIN_SIZE:
            return
        nonce = p[4:12]
        code, name = text(p[12:12 + NAME_LEN]), text(p[12 + NAME_LEN:12 + 2 * NAME_LEN]) or "Player"
        o = 12 + 2 * NAME_LEN + BUILD_LEN
        build = p[12 + 2 * NAME_LEN:o].rstrip(b"\0")
        lan_ip, lan_port = struct.unpack_from(">IH", p, o)
        c = self.join(self.su, at, c, nonce, code, name, build, now)
        if c is None:
            return
        if p[SU_JOIN_SIZE:SU_JOIN_SIZE + 2] == b"MS":   # (an older game sends no mods)
            c.mods = mods_block(p[SU_JOIN_SIZE + 2:])
        direct_ok, lan = bool(p[o + 6] & 1), ((lan_ip, lan_port) if lan_ip and lan_port else None)
        if not c.ext or direct_ok != c.direct_ok or lan != c.lan:   # (every SU game can connect directly)
            c.ext, c.direct_ok, c.lan = True, direct_ok, lan
            self.mark(c)
        self.to(c, su_header(SU_WELCOME) + nonce + struct.pack(">IQ", c.ip, c.cookie))

    # ---- rooms ----------------------------------------------------------------------------
    def on_list(self, c, seq, now, every=False):
        """The rooms this player's Rooms List shows: open ones first, fuller ones first, then the
        ones whose host is closest to the lobby (in 100 ms steps, taken when the room opened: the
        list must not reorder under the player's cursor from one scan to the next), then the oldest.
        every (SU, SN 3): the rooms of every build (SN 3: every key) of its lobby code, its own first,
        each LISTING ending with its host's build (SN 3: and simulation version and mod set). A player
        matchmaking paired sees its host's room first."""
        pool = self.code_rooms.get(c.code_key, ()) if every else self.part_rooms.get(c.part, ())
        host = c.match.host if c.match is not None and c.match.guest is c else None   # (the host it was matched with first)
        rooms = sorted((r for r in pool if r.owner is not c and now - r.updated <= self.o.listed_for),
                       key=lambda r: (r.owner is not host, r.owner.part != c.part, *self.room_order(r)))[:LIST_MAX]
        su = c.wire is self.su
        for r in rooms:
            tail = r.owner.build[:BUILD_LEN].ljust(BUILD_LEN, b"\0") if every else b""
            if every and not su:
                tail += struct.pack(">I", r.owner.sim) + r.owner.mods
            self.to(c, (su_header(SU_LISTING) if su else (c.wire or self.sn).header(LISTING)) + seq + struct.pack(">I", r.owner.ip) +
                    r.info + tail)
        self.to(c, (su_header(SU_LIST_END) if su else (c.wire or self.sn).header(LIST_END)) + seq + bytes((len(rooms),)))

    @staticmethod
    def room_order(r):
        return not r.open, -r.count, 10 if r.distance is None else r.distance, r.created

    def on_rooms(self, c, seq, now):
        """(SU, SN 3) The room browser: every room of its lobby code, its own build's (key's) first,
        then as a Rooms List; each with its host's address and name, players, build (SN 3: and
        simulation version) and mods, and flags (1 open, 2 its own, 4 its host's game says its
        mods; SN 3: 8 of its key, one it can join)."""
        rooms = sorted((r for r in self.code_rooms.get(c.code_key, ()) if now - r.updated <= self.o.listed_for),
                       key=lambda r: (r.owner.part != c.part, *self.room_order(r)))[:ROOMS_MAX]
        su, wire = c.wire is self.su, c.wire or self.sn
        for r in rooms:
            h = r.owner
            flags = (1 if r.open else 0) | (2 if h is c else 0) | (4 if h.mods is not None else 0)
            if not su and h.part == c.part:
                flags |= 8
            self.to(c, wire.header(ROOM_INFO) + seq + struct.pack(">IBBB", h.ip, r.count, r.info[NI_NODE_MAX], flags) +
                    h.name.encode()[:NAME_LEN].ljust(NAME_LEN, b"\0") + h.build[:BUILD_LEN].ljust(BUILD_LEN, b"\0") +
                    (b"" if su else struct.pack(">I", h.sim)) + (h.mods or b"\0"))
        self.to(c, wire.header(ROOMS_END) + seq + bytes((len(rooms),)))

    def on_ldn(self, c, p, dst, sport, dport, now):
        """Local wireless's own messages: only the ones its protocol has, between the players they
        concern."""
        if sport != LDN_PORT or dport != LDN_PORT or len(p) < 20 or p[16] != ord("L"):
            self.drops["ldn"] += 1
            return
        kind, body = p[17], p[20:]
        if dst in BROADCASTS:
            if kind == LM_ADVERTISE:
                self.on_advertise(c, p, body, now)
            elif kind == LM_GONE and c.listing is not None and body[:16] == c.listing.session:
                self.close(c.listing, p)
            else:
                self.drops["ldn"] += 1
            return
        b = self.by_ip.get(dst)
        if b is None or b is c or b.part != c.part or not b.verified:
            self.drops["ldn"] += 1
            return
        if kind == LM_JOIN:   # asking to join b's network
            listing = b.listing
            if listing is None or body[:16] != listing.session:
                self.drops["ldn"] += 1
                return
            if not c.joins.take(now):
                self.drops["join rate"] += 1
                return
            if c.listing is not None:   # (a host cannot be joining: its network is gone)
                self.close(c.listing)
            self.mark(c)
            c.claim = listing.session
            self.dirty.add(listing)
        elif kind in (LM_ACCEPT, LM_REJECT, LM_KICK):   # the host answering one who asked
            if c.listing is None or b.claim != c.listing.session:
                self.drops["ldn"] += 1
                return
        elif kind in (LM_KEEP, LM_LEAVE):   # a station to its host
            if not c.claim or body[:16] != c.claim or b.listing is None or b.listing.session != c.claim:
                self.drops["ldn"] += 1
                return
            if kind == LM_LEAVE:
                self.to(b, p)
                c.claim = None
                self.dirty.add(b.listing)
                self.dirty_clients.add(c)
                return
        else:
            self.drops["ldn"] += 1
            return
        self.to(b, p)

    def on_advertise(self, c, p, body, now):
        if len(body) < NI_SIZE:
            self.drops["bad advert"] += 1
            return
        info = bytes(body[:NI_SIZE])
        nodes = network_nodes(info, c.ip)
        if nodes is None:
            self.drops["bad advert"] += 1
            return
        session = info[NI_SESSION:NI_SESSION + 16]
        listing = self.listings.get(session)
        if listing is not None and listing.owner is not c:
            self.drops["session taken"] += 1
            return
        if listing is None:   # a new network (one at a time: it replaces the host's last)
            if c.listing is not None:
                self.close(c.listing)
            self.mark(c)   # (it was perhaps a station of another)
            c.claim = None
            listing = c.listing = self.listings[session] = Listing(c, session, now)
            self.part_rooms.setdefault(c.part, set()).add(listing)
            self.code_rooms.setdefault(c.code_key, set()).add(listing)
            self.log(f"{printable(c.name)} opened a room ({info[NI_NODE_MAX]} players at most)")
        if listing.distance is None and c.ping != 0xFFFF:
            listing.distance = min(c.ping, 1000) // 100
        before = self.members(listing)
        if nodes != listing.nodes:
            self.dirty.add(listing)
        listing.info, listing.nodes, listing.updated = info, nodes, now
        # (to whoever is in it, and whoever just left it, so they learn it at once)
        for m in {*before, *self.members(listing)}:
            if m is not c:
                self.to(m, p)
        if c.match is not None and c.match.host is c and c.kind == KIND_ROOM:
            self.match_room(c.match, listing, now)

    def on_game(self, c, p, dst):
        """The game's own traffic: only within a network, between its players."""
        listing = self.network_of(c)
        if listing is None:
            self.drops["not in a room"] += 1
            return
        members = self.members(listing)
        if dst in BROADCASTS:
            for m in members:
                if m is not c:
                    self.to(m, p)
                    self.counts["game relayed"] += 1
            return
        m = self.by_ip.get(dst)
        if m is None or m is c or m not in members:
            self.drops["not in the room"] += 1
            return
        self.to(m, p)
        self.counts["game relayed"] += 1
        if (c, m) not in listing.talked:   # (both ways: their games really play together - introduce them)
            listing.talked.add((c, m))
            if (m, c) in listing.talked:
                self.dirty.add(listing)

    def on_bcast(self, c, p, now):
        """A broadcast to the room's players other than the ones listed (the sender reaches those
        directly): count u8, count x address u32, then a DATA body."""
        if len(p) < 5:
            return
        n = p[4]
        at = 5 + 4 * n
        if n >= ROOM_MAX or len(p) < at + 12:
            self.drops["bad broadcast"] += 1
            return
        if not self.admit(c, p, at, now):
            return
        skip = set(struct.unpack_from(f">{n}I", p, 5))
        src, dst, sport, dport = struct.unpack_from(">IIHH", p, at)
        if src != c.ip:
            self.drops["not its address"] += 1
            return
        if dst not in BROADCASTS or sport == LDN_PORT or dport == LDN_PORT:
            self.drops["bad broadcast"] += 1
            return
        listing = self.network_of(c)
        if listing is None:
            self.drops["not in a room"] += 1
            return
        out = self.data_header(c) + p[at:]
        for m in self.members(listing):
            if m is not c and m.ip not in skip:
                self.to(m, out)
                self.counts["game relayed"] += 1

    # ---- matchmaking (SU, SN 3) -----------------------------------------------------------
    def on_match(self, c, op, sid, now):
        """MATCH: op 1 (or 2: for a rollback match) searches, sent again every 2 s while it waits or
        is matched (the answer says how it stands); op 0 cancels. sid: the search's id."""
        if op == 0:
            if c.match is not None:
                other = c.match.other(c)
                c.match = None
                self.match_end(other, MS_PEER_LEFT, f"{printable(c.name)} cancelled the match", now)
            if c.search_since is not None:
                self.log(f"match: {printable(c.name)} stopped searching")
            self.unsearch(c)
            c.search_id, c.final = sid, (sid, MS_IDLE, 0, 0, b"")
            self.match_send(c, now)
            return
        if op not in (KIND_ROOM, KIND_ROLLBACK):
            return
        if c.search_id == sid and (c.match is not None or c.search_since is not None or c.final is not None):
            self.match_send(c, now)   # (the same search: how it stands, or how it ended)
            return
        if c.match is not None:   # (a new search while matched: that match is over)
            other = c.match.other(c)
            c.match = None
            self.match_end(other, MS_PEER_LEFT, f"{printable(c.name)} searches again", now)
        c.search_id, c.final, c.kind = sid, None, op
        self.unsearch(c)
        waiting = [x for x in self.searching.get(c.part, ()) if x.kind == op]
        if not waiting:
            c.search_since = now
            self.searching.setdefault(c.part, {})[c] = None
            self.log(f"match: {printable(c.name)} searches{' for a rollback match' if op == KIND_ROLLBACK else ''}")
            self.match_send(c, now)
            return
        o = min(waiting, key=lambda x: x.search_since)
        self.unsearch(o)

        def hosts_open(x):
            return op == KIND_ROOM and x.listing is not None and x.listing.open
        host = c if hosts_open(c) and not hosts_open(o) else o   # (an open room, else the one that waited longer)
        m = c.match = o.match = Match(host, o if host is c else c, now)
        self.matches.add(m)
        if op == KIND_ROLLBACK:
            c.rb_peer, o.rb_peer = o, c
        self.log(f"match: {printable(m.host.name)} hosts, {printable(m.guest.name)} joins"
                 f"{' (a rollback match)' if op == KIND_ROLLBACK else ''}")
        self.match_send(m.host, now)
        self.match_send(m.guest, now)
        if op == KIND_ROOM and m.host.listing is not None:
            self.match_room(m, m.host.listing, now)

    def on_rb(self, c, p, now):
        """A rollback message: to a player of its build and lobby code (SN 3: its key), or (destination
        0, on a private code only) to all of them. On the public code (SU's "public", SN's "smash"),
        only to the player of its rollback match or a player of its room. Once one went each way
        between the two of a rollback match, they are together."""
        if not 16 < len(p) <= RB_MAX:
            self.drops["bad rollback"] += 1
            return
        dst = struct.unpack_from(">I", p, 12)[0]
        m = self.by_ip.get(dst) if dst else None
        if c.code == (SU_PUBLIC if c.wire is self.su else V3_PUBLIC):
            room = self.network_of(c)
            if m is None or (m is not c.rb_peer and (room is None or m not in self.members(room))):
                self.drops["rollback stranger"] += 1
                return
        if dst:
            to = [m] if m is not None and m is not c and m.part == c.part and m.verified else []
        else:
            to = [x for x in self.parts.get(c.part, ()) if x is not c and x.verified]
        if not to:
            self.drops["rollback nobody"] += 1
            return
        if not c.packets.take(now, len(to)) or not c.bytes.take(now, len(p) * len(to)):   # (every copy counts)
            self.drops["rate"] += 1
            return
        out = (c.wire or self.sn).header(RB) + struct.pack(">I", c.ip) + p[16:]
        for x in to:
            self.to(x, out)
        self.counts["rollback relayed"] += len(to)
        match = c.match
        if m is not None and match is not None and c.kind == KIND_ROLLBACK and match.other(c) is m:
            match.rb_seen.add(c)
            if len(match.rb_seen) == 2:
                host, guest = match.host, match.guest
                self.match_end(host, MS_TOGETHER, f"playing a rollback match with {printable(guest.name)}", now)
                self.match_end(guest, MS_TOGETHER, f"playing a rollback match with {printable(host.name)}", now)

    def unsearch(self, c):
        c.search_since = None
        waiting = self.searching.get(c.part)
        if waiting is not None:
            waiting.pop(c, None)
            if not waiting:
                del self.searching[c.part]

    def match_room(self, m, listing, now):
        """The host of a match advertised its room: with the guest in it, they are together; once it
        is open, both are told (once: the guest asks again anyway)."""
        if m.guest.ip in listing.nodes and m.guest.claim == listing.session:
            host, guest = m.host, m.guest
            self.match_end(host, MS_TOGETHER, f"in a room with {printable(guest.name)}", now)
            self.match_end(guest, MS_TOGETHER, f"in {printable(host.name)}'s room", now)
        elif listing.open and not m.room_said:
            m.room_said = True
            self.match_send(m.guest, now)
            self.match_send(m.host, now)

    def match_end(self, c, state, why, now):
        """c's search or match is over (MS_TIMEOUT, MS_PEER_LEFT, MS_TOGETHER, MS_EXPIRED): it is told,
        and told again when it asks."""
        m, role, peer, name = c.match, 0, 0, b""
        if m is not None:
            o = m.other(c)
            role, peer, name = ROLE_HOST if m.host is c else ROLE_JOIN, o.ip, o.name.encode()
            self.matches.discard(m)
        c.match = None
        self.unsearch(c)
        c.final = (c.search_id or 0, state, role, peer, name)
        self.match_send(c, now)
        self.log(f"match: {printable(c.name)}: {why}")

    def match_send(self, c, now):
        """MATCH_STATE: search id u32, state u8, role u8, flags u8 (1: the host's room is open), the
        other player's address u32 and name[32], then of its build and lobby code (SN 3: its key):
        players searching u16, players u16; and the seconds waited u16."""
        m, sid, role, flags, peer, name = c.match, c.search_id or 0, 0, 0, 0, b""
        if m is not None:
            o = m.other(c)
            state, role, peer, name = MS_MATCHED, ROLE_HOST if m.host is c else ROLE_JOIN, o.ip, o.name.encode()
            r = m.host.listing
            if r is not None and r.open and now - r.updated <= self.o.listed_for:
                flags |= 1
        elif c.search_since is not None:
            state = MS_SEARCHING
        elif c.final is not None:
            sid, state, role, peer, name = c.final
        else:
            state = MS_IDLE
        since = m.at if m is not None else c.search_since
        waited = 0 if since is None else min(int(now - since), 0xFFFF)
        self.to(c, (c.wire or self.sn).header(MATCH_STATE) + struct.pack(">IBBBI", sid & 0xFFFFFFFF, state, role, flags, peer) +
                name[:NAME_LEN].ljust(NAME_LEN, b"\0") +
                struct.pack(">HHH", min(len(self.searching.get(c.part, ())), 0xFFFF), min(len(self.parts.get(c.part, ())), 0xFFFF), waited))

    def match_sweep(self, now):
        for waiting in list(self.searching.values()):
            for c in list(waiting):
                if now - c.search_since > self.o.search_for:
                    self.match_end(c, MS_TIMEOUT, f"no opponent in {self.o.search_for:.0f} s", now)
        for m in list(self.matches):
            if now - m.at > self.o.match_for:
                host, guest = m.host, m.guest
                self.match_end(host, MS_EXPIRED, f"the match with {printable(guest.name)} expired", now)
                self.match_end(guest, MS_EXPIRED, f"the match with {printable(host.name)} expired", now)

    # ---- direct connections -------------------------------------------------------------
    # The players of a room whose games can connect directly are introduced to each other: each
    # learns the other's address as the lobby sees it, and a secret for the two of them, which
    # every direct packet carries. Their games then punch through their routers and send the
    # room's traffic straight to each other, falling back to the lobby when that fails. A pair
    # is introduced only once both allow it and their games have sent each other game packets
    # through the lobby (they really play together: one that only joins rooms learns nobody's
    # address), and a player in INTRO_ROOMS rooms an hour at most.
    def on_ping_ext(self, c, p, now):
        have, lan_ip, lan_port, flags, ndirect = struct.unpack_from(">IIHBB", p, PING_SIZE)
        direct_ok, lan = bool(flags & 1), ((lan_ip, lan_port) if lan_ip and lan_port else None)
        if not c.ext or direct_ok != c.direct_ok or lan != c.lan:
            c.ext, c.direct_ok, c.lan = True, direct_ok, lan
            self.mark(c)
        c.direct_count = min(ndirect, ROOM_MAX - 1)
        if have != c.peers_version:   # (a PEERS lost on the way: again)
            self.sync(c, now, again=True)

    def mark(self, c):
        """c's room (if any) needs its players' PEERS looked at."""
        listing = self.network_of(c)
        if listing is not None:
            self.dirty.add(listing)
        self.dirty_clients.add(c)

    def flush(self, now):
        while self.dirty:
            listing = self.dirty.pop()
            if self.listings.get(listing.session) is not listing:
                continue   # (closed: close() queued its players)
            members = set(self.members(listing))
            gone = listing.introduced - members
            listing.introduced = members
            if gone:   # (one that comes back is introduced anew, with a new secret)
                listing.pair_secrets = {k: v for k, v in listing.pair_secrets.items() if not k & gone}
                listing.talked = {t for t in listing.talked if t[0] not in gone and t[1] not in gone}
            self.dirty_clients |= members | gone
        todo, self.dirty_clients = self.dirty_clients, set()
        for c in todo:
            self.sync(c, now)

    def room_ok(self, c, session, now):
        if c.intro_rooms and now - min(c.intro_rooms.values()) > 3600:
            c.intro_rooms = {k: t for k, t in c.intro_rooms.items() if now - t <= 3600}
        return session in c.intro_rooms or len(c.intro_rooms) < INTRO_ROOMS

    def pair_secret(self, listing, a, b, now):
        """The secret of two players of a room, or 0 when they are not to be introduced."""
        if not (a.direct_ok and b.direct_ok):
            return 0
        key = frozenset((a, b))
        s = listing.pair_secrets.get(key)
        if s is None:
            if not (a.ext and b.ext and (a, b) in listing.talked and (b, a) in listing.talked and
                    self.room_ok(a, listing.session, now) and self.room_ok(b, listing.session, now)):
                return 0
            s = listing.pair_secrets[key] = secrets.randbits(64) | 1
            for x in (a, b):
                x.intro_rooms.setdefault(listing.session, now)
        return s

    def sync(self, c, now, again=False):
        """Send c the players of its room (none: an empty list), when they changed (or again)."""
        if not c.ext or self.by_addr.get(((c.wire or self.sn).port, c.at)) is not c:
            return
        listing = self.network_of(c)
        peers = sorted((m for m in self.members(listing) if m is not c), key=lambda m: m.ip) if listing else []
        su = c.wire is self.su
        entries, introduced = [], 0
        for m in peers:
            secret = self.pair_secret(listing, c, m, now)
            pub = lan = (0, 0)
            if secret:
                introduced += 1
                pub = (address_number(m.at[0]), m.at[1])
                if m.at[0] == c.at[0] and m.lan and m.lan != pub:   # (behind the same router: its address there too)
                    lan = m.lan
            if su:   # (SU lists only the players introduced)
                if secret:
                    entries.append(SU_PEER.pack(m.ip, m.name.encode()[:NAME_LEN], *pub, *lan, secret))
                continue
            flags = (1 if secret else 0) | (0 if m.direct_ok else 2 if m.ext else 4)   # (2: it turned them off, 4: an older game)
            entries.append(PEER.pack(m.ip, m.name.encode()[:NAME_LEN], flags, *pub, *lan, secret))
        entries = b"".join(entries)
        if entries == c.peers_sent and not again:
            return
        if now - c.peers_at < PEERS_GAP:
            self.dirty_clients.add(c)   # (later: a room changing fast sends no flood)
            return
        if entries != c.peers_sent:
            c.peers_version = (c.peers_version + 1) & 0xFFFFFFFF or 1
            c.peers_sent = entries
        c.peers_at = now
        c.introduced_count = introduced
        if su:
            self.to(c, su_header(SU_PEERS) + struct.pack(">IB", c.peers_version, introduced) + entries)
        else:
            self.to(c, (c.wire or self.sn).header(PEERS) + struct.pack(">QIB", c.cookie, c.peers_version, len(peers)) + entries)

    # ---- the status page ----------------------------------------------------------------
    def status_json(self, now):
        """Counts only: never an address, a name, a lobby code or a build."""
        return json.dumps({"up": True, "players": len(self.by_addr) - self.pending, "rooms": len(self.listings),
                           "searching": sum(len(w) for w in self.searching.values()),
                           "version": f"{LOBBY_VERSION} (protocols SN {PROTOCOL} and {V3}, SU {SU_PROTOCOL})",
                           "uptime_s": int(now - self.started)})

    async def status(self, reader, writer):
        """One request to the status page: GET or HEAD /status, OPTIONS; any page may read it (CORS),
        and nothing is logged (it would hold the callers' addresses). STATUS_OPEN at once,
        STATUS_REQUEST bytes and STATUS_TIME to ask."""
        if self.status_open >= STATUS_OPEN:
            self.drops["status requests"] += 1
            writer.transport.abort()
            return
        self.status_open += 1
        try:
            line = (await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), STATUS_TIME)).split(b"\r\n", 1)[0].split(b" ")
            method, path = (line[0], line[1].split(b"?", 1)[0].rstrip(b"/")) if len(line) == 3 else (b"", b"")
            if method == b"OPTIONS":
                code, body = 204, b""
            elif method not in (b"GET", b"HEAD"):
                code, body = 405, b'{"error":"method not allowed"}'
            elif path == b"/status":
                code, body = 200, self.status_json(time.monotonic()).encode()
            else:
                code, body = 404, b'{"error":"not found"}'
            reason = {200: "OK", 204: "No Content", 404: "Not Found", 405: "Method Not Allowed"}[code]
            head = (f"HTTP/1.0 {code} {reason}\r\nServer: ssbu-lobby-status\r\nDate: {email.utils.formatdate(usegmt=True)}\r\n"
                    f"Content-Type: {'text/plain' if code == 204 else 'application/json'}\r\nContent-Length: {len(body)}\r\n"
                    "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, OPTIONS\r\n"
                    "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
                    f"{'Allow: GET, HEAD, OPTIONS' + chr(13) + chr(10) if code == 405 else ''}Connection: close\r\n\r\n")
            writer.write(head.encode() + (b"" if method == b"HEAD" else body))
            await asyncio.wait_for(writer.drain(), STATUS_TIME)
        except (asyncio.IncompleteReadError, asyncio.LimitOverrunError, asyncio.TimeoutError, ConnectionError, OSError):
            pass
        finally:
            self.status_open -= 1
            writer.close()

    # ---- housekeeping -------------------------------------------------------------------
    def sweep(self, now):
        for c in list(self.by_addr.values()):
            if now - c.seen > (self.o.client_timeout if c.verified else self.o.pending_timeout):
                self.drop(c, "timed out")
        for listing in list(self.listings.values()):
            if now - listing.updated > self.o.listing_timeout:
                self.close(listing)
        self.match_sweep(now)
        for host, b in list(self.gates.items()):
            if now - b.at > 60:
                del self.gates[host]
        if now - self.ban_checked >= 5:
            self.load_banlist(now)
        self.flush(now)

    def report(self, secs):
        c, n = self.counts, len(self.by_addr)
        drops = ", ".join(f"{k} {v}" for k, v in sorted(self.drops.items())) or "none"
        paths = sum(p.introduced_count for p in self.by_addr.values())
        direct = sum(min(p.direct_count, p.introduced_count) for p in self.by_addr.values())
        su = sum(1 for p in self.by_addr.values() if p.wire is self.su)
        v3 = sum(1 for p in self.by_addr.values() if p.wire is self.sn3)
        self.say(f"{n} player{'' if n == 1 else 's'} ({self.pending} connecting, {v3} on SN {V3}, {su} on SU) in {len(self.parts)} "
                 f"lobb{'y' if len(self.parts) == 1 else 'ies'}, {len(self.listings)} room{'' if len(self.listings) == 1 else 's'}, "
                 f"{sum(len(w) for w in self.searching.values())} searching, {len(self.matches)} matched; "
                 f"in {c['in packets'] / secs:.0f}/s {c['in bytes'] / secs / 1024:.1f} KB/s, "
                 f"out {c['out packets'] / secs:.0f}/s {c['out bytes'] / secs / 1024:.1f} KB/s; "
                 f"game relayed {c['game relayed'] / secs:.0f}/s, rollback {c['rollback relayed'] / secs:.0f}/s; "
                 f"direct paths {direct} of {paths}; "
                 f"largest {self.max_data} B; dropped: {drops}")
        self.counts.clear()
        self.drops.clear()
        self.max_data = 0

    async def run(self):
        self.load_banlist(time.monotonic())
        last_report = time.monotonic()
        while True:
            await asyncio.sleep(self.o.tick)
            now = time.monotonic()
            self.sweep(now)
            if self.o.stats and now - last_report >= self.o.stats:
                self.report(now - last_report)
                last_report = now


async def serve(o, make=None):
    """A lobby (make(o): Lobby's own by default) on its ports: (SN's transport, the lobby)."""
    loop = asyncio.get_running_loop()
    lobby = (make or Lobby)(o)
    try:
        transport, _ = await loop.create_datagram_endpoint(lambda: Endpoint(lobby, lobby.sn), local_addr=(o.bind, o.port))
        if o.su_port is not None:
            await loop.create_datagram_endpoint(lambda: Endpoint(lobby, lobby.su), local_addr=(o.bind, o.su_port))
        if o.status_port is not None:
            lobby.status_server = await asyncio.start_server(lobby.status, o.status_bind, o.status_port, limit=STATUS_REQUEST)
    except BaseException:
        lobby.close_ports()
        raise
    return transport, lobby


def say_more(o, lobby):
    """The start-up lines for SU's port and the status page, when they are served."""
    if lobby.su.transport is not None:
        Lobby.say(f"and for ssbu-recomp's games on UDP {o.bind}:{lobby.su.transport.get_extra_info('sockname')[1]} "
                  f"(protocol SU {SU_PROTOCOL})")
    if lobby.status_server is not None:
        Lobby.say("status page on http://%s:%d/status" % lobby.status_server.sockets[0].getsockname()[:2])


async def main(o):
    transport, lobby = await serve(o)
    Lobby.say(f"SSBU netplay lobby on UDP {o.bind}:{transport.get_extra_info('sockname')[1]} (protocol {PROTOCOL})")
    say_more(o, lobby)
    task = asyncio.ensure_future(lobby.run())
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, task.cancel)
        except (NotImplementedError, AttributeError):   # (Windows: Ctrl+C raises KeyboardInterrupt)
            pass
    try:
        await task
    except asyncio.CancelledError:
        pass
    finally:
        lobby.close_ports()
        Lobby.say("stopped")


def probe(target, room, name, build, wait=3.0):
    """Join a lobby as a player would, print what it says, and leave."""
    host, _, port = target.rpartition(":")
    if not host or not port.isdigit():
        host, port = target, str(DEFAULT_PORT)
    try:
        addr = (socket.gethostbyname(host), int(port))
    except OSError as e:
        print(f"cannot find {host}: {e}")
        return 1
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(0.5)
    nonce = secrets.token_bytes(8)

    def answer(kinds, until):
        while time.monotonic() < until:
            try:
                data, src = s.recvfrom(65536)
            except (socket.timeout, ConnectionResetError):   # (Windows: "port unreachable" came back)
                return None
            if src == addr and data[:2] == b"SN" and data[3] in kinds:
                return data
        return None

    join = (header(JOIN) + nonce + room.encode()[:NAME_LEN].ljust(NAME_LEN, b"\0") +
            name.encode()[:NAME_LEN].ljust(NAME_LEN, b"\0") + build.encode()[:BUILD_LEN].ljust(BUILD_LEN, b"\0"))
    start = time.monotonic()
    welcome = None
    while welcome is None and time.monotonic() - start < wait:
        s.sendto(join, addr)
        got = answer((WELCOME, REJECT), time.monotonic() + 0.5)
        if got and got[3] == REJECT and got[4:12] == nonce:
            print(f"{target}: refused: {text(got[13:77])}")
            return 1
        if got and got[3] == WELCOME and got[4:12] == nonce and len(got) >= 25:
            welcome = got
    if welcome is None:
        print(f"{target} ({addr[0]}:{addr[1]}): no answer")
        return 1
    ip, cookie, flags = struct.unpack_from(">IQB", welcome, 12)
    sent = time.monotonic()
    s.sendto(header(PING) + nonce + struct.pack(">QQH", int(sent * 1000), cookie, 0xFFFF), addr)
    pong = answer((PONG,), time.monotonic() + wait)
    rtt = (time.monotonic() - sent) * 1000
    s.sendto(header(LIST) + struct.pack(">QI", cookie, 1), addr)
    rooms, until = [], time.monotonic() + wait
    while True:
        got = answer((LISTING, LIST_END), until)
        if got is None or got[3] == LIST_END:
            break
        if len(got) >= 12 + NI_SIZE:
            rooms.append(got[12:12 + NI_SIZE])
    s.sendto(header(LEAVE) + nonce, addr)
    if pong is None or len(pong) < 17:
        print(f"{target}: joined as {ip_text(ip)}, but no answer to a ping")
        return 1
    online, nrooms, flags = struct.unpack_from(">HHB", pong, 12)
    print(f"{target} ({addr[0]}:{addr[1]}): {'lobby' if flags & 1 else 'relay'} protocol {PROTOCOL}"
          f"{', direct connections' if welcome[24] & 2 else ''}, "
          f"ping {rtt:.0f} ms; lobby {room!r}, build {build or '(none)'}: {online} player{'' if online == 1 else 's'} "
          f"(this probe included), {nrooms} room{'' if nrooms == 1 else 's'}")
    for info in rooms:
        print(f"  room of {node_name(info)}: {info[NI_NODE_COUNT]}/{info[NI_NODE_MAX]} players"
              f"{'' if info[NI_POLICY] != 1 and info[NI_NODE_COUNT] < info[NI_NODE_MAX] else ' (closed)'}")
    return 0


def parse_args(argv=None, **defaults):
    d = options(**defaults)
    a = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    a.add_argument("--port", type=int, default=int(os.environ.get("SSBU_NET_PORT", d.port)), help="UDP port (default 39420)")
    a.add_argument("--su-port", type=int, default=d.su_port or 0,
                   help=f"UDP port for ssbu-recomp's games (protocol SU {SU_PROTOCOL}; 0: none; default %(default)s)")
    a.add_argument("--bind", default=d.bind, help="the address to listen on (default: all)")
    a.add_argument("--verbose", action="store_true", help="log players and rooms coming and going")
    a.add_argument("--banlist", help="a file of IP addresses or networks (one a line, # comments) to turn away; "
                                     "re-read when it changes")
    a.add_argument("--max-clients", type=int, default=d.max_clients, help="players at most (default %(default)s)")
    a.add_argument("--per-ip", type=int, default=d.per_ip, help="players at most from one IP address (default %(default)s)")
    a.add_argument("--packet-rate", type=float, default=d.packet_rate, help="a player's packets a second (default %(default)s)")
    a.add_argument("--byte-rate", type=float, default=d.byte_rate, help="a player's bytes a second (default %(default)s)")
    a.add_argument("--max-data", type=int, default=d.max_data, help="largest game datagram (default %(default)s)")
    a.add_argument("--su-max-data", type=int, default=d.su_max_data,
                   help="largest game datagram on SU: its LDN messages may carry a room's NetworkInfo "
                        "(ssbu-recomp's lobby took that much; default %(default)s)")
    a.add_argument("--search-secs", dest="search_for", type=float, default=d.search_for,
                   help="matchmaking: a search without an opponent ends after this (default %(default)s)")
    a.add_argument("--match-secs", dest="match_for", type=float, default=d.match_for,
                   help="matchmaking: two players matched and not together by then are told it expired (default %(default)s)")
    a.add_argument("--stats", type=float, default=d.stats, help="seconds between statistics lines (0: none; default %(default)s)")
    a.add_argument("--status-port", type=int, default=d.status_port or 0,
                   help="TCP port of the status page, GET /status: counts only (0: none; default %(default)s)")
    a.add_argument("--status-bind", default=d.status_bind,
                   help="the status page's address (default %(default)s: behind a TLS proxy on this machine)")
    a.add_argument("--probe", metavar="HOST[:PORT]", help="instead of serving: check the lobby at this address")
    a.add_argument("--room", default="smash", help="(--probe) the lobby code (default: the public lobby)")
    a.add_argument("--build", default=REQUIRED_BUILD.decode(), help="(--probe) the game build id to join with "
                   "(default: 13.0.5's, the one the lobby lets in)")
    args = a.parse_args(argv)
    for k, v in vars(args).items():
        setattr(d, k, v)
    d.su_port, d.status_port = d.su_port or None, d.status_port or None
    return d


if __name__ == "__main__":
    o = parse_args(su_port=SU_PORT)
    if o.probe:
        sys.exit(probe(o.probe, o.room, "probe", o.build))
    try:
        asyncio.run(main(o))
    except KeyboardInterrupt:
        pass
