"""Extract the system shared fonts from a Switch firmware dump.

    python fw_fonts.py <firmware_dir> <output_dir>

Looks through the firmware's NCA files for the system-data titles that hold the shared fonts
(0100000000000810..0100000000000814), decrypts their RomFS and writes each .bfttf font both
as stored and decoded to a plain TrueType/OpenType file. The decoding is checked: a decoded
file must start with a valid sfnt signature.
"""
import os
import struct
import sys

from Crypto.Cipher import AES

import romfs_extract as rx

FONT_TITLES = set(range(0x0100000000000810, 0x0100000000000815))
KAEK = ['application', 'ocean', 'system']
SFNT = (b'\x00\x01\x00\x00', b'OTTO', b'ttcf', b'true')
BFTTF_PLAIN_MAGIC = 0x7f9a0218        # first word of a decoded .bfttf header


class LooseNca:
    """Minimal stand-in for romfs_extract.Nsp + Nca over a loose .nca file."""

    def __init__(self, path):
        self.f = open(path, 'rb')
        h = rx.xts_decrypt(rx.KEYS['header_key'], self.f.read(0xC00), 0)
        self.ok = h[0x200:0x204] == b'NCA3'
        if not self.ok:
            return
        self.header = h
        self.title_id = struct.unpack('<Q', h[0x210:0x218])[0]
        self.content_type = h[0x205]
        gen = max(max(h[0x206], h[0x220]) - 1, 0)
        self.key_name = 'key_area_key_%s_%02x' % (KAEK[h[0x207]] if h[0x207] < len(KAEK) else 'application', gen)
        kaek = rx.KEYS.get(self.key_name)
        self.key = AES.new(kaek, AES.MODE_ECB).decrypt(h[0x300:0x340])[0x20:0x30] if kaek else None
        self.sections = []
        for i in range(4):
            start, end = struct.unpack('<II', h[0x240 + i * 16:0x248 + i * 16])
            if end:
                fs = h[0x400 + i * 0x200:0x600 + i * 0x200]
                self.sections.append({'offset': start * 0x200, 'size': (end - start) * 0x200, 'fs': fs,
                                      'type': fs[2], 'crypto': fs[4], 'ctr': fs[0x140:0x148][::-1]})

    def read(self, sec, offset, size):
        absolute = sec['offset'] + offset
        aligned = absolute & ~0xF
        self.f.seek(aligned)
        data = self.f.read((absolute - aligned + size + 15) & ~0xF)
        if sec['crypto'] == 1:
            plain = data
        else:
            counter = int.from_bytes(sec['ctr'] + struct.pack('>Q', aligned >> 4), 'big')
            plain = AES.new(self.key, AES.MODE_CTR, nonce=b'', initial_value=counter).decrypt(data)
        return plain[absolute - aligned:absolute - aligned + size]


class SectionReader:
    def __init__(self, nca, sec):
        self.nca, self.sec = nca, sec

    def read(self, offset, size):
        return self.nca.read(self.sec, offset, size)


def decode_bfttf(data):
    """Each big-endian 32-bit word is XORed with a key; the key follows from the known header word.
    The header is two words (magic, font size); the font itself follows."""
    words = struct.unpack('>%dI' % (len(data) // 4), data[:len(data) // 4 * 4])
    key = words[0] ^ BFTTF_PLAIN_MAGIC
    size = words[1] ^ key
    out = struct.pack('>%dI' % (len(words) - 2), *[w ^ key for w in words[2:]])
    return out[:size] if size <= len(out) else b''


def extract(fw, out_dir, say=print):
    opened = []
    try:
        return _extract(fw, out_dir, say, opened)
    finally:
        for f in opened:
            f.close()


def _extract(fw, out_dir, say, opened):
    """Write the fonts of the firmware folder fw to out_dir. -> (fonts written, problems), problems being sentences
    that name the font title (and the key) that could not be opened: the usual cause is a prod.keys older than the
    firmware (a newer console update needs the newer key generation), never a missing file."""
    os.makedirs(out_dir, exist_ok=True)
    found = 0
    problems = []
    seen = set()                              # font titles read (the same title may have several NCAs: old and new)
    if 'header_key' not in rx.KEYS:
        return 0, ['prod.keys not found or without header_key: the firmware cannot be opened']
    for name in sorted(os.listdir(fw)):
        if not name.lower().endswith('.nca') or name.lower().endswith('.cnmt.nca'):
            continue
        path = os.path.join(fw, name)
        if os.path.isdir(path):              # Ryujinx (bis\system\Contents\registered): each .nca is a folder; a small
            parts = sorted(os.listdir(path))  # one holds a single part "00", a big one 00, 01, ... (not a font: skipped)
            if parts != ['00']:
                continue
            path = os.path.join(path, '00')
        try:
            nca = LooseNca(path)
        except (OSError, ValueError, struct.error) as e:
            say('%s: skipped (%s)' % (name, e))
            continue
        opened.append(nca.f)
        if not nca.ok or nca.title_id not in FONT_TITLES:
            continue
        if nca.key is None:
            problems.append('font title %016x (%s): prod.keys has no %s, so this firmware is newer than your keys; '
                            'dump prod.keys again from the console after its update (Lockpick_RCM)'
                            % (nca.title_id, name, nca.key_name))
            continue
        sec = next((s for s in nca.sections if s['type'] == 0), None)
        if sec is None:
            continue
        level6 = struct.unpack('<Q', sec['fs'][0x8 + 0x10 + 5 * 0x18:0x8 + 0x18 + 5 * 0x18])[0]
        reader = SectionReader(nca, sec)
        for path, offset, size in rx.walk(reader, level6):
            raw = reader.read(offset, size)
            base = os.path.basename(path)
            decoded = decode_bfttf(raw) if base.endswith('.bfttf') else raw
            ok = decoded[:4] in SFNT
            target = os.path.join(out_dir, os.path.splitext(base)[0] + ('.ttf' if base.endswith('.bfttf') else os.path.splitext(base)[1]))
            if ok:
                open(target, 'wb').write(decoded)
                found += 1
                seen.add(nca.title_id)
            say('%016x  %-40s %9d bytes  %s' % (nca.title_id, path, size,
                'decoded OK (%s)' % decoded[:4].hex() if ok else 'NOT a valid font after decoding: %s' % decoded[:8].hex()))
    missing = sorted(FONT_TITLES - seen)
    if not found and not problems:
        problems.append('no font title (%s) found among the NCA files of %s: is this the firmware folder? (it has the '
                        '.nca files Goldleaf or NXDumpTool wrote)' % (', '.join('%016x' % t for t in missing), fw))
    return found, problems


def main():
    fw, out_dir = sys.argv[1], sys.argv[2]
    found, problems = extract(fw, out_dir)
    for p in problems:
        print('note: ' + p)
    print('%d fonts written to %s' % (found, out_dir))
    if not found:
        sys.exit(2)


if __name__ == '__main__':
    main()
