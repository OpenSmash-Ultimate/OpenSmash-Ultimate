/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Readable memory past the top of the stacks of the threads that make Vulkan pipelines (gpu_vk.c's makers, warm
 * threads and preparation), for NVIDIA's shader compiler on GeForce RTX 50 cards (nvgpucomp64.dll, called by
 * nvoglv64.dll), which reads PAST THE TOP of the stack of the thread that asked for the pipeline.
 *
 * What the reports show. 0.5.3 (no landing): every such crash read the stack top + 0 to 8 ("not a guest address"),
 * with the stack pointer 16 to 18 KB below that top: few frames above the driver's, ours. 0.5.4 (#629: 64 KB of zeros
 * past the top, SSBU_VK_STACK_LANDING=1): the one report read the top + 0x10004, just past the landing - so the driver
 * did not stop at the zeros, it went on reading upward through all 64 KB of them. rcx was -1 in every report, rbx too
 * (as 32 bits) in 0.5.4's. The driver's code was not looked at; why it reads up there, and what makes it stop, is not
 * known. A bigger stack from creation alone would not change it: the makers' and warm threads' stacks are already
 * 64 MB reservations, and the read is past the top (where the thread started), not past the bottom.
 *
 * SSBU_VK_STACK_LANDING:
 *   0  nothing (before #629);
 *   1  #629: 64 KB of zeros of its own past the top where that address space is free, else, where the bottom of another
 *      of these threads' stacks is there, that one page committed read-only;
 *   2  (default) a landing much larger (SSBU_VK_STACK_LANDING_MB, default 16, 1..256) reserved past the top, its first
 *      64 KB committed and filled when the thread starts, the rest committed as it is read (a vectored handler, the
 *      first one: a read there commits the 64 KB around it, filled the same, and the driver goes on). Filled with
 *      SSBU_VK_STACK_LANDING_FILL: "ones" (default: 0xff bytes, all-ones words - an end marker in much code, -1 as the
 *      registers showed, and no valid pointer: not canonical), "zero" (as #629), or a byte in hex ("5a"). The
 *      compute preparation's thread gets a 64 MB stack like the others. What is not free past the top (another
 *      stack's bottom) is as mode 1.
 * With 1 and 2 a crash or freeze report says where the driver's read was (the stack top + offset, which thread) and
 * how far the landings were read (vk_landing_report); a status line says it when a landing grew. This is not
 * reproduced here (no RTX 50 card): tests/test_vk_landing.py checks the mechanism with a made-up reader. */
#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK 0x10000ull   /* (64 KB: the allocation granularity; the first part committed, and each growth) */

static int mode_v = -1, mb_v, fill_v;
static void settings(void) {
    if (mode_v >= 0) return;
    const char *e = getenv("SSBU_VK_STACK_LANDING");
    int m = 2;
    if (e && *e) m = *e == '0' ? 0 : *e == '1' && !e[1] ? 1 : 2;
    const char *s = getenv("SSBU_VK_STACK_LANDING_MB");
    int mb = s && *s ? atoi(s) : 16;
    mb_v = mb < 1 ? 1 : mb > 256 ? 256 : mb;
    const char *f = getenv("SSBU_VK_STACK_LANDING_FILL");
    int fill = 0xff;
    if (f && *f) {
        if (!_stricmp(f, "zero") || !strcmp(f, "0")) fill = 0;
        else if (_stricmp(f, "ones")) {
            char *end;
            long v = strtol(f, &end, 16);
            if (!*end && v >= 0 && v <= 0xff) fill = (int)v;
        }
    }
    fill_v = fill;
    mode_v = m;
}
int vk_landing_mode(void) { settings(); return mode_v; }

/* the landings: a stack top, how far past it is committed, how far it may grow. A thread whose stack is put where an
 * ended one's was (the same top) takes that one's landing on. */
typedef struct Landing { uint8_t *top, *filled, *end; } Landing;
static Landing landings[256];
static volatile LONG nlandings;
static _Thread_local Landing *this_landing;   /* (this thread's, a compiling thread's) */
static void *volatile stack_bases[256];   /* (the allocation bases of these threads' stacks: mode 1's shared page) */
static volatile LONG nstack_bases;
static volatile LONG64 n_own, n_shared, n_none, n_grown, farthest;   /* (farthest: the highest offset past a top read) */
static volatile LONG64 fault_offset;   /* (the read past a top the landing did not take: the crash's, most likely) */
static volatile DWORD fault_tid;
static volatile LONG fault_seen;
static PVOID handler;

static int fill_part(uint8_t *at, SIZE_T n) {
    if (!VirtualAlloc(at, n, MEM_COMMIT, PAGE_READWRITE)) return 0;
    memset(at, fill_v, n);
    DWORD old;
    VirtualProtect(at, n, PAGE_READONLY, &old);
    return 1;
}

static void note_farthest(LONG64 off) {
    LONG64 was = farthest;
    while (off > was) {
        LONG64 got = InterlockedCompareExchange64(&farthest, off, was);
        if (got == was) break;
        was = got;
    }
}

/* First of the vectored handlers: a read past this thread's stack top, in its landing's reserved part, commits the
 * landing up to the 64 KB around it and the read is done again. Any other fault within reach of a top is noted for
 * the report and left to the other handlers (runtime/host.c's report). Only this thread's own landing is looked at:
 * a thread that is not a compiling one returns at once (the rollback's write tracking faults often). */
static LONG CALLBACK landing_fault(EXCEPTION_POINTERS *e) {
    const EXCEPTION_RECORD *r = e->ExceptionRecord;
    Landing *l = this_landing;
    if (!l || r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    uint8_t *at = (uint8_t *)r->ExceptionInformation[1];
    if (at < l->top || at >= l->top + (512ull << 20)) return EXCEPTION_CONTINUE_SEARCH;
    if (r->ExceptionInformation[0] == 0 && at >= l->filled && at < l->end) {
        uint8_t *to = (uint8_t *)(((uintptr_t)at & ~(uintptr_t)(CHUNK - 1)) + CHUNK);
        if (to > l->end) to = l->end;
        if (fill_part(l->filled, (SIZE_T)(to - l->filled))) {
            l->filled = to;
            InterlockedIncrement64(&n_grown);
            note_farthest((LONG64)(at - l->top));
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    fault_offset = (LONG64)(at - l->top);
    fault_tid = GetCurrentThreadId();
    InterlockedExchange(&fault_seen, 1);
    note_farthest((LONG64)(at - l->top));
    return EXCEPTION_CONTINUE_SEARCH;
}

/* At the start of a compiling thread, before it gives the driver anything */
static void landing_make(void) {
    int mode = vk_landing_mode();
    if (!mode) return;
    uint8_t *top = (uint8_t *)((NT_TIB *)NtCurrentTeb())->StackBase;
    MEMORY_BASIC_INFORMATION mine, above;
    if (VirtualQuery(top - 1, &mine, sizeof mine)) {
        LONG i = InterlockedIncrement(&nstack_bases) - 1;
        if (i < (LONG)(sizeof stack_bases / sizeof *stack_bases)) stack_bases[i] = mine.AllocationBase;
    }
    LONG nl = nlandings < (LONG)(sizeof landings / sizeof *landings) ? nlandings : (LONG)(sizeof landings / sizeof *landings);
    for (LONG i = 0; i < nl; i++)
        if (landings[i].top == top) {   /* (an ended thread's stack was here: its landing is this one's) */
            this_landing = &landings[i];
            InterlockedIncrement64(&n_own);
            return;
        }
    LONG slot = InterlockedIncrement(&nlandings) - 1;
    static Landing spare;   /* (past 256 of them: noted in none, never grown) */
    Landing *l = slot < (LONG)(sizeof landings / sizeof *landings) ? &landings[slot] : &spare;
    l->top = l->filled = l->end = top;
    this_landing = l;   /* (from here a fault within reach of the top is noted) */
    if (!VirtualQuery(top, &above, sizeof above)) { InterlockedIncrement64(&n_none); return; }
    if (mode == 2 && above.State == MEM_FREE && above.RegionSize >= CHUNK) {
        SIZE_T want = (SIZE_T)mb_v << 20, size = above.RegionSize < want ? above.RegionSize & ~(SIZE_T)(CHUNK - 1) : want;
        if (VirtualAlloc(top, size, MEM_RESERVE, PAGE_NOACCESS)) {
            if (fill_part(top, CHUNK)) {
                l->filled = top + CHUNK;
                l->end = top + size;
                InterlockedIncrement64(&n_own);
                return;
            }
            VirtualFree(top, 0, MEM_RELEASE);
        }
        if (!VirtualQuery(top, &above, sizeof above)) { InterlockedIncrement64(&n_none); return; }
    }
    if (above.State == MEM_FREE && above.RegionSize >= CHUNK && VirtualAlloc(top, CHUNK, MEM_RESERVE | MEM_COMMIT, PAGE_READONLY)) {
        l->filled = l->end = top + CHUNK;   /* (mode 1, #629: zeros) */
        InterlockedIncrement64(&n_own);
        return;
    }
    if (above.State == MEM_COMMIT && (above.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) {
        InterlockedIncrement64(&n_shared);   /* (readable already) */
        return;
    }
    if (above.State == MEM_RESERVE && above.Type == MEM_PRIVATE && above.AllocationBase == (void *)top) {
        LONG n = nstack_bases < (LONG)(sizeof stack_bases / sizeof *stack_bases) ? nstack_bases : (LONG)(sizeof stack_bases / sizeof *stack_bases);
        for (LONG i = 0; i < n; i++)
            if (stack_bases[i] == (void *)top) {   /* (the bottom of another compiling thread's stack) */
                if (VirtualAlloc(top, 0x1000, MEM_COMMIT, PAGE_READONLY)) {
                    l->filled = l->end = top + 0x1000;
                    InterlockedIncrement64(&n_shared);
                    return;
                }
                break;
            }
    }
    InterlockedIncrement64(&n_none);
}

typedef struct Start { LPTHREAD_START_ROUTINE fn; void *arg; HANDLE ready; } Start;
static DWORD WINAPI thread_main(void *p) {
    Start s = *(Start *)p;
    landing_make();
    SetEvent(s.ready);   /* (the creator's: it frees p once this is set) */
    return s.fn(s.arg);
}

/* CreateThread for a thread that makes pipelines: with SSBU_VK_STACK_LANDING its landing made before it is returned
 * (one at a time, so the next one's stack is not put where this one's landing goes). Mode 2: at least a 64 MB stack. */
HANDLE vk_landing_thread(SIZE_T stack, DWORD flags, LPTHREAD_START_ROUTINE fn, void *arg) {
    int mode = vk_landing_mode();
    if (!mode) return CreateThread(NULL, stack, fn, arg, flags, NULL);
    if (mode == 2) {
        if (!handler) {
            PVOID h = AddVectoredExceptionHandler(1, landing_fault);
            if (InterlockedCompareExchangePointer(&handler, h, NULL) != NULL && h) RemoveVectoredExceptionHandler(h);
        }
        if (stack < (64ull << 20)) { stack = 64ull << 20; flags |= STACK_SIZE_PARAM_IS_A_RESERVATION; }
    } else if (!handler) {   /* (mode 1: the handler only notes where a read past a top was, for the report) */
        PVOID h = AddVectoredExceptionHandler(1, landing_fault);
        if (InterlockedCompareExchangePointer(&handler, h, NULL) != NULL && h) RemoveVectoredExceptionHandler(h);
    }
    Start *s = malloc(sizeof *s);
    HANDLE ready = s ? CreateEventW(NULL, TRUE, FALSE, NULL) : NULL;
    if (!ready) { free(s); return CreateThread(NULL, stack, fn, arg, flags, NULL); }
    s->fn = fn; s->arg = arg; s->ready = ready;
    HANDLE t = CreateThread(NULL, stack, thread_main, s, flags, NULL);
    if (t) {
        HANDLE both[2] = {ready, t};
        if (WaitForMultipleObjects(2, both, FALSE, 5000) == WAIT_TIMEOUT) s = NULL;   /* (not seen started: s left to it) */
    }
    if (s) { CloseHandle(ready); free(s); }
    return t;
}

/* A crash or freeze report's lines (only snprintf and the line: the faulting thread may hold locks) */
void vk_landing_report(void (*line)(const char *text)) {
    char text[400];
    int mode = vk_landing_mode();
    if (!mode || !(n_own || n_shared || n_none)) return;
    if (fault_seen) {
        snprintf(text, sizeof text, "a read past a compiling thread's stack top the landing did not take: thread %lu, at the stack top + 0x%llx\n",
                 (unsigned long)fault_tid, (unsigned long long)fault_offset);
        line(text);
    }
    if (mode == 2)
        snprintf(text, sizeof text, "the compiling threads' landings past their stacks (SSBU_VK_STACK_LANDING=2, %d MB, filled with 0x%02x): "
                 "%lld of their own, %lld shared, %lld none; grown %lld times as read, farthest read the top + 0x%llx\n",
                 mb_v, fill_v, (long long)n_own, (long long)n_shared, (long long)n_none, (long long)n_grown, (unsigned long long)farthest);
    else
        snprintf(text, sizeof text, "the compiling threads' landings past their stacks (SSBU_VK_STACK_LANDING=1): %lld of their own, %lld shared, %lld none\n",
                 (long long)n_own, (long long)n_shared, (long long)n_none);
    line(text);
}

/* A status line when a landing grew since the last one (it is the driver reading past a stack top): 1 when said */
int vk_landing_status(char *text, size_t n) {
    static LONG64 said;
    LONG64 grown = n_grown;
    if (grown == said) return 0;
    said = grown;
    snprintf(text, n, "[status] vulkan: the driver read past a compiling thread's stack top: its landing grew %lld times, farthest the top + 0x%llx "
                      "(SSBU_VK_STACK_LANDING=2)\n", (long long)grown, (unsigned long long)farthest);
    return 1;
}
#else
typedef int gpu_vk_landing_windows_only;   /* (the Vulkan back end is Windows' here: nothing on the other systems) */
#endif
