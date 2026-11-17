"""tools/online_files.py: Epic's and Discord's files beside the program on every update and Play (issue #389); nothing
downloaded when they are current, never fatal."""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'tools'))
import discord_fetch  # noqa: E402
import eos_fetch  # noqa: E402
import online_files  # noqa: E402


class OnlineFilesTests(unittest.TestCase):
    def setUp(self):
        self.env = {k: os.environ.pop(k) for k in ('SSBU_EOS', 'SSBU_DISCORD') if k in os.environ}
        self.addCleanup(os.environ.update, self.env)
        self.calls = []

    def fetch(self, files, ok=True):
        def get(build):
            self.calls.append(files[0])
            if ok:
                for n in files:
                    with open(os.path.join(build, n), 'wb') as f:
                        f.write(n.encode())
            return ok
        return get

    def run_ensure(self, d, eos_ok=True, discord_ok=True, windows=True):
        return online_files.ensure(d, log=lambda *a: None, windows=windows,
                                   eos=self.fetch(eos_fetch.FILES, eos_ok), discord=self.fetch(discord_fetch.FILES, discord_ok))

    def test_fetched_once_then_current_without_network(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertTrue(self.run_ensure(d))
            self.assertEqual(len(self.calls), 2)
            self.assertTrue(os.path.isfile(os.path.join(d, online_files.STAMP)))
            self.assertTrue(self.run_ensure(d))
            self.assertEqual(len(self.calls), 2)   # (current: no fetch)

    def test_a_missing_file_is_fetched_again(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertTrue(self.run_ensure(d))
            os.remove(os.path.join(d, 'eos.txt'))
            self.assertTrue(self.run_ensure(d))
            self.assertEqual(len(self.calls), 4)

    def test_a_changed_file_is_fetched_again(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertTrue(self.run_ensure(d))
            with open(os.path.join(d, 'EOSSDK-Win64-Shipping.dll'), 'wb') as f:
                f.write(b'other')
            self.assertTrue(self.run_ensure(d))
            self.assertEqual(len(self.calls), 4)

    def test_a_failed_fetch_is_tried_at_the_next_run(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertFalse(self.run_ensure(d, eos_ok=False))
            self.assertFalse(os.path.isfile(os.path.join(d, online_files.STAMP)))
            self.assertTrue(self.run_ensure(d))
            self.assertEqual(len(self.calls), 4)

    def test_a_new_release_tag_is_fetched(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertTrue(self.run_ensure(d))
            old = eos_fetch.EOS_TAG
            eos_fetch.EOS_TAG = old + '-next'
            self.addCleanup(setattr, eos_fetch, 'EOS_TAG', old)
            self.assertTrue(self.run_ensure(d))
            self.assertEqual(len(self.calls), 4)

    def test_off_windows_discord_alone(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertTrue(self.run_ensure(d, eos_ok=False, windows=False))
            self.assertEqual(self.calls, ['discord_partner_sdk.dll'])

    def test_switched_off(self):
        os.environ['SSBU_EOS'] = '0'
        os.environ['SSBU_DISCORD'] = '0'
        self.addCleanup(os.environ.pop, 'SSBU_EOS', None)
        self.addCleanup(os.environ.pop, 'SSBU_DISCORD', None)
        with tempfile.TemporaryDirectory() as d:
            self.assertTrue(self.run_ensure(d, eos_ok=False, discord_ok=False))
            self.assertEqual(self.calls, [])

    def test_every_update_and_play_runs_it(self):
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        def read(*p):
            with open(os.path.join(root, *p), encoding='utf-8') as f:
                return f.read()
        ps1 = read('tools', 'install_windowsmash.ps1')
        early = ps1[ps1.index("nothing that goes into the game changed: no build"):]
        early = early[:early.index('return $true')]
        self.assertIn('online_files.py', early)   # (Step-Build's early return: issue #389)
        build = read('tools', 'build_game.py')
        prep = build[build.index('if a.prepare_only:'):]
        self.assertLess(prep.index('online_files(log)'), prep.index('return'))
        self.assertIn('online_files.py', read('OpenSmash.Launcher', 'LauncherForm.cs'))


if __name__ == '__main__':
    unittest.main()
