/* The game's two once-a-frame points, each wrapped once, and the frame hooks registered on them
 * (frame_hooks.h says how they run). The wraps were the fight test's (hle_fight_test.c) until
 * rollback needed the same two functions: the build allows one wrap per guest function.
 *
 * This file's name sorts before hle_fight_test.c on purpose. tools/recomp.py puts the exported
 * names the runtime wraps into its tools fingerprint in the order of the runtime's sources, and
 * the frame line's was the first of them there; first here too, the fingerprint is the same, so
 * no game's generated C is made again for the move.
 *
 * The wraps have no code signature yet (tools/recomp.py wrap-sig prints one, from the game). */
#include "frame_hooks.h"
#include "host.h"

typedef struct FrameHook { int order; FrameHookFn *before, *after; void *ctx; } FrameHook;
typedef struct FrameHooks { int n; FrameHook hook[FRAME_HOOKS_MAX]; } FrameHooks;

/* A point's list is never changed once it is in use: a registration makes the next one, with one
 * handler more, and swaps it in. Lists only grow, so the one with n handlers is made once, and a
 * call that started on the one before it still has it whole. */
static FrameHooks lists[FRAME_HOOK_POINTS][FRAME_HOOKS_MAX];
static const FrameHooks *in_use[FRAME_HOOK_POINTS];   /* NULL: nothing registered */
static SRWLOCK adding = SRWLOCK_INIT;

int frame_hook_add(int point, int order, FrameHookFn *before, FrameHookFn *after, void *ctx) {
    if (point < 0 || point >= FRAME_HOOK_POINTS || (!before && !after)) return 0;
    AcquireSRWLockExclusive(&adding);
    const FrameHooks *was = in_use[point];
    int n = was ? was->n : 0, added = n < FRAME_HOOKS_MAX;
    if (added) {
        FrameHooks *l = &lists[point][n];
        int at = 0;
        while (at < n && was->hook[at].order <= order) at++;   /* (after those of its order: they are outside it) */
        for (int i = 0; i < at; i++) l->hook[i] = was->hook[i];
        l->hook[at] = (FrameHook){order, before, after, ctx};
        for (int i = at; i < n; i++) l->hook[i + 1] = was->hook[i];
        l->n = n + 1;
        __atomic_store_n(&in_use[point], l, __ATOMIC_RELEASE);
    }
    ReleaseSRWLockExclusive(&adding);
    return added;
}

int frame_hook_count(int point) {
    if (point < 0 || point >= FRAME_HOOK_POINTS) return 0;
    const FrameHooks *l = __atomic_load_n(&in_use[point], __ATOMIC_ACQUIRE);
    return l ? l->n : 0;
}

/* The befores from the outermost in, the game's function, the afters from the innermost out. */
static void run(const FrameHooks *l, Cpu *c, GuestFn *orig) {
    u64 args[8];
    memcpy(args, c->x, sizeof args);
    for (int i = 0; i < l->n; i++)
        if (l->hook[i].before) l->hook[i].before(c, args, l->hook[i].ctx);
    orig(c);
    for (int i = l->n; i-- > 0;)
        if (l->hook[i].after) l->hook[i].after(c, args, l->hook[i].ctx);
}

/* The fighters' frame line: lua2cpp::L2CFighterCommon::sys_line_system_control_fighter, which the
 * game runs for every fighter once a frame before its status scripts (x0 the agent). Wrapped by
 * its exported name, as lua2cpp_common lies where each install puts it. */
HLE_WRAP_EXPORT(_ZN7lua2cpp16L2CFighterCommon31sys_line_system_control_fighterEv) {
    const FrameHooks *l = __atomic_load_n(&in_use[FRAME_HOOK_LINE], __ATOMIC_ACQUIRE);
    if (!l) { orig(c); return; }
    run(l, c, orig);
}

/* The frame end of the graphics module (FRAME_END in game_addr.h): it compares the game's refresh
 * count with the one at the last frame, and reports count - (last + 1) "dropped" refreshes through
 * its third argument (x2), with the result 0xF0044007 / 0xF0044005 through its first (x0); the
 * game then runs that many extra logic frames on the same pad sample. */
HLE_WRAP(71038701e0) {
    const FrameHooks *l = __atomic_load_n(&in_use[FRAME_HOOK_END], __ATOMIC_ACQUIRE);
    if (!l) { f_71038701e0_orig(c); return; }
    run(l, c, f_71038701e0_orig);
}
