/* State shared between the GPU device layer (hle_nv.c) and the GPU back end (gpu.c). */
#pragma once
#include "host.h"

#define NVMAP_MAX 65536
#define SYNCPT_MAX 192

typedef struct NvMap { u64 size, addr; u32 align, kind; } NvMap;          /* a memory handle */
typedef struct GpuMapping { u64 va, size, guest; } GpuMapping;           /* GPU VA -> guest memory */

extern NvMap gpu_maps[NVMAP_MAX];
extern u32 gpu_syncpt_min[SYNCPT_MAX], gpu_syncpt_max[SYNCPT_MAX];

/* Sync points are written by both the device and GPU threads. Keep every access
 * atomic, including diagnostic reads; a lock local to one reader is insufficient. */
static inline u32 gpu_syncpt_load(const u32 *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline void gpu_syncpt_store(u32 *p, u32 value) { __atomic_store_n(p, value, __ATOMIC_RELEASE); }
static inline u32 gpu_syncpt_add(u32 *p, u32 value) { return __atomic_add_fetch(p, value, __ATOMIC_ACQ_REL); }
static inline int gpu_syncpt_reached(u32 current, u32 target) { return (i32)(current - target) >= 0; }
static inline void gpu_syncpt_advance(u32 *p, u32 value) {
    u32 old = gpu_syncpt_load(p);
    while (!gpu_syncpt_reached(old, value) &&
           !__atomic_compare_exchange_n(p, &old, value, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {}
}
u32 gpu_syncpt_read(u32 id);                              /* atomic completed value; invalid id: 0 */

u64 gpu_va_to_guest(u64 va);

/* Work submitted on a channel: `count` GPFIFO entries (each an address/length pair of a
 * command list in GPU virtual memory). Run on the GPU thread (gpu_queue.c), or in the ioctl
 * with the device lock held when there is none. */
void gpu_submit(u32 channel, const u64 *entries, u32 count);

/* The GPU thread (gpu_queue.c): submissions run in order on a thread of their own, and the
 * sync point of each reaches its fence when it has run. */
int gpu_threaded(void);                                   /* is there one (SSBU_GPU_THREAD=0: no) */
void gpu_queue_submit(u32 channel, const u64 *entries, u32 count, u32 syncpoint, u32 fence);
u64 gpu_queue_mark(void);                                 /* submissions queued so far */
void gpu_queue_wait(u64 mark);                            /* until that many have run */
u64 gpu_queue_draws(u64 mark);                            /* (SSBU_SEQ) draw calls run once they had */
void gpu_queue_drain(void);                               /* until everything queued has run */
/* (rollback, hle_rb_state.c) The guest memory the GPU thread writes (semaphores, inline and copy-engine uploads,
 * constant buffer loads, the renderer's write-backs): pages, logged as runs, a run published once its writes are
 * done. A save copies memory while the GPU thread still runs what was queued before it, then waits for that and
 * copies again the pages logged meanwhile: what a drain first gave, without the wait in front of the copies. */
void gpu_rb_wrote(u64 guest, u64 bytes);                  /* (the GPU thread's writes; elsewhere nothing) */
u64 gpu_rb_written_count(void);                           /* runs published so far */
void gpu_rb_semaphore(u64 guest);                         /* (a semaphore release or report: its page, logged on its own) */
u64 gpu_rb_sema_count(void);
int gpu_rb_is_gpu_thread(void);                           /* (this thread is the GPU thread) */
u64 gpu_rb_sema_at(u64 i);                                /* page i of that log (guest), 0: overwritten since */
int gpu_rb_written_at(u64 i, u64 *lo, u64 *hi);           /* run i (guest pages [lo, hi)); 0: overwritten since */
u64 gpu_rb_bytes_count(void);                             /* the same writes byte for byte: entries so far */
u64 gpu_rb_bytes_cut(void);   /* (a save: the count; the writes after it go in new entries) */
int gpu_rb_bytes_at(u64 i, u64 *guest, u64 *bytes);       /* entry i; 0: overwritten since (GPU thread, or it idle) */
int gpu_queue_after(u64 mark, void (*fn)(void));          /* fn on the GPU thread once `mark` submissions ran; 0: ran already */
void gpu_queue_after_wait(void);                          /* until that fn has returned */
void gpu_memory_unmapping(u64 addr, u64 size);            /* guest memory about to be decommitted (hle_nv.c) */
/* Until sync point `id` reaches `threshold` (or `timeout_ms` passes; ~0u: no limit): 1 reached. */
int gpu_syncpt_wait(u32 id, u32 threshold, u32 timeout_ms);

/* The multimedia engines (gpu_host1x.c): an nvmap handle's address in their address space,
 * and a command buffer for the engine of class cls ({handle, byte offset, word count}). */
u32 host1x_map(u32 handle);
void host1x_submit(u32 cls, u32 handle, u32 offset, u32 words);
