/* Offline checks of the rollback ring (runtime/hle_rollback_ring.c) and of the write watch the
 * kernel shares between it and the renderer (runtime/kernel.c, kernel_watch.h), on made-up memory
 * mapped through the kernel's system calls, with no game.
 *
 *   Windows: clang-cl /nologo /O1 tools\dev\ringtest.c /Febuild\ringtest.exe   (with cc.BASE_CFLAGS; /link /HIGHENTROPYVA:NO)
 *   Linux:   clang -O1 -I runtime/macos/include -I runtime/linux/include -I runtime tools/dev/ringtest.c runtime/linux/win32.c -lpthread -o build/ringtest
 *   macOS:   the same with runtime/macos/memory.c and runtime/macos/platform.c in place of -lpthread
 *
 *   SSBU_ROLLBACK_RING=4 SSBU_MAP_GUARD=1 ringtest
 *       the switch read (4 frames), the ring's frame end handler registered outside the fight
 *       test's, every area watched and SSBU_MAP_GUARD left off; heap, alias memory, a made-up
 *       module's data, the main stack and a thread-local region written with patterns between
 *       saves (whole pages, a few bytes, a page written with what it had, a page set to zeros), a
 *       page mapped and unmapped by svcMapMemory: after each save the pages changed and the
 *       copies held as the frames' bytes say; the newest frame put back over writes made since,
 *       then an older one, byte for byte, and the frames after it gone; twenty saves more, past
 *       the ring's end: the oldest and the newest put back, a dropped one refused, the copies held
 *       steady; the renderer still told of pages the ring took first, and the ring of pages the
 *       renderer took first, also with the renderer asking on a thread of its own as frames are
 *       written and saved; alias memory unmapped and mapped again between two saves seen as
 *       zeros, and its pattern put back; pages unmapped now left out of a restore, and pages
 *       mapped after a frame zeros when it is put back
 *   SSBU_ROLLBACK_RING=3 ringtest run
 *       as in a run: the handler the ring registered called at 301 frame ends, which saves each
 *       frame and says what the first save and the next 300 cost (tests/test_rollback_ring.py
 *       reads the lines), the last 3 frames held
 *   ringtest off
 *       without the switch: nothing registered, only the alias area watched, and the renderer's
 *       questions the system's own; the switch's values (1, 2 to 64; anything else off)
 *
 * Prints "<n> checks, 0 failed" and exits 0 when every check passes (tests/test_rollback_ring.py). */
#include <stdarg.h>
#include "../../runtime/kernel.c"
#include "../../runtime/hle_rollback_ring.c"
#include "../../runtime/clock.h"
#ifndef _WIN32
#include <signal.h>
#endif

/* ---- what kernel.c and the ring need from the rest of the runtime --------------------------- */
int rt_trace;
u64 rt_clock_ns(void) { return clock_now_ns(); }
void rt_call(Cpu *c, u64 guest_addr) { ((GuestFn *)(uintptr_t)guest_addr)(c); }
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
void ipc_close(void *session) { (void)session; }
void gpu_memory_unmapping(u64 addr, u64 size) { (void)addr; (void)size; }
#ifdef __APPLE__
void rt_upnp_remove(void) {}
#endif

/* The frame hooks (hle_each_frame.c): what the ring registered, from its constructor. */
static int hooks_added, hook_point, hook_order;
static FrameHookFn *hook_before, *hook_after;
int frame_hook_add(int point, int order, FrameHookFn *before, FrameHookFn *after, void *ctx) {
    (void)ctx;
    hooks_added++;
    hook_point = point; hook_order = order; hook_before = before; hook_after = after;
    return 1;
}

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } } while (0)

#ifndef _WIN32
int rt_ww_fault(void *address);   /* (win32.c, macos/memory.c) */
static void on_fault(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    if (rt_ww_fault(si->si_addr)) return;   /* (a write the write watch takes, as host.c's handler has it) */
    signal(sig, SIG_DFL);                    /* (anything else: the crash it is) */
}
#endif

/* ---- guest memory --------------------------------------------------------------------------- */
#define PAGE 0x1000ull
#define HEAP 0x400000ull                       /* svcSetHeapSize */
#define PHYS (ALIAS_BASE + 0x100000)           /* svcMapPhysicalMemory */
#define PHYS_SIZE 0x100000ull
#define MOD_IMAGE 0x7100000000ull              /* a made-up module: code, then data */
#define MOD_CODE 0x10000ull
#define MOD_DATA 0x30000ull
#define DST (STACK_BASE + 0x1000000)           /* a svcMapMemory destination */

static u64 svc(Cpu *c, u32 n) { rt_svc(c, n); return (u32)c->x[0]; }
static u32 map_physical(Cpu *c, u64 at, u64 size) { c->x[0] = at; c->x[1] = size; return (u32)svc(c, 0x2C); }
static u32 unmap_physical(Cpu *c, u64 at, u64 size) { c->x[0] = at; c->x[1] = size; return (u32)svc(c, 0x2D); }
static u32 map(Cpu *c, u64 dst, u64 src, u64 size) { c->x[0] = dst; c->x[1] = src; c->x[2] = size; return (u32)svc(c, 0x04); }
static u32 unmap(Cpu *c, u64 dst, u64 src, u64 size) { c->x[0] = dst; c->x[1] = src; c->x[2] = size; return (u32)svc(c, 0x05); }

/* Made-up contents: each word from its offset and a salt. */
static u64 word(u64 off, u64 salt) { return (off + 1) * 0x9E3779B97F4A7C15ull ^ (salt * 0xD6E8FEB86659FD93ull); }
static void fill(u64 at, u64 size, u64 salt) { for (u64 off = 0; off < size; off += 8) st64(at + off, word(off, salt)); }

/* ---- the frames' bytes, as the test made them ---------------------------------------------- */
typedef struct Shot { u64 frame; KernelRange r[64]; u32 n; u8 *bytes; u64 size; } Shot;
static Shot shots[8];   /* (the frames the ring holds: shots[frame % 8]) */

static void shoot(u64 frame) {
    Shot *s = &shots[frame % 8];
    free(s->bytes);
    s->frame = frame;
    s->n = kernel_writable_ranges(s->r, 64);
    s->size = 0;
    for (u32 i = 0; i < s->n; i++) s->size += s->r[i].hi - s->r[i].lo;
    s->bytes = malloc(s->size);
    u64 at = 0;
    for (u32 i = 0; i < s->n; i++) {
        memcpy(s->bytes + at, G2H(s->r[i].lo), s->r[i].hi - s->r[i].lo);
        at += s->r[i].hi - s->r[i].lo;
    }
}

/* Saved by the ring, and as the test made it. */
static u64 save(void) {
    u64 n = rollback_ring_save();
    shoot(n);
    return n;
}

/* Guest memory as the frame's bytes are: the same regions, byte for byte. */
static int as_at(u64 frame, char *why, size_t n) {
    const Shot *s = &shots[frame % 8];
    KernelRange now[64];
    u32 k = kernel_writable_ranges(now, 64);
    if (s->frame != frame) { snprintf(why, n, "no bytes kept for frame %llu", (unsigned long long)frame); return 0; }
    if (k != s->n) { snprintf(why, n, "the memory map differs (%u ranges, %u then)", k, s->n); return 0; }
    /* (field by field: a KernelRange ends in padding no one sets, which memcmp would compare too) */
    for (u32 i = 0; i < k; i++)
        if (now[i].lo != s->r[i].lo || now[i].hi != s->r[i].hi || now[i].watched != s->r[i].watched) {
            snprintf(why, n, "range %u is [%#llx, %#llx) watched %d, then [%#llx, %#llx) watched %d", i, (unsigned long long)now[i].lo,
                     (unsigned long long)now[i].hi, now[i].watched, (unsigned long long)s->r[i].lo, (unsigned long long)s->r[i].hi, s->r[i].watched);
            return 0;
        }
    u64 at = 0;
    for (u32 i = 0; i < s->n; i++) {
        u64 size = s->r[i].hi - s->r[i].lo;
        for (u64 p = 0; p < size; p += PAGE)
            if (memcmp(G2H(s->r[i].lo + p), s->bytes + at + p, PAGE)) {
                snprintf(why, n, "the page at %#llx differs", (unsigned long long)(s->r[i].lo + p));
                return 0;
            }
        at += size;
    }
    snprintf(why, n, "ok");
    return 1;
}

static int zero_page(const u8 *p) { for (u64 i = 0; i < PAGE; i++) if (p[i]) return 0; return 1; }

/* A page's bytes in a frame (NULL if the frame did not have it). */
static const u8 *page_at(const Shot *s, u64 addr) {
    u64 at = 0;
    for (u32 i = 0; i < s->n; i++) {
        if (addr >= s->r[i].lo && addr < s->r[i].hi) return s->bytes + at + (addr - s->r[i].lo);
        at += s->r[i].hi - s->r[i].lo;
    }
    return NULL;
}

/* Pages that differ between two frames with the same regions (the pages a save must find
 * changed): all of them, or those in the ranges the write watch sees (watched 1) or does not (0). */
static u64 changed_where(u64 a, u64 b, int watched) {
    const Shot *x = &shots[a % 8], *y = &shots[b % 8];
    if (x->size != y->size) return ~0ull;
    u64 n = 0, at = 0;
    for (u32 i = 0; i < y->n; i++) {
        u64 size = y->r[i].hi - y->r[i].lo;
        if (watched < 0 || y->r[i].watched == watched)
            for (u64 p = 0; p < size; p += PAGE) n += memcmp(x->bytes + at + p, y->bytes + at + p, PAGE) != 0;
        at += size;
    }
    return n;
}
static u64 changed_between(u64 a, u64 b) { return changed_where(a, b, -1); }

/* The copies the ring should hold for frames [oldest, newest]: the newest frame's pages that are
 * not zeros, and for each frame after the oldest, the pages that changed into it and were not zeros
 * in the frame before (the regions the same throughout). */
static u64 copies_for(u64 oldest, u64 newest) {
    const Shot *top = &shots[newest % 8];
    u64 n = 0;
    for (u64 off = 0; off < top->size; off += PAGE) n += !zero_page(top->bytes + off);
    for (u64 f = oldest + 1; f <= newest; f++) {
        const Shot *was = &shots[(f - 1) % 8], *is = &shots[f % 8];
        for (u32 i = 0; i < is->n; i++)
            for (u64 a = is->r[i].lo; a < is->r[i].hi; a += PAGE) {
                const u8 *p = page_at(was, a), *q = page_at(is, a);
                if (p && memcmp(p, q, PAGE) && !zero_page(p)) n++;
            }
    }
    return n;
}

/* A frame's writes: whole pages here and there, a few bytes in others, a page written with what it
 * already held, one set to zeros. */
static void scribble(u64 salt) {
    for (u64 i = 0; i < 6; i++) fill(HEAP_BASE + ((salt * 7 + i * 13) % (HEAP / PAGE)) * PAGE, PAGE, salt * 100 + i);
    st32(HEAP_BASE + HEAP - PAGE + 0x123, (u32)salt);
    fill(PHYS + (salt % 16) * PAGE, 2 * PAGE, salt);
    st8(PHYS + PHYS_SIZE - 1, (u8)salt);
    fill(MOD_IMAGE + MOD_CODE + (salt % 32) * PAGE, PAGE, salt + 50);
    st64(MOD_IMAGE + MOD_CODE + MOD_DATA - 8, salt);
    u64 same = HEAP_BASE + 40 * PAGE;
    for (u64 off = 0; off < PAGE; off += 8) st64(same + off, ld64(same + off));
    memset(G2H(HEAP_BASE + (50 + salt % 4) * PAGE), 0, PAGE);
}

/* The same pages each frame, with other contents: once the ring is full, as many copies held
 * after each save. */
static void steady(u64 salt) {
    for (u64 i = 0; i < 8; i++) fill(HEAP_BASE + (100 + i * 3) * PAGE, PAGE, salt * 10 + i);
    fill(PHYS + 20 * PAGE, PAGE, salt);
    st64(MOD_IMAGE + MOD_CODE + 0x8000, salt);
}

/* ---- the checks ----------------------------------------------------------------------------- */
static void switch_read(void) {
    CHECK(kernel_ring_frames() == 4 && ring_switch_frames == 4, "SSBU_ROLLBACK_RING=4: 4 frames (%d)", ring_switch_frames);
    CHECK(hooks_added == 1 && hook_point == FRAME_HOOK_END && hook_order == FRAME_ORDER_ROLLBACK && !hook_before && hook_after == ring_at_frame_end,
          "one handler registered: after the frame end, at the ring's order");
    CHECK(FRAME_ORDER_ROLLBACK < FRAME_ORDER_FIGHT_TEST, "outside the fight test's (its after runs last)");
    CHECK(kernel_watch_shared && nwatch_areas == 4, "the write watch shared, over %u areas", nwatch_areas);
    CHECK(!map_guard, "SSBU_MAP_GUARD=1 left off");
}

static void frames_and_restores(Cpu *c) {
    char why[128];
    CHECK(rollback_ring_open(4) && !rollback_ring_open(4), "opened, once");
    RingStats s;
    u64 f1 = save();
    rollback_ring_stats(&s);
    u64 nonzero = copies_for(f1, f1);
    CHECK(f1 == 1 && s.changed == nonzero && s.copies == nonzero && s.pages == shots[1].size / PAGE,
          "frame 1: %llu pages, %llu not zeros (the ring: %llu changed, %llu copies)", (unsigned long long)s.pages, (unsigned long long)nonzero,
          (unsigned long long)s.changed, (unsigned long long)s.copies);
    u64 module_pages = MOD_DATA / PAGE;
    for (u64 f = 2; f <= 4; f++) {
        scribble(f);
        u64 n = save();
        rollback_ring_stats(&s);
        u64 want = changed_between(n - 1, n);
        CHECK(n == f && s.changed == want, "frame %llu: %llu pages changed (%llu found)", (unsigned long long)f, (unsigned long long)want, (unsigned long long)s.changed);
        CHECK(s.written >= changed_where(n - 1, n, 1) && s.checked == s.written + module_pages,
              "frame %llu: the watch's %llu pages (%llu of them changed) and every page of the module's data compared (%llu)", (unsigned long long)f,
              (unsigned long long)s.written, (unsigned long long)changed_where(n - 1, n, 1), (unsigned long long)s.checked);
        CHECK(s.copies == copies_for(1, n), "frame %llu: %llu copies held (%llu wanted)", (unsigned long long)f, (unsigned long long)s.copies,
              (unsigned long long)copies_for(1, n));
    }
    CHECK(rollback_ring_oldest() == 1 && rollback_ring_newest() == 4, "frames 1-4 held");
    /* the newest put back over writes since */
    scribble(77);
    st64(MOD_IMAGE + MOD_CODE, 0x1234);
    CHECK(!as_at(4, why, sizeof why), "written since frame 4");
    CHECK(rollback_ring_restore(4) && as_at(4, why, sizeof why), "frame 4 put back: %s", why);
    rollback_ring_stats(&s);
    CHECK(s.put_back >= 10 && !s.unmapped && rollback_ring_newest() == 4, "%llu pages written back", (unsigned long long)s.put_back);
    /* an older one, the frames after it gone */
    scribble(78);
    CHECK(rollback_ring_restore(2) && as_at(2, why, sizeof why), "frame 2 put back: %s", why);
    CHECK(rollback_ring_newest() == 2 && rollback_ring_oldest() == 1 && !rollback_ring_restore(3) && !rollback_ring_restore(5), "frames 3 and 4 gone");
    rollback_ring_stats(&s);
    CHECK(s.copies == copies_for(1, 2), "the copies of frames 3 and 4 given back (%llu held, %llu wanted)", (unsigned long long)s.copies, (unsigned long long)copies_for(1, 2));
    CHECK(rollback_ring_restore(1) && as_at(1, why, sizeof why), "frame 1 put back: %s", why);
    CHECK(rollback_ring_restore(1) && as_at(1, why, sizeof why), "frame 1 again: %s", why);
    /* saves after a restore: numbered on from the last */
    scribble(5);
    u64 n = save();
    rollback_ring_stats(&s);
    CHECK(n == 5 && rollback_ring_oldest() == 1 && s.changed == changed_between(1, 5), "frame 5 after frame 1 (%llu changed)", (unsigned long long)s.changed);
    /* a svcMapMemory mapping, its destination mapped for one frame */
    CHECK(map(c, DST, HEAP_BASE + 0x200000, 0x4000) == R_OK, "a mapping");
    fill(DST, 0x4000, 6);
    u64 mapped = save();
    CHECK(unmap(c, DST, HEAP_BASE + 0x200000, 0x4000) == R_OK, "unmapped");
    u64 after = save();
    rollback_ring_stats(&s);
    CHECK(s.changed == 4 + 4, "the destination gone and the source's 4 pages changed (%llu)", (unsigned long long)s.changed);
    CHECK(rollback_ring_restore(mapped) && !as_at(mapped, why, sizeof why), "frame %llu put back without its mapping", (unsigned long long)mapped);
    rollback_ring_stats(&s);
    CHECK(s.unmapped == 4, "the destination's 4 pages left out (%llu)", (unsigned long long)s.unmapped);
    const u8 *src = page_at(&shots[mapped % 8], HEAP_BASE + 0x200000);
    CHECK(src && !memcmp(G2H(HEAP_BASE + 0x200000), src, PAGE), "the source as it was then");
    (void)after;
}

static void wrap_around(void) {
    char why[128];
    RingStats s;
    u64 last = 0, held = 0;
    for (int i = 0; i < 20; i++) {
        steady(100 + (u64)i);
        last = save();
        rollback_ring_stats(&s);
        if (i == 9) held = s.copies;
    }
    CHECK(rollback_ring_oldest() == last - 3 && rollback_ring_newest() == last, "frames %llu-%llu held", (unsigned long long)rollback_ring_oldest(),
          (unsigned long long)rollback_ring_newest());
    CHECK(s.copies == copies_for(last - 3, last), "%llu copies held (%llu wanted)", (unsigned long long)s.copies, (unsigned long long)copies_for(last - 3, last));
    CHECK(s.copies == held && s.frames == 4, "the copies steady past the ring's end (%llu after 10 more saves, %llu before)", (unsigned long long)s.copies,
          (unsigned long long)held);
    CHECK(!rollback_ring_restore(last - 4), "a dropped frame refused");
    scribble(200);
    CHECK(rollback_ring_restore(last) && as_at(last, why, sizeof why), "the newest put back: %s", why);
    CHECK(rollback_ring_restore(last - 3) && as_at(last - 3, why, sizeof why), "the oldest put back: %s", why);
    CHECK(rollback_ring_oldest() == last - 3 && rollback_ring_newest() == last - 3, "the oldest the only one left");
    rollback_ring_stats(&s);
    CHECK(s.copies == copies_for(last - 3, last - 3), "%llu copies held (%llu wanted)", (unsigned long long)s.copies, (unsigned long long)copies_for(last - 3, last - 3));
}

/* The renderer's news and the ring's: each told of every write, whichever of them took the
 * system's bits first. */
static int renderer_sees(u64 lo, u64 hi, u64 page, int reset) {
    void *pages[512];
    ULONG_PTR n = 512;
    ULONG gran = 0;
    if (kernel_write_watch(WATCH_RENDERER, reset ? WRITE_WATCH_FLAG_RESET : 0, G2H(lo), hi - lo, pages, &n, &gran) != 0) return -1;
    int seen = 0;
    for (ULONG_PTR k = 0; k < n; k++) seen |= pages[k] == G2H(page);
    return seen;
}

static ULONG_PTR renderer_count(u64 lo, u64 hi) {
    void *pages[512];
    ULONG_PTR n = 512;
    ULONG gran;
    return kernel_write_watch(WATCH_RENDERER, 0, G2H(lo), hi - lo, pages, &n, &gran) ? ~(ULONG_PTR)0 : n;
}

/* (a write reports every committed 4 KiB page of its host page: macOS's are 16 KiB) */
static u64 host_page_of(u64 a) { return a & ~(HOST_PAGE - 1); }

static void shared_watch(void) {
    renderer_sees(PHYS, PHYS + PHYS_SIZE, 0, 1);   /* (its news so far taken) */
    save();
    CHECK(renderer_count(PHYS, PHYS + PHYS_SIZE) == 0, "nothing new for the renderer");
    fill(PHYS + 3 * PAGE, PAGE, 300);
    fill(PHYS + 9 * PAGE, PAGE, 301);
    RingStats s;
    save();
    rollback_ring_stats(&s);
    CHECK(s.changed == 2, "the ring took the system's bits for the two pages (%llu changed)", (unsigned long long)s.changed);
    CHECK(renderer_sees(PHYS, PHYS + PHYS_SIZE, PHYS + 3 * PAGE, 0) == 1 && renderer_sees(PHYS, PHYS + PHYS_SIZE, PHYS + 9 * PAGE, 0) == 1,
          "the renderer still told of both");
    void *first;
    ULONG_PTR one = 1;
    ULONG gran;
    CHECK(!kernel_write_watch(WATCH_RENDERER, 0, G2H(PHYS), PHYS_SIZE, &first, &one, &gran) && one == 1 && first == G2H(host_page_of(PHYS + 3 * PAGE)) &&
          gran == PAGE, "asked for one: the first, in 4 KiB pages");
    CHECK(renderer_sees(PHYS, PHYS + PHYS_SIZE, PHYS + 9 * PAGE, 1) == 1 && renderer_sees(PHYS, PHYS + PHYS_SIZE, PHYS + 3 * PAGE, 0) == 0,
          "taken with a reset: gone from the renderer's");
    fill(PHYS + 5 * PAGE, PAGE, 302);
    CHECK(renderer_sees(PHYS, PHYS + PHYS_SIZE, PHYS + 5 * PAGE, 1) == 1, "the renderer takes a write first");
    save();
    rollback_ring_stats(&s);
    CHECK(s.written == HOST_PAGE / PAGE && s.changed == 1, "the ring told of it too (%llu written, %llu changed)", (unsigned long long)s.written,
          (unsigned long long)s.changed);
    void *pages[4];
    ULONG_PTR n = 4;
    CHECK(kernel_write_watch(WATCH_RENDERER, 0, G2H(HEAP_BASE), PAGE, pages, &n, &gran) != 0, "the heap is no area of the renderer's");
}

/* The renderer taking the system's bits on a thread of its own while frames are written and
 * saved: each of them told of every page written, whichever took the bits first. */
static volatile long renderer_stop;
static u8 renderer_saw[PHYS_SIZE / PAGE];
static void renderer_take(void) {
    void *pages[512];
    ULONG_PTR n = 512;
    ULONG gran;
    if (kernel_write_watch(WATCH_RENDERER, WRITE_WATCH_FLAG_RESET, G2H(PHYS), PHYS_SIZE, pages, &n, &gran)) return;
    for (ULONG_PTR k = 0; k < n; k++) renderer_saw[(H2G(pages[k]) - PHYS) / PAGE] = 1;
}
static DWORD WINAPI renderer_main(LPVOID unused) {
    (void)unused;
    while (!__atomic_load_n(&renderer_stop, __ATOMIC_ACQUIRE)) renderer_take();
    return 0;
}

static void together(void) {
    save();
    renderer_take();
    memset(renderer_saw, 0, sizeof renderer_saw);
    HANDLE t = CreateThread(NULL, 0, renderer_main, NULL, 0, NULL);
    u64 changed = 0;
    RingStats s;
    for (u64 round = 0; round < 24; round++) {
        for (u64 i = 0; i < 8; i++) fill(PHYS + (round * 8 + i) * PAGE, PAGE, 500 + round);
        save();
        rollback_ring_stats(&s);
        changed += s.changed;
    }
    __atomic_store_n(&renderer_stop, 1, __ATOMIC_RELEASE);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    renderer_take();
    u32 missed = 0;
    for (u64 i = 0; i < 24 * 8; i++) missed += !renderer_saw[i];
    CHECK(!missed, "the renderer told of all 192 pages written (%u missed)", missed);
    CHECK(changed == 24 * 8, "the ring found all 192 changed (%llu)", (unsigned long long)changed);
}

/* Alias memory unmapped and mapped again between two saves: zeros the ring sees (the system
 * forgets a decommitted page's bit), and its pattern put back; unmapped now, left out. */
static void decommitted(Cpu *c) {
    char why[128];
    u64 at = PHYS + PHYS_SIZE - 0x10000;
    fill(at, 0x10000, 400);
    u64 before = save();
    CHECK(unmap_physical(c, at, 0x10000) == R_OK && map_physical(c, at, 0x10000) == R_OK, "unmapped and mapped again");
    CHECK(zero_page(G2H(at)), "zeros now");
    RingStats s;
    save();
    rollback_ring_stats(&s);
    CHECK(s.changed == 16, "its 16 pages changed (%llu)", (unsigned long long)s.changed);
    CHECK(rollback_ring_restore(before) && as_at(before, why, sizeof why), "the frame before put back: %s", why);
    CHECK(unmap_physical(c, at, 0x10000) == R_OK, "unmapped");
    CHECK(rollback_ring_restore(before), "put back again");
    rollback_ring_stats(&s);
    CHECK(s.unmapped == 16 && !s.put_back, "its 16 pages left out (%llu), nothing else written (%llu)", (unsigned long long)s.unmapped, (unsigned long long)s.put_back);
    /* memory mapped after a frame: zeros when it is put back, as when new */
    u64 more = PHYS + PHYS_SIZE + 0x40000;
    CHECK(map_physical(c, more, 0x8000) == R_OK, "more mapped");
    fill(more, 0x8000, 401);
    u64 grown = save();
    fill(more, 0x8000, 402);
    CHECK(rollback_ring_restore(grown) && as_at(grown, why, sizeof why), "the frame with it put back: %s", why);
    CHECK(rollback_ring_restore(before), "the frame before it put back");
    rollback_ring_stats(&s);
    int zeros = 1;
    for (u64 p = more; p < more + 0x8000; p += PAGE) zeros &= zero_page(G2H(p));
    CHECK(zeros && s.put_back == 8 && !s.unmapped, "its 8 pages zeros (%llu written)", (unsigned long long)s.put_back);
}

/* ---- as in a run: the handler the ring registered, at each frame end ------------------------ */
static void frame_ends(Cpu *c) {
    u64 args[8] = {0};
    CHECK(hooks_added == 1 && hook_after, "the handler registered");
    if (!hook_after) return;
    for (u64 i = 0; i < 301; i++) {
        steady(i);
        hook_after(c, args, NULL);
    }
    RingStats s;
    rollback_ring_stats(&s);
    CHECK(s.saves == 301 && s.frames == 3 && rollback_ring_newest() == 301 && rollback_ring_oldest() == 299 && !s.restores,
          "301 saves, the last 3 frames held, none put back (%llu saves)", (unsigned long long)s.saves);
    CHECK(s.changed == 10, "the last save: the 10 pages written with new contents changed (%llu)", (unsigned long long)s.changed);
}

/* ---- without the switch --------------------------------------------------------------------- */
static void set_switch(const char *v) {
#ifdef _WIN32
    _putenv_s("SSBU_ROLLBACK_RING", v ? v : "");
#else
    if (v) setenv("SSBU_ROLLBACK_RING", v, 1);
    else unsetenv("SSBU_ROLLBACK_RING");
#endif
}

static void switch_off(Cpu *c) {
    CHECK(!hooks_added && !ring_switch_frames, "nothing registered");
    CHECK(!kernel_watch_shared && !nwatch_areas, "the write watch the system's own");
    CHECK(map_physical(c, PHYS, PHYS_SIZE) == R_OK, "svcMapPhysicalMemory");
    ResetWriteWatch(G2H(PHYS), PHYS_SIZE);
    fill(PHYS + 2 * PAGE, PAGE, 1);
    void *pages[8];
    ULONG_PTR n = 8;
    ULONG gran;
    CHECK(!kernel_write_watch(WATCH_RENDERER, WRITE_WATCH_FLAG_RESET, G2H(PHYS), PHYS_SIZE, pages, &n, &gran) && n == HOST_PAGE / PAGE &&
          pages[0] == G2H(host_page_of(PHYS + 2 * PAGE)), "the renderer told by the system (%lu pages)", (unsigned long)n);
    n = 8;
    CHECK(!GetWriteWatch(0, G2H(PHYS), PHYS_SIZE, pages, &n, &gran) && n == 0, "and the system's bit reset by it");
    n = 8;
    CHECK(GetWriteWatch(0, G2H(HEAP_BASE), PAGE, pages, &n, &gran) != 0, "the heap not watched");
    KernelRange r[64];
    u32 k = kernel_writable_ranges(r, 64);
    int none = 1;
    for (u32 i = 0; i < k; i++) none &= !r[i].watched;
    CHECK(k >= 4 && none, "%u writable ranges, none watched", k);
    static const struct { const char *v; int frames; } values[] = {
        {"1", 8}, {"2", 2}, {"64", 64}, {"16", 16}, {"0", 0}, {"65", 0}, {"yes", 0}, {"", 0}, {" 4", 0}, {"4x", 0}, {"-3", 0},
    };
    for (u32 i = 0; i < sizeof values / sizeof *values; i++) {
        set_switch(values[i].v);
        CHECK(kernel_ring_frames() == values[i].frames, "SSBU_ROLLBACK_RING=\"%s\": %d frames (%d)", values[i].v, values[i].frames, kernel_ring_frames());
    }
    set_switch(NULL);
    CHECK(kernel_ring_frames() == 0, "unset: off");
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "";
    int on = strcmp(mode, "off") != 0;
    setvbuf(stdout, NULL, _IONBF, 0);
#ifdef __APPLE__
    if (!rt_guest_window()) { printf("cannot reserve the guest's address space\n"); return 2; }
#endif
#ifndef _WIN32
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
#endif
    kernel_init();
    u32 main_handle;
    Cpu *c = kernel_main_thread(&main_handle);
    c->x[1] = HEAP;
    CHECK(svc(c, 0x01) == R_OK, "svcSetHeapSize");
    if (!on) {
        switch_off(c);
    } else {
        if (strcmp(mode, "run")) switch_read();
        CHECK(map_physical(c, PHYS, PHYS_SIZE) == R_OK, "svcMapPhysicalMemory");
        if (VirtualAlloc(G2H(MOD_IMAGE), MOD_CODE + MOD_DATA, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != G2H(MOD_IMAGE)) {
            printf("cannot map a module image at %#llx\n", MOD_IMAGE);
            return 2;
        }
        kernel_add_region(MOD_IMAGE, MOD_CODE, GM_CODE, 5);
        kernel_add_region(MOD_IMAGE + MOD_CODE, MOD_DATA, GM_CODE_DATA, 3);
        fill(HEAP_BASE, HEAP / 2, 1);
        fill(PHYS, PHYS_SIZE / 2, 2);
        fill(MOD_IMAGE + MOD_CODE, MOD_DATA, 3);
        st64(c->tpidrro + 0x200, 0x7E57);
        if (!strcmp(mode, "run")) {
            frame_ends(c);
        } else {
            frames_and_restores(c);
            wrap_around();
            shared_watch();
            together();
            decommitted(c);
        }
    }
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
