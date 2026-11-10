"""Build optional Windows yuzu ROOM CLIENT libraries from pinned upstream sources.
No room server is built or installed. Run from any directory; output: build/yuzu/.
"""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parent.parent
DEPS = ROOT / 'deps' / 'yuzu-client'
OUTPUT = ROOT / 'build' / 'yuzu'
SOURCES = (
    ('enet', '1.3.18', 'https://codeload.github.com/lsalzman/enet/tar.gz/refs/tags/v1.3.18',
     '28603c895f9ed24a846478180ee72c7376b39b4bb1287b73877e5eae7d96b0dd'),
    ('zstd', '1.5.7', 'https://codeload.github.com/facebook/zstd/tar.gz/refs/tags/v1.5.7',
     '37d7284556b20954e56e1ca85b80226768902e2edabd3b649e9e72c0c9012ee3'),
)

def fetch(name, version, url, expected):
    DEPS.mkdir(parents=True, exist_ok=True)
    archive = DEPS / (name + '-' + version + '.tar.gz')
    if not archive.exists():
        print('Downloading', name, version, flush=True)
        with urllib.request.urlopen(url, timeout=60) as response:
            data = response.read()
        if hashlib.sha256(data).hexdigest() != expected:
            raise RuntimeError('Download checksum mismatch: ' + name)
        archive.write_bytes(data)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != expected:
        raise RuntimeError('Cached archive checksum mismatch: ' + str(archive))
    folder = DEPS / name
    # Extract only regular source/license files, never archive paths or links.
    with tarfile.open(archive) as source:
        for item in source:
            parts = Path(item.name).parts
            if not item.isfile() or len(parts) < 2:
                continue
            relative = Path(*parts[1:])
            if relative.is_absolute() or '..' in relative.parts:
                raise RuntimeError('Invalid archive path')
            rel = relative.as_posix()
            wanted = ((name == 'enet' and (rel.startswith('include/') or '/' not in rel and rel.endswith('.c') or rel == 'LICENSE'))
                      or (name == 'zstd' and (rel.startswith('lib/') or rel in ('LICENSE', 'COPYING'))))
            if wanted:
                target = folder / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                data = source.extractfile(item).read()
                if not target.exists() or target.read_bytes() != data:
                    target.write_bytes(data)
    return folder


def build(jobs=4):
    if os.name != 'nt':
        raise RuntimeError('The first yuzu-compatible client build targets Windows.')
    import cc
    clang = cc.CLANG
    if not Path(clang).is_file() and not shutil.which(clang):
        local = ROOT / 'deps' / 'llvm' / 'bin' / 'clang-cl.exe'
        if not local.is_file():
            raise RuntimeError('clang-cl is required; run the project installer first.')
        clang = str(local)
    OUTPUT.mkdir(parents=True, exist_ok=True)
    for name, version, url, digest in SOURCES:
        source = fetch(name, version, url, digest)
        objdir = DEPS / ('objects-' + name)
        objdir.mkdir(parents=True, exist_ok=True)
        if name == 'enet':
            units = sorted(p for p in source.glob('*.c') if p.name != 'unix.c')
            flags = ['/DENET_DLL', '/DENET_BUILDING_LIB', '/I' + str(source / 'include')]
            libraries = ['ws2_32.lib', 'winmm.lib']
            output = OUTPUT / 'enet.dll'
        else:
            units = sorted(p for folder in ('common', 'compress', 'decompress') for p in (source / 'lib' / folder).glob('*.c'))
            flags = ['/DZSTD_DLL_EXPORT=1', '/DZSTD_LEGACY_SUPPORT=0', '/I' + str(source / 'lib')]
            libraries = []
            output = OUTPUT / 'libzstd.dll'
        print('Building', name, version, '(' + str(len(units)) + ' C files)', flush=True)
        def compile_one(src):
            obj = objdir / (src.parent.name + '-' + src.stem + '.obj')
            subprocess.run([clang, '/nologo', '/O2', '/MT', '/DNDEBUG', *cc.LINK_FLAGS, *flags,
                            '/c', str(src), '/Fo' + str(obj)], check=True, capture_output=True)
            return obj
        with ThreadPoolExecutor(max_workers=jobs) as pool:
            objects = list(pool.map(compile_one, units))
        subprocess.run([clang, '/nologo', '/LD', *cc.LINK_FLAGS, *map(str, objects), '/Fe' + str(output),
                        '/link', *cc.LINK_OPTIONS, *libraries, '/IMPLIB:' + str(objdir / (name + '.lib'))], check=True)
        for license_name in ('LICENSE', 'COPYING'):
            origin = source / license_name
            if origin.is_file():
                shutil.copyfile(origin, OUTPUT / (name + '-' + license_name + '.txt'))
    (OUTPUT / 'versions.json').write_text(json.dumps([
        dict(name=n, version=v, url=u, sha256=h) for n, v, u, h in SOURCES], indent=2), encoding='utf-8')
    print('Client libraries ready:', OUTPUT, flush=True)
    return OUTPUT


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    try:
        build(max(1, min(16, args.jobs)))
    except subprocess.CalledProcessError as error:
        print((error.stdout or b'').decode(errors='replace'), file=sys.stderr)
        print((error.stderr or b'').decode(errors='replace'), file=sys.stderr)
        sys.exit(error.returncode or 1)
