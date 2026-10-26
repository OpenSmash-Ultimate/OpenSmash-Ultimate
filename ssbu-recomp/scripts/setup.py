#!/usr/bin/env python3
"""One entry point: from a fresh clone and your own dump to a runnable native build of the game.

    python scripts/setup.py --dump <folder> [--jobs N] [tool locations ...]
    python scripts/setup.py --target linux --dump <folder> [--jobs N]      a native Linux build, in <repo>/build-linux
                                                                            (BUILDING_LINUX.md; scripts/linux/install_deps.sh first)
    python scripts/setup.py --target macos --dump <folder> [--jobs N]      a native macOS build (Apple Silicon, macOS 14.4+), in
                                                                            <repo>/build-macos (BUILDING_MAC.md; scripts/macos/install_mac.sh)

<folder> holds what you dumped from YOUR console (see BUILDING.md):
    <folder>/exefs/    the title's ExeFS files: main  sdk  (rtld, subsdk0, main.npdm are not used), update applied
    <folder>/romfs/    the title's RomFS (data.arc ...), update applied
    optional: <folder>/sysdata/fonts (your console's system fonts; without them the open-source ones of ../third_party/fonts_open
              are used) and <folder>/sysdata/zoneinfo

Nothing of the game is stored in the repository: every step below derives its files from the dump, into <repo>/build
(ignored by git). Steps run in this order and are resumable (a finished step is skipped; --redo <step> repeats it,
--from <step> repeats it and everything after it; the compilers themselves only rebuild what changed):

    deps      check the tools and the dump                    stage    lay the sources out in build/work
    convert   dump -> main.elf / sdk.elf (tools/extract)      modules  game's script modules out of data.arc
    tables    every generated header and table                lift     lift main (recomp_eh.py)
    sdk       lift the system-library functions (sdklift.py)  compile  main game library (clang, -O1)
    nro       lift + compile the script modules               link     host runtime + game library -> exe
    check     self-tests that need no game run

Other commands:  --status (what is done)   --steps (list)   --only <step>   --run (print the command that starts the game)
Environment variables named RECOMP_* / SSBU_* are removed from the children's environment; tool locations come from the
options below, from LLVM_ROOT / SDL3_ROOT / VULKAN_HEADERS / GLSLANG_ROOT / SUYU_ROOT, or from <repo>/deps/<name>.
libopus (the music) is optional: --opus, OPUS_DLL or <repo>/deps/opus; without it the build ends with a warning and the
game's Opus streams are silent.
Dear ImGui v1.91.9b (the in-game settings menu, F1) is optional too: --imgui, IMGUI_ROOT or <repo>/deps/imgui (a folder holding
imgui.h and imgui.cpp); without it the build works and the menu is left out.

With a prebuilt runtime package (--runtime <zip or folder>, made by scripts/make_release.py; scripts/install.py downloads
the one of a GitHub Release) the exe, SDL3.dll and ssbu_glslang.dll come from the package and only the parts made from the
dump are built here, with LLVM/clang 9 alone: no MSVC Build Tools, no Windows SDK, no SDL3 / Vulkan-Headers / glslang /
suyu (RECOMP_NOCRT, tools/lifter/build.py). The package is used only by the tree it was built from (manifest.json).
--nocrt builds the game libraries the same way without a package (the exe is then still linked here, with MSVC).
"""
import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, 'scripts'))
import build_location  # noqa: E402  (the install location chosen in the installer window)
STEPS =['deps', 'stage', 'convert', 'modules', 'tables', 'lift', 'sdk', 'compile', 'nro', 'link', 'check']
# The game builds this port handles, told apart by the size of main's code (the first segment of the converted dump).
# 13.0.4 is the reference build: the game addresses written in the runtime are 13.0.4 addresses, and GA() (runtime/hostrt.c)
# gives their 13.0.5 counterparts (docs/VERSIONS.md says how they were found). sdk is the same module in both builds.
GAME_BUILDS = {0x39c7e90: '13.0.4', 0x39c8440: '13.0.5'}
SDK_TEXT_SIZE = 0x540950
EXE = 'ssbu_native.exe'
# what step_stage copies into build/work: (repository folder, folder in work, only files ending with); the open fonts
# beside the exe, where nnpl.c looks for them, from the merged repository's third_party/fonts_open
STAGE_PLAN = [('runtime', '', None), ('../third_party/fonts_open', 'fonts_open', None), ('tools/lifter', '', None),
              ('tools/nro', 'ro_study', '.py'), ('tools/audit', 'tools_audit', '.py'),
              ('fighter_sdk', 'fighter_sdk', None), ('tests', 'tests', None), ('mods', '', None)]
# files the build writes that may sit in a source folder of an in-tree build (all ignored by git): not part of the tree
GENERATED_NAMES = {'sdklift_gen.c', 'sdkdata.c', 'sdklift_install.h', 'zstd_gen.c', 'nro_registry.c', 'fake_whitelist.h', 'fake_openlist.h',
                   'difftest_gen.c', 'eh_tests.h', 'quad_tests.h', 'sdktest_data.h', 'imports_all.txt', 'sdklift_report.txt'}
LINUX_EXE = 'ssbu_native'


def exe_name(cx):
    return LINUX_EXE if cx.posix else EXE


def lib_ext(cx):
    """the extension of the game and module libraries"""
    return '.dylib' if cx.mac else '.so' if cx.linux else '.dll'


def game_lib(cx):
    return 'ssbu_game_eh_o1' + lib_ext(cx)


# ---------------------------------------------------------------------------------------------- helpers
class Fail(Exception):
    pass


class TimedOut(Fail):
    pass


def say(msg=''):
    print(msg.encode('ascii', 'replace').decode('ascii'), flush=True)


def fmt(secs):
    return '%dm%02ds' % divmod(int(secs), 60) if secs >= 60 else '%.1fs' % secs


def disk_mb(path):
    total = 0
    for root, _, files in os.walk(path):
        for f in files:
            try:
                total += os.path.getsize(os.path.join(root, f))
            except OSError:
                pass
    return total / 1048576.0


def default_jobs():
    """min(24, cpus); off Windows also one job per 4 GB of RAM: a machine that runs out is not slowed down but has processes
    killed (seen with 22 jobs on 31 GB). On Windows the compiler jobs; on Linux / macOS the lifters' worker processes only
    (Ctx.lift_jobs): the compilers there follow the CPU threads and share the RAM by reservations (build_opt.py)."""
    jobs = min(24, os.cpu_count() or 8)
    try:
        if sys.platform == 'darwin':                # macOS: no /proc; hw.memsize is in bytes
            mem = int(subprocess.run(['sysctl', '-n', 'hw.memsize'], capture_output=True, text=True).stdout.strip())
            return max(1, min(jobs, mem // (4 << 30)))
        with open('/proc/meminfo') as f:
            kb = next(int(l.split()[1]) for l in f if l.startswith('MemTotal:'))
        jobs = max(1, min(jobs, kb // (4 << 20)))
    except (OSError, StopIteration, ValueError):
        pass
    return jobs


class Ctx:
    def __init__(self, a):
        self.a = a
        self.linux = getattr(a, 'target', 'windows') == 'linux'
        self.mac = getattr(a, 'target', 'windows') == 'macos'
        self.posix = self.linux or self.mac          # "not Windows": a POSIX host build with clang and pkg-config
        # --build, else the install location chosen in the installer window (build_location.txt), else <repo>/build;
        # --target linux: <repo>/build-linux; --target macos: <repo>/build-macos
        self.build = os.path.abspath(a.build or (os.path.join(REPO, 'build-linux') if self.linux else
                                                 os.path.join(REPO, 'build-macos') if self.mac else build_location.get()))
        self.work = os.path.join(self.build, 'work')
        self.logs = os.path.join(self.build, 'logs')
        self.state_file = os.path.join(self.build, 'state.json')
        self.state = {}
        if os.path.exists(self.state_file):
            with open(self.state_file) as f:
                self.state = json.load(f)
        if self.posix:
            # Linux / macOS: a compiler per CPU thread (at most 24). build_opt.py shares RAM - 4 GB among them by reservations
            # (most files need well under 1 GB, a few many GB), so RAM no longer caps the count: a 16 GB, 8-thread Steam Deck
            # had 3 compilers under the old rule of one per 4 GB. The lifters (Python worker processes, not counted in that
            # sharing) keep the RAM rule.
            self.jobs = a.jobs or min(24, os.cpu_count() or 8)
            self.lift_jobs = min(self.jobs, default_jobs())
        else:
            self.jobs = a.jobs or default_jobs()
            self.lift_jobs = self.jobs
        self.tools = {}                          # located tools, filled by deps
        self.runtime_dir = None                  # the checked runtime package (load_runtime), or None: the exe is built here
        self.nocrt = bool(a.nocrt or a.runtime)  # the game libraries with LLVM alone (tools/lifter/build.py RECOMP_NOCRT)

    @property
    def bin(self):                               # the converted dump of the build found by the convert step
        return os.path.join(self.build, 'binaries', self.state.get('game_version', '13.0.4'))

    def save(self):
        os.makedirs(self.build, exist_ok=True)
        with open(self.state_file, 'w') as f:
            json.dump(self.state, f, indent=1)

    def env(self, **extra):
        env = {k: v for k, v in os.environ.items() if not k.startswith(('RECOMP_', 'SSBU_'))}
        env['PYTHONIOENCODING'] = 'utf-8'
        env['PYTHONUNBUFFERED'] = '1'
        env['RECOMP_BIN'] = self.bin             # where the tools find main.elf and sdk.elf
        t = self.tools
        if 'clang' in t:
            env['RECOMP_CLANG'] = t['clang']
        if 'sdl3' in t:
            env['SDL3_ROOT'] = t['sdl3']
        if 'vulkan' in t:
            env['VULKAN_HEADERS'] = t['vulkan']
        if 'glslang' in t:
            env['GLSLANG_ROOT'] = t['glslang']
        if 'suyu' in t:
            env['SUYU_ROOT'] = t['suyu']
        if 'imgui' in t:
            env['IMGUI_ROOT'] = t['imgui']
        if t.get('pkg_config_path'):               # macOS: Homebrew's keg-only openssl@3 (deps_mac)
            env['PKG_CONFIG_PATH'] = t['pkg_config_path']
        if self.nocrt:
            env['RECOMP_NOCRT'] = '1'
        env.update(extra)
        return env

    def run(self, step, argv, cwd=None, env=None, log=None, ok=(0,), timeout=None):
        """Run a child, its output to logs/<log or step>.log; returns this run's output (its last 256 KB). Raises Fail on a
        bad exit code, TimedOut (a Fail) when it runs longer than timeout seconds (the child is killed)."""
        os.makedirs(self.logs, exist_ok=True)
        path = os.path.join(self.logs, (log or step) + '.log')
        t0 = time.time()
        with open(path, 'ab') as f:
            f.write(('\n$ %s\n' % ' '.join(argv)).encode())
            f.flush()                            # the header goes above the child's output, not after it
            start = f.tell()                     # the log is appended to: only what this child wrote is returned, or a
            try:                                 # caller's check reads the result of an earlier, failed run
                p = subprocess.run(argv, cwd=cwd or self.work, env=env or self.env(), stdout=f, stderr=subprocess.STDOUT, timeout=timeout)
            except subprocess.TimeoutExpired:
                raise TimedOut('%s did not finish in %s (stopped); see %s' % (' '.join(argv[:3]), fmt(timeout), path))
        with open(path, 'rb') as f:
            f.seek(max(start, os.path.getsize(path) - (256 << 10)))
            tail = f.read().decode('utf-8', 'replace')
        if p.returncode not in ok:
            raise Fail('%s failed (exit %s) after %s; see %s\n%s' % (' '.join(argv[:3]), p.returncode, fmt(time.time() - t0), path, tail[-1500:]))
        return tail


def _tree_rows(only=None):
    """[(staged folder, sorted repo-relative paths)] of the sources the build stages (STAGE_PLAN without tests/).
    only: a set of repo-relative paths to keep (the files git tracks), or None for every file on disk."""
    out = []
    for src, _, ext in STAGE_PLAN:
        if src == 'tests':
            continue
        rows = []
        for root, dirs, files in os.walk(os.path.join(REPO, src)):
            dirs[:] = [d for d in dirs if d not in ('__pycache__', 'out')]
            for f in files:
                if (ext and not f.endswith(ext)) or f in GENERATED_NAMES or f.endswith(('.pyc', '.obj', '.lib', '.dll', '.exe', '.pdb', '.map', '.exp')):
                    continue
                rel = os.path.relpath(os.path.join(root, f), REPO).replace(os.sep, '/')
                if only is None or rel in only:
                    rows.append(rel)
        out.append((src, sorted(rows)))
    return out


def _norm_bytes(rel):
    with open(os.path.join(REPO, rel), 'rb') as fh:
        return fh.read().replace(b'\r\n', b'\n')


def tree_digest(only=None):
    """SHA-256 over the sources the build stages (STAGE_PLAN without tests/), line ends normalised: a runtime package
    (scripts/make_release.py) records the digest of the tree its exe was built from, and is used only by that tree.
    (A git checkout and GitHub's "Download ZIP" of the same commit give the same digest: checked 2026-10-20.)"""
    import hashlib
    h = hashlib.sha256()
    for _, rows in _tree_rows(only):
        for rel in rows:
            h.update(rel.encode() + b'\0' + _norm_bytes(rel) + b'\0')
    return h.hexdigest()


def tree_files():
    """{repo-relative path: SHA-256 of its line-end-normalised content} of the same files (manifest 'tree_files', since
    2026-10-20): setup.py then names the files that differ and ignores files that are only added (a mod of your own)."""
    import hashlib
    return {rel: hashlib.sha256(_norm_bytes(rel)).hexdigest() for _, rows in _tree_rows() for rel in rows}


def _git_tracked():
    """The files git tracks in this tree, or None (no git, or not a checkout: a ZIP copy)."""
    if not os.path.isdir(os.path.join(REPO, '.git')):
        return None
    try:
        r = subprocess.run(['git', '-C', REPO, 'ls-files', '-z'], capture_output=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired):
        return None
    return set(r.stdout.decode('utf-8', 'replace').split('\0')) - {''} if r.returncode == 0 else None


def _relation(commit):
    """'newer' / 'older' / 'other' / None: this tree against the package's source commit (needs git and the commit)."""
    if not commit or not os.path.isdir(os.path.join(REPO, '.git')):
        return None
    try:
        def anc(a, b):
            return subprocess.run(['git', '-C', REPO, 'merge-base', '--is-ancestor', a, b], capture_output=True, timeout=60).returncode
        up = anc(commit, 'HEAD')
        if up == 0:
            return 'newer'
        if up == 1:
            return 'older' if anc('HEAD', commit) == 0 else 'other'
    except (OSError, subprocess.TimeoutExpired):
        pass
    return None


def runtime_mismatch(m):
    """None when the package manifest m fits this tree, else the reason in plain words (for people who are not
    programmers: what is different and what to do). Reads only, writes nothing."""
    commit = str(m.get('source_commit') or '')[:7] or '?'
    made = 'the version of the project of commit %s (%s)' % (commit, m.get('built') or 'date unknown')
    rel = _relation(m.get('source_commit'))
    if rel == 'newer':
        what = ('your copy of the project is NEWER than the prebuilt runtime (made from %s), and the newer runtime is not '
                'published yet. Use the copy of that version (GitHub: Releases), or wait for the next runtime package' % made)
    elif rel in ('older', 'other'):
        what = ('your copy of the project is %s the prebuilt runtime (made from %s). Download the latest ZIP of the project '
                '(GitHub: Code > Download ZIP), unpack it into a new folder and start "Install SSBU.bat" there'
                % ('OLDER than' if rel == 'older' else 'a different version from', made))
    else:
        what = ('your copy of the project is older or newer than the prebuilt runtime (made from %s). Download the latest '
                'ZIP of the project (GitHub: Code > Download ZIP), unpack it into a NEW folder (not over the old one) and '
                'start "Install SSBU.bat" there' % made)
    if m.get('game_data_version') != game_data_version():
        return 'runtime package refused: %s (game-data interface %s, this copy has %d)' % (what, m.get('game_data_version'), game_data_version())
    want = m.get('tree_files')
    if isinstance(want, dict) and want:
        have = tree_files()
        missing = sorted(p for p in want if p not in have)
        changed = sorted(p for p in want if p in have and have[p] != want[p])
        if not missing and not changed:
            return None                         # files only added (a mod of your own): the prebuilt exe is not affected
        names = ', '.join((missing + changed)[:4]) + (' ...' if len(missing) + len(changed) > 4 else '')
        if missing and not changed:
            return ('runtime package refused: %d file(s) of the project are missing from your copy (%s): the copy is '
                    'incomplete (an unpacking that stopped, an antivirus, a file deleted by hand). Unpack the ZIP of the '
                    'project again into a new folder and start "Install SSBU.bat" there' % (len(missing), names))
        return 'runtime package refused: %s (%d file(s) differ: %s)' % (what, len(missing) + len(changed), names)
    if m.get('tree_digest') == tree_digest():
        return None
    tracked = _git_tracked()                    # a git checkout with files of your own in the source folders
    if tracked is not None and m.get('tree_digest') == tree_digest(tracked):
        return None
    return ('runtime package refused: %s. (If you added or changed files in runtime, tools\\lifter, tools\\nro, '
            'tools\\audit, fighter_sdk or mods, take them out.)' % what)


def read_manifest(where):
    """manifest.json of a runtime package (zip or folder) without unpacking anything; None if there is none."""
    import zipfile
    try:
        if os.path.isfile(where):
            with zipfile.ZipFile(where) as z:
                return json.loads(z.read('manifest.json').decode('utf-8'))
        with open(os.path.join(where, 'manifest.json'), encoding='utf-8') as f:
            return json.load(f)
    except (OSError, KeyError, ValueError, zipfile.BadZipFile):
        return None


def game_data_version():
    import re
    with open(os.path.join(REPO, 'runtime', 'game_data.h')) as f:
        return int(re.search(r'#define GAME_DATA_VERSION (\d+)', f.read()).group(1))


def sha256_file(path):
    import hashlib
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def load_runtime(cx, where):
    """The runtime package (folder or zip) -> its folder (a zip is unpacked into build/runtime), checked: every file against
    manifest.json, the tree digest and the game-data interface against this tree. Returns the manifest."""
    import zipfile
    where = os.path.abspath(where)
    if os.path.isfile(where):
        dst = os.path.join(cx.build, 'runtime')
        stamp = os.path.join(dst, '.from')
        key = '%s %d %d' % (where, os.path.getsize(where), int(os.path.getmtime(where)))
        if not os.path.exists(stamp) or open(stamp).read() != key:
            shutil.rmtree(dst, ignore_errors=True)
            try:
                with zipfile.ZipFile(where) as z:
                    for n in z.namelist():
                        if '/' in n or '\\' in n or n.startswith('.') or ':' in n:
                            raise Fail('runtime package refused: %s holds an unexpected path %r' % (where, n))
                    z.extractall(dst)
            except (zipfile.BadZipFile, OSError) as e:
                raise Fail('runtime package refused: %s cannot be read (%s): the download is broken. Delete it and start '
                           'the installer again (it downloads it again)' % (where, e))
            with open(stamp, 'w') as f:
                f.write(key)
        where = dst
    mf = os.path.join(where, 'manifest.json')
    if not os.path.isfile(mf):
        raise Fail('runtime package refused: %s is not a runtime package (no manifest.json)' % where)
    with open(mf) as f:
        m = json.load(f)
    for name, info in m.get('files', {}).items():
        p = os.path.join(where, name)
        if not os.path.isfile(p) or sha256_file(p) != info['sha256']:
            raise Fail('runtime package refused: %s is missing or damaged in it (an antivirus may have removed or changed '
                       'it; allow the project folder in the antivirus and start the installer again)' % name)
    for need in (EXE, 'SDL3.dll', 'ssbu_glslang.dll'):
        if need not in m.get('files', {}):
            raise Fail('runtime package refused: %s is not in it' % need)
    # every refusal below says "runtime package refused:" (scripts/install.py looks for it) and what to do, in plain words
    why = runtime_mismatch(m)
    if why and m.get('game_data_version') != game_data_version():
        raise Fail(why + '. Building everything here instead needs the MSVC Build Tools (--no-runtime)')
    if why:
        if not cx.a.runtime_any_tree:
            raise Fail(why + '. Building everything here instead needs the MSVC Build Tools (--no-runtime); '
                       '--runtime-any-tree uses the package anyway (only if the runtime sources did not change)')
        say('  WARNING: the runtime package was built from another version of the sources (--runtime-any-tree)')
    cx.runtime_dir = where
    return m


def find_first(cands, probe):
    for c in cands:
        if c and os.path.isdir(c) and probe(c):
            return os.path.abspath(c)
    return None


# ---------------------------------------------------------------------------------------------- steps
def step_deps(cx):
    a = cx.a
    say('  python %s' % sys.version.split()[0])
    if cx.linux:
        return deps_linux(cx)
    if cx.mac:
        return deps_mac(cx)
    if a.runtime:
        m = load_runtime(cx, a.runtime)
        say('  runtime package: %s (commit %s, built %s): the exe is not built here' % (cx.runtime_dir, str(m.get('source_commit'))[:10], m.get('built')))
    mode = 'package' if a.runtime else 'nocrt' if cx.nocrt else 'msvc'
    if cx.state.get('mode', mode) != mode:               # linked another way than this build folder was: link and check again
        for s in ('link', 'check'):
            cx.state.setdefault('done', {}).pop(s, None)
    cx.state['mode'] = mode
    if a.runtime:                                         # another package than the one linked last time (an update): link and check again
        exe_sha = m.get('files', {}).get(EXE, {}).get('sha256')
        if cx.state.get('runtime_exe') != exe_sha:
            for s in ('link', 'check'):
                cx.state.setdefault('done', {}).pop(s, None)
        cx.state['runtime_exe'] = exe_sha
    missing = []
    # Pillow (gen_sdktest.py) and unicorn (the differential test) are needed only when the exe is built here
    for mod, pkg in (('numpy', 'numpy'), ('lz4.block', 'lz4'), ('zstandard', 'zstandard')) + ((('PIL', 'Pillow'), ('unicorn', 'unicorn')) if not a.runtime else ()):
        try:
            __import__(mod)
        except ImportError:
            missing.append(pkg)
    if missing:
        raise Fail('missing Python packages: %s   (pip install %s)' % (', '.join(missing), ' '.join(missing)))
    say('  python packages: numpy lz4 zstandard%s: ok' % ('' if a.runtime else ' Pillow unicorn'))

    deps = os.path.join(REPO, 'deps')
    def is_9(d):                                   # another LLVM version installed on the PC is not used unless asked for
        if not os.path.exists(os.path.join(d, 'bin', 'clang.exe')):
            return False
        if d in (a.llvm, os.environ.get('LLVM_ROOT')):
            return True
        try:
            v = subprocess.run([os.path.join(d, 'bin', 'clang.exe'), '--version'], capture_output=True, text=True).stdout
        except OSError:
            return False
        return 'version 9.' in v or 'version 8.' in v
    llvm = find_first([a.llvm, os.environ.get('LLVM_ROOT'), os.path.join(deps, 'llvm'), r'C:\llvm-9.0.0', r'C:\Program Files\LLVM'], is_9)
    if not llvm:
        raise Fail('LLVM/clang 9.0.0 not found (--llvm <folder with bin\\clang.exe>)')
    clang = os.path.join(llvm, 'bin', 'clang.exe')
    ver = subprocess.run([clang, '--version'], capture_output=True, text=True).stdout.splitlines()[0]
    say('  clang: %s   [%s]' % (ver, clang))
    if 'version 9.' not in ver and 'version 8.' not in ver:
        say('  WARNING: the lifted code was only ever compiled with clang 8 and 9; %s is untested' % ver)
    cx.tools['clang'] = clang
    probe = os.path.join(cx.build, 'probe')
    os.makedirs(probe, exist_ok=True)
    if cx.nocrt:
        probe_nocrt(clang, probe)
        say('  clang + lld-link without the C runtime (the game libraries): ok')
    cx.tools['msvc'] = probe_msvc(clang, probe)
    if a.runtime:
        say('  MSVC Build Tools / Windows SDK: %s (not needed with the runtime package%s)' % (
            'found' if cx.tools['msvc'] else 'not found', '' if cx.tools['msvc'] else '; the check step skips the differential test'))
        # libopus (the music): the package's own copy, else --opus / OPUS_DLL / <repo>/deps/opus (install.py fetches it there);
        # before 2026-10-20 only the package was looked at, so a package without it left the game silent
        cands = [os.path.join(cx.runtime_dir, n) for n in ('opus.dll', 'libopus-0.dll')]
        for p in (a.opus, os.environ.get('OPUS_DLL'), os.path.join(REPO, 'deps', 'opus')):
            if p and os.path.isfile(p):
                cands.append(p)
            elif p:
                cands += [os.path.join(p, n) for n in ('opus.dll', 'libopus-0.dll', os.path.join('bin', 'opus.dll'), os.path.join('bin', 'libopus-0.dll'))]
        cx.tools['opus'] = next((os.path.abspath(c) for c in cands if os.path.isfile(c)), None)
        say('  libopus  %s' % (cx.tools['opus'] + ' (copied beside the exe)' if cx.tools['opus'] else
                               'NOT FOUND: the music and other Opus sounds will be silent (--opus <opus.dll or its folder>)'))
        dump_checks(cx, a)
        cx.state['tools'] = cx.tools
        return
    if not cx.tools['msvc']:
        raise Fail('clang cannot compile and link a Windows program: install the MSVC Build Tools (C++ x64 tools) and a Windows 10/11 SDK, '
                   'or use a prebuilt runtime package (--runtime, scripts/install.py)')
    say('  clang + MSVC linker + Windows SDK: ok')

    for key, opt, envv, sub, probe_rel in (
            ('sdl3', a.sdl3, 'SDL3_ROOT', 'SDL3', os.path.join('include', 'SDL3', 'SDL.h')),
            ('vulkan', a.vulkan_headers, 'VULKAN_HEADERS', 'Vulkan-Headers', os.path.join('include', 'vulkan', 'vulkan.h')),
            ('glslang', a.glslang, 'GLSLANG_ROOT', 'glslang', os.path.join('include', 'glslang', 'Include', 'glslang_c_interface.h')),
            ('suyu', a.suyu, 'SUYU_ROOT', 'suyu', os.path.join('src', 'core', 'hle', 'service', 'mii', 'types', 'raw_data.cpp'))):
        found = find_first([opt, os.environ.get(envv), os.path.join(deps, sub)], lambda d, p=probe_rel: os.path.exists(os.path.join(d, p)))
        if not found:
            raise Fail('%s not found (--%s <folder containing %s>)' % (sub, {'sdl3': 'sdl3', 'vulkan': 'vulkan-headers', 'glslang': 'glslang', 'suyu': 'suyu'}[key], probe_rel))
        cx.tools[key] = found
        say('  %-8s %s' % (sub, found))
    if not os.path.exists(os.path.join(cx.tools['sdl3'], 'lib', 'x64', 'SDL3.lib')):
        raise Fail('SDL3 folder lacks lib\\x64\\SDL3.lib (use the "VC" development package)')
    if not os.path.exists(os.path.join(cx.tools['glslang'], 'lib', 'glslang.lib')):
        raise Fail('glslang folder lacks lib\\glslang.lib (use the windows-x86_64 release package)')
    vsw = os.path.join(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'), 'Microsoft Visual Studio', 'Installer', 'vswhere.exe')
    vs = subprocess.run([vsw, '-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'],
                        capture_output=True, text=True).stdout.strip() if os.path.exists(vsw) else ''
    if not vs:
        raise Fail('Visual Studio / Build Tools with "MSVC v14x x64/x86 build tools" not found (needed for the glslang shim)')
    say('  MSVC (glslang shim): %s' % vs)

    # libopus: nnaudio.c loads it at run time to decode the game's Opus streams (the music among them). Optional: without it
    # the game runs and those streams are silent, which is easy to miss, so the deps step says so.
    def opus_in(p):
        if p and os.path.isfile(p):
            return os.path.abspath(p)
        for rel in ('opus.dll', 'libopus-0.dll', os.path.join('bin', 'opus.dll'), os.path.join('bin', 'libopus-0.dll'),
                    os.path.join('Release', 'opus.dll'), os.path.join('build', 'Release', 'opus.dll')):
            if p and os.path.isfile(os.path.join(p, rel)):
                return os.path.abspath(os.path.join(p, rel))
        return None
    opus = next((f for f in map(opus_in, [a.opus, os.environ.get('OPUS_DLL'), os.path.join(deps, 'opus')]) if f), None)
    msys = next((p for p in (r'C:\msys64\ucrt64\bin\libopus-0.dll', r'C:\msys64\mingw64\bin\libopus-0.dll', r'C:\msys64\clang64\bin\libopus-0.dll')
                 if os.path.isfile(p)), None)
    if opus:
        cx.tools['opus'] = opus
        say('  libopus  %s (the link step copies it beside the exe)' % opus)
    elif msys:
        say('  libopus  %s (MSYS2: the runtime loads it from there)' % msys)
    else:
        say('  libopus  NOT FOUND: the game will run, but its music and other Opus sounds will be silent (--opus <opus.dll or its folder>, BUILDING.md)')

    # Dear ImGui: the in-game settings menu (F1). Optional: without it the runtime builds with a stub and F1 only logs why.
    found = find_first([a.imgui, os.environ.get('IMGUI_ROOT'), os.path.join(deps, 'imgui')],
                       lambda d: os.path.exists(os.path.join(d, 'imgui.h')) and os.path.exists(os.path.join(d, 'imgui.cpp')))
    if found:
        cx.tools['imgui'] = found
        say('  %-8s %s' % ('imgui', found))
    else:
        say('  WARNING: Dear ImGui not found: the settings menu (F1) will be left out of the build (--imgui <folder with imgui.h>, '
            'IMGUI_ROOT, deps\\imgui, or scripts/install.py fetches it; BUILDING.md)')

    dump_checks(cx, a)
    cx.state['tools'] = cx.tools


def probe_msvc(clang, probe):
    """True when clang can build a Windows program with the MSVC linker and the Windows SDK."""
    with open(os.path.join(probe, 'p.c'), 'w') as f:
        f.write('#include <windows.h>\n#include <stdio.h>\nint main(void){puts("ok");return 0;}\n')
    r = subprocess.run([clang, os.path.join(probe, 'p.c'), '-o', os.path.join(probe, 'p.exe'), '-luser32'], capture_output=True, text=True)
    return not r.returncode and subprocess.run([os.path.join(probe, 'p.exe')], capture_output=True, text=True).stdout.strip() == 'ok'


def probe_nocrt(clang, probe):
    """The game libraries' way (build.py RECOMP_NOCRT): the headers of runtime/nocrt, lld-link, ucrtbase.dll / ntdll.dll."""
    lld = os.path.join(os.path.dirname(clang), 'lld-link.exe')
    if not os.path.isfile(lld):
        raise Fail('%s not found: the LLVM 9.0.0 package has it (bin\\lld-link.exe)' % lld)
    nocrt = os.path.join(REPO, 'runtime', 'nocrt')
    src = os.path.join(probe, 'nocrt_probe.c')
    with open(src, 'w') as f:
        f.write('#include <stdint.h>\n#include <string.h>\n#include <math.h>\n#include <setjmp.h>\nint _fltused;\n'
                '__declspec(dllexport) double probe(double *d, const double *s, float x) { char big[8192]; memset(big, 1, sizeof big);'
                ' memcpy(d, s, 8); return sqrt(*d) + fmaf(x, x, x) + trunc(*d) + big[(int)x & 8191]; }\n')
    obj, dll = os.path.join(probe, 'nocrt_probe.obj'), os.path.join(probe, 'nocrt_probe.dll')
    r = subprocess.run([clang, '-c', '-O1', '-nostdlibinc', '-isystem', nocrt, '-DGAME_NOCRT', src, '-o', obj], capture_output=True, text=True)
    if r.returncode:
        raise Fail('clang cannot compile with the headers of runtime/nocrt:\n' + (r.stdout + r.stderr)[-800:])
    libs = []
    for n in ('ucrtbase_min', 'ntdll_min'):
        lib = os.path.join(probe, n + '.lib')
        r = subprocess.run([lld, '/machine:x64', '/def:' + os.path.join(nocrt, n + '.def'), '/out:' + lib], capture_output=True, text=True)
        if r.returncode:
            raise Fail('lld-link cannot make an import library from %s.def:\n%s' % (n, (r.stdout + r.stderr)[-800:]))
        libs.append(lib)
    r = subprocess.run([lld, '/dll', '/noentry', '/nodefaultlib', '/machine:x64', obj, '/out:' + dll] + libs, capture_output=True, text=True)
    if r.returncode:
        raise Fail('lld-link cannot link a DLL without the C runtime:\n' + (r.stdout + r.stderr)[-800:])


def dump_checks(cx, a):
    # the dump
    check_dump(cx)


LINUX_PKGS = (('sdl3', 'SDL3'), ('freetype2', 'FreeType'), ('openssl', 'OpenSSL'), ('libavformat', 'FFmpeg'), ('libavcodec', 'FFmpeg'),
              ('libswscale', 'FFmpeg'), ('libswresample', 'FFmpeg'), ('libavutil', 'FFmpeg'))
GLSLANG_LIBS = (['-lglslang', '-lglslang-default-resource-limits'],          # shared glslang (Arch, Fedora)
                ['-lglslang', '-lglslang-default-resource-limits', '-lSPIRV', '-lMachineIndependent', '-lGenericCodeGen',
                 '-lOSDependent', '-lSPIRV-Tools-opt', '-lSPIRV-Tools', '-lstdc++', '-lpthread'])      # static glslang (Debian)
LINUX_HINT = '\n(scripts/linux/install_deps.sh installs everything the Linux build needs; see BUILDING_LINUX.md)'


def deps_linux(cx):
    """--target linux: the system's clang and the development packages of SDL3, FreeType, OpenSSL, FFmpeg, glslang, Vulkan
    and OpenGL; the Opus and Vulkan libraries the game loads at run time. scripts/linux/install_deps.sh installs them."""
    a = cx.a
    missing = []
    for mod, pkg in (('numpy', 'numpy'), ('lz4.block', 'lz4'), ('zstandard', 'zstandard'), ('PIL', 'Pillow'), ('unicorn', 'unicorn')):
        try:
            __import__(mod)
        except ImportError:
            missing.append(pkg)
    if missing:
        raise Fail('missing Python packages: %s (run setup.py with deps/venv/bin/python)%s' % (', '.join(missing), LINUX_HINT))
    say('  python packages: numpy lz4 zstandard Pillow unicorn: ok')
    clang = os.path.join(a.llvm, 'bin', 'clang') if a.llvm else shutil.which('clang')
    if not clang or not os.path.exists(clang):
        raise Fail('clang not found (install clang, or --llvm <folder with bin/clang>)' + LINUX_HINT)
    ver = subprocess.run([clang, '--version'], capture_output=True, text=True).stdout.splitlines()[0]
    say('  clang: %s   [%s]' % (ver, clang))
    try:
        major = int(ver.split('version ')[1].split('.')[0])
    except (IndexError, ValueError):
        major = 0
    if major and major < 19:
        raise Fail('clang %d is too old: the Linux build needs clang 19 or newer (C23 typeof_unqual)' % major)
    cx.tools['clang'] = clang
    if not shutil.which('pkg-config'):
        raise Fail('pkg-config not found' + LINUX_HINT)
    for pc, name in LINUX_PKGS:
        r = subprocess.run(['pkg-config', '--modversion', pc], capture_output=True, text=True)
        if r.returncode:
            raise Fail('development files of %s (%s) not found by pkg-config%s' % (name, pc, LINUX_HINT))
        say('  %-13s %s' % (pc, r.stdout.strip()))
    for h, name in (('/usr/include/glslang/Include/glslang_c_interface.h', 'glslang'), ('/usr/include/GL/gl.h', 'OpenGL (libGL)')):
        if not os.path.exists(h):
            raise Fail('%s headers not found (%s)%s' % (name, h, LINUX_HINT))
    # one program linked against every library of the build, and run: a missing library shows here, not after an hour
    probe = os.path.join(cx.build, 'probe')
    os.makedirs(probe, exist_ok=True)
    with open(os.path.join(probe, 'p.c'), 'w') as f:
        f.write('#include <stdio.h>\n#include <SDL3/SDL.h>\n#include <ft2build.h>\n#include FT_FREETYPE_H\n#include <openssl/evp.h>\n'
                '#include <libavcodec/avcodec.h>\n#include <glslang/Include/glslang_c_interface.h>\n#include <GL/gl.h>\n'
                'int main(void) {\n    FT_Library ft;\n    if (FT_Init_FreeType(&ft)) return 1;\n'
                '    printf("%d %p %p %p\\n", SDL_GetVersion(), (void *)EVP_sha256, (void *)glslang_initialize_process, (void *)glGetString);\n'
                '    if (!avcodec_find_decoder(AV_CODEC_ID_VP9)) { puts("noVP9"); return 0; }\n'
                '    if (!avcodec_find_decoder(AV_CODEC_ID_VORBIS)) { puts("noVorbis"); return 0; }\n'
                '    puts("ok");\n    return 0;\n}\n')
    flags = subprocess.run(['pkg-config', '--cflags', '--libs'] + [p for p, _ in LINUX_PKGS], capture_output=True, text=True).stdout.split()
    err = ''
    for libs in GLSLANG_LIBS:
        r = subprocess.run([clang, os.path.join(probe, 'p.c'), '-o', os.path.join(probe, 'p')] + flags + libs + ['-lGL', '-lm'], capture_output=True, text=True)
        if not r.returncode:
            cx.tools['glslang_libs'] = libs
            break
        err = r.stderr
    else:
        raise Fail('a test program using SDL3, FreeType, OpenSSL, FFmpeg, glslang and OpenGL does not build:\n%s%s' % (err[-1200:], LINUX_HINT))
    out = subprocess.run([os.path.join(probe, 'p')], capture_output=True, text=True).stdout.strip().splitlines()
    if out and out[-1].startswith('no'):
        raise Fail('this FFmpeg has no %s decoder: the game\'s movies are VP9 video with Vorbis audio (install the full FFmpeg of your distribution)' % out[-1][2:])
    if not out or out[-1] != 'ok':
        raise Fail('the test program built against the libraries does not run (output %r)' % out)
    say('  test program with SDL3, FreeType, OpenSSL, FFmpeg (VP9 + Vorbis), glslang, OpenGL: builds and runs')
    import ctypes.util
    for lib, name in (('vulkan', 'the Vulkan loader (libvulkan.so.1)'), ('opus', 'the Opus library (libopus.so.0)')):
        if not ctypes.util.find_library(lib):
            raise Fail('%s not found: the game loads it at run time%s' % (name, LINUX_HINT))
    say('  run-time libraries: libvulkan, libopus: ok')
    deps = os.path.join(REPO, 'deps')
    for key, opt, envv, sub, probe_rel in (
            ('vulkan', a.vulkan_headers, 'VULKAN_HEADERS', 'Vulkan-Headers', os.path.join('include', 'vulkan', 'vulkan.h')),
            ('suyu', a.suyu, 'SUYU_ROOT', 'suyu', os.path.join('src', 'core', 'hle', 'service', 'mii', 'types', 'raw_data.cpp'))):
        found = find_first([opt, os.environ.get(envv), os.path.join(deps, sub), '/usr' if key == 'vulkan' else None], lambda d, p=probe_rel: os.path.exists(os.path.join(d, p)))
        if not found:
            raise Fail('%s not found (--%s <folder containing %s>)%s' % (sub, {'vulkan': 'vulkan-headers', 'suyu': 'suyu'}[key], probe_rel, LINUX_HINT))
        cx.tools[key] = found
        say('  %-13s %s' % (sub, found))
    find_imgui(cx, deps)
    if not cx.a.tools_only:
        check_dump(cx)


def find_imgui(cx, deps):
    """Linux / macOS (U2): Dear ImGui for the in-game settings menu (F1), as on Windows. Optional: without it the runtime builds
    with menu_stub.c and F1 only logs why. install_deps.sh / install_mac.sh put it in deps/imgui."""
    found = find_first([cx.a.imgui, os.environ.get('IMGUI_ROOT'), os.path.join(deps, 'imgui')],
                       lambda d: os.path.exists(os.path.join(d, 'imgui.h')) and os.path.exists(os.path.join(d, 'imgui.cpp')))
    if found:
        cx.tools['imgui'] = found
        say('  %-13s %s' % ('imgui', found))
    else:
        say('  WARNING: Dear ImGui not found: the settings menu (F1) will be left out of the build (--imgui <folder with imgui.h>, '
            'IMGUI_ROOT or deps/imgui; the install script fetches it)')


MAC_MIN = '-mmacosx-version-min=14.4'          # os_sync_wait_on_address (runtime/plat_posix.c): macOS 14.4 and newer
MAC_HINT = '\n(scripts/macos/install_mac.sh installs everything the macOS build needs; see BUILDING_MAC.md)'
GLSLANG_LIBS_MAC = (['-lglslang', '-lglslang-default-resource-limits'],                    # Homebrew's shared glslang
                    ['-lglslang', '-lglslang-default-resource-limits', '-lSPIRV', '-lMachineIndependent', '-lGenericCodeGen',
                     '-lOSDependent', '-lSPIRV-Tools-opt', '-lSPIRV-Tools', '-lc++'])      # static glslang


def mac_shim_argv(clang, prefix, libs):
    """The glslang shim (runtime/vk_glslang_shim.c -> ssbu_glslang.dylib), as the link step builds it; run in a folder that holds
    the source. prefix = Homebrew's prefix."""
    return [clang, '-O2', '-dynamiclib', '-fPIC', MAC_MIN, 'vk_glslang_shim.c', '-o', 'ssbu_glslang.dylib',
            '-I' + os.path.join(prefix, 'include'), '-L' + os.path.join(prefix, 'lib')] + list(libs)


def brew_prefix():
    """Homebrew's prefix (/opt/homebrew on Apple Silicon), or None. On a Mac the native /opt/homebrew comes first: an Intel
    Homebrew in /usr/local (migrated from an Intel Mac, or installed from a Rosetta terminal) has x86_64 libraries."""
    native = '/opt/homebrew/bin/brew' if sys.platform == 'darwin' and os.path.exists('/opt/homebrew/bin/brew') else None
    brew = native or shutil.which('brew') or next((p for p in ('/opt/homebrew/bin/brew', '/usr/local/bin/brew') if os.path.exists(p)), None)
    if brew:
        r = subprocess.run([brew, '--prefix'], capture_output=True, text=True)
        if r.returncode == 0 and r.stdout.strip():
            return r.stdout.strip()
    return next((p for p in ('/opt/homebrew', '/usr/local') if os.path.isdir(p)), None)


def deps_mac(cx):
    """--target macos: Apple's (or Homebrew's) clang and the Homebrew development packages of SDL3, FreeType, OpenSSL, FFmpeg
    and glslang; the Vulkan loader, MoltenVK and Opus the game loads at run time. scripts/macos/install_mac.sh installs them."""
    a = cx.a
    missing = []
    for mod, pkg in (('numpy', 'numpy'), ('lz4.block', 'lz4'), ('zstandard', 'zstandard'), ('PIL', 'Pillow'), ('unicorn', 'unicorn')):
        try:
            __import__(mod)
        except ImportError:
            missing.append(pkg)
    if missing:
        raise Fail('missing Python packages: %s (run setup.py with deps/venv/bin/python)%s' % (', '.join(missing), MAC_HINT))
    say('  python packages: numpy lz4 zstandard Pillow unicorn: ok')
    if sys.platform == 'darwin' and subprocess.run(['uname', '-m'], capture_output=True, text=True).stdout.strip() != 'arm64':
        if subprocess.run(['sysctl', '-n', 'hw.optional.arm64'], capture_output=True, text=True).stdout.strip() == '1':
            raise Fail('this Python runs as Intel code under Rosetta on an Apple Silicon Mac: the build would mix x86_64 and arm64. '
                       'Use deps/venv/bin/python made by scripts/macos/install_mac.sh, from a Terminal that is not set to "Open '
                       'using Rosetta"' + MAC_HINT)
        say('  WARNING: this Mac is not an Apple Silicon Mac (arm64): only Apple Silicon is supported')
    prefix = brew_prefix()
    if not prefix:
        raise Fail('Homebrew not found (https://brew.sh)' + MAC_HINT)
    cx.tools['brew_prefix'] = prefix
    clang = os.path.join(a.llvm, 'bin', 'clang') if a.llvm else None
    if not clang:          # /usr/bin/clang: Apple's wrapper, which finds the macOS SDK (the toolchain's own clang does not)
        clang = '/usr/bin/clang' if os.path.exists('/usr/bin/clang') else shutil.which('clang')
    if not clang or not os.path.exists(clang):
        raise Fail('clang not found (install the Xcode Command Line Tools: xcode-select --install; or --llvm <folder with bin/clang>)' + MAC_HINT)
    ver = subprocess.run([clang, '--version'], capture_output=True, text=True).stdout.splitlines()[0]
    say('  clang: %s   [%s]' % (ver, clang))
    try:
        major = int(ver.split('version ')[1].split('.')[0])
    except (IndexError, ValueError):
        major = 0
    if major and major < 15:
        raise Fail('clang %d is too old: Apple clang 15 (Xcode 15 / Command Line Tools 15) or newer is needed' % major)
    cx.tools['clang'] = clang
    if not shutil.which('pkg-config'):
        raise Fail('pkg-config not found (brew install pkgconf)' + MAC_HINT)

    def pc_env(path):
        e = dict(os.environ)
        if path:
            e['PKG_CONFIG_PATH'] = path
        return e
    # openssl@3 is keg-only: pkg-config does not see it unless its folder is on PKG_CONFIG_PATH (the children get it too)
    path = None
    if any(subprocess.run(['pkg-config', '--exists', pc], capture_output=True).returncode for pc, _ in LINUX_PKGS):
        extra = [os.path.join(prefix, 'opt', 'openssl@3', 'lib', 'pkgconfig'), os.path.join(prefix, 'lib', 'pkgconfig'),
                 os.path.join(prefix, 'share', 'pkgconfig')]
        path = os.pathsep.join([p for p in extra if os.path.isdir(p)] + ([os.environ['PKG_CONFIG_PATH']] if os.environ.get('PKG_CONFIG_PATH') else []))
        cx.tools['pkg_config_path'] = path
        say('  PKG_CONFIG_PATH for the build: %s' % path)
    for pc, name in LINUX_PKGS:
        r = subprocess.run(['pkg-config', '--modversion', pc], capture_output=True, text=True, env=pc_env(path))
        if r.returncode:
            raise Fail('development files of %s (%s) not found by pkg-config%s' % (name, pc, MAC_HINT))
        say('  %-13s %s' % (pc, r.stdout.strip()))
    gl_h = os.path.join(prefix, 'include', 'glslang', 'Include', 'glslang_c_interface.h')
    if not os.path.exists(gl_h):
        raise Fail('glslang headers not found (%s; brew install glslang)%s' % (gl_h, MAC_HINT))
    # one program linked against every library of the build, and run: a missing library shows here, not after an hour
    probe = os.path.join(cx.build, 'probe')
    os.makedirs(probe, exist_ok=True)
    with open(os.path.join(probe, 'p.c'), 'w') as f:
        f.write('#include <stdio.h>\n#include <SDL3/SDL.h>\n#include <ft2build.h>\n#include FT_FREETYPE_H\n#include <openssl/evp.h>\n'
                '#include <libavcodec/avcodec.h>\n#include <glslang/Include/glslang_c_interface.h>\n'
                'int main(void) {\n    FT_Library ft;\n    if (FT_Init_FreeType(&ft)) return 1;\n'
                '    printf("%d %p %p\\n", SDL_GetVersion(), (void *)EVP_sha256, (void *)glslang_initialize_process);\n'
                '    if (!avcodec_find_decoder(AV_CODEC_ID_VP9)) { puts("noVP9"); return 0; }\n'
                '    if (!avcodec_find_decoder(AV_CODEC_ID_VORBIS)) { puts("noVorbis"); return 0; }\n'
                '    puts("ok");\n    return 0;\n}\n')
    flags = subprocess.run(['pkg-config', '--cflags', '--libs'] + [p for p, _ in LINUX_PKGS], capture_output=True, text=True, env=pc_env(path)).stdout.split()
    err = ''
    for libs in GLSLANG_LIBS_MAC:
        r = subprocess.run([clang, MAC_MIN, os.path.join(probe, 'p.c'), '-o', os.path.join(probe, 'p'), '-I' + os.path.join(prefix, 'include'),
                            '-L' + os.path.join(prefix, 'lib')] + flags + libs + ['-framework', 'OpenGL', '-lm'], capture_output=True, text=True)
        if not r.returncode:
            cx.tools['glslang_libs'] = libs
            break
        err = r.stderr
    else:
        raise Fail('a test program using SDL3, FreeType, OpenSSL, FFmpeg, glslang and OpenGL does not build:\n%s%s' % (err[-1200:], MAC_HINT))
    out = subprocess.run([os.path.join(probe, 'p')], capture_output=True, text=True).stdout.strip().splitlines()
    if out and out[-1].startswith('no'):
        raise Fail('this FFmpeg has no %s decoder: the game\'s movies are VP9 video with Vorbis audio (brew reinstall ffmpeg)' % out[-1][2:])
    if not out or out[-1] != 'ok':
        raise Fail('the test program built against the libraries does not run (output %r)' % out)
    say('  test program with SDL3, FreeType, OpenSSL, FFmpeg (VP9 + Vorbis), glslang, OpenGL: builds and runs')
    # what the game loads at run time (plat_posix.c looks in Homebrew's lib folder, as dyld does not)
    libdir = os.path.join(prefix, 'lib')
    for names, name in ((('libvulkan.1.dylib',), 'the Vulkan loader (libvulkan.1.dylib; brew install vulkan-loader)'),
                        (('libopus.0.dylib', 'libopus.dylib'), 'the Opus library (libopus.0.dylib; brew install opus)')):
        if not any(os.path.exists(os.path.join(libdir, n)) for n in names):
            raise Fail('%s not found in %s: the game loads it at run time%s' % (name, libdir, MAC_HINT))
    icd = next((p for p in (os.path.join(prefix, 'etc', 'vulkan', 'icd.d', 'MoltenVK_icd.json'),
                            os.path.join(prefix, 'share', 'vulkan', 'icd.d', 'MoltenVK_icd.json')) if os.path.exists(p)), None)
    if not icd:
        raise Fail('the MoltenVK driver file (MoltenVK_icd.json) was not found under %s (brew install molten-vk)%s' % (prefix, MAC_HINT))
    cx.tools['moltenvk_icd'] = icd
    say('  run-time libraries: libvulkan, libopus, MoltenVK (%s): ok' % icd)
    deps = os.path.join(REPO, 'deps')
    for key, opt, envv, sub, probe_rel in (
            ('vulkan', a.vulkan_headers, 'VULKAN_HEADERS', 'Vulkan-Headers', os.path.join('include', 'vulkan', 'vulkan.h')),
            ('suyu', a.suyu, 'SUYU_ROOT', 'suyu', os.path.join('src', 'core', 'hle', 'service', 'mii', 'types', 'raw_data.cpp'))):
        found = find_first([opt, os.environ.get(envv), os.path.join(deps, sub), prefix if key == 'vulkan' else None], lambda d, p=probe_rel: os.path.exists(os.path.join(d, p)))
        if not found:
            raise Fail('%s not found (--%s <folder containing %s>)%s' % (sub, {'vulkan': 'vulkan-headers', 'suyu': 'suyu'}[key], probe_rel, MAC_HINT))
        cx.tools[key] = found
        say('  %-13s %s' % (sub, found))
    find_imgui(cx, deps)
    if not cx.a.tools_only:
        check_dump(cx)


def check_dump(cx):
    a = cx.a
    dump = a.dump
    exefs = a.exefs or (os.path.join(dump, 'exefs') if dump else None)
    romfs = a.romfs or (os.path.join(dump, 'romfs') if dump else None)
    if not exefs or not romfs:
        raise Fail('--dump <folder> is required (or --exefs and --romfs)')
    for f in ('main', 'sdk'):
        if not os.path.isfile(os.path.join(exefs, f)):
            raise Fail('%s not found: the ExeFS folder needs the files main and sdk' % os.path.join(exefs, f))
    if not os.path.isfile(os.path.join(romfs, 'data.arc')):
        raise Fail('%s not found: the RomFS folder needs data.arc' % os.path.join(romfs, 'data.arc'))
    arc, size = os.path.join(romfs, 'data.arc'), 0
    try:                                          # a dump cut short: good header, missing end (testers, 2026-10-20)
        size = os.path.getsize(arc)
        with open(arc, 'rb') as f:
            hdr = struct.unpack('<7Q', f.read(0x38))
            f.seek(hdr[4])
            m, _, comp, _ = struct.unpack('<4I', f.read(16))
        whole = hdr[0] == 0xABCDEF9876543210 and max(hdr[1:6]) + 16 <= size and m == 0x10 and hdr[4] + 16 + comp <= size
    except (OSError, struct.error):
        whole = False
    if not whole:
        raise Fail('%s is incomplete (%.1f GB): the game files were not unpacked to the end (a full disk, or the window '
                   'closed while unpacking). Delete the dump folder and run the installer again, with about 17 GB free.'
                   % (arc, size / 2 ** 30))
    cx.state['dump'] = {'exefs': os.path.abspath(exefs), 'romfs': os.path.abspath(romfs)}
    sysdata = a.sysdata or (os.path.join(dump, 'sysdata') if dump else None)
    if sysdata and os.path.isdir(os.path.join(sysdata, 'fonts')):
        cx.state['dump']['sysdata'] = os.path.abspath(sysdata)
        say('  dump: exefs, romfs, sysdata (fonts%s)' % (', zoneinfo' if os.path.isdir(os.path.join(sysdata, 'zoneinfo')) else ''))
    else:
        say('  dump: exefs, romfs   (no sysdata\\fonts: the open-source fonts of ..\\third_party\\fonts_open are used, staged beside the exe; '
            'no Chinese, Japanese or Korean characters. Your console\'s own: tools/extract/fw_fonts.py, BUILDING.md)')
        shipped = os.path.join(os.path.dirname(REPO), 'third_party', 'fonts_open')
        if not os.path.isdir(shipped) or sum(n.lower().endswith('.ttf') for n in os.listdir(shipped)) < 7:
            raise Fail('..\\third_party\\fonts_open lacks its 7 font files and the dump has no sysdata\\fonts: the game would stop at start. '
                       'Restore the folder (scripts/update.py, git) or make it with tools/extract/open_fonts.py')
    if a.sdk_roots:
        if not os.path.isfile(a.sdk_roots):
            raise Fail('--sdk-roots file not found')
        cx.state['sdk_roots'] = os.path.abspath(a.sdk_roots)
    elif not cx.state.get('sdk_roots'):
        cx.state['sdk_roots'] = os.path.join(REPO, 'tools', 'lifter', 'sdklift_roots.txt')


def copy_if_changed(src, dst):
    if os.path.exists(dst) and os.path.getsize(dst) == os.path.getsize(src):
        with open(src, 'rb') as a, open(dst, 'rb') as b:
            if a.read() == b.read():
                return False
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)
    return True


def step_stage(cx):
    """The sources are laid out the way the tools expect them (one folder, ro_study\\, tools_audit\\, fighter_sdk\\, tests\\),
    next to binaries\\ and romfs: the tools address the dump by relative paths."""
    n = changed = 0
    for src, dst, ext in STAGE_PLAN:
        for root, dirs, files in os.walk(os.path.join(REPO, src)):
            dirs[:] = [d for d in dirs if d != '__pycache__']
            for f in files:
                if ext and not f.endswith(ext):
                    continue
                rel = os.path.relpath(os.path.join(root, f), os.path.join(REPO, src))
                n += 1
                changed += copy_if_changed(os.path.join(root, f), os.path.join(cx.work, dst, rel))
    for d in ('obj_host', os.path.join('tests', 'logs')):
        os.makedirs(os.path.join(cx.work, d), exist_ok=True)
    say('  %d source files, %d copied into %s' % (n, changed, cx.work))
    if changed and getattr(cx, 'state', {}).get('done', {}).get('stage'):     # (make_release.py stages with a bare context)
        # sources changed since the last build (scripts/update.py, an edit): the incremental steps run again and redo only
        # what is older than its sources (objects, module DLLs, the links). A changed lifter still needs --from lift.
        for s in ('compile', 'nro', 'link', 'check'):
            cx.state['done'].pop(s, None)
        say('  sources changed: compile, nro, link and check run again (only what changed is rebuilt)')


def step_convert(cx):
    d = cx.state['dump']
    conv = os.path.join(REPO, 'tools', 'extract', 'nso2elf.py')
    tmp = os.path.join(cx.build, 'binaries', 'converting')
    os.makedirs(tmp, exist_ok=True)
    version = None
    for name in ('main', 'sdk'):
        out = cx.run('convert', [sys.executable, conv, os.path.join(d['exefs'], name), os.path.join(tmp, name + '.elf')], cwd=REPO, log='convert_' + name)
        seg0 = next((l for l in out.splitlines() if l.startswith('seg 0')), '')
        got = int(seg0.split('size ')[1].split()[0], 16) if 'size ' in seg0 else 0
        known = GAME_BUILDS.get(got) if name == 'main' else got == SDK_TEXT_SIZE
        if 'MISMATCH' in out or not known:
            raise Fail('%s is not a build this port knows (code size %#x, or a segment hash mismatch): it handles Super Smash Bros. '
                       'Ultimate %s (update applied). Your dump is from another version: usually an older update (13.0.3 or '
                       'before) or the base game without its update. Fix: give the installer the base game AND the 13.0.4 '
                       '(v1966080) or 13.0.5 update file instead of this dump folder, or dump again with that update installed'
                       % (name, got, ' and '.join(sorted(GAME_BUILDS.values()))))
        if name == 'main':
            version = known
        say('  %s -> %s.elf   code %.1f MB, segment hashes ok' % (name, name, got / 1048576.0))
    if cx.state.get('game_version', version) != version:      # another build than the one the later steps were made from
        for s in STEPS[STEPS.index('convert') + 1:]:
            cx.state.setdefault('done', {}).pop(s, None)
        say('  the dump is %s, the build folder held %s: every later step runs again' % (version, cx.state['game_version']))
    cx.state['game_version'] = version
    os.makedirs(cx.bin, exist_ok=True)
    for name in os.listdir(tmp):
        os.replace(os.path.join(tmp, name), os.path.join(cx.bin, name))
    os.rmdir(tmp)
    say('  game build %s -> %s' % (version, os.path.relpath(cx.bin, cx.build)))
    for name in os.listdir(d['exefs']):                 # main.npdm (main thread stack size) and the other ExeFS files sit beside the ELFs
        if os.path.isfile(os.path.join(d['exefs'], name)):
            shutil.copy2(os.path.join(d['exefs'], name), os.path.join(cx.bin, name))
    if not os.path.isfile(os.path.join(cx.bin, 'main.npdm')):
        raise Fail('exefs\\main.npdm is missing: dump the whole ExeFS (main, sdk, rtld, subsdk0, main.npdm)')
    if d.get('sysdata'):
        for sub in ('fonts', 'zoneinfo'):
            if os.path.isdir(os.path.join(d['sysdata'], sub)):
                dst = os.path.join(cx.build, 'sysdata', sub)
                if not os.path.isdir(dst):
                    shutil.copytree(os.path.join(d['sysdata'], sub), dst)


def step_modules(cx):
    ro = os.path.join(cx.work, 'ro_study')
    env = cx.env(SSBU_ARC=os.path.join(cx.state['dump']['romfs'], 'data.arc'))
    if not os.path.isdir(os.path.join(ro, 'nro')) or not any(f.endswith('.nro') for f in os.listdir(os.path.join(ro, 'nro'))):
        cx.run('modules', [sys.executable, 'arc_extract.py', 'nro', 'nro'], cwd=ro, env=env)
    n = len([f for f in os.listdir(os.path.join(ro, 'nro')) if f.endswith('.nro')])
    cx.run('modules', [sys.executable, 'lift_all.py', 'map'], cwd=ro, env=env)
    cx.run('modules', [sys.executable, 'build_nro.py'], cwd=cx.work)          # writes nro_registry.c from ro_study\nro_map.json
    say('  %d script modules extracted from data.arc, nro_map.json and nro_registry.c written' % n)


STUB_LIST = '/* no audit data: the strict-mode lists are empty (the port runs the same; SSBU_STRICT then reports nothing) */\nstatic const char *const %s[] = {\n    "",\n};\n'


def step_tables(cx):
    w = cx.work
    py = sys.executable
    cx.run('tables', [py, 'gen_imports.py'])
    cx.run('tables', [py, 'gen_sdkdata.py'])
    if not cx.runtime_dir:                    # host-only headers (the self-tests' reference data): not needed with a prebuilt exe
        cx.run('tables', [py, 'gen_sdktest.py'])
        cx.run('tables', [py, 'gen_quad_tests.py'])
    cx.run('tables', [py, 'gen_eh_tests.py'])
    for name, var in (('fake_whitelist.h', 'audit_whitelist'), ('fake_openlist.h', 'audit_openlist')):
        if not os.path.exists(os.path.join(w, name)):
            with open(os.path.join(w, name), 'w', newline='\n') as f:
                f.write(STUB_LIST % var)
    say('  imports_all.txt, sdkdata.c, sdktest_data.h, quad_tests.h, eh_tests.h, strict-mode lists (empty)')


def step_lift(cx):
    out = cx.run('lift', [sys.executable, 'recomp_eh.py', 'gen_eh_o2', str(cx.lift_jobs)])
    n = len([f for f in os.listdir(os.path.join(cx.work, 'gen_eh_o2')) if f.endswith('.c')])
    say('  main lifted into gen_eh_o2: %d C files (%.0f MB)' % (n, disk_mb(os.path.join(cx.work, 'gen_eh_o2'))))


def install_header(roots_text):
    """sdklift_install.h = the names of the managed block of sdklift_roots.txt (what tools/audit/lift_select.py writes)."""
    import re
    blk = re.search(r'# >>> audit block.*?# <<< audit block', roots_text, re.S)
    names = []
    if blk:
        names = [l.strip() for l in blk.group(0).splitlines() if l.strip() and not l.startswith('#') and l.split()[0] not in ('cut', 'lift', 'trap', 'addr')]
    return ('/* generated by scripts/setup.py from the managed block of sdklift_roots.txt: imports whose handler is the real code lifted from sdk.elf,\n'
            ' * installed over the host re-implementation by nnsdk2.c */\nstatic const char *const sdk_install_names[] = {\n' +
            ''.join('    "%s",\n' % n for n in names) + '};\n')


def step_sdk(cx):
    roots = cx.state.get('sdk_roots')
    if not roots or not os.path.isfile(roots):
        raise Fail('the list of SDK functions to lift was not found (tools/lifter/sdklift_roots.txt, or --sdk-roots <file>)')
    shutil.copyfile(roots, os.path.join(cx.work, 'sdklift_roots.txt'))
    with open(roots, encoding='utf-8', errors='replace') as f:
        text = f.read()
    with open(os.path.join(cx.work, 'sdklift_install.h'), 'w', newline='\n') as f:
        f.write(install_header(text))
    out = cx.run('sdk', [sys.executable, 'sdklift.py', 'gen'])
    line = next((l for l in out.splitlines() if l.startswith('functions lifted')), '')
    say('  ' + (line or 'sdklift_gen.c written'))


def inline_flag(cx):
    """RECOMP_INLINE for the game's compile (tools/lifter/build_opt.py): the guest load / store / flag / NEON helpers inlined
    into the lifted code, the main thread about 25-30 % faster; --no-inline turns it off. Objects compiled before keep
    what they were compiled with until they are compiled again."""
    return '0' if getattr(cx.a, 'no_inline', False) else '1'


def build_env(cx):
    if cx.linux:
        return cx.env(RECOMP_GEN='gen_eh_o2', RECOMP_OBJ='obj_eh_o1', RECOMP_DLL=game_lib(cx), RECOMP_NRO_SUFFIX='_eh_o2', RECOMP_TARGET='linux', RECOMP_INLINE=inline_flag(cx),
                      RECOMP_LIFT_JOBS=str(cx.lift_jobs))
    if cx.mac:
        return cx.env(RECOMP_GEN='gen_eh_o2', RECOMP_OBJ='obj_eh_o1', RECOMP_DLL=game_lib(cx), RECOMP_NRO_SUFFIX='_eh_o2', RECOMP_TARGET='macos', RECOMP_INLINE=inline_flag(cx),
                      RECOMP_LIFT_JOBS=str(cx.lift_jobs))
    # FFMPEG_ROOT: build.py compiles the movie fallback (mf_ffmpeg.c) with the headers install.py put in deps\ffmpeg
    return cx.env(RECOMP_GEN='gen_eh_o2', RECOMP_OBJ='obj_eh_o1', RECOMP_DLL='ssbu_game_eh_o1.dll', RECOMP_NRO_SUFFIX='_eh_o2',
                  RECOMP_INLINE=inline_flag(cx),
                  FFMPEG_ROOT=os.environ.get('FFMPEG_ROOT') or os.path.join(REPO, 'deps', 'ffmpeg'))


def copy_logo(cx):
    """media/opensmash.ico beside the exe: display.c loads it as the window's and taskbar's icon; and the loading
    animation's pictures (media/loading_kong.png, loading_kong_back.png, loading_arm.png: runtime/loading_anim.c paints
    them in the window until the game's first frame)."""
    for name in ('opensmash.ico', 'loading_kong.png', 'loading_kong_back.png', 'loading_arm.png'):
        src = os.path.join(REPO, 'media', name)
        if os.path.isfile(src):
            shutil.copy2(src, os.path.join(cx.work, name))


def copy_ffmpeg(cx):
    """The movie fallback's FFmpeg DLLs (deps\\ffmpeg\\bin, install.py) beside the exe, where mf_ffmpeg.c loads them from.
    Returns how many were copied (0: none there; the movies then play only where Windows' own codecs can)."""
    src = os.path.join(os.environ.get('FFMPEG_ROOT') or os.path.join(REPO, 'deps', 'ffmpeg'), 'bin')
    names = [n for n in (os.listdir(src) if os.path.isdir(src) else []) if n.lower().endswith('.dll')]
    for n in names:
        shutil.copy2(os.path.join(src, n), os.path.join(cx.work, n))
    say('  FFmpeg (movie fallback): %s' % ('%d DLLs beside the exe' % len(names) if names else
                                           'not in deps\\ffmpeg: movies only where Windows\' own codecs can play them'))
    return len(names)


def step_compile(cx):
    if cx.posix:
        say('  up to %d compilers at once, sharing the RAM (logs/compile.log shows the progress)' % cx.jobs)
    cx.run('compile', [sys.executable, 'build_opt.py', 'main', str(cx.jobs)], env=build_env(cx))
    dll = os.path.join(cx.work, game_lib(cx))
    say('  %s %.0f MB; objects %.0f MB' % (game_lib(cx), os.path.getsize(dll) / 1048576.0, disk_mb(os.path.join(cx.work, 'obj_eh_o1'))))


def step_nro(cx):
    mods = [m for m in (cx.a.modules or 'all').split(',') if m]
    argv = [sys.executable, 'build_opt.py', 'nro', str(cx.jobs)] + ([] if mods == ['all'] else mods)
    cx.run('nro', argv, env=build_env(cx))
    d = os.path.join(cx.work, 'nro_bin_eh_o2')
    say('  %d module libraries in nro_bin_eh_o2 (%.0f MB)' % (len([f for f in os.listdir(d) if f.endswith(lib_ext(cx))]), disk_mb(d)))


def step_link(cx):
    if cx.linux:
        return link_linux(cx)
    if cx.mac:
        return link_mac(cx)
    if cx.runtime_dir:                        # the prebuilt runtime: copied and checked; only the game DLL is linked here
        cx.run('link', [sys.executable, 'build.py', 'gamedll'], env=cx.env(**{k: v for k, v in build_env(cx).items() if k != 'RECOMP_NRO_SUFFIX'}))
        with open(os.path.join(cx.runtime_dir, 'manifest.json')) as f:
            files = json.load(f)['files']
        for name in files:
            if name.lower().endswith(('.exe', '.dll')):
                shutil.copy2(os.path.join(cx.runtime_dir, name), os.path.join(cx.work, name))
                if sha256_file(os.path.join(cx.work, name)) != files[name]['sha256']:
                    raise Fail('%s does not match the runtime package after copying' % name)
        for f in (EXE, 'SDL3.dll', 'ssbu_glslang.dll', 'ssbu_game_eh_o1.dll'):
            if not os.path.exists(os.path.join(cx.work, f)):
                raise Fail('%s was not produced' % f)
        opus = [n for n in files if n.lower() in ('opus.dll', 'libopus-0.dll')]
        src = cx.tools.get('opus')                      # the deps step's choice: the package's copy or deps\opus (install.py)
        if not opus and src:
            opus = [os.path.basename(src) if os.path.basename(src).lower() in ('opus.dll', 'libopus-0.dll') else 'opus.dll']
            shutil.copy2(src, os.path.join(cx.work, opus[0]))
        say('  %s, SDL3.dll, ssbu_glslang.dll from the runtime package%s; ssbu_game_eh_o1.dll %.0f MB linked here' % (
            EXE, ', ' + opus[0] + ' (music)' if opus else '', os.path.getsize(os.path.join(cx.work, 'ssbu_game_eh_o1.dll')) / 1048576.0))
        if not opus:
            say('  WARNING: no libopus (neither in the runtime package nor in deps\\opus): music and the other Opus sounds will be silent')
        copy_ffmpeg(cx)
        copy_logo(cx)
        return
    cx.run('link', [os.environ.get('COMSPEC', 'cmd.exe'), '/c', os.path.join(cx.work, 'build_glslang.bat')], env=cx.env())
    cx.run('link', [sys.executable, 'build.py', 'link'], env=cx.env(**{k: v for k, v in build_env(cx).items() if k != 'RECOMP_NRO_SUFFIX'}, RECOMP_EXE=EXE))
    exe = os.path.join(cx.work, EXE)
    for f in (EXE, 'SDL3.dll', 'ssbu_glslang.dll', 'ssbu_game_eh_o1.dll'):
        if not os.path.exists(os.path.join(cx.work, f)):
            raise Fail('%s was not produced' % f)
    opus = cx.tools.get('opus')
    if opus:                                            # under a name nnaudio.c looks for (opus.dll or libopus-0.dll)
        name = os.path.basename(opus) if os.path.basename(opus).lower() in ('opus.dll', 'libopus-0.dll') else 'opus.dll'
        shutil.copy2(opus, os.path.join(cx.work, name))
    say('  %s %.1f MB, SDL3.dll, ssbu_glslang.dll, ssbu_game_eh_o1.dll%s beside it' % (EXE, os.path.getsize(exe) / 1048576.0,
                                                                                         ', ' + name if opus else ''))
    if not opus:
        say('  WARNING: no libopus: music and the other Opus sounds will be silent (deps step above; BUILDING.md)')
    copy_ffmpeg(cx)
    copy_logo(cx)


def link_linux(cx):
    cx.run('link', [cx.tools['clang'], '-O2', '-shared', '-fPIC', 'vk_glslang_shim.c', '-o', 'ssbu_glslang.so'] + cx.tools.get('glslang_libs', GLSLANG_LIBS[0]))
    cx.run('link', [sys.executable, 'build.py', 'link'], env=cx.env(**{k: v for k, v in build_env(cx).items() if k != 'RECOMP_NRO_SUFFIX'}, RECOMP_EXE=LINUX_EXE))
    for f in (LINUX_EXE, 'ssbu_glslang.so', game_lib(cx)):
        if not os.path.exists(os.path.join(cx.work, f)):
            raise Fail('%s was not produced' % f)
    say('  %s %.1f MB, ssbu_glslang.so, %s beside it' % (LINUX_EXE, os.path.getsize(os.path.join(cx.work, LINUX_EXE)) / 1048576.0, game_lib(cx)))


def link_mac(cx):
    prefix = cx.tools['brew_prefix']
    libs_list = [cx.tools['glslang_libs']] if cx.tools.get('glslang_libs') else list(GLSLANG_LIBS_MAC)
    for i, libs in enumerate(libs_list):                 # (the deps step already found the working list)
        try:
            cx.run('link', mac_shim_argv(cx.tools['clang'], prefix, libs))
            break
        except Fail:
            if i == len(libs_list) - 1:
                raise
    cx.run('link', [sys.executable, 'build.py', 'link'], env=cx.env(**{k: v for k, v in build_env(cx).items() if k != 'RECOMP_NRO_SUFFIX'}, RECOMP_EXE=LINUX_EXE))
    for f in (LINUX_EXE, 'ssbu_glslang.dylib', game_lib(cx)):
        if not os.path.exists(os.path.join(cx.work, f)):
            raise Fail('%s was not produced' % f)
    say('  %s %.1f MB, ssbu_glslang.dylib, %s beside it' % (LINUX_EXE, os.path.getsize(os.path.join(cx.work, LINUX_EXE)) / 1048576.0, game_lib(cx)))


def game_env(cx, **extra):
    d = cx.state['dump']
    e = cx.env(SSBU_ROMFS=d['romfs'], SSBU_ONLINE='off', **extra)      # self-tests offline: online play is on by default
                                                                         # and replaces the sockets the SDK self-test checks
    if os.path.isdir(os.path.join(cx.build, 'sysdata', 'fonts')):
        e['SSBU_FONTS'] = os.path.join(cx.build, 'sysdata', 'fonts')
    if os.path.isdir(os.path.join(cx.build, 'sysdata', 'zoneinfo')):
        e['SSBU_TZDIR'] = os.path.join(cx.build, 'sysdata', 'zoneinfo')
    if cx.mac and cx.tools.get('moltenvk_icd') and not (e.get('VK_DRIVER_FILES') or e.get('VK_ICD_FILENAMES')):
        e['VK_DRIVER_FILES'] = cx.tools['moltenvk_icd']          # MoltenVK, as play_mac.sh names it for the game
    return e


def step_check(cx):
    exe = os.path.join(cx.work, exe_name(cx))
    main_elf = os.path.join(cx.bin, 'main.elf')
    scratch = os.path.join(cx.build, 'selftest_userdata')
    shutil.rmtree(scratch, ignore_errors=True)
    os.makedirs(scratch)
    # the self-test exits with its number of failures (a crash is a negative exit code: a signal)
    # each game run has a time limit: a self-test that hangs on a PC (testers waited over an hour, 2026-10-20) is stopped
    # and reported, and the install goes on: the game was built, these runs only check it
    try:
        out = cx.run('check', [exe, main_elf, '60'], env=game_env(cx, SSBU_SDK_SELFTEST='1', SSBU_USERDATA=scratch), log='check_sdk',
                     ok=tuple(range(256)), timeout=SELFTEST_LIMIT)
    except TimedOut as e:
        say('  WARNING: the SDK self-test hung and was stopped (%s). The game is installed; if it does not start, send that log' % e)
        out = None
    if out is not None:
        _sdk_result(cx, out)
    for name, var, pick in (('pad', 'SSBU_PAD_SELFTEST', None), ('cxxrt', 'SSBU_CXXRT_SELFTEST', '=== self-test:')):
        try:
            out = cx.run('check', [exe, main_elf, '5'], env=game_env(cx, **{var: '1', 'SSBU_USERDATA': scratch}), log='check_' + name,
                         timeout=SELFTEST_LIMIT)
        except TimedOut as e:
            say('  WARNING: %s hung and was stopped (%s)' % (var, e))
            continue
        if pick is None:
            line = next((l for l in reversed(out.splitlines()) if 'pad' in l.lower() and '/' in l), out.strip().splitlines()[-1] if out.strip() else '')
            say('  %s: %s' % (var, line.strip()))
            continue
        line = next((l for l in reversed(out.splitlines()) if l.startswith(pick)), '')
        # the exception tables of main among the checks (eh_tests.h, read from the game DLL)
        say('  %s: %s' % (var, line.strip() or 'no result line (logs/check_cxxrt.log)'))
        if not line.rstrip().endswith(' 0 failed'):
            say('  WARNING: the C++ runtime self-test reports failures (logs/check_cxxrt.log)')
    _difftest(cx)


SELFTEST_LIMIT = 600                     # seconds per self-test run (a normal one takes about 1 minute)


def _sdk_result(cx, out):
    line = next((l for l in reversed(out.splitlines()) if l.startswith('SDK self-test:')), '')
    failed = [l.strip() for l in out.splitlines() if l.rstrip().endswith('FAILED')]
    known = [l for l in failed if cx.posix and l.startswith('album:')]     # recording movies to the album: not on Linux / macOS yet
    # the on-screen keyboard check drives a real Win32 dialog: on a PC where the self-test's window cannot take the focus
    # (another window in front, a remote or service session) it fails without anything being wrong with the build
    known += [l for l in failed if l.startswith('swkbd: Win32 dialog')]
    if '0 failure(s)' not in line and not (failed and len(known) == len(failed) and ('%d failure(s)' % len(failed)) in line):
        raise Fail('SSBU_SDK_SELFTEST: ' + (line or 'no result line'))
    say('  SSBU_SDK_SELFTEST: ' + line.strip())
    if known:
        say('  (accepted, not a fault of the build: %s)' % '; '.join(l.split(':')[0] + ': ' + ('the on-screen keyboard dialog could not take the focus'
            if l.startswith('swkbd') else 'album movie recording, not in the %s build yet' % ('Linux' if cx.linux else 'macOS')) for l in known))


def _difftest(cx):
    # the lifter's differential test against the Unicorn emulator, on a sample of the instruction shapes of main
    py, w = sys.executable, cx.work
    if not getattr(cx.a, 'full_check', False):
        say('  differential test of the translator: skipped (it is the same on every PC; --full-check runs it)')
        return
    if not cx.tools.get('msvc', True):
        say('  differential test: skipped (it needs the MSVC Build Tools and the Windows SDK, which this PC does not have)')
        return
    cx.run('check', [py, 'difftest.py', 'sample'], log='check_difftest')
    cx.run('check', [py, 'difftest.py', 'gen'], log='check_difftest')
    dt = 'difftest' if cx.posix else 'difftest.exe'
    cx.run('check', [cx.tools['clang'], '-O0', '-w', '-fno-strict-aliasing', '-I', '.', 'difftest_gen.c', 'difftest_host.c', '-o', dt] + (['-lm'] if cx.posix else []), log='check_difftest')
    out = cx.run('check', [py, 'difftest.py', 'check'], env=cx.env(DIFFTEST_EXE=os.path.join(w, dt)), log='check_difftest')
    line = next((l for l in out.splitlines() if l.startswith('tests:')), '')
    if not line:
        raise Fail('difftest printed no result')
    say('  differential test: ' + line.strip())
    if 'different: 0' not in line:
        say('  WARNING: the differential test reports differences (see logs/check_difftest.log)')


RUNNERS = {'deps': step_deps, 'stage': step_stage, 'convert': step_convert, 'modules': step_modules, 'tables': step_tables,
           'lift': step_lift, 'sdk': step_sdk, 'compile': step_compile, 'nro': step_nro,
           'link': step_link, 'check': step_check}
ALWAYS = {'deps', 'stage'}               # cheap, and they fill the in-memory context


def how_to_run(cx):
    d = cx.state.get('dump', {})
    say('')
    say('Start the game (from a shell; the window opens at once, the first run compiles shaders and is slow):')
    if cx.linux:
        say('  deps/venv/bin/python scripts/run.py --linux        (saves in ~/.local/share/ssbu-recomp/userdata)')
        say('  scripts/linux/install_desktop.sh                   adds a launcher to your application menu')
        say('or by hand:   cd %s   and run   ./%s ../binaries/%s/main.elf 0' % (cx.work, LINUX_EXE, cx.state.get('game_version', '13.0.4')))
        say('with the environment variables SSBU_ROMFS=%s  SSBU_USERDATA=<saves>' % d.get('romfs', '<romfs>'))
        return
    if cx.mac:
        say('  ./play_mac.sh                                       (saves in ~/Library/Application Support/ssbu-recomp/userdata;')
        say('                                                       the game log goes to build-macos/logs)')
        say('or:   deps/venv/bin/python scripts/run.py --mac --play')
        say('or by hand:   cd %s   and run   ./%s ../binaries/%s/main.elf 0' % (cx.work, LINUX_EXE, cx.state.get('game_version', '13.0.4')))
        say('with the environment variables SSBU_ROMFS=%s  SSBU_USERDATA=<saves>' % d.get('romfs', '<romfs>'))
        return
    say('  python scripts/run.py --userdata <a folder for your saves> [--seconds 0]')
    say('or by hand:   cd build\\work   and run   %s ..\\binaries\\%s\\main.elf 0' % (EXE, cx.state.get('game_version', '13.0.4')))
    say('with the environment variables SSBU_ROMFS=%s  SSBU_USERDATA=<saves>  (options: BUILDING.md)' % d.get('romfs', '<romfs>'))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--dump', help='folder with exefs\\ and romfs\\ (and optionally sysdata\\) dumped from your console')
    ap.add_argument('--exefs'), ap.add_argument('--romfs'), ap.add_argument('--sysdata')
    ap.add_argument('--target', choices=('windows', 'linux', 'macos'), default='windows', help='windows (default: the Windows exe), linux (a native Linux build: BUILDING_LINUX.md) or macos (Apple Silicon: BUILDING_MAC.md)')
    ap.add_argument('--tools-only', action='store_true', help='only check the tools (step deps, no dump needed; --target linux / macos)')
    ap.add_argument('--build', help='build folder (default: the install location of build_location.txt, else <repo>/build; <repo>/build-linux for --target linux, <repo>/build-macos for --target macos)')
    ap.add_argument('--jobs', type=int, help='parallel compiler jobs (default min(24, cpus); on Windows also at most one per 4 GB of RAM; on Linux / macOS the '
                                                  'compilers share RAM - 4 GB by reservations instead)')
    ap.add_argument('--llvm', help='LLVM 9.0.0 folder (bin\\clang.exe)')
    ap.add_argument('--sdl3', help='SDL3 development package (VC) folder: include\\, lib\\x64\\')
    ap.add_argument('--vulkan-headers', help='Khronos Vulkan-Headers folder (include\\vulkan\\)')
    ap.add_argument('--glslang', help='glslang windows-x86_64 release folder (include\\, lib\\)')
    ap.add_argument('--suyu', help='source checkout of a yuzu-family emulator (only src\\core\\hle\\service\\mii is read)')
    ap.add_argument('--opus', help='libopus DLL (opus.dll or libopus-0.dll) or the folder holding it: the music (optional, BUILDING.md)')
    ap.add_argument('--imgui', help='Dear ImGui v1.91.9b source folder (imgui.h, imgui.cpp): the in-game settings menu (optional; BUILDING.md)')
    ap.add_argument('--sdk-roots', help='another list of SDK functions to lift (default: tools/lifter/sdklift_roots.txt)')
    ap.add_argument('--modules', help='script modules to build: all (default) or a comma list, e.g. lua2cpp_common,lua2cpp_mario')
    ap.add_argument('--runtime', help='prebuilt runtime package (zip or folder, scripts/make_release.py): the exe is not built here, '
                                      'and neither MSVC, the Windows SDK, SDL3, Vulkan-Headers, glslang nor suyu is needed')
    ap.add_argument('--runtime-any-tree', action='store_true', help='use a runtime package made from another version of the sources')
    ap.add_argument('--nocrt', action='store_true', help='build the game libraries with LLVM alone (the exe is still built here)')
    ap.add_argument('--redo', help='repeat this step')
    ap.add_argument('--from', dest='from_step', help='repeat this step and all after it')
    ap.add_argument('--rollback', action='store_true', help='not needed any more (kept so older command lines still work): online '
                    'rollback (F1 > Experimental > Rollback netcode) runs on the normal build')
    ap.add_argument('--no-inline', action='store_true', help='compile the game without the inlined helpers (RECOMP_INLINE): a '
                    'shorter compile on a weak PC, a slower game (the main thread about 25-30 %% slower)')
    ap.add_argument('--full-check', action='store_true', help='the check step also runs the lifter\'s differential test against '
                    'Unicorn (minutes on a fast PC, up to an hour on a weak one; it tests the translator, the same everywhere)')
    ap.add_argument('--only', help='run only this step')
    ap.add_argument('--dry-run', action='store_true', help='check the tools and the dump (step deps), then list what would run')
    ap.add_argument('--status', action='store_true'), ap.add_argument('--steps', action='store_true'), ap.add_argument('--run', action='store_true')
    a = ap.parse_args()
    if a.steps:
        say(' '.join(STEPS))
        return 0
    cx = Ctx(a)
    if a.status:
        for s in STEPS:
            r = cx.state.get('done', {}).get(s)
            say('%-8s %s' % (s, ('done  %s' % fmt(r['secs'])) if r else '-'))
        return 0
    if a.run:
        how_to_run(cx)
        return 0
    todo = STEPS
    done = cx.state.setdefault('done', {})
    force = set()
    for opt in (a.redo, a.from_step, a.only):
        if opt and opt not in STEPS:
            say('unknown step %s (steps: %s)' % (opt, ' '.join(STEPS)))
            return 2
    if a.redo:
        force.add(a.redo)
    if a.from_step:
        force |= set(STEPS[STEPS.index(a.from_step):])
    if a.only:
        todo = sorted({'deps', 'stage', a.only}, key=STEPS.index)
        force.add(a.only)
    if a.tools_only:
        todo = ['deps']
    # a finished build whose game files have gone since (an antivirus quarantines the 500 MB unsigned game DLL, or a
    # copy was interrupted): compile (the objects are kept, so only the DLL is linked again) and link run again
    if 'link' in done and not a.dry_run:
        gone = [f for f in ((EXE, 'ssbu_game_eh_o1.dll', 'SDL3.dll') if not cx.posix else (LINUX_EXE, game_lib(cx), 'ssbu_glslang' + lib_ext(cx)))
                if not os.path.exists(os.path.join(cx.work, f))]
        if gone:
            say('%s missing from %s although the build was finished (an antivirus may have removed it: Windows Security > '
                'Protection history): it is made again' % (', '.join(gone), cx.work))
            for s in ('compile', 'link', 'check'):
                done.pop(s, None)
    if getattr(a, 'rollback', False):
        say('--rollback: not needed any more: online rollback runs on the normal build (no second game DLL, no second set of '
            'script modules); the option is ignored')
    t_all = time.time()
    for i, s in enumerate(STEPS):
        if s not in todo:
            continue
        if s in done and s not in force and s not in ALWAYS:
            say('[%d/%d] %s: done earlier (%s), skipped' % (i + 1, len(STEPS), s, fmt(done[s]['secs'])))
            continue
        if a.dry_run and s != 'deps':
            say('[%d/%d] %s: would run' % (i + 1, len(STEPS), s))
            continue
        say('[%d/%d] %s ...' % (i + 1, len(STEPS), s))
        t0 = time.time()
        try:
            RUNNERS[s](cx)
        except Fail as e:
            say('FAILED at step %s: %s' % (s, e))
            say('fix the problem and run the same command again: finished steps are skipped.')
            cx.save()
            return 1
        secs = time.time() - t0
        if s not in ALWAYS or s not in done:
            done[s] = {'secs': secs, 'at': time.strftime('%Y-%m-%d %H:%M')}
        cx.save()
        say('[%d/%d] %s: ok  %s' % (i + 1, len(STEPS), s, fmt(secs)))
    say('')
    say('all steps finished in %s; build folder %.0f MB' % (fmt(time.time() - t_all), disk_mb(cx.build)))
    how_to_run(cx)
    return 0


if __name__ == '__main__':
    sys.exit(main())
