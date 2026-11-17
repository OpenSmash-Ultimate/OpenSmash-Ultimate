"""tools/glslang_lib.py: the prebuilt ssbu_glslang.dll from the release of this source, checked by SHA-256, kept
while current; the build here as the fallback."""
import hashlib
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'tools'))
import glslang_lib  # noqa: E402


def release(dll, listed=None):
    listed = dll if listed is None else listed
    sums = f'{hashlib.sha256(listed).hexdigest()}  ssbu_glslang.dll\n'.encode()
    calls = []
    def get(url):
        calls.append(url)
        assert glslang_lib.prebuilt_tag() in url, url
        return sums if url.endswith('/SHA256SUMS') else dll
    return get, calls


class PrebuiltTests(unittest.TestCase):
    def test_key_is_stable_and_names_the_version(self):
        self.assertEqual(glslang_lib.prebuilt_key(), glslang_lib.prebuilt_key())
        self.assertTrue(glslang_lib.prebuilt_tag().startswith(f'glslang-{glslang_lib.VERSION}-'))

    def test_fetched_then_kept(self):
        with tempfile.TemporaryDirectory() as d:
            dll = os.path.join(d, 'ssbu_glslang.dll')
            get, calls = release(b'MZ prebuilt')
            self.assertTrue(glslang_lib.fetch_prebuilt(dll, get=get, log=lambda *a: None))
            with open(dll, 'rb') as f:
                self.assertEqual(f.read(), b'MZ prebuilt')
            n = len(calls)
            self.assertTrue(glslang_lib.fetch_prebuilt(dll, get=get, log=lambda *a: None))
            self.assertEqual(len(calls), n)   # (current: nothing downloaded)

    def test_wrong_hash_refused(self):
        with tempfile.TemporaryDirectory() as d:
            dll = os.path.join(d, 'ssbu_glslang.dll')
            get, _ = release(b'MZ tampered', listed=b'MZ prebuilt')
            with self.assertRaises(Exception):
                glslang_lib.fetch_prebuilt(dll, get=get, log=lambda *a: None)
            self.assertFalse(os.path.exists(dll))

    def test_ensure_falls_back_to_the_build(self):
        with tempfile.TemporaryDirectory() as d:
            dll = os.path.join(d, 'ssbu_glslang.dll')
            def offline(url):
                raise OSError('offline')
            said = []
            orig_fetch, orig_build = glslang_lib.fetch, glslang_lib.build
            glslang_lib.fetch = lambda *a, **k: True
            glslang_lib.build = lambda path, log=print: said.append(('build', path)) or True
            try:
                self.assertTrue(glslang_lib.ensure(dll, log=said.append, get=offline))
            finally:
                glslang_lib.fetch, glslang_lib.build = orig_fetch, orig_build
            self.assertIn(('build', dll), said)


if __name__ == '__main__':
    unittest.main()
