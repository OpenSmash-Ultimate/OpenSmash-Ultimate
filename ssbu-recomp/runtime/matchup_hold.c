/* matchup_hold.c - a minimum time for the matchup screen: graphics.ini matchup_screen_min = <seconds> (default 0 = the
 * game as it is). The matchup screen is the loading screen that names the fight, such as "ROUND 1 / Vs." before each round
 * of Classic Mode.
 *
 * Why: the game shows that screen while it loads the fight and takes it away as soon as the fight is loaded. On the
 * console the loading takes seconds; here it takes a fraction of one, so the screen was up for about 26 frames (0.4 s;
 * measured 2026-10-26, Classic Mode, 13.0.5, Linux, frame-by-frame screenshots): too short to read.
 *
 * How the game does it (main 13.0.4; found in the binary, then confirmed with hooks and logs in scripted Classic Mode
 * runs on 13.0.5, where these code addresses are the same and the data addresses one page later):
 *   - the LoadingManager is *(*0x710532e7d8 + 8): +0xe8 the loading causes (bit 0 = LOAD), +0xf1 "loading", +0xf4 the
 *     option MATCHUP (the Lua constants LOADING_OPTION_NONE 0 / TIPS 1 / MATCHUP 2; the start sets +0xf4 for option 2);
 *   - the loading screen is a scene of the game's scene stack; its leave (vtable + 0x18 = 0x7101343040) clears LOAD;
 *   - the stack is unwound when a transient scene of class 0x7105061518 finds its word +0x30 at 0 in its update
 *     (vtable + 0x30 = 0x710135dc30; x0 = the scene, x1 = &stack) and queues its own pop (command type 4); the scenes
 *     below it pop themselves in turn, the loading screen's last, 6 frames later. In the runs that scene's first update
 *     came with the matchup screen and its pop about 1 s later.
 * The hold: while the matchup screen is up and less than the minimum has passed since that scene's first update, its
 * update is not run (it does nothing else), so nothing is queued; on the first frame after, it runs as before. Measured
 * with 3.5 s (Classic Mode round 1, 13.0.5, Linux): the fight was ready after 734 ms, the "Vs." picture then stayed up
 * for about 138 frames (2.3 s) instead of 26, its animation running, then Ready / GO and the match clock as usual.
 * Holding the pop command in the queue instead does NOT work: the scenes queue it again every frame (33 pops queued,
 * then a crash).
 * Never in a Local Wireless room (online play): the games there must leave the screen together.
 * Not measured: other modes' matchup screens (Smash, Spirits); the hold applies to any screen with option MATCHUP. */
#include <stdio.h>
#include <windows.h>
#include "runtime.h"
#include "hostrt.h"
#include "gfx_options.h"

int hook_install(uint64_t addr, void (*repl)(Ctx *, void *), void *user, ImportFn *orig);      /* fighter_sdk\hook_host.c (in nnro.c) */
int nnldn_role(void);                                                                          /* nnldn.c: 0 = not in a Local Wireless room */

static ImportFn g_orig_transient;
static int g_min_ms;
static uint64_t g_scene;                     /* the transient scene being timed (main thread only) */
static ULONGLONG g_t0;
static int g_said;

static void transient_update(Ctx *c, void *user) {
    (void)user;
    uint64_t scene = c->x[0];
    if (rd64(scene) == GA(0x7105061518ULL)) {
        uint64_t slot = rd64(GA(0x710532e7d8ULL)), lm = slot ? rd64(slot + 8) : 0;
        if (!lm || !rd8(lm + 0xf4)) g_scene = 0;                      /* not a matchup screen */
        else {
            if (scene != g_scene) { g_scene = scene; g_t0 = GetTickCount64(); g_said = 0; }
            if (!rd32(scene + 0x30) && !nnldn_role()) {
                ULONGLONG waited = GetTickCount64() - g_t0;
                if (waited < (ULONGLONG)g_min_ms) {
                    if (!g_said++) { printf("matchup screen: the fight was ready after %llu ms; the screen stays until %d ms (matchup_screen_min)\n", waited, g_min_ms); fflush(stdout); }
                    return;
                }
                g_scene = 0;                                          /* released: the next screen's scene may get the same address */
            }
        }
    }
    g_orig_transient(c);
}

void matchup_hold_init(void) {                /* boot.c, before nnMain: a first hook must be installed before the game runs */
    g_min_ms = gfx_matchup_screen_min_ms();
    if (g_min_ms <= 0) return;
    int id = hook_install(GA(0x710135dc30ULL), transient_update, NULL, &g_orig_transient);
    printf("matchup screen: kept at least %d ms (graphics.ini matchup_screen_min)%s\n", g_min_ms, id ? "" : ": the hook was refused, the option does nothing");
}
