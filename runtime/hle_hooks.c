/* Hook slots (tools/hooks.txt `hookable`; runtime/hookable.h): where they are, and putting a
 * hook in one. Each slot adds itself to the list from a constructor, before main runs. */
#include "hookable.h"

static RtHook *hooks;

void rt_hook_add(RtHook *h) {
    RtHook *top = __atomic_load_n(&hooks, __ATOMIC_ACQUIRE);
    do h->next = top;
    while (!__atomic_compare_exchange_n(&hooks, &top, h, 1, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE));
}

RtHook *rt_hook_list(void) { return __atomic_load_n(&hooks, __ATOMIC_ACQUIRE); }

static RtHook *find(u64 addr) {
    for (RtHook *h = rt_hook_list(); h; h = h->next)
        if (h->addr == addr) return h;
    return NULL;
}

GuestFn **rt_hook_slot(u64 guest_addr) { RtHook *h = find(guest_addr); return h ? &h->slot : NULL; }

GuestFn *rt_hook_body(u64 guest_addr) { RtHook *h = find(guest_addr); return h ? h->body : NULL; }

/* (*next written before the release that publishes hook: a caller that loads hook from the slot,
 * with acquire, finds it written) */
int rt_hook_install(GuestFn **slot, GuestFn *hook, GuestFn **next) {
    if (!slot) return 0;
    GuestFn *old = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    do __atomic_store_n(next, old, __ATOMIC_RELAXED);
    while (!__atomic_compare_exchange_n(slot, &old, hook, 0, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE));
    return 1;
}

int rt_hook_remove(GuestFn **slot, GuestFn *hook, GuestFn *next) {
    return slot && __atomic_compare_exchange_n(slot, &hook, next, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}
