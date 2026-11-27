"""The ping overlay (runtime/net_ping.h; drawn by runtime/gpu_overlay.c, measured by runtime/hle_rb_core.c): the ping
shown is the median of the last 5 windows of round trips (each window's least), "--" once nothing has come for 3 s;
the colours, bars and text; where a 2-player fight's item goes; the switch (SSBU_NET_PING, display.ini net_ping) and
the launchers' row for it. The header is built alone with clang (clang-cl on Windows); the rest is read as text."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import cc  # noqa: E402

if os.name == 'nt':
    HAVE_CLANG = os.path.exists(cc.CLANG) or shutil.which(cc.CLANG) is not None
else:
    HAVE_CLANG = shutil.which('clang') is not None

SOURCE = r'''#include <stdio.h>
#include <string.h>
#include "net_ping.h"
#define CHECK(x) do { if (!(x)) { printf("failed: %s (line %d)\n", #x, __LINE__); return 1; } } while (0)
int main(void) {
    NetPingHist h; char t[16];
    net_ping_reset(&h);
    CHECK(net_ping_shown(&h, 1000) == -1);
    /* the first round trip is shown at once */
    net_ping_sample(&h, 1000, 48.4);
    CHECK(net_ping_shown(&h, 1000) == 48);
    /* then each 250 ms window's least: 60 samples a second, late by up to a frame */
    double now = 1000;
    for (int i = 0; i < 60; i++) { now += 1000.0 / 60; net_ping_sample(&h, now, 50 + (i * 7) % 17); }
    CHECK(h.n == 4 || h.n == 5);
    CHECK(net_ping_shown(&h, now) >= 50 && net_ping_shown(&h, now) <= 52);
    /* one late packet is no spike: a window of 400 ms among 50s */
    net_ping_reset(&h);
    net_ping_sample(&h, 10, 50);
    double ws[] = {50, 400, 51, 50, 52, 50};
    now = 10;
    for (int k = 0; k < 6; k++) { now += 300; net_ping_sample(&h, now, ws[k]); }
    CHECK(net_ping_shown(&h, now) == 50 || net_ping_shown(&h, now) == 51);
    /* the median of the last 5 windows, not of all */
    net_ping_reset(&h);
    net_ping_sample(&h, 1, 200);
    now = 1;
    for (int k = 0; k < 10; k++) { now += 260; net_ping_sample(&h, now, 30); }
    CHECK(net_ping_shown(&h, now) == 30);
    /* nothing for 3 s: not known */
    CHECK(net_ping_shown(&h, now + 2999) == 30);
    CHECK(net_ping_shown(&h, now + 3001) == -1);
    /* nonsense is not a round trip */
    net_ping_reset(&h);
    net_ping_sample(&h, 5, 0); net_ping_sample(&h, 5, -3); net_ping_sample(&h, 5, 6000);
    CHECK(h.n == 0 && net_ping_shown(&h, 5) == -1);
    /* SSBU Arena's colours and bars */
    CHECK(net_ping_level(0) == 0 && net_ping_level(60) == 0 && net_ping_level(61) == 1 && net_ping_level(120) == 1);
    CHECK(net_ping_level(121) == 2 && net_ping_level(-1) == 2);
    CHECK(net_ping_bars(40) == 4 && net_ping_bars(41) == 3 && net_ping_bars(80) == 3 && net_ping_bars(130) == 2);
    CHECK(net_ping_bars(131) == 1 && net_ping_bars(-1) == 0);
    net_ping_text(t, sizeof t, 48); CHECK(!strcmp(t, "48"));
    net_ping_text(t, sizeof t, 5000); CHECK(!strcmp(t, "999"));
    net_ping_text(t, sizeof t, -1); CHECK(!strcmp(t, "--"));
    net_ping_delay_text(t, sizeof t, 2, NET_DELAY_AUTO); CHECK(!strcmp(t, "2f auto"));
    net_ping_delay_text(t, sizeof t, 3, NET_DELAY_FIXED); CHECK(!strcmp(t, "3f fixed"));
    net_ping_delay_text(t, sizeof t, 4, NET_DELAY_UNKNOWN); CHECK(!strcmp(t, "4f"));
    net_ping_delay_text(t, sizeof t, -1, NET_DELAY_AUTO); CHECK(!t[0]);
    /* where it goes: 1920 x 1080, and a 21:9 picture whose HUD stays in the middle 16:9 */
    float r, b;
    net_ping_anchor(1920, 1080, 0, &r, &b); CHECK(r > 466 && r < 467 && b > 1056 && b < 1057);
    net_ping_anchor(1920, 1080, 1, &r, &b); CHECK(r > 1205 && r < 1206);
    net_ping_anchor(2560, 1080, 1, &r, &b); CHECK(r > 320 + 1205 && r < 320 + 1206);
    CHECK(net_ping_dot(1080) == 2 && net_ping_size(1080) == 26);
    CHECK(net_ping_width(1080, 2, 0) == 4 * 2 + 3 * 1 + 4 + 2 * 13);
    CHECK(net_ping_width(1080, 2, 7) == 4 * 2 + 3 * 1 + 4 + 2 * 13 + 6 + 7 * 13);
    puts("ok");
    return 0;
}
'''


def read(rel):
    return (ROOT / rel).read_text(encoding='utf-8-sig')


@unittest.skipUnless(HAVE_CLANG, 'needs clang-cl on Windows (deps\\llvm, or Visual Studio\'s C++ Clang tools), or clang')
class NetPingHeader(unittest.TestCase):
    def test_rules(self):
        with tempfile.TemporaryDirectory(prefix='netping ') as tmp:
            src = os.path.join(tmp, 'netping.c')
            Path(src).write_text(SOURCE)
            if os.name == 'nt':
                exe = os.path.join(tmp, 'netping.exe')
                cmd = [cc.CLANG, '/nologo', *cc.LINK_FLAGS, '/I', str(ROOT / 'runtime'), src, '/Fe' + exe, '/Fo' + tmp + os.sep,
                       '/link', *cc.LINK_OPTIONS]
            else:
                exe = os.path.join(tmp, 'netping')
                cmd = ['clang', '-O1', '-w', '-I', str(ROOT / 'runtime'), src, '-o', exe]
            r = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([exe], capture_output=True, text=True, timeout=30)
            self.assertEqual((r.returncode, r.stdout.strip()), (0, 'ok'), r.stdout)


class NetPingWiring(unittest.TestCase):
    def test_switch(self):
        """SSBU_NET_PING wins over display.ini's net_ping; on unless either says 0; the default display.ini has it."""
        display = read('runtime/gpu_display.c')
        self.assertIn('getenv("SSBU_NET_PING")', display)
        self.assertRegex(display, r'"net_ping = 1\\n"')
        self.assertIn('!_stricmp(key, "net_ping")', display)
        self.assertIn('int display_net_ping(void);', read('runtime/display.h'))

    def test_overlay_off_asks_nothing(self):
        """Off: neither the fight's ping nor the room's badge is asked for or drawn."""
        ovl = read('runtime/gpu_overlay.c')
        for fn in ('static int fight_ping_wanted(void) {', 'static int badge_wanted(void) {'):
            body = ovl[ovl.index(fn):]
            body = body[:body.index('\n}\n')]
            first = [l.strip() for l in body.split('\n')[1:] if l.strip() and not l.strip().startswith('static u64')][0]
            self.assertTrue(first.startswith('if (!display_net_ping()) return 0;'), (fn, first))
        # both renderers' entry points draw it (Direct3D 11's overlay_present, Vulkan's overlay_quads)
        self.assertEqual(ovl.count('if (fping) fight_ping((float)w, (float)h);'), 2)
        self.assertEqual(ovl.count('fping = !menu && fight_ping_wanted()'), 2)

    def test_measured_and_shown_only(self):
        """The rollback side feeds the overlay and its view, and the HELLO says how the delay was chosen."""
        core = read('runtime/hle_rb_core.c')
        self.assertEqual(core.count('net_ping_sample(&ping_hist[p], now_ms(), rtt);'), 2)
        self.assertIn('int rb_net_ping_view(NetPingView *v) { memset(v, 0, sizeof *v); return 0; }', core)   # (no rollback: none)
        self.assertIn('h.delay_how = net_delay_how;', core)
        self.assertIn('net_delay_how = h.delay_how;', core)
        net = read('runtime/hle_rb_net.c')
        self.assertIn('b[7] = (uint8_t)(h->delay_how == 1 || h->delay_how == 2 ? h->delay_how : 0);', net)

    def test_launchers(self):
        """The Online page of both launchers has the row; it writes display.ini's net_ping, default 1 as the game's."""
        self.assertIn('("display.ini", "", "net_ping", "1")', read('OpenSmash.Launcher/SettingDefaults.cs'))
        for rel in ('OpenSmash.Launcher/LauncherForm.cs', 'OpenSmash.Avalonia/Visuals/SettingsDefinitions.cs'):
            text = read(rel)
            online = re.search(r'Name = "Online", SettingKeys = new\[\] \{([^}]*)\}', text).group(1)
            self.assertIn('"net_ping"', online, rel)
            self.assertIn('new Setting { Key = "net_ping"', text, rel)
            lists = re.findall(r'foreach \(string key in new\[\] \{([^}]*)\}\)', text)
            display_lists = [l for l in lists if '"overlay"' in l]
            self.assertEqual(len(display_lists), 2, rel)   # (read and written with display.ini's other keys)
            for l in display_lists:
                self.assertIn('"net_ping"', l, rel)


if __name__ == '__main__':
    unittest.main()
