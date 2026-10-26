/* The write watch shared between its users, and the memory map as the rollback ring reads it
 * (runtime/kernel.c; the ring is hle_rollback_ring.c).
 *
 * The write watch keeps one bit a page, and a query that resets the bits takes the news from
 * everyone else asking about that page. The renderer resets the alias area's bits as it takes
 * them (gpu_d3d11.c, gpu_metal.m: watch_take), so a second user asking the system directly would
 * lose writes to the renderer, or steal them from it. So the renderer asks through
 * kernel_write_watch, which is GetWriteWatch itself unless SSBU_ROLLBACK_RING is on. With it on,
 * the kernel takes the system's bits in its own place, and keeps a bit a page for each user: a
 * write is news to each of them until that user takes it, as each would see it alone. */
#pragma once
#include "host.h"

enum { WATCH_RENDERER, WATCH_RING, WATCH_USERS };

/* Set once by kernel_init, before any guest thread: the rollback ring is on, and the write watch
 * is shared (kernel_watch_query). */
extern int kernel_watch_shared;

UINT kernel_watch_query(int who, DWORD flags, PVOID base, SIZE_T size, PVOID *addresses, ULONG_PTR *count, ULONG *granularity);

/* GetWriteWatch, for one user of it: the pages written since this user last took them (with
 * WRITE_WATCH_FLAG_RESET), in address order, 4 KiB each. Off, the system's own answer. */
static inline UINT kernel_write_watch(int who, DWORD flags, PVOID base, SIZE_T size, PVOID *addresses, ULONG_PTR *count, ULONG *granularity) {
    if (!kernel_watch_shared) return GetWriteWatch(flags, base, size, addresses, count, granularity);
    return kernel_watch_query(who, flags, base, size, addresses, count, granularity);
}

/* SSBU_ROLLBACK_RING: the frames the ring keeps (1: 8; 2 to 64: that many), 0 when off (unset or
 * any other value). Read from the environment each time, so it can be asked before kernel_init. */
int kernel_ring_frames(void);

/* The memory map's writable regions (read-write in svcQueryMemory's terms), sorted and merged
 * where they touch: guest addresses [lo, hi), and whether the write watch sees them. Between
 * kernel_memory_hold and kernel_memory_release nothing is mapped or unmapped (the kernel's lock:
 * every guest system call that uses the kernel's state waits, the memory ones and the waits and
 * signals alike). Returns how many there are; fills at most max. */
typedef struct KernelRange { u64 lo, hi; int watched; } KernelRange;
void kernel_memory_hold(void);
void kernel_memory_release(void);
u32 kernel_writable_ranges(KernelRange *out, u32 max);
