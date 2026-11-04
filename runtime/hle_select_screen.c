/* The character select screen with more than 91 tiles (tools/css_layout.py makes its layout and table).
 *
 * The game's grid function (main 13.0.5 0x71019eb840: (screen, -, layout holder, flags, vector of
 * roster ids); plan/phases/N2_real_slots.md 3.2) lays out N tiles. Its layout is [[x2]] + 8 (0x71019ed7e4
 * and 0x71019eda78). It reads row N of its table (the abort at 0x71019f1df0 when there is none), then
 *   - walks tiles 0..90 (cmp #0x5b at 0x71019edd30): each tile's two panes, "set_parts_chr_s_%02d" and
 *     "set_parts_chr_bg_s_%02d", get their size animation (window_size_<row's size>) and the reset ones
 *     (clear_off, select_hl, name_line1, black_off, bg_set_*, compe_bg_*);
 *   - places tile k of 0..N-1 (pane by the same names: any k), the picture panes sized to the tile;
 *   - hides tiles N..90 (cmp #0x5a at 0x71019f043c / 0x71019f04cc).
 * A layout with more tiles (P > 91: tools/css_layout.py) has tiles 91..P-1 the game places but neither
 * sizes nor hides. Running the function twice is no way round it: a second run leaves every tile
 * invisible (tried). So, after the game's one run:
 *   - tiles 91.. of the rows, which show the tile layout as it is made (the size of window_size_08, the reset
 *     states), get the size of the row's tiles as a scale of the tile's part pane: 0.97 ** (size - 8) for
 *     tools/css_layout.py's sizes 09.. (its FACTOR), 1 for the game's own;
 *   - the tiles past the rows' (their counts at screen + 0x1d4; Random's entry, the vector's last, is not
 *     one of them) from 91 on are hidden (the pane's +0x58 bit 0, as the game hides).
 * More fighters than the layout holds (no row in the table: the game would abort) are not given to it:
 * the last ones are left out, with a warning in the log. This replaces the roster component's own guard
 * (ssbu-recomp/mods/mods_n6_src/n6_roster.cpp, N10_GRID_MAX: 91), which is told to stand aside.
 * With the game's own layout and up to 91 tiles nothing changes: the function runs as it is.
 * SSBU_CSS_FIT=0 leaves the hook out; SSBU_CSS_LOG=1 logs each layout. */
#define _CRT_SECURE_NO_WARNINGS
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "host.h"

#define GRID        0x71019eb840ull   /* the select screen's grid */
#define PANE_LOOKUP 0x7103776910ull   /* (layout, name format, index) -> [x8] {.., holder}: [holder] = the pane */
#define FREE        0x710392eb40ull   /* operator delete */
#define FMT_CHR     0x71044122caull   /* "set_parts_chr_s_%02d" */
#define FMT_CHR_BG  0x710436bc2eull   /* "set_parts_chr_bg_s_%02d" */
#define GAME_TILES  91
#define MAX_TILES   4000
#define OUR_SIZES   9                 /* tools/css_layout.py: FIRST_SIZE, FACTOR */
#define FACTOR      0.97

static u64 orig_grid;

static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}

static int ss_log(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("SSBU_CSS_LOG"); on = e && *e && *e != '0'; }
    return on;
}

/* The pane named fmt % k in the layout, or 0: the game's own look-up, on this thread's context and stack. */
static u64 find_pane(Cpu *c, u64 layout, u64 fmt, u32 k) {
    Cpu saved = *c;
    u64 out = (c->sp - 0x200) & ~0xFull;
    st64(out, 0); st64(out + 8, 0);
    c->sp = out - 0x40;
    c->x[0] = layout; c->x[1] = fmt; c->x[2] = k; c->x[8] = out;
    rt_call(c, PANE_LOOKUP);
    u64 holder = ld64(out + 8), pane = holder ? ld64(holder) : 0;
    if (holder) { c->x[0] = holder; rt_call(c, FREE); }
    *c = saved;
    return pane;
}

/* How many tiles the layout has: 91, or more (tools/css_layout.py); found once per layout. */
static u32 layout_tiles(Cpu *c, u64 layout) {
    static u64 seen;
    static u32 tiles;
    if (layout == seen) return tiles;
    u32 k = GAME_TILES;
    if (find_pane(c, layout, FMT_CHR, GAME_TILES - 1))   /* (else not the select screen's layout) */
        while (k < MAX_TILES && find_pane(c, layout, FMT_CHR, k) && find_pane(c, layout, FMT_CHR_BG, k)) k++;
    seen = layout; tiles = k;
    if (ss_log() || k > GAME_TILES) say("[css] the select screen's layout has %u tiles\n", k);
    return k;
}

static void grid(Cpu *c) {
    u64 screen = c->x[0], holder = c->x[2], vec = c->x[4];
    u64 begin = vec ? ld64(vec) : 0, end = vec ? ld64(vec + 8) : 0;
    u64 n = begin && end > begin ? (end - begin) / 8 : 0;
    u64 layout = holder && ld64(holder) && ld64(ld64(holder)) ? ld64(ld64(holder)) + 8 : 0;
    u32 tiles = layout ? layout_tiles(c, layout) : GAME_TILES;
    if (n > tiles) {   /* more fighters than the screen holds: the last ones are left out */
        static u64 warned;
        if (warned != n) {
            warned = n;
            say("[css] WARNING: %llu fighters for the select screen and its layout holds %u: the last %llu are not shown "
                "(tools/mods.py makes a bigger screen when it applies the mods that add them)\n",
                (unsigned long long)n, tiles, (unsigned long long)(n - tiles));
        }
        st64(vec + 8, begin + 8ull * tiles);
    }
    Cpu entry = *c;
    rt_call(c, orig_grid);
    if (n > tiles) { st64(vec + 8, end); n = tiles; }
    if (tiles <= GAME_TILES || !layout) return;
    Cpu after = *c;
    *c = entry;
    u32 placed = 0;   /* (the rows' tiles: Random's entry is not one of them) */
    for (int r = 0; r < 13; r++) placed += ld32(screen + 0x1d4 + 4 * r);
    if (placed < n) n = placed;
    u32 size = ld32(screen + 0x1d0);
    float scale = size >= OUR_SIZES ? (float)pow(FACTOR, (double)size - (OUR_SIZES - 1)) : 1.0f;
    for (u32 k = GAME_TILES; k < tiles; k++) {
        u64 part = find_pane(c, layout, FMT_CHR, k), pic = find_pane(c, layout, FMT_CHR_BG, k);
        if (k < n) {
            if (part) {
                st32(part + 0x48, *(u32 *)&scale);
                st32(part + 0x4c, *(u32 *)&scale);
                st8(part + 0x58, ld8(part + 0x58) | 0x10);
            }
        } else {
            if (part) st8(part + 0x58, ld8(part + 0x58) & 0xFE);
            if (pic) st8(pic + 0x58, ld8(pic + 0x58) & 0xFE);
        }
    }
    *c = after;
    if (ss_log()) say("[css] grid: %llu tiles of %u, rows %u %u %u .., size %u (tiles past 91 scaled %.3f)\n", (unsigned long long)n,
                      tiles, ld32(screen + 0x1d4), ld32(screen + 0x1d8), ld32(screen + 0x1dc), size, scale);
}

/* (hle_fs.c, when the game mounts its data: before the mods' DLLs load) */
void select_screen_install(void) {
    static int done;
    if (done) return;
    done = 1;
    const char *off = getenv("SSBU_CSS_FIT");
    if (off && *off == '0') return;
    orig_grid = rt_hook_function(GRID, rt_native(grid, "select screen: grid"));
    if (!orig_grid) {
        say("[css] the select screen's hook missed: 91 tiles at most\n");
        return;
    }
#ifdef _WIN32
    /* the roster component's guard (N10_GRID_MAX, 91 by default) stands aside: this file guards */
    if (!getenv("N10_GRID_MAX")) { _putenv("N10_GRID_MAX=0"); SetEnvironmentVariableA("N10_GRID_MAX", "0"); }
#endif
}
