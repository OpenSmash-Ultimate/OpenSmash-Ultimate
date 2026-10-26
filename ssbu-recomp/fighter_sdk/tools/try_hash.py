"""Which 64-bit hash does lib::lua_bind_get_value use for constant names? Try the usual ones against the key table."""
import re
import zlib

import sdk_paths
from nrolib import Nro

n = Nro('common')
keys = set(n.u64(0x6a5b70 + 8 * i) for i in range(0x4c0d))
with open(sdk_paths.reference('skyline-smash', 'src', 'lua_const.rs'), encoding='utf-8') as f:
    names = re.findall(r'^\s+([A-Z0-9_a-z]+): (0x[0-9A-Fa-f]+|-?\d+),', f.read(), re.M)
print(len(keys), 'keys', len(names), 'names')
M = (1 << 64) - 1


def fnv1a(s):
    h = 0xcbf29ce484222325
    for c in s.encode():
        h = ((h ^ c) * 0x100000001b3) & M
    return h


def fnv1(s):
    h = 0xcbf29ce484222325
    for c in s.encode():
        h = ((h * 0x100000001b3) & M) ^ c
    return h


def murmur2_64a(s, seed=0):
    data = s.encode()
    m = 0xc6a4a7935bd1e995
    r = 47
    h = (seed ^ (len(data) * m)) & M
    nb = len(data) // 8
    for i in range(nb):
        k = int.from_bytes(data[i * 8:i * 8 + 8], 'little')
        k = (k * m) & M
        k ^= k >> r
        k = (k * m) & M
        h ^= k
        h = (h * m) & M
    tail = data[nb * 8:]
    if tail:
        h ^= int.from_bytes(tail, 'little')
        h = (h * m) & M
    h ^= h >> r
    h = (h * m) & M
    h ^= h >> r
    return h


def djb2(s):
    h = 5381
    for c in s.encode():
        h = (h * 33 + c) & M
    return h


cands = {'fnv1a': fnv1a, 'fnv1': fnv1, 'murmur2_64a seed0': murmur2_64a, 'djb2': djb2,
         'murmur2 seed c70f6907': lambda s: murmur2_64a(s, 0xc70f6907),
         'crc|len': lambda s: zlib.crc32(s.encode()) | (len(s) << 32)}
for label, fn in cands.items():
    for variant, tr in (('as is', lambda s: s), ('lower', str.lower)):
        hit = sum(1 for nm, v in names[:3000] if fn(tr(nm)) in keys)
        print('%-24s %-6s %d / 3000' % (label, variant, hit))
