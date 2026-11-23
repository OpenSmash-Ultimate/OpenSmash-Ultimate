/* display.ini's frame_rate (120 or 240): the game's logic kept at 60 frames a second while its main loop runs 2 or 4
 * times as often (ipc_vi.c's vsync thread gives it 120 / 240 refreshes a second: display_refresh_mult).
 *
 * The game's frame scheduler (in the main loop, main + 0x374a0e8 in 13.0.5) counts in sixtieths of a refresh. Each
 * pass of the loop takes its step, a u32 at main + 0x523d004, off a counter, and runs one logic frame each time the
 * counter falls to 0 or below, putting 60 back. The step is 60 at normal speed: a logic frame every pass. The game's
 * speed settings set it from a table of eight (main + 0x4742e00, read-only: 60 30 40 90 120 150 180 210, its index at
 * main + 0x593b444 - training's 1/2 and 2/3 speed, Special Smash's fast speeds), always the table's entry for the index
 * written beside it. Smash Ultimate's 120/240 FPS mods for the console (Fl4sh's, for 13.0.5) do the same with a
 * cheat: the step written as 30 at 120 refreshes a second and 15 at 240, and the swap interval patched so that the
 * console's compositor - an emulator's - gives the game that many refreshes. Here the vsync thread gives them, and
 * this file writes the step after the graphics module's frame end (FRAME_END: early in each pass, before the
 * scheduler reads it): the speed's own entry divided by the refreshes per 60 Hz frame, so 15 at 240 at normal
 * speed - the mod's value - and the speed settings still work (training's 1/2 speed: 7 and 8 in turn at 240, 7.5 on
 * average). Back at 60 a second (an online match: display_refresh_mult) the game's own value is put back once.
 *
 * Its pads are read at every pass (hle_hid.c reads the controllers when the game asks, a new sample each time), so a
 * press is seen at most a 240th of a second after it happens, as on the console with the mod: what Steve's NIL
 * techniques - two frames to place the block - need, which 120 breaks and 240 does not (the owner's emulator tests).
 *
 * Checked once before the first write: the game's build is 13.0.5's (the addresses are its) and the table reads as
 * above; else this file does nothing and says so. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <string.h>

#include "host.h"
#include "frame_hooks.h"

#define FR_STEP  0x710523d004ull   /* u32: the frame scheduler's step, sixtieths of a refresh (60: a logic frame a refresh) */
#define FR_SPEED 0x710593b444ull   /* u32: the game speed's index into FR_TABLE (0: normal) */
#define FR_TABLE 0x7104742e00ull   /* u32[8], read-only data: each game speed's step at 60 refreshes a second */
#define FR_BUILD "21450C647B8C5940ED5B68218EA128FD3C5D597D"   /* (13.0.5's main) */

extern volatile LONG rt_refresh_mult;   /* (kernel.c; ipc_vi.c's vsync thread sets it) */

static int ok = -1;             /* the build and the table checked: 1 good, 0 not this game's */
static int written;             /* the step in the game's memory is ours (a write since the last put back) */
static u32 rest, mult_was = 1;  /* (the division's remainder, carried: the steps average the entry / refreshes exactly) */

static int checked(void) {
    if (ok >= 0) return ok;
    ok = 0;
    if (!rt_module_main.build_id || strcmp(rt_module_main.build_id, FR_BUILD)) {
        fprintf(stderr, "[frame rate] the game's main module is not 13.0.5's (%s): frame_rate stays at the game's own 60\n",
                rt_module_main.build_id ? rt_module_main.build_id : "no build id");
        return 0;
    }
    static const u32 want[8] = {60, 30, 40, 90, 120, 150, 180, 210};
    for (int i = 0; i < 8; i++)
        if (ld32(FR_TABLE + 4 * (u64)i) != want[i]) {
            fprintf(stderr, "[frame rate] the game's speed table is not as expected (entry %d reads %u): frame_rate stays at 60\n", i,
                    ld32(FR_TABLE + 4 * (u64)i));
            return 0;
        }
    return ok = 1;
}

/* The game's own step for its speed now: the table's entry for the index beside it. */
static u32 game_step(void) {
    u32 speed = ld32(FR_SPEED);
    return speed < 8 ? ld32(FR_TABLE + 4 * (u64)speed) : 60;
}

static void frame_end_after(Cpu *c, const u64 args[8], void *ctx) {
    (void)c; (void)args; (void)ctx;
    u32 m = (u32)rt_refresh_mult;
    if (m <= 1 && !written) return;   /* (60 a second and nothing of ours in the game: the game's own, untouched) */
    if (!checked()) return;
    u32 own = game_step();
    if (m <= 1) {   /* back at 60: the game's own step, once */
        st32(FR_STEP, own);
        written = 0; rest = 0; mult_was = 1;
        fprintf(stderr, "[frame rate] the game's frame scheduler at its own step again (%u)\n", own);
        return;
    }
    if (m != mult_was) {
        fprintf(stderr, "[frame rate] %u refreshes a second: the game's frame scheduler steps %u / %u a refresh (its logic at 60 frames a second)\n",
                60 * m, own, m);
        mult_was = m; rest = 0;
    }
    u32 total = rest + own, step = total / m;
    rest = total % m;
    st32(FR_STEP, step ? step : 1);   /* (never 0: the scheduler would not run a logic frame again) */
    written = 1;
}

__attribute__((constructor)) static void frame_rate_init(void) {
    /* (innermost on the frame end: its after runs first, before rollback's, which this does not touch) */
    frame_hook_add(FRAME_HOOK_END, 400, NULL, frame_end_after, NULL);
}
