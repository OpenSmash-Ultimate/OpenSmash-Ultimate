"""The online lobby's wire protocol (SN, versions 2 and 3) as the game's netplay client speaks it (runtime/net.h,
runtime/net_relay.c), for the tests that play a lobby by hand, plus where the lobby server itself is.

The lobby server (ssbu_relay.py, ssbu_online_lobby.py and the game server, nex/) is not part of this repository.
Tests that run a game's client against the real lobby, or compare the in-game NEX server with it, need a checkout
of it: OPENSMASH_SERVER_DIR=<its folder>, or a folder opensmash-server next to this repository. Without one they
are skipped; everything else here runs on its own."""
from pathlib import Path
import os
import struct
import sys
import time
import unittest

ROOT = Path(__file__).resolve().parent.parent
SERVER_DIR = Path(os.environ.get('OPENSMASH_SERVER_DIR') or ROOT.parent / 'opensmash-server')
HAVE_LOBBY = (SERVER_DIR / 'tools' / 'ssbu_relay.py').is_file()
NO_LOBBY = 'needs the lobby server (OPENSMASH_SERVER_DIR, tests/netproto.py)'
if HAVE_LOBBY:
    for _p in (SERVER_DIR / 'tests', SERVER_DIR / 'tools'):
        if str(_p) not in sys.path:
            sys.path.insert(0, str(_p))


def need_lobby():
    """Skips the calling test (or module, at import) without a checkout of the lobby server."""
    if not HAVE_LOBBY:
        raise unittest.SkipTest(NO_LOBBY)


# ---- the protocol (version 2, and 3 on the same port) ----
PROTOCOL = 2
JOIN, WELCOME, REJECT, PING, PONG, DATA, MEMBERS, LEAVE, LIST, LISTING, LIST_END = range(1, 12)
PEERS, BCAST, PUNCH, DIRECT = range(12, 16)
NAME_LEN = 32
BUILD_LEN = 40
JOIN_SIZE = 4 + 8 + 2 * NAME_LEN + BUILD_LEN
V1_JOIN_SIZE = 4 + 8 + 2 * NAME_LEN
PEER = struct.Struct(">I32sBIHIHQ")   # a PEERS entry
SUBNET = 0x0A0D0000
BROADCASTS = (0x0A0DFFFF, 0xFFFFFFFF)
V3 = 3
RB, ROOMS, ROOM_INFO, ROOMS_END, MATCH, MATCH_STATE = range(16, 22)
V3_JOIN_SIZE = JOIN_SIZE + 4 + 1
LOBBY, INTRODUCES, HAS_ROOMS, HAS_MATCH, HAS_RB = 1, 2, 4, 8, 16   # WELCOME's flags
MS_IDLE, MS_SEARCHING, MS_MATCHED, MS_TIMEOUT, MS_PEER_LEFT, MS_TOGETHER, MS_EXPIRED = range(7)

# Local wireless's own messages, carried in DATA between port 65511s: 'L', type, 0, 0, payload
LDN_PORT = 65511
LM_ADVERTISE, LM_JOIN, LM_ACCEPT, LM_REJECT, LM_KEEP, LM_LEAVE, LM_GONE, LM_KICK = range(1, 9)
# a network's NetworkInfo, as the console lays it out (little-endian)
NI_SIZE = 0x480
NI_SESSION = 0x10
NI_POLICY = 0x62
NI_NODE_MAX = 0x66
NI_NODE_COUNT = 0x67
NI_NODES = 0x68
NODE_SIZE = 0x40

BUILD = "21450C647B8C5940ED5B68218EA128FD3C5D597D"         # (13.0.5's: the one the lobby lets in)
OTHER_BUILD = "4A3F8ED7DCCE3AD4541A3A2F7622B5B57FC8A55C"   # (11.0.0's)


def reject_detail(build, sim=0, mods=None):
    """A REJECT's detail: 'W' 'D', build[40], sim u32, flags u8 (1: sim and the mod set known), mod set."""
    known = mods is not None
    return (b"WD" + bytes(build)[:BUILD_LEN].ljust(BUILD_LEN, b"\0") + struct.pack(">IB", sim if known else 0, 1 if known else 0)
            + (bytes(mods) if known else b""))


def mods(*entries):
    """A mod set: (name, hash), ..."""
    return bytes((len(entries),)) + b"".join(struct.pack(">IB", h, len(n)) + n for n, h in entries)


def until(cond, wait=3.0):
    """Whether cond() comes true within wait seconds (looked at every 20 ms)."""
    end = time.monotonic() + wait
    while not cond():
        if time.monotonic() >= end:
            return False
        time.sleep(0.02)
    return True


def info_of(session, owner, stations=(), most=8, policy=0, name=b"Host"):
    """A NetworkInfo of a room: its session id, owner and stations."""
    info = bytearray(NI_SIZE)
    info[NI_SESSION:NI_SESSION + 16] = session
    info[NI_POLICY] = policy
    info[NI_NODE_MAX] = most
    ips = [owner, *stations]
    info[NI_NODE_COUNT] = len(ips)
    for i, ip in enumerate(ips):
        o = NI_NODES + NODE_SIZE * i
        struct.pack_into("<I", info, o, ip)
        info[o + 0xA] = i
        info[o + 0xB] = 1
        info[o + 0xC:o + 0xC + len(name)] = name if i == 0 else b"Guest"
    return bytes(info)
