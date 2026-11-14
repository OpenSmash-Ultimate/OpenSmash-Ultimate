"""A mod's own plugin.nro is only read (tools/skyline.py module_of): a moveset built from Smashline's
template (module smashline_test) gets the character's name in its copy in dump/nro/, never in the
mod's file; and a rebuilt plugin takes its earlier build's place and name. On a made-up NRO."""
from pathlib import Path
import hashlib
import os
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import skyline  # noqa: E402
from nso import Module  # noqa: E402


def nro(name=b'smashline_test', build_id=bytes([0x12, 0x34] * 8)):
    """text [0, 0x200) (the header), rodata [0x200, 0x280) (the name, then MOD0), data [0x280, 0x300)."""
    data = bytearray(0x300)
    struct.pack_into('<II', data, 0, 0, 0x240)
    data[0x10:0x14] = b'NRO0'
    struct.pack_into('<6I', data, 0x20, 0, 0x200, 0x200, 0x80, 0x280, 0x80)
    data[0x40:0x40 + len(build_id)] = build_id
    struct.pack_into('<I', data, 0x204, len(name))
    data[0x208:0x208 + len(name)] = name
    data[0x240:0x244] = b'MOD0'
    struct.pack_into('<6i', data, 0x244, 0x40, 0x40, 0x40, 0, 0, 0)   # (dynamic at 0x280; no bss, no eh)
    struct.pack_into('<QQQQQQ', data, 0x280, 5, 0x2f0, 6, 0x2f0, 0, 0)
    return bytes(data)


class PluginUntouched(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.p = skyline.Paths(self.tmp.name)
        os.makedirs(self.p.nro)
        self.source = os.path.join(self.tmp.name, 'mods', 'Gogeta', 'plugin.nro')
        os.makedirs(os.path.dirname(self.source))
        Path(self.source).write_bytes(nro())
        self.sha = hashlib.sha256(nro()).hexdigest()

    def tearDown(self):
        self.tmp.cleanup()

    def entry(self, name):
        return {'module': name, 'mod': 'Gogeta', 'file': os.path.relpath(self.source, self.tmp.name)}

    def test_template_named_in_copy_only(self):
        build_id, name = skyline.module_of(self.p, self.source, {}, 'gogeta')
        self.assertEqual(hashlib.sha256(Path(self.source).read_bytes()).hexdigest(), self.sha)
        placed = os.path.join(self.p.nro, name + '.nro')
        self.assertEqual(Module(placed).module_name(), 'gogeta')
        self.assertEqual(Module(placed).build_id, build_id)

        # (the next sync: the copy already holds those bytes, so it is not written again)
        record = {build_id: {'module': name}}
        before = os.stat(placed).st_mtime_ns
        self.assertEqual(skyline.module_of(self.p, self.source, record, 'gogeta'), (build_id, name))
        self.assertEqual(os.stat(placed).st_mtime_ns, before)
        self.assertEqual(hashlib.sha256(Path(self.source).read_bytes()).hexdigest(), self.sha)

    def test_new_build_replaces_old(self):
        record = {}
        first, name = skyline.module_of(self.p, self.source, record, 'gogeta')
        record[first] = self.entry(name)
        for build_id in (bytes([0x56, 0x78] * 8), bytes([0x9a, 0xbc] * 8)):   # (the plugin edited and rebuilt, twice)
            Path(self.source).write_bytes(nro(build_id=build_id))
            new, again = skyline.module_of(self.p, self.source, record, 'gogeta')
            self.assertEqual(again, 'gogeta')
            record[new] = self.entry(again)
            self.assertEqual(list(record), [new])
        rows = [line.split() for line in Path(self.p.nro, 'bases.txt').read_text().splitlines()]
        self.assertEqual([(r[0], r[3]) for r in rows], [('gogeta', new)])
        self.assertEqual(sorted(os.listdir(self.p.nro)), ['bases.txt', 'gogeta.nro'])
        self.assertEqual(Module(os.path.join(self.p.nro, 'gogeta.nro')).build_id, new)

    def test_already_suffixed_name_kept(self):
        # (a folder from before: the earlier build got gogeta_ because an older one held gogeta)
        record = {}
        Path(self.source).write_bytes(nro(name=b'gogeta_'))
        first, name = skyline.module_of(self.p, self.source, record, 'gogeta')
        self.assertEqual(name, 'gogeta_')
        record[first] = self.entry(name)
        Path(self.source).write_bytes(nro(name=b'gogeta_', build_id=bytes([0x56, 0x78] * 8)))
        self.assertEqual(skyline.module_of(self.p, self.source, record, 'gogeta')[1], 'gogeta_')

    def test_with_module_name_pads_to_mod0(self):
        data = nro()
        out = skyline.with_module_name(data, Module(self.source), 'ab')
        self.assertEqual(out[0x204:0x240], struct.pack('<I', 2) + b'ab' + bytes(0x240 - 0x20a))
        self.assertEqual(out[:0x204], data[:0x204])
        self.assertEqual(out[0x240:], data[0x240:])


if __name__ == '__main__':
    unittest.main()
