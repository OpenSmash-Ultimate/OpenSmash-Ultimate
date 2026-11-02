"""Rollback netplay's netcode and link (runtime/hle_rb_net.c, runtime/hle_rb_link.c; SSBU_RB, docs/TECHNICAL.md)
through tools/dev/rbnettest.c and tools/dev/rblinktest.c, with no game. rbnettest: two to four made-up
peers run a toy fight over a link that drops, delays and reorders messages, each rolling back on its
wrong guesses; it passes when every peer's confirmed states agree frame by frame and equal a run with
every pad known - with input delays 0, 2 and 4, a peer that runs slower for a while, three and four
players through the host, a player leaving a 3- or 4-player match - and it must fail with rollbacks
turned off (the test can fail). rblinktest (Windows: the link is winsock's): the link on this PC's
loopback, listening on 127.0.0.1 only (SSBU_RB_BIND) so that no firewall asks - a waiting game found,
a game following the other to another port, strangers dropped, the host with several players.
Built with clang-cl on Windows (as tests/test_kernel_waits.py builds waittest.c), rbnettest with clang
elsewhere; skipped without one. Both from Piaka11's WindowSmash PR #103 (GPL-3.0)."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import cc  # noqa: E402

if os.name == 'nt':
    HAVE_CLANG = os.path.exists(cc.CLANG) or shutil.which(cc.CLANG) is not None
else:
    HAVE_CLANG = shutil.which('clang') is not None


def build(folder, name, sources, libs=()):
    srcs = [str(ROOT / s) for s in sources]
    if os.name == 'nt':
        exe = os.path.join(folder, name + '.exe')
        cmd = [cc.CLANG, '/nologo', '/O2', *cc.LINK_FLAGS, *cc.BASE_CFLAGS, *srcs, '/Fe' + exe, '/Fo' + folder + os.sep,
               '/link', *cc.LINK_OPTIONS, *libs]
    else:
        exe = os.path.join(folder, name)
        cmd = ['clang', '-O2', '-w', *srcs, '-o', exe]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.returncode:
        raise AssertionError(f'{name} did not build:\n' + r.stdout + r.stderr)
    return exe


@unittest.skipUnless(HAVE_CLANG, 'needs clang-cl on Windows (deps\\llvm, or Visual Studio\'s C++ Clang tools), or clang')
class NetcodeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp(prefix='rbnettest ')
        cls.exe = build(cls.dir, 'rbnettest', ['tools/dev/rbnettest.c', 'runtime/hle_rb_net.c'])

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir, ignore_errors=True)

    def run_test(self, *args):
        return subprocess.run([self.exe, *map(str, args)], capture_output=True, text=True, timeout=300)

    def passes(self, *args):
        r = self.run_test(*args)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertRegex(r.stdout, r'frames compared with the run with all pads known: PASS\n')
        return r

    def test_two_players_each_delay(self):
        for delay in (0, 2, 4):
            with self.subTest(delay=delay):
                self.passes(delay)

    def test_a_slow_peer(self):
        """The frame advantage holds the other back while one runs slower for a while."""
        self.assertIn('(B slow)', self.passes(2, 'slow').stdout)
        self.passes(2, 'slow', 3)

    def test_three_and_four_players(self):
        for players in (3, 4):
            for delay in (0, 2, 4):
                with self.subTest(players=players, delay=delay):
                    self.assertIn(f'{players} players', self.passes(delay, '-', players).stdout)

    def test_a_player_leaves(self):
        """The host cuts the last player off, and the players left stay the same."""
        for players in (3, 4):
            for delay in (0, 2, 4):
                with self.subTest(players=players, delay=delay):
                    self.assertIn('the last one leaves', self.passes(delay, 'leave', players).stdout)

    def test_without_rollbacks_it_fails(self):
        """The test can fail: with the wrong guesses kept, the peers go apart."""
        for args in ((2, 'norollback'), (2, 'norollback', 3)):
            with self.subTest(args=args):
                r = self.run_test(*args)
                self.assertNotEqual(r.returncode, 0, r.stdout + r.stderr)


@unittest.skipUnless(os.name == 'nt' and HAVE_CLANG, 'the link is winsock\'s: Windows, with clang-cl')
class LinkTests(unittest.TestCase):
    def test_loopback(self):
        with tempfile.TemporaryDirectory(prefix='rblinktest ') as folder:
            exe = build(folder, 'rblinktest', ['tools/dev/rblinktest.c', 'runtime/hle_rb_link.c'],
                        ['ws2_32.lib', 'ole32.lib', 'oleaut32.lib'])
            env = {**os.environ, 'SSBU_RB_BIND': '127.0.0.1'}
            env.pop('SSBU_RB_UPNP', None)   # (the router left alone)
            r = subprocess.run([exe], capture_output=True, text=True, timeout=120, env=env)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn('link test: PASS', r.stdout)
            self.assertIn('UPnP off', r.stderr)


if __name__ == '__main__':
    unittest.main()
