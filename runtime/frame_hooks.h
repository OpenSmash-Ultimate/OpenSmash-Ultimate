/* The frame hooks: code that runs at the game's two once-a-frame points, for any subsystem that
 * needs them, through one wrap of each. tools/recomp.py allows one wrap per guest function, and
 * both the fight test switches (hle_fight_test.c) and rollback need these two, so
 * hle_each_frame.c holds the wraps and runs the handlers registered here:
 *   FRAME_HOOK_END   the graphics module's frame end (FRAME_END in game_addr.h), once a frame;
 *   FRAME_HOOK_LINE  the fighters' frame line (lua2cpp::L2CFighterCommon::sys_line_system_control_fighter),
 *                    which the game runs for every fighter once a frame, before its status scripts.
 *
 * A registration is a pair of handlers, either of them NULL: `before` runs before the game's
 * function, `after` after it. The pairs nest as wraps would, one inside the other: the lowest
 * order is the outermost, so its before runs first and its after runs last. Pairs of the same
 * order nest in the order they were registered, the first one outside. Each handler is given
 * the guest context (before the call: its arguments; after it: its results), the argument
 * registers x0-x7 as the game called the function, and the pointer it was registered with.
 *
 * Registering is allowed at any time, from any thread: a call already running keeps the list it
 * started with, and the next call has the new one. A handler cannot be taken out again (one that
 * should stop checks a flag of its own). With nothing registered at a point, its wrap only calls
 * the game's function, at the cost of one load and a branch. */
#pragma once
#include "recomp.h"

enum { FRAME_HOOK_END, FRAME_HOOK_LINE, FRAME_HOOK_POINTS };
enum { FRAME_HOOKS_MAX = 8 };   /* registrations a point holds */

/* The orders in use, lowest outermost. The rollback ring is outside the fight test, so that it
 * saves the frame end's report as SSBU_DETERMINISTIC leaves it. */
enum { FRAME_ORDER_ROLLBACK = 100,     /* (hle_rollback_ring.c) */
       FRAME_ORDER_FIGHT_TEST = 200 };   /* (hle_fight_test.c) */

typedef void FrameHookFn(Cpu *c, const u64 args[8], void *ctx);

/* 1 when registered; 0 when the point is not one of the above, both handlers are NULL, or the
 * point holds FRAME_HOOKS_MAX already. */
int frame_hook_add(int point, int order, FrameHookFn *before, FrameHookFn *after, void *ctx);
/* The registrations a point holds now. */
int frame_hook_count(int point);
