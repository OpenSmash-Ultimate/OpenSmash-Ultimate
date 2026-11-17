"""The select screen with more than 91 tiles (runtime/hle_select_screen.c grid): after the game's own
run, the tiles past 91 that the rows use are left as the game drew them (no second scale), and the
ones past the rows are hidden. No game files needed: grid() is compiled from the runtime's source
with made-up panes; the game's own grid function is a stub that does nothing."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "runtime/hle_select_screen.c"


def grid_source():
    source = SOURCE.read_text(encoding="utf-8")
    begin = source.index("static void grid(Cpu *c) {")
    end = source.index("\n}\n", begin) + 3
    defines = [line for line in source.splitlines() if line.startswith("#define ") and "_CRT_" not in line]
    return "\n".join(defines), source[begin:end]


HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint64_t u64; typedef uint32_t u32; typedef uint8_t u8;
typedef struct { u64 x[31], sp; } Cpu;
%(defines)s
u64 ld64(u64 a) { u64 v; memcpy(&v, (void *)(uintptr_t)a, 8); return v; }
u32 ld32(u64 a) { u32 v; memcpy(&v, (void *)(uintptr_t)a, 4); return v; }
u8 ld8(u64 a) { return *(u8 *)(uintptr_t)a; }
void st64(u64 a, u64 v) { memcpy((void *)(uintptr_t)a, &v, 8); }
void st32(u64 a, u32 v) { memcpy((void *)(uintptr_t)a, &v, 4); }
void st8(u64 a, u8 v) { *(u8 *)(uintptr_t)a = v; }
enum { PANE = 0x60, ROOM = 400 };
static u8 parts[ROOM][PANE], pics[ROOM][PANE];
static u32 layout_n;
static u64 orig_grid;
void rt_call(Cpu *c, u64 fn) { (void)c; (void)fn; }   /* the game's grid: places and sizes, not modelled */
u64 find_pane(Cpu *c, u64 layout, u64 fmt, u32 k) {
    (void)c; (void)layout;
    return k < layout_n ? (u64)(uintptr_t)(fmt == FMT_CHR ? parts[k] : pics[k]) : 0;
}
u32 layout_tiles(Cpu *c, u64 layout) { (void)c; (void)layout; return layout_n; }
void say(const char *fmt, ...) { (void)fmt; }
int ss_log(void) { return 0; }
%(function)s
/* argv: tiles in the layout, entries in the vector (fighters + Random), tile size, then the rows' counts */
int main(int argc, char **argv) {
    static u64 screen[0x400 / 8], entries[ROOM + 1], vec[2], object[2];
    u64 cell = (u64)(uintptr_t)object, holder = (u64)(uintptr_t)&cell;
    u32 n = (u32)atoi(argv[2]), size = (u32)atoi(argv[3]);
    float one = 1.0f;
    layout_n = (u32)atoi(argv[1]);
    for (u32 k = 0; k < layout_n; k++) {   /* every tile as the game leaves a row's tile: shown, its part pane unscaled */
        memcpy(parts[k] + 0x48, &one, 4); memcpy(parts[k] + 0x4c, &one, 4);
        parts[k][0x58] = pics[k][0x58] = 0x01;
    }
    vec[0] = (u64)(uintptr_t)entries; vec[1] = vec[0] + 8ull * n;
    st32((u64)(uintptr_t)screen + 0x1d0, size);
    for (int r = 0; r + 4 < argc && r < 13; r++) st32((u64)(uintptr_t)screen + 0x1d4 + 4 * r, (u32)atoi(argv[4 + r]));
    Cpu c;
    memset(&c, 0, sizeof c);
    c.x[0] = (u64)(uintptr_t)screen; c.x[2] = (u64)(uintptr_t)&holder; c.x[4] = (u64)(uintptr_t)vec;
    grid(&c);
    for (u32 k = 0; k < layout_n; k++) {
        float sx, sy;
        memcpy(&sx, parts[k] + 0x48, 4); memcpy(&sy, parts[k] + 0x4c, 4);
        printf("%%u %%.3f %%.3f %%02x %%02x\n", k, sx, sy, parts[k][0x58], pics[k][0x58]);
    }
    return 0;
}
'''

SHOWN = ("1.000", "1.000", "01", "01")     # as the game left it: unscaled part pane, both panes visible
HIDDEN = ("1.000", "1.000", "00", "00")    # the visible bit (pane + 0x58 bit 0) cleared on both panes


class GridTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang") or shutil.which("cc")
        if not compiler:
            raise unittest.SkipTest("C compiler not installed")
        cls.tmp = tempfile.TemporaryDirectory(prefix="select screen grid test ")
        defines, function = grid_source()
        src = Path(cls.tmp.name) / "grid.c"
        src.write_text(HARNESS % {"defines": defines, "function": function}, encoding="utf-8")
        cls.exe = Path(cls.tmp.name) / ("grid.exe" if os.name == "nt" else "grid")
        subprocess.run([compiler, "-std=gnu11", "-Wall", "-Werror", "-o", str(cls.exe), str(src)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def grid(self, tiles, entries, size, rows):
        args = [str(self.exe), str(tiles), str(entries), str(size)] + [str(r) for r in rows]
        out = subprocess.run(args, check=True, capture_output=True, text=True).stdout.splitlines()
        return {int(line.split()[0]): tuple(line.split()[1:]) for line in out}

    def test_a_rows_tile_past_91_keeps_the_size_the_game_gave_it(self):
        # 108 fighters + Random on a 112-tile layout (14 x 8, size 13). The old code wrote
        # 0.97 ** 5 = 0.859 on the part panes of tiles 91..107, which already had the row's size:
        # on a 2560-wide picture they came out about 143 px wide next to 165 px tiles.
        tiles = self.grid(112, 109, 13, [14] * 7 + [10])
        self.assertEqual({k: tiles[k] for k in range(91, 108)}, {k: SHOWN for k in range(91, 108)})

    def test_tiles_past_the_rows_are_hidden(self):
        tiles = self.grid(112, 109, 13, [14] * 7 + [10])
        self.assertEqual({k: tiles[k] for k in range(108, 112)}, {k: HIDDEN for k in range(108, 112)})

    def test_fewer_fighters_than_91_hide_every_added_tile_and_no_game_tile(self):
        # 87 fighters + Random on the same layout: tiles 87..90 are the game's to hide (its own loop).
        tiles = self.grid(112, 88, 7, [13] * 6 + [9])
        self.assertEqual({k: tiles[k] for k in range(91, 112)}, {k: HIDDEN for k in range(91, 112)})
        self.assertEqual(tiles[90], SHOWN)


if __name__ == "__main__":
    unittest.main()
