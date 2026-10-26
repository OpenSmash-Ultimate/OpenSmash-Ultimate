/* The rollback ring (hle_rollback_ring.c): the guest's writable memory as it was at each of the
 * last frames, kept with only the pages that changed from one to the next, and any of those
 * frames put back. */
#pragma once
#include "recomp.h"

typedef struct RingStats {
    u64 saves, restores;      /* so far */
    u64 frames;               /* frames held now */
    u64 written;              /* the last save: pages the write watch reported written */
    u64 checked;              /* ... pages compared with their copy (those, and the pages no watch sees) */
    u64 changed;              /* ... pages that differed, copied (or noted as zeros) */
    u64 save_ns;              /* ... its time */
    u64 pages;                /* pages the newest frame has: the memory map's writable regions */
    u64 copies;               /* page copies held now, 4 KiB each */
    u64 bytes;                /* host memory the ring holds: its copies' blocks, page table, undo logs, region lists */
    u64 put_back, unmapped;   /* the last restore: pages written back; pages of the frame not mapped now, left out */
} RingStats;

/* Ready for frames frames (2 or more): 1, or 0 if it was opened already. */
int rollback_ring_open(int frames);
/* The guest's writable memory now as the newest frame, the oldest dropped when the ring is full:
 * the frame's number (the first save's is 1). */
u64 rollback_ring_save(void);
/* Guest memory put back as it was at that frame, and the frames after it dropped (it is the
 * newest again). 0 if the ring does not have it. The memory map is not put back: pages the frame
 * had that are not mapped now are left out, and pages mapped since are zeros, as when new. */
int rollback_ring_restore(u64 frame);
u64 rollback_ring_oldest(void);   /* the frames held: 0 when none */
u64 rollback_ring_newest(void);
void rollback_ring_stats(RingStats *out);
