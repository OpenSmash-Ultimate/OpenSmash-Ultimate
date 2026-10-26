"""CityHash64 as used by libc++'s std::hash<std::string> (public algorithm, libc++ __murmur2_or_cityhash<size_t, 64>).

libc++'s differs from Google's CityHash in two places, kept here: for 4 to 8 bytes the first word is shifted
as a 32-bit value (libc++'s stable ABI: _LIBCPP_ABI_FIX_CITYHASH_IMPLEMENTATION is off), and for 33 to 64
bytes the last word is added to z instead of replacing it."""
M = (1 << 64) - 1
K0 = 0xc3a5c85c97cb3127
K1 = 0xb492b66fbe98f273
K2 = 0x9ae16a3b2f90404f
K3 = 0xc949d7c7509e6557


def _u64(s, i):
    return int.from_bytes(s[i:i + 8], 'little')


def _u32(s, i):
    return int.from_bytes(s[i:i + 4], 'little')


def _rot(v, n):
    return v if n == 0 else ((v >> n) | (v << (64 - n))) & M


def _rot1(v, n):
    return ((v >> n) | (v << (64 - n))) & M


def _smix(v):
    return v ^ (v >> 47)


def _h16(u, v):
    mul = 0x9ddfea08eb382d69
    a = ((u ^ v) * mul) & M
    a ^= a >> 47
    b = ((v ^ a) * mul) & M
    b ^= b >> 47
    return (b * mul) & M


def _len0_16(s):
    n = len(s)
    if n > 8:
        a = _u64(s, 0)
        b = _u64(s, n - 8)
        return _h16(a, _rot1((b + n) & M, n)) ^ b
    if n >= 4:
        a = _u32(s, 0)
        b = _u32(s, n - 4)
        return _h16((n + ((a << 3) & 0xffffffff)) & M, b)
    if n > 0:
        a, b, c = s[0], s[n >> 1], s[n - 1]
        y = a + (b << 8)
        z = n + (c << 2)
        return (_smix(((y * K2) & M) ^ ((z * K3) & M)) * K2) & M
    return K2


def _len17_32(s):
    n = len(s)
    a = (_u64(s, 0) * K1) & M
    b = _u64(s, 8)
    c = (_u64(s, n - 8) * K2) & M
    d = (_u64(s, n - 16) * K0) & M
    return _h16((_rot((a - b) & M, 43) + _rot(c, 30) + d) & M, (a + _rot(b ^ K3, 20) - c + n) & M)


def _weak(w, x, y, z, a, b):
    a = (a + w) & M
    b = _rot((b + a + z) & M, 21)
    c = a
    a = (a + x) & M
    a = (a + y) & M
    b = (b + _rot(a, 44)) & M
    return (a + z) & M, (b + c) & M


def _weak_s(s, i, a, b):
    return _weak(_u64(s, i), _u64(s, i + 8), _u64(s, i + 16), _u64(s, i + 24), a, b)


def _len33_64(s):
    n = len(s)
    z = _u64(s, 24)
    a = (_u64(s, 0) + (n + _u64(s, n - 16)) * K0) & M
    b = _rot((a + z) & M, 52)
    c = _rot(a, 37)
    a = (a + _u64(s, 8)) & M
    c = (c + _rot(a, 7)) & M
    a = (a + _u64(s, 16)) & M
    vf = (a + z) & M
    vs = (b + _rot(a, 31) + c) & M
    a = (_u64(s, 16) + _u64(s, n - 32)) & M
    z = (z + _u64(s, n - 8)) & M
    b = _rot((a + z) & M, 52)
    c = _rot(a, 37)
    a = (a + _u64(s, n - 24)) & M
    c = (c + _rot(a, 7)) & M
    a = (a + _u64(s, n - 16)) & M
    wf = (a + z) & M
    ws = (b + _rot(a, 31) + c) & M
    r = _smix(((vf + ws) * K2 + (wf + vs) * K0) & M)
    return (_smix((r * K0 + vs) & M) * K2) & M


def cityhash64(s):
    if isinstance(s, str):
        s = s.encode()
    n = len(s)
    if n <= 32:
        return _len0_16(s) if n <= 16 else _len17_32(s)
    if n <= 64:
        return _len33_64(s)
    x = _u64(s, n - 40)
    y = (_u64(s, n - 16) + _u64(s, n - 56)) & M
    z = _h16((_u64(s, n - 48) + n) & M, _u64(s, n - 24))
    v = _weak_s(s, n - 64, n, z)
    w = _weak_s(s, n - 32, (y + K1) & M, x)
    x = (x * K1 + _u64(s, 0)) & M
    i = 0
    n = (n - 1) & ~63
    while True:
        x = (_rot((x + y + v[0] + _u64(s, i + 8)) & M, 37) * K1) & M
        y = (_rot((y + v[1] + _u64(s, i + 48)) & M, 42) * K1) & M
        x ^= w[1]
        y = (y + v[0] + _u64(s, i + 40)) & M
        z = (_rot((z + w[0]) & M, 33) * K1) & M
        v = _weak_s(s, i, (v[1] * K1) & M, (x + w[0]) & M)
        w = _weak_s(s, i + 32, (z + w[1]) & M, (y + _u64(s, i + 16)) & M)
        z, x = x, z
        i += 64
        n -= 64
        if n == 0:
            break
    return _h16((_h16(v[0], w[0]) + _smix(y) * K1 + z) & M, (_h16(v[1], w[1]) + x) & M)
