"""The Mods app's switches for many mods at once (tools/modmanager/server.py): a download's parts
(/api/group) and every mod of My mods (/api/all). Off remembers which were on; on brings back just
those, so a pack's alternative left off (Sonic Re-Imagined's other style, a "(2)" copy) stays off.
HewDraw Remix's mods are left to its own switch. On a made-up tree, through App.api."""
from pathlib import Path
import os
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'tools' / 'modmanager'))
import server  # noqa: E402

PACK = ['Sonic Pack - Battle Style', 'Sonic Pack - Modern Style', 'Sonic Pack - Voices']


class Switches(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        for m in ['Cool Mario', *PACK, 'hdr']:
            (root / 'mods' / m / 'fighter').mkdir(parents=True)
        self.app = server.App(str(root), 'key', False)
        state = self.app.tree.load()   # (a folder just found starts off: these were turned on)
        for n in state['mods']:
            state['mods'][n]['enabled'] = True
        state['mods']['Sonic Pack - Modern Style']['enabled'] = False   # (the pack's alternative, installed off)
        state['mods']['hdr']['enabled'] = False
        self.app.tree.save(state)
        patch = mock.patch.object(self.app, 'library', lambda: None)   # (the page's listing: not what's tested)
        patch.start()
        self.addCleanup(patch.stop)

    def tearDown(self):
        self.tmp.cleanup()

    def on(self):
        state = self.app.tree.load()
        return sorted(n for n, m in state['mods'].items() if m.get('enabled'))

    def call(self, path, body):
        self.app.api('POST', path, {}, body)

    def test_a_group_back_on_keeps_its_alternative_off(self):
        self.call('/api/group', {'names': PACK, 'on': False})
        self.assertEqual(self.on(), ['Cool Mario'])
        self.call('/api/group', {'names': PACK, 'on': True})
        self.assertEqual(self.on(), ['Cool Mario', 'Sonic Pack - Battle Style', 'Sonic Pack - Voices'])

    def test_a_group_with_nothing_remembered_turns_every_part_on(self):
        for n in PACK:
            self.call('/api/enable', {'names': [n], 'enabled': False})
        self.call('/api/group', {'names': PACK, 'on': True})
        self.assertEqual(self.on(), sorted(['Cool Mario', *PACK]))

    def test_all_off_then_on_brings_back_what_was_on(self):
        self.call('/api/all', {'on': False})
        self.assertEqual(self.on(), [])
        self.call('/api/all', {'on': False})   # (again: what's remembered stays)
        self.call('/api/all', {'on': True})
        self.assertEqual(self.on(), ['Cool Mario', 'Sonic Pack - Battle Style', 'Sonic Pack - Voices'])

    def test_all_leaves_hdr_to_its_own_switch(self):
        state = self.app.tree.load()
        state['mods']['hdr']['enabled'] = True
        self.app.tree.save(state)
        self.call('/api/all', {'on': False})
        self.assertEqual(self.on(), ['hdr'])


if __name__ == '__main__':
    unittest.main()
