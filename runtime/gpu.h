/* State shared between the GPU device layer (hle_nv.c) and the GPU back end (gpu.c). */
#pragma once
#include "host.h"

#define NVMAP_MAX 65536
#define SYNCPT_MAX 192

typedef struct NvMap { u64 size, addr; u32 align, kind; } NvMap;          /* a memory handle */
typedef struct GpuMapping { u64 va, size, guest; } GpuMapping;           /* GPU VA -> guest memory */

extern NvMap gpu_maps[NVMAP_MAX];
extern u32 gpu_syncpt_min[SYNCPT_MAX], gpu_syncpt_max[SYNCPT_MAX];

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
void gpu_memory_unmapping(u64 addr, u64 size);            /* guest memory about to be decommitted (hle_nv.c) */
/* Until sync point `id` reaches `threshold` (or `timeout_ms` passes; ~0u: no limit): 1 reached. */
int gpu_syncpt_wait(u32 id, u32 threshold, u32 timeout_ms);

/* The multimedia engines (gpu_host1x.c): an nvmap handle's address in their address space,
 * and a command buffer for the engine of class cls ({handle, byte offset, word count}). */
u32 host1x_map(u32 handle);
void host1x_submit(u32 cls, u32 handle, u32 offset, u32 words);
