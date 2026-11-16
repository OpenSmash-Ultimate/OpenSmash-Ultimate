"""Shared catalog validation and the production external menu, with synthetic data only."""
from pathlib import Path
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import localization as L  # noqa: E402
from test_lobby_menu import Menu, build  # noqa: E402
from test_net_client_v3 import HAVE_CLANG  # noqa: E402


class CatalogTests(unittest.TestCase):
    def test_committed_catalog_is_current(self):
        self.assertEqual(L.OUTPUT.read_text(encoding='utf-8'), L.generate(L.load_catalogs()))

    def test_placeholder_safety(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)
            (path / 'en.json').write_text('{"name":"English","messages":{}}')
            for source, target in [('%s: %d', '%d: %s'), ('%s', '%n'), ('2%s', '2%d'), ('Name {0}', 'Name {1}'), ('{0:N0}', '{0:N2}')]:
                with self.subTest(source=source, target=target):
                    (path / 'fr.json').write_text(json.dumps({'name': 'French', 'messages': {source: target}}))
                    with self.assertRaisesRegex(ValueError, 'placeholders'):
                        L.load_catalogs(path)
            (path / 'fr.json').write_text('{"name":"French","messages":{"Name":"A","Name":"B"}}')
            with self.assertRaisesRegex(ValueError, 'duplicate'):
                L.load_catalogs(path)

    def test_new_language_needs_only_a_catalog(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)
            (path / 'en.json').write_text('{"name":"English","messages":{}}')
            (path / 'fr.json').write_text('{"name":"Français","messages":{"Settings":"Paramètres","{0} of {1}":"{1} sur {0}"}}', encoding='utf-8')
            catalogs = L.load_catalogs(path)
            generated = L.generate(catalogs)
            self.assertIn('"fr"', generated)
            self.assertIn('"Settings"', generated)
            self.assertIn('"SETTINGS"', generated)
            self.assertIn('UI_GLYPH_COUNT', generated)

    def test_shipped_languages_cover_core_ui_and_glyphs(self):
        catalogs = dict(L.load_catalogs())
        glyphs = set(L.catalog_glyphs(L.load_catalogs()))
        for code in ('de', 'es', 'fr', 'ja', 'ko', 'zh-CN'):
            with self.subTest(language=code):
                messages = catalogs[code]['messages']
                for source in ('Settings', 'Interface language', 'Game language', 'Display', 'Controllers', 'Volume', 'Restart the game'):
                    self.assertIn(source, messages)
                    self.assertTrue(messages[source])
                self.assertGreaterEqual(len(messages), 150)
                for cp in map(ord, catalogs[code]['name'] + ''.join(messages.values())):
                    if cp >= 128:
                        self.assertIn(cp, glyphs)

    def test_literal_percent_is_not_a_printf_argument(self):
        self.assertEqual(L.placeholders('Push past 80% strength.'), ([], []))
        self.assertEqual(L.placeholders('%d%%')[0], ['%d'])


@unittest.skipUnless(HAVE_CLANG, 'needs clang')
class Utf8Tests(unittest.TestCase):
    def test_layout_in_native_c(self):
        with tempfile.TemporaryDirectory() as folder:
            exe = str(Path(folder) / ('utf8.exe' if os.name == 'nt' else 'utf8'))
            source = str(ROOT / 'tools/dev/ui_utf8_test.c')
            if os.name == 'nt':
                import cc
                command = [cc.CLANG, '/nologo', *cc.LINK_FLAGS, *cc.CFLAGS, source, '/Fe' + exe, '/Fo' + folder + os.sep,
                           '/link', *cc.LINK_OPTIONS]
            else:
                command = [shutil.which('clang'), source, '-o', exe]
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([exe], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


@unittest.skipUnless(os.name == 'nt' and HAVE_CLANG, 'Windows overlay uses GDI')
class WindowsOverlayTests(unittest.TestCase):
    def test_actual_font_atlas_and_unicode_quads(self):
        import cc
        with tempfile.TemporaryDirectory() as folder:
            exe = str(Path(folder) / 'overlay.exe')
            command = [cc.CLANG, '/nologo', '/O1', *cc.LINK_FLAGS, *cc.CFLAGS,
                       str(ROOT / 'tools/dev/ui_overlay_test.c'), '/Fe' + exe, '/Fo' + folder + os.sep,
                       '/link', *cc.LINK_OPTIONS, *cc.SYSTEM_LIBS]
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([exe], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


@unittest.skipUnless(HAVE_CLANG, 'needs clang')
class MenuLocaleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.folder = tempfile.TemporaryDirectory(prefix='UI locale menu ')
        try:
            cls.exe = build(cls.folder.name)
        except BaseException:
            cls.folder.cleanup()
            raise

    @classmethod
    def tearDownClass(cls):
        cls.folder.cleanup()

    def menu(self, code='zh-CN', **kwargs):
        menu = Menu(self.exe, self.folder.name, SSBU_UI_LANGUAGE=code, **kwargs)
        self.addCleanup(menu.close)
        menu.key('ESC')
        return menu

    def test_chinese_labels_help_and_draw(self):
        menu = self.menu()
        self.assertIn('窗口模式', menu.labels())
        self.assertIn('窗口', menu.row('窗口模式')['help'])
        drawing = menu.draw()
        self.assertIn('设置', drawing)
        self.assertTrue(any('显示' in line for line in drawing), drawing)
        menu.type('4')
        self.assertEqual(menu.row('界面语言')['value'], '跟随系统')
        self.assertIn('游戏语言', menu.labels())

    def test_switch_persists_only_ui_language_and_preserves_other_settings(self):
        before = '; keep this comment\nwindow = borderless\nlanguage = ja\nui_language = zh-CN ; UI only\n'
        menu = self.menu('', files={'display.ini': before})
        menu.type('4')
        menu.go_to('界面语言')
        for _ in range(next(i for i, (code, _) in enumerate(L.load_catalogs()) if code == 'zh-CN')):
            menu.key('LEFT')
        self.assertIn('Interface language', menu.labels())
        saved = (menu.dir / 'display.ini').read_text(encoding='utf-8')
        self.assertIn('; keep this comment', saved)
        self.assertIn('ui_language = en ; UI only', saved)
        self.assertIn('window = borderless', saved)
        self.assertIn('language = ja', saved)
        for _ in range(next(i for i, (code, _) in enumerate(L.load_catalogs()) if code == 'zh-CN')):
            menu.key('RIGHT')
        self.assertIn('界面语言', menu.labels())

    def test_unsupported_language_and_missing_messages_fall_back(self):
        menu = self.menu('not-a-language')
        self.assertIn('Window', menu.labels())
        menu.type('4')
        self.assertIn('Interface language', menu.labels())

    def test_locale_alias_and_environment_override(self):
        menu = self.menu('ZH_hAnS.UTF-8', files={'display.ini': 'ui_language = en\n'})
        self.assertIn('窗口模式', menu.labels())
        self.assertEqual(menu.file('display.ini'), 'ui_language = en')

    def test_each_added_language_and_regional_alias(self):
        for code in ('de', 'es', 'fr', 'ja', 'ko'):
            with self.subTest(language=code):
                catalog = dict(L.load_catalogs())[code]['messages']
                menu = self.menu(code.upper() + '_XX.UTF-8', files={'display.ini': 'language = en\n'})
                self.assertIn(catalog['Window'], menu.labels())
                menu.type('4')
                self.assertIn(catalog['Interface language'], menu.labels())
                self.assertIn(catalog['Game language'], menu.labels())
                self.assertEqual(menu.file('display.ini'), 'language = en')

    def test_player_name_is_not_translated(self):
        menu = self.menu('zh-CN', files={'online_name.txt': 'Settings\n'})
        menu.type('3')
        self.assertEqual(menu.row('玩家名称')['value'], 'Settings')


if __name__ == '__main__':
    unittest.main()
