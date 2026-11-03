"""tools/runtime_fetch.py on a made-up release: the zip for the build's instruction set, checked against
SHA256SUMS, put in place; a wrong hash changes nothing; a runtime that refuses the game library is put back."""
import hashlib
import io
import os
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'tools'))
import runtime_fetch as rf  # noqa: E402


def release(isa='avx2', exe=b'new runtime', bad_hash=False):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w') as z:
        z.writestr('ssbu.exe', exe)
        z.writestr('isa.txt', isa + '\n')
        z.writestr('commit.txt', 'abc')
    data = buf.getvalue()
    name = f'opensmash-runtime-windows-{isa}.zip'
    digest = '0' * 64 if bad_hash else hashlib.sha256(data).hexdigest()
    files = {'SHA256SUMS': f'{digest}  {name}\n'.encode(), name: data}
    asked = []

    def get(url):
        asked.append(url)
        return files[url.rsplit('/', 1)[1]]
    return get, asked


@unittest.skipUnless(os.name == 'nt', 'the prebuilt runtime is for Windows')
class FetchTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.build = self.tmp.name
        for name, data in (('ssbu.exe', b'old runtime'), ('ssbu_game.dll', b'game'), ('isa.txt', b'avx2\nchosen\n')):
            with open(os.path.join(self.build, name), 'wb') as f:
                f.write(data)

    def tearDown(self):
        self.tmp.cleanup()

    def exe(self):
        with open(os.path.join(self.build, 'ssbu.exe'), 'rb') as f:
            return f.read()

    def test_in_place(self):
        get, asked = release()
        rf.fetch('runtime-v1', self.build, get=get, run_check=lambda exe: (True, ''), log=lambda _l: None)
        self.assertEqual(self.exe(), b'new runtime')
        self.assertTrue(os.path.isfile(os.path.join(self.build, 'ssbu.exe.old')))
        self.assertTrue(all(u.startswith('https://github.com/OpenSmash-Ultimate/OpenSmash-Ultimate/releases/download/runtime-v1/')
                            for u in asked))

    def test_wrong_hash_changes_nothing(self):
        get, _ = release(bad_hash=True)
        with self.assertRaises(rf.Error):
            rf.fetch('runtime-v1', self.build, get=get, run_check=lambda exe: (True, ''), log=lambda _l: None)
        self.assertEqual(self.exe(), b'old runtime')

    def test_refused_library_puts_the_old_one_back(self):
        get, _ = release()
        with self.assertRaises(rf.Error):
            rf.fetch('runtime-v1', self.build, get=get, run_check=lambda exe: (False, 'interface 2, wanted 1'), log=lambda _l: None)
        self.assertEqual(self.exe(), b'old runtime')

    def test_needs_a_game_library_build_and_a_tag(self):
        get, _ = release()
        with self.assertRaises(rf.Error):
            rf.fetch('latest', self.build, get=get, run_check=lambda exe: (True, ''), log=lambda _l: None)
        os.remove(os.path.join(self.build, 'ssbu_game.dll'))
        with self.assertRaises(rf.Error):
            rf.fetch('runtime-v1', self.build, get=get, run_check=lambda exe: (True, ''), log=lambda _l: None)

    def test_the_build_s_instruction_set(self):
        with open(os.path.join(self.build, 'isa.txt'), 'w') as f:
            f.write('x86-64-v2\n')
        get, asked = release(isa='x86-64-v2')
        rf.fetch('runtime-v1', self.build, get=get, run_check=lambda exe: (True, ''), log=lambda _l: None)
        self.assertTrue(asked[-1].endswith('opensmash-runtime-windows-x86-64-v2.zip'))


if __name__ == '__main__':
    unittest.main()
