/* How long the drawing thread may wait, in one frame, for new shaders and pipelines (gpu_vk.c).
 * A new one is waited for only up to what is left of the frame's budget; past that its draw is
 * skipped and the maker carries on, so it draws from its next use. One hitch per new effect
 * becomes, at worst, the budget. */
#ifndef GPU_WAIT_BUDGET_H
#define GPU_WAIT_BUDGET_H
#include <stdint.h>

typedef struct WaitBudget {
    uint64_t frame;      /* the frame whose waits are counted */
    uint64_t spent_ns;   /* waited in it so far */
    uint64_t budget_ns;  /* a frame's whole budget */
} WaitBudget;

/* The time (now_ns's clock) a wait that starts now in `frame` may last until: its own cap, cut to
 * what is left of the frame's budget. */
static inline uint64_t wait_budget_until(WaitBudget *b, uint64_t frame, uint64_t now_ns, uint64_t cap_ns) {
    if (b->frame != frame) { b->frame = frame; b->spent_ns = 0; }
    uint64_t left = b->budget_ns > b->spent_ns ? b->budget_ns - b->spent_ns : 0;
    return now_ns + (cap_ns < left ? cap_ns : left);
}

static inline void wait_budget_spend(WaitBudget *b, uint64_t ns) { b->spent_ns += ns; }

#endif
