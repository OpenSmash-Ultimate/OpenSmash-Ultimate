/* Checks of the macOS platform layer: the Windows API of runtime/linux/include/windows.h as the
 * macOS build has it (runtime/linux/win32.c with runtime/macos/'s memory, fibers and platform
 * code) and the profiler (runtime/macos/profile.c) - what differs from Linux, on this Mac:
 *   - the guest's address space: its window and moats, reserving and committing at guest
 *     addresses, 4 KiB decommits on 16 KiB pages (zeros on recommit, neighbours kept);
 *   - the write watch: faults, exactly the written 4 KiB pages reported (all of a host page's
 *     committed ones, where one was written), resets re-arming, writes by system calls (ReadFile
 *     against a thread re-arming its pages all the while), a 4 KiB decommit writing nothing, and
 *     no write lost to a reset racing it (four writers against a reader resetting);
 *   - fibers: ping-pong, a thousand made and deleted, one resumed on another thread, FPCR kept,
 *     backtrace() and the frame walk inside one;
 *   - waits: condition variables (both ways of waiting on an address), events, waitable timers,
 *     with timeouts that take as long as they should;
 *   - threads: ids, names, QoS classes; CRITICAL_SECTION's owner through SleepConditionVariableCS;
 *   - files and FindFirstFile; the clock; a fault's report (signal, ESR, address, frames); the
 *     frame walk over made-up chains (one record, none, a record ending it);
 *   - UPnP's requests (runtime/linux/upnp.c): an answer that takes a while, and a router that
 *     does not answer at all given up on in seconds, not TCP's 75;
 *   - the profiler: every thread's stack (named from ssbu.syms beside the program), a sampling
 *     report, the hitch log.
 * Built with -DSHIMTEST_HOST and the loader and kernel (runtime/host.c, kernel.c, hle_hooks.c, linux/upnp.c),
 * it also starts a made-up game in a child process the way the game starts (guest window,
 * modules mapped at guest addresses, kernel areas, the guest main thread), checks its memory
 * and system calls, and lets it crash on purpose: the crash report must name the guest
 * function and the address it touched.
 *
 * Build and run, from the repository root (the game's flags: tools/mac/toolchain.py):
 *   F="-arch arm64 -mmacosx-version-min=14.0 -mcpu=apple-m1 -fno-strict-aliasing -ffp-contract=off -fno-stack-protector
 *      -fvisibility=hidden -O2 -I runtime/macos/include -I runtime/linux/include -I runtime"
 *   S="runtime/linux/win32.c runtime/macos/memory.c runtime/macos/fiber.c runtime/macos/fiber_arm64.S
 *      runtime/macos/platform.c runtime/macos/profile.c"
 *   mkdir -p build-mac/shimtest
 *   clang $F -Dmain=rt_host_main -c runtime/host.c -o build-mac/shimtest/host.o
 *   clang $F -DSHIMTEST_HOST tools/mac/shimtest.c build-mac/shimtest/host.o runtime/kernel.c runtime/hle_hooks.c \
 *       runtime/linux/upnp.c $S -o build-mac/shimtest/shimtest
 *   nm -n --defined-only build-mac/shimtest/shimtest > build-mac/shimtest/ssbu.syms   (the profiler's names)
 *   (cd build-mac/shimtest && ./shimtest)
 * or without the loader and kernel: clang $F tools/mac/shimtest.c $S -o build-mac/shimtest/shimtest. Exit
 * status 0 when every check passes; each failure is a FAIL line. SHIMTEST_VERBOSE=1 shows the
 * thread dump, profile, hitch log and the made-up game's output; SHIMTEST_KEEP=1 keeps its folder. */
#include <dirent.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/mach_vm.h>
#include <mach-o/ldsyms.h>
#include <pthread/qos.h>
#include <setjmp.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <windows.h>

#include "recomp.h"
#include "debug_profile.h"

extern char **environ;

/* (runtime/macos: not in windows.h, the shim's own) */
int rt_ww_fault(void *address);
const char *rt_futex_mode(void);

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } } while (0)

#define G(a) ((u8 *)(uintptr_t)((u64)(a) + RT_MEMBASE))   /* a guest address's host memory */

static double now_ms(void) {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

static const char *name_of(void *pc) {   /* the symbol at pc, or "" */
    Dl_info di;
    return dladdr(pc, &di) && di.dli_sname ? di.dli_sname : "";
}

/* ---- faults: write-watch writes go on; probes come back; anything else is reported ------- */
static struct { int sig; u64 esr, far, pc, lr, fp; } last_fault;
static sigjmp_buf *volatile probe_jmp;
static pthread_t probe_thread;
static volatile int ww_faults;

static void on_fault(int sig, siginfo_t *si, void *ucv) {
    (void)si;
    ucontext_t *uc = ucv;
    u64 esr = uc->uc_mcontext->__es.__esr, far = uc->uc_mcontext->__es.__far;
    int writing = ((esr >> 26) == 0x24 || (esr >> 26) == 0x25) && (esr & (1u << 6));
    if ((sig == SIGBUS || sig == SIGSEGV) && writing && rt_ww_fault((void *)(uintptr_t)far)) {
        __atomic_add_fetch(&ww_faults, 1, __ATOMIC_RELAXED);
        return;
    }
    if (probe_jmp && pthread_equal(pthread_self(), probe_thread)) {
        last_fault.sig = sig;
        last_fault.esr = esr;
        last_fault.far = far;
        last_fault.pc = uc->uc_mcontext->__ss.__pc;
        last_fault.lr = uc->uc_mcontext->__ss.__lr;
        last_fault.fp = uc->uc_mcontext->__ss.__fp;
        siglongjmp(*probe_jmp, 1);
    }
    static const char text[] = "shimtest: an unexpected fault\n";
    write(2, text, sizeof text - 1);
    _exit(99);
}

static void install_fault_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}

/* Whether a byte can be read (or written: the same value back) without a fault. */
static int probe(volatile u8 *p, int write) {
    sigjmp_buf jb;
    if (sigsetjmp(jb, 1)) { probe_jmp = NULL; return 0; }
    probe_thread = pthread_self();
    probe_jmp = &jb;
    u8 v = *p;
    if (write) *p = v;
    probe_jmp = NULL;
    return 1;
}
#define READABLE(p) probe((volatile u8 *)(p), 0)
#define WRITABLE(p) probe((volatile u8 *)(p), 1)

static int all_are(const u8 *p, size_t n, u8 v) {
    for (size_t i = 0; i < n; i++) if (p[i] != v) return 0;
    return 1;
}

/* The VM region at or after `at`: its start and size, protections; 0 if none. */
static int region(u64 at, u64 *start, u64 *size, int *prot, int *max) {
    mach_vm_address_t a = at;
    mach_vm_size_t s = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t n = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &a, &s, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &n, &object) != KERN_SUCCESS) return 0;
    *start = a; *size = s; *prot = info.protection; *max = info.max_protection;
    return 1;
}

/* ---- the guest's window and its memory ---------------------------------------------------- */
static void test_window(void) {
    CHECK(rt_guest_window() == 1, "the guest window was not reserved");
    CHECK(rt_guest_window() == 1, "reserving the guest window twice failed");
    u64 start, size;
    int prot, max;
    CHECK(region(RT_MEMBASE, &start, &size, &prot, &max) && start == RT_MEMBASE && prot == 0 && max == 3,
          "the window: region at %#llx size %#llx prot %d/%d", start, size, prot, max);
    CHECK(region(RT_MEMBASE - (64ull << 30), &start, &size, &prot, &max) && start == RT_MEMBASE - (64ull << 30) && size == 64ull << 30 && max == 0,
          "the moat below: %#llx size %#llx max %d", start, size, max);
    CHECK(region(RT_MEMBASE + (1ull << 39), &start, &size, &prot, &max) && start == RT_MEMBASE + (1ull << 39) && size == 512ull << 30 && max == 0,
          "the moat above: %#llx size %#llx max %d", start, size, max);
    CHECK(!READABLE(G(0)) && !READABLE(G(0x7100000000ull)) && !READABLE((u8 *)(uintptr_t)(RT_MEMBASE - 16)), "the window or moat is readable");
    void *big = malloc(1ull << 30), *mapped = mmap(NULL, 4ull << 30, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    u64 b = (u64)(uintptr_t)big, m = (u64)(uintptr_t)mapped;
    CHECK(b + (1ull << 30) <= RT_MEMBASE - (64ull << 30) || b >= RT_MEMBASE + (1ull << 40), "malloc landed in or beside the window: %#llx", b);
    CHECK(m + (4ull << 30) <= RT_MEMBASE - (64ull << 30) || m >= RT_MEMBASE + (1ull << 40), "mmap landed in or beside the window: %#llx", m);
    free(big);
    munmap(mapped, 4ull << 30);
}

static void test_memory(void) {
    const u64 T = 0x7400000000ull;   /* (the heap's place) */
    CHECK(VirtualAlloc(G(T), 64 << 20, MEM_RESERVE, PAGE_READWRITE) == G(T), "reserve at a guest address");
    CHECK(!VirtualAlloc(G(T + (1 << 20)), 1 << 20, MEM_RESERVE, PAGE_READWRITE) && GetLastError() == ERROR_INVALID_ADDRESS, "an overlapping reservation succeeded");
    CHECK(!VirtualAlloc(G(T + (64 << 20)), 0x1000, MEM_COMMIT, PAGE_READWRITE) && GetLastError() == ERROR_INVALID_ADDRESS, "a commit outside every reservation succeeded");
    CHECK(!READABLE(G(T)), "reserved memory is readable");
    CHECK(VirtualAlloc(G(T + 0x1000), 0x1000, MEM_COMMIT, PAGE_READWRITE) == G(T + 0x1000), "commit 4 KiB");
    CHECK(WRITABLE(G(T + 0x1000)) && READABLE(G(T + 0x3FFF)), "committed 4 KiB not writable");
    CHECK(!READABLE(G(T + 0x4000)), "the next host page became readable");

    /* 4 KiB decommits on 16 KiB pages: [0x3000, 0x9000) of 64 KiB filled with 0xAB */
    CHECK(VirtualAlloc(G(T), 0x10000, MEM_COMMIT, PAGE_READWRITE) == G(T), "commit 64 KiB");
    memset(G(T), 0xAB, 0x10000);
    CHECK(VirtualFree(G(T + 0x3000), 0x6000, MEM_DECOMMIT), "decommit [0x3000, 0x9000)");
    CHECK(all_are(G(T), 0x3000, 0xAB), "the committed neighbours below lost their data");
    CHECK(all_are(G(T + 0x3000), 0x1000, 0), "a decommitted 4 KiB page sharing a host page was not zeroed");
    CHECK(!READABLE(G(T + 0x4000)) && !READABLE(G(T + 0x7FFF)), "a wholly decommitted host page stayed accessible");
    CHECK(all_are(G(T + 0x8000), 0x1000, 0) && all_are(G(T + 0x9000), 0x7000, 0xAB), "the upper partial host page: zeroed part or kept part wrong");
    CHECK(VirtualAlloc(G(T + 0x3000), 0x6000, MEM_COMMIT, PAGE_READWRITE) == G(T + 0x3000), "recommit");
    CHECK(all_are(G(T + 0x3000), 0x6000, 0), "recommitted memory is not zero");
    CHECK(all_are(G(T), 0x3000, 0xAB) && all_are(G(T + 0x9000), 0x7000, 0xAB), "recommitting changed the neighbours");
    /* one host page, its four 4 KiB pages decommitted one at a time */
    for (int k = 0; k < 4; k++) {
        CHECK(VirtualFree(G(T + 0xC000 + k * 0x1000), 0x1000, MEM_DECOMMIT), "decommit 4 KiB #%d", k);
        CHECK(k < 3 ? all_are(G(T + 0xC000 + k * 0x1000), 0x1000, 0) && all_are(G(T + 0xD000 + k * 0x1000), 0x3000 - k * 0x1000, 0xAB)
                    : !READABLE(G(T + 0xC000)), "after decommitting 4 KiB #%d", k);
    }
    CHECK(VirtualFree(G(T), 0x10000, MEM_DECOMMIT) && !READABLE(G(T)) && !READABLE(G(T + 0x9000)), "decommit all 64 KiB");
    CHECK(VirtualAlloc(G(T + 0x2000), 0x1000, MEM_COMMIT, PAGE_READWRITE) && all_are(G(T), 0x4000, 0), "recommit after a full decommit: not zero");
    MEMORY_BASIC_INFORMATION mi;
    CHECK(VirtualQuery(G(T + 0x2000), &mi, sizeof mi) == sizeof mi && mi.State == MEM_COMMIT && mi.Protect == PAGE_READWRITE, "VirtualQuery of committed memory");
    CHECK(VirtualQuery(G(T + 0x100000), &mi, sizeof mi) == sizeof mi && mi.State == MEM_RESERVE, "VirtualQuery of reserved memory: state %#lx", (unsigned long)mi.State);
    CHECK(VirtualFree(G(T), 0, MEM_DECOMMIT) && !READABLE(G(T + 0x2000)), "decommit a whole reservation (size 0)");
    CHECK(VirtualFree(G(T), 0, MEM_RELEASE), "release");
    CHECK(!VirtualFree(G(T), 0, MEM_RELEASE) && !VirtualAlloc(G(T), 0x1000, MEM_COMMIT, PAGE_READWRITE), "a released reservation still counts");
    CHECK(!READABLE(G(T)), "released memory is accessible");
    CHECK(VirtualAlloc(G(T), 1 << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) == G(T) && all_are(G(T), 1 << 20, 0), "reserve and commit again: not zero");
    VirtualFree(G(T), 0, MEM_RELEASE);

    /* a big one, as the heap: 8 GiB, committed 2 MiB at a time up to 1 GiB */
    double t0 = now_ms();
    CHECK(VirtualAlloc(G(T), 8ull << 30, MEM_RESERVE, PAGE_READWRITE) == G(T), "reserve 8 GiB");
    int ok = 1;
    for (u64 at = 0; at < (1ull << 30); at += 2 << 20) ok &= VirtualAlloc(G(T + at), 2 << 20, MEM_COMMIT, PAGE_READWRITE) == G(T + at);
    CHECK(ok, "commit 1 GiB in 2 MiB steps");
    for (u64 at = 0; at < (1ull << 30); at += 0x4000) G(T + at)[0x123] = 1;
    double t1 = now_ms();
    CHECK(VirtualFree(G(T + 0x1000), (1ull << 30) - 0x2000, MEM_DECOMMIT) && G(T)[0x123] == 1 && !READABLE(G(T + 0x8000)), "decommit nearly 1 GiB");
    VirtualFree(G(T), 0, MEM_RELEASE);
    printf("  (8 GiB reserved, 1 GiB committed and touched in %.0f ms, decommitted and released in %.1f ms)\n", t1 - t0, now_ms() - t1);

    /* outside the window: fixed reservations map (and fail where anything is mapped) */
    mach_vm_address_t free_at = 0;
    CHECK(mach_vm_allocate(mach_task_self(), &free_at, 1ull << 30, VM_FLAGS_ANYWHERE) == KERN_SUCCESS, "find a free gigabyte");
    mach_vm_deallocate(mach_task_self(), free_at, 1ull << 30);
    void *o = VirtualAlloc((void *)(uintptr_t)free_at, 64 << 20, MEM_RESERVE, PAGE_READWRITE);
    CHECK(o == (void *)(uintptr_t)free_at && !READABLE(o), "reserve outside the window: %p", o);
    CHECK(!VirtualAlloc((void *)(uintptr_t)free_at, 1 << 20, MEM_RESERVE, PAGE_READWRITE), "a fixed reservation over a mapping succeeded");
    CHECK(VirtualAlloc((u8 *)o + 0x5000, 0x2000, MEM_COMMIT, PAGE_READWRITE) == (u8 *)o + 0x5000 && WRITABLE((u8 *)o + 0x6FFF), "commit outside the window");
    memset((u8 *)o + 0x4000, 0x11, 0x4000);
    CHECK(VirtualFree((u8 *)o + 0x6000, 0x1000, MEM_DECOMMIT) && all_are((u8 *)o + 0x6000, 0x1000, 0) && all_are((u8 *)o + 0x4000, 0x2000, 0x11),
          "4 KiB decommit outside the window");
    CHECK(VirtualFree(o, 0, MEM_RELEASE) && region((u64)(uintptr_t)o, &free_at, &(u64){0}, &(int){0}, &(int){0}) && free_at != (u64)(uintptr_t)o,
          "release outside the window left a mapping");
    CHECK(!VirtualAlloc(G(0) - 0x100000, 0x10000, MEM_RESERVE, PAGE_READWRITE), "a reservation in the moat succeeded");

    /* anywhere */
    u8 *any = VirtualAlloc(NULL, 3 << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(any && all_are(any, 3 << 20, 0) && WRITABLE(any + (3 << 20) - 1), "reserve and commit anywhere");
    CHECK(VirtualFree(any, 0, MEM_RELEASE), "release anywhere");
    any = VirtualAlloc(NULL, 1 << 20, MEM_RESERVE, PAGE_NOACCESS);
    CHECK(any && !READABLE(any), "reserve anywhere");
    CHECK(VirtualAlloc(any + 0x5000, 0x10, MEM_COMMIT, PAGE_READWRITE) == any + 0x5000 && WRITABLE(any + 0x5000) && !READABLE(any + 0x8000),
          "commit inside a reservation made anywhere");
    VirtualFree(any, 0, MEM_RELEASE);
}

/* ---- the write watch ---------------------------------------------------------------------- */
static ULONG_PTR watch(DWORD flags, u64 at, u64 size, u64 *out, ULONG_PTR cap) {   /* GetWriteWatch: guest pages relative to `at` */
    void *pages[1024];
    ULONG_PTR n = cap < 1024 ? cap : 1024;
    ULONG gran = 0;
    if (GetWriteWatch(flags, G(at), size, pages, &n, &gran) || gran != 4096) return (ULONG_PTR)-1;
    for (ULONG_PTR i = 0; i < n; i++) out[i] = (u64)(uintptr_t)pages[i] - (u64)(uintptr_t)G(at);
    return n;
}

static int pages_are(const u64 *got, ULONG_PTR n, int count, ...) {
    va_list ap;
    va_start(ap, count);
    int ok = (int)n == count;
    for (int i = 0; i < count && ok; i++) ok = got[i] == va_arg(ap, u64);
    va_end(ap);
    return ok;
}

/* ReadFile into watched memory while another thread protects its pages again all the while (as
 * the renderer does with what it checks, whatever shares their host pages): every read must
 * succeed with the file's bytes - not fail when a protected page is met a second time. */
#define REARMED_SIZE (8u << 20)
static volatile int rearm_stop;
static volatile u64 rearm_at;
static DWORD WINAPI rearmer(void *unused) {
    (void)unused;
    while (!rearm_stop) {
        ResetWriteWatch(G(rearm_at), REARMED_SIZE);
        for (volatile int k = 0; k < 2000; k++) {}
    }
    return 0;
}

static void test_read_rearmed(u64 at) {
    char path[] = "/tmp/shimtest.XXXXXX";
    int fd = mkstemp(path);
    u32 *data = malloc(REARMED_SIZE + 0x40000);
    for (u32 i = 0; i < (REARMED_SIZE + 0x40000) / 4; i++) data[i] = i * 0x9E3779B9u;
    CHECK(fd >= 0 && write(fd, data, REARMED_SIZE + 0x40000) == (ssize_t)(REARMED_SIZE + 0x40000), "a file to read");
    CHECK(VirtualAlloc(G(at), REARMED_SIZE, MEM_COMMIT, PAGE_READWRITE) == G(at), "commit 8 MiB to read into");
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, NULL);
    rearm_at = at;
    rearm_stop = 0;
    HANDLE t = CreateThread(NULL, 0, rearmer, NULL, 0, NULL);
    int failed = 0, wrong = 0, reads = 16;
    double t0 = now_ms();
    for (int i = 0; i < reads; i++) {   /* (from another offset each time: a read that left the last one's bytes is seen) */
        OVERLAPPED ov = {0};
        ov.Offset = (DWORD)i * 0x4000;
        DWORD got = 0;
        if (!ReadFile(h, G(at), REARMED_SIZE, &got, &ov) || got != REARMED_SIZE) failed++;
        else wrong += memcmp(G(at), (u8 *)data + (size_t)i * 0x4000, REARMED_SIZE) != 0;
    }
    rearm_stop = 1;
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    CHECK(!failed && !wrong, "ReadFile into pages re-armed all the while: %d of %d failed, %d with wrong bytes (error %lu)", failed, reads, wrong,
          (unsigned long)GetLastError());
    /* the same through the file offset: read() moves it on before it fails on a protected page */
    SetFilePointerEx(h, (LARGE_INTEGER){.QuadPart = 0x8000}, NULL, FILE_BEGIN);
    ResetWriteWatch(G(at), REARMED_SIZE);
    *G(at) = 1;   /* (its first host page open, the rest protected: the kernel fills some, then fails) */
    DWORD got = 0;
    LARGE_INTEGER where = {0};
    CHECK(ReadFile(h, G(at), REARMED_SIZE, &got, NULL) && got == REARMED_SIZE && !memcmp(G(at), (u8 *)data + 0x8000, REARMED_SIZE) &&
          SetFilePointerEx(h, (LARGE_INTEGER){0}, &where, FILE_CURRENT) && where.QuadPart == 0x8000 + REARMED_SIZE,
          "ReadFile at the file offset into protected pages: %lu bytes, offset %#llx after", (unsigned long)got, (unsigned long long)where.QuadPart);
    /* and into memory no store can go to (past the committed part): a failure, as before, not a crash */
    OVERLAPPED ov = {0};
    CHECK(!ReadFile(h, G(at + REARMED_SIZE - 0x4000), 0x8000, &got, &ov), "ReadFile into memory partly not committed succeeded");
    printf("  (ReadFile against a re-arming thread: %d reads of 8 MiB in %.0f ms)\n", reads, now_ms() - t0);
    CloseHandle(h);
    close(fd);
    unlink(path);
    free(data);
}

static void test_write_watch(void) {
    const u64 W = 0x6000000000ull;   /* (the alias area's place) */
    u64 got[1024];
    ULONG_PTR n;
    CHECK(VirtualAlloc(G(W), 1 << 30, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE) == G(W), "reserve a watched area");
    CHECK(VirtualAlloc(G(W), 256 << 10, MEM_COMMIT, PAGE_READWRITE) == G(W), "commit in it");
    int f0 = ww_faults;
    volatile u8 sum = 0;
    for (u64 i = 0; i < (256 << 10); i += 0x1000) sum += G(W)[i];
    CHECK(ww_faults == f0 && sum == 0, "reads faulted");
    CHECK((n = watch(0, W, 256 << 10, got, 64)) == 0, "written pages before any write: %lu", (unsigned long)n);
    G(W)[0x5000] = 1;
    CHECK(ww_faults == f0 + 1, "a first write did not fault (%d)", ww_faults - f0);
    G(W)[0x6000] = 1;
    G(W)[0x7FFF] = 1;
    CHECK(ww_faults == f0 + 1, "a second write to the same host page faulted");
    n = watch(0, W, 256 << 10, got, 64);
    CHECK(pages_are(got, n, 4, 0x4000ull, 0x5000ull, 0x6000ull, 0x7000ull), "a written host page: its 4 KiB pages (%lu)", (unsigned long)n);
    n = watch(0, W + 0x5000, 0x1000, got, 64);
    CHECK(pages_are(got, n, 1, 0ull), "clipped to the range asked: %lu", (unsigned long)n);
    CHECK(!ResetWriteWatch(G(W + 0x5000), 0x1000), "ResetWriteWatch");
    n = watch(0, W, 256 << 10, got, 64);
    CHECK(pages_are(got, n, 3, 0x4000ull, 0x6000ull, 0x7000ull), "a reset of one 4 KiB page cleared others or kept it (%lu)", (unsigned long)n);
    G(W)[0x5000] = 2;
    CHECK(ww_faults == f0 + 2, "a write after a reset did not fault");
    n = watch(WRITE_WATCH_FLAG_RESET, W, 256 << 10, got, 64);
    CHECK(pages_are(got, n, 4, 0x4000ull, 0x5000ull, 0x6000ull, 0x7000ull), "written pages with a reset (%lu)", (unsigned long)n);
    CHECK((n = watch(0, W, 256 << 10, got, 64)) == 0, "pages still written after a reset: %lu", (unsigned long)n);
    G(W)[0x4000] = 3;
    CHECK(ww_faults == f0 + 3 && G(W)[0x5000] == 2 && G(W)[0x7FFF] == 1, "re-armed: no fault, or data lost");

    /* exactly the written page: a host page with only it committed */
    CHECK(VirtualFree(G(W + 0x20000), 0x4000, MEM_DECOMMIT) && VirtualAlloc(G(W + 0x21000), 0x1000, MEM_COMMIT, PAGE_READWRITE), "commit one 4 KiB page");
    G(W)[0x21234] = 7;
    n = watch(WRITE_WATCH_FLAG_RESET, W + 0x20000, 0x4000, got, 64);
    CHECK(pages_are(got, n, 1, 0x1000ull), "exactly the written 4 KiB page (%lu)", (unsigned long)n);
    G(W)[0x21235] = 7;
    int f1 = ww_faults;
    CHECK(VirtualAlloc(G(W + 0x22000), 0x1000, MEM_COMMIT, PAGE_READWRITE), "commit a neighbour");
    G(W)[0x21236] = 7;
    CHECK(ww_faults == f1 + 1, "a commit beside a written page did not re-arm it");
    n = watch(0, W + 0x20000, 0x4000, got, 64);
    CHECK(pages_are(got, n, 2, 0x1000ull, 0x2000ull), "after the neighbour's commit (%lu)", (unsigned long)n);

    /* fewer room than written pages: the rest stay written */
    for (u64 i = 0; i < 4; i++) G(W)[0x30000 + i * 0x4000] = 1;
    watch(WRITE_WATCH_FLAG_RESET, W + 0x20000, 0x4000, got, 64);
    n = watch(WRITE_WATCH_FLAG_RESET, W + 0x30000, 0x10000, got, 6);
    CHECK(n == 6 && got[0] == 0 && got[5] == 0x5000, "a full array: %lu", (unsigned long)n);
    n = watch(WRITE_WATCH_FLAG_RESET, W + 0x30000, 0x10000, got, 64);
    CHECK(n == 10 && got[0] == 0x6000 && got[9] == 0xF000, "the rest after a full array: %lu", (unsigned long)n);

    /* a system call's write into an armed page: read() fails, ReadFile copies in a read of its own */
    char path[] = "/tmp/shimtest.XXXXXX";
    int fd = mkstemp(path);
    const char text[] = "written by the kernel, not by a store";
    CHECK(fd >= 0 && write(fd, text, sizeof text) == (ssize_t)sizeof text, "a file to read");
    watch(WRITE_WATCH_FLAG_RESET, W, 256 << 10, got, 1024);
    CHECK(pread(fd, G(W + 0x24100), sizeof text, 0) == -1 && errno == EFAULT, "read() into an armed page did not fail with EFAULT");
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, NULL);
    DWORD got_bytes = 0;
    OVERLAPPED ov = {0};
    CHECK(h != INVALID_HANDLE_VALUE && ReadFile(h, G(W + 0x24100), sizeof text, &got_bytes, &ov) && got_bytes == sizeof text &&
          !memcmp(G(W + 0x24100), text, sizeof text), "ReadFile into an armed page");
    n = watch(0, W + 0x24000, 0x4000, got, 64);
    CHECK(pages_are(got, n, 4, 0ull, 0x1000ull, 0x2000ull, 0x3000ull), "ReadFile's pages not reported written (%lu)", (unsigned long)n);
    CloseHandle(h);
    close(fd);
    unlink(path);
    test_read_rearmed(W + 0x1000000);

    /* a 4 KiB decommit in an armed page: zeroed, its neighbours not written (as on Windows), the
     * page still armed */
    memset(G(W + 0x28000), 0x5A, 0x4000);
    watch(WRITE_WATCH_FLAG_RESET, W, 256 << 10, got, 1024);
    CHECK(VirtualFree(G(W + 0x29000), 0x1000, MEM_DECOMMIT), "decommit 4 KiB of a watched page");
    CHECK(all_are(G(W + 0x29000), 0x1000, 0) && all_are(G(W + 0x28000), 0x1000, 0x5A) && all_are(G(W + 0x2A000), 0x2000, 0x5A), "data after the decommit");
    n = watch(0, W + 0x28000, 0x4000, got, 64);
    CHECK(n == 0, "a 4 KiB decommit reported its neighbours written (%lu)", (unsigned long)n);
    int f2 = ww_faults;
    G(W)[0x2A000] = 0x5B;
    n = watch(0, W + 0x28000, 0x4000, got, 64);
    CHECK(ww_faults == f2 + 1 && pages_are(got, n, 3, 0ull, 0x2000ull, 0x3000ull), "a write after a 4 KiB decommit: %d faults, %lu pages", ww_faults - f2, (unsigned long)n);
    CHECK(VirtualAlloc(G(W + 0x29000), 0x1000, MEM_COMMIT, PAGE_READWRITE) && all_are(G(W + 0x29000), 0x1000, 0), "recommit in a watched page");
    /* and in an open one: in place */
    CHECK(VirtualFree(G(W + 0x2B000), 0x1000, MEM_DECOMMIT) && all_are(G(W + 0x2B000), 0x1000, 0) && G(W)[0x2A000] == 0x5B, "a 4 KiB decommit in an open page");

    void *pages[4];
    ULONG_PTR cap = 4;
    ULONG gran;
    CHECK(GetWriteWatch(0, &cap, 16, pages, &cap, &gran) != 0, "GetWriteWatch of unwatched memory succeeded");
    CHECK(VirtualFree(G(W), 0, MEM_RELEASE), "release a watched area");
    cap = 4;
    CHECK(GetWriteWatch(0, G(W), 0x1000, pages, &cap, &gran) != 0, "GetWriteWatch of a released area succeeded");
    printf("  (write watch: %s)\n", rt_ww_mode());
}

/* Four writers write random pages while a reader resets: every write must be reported by a
 * reset that ends after it began and starts no later than the first to start after it ended.
 * (Many pages, so that another write to the same one seldom hides a lost one.) */
#define STRESS_PAGES 4096
#define STRESS_WRITES 100000
#define STRESS_WRITERS 4
static volatile u64 stress_clock;
static struct { u16 page; u64 before, after; } stress_log[STRESS_WRITERS][STRESS_WRITES];
static struct { u64 start, end; u32 first, count; } *stress_resets;
static u16 *stress_pages;
static u32 stress_nresets, stress_npages;
static const u64 STRESS_BASE = 0x6100000000ull;

static DWORD WINAPI stress_writer(void *arg) {
    int w = (int)(uintptr_t)arg;
    u32 r = 0x9E3779B9u * (u32)(w + 1);
    for (int i = 0; i < STRESS_WRITES; i++) {
        r ^= r << 13; r ^= r >> 17; r ^= r << 5;
        u16 p = (u16)(r % STRESS_PAGES);
        stress_log[w][i].page = p;
        stress_log[w][i].before = __atomic_load_n(&stress_clock, __ATOMIC_SEQ_CST);
        G(STRESS_BASE)[(u64)p * 0x1000 + (r >> 20 & 0xFFF)] = (u8)i;
        stress_log[w][i].after = __atomic_add_fetch(&stress_clock, 1, __ATOMIC_SEQ_CST);
        for (volatile int k = r & 255; k > 0; k--) {}
    }
    return 0;
}

static void stress_reset(void) {
    static void *pages[STRESS_PAGES];
    ULONG_PTR n = STRESS_PAGES;
    ULONG gran;
    u64 start = __atomic_load_n(&stress_clock, __ATOMIC_SEQ_CST);
    UINT bad = GetWriteWatch(WRITE_WATCH_FLAG_RESET, G(STRESS_BASE), STRESS_PAGES * 0x1000, pages, &n, &gran);
    u64 end = __atomic_load_n(&stress_clock, __ATOMIC_SEQ_CST);
    if (bad) n = 0;
    stress_resets[stress_nresets].start = start;
    stress_resets[stress_nresets].end = end;
    stress_resets[stress_nresets].first = stress_npages;
    stress_resets[stress_nresets++].count = (u32)n;
    for (ULONG_PTR i = 0; i < n; i++) stress_pages[stress_npages++] = (u16)(((u64)(uintptr_t)pages[i] - (u64)(uintptr_t)G(STRESS_BASE)) / 0x1000);
}

static void test_write_watch_race(void) {
    CHECK(VirtualAlloc(G(STRESS_BASE), STRESS_PAGES * 0x1000, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE) == G(STRESS_BASE), "a watched area");
    const u32 max_resets = 4000000, max_pages = 64000000;
    stress_resets = malloc(sizeof *stress_resets * max_resets);
    stress_pages = malloc(sizeof *stress_pages * max_pages);
    int f0 = ww_faults;
    HANDLE t[STRESS_WRITERS];
    double t0 = now_ms();
    stress_reset();
    for (int w = 0; w < STRESS_WRITERS; w++) t[w] = CreateThread(NULL, 0, stress_writer, (void *)(uintptr_t)w, 0, NULL);
    for (int done = 0; !done && stress_nresets < max_resets - 4 && stress_npages < max_pages - 3 * STRESS_PAGES;) {
        stress_reset();
        done = 1;
        for (int w = 0; w < STRESS_WRITERS; w++) done &= WaitForSingleObject(t[w], 0) == WAIT_OBJECT_0;
    }
    for (int w = 0; w < STRESS_WRITERS; w++) { WaitForSingleObject(t[w], INFINITE); CloseHandle(t[w]); }
    stress_reset();
    u64 lost = 0;
    for (int w = 0; w < STRESS_WRITERS; w++)
        for (int i = 0; i < STRESS_WRITES; i++) {
            u64 b = stress_log[w][i].before, a = stress_log[w][i].after;
            u32 lo = 0, hi = stress_nresets;   /* the first reset ending at or after b */
            while (lo < hi) { u32 mid = (lo + hi) / 2; if (stress_resets[mid].end < b) lo = mid + 1; else hi = mid; }
            u32 from = lo;
            lo = 0, hi = stress_nresets;       /* the first starting at or after a */
            while (lo < hi) { u32 mid = (lo + hi) / 2; if (stress_resets[mid].start < a) lo = mid + 1; else hi = mid; }
            int seen = 0;
            for (u32 j = from; j <= lo && j < stress_nresets && !seen; j++)
                for (u32 k = 0; k < stress_resets[j].count && !seen; k++) seen = stress_pages[stress_resets[j].first + k] == stress_log[w][i].page;
            lost += !seen;
        }
    CHECK(lost == 0, "%llu of %d writes not reported by the resets racing them", lost, STRESS_WRITERS * STRESS_WRITES);
    printf("  (write-watch race: %d writes, %u resets reporting %u pages, %d faults in %.0f ms)\n", STRESS_WRITERS * STRESS_WRITES, stress_nresets,
           stress_npages, ww_faults - f0, now_ms() - t0);
    VirtualFree(G(STRESS_BASE), 0, MEM_RELEASE);
    free(stress_resets);
    free(stress_pages);
}

/* ---- fibers ------------------------------------------------------------------------------- */
static void *main_fiber, *other_fiber;
static volatile int pings;
static volatile pthread_t fiber_ran_on;
static void *bt_frames[64];
static int bt_count, walk_count;
static void *walk_frames[64];

static void WINAPI pong(void *arg) {
    for (;;) {
        pings += (int)(uintptr_t)GetFiberData() == (int)(uintptr_t)arg;
        SwitchToFiber(main_fiber);
    }
}

static void WINAPI once(void *arg) {
    ++*(int *)arg;
    for (;;) SwitchToFiber(main_fiber);
}

__attribute__((noinline)) static void bt_fiber_inner(void) {
    bt_count = backtrace(bt_frames, 64);
    walk_count = rt_frame_walk((u64)(uintptr_t)&&here, 0, (u64)(uintptr_t)__builtin_frame_address(0), walk_frames, 64);
here:
    __asm__ volatile("" ::: "memory");
}
__attribute__((noinline)) static void bt_fiber_outer(void) { bt_fiber_inner(); __asm__ volatile("" ::: "memory"); }
static void WINAPI bt_fiber(void *arg) {
    (void)arg;
    bt_fiber_outer();
    for (;;) SwitchToFiber(main_fiber);
}

static void WINAPI fpcr_fiber(void *arg) {
    u64 fpcr = 3ull << 22;   /* round towards zero */
    __asm__ volatile("msr fpcr, %0" ::"r"(fpcr));
    for (;;) {
        __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
        *(u64 *)arg = fpcr;
        SwitchToFiber(main_fiber);
    }
}

static void *volatile wander_back;   /* where the wanderer switches back to */
static void WINAPI wanderer(void *arg) {   /* (made on one thread, run on another) */
    (void)arg;
    for (;;) {
        fiber_ran_on = pthread_self();
        SwitchToFiber(wander_back);
    }
}
static DWORD WINAPI fiber_host(void *fiber) {
    other_fiber = ConvertThreadToFiber(NULL);
    wander_back = other_fiber;
    SwitchToFiber(fiber);
    return 0;
}

static void test_fibers(void) {
    CHECK(!IsThreadAFiber(), "a fiber before ConvertThreadToFiber");
    main_fiber = ConvertThreadToFiber(NULL);
    CHECK(main_fiber && IsThreadAFiber() && GetCurrentFiber() == main_fiber && ConvertThreadToFiber(NULL) == main_fiber, "ConvertThreadToFiber");
    void *f = CreateFiberEx(64 << 10, 64 << 20, FIBER_FLAG_FLOAT_SWITCH, pong, (void *)42);
    CHECK(f != NULL, "CreateFiberEx");
    double t0 = now_ms();
    for (int i = 0; i < 100000; i++) SwitchToFiber(f);
    double us = (now_ms() - t0) * 1000.0 / 200000;
    CHECK(pings == 100000, "ping-pong: %d of 100000", pings);
    CHECK(GetCurrentFiber() == main_fiber, "back on the main fiber");
    printf("  (a fiber switch: %.0f ns)\n", us * 1000);
    DeleteFiber(f);

    int ran = 0;
    for (int i = 0; i < 1000; i++) {
        void *k = CreateFiberEx(64 << 10, 64 << 20, FIBER_FLAG_FLOAT_SWITCH, once, &ran);
        if (!k) break;
        SwitchToFiber(k);
        DeleteFiber(k);
    }
    CHECK(ran == 1000, "1000 fibers made, run and deleted: %d", ran);

    void *b = CreateFiber(0, bt_fiber, NULL);
    SwitchToFiber(b);
    int inner = 0, outer = 0, end_at = -1;
    for (int i = 0; i < bt_count; i++) {
        const char *n = name_of(bt_frames[i]);
        inner |= strstr(n, "bt_fiber_inner") != NULL;
        outer |= strstr(n, "bt_fiber_outer") != NULL;
        if (strstr(n, "rt_fiber_entry") || strstr(n, "rt_fiber_trampoline")) end_at = i;
    }
    CHECK(inner && outer && end_at >= 0 && bt_count <= end_at + 2, "backtrace() in a fiber: %d frames, ends at %d", bt_count, end_at);
    outer = 0;
    for (int i = 0; i < walk_count; i++) outer |= strstr(name_of(walk_frames[i]), "bt_fiber_outer") != NULL;
    CHECK(outer && walk_count < 8, "the frame walk in a fiber: %d frames", walk_count);
    DeleteFiber(b);

    u64 inside = 0, mine, after;
    __asm__ volatile("mrs %0, fpcr" : "=r"(mine));
    void *fp = CreateFiber(0, fpcr_fiber, &inside);
    SwitchToFiber(fp);
    __asm__ volatile("mrs %0, fpcr" : "=r"(after));
    CHECK(after == mine && (inside >> 22 & 3) == 3, "FPCR across a switch: mine %#llx -> %#llx, the fiber's %#llx", mine, after, inside);
    SwitchToFiber(fp);
    CHECK((inside >> 22 & 3) == 3, "the fiber's FPCR lost");
    DeleteFiber(fp);

    /* a fiber made here, resumed on another thread (as the guest's are), then here again */
    void *w = CreateFiber(0, wanderer, NULL);
    HANDLE t = CreateThread(NULL, 0, fiber_host, w, 0, NULL);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    CHECK(fiber_ran_on && !pthread_equal(fiber_ran_on, pthread_self()), "the fiber did not run on the other thread");
    wander_back = main_fiber;
    SwitchToFiber(w);
    CHECK(pthread_equal(fiber_ran_on, pthread_self()), "the fiber, back on this thread, did not run here");
    DeleteFiber(w);

    u64 start, size;
    int prot, max;
    void *g = CreateFiberEx(0, 1 << 20, 0, pong, NULL);
    u64 stack = *(u64 *)((u8 *)g + 24);   /* (Fiber.stack) */
    CHECK(region(stack, &start, &size, &prot, &max) && start == stack && size == 0x10000 && prot == 0, "a fiber's guard: %#llx +%#llx prot %d", start, size, prot);
    DeleteFiber(g);
}

/* ---- waits -------------------------------------------------------------------------------- */
static HANDLE ev_auto, ev_manual;
static CRITICAL_SECTION cs;
static CONDITION_VARIABLE cv;
static SRWLOCK srw = SRWLOCK_INIT;
static volatile int cv_flag, cs_owner_seen, cs_depth_seen;

static DWORD WINAPI set_later(void *ms) {
    Sleep((DWORD)(uintptr_t)ms);
    SetEvent(ev_auto);
    return 0;
}

static DWORD WINAPI cs_waiter(void *unused) {
    (void)unused;
    EnterCriticalSection(&cs);
    EnterCriticalSection(&cs);   /* (twice: the wait releases one level, as Linux's) */
    LeaveCriticalSection(&cs);
    while (!cv_flag) SleepConditionVariableCS(&cv, &cs, INFINITE);
    cs_owner_seen = (int)cs.OwningThread == (int)GetCurrentThreadId();
    cs_depth_seen = cs.depth;
    LeaveCriticalSection(&cs);
    return 0;
}

static DWORD WINAPI srw_waker(void *unused) {
    (void)unused;
    Sleep(20);
    AcquireSRWLockExclusive(&srw);
    cv_flag = 2;
    ReleaseSRWLockExclusive(&srw);
    WakeAllConditionVariable(&cv);
    return 0;
}

/* producers and consumers handing items through one lock and two condition variables */
static CONDITION_VARIABLE not_empty, not_full;
static int queue_items, queue_produced, queue_consumed;
static DWORD WINAPI producer(void *unused) {
    (void)unused;
    for (int i = 0; i < 20000; i++) {
        EnterCriticalSection(&cs);
        while (queue_items == 4) SleepConditionVariableCS(&not_full, &cs, INFINITE);
        queue_items++;
        queue_produced++;
        LeaveCriticalSection(&cs);
        WakeConditionVariable(&not_empty);
    }
    return 0;
}
static DWORD WINAPI consumer(void *unused) {
    (void)unused;
    for (int i = 0; i < 20000; i++) {
        EnterCriticalSection(&cs);
        while (!queue_items) SleepConditionVariableCS(&not_empty, &cs, INFINITE);
        queue_items--;
        queue_consumed++;
        LeaveCriticalSection(&cs);
        WakeConditionVariable(&not_full);
    }
    return 0;
}

static void test_waits(void) {
    printf("  (waiting on an address: %s)\n", rt_futex_mode());
    InitializeCriticalSection(&cs);
    InitializeConditionVariable(&cv);
    ev_auto = CreateEventW(NULL, FALSE, FALSE, NULL);
    ev_manual = CreateEventA(NULL, TRUE, FALSE, NULL);
    double t0 = now_ms();
    CHECK(WaitForSingleObject(ev_auto, 50) == WAIT_TIMEOUT, "an unset event's wait");
    double e = now_ms() - t0;
    CHECK(e >= 49 && e < 250, "a 50 ms event timeout took %.1f ms", e);
    CHECK(WaitForSingleObject(ev_auto, 0) == WAIT_TIMEOUT, "an unset event, no wait");
    HANDLE t = CreateThread(NULL, 0, set_later, (void *)30, 0, NULL);
    t0 = now_ms();
    CHECK(WaitForSingleObject(ev_auto, INFINITE) == WAIT_OBJECT_0, "an event set by another thread");
    e = now_ms() - t0;
    CHECK(e >= 25 && e < 250, "an event set after 30 ms woke the wait after %.1f ms", e);
    CHECK(WaitForSingleObject(ev_auto, 0) == WAIT_TIMEOUT, "an auto-reset event stayed set");
    CHECK(WaitForSingleObject(t, 1000) == WAIT_OBJECT_0, "a finished thread's handle");
    CloseHandle(t);
    SetEvent(ev_manual);
    CHECK(WaitForSingleObject(ev_manual, 0) == WAIT_OBJECT_0 && WaitForSingleObject(ev_manual, 10) == WAIT_OBJECT_0, "a manual-reset event");
    ResetEvent(ev_manual);
    CHECK(WaitForSingleObject(ev_manual, 0) == WAIT_TIMEOUT, "ResetEvent");

    EnterCriticalSection(&cs);
    t0 = now_ms();
    CHECK(!SleepConditionVariableCS(&cv, &cs, 20) && GetLastError() == WAIT_TIMEOUT, "a condition variable's timeout");
    e = now_ms() - t0;
    CHECK(e >= 19 && e < 200, "a 20 ms condition variable timeout took %.1f ms", e);
    CHECK(cs.OwningThread == GetCurrentThreadId() && cs.depth == 1, "the critical section's owner after a timed-out wait");
    LeaveCriticalSection(&cs);
    AcquireSRWLockExclusive(&srw);
    t = CreateThread(NULL, 0, srw_waker, NULL, 0, NULL);
    t0 = now_ms();
    while (cv_flag != 2) if (!SleepConditionVariableSRW(&cv, &srw, 2000, 0)) break;
    e = now_ms() - t0;
    CHECK(cv_flag == 2 && e >= 15 && e < 500, "SleepConditionVariableSRW woken after %.1f ms (flag %d)", e, cv_flag);
    ReleaseSRWLockExclusive(&srw);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    cv_flag = 0;

    t0 = now_ms();
    HANDLE ts[4] = {CreateThread(NULL, 0, producer, NULL, 0, NULL), CreateThread(NULL, 0, producer, NULL, 0, NULL),
                    CreateThread(NULL, 0, consumer, NULL, 0, NULL), CreateThread(NULL, 0, consumer, NULL, 0, NULL)};
    for (int i = 0; i < 4; i++) { CHECK(WaitForSingleObject(ts[i], 20000) == WAIT_OBJECT_0, "producers and consumers hung"); CloseHandle(ts[i]); }
    CHECK(queue_produced == 40000 && queue_consumed == 40000 && queue_items == 0, "handed over: %d made, %d taken", queue_produced, queue_consumed);
    printf("  (40000 items through two condition variables in %.0f ms)\n", now_ms() - t0);

    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due = {.QuadPart = -100000};   /* 10 ms */
    CHECK(SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE), "SetWaitableTimer");
    t0 = now_ms();
    CHECK(WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0, "a timer's wait");
    e = now_ms() - t0;
    CHECK(e >= 9.5 && e < 60, "a 10 ms timer took %.2f ms", e);
    CHECK(WaitForSingleObject(timer, 5) == WAIT_TIMEOUT && WaitForSingleObject(timer, INFINITE) == WAIT_FAILED, "a timer that went off waited again");
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    due.QuadPart = (LONGLONG)((u64)ft.dwHighDateTime << 32 | ft.dwLowDateTime) + 150000;   /* 15 ms from now, absolute */
    SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
    t0 = now_ms();
    CHECK(WaitForSingleObject(timer, 1000) == WAIT_OBJECT_0, "an absolute timer");
    e = now_ms() - t0;
    CHECK(e >= 13 && e < 80, "a timer due 15 ms from now took %.2f ms", e);
    due.QuadPart = -10000000;   /* 1 s: the wait's own limit first */
    SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
    t0 = now_ms();
    CHECK(WaitForSingleObject(timer, 20) == WAIT_TIMEOUT && now_ms() - t0 < 100, "a timer wait's own limit");
    CloseHandle(timer);
    t0 = now_ms();
    Sleep(10);
    e = now_ms() - t0;
    CHECK(e >= 10 && e < 60, "Sleep(10) took %.2f ms", e);
    CloseHandle(ev_auto);
    CloseHandle(ev_manual);
}

/* ---- threads and critical sections -------------------------------------------------------- */
static volatile DWORD seen_id;
static volatile int seen_qos[3];
static char seen_name[64];

static DWORD WINAPI named(void *unused) {
    (void)unused;
    seen_id = GetCurrentThreadId();
    SetThreadDescription(GetCurrentThread(), L"a thread with a long description, longer than Linux's 15 characters");
    pthread_getname_np(pthread_self(), seen_name, sizeof seen_name);
    seen_qos[0] = qos_class_self();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    seen_qos[1] = qos_class_self();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    seen_qos[2] = qos_class_self();
    Sleep(100);
    return 0;
}

static volatile int tried = -1;
static DWORD WINAPI try_enter(void *unused) {
    (void)unused;
    BOOL got = TryEnterCriticalSection(&cs);
    if (got) LeaveCriticalSection(&cs);
    tried = got;
    return 0;
}

static void test_threads(void) {
    DWORD id = 0;
    HANDLE t = CreateThread(NULL, 0, named, NULL, 0, &id);
    while (!seen_qos[2]) Sleep(1);
    CHECK(id && id == seen_id && GetThreadId(t) == id && rt_thread_tid(t) == id, "thread ids: %u, %u, %u", id, seen_id, GetThreadId(t));
    CHECK(!strcmp(rt_thread_description(id), "a thread with a long description, longer than Linux's 15 characters"), "the description: %s", rt_thread_description(id));
    CHECK(!strncmp(seen_name, "a thread with a long description", 32), "the thread's name: %s", seen_name);
    CHECK(seen_qos[0] == QOS_CLASS_USER_INTERACTIVE && seen_qos[1] == QOS_CLASS_USER_INITIATED && seen_qos[2] == QOS_CLASS_UTILITY,
          "QoS classes: %#x, %#x, %#x", seen_qos[0], seen_qos[1], seen_qos[2]);
    CHECK(!SetThreadPriority(t, THREAD_PRIORITY_HIGHEST), "another thread's priority changed");
    thread_basic_info_data_t info;
    mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
    CHECK(thread_info((thread_act_t)id, THREAD_BASIC_INFO, (thread_info_t)&info, &n) == KERN_SUCCESS, "the id is not the thread's Mach port");
    CHECK(GetCurrentThreadId() == pthread_mach_thread_np(pthread_self()), "GetCurrentThreadId");
    WaitForSingleObject(t, INFINITE);
    CHECK(!*rt_thread_description(id), "a finished thread's description stayed");
    CloseHandle(t);
    HANDLE real = NULL;
    CHECK(DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &real, 0, FALSE, DUPLICATE_SAME_ACCESS) &&
          GetThreadId(real) == GetCurrentThreadId(), "DuplicateHandle of this thread");
    CloseHandle(real);

    CHECK(cs.OwningThread == 0 && cs.depth == 0, "a free critical section's owner: %u", cs.OwningThread);
    EnterCriticalSection(&cs);
    EnterCriticalSection(&cs);
    CHECK(cs.OwningThread == GetCurrentThreadId() && cs.depth == 2, "the owner, twice entered");
    t = CreateThread(NULL, 0, try_enter, NULL, 0, NULL);
    WaitForSingleObject(t, INFINITE);
    CHECK(tried == 0, "another thread entered a held critical section");
    CloseHandle(t);
    LeaveCriticalSection(&cs);
    CHECK(cs.OwningThread == GetCurrentThreadId() && cs.depth == 1, "the owner after one leave");
    LeaveCriticalSection(&cs);
    CHECK(cs.OwningThread == 0, "the owner after leaving");
    CHECK(TryEnterCriticalSection(&cs) && cs.OwningThread == GetCurrentThreadId(), "TryEnterCriticalSection");
    LeaveCriticalSection(&cs);

    /* a thread waiting in SleepConditionVariableCS does not own it; it does again once woken */
    cv_flag = 0;
    t = CreateThread(NULL, 0, cs_waiter, NULL, 0, &id);
    Sleep(30);
    EnterCriticalSection(&cs);
    CHECK(cs.OwningThread == GetCurrentThreadId() && cs.depth == 1, "while another thread waits: owner %u depth %d", cs.OwningThread, cs.depth);
    cv_flag = 1;
    LeaveCriticalSection(&cs);
    WakeAllConditionVariable(&cv);
    CHECK(WaitForSingleObject(t, 2000) == WAIT_OBJECT_0, "the waiting thread did not finish");
    CHECK(cs_owner_seen && cs_depth_seen == 1, "the woken thread's ownership: %d, depth %d", cs_owner_seen, cs_depth_seen);
    CHECK(cs.OwningThread == 0 && cs.depth == 0, "left owned: %u", cs.OwningThread);
    CloseHandle(t);
}

/* ---- files -------------------------------------------------------------------------------- */
static void test_files(void) {
    char dir[] = "/tmp/shimtest.d.XXXXXX", path[256], win[256], other[256];
    CHECK(mkdtemp(dir) != NULL, "a folder");
    snprintf(path, sizeof path, "%s/a.txt", dir);
    snprintf(win, sizeof win, "%s\\sub\\b.txt", dir);
    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(h != INVALID_HANDLE_VALUE, "CreateFileA, CREATE_NEW");
    CHECK(CreateFileA(path, GENERIC_READ, 0, NULL, CREATE_NEW, 0, NULL) == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_EXISTS, "CREATE_NEW over a file");
    DWORD n = 0;
    CHECK(WriteFile(h, "0123456789", 10, &n, NULL) && n == 10, "WriteFile");
    OVERLAPPED ov = {0};
    ov.Offset = 4;
    char buf[32] = {0};
    CHECK(ReadFile(h, buf, 3, &n, &ov) && n == 3 && !memcmp(buf, "456", 3), "ReadFile at an offset");
    ov.Offset = 100;
    CHECK(!ReadFile(h, buf, 3, &n, &ov) && n == 0 && GetLastError() == ERROR_HANDLE_EOF, "ReadFile past the end");
    LARGE_INTEGER size, at = {.QuadPart = 6};
    CHECK(GetFileSizeEx(h, &size) && size.QuadPart == 10, "GetFileSizeEx");
    CHECK(SetFilePointerEx(h, at, NULL, FILE_BEGIN) && SetEndOfFile(h) && GetFileSizeEx(h, &size) && size.QuadPart == 6, "SetEndOfFile");
    CHECK(FlushFileBuffers(h), "FlushFileBuffers");
    FILETIME c, a, w, now;
    GetSystemTimeAsFileTime(&now);
    u64 now100 = (u64)now.dwHighDateTime << 32 | now.dwLowDateTime;
    CHECK(GetFileTime(h, &c, &a, &w), "GetFileTime");
    u64 c100 = (u64)c.dwHighDateTime << 32 | c.dwLowDateTime, w100 = (u64)w.dwHighDateTime << 32 | w.dwLowDateTime;
    CHECK(c100 <= w100 && now100 - w100 < 600000000ull && now100 - c100 < 600000000ull, "file times: created %llu written %llu now %llu", c100, w100, now100);
    FILETIME set = {(DWORD)(c100 - 864000000000ull), (DWORD)((c100 - 864000000000ull) >> 32)};   /* (a day before) */
    CHECK(SetFileTime(h, NULL, NULL, &set) && GetFileTime(h, NULL, NULL, &w) && w.dwLowDateTime / 10 == set.dwLowDateTime / 10, "SetFileTime");
    int closed_fd = rt_fd_of(h);
    CHECK(closed_fd >= 0, "an open file handle's descriptor");
    CloseHandle(h);   /* (h is freed: not read again) */
    CHECK(fcntl(closed_fd, F_GETFD) == -1 && errno == EBADF, "CloseHandle closed the descriptor (%d)", closed_fd);
    CHECK(rt_fd_of(INVALID_HANDLE_VALUE) == -1 && rt_fd_of(NULL) == -1, "no descriptor behind an invalid handle");
    snprintf(other, sizeof other, "%s\\sub", dir);
    CHECK(CreateDirectoryA(other, NULL) && GetFileAttributesA(other) == FILE_ATTRIBUTE_DIRECTORY, "CreateDirectoryA with backslashes");
    CHECK(MoveFileExA(path, win, 0) && GetFileAttributesA(win) == FILE_ATTRIBUTE_NORMAL && GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES, "MoveFileExA");
    h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
    CHECK(h != INVALID_HANDLE_VALUE && WriteFile(h, "new", 3, &n, NULL) && FlushFileBuffers(h), "a second file");
    CloseHandle(h);
    CHECK(MoveFileExA(path, win, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) && GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES,
          "MoveFileExA over a file, written through");
    h = CreateFileA(win, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    memset(buf, 0, sizeof buf);
    CHECK(h != INVALID_HANDLE_VALUE && ReadFile(h, buf, sizeof buf, &n, NULL) && n == 3 && !memcmp(buf, "new", 3), "the moved file replaced the other");
    CloseHandle(h);
    for (int i = 0; i < 3; i++) {
        snprintf(path, sizeof path, "%s/sub/%c.dat", dir, 'x' + i);
        HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        CloseHandle(f);
    }
    snprintf(path, sizeof path, "%s\\sub\\*.dat", dir);
    WIN32_FIND_DATAA fd;
    int found = 0;
    HANDLE s = FindFirstFileA(path, &fd);
    CHECK(s != INVALID_HANDLE_VALUE, "FindFirstFileA");
    if (s != INVALID_HANDLE_VALUE) {
        do found += strlen(fd.cFileName) == 5 && !strcmp(fd.cFileName + 1, ".dat") && fd.dwFileAttributes == FILE_ATTRIBUTE_NORMAL; while (FindNextFileA(s, &fd));
        CHECK(GetLastError() == ERROR_NO_MORE_FILES, "the end of a search");
        FindClose(s);
    }
    CHECK(found == 3, "FindFirstFileA found %d of 3", found);
    snprintf(path, sizeof path, "%s/sub/*.*", dir);
    s = FindFirstFileA(path, &fd);
    found = 0;
    if (s != INVALID_HANDLE_VALUE) { do found++; while (FindNextFileA(s, &fd)); FindClose(s); }
    CHECK(found == 6, "*.* lists . and .. too: %d", found);
    snprintf(path, sizeof path, "%s/none/*", dir);
    CHECK(FindFirstFileA(path, &fd) == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PATH_NOT_FOUND, "a search in no folder: %lu", (unsigned long)GetLastError());
    CHECK(!RemoveDirectoryA(other) && GetLastError() == ERROR_DIR_NOT_EMPTY, "removing a full folder");
    for (int i = 0; i < 3; i++) { snprintf(path, sizeof path, "%s/sub/%c.dat", dir, 'x' + i); DeleteFileA(path); }
    CHECK(DeleteFileA(win) && !DeleteFileA(win) && GetLastError() == ERROR_FILE_NOT_FOUND, "DeleteFileA");
    CHECK(RemoveDirectoryA(other) && RemoveDirectoryA(dir), "RemoveDirectoryA");
    CHECK(CreateFileA(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL) == INVALID_HANDLE_VALUE, "opening a missing file");
}

/* ---- the clock and the rest --------------------------------------------------------------- */
static volatile LONG64 latest;
static volatile int clock_back;
static DWORD WINAPI clock_reader(void *unused) {
    (void)unused;
    for (int i = 0; i < 300000; i++) {
        LONG64 seen = latest;
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        if (t.QuadPart < seen) clock_back = 1;
        LONG64 cur = latest;
        while (t.QuadPart > cur && !__atomic_compare_exchange_n(&latest, &cur, t.QuadPart, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {}
    }
    return 0;
}

static void test_clock_and_more(void) {
    LARGE_INTEGER f, a, b;
    CHECK(QueryPerformanceFrequency(&f) && f.QuadPart > 1000000, "QueryPerformanceFrequency: %lld", f.QuadPart);
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    CHECK(f.QuadPart == (LONGLONG)(1000000000ull * tb.denom / tb.numer) || f.QuadPart == 1000000000, "the frequency is not the counter's: %lld", f.QuadPart);
    int back = 0;
    QueryPerformanceCounter(&a);
    double t0 = now_ms();
    for (int i = 0; i < 1000000; i++) { QueryPerformanceCounter(&b); back |= b.QuadPart < a.QuadPart; a = b; }
    double ns = (now_ms() - t0) * 1e6 / 1000000;
    CHECK(!back, "QueryPerformanceCounter went back");
    HANDLE t[3];
    for (int i = 0; i < 3; i++) t[i] = CreateThread(NULL, 0, clock_reader, NULL, 0, NULL);
    for (int i = 0; i < 3; i++) { WaitForSingleObject(t[i], INFINITE); CloseHandle(t[i]); }
    CHECK(!clock_back, "a thread read a time before one another thread had read");
    ULONGLONG tick = GetTickCount64();
    QueryPerformanceCounter(&a);
    Sleep(50);
    QueryPerformanceCounter(&b);
    double qpc_ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart, tick_ms = (double)(GetTickCount64() - tick);
    CHECK(qpc_ms >= 50 && qpc_ms < 120 && tick_ms >= 48 && tick_ms < 125, "Sleep(50): %.1f ms by the counter, %.0f by the tick count", qpc_ms, tick_ms);
    printf("  (the counter: %.2f MHz, %.1f ns a read)\n", f.QuadPart / 1e6, ns);
    SYSTEMTIME st;
    GetLocalTime(&st);
    CHECK(st.wYear >= 2024 && st.wMonth >= 1 && st.wMonth <= 12, "GetLocalTime");
    unsigned r1 = 0, r2 = 0;
    CHECK(!rand_s(&r1) && !rand_s(&r2) && r1 != r2, "rand_s");
    CHECK(!IsDebuggerPresent() || getenv("SHIMTEST_DEBUGGER"), "IsDebuggerPresent says a debugger is attached");
    const struct mach_header_64 *mh = (const struct mach_header_64 *)GetModuleHandleW(NULL);
    CHECK(mh == &_mh_execute_header && mh->magic == MH_MAGIC_64 && mh->filetype == MH_EXECUTE, "GetModuleHandleW(NULL)");
    const char *exe = rt_exe_path();
    CHECK(exe[0] == '/' && strstr(exe, "shimtest"), "rt_exe_path: %s", exe);
    void *frames[8];
    CHECK(CaptureStackBackTrace(0, 8, frames, NULL) >= 2, "CaptureStackBackTrace");
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    CHECK(si.dwNumberOfProcessors >= 1 && si.dwPageSize == 4096, "GetSystemInfo");
    char p[64];
    CHECK(!strcmp(rt_unix_path("a\\b\\c", p, sizeof p), "a/b/c"), "rt_unix_path");
}

/* ---- a fault: what host.c's handler reads from it ----------------------------------------- */
static u8 *ro_page;
__attribute__((noinline)) static void fault_inner(volatile u8 *p, int write) {
    if (write) *p = 1;
    else (void)*p;
    __asm__ volatile("" ::: "memory");
}
__attribute__((noinline)) static void fault_outer(volatile u8 *p, int write) { fault_inner(p, write); __asm__ volatile("" ::: "memory"); }

static volatile int fault_ok[2];
static void *fault_walk[16];
static int fault_walk_n;
static DWORD WINAPI faulting_thread(void *unused) {
    (void)unused;
    sigjmp_buf jb;
    if (!sigsetjmp(jb, 1)) {
        probe_thread = pthread_self();
        probe_jmp = &jb;
        fault_outer(ro_page + 0x123, 1);
    }
    probe_jmp = NULL;
    fault_ok[0] = last_fault.sig == SIGBUS && (last_fault.esr >> 26) == 0x24 && (last_fault.esr >> 6 & 1) && last_fault.far == (u64)(uintptr_t)(ro_page + 0x123);
    fault_walk_n = rt_frame_walk(last_fault.pc, last_fault.lr, last_fault.fp, fault_walk, 16);
    if (!sigsetjmp(jb, 1)) {
        probe_jmp = &jb;
        fault_outer(ro_page + 0x8123, 0);
    }
    probe_jmp = NULL;
    fault_ok[1] = (last_fault.sig == SIGBUS || last_fault.sig == SIGSEGV) && !(last_fault.esr >> 6 & 1) && last_fault.far == (u64)(uintptr_t)(ro_page + 0x8123);
    return 0;
}

static void test_fault(void) {
    ro_page = mmap(NULL, 0x10000, PROT_READ, MAP_PRIVATE | MAP_ANON, -1, 0);
    mprotect(ro_page + 0x8000, 0x4000, PROT_NONE);
    HANDLE t = CreateThread(NULL, 0, faulting_thread, NULL, 0, NULL);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    CHECK(fault_ok[0], "a write to a read-only page: signal %d, ESR %#llx, address %#llx", last_fault.sig, last_fault.esr, last_fault.far);
    CHECK(fault_ok[1], "a read of an inaccessible page: signal %d, ESR %#llx", last_fault.sig, last_fault.esr);
    int inner = 0, outer = 0;
    for (int i = 0; i < fault_walk_n; i++) {
        inner |= strstr(name_of(fault_walk[i]), "fault_inner") != NULL;
        outer |= strstr(name_of(fault_walk[i]), "fault_outer") != NULL;
    }
    CHECK(inner && outer && fault_walk_n >= 3, "the frame walk from a fault's context: %d frames (inner %d, outer %d)", fault_walk_n, inner, outer);
    munmap(ro_page, 0x10000);

    /* made-up chains (pc 0x1000, lr 0x3000): a record ending the chain gives its return address
     * once, after lr - lr not again after it */
    _Alignas(16) u64 rec[4] = {0, 0x2000, 0, 0};
    void *f[8];
    int n = rt_frame_walk(0x1000, 0x3000, (u64)(uintptr_t)rec, f, 8);
    CHECK(n == 3 && f[0] == (void *)0x1000 && f[1] == (void *)0x3000 && f[2] == (void *)0x2000, "a walk of the last record, lr not saved: %d frames", n);
    n = rt_frame_walk(0x1000, 0x2000, (u64)(uintptr_t)rec, f, 8);
    CHECK(n == 2 && f[1] == (void *)0x2000, "a walk of the last record, lr saved in it: %d frames", n);
    rec[1] = 0;
    n = rt_frame_walk(0x1000, 0x3000, (u64)(uintptr_t)rec, f, 8);
    CHECK(n == 2 && f[1] == (void *)0x3000, "a walk of a record with no return address: %d frames", n);
    rec[0] = (u64)(uintptr_t)&rec[2], rec[1] = 0x2000, rec[3] = 0x4000;
    n = rt_frame_walk(0x1000, 0x3000, (u64)(uintptr_t)rec, f, 8);
    CHECK(n == 4 && f[1] == (void *)0x3000 && f[2] == (void *)0x2000 && f[3] == (void *)0x4000, "a walk of two records: %d frames", n);
    n = rt_frame_walk(0x1000, 0x3000, 0, f, 8);
    CHECK(n == 2 && f[1] == (void *)0x3000, "a walk with no record: %d frames", n);
}

/* ---- UPnP's requests: a copy of runtime/linux/upnp.c, its state set by hand --------------- */
#define rt_upnp_add shimtest_upnp_add
#define rt_upnp_remove shimtest_upnp_remove
#include "linux/upnp.c"
#undef rt_upnp_add
#undef rt_upnp_remove

static int router;           /* a listening socket: the router */
static char router_got[8192];
static DWORD WINAPI router_once(void *unused) {   /* one request, answered after a while */
    (void)unused;
    int c = accept(router, NULL, NULL);
    size_t n = 0;
    router_got[0] = 0;
    for (ssize_t r; n + 1 < sizeof router_got && (r = recv(c, router_got + n, sizeof router_got - 1 - n, 0)) > 0;) {
        router_got[n += (size_t)r] = 0;
        if (strstr(router_got, "</s:Envelope>") || (!strncmp(router_got, "GET", 3) && strstr(router_got, "\r\n\r\n"))) break;
    }
    Sleep(200);   /* (recv must wait for it: the socket blocking again after its connect) */
    static const char reply[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
    send(c, reply, sizeof reply - 1, 0);
    close(c);
    return 0;
}

static void test_upnp(void) {
    struct sockaddr_in a = {0};
    socklen_t len = sizeof a;
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    router = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(router >= 0 && !bind(router, (struct sockaddr *)&a, sizeof a) && !listen(router, 2) && !getsockname(router, (struct sockaddr *)&a, &len),
          "a router on the loopback");
    HANDLE t = CreateThread(NULL, 0, router_once, NULL, 0, NULL);
    char out[1024] = "";
    int status = http("127.0.0.1", ntohs(a.sin_port), "GET /desc.xml HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n", out, sizeof out);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    CHECK(status == 200 && strstr(out, "\r\n\r\nok"), "an HTTP request answered late: status %d", status);

    snprintf(control_host, sizeof control_host, "127.0.0.1");
    control_port = ntohs(a.sin_port);
    snprintf(control_path, sizeof control_path, "/ctl");
    snprintf(service, sizeof service, "urn:schemas-upnp-org:service:WANIPConnection:1");
    mapped_port = 51000;
    t = CreateThread(NULL, 0, router_once, NULL, 0, NULL);
    shimtest_upnp_remove();
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    CHECK(strstr(router_got, "DeletePortMapping") && strstr(router_got, "<NewExternalPort>51000</NewExternalPort>"), "the mapping's removal: %.60s", router_got);
    double t0 = now_ms();
    shimtest_upnp_remove();
    CHECK(now_ms() - t0 < 100, "a second removal asked the router again");
    close(router);

    snprintf(control_host, sizeof control_host, "192.0.2.1");   /* (TEST-NET-1: nobody answers) */
    mapped_port = 51000;
    t0 = now_ms();
    shimtest_upnp_remove();
    double ms = now_ms() - t0;
    CHECK(ms < 6000, "removing a mapping through a router that does not answer took %.0f ms", ms);
    printf("  (UPnP: a router that does not answer given up on after %.0f ms)\n", ms);
}

/* ---- the profiler ------------------------------------------------------------------------ */
#ifndef SHIMTEST_HOST   /* (host.c's, and the renderer's) */
const char *rt_describe_host(u64 pc, char *buf, size_t n) { (void)pc; (void)buf; (void)n; return NULL; }
#endif
DWORD gfx_last_enter_thread(void) { return 0; }

static HANDLE park;
static volatile int spin_until_ms;
static char dump[1 << 16];
static size_t dump_len;
static void dump_line(const char *text) {
    size_t n = strlen(text);
    if (dump_len + n < sizeof dump) { memcpy(dump + dump_len, text, n); dump_len += n; dump[dump_len] = 0; }
}

__attribute__((noinline)) void shimtest_parked(void) { WaitForSingleObject(park, INFINITE); __asm__ volatile("" ::: "memory"); }
__attribute__((noinline)) void shimtest_spin(void) {
    volatile u64 x = 0;
    while (now_ms() < spin_until_ms) for (int i = 0; i < 1000; i++) x += (u64)i * i;
}
static DWORD WINAPI parked_thread(void *desc) {
    SetThreadDescription(GetCurrentThread(), desc);
    shimtest_parked();
    return 0;
}
static DWORD WINAPI busy_thread(void *unused) {
    (void)unused;
    SetThreadDescription(GetCurrentThread(), L"busy thread");
    shimtest_spin();
    return 0;
}
static DWORD WINAPI vsync_thread(void *unused) {   /* (presents: a frame every 5 ms, one of 40) */
    (void)unused;
    SetThreadDescription(GetCurrentThread(), L"vsync");
    for (int i = 0; i < 60; i++) {
        Sleep(i == 40 ? 40 : 5);
        hitch_present();
    }
    return 0;
}

static void test_profiler(void) {
    park = CreateEventW(NULL, TRUE, FALSE, NULL);
    DWORD parked_id = 0;
    HANDLE p = CreateThread(NULL, 0, parked_thread, L"parked thread", 0, &parked_id), q = CreateThread(NULL, 0, parked_thread, L"guest main thread", 0, NULL);
    Sleep(50);
    dump_len = 0;
    CHECK(profile_dump_threads(dump_line), "profile_dump_threads");
    char head[96];
    snprintf(head, sizeof head, "thread %u parked thread (CPU ", (unsigned)parked_id);
    char *at = strstr(dump, head), *next = at ? strstr(at + 1, "\nthread ") : NULL;
    if (next) *next = 0;
    CHECK(at && strstr(at, "shimtest_parked") && strstr(at, "WaitForSingleObject") && strstr(at, "libsystem_"),
          "the parked thread's stack:\n%s", at ? at : dump);
    if (next) *next = '\n';
    if (getenv("SHIMTEST_VERBOSE")) printf("%s", dump);

    /* a second of sampling, a thread busy all of it */
    spin_until_ms = (int)now_ms() + 1600;
    HANDLE b = CreateThread(NULL, 0, busy_thread, NULL, 0, NULL);
    double t0 = now_ms();
    profile_start(1.0);
    CHECK(profile_running(), "the profiler did not start");
    while (profile_running() && now_ms() - t0 < 20000) Sleep(20);
    CHECK(!profile_running(), "the profiler did not finish");
    struct stat bs;
    const char *where = !stat("build", &bs) && S_ISDIR(bs.st_mode) ? "build/" : "";   /* (where the profiler writes) */
    DIR *d = opendir(*where ? "build" : ".");
    char newest[256] = "", name[256];
    for (struct dirent *e; d && (e = readdir(d));) {
        snprintf(name, sizeof name, "%s%s", where, e->d_name);
        if (!strncmp(e->d_name, "profile_", 8) && strcmp(name, newest) > 0) snprintf(newest, sizeof newest, "%s", name);
    }
    if (d) closedir(d);
    FILE *f = newest[0] ? fopen(newest, "r") : NULL;
    static char report[1 << 18];
    size_t n = f ? fread(report, 1, sizeof report - 1, f) : 0;
    report[n] = 0;
    if (f) fclose(f);
    char *busiest = strstr(report, "Busiest code");
    CHECK(f && strstr(report, "Sampling profile: 1 s") && busiest && strstr(busiest, "shimtest_spin") && strstr(report, "busy thread"),
          "the profile (%s):\n%.3000s", newest, report);
    if (getenv("SHIMTEST_VERBOSE")) printf("%.6000s", report);
    if (newest[0]) unlink(newest);
    WaitForSingleObject(b, INFINITE);
    CloseHandle(b);

    /* the hitch log: a 40 ms frame, the guest's main thread waiting through it */
    char path[] = "/tmp/shimtest.hitch.XXXXXX";
    int fd = mkstemp(path), saved = dup(2);
    fflush(stderr);
    dup2(fd, 2);
    setenv("SSBU_HITCH", "20", 1);
    HANDLE v = CreateThread(NULL, 0, vsync_thread, NULL, 0, NULL);
    WaitForSingleObject(v, INFINITE);
    CloseHandle(v);
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    static char log[1 << 16];
    n = (size_t)pread(fd, log, sizeof log - 1, 0);
    log[n < sizeof log ? n : 0] = 0;
    close(fd);
    unlink(path);
    char *frame = strstr(log, "[hitch] frame");
    CHECK(strstr(log, "[hitch] logging frames over 20 ms") && frame && strstr(frame, "main:") && strstr(frame, "WaitForSingleObject") &&
          strstr(frame, "vsync:"), "the hitch log:\n%s", log);
    if (getenv("SHIMTEST_VERBOSE")) printf("%s", log);
    SetEvent(park);
    WaitForSingleObject(p, INFINITE);
    WaitForSingleObject(q, INFINITE);
    CloseHandle(p);
    CloseHandle(q);
    CloseHandle(park);
}

/* ---- the loader and kernel, on a made-up game (-DSHIMTEST_HOST) -------------------------- */
#ifdef SHIMTEST_HOST
#include "host.h"
#include "fight_test.h"   /* (SSBU_FIXED_SEED's entropy) */

int rt_host_main(int argc, char **argv);   /* (runtime/host.c's main, renamed) */
u64 rt_guest_copy(const void *data, size_t size);
DWORD kernel_lock_owner(void);

/* what the rest of the runtime would give the loader and kernel */
u64 gpu_stat_batches, gpu_stat_lists, gpu_stat_draws, gpu_stat_dispatches, gfx_stat_dispatched, gfx_stat_residents, gfx_stat_residents_retired, gfx_stat_clears,
    gfx_stat_drawn, gfx_stat_skipped, gfx_stat_shaders, gfx_stat_shader_failures, gfx_stat_textures, gfx_stat_unpacked, gfx_stat_watch_us,
    gfx_skip_shader, gfx_skip_topology, gfx_skip_target, gfx_stat_unmap_dropped;
u64 present_frames(void) { return 0; }
void gpu_memory_unmapping(u64 addr, u64 size) { (void)addr; (void)size; }   /* (hle_nv.c's: no GPU here) */
void gfx_report_compiles(void (*line)(const char *text)) { line("(shimtest: no shaders)\n"); }
void gfx_status_lines(void) {}   /* (the renderer's [status] lines: none here) */
void settings_args(int argc, char **argv) { (void)argc; (void)argv; }   /* (hle_settings.c's --online: no netplay here) */
void display_dpi_aware(void) {}
int input_seq_on(void) { return 0; }   /* (hle_hid.c's SSBU_SEQ, and hle_movie_skip.c's movies: none here) */
void input_seq_mark(const char *what) { (void)what; }
void movie_test_start(void) {}
void test_fight_start(void) {}   /* (hle_fight_test.c's switches: none here) */
void fiber_counts(u32 *slots, u32 *finalized, u64 *made) { *slots = *finalized = 0; *made = 0; }
void *ipc_connect(const char *port) { return !strcmp(port, "sm:") ? (void *)&checks : NULL; }
u32 ipc_request(Cpu *c, void *session, u64 message, u64 size) { (void)c; (void)session; (void)message; (void)size; return 0; }
void ipc_close(void *session) { (void)session; }
int rt_window_init(void) { return 0; }
_Noreturn void rt_window_loop(int every, void (*status)(void)) { (void)every; (void)status; for (;;) Sleep(1000); }

#define MAIN 0x7100000000ull
#define SUBSDK 0x7108000000ull
#define SDK 0x7109000000ull
#define NRO 0x7200000000ull
static volatile int inits;
static void say(int ok, const char *what) { fprintf(stderr, "shimtest-host: %s: %s\n", what, ok ? "ok" : "FAIL"); }

static void g_init(Cpu *c) { (void)c; inits++; }
static void hle_test_native(Cpu *c) { c->x[0] = 0x1234; }
static void g_thread(Cpu *c) {   /* a guest thread: leaves through svcExitThread (a longjmp to its start) */
    st32(HEAP_BASE + 0x6000, 0xBEEF);
    rt_svc(c, 0x0A);
}
static void g_waiter(Cpu *c) {   /* waits on the event at HEAP_BASE + 0x5100 until it is signalled */
    c->x[1] = HEAP_BASE + 0x5100; c->x[2] = 1; c->x[3] = (u64)-1;
    rt_svc(c, 0x18);
    st32(HEAP_BASE + 0x6004, (u32)c->x[0] + 1);
    rt_svc(c, 0x0A);
}
static void g_crash(Cpu *c) { (void)c; st64(HEAP_BASE + (16 << 20), 1); }   /* (beyond the heap: not committed) */
static void g_trap(Cpu *c) { (void)c; __builtin_trap(); }
static void g_abort(Cpu *c) { (void)c; abort(); }

static void g_start(Cpu *c) {   /* nn::init::Start, standing in for the game */
    const char *mode = getenv("SHIMTEST_MODE");
    if (mode && !strcmp(mode, "trap")) rt_call(c, MAIN + 0x500);
    if (mode && !strcmp(mode, "abort")) rt_call(c, MAIN + 0x600);
    rt_call(c, c->x[3]);   /* call_initializers */
    say(inits == 3, "the modules' initialisers ran");
    struct rlimit rl;
    int max_files = 0;
    size_t len = sizeof max_files;
    sysctlbyname("kern.maxfilesperproc", &max_files, &len, NULL, 0);
    say(!getrlimit(RLIMIT_NOFILE, &rl) && rl.rlim_cur >= (rlim_t)(max_files < 10240 ? max_files : 10240), "the open-file limit raised");
    say(ld32(MAIN + 0x8000) == 0xC0DE8000u && ld32(SDK + 0x10) == 0xC0DE0010u && *(u32 *)G2H(MAIN + 0x8000) == 0xC0DE8000u, "the images at their guest addresses");
    say(ld64(MAIN + 0x9000) == SDK + 0x8000 + 8, "a symbol relocation across modules");
    u64 native = ld64(MAIN + 0x9008);
    c->x[0] = 0;
    if (native - NATIVE_BASE < 0x100000) rt_call(c, native);
    say(c->x[0] == 0x1234, "an import bound to a native function, called through rt_lookup");
    u64 copy = rt_guest_copy("a guest copy", 13);
    say(copy - NATIVE_BASE < 0x100000 && !strcmp(P(const char, copy), "a guest copy"), "rt_guest_copy");
    c->x[1] = 4 << 20;
    rt_svc(c, 0x01);
    say(c->x[0] == 0 && c->x[1] == HEAP_BASE, "svcSetHeapSize");
    st64(HEAP_BASE + 0x1000, 42);
    say(ld64(HEAP_BASE + 0x1000) == 42 && *(u64 *)G2H(HEAP_BASE + 0x1000) == 42, "the heap");
    memcpy(G2H(HEAP_BASE + 0x2000), "hello from the guest\n", 21);
    c->x[0] = HEAP_BASE + 0x2000; c->x[1] = 21;
    rt_svc(c, 0x27);
    memcpy(G2H(HEAP_BASE + 0x3000), "sm:", 4);
    c->x[1] = HEAP_BASE + 0x3000;
    rt_svc(c, 0x1F);
    say(c->x[0] == 0 && c->x[1] && c->x[1] < 0x40000000, "svcConnectToNamedPort with the name in guest memory");   /* (either handle layout: kernel.c) */
    u64 dst = STACK_BASE + 0x100000;
    c->x[0] = dst; c->x[1] = HEAP_BASE; c->x[2] = 0x10000;
    rt_svc(c, 0x04);
    say(c->x[0] == 0 && ld64(dst + 0x1000) == 42, "svcMapMemory");
    st64(dst + 0x1000, 77);
    c->x[0] = dst; c->x[1] = HEAP_BASE; c->x[2] = 0x10000;
    rt_svc(c, 0x05);
    MEMORY_BASIC_INFORMATION mi;
    say(ld64(HEAP_BASE + 0x1000) == 77 && VirtualQuery(G2H(dst), &mi, sizeof mi) && mi.State == MEM_RESERVE, "svcUnmapMemory: copied back, decommitted");
    c->x[0] = HEAP_BASE + 0x4000; c->x[2] = HEAP_BASE + 0x10;
    rt_svc(c, 0x06);
    say(ld64(HEAP_BASE + 0x4000) == HEAP_BASE && ld32(HEAP_BASE + 0x4010) == 5, "svcQueryMemory");
    /* svcGetInfo 11, the random entropy (sub-ids 0 and 3): the clock's, unless SSBU_FIXED_SEED (test_host's SSBU_CWD run) */
    u64 entropy[2], seed, ok = 1;
    for (int i = 0; i < 2; i++) {
        c->x[1] = 11; c->x[2] = 0; c->x[3] = i ? 3 : 0;
        rt_svc(c, 0x29);
        ok &= c->x[0] == 0;
        entropy[i] = c->x[1];
    }
    if (fixed_seed_parse(getenv("SSBU_FIXED_SEED"), &seed))
        say(ok && entropy[0] == fixed_entropy(seed, 0) && entropy[1] == fixed_entropy(seed, 3), "svcGetInfo 11 with SSBU_FIXED_SEED: the seed's");
    else   /* (golden * (sub + 1) ^ the clock's ns, which had not gone on by a second since) */
        say(ok && rt_clock_ns() - (entropy[0] ^ FIXED_GOLDEN) < 1000000000ull && rt_clock_ns() - (entropy[1] ^ FIXED_GOLDEN * 4) < 1000000000ull,
            "svcGetInfo 11: the clock's");
    c->x[0] = ALIAS_BASE + 0x100000; c->x[1] = 0x20000;
    rt_svc(c, 0x2C);
    st32(ALIAS_BASE + 0x105000, 1);   /* (a write the write watch takes, in host.c's handler) */
    void *pages[16];
    ULONG_PTR n = 16;
    ULONG gran;
    say(!GetWriteWatch(WRITE_WATCH_FLAG_RESET, G2H(ALIAS_BASE + 0x100000), 0x20000, pages, &n, &gran) && n == 4 && pages[0] == G2H(ALIAS_BASE + 0x104000),
        "physical memory in the alias area, watched");
    c->x[0] = ALIAS_BASE + 0x100000; c->x[1] = 0x20000;
    rt_svc(c, 0x2D);
    say(VirtualQuery(G2H(ALIAS_BASE + 0x104000), &mi, sizeof mi) && mi.State == MEM_RESERVE, "svcUnmapPhysicalMemory");

    /* a guest thread that leaves through svcExitThread; one that waits (in the thread dump) until signalled */
    c->x[1] = MAIN + 0x400; c->x[2] = 0; c->x[3] = STACK_BASE + 0x1000; c->x[4] = 44; c->x[5] = 0;
    rt_svc(c, 0x08);
    u32 t1 = (u32)c->x[1];
    c->x[0] = t1;
    rt_svc(c, 0x09);
    st32(HEAP_BASE + 0x5000, t1);
    c->x[1] = HEAP_BASE + 0x5000; c->x[2] = 1; c->x[3] = 5000000000ull;
    rt_svc(c, 0x18);
    say(c->x[0] == 0 && ld32(HEAP_BASE + 0x6000) == 0xBEEF, "a guest thread ran, left through svcExitThread, was waited for");
    rt_svc(c, 0x45);
    u32 ev = (u32)c->x[1];
    st32(HEAP_BASE + 0x5100, ev);
    c->x[1] = MAIN + 0x200; c->x[2] = 0; c->x[3] = STACK_BASE + 0x2000; c->x[4] = 45; c->x[5] = 0;
    rt_svc(c, 0x08);
    u32 t2 = (u32)c->x[1];
    c->x[0] = t2;
    rt_svc(c, 0x09);
    Sleep(100);
    fprintf(stderr, "shimtest-host: the thread dump:\n");
    kernel_dump_threads();
    fprintf(stderr, "shimtest-host: the thread dump's end\n");
    say(kernel_lock_owner() == 0, "the kernel's lock free, its owner 0");
    c->x[0] = ev;
    rt_svc(c, 0x11);
    st32(HEAP_BASE + 0x5000, t2);
    c->x[1] = HEAP_BASE + 0x5000; c->x[2] = 1; c->x[3] = 5000000000ull;
    rt_svc(c, 0x18);
    say(c->x[0] == 0 && ld32(HEAP_BASE + 0x6004) == 1, "a waiting guest thread signalled");

    /* an NRO the game loads: reserved at start, mapped into its reservation now */
    memset(G2H(HEAP_BASE + 0x7000), 0, 0x100);
    *(u8 *)G2H(HEAP_BASE + 0x7040) = 0xAB;
    say(rt_load_nro(c, HEAP_BASE + 0x7000) == NRO && inits == 4 && ld32(NRO + 0x8000) == 0xC0DE8000u, "rt_load_nro");
    fflush(stderr);
    rt_call(c, MAIN + 0x300);   /* the crash */
    say(0, "still running after the crash");
}

static const RtFunc main_funcs[] = {{MAIN + 0x100, g_init}, {MAIN + 0x200, g_waiter}, {MAIN + 0x300, g_crash}, {MAIN + 0x400, g_thread},
                                    {MAIN + 0x500, g_trap}, {MAIN + 0x600, g_abort}};
static const char *const main_imports[] = {"sdk_value", "hle_test_native"};
static const u8 main_weak[] = {0, 0};
static const RtReloc main_relocs[] = {{0x9000, 8, 0}, {0x9008, 0, 1}};
/* (build id: 13.0.5's, which host.c requires of the main module before it starts the game) */
const RtModule rt_module_main = {"main", MAIN, 0x20000, 0x8000, MAIN + 0x100, main_funcs, 6, NULL, 0, main_imports, main_weak, 2, main_relocs, 2,
                                 "21450C647B8C5940ED5B68218EA128FD3C5D597D"};
static const RtFunc subsdk_funcs[] = {{SUBSDK + 0x100, g_init}};
const RtModule rt_module_subsdk0 = {"subsdk0", SUBSDK, 0x20000, 0x8000, SUBSDK + 0x100, subsdk_funcs, 1, NULL, 0, NULL, NULL, 0, NULL, 0, ""};
static const RtFunc sdk_funcs[] = {{SDK + 0x100, g_init}, {SDK + 0x200, g_start}};
static const RtSym sdk_exports[] = {{"_ZN2nn4init5StartEmmPFvvES2_", SDK + 0x200}, {"sdk_value", SDK + 0x8000}};
const RtModule rt_module_sdk = {"sdk", SDK, 0x20000, 0x8000, SDK + 0x100, sdk_funcs, 2, sdk_exports, 2, NULL, NULL, 0, NULL, 0, ""};
static const RtFunc nro_funcs[] = {{NRO + 0x100, g_init}};
static const RtModule nro_test = {"nro_test", NRO, 0x20000, 0x8000, NRO + 0x100, nro_funcs, 1, NULL, 0, NULL, NULL, 0, NULL, 0, "AB"};
const RtModule *const rt_nro_modules[] = {&nro_test};
const u32 rt_nro_count = 1;
const HleEntry hle_table[] = {{"hle_test_native", hle_test_native}};
const u32 hle_count = 1;

/* Run the made-up game in a child: exe with args, in folder cwd, SHIMTEST_MODE=mode (and SSBU_LOG
 * and SSBU_CWD when given); its output (to the file log: also SSBU_LOG's) in out; its exit status. */
static int spawn_game(const char *exe, char *const *args, const char *cwd, const char *mode, const char *log, int via_ssbu_log, const char *ssbu_cwd,
                      char *out, size_t cap) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, via_ssbu_log ? "/dev/null" : log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    posix_spawn_file_actions_addchdir_np(&fa, cwd);
    setenv("SHIMTEST_MODE", mode, 1);
    if (via_ssbu_log) setenv("SSBU_LOG", log, 1);
    if (ssbu_cwd) setenv("SSBU_CWD", ssbu_cwd, 1);
    pid_t pid;
    int status = -1;
    if (posix_spawn(&pid, exe, &fa, NULL, args, environ) || waitpid(pid, &status, 0) != pid) status = -1;
    unsetenv("SHIMTEST_MODE");
    unsetenv("SSBU_LOG");
    unsetenv("SSBU_CWD");
    posix_spawn_file_actions_destroy(&fa);
    FILE *f = fopen(log, "r");
    size_t n = f ? fread(out, 1, cap - 1, f) : 0;
    out[n] = 0;
    if (f) fclose(f);
    return status;
}

static int run_game(const char *dir, const char *mode, char *out, size_t cap) {
    char log[512], modules[512];
    snprintf(log, sizeof log, "%s/out_%s.txt", dir, mode);
    snprintf(modules, sizeof modules, "%s/modules", dir);
    char *args[] = {(char *)rt_exe_path(), "--host", modules, "--status", "0", NULL};
    return spawn_game(rt_exe_path(), args, dir, mode, log, 0, NULL, out, cap);
}

static int newest_report(const char *dir, char *path, size_t n) {   /* <dir>/reports/crash_*.txt: 1 if there is one */
    char reports[512];
    snprintf(reports, sizeof reports, "%s/reports", dir);
    DIR *d = opendir(reports);
    int found = 0;
    for (struct dirent *e; d && (e = readdir(d));)
        if (!strncmp(e->d_name, "crash_", 6)) { snprintf(path, n, "%s/%s", reports, e->d_name); found = 1; }
    if (d) closedir(d);
    return found;
}

static void write_images(const char *modules) {   /* word i holds 0xC0DE0000 + its offset */
    const char *names[] = {"main", "subsdk0", "sdk", "nro_test"};
    char path[600];
    mkdir(modules, 0755);
    for (int m = 0; m < 4; m++) {
        snprintf(path, sizeof path, "%s/%s.reloc.bin", modules, names[m]);
        FILE *f = fopen(path, "wb");
        for (u32 i = 0; i < 0x20000; i += 4) { u32 w = 0xC0DE0000u + (i & 0xFFFF); fwrite(&w, 4, 1, f); }
        fclose(f);
    }
}

static void test_host(void) {
    char dir[] = "/tmp/shimtest.game.XXXXXX", path[512];
    CHECK(mkdtemp(dir) != NULL, "a folder for the game");
    snprintf(path, sizeof path, "%s/modules", dir);
    write_images(path);
    static char out[1 << 17];
    unsetenv("SSBU_FIXED_SEED");   /* (set only for the SSBU_CWD run, below) */
    int status = run_game(dir, "crash", out, sizeof out);
    if (getenv("SHIMTEST_VERBOSE")) printf("%s", out);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 5, "the game's exit status %#x (want 5: a crash report)", status);
    CHECK(strstr(out, "loaded 3 modules: 9 functions, 2 exported symbols, 2 symbol relocations; 1 symbols bound to native code"), "the loader's line:\n%s", out);
    CHECK(strstr(out, "[guest] hello from the guest"), "svcOutputDebugString");
    CHECK(strstr(out, "[mem] write watch: page protection"), "the write watch's line");
    int oks = 0;
    for (const char *at = out; (at = strstr(at, ": ok\n")); at++) oks++;
    CHECK(oks == 19 && !strstr(out, ": FAIL") && strstr(out, "svcGetInfo 11: the clock's: ok"), "the game's own checks: %d ok\n%s", oks, out);
    const char *d0 = strstr(out, "the thread dump:\n"), *d1 = strstr(out, "the thread dump's end");
    const char *t3 = d0 ? strstr(d0, "  thread 3 (priority 45): running or in a system call\n      main+0x200\n") : NULL;
    CHECK(t3 && t3 < d1 && strstr(d0, "  thread 1 (priority 44)"), "the thread dump (each guest thread stopped, its stack read):\n%.*s", d0 && d1 ? (int)(d1 - d0) : 0, d0 ? d0 : "");
    const char *crash = strstr(out, "*** host exception (signal 10: bus error) during start-up after static constructors: writing address 0x7401000000");
    CHECK(crash != NULL, "the crash report's first line:\n%s", out);
    const char *bt = crash ? strstr(crash, "guest backtrace (innermost first):\n  main+0x300\n") : NULL;
    CHECK(bt && strstr(bt, "sdk+0x200"), "the crash's guest backtrace:\n%s", crash ? crash : "");
    const char *rep = crash ? strstr(crash, "report: reports/crash_") : NULL;
    char report[256] = "";
    if (rep) sscanf(rep, "report: %200s", report);
    snprintf(path, sizeof path, "%s/%s", dir, report);
    struct stat st;
    CHECK(rep && !stat(path, &st) && st.st_size > 100, "the crash report file %s", path);
    status = run_game(dir, "trap", out, sizeof out);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 5 && strstr(out, "*** host exception (signal 5: trap) during SDK initialisation") &&
          strstr(out, "guest backtrace (innermost first):\n  main+0x500\n"), "a trap's report (status %#x):\n%s", status, out);
    status = run_game(dir, "abort", out, sizeof out);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 5 && strstr(out, "*** host exception (signal 6: abort) during SDK initialisation"),
          "an abort's report (status %#x):\n%s", status, out);
    CHECK(strstr(out, "main+0x600"), "an abort's guest backtrace (want main+0x600):\n%s", out);

    /* started as `open` starts the app: from /, output to SSBU_LOG, no directory given - the
     * tree's modules (<tree>/build-mac/WindowSmash.app/Contents/MacOS/ssbu), the root (the
     * nearest folder up with tools/recomp.py) the working folder */
    char app[700], exe[800], cmd[2000];
    snprintf(app, sizeof app, "%s/root/v13/build-mac/WindowSmash.app/Contents/MacOS", dir);
    snprintf(cmd, sizeof cmd, "mkdir -p '%s' '%s/root/tools' && cp '%s' '%s/ssbu' && touch '%s/root/tools/recomp.py'", app, dir, rt_exe_path(), app, dir);
    CHECK(!system(cmd), "an app to start: %s", cmd);
    snprintf(path, sizeof path, "%s/root/v13/dump", dir);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/root/v13/dump/modules", dir);
    write_images(path);
    snprintf(exe, sizeof exe, "%s/ssbu", app);
    snprintf(path, sizeof path, "%s/root/play.log", dir);
    char *app_args[] = {exe, "--host-app", NULL};
    status = spawn_game(exe, app_args, "/", "crash", path, 1, NULL, out, sizeof out);
    char root[600], want[1200];
    snprintf(root, sizeof root, "%s/root", dir);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 5 && strstr(out, "loaded 3 modules") && strstr(out, "/root/v13/build-mac/WindowSmash.app/Contents/MacOS/ssbu, built"),
          "the app, started from / (status %#x), its SSBU_LOG:\n%s", status, out);
    CHECK(newest_report(root, want, sizeof want), "the app's crash report not in the root's reports/: %s", root);
    /* SSBU_CWD: the working folder given (and SSBU_FIXED_SEED: the kernel's entropy the seed's) */
    snprintf(path, sizeof path, "%s/cwd", dir);
    mkdir(path, 0755);
    snprintf(want, sizeof want, "%s/out_cwd.txt", dir);
    char modules[600];
    snprintf(modules, sizeof modules, "%s/modules", dir);
    char *cwd_args[] = {(char *)rt_exe_path(), "--host", modules, "--status", "0", NULL};
    setenv("SSBU_FIXED_SEED", "42", 1);
    status = spawn_game(rt_exe_path(), cwd_args, "/", "crash", want, 0, path, out, sizeof out);
    unsetenv("SSBU_FIXED_SEED");
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 5 && newest_report(path, want, sizeof want), "SSBU_CWD: no report in %s (status %#x)", path, status);
    CHECK(strstr(out, "svcGetInfo 11 with SSBU_FIXED_SEED: the seed's: ok") != NULL, "SSBU_FIXED_SEED=42: svcGetInfo 11's entropy\n%s", out);
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (!getenv("SHIMTEST_KEEP")) system(cmd);
}
#endif

/* ---- main --------------------------------------------------------------------------------- */
#define RUN(test) do { printf("%s\n", #test); fflush(stdout); test(); } while (0)

int main(int argc, char **argv) {
#ifdef SHIMTEST_HOST
    if (argc > 2 && !strcmp(argv[1], "--host")) {   /* (the child: the game, through runtime/host.c's main) */
        char *args[] = {argv[0], argv[2], argv[3], argv[4], NULL};
        return rt_host_main(argc - 1, args);
    }
    if (argc == 2 && !strcmp(argv[1], "--host-app")) {   /* (the app: no directory given) */
        char *args[] = {argv[0], NULL};
        return rt_host_main(1, args);
    }
#endif
    setvbuf(stdout, NULL, _IOLBF, 0);
    install_fault_handler();
    if (argc > 1 && !strcmp(argv[1], "--waits")) {   /* (again, with the other way of waiting: below) */
        RUN(test_waits);
        printf("%d checks, %d failed\n", checks, failures);
        return failures != 0;
    }
    RUN(test_window);
    RUN(test_memory);
    RUN(test_write_watch);
    RUN(test_write_watch_race);
    RUN(test_fibers);
    RUN(test_waits);
    RUN(test_threads);
    RUN(test_files);
    RUN(test_clock_and_more);
    RUN(test_fault);
    RUN(test_upnp);
    RUN(test_profiler);
#ifdef SHIMTEST_HOST
    RUN(test_host);
#endif
    {   /* the waits once more through __ulock_wait2, macOS 14.0 - 14.3's way */
        pid_t pid;
        char *args[] = {argv[0], "--waits", NULL};
        setenv("SSBU_FUTEX", "ulock", 1);
        int status = -1;
        CHECK(!posix_spawn(&pid, rt_exe_path(), NULL, NULL, args, environ) && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status),
              "the waits through __ulock_wait2 (status %#x)", status);
        unsetenv("SSBU_FUTEX");
    }
    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
