/* Self-test of runtime/plat_posix.c (the Win32 subset of the Linux and macOS builds). Build from runtime/:
 *   clang -O1 -include plat/plat_pre.h -I plat $(pkg-config --cflags sdl3 openssl) ../tests/plat_selftest.c plat_posix.c -o plat_selftest $(pkg-config --libs sdl3 openssl) -lpthread -ldl */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int fails;
__attribute__((noinline)) static void dirty_stack(void) { volatile uint8_t b[4096]; for (int i = 0; i < (int)sizeof b; i++) b[i] = 0xff; }
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s (line %d)\n", what, __LINE__); fails++; } else printf("ok   %s\n", what); } while (0)

static volatile LONG counter;
static CRITICAL_SECTION cs;
static CONDITION_VARIABLE cv = CONDITION_VARIABLE_INIT;
static int ready;
static DWORD WINAPI worker(LPVOID arg) {
    for (int i = 0; i < 100000; i++) InterlockedIncrement(&counter);
    EnterCriticalSection(&cs); ready++; LeaveCriticalSection(&cs);
    WakeAllConditionVariable(&cv);
    return (DWORD)(uintptr_t)arg;
}
static HANDLE ev;
static DWORD WINAPI setter(LPVOID arg) { (void)arg; Sleep(50); SetEvent(ev); return 0; }
static void *main_fiber, *fib;
static int fiber_steps;
static void fiber_fn(LPVOID p) { for (;;) { fiber_steps += (int)(uintptr_t)p; SwitchToFiber(main_fiber); } }
static BOOL CALLBACK once_fn(PINIT_ONCE o, PVOID p, PVOID *c) { (void)o; (void)c; (*(int *)p)++; return TRUE; }
#include <unistd.h>
static uint8_t *ww_mem;
static DWORD WINAPI ww_writer(LPVOID arg) {          /* writes one byte in each 4 KB piece of its quarter of 1 MB */
    for (int k = 0; k < 64; k++) ww_mem[(uintptr_t)arg * 0x40000 + (uintptr_t)k * 0x1000 + 17] = (uint8_t)k;
    return 0;
}
static int has_page(void **v, ULONG_PTR n, void *p) { for (ULONG_PTR i = 0; i < n; i++) if (v[i] == p) return 1; return 0; }

#ifdef __APPLE__
#include <pthread/qos.h>
static void sleep_overshoot(const char *what) {     /* macOS coalesces the timers of a process that is not interactive */
    LARGE_INTEGER a, b, f; double worst = 0;
    QueryPerformanceFrequency(&f);
    for (int i = 0; i < 5; i++) { QueryPerformanceCounter(&a); Sleep(20); QueryPerformanceCounter(&b); double ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart; if (ms > worst) worst = ms; }
    printf("     Sleep(20), worst of 5, %s: %.1f ms\n", what, worst);
}
#endif
int main(void) {
#ifdef __APPLE__
    sleep_overshoot("default QoS");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    sleep_overshoot("QOS_CLASS_USER_INTERACTIVE");
#endif
    /* interlocked */
    volatile LONG v = 5; volatile LONG64 v64 = 1;
    CHECK(InterlockedIncrement(&v) == 6 && InterlockedExchangeAdd(&v, 4) == 6 && v == 10, "Interlocked add/inc");
    CHECK(InterlockedCompareExchange(&v, 20, 10) == 10 && v == 20 && InterlockedCompareExchange(&v, 30, 10) == 20 && v == 20, "InterlockedCompareExchange");
    CHECK(InterlockedExchange64(&v64, 7) == 1 && InterlockedAdd64(&v64, 3) == 10, "Interlocked 64");
    void *volatile pp = NULL; int x;
    CHECK(InterlockedCompareExchangePointer(&pp, &x, NULL) == NULL && pp == &x, "InterlockedCompareExchangePointer");
    /* threads + critical section + condition variable */
    InitializeCriticalSection(&cs);
    HANDLE t[4];
    for (int i = 0; i < 4; i++) t[i] = CreateThread(NULL, 0, worker, (LPVOID)(uintptr_t)(i + 1), 0, NULL);
    EnterCriticalSection(&cs);
    EnterCriticalSection(&cs);                               /* recursive */
    LeaveCriticalSection(&cs);
    while (ready < 4) SleepConditionVariableCS(&cv, &cs, INFINITE);
    LeaveCriticalSection(&cs);
    for (int i = 0; i < 4; i++) CHECK(WaitForSingleObject(t[i], 2000) == WAIT_OBJECT_0, "thread finished (WaitForSingleObject)");
    DWORD code = 0; GetExitCodeThread(t[2], &code);
    CHECK(code == 3, "GetExitCodeThread");
    for (int i = 0; i < 4; i++) CloseHandle(t[i]);
    CHECK(counter == 400000, "4 threads x 100000 InterlockedIncrement");
    /* timed waits */
    EnterCriticalSection(&cs);
    ULONGLONG a = GetTickCount64();
    BOOL r = SleepConditionVariableCS(&cv, &cs, 30);
    ULONGLONG b = GetTickCount64();
    LeaveCriticalSection(&cs);
    CHECK(!r && GetLastError() == 1460 && b - a >= 25 && b - a < 200, "SleepConditionVariableCS time-out ~30 ms");
    /* events */
    ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    CHECK(WaitForSingleObject(ev, 10) == WAIT_TIMEOUT, "auto event not set: time-out");
    HANDLE s = CreateThread(NULL, 0, setter, NULL, 0, NULL);
    CHECK(WaitForSingleObject(ev, 2000) == WAIT_OBJECT_0, "auto event set by another thread");
    CHECK(WaitForSingleObject(ev, 0) == WAIT_TIMEOUT, "auto event reset after one wait");
    WaitForSingleObject(s, INFINITE); CloseHandle(s);
    HANDLE m = CreateEventA(NULL, TRUE, TRUE, NULL);
    CHECK(WaitForSingleObject(m, 0) == WAIT_OBJECT_0 && WaitForSingleObject(m, 0) == WAIT_OBJECT_0, "manual event stays set");
    HANDLE two[2] = {ev, m};
    CHECK(WaitForMultipleObjects(2, two, FALSE, 100) == WAIT_OBJECT_0 + 1, "WaitForMultipleObjects any");
    HANDLE sem = CreateSemaphoreA(NULL, 0, 2, NULL);
    CHECK(ReleaseSemaphore(sem, 2, NULL) && !ReleaseSemaphore(sem, 1, NULL), "semaphore maximum");
    CHECK(WaitForSingleObject(sem, 0) == WAIT_OBJECT_0 && WaitForSingleObject(sem, 0) == WAIT_OBJECT_0 && WaitForSingleObject(sem, 0) == WAIT_TIMEOUT, "semaphore count");
    /* suspended thread */
    counter = 0;
    ready = 0;
    HANDLE su = CreateThread(NULL, 1 << 20, worker, NULL, CREATE_SUSPENDED, NULL);
    Sleep(30);
    CHECK(counter == 0, "CREATE_SUSPENDED thread does not run");
    ResumeThread(su);
    CHECK(WaitForSingleObject(su, 2000) == WAIT_OBJECT_0 && counter == 100000, "ResumeThread runs it");
    /* timer */
    HANDLE tm = CreateWaitableTimerExW(NULL, NULL, 2, TIMER_ALL_ACCESS);
    LARGE_INTEGER due; due.QuadPart = -200000;              /* 20 ms */
    LARGE_INTEGER q0, q1, f; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&q0);
    SetWaitableTimer(tm, &due, 0, NULL, NULL, FALSE);
    CHECK(WaitForSingleObject(tm, 1000) == WAIT_OBJECT_0, "waitable timer fires");
    QueryPerformanceCounter(&q1);
    double ms = (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)f.QuadPart;
    printf("     waitable timer: %.1f ms\n", ms);
    #ifdef __APPLE__                 /* GitHub's macOS runner VMs coalesce timers: up to ~95 ms seen for Sleep(20) on macos-14 (plan MAC1 "Timing"); not yet measured on macos-15 */
    CHECK(ms >= 19 && ms < 150, "waitable timer ~20 ms (macOS VM: < 150 ms)");
#else
    CHECK(ms >= 19 && ms < 60, "waitable timer ~20 ms");
#endif
    /* init once */
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT; int n = 0;
    InitOnceExecuteOnce(&once, once_fn, &n, NULL); InitOnceExecuteOnce(&once, once_fn, &n, NULL);
    CHECK(n == 1, "InitOnceExecuteOnce once");
    /* virtual memory */
    uint8_t *res = VirtualAlloc((void *)0x7300000000ULL, 0x100000, MEM_RESERVE, PAGE_NOACCESS);
    CHECK(res == (void *)0x7300000000ULL, "reserve at a fixed address");
    MEMORY_BASIC_INFORMATION mi;
    CHECK(VirtualQuery(res + 0x5000, &mi, sizeof mi) && mi.State == MEM_RESERVE && mi.AllocationBase == res, "VirtualQuery: reserved");
    CHECK(VirtualAlloc(res + 0x10000, 0x2000, MEM_COMMIT, PAGE_READWRITE) == res + 0x10000, "commit inside");
    res[0x10000] = 42; res[0x11fff] = 7;
    CHECK(VirtualQuery(res + 0x10800, &mi, sizeof mi) && mi.State == MEM_COMMIT && mi.Protect == PAGE_READWRITE && (uint8_t *)mi.BaseAddress == res + 0x10000 && mi.RegionSize == 0x2000, "VirtualQuery: committed run");
    CHECK(VirtualQuery(res + 0x12000, &mi, sizeof mi) && mi.State == MEM_RESERVE && mi.RegionSize == 0x100000 - 0x12000, "VirtualQuery: rest reserved");
    {   /* ReadProcessMemory (debug_mode.c's read that cannot fault): every call must work whatever the stack holds */
        int good = 0;
        for (int i = 0; i < 200; i++) {
            dirty_stack();
            uint8_t v[2] = {0};
            SIZE_T got = 0;
            good += ReadProcessMemory(GetCurrentProcess(), res + 0x10000, v, 1, &got) && got == 1 && v[0] == 42;
        }
        CHECK(good == 200, "ReadProcessMemory: 200 of 200 reads after the stack was filled with 0xff");
        uint8_t v = 0;
        CHECK(!ReadProcessMemory(GetCurrentProcess(), res + 0x5000, &v, 1, NULL), "ReadProcessMemory: reserved (unreadable) memory fails, no fault");
    }
    DWORD old;
    CHECK(VirtualProtect(res + 0x10000, 0x1000, PAGE_READONLY, &old) && old == PAGE_READWRITE, "VirtualProtect");
    CHECK(VirtualQuery(res + 0x10000, &mi, sizeof mi) && mi.Protect == PAGE_READONLY && mi.RegionSize == 0x1000, "VirtualQuery after protect");
    CHECK(IsBadWritePtr(res + 0x10000, 4) && !IsBadReadPtr(res + 0x10000, 4) && !IsBadWritePtr(res + 0x11000, 4) && IsBadReadPtr(res + 0x20000, 1), "IsBad*Ptr");
    VirtualProtect(res + 0x10000, 0x1000, PAGE_READWRITE, &old);
    VirtualFree(res + 0x10000, 0x2000, MEM_DECOMMIT);
    CHECK(VirtualQuery(res + 0x10000, &mi, sizeof mi) && mi.State == MEM_RESERVE, "decommit");
    VirtualAlloc(res + 0x10000, 0x1000, MEM_COMMIT, PAGE_READWRITE);
    CHECK(res[0x10000] == 0, "recommitted page is zero");
    {   /* 4 KB pieces inside one host page (16 KB on Apple Silicon): rights, decommit, zeroing */
        size_t hp = (size_t)getpagesize();
        SYSTEM_INFO si; GetSystemInfo(&si);
        printf("     host page %zu bytes, reported page %lu\n", hp, (unsigned long)si.dwPageSize);
        CHECK(si.dwPageSize == 4096, "GetSystemInfo: 4 KB pages (the bookkeeping's)");
        uint8_t *q = res + 0x40000;                                   /* a 64 KB-aligned host page */
        CHECK(VirtualAlloc(q + 0x1000, 0x1000, MEM_COMMIT, PAGE_READWRITE) == q + 0x1000, "commit one 4 KB piece of a host page");
        q[0x1000] = 1; q[0x1fff] = 2;
        CHECK(VirtualAlloc(q + 0x3000, 0x1000, MEM_COMMIT, PAGE_READONLY) == q + 0x3000 && q[0x3000] == 0, "commit a read-only piece beside it");
        CHECK(VirtualQuery(q, &mi, sizeof mi) && mi.State == MEM_RESERVE && mi.RegionSize == 0x1000, "the piece before stays reserved (bookkeeping)");
        CHECK(VirtualProtect(q + 0x1000, 0x1000, PAGE_READONLY, &old) && old == PAGE_READWRITE && IsBadWritePtr(q + 0x1000, 1), "VirtualProtect of one piece: read-only in the bookkeeping");
        CHECK(VirtualProtect(q + 0x1000, 0x1000, PAGE_READWRITE, &old) && old == PAGE_READONLY, "VirtualProtect back to read-write");
        q[0x1000] = 3;                                                 /* would fault if the host page had kept read-only */
        CHECK(VirtualAlloc(q + 0x2000, 0x1000, MEM_COMMIT, PAGE_READWRITE) == q + 0x2000, "commit the piece between");
        q[0x2000] = 9;
        CHECK(VirtualFree(q + 0x2000, 0x1000, MEM_DECOMMIT) && q[0x1000] == 3 && q[0x1fff] == 2, "decommit one piece: the neighbour keeps its data");
        CHECK(VirtualAlloc(q + 0x2000, 0x1000, MEM_COMMIT, PAGE_READWRITE) == q + 0x2000 && q[0x2000] == 0, "the decommitted piece comes back zero");
        CHECK(VirtualQuery(q + 0x3000, &mi, sizeof mi) && mi.Protect == PAGE_READONLY && mi.RegionSize == 0x1000, "the read-only piece keeps its bookkeeping");
        uint8_t *big = res + 0x80000;                                  /* commit across host pages, from the middle of one */
        CHECK(VirtualAlloc(big + 0x3000, 0x6000, MEM_COMMIT, PAGE_READWRITE) == big + 0x3000, "commit 24 KB starting at a 4 KB piece");
        big[0x3000] = 1; big[0x8fff] = 2;
        CHECK(VirtualFree(big + 0x4000, 0x4000, MEM_DECOMMIT) && big[0x3000] == 1 && big[0x8fff] == 2, "decommit a whole host page between two kept pieces");
        CHECK(VirtualAlloc(big + 0x4000, 0x4000, MEM_COMMIT, PAGE_READWRITE) && big[0x4000] == 0 && big[0x7fff] == 0, "the whole host page comes back zero");
    }
    CHECK(VirtualFree(res, 0, MEM_RELEASE) && VirtualQuery(res, &mi, sizeof mi) && mi.State == MEM_FREE, "release");
    uint8_t *any = VirtualAlloc(NULL, 12345, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(any && ((uintptr_t)any & 0xffff) == 0, "VirtualAlloc(NULL) 64 KB aligned");
    int local;
    CHECK(VirtualQuery(&local, &mi, sizeof mi) && mi.State == MEM_COMMIT && (mi.Protect & PAGE_READWRITE), "VirtualQuery of the stack (/proc/self/maps)");
#ifdef __linux__                 /* the comparison reads /proc (Linux only) */
    {   /* untracked memory against the text of /proc/self/maps: every mapping (start and middle) and every gap */
        static unsigned long lo[4096], hi[4096]; static char perm[4096][8]; int n = 0, bad = 0, asked = 0;
        FILE *f = fopen("/proc/self/maps", "r"); char line[512];
        while (f && n < 4096 && fgets(line, sizeof line, f)) if (sscanf(line, "%lx-%lx %7s", &lo[n], &hi[n], perm[n]) == 3 && hi[n] <= 0x800000000000UL) n++;
        if (f) fclose(f);
        for (int k = 0; k < n; k++) {
            if (lo[k] < (uintptr_t)any + 0x10000 && hi[k] > (uintptr_t)any) continue;          /* the tracked allocation above */
            DWORD want = perm[k][2] == 'x' ? (perm[k][1] == 'w' ? PAGE_EXECUTE_READWRITE : perm[k][0] == 'r' ? PAGE_EXECUTE_READ : PAGE_EXECUTE)
                                           : (perm[k][1] == 'w' ? PAGE_READWRITE : perm[k][0] == 'r' ? PAGE_READONLY : PAGE_NOACCESS);
            unsigned long at[2] = { lo[k], (lo[k] + (hi[k] - lo[k]) / 2) & ~0xffful };
            for (int j = 0; j < 2; j++, asked++)
                if (!VirtualQuery((void *)at[j], &mi, sizeof mi) || mi.State != MEM_COMMIT || mi.Protect != want || (uintptr_t)mi.AllocationBase != lo[k] || (uintptr_t)mi.BaseAddress + mi.RegionSize != hi[k]) {
                    if (bad++ < 5) printf("     %lx (mapping %lx-%lx %s): state %lx protect %lx base %p size %zx\n", at[j], lo[k], hi[k], perm[k], (unsigned long)mi.State, (unsigned long)mi.Protect, mi.AllocationBase, (size_t)mi.RegionSize);
                }
            if (k + 1 < n && hi[k] < lo[k + 1] && !(hi[k] < (uintptr_t)any + 0x10000 && lo[k + 1] > (uintptr_t)any)) {
                asked++;
                if (!VirtualQuery((void *)hi[k], &mi, sizeof mi) || mi.State != MEM_FREE || mi.RegionSize != lo[k + 1] - hi[k]) {
                    if (bad++ < 5) printf("     gap %lx-%lx: state %lx size %zx\n", hi[k], lo[k + 1], (unsigned long)mi.State, (size_t)mi.RegionSize);
                }
            }
        }
        printf("     %d mappings, %d addresses asked, %d answers differ\n", n, asked, bad);
        CHECK(n > 10 && bad == 0, "VirtualQuery of untracked memory matches /proc/self/maps");
        LARGE_INTEGER t0, t1, fq; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&t0);
        for (int k = 0; k < 2000; k++) VirtualQuery(&local, &mi, sizeof mi);
        QueryPerformanceCounter(&t1);
        printf("     VirtualQuery of the stack: %.2f us a call\n", (double)(t1.QuadPart - t0.QuadPart) * 1e6 / (double)fq.QuadPart / 2000);
    }
#endif
    void *pages[64]; ULONG_PTR cnt = 64; ULONG gr = 0;
    {   /* write watch: Linux userfaultfd, macOS mprotect + the fault handler (a written host page reports all its 4 KB pieces) */
        const uintptr_t G = (uintptr_t)getpagesize(), per = G / 4096;
        uint8_t *ww = VirtualAlloc(NULL, 0x400000, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_NOACCESS);
        VirtualAlloc(ww, 0x100000, MEM_COMMIT, PAGE_READWRITE);
        CHECK(GetWriteWatch(WRITE_WATCH_FLAG_RESET, ww, 0x100000, pages, &cnt, &gr) == 0 && cnt == 0 && gr == 4096, "write watch: nothing written");
        ww[0x3000] = 1; ww[0x3001] = 2; ww[0x9000] = 3;
        cnt = 64;
        CHECK(GetWriteWatch(WRITE_WATCH_FLAG_RESET, ww, 0x100000, pages, &cnt, &gr) == 0 && cnt == 2 * per && has_page(pages, cnt, ww + 0x3000) &&
              has_page(pages, cnt, ww + 0x9000) && pages[0] == ww + (0x3000 & ~(G - 1)), "write watch: 2 host pages written");
        cnt = 64;
        CHECK(GetWriteWatch(0, ww, 0x100000, pages, &cnt, &gr) == 0 && cnt == 0, "write watch: reset");
        ww[0x9000] = 4; cnt = 64;
        CHECK(GetWriteWatch(0, ww + 0x8000, 0x2000, pages, &cnt, &gr) == 0 && cnt == (per > 1 ? 2 : 1) && GetWriteWatch(0, ww, 0x1000, pages, &cnt, &gr) == 0 && cnt == 0, "write watch: sub-range");
        ResetWriteWatch(ww, 0x100000);
        ww[0x20000] = 5; cnt = 64;                       /* reset one piece: the other pieces of its host page stay written */
        CHECK(GetWriteWatch(WRITE_WATCH_FLAG_RESET, ww + 0x20000, 0x1000, pages, &cnt, &gr) == 0 && cnt == 1 && pages[0] == ww + 0x20000, "write watch: one piece asked and reset");
        cnt = 64;
        CHECK(GetWriteWatch(0, ww + 0x21000, 0x3000, pages, &cnt, &gr) == 0 && cnt == per - 1, "write watch: the rest of its host page still written");
        ww[0x20000] = 6; cnt = 64;                       /* the reset protected the page again: this write is seen */
        CHECK(GetWriteWatch(0, ww + 0x20000, 0x1000, pages, &cnt, &gr) == 0 && cnt == 1, "write watch: a write after a partial reset is seen");
        ResetWriteWatch(ww, 0x100000);
        {   /* the kernel's own writes: ReadFile and a large fread into clean watched memory */
            static uint8_t data[200000];
            for (size_t k = 0; k < sizeof data; k++) data[k] = (uint8_t)(k * 7);
            FILE *wf = fopen("plat_ww.bin", "wb"); fwrite(data, 1, sizeof data, wf); fclose(wf);
            HANDLE rh = CreateFileA("plat_ww.bin", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            DWORD got = 0;
            CHECK(rh != INVALID_HANDLE_VALUE && ReadFile(rh, ww + 0x40000, 100000, &got, NULL) && got == 100000 && !memcmp(ww + 0x40000, data, 100000), "ReadFile into write-watched memory");
            CloseHandle(rh);
            FILE *rf = fopen("plat_ww.bin", "rb");
            CHECK(rf && fread(ww + 0x80000, 1, sizeof data, rf) == sizeof data && !memcmp(ww + 0x80000, data, sizeof data), "fread (200 KB) into write-watched memory");
            if (rf) fclose(rf);
            DeleteFileA("plat_ww.bin");
            static void *many[256]; ULONG_PTR c2 = 256;
            CHECK(GetWriteWatch(WRITE_WATCH_FLAG_RESET, ww + 0x40000, 100000, many, &c2, &gr) == 0 && c2 >= 25, "write watch: the read's pages reported");
        }
        ResetWriteWatch(ww, 0x100000);
        {   /* four threads write every 4 KB piece of 1 MB at once (faults on many threads) */
            ww_mem = ww; HANDLE th[4];
            for (int k = 0; k < 4; k++) th[k] = CreateThread(NULL, 0, ww_writer, (LPVOID)(uintptr_t)k, 0, NULL);
            for (int k = 0; k < 4; k++) { WaitForSingleObject(th[k], INFINITE); CloseHandle(th[k]); }
            static void *all[512]; ULONG_PTR c3 = 512;
            CHECK(GetWriteWatch(WRITE_WATCH_FLAG_RESET, ww, 0x100000, all, &c3, &gr) == 0 && c3 == 256, "write watch: 256 pieces written by 4 threads");
            int same = 1;
            for (int k = 0; k < 256; k++) same &= ww[(uintptr_t)k * 0x1000 + 17] == (uint8_t)(k % 64);
            CHECK(same, "write watch: every write landed");
        }
        CHECK(VirtualFree(ww + 0x1000, 0x1000, MEM_DECOMMIT) && VirtualAlloc(ww + 0x1000, 0x1000, MEM_COMMIT, PAGE_READWRITE) && ww[0x1000] == 0, "write watch: decommit one piece");
        cnt = 64;
        CHECK(GetWriteWatch(0, ww, 0x100000, pages, &cnt, &gr) == 0 && cnt == 0, "write watch: decommit is not a write");
        ww[0x1000] = 1; cnt = 64;
        CHECK(GetWriteWatch(0, ww, 0x100000, pages, &cnt, &gr) == 0 && cnt == per, "write watch: a write after the decommit is seen");
        VirtualFree(ww, 0, MEM_RELEASE);
    }
    cnt = 64;
    CHECK(GetWriteWatch(0, any, 4096, pages, &cnt, &gr) != 0, "write watch: unwatched memory refused");
    /* fibers */
    main_fiber = ConvertThreadToFiber(NULL);
    fib = CreateFiberEx(0, 1 << 20, 0, fiber_fn, (LPVOID)5);
    SwitchToFiber(fib); SwitchToFiber(fib);
    CHECK(fiber_steps == 10 && GetCurrentFiber() == main_fiber, "fibers");
    /* files, with Windows separators */
    CreateDirectoryA("plat_test_dir", NULL);
    CHECK(!CreateDirectoryA("plat_test_dir", NULL) && GetLastError() == ERROR_ALREADY_EXISTS, "CreateDirectory twice: ERROR_ALREADY_EXISTS");
    HANDLE fh = CreateFileA("plat_test_dir\\A.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD put = 0;
    CHECK(fh != INVALID_HANDLE_VALUE && WriteFile(fh, "hello", 5, &put, NULL) && put == 5, "CreateFile + WriteFile");
    CloseHandle(fh);
    FILE *fp = fopen("plat_test_dir\\A.txt", "rb"); char buf[8] = {0};
    CHECK(fp && fread(buf, 1, 5, fp) == 5 && !strcmp(buf, "hello"), "fopen with backslashes");
    if (fp) fclose(fp);
    WIN32_FIND_DATAA fd; int found = 0;
    HANDLE fnd = FindFirstFileA("plat_test_dir\\*.txt", &fd);
    if (fnd != INVALID_HANDLE_VALUE) { do found += !strcmp(fd.cFileName, "A.txt") && fd.nFileSizeLow == 5; while (FindNextFileA(fnd, &fd)); FindClose(fnd); }
    CHECK(found == 1, "FindFirstFile pattern");
    CHECK(MoveFileExA("plat_test_dir\\A.txt", "plat_test_dir\\B.txt", MOVEFILE_REPLACE_EXISTING) && GetFileAttributesA("plat_test_dir\\B.txt") != INVALID_FILE_ATTRIBUTES, "MoveFileEx");
    CHECK(DeleteFileA("plat_test_dir\\B.txt") && RemoveDirectoryA("plat_test_dir"), "delete");
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    CHECK(exe[0] == '\\' && !strchr(exe, '/') && strlen(exe) > 1, "GetModuleFileNameA (Windows separators)");
    /* modules */
    HMODULE vk = LoadLibraryA("vulkan-1.dll");
    CHECK(vk && GetProcAddress(vk, "vkGetInstanceProcAddr"), "LoadLibrary(vulkan-1.dll) -> the Vulkan loader");
    HMODULE bc = LoadLibraryA("bcrypt.dll");
    typedef LONG (*Hash)(void *, uint8_t *, ULONG, const uint8_t *, ULONG, uint8_t *, ULONG);
    uint8_t dg[32];
    Hash h = (Hash)GetProcAddress(bc, "BCryptHash");
    CHECK(h && h(NULL, NULL, 0, (const uint8_t *)"abc", 3, dg, 32) == 0 && dg[0] == 0xba && dg[1] == 0x78 && dg[31] == 0xad, "SHA-256(abc) through bcrypt.dll");
    char u8[16]; WCHAR w[16];
    CHECK(MultiByteToWideChar(CP_UTF8, 0, "a\xc3\xa9\xe3\x82\xbc", -1, w, 16) == 4 && w[1] == 0xe9 && w[2] == 0x30bc &&
          WideCharToMultiByte(CP_UTF8, 0, w, -1, u8, 16, NULL, NULL) == 7 && !strcmp(u8, "a\xc3\xa9\xe3\x82\xbc"), "UTF-8 <-> wide");
    printf("%d failure(s)\n", fails);
    return fails != 0;
}
