"""update.py's hash tree: every block of the updated image checked against the hash the update
carries for it; an update that takes some of its hashes from the original release (13.0.5 never
does) is not 13.0.5's, and is refused. Made-up hash trees with 64-byte blocks (the game's are
16 KB); no game files."""
from pathlib import Path
import hashlib
import struct
import sys
import tempfile
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import update  # noqa: E402

BLOG = 6
BS = 1 << BLOG   # two hashes per block, so every level of a small tree is used
BLOCKS = 16


def sha(data):
    return hashlib.sha256(data).digest()


def ivfc(l6):
    """Levels 1 to 6 (each padded with zeros to whole blocks) and the master hash: each hash
    level holds the SHA-256 of every block of the level below."""
    levels = [l6]
    for _ in range(5):
        below = levels[0]
        h = b''.join(sha(below[j:j + BS]) for j in range(0, len(below), BS))
        levels.insert(0, h + bytes(-len(h) % BS))
    return levels, sha(levels[0][:BS])


def data():
    return b''.join(bytes([b]) * BS for b in range(BLOCKS))


def digests(l6, corrupt=()):
    return b''.join(sha(l6[b * BS:(b + 1) * BS] if b not in corrupt else b'x' * BS) for b in range(BLOCKS))


class UnconfirmedTests(unittest.TestCase):
    def test_every_block_against_its_hash(self):
        levels, _master = ivfc(data())
        patch = types.SimpleNamespace(hashes=levels[4], blocks=BLOCKS)
        self.assertEqual(update.unconfirmed(patch, digests(data())), [])
        self.assertEqual(update.unconfirmed(patch, digests(data(), corrupt={3, 9})), [3, 9])


def patch_section(reused):
    """An update's RomFS section (BKTR) over the tree of data(): its own data is the image itself,
    except the virtual ranges in `reused`, which it takes from the original release."""
    levels, master = ivfc(data())
    image, offs = b''.join(levels), []
    for level in levels:
        offs.append(sum(len(x) for x in levels[:len(offs)]))
    pieces, at = [], 0
    for a, z in reused:
        pieces += [(at, at, 1), (a, 0x40, 0)]
        at = z
    pieces.append((at, at, 1))

    def table(entries, fmt, end):
        node = bytearray(0x4000)
        struct.pack_into('<IIQ', node, 0, 0, len(entries), end)
        size = struct.calcsize(fmt)
        for j, e in enumerate(entries):
            struct.pack_into(fmt, node, 0x10 + j * size, *e)
        head = bytearray(0x4000)
        struct.pack_into('<IIQ', head, 0, 0, 1, end)
        return bytes(head + node)
    ind_off = len(image)
    ind = table(pieces, '<QQi', len(image))
    ctr_off = ind_off + len(ind)
    ctr = table([(0, 0, 0)], '<QIi', ind_off)
    physical = image + ind + ctr
    fsh = bytearray(0x200)
    fsh[0x08:0x0C] = b'IVFC'
    for i, level in enumerate(levels):
        struct.pack_into('<QQI', fsh, 0x18 + i * 0x18, offs[i], len(level), BLOG)
    fsh[0xC8:0xE8] = master
    struct.pack_into('<QQ', fsh, 0x100, ind_off, len(ind))
    fsh[0x110:0x114] = b'BKTR'
    struct.pack_into('<QQ', fsh, 0x120, ctr_off, len(ctr))
    fsh[0x130:0x134] = b'BKTR'
    sec = types.SimpleNamespace(enc_type=4, fs_header=bytes(fsh),
                                read_raw=lambda off, size, ctr_hi=None: physical[off:off + size])
    nca = types.SimpleNamespace(content_type=0, section=lambda index: sec if index == 0 else None)
    return nca, offs


class OpenTests(unittest.TestCase):
    def open(self, reused_entries):
        l5 = sum(len(level) for level in ivfc(data())[0][:4])
        nca, _offs = patch_section([(l5 + a * 32, l5 + z * 32) for a, z in reused_entries])
        with mock.patch.object(update, 'nsp_ncas', lambda nsp, keydir: [('update.nca', nca)]):
            return update.Patch('update.nsp', 'keys')

    def test_an_update_with_all_its_hashes(self):
        p = self.open([])
        self.assertEqual(p.block_hash(3), sha(data()[3 * BS:4 * BS]))
        self.assertEqual(update.unconfirmed(p, digests(data(), corrupt={7})), [7])

    def test_an_update_reusing_hashes_is_not_13_0_5(self):
        """(What a player's other update did: level 5 partly the original release's.)"""
        with self.assertRaises(SystemExit) as e:
            self.open([(4, 12)])
        self.assertIn('WindowSmash needs version 13.0.5 of the game, and this update takes some of its hashes from '
                      'the original release', str(e.exception.code))


# ---- the blocks left unconfirmed: what of the game's data each holds -----------------------------
FILE_DATA = 0x100   # where the made-up RomFS's file data starts
ARC = 0x400         # data.arc: the first file, this long; other.bin after it
TABLE = 0x40        # data.arc's file table: [0x40, 0x70)
FILE_A, FILE_B, STREAM = 0x100, 0x140, 0x200   # (data.arc offsets) a file by path, a file no path names, a stream


def romfs(files):
    """A RomFS image (header, one root folder, `files` [(name, data)] in it, the data from FILE_DATA)."""
    meta, at, out = bytearray(), 0, bytearray()
    for k, (name, data) in enumerate(files):
        raw = name.encode()
        entry_size = 0x20 + (len(raw) + 3) // 4 * 4
        sibling = len(meta) + entry_size if k + 1 < len(files) else 0xFFFFFFFF
        meta += struct.pack('<IIQQII', 0, sibling, at, len(data), 0, len(raw)) + raw + bytes(-len(raw) % 4)
        out += data
        at += len(data)
    dirs = struct.pack('<6I', 0, 0xFFFFFFFF, 0xFFFFFFFF, 0, 0, 0)
    head = struct.pack('<10Q', 0x50, 0x50, 0, 0x50, len(dirs), 0x50 + len(dirs), 0, 0x50 + len(dirs), len(meta), FILE_DATA)
    tables = head + dirs + bytes(meta)
    return tables + bytes(FILE_DATA - len(tables)) + bytes(out)


class FakeArc:
    """data.arc's tables as update.held_in reads them (the real table is tens of MB, zstd)."""
    header = (0, 0, 0, 0, TABLE, 0, 0)

    def __init__(self, read):
        self.read = lambda off, n: struct.pack('<4I', 0x10, 0x40, 0x20, 0) if off == TABLE else read(off, n)

    def files_by_path(self):
        return {(0x2A0B1C2D3E, 0): (FILE_A, 0x40, 0x80, 3), (0x2A0B1C2D3E, 1): (FILE_A, 0x40, 0x80, 3)}

    def reachable(self):
        return [(FILE_A, 0x40, 0x80, 3), (FILE_B, 0x20, 0x20, 0)]

    def streams(self):
        return {(0x1F00000001, 0): (STREAM, 0x100)}


class UnverifiedFilesTests(unittest.TestCase):
    """romfs.unverified.txt names what each unconfirmed block holds; one inside a file the game
    loads stops the install (a player's merged base: LoupeSubstance's models wrong, a crash at boot)."""

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory(prefix='update-unverified-')
        self.addCleanup(self.dir.cleanup)
        top = Path(self.dir.name)
        (top / 'rom.bin').write_bytes(b'\xAA' * 0x10 + romfs([('data.arc', bytes(ARC)), ('other.bin', bytes(0x40))]))
        self.cfg = top / 'romfs.cfg'
        self.cfg.write_text(f"{top / 'rom.bin'}\n16\n")   # (the image 16 bytes into its file)
        self.report = top / 'romfs.unverified.txt'
        patcher = mock.patch.object(update, 'Arc', FakeArc)
        patcher.start()
        self.addCleanup(patcher.stop)

    def block(self, arc_offset):
        return (FILE_DATA + arc_offset) // BS

    def held(self, *blocks):
        return update.held_in(str(self.cfg), list(blocks), BS)

    def test_what_each_block_holds(self):
        a, b, pad, stream = self.block(FILE_A), self.block(FILE_B), self.block(0x180), self.block(STREAM)
        other = (FILE_DATA + ARC) // BS
        held = self.held(a, b, pad, stream, other, 0, self.block(0), self.block(TABLE))
        self.assertEqual(held[a], [(True, 'file data.arc+0x100 (0x40 bytes): path hash40 0x2a0b1c2d3e, 0x2a0b1c2d3e (region 1)')])
        self.assertEqual(held[b], [(True, 'file data.arc+0x140 (0x20 bytes): reached by no path of its own')])
        self.assertEqual(held[pad], [])
        self.assertEqual(held[stream], [(False, 'stream data.arc+0x200 (0x100 bytes): hash40 0x1f00000001, music or a movie')])
        self.assertEqual(held[other], [(True, 'RomFS file other.bin')])
        self.assertEqual(held[0], [(True, "the RomFS's own header and tables")])
        self.assertEqual(held[self.block(0)], [(True, "data.arc's header")])
        self.assertEqual(held[self.block(TABLE)], [(True, "data.arc's file table")])

    def test_an_unreadable_file_table(self):
        def broken(read):
            raise SystemExit('data.arc does not begin as the game\'s does')
        with mock.patch.object(update, 'Arc', broken):
            held = self.held(self.block(0x180))
        self.assertEqual(held[self.block(0x180)], [(True, "data.arc, whose file table could not be read (data.arc does not "
                                                          "begin as the game's does)")])

    def test_the_report(self):
        a, pad, stream = self.block(FILE_A), self.block(0x180), self.block(STREAM)
        loaded = update.report_unverified(str(self.cfg), [a, pad, stream], BS, str(self.report))
        self.assertEqual(loaded, [a])
        lines = self.report.read_text().splitlines()
        body = [l for l in lines if not l.startswith('#')]
        self.assertEqual(body, [f'{a} {a * BS:#x}', '  loaded file data.arc+0x100 (0x40 bytes): path hash40 0x2a0b1c2d3e, '
                                '0x2a0b1c2d3e (region 1)',
                                f'{pad} {pad * BS:#x}', '  listed no file: padding between files',
                                f'{stream} {stream * BS:#x}', '  listed stream data.arc+0x200 (0x100 bytes): hash40 0x1f00000001, '
                                'music or a movie'])
        self.assertIn('# 3 blocks, 1 inside files the game loads', lines)
        # (the runtime counts the lines that start with a digit: runtime/hle_fs.c rom_unverified_note)
        self.assertEqual(sum(1 for l in lines if l[:1].isdigit()), 3)
        self.assertEqual(update.report_unverified(str(self.cfg), [], BS, str(self.report)), [])
        self.assertFalse(self.report.exists())   # (every block confirmed: no report)

    def check(self, bad):
        patch = types.SimpleNamespace(block=BS)
        with mock.patch.object(update, 'verify', lambda p, cfg: bad):
            return update.check_result(patch, str(self.cfg), str(self.report))

    def test_harmless_blocks_go_on(self):
        self.assertEqual(self.check([self.block(0x180), self.block(STREAM)]), [self.block(0x180), self.block(STREAM)])
        self.assertTrue(self.cfg.exists())
        self.assertTrue(self.report.exists())

    def test_a_loaded_file_stops_the_install(self):
        with self.assertRaises(update.UnsafeData) as e:
            self.check([self.block(FILE_A), self.block(STREAM)])
        self.assertEqual(e.exception.code, update.UNSAFE_EXIT)
        self.assertIn('2 blocks of 16 KB of the updated game data could not be confirmed, 1 of them inside files the game '
                      'loads', e.exception.message)
        self.assertIn('original release (1.0.0', e.exception.message)
        self.assertFalse(self.cfg.exists())   # (set aside: nothing starts the game with it, the next build applies again)
        self.assertTrue(Path(str(self.cfg) + '.unsafe').exists())
        self.assertTrue(self.report.exists())


if __name__ == '__main__':
    unittest.main()
