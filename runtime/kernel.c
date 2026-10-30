/* A native stand-in for the console's kernel, at the system-call boundary.
 *
 * The recompiled SDK reaches the kernel through a few dozen `svc` wrappers; implementing
 * those calls here lets the SDK's own threading, locking, timing and memory code run
 * unmodified on host threads. Register conventions follow the SDK's wrappers: arguments in
 * x0-x7, the result code in w0, further outputs in x1 onwards.
 *
 * One lock guards all kernel state. A blocked thread sleeps on its own condition variable, in
 * the wait queue of what it waits for: only a signal of that, a cancel of its WaitSynchronization
 * or its timeout wakes it (see "wait queues"). Host threads are not pinned to processors (see
 * "priorities and cores").
 *
 * Calls that would talk to a system service (IPC) are not implemented here: they stop the
 * run and say so, since services are replaced at the SDK function level instead. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "host.h"
#include "gpu.h"   /* (gpu_memory_unmapping) */
#include "kernel_watch.h"   /* (the write watch shared with the rollback ring) */
#include "fight_test.h"   /* (SSBU_FIXED_SEED) */
#include "sleep.h"   /* (svcSleepThread) */

#ifdef __linux__
#include <sys/resource.h>   /* (RLIMIT_NICE: see "priorities and cores") */
#include <unistd.h>         /* (geteuid) */
#endif
#undef R_OK   /* (unistd.h's access() flag: here R_OK is the kernel's result code, below) */

#ifdef __APPLE__
void rt_upnp_remove(void);   /* (linux/upnp.c: the netplay relay's port mapping, if it made one) */
#endif

enum { R_OK = 0, R_TIMEOUT = 0xEA01, R_CANCELLED = 0xEC01, R_INVALID_HANDLE = 0xE401, R_INVALID_STATE = 0xFA01,
       R_INVALID_ADDRESS = 0xCC01, R_INVALID_ENUM = 0xF001, R_OUT_OF_MEMORY = 0xD001, R_INVALID_PRIORITY = 0xE001,
       R_INVALID_CORE = 0xE201, R_INVALID_COMBINATION = 0xE801 };
enum { GM_FREE = 0, GM_CODE = 3, GM_CODE_DATA = 4, GM_HEAP = 5, GM_STACK = 0xB, GM_TLS = 0xC, GM_INACCESSIBLE = 0x10 };
enum { OBJ_FREE, OBJ_THREAD, OBJ_EVENT, OBJ_SESSION, OBJ_MEMORY };

#define WAITERS 0x40000000u          /* "has waiters" bit of a userspace mutex word */
#define CUR_THREAD 0xFFFF8000u
#define CUR_PROCESS 0xFFFF8001u
#define HOST_STACK (256ull << 20)    /* guest call depth is host call depth */
#define MAIN_STACK (1u << 20)     /* from the program's metadata */
#define CORES 0x7ull                        /* the cores an application may use (GetInfo 0) */
#define PRIORITIES 0x0FFFFFFFF0000000ull    /* ... and its priorities, 28 to 59 (GetInfo 1) */

/* what a thread waits for in the kernel (KThread.wait; see "wait queues") */
enum { WAIT_NONE, WAIT_OBJECTS, WAIT_MUTEX, WAIT_CONDITION, WAIT_ADDRESS };

typedef struct KWaitNode KWaitNode;

typedef struct KThread {
    u32 handle;
    HANDLE host;
    Cpu *cpu;
    u64 entry, arg, stack_top, tlr, id;
    i32 priority;       /* (written under the lock, read by the thread itself outside it: atomic) */
    i32 core;           /* its ideal core, -1 for none (see "priorities and cores") */
    u64 mask;           /* its core mask */
    i32 processor;      /* what GetCurrentProcessorNumber answers on it (atomic, as priority) */
    int band;           /* (SSBU_HOST_PRIORITIES) the host priority it has set itself */
    int started, exited, cancelled;
    int wait;                   /* WAIT_NONE, or what the thread waits for now */
    struct WaitQueue *queue;    /* (a mutex, a condition variable, an address) the queue it is in */
    struct KThread *qprev, *qnext;   /* its neighbours there */
    KWaitNode *nodes;           /* (WaitSynchronization) its place in each object's queue, one a handle */
    u32 wait_count;
    u32 mutex_tag;      /* value to store in the mutex word when it is handed over */
    u64 cv_mutex;       /* the mutex to re-acquire when a condition variable wait is signalled */
    u64 wakes;          /* how often it was woken (kwake): the tests check a signal wakes only its own waiters */
    CONDITION_VARIABLE woken;   /* what this thread sleeps on (kwait): wakes go to the threads concerned */
    struct KThread *next_dead;   /* (recycling) the next thread waiting for its host thread to end */
} KThread;

/* refs: handles open on it; waits: WaitSynchronization calls holding it; gen: how often its slot
 * was used, in the handle's upper bits (these three for recycling: see new_handle); first, last:
 * its queue of WaitSynchronization waiters */
typedef struct KObject { int type, signaled; KThread *thread; void *session; u32 gen, refs, waits; KWaitNode *first, *last; } KObject;
/* a WaitSynchronization's place in one object's queue (on the waiting thread's stack) */
struct KWaitNode { KThread *thread; KObject *object; KWaitNode *prev, *next; u32 index; };
typedef struct Region { u64 base, size; u32 state, perm; } Region;

static CRITICAL_SECTION klock;
/* The threads running or named by a handle (an exited one stays while its handle is open: it may
 * still be waited on), or without recycling every thread the kernel made. */
static KThread *threads[1024];
static u32 nthreads;
static KObject objects[4096];
#define REGIONS 16384
static Region regions[REGIONS];
static u32 region_count;
static struct { u64 dst, src, size; } maps[1024];
static u64 heap_size, tlr_next = KERNEL_BASE, next_thread_id = 1;
#define TLR_AREA 0x1000000ull                /* thread-local regions: a page each from KERNEL_BASE */
static int recycle;                          /* SSBU_THREAD_RECYCLING=1 (see "thread lifetime") */
static KThread *dead;                        /* (recycling) threads freed but for their host thread */
static u64 spare_tlrs[TLR_AREA / 0x1000];    /* (recycling) regions of freed threads, for the next ones */
static u32 nspare_tlrs;
static int said_half_full;                   /* (recycling) see say_half_full */
static int priority_queues;                  /* SSBU_PRIORITY_QUEUES=1 (see "wait queues") */
static int host_priorities;                  /* SSBU_HOST_PRIORITIES=1 (see "priorities and cores") */
static int core_masks;                       /* SSBU_CORE_MASKS=1 (likewise) */

/* ---- time ------------------------------------------------------------------------ */
static u64 now_ns(void) { return rt_clock_ns(); }

u64 rt_ticks(void) { return rt_clock_ns() * 12 / 625; }   /* (19.2 MHz) */

static u64 deadline_of(i64 timeout_ns) { return timeout_ns < 0 ? ~0ull : now_ns() + (u64)timeout_ns; }

/* Sleep until something wakes this thread (kwake) or the deadline passes; returns 0 once it
 * has passed. Each thread sleeps on its own condition variable: a mutex handed over, a
 * condition variable signalled, an event set wakes just the threads waiting for it (one shared
 * one, woken in full, sent every sleeping thread round the kernel lock at every change). The
 * callers look at their wait again either way (the host can wake a thread for nothing). */
static int kwait(KThread *me, u64 deadline) {
    DWORD ms = INFINITE;
    if (deadline != ~0ull) {
        u64 now = now_ns();
        if (now >= deadline) return 0;
        ms = (DWORD)((deadline - now + 999999) / 1000000);
    }
    SleepConditionVariableCS(&me->woken, &klock, ms);
    return 1;
}

static void kwake(KThread *t) { t->wakes++; WakeConditionVariable(&t->woken); }

/* ---- wait queues -------------------------------------------------------------------
 * A blocked thread is in the queue of what it waits for, and a signal wakes only the threads in
 * that queue: a userspace mutex, a condition variable and an arbiter address have one each (by
 * address, while a thread waits there), a kernel object has its own for WaitSynchronization (a
 * wait on several objects is in each of their queues). First come, first served, as on the
 * console among threads of one priority. Whoever wakes a thread takes it out of its queues first
 * and sets its wait back to WAIT_NONE, so a thread's wait - what it waits for and where in the
 * queue - is all in its KThread. (A woken WaitSynchronization looks at its objects again, as
 * before: one cleared before it runs is waited on again, where on the console the signal would
 * have ended the wait.)
 *
 * SSBU_PRIORITY_QUEUES=1 orders a mutex's, a condition variable's and an address's queue as the
 * console does, by priority and then arrival (mesosphere: a mutex owner's waiter list and the
 * condition variable and arbiter trees), and a thread whose priority changes while it waits moves
 * behind the threads of its new priority. (An object's queue stays as it is: a signal wakes all
 * of it.) Off by default: on the console the higher thread is also the one that runs, while here
 * every thread runs at once whatever its priority, and only first come first served is sure never
 * to leave a waiter behind for good. */
#define QUEUES 1024   /* (a waiting thread is in one at most) */
typedef struct WaitQueue { struct WaitQueue *next; KThread *first, *last; u64 key; u32 count; int kind; } WaitQueue;
static WaitQueue queue_pool[QUEUES], *spare_queues, *queues[QUEUES];
static u32 queues_made;

static WaitQueue **queue_slot(int kind, u64 key) {
    WaitQueue **p = &queues[((key ^ (u64)kind << 60) * 0x9E3779B97F4A7C15ull) >> 54];
    while (*p && ((*p)->kind != kind || (*p)->key != key)) p = &(*p)->next;
    return p;
}

/* The first thread waiting for (kind, key), and how many wait there; NULL if none. */
static KThread *first_waiting(int kind, u64 key, u32 *count) {
    WaitQueue *q = *queue_slot(kind, key);
    *count = q ? q->count : 0;
    return q ? q->first : NULL;
}

/* `t` into queue q: behind the threads there, or with SSBU_PRIORITY_QUEUES behind those of its
 * priority and higher only. */
static void place(WaitQueue *q, KThread *t) {
    KThread *ahead = q->last;
    if (priority_queues)
        while (ahead && ahead->priority > t->priority) ahead = ahead->qprev;
    t->qprev = ahead;
    t->qnext = ahead ? ahead->qnext : q->first;
    if (t->qnext) t->qnext->qprev = t; else q->last = t;
    if (ahead) ahead->qnext = t; else q->first = t;
}

static void unlink_thread(WaitQueue *q, KThread *t) {
    if (t->qprev) t->qprev->qnext = t->qnext; else q->first = t->qnext;
    if (t->qnext) t->qnext->qprev = t->qprev; else q->last = t->qprev;
}

/* `t` waits for (kind, key) from now on, behind the threads waiting there already. */
static void enqueue(KThread *t, int kind, u64 key) {
    WaitQueue **p = queue_slot(kind, key), *q = *p;
    if (!q) {
        if (spare_queues) { q = spare_queues; spare_queues = q->next; }
        else if (queues_made < QUEUES) q = &queue_pool[queues_made++];
        else rt_fatal("kernel: out of wait queues");
        *q = (WaitQueue){NULL, NULL, NULL, key, 0, kind};
        *p = q;
    }
    t->wait = kind;
    t->queue = q;
    place(q, t);
    q->count++;
}

/* `t`'s wait for a mutex, a condition variable or an address is over: out of its queue (a queue
 * left empty goes back to the spares). */
static void unqueue(KThread *t) {
    WaitQueue *q = t->queue;
    unlink_thread(q, t);
    t->wait = WAIT_NONE;
    t->queue = NULL;
    t->qprev = t->qnext = NULL;
    if (--q->count == 0) {
        *queue_slot(q->kind, q->key) = q->next;
        q->next = spare_queues;
        spare_queues = q;
    }
}

/* (SSBU_PRIORITY_QUEUES) `t`'s priority changed while it waits for a mutex, a condition variable
 * or an address: behind the threads of its new priority there. */
static void requeue(KThread *t) {
    if (!priority_queues || !t->queue) return;
    unlink_thread(t->queue, t);
    place(t->queue, t);
}

/* `t`, in WaitSynchronization, out of every object's queue. */
static void unqueue_objects(KThread *t) {
    for (u32 i = 0; i < t->wait_count; i++) {
        KWaitNode *n = &t->nodes[i];
        if (n->prev) n->prev->next = n->next; else n->object->first = n->next;
        if (n->next) n->next->prev = n->prev; else n->object->last = n->prev;
    }
    t->wait = WAIT_NONE;
    t->nodes = NULL;
    t->wait_count = 0;
}

/* Object `o` became signalled: every thread waiting for it in WaitSynchronization is woken, first
 * come first (and leaves its other objects' queues: one signal wakes it once). */
static void notify(KObject *o) {
    while (o->first) {
        KThread *t = o->first->thread;
        unqueue_objects(t);
        kwake(t);
    }
}

/* ---- handles ---------------------------------------------------------------------
 * Without recycling a handle is 0xA000 plus its object's slot, as before. With it, the console's
 * layout as Atmosphere's open-source kernel has it (mesosphere's kern_k_handle_table.hpp: index
 * in bits 0-14, a linear id 1 to 0x7FFF in bits 15-29, bits 30-31 zero) - the linear id here is
 * the slot's use count, so that a handle kept past CloseHandle names nothing rather than the
 * slot's next object. Never 0, and below bit 30: mutex words hold owner handles under their
 * WAITERS bit, and the pseudo-handles (0xFFFF800x) have the top bits set. */
static void say_half_full(void);

static u32 new_handle(int type, KThread *t) {
    for (u32 i = 1; i < 4096; i++)
        if (objects[i].type == OBJ_FREE) {
            u32 gen = objects[i].gen % 0x7FFF + 1;
            /* (the queue stays: without recycling a wait on a handle closed meanwhile waits on the
             * slot's next object, as when waits matched handle values) */
            objects[i] = (KObject){type, 0, t, NULL, gen, 1, 0, objects[i].first, objects[i].last};
            if (recycle && i >= 2048 && !said_half_full) say_half_full();
            return recycle ? gen << 15 | i : 0xA000 + i;
        }
    rt_fatal("kernel: out of handles");
}

/* (recycling) Said once, when the first free slot is past half the table. A handle nothing
 * closes keeps its slot for good - one of svcCreateEvent's two values never closed, or one the
 * game moves to a native service (ipc.c keeps none) - and a slow leak ends at "out of handles"
 * (without recycling an event's first CloseHandle frees it). */
static void say_half_full(void) {
    u32 n[OBJ_MEMORY + 1] = {0};
    for (u32 i = 1; i < 4096; i++) n[objects[i].type]++;
    said_half_full = 1;
    fprintf(stderr, "[kernel] half the handle table in use: %u threads, %u events, %u sessions, %u transfer memories (4095 stop the run)\n",
            n[OBJ_THREAD], n[OBJ_EVENT], n[OBJ_SESSION], n[OBJ_MEMORY]);
}

static u32 index_of(u32 handle) { return recycle ? handle & 0x7FFF : handle - 0xA000; }

/* The slot of the object an open handle names, or 0 (never a slot). With recycling a closed
 * handle names nothing, though its object may live on (a running thread, one a wait holds). */
static u32 slot_of(u32 handle) {
    u32 i = index_of(handle);
    if (i >= 4096 || objects[i].type == OBJ_FREE) return 0;
    return !recycle || (objects[i].refs && handle >> 15 == objects[i].gen) ? i : 0;
}

static KObject *object_of(Cpu *c, u32 handle, int type) {
    u32 i = handle == CUR_THREAD && type == OBJ_THREAD ? index_of(((KThread *)c->thread)->handle) : slot_of(handle);
    return i && objects[i].type == type ? &objects[i] : NULL;
}

static void drop_thread(KThread *t);

/* (recycling) Free an object nothing names any more: no handle open, no wait holding it, and
 * for a thread, not running (exited, or never started). */
static void release(u32 i) {
    KObject *o = &objects[i];
    if (o->refs || o->waits) return;
    if (o->type == OBJ_THREAD) {
        if (o->thread->started && !o->thread->exited) return;
        drop_thread(o->thread);
    }
    o->type = OBJ_FREE;   /* (gen stays: the slot's next object gets the next one) */
}

/* (recycling) CloseHandle. svcCreateEvent's object counts its two handles, one value here: the
 * SDK closes both when it destroys a system event (inferred, not seen in the game: Atmosphere's
 * libstratosphere, an open reimplementation of nn::os, does so in DestroyInterProcessEvent, and
 * aborts if a CloseHandle fails - so the second close must succeed too). A handle native code
 * hands out again counts once more each time (kernel_copy_handle: on the console each copy is a
 * handle of its own). */
static u32 close_handle(u32 handle) {
    u32 k = slot_of(handle);
    if (!k) return R_INVALID_HANDLE;
    if (--objects[k].refs == 0 && objects[k].type == OBJ_SESSION) ipc_close(objects[k].session);
    release(k);
    return R_OK;
}

/* ---- memory map ------------------------------------------------------------------ */
/* The regions are sorted and apart. Memory the game maps and unmaps as it goes (heap, alias
 * memory, stacks, thread-local regions) is merged with a neighbour of the same kind, as the
 * console's kernel does: the game grows its pools a piece at a time, and with one region per
 * piece a long session of matches ran out of them ("too many memory regions", ten minutes and
 * ~90 fighters in). */
static int merges(u32 state) { return state == GM_HEAP || state == GM_STACK || state == GM_TLS; }

static u32 insert_region(u64 base, u64 size, u32 state, u32 perm) {
    if (region_count == REGIONS) rt_fatal("kernel: too many memory regions");
    u32 i = region_count++;
    for (; i > 0 && regions[i - 1].base > base; i--) regions[i] = regions[i - 1];
    regions[i] = (Region){base, size, state, perm};
    return i;
}

static void erase_region(u32 i) { memmove(&regions[i], &regions[i + 1], (--region_count - i) * sizeof *regions); }

/* Forget [base, base + size): regions inside it go, regions straddling its ends are trimmed. */
static void unmap_range(u64 base, u64 size) {
    u64 end = base + size;
    for (u32 i = 0; i < region_count;) {
        Region r = regions[i];
        u64 rend = r.base + r.size;
        if (rend <= base || r.base >= end || !merges(r.state)) { i++; continue; }
        erase_region(i);
        if (r.base < base) insert_region(r.base, base - r.base, r.state, r.perm);
        if (rend > end) insert_region(end, rend - end, r.state, r.perm);
        i = 0;
    }
}

/* (also from the loader, outside system calls: the lock keeps QueryMemory's search off a
 * half-moved table) */
void kernel_add_region(u64 base, u64 size, u32 state, u32 perm) {
    if (!size) return;
    EnterCriticalSection(&klock);
    if (!merges(state)) insert_region(base, size, state, perm);
    else {
        unmap_range(base, size);   /* (mapped again over itself: the new mapping is the one there) */
        u32 i = insert_region(base, size, state, perm);
        if (i + 1 < region_count && regions[i + 1].base == base + size && regions[i + 1].state == state && regions[i + 1].perm == perm) {
            regions[i].size += regions[i + 1].size;
            erase_region(i + 1);
        }
        if (i > 0 && regions[i - 1].base + regions[i - 1].size == base && regions[i - 1].state == state && regions[i - 1].perm == perm) {
            regions[i - 1].size += regions[i].size;
            erase_region(i);
        }
    }
    LeaveCriticalSection(&klock);
}

static void query_memory(u64 info, u64 addr) {
    Region r = {0, 0, GM_FREE, 0};
    u64 end = ASPACE_BASE + ASPACE_SIZE, prev = 0;
    if (addr >= end) r = (Region){end, 0 - end, GM_INACCESSIBLE, 0};
    else {
        /* the first region ending above addr: the regions are sorted and apart, so their ends
         * are in order too (thousands of them, and C++ exceptions ask tens of thousands of
         * times a second - libunwind finds each frame's module through QueryMemory) */
        u32 i = 0, hi = region_count;
        while (i < hi) {
            u32 mid = (i + hi) / 2;
            if (regions[mid].base + regions[mid].size <= addr) i = mid + 1;
            else hi = mid;
        }
        if (i) prev = regions[i - 1].base + regions[i - 1].size;
        if (i < region_count && regions[i].base <= addr) r = regions[i];
        else r = (Region){prev, (i < region_count ? regions[i].base : end) - prev, GM_FREE, 0};
    }
    st64(info, r.base);
    st64(info + 8, r.size);
    st32(info + 16, r.state);
    st32(info + 20, 0);
    st32(info + 24, r.perm);
    st32(info + 28, 0);
    st64(info + 32, 0);
}

static int map_guarded(u64 addr, u64 size);   /* (see "memory mappings") */

/* 8 bytes of guest memory at addr if a region holds them: 0 if that memory is not mapped (hle_fiber.c
 * looks into fiber objects the game may have freed), or is a mapping's source SSBU_MAP_GUARD made
 * inaccessible. Under the lock, so it cannot be unmapped between the look and the read. */
int kernel_read64(u64 addr, u64 *out) {
    int ok = 0;
    EnterCriticalSection(&klock);
    u32 i = 0, hi = region_count;
    while (i < hi) {
        u32 mid = (i + hi) / 2;
        if (regions[mid].base + regions[mid].size <= addr) i = mid + 1;
        else hi = mid;
    }
    if (i < region_count && regions[i].base <= addr && addr + 8 <= regions[i].base + regions[i].size && (regions[i].perm & 1) && !map_guarded(addr, 8)) {
        *out = ld64(addr);
        ok = 1;
    }
    LeaveCriticalSection(&klock);
    return ok;
}

/* ---- the write watch, shared (kernel_watch.h) --------------------------------------
 * With SSBU_ROLLBACK_RING on, the heap, stack and thread-local areas are reserved with the write
 * watch as the alias area is, and every question goes through kernel_watch_query: the system's
 * bits are taken (and reset) here, into a bit a page for each user of the area, and each user is
 * answered from its own. The renderer has the alias area, as it had the system's bits there
 * before; the ring has all four. A page decommitted is news to the ring, which saw its contents
 * go. The renderer's bits are left as they are: the system may forget a decommitted page's bit
 * (the Linux and macOS layers do), so the renderer can hear of a page it would not have alone,
 * which costs an upload, but never misses one.
 *
 * What sharing costs, only with the switch on: a renderer's question takes the system's bits
 * whenever any are set, where its first, cheap one only looked before; and the ring takes all
 * four areas' bits at each frame end, so where the watch is page protection (macOS, and Linux
 * before 6.7) the first write to each page after a frame end faults. On Windows and newer Linux
 * the system notes the writes itself. */
#define WATCH_PAGE 0x1000ull
#define WATCH_BRIDGE (32 * WATCH_PAGE)   /* written pages closer than this are reset in one call */
typedef struct KWatch { u64 base, size; u32 users; u64 *pending[WATCH_USERS]; } KWatch;
int kernel_watch_shared;
static KWatch watch_areas[4];
static u32 nwatch_areas;
static SRWLOCK watch_lock = SRWLOCK_INIT;
static void **watch_asked, **watch_taken;   /* (65536 each, under watch_lock; made with the first area) */

static void watch_add(u64 base, u64 size, u32 users) {
    if (!watch_asked && (!(watch_asked = malloc(65536 * sizeof *watch_asked)) || !(watch_taken = malloc(65536 * sizeof *watch_taken))))
        rt_fatal("kernel: no memory for the write watch's answers");
    KWatch *a = &watch_areas[nwatch_areas++];
    a->base = base;
    a->size = size;
    a->users = users;
    for (int u = 0; u < WATCH_USERS; u++)
        if ((users >> u & 1) && !(a->pending[u] = calloc(size / WATCH_PAGE / 64 + 1, 8)))
            rt_fatal("kernel: no memory for the write watch's bits");
}

static KWatch *watch_area(u64 lo, u64 hi) {
    for (u32 i = 0; i < nwatch_areas; i++)
        if (watch_areas[i].base <= lo && hi <= watch_areas[i].base + watch_areas[i].size) return &watch_areas[i];
    return NULL;
}

static void watch_mark(KWatch *a, u64 addr) {
    u64 i = (addr - a->base) / WATCH_PAGE;
    for (int u = 0; u < WATCH_USERS; u++)
        if (a->users >> u & 1) a->pending[u][i >> 6] |= 1ull << (i & 63);
}

/* The system's bits for [lo, hi) of an area into its users' (under watch_lock). Asking is cheap
 * and resetting is not (on macOS it protects every host page it touches again): so ask first,
 * then reset only around the pages found written. A write landing after its page's reset is
 * news the next time. */
static void watch_drain(KWatch *a, u64 lo, u64 hi) {
    for (u64 at = lo; at < hi;) {
        ULONG_PTR n = 65536;
        ULONG granularity;
        if (GetWriteWatch(0, G2H(at), hi - at, watch_asked, &n, &granularity) != 0 || !n) return;
        for (ULONG_PTR k = 0; k < n;) {
            u64 from = H2G(watch_asked[k]), to = from + WATCH_PAGE;
            for (k++; k < n && H2G(watch_asked[k]) <= to + WATCH_BRIDGE; k++) to = H2G(watch_asked[k]) + WATCH_PAGE;
            for (u64 t = from; t < to;) {
                ULONG_PTR m = 65536;
                if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, G2H(t), to - t, watch_taken, &m, &granularity) != 0 || !m) break;
                for (ULONG_PTR j = 0; j < m; j++) watch_mark(a, H2G(watch_taken[j]));
                if (m < 65536) break;
                t = H2G(watch_taken[m - 1]) + WATCH_PAGE;
            }
        }
        if (n < 65536) return;
        at = H2G(watch_asked[n - 1]) + WATCH_PAGE;
    }
}

UINT kernel_watch_query(int who, DWORD flags, PVOID base, SIZE_T size, PVOID *addresses, ULONG_PTR *count, ULONG *granularity) {
    u64 lo = H2G(base) & ~(WATCH_PAGE - 1), hi = (H2G(base) + size + WATCH_PAGE - 1) & ~(WATCH_PAGE - 1);
    KWatch *a = watch_area(lo, hi);
    if (!a || who < 0 || who >= WATCH_USERS || !(a->users >> who & 1) || !count || hi <= lo) {   /* (as the system: not a watched area of its) */
        SetLastError(ERROR_INVALID_PARAMETER);
        return 1;
    }
    AcquireSRWLockExclusive(&watch_lock);
    watch_drain(a, lo, hi);
    u64 *bits = a->pending[who];
    ULONG_PTR cap = *count, n = 0;
    for (u64 i = (lo - a->base) / WATCH_PAGE, end = (hi - a->base) / WATCH_PAGE; i < end && n < cap;) {
        u64 w = bits[i >> 6] >> (i & 63);
        if (!w) { i = (i | 63) + 1; continue; }
        i += (u64)__builtin_ctzll(w);
        if (i >= end) break;
        addresses[n++] = G2H(a->base + i * WATCH_PAGE);
        if (flags & WRITE_WATCH_FLAG_RESET) bits[i >> 6] &= ~(1ull << (i & 63));
        i++;
    }
    ReleaseSRWLockExclusive(&watch_lock);
    *count = n;
    if (granularity) *granularity = (ULONG)WATCH_PAGE;
    return 0;
}

/* (svcUnmapMemory, svcUnmapPhysicalMemory: before the decommit) */
static void watch_decommitting(u64 base, u64 size) {
    if (!kernel_watch_shared) return;
    u64 lo = base & ~(WATCH_PAGE - 1), hi = (base + size + WATCH_PAGE - 1) & ~(WATCH_PAGE - 1);
    KWatch *a = watch_area(lo, hi);
    if (!a || !(a->users >> WATCH_RING & 1)) return;
    AcquireSRWLockExclusive(&watch_lock);
    for (u64 i = (lo - a->base) / WATCH_PAGE; i < (hi - a->base) / WATCH_PAGE; i++) a->pending[WATCH_RING][i >> 6] |= 1ull << (i & 63);
    ReleaseSRWLockExclusive(&watch_lock);
}

int kernel_ring_frames(void) {
    const char *v = getenv("SSBU_ROLLBACK_RING");
    if (!v || !*v) return 0;
    if (!strcmp(v, "1")) return 8;
    for (const char *d = v; *d; d++)
        if (*d < '0' || *d > '9') return 0;
    long n = strtol(v, NULL, 10);
    return n >= 2 && n <= 64 ? (int)n : 0;
}

void kernel_memory_hold(void) { EnterCriticalSection(&klock); }
void kernel_memory_release(void) { LeaveCriticalSection(&klock); }

/* (merged only inside one watched area, or outside them all: the stack area ends where the
 * thread-local one starts, and a question of the write watch is about one area) */
u32 kernel_writable_ranges(KernelRange *out, u32 max) {
    EnterCriticalSection(&klock);
    u32 n = 0;
    KernelRange last = {0, 0, 0};
    KWatch *last_area = NULL;
    for (u32 i = 0; i < region_count; i++) {
        if ((regions[i].perm & 3) != 3) continue;
        KWatch *a = watch_area(regions[i].base, regions[i].base + regions[i].size);
        KernelRange k = {regions[i].base, regions[i].base + regions[i].size, a != NULL};
        if (n && last.hi == k.lo && last_area == a) {
            last.hi = k.hi;
            if (n <= max) out[n - 1].hi = k.hi;
            continue;
        }
        if (n < max) out[n] = k;
        last = k;
        last_area = a;
        n++;
    }
    LeaveCriticalSection(&klock);
    return n;
}

/* ---- memory mappings (svcMapMemory) ------------------------------------------------
 * On the console a mapping's pages appear at its destination, and its source is locked (no
 * access at all) until the unmap. Here the destination is a copy, written back over the source
 * at the unmap: the same to a game that keeps to the console's rules, for two copies and the
 * destination's memory. (An alias needs the source in memory that can be mapped twice: a
 * section on Windows, whose views cannot be decommitted with VirtualFree or write-watched; a
 * shared mapping on Linux; on macOS anonymous memory will do, but in 16 KiB pages, not the
 * guest's 4 KiB. The heap and the alias area would have to be allocated that way.) The status
 * line says what the copies cost.
 *
 * SSBU_MAP_GUARD=1 makes the source inaccessible while it is mapped, as on the console, so that
 * a stray access to it faults - and the crash report names the guest function - rather than
 * read what the copy left there or write what the unmap overwrites. Guarded: a source that is
 * read-write memory in the memory map, outside the alias area (where the write watch sets the
 * pages' protection itself, on macOS and on Linux before 6.7); on macOS only the 16 KiB host
 * pages wholly inside it (a host page it shares with other memory stays as it is). svcQueryMemory
 * reports the source as without the switch; kernel_read64 treats the guarded pages as not mapped
 * (it would fault on them). A system call writing into them (a file read, say) fails with an
 * error rather than faulting. */
#ifdef __APPLE__
#define HOST_PAGE 0x4000ull
#else
#define HOST_PAGE 0x1000ull
#endif
static struct { u64 lo, hi; } guarded[1024];   /* (a slot of maps each) the source's pages made inaccessible */
static int map_guard;
static u64 maps_made, maps_guarded, map_bytes, map_copy_ns;
static u32 maps_live, maps_peak;

/* Whether the memory map has all of [base, base + size) as read-write memory. */
static int read_write(u64 base, u64 size) {
    u32 i = 0, hi = region_count;
    while (i < hi) {
        u32 mid = (i + hi) / 2;
        if (regions[mid].base + regions[mid].size <= base) i = mid + 1;
        else hi = mid;
    }
    for (u64 at = base; at < base + size; i++) {
        if (i == region_count || regions[i].base > at || (regions[i].perm & 3) != 3) return 0;
        at = regions[i].base + regions[i].size;
    }
    return 1;
}

static void guard_source(u32 i) {
    u64 lo = (maps[i].src + HOST_PAGE - 1) & ~(HOST_PAGE - 1), hi = (maps[i].src + maps[i].size) & ~(HOST_PAGE - 1);
    DWORD old;
    if (lo >= hi || !read_write(maps[i].src, maps[i].size) || (maps[i].src < ALIAS_BASE + ALIAS_SIZE && ALIAS_BASE < maps[i].src + maps[i].size) ||
        !VirtualProtect(G2H(lo), hi - lo, PAGE_NOACCESS, &old))
        return;
    guarded[i].lo = lo; guarded[i].hi = hi;
    maps_guarded++;
}

/* Whether a guarded source has any of [addr, addr + size) (kernel_read64 must not read there). */
static int map_guarded(u64 addr, u64 size) {
    for (u32 i = 0; map_guard && i < 1024; i++)
        if (guarded[i].hi && addr < guarded[i].hi && guarded[i].lo < addr + size) return 1;
    return 0;
}

static void unguard_source(u32 i) {
    DWORD old;
    if (guarded[i].hi && !VirtualProtect(G2H(guarded[i].lo), guarded[i].hi - guarded[i].lo, PAGE_READWRITE, &old))
        rt_fatal("kernel: cannot make the source of the mapping at %#llx accessible again (error %lu)", maps[i].dst, (unsigned long)GetLastError());
    guarded[i].lo = guarded[i].hi = 0;
}

/* For the status lines: the mappings made and what their copies took. */
void kernel_map_status(void) {
    EnterCriticalSection(&klock);
    fprintf(stderr, "[status] memory mappings: %llu made (%.1f MiB in all), %u mapped now (at most %u at once, of 1024); %.1f ms copying",
            maps_made, (double)map_bytes / (1 << 20), maps_live, maps_peak, (double)map_copy_ns / 1e6);
    if (map_guard) fprintf(stderr, "; %llu sources made inaccessible while mapped (SSBU_MAP_GUARD)", maps_guarded);
    fprintf(stderr, "\n");
    LeaveCriticalSection(&klock);
}

static void *commit(u64 at, u64 size, const char *what) {
    void *p = VirtualAlloc(G2H(at), size, MEM_COMMIT, PAGE_READWRITE);
    if (!p) rt_fatal("kernel: cannot commit %llu KiB at %#llx for %s (error %lu)", size >> 10, at, what, (unsigned long)GetLastError());
    return p;
}

/* ---- thread lifetime ---------------------------------------------------------------
 * With SSBU_THREAD_RECYCLING=1, as on the console: a thread object lives while its thread runs
 * or a handle names it, and once it has exited and its handle is closed, its place in threads[],
 * its handle and its thread-local region go to the next ones. Without it nothing of a thread is
 * ever freed, as before: a session that had made 1024 threads, running or not, stopped at the
 * next ("too many threads").
 *
 * A thread leaves the tables at once. Its KThread, Cpu and thread-local region, and its host
 * thread's handle, wait until its host thread has ended, which the next thread made or object
 * freed checks (WaitForSingleObject(host, 0)): exit_thread runs on the exiting thread itself,
 * which is still on its way out then. Windows signals a thread's handle once it has ended; the
 * POSIX shim once its start routine has returned, after which it touches nothing of ours (it
 * never signals the guest main thread's, a DuplicateHandle: that one is never freed there). The
 * only other holder of a thread's Cpu is the fiber made of the thread itself (hle_fiber.c), which
 * runs on that thread's own stack: nothing can resume it once the thread has ended.
 *
 * Not freed with a thread, outside this file: the Linux and macOS platform layer's 256 KiB signal
 * stack of each host thread (linux/win32.c, address space only until a signal runs on it), and
 * hle_fiber.c's slot of a thread made a fiber (16384 slots, "too many fibers"). */
static void reap(void) {
    for (KThread **p = &dead; *p;) {
        KThread *t = *p;
        if (t->host && WaitForSingleObject(t->host, 0) != WAIT_OBJECT_0) { p = &t->next_dead; continue; }
        *p = t->next_dead;
        if (t->host) CloseHandle(t->host);
        spare_tlrs[nspare_tlrs++] = t->tlr;   /* (committed, its memory-map region kept) */
        _aligned_free(t->cpu);
        free(t);
    }
}

static void drop_thread(KThread *t) {
    for (u32 i = 0; i < nthreads; i++)
        if (threads[i] == t) { threads[i] = threads[--nthreads]; break; }
    t->next_dead = dead;
    dead = t;
    reap();
}

/* ---- priorities and cores ------------------------------------------------------------
 * A thread's priority (0 highest to 63 lowest; an application's 28 to 59, which are nn::os's 0 to
 * 31 plus 28, its default 16 being 44 here) is kept as given and answered back, and its core as
 * given to CreateThread; SetThreadCoreMask is kept nowhere, GetThreadCoreMask's mask is 0x7 and
 * GetCurrentProcessorNumber 0, whatever the thread. The host is not told about cores either: host
 * threads run on any processor (in ssbu-recomp, whose SSBU_OS_CONSOLE_CORES pins three, that cost
 * frame rate on a busy PC).
 *
 * SSBU_CORE_MASKS=1 gives the console's answers instead, with its checks, as Atmosphere's
 * open-source kernel has them (mesosphere's kern_svc_thread.cpp and kern_k_thread.cpp): priorities
 * 28 to 59 and cores 0 to 2 only, a new thread's mask its core alone, -2 for the core the
 * process's (0, from the program's metadata), -1 for no ideal core and -3 to keep it in
 * SetThreadCoreMask, and GetCurrentProcessorNumber the thread's ideal core, or the first of its
 * mask without one - where the console's scheduler mostly keeps it, not where it is right now. Off
 * by default until a play session has been run with it: a value the console refuses was taken
 * before, and the game may keep data by processor number.
 *
 * With SSBU_HOST_PRIORITIES=1 the host thread's priority follows the guest's, in four bands: above
 * normal (28 to 43, nn::os's 0 to 15), normal (44), below normal (45 to 55) and lowest (56 to 59,
 * nn::os's 28 to 31; ssbu-recomp's bands, less its highest for 0 to 4) - never above the
 * renderer's and input's own threads (above normal) or audio's (time critical). The platform
 * layer (linux/win32.c) makes them quality-of-service classes on macOS (user-interactive, which
 * every thread starts in, for the top two; user-initiated; utility, which Apple silicon may run on
 * its efficiency cores) and nice values on Linux (0, 0, 2, 5: nothing real-time, nothing above
 * where threads start). macOS sets only the calling thread's class, so each thread sets its own:
 * at its start, and as it leaves a system call after its priority changed. Off by default: the
 * console lends a waiting thread's priority to the owner of the mutex it waits for and this does
 * not, so while every processor is busy a low thread holding what a high one waits for runs low;
 * and on Linux a thread lowered cannot be raised again without RLIMIT_NICE of 20 or more (or
 * root), so there the switch is left off without it. */
static int valid_priority(i32 priority) { return priority >= 0 && priority <= 63 && PRIORITIES >> priority & 1; }

static int host_band(i32 priority) {
    return priority < 44 ? THREAD_PRIORITY_ABOVE_NORMAL : priority == 44 ? THREAD_PRIORITY_NORMAL :
           priority < 56 ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_LOWEST;
}

/* (SSBU_HOST_PRIORITIES) On the thread itself: its host priority after its guest one. A thread
 * starts in the normal band (KThread.band 0). */
static void follow_priority(KThread *t) {
    int band = host_band(__atomic_load_n(&t->priority, __ATOMIC_RELAXED));
    if (band == t->band) return;
    t->band = band;
    SetThreadPriority(GetCurrentThread(), band);
}

#ifdef __linux__
static int nice_reversible(void) {
    struct rlimit r;
    return geteuid() == 0 || (getrlimit(RLIMIT_NICE, &r) == 0 && (r.rlim_cur == RLIM_INFINITY || r.rlim_cur >= 20));
}
#else
static int nice_reversible(void) { return 1; }
#endif

/* SetThreadCoreMask's arguments, checked in mesosphere's order; -2 becomes the process's core. */
static u32 check_core_mask(i32 *core, u64 *mask) {
    if (*core == -2) { *core = 0; *mask = 1; return R_OK; }   /* (the mask is not looked at) */
    if (*mask & ~CORES) return R_INVALID_CORE;
    if (!*mask) return R_INVALID_COMBINATION;
    if (*core >= 0 && *core <= 3) return *mask >> *core & 1 ? R_OK : R_INVALID_COMBINATION;
    return *core == -1 || *core == -3 ? R_OK : R_INVALID_CORE;
}

static i32 processor_of(i32 core, u64 mask) { return core >= 0 ? core : __builtin_ctzll(mask); }

/* ---- threads --------------------------------------------------------------------- */
static KThread *new_thread(u64 entry, u64 arg, u64 stack_top, i32 priority, i32 core) {
    KThread *t = calloc(1, sizeof *t);
    if (nthreads == sizeof threads / sizeof *threads) rt_fatal("kernel: too many threads");
    threads[nthreads++] = t;
    InitializeConditionVariable(&t->woken);
    t->entry = entry; t->arg = arg; t->stack_top = stack_top; t->priority = priority; t->core = core;
    t->mask = (u32)core < 3 ? 1ull << core : CORES;   /* (any core without SSBU_CORE_MASKS) */
    t->processor = (u32)core < 3 ? core : 0;
    t->id = next_thread_id++;
    if (recycle) reap();
    if (nspare_tlrs) {
        t->tlr = spare_tlrs[--nspare_tlrs];
        memset(G2H(t->tlr), 0, 0x1000);   /* (a new thread's starts zeroed) */
    } else {
        if (tlr_next == KERNEL_BASE + TLR_AREA) rt_fatal("kernel: out of thread-local regions");
        t->tlr = tlr_next;
        tlr_next += 0x1000;
        commit(t->tlr, 0x1000, "a thread-local region");
        kernel_add_region(t->tlr, 0x1000, GM_TLS, 3);
    }
    t->cpu = _aligned_malloc(sizeof *t->cpu, 64);
    memset(t->cpu, 0, sizeof *t->cpu);
    t->cpu->thread = t;
    t->cpu->tpidrro = t->tlr;
    t->cpu->sp = stack_top;
    t->cpu->x[0] = arg;
    t->handle = new_handle(OBJ_THREAD, t);
    return t;
}

#ifndef _WIN32
/* Linux and macOS: a thread leaves from wherever it is by jumping back to its start (ExitThread
 * would be pthread_exit, which unwinds every guest frame on the way out). (RT_SETJMP: recomp.h's,
 * which on macOS leaves the signal mask alone - nothing changes it on a guest thread.) */
static __thread jmp_buf *exit_point;
#endif

static _Noreturn void exit_thread(KThread *t) {
    EnterCriticalSection(&klock);
    t->exited = 1;
    objects[index_of(t->handle)].signaled = 1;
    notify(&objects[index_of(t->handle)]);
    if (recycle) release(index_of(t->handle));   /* (freed now if its handle was closed already) */
    LeaveCriticalSection(&klock);
#ifndef _WIN32
    if (exit_point) RT_LONGJMP(*exit_point, 1);
#endif
    ExitThread(0);
}

static DWORD WINAPI thread_main(LPVOID param) {
    KThread *t = param;
    char where[96];
    WCHAR desc[128];   /* for the profiler and debuggers */
    i32 priority = __atomic_load_n(&t->priority, __ATOMIC_RELAXED);
#ifdef _WIN32
    _snwprintf(desc, 127, L"guest thread %llu (%hs, priority %d)", t->id, rt_describe(t->entry, where, sizeof where), priority);
#else
    swprintf(desc, 127, L"guest thread %llu (%s, priority %d)", (unsigned long long)t->id, rt_describe(t->entry, where, sizeof where), priority);
#endif
    desc[127] = 0;
    SetThreadDescription(GetCurrentThread(), desc);
    if (host_priorities) follow_priority(t);
#ifndef _WIN32
    jmp_buf out;
    exit_point = &out;
    if (RT_SETJMP(out)) return 0;
#endif
    rt_call(t->cpu, t->entry);
    exit_thread(t);
}

Cpu *kernel_main_thread(u32 *handle) {
    u64 top = STACK_BASE + STACK_SIZE - 0x10000;
    commit(top - MAIN_STACK, MAIN_STACK, "the main stack");
    EnterCriticalSection(&klock);
    kernel_add_region(top - MAIN_STACK, MAIN_STACK, GM_STACK, 3);
    KThread *t = new_thread(0, 0, top, 44, 0);
    t->started = 1;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &t->host, 0, FALSE, DUPLICATE_SAME_ACCESS);
    LeaveCriticalSection(&klock);
    if (host_priorities) follow_priority(t);
    *handle = t->handle;
    return t->cpu;
}

/* ---- userspace mutexes (svcArbitrateLock / Unlock) --------------------------------
 * The mutex word holds the owner's thread handle, plus WAITERS when the kernel has queued
 * threads on it. Unlocking hands the mutex directly to the longest-waiting thread. */
static void mutex_release(u64 addr) {
    u32 count;
    KThread *next = first_waiting(WAIT_MUTEX, addr, &count);
    if (!next) { st32(addr, 0); return; }
    unqueue(next);   /* this is what the waiter is watching for */
    st32(addr, next->mutex_tag | (count > 1 ? WAITERS : 0));
    kwake(next);
}

/* Give `t` the mutex if it is free (1), otherwise queue it behind the current owner (0). */
static int mutex_acquire_or_queue(KThread *t, u64 addr, u32 tag) {
    for (;;) {
        u32 v = ld32(addr);
        if (v == 0) {
            if (cas32(addr, 0, tag)) return 1;
        } else if (cas32(addr, v, v | WAITERS)) {
            t->mutex_tag = tag;
            enqueue(t, WAIT_MUTEX, addr);
            return 0;
        }
    }
}

/* ---- the system calls ------------------------------------------------------------ */
static u32 get_info(Cpu *c, u32 id, u64 sub, u64 *out) {
    (void)c;
    u64 image = 0;
    for (u32 i = 0; i < region_count; i++)
        if (regions[i].state == GM_CODE || regions[i].state == GM_CODE_DATA) image += regions[i].size;
    switch (id) {
    case 0: *out = 0x7; break;                          /* cores an application may use */
    case 1: *out = 0x0FFFFFFFF0000000ull; break;        /* priorities 28..59 */
    case 2: *out = ALIAS_BASE; break;
    case 3: *out = ALIAS_SIZE; break;
    case 4: *out = HEAP_BASE; break;
    case 5: *out = HEAP_LIMIT; break;
    case 6: case 21: *out = 0xCD500000ull; break;       /* memory an application gets on the console */
    case 7: case 22: *out = image + heap_size; break;
    case 8: *out = 0; break;                            /* no debugger */
    case 11: {   /* entropy; with SSBU_FIXED_SEED a function of the seed (docs/TESTING-RUNS.md) */
        static int fixed = -1;
        static u64 seed;
        if (fixed < 0) fixed = fixed_seed_parse(getenv("SSBU_FIXED_SEED"), &seed);
        *out = fixed ? fixed_entropy(seed, sub) : 0x9E3779B97F4A7C15ull * (sub + 1) ^ now_ns();
        break;
    }
    case 12: *out = ASPACE_BASE; break;
    case 13: *out = ASPACE_SIZE; break;
    case 14: *out = STACK_BASE; break;
    case 15: *out = STACK_SIZE; break;
    case 16: *out = 0x1000000; break;                   /* system resource size, from the program's metadata */
    case 17: *out = 0; break;
    case 18: *out = 0x01006A800016E000ull; break;       /* program id */
    case 20: *out = 0; break;
    case 23: *out = 1; break;                           /* is an application */
    default:
        fprintf(stderr, "[kernel] svcGetInfo: unknown id %u (sub %llu)\n", id, sub);
        return R_INVALID_ENUM;
    }
    return R_OK;
}

/* An object signalled ends the call (the first in the handles), otherwise a cancel, otherwise a
 * timeout of 0; else the thread waits in each object's queue until a signal of one of them
 * (notify), a cancel or the deadline wakes it, and looks again. */
static u32 wait_objects(Cpu *c, u64 handles, u32 count, i64 timeout, u32 *index) {
    KThread *me = c->thread;
    u64 deadline = deadline_of(timeout);
    KWaitNode local[64], *nodes = count <= 64 ? local : malloc(count * sizeof *nodes);
    if (!nodes) rt_fatal("kernel: svcWaitSynchronization on %u handles", count);
    u32 r = ~0u;
    while (r == ~0u) {
        for (u32 i = 0; i < count && r == ~0u; i++) {
            u32 h = ld32(handles + 4 * i), k = recycle ? index_of(h) : slot_of(h);   /* (recycling: held, closed or not) */
            if (!k) r = R_INVALID_HANDLE;
            else if (objects[k].signaled) { *index = i; r = R_OK; }
            else nodes[i] = (KWaitNode){me, &objects[k], NULL, NULL, i};
        }
        if (r != ~0u) break;
        if (me->cancelled) { me->cancelled = 0; r = R_CANCELLED; break; }
        if (timeout == 0) { r = R_TIMEOUT; break; }
        for (u32 i = 0; i < count; i++) {   /* (at the end of each queue: first come first) */
            KObject *o = nodes[i].object;
            nodes[i].prev = o->last;
            if (o->last) o->last->next = &nodes[i]; else o->first = &nodes[i];
            o->last = &nodes[i];
        }
        me->wait = WAIT_OBJECTS;
        me->nodes = nodes;
        me->wait_count = count;
        int more = kwait(me, deadline);
        if (me->wait == WAIT_OBJECTS) unqueue_objects(me);   /* (woken by the deadline, or by the host for nothing) */
        if (!more) r = R_TIMEOUT;
    }
    if (nodes != local) free(nodes);
    return r;
}

/* With recycling the wait holds its objects, as the console's does: one whose handle another
 * thread closes meanwhile is still waited on (an exiting thread whose handle was closed is still
 * seen signalled), and is freed when the wait ends. (The handle array is the waiting thread's
 * own: it does not change under the wait.) */
static u32 wait_synchronization(Cpu *c, u64 handles, u32 count, i64 timeout, u32 *index) {
    if (!recycle) return wait_objects(c, handles, count, timeout, index);
    for (u32 i = 0; i < count; i++)
        if (!slot_of(ld32(handles + 4 * i))) return R_INVALID_HANDLE;
    for (u32 i = 0; i < count; i++) objects[index_of(ld32(handles + 4 * i))].waits++;
    u32 r = wait_objects(c, handles, count, timeout, index);
    for (u32 i = 0; i < count; i++) {
        u32 k = index_of(ld32(handles + 4 * i));
        objects[k].waits--;
        release(k);
    }
    return r;
}

static u32 wait_for_address(Cpu *c, u64 addr, u32 type, i32 value, i64 timeout) {
    KThread *me = c->thread;
    i32 v = (i32)ld32(addr);
    if (type == 0 || type == 1) { if (!(v < value)) return R_INVALID_STATE; }
    else if (type == 2) { if (v != value) return R_INVALID_STATE; }
    else return R_INVALID_ENUM;
    if (type == 1) st32(addr, (u32)(v - 1));
    if (timeout == 0) return R_TIMEOUT;
    u64 deadline = deadline_of(timeout);
    enqueue(me, WAIT_ADDRESS, addr);
    while (me->wait == WAIT_ADDRESS)
        if (!kwait(me, deadline)) { unqueue(me); return R_TIMEOUT; }
    return R_OK;
}

static u32 signal_to_address(u64 addr, u32 type, i32 value, i32 count) {
    u32 waiting;
    KThread *t = first_waiting(WAIT_ADDRESS, addr, &waiting);
    if (type == 1) {
        if ((i32)ld32(addr) != value) return R_INVALID_STATE;
        st32(addr, (u32)(value + 1));
    } else if (type == 2) {
        if ((i32)ld32(addr) != value) return R_INVALID_STATE;
        i32 next = value + 1;
        if (waiting) next = count <= 0 ? value - 2 : ((i32)waiting <= count ? value - 1 : value);
        st32(addr, (u32)next);
    } else if (type != 0) return R_INVALID_ENUM;
    for (i32 n = 0; t && (count <= 0 || n < count); n++) {
        unqueue(t);
        kwake(t);
        t = first_waiting(WAIT_ADDRESS, addr, &waiting);
    }
    return R_OK;
}

static u32 wait_condition(Cpu *c, u64 mutex, u64 key, u32 tag, i64 timeout) {
    KThread *me = c->thread;
    u64 deadline = deadline_of(timeout);
    u32 others;
    mutex_release(mutex);
    st32(key, 1);
    me->mutex_tag = tag;
    me->cv_mutex = mutex;
    enqueue(me, WAIT_CONDITION, key);
    while (me->wait == WAIT_CONDITION)
        if (!kwait(me, deadline)) {
            unqueue(me);
            first_waiting(WAIT_CONDITION, key, &others);
            if (!others) st32(key, 0);
            return R_TIMEOUT;                       /* the caller re-locks the mutex itself */
        }
    while (me->wait == WAIT_MUTEX) kwait(me, ~0ull);   /* the signaller queued us on the mutex */
    return R_OK;
}

static void signal_condition(u64 key, i32 count) {
    u32 waiting;
    KThread *t = first_waiting(WAIT_CONDITION, key, &waiting);
    for (i32 n = 0; t && (count <= 0 || n < count); n++) {
        unqueue(t);
        /* the woken thread must own the mutex again before it returns: queued on it instead, it
         * is woken when the mutex is handed to it */
        if (mutex_acquire_or_queue(t, t->cv_mutex, t->mutex_tag)) kwake(t);
        t = first_waiting(WAIT_CONDITION, key, &waiting);
    }
    if (!t) st32(key, 0);
}

static void system_call(Cpu *c, u32 n) {
    KThread *me = c->thread;
    KObject *o;
    u64 *x = c->x;
    if (rt_trace) fprintf(stderr, "[svc %#04x] thread %llu x0=%#llx x1=%#llx x2=%#llx x3=%#llx\n", n, me->id, x[0], x[1], x[2], x[3]);

    void skyline_note_sleep(void);
    /* calls that never touch shared kernel state */
    switch (n) {
    case 0x0B:   /* SleepThread(ns): non-positive values are yield requests */
        if ((i64)x[0] > 0) skyline_note_sleep();   /* (hle_skyline.c: ARCropolis' event thread going round) */
        if ((i64)x[0] > 0) sleep_ns(x[0]); else SwitchToThread();
        return;
    case 0x10:                                           /* GetCurrentProcessorNumber (see "priorities and cores") */
        x[0] = core_masks ? (u32)__atomic_load_n(&me->processor, __ATOMIC_RELAXED) : 0;
        return;
    case 0x1E: x[0] = rt_ticks(); return;                /* GetSystemTick */
    case 0x27:                                           /* OutputDebugString(ptr, len) */
        fprintf(stderr, "[guest] %.*s%s", (int)x[1], P(const char, x[0]), x[1] && P(const char, x[0])[x[1] - 1] == '\n' ? "" : "\n");
        x[0] = R_OK;
        return;
    case 0x26:                                           /* Break(reason, address, size) */
        if (x[2] == 4 && x[1])   /* the info is a failed result */
            rt_fatal("the guest aborted on result %#x (module %u, description %u)", ld32(x[1]), ld32(x[1]) & 0x1FF, (ld32(x[1]) >> 9) & 0x1FFF);
        rt_fatal("the guest called svcBreak (reason %#llx, info %#llx, size %#llx)", x[0], x[1], x[2]);
#ifdef __APPLE__   /* (not exit(): atexit handlers and C++ destructors under the threads still running - Metal's, AppKit's - can crash on the way out) */
    case 0x07: fprintf(stderr, "guest process exited\n"); rt_upnp_remove(); ExitProcess(0);
#else
    case 0x07: fprintf(stderr, "guest process exited\n"); exit(0);
#endif
    case 0x0A: exit_thread(me);
    case 0x1F: {   /* ConnectToNamedPort(name) -> session handle */
        void *s = ipc_connect(P(const char, x[1]));
        if (!s) rt_fatal("svcConnectToNamedPort(\"%s\"): no such port here", P(const char, x[1]));
        x[0] = R_OK; x[1] = kernel_new_session(s);
        return;
    }
    case 0x21: case 0x22: {   /* SendSyncRequest(handle) / ...WithUserBuffer(buffer, size, handle) */
        u32 h = (u32)(n == 0x21 ? x[0] : x[2]);
        void *s = kernel_session_of(h);
        if (!s) { x[0] = R_INVALID_HANDLE; return; }
        x[0] = ipc_request(c, s, n == 0x21 ? c->tpidrro : x[0], n == 0x21 ? 0x100 : x[1]);
        return;
    }
    }

    EnterCriticalSection(&klock);
    switch (n) {
    case 0x01: {   /* SetHeapSize(size) -> address */
        u64 size = x[1];
        if (size > HEAP_LIMIT) { x[0] = R_OUT_OF_MEMORY; break; }
        if (size > heap_size) commit(HEAP_BASE + heap_size, size - heap_size, "the heap");
        if (heap_size) unmap_range(HEAP_BASE, heap_size);
        heap_size = size;
        if (size) kernel_add_region(HEAP_BASE, size, GM_HEAP, 3);
        x[0] = R_OK; x[1] = HEAP_BASE;
        break;
    }
    case 0x02: case 0x03: x[0] = R_OK; break;            /* SetMemoryPermission / SetMemoryAttribute */
    case 0x04: {   /* MapMemory(dst, src, size): the pages appear at dst; here, as a copy (see "memory mappings") */
        u32 i = 0;
        while (i < 1024 && maps[i].size) i++;
        if (i == 1024) rt_fatal("kernel: too many memory mappings");
        commit(x[0], x[2], "a memory mapping");
        u64 t0 = now_ns();
        memcpy(G2H(x[0]), G2H(x[1]), x[2]);
        map_copy_ns += now_ns() - t0;
        maps[i].dst = x[0]; maps[i].src = x[1]; maps[i].size = x[2];
        if (map_guard) guard_source(i);
        kernel_add_region(x[0], x[2], GM_STACK, 3);
        maps_made++;
        map_bytes += x[2];
        if (++maps_live > maps_peak) maps_peak = maps_live;
        x[0] = R_OK;
        break;
    }
    case 0x05: {   /* UnmapMemory(dst, src, size) */
        for (u32 i = 0; i < 1024; i++)
            if (maps[i].size && maps[i].dst == x[0]) {
                unguard_source(i);
                u64 t0 = now_ns();
                memcpy(G2H(maps[i].src), G2H(maps[i].dst), maps[i].size);
                map_copy_ns += now_ns() - t0;
                gpu_memory_unmapping(maps[i].dst, maps[i].size);   /* (queued GPU work and the renderer may read it) */
                watch_decommitting(maps[i].dst, maps[i].size);
                VirtualFree(G2H(maps[i].dst), maps[i].size, MEM_DECOMMIT);
                unmap_range(maps[i].dst, maps[i].size);
                maps[i].size = 0;
                maps_live--;
            }
        x[0] = R_OK;
        break;
    }
    case 0x06: query_memory(x[0], x[2]); x[0] = R_OK; x[1] = 0; break;
    case 0x08: {   /* CreateThread(entry, arg, stack top, priority, core) -> handle */
        i32 priority = (i32)x[4], core = (i32)x[5];
        if (core_masks) {
            if (core == -2) core = 0;   /* the process's core */
            if ((u32)core > 2) { x[0] = R_INVALID_CORE; break; }
            if (!valid_priority(priority)) { x[0] = R_INVALID_PRIORITY; break; }
        }
        KThread *t = new_thread(x[1], x[2], x[3], priority, core);
        x[0] = R_OK; x[1] = t->handle;
        break;
    }
    case 0x09:     /* StartThread(handle) */
        if (!(o = object_of(c, (u32)x[0], OBJ_THREAD))) { x[0] = R_INVALID_HANDLE; break; }
        o->thread->started = 1;
        o->thread->host = CreateThread(NULL, HOST_STACK, thread_main, o->thread, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        if (!o->thread->host) rt_fatal("kernel: cannot start a host thread (error %lu)", (unsigned long)GetLastError());
        x[0] = R_OK;
        break;
    case 0x0C:     /* GetThreadPriority(handle) -> priority */
        if (!(o = object_of(c, (u32)x[1], OBJ_THREAD))) { x[0] = R_INVALID_HANDLE; break; }
        x[0] = R_OK; x[1] = (u32)o->thread->priority;
        break;
    case 0x0D:     /* SetThreadPriority(handle, priority): the host's follows as the thread leaves its system call */
        if (core_masks && !valid_priority((i32)x[1])) { x[0] = R_INVALID_PRIORITY; break; }
        if (!(o = object_of(c, (u32)x[0], OBJ_THREAD))) { x[0] = R_INVALID_HANDLE; break; }
        __atomic_store_n(&o->thread->priority, (i32)x[1], __ATOMIC_RELAXED);
        requeue(o->thread);
        x[0] = R_OK;
        break;
    case 0x0E:     /* GetThreadCoreMask(handle) -> ideal core, mask */
        if (!(o = object_of(c, (u32)x[2], OBJ_THREAD))) { x[0] = R_INVALID_HANDLE; break; }
        x[0] = R_OK; x[1] = (u32)o->thread->core; x[2] = core_masks ? o->thread->mask : CORES;
        break;
    case 0x0F: {   /* SetThreadCoreMask(handle, ideal core, mask): with SSBU_CORE_MASKS kept, never given to the host thread (see "priorities and cores") */
        i32 core = (i32)x[1];
        u64 mask = x[2];
        u32 r = core_masks ? check_core_mask(&core, &mask) : R_OK;
        if (r) { x[0] = r; break; }
        if (!(o = object_of(c, (u32)x[0], OBJ_THREAD))) { x[0] = recycle ? R_INVALID_HANDLE : R_OK; break; }   /* (without recycling any handle, as before) */
        if (!core_masks) { x[0] = R_OK; break; }
        KThread *t = o->thread;
        if (core == -3) {   /* (a kept ideal core must be in the new mask) */
            if (t->core >= 0 && !(mask >> t->core & 1)) { x[0] = R_INVALID_COMBINATION; break; }
            core = t->core;
        }
        t->core = core;
        t->mask = mask;
        __atomic_store_n(&t->processor, processor_of(core, mask), __ATOMIC_RELAXED);
        x[0] = R_OK;
        break;
    }
    case 0x11:     /* SignalEvent(handle) */
        if (!(o = object_of(c, (u32)x[0], OBJ_EVENT))) { x[0] = R_INVALID_HANDLE; break; }
        o->signaled = 1;
        notify(o);
        x[0] = R_OK;
        break;
    case 0x12:     /* ClearEvent(handle) */
        if (!(o = object_of(c, (u32)x[0], OBJ_EVENT))) { x[0] = R_INVALID_HANDLE; break; }
        o->signaled = 0;
        x[0] = R_OK;
        break;
    case 0x16: {   /* CloseHandle(handle) */
        if (recycle) { x[0] = close_handle((u32)x[0]); break; }
        u32 k = (u32)x[0] - 0xA000;
        if (k < 4096 && objects[k].type == OBJ_SESSION) { ipc_close(objects[k].session); objects[k].type = OBJ_FREE; }
        if (k < 4096 && (objects[k].type == OBJ_EVENT || objects[k].type == OBJ_MEMORY)) objects[k].type = OBJ_FREE;
        x[0] = R_OK;   /* thread objects stay: their thread may still be running */
        break;
    }
    case 0x17: {   /* ResetSignal(handle) */
        u32 k = slot_of((u32)x[0]);
        if (!k) { x[0] = R_INVALID_HANDLE; break; }
        x[0] = objects[k].signaled && objects[k].type == OBJ_EVENT ? R_OK : R_INVALID_STATE;
        if (objects[k].type == OBJ_EVENT) objects[k].signaled = 0;
        break;
    }
    case 0x18: {   /* WaitSynchronization(handles, count, timeout) -> index */
        u32 index = 0;
        x[0] = wait_synchronization(c, x[1], (u32)x[2], (i64)x[3], &index);
        x[1] = index;
        break;
    }
    case 0x19:     /* CancelSynchronization(thread): its WaitSynchronization ends, now or the next */
        if (!(o = object_of(c, (u32)x[0], OBJ_THREAD))) { x[0] = R_INVALID_HANDLE; break; }
        o->thread->cancelled = 1;
        if (o->thread->wait == WAIT_OBJECTS) {   /* (only that wait: a mutex, a condition variable, an address wait on) */
            unqueue_objects(o->thread);
            kwake(o->thread);
        }
        x[0] = R_OK;
        break;
    case 0x1A:     /* ArbitrateLock(owner handle, mutex address, own handle) */
        if (ld32(x[1]) == ((u32)x[0] | WAITERS)) {
            me->mutex_tag = (u32)x[2];
            enqueue(me, WAIT_MUTEX, x[1]);
            while (me->wait == WAIT_MUTEX) kwait(me, ~0ull);
        }
        x[0] = R_OK;
        break;
    case 0x1B: mutex_release(x[0]); x[0] = R_OK; break;  /* ArbitrateUnlock(mutex address) */
    case 0x1C: x[0] = wait_condition(c, x[0], x[1], (u32)x[2], (i64)x[3]); break;
    case 0x1D: signal_condition(x[0], (i32)x[1]); x[0] = R_OK; break;
    case 0x25:     /* GetThreadId(handle) -> id */
        if (!(o = object_of(c, (u32)x[1], OBJ_THREAD))) { x[0] = R_INVALID_HANDLE; break; }
        x[0] = R_OK; x[1] = o->thread->id;
        break;
    case 0x24: x[0] = R_OK; x[1] = 0x51; break;          /* GetProcessId */
    case 0x29: {   /* GetInfo(id, handle, sub id) -> value */
        u64 value = 0;
        x[0] = get_info(c, (u32)x[1], x[3], &value);
        x[1] = value;
        break;
    }
    case 0x2C:     /* MapPhysicalMemory(address, size): back part of the alias area with memory */
        commit(x[0], x[1], "physical memory");
        kernel_add_region(x[0], x[1], GM_HEAP, 3);
        x[0] = R_OK;
        break;
    case 0x2D:     /* UnmapPhysicalMemory(address, size) */
        gpu_memory_unmapping(x[0], x[1]);   /* (queued GPU work and the renderer may read it) */
        watch_decommitting(x[0], x[1]);
        VirtualFree(G2H(x[0]), x[1], MEM_DECOMMIT);
        unmap_range(x[0], x[1]);
        x[0] = R_OK;
        break;
    case 0x34: x[0] = wait_for_address(c, x[0], (u32)x[1], (i32)x[2], (i64)x[3]); break;
    case 0x35: x[0] = signal_to_address(x[0], (u32)x[1], (i32)x[2], (i32)x[3]); break;
    case 0x15: {   /* CreateTransferMemory(address, size, permission) -> handle: lent memory stays where it is */
        x[1] = new_handle(OBJ_MEMORY, NULL);
        x[0] = R_OK;
        break;
    }
    case 0x45: {   /* CreateEvent() -> writable handle, readable handle (one object here) */
        u32 h = new_handle(OBJ_EVENT, NULL);
        objects[index_of(h)].refs = 2;
        x[0] = R_OK; x[1] = h; x[2] = h;
        break;
    }
    default:
        LeaveCriticalSection(&klock);
        rt_fatal("system call %#x is not implemented by the native kernel", n);
    }
    LeaveCriticalSection(&klock);
    if (rt_trace) fprintf(stderr, "[svc %#04x] -> %#llx (x1=%#llx)\n", n, x[0], x[1]);
}

void rt_svc(Cpu *c, u32 n) {
    system_call(c, n);
    if (host_priorities) follow_priority(c->thread);   /* (a priority changed during the call, its own or by another thread) */
}

/* For diagnosis: where is every guest thread right now? */
DWORD kernel_lock_owner(void) { return (DWORD)(uintptr_t)klock.OwningThread; }

static const char *wait_state(const KThread *t) {
    return t->wait == WAIT_MUTEX ? "waiting for a mutex" : t->wait == WAIT_CONDITION ? "waiting on a condition" :
           t->wait == WAIT_ADDRESS ? "waiting on an address" : "running or in a system call";
}

/* For a freeze report: every guest thread's state and host thread id (its stack is in the host
 * threads' part of the report). Without waiting for the kernel's lock: says so when it is held. */
void kernel_thread_states(void (*line)(const char *text)) {
    char text[200];
    if (!TryEnterCriticalSection(&klock)) {
        snprintf(text, sizeof text, "  (the kernel's lock is held by thread %lu: guest thread states not read)\n", (unsigned long)kernel_lock_owner());
        line(text);
        return;
    }
    for (u32 i = 1; i < 4096; i++) {
        KThread *t = objects[i].type == OBJ_THREAD ? objects[i].thread : NULL;
        if (!t || !t->started || t->exited || !t->host) continue;
        snprintf(text, sizeof text, "  guest thread %llu (host thread %lu, priority %d): %s\n", t->id, (unsigned long)GetThreadId(t->host), t->priority, wait_state(t));
        line(text);
    }
    LeaveCriticalSection(&klock);
}

void kernel_dump_threads(void) {
    EnterCriticalSection(&klock);
    for (u32 i = 1; i < 4096; i++) {
        KThread *t = objects[i].type == OBJ_THREAD ? objects[i].thread : NULL;
        if (!t || !t->started || t->exited || !t->host) continue;
        fprintf(stderr, "  thread %llu (priority %d): %s\n", t->id, t->priority, wait_state(t));
        rt_backtrace_thread(t->host, 6);
    }
    LeaveCriticalSection(&klock);
}

u32 kernel_new_session(void *session) {
    EnterCriticalSection(&klock);
    u32 h = new_handle(OBJ_SESSION, NULL);
    objects[index_of(h)].session = session;
    LeaveCriticalSection(&klock);
    return h;
}

void *kernel_session_of(u32 handle) {
    u32 k = slot_of(handle);
    return k && objects[k].type == OBJ_SESSION ? objects[k].session : NULL;
}

/* A handle native code gives the guest once more (an IPC copy handle, ipc.c): on the console a
 * handle of its own, so one more CloseHandle to come. */
void kernel_copy_handle(u32 handle) {
    EnterCriticalSection(&klock);
    u32 k = slot_of(handle);
    if (k) objects[k].refs++;
    LeaveCriticalSection(&klock);
}

u32 kernel_create_event(void) {
    EnterCriticalSection(&klock);
    u32 h = new_handle(OBJ_EVENT, NULL);
    LeaveCriticalSection(&klock);
    return h;
}

/* (an event the guest has closed: nothing, as one it was handed and closed - rt_attach_system_event's
 * - names nothing any more) */
void kernel_signal_event(u32 handle) {
    EnterCriticalSection(&klock);
    u32 k = slot_of(handle);
    if (k && objects[k].type == OBJ_EVENT) { objects[k].signaled = 1; notify(&objects[k]); }
    LeaveCriticalSection(&klock);
}

u32 rt_attach_system_event(Cpu *c, u64 system_event, int auto_clear) {
    u32 h = kernel_create_event();
    /* nn::os::AttachReadableHandleToSystemEvent(event, handle, manage the handle, clear mode) */
    rt_callsym(c, "_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE", 4,
               system_event, (u64)h, (u64)1, (u64)(auto_clear ? 1 : 0));
    return h;
}

void kernel_init(void) {
    InitializeCriticalSection(&klock);
    recycle = getenv("SSBU_THREAD_RECYCLING") && !strcmp(getenv("SSBU_THREAD_RECYCLING"), "1");
    if (recycle) fprintf(stderr, "[kernel] exited threads and their handles recycled (SSBU_THREAD_RECYCLING=1)\n");
    rt_clock_ns();   /* (the clock set up - and its line logged - before the game asks it) */
    /* Reserve the areas the guest maps into; pages are committed as they are used. The alias
     * area, where the game keeps its GPU memory, has its writes watched so that the renderer
     * learns which textures the CPU changed (GetWriteWatch). With SSBU_ROLLBACK_RING every area
     * has, for the ring too (see "the write watch, shared"). */
    static const struct { u64 base, size; const char *what; int renderer; } areas[] = {
        {HEAP_BASE, HEAP_LIMIT, "heap", 0}, {ALIAS_BASE, ALIAS_SIZE, "alias", 1}, {STACK_BASE, STACK_SIZE, "stack", 0},
        {KERNEL_BASE, TLR_AREA, "thread-local", 0},
    };
    int ring = kernel_ring_frames();
    kernel_watch_shared = ring > 0;
    for (u32 i = 0; i < sizeof areas / sizeof *areas; i++) {
        DWORD watch = areas[i].renderer || ring ? MEM_WRITE_WATCH : 0;
        if (VirtualAlloc(G2H(areas[i].base), areas[i].size, MEM_RESERVE | watch, PAGE_READWRITE) != G2H(areas[i].base))
            rt_fatal("kernel: cannot reserve the %s area at %#llx", areas[i].what, areas[i].base);
        if (ring) watch_add(areas[i].base, areas[i].size, (areas[i].renderer ? 1u << WATCH_RENDERER : 0) | 1u << WATCH_RING);
    }
    if (ring)
        fprintf(stderr, "[kernel] the heap, stack and thread-local areas' writes watched too, and the write watch shared by the renderer and the rollback ring (SSBU_ROLLBACK_RING)\n");
    map_guard = getenv("SSBU_MAP_GUARD") && !strcmp(getenv("SSBU_MAP_GUARD"), "1");
    if (map_guard && ring) {   /* (the ring reads every page the memory map has; and the watch protects the heap's pages itself, on macOS and older Linux) */
        map_guard = 0;
        fprintf(stderr, "[kernel] SSBU_MAP_GUARD=1 left off: the rollback ring reads every mapped page (SSBU_ROLLBACK_RING)\n");
    }
    if (map_guard) fprintf(stderr, "[kernel] a memory mapping's source is inaccessible while it is mapped (SSBU_MAP_GUARD=1)\n");
    priority_queues = getenv("SSBU_PRIORITY_QUEUES") && !strcmp(getenv("SSBU_PRIORITY_QUEUES"), "1");
    if (priority_queues) fprintf(stderr, "[kernel] mutex, condition variable and address waiters queued by priority, then arrival (SSBU_PRIORITY_QUEUES=1)\n");
    core_masks = getenv("SSBU_CORE_MASKS") && !strcmp(getenv("SSBU_CORE_MASKS"), "1");
    if (core_masks) fprintf(stderr, "[kernel] thread priorities and core masks checked and kept as on the console (SSBU_CORE_MASKS=1)\n");
    host_priorities = getenv("SSBU_HOST_PRIORITIES") && !strcmp(getenv("SSBU_HOST_PRIORITIES"), "1");
    if (host_priorities && !nice_reversible()) {
        host_priorities = 0;
        fprintf(stderr, "[kernel] SSBU_HOST_PRIORITIES=1 left off: a thread lowered could not be raised again (RLIMIT_NICE below 20)\n");
    }
    if (host_priorities) fprintf(stderr, "[kernel] host thread priorities follow the guest's, in four bands (SSBU_HOST_PRIORITIES=1)\n");
}
