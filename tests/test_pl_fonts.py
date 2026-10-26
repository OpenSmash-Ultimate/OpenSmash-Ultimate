"""The system's shared fonts (runtime/hle_pl.c) through tools/dev/pltest.c: the font area laid out
from made-up firmware fonts (.bfttf, each with a key of its own) and the open ones of
third_party/fonts_open, or made-up ones - the firmware's for every type, for none, for some, a
firmware file that is no font or does not fit - and what nn::pl tells the game of each type.
An open font comes with U+25A1 added to its character map; the rest of it, and every firmware
font, as they were. No game files. Built with the platform's compiler: clang-cl on Windows,
clang elsewhere (the Windows types from runtime/macos/include and runtime/linux/include);
skipped where there is none, and on hosts the platform layer is not written for (Linux on
x86-64 and macOS on Apple silicon only, as the game)."""
from pathlib import Path
import os
import platform
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import cc  # noqa: E402

if os.name == 'nt':
    HAVE_CLANG = os.path.exists(cc.CLANG) or shutil.which(cc.CLANG) is not None
else:
    HAVE_CLANG = shutil.which('clang') is not None and platform.machine() in (('arm64',) if sys.platform == 'darwin' else ('x86_64', 'AMD64'))

OPEN = ROOT / 'third_party' / 'fonts_open'
NAMES = ('nintendo_udsg-r_std_003', 'nintendo_udsg-r_org_zh-cn_003', 'nintendo_udsg-r_ext_zh-cn_003',
         'nintendo_udjxh-db_zh-tw_003', 'nintendo_udsg-r_ko_003', 'nintendo_ext_003')
TYPES = ('Standard', 'ChineseSimple', 'ChineseSimpleExtension', 'ChineseTraditional', 'Korean', 'NintendoExtension')
DECODED_MAGIC, FILE_KEY = 0x7F9A0218, 0x49621806
BOX = 0x25A1


# ---- TrueType, as much as the tests need ---------------------------------------------------
def sfnt(tables):
    """A TrueType font of {tag: bytes}, tables 4-byte aligned, in tag order."""
    tags = sorted(tables)
    head = struct.pack('>IHHHH', 0x00010000, len(tags), 16, 0, 0)
    at = 12 + 16 * len(tags)
    records, body = b'', b''
    for tag in tags:
        data = tables[tag] + b'\0' * (-len(tables[tag]) % 4)
        records += struct.pack('>4sIII', tag.encode(), checksum(data), at + len(body), len(tables[tag]))
        body += data
    return head + records + body


def checksum(data):
    data += b'\0' * (-len(data) % 4)
    return sum(struct.unpack('>%dI' % (len(data) // 4), data)) & 0xFFFFFFFF


def directory(font):
    """{tag: (checksum, offset, length)}."""
    n = struct.unpack('>H', font[4:6])[0]
    return {font[12 + 16 * i:16 + 16 * i].decode('latin-1'): struct.unpack('>III', font[16 + 16 * i:28 + 16 * i]) for i in range(n)}


def cmap_table(subtables):
    """A cmap table of [(platform, encoding, subtable bytes)]."""
    out, at = struct.pack('>HH', 0, len(subtables)), 4 + 8 * len(subtables)
    body = b''
    for p, e, data in subtables:
        out += struct.pack('>HHI', p, e, at + len(body))
        body += data
    return out + body


def format4(mapping, with_array=()):
    """A format 4 subtable: a segment for each character, those in `with_array` through glyphIdArray."""
    codes = sorted(mapping) + [0xFFFF]
    n = len(codes)
    ends = starts = struct.pack('>%dH' % n, *codes)
    deltas = struct.pack('>%dh' % n, *[0 if c in with_array else ((mapping.get(c, 0) - c + 0x8000) % 0x10000) - 0x8000 for c in codes])
    array = [mapping[c] for c in codes if c in with_array]
    ranges, k = [], 0
    for i, c in enumerate(codes):
        if c in with_array:
            ranges.append(2 * (n - i) + 2 * k)
            k += 1
        else:
            ranges.append(0)
    body = ends + b'\0\0' + starts + deltas + struct.pack('>%dH' % n, *ranges) + struct.pack('>%dH' % len(array), *array)
    return struct.pack('>HHHHHHH', 4, 14 + len(body), 0, 2 * n, 0, 0, 0) + body


def format12(mapping):
    groups = b''.join(struct.pack('>III', c, c, g) for c, g in sorted(mapping.items()))
    return struct.pack('>HHIII', 12, 0, 16 + len(groups), 0, len(mapping)) + groups


def read_format4(t):
    n = struct.unpack('>H', t[6:8])[0] // 2
    ends = struct.unpack('>%dH' % n, t[14:14 + 2 * n])
    starts = struct.unpack('>%dH' % n, t[16 + 2 * n:16 + 4 * n])
    deltas = struct.unpack('>%dH' % n, t[16 + 4 * n:16 + 6 * n])
    ranges = struct.unpack('>%dH' % n, t[16 + 6 * n:16 + 8 * n])
    out = {}
    for i in range(n):
        for c in range(starts[i], ends[i] + 1):
            if c == 0xFFFF:
                continue
            if ranges[i]:
                at = 16 + 6 * n + 2 * i + ranges[i] + 2 * (c - starts[i])
                g = struct.unpack('>H', t[at:at + 2])[0]
                g = (g + deltas[i]) % 0x10000 if g else 0
            else:
                g = (c + deltas[i]) % 0x10000
            if g:
                out[c] = g
    return out


def read_format12(t):
    out = {}
    for i in range(struct.unpack('>I', t[12:16])[0]):
        first, last, g = struct.unpack('>III', t[16 + 12 * i:28 + 12 * i])
        out.update((c, g + c - first) for c in range(first, last + 1))
    return out


def subtables(font):
    """{(platform, encoding, format): {character: glyph}} of the cmap the table directory names."""
    _sum, at, length = directory(font)['cmap']
    t = font[at:at + length]
    out = {}
    for i in range(struct.unpack('>H', t[2:4])[0]):
        p, e, off = struct.unpack('>HHI', t[4 + 8 * i:12 + 8 * i])
        fmt = struct.unpack('>H', t[off:off + 2])[0]
        if fmt in (4, 12):
            out[p, e, fmt] = (read_format4 if fmt == 4 else read_format12)(t[off:])
    return out


def characters(font):
    """What the font maps: its full Unicode subtable's, else its BMP one's."""
    tables = subtables(font)
    full = [m for (p, e, fmt), m in tables.items() if fmt == 12 and (p == 0 or (p, e) == (3, 10))]
    return dict(full[0] if full else next(m for (p, e, fmt), m in tables.items() if fmt == 4 and (p == 0 or (p, e) == (3, 1))))


def bfttf(plain, key):
    """A font as the firmware stores it: the header, then the font XORed word by word with `key`."""
    words = struct.unpack('>%dI' % (len(plain) // 4), plain)
    return struct.pack('>II', DECODED_MAGIC ^ key, len(plain) ^ key) + b''.join(struct.pack('>I', w ^ key) for w in words)


def made_up_font(i, size=1000):
    """A made-up TrueType font of about `size` bytes, different for each `i` (never parsed past its
    first word by the runtime when it is a firmware font)."""
    filler = bytes((i * 37 + k * 11) % 251 for k in range(size))
    return sfnt({'zzzz': filler, 'cmap': cmap_table([(3, 1, format4({0x41: 3 + i, 0x25A1: 2}))])})


# ---- the harness ------------------------------------------------------------------------------
def build(folder):
    src = str(ROOT / 'tools' / 'dev' / 'pltest.c')
    if os.name == 'nt':
        exe = os.path.join(folder, 'pltest.exe')
        cmd = [cc.CLANG, '/nologo', '/O1', *cc.LINK_FLAGS, *cc.BASE_CFLAGS, src, '/Fe' + exe, '/Fo' + os.path.join(folder, 'pltest.obj'),
               '/link', *cc.LINK_OPTIONS]
    else:
        exe = os.path.join(folder, 'pltest')
        cmd = ['clang', '-O1', '-w', '-I', 'runtime/macos/include', '-I', 'runtime/linux/include', '-I', 'runtime', src, '-o', exe]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.returncode:
        raise AssertionError('pltest.c did not build:\n' + r.stdout + r.stderr)
    return exe


@unittest.skipUnless(HAVE_CLANG, 'needs clang on x86-64 Linux or an Apple silicon Mac, or clang-cl on Windows (deps\\llvm, or Visual Studio\'s C++ Clang tools)')
class SharedFontTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp(prefix='pltest ')
        cls.exe = build(cls.dir)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir, ignore_errors=True)

    def folder(self, files=None):
        """A new folder of {name: bytes}."""
        d = tempfile.mkdtemp(dir=self.dir)
        for name, data in (files or {}).items():
            with open(os.path.join(d, name), 'wb') as f:
                f.write(data)
        return d

    def firmware(self, types, plain=None, keys=None):
        """A folder of made-up firmware fonts for those types: {type: plain font}, and the folder."""
        plain = plain or {i: made_up_font(i) for i in types}
        keys = keys or {i: (0x13572468 * (i + 1)) & 0xFFFFFFFF for i in types}
        return plain, self.folder({NAMES[i] + '.bfttf': bfttf(plain[i], keys[i]) for i in types})

    def pltest(self, firmware, open_dir, room=None):
        """[(state, offset, size, from)] by type and one past them, the area, and the runtime's lines."""
        area = os.path.join(self.dir, 'area.bin')
        r = subprocess.run([self.exe, firmware, str(open_dir), area, *([str(room)] if room else [])],
                           capture_output=True, text=True, timeout=120)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        rows = [tuple(int(v) if v.isdigit() else v for v in m.groups())
                for m in re.finditer(r'^type \d+ state (\d+) offset (\d+) size (\d+) from (\S+)$', r.stdout, re.M)]
        self.assertEqual(len(rows), 7, r.stdout)
        with open(area, 'rb') as f:
            return rows, f.read(), r.stderr

    def font_at(self, area, row):
        _state, offset, size, _from = row
        return area[offset:offset + size]

    def check_layout(self, rows, area):
        """Back to back from the area's start, each behind the header a decoded .bfttf has; an
        empty type nowhere; past the last type, the first type's."""
        at = 0
        for state, offset, size, src in rows[:6]:
            if src == 'none':
                self.assertEqual((state, offset, size), (0, 0, 0))
                continue
            self.assertEqual(state, 1)
            self.assertEqual(offset, at + 8)
            self.assertEqual(struct.unpack('>I', area[offset - 8:offset - 4])[0], DECODED_MAGIC)
            at = (offset + size + 3) & ~3
        self.assertEqual(rows[6][:3], rows[0][:3])

    def check_open(self, row, area, name):
        """The open font as its file has it, but for its cmap entry, and a new cmap after it with
        all its characters and U+25A1, drawn as its U+FFFD, else U+25CA, else space."""
        with open(OPEN / (name + '.ttf'), 'rb') as f:
            original = f.read()
        got = self.font_at(area, row)
        header = area[row[1] - 8:row[1]]
        self.assertEqual(struct.unpack('>I', header[4:])[0] ^ FILE_KEY, len(got))
        entry = 12 + 16 * list(directory(original)).index('cmap')
        self.assertEqual(got[:entry + 4] + got[entry + 16:len(original)], original[:entry + 4] + original[entry + 16:])
        old = characters(original)
        self.assertNotIn(BOX, old)
        stand_in = old.get(0xFFFD) or old.get(0x25CA) or old[0x20]
        want = {**old, BOX: stand_in}
        new = subtables(got)
        self.assertEqual(sorted(new), [(3, 1, 4), (3, 10, 12)])
        self.assertEqual(new[3, 10, 12], want)
        self.assertEqual(new[3, 1, 4], {c: g for c, g in want.items() if c < 0xFFFF})
        total, at, length = directory(got)['cmap']
        self.assertEqual(at % 4, 0)
        self.assertGreaterEqual(at, len(original))
        self.assertEqual(at + length, len(got))
        self.assertEqual(total, checksum(got[at:at + length]))
        return stand_in, old

    def test_firmware_fonts_for_every_type(self):
        """Each decoded with its own key, as it is; the open fonts not used."""
        plain, fw = self.firmware(range(6))
        rows, area, log = self.pltest(fw, OPEN)
        self.check_layout(rows, area)
        for i in range(6):
            self.assertEqual(rows[i][3], 'firmware')
            self.assertEqual(self.font_at(area, rows[i]), plain[i])
        self.assertIn('[pl] shared fonts from the firmware (%s): %s\n' % (fw, ', '.join(TYPES)), log)
        self.assertNotIn('instead', log)
        self.assertNotIn('open shared fonts', log)

    def test_no_firmware_fonts(self):
        """Every type from third_party/fonts_open, said so for each."""
        fw = self.folder()
        rows, area, log = self.pltest(fw, OPEN)
        self.check_layout(rows, area)
        stand_ins = []
        for i in range(6):
            self.assertEqual(rows[i][3], 'open')
            stand_in, old = self.check_open(rows[i], area, NAMES[i])
            stand_ins.append([c for c in (0xFFFD, 0x25CA, 0x20) if old.get(c) == stand_in][0])
            self.assertIn('[pl] %s/%s.bfttf is missing: %s/%s.ttf instead\n' % (fw, NAMES[i], OPEN, NAMES[i]), log)
        # Open Sans has U+FFFD, Source Sans Pro (ChineseSimpleExtension) U+25CA, the button font a space
        self.assertEqual(stand_ins, [0xFFFD, 0xFFFD, 0x25CA, 0xFFFD, 0xFFFD, 0x20])
        self.assertIn('[pl] open shared fonts (%s; no Chinese, Japanese or Korean characters): %s\n' % (OPEN, ', '.join(TYPES)), log)
        self.assertNotIn('from the firmware', log)

    def test_some_firmware_fonts(self):
        """The firmware's where they are, the open ones for the rest, in type order."""
        plain, fw = self.firmware((0, 3, 5))
        rows, area, log = self.pltest(fw, OPEN)
        self.check_layout(rows, area)
        self.assertEqual([r[3] for r in rows[:6]], ['firmware', 'open', 'open', 'firmware', 'open', 'firmware'])
        for i in (0, 3, 5):
            self.assertEqual(self.font_at(area, rows[i]), plain[i])
        for i in (1, 2, 4):
            self.check_open(rows[i], area, NAMES[i])
        self.assertIn('[pl] shared fonts from the firmware (%s): Standard, ChineseTraditional, NintendoExtension\n' % fw, log)
        self.assertIn('[pl] open shared fonts (%s; no Chinese, Japanese or Korean characters): ChineseSimple, ChineseSimpleExtension, Korean\n' % OPEN, log)
        self.assertEqual(log.count(' instead\n'), 3)

    def test_firmware_file_that_is_no_font(self):
        """One that decodes to something else, and one shorter than a header and a font's first word:
        the open ones in their place, and why."""
        plain, fw = self.firmware((0, 1, 2, 3, 4, 5))
        with open(os.path.join(fw, NAMES[1] + '.bfttf'), 'wb') as f:
            f.write(bfttf(b'OTTO' + bytes(60), 0x2468ACE0))
        with open(os.path.join(fw, NAMES[4] + '.bfttf'), 'wb') as f:
            f.write(bytes(8))
        rows, area, log = self.pltest(fw, OPEN)
        self.check_layout(rows, area)
        self.assertEqual([r[3] for r in rows[:6]], ['firmware', 'open', 'firmware', 'firmware', 'open', 'firmware'])
        self.check_open(rows[1], area, NAMES[1])
        self.check_open(rows[4], area, NAMES[4])
        self.assertIn('%s/%s.bfttf is no TrueType font once decoded: %s/%s.ttf instead\n' % (fw, NAMES[1], OPEN, NAMES[1]), log)
        self.assertIn('%s/%s.bfttf is too short for a font: %s/%s.ttf instead\n' % (fw, NAMES[4], OPEN, NAMES[4]), log)

    def test_no_font_at_all(self):
        """Neither folder has them: every type empty, as the game would see a font still loading."""
        fw, open_dir = self.folder(), self.folder()
        rows, area, log = self.pltest(fw, open_dir)
        self.check_layout(rows, area)
        self.assertEqual(rows, [(0, 0, 0, 'none')] * 6 + [(0, 0, 0, '-')])
        self.assertIn('[pl] %s/%s.bfttf is missing, and %s/%s.ttf is missing: that font is empty\n' % (fw, NAMES[2], open_dir, NAMES[2]), log)
        self.assertIn('[pl] no shared font for %s\n' % ', '.join(TYPES), log)

    def test_area_full(self):
        """A firmware font bigger than what is left of the area: the open one if it fits there, else none."""
        big = {i: made_up_font(i, 6000) for i in range(6)}
        plain, fw = self.firmware(range(6), plain=big)
        small = {i: made_up_font(10 + i, 200) for i in range(6)}
        open_dir = self.folder({NAMES[i] + '.ttf': small[i] for i in range(6)})
        sizes = [len(big[0]) + 8, len(big[1]) + 8]
        room = sum(sizes) + len(small[2]) + 8 + 16
        rows, area, log = self.pltest(fw, open_dir, room)
        self.check_layout(rows, area)
        self.assertEqual([r[3] for r in rows[:6]], ['firmware', 'firmware', 'open', 'none', 'none', 'none'])
        self.assertEqual(self.font_at(area, rows[2]), small[2])   # (it has U+25A1: as it is)
        self.assertIn('%s/%s.bfttf does not fit in what is left of the font area: %s/%s.ttf instead\n' % (fw, NAMES[2], open_dir, NAMES[2]), log)
        self.assertIn('%s/%s.bfttf does not fit in what is left of the font area, and %s/%s.ttf does not fit in what is left of the font area: that font is empty\n'
                      % (fw, NAMES[3], open_dir, NAMES[3]), log)

    def test_made_up_open_fonts(self):
        """Character maps of the other kinds: a BMP one through glyphIdArray, a full one with a character
        past the BMP, one with neither U+FFFD, U+25CA nor a space (left as it is), one with U+25A1
        already (as it is)."""
        bmp = {0x20: 3, 0x41: 4, 0x42: 5, 0x43: 9, 0x2603: 7, 0xFFFD: 8}
        full = {0x20: 3, 0x41: 4, 0x25CA: 6, 0x1F600: 10}
        fonts = {
            0: sfnt({'cmap': cmap_table([(3, 1, format4(bmp, with_array=(0x43, 0x2603)))]), 'glyf': bytes(40)}),
            1: sfnt({'cmap': cmap_table([(0, 3, format4({0x41: 4})), (3, 10, format12(full))]), 'glyf': bytes(40)}),
            2: sfnt({'cmap': cmap_table([(3, 1, format4({0x41: 4, 0x42: 5}))]), 'glyf': bytes(40)}),
            3: made_up_font(3),
            4: sfnt({'glyf': bytes(40)}),   # (no cmap at all: as it is)
            5: sfnt({'cmap': cmap_table([(1, 0, bytes(262))]), 'glyf': bytes(40)}),   # (no Unicode subtable: as it is)
        }
        open_dir = self.folder({NAMES[i] + '.ttf': fonts[i] for i in fonts})
        rows, area, _log = self.pltest(self.folder(), open_dir)
        self.check_layout(rows, area)
        got = {i: self.font_at(area, rows[i]) for i in range(6)}
        self.assertEqual(read_format4(format4(bmp, with_array=(0x43, 0x2603))), bmp)   # (the test's own format 4)
        self.assertEqual(subtables(got[0])[3, 10, 12], {**bmp, BOX: 8})
        self.assertEqual(subtables(got[0])[3, 1, 4], {**bmp, BOX: 8})
        self.assertEqual(subtables(got[1])[3, 10, 12], {**full, BOX: 6})
        self.assertEqual(subtables(got[1])[3, 1, 4], {0x20: 3, 0x41: 4, 0x25CA: 6, BOX: 6})
        for i in (2, 3, 4, 5):
            self.assertEqual(got[i], fonts[i])


class OpenFontFilesTests(unittest.TestCase):
    def test_every_type_has_its_open_font(self):
        """third_party/fonts_open holds a font for each name hle_pl.c asks for, with its licences."""
        with open(ROOT / 'runtime' / 'hle_pl.c', encoding='utf-8') as f:
            source = f.read()
        for name in NAMES:
            self.assertIn('"%s"' % name, source)
            with open(OPEN / (name + '.ttf'), 'rb') as f:
                self.assertEqual(f.read(4), b'\0\1\0\0')
        for licence in ('LICENSE-Apache-2.0.txt', 'LICENSE-OFL-1.1.txt', 'LICENSE-GPL-2.0-or-later.txt', 'README.md'):
            self.assertTrue((OPEN / licence).is_file(), licence)


if __name__ == '__main__':
    unittest.main()
