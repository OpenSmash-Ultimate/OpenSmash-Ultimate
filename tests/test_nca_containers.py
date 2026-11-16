"""tools/nca.py's packages: the NCAs inside an .nsp (PFS0) and an .xci (HFS0), with made-up files,
and an NCA under a title key: read with its NSP's ticket, then decrypted and read without it."""
from pathlib import Path
import os
import struct
import subprocess
import sys
import tempfile
import unittest

from Cryptodome.Cipher import AES

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import nca  # noqa: E402


def partition(files, hfs):
    """A PFS0 (0x18-byte entries) or HFS0 (0x40-byte entries) of (name, data)."""
    strings, entries, data = b'', b'', b''
    for name, body in files:
        entry = struct.pack('<QQI', len(data), len(body), len(strings))
        entries += entry + bytes((0x40 if hfs else 0x18) - len(entry))
        strings += name.encode() + b'\0'
        data += body
    head = (b'HFS0' if hfs else b'PFS0') + struct.pack('<II', len(files), len(strings)) + bytes(4)
    return head + entries + strings + data


def xci(secure_files, prefix=0x200):
    """A game card image: its header ("HEAD" at 0x100, the root partition's place at 0x130), then
    the root HFS0 with the update, normal and secure partitions."""
    root = partition([('update', partition([], True)), ('normal', partition([], True)),
                      ('secure', partition(secure_files, True))], True)
    head = bytearray(prefix)
    head[0x100:0x104] = b'HEAD'
    struct.pack_into('<Q', head, 0x130, prefix)
    return bytes(head) + root


def xts_encrypt(key32, data, sector_size=0x200):
    """nca.xts_decrypt's inverse (sectors numbered from 0)."""
    k1 = AES.new(key32[:16], AES.MODE_ECB)
    k2 = AES.new(key32[16:], AES.MODE_ECB)
    out = bytearray()
    for s in range(len(data) // sector_size):
        t = int.from_bytes(k2.encrypt(s.to_bytes(16, 'big')), 'little')
        for b in range(s * sector_size, (s + 1) * sector_size, 16):
            p = int.from_bytes(data[b:b + 16], 'little') ^ t
            out += (int.from_bytes(k1.encrypt(p.to_bytes(16, 'little')), 'little') ^ t).to_bytes(16, 'little')
            t <<= 1
            if t >> 128:
                t = (t & ((1 << 128) - 1)) ^ 0x87
    return bytes(out)


HEADER_KEY = bytes(range(32))
TITLEKEK = bytes(range(0x40, 0x50))
TITLE_KEY = bytes(range(0x80, 0x90))
RIGHTS_ID = bytes.fromhex('01006a800016f00c000000000000000b')


def title_key_nca(payload):
    """An NCA whose one RomFS section (AES-CTR) is under a title key, as an eShop dump has them."""
    start = 0xC00
    hdr = bytearray(0xC00)
    hdr[0x200:0x204] = b'NCA3'
    hdr[0x205] = 5                                     # PublicData
    struct.pack_into('<QQ', hdr, 0x208, start + len(payload), 0x01006A800016F00C)
    hdr[0x230:0x240] = RIGHTS_ID
    struct.pack_into('<II', hdr, 0x240, start // 0x200, (start + len(payload)) // 0x200)
    fsh = 0x400
    hdr[fsh + 0x02], hdr[fsh + 0x03], hdr[fsh + 0x04] = 0, 3, 3   # RomFS, IVFC, CTR
    struct.pack_into('<QQ', hdr, fsh + 0x18 + 5 * 0x18, 0, len(payload))
    ctr_hi = bytes.fromhex('0000000100000000')
    hdr[fsh + 0x140:fsh + 0x148] = bytes(reversed(ctr_hi))
    iv = int.from_bytes(ctr_hi + (start >> 4).to_bytes(8, 'big'), 'big')
    body = AES.new(TITLE_KEY, AES.MODE_CTR, nonce=b'', initial_value=iv).encrypt(payload)
    return xts_encrypt(HEADER_KEY, bytes(hdr)) + body


def common_ticket():
    """A common ticket (RSA-2048 signature: the body at 0x140) holding the encrypted title key."""
    t = bytearray(0x140 + 0x180)
    struct.pack_into('<I', t, 0, 0x10004)
    t[0x140 + 0x40:0x140 + 0x50] = AES.new(TITLEKEK, AES.MODE_ECB).encrypt(TITLE_KEY)
    t[0x140 + 0x160:0x140 + 0x170] = RIGHTS_ID
    return bytes(t)


class ContainerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='nca-containers-')
        self.addCleanup(self.temp.cleanup)
        self.dir = Path(self.temp.name)

    def write(self, name, data):
        path = self.dir / name
        path.write_bytes(data)
        return str(path)

    def read(self, path, off, size):
        with open(path, 'rb') as f:
            f.seek(off)
            return f.read(size)

    def test_xci_secure_partition(self):
        path = self.write('game [v0].xci', xci([('aa.nca', b'PROGRAM-NCA'), ('bb.cnmt.nca', b'META'), ('cert', b'x')]))
        self.assertEqual(nca.container_kind(path), 'xci')
        files = nca.xci_files(path)
        self.assertEqual([n for n, _o, _s in files], ['aa.nca', 'bb.cnmt.nca', 'cert'])
        name, off, size = files[0]
        self.assertEqual(self.read(path, off, size), b'PROGRAM-NCA')
        self.assertEqual([n for n, _o, _s in nca.container_files(path)], ['aa.nca', 'bb.cnmt.nca'])   # NCAs only
        self.assertEqual(nca.nsp_title_keys(path), {})   # a game card's NCAs need no tickets

    def test_nsp(self):
        path = self.write('update.nsp', partition([('cc.nca', b'UPDATE-PROGRAM'), ('dd.tik', b'')], False))
        self.assertEqual(nca.container_kind(path), 'nsp')
        files = nca.container_files(path)
        self.assertEqual([n for n, _o, _s in files], ['cc.nca'])
        self.assertEqual(self.read(path, files[0][1], files[0][2]), b'UPDATE-PROGRAM')

    def test_bare_nca_and_bad_xci(self):
        path = self.write('program.nca', bytes(0x400))
        self.assertEqual(nca.container_kind(path), 'nca')
        self.assertEqual(nca.container_files(path), [('program.nca', 0, 0x400)])
        no_secure = bytearray(xci([]))
        bad = self.write('bad.xci', bytes(no_secure).replace(b'secure', b'normaL'))
        with self.assertRaises(SystemExit):
            nca.xci_files(bad)

    def test_decrypted_copy_needs_no_title_key(self):
        # the game from an eShop .nsp: its title key is in the NSP's ticket, which the copy
        # `decrypt` writes leaves behind; step 6 (romfs-cfg) and update.py then read that copy
        keys = self.dir / 'keys'
        keys.mkdir()
        (keys / 'prod.keys').write_text(f'header_key = {HEADER_KEY.hex()}\ntitlekek_00 = {TITLEKEK.hex()}\n')
        payload = os.urandom(0x400)
        path = self.write('game [v0].nsp', partition([('aa.nca', title_key_nca(payload)), ('rights.tik', common_ticket())], False))
        (_name, packaged), = nca.nsp_ncas(path, str(keys))
        self.assertTrue(packaged.has_rights)
        self.assertEqual(packaged.section(0).read(0, len(payload)), payload)   # (with the ticket)
        copy = str(self.dir / 'base.nca')
        nca.decrypt_nca(packaged, copy)
        packaged.f.close()
        plain = nca.Nca(copy, str(keys))   # no title.keys, no ticket
        self.assertTrue(plain.plaintext)
        self.assertEqual(plain.section(0).read(0, len(payload)), payload)
        plain.f.close()
        cfg = self.dir / 'romfs_base.cfg'
        r = subprocess.run([sys.executable, str(ROOT / 'tools/nca.py'), 'romfs-cfg', copy, '--keys', str(keys), '--out', str(cfg)],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(cfg.read_text().splitlines()[:2], ['base.nca', str(0xC00)])
        image = nca.RomfsImage(str(cfg))
        try:
            self.assertEqual(image.read(0, len(payload)), payload)
        finally:
            image.close()


if __name__ == '__main__':
    unittest.main()
