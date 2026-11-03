"""tools/data_location.py on made-up folders: dump and build moved to another folder, a link left in their
place, the files read through it, and moved back home."""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'tools'))
import data_location as dl  # noqa: E402


class MoveTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = os.path.join(self.tmp.name, 'repo')
        self.other = os.path.join(self.tmp.name, 'other drive')
        for name, text in (('dump', 'game'), ('build', 'exe')):
            os.makedirs(os.path.join(self.root, name, 'sub'))
            with open(os.path.join(self.root, name, 'sub', 'f.txt'), 'w') as f:
                f.write(text)

    def tearDown(self):
        for name in dl.FOLDERS:   # (a junction left behind would make the cleanup walk into its target)
            here = os.path.join(self.root, name)
            if dl.is_link(here):
                dl.remove_link(here)
        self.tmp.cleanup()

    def read(self, name):
        with open(os.path.join(self.root, name, 'sub', 'f.txt')) as f:
            return f.read()

    def test_moved_and_back(self):
        dl.move(self.root, self.other, log=lambda _l: None)
        for name in dl.FOLDERS:
            self.assertTrue(dl.is_link(os.path.join(self.root, name)))
            self.assertTrue(os.path.isfile(os.path.join(self.other, name, 'sub', 'f.txt')))
        self.assertEqual((self.read('dump'), self.read('build')), ('game', 'exe'))
        dl.move(self.root, self.root, log=lambda _l: None)
        for name in dl.FOLDERS:
            self.assertFalse(dl.is_link(os.path.join(self.root, name)))
            self.assertFalse(os.path.exists(os.path.join(self.other, name)))
        self.assertEqual((self.read('dump'), self.read('build')), ('game', 'exe'))

    def test_moved_again_elsewhere(self):
        dl.move(self.root, self.other, log=lambda _l: None)
        third = os.path.join(self.tmp.name, 'third')
        dl.move(self.root, third, log=lambda _l: None)
        self.assertTrue(os.path.isfile(os.path.join(third, 'dump', 'sub', 'f.txt')))
        self.assertFalse(os.path.exists(os.path.join(self.other, 'dump')))
        self.assertEqual(self.read('dump'), 'game')

    def test_refuses_a_folder_in_the_way(self):
        os.makedirs(os.path.join(self.other, 'dump'))
        with self.assertRaises(SystemExit):
            dl.move(self.root, self.other, log=lambda _l: None)
        self.assertFalse(dl.is_link(os.path.join(self.root, 'dump')))
        self.assertEqual(self.read('dump'), 'game')


if __name__ == '__main__':
    unittest.main()
