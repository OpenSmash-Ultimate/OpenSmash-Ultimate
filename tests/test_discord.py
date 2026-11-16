"""Discord's sign-in and Rich Presence without an account or the SDK (runtime/net_discord_core.c, runtime/net_discord.c
with no discord_partner_sdk.dll beside the program): tools/dev/discordtest.c checks the token file the game and the
launchers share, the application id file, the presence's text, the join secret, the fighters' names, SHA-256 / HMAC,
reading the lobby's GET /gsp/me and the site's leaderboard.json, and that nothing changes without the SDK; its signed
request must be the one tools/nex/board.py accepts. And tools/discord_fetch.py: the SDK's DLL from the release, checked
by SHA-256, never fatal. The C part is skipped where there is no clang-cl."""
from pathlib import Path
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import cc  # noqa: E402
import discord_fetch  # noqa: E402
from nex import board  # noqa: E402

HAVE_CLANG = os.name == 'nt' and (os.path.exists(cc.CLANG) or shutil.which(cc.CLANG) is not None)
SOURCES = ['tools/dev/discordtest.c', 'runtime/net_discord_core.c', 'runtime/net_discord.c']


@unittest.skipUnless(HAVE_CLANG, 'needs clang-cl (Windows)')
class DiscordCore(unittest.TestCase):
    def test_discordtest(self):
        out = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, out, ignore_errors=True)
        exe = os.path.join(out, 'discordtest.exe')
        r = subprocess.run([cc.CLANG, '/nologo', '/O1', *cc.BASE_CFLAGS, '/I', str(ROOT / 'runtime'),
                            *[str(ROOT / s) for s in SOURCES], '/Fe' + exe, '/Fo' + out + os.sep],
                           capture_output=True, text=True, cwd=out)
        self.assertEqual(r.returncode, 0, 'discordtest.c did not build:\n' + r.stdout + r.stderr)
        env = {k: v for k, v in os.environ.items() if not k.startswith('SSBU_')}
        r = subprocess.run([exe, out], capture_output=True, text=True, cwd=out, env=env, timeout=60)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn(' 0 failed', r.stdout)
        self.assertIn('no discord_partner_sdk.dll beside the program: off', r.stderr)
        # the game's signed request is the one the lobby's game server checks
        line = next(x for x in r.stdout.splitlines() if x.startswith('sign '))
        _, player, sig, account = line.split()
        nsa = '00000000000000a1'
        self.assertEqual(player, board.player_id(nsa))
        self.assertEqual(sig, board.signature(nsa, 'profile', player, 1763300000, 'Nelly\nab12'))
        self.assertEqual(account, hashlib.sha256(b'opensmash-discord\n80351110224678912').hexdigest()[:16])


def release(files, sums=None):
    sums = sums or files
    listing = ''.join(f'{hashlib.sha256(d).hexdigest()}  {n}\n' for n, d in sums.items()).encode()

    def get(url):
        name = url.rsplit('/', 1)[1]
        if name == 'SHA256SUMS':
            return listing
        if name not in files:
            raise OSError('404')
        return files[name]
    return get


class DiscordFetch(unittest.TestCase):
    FILES = {'discord_partner_sdk.dll': b'MZ discord', 'discord_partner_sdk_notices.txt': b'Open Source Software Disclosure',
             'discord.txt': b'ApplicationId=123456789012345678\n'}

    def test_fetched_then_current(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertTrue(discord_fetch.ensure(d, get=release(self.FILES), log=lambda *a: None, files=discord_fetch.FILES))
            with open(os.path.join(d, 'discord_partner_sdk.dll'), 'rb') as f:
                self.assertEqual(f.read(), b'MZ discord')
            self.assertTrue(os.path.isfile(os.path.join(d, 'discord.txt')))
            calls = []

            def counting(url):
                calls.append(url)
                return release(self.FILES)(url)
            self.assertTrue(discord_fetch.ensure(d, get=counting, log=lambda *a: None, files=discord_fetch.FILES))
            self.assertEqual(len(calls), 1)   # (only the sums: the files are current)

    def test_a_wrong_hash_is_not_kept(self):
        with tempfile.TemporaryDirectory() as d:
            bad = release({**self.FILES, 'discord_partner_sdk.dll': b'evil'}, sums=self.FILES)
            self.assertFalse(discord_fetch.ensure(d, get=bad, log=lambda *a: None, files=discord_fetch.FILES))
            self.assertFalse(os.path.exists(os.path.join(d, 'discord_partner_sdk.dll')))
            self.assertFalse(os.path.exists(os.path.join(d, 'discord_partner_sdk.dll.part')))

    def test_a_release_without_the_id_is_not_enough(self):
        with tempfile.TemporaryDirectory() as d:
            files = {k: v for k, v in self.FILES.items() if k != 'discord.txt'}
            self.assertFalse(discord_fetch.ensure(d, get=release(files), log=lambda *a: None, files=discord_fetch.FILES))

    def test_the_id_alone_off_windows(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertTrue(discord_fetch.ensure(d, get=release(self.FILES), log=lambda *a: None, files=discord_fetch.ID_ONLY))
            self.assertEqual(sorted(os.listdir(d)), ['discord.txt'])

    def test_no_release_is_not_fatal(self):
        def offline(url):
            raise OSError('no network')
        with tempfile.TemporaryDirectory() as d:
            self.assertFalse(discord_fetch.ensure(d, get=offline, log=lambda *a: None))

    def test_switched_off(self):
        old = os.environ.get('SSBU_DISCORD')
        os.environ['SSBU_DISCORD'] = '0'
        try:
            with tempfile.TemporaryDirectory() as d:
                self.assertFalse(discord_fetch.ensure(d, get=release(self.FILES), log=lambda *a: None))
        finally:
            if old is None:
                del os.environ['SSBU_DISCORD']
            else:
                os.environ['SSBU_DISCORD'] = old


if __name__ == '__main__':
    unittest.main()
