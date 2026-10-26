/* Hook slots: the functions tools/hooks.txt lists as `hookable`.
 *
 * A hookable function's lifted code is f_<address>_body: kept out of line (noinline) and, on
 * Windows, out of reach of the linker's identical-code folding (a section of its own, not a
 * COMDAT: /OPT:ICF folds only COMDATs; the macOS link passes -no_deduplicate, the Linux link no
 * --icf). Nothing calls it by name. Every way the guest reaches the function goes through its
 * slot, rt_hook_<address>.slot, which starts out holding f_<address>_body: a direct call or tail
 * call loads the slot and calls what it holds (RT_HOOK), and f_<address>, which the module's
 * table holds for rt_lookup (indirect calls, other modules' import stubs, rt_call), does the
 * same. A hook takes the function's place by swapping itself into the slot (rt_hook_install);
 * no machine code is patched. Calls load the slot with acquire and the swap stores it with
 * release, so a hook may go in while other threads call through the slot.
 *
 * A hookable function is also an exact-callee: its callers hand it every register they have
 * changed and read back every one they use, so a hook sees and can change all of them. A caller
 * through a pointer does so only if tools/hooks.txt lists it as an exact-caller; otherwise a
 * hook sees its x0-x18, x29, x30 and SP but stale x19-x28, and what it writes to x19-x28, x29 or
 * SP is lost. */
#pragma once
#include "recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RtHook {
    u64 addr;              /* the function's guest address */
    GuestFn *slot;         /* what calls to it run: the function, or a hook (atomic) */
    GuestFn *body;         /* the function itself, f_<address>_body */
    struct RtHook *next;
} RtHook;

/* ---- for the generated code ---- */
void rt_hook_add(RtHook *h);   /* (at start-up, from each slot's constructor) */
#define RT_HOOK(h) __atomic_load_n(&(h).slot, __ATOMIC_ACQUIRE)
#define RT_HOOK_ADD(h) static void __attribute__((constructor)) h##_add(void) { rt_hook_add(&h); }
#ifdef _WIN32
#define RT_HOOKABLE __attribute__((noinline, section(".text$rt_hook")))
#else
#define RT_HOOKABLE __attribute__((noinline))
#endif

/* ---- for hooks (runtime/hle_hooks.c) ---- */
RtHook *rt_hook_list(void);                 /* every hookable function in the program */
GuestFn **rt_hook_slot(u64 guest_addr);     /* its slot, or NULL: no hookable function starts there */
GuestFn *rt_hook_body(u64 guest_addr);      /* the function itself, or NULL */
/* Put hook in the slot. *next is set first to what the hook replaces (the function, or a hook
 * put in earlier), so a hook that runs the original by calling next(c) never finds it unset.
 * 0 if slot is NULL. */
int rt_hook_install(GuestFn **slot, GuestFn *hook, GuestFn **next);
/* Take hook back out, putting back next (what rt_hook_install gave it), if it is still the last
 * one put in: 1 if so, else 0 and nothing changes. A thread may still be running the hook, so
 * it stays callable. */
int rt_hook_remove(GuestFn **slot, GuestFn *hook, GuestFn *next);

#ifdef __cplusplus
}
#endif
