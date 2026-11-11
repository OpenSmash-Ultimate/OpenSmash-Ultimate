"""The firmware font step (ssbu-recomp/tools/extract/fw_fonts.py) on made-up firmware folders: NCA headers
encrypted with a made-up header key, laid out as Goldleaf (<hash>.nca, <hash>.cnmt.nca), as upper-case names and as
Ryujinx folders. A firmware newer than the keys (issue #164: 23.0.1) must name the font title and the key that is
missing instead of finding nothing in silence; other cases give a clear sentence too. No game or firmware files."""
from pathlib import Path
import os
import struct
import sys
import tempfile
import unittest

from Crypto.Cipher import AES

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'ssbu-recomp' / 'tools' / 'extract'))
import romfs_extract as rx  # noqa: E402
import fw_fonts  # noqa: E402

HEADER_KEY = bytes(range(32))
KAEK_KEY = bytes(range(100, 116))


def xts_encrypt(key, data, sector=0, size=0x200):
    k1, k2 = AES.new(key[:16], AES.MODE_ECB), AES.new(key[16:], AES.MODE_ECB)
    out = bytearray()
    for s in range(len(data) // size):
        tweak = int.from_bytes(k2.encrypt((sector + s).to_bytes(16, 'big')), 'little')
        for b in range(0, size, 16):
            block = data[s * size + b:s * size + b + 16]
            t = tweak.to_bytes(16, 'little')
            out += bytes(x ^ y for x, y in zip(k1.encrypt(bytes(x ^ y for x, y in zip(block, t))), t))
            tweak <<= 1
            if tweak >> 128:
                tweak = (tweak & ((1 << 128) - 1)) ^ 0x87
    return bytes(out)


def nca_bytes(title, key_gen_byte, magic=b'NCA3'):
    h = bytearray(0xC00)
    h[0x200:0x204] = magic
    h[0x205] = 2                                  # content type: data
    h[0x206] = key_gen_byte
    h[0x207] = 0                                  # key area: application
    h[0x210:0x218] = struct.pack('<Q', title)
    return xts_encrypt(HEADER_KEY, bytes(h)) + bytes(0x200)


class FwFonts(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.fw = os.path.join(self.tmp.name, 'fw')
        self.out = os.path.join(self.tmp.name, 'out')
        os.makedirs(self.fw)
        self.saved = dict(rx.KEYS)
        rx.KEYS.clear()
        rx.KEYS['header_key'] = HEADER_KEY
        rx.KEYS['key_area_key_application_00'] = KAEK_KEY

    def tearDown(self):
        rx.KEYS.clear()
        rx.KEYS.update(self.saved)
        self.tmp.cleanup()

    def put(self, name, data):
        with open(os.path.join(self.fw, name), 'wb') as f:
            f.write(data)

    def run_step(self):
        lines = []
        found, problems = fw_fonts.extract(self.fw, self.out, say=lines.append)
        return found, problems, lines

    def test_header_reads_title_and_key_name(self):
        self.put('a.nca', nca_bytes(0x0100000000000811, 0x17))        # key generation 0x16 (firmware 23 era)
        n = fw_fonts.LooseNca(os.path.join(self.fw, 'a.nca'))
        self.assertTrue(n.ok)
        self.assertEqual(n.title_id, 0x0100000000000811)
        self.assertEqual(n.key_name, 'key_area_key_application_16')
        self.assertIsNone(n.key)

    def test_firmware_newer_than_keys_names_title_and_key(self):
        self.put('0123abcd.nca', nca_bytes(0x0100000000000810, 0x17))
        self.put('0123abcd.cnmt.nca', nca_bytes(0x0100000000000810, 0x17))
        self.put('FFFF0000.NCA', nca_bytes(0x0100000000000814, 0x17))
        found, problems, _ = self.run_step()
        self.assertEqual(found, 0)
        self.assertEqual(len(problems), 2)                            # the .cnmt.nca is not a font
        for p in problems:
            self.assertIn('key_area_key_application_16', p)
            self.assertIn('newer than your keys', p)
        self.assertIn('0100000000000810', problems[0])
        self.assertIn('0100000000000814', problems[1])

    def test_old_generation_with_key_is_not_a_key_problem(self):
        self.put('x.nca', nca_bytes(0x0100000000000812, 0x01))        # generation 0: key present, no RomFS section
        found, problems, _ = self.run_step()
        self.assertEqual(found, 0)
        self.assertFalse(any('newer than your keys' in p for p in problems))
        self.assertTrue(any('no font title' in p for p in problems))  # nothing readable: tells to check the folder

    def test_not_a_firmware_folder(self):
        self.put('readme.txt', b'x')
        self.put('other.nca', nca_bytes(0x0100000000001234, 0x01))   # a game NCA, no font title
        found, problems, _ = self.run_step()
        self.assertEqual(found, 0)
        self.assertEqual(len(problems), 1)
        self.assertIn('0100000000000810', problems[0])
        self.assertIn('firmware folder', problems[0])

    def test_ryujinx_folder_layout_is_looked_into(self):
        d = os.path.join(self.fw, 'abcd.nca')
        os.makedirs(d)
        with open(os.path.join(d, '00'), 'wb') as f:
            f.write(nca_bytes(0x0100000000000813, 0x17))
        found, problems, _ = self.run_step()
        self.assertEqual(found, 0)
        self.assertIn('0100000000000813', problems[0])

    def test_no_header_key_is_a_sentence_not_a_crash(self):
        rx.KEYS.clear()
        self.put('a.nca', nca_bytes(0x0100000000000811, 0x01))
        found, problems, _ = self.run_step()
        self.assertEqual(found, 0)
        self.assertIn('prod.keys', problems[0])


if __name__ == '__main__':
    unittest.main()
