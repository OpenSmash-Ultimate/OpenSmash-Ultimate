/* Offline checks of the native kernel's threads and handles (runtime/kernel.c), through its system
 * calls, with made-up guest threads (C functions standing in for guest code) and no game.
 *
 *   Windows: clang-cl /nologo /O1 tools\dev\kerneltest.c /Febuild\kerneltest.exe   (with cc.BASE_CFLAGS)
 *   Linux:   clang -O1 -I runtime/macos/include -I runtime/linux/include -I runtime tools/dev/kerneltest.c runtime/linux/win32.c -lpthread -o build/kerneltest
 *   macOS:   the same with runtime/macos/memory.c and runtime/macos/platform.c in place of -lpthread
 *   (runtime/macos/include: the Windows types without the Linux build's Direct3D headers)
 *
 *   SSBU_THREAD_RECYCLING=1 kerneltest recycling
 *       one slot's handles through a full turn of its use count (0x7FFF + 4 uses: nonzero, below
 *       1 << 30, the first value back only after 0x7FFF); 2100 events open at once said once in
 *       the log; 6000 threads made, started, ended (returning, or through svcExitThread) and
 *       closed one after another, and 40 batches of 50 at once, without a fatal error: the table
 *       of live threads stays small, at most 1024 thread-local regions are made (more would mean
 *       none used again) and the memory map keeps its count of regions; a thread closed before
 *       it exits and one closed after are both freed; an exited thread stays signalled while its
 *       handle is open; a handle whose object was freed refused by StartThread, Get/
 *       SetThreadPriority, Get/SetThreadCoreMask, SignalEvent, ClearEvent, CloseHandle,
 *       ResetSignal, WaitSynchronization, CancelSynchronization, SendSyncRequest (both) and
 *       GetThreadId, its slot then giving a different value; a reused thread-local region
 *       zeroed; a wait on a thread whose handle another thread closes still sees it end, one on
 *       a closed event times out, both then freed; CancelSynchronization; the two handles of
 *       svcCreateEvent, IPC copies of a native event and sessions closed one reference at a
 *       time; userspace mutexes handed over and condition variables signalled among threads
 *       that come and go
 *   kerneltest old
 *       without the switch, as before: handles 0xA000 + slot, thread objects kept after
 *       CloseHandle, which always succeeds, SetThreadCoreMask taking any handle, nothing said
 *       of the handle table
 *
 * Exit code 0 when every check passes. tests/test_kernel_threads.py builds and runs both. */
#include <stdarg.h>
#include "../../runtime/kernel.c"
#include "../../runtime/clock.h"

/* ---- what kernel.c needs from the rest of the runtime ---------------------------------- */
int rt_trace;
u64 rt_clock_ns(void) { return clock_now_ns(); }
void rt_call(Cpu *c, u64 guest_addr) { ((GuestFn *)(uintptr_t)guest_addr)(c); }   /* (an "entry" is a C function here) */
const char *rt_describe(u64 guest_addr, char *buf, size_t n) { snprintf(buf, n, "test function %#llx", (unsigned long long)guest_addr); return buf; }
void rt_backtrace_thread(HANDLE thread, int max_frames) { (void)thread; (void)max_frames; }
_Noreturn void rt_fatal(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    printf("FATAL: ");
    vprintf(fmt, a);
    va_end(a);
    printf("\n");
    fflush(stdout);
    exit(3);
}
u64 rt_callsym(Cpu *c, const char *symbol, int n, ...) { (void)c; (void)symbol; (void)n; return 0; }
void *ipc_connect(const char *port) { (void)port; return NULL; }
u32 ipc_request(Cpu *c, void *session, u64 message, u64 size) { (void)c; (void)session; (void)message; (void)size; return 0; }
static volatile int sessions_closed;
void ipc_close(void *session) { (void)session; sessions_closed++; }
void gpu_memory_unmapping(u64 addr, u64 size) { (void)addr; (void)size; }
#ifdef __APPLE__
void rt_upnp_remove(void) {}
#endif

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } } while (0)

/* ---- guest memory: the heap ---------------------------------------------------------------- */
#define MAIN_ARRAY (HEAP_BASE)              /* the main thread's WaitSynchronization handles */
#define SHARED (HEAP_BASE + 0x1000)         /* the mutex test's words */
#define MUTEX (SHARED)
#define CV (SHARED + 4)
#define GATE (SHARED + 8)
#define COUNTER (SHARED + 12)
#define INSIDE (SHARED + 16)
#define BAD (SHARED + 20)
#define ARRIVED (SHARED + 24)
#define RECORDS (HEAP_BASE + 0x10000)       /* a record a thread: its handle array, result, ... */
#define REC(i) (RECORDS + 0x100ull * (i))
/* in a record: +0 the handle it waits on, +4 that wait's result, +8 its timeout (ns), +16 its
 * TPIDRRO_EL0, +24 1 if its thread-local region was all zeros */
#define SECOND 1000000000ll

static u64 svc(Cpu *c, u32 n) { rt_svc(c, n); return (u32)c->x[0]; }
static u32 wait_on(Cpu *c, u64 array, u32 h, i64 timeout) {
    st32(array, h);
    c->x[1] = array; c->x[2] = 1; c->x[3] = (u64)timeout;
    return (u32)svc(c, 0x18);
}
static u32 create(Cpu *c, GuestFn *fn, u64 arg, u32 *h) {
    c->x[1] = (u64)(uintptr_t)fn; c->x[2] = arg; c->x[3] = 0; c->x[4] = 44; c->x[5] = 0;
    u32 r = (u32)svc(c, 0x08);
    *h = (u32)c->x[1];
    return r;
}
static u32 start(Cpu *c, u32 h) { c->x[0] = h; return (u32)svc(c, 0x09); }
static u32 close_h(Cpu *c, u32 h) { c->x[0] = h; return (u32)svc(c, 0x16); }
static u32 new_event(Cpu *c) { svc(c, 0x45); return (u32)c->x[1]; }
static u32 signal_event(Cpu *c, u32 h) { c->x[0] = h; return (u32)svc(c, 0x11); }
static u32 thread_id(Cpu *c, u32 h, u64 *id) { c->x[1] = h; u32 r = (u32)svc(c, 0x25); *id = c->x[1]; return r; }

/* kernel state, under its lock */
static u32 live(void) { EnterCriticalSection(&klock); u32 n = nthreads; LeaveCriticalSection(&klock); return n; }
static u32 used_slots(void) {
    EnterCriticalSection(&klock);
    u32 n = 0;
    for (u32 i = 1; i < 4096; i++) n += objects[i].type != OBJ_FREE;
    LeaveCriticalSection(&klock);
    return n;
}
static int slot_type(u32 h) { EnterCriticalSection(&klock); int t = objects[index_of(h)].type; LeaveCriticalSection(&klock); return t; }
static KThread *thread_of(u32 h) { EnterCriticalSection(&klock); KThread *t = objects[index_of(h)].thread; LeaveCriticalSection(&klock); return t; }
static int waiting(KThread *t) { EnterCriticalSection(&klock); int w = t->wait_count > 0; LeaveCriticalSection(&klock); return w; }
/* up to 10 s for cond (a loaded machine); 0 if it never held */
static int until(int (*cond)(void *), void *arg) {
    for (u64 end = rt_clock_ns() + 10 * SECOND; !cond(arg);) {
        if (rt_clock_ns() > end) return 0;
        Sleep(1);
    }
    return 1;
}
static int is_waiting(void *t) { return waiting(t); }
static int live_is(void *n) { return live() == (u32)(uintptr_t)n; }
static int reaped(void *unused) {   /* every freed thread's host thread ended, its memory freed */
    (void)unused;
    EnterCriticalSection(&klock);
    reap();
    int empty = !dead;
    LeaveCriticalSection(&klock);
    return empty;
}
static int settle(void) { return until(reaped, NULL); }

/* ---- made-up guest threads ------------------------------------------------------------------ */
static void g_return(Cpu *c) { (void)c; }             /* leaves by returning from its entry */
static void g_exit(Cpu *c) { rt_svc(c, 0x0A); }        /* leaves through svcExitThread */
static void g_wait(Cpu *c) {                           /* waits on the handle in its record */
    u64 rec = c->x[0];
    i64 timeout = (i64)ld64(rec + 8);
    st32(rec + 4, wait_on(c, rec, ld32(rec), timeout ? timeout : 10 * SECOND));
}
static void g_tls(Cpu *c) {                            /* looks at its thread-local region, then fills it */
    u64 rec = c->x[0];
    const u8 *p = G2H(c->tpidrro);
    int zero = 1;
    for (int i = 0; i < 0x1000; i++) if (p[i]) { zero = 0; break; }
    st64(rec + 16, c->tpidrro);
    st32(rec + 24, (u32)zero);
    memset(G2H(c->tpidrro), 0xAB, 0x1000);
}

/* userspace mutexes as the SDK's work: the word holds the owner's handle, the kernel queues
 * the others (svcArbitrateLock) and hands the mutex over (svcArbitrateUnlock) */
static u32 own_handle(Cpu *c) { return ((KThread *)c->thread)->handle; }
static void guest_lock(Cpu *c, u64 m, u32 me) {
    for (;;) {
        u32 v = ld32(m);
        if (v == 0) { if (cas32(m, 0, me)) return; continue; }
        if ((v & ~WAITERS) == me) return;   /* (handed over) */
        if (!(v & WAITERS) && !cas32(m, v, v | WAITERS)) continue;
        c->x[0] = v & ~WAITERS; c->x[1] = m; c->x[2] = me;
        rt_svc(c, 0x1A);
    }
}
static void guest_unlock(Cpu *c, u64 m, u32 me) {
    if (cas32(m, me, 0)) return;
    c->x[0] = m;
    rt_svc(c, 0x1B);
}
static void bad(void) { __atomic_fetch_add((u32 *)G2H(BAD), 1, __ATOMIC_SEQ_CST); }
static void g_worker(Cpu *c) {   /* 20 turns in the mutex, then waits for the gate on the condition variable */
    u32 me = own_handle(c);
    for (int r = 0; r < 20; r++) {
        guest_lock(c, MUTEX, me);
        if (ld32(INSIDE)) bad();
        st32(INSIDE, 1);
        st32(COUNTER, ld32(COUNTER) + 1);
        if (r % 4 == 0) { c->x[0] = 0; rt_svc(c, 0x0B); }   /* (a yield inside: the others queue) */
        st32(INSIDE, 0);
        guest_unlock(c, MUTEX, me);
    }
    guest_lock(c, MUTEX, me);
    while (!ld32(GATE)) {
        c->x[0] = MUTEX; c->x[1] = CV; c->x[2] = me; c->x[3] = 10 * SECOND;
        rt_svc(c, 0x1C);
        if ((u32)c->x[0] == R_TIMEOUT) { bad(); guest_lock(c, MUTEX, me); }   /* (a timed-out wait returns without the mutex) */
    }
    if ((ld32(MUTEX) & ~WAITERS) != me) bad();
    st32(ARRIVED, ld32(ARRIVED) + 1);
    guest_unlock(c, MUTEX, me);
}

/* ---- with recycling ------------------------------------------------------------------------- */
/* one slot used 0x7FFF + 4 times over (an event made and closed, nothing else running): its
 * handles' upper bits count 1 to 0x7FFF and start again, every value nonzero and below the
 * WAITERS bit, the slot's first value back only after 0x7FFF uses */
static void generations(Cpu *c) {
    u32 first = new_event(c), h = first, ok = 1, back = 0, top = 0;
    for (u32 n = 1; n <= 0x7FFF + 4 && ok; n++) {
        ok = close_h(c, h) == R_OK && close_h(c, h) == R_OK;
        u32 next = new_event(c);
        ok = ok && next && next < WAITERS && next != h && (next & 0x7FFF) == (first & 0x7FFF) && next >> 15 == (h >> 15) % 0x7FFF + 1;
        CHECK(ok, "use %u of slot %u: %#x after %#x", n, first & 0x7FFF, next, h);
        if (next == first && !back) back = n;
        if (next >> 15 == 0x7FFF) top = next;
        h = next;
    }
    CHECK(close_h(c, h) == R_OK && close_h(c, h) == R_OK && slot_type(h) == OBJ_FREE, "the last event");
    CHECK(back == 0x7FFF, "the slot's first handle %#x back after %u uses, not 0x7FFF", first, back);
    CHECK(top == (0x7FFFu << 15 | (first & 0x7FFF)), "the slot's highest handle %#x", top);
}

/* 2100 events open at once: the log says once that half the handle table is in use (a slow leak
 * shows before "out of handles"), and closing them gives every slot back */
static u32 many[2100];
static void half_table(Cpu *c) {
    u32 slots0 = used_slots();
    CHECK(!said_half_full, "half the handle table said in use with %u slots used", slots0);
    for (int i = 0; i < 2100; i++) many[i] = new_event(c);
    CHECK(said_half_full, "%u handle slots in use, nothing said", used_slots());
    for (int i = 0; i < 2100; i++) CHECK(close_h(c, many[i]) == R_OK && close_h(c, many[i]) == R_OK, "event %d (%#x)", i, many[i]);
    CHECK(used_slots() == slots0, "%u handle slots used, %u before", used_slots(), slots0);
}

static void churn(Cpu *c) {
    EnterCriticalSection(&klock);
    u32 regions0 = region_count;
    u64 tlr0 = tlr_next;
    LeaveCriticalSection(&klock);
    u32 base = live(), slots0 = used_slots(), max_live = 0, made = 0, ok = 1;
    static u32 last[4096];
    for (int i = 0; i < 6000 && ok; i++) {
        u32 h;
        ok = create(c, i & 1 ? g_exit : g_return, 0, &h) == R_OK && h && h < WAITERS && h != CUR_THREAD && h != CUR_PROCESS &&
             h != last[h & 0x7FFF] && (h & 0x7FFF) < 4096;
        CHECK(ok, "thread %d: handle %#x (the slot's previous one %#x)", i, h, last[h & 0x7FFF & 4095]);
        last[h & 0x7FFF & 4095] = h;
        if (live() > max_live) max_live = live();
        ok = ok && start(c, h) == R_OK && wait_on(c, MAIN_ARRAY, h, 10 * SECOND) == R_OK && close_h(c, h) == R_OK;
        CHECK(ok, "thread %d (%#x) did not start, end and close", i, h);
        made++;
    }
    /* batches: 50 threads at once wait on an event, half closed before it is set */
    for (int b = 0; b < 40 && ok; b++) {
        u32 ev = new_event(c), h[50];
        for (int i = 0; i < 50; i++) {
            st32(REC(i), ev);
            st64(REC(i) + 8, 0);
            ok = ok && create(c, g_wait, REC(i), &h[i]) == R_OK && start(c, h[i]) == R_OK;
        }
        made += 50;
        if (live() > max_live) max_live = live();
        CHECK(live() == base + 50, "batch %d: %u threads live, not %u", b, live(), base + 50);
        for (int i = 0; i < 25; i++) ok = ok && close_h(c, h[i]) == R_OK;
        CHECK(live() == base + 50, "batch %d: closing running threads' handles freed them (%u live)", b, live());
        ok = ok && signal_event(c, ev) == R_OK;
        for (int i = 25; i < 50; i++) ok = ok && wait_on(c, MAIN_ARRAY, h[i], 10 * SECOND) == R_OK && close_h(c, h[i]) == R_OK;
        CHECK(ok, "batch %d did not run", b);
        ok = ok && until(live_is, (void *)(uintptr_t)base);
        CHECK(ok, "batch %d: %u threads live after it ended, not %u", b, live(), base);
        for (int i = 0; i < 50; i++) CHECK(ld32(REC(i) + 4) == R_OK, "batch %d thread %d: its wait gave %#x", b, i, ld32(REC(i) + 4));
        CHECK(close_h(c, ev) == R_OK && close_h(c, ev) == R_OK && close_h(c, ev) == R_INVALID_HANDLE, "svcCreateEvent's two handles, then a stale one");
    }
    CHECK(settle(), "freed threads' host threads never ended");
    EnterCriticalSection(&klock);
    u32 regions = region_count, tlr_pages = (u32)((tlr_next - tlr0) >> 12);
    LeaveCriticalSection(&klock);
    printf("%u threads made: at most %u live, %u thread-local regions made, %u memory-map regions before and %u after\n", made, max_live, tlr_pages,
           regions0, regions);
    CHECK(made == 8000, "%u threads made", made);
    CHECK(max_live <= base + 50, "%u threads live at once", max_live);
    CHECK(live() == base, "%u threads left, not %u", live(), base);
    CHECK(used_slots() == slots0, "%u handle slots used, %u before", used_slots(), slots0);
    CHECK(regions == regions0, "%u memory-map regions, %u before", regions, regions0);
    CHECK(tlr_pages <= 1024, "%u thread-local regions made for %u threads", tlr_pages, made);   /* (some 51 but where host threads end slowly) */
}

static void lifetimes(Cpu *c) {
    u32 base = live(), ev = new_event(c), t, w, h2;
    u64 id, id2;
    /* closed while it runs: it goes on, its handle names nothing, and it is freed when it exits */
    st32(REC(0), ev);
    st64(REC(0) + 8, 0);
    create(c, g_wait, REC(0), &t);
    start(c, t);
    KThread *kt = thread_of(t);
    CHECK(until(is_waiting, kt), "the thread never waited");
    CHECK(close_h(c, t) == R_OK, "CloseHandle of a running thread");
    CHECK(live() == base + 1 && slot_type(t) == OBJ_THREAD, "a running thread freed at CloseHandle");
    CHECK(start(c, t) == R_INVALID_HANDLE && thread_id(c, t, &id) == R_INVALID_HANDLE && close_h(c, t) == R_INVALID_HANDLE &&
          wait_on(c, MAIN_ARRAY, t, 0) == R_INVALID_HANDLE, "a closed handle still names its thread");
    signal_event(c, ev);
    CHECK(until(live_is, (void *)(uintptr_t)base), "a closed thread not freed once it exited (%u live)", live());
    CHECK(slot_type(t) == OBJ_FREE && ld32(REC(0) + 4) == R_OK, "its slot");
    CHECK(settle(), "its host thread never ended");

    /* exited, its handle open: signalled, waitable, its id there, until it is closed */
    create(c, g_exit, 0, &t);
    start(c, t);
    CHECK(wait_on(c, MAIN_ARRAY, t, 10 * SECOND) == R_OK, "waiting for a thread");
    CHECK(settle(), "an exited thread's host thread never ended");
    CHECK(wait_on(c, MAIN_ARRAY, t, 0) == R_OK && wait_on(c, MAIN_ARRAY, t, -1) == R_OK, "an exited thread whose handle is open not signalled");
    CHECK(thread_id(c, t, &id) == R_OK && id, "an exited thread's id");
    CHECK(live() == base + 1 && slot_type(t) == OBJ_THREAD, "an exited thread freed while its handle is open");
    CHECK(close_h(c, t) == R_OK && live() == base && slot_type(t) == OBJ_FREE, "an exited thread not freed at CloseHandle");

    /* the stale handle: refused by every call taking one, and its slot's next handle differs */
    CHECK(wait_on(c, MAIN_ARRAY, t, 0) == R_INVALID_HANDLE, "WaitSynchronization on a stale handle");
    CHECK(close_h(c, t) == R_INVALID_HANDLE, "CloseHandle of a stale handle");
    CHECK(start(c, t) == R_INVALID_HANDLE, "StartThread on a stale handle");
    CHECK(thread_id(c, t, &id2) == R_INVALID_HANDLE, "GetThreadId on a stale handle");
    c->x[1] = t; CHECK(svc(c, 0x0C) == R_INVALID_HANDLE, "GetThreadPriority on a stale handle");
    c->x[0] = t; c->x[1] = 40; CHECK(svc(c, 0x0D) == R_INVALID_HANDLE, "SetThreadPriority on a stale handle");
    c->x[2] = t; CHECK(svc(c, 0x0E) == R_INVALID_HANDLE, "GetThreadCoreMask on a stale handle");
    c->x[0] = t; CHECK(svc(c, 0x19) == R_INVALID_HANDLE, "CancelSynchronization on a stale handle");
    c->x[0] = t; CHECK(svc(c, 0x17) == R_INVALID_HANDLE, "ResetSignal on a stale handle");
    c->x[0] = t; c->x[1] = 0; c->x[2] = 1; CHECK(svc(c, 0x0F) == R_INVALID_HANDLE, "SetThreadCoreMask on a stale handle");
    c->x[0] = 0x12345; c->x[1] = 0; c->x[2] = 1; CHECK(svc(c, 0x0F) == R_INVALID_HANDLE, "SetThreadCoreMask on a made-up handle");
    c->x[0] = CUR_THREAD; c->x[1] = 0; c->x[2] = 1; CHECK(svc(c, 0x0F) == R_OK, "SetThreadCoreMask on the current thread");
    create(c, g_exit, 0, &h2);
    CHECK((h2 & 0x7FFF) == (t & 0x7FFF) && h2 != t, "the slot used again: %#x after %#x", h2, t);
    c->x[0] = h2; c->x[1] = 0; c->x[2] = 1; CHECK(svc(c, 0x0F) == R_OK, "SetThreadCoreMask on the slot's new thread");
    CHECK(thread_id(c, h2, &id2) == R_OK && id2 > id, "the new thread's id %llu after %llu", (unsigned long long)id2, (unsigned long long)id);
    CHECK(thread_id(c, t, &id) == R_INVALID_HANDLE && start(c, t) == R_INVALID_HANDLE, "the stale handle names the slot's new thread");
    CHECK(close_h(c, h2) == R_OK && live() == base, "a thread never started not freed at CloseHandle");

    /* a thread-local region used again, zeroed */
    CHECK(settle(), "host threads never ended");
    create(c, g_tls, REC(1), &t);
    start(c, t);
    wait_on(c, MAIN_ARRAY, t, 10 * SECOND);
    close_h(c, t);
    CHECK(settle(), "its host thread never ended");
    create(c, g_tls, REC(2), &t);
    start(c, t);
    wait_on(c, MAIN_ARRAY, t, 10 * SECOND);
    close_h(c, t);
    CHECK(ld32(REC(1) + 24) == 1 && ld32(REC(2) + 24) == 1, "a thread-local region not zeroed (%u, then %u)", ld32(REC(1) + 24), ld32(REC(2) + 24));
    CHECK(ld64(REC(1) + 16) == ld64(REC(2) + 16) && ld64(REC(1) + 16) >= KERNEL_BASE, "the freed region (%#llx) not used again (%#llx)",
          (unsigned long long)ld64(REC(1) + 16), (unsigned long long)ld64(REC(2) + 16));

    /* a wait on a thread whose handle another thread closes: it still sees the thread end */
    u32 ev2 = new_event(c);
    st32(REC(3), ev2);
    st64(REC(3) + 8, 0);
    create(c, g_wait, REC(3), &t);
    start(c, t);
    st32(REC(4), t);
    st64(REC(4) + 8, 10 * SECOND);
    create(c, g_wait, REC(4), &w);
    start(c, w);
    CHECK(until(is_waiting, thread_of(w)), "the waiter never waited");
    CHECK(close_h(c, t) == R_OK, "closing the waited-on thread's handle");
    signal_event(c, ev2);
    CHECK(wait_on(c, MAIN_ARRAY, w, 10 * SECOND) == R_OK && ld32(REC(4) + 4) == R_OK, "a wait on a thread whose handle was closed gave %#x",
          ld32(REC(4) + 4));
    CHECK(close_h(c, w) == R_OK && until(live_is, (void *)(uintptr_t)base), "the threads not freed (%u live)", live());
    CHECK(slot_type(t) == OBJ_FREE, "the waited-on thread's slot");
    /* ... and one on an event whose handles are closed: it times out, then the event is gone */
    u32 ev3 = new_event(c);
    st32(REC(5), ev3);
    st64(REC(5) + 8, SECOND / 4);
    create(c, g_wait, REC(5), &w);
    start(c, w);
    CHECK(until(is_waiting, thread_of(w)), "the waiter never waited");
    c->x[0] = ev3; CHECK(svc(c, 0x12) == R_OK, "ClearEvent");
    CHECK(close_h(c, ev3) == R_OK && close_h(c, ev3) == R_OK && slot_type(ev3) == OBJ_EVENT, "an event a wait holds freed at CloseHandle");
    CHECK(signal_event(c, ev3) == R_INVALID_HANDLE, "SignalEvent on a closed handle");
    c->x[0] = ev3; CHECK(svc(c, 0x12) == R_INVALID_HANDLE, "ClearEvent on a closed handle");
    CHECK(wait_on(c, MAIN_ARRAY, w, 10 * SECOND) == R_OK && ld32(REC(5) + 4) == R_TIMEOUT, "a wait on a closed event gave %#x", ld32(REC(5) + 4));
    CHECK(slot_type(ev3) == OBJ_FREE && wait_on(c, MAIN_ARRAY, ev3, 0) == R_INVALID_HANDLE, "the closed event not freed after the wait");
    close_h(c, w);

    /* CancelSynchronization ends a wait */
    u32 ev4 = new_event(c);
    st32(REC(6), ev4);
    st64(REC(6) + 8, 0);
    create(c, g_wait, REC(6), &w);
    start(c, w);
    CHECK(until(is_waiting, thread_of(w)), "the waiter never waited");
    c->x[0] = w;
    CHECK(svc(c, 0x19) == R_OK, "CancelSynchronization");
    CHECK(wait_on(c, MAIN_ARRAY, w, 10 * SECOND) == R_OK && ld32(REC(6) + 4) == R_CANCELLED, "a cancelled wait gave %#x", ld32(REC(6) + 4));
    close_h(c, w);
    close_h(c, ev);
    close_h(c, ev);
    close_h(c, ev2);
    close_h(c, ev2);
    close_h(c, ev4);
    close_h(c, ev4);

    /* a native event handed out twice (IPC copies): each copy closed on its own, native code's kept */
    u32 ne = kernel_create_event();
    kernel_copy_handle(ne);
    kernel_copy_handle(ne);
    CHECK(close_h(c, ne) == R_OK && close_h(c, ne) == R_OK, "closing IPC copies of a native event");
    kernel_signal_event(ne);
    CHECK(wait_on(c, MAIN_ARRAY, ne, 0) == R_OK, "a native event after its copies were closed");
    CHECK(close_h(c, ne) == R_OK && slot_type(ne) == OBJ_FREE, "the native event's last handle");
    kernel_signal_event(ne);   /* (names nothing now) */
    /* a session: ipc_close at its CloseHandle, once */
    int closed = sessions_closed;
    u32 s = kernel_new_session(&checks);
    CHECK(kernel_session_of(s) == &checks, "a session's handle");
    c->x[0] = s; CHECK(svc(c, 0x21) == R_OK, "SendSyncRequest");
    CHECK(close_h(c, s) == R_OK && sessions_closed == closed + 1 && !kernel_session_of(s), "closing a session");
    CHECK(close_h(c, s) == R_INVALID_HANDLE && sessions_closed == closed + 1, "closing a session twice");
    c->x[0] = s; CHECK(svc(c, 0x21) == R_INVALID_HANDLE, "SendSyncRequest on a closed session");
    c->x[0] = 0; c->x[1] = 0; c->x[2] = s; CHECK(svc(c, 0x22) == R_INVALID_HANDLE, "SendSyncRequestWithUserBuffer on a closed session");
    CHECK(live() == base, "%u threads left, not %u", live(), base);
}

static void mutexes(Cpu *c, u32 main_handle) {
    u32 base = live(), h[8];
    memset(G2H(SHARED), 0, 0x100);
    for (int round = 0; round < 25; round++) {
        st32(GATE, 0);
        for (int i = 0; i < 8; i++) {
            create(c, g_worker, 0, &h[i]);
            start(c, h[i]);
        }
        guest_lock(c, MUTEX, main_handle);
        st32(GATE, 1);
        c->x[0] = CV; c->x[1] = (u64)-1;
        svc(c, 0x1D);
        guest_unlock(c, MUTEX, main_handle);
        int ok = 1;
        for (int i = 0; i < 8; i++) ok = ok && wait_on(c, MAIN_ARRAY, h[i], 10 * SECOND) == R_OK && close_h(c, h[i]) == R_OK;
        CHECK(ok, "round %d: the workers did not end", round);
        if (!ok) return;
    }
    CHECK(ld32(COUNTER) == 25 * 8 * 20 && ld32(ARRIVED) == 25 * 8 && ld32(BAD) == 0 && ld32(MUTEX) == 0,
          "mutex and condition variable: %u turns of %u, %u through the gate of %u, %u wrong, the word %#x at the end", ld32(COUNTER), 25 * 8 * 20,
          ld32(ARRIVED), 25 * 8, ld32(BAD), ld32(MUTEX));
    CHECK(until(live_is, (void *)(uintptr_t)base), "%u threads left, not %u", live(), base);
}

/* ---- without recycling: as before -------------------------------------------------------- */
static void old_behaviour(Cpu *c) {
    u32 base = live(), t;
    u64 id;
    create(c, g_exit, 0, &t);
    CHECK(t > 0xA000 && t < 0xB000, "handle %#x", t);
    start(c, t);
    CHECK(wait_on(c, MAIN_ARRAY, t, 10 * SECOND) == R_OK, "waiting for a thread");
    CHECK(close_h(c, t) == R_OK && slot_type(t) == OBJ_THREAD && live() == base + 1 && thread_id(c, t, &id) == R_OK,
          "a closed thread's object not kept");
    CHECK(close_h(c, t) == R_OK && close_h(c, 0x1234) == R_OK, "CloseHandle failed");
    c->x[0] = 0x12345; c->x[1] = 0; c->x[2] = 1; CHECK(svc(c, 0x0F) == R_OK, "SetThreadCoreMask refused a handle without the switch");
    u32 ev = new_event(c);
    CHECK(ev > 0xA000 && ev < 0xB000 && close_h(c, ev) == R_OK && slot_type(ev) == OBJ_FREE, "an event not freed at its first CloseHandle");
    for (int i = 0; i < 2100; i++) many[i] = new_event(c);
    CHECK(!said_half_full, "half the handle table said in use without the switch");
    for (int i = 0; i < 2100; i++) close_h(c, many[i]);
    for (int i = 0; i < 200; i++) {
        create(c, g_return, 0, &t);
        start(c, t);
        wait_on(c, MAIN_ARRAY, t, 10 * SECOND);
        close_h(c, t);
    }
    CHECK(live() == base + 201, "%u threads, not %u: threads freed without the switch", live(), base + 201);
}

int main(int argc, char **argv) {
    int want = !(argc > 1 && !strcmp(argv[1], "old"));
    setvbuf(stdout, NULL, _IONBF, 0);
#ifdef __APPLE__
    if (!rt_guest_window()) { printf("cannot reserve the guest's address space\n"); return 2; }
#endif
    kernel_init();
    CHECK(recycle == want, "SSBU_THREAD_RECYCLING read as %d", recycle);
    u32 main_handle;
    Cpu *c = kernel_main_thread(&main_handle);
    CHECK(main_handle && main_handle < WAITERS, "the main thread's handle %#x", main_handle);
    c->x[1] = 16 << 20;
    CHECK(svc(c, 0x01) == R_OK, "svcSetHeapSize");
    u64 t0 = rt_clock_ns();
    if (recycle) {
        u64 id;
        CHECK(thread_id(c, CUR_THREAD, &id) == R_OK && id == 1, "GetThreadId of the current thread");
        generations(c);
        half_table(c);
        churn(c);
        lifetimes(c);
        mutexes(c, main_handle);
    } else old_behaviour(c);
    printf("%.1f s\n%d checks, %d failed\n", (double)(rt_clock_ns() - t0) / 1e9, checks, failures);
    return failures ? 1 : 0;
}
