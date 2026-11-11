"""A character a mod adds with its ui_chara_db.prcxml alone - no plugin, no CSK Collection call - on
another fighter's extra costumes (Popple: Mario's c24-c31, "color_start_index" 24). The CSK
Collection redirects such a character to those costumes on the console; here the runtime's redirect
(runtime/hle_skyline.c) knew only the characters plugins add, so Popple played as Mario's c00.
tools/mods.py prcxml_redirects lists them in dump/skyline/prcxml_redirects.txt (ui_chara hash40 and
first costume), and the runtime adds that list to its redirects. On a made-up tree and with the
runtime's functions compiled from its source."""
from pathlib import Path
import os
from unittest import mock
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import mods  # noqa: E402

POPPLE = 0xFA91015E3   # hash40("ui_chara_popple")
DB = 'ui/param/database/ui_chara_db.prcxml'


def chara_db(entries):
    """A ui_chara_db.prcxml as mods ship it: declared UTF-16, written as UTF-8, dummies before the entry."""
    out = ['<?xml version="1.0" encoding="UTF-16"?>\n<struct>\n\t<list hash="db_root">\n']
    for index, fields in entries:
        out += [f'\t\t<hash40 index="{i}">dummy</hash40>\n' for i in range(index)]
        out.append(f'\t\t<struct index="{index}">\n')
        out += [f'      <{tag} hash="{key}">{value}</{tag}>\n' for tag, key, value in fields]
        out.append('    </struct>\n')
    out.append('\t</list>\n</struct>\n')
    return ''.join(out)


POPPLE_FIELDS = [('hash40', 'ui_chara_id', 'ui_chara_popple'), ('hash40', 'fighter_kind', 'fighter_kind_mario'),
                 ('byte', 'color_num', '8'), ('hash40', '0x160ab9eb98', 'ui_chara_mario'), ('byte', '0x11895f00fc', '24')]


class PrcxmlRedirectList(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.tree = mods.Tree(self.tmp.name)
        self.listed = Path(self.tree.root, 'dump', 'skyline', 'prcxml_redirects.txt')
        self.log = []

    def tearDown(self):
        self.tmp.cleanup()

    def put(self, mod, rel, text):
        p = Path(self.tree.mods, mod, rel)
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(text.encode('utf-8'))

    def run_list(self, on):
        mods.prcxml_redirects(self.tree, on, self.log.append)
        return self.listed.read_text(encoding='utf-8').splitlines()

    def test_echo_slot_character_is_listed_with_its_first_costume(self):
        self.put('Popple _Echoslot_Oneslot', DB, chara_db([(121, POPPLE_FIELDS)]))
        self.assertEqual(self.run_list(['Popple _Echoslot_Oneslot']), [f'{POPPLE:#x} 24'])

    def test_named_key_and_hex_ui_chara_are_read_too(self):
        fields = [('hash40', 'ui_chara_id', f'{POPPLE:#x}'), ('byte', 'color_start_index', '32')]
        self.put('Named', DB, chara_db([(90, fields)]))
        self.assertEqual(self.run_list(['Named']), [f'{POPPLE:#x} 32'])

    def test_entry_without_a_costume_start_is_not_listed(self):
        # a vanilla character's row changed (its name, its order): no extra costumes
        self.put('Reorder', DB, chara_db([(3, [('hash40', 'ui_chara_id', 'ui_chara_mario'), ('sbyte', 'disp_order', '5')])]))
        self.assertEqual(self.run_list(['Reorder']), [])

    def test_mod_turned_off_is_not_listed_and_a_stale_list_is_emptied(self):
        self.listed.parent.mkdir(parents=True)
        self.listed.write_text(f'{POPPLE:#x} 24\n', encoding='utf-8')
        self.put('Popple _Echoslot_Oneslot', DB, chara_db([(121, POPPLE_FIELDS)]))
        self.assertEqual(self.run_list([]), [])

    def test_apply_with_every_mod_off_empties_a_stale_list(self):
        # apply() writes the list whatever path it takes: a list from an earlier apply would
        # otherwise keep redirecting a character whose mod is now off
        self.listed.parent.mkdir(parents=True)
        self.listed.write_text(f'{POPPLE:#x} 24\n', encoding='utf-8')
        Path(self.tree.vanilla_cfg).parent.mkdir(parents=True, exist_ok=True)
        Path(self.tree.vanilla_cfg).write_text('layered\n', encoding='utf-8')
        self.put('Popple _Echoslot_Oneslot', DB, chara_db([(121, POPPLE_FIELDS)]))
        with mock.patch.object(mods, 'sync_plugins', lambda tree, log: None):
            mods.apply(self.tree, self.log.append)
        self.assertEqual(self.listed.read_text(encoding='utf-8'), '')

    def test_apply_with_arcropolis_lists_a_turned_on_mods_character(self):
        # the path players take: the real ARCropolis among the standard plugins; the steps that read the
        # game's archive (missing shares, the select screen's layout) and the plugin build are left out here
        Path(self.tree.root, 'skyline', 'plugins').mkdir(parents=True)
        Path(self.tree.root, 'skyline', 'plugins', 'libarcropolis.nro').write_bytes(b'made-up')
        Path(self.tree.vanilla_cfg).parent.mkdir(parents=True, exist_ok=True)
        Path(self.tree.vanilla_cfg).write_text('layered\n', encoding='utf-8')
        self.put('Popple _Echoslot_Oneslot', DB, chara_db([(121, POPPLE_FIELDS)]))
        state = self.tree.load()
        state['mods']['Popple _Echoslot_Oneslot'].update(enabled=True)
        self.tree.save(state)
        with mock.patch.object(mods, 'missing_shares', lambda tree, on, log: None), \
                mock.patch.object(mods, 'select_screen_arcropolis', lambda tree, on, log: None), \
                mock.patch.object(mods, 'sync_plugins', lambda tree, log: None):
            mods.apply(self.tree, self.log.append)
        self.assertEqual(self.listed.read_text(encoding='utf-8'), f'{POPPLE:#x} 24\n')

    def test_unreadable_file_is_reported_and_the_others_still_listed(self):
        self.put('Broken', DB, '<struct><list hash="db_root"><struct index="1">')
        self.put('Popple _Echoslot_Oneslot', DB, chara_db([(121, POPPLE_FIELDS)]))
        self.assertEqual(self.run_list(['Broken', 'Popple _Echoslot_Oneslot']), [f'{POPPLE:#x} 24'])
        self.assertTrue(any('Broken' in line for line in self.log), self.log)


def function_source(source, head):
    begin = source.index(head)
    return source[begin:source.index('\n}\n', begin) + 3]


HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint64_t u64; typedef uint32_t u32; typedef uint8_t u8;
#define LOG(...) (printf("log: " __VA_ARGS__), printf("\n"))
typedef struct { u64 ui_chara, base; int start; } Redirect;
static Redirect redirects[64];
static u32 redirect_n;
%(load)s
/* argv: the list file; prints the redirect table after a plugin's (CSK) character and the list */
int main(int argc, char **argv) {
    (void)argc;
    redirects[redirect_n++] = (Redirect){0xfee536919ull, 0, 120};
    load_prcxml_redirects(argv[1]);
    for (u32 r = 0; r < redirect_n; r++) printf("%%llx %%d\n", (unsigned long long)redirects[r].ui_chara, redirects[r].start);
    return 0;
}
'''

NINTEN = 'fee536919 120'   # the plugin's character, always first


class RuntimeLoadsTheList(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('clang') or shutil.which('cc')
        if not compiler:
            raise unittest.SkipTest('C compiler not installed')
        source = (ROOT / 'runtime/hle_skyline.c').read_text(encoding='utf-8')
        cls.tmp = tempfile.TemporaryDirectory(prefix='prcxml redirect test ')
        src = Path(cls.tmp.name) / 'load.c'
        src.write_text(HARNESS % {'load': function_source(source, 'static void load_prcxml_redirects(')}, encoding='utf-8')
        cls.exe = Path(cls.tmp.name) / ('load.exe' if os.name == 'nt' else 'load')
        crt = ['-D_CRT_SECURE_NO_WARNINGS'] if os.name == 'nt' else []   # (clang on Windows: the CRT's fopen/sscanf deprecation warnings, -Werror)
        subprocess.run([compiler, '-std=c11', '-Wall', '-Werror', *crt, '-o', str(cls.exe), str(src)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def table(self, listed=None):
        path = Path(self.tmp.name) / 'prcxml_redirects.txt'
        path.unlink(missing_ok=True)
        if listed is not None:
            path.write_text(listed, encoding='utf-8')
        out = subprocess.run([str(self.exe), str(path)], check=True, capture_output=True, text=True).stdout
        return [l for l in out.splitlines() if not l.startswith('log: ')]

    def test_listed_character_is_added_with_its_first_costume(self):
        self.assertEqual(self.table(f'{POPPLE:#x} 24\n'), [NINTEN, f'{POPPLE:x} 24'])

    def test_tag_bits_above_the_hash_are_dropped(self):
        self.assertEqual(self.table(f'{(0x07 << 40) | POPPLE:#x} 24\n'), [NINTEN, f'{POPPLE:x} 24'])

    def test_character_a_plugin_added_keeps_the_plugins_redirect(self):
        self.assertEqual(self.table('0xfee536919 64\n'), [NINTEN])

    def test_bad_lines_are_skipped(self):
        self.assertEqual(self.table(f'nonsense\n{POPPLE:#x}\n{POPPLE:#x} 0\n{POPPLE:#x} 300\n'), [NINTEN])

    def test_missing_list_adds_nothing(self):
        self.assertEqual(self.table(None), [NINTEN])


if __name__ == '__main__':
    unittest.main()
