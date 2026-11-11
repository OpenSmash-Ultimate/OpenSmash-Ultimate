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

    def check(self, bad, found=()):
        patch = types.SimpleNamespace(block=BS)
        logged = []
        with mock.patch.object(update, 'verify', lambda p, cfg: bad), \
                mock.patch.object(update, 'recover', lambda p, cfg, b: {k: ('src', 0) for k in found}), \
                mock.patch.object(update, 'log', logged.append):
            return update.check_result(patch, str(self.cfg), str(self.report)), logged

    def test_harmless_blocks_go_on(self):
        left, _ = self.check([self.block(0x180), self.block(STREAM)])
        self.assertEqual(left, [self.block(0x180), self.block(STREAM)])
        self.assertTrue(self.cfg.exists())
        self.assertTrue(self.report.exists())

    def test_a_loaded_file_warns_and_goes_on(self):
        """(Most players have only a merged base - a console dumps the game updated: the install
        goes on, with a warning, the report, and the runtime's line in every crash report.)"""
        left, logged = self.check([self.block(FILE_A), self.block(STREAM)])
        self.assertEqual(left, [self.block(FILE_A), self.block(STREAM)])
        self.assertTrue(self.cfg.exists())
        self.assertFalse(Path(str(self.cfg) + '.unsafe').exists())
        self.assertTrue(self.report.exists())
        warning = [line for line in logged if line.startswith('WARNING')]
        self.assertEqual(len(warning), 1)
        self.assertIn('2 blocks of 16 KB of the updated game data are unconfirmed, 1 of them inside files the game loads',
                      warning[0])
        self.assertIn('A crash in the game\'s data may come from them', warning[0])

    def test_recovered_blocks_are_not_listed(self):
        left, logged = self.check([self.block(FILE_A), self.block(STREAM)], found=[self.block(FILE_A)])
        self.assertEqual(left, [self.block(STREAM)])
        self.assertNotIn(f'{self.block(FILE_A)} ', self.report.read_text())
        left, logged = self.check([self.block(FILE_A)], found=[self.block(FILE_A)])
        self.assertEqual(left, [])
        self.assertFalse(self.report.exists())
        self.assertFalse([line for line in logged if line.startswith('WARNING')])


# ---- the blocks still unconfirmed, looked for by their hash ------------------------------------
class FakePatch:
    """An update over a made-up image: in each block in `own`, the second half is the update's own
    data (certain); every other byte is the original release's, to be found in the base."""

    def __init__(self, image, own):
        self.l6, self.l6size, self.block = 0, len(image), BS
        self.blocks = (len(image) + BS - 1) // BS
        self.image, self.own = image, set(own)
        self.hashes = b''.join(sha(image[i:i + BS] + bytes(BS - len(image[i:i + BS]))) for i in range(0, len(image), BS))

    def block_hash(self, b):
        return self.hashes[b * 32:b * 32 + 32]

    def pieces(self, v, size):
        end = v + size
        while v < end:
            b, half = v // BS, BS // 2
            mid = b * BS + half
            st = 1 if b in self.own and v >= mid else 0
            n = min(end, mid if v < mid else (b + 1) * BS) - v
            yield v, n, st, v
            v += n

    def storage(self, off, n):
        return self.image[off:off + n]


def noise(n, seed):
    out, x = bytearray(), seed
    for _ in range(n):
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        out.append(x >> 16 & 0xFF)
    return bytes(out)


class RecoverTests(unittest.TestCase):
    """A block left wrong by a merged base is found again by its hash: its certain bytes (the
    update's own data) anchor the search over the base's file and romfs_patch.bin."""
    LOST, ELSEWHERE, IN_PATCH, GONE, NO_ANCHOR = 3, 5, 7, 9, 11

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory(prefix='update-recover-')
        self.addCleanup(self.dir.cleanup)
        top = Path(self.dir.name)
        self.image = noise(BLOCKS * BS, 7)
        own = [b for b in range(BLOCKS) if b != self.NO_ANCHOR]
        self.patch = FakePatch(self.image, own)
        base = bytearray(self.image)
        for b in (self.LOST, self.ELSEWHERE, self.IN_PATCH, self.GONE, self.NO_ANCHOR):
            base[b * BS:b * BS + BS // 2] = noise(BS // 2, 100 + b)       # the merged base: the original bytes moved away
        base += noise(1000, 3) + self.blk(self.LOST) + noise(37, 4) + self.blk(self.ELSEWHERE)   # (at odd offsets)
        patch_bin = noise(500, 5) + self.blk(self.IN_PATCH) + noise(11, 6)
        (top / 'base.bin').write_bytes(bytes(base))
        (top / 'romfs_patch.bin').write_bytes(patch_bin)
        self.layers = top / 'romfs.layers'
        update.write_layers(str(self.layers), len(self.image), [(0, 0, 0)])
        self.cfg = top / 'romfs.cfg'
        self.cfg.write_text(f"layered\n{self.layers}\n{top / 'base.bin'}\n{top / 'romfs_patch.bin'}\n")
        self.where = {self.LOST: (str(top / 'base.bin'), BLOCKS * BS + 1000),
                      self.ELSEWHERE: (str(top / 'base.bin'), BLOCKS * BS + 1000 + BS + 37),
                      self.IN_PATCH: (str(top / 'romfs_patch.bin'), 500)}
        for name, value in (('SCAN', 100), ('log', lambda msg: None)):   # (small reads: anchors across their edges)
            patcher = mock.patch.object(update, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)

    def blk(self, b):
        return self.image[b * BS:(b + 1) * BS]

    def bad(self):
        return update.verify(self.patch, str(self.cfg))

    def test_the_anchor(self):
        self.assertEqual(update.anchor([(32, self.image[32:64])]), (32, self.image[32:64]))
        self.assertIsNone(update.anchor([(0, bytes(64))]))            # (zeros tell nothing)
        self.assertIsNone(update.anchor([(0, self.image[:31])]))      # (too short)

    def test_dry_run(self):
        bad = self.bad()
        self.assertEqual(bad, [self.LOST, self.ELSEWHERE, self.IN_PATCH, self.GONE, self.NO_ANCHOR])
        before = self.layers.read_bytes()
        found = update.recover(self.patch, str(self.cfg), bad, dry_run=True)
        self.assertEqual(found, self.where)
        self.assertEqual(self.layers.read_bytes(), before)            # (nothing changed)

    def test_found_blocks_are_mapped(self):
        found = update.recover(self.patch, str(self.cfg), self.bad())
        self.assertEqual(found, self.where)
        self.assertEqual(self.bad(), [self.GONE, self.NO_ANCHOR])     # (the others read back right through romfs.cfg)
        img = update.RomfsImage(str(self.cfg))
        try:
            for b in range(BLOCKS):
                if b not in (self.GONE, self.NO_ANCHOR):
                    self.assertEqual(img.read(b * BS, BS), self.blk(b))
        finally:
            img.close()

    def test_a_block_of_original_release_data_by_its_guess(self):
        """No certain bytes in it (all of it the original release's): pieces of its best guess,
        right where the guess is right, anchor the search (13.0.5 over a merged 13.0.4: such a
        block, its guess wrong in 202 of 16,384 bytes, was found 4 KB before its place)."""
        base = Path(self.dir.name) / 'base.bin'
        at = base.stat().st_size + 3
        base.write_bytes(base.read_bytes() + b'\x01\x02\x03' + self.blk(self.NO_ANCHOR))
        with mock.patch.object(update, 'GUESSES', 2):   # (64-byte blocks: two 32-byte pieces of the guess)
            found = update.recover(self.patch, str(self.cfg), self.bad())
        self.assertEqual(found[self.NO_ANCHOR], (str(base), at))
        self.assertEqual(self.bad(), [self.GONE])

    def test_too_many_is_not_this_image(self):
        with mock.patch.object(update, 'MAX_RECOVER', 2):
            self.assertEqual(update.recover(self.patch, str(self.cfg), self.bad()), {})


if __name__ == '__main__':
    unittest.main()
