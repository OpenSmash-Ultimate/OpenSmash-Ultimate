"""tools/css_layout.py, the select screen for any number of fighters, and tools/mods.py's use of it
(select_screen): made-up layout, animation and table files of the game's formats, no game data.
  - the archive (SARC) read and written back the same, its names hashed as the game's are;
  - the layout (FLYT) given more tiles: copies of tile 0's two panes, a material of its own for each
    picture, the section count and the size right;
  - a tile size (FLAN) as a smaller copy: places and scales (or sizes) times the factor;
  - the table's rows for any count: 13 rows at most, every tile in them, the tiles in the screen's area;
  - mods.select_screen: made past 4 new fighters, kept while nothing changes, gone at 4 or fewer, left
    to a mod that brings its own arrangement."""
import json
import os
import struct
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import css_layout  # noqa: E402
import mods  # noqa: E402
import prc  # noqa: E402


def section(magic, body):
    return magic + struct.pack('<I', 8 + len(body)) + body


def pane(magic, name, extra=b''):
    body = bytearray(0x4C)
    body[0] = 1                                            # (visible)
    body[4:4 + len(name)] = name.encode()
    struct.pack_into('<10f', body, 0x24, -864.0, 415.0, 0, 0, 0, 0, 1.0, 1.0, 144.0, 80.0)
    return section(magic, bytes(body) + extra)


def material(name):
    return name.encode().ljust(28, b'\0') + b'\x15\x14\0\0' * 4


def flyt(tiles=1):
    mats = [material(css_layout.CHR_BG.format(k)) for k in range(tiles)] + [material('other')]
    table = 12 + 4 * len(mats)
    offs, pos = [], table
    for m in mats:
        offs.append(pos)
        pos += len(m)
    mat1 = section(b'mat1', struct.pack('<HH', len(mats), 0) + struct.pack(f'<{len(mats)}I', *offs) + b''.join(mats))
    secs = [mat1, pane(b'pan1', 'RootPane'), section(b'pas1', b'')]
    for k in range(tiles):
        secs.append(pane(b'pic1', css_layout.CHR_BG.format(k), bytes(16) + struct.pack('<H', k) + bytes(50)))
    for k in range(tiles):
        secs.append(pane(b'prt1', css_layout.CHR.format(k), struct.pack('<Iff', 0, 1.0, 1.0) + b'chara_select_btn0_00\0\0\0\0'))
    secs.append(section(b'pae1', b''))
    body = b''.join(secs)
    return b'FLYT' + struct.pack('<HHIIHH', 0xFEFF, 0x14, 0x09000000, 0x14 + len(body), len(secs), 0) + body


def target(index, kind, value):
    head = struct.pack('<BBBBHHI', 0, index, 2, 0, 1, 0, 12)
    return head + struct.pack('<3f', 0.0, value, value / 2)


def tag(magic, targets):
    offs, pos = [], 8 + 4 * len(targets)
    for t in targets:
        offs.append(pos)
        pos += len(t)
    return magic + struct.pack('<B3x', len(targets)) + struct.pack(f'<{len(targets)}I', *offs) + b''.join(targets)


def content(name, tags):
    offs, pos = [], 32 + 4 * len(tags)
    for t in tags:
        offs.append(pos)
        pos += len(t)
    return name.encode().ljust(28, b'\0') + struct.pack('<BBxx', len(tags), 0) + struct.pack(f'<{len(tags)}I', *offs) + b''.join(tags)


def flan(name):
    conts = [content('hit', [tag(b'FLPA', [target(8, 2, 144.0), target(9, 2, 82.0)])]),
             content('hd', [tag(b'FLPA', [target(0, 2, -52.0), target(6, 2, 0.5), target(8, 2, 20.0)]),
                            tag(b'FLVI', [])])]
    offs, pos = [], 0x14 + 4 * len(conts)
    for c in conts:
        offs.append(pos)
        pos += len(c)
    pai1 = section(b'pai1', struct.pack('<HBBHHI', 0, 0, 0, 0, len(conts), 0x14) + struct.pack(f'<{len(conts)}I', *offs) + b''.join(conts))
    pat1 = section(b'pat1', struct.pack('<HH', 17, 1) + bytes(20) + name.encode() + b'\0\0window_size\0')
    body = pat1 + pai1
    return b'FLAN' + struct.pack('<HHIIHH', 0xFEFF, 0x14, 0x09000000, 0x14 + len(body), 2, 0) + body


def read_flan(data):
    """{(pane, target): value} of a FLAN made by flan()."""
    out = {}
    o = 0x14
    for _ in range(struct.unpack_from('<H', data, 0x10)[0]):
        size = struct.unpack_from('<I', data, o + 4)[0]
        if data[o:o + 4] == b'pai1':
            _ntex, n, at = struct.unpack_from('<HHI', data, o + 0x0C)
            for co in struct.unpack_from(f'<{n}I', data, o + at):
                c = o + co
                name = data[c:c + 28].split(b'\0')[0].decode()
                for t in struct.unpack_from(f'<{data[c + 28]}I', data, c + 32):
                    to = c + t
                    for tt in struct.unpack_from(f'<{data[to + 4]}I', data, to + 8):
                        a = to + tt
                        if data[to:to + 4] == b'FLPA':
                            out[(name, data[a + 1])] = struct.unpack_from('<f', data, a + 12 + 4)[0]
        o += size
    return out


ROW_KEYS = ['chara_num', 'icon_size', 'margin_x', 'margin_y', 'offset'] + [f'row{r:02d}_num' for r in range(13)]


def arrangement(rows=91):
    def row(n):
        cols = 13
        counts = [cols] * (n // cols) + ([n % cols] if n % cols else [])
        vals = {'chara_num': n, 'icon_size': 7, 'margin_x': 0, 'margin_y': 0, 'offset': 1}
        st = prc.Struct()
        for k in ROW_KEYS:
            v = counts[int(k[3:5])] if k.startswith('row') and int(k[3:5]) < len(counts) else vals.get(k, 0)
            st.append((prc.hash40(k), (prc.I8 if k == 'offset' else prc.U8, v)))
        return (prc.STRUCT, st)
    root = prc.Struct([(prc.hash40('normal_layout_param'), (prc.LIST, prc.List(row(n) for n in range(1, rows + 1)))),
                       (prc.hash40('compe_layout_param'), (prc.LIST, prc.List(row(n) for n in range(1, rows + 1))))])
    return prc.write((prc.STRUCT, root))


def layout_arc(tiles=3):
    info = b'chara_select.bflyt\0bg_melee\0\0chara_select_btn0_00\0window_size_07\0\0\0\0'
    files = [(css_layout.MAIN, flyt(tiles), 16), (css_layout.INFO, info, 16),
             (css_layout.SIZE_ANIM.format(7), flan('window_size_07'), 16), ('timg/__Combined.bntx', b'BNTX' + bytes(60), 0x1000)]
    return css_layout.sarc_write(files)


def rows_of(data, key):
    root = dict(prc.read(data)[1])
    out = []
    for _k, st in root[prc.hash40(key)][1]:
        d = {k: v for k, (_t, v) in st}
        out.append((d[prc.hash40('chara_num')], d[prc.hash40('icon_size')],
                    [d[prc.hash40(f'row{r:02d}_num')] for r in range(13)]))
    return out


class Archive(unittest.TestCase):
    def test_round_trip(self):
        data = layout_arc()
        files = css_layout.sarc_read(data)
        self.assertEqual([n for n, _b, _a in files], [css_layout.MAIN, css_layout.INFO, css_layout.SIZE_ANIM.format(7), 'timg/__Combined.bntx'])
        again = css_layout.sarc_write(files)
        self.assertEqual([(n, b) for n, b, _a in css_layout.sarc_read(again)], [(n, b) for n, b, _a in files])
        self.assertEqual(css_layout.sarc_write(css_layout.sarc_read(again)), again)   # (each file where it was)
        self.assertEqual(files[3][2] % 0x1000, 0)

    def test_hash(self):
        self.assertEqual(css_layout.sarc_hash(''), 0)
        self.assertEqual(css_layout.sarc_hash('ab'), (ord('a') * 0x65 + ord('b')) & 0xFFFFFFFF)


class Layout(unittest.TestCase):
    def test_more_tiles(self):
        out, had = css_layout.add_tiles(flyt(2), 5)
        self.assertEqual(had, 2)
        head, secs = css_layout.sections(out)
        self.assertEqual(struct.unpack_from('<I', out, 0x0C)[0], len(out))
        self.assertEqual(struct.unpack_from('<H', out, 0x10)[0], len(secs))
        names = [css_layout.pane_name(s) for s in secs]
        for k in range(5):
            self.assertIn(css_layout.CHR.format(k), names)
            self.assertIn(css_layout.CHR_BG.format(k), names)
        # the pictures' materials: each new one its own, named as the picture
        mat = next(s for s in secs if bytes(s[:4]) == b'mat1')
        count = struct.unpack_from('<H', mat, 8)[0]
        self.assertEqual(count, 3 + 3)
        offs = struct.unpack_from(f'<{count}I', mat, 12)
        for k in range(2, 5):
            pic = secs[names.index(css_layout.CHR_BG.format(k))]
            m = struct.unpack_from('<H', pic, 0x54 + 16)[0]
            self.assertEqual(bytes(mat[offs[m]:offs[m] + 28]).split(b'\0')[0].decode(), css_layout.CHR_BG.format(k))
        # new parts after the last old one, new pictures after the last old picture, inside the same parent
        self.assertLess(names.index(css_layout.CHR_BG.format(4)), names.index(css_layout.CHR.format(0)))
        self.assertEqual(bytes(secs[-1][:4]), b'pae1')

    def test_no_change_when_enough(self):
        lyt = flyt(3)
        self.assertEqual(css_layout.add_tiles(lyt, 3), (lyt, 3))


class Sizes(unittest.TestCase):
    def test_scaled(self):
        base = read_flan(flan('window_size_07'))
        out = css_layout.scaled_size(flan('window_size_07'), 'window_size_07', 'window_size_12', 0.5)
        self.assertIn(b'window_size_12\0', out)
        self.assertNotIn(b'window_size_07\0', out)
        got = read_flan(out)
        self.assertAlmostEqual(got[('hit', 8)], base[('hit', 8)] * 0.5)      # (no scale: the size)
        self.assertAlmostEqual(got[('hit', 9)], base[('hit', 9)] * 0.5)
        self.assertAlmostEqual(got[('hd', 0)], base[('hd', 0)] * 0.5)        # the place
        self.assertAlmostEqual(got[('hd', 6)], base[('hd', 6)] * 0.5)        # a scale: the scale...
        self.assertAlmostEqual(got[('hd', 8)], base[('hd', 8)])              # ...and not the size too

    def test_factors(self):
        self.assertEqual(css_layout.size_factor(8), 1.0)
        self.assertAlmostEqual(css_layout.size_factor(9), css_layout.FACTOR)
        self.assertEqual(css_layout.pick_size(0.95), 10)
        self.assertEqual(css_layout.pick_size(0.001), css_layout.FIRST_SIZE + css_layout.SIZES - 1)

    def test_info(self):
        info = b'chara_select.bflyt\0a\0\0chara_select_btn0_00\0window_size_07\0\0x\0\0'
        out = css_layout.add_info_names(info, ['window_size_09', 'window_size_07'])
        self.assertEqual(out, b'chara_select.bflyt\0a\0\0chara_select_btn0_00\0window_size_09\0window_size_07\0\0x\0\0')


class Table(unittest.TestCase):
    def test_plan(self):
        for compe in (False, True):
            for n in list(range(92, 700)) + [999, css_layout.MAX_TILES]:
                counts, size = css_layout.plan(n, compe)
                self.assertEqual(sum(counts), n)
                self.assertLessEqual(len(counts), 13)
                self.assertGreaterEqual(counts[-1], 1)
                self.assertTrue(all(c == counts[0] for c in counts[:-1]))
                f = css_layout.size_factor(size)
                width = css_layout.COMPE_W if compe else css_layout.NORMAL_COLS * css_layout.CELL_W
                self.assertLessEqual(counts[0] * css_layout.CELL_W * f, width + 0.01, (n, compe))
                if not compe:
                    self.assertLessEqual(len(counts) * css_layout.CELL_H * f, css_layout.NORMAL_ROWS * css_layout.CELL_H + 0.01)
        self.assertEqual(css_layout.plan(147)[0], [17] * 8 + [11])

    def test_rows(self):
        out = css_layout.add_rows(arrangement(91), 150)
        for key in ('normal_layout_param', 'compe_layout_param'):
            rows = rows_of(out, key)
            self.assertEqual([r[0] for r in rows], list(range(1, 151)))
            self.assertEqual(rows[:91], rows_of(arrangement(91), key))   # (the game's kept)
            for n, size, counts in rows[91:]:
                self.assertEqual(sum(counts), n)
                self.assertGreaterEqual(size, css_layout.FIRST_SIZE)

    def test_make(self):
        lay, arr = css_layout.make(120, layout_arc(), arrangement(91))
        files = {n: b for n, b, _a in css_layout.sarc_read(lay)}
        _h, secs = css_layout.sections(files[css_layout.MAIN])
        names = [css_layout.pane_name(s) for s in secs]
        self.assertIn(css_layout.CHR.format(119), names)
        self.assertNotIn(css_layout.CHR.format(120), names)
        for size in range(css_layout.FIRST_SIZE, css_layout.FIRST_SIZE + css_layout.SIZES):
            self.assertIn(css_layout.SIZE_ANIM.format(size), files)
            self.assertIn(f'window_size_{size:02d}\0'.encode(), files[css_layout.INFO])
        self.assertEqual(len(rows_of(arr, 'normal_layout_param')), 120)
        with self.assertRaises(css_layout.Error):
            css_layout.make(91, layout_arc(), arrangement(91))


class FakeTable:
    def __init__(self):
        self.files = {css_layout.LAYOUT: layout_arc(), css_layout.ARRANGEMENT: arrangement(91)}

    def file_targets(self, path):
        return [(path, 0)]

    def original(self, path, _data):
        return self.files[path]


class SelectScreen(unittest.TestCase):
    def tree(self, d, fighters, arranged=False):
        tree = mods.Tree(d)
        os.makedirs(os.path.join(tree.mods, 'new'), exist_ok=True)
        with open(os.path.join(tree.mods, 'new', 'config.json'), 'w') as f:
            json.dump({'new-fighters': {f'f{k}': 'mario' for k in range(fighters)}}, f)
        if arranged:
            own = os.path.join(tree.mods, 'new', *mods.ARRANGEMENT)
            os.makedirs(os.path.dirname(own), exist_ok=True)
            open(own, 'wb').close()
        return tree

    def test_made_kept_gone(self):
        with tempfile.TemporaryDirectory() as d:
            tree = self.tree(d, 9)
            folder = os.path.join(tree.work, 'select_screen')
            said = []
            made = mods.select_screen(tree, ['new'], FakeTable(), folder, said.append)
            self.assertEqual([p for p, _s in made], [css_layout.LAYOUT, css_layout.ARRANGEMENT])
            self.assertTrue(all(os.path.isfile(s) for _p, s in made))
            self.assertIn('the select screen made for 100 tiles', said[-1])
            with open(made[0][1], 'rb') as f:
                files = {n: b for n, b, _a in css_layout.sarc_read(f.read())}
            names = [css_layout.pane_name(s) for s in css_layout.sections(files[css_layout.MAIN])[1]]
            self.assertIn(css_layout.CHR.format(99), names)
            stamp = os.path.getmtime(made[0][1])
            again = mods.select_screen(tree, ['new'], FakeTable(), folder, said.append)
            self.assertEqual(again, made)
            self.assertEqual(os.path.getmtime(made[0][1]), stamp)     # (kept: nothing it is made from changed)
            self.tree(d, 4)
            self.assertEqual(mods.select_screen(tree, ['new'], FakeTable(), folder, said.append), [])
            self.assertFalse(os.path.exists(folder))

    def test_mods_own_arrangement(self):
        with tempfile.TemporaryDirectory() as d:
            tree = self.tree(d, 9, arranged=True)
            said = []
            self.assertEqual(mods.select_screen(tree, ['new'], FakeTable(), os.path.join(tree.work, 'ss'), said.append), [])
            self.assertIn("a mod's own select screen arrangement is used", said[-1])


if __name__ == '__main__':
    unittest.main()
