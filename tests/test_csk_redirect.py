"""The CSK Collection's costume redirect (runtime/hle_skyline.c redirect_in): a player's fighter
record is redirected only by the character it names itself, never by another character's
ui_chara hash found elsewhere in the record. No game files needed: the function is compiled
from the runtime's source with a synthetic record."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

NINTEN = 0xFEE536919       # hash40("ui_chara_ninten"): a CSK character on Ness's c120
SANDSHREW = 0x126C02D716   # hash40("ui_chara_sandshrew"): a CSK character on Pikachu's c64
POPPLE = 0xFA91015E3       # hash40("ui_chara_popple"): a .prcxml echo on Mario's c24 (no CSK redirect)


def function_source(name):
    source = (ROOT / "runtime/hle_skyline.c").read_text(encoding="utf-8")
    begin = source.index(f"static Redirect *{name}(")
    end = source.index("\n}\n", begin) + 3
    return source[begin:end]


HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t u64; typedef uint32_t u32; typedef uint8_t u8;
#define P(T, addr) ((T *)(uintptr_t)(addr))
typedef struct { u64 ui_chara, base; int start; } Redirect;
static Redirect redirects[64];
static u32 redirect_n;
%(function)s
/* argv: hash at +0, then pairs "offset hash" for other words of the 0x400-byte record */
int main(int argc, char **argv) {
    static u64 rec[0x400 / 8];
    redirects[redirect_n++] = (Redirect){0x%(ninten)xull, 0, 120};
    redirects[redirect_n++] = (Redirect){0x%(sandshrew)xull, 0, 64};
    rec[0] = strtoull(argv[1], 0, 16);
    for (int i = 2; i + 1 < argc; i += 2) rec[strtoull(argv[i], 0, 16) / 8] = strtoull(argv[i + 1], 0, 16);
    Redirect *r = redirect_in((u64)(uintptr_t)rec);
    if (r) printf("%%llx %%d\n", (unsigned long long)r->ui_chara, r->start);
    else printf("none\n");
    return 0;
}
'''


class RedirectMatchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang") or shutil.which("cc")
        if not compiler:
            raise unittest.SkipTest("C compiler not installed")
        cls.tmp = tempfile.TemporaryDirectory(prefix="csk redirect test ")
        src = Path(cls.tmp.name) / "redirect.c"
        src.write_text(HARNESS % {"function": function_source("redirect_in"), "ninten": NINTEN,
                                  "sandshrew": SANDSHREW}, encoding="utf-8")
        cls.exe = Path(cls.tmp.name) / ("redirect.exe" if os.name == "nt" else "redirect")
        subprocess.run([compiler, "-std=c11", "-Wall", "-Werror", "-include", "stdlib.h", "-o",
                        str(cls.exe), str(src)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def redirect(self, own, *others):
        args = [str(self.exe), f"{own:x}"] + [f"{v:x}" for pair in others for v in pair]
        return subprocess.run(args, check=True, capture_output=True, text=True).stdout.split()

    def test_character_named_by_the_record_is_redirected(self):
        self.assertEqual(self.redirect(NINTEN), [f"{NINTEN:x}", "120"])

    def test_tag_bits_above_the_hash_are_ignored(self):
        self.assertEqual(self.redirect((0x07 << 40) | SANDSHREW), [f"{SANDSHREW:x}", "64"])

    def test_another_players_character_later_in_the_record_is_not_taken(self):
        # Popple (a .prcxml Mario echo) vs Ninten: Ninten's hash sat at +0x260 of Popple's record,
        # and the old scan gave Mario Ninten's c120 - a costume Mario lacks; the match crashed loading
        # its effects (main+0x355fea0 under smashline's effect hook).
        self.assertEqual(self.redirect(POPPLE, (0x260, NINTEN)), ["none"])

    def test_own_character_wins_over_another_later_in_the_record(self):
        self.assertEqual(self.redirect(SANDSHREW, (0x260, NINTEN)), [f"{SANDSHREW:x}", "64"])


if __name__ == "__main__":
    unittest.main()
