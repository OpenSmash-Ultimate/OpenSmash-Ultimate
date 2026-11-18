"""tools/online_files.py: Discord's files beside the program on every update and Play (issue #389); nothing downloaded
when they are current, never fatal. (Epic Online Services' files are gone with EOS, 0.5.0.)"""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'tools'))
import discord_fetch  # noqa: E402
import online_files  # noqa: E402


class OnlineFilesTests(unittest.TestCase):
    def setUp(self):
        names = ('SSBU_EOS', 'SSBU_DISCORD', 'OPENSMASH_EOS', 'SSBU_EOS_FIRST', 'SSBU_NET_RELAY')
        self.env = {k: os.environ.pop(k) for k in names if k in os.environ}
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

    def run_ensure(self, d, discord_ok=True, windows=True):
        files = discord_fetch.FILES if windows else discord_fetch.ID_ONLY
        return online_files.ensure(d, log=lambda *a: None, windows=windows, discord=self.fetch(files, discord_ok))

    def test_fetched_once_then_current_without_network(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertTrue(self.run_ensure(d))
            self.assertEqual(len(self.calls), 1)
            self.assertTrue(os.path.isfile(os.path.join(d, online_files.STAMP)))
            self.assertTrue(self.run_ensure(d))
            self.assertEqual(len(self.calls), 1)   # (current: no fetch)

    def test_a_missing_file_is_fetched_again(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertTrue(self.run_ensure(d))
            os.remove(os.path.join(d, discord_fetch.FILES[0]))
            self.assertTrue(self.run_ensure(d))
            self.assertEqual(len(self.calls), 2)

    def test_a_changed_file_is_fetched_again(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertTrue(self.run_ensure(d))
            with open(os.path.join(d, discord_fetch.FILES[0]), 'wb') as f:
                f.write(b'other')
            self.assertTrue(self.run_ensure(d))
            self.assertEqual(len(self.calls), 2)

    def test_a_failed_fetch_is_tried_at_the_next_run(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertFalse(self.run_ensure(d, discord_ok=False))
            self.assertFalse(os.path.isfile(os.path.join(d, online_files.STAMP)))
            self.assertTrue(self.run_ensure(d))
            self.assertEqual(len(self.calls), 2)

    def test_a_new_release_tag_is_fetched(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertTrue(self.run_ensure(d))
            old = discord_fetch.DISCORD_TAG
            discord_fetch.DISCORD_TAG = old + '-next'
            self.addCleanup(setattr, discord_fetch, 'DISCORD_TAG', old)
            self.assertTrue(self.run_ensure(d))
            self.assertEqual(len(self.calls), 2)

    def test_an_older_stamp_with_epic_is_not_current(self):
        # (v0.4.x wrote Epic Online Services' release in the stamp too: Discord's files are checked again once)
        with tempfile.TemporaryDirectory() as d:
            self.assertTrue(self.run_ensure(d))
            with open(os.path.join(d, online_files.STAMP), encoding='utf-8') as f:
                text = f.read()
            with open(os.path.join(d, online_files.STAMP), 'w', encoding='utf-8') as f:
                f.write('# eos-1\n' + text)
            self.assertTrue(self.run_ensure(d))
            self.assertEqual(len(self.calls), 2)
            self.assertTrue(self.run_ensure(d))
            self.assertEqual(len(self.calls), 2)

    def test_off_windows_discord_id_alone(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertTrue(self.run_ensure(d, windows=False))
            self.assertEqual(self.calls, [discord_fetch.ID_ONLY[0]])

    def test_nothing_of_epic_whatever_is_asked(self):
        # (Epic Online Services are gone: neither the variables nor an "eos" line fetch anything of theirs)
        for name, value in (('OPENSMASH_EOS', '1'), ('SSBU_EOS', '1'), ('SSBU_EOS_FIRST', '1'), ('SSBU_NET_RELAY', 'eos')):
            os.environ[name] = value
            self.addCleanup(os.environ.pop, name, None)
        self.assertEqual([t for t, _ in online_files.wanted(True)], [discord_fetch.DISCORD_TAG])
        self.assertFalse(hasattr(online_files, 'eos_asked'))

    def test_switched_off(self):
        os.environ['SSBU_DISCORD'] = '0'
        self.addCleanup(os.environ.pop, 'SSBU_DISCORD', None)
        with tempfile.TemporaryDirectory() as d:
            self.assertTrue(self.run_ensure(d, discord_ok=False))
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
