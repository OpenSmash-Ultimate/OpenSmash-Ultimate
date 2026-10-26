"""Lua constant names of the script modules.

Fighter scripts read numeric constants (FIGHTER_STATUS_KIND_*, ATTACK_REGION_*, ...) from
lua2cpp::L2CAgentGeneratedBase::const_value_table__ (in lua2cpp_common: 19,469 ints). That table is filled at
start-up by construct_bind_const_value_table from an array of 64-bit keys (common +0x6a5b70), each looked up
with lib::lua_bind_get_value<int>. The key is XXH64(name, seed 0xFFFFFFFFFFFFFFFF) (read off main 0x710399f0b0;
public algorithm; 19,308 of the 19,469 keys get a name this way). Names: reference/skyline-smash/src/lua_const.rs (public bindings).

    table_names()  ->  list: index in const_value_table__ -> (name or None, value from lua_const.rs or None)
"""
import re

import sdk_paths
from nrolib import Nro

M = (1 << 64) - 1
P1, P2, P3, P4, P5 = 0x9E3779B185EBCA87, 0xC2B2AE3D27D4EB4F, 0x165667B19E3779F9, 0x85EBCA77C2B2AE63, 0x27D4EB2F165667C5

KEYS_OFF = 0x6a5b70
COUNT = 0x4c0d


def _rotl(v, n):
    return ((v << n) | (v >> (64 - n))) & M


def _round(acc, inp):
    return (_rotl((acc + inp * P2) & M, 31) * P1) & M


def xxh64(data, seed=0):
    if isinstance(data, str):
        data = data.encode()
    n = len(data)
    i = 0
    if n >= 32:
        v = [(seed + P1 + P2) & M, (seed + P2) & M, seed, (seed - P1) & M]
        while i + 32 <= n:
            for k in range(4):
                v[k] = _round(v[k], int.from_bytes(data[i + 8 * k:i + 8 * k + 8], 'little'))
            i += 32
        h = (_rotl(v[0], 1) + _rotl(v[1], 7) + _rotl(v[2], 12) + _rotl(v[3], 18)) & M
        for k in range(4):
            h = ((h ^ _round(0, v[k])) * P1 + P4) & M
    else:
        h = (seed + P5) & M
    h = (h + n) & M
    while i + 8 <= n:
        h ^= _round(0, int.from_bytes(data[i:i + 8], 'little'))
        h = (_rotl(h, 27) * P1 + P4) & M
        i += 8
    if i + 4 <= n:
        h ^= (int.from_bytes(data[i:i + 4], 'little') * P1) & M
        h = (_rotl(h, 23) * P2 + P3) & M
        i += 4
    while i < n:
        h ^= (data[i] * P5) & M
        h = (_rotl(h, 11) * P1) & M
        i += 1
    h ^= h >> 33
    h = (h * P2) & M
    h ^= h >> 29
    h = (h * P3) & M
    h ^= h >> 32
    return h


def lua_const_names():
    with open(sdk_paths.reference('skyline-smash', 'src', 'lua_const.rs'), encoding='utf-8') as f:
        text = f.read()
    out = {}
    for nm, v in re.findall(r'^\s+([A-Za-z0-9_]+): (-?0x[0-9A-Fa-f]+|-?\d+),', text, re.M):
        out.setdefault(nm, int(v, 0))
    return out


_cache = None


def table_names():
    global _cache
    if _cache is None:
        n = Nro('common')
        by_hash = {xxh64(nm, M): (nm, v) for nm, v in lua_const_names().items()}
        _cache = [by_hash.get(n.u64(KEYS_OFF + 8 * i), (None, None)) for i in range(COUNT)]
    return _cache


if __name__ == '__main__':
    t = table_names()
    print('named %d of %d table entries' % (sum(1 for a, b in t if a), len(t)))
    for off in (0xb990, 0xe734, 0x4b54, 0xe738, 0x34f4):
        print(hex(off), t[off // 4])
