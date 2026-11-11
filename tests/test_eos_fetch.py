"""tools/eos_fetch.py: EOS's DLL and eos.txt from the release, checked by SHA-256; never fatal."""
import hashlib
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'tools'))
import eos_fetch  # noqa: E402


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


class EosFetchTests(unittest.TestCase):
    FILES = {'EOSSDK-Win64-Shipping.dll': b'MZ dll', 'eos.txt': b'ProductId=x\n'}

    def test_fetched_and_kept(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertTrue(eos_fetch.ensure(d, get=release(self.FILES), log=lambda *a: None))
            for n, data in self.FILES.items():
                with open(os.path.join(d, n), 'rb') as f:
                    self.assertEqual(f.read(), data)
            calls = []
            def counting(url):
                calls.append(url)
                return release(self.FILES)(url)
            self.assertTrue(eos_fetch.ensure(d, get=counting, log=lambda *a: None))
            self.assertEqual(len(calls), 1)   # (only the sums: the files are current)

    def test_wrong_hash_is_refused(self):
        with tempfile.TemporaryDirectory() as d:
            bad = dict(self.FILES, **{'eos.txt': b'tampered'})
            self.assertFalse(eos_fetch.ensure(d, get=release(bad, sums=self.FILES), log=lambda *a: None))
            self.assertFalse(os.path.exists(os.path.join(d, 'eos.txt')))

    def test_offline_is_not_fatal(self):
        def offline(url):
            raise OSError('no network')
        with tempfile.TemporaryDirectory() as d:
            self.assertFalse(eos_fetch.ensure(d, get=offline, log=lambda *a: None))


if __name__ == '__main__':
    unittest.main()
