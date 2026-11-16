"""The launcher's settings (OpenSmash.Launcher/SettingDefaults.cs, PadProfile.cs) against the game's own defaults and
names, read from the runtime's sources: the default display.ini and the display settings' starting values
(runtime/gpu_display.c, renderer.h), controllers.ini's [general] (runtime/hle_pad.c), the master volume
(runtime/ipc_audio.c), the online files and the lists the in-game settings menu cycles through (runtime/gpu_menu.c),
and controllers.ini's button and control names. A setting changed on one side only fails here, so a fresh download's
launcher never shows (or writes) something other than what the game does. Text only: runs anywhere."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parent.parent
LAUNCHER = ROOT / 'OpenSmash.Launcher'
RUNTIME = ROOT / 'runtime'


def read(path):
    return path.read_text(encoding='utf-8-sig')


def launcher_table():
    text = read(LAUNCHER / 'SettingDefaults.cs')
    block = text[text.index('Table ='):text.index('};', text.index('Table ='))]
    rows = re.findall(r'\("([^"]+)", "([^"]*)", "([^"]*)", "([^"]*)"\)', block)
    return {(f, k): d for f, _s, k, d in rows}, {(f, k): s for f, s, k, _d in rows}


def launcher_choices():
    text = read(LAUNCHER / 'SettingDefaults.cs')
    block = text[text.index('Choices ='):text.index('};', text.index('Choices ='))]
    return {k: re.findall(r'"([^"]*)"', v) for k, v in re.findall(r'\["(\w+)"\] = new\[\] \{([^}]*)\}', block)}


def c_string_block(text, start):
    """The C string literals after `start` up to the closing ';', joined (escapes \\n kept as newlines)."""
    i = text.index(start)
    end = min(j for j in (text.find('\n    ;', i), text.find('";\n', i) + 1) if j > 0)
    return ''.join(s.replace('\\n', '\n').replace('\\"', '"') for s in re.findall(r'"((?:[^"\\]|\\.)*)"', text[i:end]))


def windows_only(text):
    """The source as the Windows build sees it: the #ifdef _WIN32 / __APPLE__ branches resolved (enough for these files)."""
    out, stack = [], []
    for line in text.split('\n'):
        t = line.strip()
        if t.startswith('#ifdef ') or t.startswith('#ifndef ') or t.startswith('#if '):
            cond = t.split(None, 1)[1]
            if t.startswith('#ifdef'):
                on = cond == '_WIN32'
            elif t.startswith('#ifndef'):
                on = cond != '_WIN32'
            else:
                on = 'defined _WIN32' in cond or cond == '_WIN32' or ('!defined __APPLE__' in cond)
            stack.append([on, on])
            continue
        if t.startswith('#elif'):
            top = stack[-1]
            on = not top[1] and ('_WIN32' in t and '!' not in t)
            top[0] = on
            top[1] = top[1] or on
            continue
        if t.startswith('#else'):
            top = stack[-1]
            top[0] = not top[1]
            top[1] = True
            continue
        if t.startswith('#endif'):
            stack.pop()
            continue
        if all(s[0] for s in stack):
            out.append(line)
    return '\n'.join(out)


def ini_values(text):
    vals = {}
    for line in text.split('\n'):
        line = line.strip()
        if not line or line[0] in ';#[' or '=' not in line:
            continue
        k, v = line.split('=', 1)
        vals.setdefault(k.strip(), v.strip())
    return vals


class LauncherDefaultsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.table, cls.sections = launcher_table()
        cls.choices = launcher_choices()
        cls.renderer_h = windows_only(read(RUNTIME / 'renderer.h'))
        cls.renderer_default = re.search(r'#define RENDERER_DEFAULT_NAME "(\w+)"', cls.renderer_h).group(1)
        display = windows_only(read(RUNTIME / 'gpu_display.c'))
        default_ini = c_string_block(display.replace('"renderer = " RENDERER_DEFAULT_NAME', '"renderer = %s"' % cls.renderer_default),
                                     'static const char DEFAULT_INI[] =')
        cls.display_ini = ini_values(default_ini)
        cls.display_c = display
        cls.pad_c = read(RUNTIME / 'hle_pad.c')
        cls.menu_c = read(RUNTIME / 'gpu_menu.c')

    def test_table_is_read(self):
        self.assertGreaterEqual(len(self.table), 16)
        self.assertEqual(self.renderer_default, 'auto', 'Windows renderer default is auto (Vulkan first)')

    def test_display_ini_defaults(self):
        """Each display.ini key the launcher shows has the default the game writes into a new display.ini."""
        for (f, key), default in self.table.items():
            if f != 'display.ini':
                continue
            self.assertIn(key, self.display_ini, f'{key} is not in the game\'s default display.ini')
            self.assertEqual(self.display_ini[key], default, f'display.ini {key}: launcher {default!r}, game {self.display_ini[key]!r}')

    def test_display_starting_values(self):
        """A display.ini without the key: the game's starting values (gpu_display.c's cfg) are the same defaults."""
        m = re.search(r'static struct \{(.*?)\} cfg = \{(.*?)\};', self.display_c, re.S)
        fields = []
        for decl in m.group(1).split(';'):
            decl = re.sub(r'/\*.*?\*/', '', decl, flags=re.S).strip()
            if not decl:
                continue
            names = decl.split(None, 1)[1] if ' ' in decl else ''
            for n in names.split(','):
                fields.append(re.sub(r'\[.*?\]', '', n).strip().lstrip('*'))
        values = [v.strip() for v in m.group(2).split(',')]
        cfg = dict(zip(fields, values))
        modes = {'MODE_WINDOWED': 'windowed', 'MODE_BORDERLESS': 'borderless', 'MODE_FULLSCREEN': 'fullscreen'}
        pacing = {'PACING_CLOCK': 'clock', 'PACING_VRR': 'vrr', 'PACING_DISPLAY': 'display'}
        self.assertEqual(modes[cfg['mode']], self.table[('display.ini', 'window')])
        self.assertEqual(cfg['width'], self.table[('display.ini', 'width')])
        self.assertEqual(cfg['height'], self.table[('display.ini', 'height')])
        self.assertEqual(cfg['hide_cursor'], self.table[('display.ini', 'hide_cursor')])
        self.assertEqual(cfg['overlay'], self.table[('display.ini', 'overlay')])
        self.assertEqual(pacing[cfg['pacing']], self.table[('display.ini', 'pacing')])
        self.assertEqual(cfg['gpu'].strip('"'), self.table[('display.ini', 'gpu')])
        self.assertEqual(float(cfg['render_scale'].rstrip('f')), float(self.table[('display.ini', 'render_scale')]))
        self.assertEqual(cfg['renderer'], 'RENDERER_DEFAULT')

    def test_volume(self):
        audio = read(RUNTIME / 'ipc_audio.c')
        self.assertIn('"volume"', audio)
        self.assertEqual(re.search(r'atoi\(v\) : (\d+);', audio).group(1), self.table[('display.ini', 'volume')])

    def test_ui_language(self):
        self.assertIn('ui_language', read(RUNTIME / 'ui_locale.h'))
        self.assertEqual(self.display_ini['ui_language'], self.table[('display.ini', 'ui_language')])

    def test_controllers_general(self):
        self.assertIn('const char *pad_default_ini(void) { return DEFAULT_INI; }', self.pad_c)
        pad_ini = ini_values(c_string_block(self.pad_c, 'static const char DEFAULT_INI[] ='))
        for key in ('keyboard_player', 'background'):
            self.assertEqual(self.sections[('controllers.ini', key)], 'general')
            self.assertEqual(pad_ini[key], self.table[('controllers.ini', key)], f'controllers.ini {key}')
        init = re.search(r'\} cfg = \{([^;]*)\};', self.pad_c).group(1)
        self.assertEqual(int(re.search(r'\.keyboard_slot = (\d+)', init).group(1)) + 1, int(self.table[('controllers.ini', 'keyboard_player')]))
        self.assertEqual(re.search(r'\.background = (\d+)', init).group(1), self.table[('controllers.ini', 'background')])

    def test_online_files(self):
        load = self.menu_c[self.menu_c.index('static void net_load(void)'):]
        load = load[:load.index('\n}\n')]
        self.assertRegex(load, r'if \(!host\[0\] \|\| !_stricmp\(host, "off"\)\)\s*net\.mode = NET_OFFLINE')
        self.assertEqual(self.table[('online_server.txt', '')], 'off')
        self.assertRegex(load, r'net\.direct = !\(settings_line_read\(config_path\(CONFIG_ONLINE_DIRECT\).*!_strnicmp\(direct, "off", 3\)\)')
        self.assertEqual(self.table[('online_direct.txt', '')], 'on')
        self.assertEqual(self.table[('online_name.txt', '')], '')
        public = re.search(r'#define PUBLIC_LOBBY "([^"]+)"', self.menu_c).group(1)
        self.assertIn(f'PublicLobby = "{public}"', read(LAUNCHER / 'SettingDefaults.cs'))

    def test_lobby_choices(self):
        """The lobby lines the game's menu writes are exactly the launcher's choices (private and custom carry an address)."""
        save = self.menu_c[self.menu_c.index('static void net_save_server(void)'):]
        save = save[:save.index('\n}\n')]
        written = set(re.findall(r'snprintf\(line, sizeof line, "(\w+)"\)', save))
        self.assertEqual(written, {'off', 'lan', 'auto'})
        as_choice = {'off': 'offline'}
        self.assertEqual({as_choice.get(w, w) for w in written} | {'private', 'custom'}, set(self.choices['lobby']))
        modes = re.search(r'enum \{ NET_PUBLIC, (.*?), NET_MODE_COUNT \}', self.menu_c).group(1)
        self.assertEqual(len(modes.split(',')) + 1, len(self.choices['lobby']), 'one launcher choice per game menu choice')

    def test_lists(self):
        def body(name):
            start = self.display_c.index(f'static void parse_{name}(')
            return self.display_c[start:self.display_c.index('\n}\n', start)]
        parse = {k: re.findall(r'!_stricmp\(v, "(\w+)"\)', body(k)) for k in ('window', 'pacing')}
        self.assertEqual(parse['window'], self.choices['window'])
        self.assertEqual(parse['pacing'], self.choices['pacing'])
        scales = re.search(r'SCALES\[\] = \{([^}]*)\}', self.menu_c).group(1)
        self.assertEqual(re.findall(r'"([^"]+)"', scales), self.choices['render_scale'])
        sizes = re.search(r'SIZES\[\]\[2\] = \{(.*?)\};', self.menu_c).group(1)
        self.assertEqual([f'{w}x{h}' for w, h in re.findall(r'\{(\d+), (\d+)\}', sizes)], self.choices['window_size'])
        names = re.search(r'renderer_name\(int r\) \{ return (.*?); \}', self.renderer_h).group(1)
        self.assertEqual(set(re.findall(r'"(\w+)"', names)) | {self.renderer_default}, set(self.choices['renderer']))

    def test_launcher_reads_through_the_table(self):
        """ReadSettings and WriteSettingsFiles take a missing value from SettingDefaults, not literals of their own."""
        form = read(LAUNCHER / 'LauncherForm.cs')
        read_block = form[form.index('        void ReadSettings()'):form.index('        bool SettingOn(')]
        write_block = form[form.index('        void WriteSettingsFiles()'):form.index('        bool SaveGameSettings()')]
        for block in (read_block, write_block):
            self.assertNotRegex(block, r'\?\? "(?:windowed|1280|720|clock|1|auto|100)"')
            self.assertNotRegex(block, r'\("(?:window|pacing|volume|renderer)", "[^"]+"\)')
        self.assertIn('SettingDefaults.Of(', read_block)

    def test_pad_names(self):
        prof = read(LAUNCHER / 'PadProfile.cs')
        buttons = re.findall(r'"(\w+)"', re.search(r'Buttons = \{([^}]*)\}', prof).group(1))
        sources = re.findall(r'"(\w+)"', re.search(r'Sources = \{([^}]*)\}', prof).group(1))
        npad = re.findall(r'"(\w+)"', re.search(r'NPAD_BUTTON_NAMES\[16\] = \{([^}]*)\}', self.pad_c).group(1))
        raw = re.findall(r'"(\w+)"', re.search(r'RAW_BUTTON_NAMES\[RB_COUNT\] = \{([^}]*)\}', self.pad_c).group(1))
        self.assertEqual(sorted(buttons), sorted(npad))
        self.assertEqual(sources, raw + ['lefttrigger', 'righttrigger'])
        self.assertIn('"lefttrigger"', self.pad_c)
        self.assertIn('"righttrigger"', self.pad_c)


if __name__ == '__main__':
    unittest.main()
