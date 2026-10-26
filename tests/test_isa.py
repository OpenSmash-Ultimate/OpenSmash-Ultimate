"""The instruction set the game is compiled for (tools/isa.py): which one, from made-up processors and
SSBU_ISA; the build folder's record, and a change of it building again; each one's flags, and what the
compilers make of them (runtime/cpu_check.h's bits); opus without its AVX2 code at x86-64-v2. Then,
where this machine can build and run x86-64 code (Windows and Linux on x86-64; Rosetta on a Mac):
runtime/cpu_check.c compiled for any x86-64 processor and run before main, and hand-assembled
instruction words lifted (tools/lift.py) and compiled at x86-64-v2 - no AVX instruction, the same
results as at avx2, and fma() exactly rounded. Made-up processors and instruction words only."""
from fractions import Fraction
from pathlib import Path
from unittest import mock
import math
import os
import platform
import random
import re
import runpy
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parent.parent
RUNTIME = ROOT / 'runtime'
sys.path.insert(0, str(ROOT / 'tools'))
import cc  # noqa: E402
import codec_libs  # noqa: E402
import isa  # noqa: E402
import lift  # noqa: E402

AVX2 = frozenset(isa.NEEDS['avx2'])
V2 = frozenset(isa.NEEDS['x86-64-v2'])
OLD = V2 - {'sse4.2', 'popcnt'}   # (a processor from before x86-64-v2)


class ChooseTests(unittest.TestCase):
    def test_by_the_processor(self):
        self.assertEqual(isa.choose({}, AVX2, None)[:2], ('avx2', False))
        target, told, why = isa.choose({}, V2, None)
        self.assertEqual((target, told), ('x86-64-v2', False))
        self.assertIn('lacks avx, avx2, fma, bmi1, bmi2, lzcnt', why)
        self.assertIn('expected to be slower', why)
        target, _told, why = isa.choose({}, AVX2 - {'bmi2'}, None)   # (one missing is enough)
        self.assertEqual((target, why.split(':')[0]), ('x86-64-v2', 'this processor lacks bmi2'))
        target, _told, why = isa.choose({}, OLD, None)
        self.assertEqual(target, 'x86-64-v2')
        self.assertIn('lacks sse4.2, popcnt: WindowSmash cannot run on it', why)
        # not known: x86-64-v2, saying how to build for AVX2
        target, told, why = isa.choose({}, None, None)
        self.assertEqual((target, told), ('x86-64-v2', False))
        self.assertIn('SSBU_ISA=avx2', why)

    def test_ssbu_isa(self):
        self.assertEqual(isa.choose({'SSBU_ISA': 'avx2'}, V2, None), ('avx2', True, 'SSBU_ISA'))
        self.assertEqual(isa.choose({'SSBU_ISA': 'x86-64-v2'}, AVX2, None), ('x86-64-v2', True, 'SSBU_ISA'))
        self.assertEqual(isa.choose({'SSBU_ISA': ' AVX2 '}, None, None)[:2], ('avx2', True))
        self.assertEqual(isa.choose({'SSBU_ISA': ''}, AVX2, None)[:2], ('avx2', False))
        self.assertEqual(isa.choose({'SSBU_ISA': 'auto'}, V2, ('avx2', True))[:2], ('x86-64-v2', False))
        with self.assertRaisesRegex(SystemExit, 'SSBU_ISA=sse2: the instruction set to build for is one of avx2, x86-64-v2'):
            isa.choose({'SSBU_ISA': 'sse2'}, AVX2, None)

    def test_the_last_choice(self):
        # SSBU_ISA's choice is kept by the record, unless the processor cannot run it or SSBU_ISA=auto
        self.assertEqual(isa.choose({}, AVX2, ('x86-64-v2', True))[:2], ('x86-64-v2', True))
        self.assertEqual(isa.choose({'SSBU_ISA': 'auto'}, AVX2, ('x86-64-v2', True))[:2], ('avx2', False))
        self.assertEqual(isa.choose({}, V2, ('avx2', True))[:2], ('x86-64-v2', False))
        self.assertEqual(isa.choose({}, None, ('avx2', True))[:2], ('avx2', True))
        # a choice of its own is made again; when the processor cannot be asked, the last one stays
        self.assertEqual(isa.choose({}, AVX2, ('x86-64-v2', False))[:2], ('avx2', False))
        self.assertEqual(isa.choose({}, None, ('avx2', False))[:2], ('avx2', False))

    def test_proc_cpuinfo(self):
        flags = 'fpu sse sse2 pni ssse3 cx16 sse4_1 sse4_2 popcnt avx avx2 fma bmi1 bmi2 abm f16c'
        text = (f'processor\t: 0\nmodel name\t: Made-up CPU\nflags\t\t: {flags}\n\n'
                'processor\t: 1\nflags\t\t: fpu sse sse2\n')   # (the first processor's)
        self.assertEqual(isa.cpuinfo_features(text), AVX2)
        self.assertEqual(isa.best(isa.cpuinfo_features(text)), 'avx2')
        no_ymm = text.replace(' avx avx2 fma', ' ')   # (a kernel or VM without the YMM registers lists no avx)
        self.assertEqual(isa.best(isa.cpuinfo_features(no_ymm)), 'x86-64-v2')
        self.assertIsNone(isa.best(isa.cpuinfo_features(text.replace(' sse4_2', ''))))
        self.assertIsNone(isa.cpuinfo_features('processor\t: 0\nmodel name\t: Made-up CPU\n'))
        self.assertIsNone(isa.cpuinfo_features(''))

    def test_windows(self):
        everything = {13, 14, 36, 37, 38, 39, 40}
        ymm = isa.XSTATE_AVX | 3
        self.assertIsNone(isa.windows_features(18363, lambda pf: pf in everything, lambda: ymm))   # (before 2004: not asked)
        self.assertEqual(isa.windows_features(19041, lambda pf: pf in everything, lambda: ymm), AVX2)
        # a system that does not save the YMM registers: no AVX, whatever the processor has
        self.assertEqual(isa.best(isa.windows_features(26100, lambda pf: pf in everything, lambda: 3)), 'x86-64-v2')
        self.assertEqual(isa.best(isa.windows_features(26100, lambda pf: pf in everything - {40}, lambda: ymm)), 'x86-64-v2')
        self.assertIsNone(isa.best(isa.windows_features(26100, lambda pf: pf in everything - {38, 39, 40}, lambda: ymm)))

    def test_this_pc(self):
        features = isa.cpu_features()
        self.assertTrue(features is None or isinstance(features, frozenset))
        if sys.platform.startswith('linux') and platform.machine() == 'x86_64':
            self.assertIsNotNone(features)   # (/proc/cpuinfo)
            self.assertIsNotNone(isa.best(features), 'a Linux runner below x86-64-v2?')
        if sys.platform == 'darwin':
            self.assertIsNone(features)


class RecordTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix='isa-test-')
        self.addCleanup(tmp.cleanup)
        self.dir = Path(tmp.name) / 'build'

    def test_record(self):
        self.assertIsNone(isa.recorded(str(self.dir)))
        self.assertEqual(isa.built_for(str(self.dir)), 'avx2')   # (a folder built before the record: avx2)
        isa.record(str(self.dir), 'x86-64-v2', True)
        self.assertEqual(isa.recorded(str(self.dir)), ('x86-64-v2', True))
        self.assertEqual(isa.built_for(str(self.dir)), 'x86-64-v2')
        isa.record(str(self.dir), 'avx2', False)
        self.assertEqual(isa.recorded(str(self.dir)), ('avx2', False))
        (self.dir / isa.RECORD).write_text('sse9\ntold\n')
        self.assertIsNone(isa.recorded(str(self.dir)))
        (self.dir / isa.RECORD).write_bytes(b'\xef\xbb\xbfx86-64-v2\r\ntold\r\n')   # saved again by an editor: BOM, CRLF
        self.assertEqual(isa.recorded(str(self.dir)), ('x86-64-v2', True))

    def test_target_reads_the_record(self):
        isa.record(str(self.dir), 'x86-64-v2', True)
        with mock.patch.dict(os.environ, {}, clear=False), mock.patch.object(isa, 'cpu_features', lambda: AVX2):
            os.environ.pop('SSBU_ISA', None)
            self.assertEqual(isa.target(str(self.dir))[:2], ('x86-64-v2', True))
            os.environ['SSBU_ISA'] = 'avx2'
            self.assertEqual(isa.target(str(self.dir))[:2], ('avx2', True))

    def test_a_folder_an_older_version_linked(self):
        """No record, but a program an older version linked there (avx2, as they all built, and its installer stopped
        for a processor whose name said it had no AVX2): avx2 is its last choice, kept when the processor cannot be
        asked (Windows before build 19041, a name the installer does not know), not built again for x86-64-v2."""
        self.assertIsNone(isa.last_choice(str(self.dir)))
        with mock.patch.dict(os.environ, {}, clear=False), mock.patch.object(isa, 'cpu_features', lambda: None):
            os.environ.pop('SSBU_ISA', None)
            self.assertEqual(isa.target(str(self.dir))[0], 'x86-64-v2')   # (nothing built here: not asked, not known)
            for program in isa.PROGRAMS:   # (build/ssbu.exe, build-linux/ssbu)
                with self.subTest(program=program):
                    self.dir.mkdir(parents=True, exist_ok=True)
                    (self.dir / program).write_bytes(b'')
                    self.assertEqual(isa.last_choice(str(self.dir)), ('avx2', False))
                    target, told, why = isa.target(str(self.dir))
                    self.assertEqual((target, told), ('avx2', False))
                    self.assertIn('as the last build: the processor could not be asked (SSBU_ISA=x86-64-v2 builds for older '
                                  'processors)', why)
                    (self.dir / program).unlink()
            (self.dir / 'ssbu.exe').write_bytes(b'')
            isa.record(str(self.dir), 'x86-64-v2', False)   # (a record says)
            self.assertEqual(isa.last_choice(str(self.dir)), ('x86-64-v2', False))
            self.assertEqual(isa.target(str(self.dir))[0], 'x86-64-v2')
            (self.dir / isa.RECORD).write_text('sse9\n')   # (one that cannot be read: as none, as built_for has it)
            self.assertEqual(isa.last_choice(str(self.dir)), ('avx2', False))
        with mock.patch.object(isa, 'cpu_features', lambda: V2):   # (a processor known to lack AVX2: x86-64-v2 all the same)
            self.assertEqual(isa.target(str(self.dir))[0], 'x86-64-v2')

    def test_adopts(self):
        """An older version's objects are kept by date only by a build compiling with its very flags, for the instruction
        set the folder is built for."""
        folder = str(self.dir)
        self.assertTrue(isa.adopts(folder, 'avx2', isa.LEGACY_CLANG_CL, isa.LEGACY_CLANG_CL))   # (no record: avx2's)
        self.assertFalse(isa.adopts(folder, 'avx2', isa.CLANG_CL['avx2'], isa.LEGACY_CLANG_CL))   # (today's flags differ)
        self.assertFalse(isa.adopts(folder, 'avx2', isa.CLANG['avx2'], isa.LEGACY_CLANG))
        self.assertFalse(isa.adopts(folder, 'x86-64-v2', isa.LEGACY_CLANG_CL, isa.LEGACY_CLANG_CL))   # (avx2's folder)
        isa.record(folder, 'x86-64-v2', True)
        self.assertTrue(isa.adopts(folder, 'x86-64-v2', ('-march=x86-64-v2',), ('-march=x86-64-v2',)))
        self.assertFalse(isa.adopts(folder, 'avx2', isa.LEGACY_CLANG_CL, isa.LEGACY_CLANG_CL))
        # today, no target compiles with the older versions' flags: a folder one built is compiled again whole, once
        for target in isa.TARGETS:
            self.assertNotEqual(isa.CLANG_CL[target], isa.LEGACY_CLANG_CL)
            self.assertNotEqual(isa.CLANG[target], isa.LEGACY_CLANG)


class BuildIsCurrentTests(unittest.TestCase):
    """build_game.py: a game compiled for another instruction set than today's is built again (Windows and
    Linux; the record is beside the program), and step 1 stops below x86-64-v2."""
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix='isa-build-test-')
        self.addCleanup(tmp.cleanup)
        top = Path(tmp.name)
        (top / 'exefs').mkdir()
        (top / 'exefs' / 'main').write_bytes(b'')
        self.exe = top / 'build' / 'ssbu.exe'
        self.exe.parent.mkdir()
        self.exe.write_bytes(b'MZ')
        later = time.time() + 3600   # (newer than everything it is built from)
        os.utime(self.exe, (later, later))
        self.ns = runpy.run_path(str(ROOT / 'tools/build_game.py'), run_name='build_game_isa')['build'].__globals__
        self.ns.update(LINUX=False, MACOS=False, EXE=str(self.exe), EXEFS=str(top / 'exefs'), DUMP=str(top))
        for patch in (mock.patch.object(cc, 'SDL3_DLL', None), mock.patch.dict(os.environ, {}, clear=False)):
            patch.start()
            self.addCleanup(patch.stop)
        os.environ.pop('SSBU_ISA', None)

    def current(self, features=AVX2, env=None):
        os.environ.pop('SSBU_ISA', None)
        os.environ.update(env or {})
        with mock.patch.object(isa, 'cpu_features', lambda: features):
            return self.ns['build_is_current']()

    def test_target_change(self):
        self.assertTrue(self.current())   # (no record: avx2, as every build before it)
        self.assertTrue(self.current(None))   # (and the processor not to be asked: avx2 kept, isa.last_choice)
        self.assertFalse(self.current(V2))   # this processor lacks AVX2 now: built again, for x86-64-v2
        self.assertFalse(self.current(env={'SSBU_ISA': 'x86-64-v2'}))
        isa.record(str(self.exe.parent), 'x86-64-v2', True)
        self.assertTrue(self.current())   # (SSBU_ISA's choice, kept)
        self.assertTrue(self.current(None))
        self.assertFalse(self.current(env={'SSBU_ISA': 'auto'}))
        isa.record(str(self.exe.parent), 'x86-64-v2', False)
        self.assertFalse(self.current())   # (chosen for a processor without AVX2; this one has it)
        self.assertTrue(self.current(V2))
        self.assertTrue(self.current(None))   # (not known: the last one)

    def test_step_1(self):
        instruction_set, failed = self.ns['instruction_set'], self.ns['Failed']
        with mock.patch.object(isa, 'cpu_features', lambda: V2):
            self.assertEqual(instruction_set()[0], 'x86-64-v2')
        with mock.patch.object(isa, 'cpu_features', lambda: OLD), \
                self.assertRaisesRegex(failed, "this PC's processor lacks sse4.2, popcnt, which WindowSmash needs"):
            instruction_set()
        os.environ['SSBU_ISA'] = 'x86-64-v2'   # (told: built anyway, say for another PC)
        with mock.patch.object(isa, 'cpu_features', lambda: OLD):
            self.assertEqual(instruction_set()[0], 'x86-64-v2')
        os.environ['SSBU_ISA'] = 'pentium'
        with self.assertRaisesRegex(failed, 'SSBU_ISA=pentium'):
            instruction_set()


# ---- the flags -----------------------------------------------------------------------------------
def cpu_bits():
    """runtime/cpu_check.h: macro -> its RT_CPU_ bit, and the bits by name."""
    text = (RUNTIME / 'cpu_check.h').read_text(encoding='utf-8')
    bits = {name: 1 << int(n) for name, n in re.findall(r'#define RT_CPU_(\w+)\s+\(1u << (\d+)\)', text)}
    return {macro: bits[name] for macro, name in re.findall(r'#ifdef (\w+)\n\s+\| RT_CPU_(\w+)', text)}, bits


def which_clang():
    """A clang that takes gcc's options (with --driver-mode=cl, clang-cl's), or None."""
    found = shutil.which('clang')
    if not found and os.name == 'nt':
        found = os.path.join(os.path.dirname(cc.CLANG), 'clang.exe')
    return found if found and os.path.exists(found) else None


CLANG = which_clang()
HAVE_CLANG_CL = os.name == 'nt' and (os.path.exists(cc.CLANG) or shutil.which(cc.CLANG) is not None)


class FlagTests(unittest.TestCase):
    def test_flag_sets(self):
        every = [f for flags in (*isa.CLANG_CL.values(), *isa.CLANG.values()) for f in flags]
        self.assertNotIn('-mf16c', every)   # (half floats are done in software: runtime/recomp.h)
        self.assertEqual(isa.CLANG_CL['avx2'], ('/arch:AVX2', '-mfma', '-mno-movbe', '-mno-f16c'))
        self.assertEqual(isa.CLANG['avx2'], ('-mavx2', '-mfma', '-mbmi', '-mbmi2', '-mlzcnt', '-mpopcnt'))
        self.assertEqual(isa.CLANG_CL['x86-64-v2'], ('/clang:-march=x86-64-v2',))
        self.assertEqual(isa.CLANG['x86-64-v2'], ('-march=x86-64-v2',))
        for target in isa.TARGETS:   # (cc.py's, which the runtime, the game and the codecs are compiled with)
            flags = cc.base_cflags(target)
            self.assertEqual(flags[1:1 + len(isa.CLANG_CL[target])], list(isa.CLANG_CL[target]))
            self.assertIn('/clang:-ffp-contract=off', flags)
        self.assertEqual(cc.BASE_CFLAGS, cc.base_cflags(cc.ISA))
        self.assertTrue(set(isa.CLANG_CL[cc.ISA]) <= set(cc.CFLAGS))
        self.assertFalse(set(isa.CLANG_CL['avx2'] + isa.CLANG_CL['x86-64-v2']) & set(cc.CHECK_CFLAGS))

    # the older versions: main before there was a choice, and phase2/object-cache (#7), this branch's base
    OLDER = ('b04e6282833dce1243d40869b3fd27be4dea5498', '1c42d682241379f7e345b238033f0e3d3ed6cd72')

    def test_the_older_versions_flags(self):
        """isa.LEGACY_CLANG_CL and LEGACY_CLANG are what the older versions compiled with (their cc.py's BASE_CFLAGS and
        toolchain.py's ARCH): read from them where git has them (not in a shallow clone, nor in a download), and
        written out here, so that neither changes unnoticed."""
        self.assertEqual(isa.LEGACY_CLANG_CL, ('/arch:AVX2', '-mfma', '-mf16c'))
        self.assertEqual(isa.LEGACY_CLANG, ('-mavx2', '-mfma', '-mf16c', '-mbmi', '-mbmi2', '-mlzcnt', '-mpopcnt'))

        def show(rev, path):
            try:
                r = subprocess.run(['git', 'show', f'{rev}:{path}'], capture_output=True, text=True, cwd=ROOT, timeout=60)
            except (OSError, subprocess.SubprocessError):
                return None
            return r.stdout if r.returncode == 0 else None
        for rev in self.OLDER:
            with self.subTest(rev=rev[:7]):
                cc_py, toolchain_py = show(rev, 'tools/cc.py'), show(rev, 'tools/linux/toolchain.py')
                if cc_py is None or toolchain_py is None:
                    self.skipTest(f'git does not have {rev[:7]} here')
                base = re.search(r'^BASE_CFLAGS = \["/nologo", (.*?), "-fno-strict-aliasing"', cc_py, re.M | re.S)
                self.assertEqual(tuple(re.findall(r'"([^"]*)"', base.group(1))), isa.LEGACY_CLANG_CL)
                arch = re.search(r'^ARCH = \[(.*?)\]', toolchain_py, re.M)
                self.assertEqual(tuple(re.findall(r'"([^"]*)"', arch.group(1))), isa.LEGACY_CLANG)

    def macros(self, cmd, folder):
        r = subprocess.run(cmd, capture_output=True, text=True, cwd=folder, timeout=120)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return {line.split()[1] for line in r.stdout.splitlines() if line.startswith('#define ')}

    def test_what_the_compilers_make_of_them(self):
        """Each target's macros as clang (Linux) and clang-cl (Windows) define them, and the processor check's
        bits (runtime/cpu_check.h) they make: x86-64-v2 exactly RT_CPU_V2; avx2 what isa.NEEDS says, on both;
        nothing for the check itself."""
        if not CLANG and not HAVE_CLANG_CL:
            self.skipTest('no clang')
        by_macro, bits = cpu_bits()
        v2 = sum(bits[n] for n in ('SSE3', 'SSSE3', 'SSE41', 'SSE42', 'POPCNT', 'CX16', 'LAHF'))
        avx = sum(bits[n] for n in ('AVX', 'AVX2', 'FMA', 'BMI1', 'BMI2', 'LZCNT', 'POPCNT', 'SSE42', 'SSE41', 'SSSE3', 'SSE3'))
        with tempfile.TemporaryDirectory(prefix='isa-macros-') as folder:
            Path(folder, 'empty.c').write_text('')
            drivers = {}
            if CLANG:
                drivers['clang'] = lambda flags: [CLANG, '--target=x86_64-linux-gnu', *flags, '-dM', '-E', '-x', 'c', 'empty.c']
            cl = [cc.CLANG] if HAVE_CLANG_CL else [CLANG, '--driver-mode=cl', '--target=x86_64-pc-windows-msvc']
            drivers['clang-cl'] = lambda flags: [*cl, '/nologo', *flags, '/clang:-dM', '/E', 'empty.c']
            for driver, command in drivers.items():
                table = isa.CLANG if driver == 'clang' else isa.CLANG_CL
                for target in (*isa.TARGETS, isa.ANY):
                    with self.subTest(driver=driver, target=target):
                        defined = self.macros(command(table[target]), folder)
                        needs = sum(bit for macro, bit in by_macro.items() if macro in defined)
                        if target == 'x86-64-v2':
                            self.assertEqual(needs, v2)
                        elif target == 'avx2':
                            # (on Windows the CX16 and LAHF-SAHF of every AVX2 processor too: /arch:AVX2 is Haswell's,
                            # less MOVBE and F16C, which isa.NEEDS and Windows' answer do not cover)
                            self.assertEqual(needs & avx, avx)
                            self.assertEqual(needs & ~(avx | v2), 0)
                        else:
                            self.assertEqual(needs, 0)
                            self.assertIn('__SSE2__', defined)


class CodecTests(unittest.TestCase):
    AVX2_FILES = {'pitch_avx.c', 'NSQ_del_dec_avx2.c', 'inner_product_FLP_avx2.c'}

    def test_opus_defines(self):
        self.assertEqual(codec_libs.LIBRARIES, codec_libs.libraries('avx2'))
        avx2, v2 = codec_libs.libraries('avx2')['opus'][4], codec_libs.libraries('x86-64-v2')['opus'][4]
        self.assertIn('OPUS_X86_PRESUME_AVX2', avx2)
        self.assertEqual([d for d in v2 if 'AVX' in d], [])
        self.assertIn('OPUS_X86_PRESUME_SSE4_1', v2)
        self.assertEqual(set(avx2) - set(v2), {'OPUS_X86_MAY_HAVE_AVX2', 'OPUS_X86_PRESUME_AVX2'})
        self.assertEqual(codec_libs.libraries('x86-64-v2')['libvpx'], codec_libs.libraries('avx2')['libvpx'])

    def test_opus_avx2_files_left_out(self):
        srcs = codec_libs.sources(codec_libs.OPUS, codec_libs.OPUS_FOLDERS)
        for target, kept in (('avx2', set()), ('x86-64-v2', self.AVX2_FILES)):
            defines = codec_libs.libraries(target)['opus'][4]
            left = {os.path.basename(s) for s in srcs if not codec_libs.wanted(s, defines)}
            self.assertEqual(left, kept, target)

    def test_built_again_for_another_instruction_set(self):
        with tempfile.TemporaryDirectory(prefix='isa-codec-') as tmp:
            root = Path(tmp)
            (root / 'src').mkdir()
            source = root / 'src' / 'a.c'
            source.write_text('int a;')
            old = time.time() - 100
            os.utime(source, (old, old))
            lib = root / 'a.lib'
            lib.write_bytes(b'lib')
            commands = {t: codec_libs.windows_command(str(root), ('',), ('A',), t) for t in isa.TARGETS}
            self.assertIn('/arch:AVX2', commands['avx2'])
            self.assertIn('/clang:-march=x86-64-v2', commands['x86-64-v2'])
            (root / 'a.lib.flags').write_text(commands['avx2'])
            self.assertTrue(codec_libs.static_is_current(str(lib), str(root), ('src',), (), commands['avx2']))
            self.assertFalse(codec_libs.static_is_current(str(lib), str(root), ('src',), (), commands['x86-64-v2']))


# ---- building and running x86-64 code here ---------------------------------------------------------
ON_MAC = sys.platform == 'darwin'
ROSETTA = ON_MAC and CLANG is not None and subprocess.run(['arch', '-x86_64', '/usr/bin/true'], capture_output=True).returncode == 0
LINUX_X86 = sys.platform.startswith('linux') and platform.machine() == 'x86_64' and CLANG is not None
WINDOWS_X64 = HAVE_CLANG_CL and platform.machine().lower() in ('amd64', 'x86_64')
CAN_RUN_X86 = ROSETTA or LINUX_X86 or WINDOWS_X64
WHY_NOT = 'needs x86-64 code built and run here: clang-cl on Windows, clang on x86-64 Linux, or clang and Rosetta on a Mac'
ILLEGAL = (-signal.SIGILL if hasattr(signal, 'SIGILL') else None, 0xC000001D)   # (an instruction this processor lacks)


def compile_x86(src, obj, target, *extra, opt='-O2'):
    """src compiled into obj for an instruction set, as the game's code is (cc.py's flags on Windows,
    tools/linux/toolchain.py's on Linux and the Mac)."""
    if os.name == 'nt':
        cmd = [cc.CLANG, '/c', opt.replace('-', '/'), *cc.base_cflags(target), '/I', str(RUNTIME), *extra, str(src), '/Fo' + str(obj)]
    else:
        cmd = [CLANG, *(['-arch', 'x86_64'] if ON_MAC else []), '-c', opt, *isa.CLANG[target], '-fno-strict-aliasing',
               '-ffp-contract=off', '-w', '-I', str(RUNTIME), *extra, str(src), '-o', str(obj)]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    if r.returncode:
        raise AssertionError(f'{Path(src).name} did not compile for {target}:\n{r.stdout}{r.stderr}')


def link_x86(objs, exe):
    if os.name == 'nt':
        cmd = [cc.CLANG, '/nologo', *cc.LINK_FLAGS, *map(str, objs), '/Fe' + str(exe), '/link', *cc.LINK_OPTIONS, 'user32.lib']
    else:
        cmd = [CLANG, *(['-arch', 'x86_64'] if ON_MAC else []), *map(str, objs), '-o', str(exe), '-lm']
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    if r.returncode:
        raise AssertionError(f'{Path(exe).name} did not link:\n{r.stdout}{r.stderr}')


def run_x86(exe, *args, stdin=None, env=None):
    return subprocess.run([*(['arch', '-x86_64'] if ON_MAC else []), str(exe), *args], input=stdin, capture_output=True,
                          text=True, env=env, timeout=120)


def objdump():
    """A disassembler for this machine's objects, or None."""
    found = [os.path.join(os.path.dirname(cc.CLANG), 'llvm-objdump.exe')] if os.name == 'nt' else []
    found += [shutil.which('llvm-objdump'), shutil.which('objdump'), r'C:\Program Files\LLVM\bin\llvm-objdump.exe']
    return next((f for f in found if f and os.path.isfile(f)), None)


def disassembly(test, *objs, relocations=False):
    tool = objdump()
    if not tool:
        test.skipTest('no objdump or llvm-objdump to disassemble with')
    r = subprocess.run([tool, *(['-r'] if relocations else []), '-d', '--no-show-raw-insn', *map(str, objs)],
                       capture_output=True, text=True, timeout=300)
    test.assertEqual(r.returncode, 0, r.stderr)
    return r.stdout


PREFIXES = ('lock', 'rep', 'repe', 'repne', 'repz', 'repnz', 'data16', 'notrack', 'bnd', 'cs', 'ds')


def mnemonics(text):
    """The instructions of objdump -d output (llvm's or GNU's, AT&T syntax), with the one after a prefix."""
    found = []
    for line in text.splitlines():
        m = re.match(r'\s*[0-9a-f]+:\s+([a-z][a-z0-9]*)(?:\s+([a-z][a-z0-9]*))?', line)
        if m:
            found.append(m.group(1))
            if m.group(1) in PREFIXES and m.group(2):
                found.append(m.group(2))
    return found


def named(insns, names, prefixes=()):
    """Those of insns that are one of names (also with AT&T's size suffix) or start with one of prefixes."""
    return sorted({i for i in insns if i in names or (i[-1:] in 'bwlq' and i[:-1] in names) or i.startswith(prefixes)})


# beyond x86-64-v2 (VEX: every v... instruction, and the YMM registers, are AVX)
BEYOND_V2 = {'lzcnt', 'tzcnt', 'andn', 'bextr', 'blsi', 'blsmsk', 'blsr', 'bzhi', 'mulx', 'pdep', 'pext', 'rorx', 'sarx', 'shlx',
             'shrx', 'movbe'}
# beyond plain x86-64 (SSE3 to SSE4.2, POPCNT, CMPXCHG16B, LAHF-SAHF)
BEYOND_X86_64 = BEYOND_V2 | {
    'popcnt', 'crc32', 'cmpxchg16b', 'lahf', 'sahf', 'lddqu', 'movddup', 'movshdup', 'movsldup', 'haddps', 'haddpd', 'hsubps',
    'hsubpd', 'addsubps', 'addsubpd', 'fisttp', 'pshufb', 'palignr', 'pmaddubsw', 'pmulhrsw', 'pabsb', 'pabsw', 'pabsd',
    'psignb', 'psignw', 'psignd', 'phaddw', 'phaddd', 'phaddsw', 'phsubw', 'phsubd', 'phsubsw', 'pminsb', 'pminsd', 'pminuw',
    'pminud', 'pmaxsb', 'pmaxsd', 'pmaxuw', 'pmaxud', 'pmulld', 'pmuldq', 'packusdw', 'dpps', 'dppd', 'mpsadbw',
    'phminposuw', 'movntdqa', 'insertps', 'extractps', 'ptest', 'pcmpeqq', 'pcmpgtq', 'pinsrb', 'pinsrd', 'pinsrq', 'pextrb',
    'pextrd', 'pextrq'}
SSE4_PREFIXES = ('round', 'pmovzx', 'pmovsx', 'pblend', 'blendp', 'blendvp', 'pcmpestr', 'pcmpistr')


def avx_in(test, text):
    """The AVX instructions (VEX-encoded, or naming a YMM or ZMM register) of a disassembly."""
    return sorted({i for i in mnemonics(text) if i.startswith('v')} | set(re.findall(r'%?\b[yz]mm\d+', text)))


# The processor check run as the game runs it (before main on Windows and Linux, from main on a Mac),
# against the made-up processors of RT_CPU_TEST and this one.
NEEDS_C = '#define RT_CPU_DEFINE_NEEDS\n#include "cpu_check.h"\n'
MAIN_C = r'''#include <stdio.h>
void rt_cpu_check(void);
int main(void) {
#ifdef __APPLE__
    rt_cpu_check();   /* (a Mach-O program has no hook for it: on Windows and Linux it ran already) */
#endif
    puts("main");
    return 0;
}
'''


@unittest.skipUnless(CAN_RUN_X86, WHY_NOT)
class CpuCheckTests(unittest.TestCase):
    MADE_UP = {'everything': (1 << 15) - 1, 'v2': 0x7F, 'old': 0x7F & ~0x18}   # (old: no SSE4.2, no POPCNT)

    @classmethod
    def setUpClass(cls):
        cls.dir = Path(tempfile.mkdtemp(prefix='isa-check-'))
        src = cls.dir / 'cpu_check.c'
        shutil.copy(RUNTIME / 'cpu_check.c', src)
        (cls.dir / 'needs.c').write_text(NEEDS_C)
        (cls.dir / 'main.c').write_text(MAIN_C)
        cls.check = cls.dir / 'check.obj'
        compile_x86(src, cls.check, isa.ANY)
        compile_x86(cls.dir / 'main.c', cls.dir / 'main.obj', isa.ANY)
        for target in isa.TARGETS:
            compile_x86(cls.dir / 'needs.c', cls.dir / f'needs_{target}.obj', target)
        define = '/DRT_CPU_TEST=' if os.name == 'nt' else '-DRT_CPU_TEST='
        for name, has in cls.MADE_UP.items():
            compile_x86(src, cls.dir / f'check_{name}.obj', isa.ANY, f'{define}{has:#x}')

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir, ignore_errors=True)

    def run_check(self, check, target):
        exe = self.dir / f'run_{check}_{target}.exe'
        if not exe.exists():
            link_x86([self.dir / f'{check}.obj', self.dir / f'needs_{target}.obj', self.dir / 'main.obj'], exe)
        r = run_x86(exe)
        return r.returncode, r.stdout.strip(), r.stderr.strip()

    def test_made_up_processors(self):
        code, out, err = self.run_check('check_v2', 'avx2')
        self.assertEqual((code, out), (5, ''), err)   # (stopped before main)
        self.assertTrue(err.startswith('[boot] This copy of WindowSmash was built for processors with AVX2, and this processor '
                                       'lacks AVX, AVX2, FMA'), err)
        self.assertIn('If this message comes back, build it with SSBU_ISA=x86-64-v2 set', err)
        self.assertEqual(self.run_check('check_everything', 'avx2'), (0, 'main', ''))
        self.assertEqual(self.run_check('check_v2', 'x86-64-v2'), (0, 'main', ''))
        code, out, err = self.run_check('check_old', 'x86-64-v2')
        self.assertEqual((code, out), (5, ''), err)
        self.assertIn('[boot] This processor lacks SSE4.2, POPCNT, which WindowSmash needs', err)
        self.assertIn('It cannot run on this PC.', err)

    def test_this_processor(self):
        self.assertEqual(self.run_check('check', 'x86-64-v2'), (0, 'main', ''))
        features = isa.cpu_features()
        if os.name == 'nt' and (features is None or isa.best(features) != 'avx2'):
            self.skipTest('(it would show its message box)')
        code, out, err = self.run_check('check', 'avx2')
        if features is not None:   # (as the build asks: the same answer)
            self.assertEqual(code, 0 if isa.best(features) == 'avx2' else 5, err)
        self.assertIn(code, (0, 5))
        if code:
            self.assertIn('built for processors with AVX2', err)
        else:
            self.assertEqual(out, 'main')

    def test_compiled_for_any_x86_64_processor(self):
        text = disassembly(self, self.check)
        insns = mnemonics(text)
        self.assertIn('cpuid', insns)
        self.assertIn('xgetbv', insns)
        self.assertEqual(avx_in(self, text), [])
        self.assertEqual(named(insns, BEYOND_X86_64, SSE4_PREFIXES), [])
        headers = subprocess.run([objdump(), '-h', str(self.check)], capture_output=True, text=True, timeout=60).stdout
        if os.name == 'nt':
            self.assertIn('.CRT$XID', headers)   # (a C initialiser: after the C runtime's, before C++'s and main)
        elif not ON_MAC:
            self.assertIn('.preinit_array', headers)   # (before every constructor)


@unittest.skipUnless(WINDOWS_X64 or LINUX_X86 or (ON_MAC and CLANG), 'needs clang-cl on Windows, or clang')
class OpusCompileTests(unittest.TestCase):
    def compile_all(self, target, keep=lambda name: True):
        """opus's sources for an instruction set (its defines and files: codec_libs.py), compiled; their disassembly."""
        _lib, root, folders, includes, defines, _have = codec_libs.libraries(target)['opus']
        srcs = [s for s in codec_libs.sources(root, folders) if codec_libs.wanted(s, defines) and keep(os.path.basename(s))]
        if os.name == 'nt':
            flags = [cc.CLANG, '/c', *codec_libs.windows_flags(root, includes, defines, target)]
        else:
            flags = [CLANG, '-c', *codec_libs.static_flags([*(['-arch', 'x86_64'] if ON_MAC else []), *isa.CLANG[target], '-w'],
                                                          root, includes, defines)]
        folder = Path(tempfile.mkdtemp(prefix='isa-opus-'))
        self.addCleanup(shutil.rmtree, folder, True)

        def one(src):
            obj = folder / (os.path.relpath(src, root).replace(os.sep, '_')[:-2] + '.obj')
            out = ['/Fo' + str(obj)] if os.name == 'nt' else ['-o', str(obj)]
            r = subprocess.run([*flags, src, *out], capture_output=True, text=True, timeout=300)
            return obj if r.returncode == 0 else (src, r.stdout + r.stderr)
        from concurrent.futures import ThreadPoolExecutor
        with ThreadPoolExecutor(8) as pool:
            done = list(pool.map(one, srcs))
        self.assertEqual([f'{os.path.basename(s)}: {e[:500]}' for s, e in (d for d in done if isinstance(d, tuple))], [])
        return len(srcs), disassembly(self, *done)

    def test_no_avx_at_x86_64_v2(self):
        count, text = self.compile_all('x86-64-v2')
        self.assertGreater(count, 140)
        self.assertEqual(avx_in(self, text), [])
        self.assertEqual(named(mnemonics(text), BEYOND_V2), [])
        # (and the AVX2 files, at avx2, are AVX2)
        _count, avx2 = self.compile_all('avx2', keep=lambda name: name in CodecTests.AVX2_FILES)
        self.assertTrue([i for i in mnemonics(avx2) if i.startswith('v')])


# ---- the lifted code -----------------------------------------------------------------------------------
# Hand-assembled AArch64 words (their fields as the Arm ARM gives them), one function: rounding (FRINT*),
# fused multiply-adds (FMADD, FMSUB, FMLA), bit counts (CNT, CLZ), table lookups (TBL) and half floats.
def frint(rmode, d, n, double=True):   # FRINT<N P M Z A _ X I> (scalar): 0 00 11110 type 1 001 rmode 10000 Rn Rd
    return 0x1E244000 | double << 22 | rmode << 15 | n << 5 | d


def fmadd(d, n, m, a, double=True, sub=False):   # FMADD/FMSUB: 0 00 11111 type 0 Rm o0 Ra Rn Rd
    return 0x1F000000 | double << 22 | m << 16 | sub << 15 | a << 10 | n << 5 | d


def fmla(d, n, m, double):   # FMLA (vector), Q=1: 0 1 0 01110 0 sz 1 Rm 11001 1 Rn Rd
    return 0x4E20CC00 | double << 22 | m << 16 | n << 5 | d


def cnt16b(d, n):   # CNT Vd.16B: 0 1 0 01110 00 10000 00101 10 Rn Rd
    return 0x4E205800 | n << 5 | d


def clz(d, n, wide):   # CLZ: sf 1 0 11010110 00000 00010 0 Rn Rd
    return 0x5AC01000 | wide << 31 | n << 5 | d


def tbl16b(d, n, regs, m):   # TBL Vd.16B, {Vn.16B...}, Vm.16B: 0 1 001110 000 Rm 0 len 0 00 Rn Rd
    return 0x4E000000 | m << 16 | (regs - 1) << 13 | n << 5 | d


def fcvt(d, n, to, frm):   # FCVT (scalar): 0 00 11110 type 1 0001 opc 10000 Rn Rd (type, opc: 0 single, 1 double, 3 half)
    return 0x1E224000 | frm << 22 | to << 15 | n << 5 | d


RET = 0xD65F03C0
FRINTS = 'npmza_xi'   # (rmode 5 is no instruction)
WORDS = [frint(FRINTS.index(k), 16 + i, 1) for i, k in enumerate('npmzaxi')] + [
    fmadd(23, 2, 3, 4), fmadd(24, 13, 14, 15, double=False), fmadd(25, 2, 3, 4, sub=True),
    fmla(26, 5, 6, double=False), fmla(27, 7, 8, double=True), cnt16b(28, 9), clz(2, 1, True), clz(3, 1, False),
    tbl16b(29, 9, 1, 10), tbl16b(30, 11, 2, 10), fcvt(31, 0, 0, 3), fcvt(1, 13, 3, 0), frint(0, 2, 13, double=False), RET]
NAMES = ['frintn d16, d1', 'frintp d17, d1', 'frintm d18, d1', 'frintz d19, d1', 'frinta d20, d1', 'frintx d21, d1',
         'frinti d22, d1', 'fmadd d23, d2, d3, d4', 'fmadd s24, s13, s14, s15', 'fmsub d25, d2, d3, d4',
         'fmla v26.4s, v5.4s, v6.4s', 'fmla v27.2d, v7.2d, v8.2d', 'cnt v28.16b, v9.16b', 'clz x2, x1', 'clz w3, w1',
         'tbl v29.16b, {v9.16b}, v10.16b', 'tbl v30.16b, {v11.16b, v12.16b}, v10.16b', 'fcvt s31, h0', 'fcvt h1, s13',
         'frintn s2, s13', 'ret']
INPUTS = list(range(16)) + [26, 27]   # (v26, v27: the accumulators)
OUTPUTS = [1, 2] + list(range(16, 32))
WRITES = {int(m.group(1)): name for name in NAMES if (m := re.match(r'\w+ [dshv](\d+)', name))}   # (vector register -> its instruction)

HARNESS = r'''/* runs the lifted function on the cases in stdin (x1, then the input registers' halves), printing
   x2, x3 and the output registers (tests/test_isa.py) */
#include <stdio.h>
#include <stdlib.h>
#include "recomp.h"
#ifdef _WIN32
int __cdecl _set_FMA3_enable(int flag);   /* (the C runtime's: 0 makes its fma() the software one) */
#endif
_Noreturn void rt_trap(Cpu *c, u64 pc, const char *why) { (void)c; fprintf(stderr, "trap at %llx: %s\n", (unsigned long long)pc, why); exit(4); }
GuestFn f_7100000000;
static const int in[] = {INPUTS}, out[] = {OUTPUTS};
int main(int argc, char **argv) {
#ifdef _WIN32
    if (argc > 1 && !strcmp(argv[1], "nofma3")) _set_FMA3_enable(0);
#endif
    (void)argc; (void)argv;
    unsigned long long x1, lo, hi;
    while (scanf("%llx", &x1) == 1) {
        Cpu c;
        memset(&c, 0, sizeof c);
        c.x[1] = x1;
        for (size_t i = 0; i < sizeof in / sizeof in[0]; i++) {
            if (scanf("%llx %llx", &lo, &hi) != 2) return 2;
            c.v[in[i]].d[0] = lo;
            c.v[in[i]].d[1] = hi;
        }
        f_7100000000(&c);
        printf("%016llx %016llx", (unsigned long long)c.x[2], (unsigned long long)c.x[3]);
        for (size_t i = 0; i < sizeof out / sizeof out[0]; i++)
            printf(" %016llx%016llx", (unsigned long long)c.v[out[i]].d[1], (unsigned long long)c.v[out[i]].d[0]);
        putchar('\n');
    }
    return 0;
}
'''.replace('{INPUTS}', '{' + ', '.join(map(str, INPUTS)) + '}').replace('{OUTPUTS}', '{' + ', '.join(map(str, OUTPUTS)) + '}')


def lifted_c():
    img = struct.pack(f'<{len(WORDS)}I', *WORDS) + bytes(0x100)
    cached, lift.REGCACHE = lift.REGCACHE, False
    try:
        text, reasons, _callees = lift.Lifter(img, {0}).lift_function(0, 4 * len(WORDS))
    finally:
        lift.REGCACHE = cached
    assert not reasons, reasons
    return '#include "recomp.h"\n\nGuestFn f_7100000000;\n\n' + text + '\n'


# exact results: Python's floats and fractions
def b64(x):
    return struct.unpack('<Q', struct.pack('<d', x))[0]


def f64(b):
    return struct.unpack('<d', struct.pack('<Q', b))[0]


def b32(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def f32(b):
    return struct.unpack('<f', struct.pack('<I', b & 0xFFFFFFFF))[0]


def nearest(x, p, emin, emax):
    """The Fraction x rounded to the binary float with p-bit significands and exponents emin..emax nearest
    it (ties to even), as a Python float: inf past the largest, a zero with x's sign."""
    a = abs(x)
    if a == 0:
        return 0.0
    e = a.numerator.bit_length() - a.denominator.bit_length()
    if Fraction(2) ** e > a:
        e -= 1
    ulp = Fraction(2) ** (max(e, emin) - p + 1)
    v = round(a / ulp) * ulp   # (Fraction's round: half to even)
    r = math.inf if v >= Fraction(2) ** (emax + 1) else float(v)
    return -r if x < 0 else r


def to32(x):
    return nearest(Fraction(x), 24, -126, 127)


def fused(a, b, c, single=False):
    """fma(a, b, c): a * b + c rounded once (nan for an invalid one)."""
    if any(map(math.isnan, (a, b, c))):
        return math.nan
    if math.isinf(a) or math.isinf(b):
        if a == 0 or b == 0:
            return math.nan
        prod = math.copysign(math.inf, a) * math.copysign(1, b)
        return math.nan if math.isinf(c) and c != prod else prod
    if math.isinf(c):
        return c
    exact = Fraction(a) * Fraction(b) + Fraction(c)
    if exact == 0:   # (an exact zero: -0 only for -0 + -0)
        prod_negative = (math.copysign(1, a) * math.copysign(1, b)) < 0
        return -0.0 if (a == 0 or b == 0) and c == 0 and prod_negative and math.copysign(1, c) < 0 else 0.0
    return nearest(exact, 24, -126, 127) if single else nearest(exact, 53, -1022, 1023)


def rounded(x, mode):
    """FRINT<mode> of x: n nearest (ties to even), p up, m down, z toward zero, a nearest (ties away), x and i
    the current rounding mode (nearest)."""
    if math.isnan(x) or math.isinf(x):
        return x
    f = Fraction(x)
    down = math.floor(f)
    half = f - down
    r = {'n': round(f), 'x': round(f), 'i': round(f), 'm': down, 'p': down + (half > 0), 'z': math.trunc(f),
         'a': math.floor(f + Fraction(1, 2)) if f >= 0 else -math.floor(-f + Fraction(1, 2))}[mode]
    return math.copysign(0.0, x) if r == 0 else float(r)


def half_to_single(h):
    return struct.unpack('<e', h.to_bytes(2, 'little'))[0]


def single_to_half(x):
    try:
        return int.from_bytes(struct.pack('<e', x), 'little')
    except OverflowError:   # (past 65504 by half a step or more: infinity)
        return 0xFC00 if x < 0 else 0x7C00


def lanes32(*xs):
    v = 0
    for i, x in enumerate(xs):
        v |= b32(x) << 32 * i
    return v


def lanes64(*xs):
    return b64(xs[0]) | b64(xs[1]) << 64


# the made-up inputs: the hard cases by hand, then random ones (the same every run)
FRINT_CASES = [0.5, 1.5, 2.5, -0.5, -1.5, -2.5, 0.49999999999999994, -0.49999999999999994, 2.0 ** 52 - 0.5, -(2.0 ** 52 - 0.5),
               2.0 ** 52 + 1, 2.0 ** 53, 1e300, -1e300, 0.0, -0.0, 5e-324, -5e-324, 2.2250738585072014e-308, math.inf,
               -math.inf, math.nan, 3.7, -3.7, 1234567.5, -1234566.5, 0.75, -0.25, -0.75, 4503599627370497.0]
FMA_CASES = [
    (1 + 2.0 ** -52, 1 - 2.0 ** -52, -1.0),                     # -2^-104, where a multiply then an add gives 0
    (1 + 2.0 ** -27, 1 + 2.0 ** -27, -(1 + 2.0 ** -26)),         # 2^-54
    (0.1, 10.0, -1.0),
    (2.0 ** 500 * (1 + 2.0 ** -52), 2.0 ** 500 * (1 - 2.0 ** -52), -(2.0 ** 1000)),
    (1e308, 2.0, -1e308),                                       # the product overflows, the sum does not
    (2.0 ** -600, -(2.0 ** -500), 0.0),                         # rounded to -0
    (1 + 2.0 ** -52, 2.0 ** -1060, 2.0 ** -1074),               # subnormal
    (1.5, 2.0 ** -1074, 0.0),                                   # a tie among the subnormals: to even
    (3.0, 1.0 / 3.0, -1.0),
    (-0.0, 5.0, -0.0), (0.0, -5.0, 0.0), (math.inf, 0.0, 1.0), (math.inf, 1.0, -math.inf), (1e308, 10.0, -math.inf),
    (2.0, 3.0, math.nan),
]
HALF_CASES = [1 + 2.0 ** -11, 1 + 3 * 2.0 ** -11, 2.0 ** -25, 3 * 2.0 ** -25, 2.0 ** -24, 65504.0, 65519.0, 65520.0, 1e5,
              0.1, -2.5, 0.5, 1.5, 2.0 ** -14, 2.0 ** -15 + 2.0 ** -26, -0.0, 1e-8, 3.5, -65520.0, 16777217.0]
HALVES = [0x0001, 0x03FF, 0x0400, 0x3C00, 0x7BFF, 0x7C00, 0xFC00, 0x8000, 0x7E00, 0x8001, 0x3555]


def make_cases(n=400):
    rng = random.Random(20261023)

    def double(lo=-60, hi=60):
        return rng.choice((1, -1)) * rng.uniform(1, 2) * 2.0 ** rng.randint(lo, hi)

    def single(lo=-20, hi=20):
        return f32(rng.getrandbits(1) << 31 | (127 + rng.randint(lo, hi)) << 23 | rng.getrandbits(23))

    cases = []
    for i in range(n):
        if i < len(FMA_CASES):
            a, b, c = FMA_CASES[i]
        elif i % 3 == 0:   # (in the subnormals)
            a, b = double(-540, -530), double(-540, -530)
            c = -(a * b)
        else:   # a product's rounding error: what only a fused multiply-add gives exactly
            a, b = double(), double()
            c = -(a * b) if i % 5 else double()
        x = FRINT_CASES[i] if i < len(FRINT_CASES) else rng.choice((rng.uniform(-1e6, 1e6), rng.randint(-99, 99) + 0.5))
        s13 = HALF_CASES[i // 2 % len(HALF_CASES)] if i % 2 else single()
        s13 = f32(b32(s13))
        s14 = single(-3, 3)
        s15 = -to32(s13 * s14) if math.isfinite(s13 * s14) and i % 7 else single()
        f_a, f_b = [single() for _ in range(4)], [single() for _ in range(4)]
        f_acc = [-to32(p * q) if (i + k) % 4 else single() for k, (p, q) in enumerate(zip(f_a, f_b))]
        d_a, d_b = [double() for _ in range(2)], [double() for _ in range(2)]
        d_acc = [-(p * q) for p, q in zip(d_a, d_b)]
        if i == 4:   # (a product too small for the format, plus +0: -0, in each fused multiply-add)
            s13, s14, s15 = 2.0 ** -80, -(2.0 ** -75), 0.0
            f_a[0], f_b[0], f_acc[0] = 2.0 ** -80, -(2.0 ** -75), 0.0
            d_a[0], d_b[0], d_acc[0] = 2.0 ** -600, -(2.0 ** -500), 0.0
        half = HALVES[i] if i < len(HALVES) else rng.getrandbits(16)
        x1 = rng.getrandbits(rng.randint(0, 64)) if i > 2 else (0, 1 << 63, 1 << 31)[i]
        junk = rng.getrandbits(64)   # (the upper halves of scalar inputs: never read)
        v = {0: (half, 0), 1: (b64(x), junk), 2: (b64(a), junk), 3: (b64(b), 0), 4: (b64(c), 0),
             5: split(lanes32(*f_a)), 6: split(lanes32(*f_b)), 7: split(lanes64(*d_a)), 8: split(lanes64(*d_b)),
             13: (b32(s13) | (junk & 0xFFFFFFFF) << 32, junk), 14: (b32(s14), 0), 15: (b32(s15), 0), 26: split(lanes32(*f_acc)),
             27: split(lanes64(*d_acc))}
        for r in (9, 10, 11, 12):
            v[r] = split(rng.getrandbits(128) if r != 10 else int.from_bytes(bytes(rng.randrange(40) for _ in range(16)), 'little'))
        cases.append((x1, v))
    return cases


def split(v):
    return v & (1 << 64) - 1, v >> 64


def case_line(case):
    x1, v = case
    return ' '.join([f'{x1:x}'] + [f'{v[r][0]:x} {v[r][1]:x}' for r in INPUTS])


NAN64, NAN32 = 'nan64', 'nan32'   # (any NaN: its payload is the host's)


def expected(case):
    """reg -> its 128 bits as the console computes them (NAN64 / NAN32: a NaN in the low double or single),
    and x2, x3."""
    x1, v = case
    d = {r: f64(v[r][0]) for r in (1, 2, 3, 4)}
    s = {r: f32(v[r][0]) for r in (13, 14, 15)}
    scalar64 = lambda x: NAN64 if math.isnan(x) else b64(x)   # noqa: E731
    scalar32 = lambda x: NAN32 if math.isnan(x) else b32(x)   # noqa: E731
    out = {r: scalar64(rounded(d[1], k)) for r, k in zip(range(16, 23), 'npmzaxi')}
    out[23] = scalar64(fused(d[2], d[3], d[4]))
    out[24] = scalar32(fused(s[13], s[14], s[15], single=True))
    out[25] = scalar64(fused(-d[2], d[3], d[4]))
    wide = lambda r: v[r][0] | v[r][1] << 64   # noqa: E731
    lane32 = lambda r, i: f32(wide(r) >> 32 * i)   # noqa: E731
    lane64 = lambda r, i: f64(wide(r) >> 64 * i & (1 << 64) - 1)   # noqa: E731
    out[26] = lanes32(*[fused(lane32(5, i), lane32(6, i), lane32(26, i), single=True) for i in range(4)])
    out[27] = lanes64(*[fused(lane64(7, i), lane64(8, i), lane64(27, i)) for i in range(2)])
    table, two, idx = wide(9).to_bytes(16, 'little'), wide(11).to_bytes(16, 'little') + wide(12).to_bytes(16, 'little'), \
        wide(10).to_bytes(16, 'little')
    out[28] = int.from_bytes(bytes(bin(b).count('1') for b in table), 'little')
    out[29] = int.from_bytes(bytes(table[i] if i < 16 else 0 for i in idx), 'little')
    out[30] = int.from_bytes(bytes(two[i] if i < 32 else 0 for i in idx), 'little')
    h = half_to_single(v[0][0] & 0xFFFF)
    out[31] = scalar32(h)
    out[1] = single_to_half(s[13])
    out[2] = b32(rounded(s[13], 'n'))
    return out, 64 - x1.bit_length(), 32 - (x1 & 0xFFFFFFFF).bit_length()


def parse(line):
    """(x2, x3, {reg: 128 bits}) of a line the harness printed."""
    fields = line.split()
    return int(fields[0], 16), int(fields[1], 16), {r: int(f, 16) for r, f in zip(OUTPUTS, fields[2:])}


@unittest.skipUnless(CAN_RUN_X86, WHY_NOT)
class LiftedCodeTests(unittest.TestCase):
    maxDiff = None   # (every wrong result shown, up to 20)

    @classmethod
    def setUpClass(cls):
        cls.dir = Path(tempfile.mkdtemp(prefix='isa-lifted-'))
        (cls.dir / 'g0000.c').write_text(lifted_c())
        (cls.dir / 'harness.c').write_text(HARNESS)
        cls.obj, cls.exe = {}, {}
        for target in isa.TARGETS:
            cls.obj[target] = cls.dir / f'g0000_{target}.obj'
            compile_x86(cls.dir / 'g0000.c', cls.obj[target], target)
            compile_x86(cls.dir / 'harness.c', cls.dir / f'harness_{target}.obj', target)
            cls.exe[target] = cls.dir / f'run_{target}.exe'
            link_x86([cls.obj[target], cls.dir / f'harness_{target}.obj'], cls.exe[target])
        cls.cases = make_cases()
        cls.stdin = '\n'.join(map(case_line, cls.cases)) + '\n'

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir, ignore_errors=True)

    def results(self, target, *args, env=None):
        r = run_x86(self.exe[target], *args, stdin=self.stdin, env=env)
        if r.returncode in ILLEGAL:
            self.skipTest(f'this processor cannot run the {target} build')
        self.assertEqual(r.returncode, 0, r.stderr)
        lines = r.stdout.splitlines()
        self.assertEqual(len(lines), len(self.cases))
        return lines

    def check_exact(self, lines, what):
        wrong = []
        for n, (case, line) in enumerate(zip(self.cases, lines)):
            want, x2, x3 = expected(case)
            got2, got3, got = parse(line)
            if (got2, got3) != (x2, x3):
                wrong.append(f'case {n}: clz {got2}, {got3}, not {x2}, {x3}')
            for reg, value in want.items():
                ok = (math.isnan(f64(got[reg])) and got[reg] >> 64 == 0 if value == NAN64 else
                      math.isnan(f32(got[reg])) and got[reg] >> 32 == 0 if value == NAN32 else got[reg] == value)
                if not ok:
                    shown = value if isinstance(value, str) else f'{value:032x}'
                    wrong.append(f'case {n}: {WRITES[reg]}: {got[reg]:032x}, not {shown}')
        self.assertEqual(wrong[:20], [], f'{what}: {len(wrong)} results differ from the exact ones')

    def test_words(self):
        """(the words are the instructions named: capstone says so)"""
        try:
            import capstone
        except ImportError:
            self.skipTest('no capstone')
        md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        code = struct.pack(f'<{len(WORDS)}I', *WORDS)
        self.assertEqual([f'{i.mnemonic} {i.op_str}'.strip() for i in md.disasm(code, 0)], NAMES)

    def test_no_avx_at_x86_64_v2(self):
        text = disassembly(self, self.obj['x86-64-v2'], relocations=True)
        insns = mnemonics(text)
        self.assertEqual(avx_in(self, text), [])
        self.assertEqual(named(insns, BEYOND_V2), [])
        # what x86-64-v2 has is used (SSE4.1's rounding), and fma() is the C library's
        self.assertIn('roundsd', insns)
        self.assertRegex(text, r'\b_?fma\b')
        avx2 = disassembly(self, self.obj['avx2'])
        self.assertTrue([i for i in mnemonics(avx2) if i.startswith('vfmadd')], 'the avx2 build has no FMA instruction')

    def test_same_results_at_both(self):
        v2 = self.results('x86-64-v2')
        self.check_exact(v2, 'x86-64-v2')
        avx2 = self.results('avx2')
        self.assertEqual([n for n, (a, b) in enumerate(zip(v2, avx2)) if a != b][:20], [])

    def test_software_fma(self):
        """x86-64-v2's fma() where the C library does it without the processor's FMA instruction: Windows' with
        _set_FMA3_enable(0), glibc's told the processor has none (GLIBC_TUNABLES), a Mac's under Rosetta (which
        says there is none). (Windows' gives +0 for a product too small for a double plus +0: recomp.h's
        rt_fma_v2 takes a zero c itself.)"""
        if os.name == 'nt':
            lines = self.results('x86-64-v2', 'nofma3')
        else:
            env = dict(os.environ, GLIBC_TUNABLES='glibc.cpu.hwcaps=-FMA,-FMA4,-AVX2,-FMA_Usable,-FMA4_Usable,-AVX2_Usable')
            lines = self.results('x86-64-v2', env=env)
        self.check_exact(lines, 'x86-64-v2, fma() in software')


if __name__ == '__main__':
    unittest.main()
