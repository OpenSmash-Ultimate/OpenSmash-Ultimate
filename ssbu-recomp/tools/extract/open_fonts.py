"""Make the open-source replacement system fonts (../third_party/fonts_open) from their published source.

    python open_fonts.py <output_dir> [--from <folder with the six .cpp files>]

The game asks the system for 7 shared fonts. A console has them in its firmware (tools/extract/fw_fonts.py reads
them from a firmware dump). Without one, the runtime uses open-source fonts in their place: the ones the yuzu
project put together for the same purpose (Open Sans, Source Sans Pro and its own small font of button icons),
kept in the suyu repository as C++ arrays of the plain TrueType bytes. This script downloads those six source
files at one fixed commit (or reads them from --from), checks each against its pinned SHA-256, turns the array
back into the .ttf it holds and writes it under the name the runtime (nnpl.c) asks for. The output is byte for
byte the files in ../third_party/fonts_open; their licences are in ../third_party/fonts_open/README.md.
Only the Python standard library is needed.
"""
import hashlib
import os
import re
import sys
import urllib.request

COMMIT = 'c0dd2ff554327657f0c6318f29b740ac7e329ea0'
BASE = 'https://raw.githubusercontent.com/suyu-emu/suyu-v0.0.4/%s/src/core/file_sys/system_archive/data/' % COMMIT
# source file -> SHA-256 of the .cpp as published at COMMIT
SOURCES = {
    'font_standard.cpp': '0355fb64938bd66d96a326ab0681d551803281fe641078bf42cf84e729e5780c',
    'font_chinese_simplified.cpp': 'f91d1223e5c79509daf11318bf83e8ca5ed3626a0d2c6ee86d3b202f303bc0e5',
    'font_extended_chinese_simplified.cpp': 'da47566f0dd8b5e28129b1dca7b2bb5f5b2af5c1841552742ed48cb27e9f4200',
    'font_chinese_traditional.cpp': 'bc6d49f2c471861e61e6a08c952088690c2bee52ef443a4fdc9a36af816b2394',
    'font_korean.cpp': '2cb950e2de8ff3ff79c2bd88e22f7eaa70063a7c50abbd06698c81f74bf52a63',
    'font_nintendo_extended.cpp': '6fbbf5176ff4fd2024f9d150c889477f6b45ece4ac5e45d51286e32f8b13a6f2',
}
# the console's file name (what nnpl.c opens) -> the source it is made from
OUTPUTS = {
    'nintendo_udsg-r_std_003.ttf': 'font_standard.cpp',
    'nintendo_udsg-r_org_zh-cn_003.ttf': 'font_chinese_simplified.cpp',
    'nintendo_udsg-r_ext_zh-cn_003.ttf': 'font_extended_chinese_simplified.cpp',
    'nintendo_udjxh-db_zh-tw_003.ttf': 'font_chinese_traditional.cpp',
    'nintendo_udsg-r_ko_003.ttf': 'font_korean.cpp',
    'nintendo_ext_003.ttf': 'font_nintendo_extended.cpp',
    'nintendo_ext2_003.ttf': 'font_nintendo_extended.cpp',
}
SFNT = (b'\x00\x01\x00\x00', b'OTTO', b'true')
ARRAY = re.compile(r'std::array<unsigned char,\s*(\d+)>\s*\w+\s*\{\{(.*?)\}\};', re.S)


def fetch(name, local):
    if local:
        with open(os.path.join(local, name), 'rb') as f:
            data = f.read()
    else:
        with urllib.request.urlopen(BASE + name, timeout=60) as r:
            data = r.read()
    got = hashlib.sha256(data).hexdigest()
    if got != SOURCES[name]:
        sys.exit('%s: SHA-256 %s, expected %s: not the pinned source, stopping' % (name, got, SOURCES[name]))
    return data


def ttf_of(name, source):
    m = ARRAY.search(source.decode('utf-8', 'replace'))
    if not m:
        sys.exit('%s: no byte array found' % name)
    data = bytes(int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})', m.group(2)))
    if len(data) != int(m.group(1)) or data[:4] not in SFNT:
        sys.exit('%s: the array does not hold a TrueType font (%d bytes of %s)' % (name, len(data), m.group(1)))
    return data


def make(out, local=None):
    os.makedirs(out, exist_ok=True)
    fonts = {}
    for name in SOURCES:
        fonts[name] = ttf_of(name, fetch(name, local))
        print('  %s: %d bytes of font, source SHA-256 checked' % (name, len(fonts[name])))
    for target, name in OUTPUTS.items():
        with open(os.path.join(out, target), 'wb') as f:
            f.write(fonts[name])
        print('  -> %s (%s)' % (target, name))
    return list(OUTPUTS)


def main():
    args = sys.argv[1:]
    local = None
    if '--from' in args:
        i = args.index('--from')
        local = args[i + 1]
        del args[i:i + 2]
    if len(args) != 1:
        sys.exit(__doc__)
    print('open-source system fonts from suyu %s%s' % (COMMIT[:12], ' (local copies in %s)' % local if local else ''))
    make(args[0], local)
    print('7 font files written to %s' % args[0])


if __name__ == '__main__':
    main()
