"""ARCropolis' patch files as tools/mods.py merges them: a .prcx / .prcxml value of another type than
the file's is refused (prc-rs), an .xmsbt in UTF-16 with no byte-order mark and one with a base64
entry are read, and a +<region> name is told from the file's own (ARCropolis' strip_regional).
Files made up here, from the formats' rules."""
from pathlib import Path
import base64
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import msbt  # noqa: E402
import prc  # noqa: E402
import mods  # noqa: E402


def a_prc(value):
    """A param file with one field, "x", of `value` = (kind, v)."""
    return prc.write((prc.STRUCT, prc.Struct([(prc.hash40("x"), value)])))


def a_msbt(labels):
    """An .msbt with each (label, text): LBL1 in one bucket, ATR1 with no attributes, TXT2."""
    lbl = struct.pack("<I2I", 1, len(labels), 12)
    for i, (label, _) in enumerate(labels):
        lbl += bytes([len(label)]) + label.encode() + struct.pack("<I", i)
    atr = struct.pack("<2I", len(labels), 0)
    texts = [t.encode("utf-16-le") + b"\0\0" for _, t in labels]
    offs, at = [], 4 + 4 * len(texts)
    for t in texts:
        offs.append(at)
        at += len(t)
    txt = struct.pack(f"<I{len(texts)}I", len(texts), *offs) + b"".join(texts)
    body = b""
    for magic, data in ((b"LBL1", lbl), (b"ATR1", atr), (b"TXT2", txt)):
        body += magic + struct.pack("<I", len(data)) + b"\0" * 8 + data + b"\xab" * (-len(data) % 16)
    head = bytearray(b"MsgStdBn\xff\xfe\0\0\x01\x03" + struct.pack("<HHI", 3, 0, 0x20 + len(body)) + b"\0" * 10)
    return bytes(head) + body


class PrcPatches(unittest.TestCase):
    def test_same_type_is_patched(self):
        out = prc.read(prc.patch_bytes(a_prc((prc.I16, 5)), a_prc((prc.I16, 9))))
        self.assertEqual(out[1][0][1], (prc.I16, 9))

    def test_other_type_is_refused(self):
        # (a short in the file, an sbyte in the patch: prc-rs' NotSamePType)
        with self.assertRaises(prc.Error):
            prc.patch_bytes(a_prc((prc.I16, 5)), a_prc((prc.I8, 9)))

    def test_xml_other_type_is_refused(self):
        patch = '<?xml version="1.0" encoding="utf-16"?><struct><sbyte hash="x">9</sbyte></struct>'.encode("utf-8")
        with self.assertRaises(prc.Error):
            prc.patch_bytes(a_prc((prc.I16, 5)), patch, xml=True)


class Xmsbt(unittest.TestCase):
    BASE = a_msbt([("a", "one"), ("b", "two")])

    def test_utf16_without_bom(self):
        x = '<?xml version="1.0" encoding="utf-16"?><xmsbt><entry label="a"><text>uno</text></entry></xmsbt>'
        m = msbt.Msbt(msbt.patch_bytes(self.BASE, x.encode("utf-16-le")))
        self.assertEqual(m.text("a"), "uno")
        self.assertEqual(m.text("b"), "two")

    def test_base64_entry(self):
        raw = "Zwei".encode("utf-16-le")
        x = ('<xmsbt><entry label="b" base64="true"><text>%s</text></entry>'
             '<entry label="c"><text>new</text></entry></xmsbt>') % base64.b64encode(raw).decode()
        m = msbt.Msbt(msbt.patch_bytes(self.BASE, x.encode("utf-8")))
        self.assertEqual(m.text("b"), "Zwei")
        self.assertEqual(m.text("c"), "new")

    def test_bad_base64_is_an_error(self):
        x = b'<xmsbt><entry label="b" base64="true"><text>@@@</text></entry></xmsbt>'
        with self.assertRaises(msbt.Error):
            msbt.patch_bytes(self.BASE, x)


class Regions(unittest.TestCase):
    def test_strip_region(self):
        self.assertEqual(mods.strip_region("ui/message/msg_name+us_en.xmsbt"), ("ui/message/msg_name.xmsbt", "us_en"))
        self.assertEqual(mods.strip_region("sound/bank/fighter_voice/vc_mario+JP_JA.nus3audio"),
                         ("sound/bank/fighter_voice/vc_mario.nus3audio", "jp_ja"))

    def test_not_a_region(self):
        for path in ("ui/message/msg_name.xmsbt", "fighter/mario/a+b/c.bin", "ui/message/msg+xx_yy.msbt"):
            self.assertEqual(mods.strip_region(path), (path, None))

    def test_patch_kinds(self):
        for ext, base in ((".stdatx", ".stdat"), (".stdatxml", ".stdat"), (".stprmx", ".stprm"), (".stprmxml", ".stprm")):
            self.assertEqual(mods.PATCHES[ext], base)


if __name__ == "__main__":
    unittest.main()
