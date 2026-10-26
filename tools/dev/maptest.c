/* Offline checks of the native kernel's memory mappings (runtime/kernel.c's svcMapMemory and
 * svcUnmapMemory), through its system calls, with no game.
 *
 *   Windows: clang-cl /nologo /O1 tools\dev\maptest.c /Febuild\maptest.exe   (with cc.BASE_CFLAGS)
 *   Linux:   clang -O1 -I runtime/macos/include -I runtime/linux/include -I runtime tools/dev/maptest.c runtime/linux/win32.c -lpthread -o build/maptest
 *   macOS:   the same with runtime/macos/memory.c and runtime/macos/platform.c in place of -lpthread
 *   (runtime/macos/include: the Windows types without the Linux build's Direct3D headers)
 *
 *   maptest
 *       a mapping's pages at its destination, written there and read back, and in its source after
 *       the unmap; the destination then free (svcQueryMemory) and decommitted, and the renderer
 *       told of it once (gpu_memory_unmapping); two mappings side by side, and two from
 *       neighbouring sources, unmapped one at a time; 3000 mappings made and unmapped in turn
 *       (more than the 1024 slots); a source in the alias area, its copy-back seen by the write
 *       watch; sources in a made-up module's code and data; an unmap of nothing mapped; the
 *       status line's counts and the copies' time; the source accessible while mapped, as before,
 *       and kernel_read64 reading it
 *   SSBU_MAP_GUARD=1 maptest guard
 *       the same, with the source inaccessible while mapped (on macOS its 16 KiB pages wholly
 *       inside it), its neighbours accessible and unchanged, and the source accessible again, with
 *       the writes, after the unmap; kernel_read64 refusing the source (it would fault) while it is
 *       mapped; of two neighbouring sources, unmapping one leaves the other guarded; a source in the
 *       alias area, in a module's code, across its code and data, from its data into a stretch the
 *       memory map does not have, or into read-only memory left accessible, one in its data
 *       guarded; the unmap of an unguarded mapping leaving guarded a source that its slot's earlier
 *       mapping had guarded (and another mapping guards now)
 *   (SSBU_MAP_GUARD unset, or another value: the first)
 *
 * Exit code 0 when every check passes. tests/test_kernel_map_memory.py builds and runs both. */
#include <stdarg.h>
#include "../../runtime/kernel.c"
#include "../../runtime/clock.h"
#ifndef _WIN32
#include <setjmp.h>
#include <signal.h>
#endif

/* ---- what kernel.c needs from the rest of the runtime ---------------------------------- */
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
static u64 told_addr, told_size;
static int told;
void gpu_memory_unmapping(u64 addr, u64 size) { told_addr = addr; told_size = size; told++; }
#ifdef __APPLE__
void rt_upnp_remove(void) {}
#endif

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } } while (0)

/* ---- is a guest page accessible? (a read, and the byte written back) ------------------------ */
#ifdef _WIN32
static __declspec(noinline) void touch(volatile u8 *p) { *p = *p; }
static int accessible(u64 a) {
    __try {
        touch(G2H(a));   /* (a call of its own: clang-cl catches a fault inside __try only in a callee; in place, the __try is compiled away) */
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 1;
}
#else
int rt_ww_fault(void *address);   /* (win32.c, macos/memory.c) */
static sigjmp_buf probe_out;
static volatile int probing;

static void on_fault(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    if (rt_ww_fault(si->si_addr)) return;   /* (a write the write watch takes, as host.c's handler has it) */
    if (probing) siglongjmp(probe_out, 1);
    signal(sig, SIG_DFL);                    /* (anything else: the crash it is) */
}

static int accessible(u64 a) {
    int ok = 0;
    probing = 1;
    if (!sigsetjmp(probe_out, 1)) {
        volatile u8 *p = G2H(a);
        *p = *p;
        ok = 1;
    }
    probing = 0;
    return ok;
}
#endif

/* ---- guest memory and system calls ---------------------------------------------------------- */
#define INFO (HEAP_BASE)                       /* svcQueryMemory's answer */
#define SRC (HEAP_BASE + 0x100000)             /* sources, from here up */
#define DST (STACK_BASE + 0x1000000)           /* destinations, from here up */
#define PAGE 0x1000ull
static int guard;                              /* the mode asked for: maptest guard */

static u64 svc(Cpu *c, u32 n) { rt_svc(c, n); return (u32)c->x[0]; }
static u32 map(Cpu *c, u64 dst, u64 src, u64 size) { c->x[0] = dst; c->x[1] = src; c->x[2] = size; return (u32)svc(c, 0x04); }
static u32 unmap(Cpu *c, u64 dst, u64 src, u64 size) { c->x[0] = dst; c->x[1] = src; c->x[2] = size; return (u32)svc(c, 0x05); }
static u32 query(Cpu *c, u64 addr, u64 *base, u64 *size) {   /* -> state */
    c->x[0] = INFO; c->x[2] = addr;
    svc(c, 0x06);
    *base = ld64(INFO); *size = ld64(INFO + 8);
    return ld32(INFO + 16);
}

/* Made-up contents: each word from its offset and a salt. */
static u64 word(u64 off, u64 salt) { return (off + 1) * 0x9E3779B97F4A7C15ull ^ salt; }
static void fill(u64 at, u64 size, u64 salt) { for (u64 off = 0; off < size; off += 8) st64(at + off, word(off, salt)); }
static int holds(u64 at, u64 size, u64 salt) {
    for (u64 off = 0; off < size; off += 8)
        if (ld64(at + off) != word(off, salt)) return 0;
    return 1;
}

/* Pages of [at, at + size) inaccessible: their count (each probed). */
static u32 closed(u64 at, u64 size) {
    u32 n = 0;
    for (u64 p = at; p < at + size; p += PAGE) n += !accessible(p);
    return n;
}

/* ---- the checks ------------------------------------------------------------------------------ */
static void one_mapping(Cpu *c) {
    u64 src = SRC, dst = DST, size = 0x10000, base, len;
    fill(src - PAGE, PAGE, 7);                  /* (the neighbours, which nothing may touch) */
    fill(src + size, PAGE, 8);
    fill(src, size, 1);
    int before = told;
    CHECK(map(c, dst, src, size) == R_OK, "svcMapMemory");
    CHECK(holds(dst, size, 1), "the source's contents at the destination");
    CHECK(query(c, dst, &base, &len) == GM_STACK && base == dst && len == size && ld32(INFO + 24) == 3,
          "svcQueryMemory at the destination: state %#x, %#llx + %#llx", ld32(INFO + 16), (unsigned long long)base, (unsigned long long)len);
    CHECK(closed(src, size) == (guard ? size / PAGE : 0), "%u of the source's 16 pages inaccessible while mapped", closed(src, size));
    CHECK(!closed(src - PAGE, PAGE) && !closed(src + size, PAGE), "the source's neighbours accessible");
    u64 v = 0;   /* (kernel_read64, hle_fiber.c's look into memory: a guarded source is not there) */
    CHECK(kernel_read64(src + 0x100, &v) == !guard && kernel_read64(src - 4, &v) == !guard, "kernel_read64 of the source%s", guard ? " refused" : "");
    CHECK(kernel_read64(src - 8, &v) && v == word(PAGE - 8, 7), "kernel_read64 of its neighbour");
    CHECK(!closed(dst, size), "the destination accessible");
    fill(dst, size, 2);
    CHECK(holds(dst, size, 2), "written at the destination, read back there");
    CHECK(unmap(c, dst, src, size) == R_OK, "svcUnmapMemory");
    CHECK(!closed(src, size) && holds(src, size, 2), "the source accessible again, with what was written at the destination");
    CHECK(kernel_read64(src + 0x100, &v) && v == word(0x100, 2), "kernel_read64 of the source after the unmap");
    CHECK(holds(src - PAGE, PAGE, 7) && holds(src + size, PAGE, 8), "the source's neighbours unchanged");
    CHECK(query(c, dst, &base, &len) == GM_FREE, "svcQueryMemory at the destination after the unmap: state %#x", ld32(INFO + 16));
    CHECK(closed(dst, size) == size / PAGE, "the destination decommitted (%u of 16 pages inaccessible)", closed(dst, size));
    CHECK(told == before + 1 && told_addr == dst && told_size == size, "the renderer told of the unmap once, with the destination (%d times, %#llx + %#llx)",
          told - before, (unsigned long long)told_addr, (unsigned long long)told_size);
    /* again from the same source, elsewhere: what it holds now goes */
    CHECK(map(c, dst + 0x100000, src, size) == R_OK && holds(dst + 0x100000, size, 2), "mapped again, elsewhere");
    fill(dst + 0x100000, size, 3);
    CHECK(unmap(c, dst + 0x100000, src, size) == R_OK && holds(src, size, 3), "unmapped again, the second writes in the source");
}

/* A source 4 KiB- but not 16 KiB-aligned: on macOS only the host pages wholly inside it are guarded. */
static void offset_source(Cpu *c) {
    u64 src = SRC + 0x101000, dst = DST + 0x200000, size = 0x9000;   /* [+0x1000, +0xA000) of a 16 KiB-aligned stretch */
    fill(src - PAGE, PAGE, 17);
    fill(src + size, PAGE, 18);
    fill(src, size, 11);
    CHECK(map(c, dst, src, size) == R_OK && holds(dst, size, 11), "a source at a 4 KiB offset mapped");
#ifdef __APPLE__
    u32 want = guard ? 4 : 0;                   /* [+0x4000, +0x8000): the one host page inside it */
    CHECK(!guard || (accessible(src) && accessible(src + 0x2000) && !accessible(src + 0x3000) && !accessible(src + 0x6000) && accessible(src + 0x7000)),
          "the pages of the host pages it shares accessible, the others not");
#else
    u32 want = guard ? 9 : 0;
#endif
    CHECK(closed(src, size) == want, "%u of the source's 9 pages inaccessible while mapped (%u expected)", closed(src, size), want);
    CHECK(!closed(src - PAGE, PAGE) && !closed(src + size, PAGE), "its neighbours accessible");
    fill(dst, size, 12);
    CHECK(unmap(c, dst, src, size) == R_OK && !closed(src, size) && holds(src, size, 12), "unmapped: the source accessible, with the writes");
    CHECK(holds(src - PAGE, PAGE, 17) && holds(src + size, PAGE, 18), "its neighbours unchanged");
}

/* Destinations side by side (one memory-map region), sharing a 16 KiB host page. */
static void side_by_side(Cpu *c) {
    u64 a = DST + 0x301000, b = a + 0x6000, sa = SRC + 0x200000, sb = SRC + 0x210000, base, len;
    fill(sa, 0x6000, 21);
    fill(sb, 0x5000, 22);
    CHECK(map(c, a, sa, 0x6000) == R_OK && map(c, b, sb, 0x5000) == R_OK, "two mappings side by side");
    CHECK(query(c, a, &base, &len) == GM_STACK && base == a && len == 0xB000, "one region of the memory map (%#llx + %#llx)",
          (unsigned long long)base, (unsigned long long)len);
    fill(a, 0x6000, 23);
    fill(b, 0x5000, 24);
    CHECK(unmap(c, a, sa, 0x6000) == R_OK, "the first unmapped");
    CHECK(query(c, a, &base, &len) == GM_FREE && query(c, b, &base, &len) == GM_STACK && base == b && len == 0x5000,
          "the second still in the memory map, alone (%#llx + %#llx)", (unsigned long long)base, (unsigned long long)len);
    CHECK(!closed(b, 0x5000) && holds(b, 0x5000, 24), "the second's pages kept, with what was written there");
    CHECK(holds(sa, 0x6000, 23), "the first's source has its writes");
    CHECK(unmap(c, b, sb, 0x5000) == R_OK && holds(sb, 0x5000, 24) && query(c, b, &base, &len) == GM_FREE, "the second unmapped");
}

/* Sources side by side, sharing a 16 KiB host page: unmapping one leaves the other guarded. */
static void neighbouring_sources(Cpu *c) {
    u64 sa = SRC + 0x301000, sb = sa + 0xA000, da = DST + 0x400000, db = DST + 0x500000;   /* [+0x1000, +0xB000) and [+0xB000, +0x14000) */
    fill(sa, 0xA000, 31);
    fill(sb, 0x9000, 32);
    CHECK(map(c, da, sa, 0xA000) == R_OK && map(c, db, sb, 0x9000) == R_OK, "two mappings from neighbouring sources");
    CHECK(holds(da, 0xA000, 31) && holds(db, 0x9000, 32), "each source's contents at its destination");
    fill(da, 0xA000, 33);
    fill(db, 0x9000, 34);
    CHECK(unmap(c, da, sa, 0xA000) == R_OK && holds(sa, 0xA000, 33), "the first unmapped, its source with its writes");
#ifdef __APPLE__
    u32 want = guard ? 8 : 0;                   /* [+0xC000, +0x14000); the host page it shares with the first stays open */
#else
    u32 want = guard ? 9 : 0;
#endif
    CHECK(closed(sb, 0x9000) == want, "the second's source still guarded: %u of 9 pages inaccessible (%u expected)", closed(sb, 0x9000), want);
    CHECK(unmap(c, db, sb, 0x9000) == R_OK && !closed(sb, 0x9000) && holds(sb, 0x9000, 34), "the second unmapped, its source with its writes");
}

/* More mappings, one after another, than there are slots. */
static void many(Cpu *c) {
    int bad = 0;
    for (u64 i = 0; i < 3000; i++) {
        u64 src = SRC + 0x400000 + (i % 4) * 0x4000, dst = DST + 0x600000 + (i % 3) * 0x8000;
        if (map(c, dst, src, 0x4000) != R_OK) bad++;
        if (guard && accessible(src)) bad++;
        st64(dst + 0x3FF8, i);
        if (unmap(c, dst, src, 0x4000) != R_OK || ld64(src + 0x3FF8) != i) bad++;
    }
    CHECK(!bad, "3000 mappings made and unmapped in turn: %d wrong", bad);
}

/* A source in the alias area (physical memory, its writes watched): never guarded; the copy-back is a write the watch sees. */
static void alias_source(Cpu *c) {
    u64 phys = ALIAS_BASE + 0x100000, src = phys + 0x4000, dst = DST + 0x700000, size = 0x8000;
    c->x[0] = phys; c->x[1] = 0x20000;
    CHECK(svc(c, 0x2C) == R_OK, "svcMapPhysicalMemory");
    fill(src, size, 41);
    CHECK(map(c, dst, src, size) == R_OK && holds(dst, size, 41), "a source in the alias area mapped");
    CHECK(!closed(src, size), "an alias-area source accessible while mapped");
    ResetWriteWatch(G2H(phys), 0x20000);   /* (after the probes' writes) */
    fill(dst, size, 42);
    CHECK(unmap(c, dst, src, size) == R_OK && holds(src, size, 42), "unmapped: the source has the writes");
    void *pages[32];
    ULONG_PTR n = 32;
    ULONG gran = 0;
    int all = !GetWriteWatch(0, G2H(phys), 0x20000, pages, &n, &gran);
    for (u64 p = src; all && p < src + size; p += PAGE) {
        int seen = 0;
        for (ULONG_PTR k = 0; k < n; k++) seen |= pages[k] == G2H(p);
        all = seen;
    }
    CHECK(all, "the write watch saw the copy-back (%lu pages reported)", (unsigned long)n);
    /* an unguarded mapping in a slot a guarded one had: its unmap leaves alone a source guarded now */
    u64 heap = SRC + 0x500000, hdst = DST + 0x780000;
    CHECK(map(c, hdst, heap, 0x4000) == R_OK && unmap(c, hdst, heap, 0x4000) == R_OK, "a heap source mapped and unmapped");
    CHECK(map(c, dst, src, size) == R_OK && map(c, hdst, heap, 0x4000) == R_OK, "the alias-area source in its slot, the heap one in the next");
    CHECK(unmap(c, dst, src, size) == R_OK && closed(heap, 0x4000) == (guard ? 4 : 0), "the alias-area one unmapped: the heap source as it was (%u pages inaccessible)",
          closed(heap, 0x4000));
    CHECK(unmap(c, hdst, heap, 0x4000) == R_OK && !closed(heap, 0x4000), "the heap one unmapped");
    int before = told;
    c->x[0] = phys; c->x[1] = 0x20000;
    CHECK(svc(c, 0x2D) == R_OK && told == before + 1 && told_addr == phys, "svcUnmapPhysicalMemory, the renderer told");
}

/* A source in a module's image (a static stack in its .bss, say): guarded in its data, which the memory
 * map has as read-write, not where any of it is not: in its code, across code and data, from data
 * into a stretch the memory map does not have (committed, so the copy can read it) and on into more
 * data, or from data into read-only memory. */
#define MOD_IMAGE 0x7100000000ull
static void module_source(Cpu *c) {
    if (VirtualAlloc(G2H(MOD_IMAGE), 0x60000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != G2H(MOD_IMAGE)) { CHECK(0, "a module image at %#llx", MOD_IMAGE); return; }
    kernel_add_region(MOD_IMAGE, 0x20000, GM_CODE, 5);                   /* (as the loader has them) */
    kernel_add_region(MOD_IMAGE + 0x20000, 0x20000, GM_CODE_DATA, 3);
    kernel_add_region(MOD_IMAGE + 0x50000, 0x8000, GM_CODE_DATA, 3);    /* (after a gap: another module's, say) */
    kernel_add_region(MOD_IMAGE + 0x58000, 0x8000, GM_CODE, 1);
    static const struct { u64 src, size; u32 pages; const char *what; } cases[] = {
        {MOD_IMAGE + 0x28000, 0x8000, 8, "data"}, {MOD_IMAGE + 0x8000, 0x8000, 0, "code"}, {MOD_IMAGE + 0x1C000, 0x8000, 0, "code and data"},
        {MOD_IMAGE + 0x3C000, 0x18000, 0, "data, a gap and data"}, {MOD_IMAGE + 0x54000, 0x8000, 0, "data and read-only memory"},
    };
    for (u32 k = 0; k < 5; k++) {
        u64 src = cases[k].src, size = cases[k].size, dst = DST + 0x800000 + k * 0x20000;
        fill(src, size, 51 + k);
        CHECK(map(c, dst, src, size) == R_OK && holds(dst, size, 51 + k), "a source in a module's %s mapped", cases[k].what);
        CHECK(closed(src, size) == (guard ? cases[k].pages : 0), "%u pages of a source in a module's %s inaccessible", closed(src, size), cases[k].what);
        fill(dst, size, 61 + k);
        CHECK(unmap(c, dst, src, size) == R_OK && !closed(src, size) && holds(src, size, 61 + k), "unmapped, the source with the writes");
    }
}

int main(int argc, char **argv) {
    guard = argc > 1 && !strcmp(argv[1], "guard");
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
    CHECK(map_guard == guard, "SSBU_MAP_GUARD read as %d", map_guard);
    u32 main_handle;
    Cpu *c = kernel_main_thread(&main_handle);
    c->x[1] = 16 << 20;
    CHECK(svc(c, 0x01) == R_OK, "svcSetHeapSize");
    one_mapping(c);
    offset_source(c);
    side_by_side(c);
    neighbouring_sources(c);
    many(c);
    alias_source(c);
    module_source(c);
    int before = told;
    CHECK(unmap(c, DST + 0x900000, SRC, 0x4000) == R_OK && told == before, "an unmap of nothing mapped: nothing done");
    CHECK(maps_made == 3016 && maps_live == 0 && maps_peak == 2 &&
          map_bytes == 0x10000 * 2 + 0x9000 + 0xB000 + 0x13000 + 3000 * 0x4000 + 0x8000 * 2 + 0x4000 * 2 + 0x8000 * 4 + 0x18000,
          "the counts: %llu made, %u live, at most %u, %llu bytes", (unsigned long long)maps_made, maps_live, maps_peak, (unsigned long long)map_bytes);
    CHECK(maps_guarded == (guard ? 3010 : 0), "%llu sources guarded", (unsigned long long)maps_guarded);
    CHECK(map_copy_ns > 0, "the copies' time counted (%llu ns)", (unsigned long long)map_copy_ns);
    kernel_map_status();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
