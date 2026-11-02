"""tools/nsz.py: compressed packages (.nsz) made plain again, on made-up packages with no game data:
an NCA with an AES-CTR section and a plain one, compressed as NCZ in both forms (one zstd stream,
and NCZBLOCK blocks, one of them stored), next to a ticket; the plain package must hold the very
NCA and ticket, and plain_nsp must make its copy once and again only when the .nsz changes."""
from pathlib import Path
import os
import struct
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import nsz  # noqa: E402
from Cryptodome.Cipher import AES  # noqa: E402
from Cryptodome.Util import Counter  # noqa: E402

try:
    from compression.zstd import compress as zstd_compress
except ImportError:
    import zstandard

    def zstd_compress(data):
        return zstandard.ZstdCompressor().compress(data)

KEY, CTR = bytes(range(16)), bytes(range(16, 32))
BODY = 0x30000                                   # the NCA's body, from 0x4000
SECS = [(0x4000, 0x10000, 1, bytes(16), bytes(16)),          # plain
        (0x14000, BODY - 0x10000, 3, KEY, CTR)]               # AES-CTR


def plain_body():
    return bytes((i * 7 + (i >> 9)) & 0xFF for i in range(BODY))


def nca_bytes():
    """The NCA as it is: a header, then the body with the second section encrypted."""
    head = b'NCA3' + bytes(0x4000 - 4)
    body = bytearray(plain_body())
    lo = SECS[1][0] - 0x4000
    c = Counter.new(64, prefix=CTR[:8], initial_value=SECS[1][0] >> 4)
    body[lo:] = AES.new(KEY, AES.MODE_CTR, counter=c).encrypt(bytes(body[lo:]))
    return head + bytes(body)


def ncz_bytes(blocks):
    out = b'NCA3' + bytes(0x4000 - 4) + struct.pack('<8sQ', b'NCZSECTN', len(SECS))
    for o, s, crypto, key, ctr in SECS:
        out += struct.pack('<QQQQ', o, s, crypto, 0) + key + ctr
    body = plain_body()
    if not blocks:
        return out + zstd_compress(body)
    exp = 16
    parts = [body[i:i + (1 << exp)] for i in range(0, len(body), 1 << exp)]
    packed = [p if i == 1 else zstd_compress(p) for i, p in enumerate(parts)]   # (block 1 stored)
    out += b'NCZBLOCK' + struct.pack('<BBBBIQ', 2, 1, 0, exp, len(parts), len(body))
    out += struct.pack('<%dI' % len(parts), *[len(p) for p in packed])
    return out + b''.join(packed)


def pfs0(entries):
    strings = b''.join(n.encode() + b'\0' for n, _ in entries)
    strings += b'\0' * ((-(16 + 24 * len(entries) + len(strings))) % 0x20)
    table, at, name_off = b'', 0, 0
    for n, data in entries:
        table += struct.pack('<QQII', at, len(data), name_off, 0)
        at += len(data)
        name_off += len(n) + 1
    return struct.pack('<4sIII', b'PFS0', len(entries), len(strings), 0) + table + strings + b''.join(d for _, d in entries)


def entries_of(path):
    with open(path, 'rb') as f:
        out = {}
        for name, off, size in nsz.pfs0(f):
            f.seek(off)
            out[name] = f.read(size)
        return out


class NszTests(unittest.TestCase):
    def test_both_forms(self):
        tik = bytes(range(256)) * 2
        with tempfile.TemporaryDirectory(prefix='nsztest ') as d:
            for blocks in (False, True):
                src = os.path.join(d, 'game%d.nsz' % blocks)
                with open(src, 'wb') as f:
                    f.write(pfs0([('abcd.ncz', ncz_bytes(blocks)), ('t.tik', tik)]))
                out = os.path.join(d, 'game%d.nsp' % blocks)
                nsz.convert(src, out, log=lambda line: None)
                got = entries_of(out)
                self.assertEqual(sorted(got), ['abcd.nca', 't.tik'], blocks)
                self.assertEqual(got['abcd.nca'], nca_bytes(), 'the NCA, %s' % ('blocks' if blocks else 'one stream'))
                self.assertEqual(got['t.tik'], tik)

    def test_plain_copy_made_once(self):
        with tempfile.TemporaryDirectory(prefix='nsztest ') as d:
            src = os.path.join(d, 'pkg.nsz')
            with open(src, 'wb') as f:
                f.write(pfs0([('abcd.ncz', ncz_bytes(True))]))
            said = []
            cache = os.path.join(d, 'cache')
            first = nsz.plain_nsp(src, cache, log=said.append)
            made = len(said)
            again = nsz.plain_nsp(src, cache, log=said.append)
            self.assertEqual(first, again)
            self.assertEqual(len(said), made, 'not made again for the same .nsz')
            t = time.time() + 10
            os.utime(src, (t, t))
            nsz.plain_nsp(src, cache, log=said.append)
            self.assertGreater(len(said), made, 'made again once the .nsz changed')


if __name__ == '__main__':
    unittest.main()
