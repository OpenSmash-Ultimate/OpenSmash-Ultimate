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
 * Its pads: hle_hid.c reads the controllers when the game asks, a new sample each time, but the game asks for player 1's
 * pad only in the passes that run a logic frame (measured 2026-11-24, plan/INPUT_LAG_2026-11-24.md 4: 0.50 / 0.25 reads a
 * pass at 120 / 240, all on the same phase), so input is still taken 60 times a second, as the logic that uses it. A
 * read in the other passes could not be used before the next logic frame anyway: frame_rate gives smoother pictures,
 * not less input delay (that is low_latency's and run-ahead's work). (The console mod is said to help Steve's NIL
 * techniques at 240 - the owner's emulator tests - which a 60 Hz read would not explain: open, see plan/TODO.md.)
 *
 * Refreshes the game missed: the frame end reports them (FRAME_END's third argument: refreshes counted since the last
 * frame, less one), and in the scenes that allow it the game catches up with one extra logic frame for each. At 60 a
 * second that is right - a refresh is a logic frame - but at 240 one missed refresh is a quarter of one, and the game
 * would run a whole logic frame for it: a pass of the loop slower than 4.2 ms made the game jump ahead. So the report
 * is counted in logic frames here: missed refreshes are summed, and the game told of one each time they make a whole
 * 60 Hz frame (4 at 240, 2 at 120), the rest carried. A PC that keeps up misses none, and the game sees none.
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
/* The pictures a logic frame lasts at the game's speed now: refreshes a 60 Hz frame x 60 / the speed's step (4 at 240 at
 * normal speed, 8 at training's 1/2 speed). For gpu_motion.c's blending. */
volatile float rt_passes_per_logic = 1;

static int ok = -1;             /* the build and the table checked: 1 good, 0 not this game's */
static int written;             /* the step in the game's memory is ours (a write since the last put back) */
static u32 rest, mult_was = 1;  /* (the division's remainder, carried: the steps average the entry / refreshes exactly) */
static u64 missed;              /* (refreshes missed, short of a whole logic frame's) */

extern volatile int display_frame_rate_refused;   /* (gpu_display.c: display_refresh_mult stays 1 then) */
static int checked(void) {
    if (ok >= 0) return ok;
    ok = 0;
    display_frame_rate_refused = 1;   /* (until both checks pass) */
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
    display_frame_rate_refused = 0;
    return ok = 1;
}

/* The game's own step for its speed now: the table's entry for the index beside it. */
static u32 game_step(void) {
    u32 speed = ld32(FR_SPEED);
    return speed < 8 ? ld32(FR_TABLE + 4 * (u64)speed) : 60;
}

static void frame_end_after(Cpu *c, const u64 args[8], void *ctx) {
    (void)c; (void)ctx;
    u32 m = (u32)rt_refresh_mult;
    if (m > 1 && !checked()) return;   /* (not this game's: nothing rewritten; display_refresh_mult goes back to 1) */
    if (m > 1 && args[2]) {   /* the missed refreshes in logic frames (as hle_rb_core.c reads the report: count at x2, result at x0) */
        u64 p1 = args[0], p3 = args[2], n = ld64(p3);
        if (n) {
            missed += n;
            u64 frames = missed / m;
            missed %= m;
            st64(p3, frames);
            if (!frames && p1 && (ld32(p1) == 0xF0044007u || ld32(p1) == 0xF0044005u)) st32(p1, 0);
        }
    } else missed = 0;
    if (m <= 1 && !written) return;   /* (60 a second and nothing of ours in the game: the game's own, untouched) */
    if (!checked()) return;
    u32 own = game_step();
    if (m <= 1) {   /* back at 60: the game's own step, once */
        st32(FR_STEP, own);
        written = 0; rest = 0; mult_was = 1; rt_passes_per_logic = 1;
        fprintf(stderr, "[frame rate] the game's frame scheduler at its own step again (%u)\n", own);
        return;
    }
    if (m != mult_was) {
        fprintf(stderr, "[frame rate] %u refreshes a second: the game's frame scheduler steps %u / %u a refresh (its logic at 60 frames a second)\n",
                60 * m, own, m);
        mult_was = m; rest = 0;
    }
    rt_passes_per_logic = own ? (float)m * 60.0f / (float)own : (float)m;
    u32 total = rest + own, step = total / m;
    rest = total % m;
    st32(FR_STEP, step ? step : 1);   /* (never 0: the scheduler would not run a logic frame again) */
    written = 1;
}

__attribute__((constructor)) static void frame_rate_init(void) {
    /* (innermost on the frame end: its after runs first, before rollback's, which this does not touch) */
    frame_hook_add(FRAME_HOOK_END, 400, NULL, frame_end_after, NULL);
}
