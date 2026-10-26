/* The Windows API of runtime/linux/include/windows.h, on Linux.
 *
 * Threads are POSIX threads; condition variables are futex sequence numbers; handles are small
 * objects (thread, event, file, directory search); fibers switch stacks with a few lines of
 * assembly; memory is reserved with mmap and committed with mprotect.
 *
 * The write watch (GetWriteWatch / ResetWriteWatch) is what the renderer uses to learn which
 * guest pages the CPU wrote. Linux 6.7 and later do exactly that with userfaultfd's
 * asynchronous write protection and the PAGEMAP_SCAN ioctl: the kernel notes the first write
 * to each protected page itself, with no signal, and a scan reports (and re-protects) the
 * written ones. Older kernels get a fallback that protects pages with mprotect and takes the
 * first write as a SIGSEGV (rt_ww_fault, from the fault handler); system calls cannot write
 * such pages, so I/O into guest memory goes through rt_ww_prepare_write first.
 *
 * macOS builds this file too. There the memory functions and the write watch (16 KiB pages,
 * protection only) are runtime/macos/memory.c's and the fibers runtime/macos/fiber.c's; what
 * stays here differs in small ways (__APPLE__): condition variables wait on an address
 * (runtime/macos/platform.c), thread ids are Mach thread ports, priorities are QoS classes,
 * and the clock is mach_absolute_time. */
#define _GNU_SOURCE 1
#include <dirent.h>
#include <execinfo.h>
#include <fcntl.h>
#include <fnmatch.h>
#ifndef __APPLE__
#include <linux/futex.h>
#include <linux/userfaultfd.h>
#endif
#include <signal.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sched.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach_time.h>
#include <mach-o/ldsyms.h>
#include <pthread/qos.h>
#include <sys/sysctl.h>
#endif

#include <windows.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

/* ---- errors ----------------------------------------------------------------------------- */
static __thread DWORD last_error;
DWORD GetLastError(void) { return last_error; }
void SetLastError(DWORD error) { last_error = error; }

DWORD rt_win32_error(int err) {
    switch (err) {
    case 0: return ERROR_SUCCESS;
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EACCES: case EPERM: case EROFS: return ERROR_ACCESS_DENIED;
    case EBADF: return ERROR_INVALID_HANDLE;
    case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
    case EEXIST: return ERROR_ALREADY_EXISTS;
    case ENOTEMPTY: return ERROR_DIR_NOT_EMPTY;
    case ENOSPC: return ERROR_DISK_FULL;
    default: return ERROR_INVALID_PARAMETER;
    }
}
static BOOL fail_errno(void) { last_error = rt_win32_error(errno); return FALSE; }

/* ---- locks and condition variables ------------------------------------------------------- */
#ifndef __APPLE__
_Static_assert(offsetof(CRITICAL_SECTION, OwningThread) == offsetof(pthread_mutex_t, __data.__owner) && sizeof(CRITICAL_SECTION) == sizeof(pthread_mutex_t),
               "CRITICAL_SECTION's OwningThread is glibc's mutex owner");
#endif

void InitializeCriticalSection(CRITICAL_SECTION *cs) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&cs->m, &a);
    pthread_mutexattr_destroy(&a);
#ifdef __APPLE__
    cs->OwningThread = 0;
    cs->depth = 0;
#endif
}

#ifdef __APPLE__
/* (runtime/macos/platform.c: os_sync_wait_on_address, or __ulock_wait2 before macOS 14.4) */
int rt_futex_wait(volatile int *addr, int value, DWORD ms);
void rt_futex_wake(volatile int *addr, int all);

void WakeConditionVariable(CONDITION_VARIABLE *cv) {
    __atomic_add_fetch(&cv->seq, 1, __ATOMIC_SEQ_CST);
    rt_futex_wake(&cv->seq, 0);
}

void WakeAllConditionVariable(CONDITION_VARIABLE *cv) {
    __atomic_add_fetch(&cv->seq, 1, __ATOMIC_SEQ_CST);
    rt_futex_wake(&cv->seq, 1);
}

/* Wait for the sequence number to move on from `seq`: 1 woken (or spuriously), 0 timed out. */
static int cv_wait(CONDITION_VARIABLE *cv, int seq, DWORD ms) { return rt_futex_wait(&cv->seq, seq, ms); }

/* (through Leave and Enter, which keep OwningThread and the recursion depth right: another
 * thread holds it while this one waits) */
BOOL SleepConditionVariableCS(CONDITION_VARIABLE *cv, CRITICAL_SECTION *cs, DWORD ms) {
    int seq = __atomic_load_n(&cv->seq, __ATOMIC_SEQ_CST);
    LeaveCriticalSection(cs);
    int woken = cv_wait(cv, seq, ms);
    EnterCriticalSection(cs);
    if (!woken) { last_error = WAIT_TIMEOUT; return FALSE; }
    return TRUE;
}
#else
static long futex(volatile int *addr, int op, int val, const struct timespec *timeout) {
    return syscall(SYS_futex, addr, op, val, timeout, NULL, 0);
}

void WakeConditionVariable(CONDITION_VARIABLE *cv) {
    __atomic_add_fetch(&cv->seq, 1, __ATOMIC_SEQ_CST);
    futex(&cv->seq, FUTEX_WAKE_PRIVATE, 1, NULL);
}

void WakeAllConditionVariable(CONDITION_VARIABLE *cv) {
    __atomic_add_fetch(&cv->seq, 1, __ATOMIC_SEQ_CST);
    futex(&cv->seq, FUTEX_WAKE_PRIVATE, 0x7FFFFFFF, NULL);
}

/* Wait for the sequence number to move on from `seq`: 1 woken (or spuriously), 0 timed out. */
static int cv_wait(CONDITION_VARIABLE *cv, int seq, DWORD ms) {
    struct timespec ts, *t = NULL;
    if (ms != INFINITE) { ts.tv_sec = ms / 1000; ts.tv_nsec = (long)(ms % 1000) * 1000000L; t = &ts; }
    if (futex(&cv->seq, FUTEX_WAIT_PRIVATE, seq, t) == -1 && errno == ETIMEDOUT) return 0;
    return 1;
}

BOOL SleepConditionVariableCS(CONDITION_VARIABLE *cv, CRITICAL_SECTION *cs, DWORD ms) {
    int seq = __atomic_load_n(&cv->seq, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&cs->m);
    int woken = cv_wait(cv, seq, ms);
    pthread_mutex_lock(&cs->m);
    if (!woken) { last_error = WAIT_TIMEOUT; return FALSE; }
    return TRUE;
}
#endif

BOOL SleepConditionVariableSRW(CONDITION_VARIABLE *cv, SRWLOCK *l, DWORD ms, ULONG flags) {
    int seq = __atomic_load_n(&cv->seq, __ATOMIC_SEQ_CST);
    pthread_rwlock_unlock(&l->l);
    int woken = cv_wait(cv, seq, ms);
    if (flags & CONDITION_VARIABLE_LOCKMODE_SHARED) pthread_rwlock_rdlock(&l->l); else pthread_rwlock_wrlock(&l->l);
    if (!woken) { last_error = WAIT_TIMEOUT; return FALSE; }
    return TRUE;
}

BOOL InitOnceExecuteOnce(PINIT_ONCE once, PINIT_ONCE_FN fn, PVOID param, LPVOID *context) {
    if (__atomic_load_n(&once->done, __ATOMIC_ACQUIRE)) return TRUE;
    pthread_mutex_lock(&once->m);
    BOOL ok = TRUE;
    if (!once->done) {
        ok = fn(once, param, context);
        if (ok) __atomic_store_n(&once->done, 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&once->m);
    return ok;
}

BOOL InitOnceBeginInitialize(LPINIT_ONCE once, DWORD flags, PBOOL pending, LPVOID *context) {
    (void)flags; (void)context;
    if (__atomic_load_n(&once->done, __ATOMIC_ACQUIRE)) { *pending = FALSE; return TRUE; }
    pthread_mutex_lock(&once->m);
    if (once->done) { pthread_mutex_unlock(&once->m); *pending = FALSE; return TRUE; }
    *pending = TRUE;   /* (the lock stays held until InitOnceComplete) */
    return TRUE;
}

BOOL InitOnceComplete(LPINIT_ONCE once, DWORD flags, LPVOID context) {
    (void)flags; (void)context;
    __atomic_store_n(&once->done, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&once->m);
    return TRUE;
}

/* ---- handles ------------------------------------------------------------------------------ */
enum { H_THREAD = 1, H_EVENT, H_FILE, H_FIND, H_TIMER };
#define H_MAGIC 0x52544831u

typedef struct Obj {
    u32 magic;
    int type;
    volatile int refs;
    pthread_mutex_t m;
    pthread_cond_t cv;
    int signaled, manual;          /* events; threads: signaled once finished */
    pthread_t thread;
    volatile RtTid tid;
    LPTHREAD_START_ROUTINE fn;
    void *param;
    int fd;                        /* files */
    DIR *dir;                      /* searches */
    u64 due_ns;                    /* timers: when (CLOCK_MONOTONIC), while armed */
    int armed;
    char dir_path[1024], pattern[256];
} Obj;

#define PSEUDO_PROCESS ((HANDLE)(intptr_t)-1)
#define PSEUDO_THREAD ((HANDLE)(intptr_t)-2)

static Obj *new_obj(int type) {
    Obj *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->magic = H_MAGIC;
    o->type = type;
    o->refs = 1;
    o->fd = -1;
    pthread_mutex_init(&o->m, NULL);
    pthread_condattr_t a;
    pthread_condattr_init(&a);
#ifndef __APPLE__   /* (macOS: no choice of clock - its waits are relative, see WaitForSingleObject) */
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
#endif
    pthread_cond_init(&o->cv, &a);
    pthread_condattr_destroy(&a);
    return o;
}

static Obj *obj_of(HANDLE h, int type) {
    Obj *o = (Obj *)h;
    if (!h || h == INVALID_HANDLE_VALUE || h == PSEUDO_THREAD || o->magic != H_MAGIC || (type && o->type != type)) return NULL;
    return o;
}

static void obj_release(Obj *o) {
    if (__atomic_sub_fetch(&o->refs, 1, __ATOMIC_ACQ_REL)) return;
    if (o->fd >= 0) close(o->fd);
    if (o->dir) closedir(o->dir);
    pthread_mutex_destroy(&o->m);
    pthread_cond_destroy(&o->cv);
    o->magic = 0;
    free(o);
}

static void obj_signal(Obj *o) {
    pthread_mutex_lock(&o->m);
    o->signaled = 1;
    pthread_cond_broadcast(&o->cv);
    pthread_mutex_unlock(&o->m);
}

BOOL CloseHandle(HANDLE h) {
    Obj *o = obj_of(h, 0);
    if (!o) { last_error = ERROR_INVALID_HANDLE; return FALSE; }
    obj_release(o);
    return TRUE;
}

#ifdef __APPLE__
/* (runtime/macos/platform.c: mach_absolute_time in nanoseconds, and a timer the system does
 * not put off) */
u64 rt_uptime_ns(void);
u64 rt_uptime_hz(void);
void rt_sleep_until_ns(u64 t);
static u64 monotonic_ns(void) { return rt_uptime_ns(); }
static void sleep_until_ns(u64 t) { rt_sleep_until_ns(t); }
#else
static u64 monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

static void sleep_until_ns(u64 t) {
    struct timespec ts = {(time_t)(t / 1000000000ull), (long)(t % 1000000000ull)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}
#endif

/* A timer: sleeps until it is due (then disarms it) or ms have passed. */
static DWORD wait_timer(Obj *o, DWORD ms) {
    pthread_mutex_lock(&o->m);
    u64 due = o->due_ns;
    int armed = o->armed;
    pthread_mutex_unlock(&o->m);
    u64 now = monotonic_ns(), limit = ms == INFINITE ? ~0ull : now + (u64)ms * 1000000ull;
    if (!armed) {
        if (ms == INFINITE) { last_error = ERROR_INVALID_PARAMETER; return WAIT_FAILED; }   /* (it would never be due) */
        sleep_until_ns(limit);
        return WAIT_TIMEOUT;
    }
    sleep_until_ns(due < limit ? due : limit);
    if (due > limit) return WAIT_TIMEOUT;
    pthread_mutex_lock(&o->m);
    if (o->due_ns == due) o->armed = 0;
    pthread_mutex_unlock(&o->m);
    return WAIT_OBJECT_0;
}

DWORD WaitForSingleObject(HANDLE h, DWORD ms) {
    Obj *o = obj_of(h, 0);
    if (o && o->type == H_TIMER) return wait_timer(o, ms);
    if (!o || (o->type != H_THREAD && o->type != H_EVENT)) { last_error = ERROR_INVALID_HANDLE; return WAIT_FAILED; }
#ifdef __APPLE__   /* (the time left, each time round: a relative wait) */
    u64 until = ms == INFINITE ? 0 : monotonic_ns() + (u64)ms * 1000000ull;
    pthread_mutex_lock(&o->m);
    while (!o->signaled) {
        if (ms == 0) break;
        if (ms == INFINITE) { pthread_cond_wait(&o->cv, &o->m); continue; }
        u64 now = monotonic_ns(), ns = until - now;
        if (now >= until) break;
        if (ns > 1000000) ns /= 2;   /* (a timed wait may run late by a share of its length: half the rest at a time) */
        struct timespec left = {(time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull)};
        pthread_cond_timedwait_relative_np(&o->cv, &o->m, &left);
    }
#else
    struct timespec until;
    if (ms != INFINITE) {
        clock_gettime(CLOCK_MONOTONIC, &until);
        until.tv_sec += ms / 1000;
        until.tv_nsec += (long)(ms % 1000) * 1000000L;
        if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
    }
    pthread_mutex_lock(&o->m);
    while (!o->signaled) {
        if (ms == 0) break;
        if (ms == INFINITE) pthread_cond_wait(&o->cv, &o->m);
        else if (pthread_cond_timedwait(&o->cv, &o->m, &until) == ETIMEDOUT) break;
    }
#endif
    int got = o->signaled;
    if (got && o->type == H_EVENT && !o->manual) o->signaled = 0;
    pthread_mutex_unlock(&o->m);
    return got ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
}

HANDLE CreateEventA(void *security, BOOL manual_reset, BOOL initial, const char *name) {
    (void)security; (void)name;
    Obj *o = new_obj(H_EVENT);
    o->manual = manual_reset;
    o->signaled = initial;
    return o;
}
HANDLE CreateEventW(void *security, BOOL manual_reset, BOOL initial, const WCHAR *name) {
    (void)name;
    return CreateEventA(security, manual_reset, initial, NULL);
}
BOOL SetEvent(HANDLE h) {
    Obj *o = obj_of(h, H_EVENT);
    if (!o) return FALSE;
    obj_signal(o);
    return TRUE;
}
HANDLE CreateWaitableTimerExW(void *security, LPCWSTR name, DWORD flags, DWORD access) {
    (void)security; (void)name; (void)flags; (void)access;
    return new_obj(H_TIMER);
}

/* due < 0: relative, in 100 ns units; else an absolute FILETIME (UTC). No period or callback. */
BOOL SetWaitableTimer(HANDLE timer, const LARGE_INTEGER *due, LONG period, PTIMERAPCROUTINE fn, LPVOID arg, BOOL resume) {
    (void)resume; (void)arg;
    Obj *o = obj_of(timer, H_TIMER);
    if (!o || period || fn) { last_error = ERROR_INVALID_PARAMETER; return FALSE; }
    u64 now = monotonic_ns(), at;
    if (due->QuadPart < 0) at = now + (u64)(-due->QuadPart) * 100;
    else {
        struct timespec rt;
        clock_gettime(CLOCK_REALTIME, &rt);
        long long unix_ns = (due->QuadPart - 116444736000000000LL) * 100, real = (long long)rt.tv_sec * 1000000000LL + rt.tv_nsec;
        at = unix_ns > real ? now + (u64)(unix_ns - real) : now;
    }
    pthread_mutex_lock(&o->m);
    o->due_ns = at;
    o->armed = 1;
    pthread_mutex_unlock(&o->m);
    return TRUE;
}

BOOL ResetEvent(HANDLE h) {
    Obj *o = obj_of(h, H_EVENT);
    if (!o) return FALSE;
    pthread_mutex_lock(&o->m);
    o->signaled = 0;
    pthread_mutex_unlock(&o->m);
    return TRUE;
}

/* ---- threads ------------------------------------------------------------------------------ */
/* Full thread descriptions by thread id: the profiler's and backtraces' names. */
#define DESCS 1024
static struct { volatile RtTid tid; char text[112]; } descs[DESCS];
static pthread_mutex_t descs_lock = PTHREAD_MUTEX_INITIALIZER;

#ifdef __APPLE__
static RtTid self_tid(void) { return pthread_mach_thread_np(pthread_self()); }   /* (no system call, no thread-local) */
#else
static RtTid self_tid(void) { return gettid(); }
#endif

const char *rt_thread_description(RtTid tid) {
    for (u32 i = 0; i < DESCS; i++) if (descs[i].tid == tid) return descs[i].text;
    return "";
}

static void set_description(RtTid tid, const char *text) {
    pthread_mutex_lock(&descs_lock);
    int slot = -1;
    for (u32 i = 0; i < DESCS && slot < 0; i++) if (descs[i].tid == tid) slot = (int)i;
    for (u32 i = 0; i < DESCS && slot < 0; i++) if (!descs[i].tid) slot = (int)i;
    if (slot >= 0) {
        snprintf(descs[slot].text, sizeof descs[slot].text, "%s", text);
        descs[slot].tid = tid;
    }
    pthread_mutex_unlock(&descs_lock);
}

static void forget_description(RtTid tid) {
    pthread_mutex_lock(&descs_lock);
    for (u32 i = 0; i < DESCS; i++) if (descs[i].tid == tid) descs[i].tid = 0;
    pthread_mutex_unlock(&descs_lock);
}

DWORD GetCurrentThreadId(void) { return (DWORD)self_tid(); }
DWORD GetCurrentProcessId(void) { return (DWORD)getpid(); }
HANDLE GetCurrentThread(void) { return PSEUDO_THREAD; }
HANDLE GetCurrentProcess(void) { return PSEUDO_PROCESS; }

/* A signal stack for every thread we start, so that a fault report (stack overflows included)
 * has room to run. */
void rt_thread_signal_stack(void) {
    stack_t ss = {0};
    ss.ss_size = 256 << 10;
    ss.ss_sp = mmap(NULL, ss.ss_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ss.ss_sp != MAP_FAILED) sigaltstack(&ss, NULL);
}

static void *thread_start(void *arg) {
    Obj *o = arg;
    o->tid = self_tid();
    rt_thread_signal_stack();
    DWORD code = o->fn(o->param);
    (void)code;
    forget_description(o->tid);
    obj_signal(o);
    obj_release(o);   /* (the thread's own reference) */
    return NULL;
}

HANDLE CreateThread(void *security, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID param, DWORD flags, DWORD *id) {
    (void)security; (void)flags;
    Obj *o = new_obj(H_THREAD);
    o->fn = fn;
    o->param = param;
    o->refs = 2;   /* the caller's handle and the thread itself */
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (!stack) stack = 8u << 20;
    stack = (stack + 0xFFFF) & ~(SIZE_T)0xFFFF;
    pthread_attr_setstacksize(&a, stack);
#ifdef __APPLE__
    /* The game's threads (the guest's, the renderer, audio, input) keep pace with frames: on
     * Apple silicon a thread's class decides between the performance and efficiency cores.
     * Those that may wait lower themselves (SetThreadPriority). */
    pthread_attr_set_qos_class_np(&a, QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    int err = pthread_create(&o->thread, &a, thread_start, o);
    pthread_attr_destroy(&a);
    if (err) { o->refs = 1; obj_release(o); last_error = rt_win32_error(err); return NULL; }
    if (id) { while (!o->tid) sched_yield(); *id = (DWORD)o->tid; }
    return o;
}

void ExitThread(DWORD code) {
    (void)code;
    pthread_exit(NULL);
}

DWORD GetThreadId(HANDLE thread) {
    if (thread == PSEUDO_THREAD) return GetCurrentThreadId();
    Obj *o = obj_of(thread, H_THREAD);
    return o ? (DWORD)o->tid : 0;
}

RtTid rt_thread_tid(HANDLE thread) { return (RtTid)GetThreadId(thread); }
pthread_t rt_thread_pthread(HANDLE thread) {
    if (thread == PSEUDO_THREAD) return pthread_self();
    Obj *o = obj_of(thread, H_THREAD);
    return o ? o->thread : 0;
}

BOOL DuplicateHandle(HANDLE src_process, HANDLE src, HANDLE dst_process, HANDLE *dst, DWORD access, BOOL inherit, DWORD options) {
    (void)src_process; (void)dst_process; (void)access; (void)inherit; (void)options;
    if (src == PSEUDO_THREAD) {   /* a real handle for the calling thread */
        Obj *o = new_obj(H_THREAD);
        o->thread = pthread_self();
        o->tid = self_tid();
        *dst = o;
        return TRUE;
    }
    Obj *o = obj_of(src, 0);
    if (!o) { last_error = ERROR_INVALID_HANDLE; return FALSE; }
    __atomic_add_fetch(&o->refs, 1, __ATOMIC_ACQ_REL);
    *dst = o;
    return TRUE;
}

#ifdef __APPLE__
/* (macOS names, and sets the class of, only the calling thread - every caller's own) */
static int is_self(HANDLE thread) {
    pthread_t t = rt_thread_pthread(thread);
    return t && pthread_equal(t, pthread_self());
}

HRESULT SetThreadDescription(HANDLE thread, PCWSTR description) {
    char text[112];
    size_t n = 0;
    for (; description[n] && n + 1 < sizeof text; n++) text[n] = description[n] < 128 ? (char)description[n] : '?';
    text[n] = 0;
    RtTid tid = rt_thread_tid(thread);
    if (!tid) return E_INVALIDARG;
    set_description(tid, text);
    if (is_self(thread)) {
        char name[64];
        snprintf(name, sizeof name, "%s", text);
        pthread_setname_np(name);
    }
    return S_OK;
}

BOOL SetThreadPriority(HANDLE thread, int priority) {
    if (!is_self(thread)) { last_error = ERROR_INVALID_PARAMETER; return FALSE; }
    qos_class_t qos = priority <= THREAD_PRIORITY_LOWEST ? QOS_CLASS_UTILITY
                      : priority == THREAD_PRIORITY_BELOW_NORMAL ? QOS_CLASS_USER_INITIATED : QOS_CLASS_USER_INTERACTIVE;
    return pthread_set_qos_class_self_np(qos, 0) == 0;   /* (never setpriority: its "process" would be a thread's port) */
}
#else
HRESULT SetThreadDescription(HANDLE thread, PCWSTR description) {
    char text[112];
    size_t n = 0;
    for (; description[n] && n + 1 < sizeof text; n++) text[n] = description[n] < 128 ? (char)description[n] : '?';
    text[n] = 0;
    RtTid tid = thread == PSEUDO_THREAD ? self_tid() : rt_thread_tid(thread);
    if (!tid) return E_INVALIDARG;
    set_description(tid, text);
    char shortname[16];
    snprintf(shortname, sizeof shortname, "%s", text);
    pthread_setname_np(thread == PSEUDO_THREAD ? pthread_self() : rt_thread_pthread(thread), shortname);
    return S_OK;
}

BOOL SetThreadPriority(HANDLE thread, int priority) {
    RtTid tid = thread == PSEUDO_THREAD ? self_tid() : rt_thread_tid(thread);
    if (!tid) return FALSE;
    int nice = priority <= THREAD_PRIORITY_LOWEST ? 5 : priority == THREAD_PRIORITY_BELOW_NORMAL ? 2 : 0;
    setpriority(PRIO_PROCESS, (id_t)tid, nice);   /* (raising priority needs privileges: left as it is) */
    return TRUE;
}
#endif

BOOL SetThreadStackGuarantee(PULONG size) { (void)size; return TRUE; }
BOOL SwitchToThread(void) { sched_yield(); return TRUE; }

void Sleep(DWORD ms) {
    if (!ms) { sched_yield(); return; }
#ifdef __APPLE__   /* (on a critical timer, as waitable timers: runtime/macos/platform.c) */
    sleep_until_ns(monotonic_ns() + (u64)ms * 1000000ull);
#else
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L};
    while (nanosleep(&t, &t) == -1 && errno == EINTR) {}
#endif
}

/* ---- fibers ------------------------------------------------------------------------------- */
#ifndef __APPLE__   /* (macOS: runtime/macos/fiber.c and fiber_arm64.S) */
typedef struct Fiber {
    void *sp;              /* the stack pointer it stopped at (while not running) */
    void *param;           /* GetFiberData */
    LPFIBER_START_ROUTINE fn;
    void *stack;
    size_t stack_size;
} Fiber;
static __thread Fiber *current_fiber;

void rt_fiber_switch(void **save_sp, void *load_sp);
void rt_fiber_trampoline(void);
/* Save the callee-saved registers and the floating-point control words on this stack, switch
 * stacks, restore the other context's and return into it. A new fiber's stack is prepared to
 * "return" into the trampoline, which calls rt_fiber_entry(fiber in r12) and marks the end of
 * the stack for unwinders. */
__asm__(
    ".text\n"
    ".globl rt_fiber_switch\n"
    ".type rt_fiber_switch,@function\n"
    "rt_fiber_switch:\n"
    "    pushq %rbp\n"
    "    pushq %rbx\n"
    "    pushq %r12\n"
    "    pushq %r13\n"
    "    pushq %r14\n"
    "    pushq %r15\n"
    "    subq $8, %rsp\n"
    "    stmxcsr (%rsp)\n"
    "    fnstcw 4(%rsp)\n"
    "    movq %rsp, (%rdi)\n"
    "    movq %rsi, %rsp\n"
    "    ldmxcsr (%rsp)\n"
    "    fldcw 4(%rsp)\n"
    "    addq $8, %rsp\n"
    "    popq %r15\n"
    "    popq %r14\n"
    "    popq %r13\n"
    "    popq %r12\n"
    "    popq %rbx\n"
    "    popq %rbp\n"
    "    ret\n"
    ".size rt_fiber_switch, .-rt_fiber_switch\n"
    ".globl rt_fiber_trampoline\n"
    ".type rt_fiber_trampoline,@function\n"
    "rt_fiber_trampoline:\n"
    "    .cfi_startproc\n"
    "    .cfi_undefined rip\n"
    "    movq %r12, %rdi\n"
    "    call rt_fiber_entry\n"
    "    ud2\n"
    "    .cfi_endproc\n"
    ".size rt_fiber_trampoline, .-rt_fiber_trampoline\n");

__attribute__((used)) void rt_fiber_entry(Fiber *f) {
    f->fn(f->param);
    fprintf(stderr, "a fiber's start routine returned\n");
    abort();
}

LPVOID ConvertThreadToFiberEx(LPVOID param, DWORD flags) {
    (void)flags;
    if (current_fiber) return current_fiber;
    Fiber *f = calloc(1, sizeof *f);
    f->param = param;
    current_fiber = f;
    return f;
}
LPVOID ConvertThreadToFiber(LPVOID param) { return ConvertThreadToFiberEx(param, 0); }

LPVOID CreateFiberEx(SIZE_T commit, SIZE_T reserve, DWORD flags, LPFIBER_START_ROUTINE fn, LPVOID param) {
    (void)flags;
    size_t size = reserve ? reserve : commit ? commit : (1u << 20);
    size = (size + 0xFFFF) & ~(size_t)0xFFFF;
    u8 *stack = mmap(NULL, size + 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK, -1, 0);
    if (stack == MAP_FAILED) { last_error = ERROR_NOT_ENOUGH_MEMORY; return NULL; }
    mprotect(stack, 0x10000, PROT_NONE);   /* a guard below */
    Fiber *f = calloc(1, sizeof *f);
    f->fn = fn;
    f->param = param;
    f->stack = stack;
    f->stack_size = size + 0x10000;
    u64 *sp = (u64 *)(((uintptr_t)(stack + size + 0x10000) - 128) & ~(uintptr_t)15);
    sp -= 8;   /* control words, r15, r14, r13, r12, rbx, rbp, return address: 16-aligned after the ret */
    u32 mxcsr = 0x1F80;
    u16 fpcw = 0x037F;
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    __asm__ volatile("fnstcw %0" : "=m"(fpcw));
    sp[0] = mxcsr | (u64)fpcw << 32;
    sp[1] = 0; sp[2] = 0; sp[3] = 0;
    sp[4] = (u64)(uintptr_t)f;   /* r12 */
    sp[5] = 0; sp[6] = 0;
    sp[7] = (u64)(uintptr_t)rt_fiber_trampoline;
    f->sp = sp;
    return f;
}
LPVOID CreateFiber(SIZE_T stack, LPFIBER_START_ROUTINE fn, LPVOID param) { return CreateFiberEx(stack, stack, 0, fn, param); }

void SwitchToFiber(LPVOID fiber) {
    Fiber *me = current_fiber, *next = fiber;
    if (!me || me == next) return;
    current_fiber = next;
    rt_fiber_switch(&me->sp, next->sp);
    /* resumed: possibly on another thread (whose switch set current_fiber) */
}

void DeleteFiber(LPVOID fiber) {
    Fiber *f = fiber;
    if (!f || f == current_fiber) return;
    if (f->stack) munmap(f->stack, f->stack_size);
    free(f);
}
BOOL IsThreadAFiber(void) { return current_fiber != NULL; }
PVOID GetFiberData(void) { return current_fiber ? current_fiber->param : NULL; }
PVOID GetCurrentFiber(void) { return current_fiber; }
#endif

/* ---- time --------------------------------------------------------------------------------- */
#ifdef __APPLE__
/* The processor's counter itself (mach_absolute_time, 24 MHz on Apple silicon, a few ns a read),
 * or its nanoseconds where its rate is not a whole number of ticks a second. */
BOOL QueryPerformanceCounter(LARGE_INTEGER *t) {
    t->QuadPart = (LONGLONG)(rt_uptime_hz() ? mach_absolute_time() : rt_uptime_ns());
    return TRUE;
}
BOOL QueryPerformanceFrequency(LARGE_INTEGER *f) {
    u64 hz = rt_uptime_hz();
    f->QuadPart = (LONGLONG)(hz ? hz : 1000000000ull);
    return TRUE;
}
ULONGLONG GetTickCount64(void) { return rt_uptime_ns() / 1000000; }
#else
BOOL QueryPerformanceCounter(LARGE_INTEGER *t) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t->QuadPart = (LONGLONG)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    return TRUE;
}
BOOL QueryPerformanceFrequency(LARGE_INTEGER *f) { f->QuadPart = 1000000000LL; return TRUE; }
ULONGLONG GetTickCount64(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ULONGLONG)ts.tv_sec * 1000 + (ULONGLONG)ts.tv_nsec / 1000000;
}
#endif
DWORD GetTickCount(void) { return (DWORD)GetTickCount64(); }

static void system_time(SYSTEMTIME *out, int local) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    time_t t = ts.tv_sec;
    if (local) localtime_r(&t, &tm); else gmtime_r(&t, &tm);
    out->wYear = (WORD)(tm.tm_year + 1900);
    out->wMonth = (WORD)(tm.tm_mon + 1);
    out->wDayOfWeek = (WORD)tm.tm_wday;
    out->wDay = (WORD)tm.tm_mday;
    out->wHour = (WORD)tm.tm_hour;
    out->wMinute = (WORD)tm.tm_min;
    out->wSecond = (WORD)tm.tm_sec;
    out->wMilliseconds = (WORD)(ts.tv_nsec / 1000000);
}
void GetLocalTime(SYSTEMTIME *t) { system_time(t, 1); }
void GetSystemTime(SYSTEMTIME *t) { system_time(t, 0); }

/* FILETIME: 100 ns units since 1601 (UTC) */
static FILETIME filetime_of(struct timespec ts) {
    u64 v = ((u64)ts.tv_sec + 11644473600ull) * 10000000ull + (u64)ts.tv_nsec / 100;
    return (FILETIME){(DWORD)v, (DWORD)(v >> 32)};
}
static struct timespec timespec_of(const FILETIME *t) {
    u64 v = (u64)t->dwHighDateTime << 32 | t->dwLowDateTime;
    long long unix100 = (long long)v - 116444736000000000LL;
    return (struct timespec){(time_t)(unix100 / 10000000LL), (long)(unix100 % 10000000LL) * 100};
}
void GetSystemTimeAsFileTime(FILETIME *t) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    *t = filetime_of(ts);
}

/* ---- memory ------------------------------------------------------------------------------- */
#ifndef __APPLE__   /* (macOS: runtime/macos/memory.c, with the write watch) */
#define PAGE 4096ull
static pthread_mutex_t mem_lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct Reservation { u64 base, size; int watch; } Reservation;
static Reservation reservations[256];
static int nreservations;

static Reservation *reservation_of(u64 lo, u64 hi) {
    for (int i = 0; i < nreservations; i++)
        if (reservations[i].base <= lo && hi <= reservations[i].base + reservations[i].size) return &reservations[i];
    return NULL;
}

static int prot_of(DWORD protect) {
    switch (protect & 0xFF) {
    case PAGE_NOACCESS: return PROT_NONE;
    case PAGE_READONLY: return PROT_READ;
    case PAGE_EXECUTE: case PAGE_EXECUTE_READ: return PROT_READ | PROT_EXEC;
    case PAGE_EXECUTE_READWRITE: return PROT_READ | PROT_WRITE | PROT_EXEC;
    default: return PROT_READ | PROT_WRITE;
    }
}

static void ww_register(u64 base, u64 size);
static void ww_commit(u64 lo, u64 hi);
static void ww_decommit(u64 lo, u64 hi);

LPVOID VirtualAlloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect) {
    u64 lo = (u64)(uintptr_t)address & ~(PAGE - 1), hi = ((u64)(uintptr_t)address + size + PAGE - 1) & ~(PAGE - 1);
    if (!size) { last_error = ERROR_INVALID_PARAMETER; return NULL; }
    if ((type & MEM_RESERVE) || !address) {
        u64 bytes = hi - lo;
        int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | (address ? MAP_FIXED_NOREPLACE : 0);
        void *p = mmap(address ? (void *)(uintptr_t)lo : NULL, bytes, (type & MEM_COMMIT) ? prot_of(protect) : PROT_NONE, flags, -1, 0);
        if (p == MAP_FAILED) { last_error = ERROR_INVALID_ADDRESS; return NULL; }
        if (address && (u64)(uintptr_t)p != lo) { munmap(p, bytes); last_error = ERROR_INVALID_ADDRESS; return NULL; }
        lo = (u64)(uintptr_t)p;
        hi = lo + bytes;
        pthread_mutex_lock(&mem_lock);
        if (nreservations < 256) reservations[nreservations++] = (Reservation){lo, hi - lo, (type & MEM_WRITE_WATCH) != 0};
        pthread_mutex_unlock(&mem_lock);
        if (type & MEM_WRITE_WATCH) {
            ww_register(lo, hi - lo);
            if (type & MEM_COMMIT) ww_commit(lo, hi);
        }
        return p;
    }
    if (!(type & MEM_COMMIT)) { last_error = ERROR_INVALID_PARAMETER; return NULL; }
    pthread_mutex_lock(&mem_lock);
    Reservation *r = reservation_of(lo, hi);
    int watch = r && r->watch;
    pthread_mutex_unlock(&mem_lock);
    if (!r) { last_error = ERROR_INVALID_ADDRESS; return NULL; }
    if (watch) ww_commit(lo, hi);
    else if (mprotect((void *)(uintptr_t)lo, hi - lo, prot_of(protect))) { last_error = ERROR_NOT_ENOUGH_MEMORY; return NULL; }
    return (void *)(uintptr_t)lo;
}

BOOL VirtualFree(LPVOID address, SIZE_T size, DWORD type) {
    u64 lo = (u64)(uintptr_t)address & ~(PAGE - 1);
    if (type & MEM_RELEASE) {
        pthread_mutex_lock(&mem_lock);
        for (int i = 0; i < nreservations; i++)
            if (reservations[i].base == lo) {
                munmap((void *)(uintptr_t)lo, reservations[i].size);
                reservations[i] = reservations[--nreservations];
                pthread_mutex_unlock(&mem_lock);
                return TRUE;
            }
        pthread_mutex_unlock(&mem_lock);
        last_error = ERROR_INVALID_ADDRESS;
        return FALSE;
    }
    if (type & MEM_DECOMMIT) {
        u64 hi = ((u64)(uintptr_t)address + size + PAGE - 1) & ~(PAGE - 1);
        pthread_mutex_lock(&mem_lock);
        Reservation *r = reservation_of(lo, hi);
        int watch = r && r->watch;
        pthread_mutex_unlock(&mem_lock);
        if (watch) ww_decommit(lo, hi);
        madvise((void *)(uintptr_t)lo, hi - lo, MADV_DONTNEED);   /* (the contents go, as on Windows) */
        mprotect((void *)(uintptr_t)lo, hi - lo, PROT_NONE);
        return TRUE;
    }
    last_error = ERROR_INVALID_PARAMETER;
    return FALSE;
}

BOOL VirtualProtect(LPVOID address, SIZE_T size, DWORD protect, PDWORD old) {
    if (old) *old = PAGE_READWRITE;
    u64 lo = (u64)(uintptr_t)address & ~(PAGE - 1), hi = ((u64)(uintptr_t)address + size + PAGE - 1) & ~(PAGE - 1);
    return mprotect((void *)(uintptr_t)lo, hi - lo, prot_of(protect)) ? fail_errno() : TRUE;
}

/* From /proc/self/maps: the mapping holding the address (committed, if readable or writable),
 * or the free gap before the next one. */
SIZE_T VirtualQuery(LPCVOID address, MEMORY_BASIC_INFORMATION *info, SIZE_T length) {
    if (length < sizeof *info) return 0;
    u64 a = (u64)(uintptr_t)address & ~(PAGE - 1);
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    u64 next = ~0ull;
    memset(info, 0, sizeof *info);
    while (fgets(line, sizeof line, f)) {
        unsigned long long lo, hi;
        char perms[8] = "";
        if (sscanf(line, "%llx-%llx %7s", &lo, &hi, perms) != 3) continue;
        if (a >= lo && a < hi) {
            info->BaseAddress = info->AllocationBase = (void *)(uintptr_t)lo;
            info->RegionSize = hi - lo;
            int r = perms[0] == 'r', w = perms[1] == 'w';
            info->State = r || w ? MEM_COMMIT : MEM_RESERVE;
            info->Protect = w ? PAGE_READWRITE : r ? PAGE_READONLY : PAGE_NOACCESS;
            info->Type = MEM_PRIVATE;
            fclose(f);
            return sizeof *info;
        }
        if (lo > a && lo < next) next = lo;
    }
    fclose(f);
    if (next == ~0ull) return 0;
    info->BaseAddress = (void *)(uintptr_t)a;
    info->RegionSize = next - a;
    info->State = MEM_FREE;
    info->Protect = PAGE_NOACCESS;
    return sizeof *info;
}
#endif

void GetSystemInfo(SYSTEM_INFO *info) {
    memset(info, 0, sizeof *info);
    info->dwPageSize = 4096;
    info->dwAllocationGranularity = 65536;
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    info->dwNumberOfProcessors = n > 0 ? (DWORD)n : 1;
    info->lpMinimumApplicationAddress = (void *)0x10000;
    info->lpMaximumApplicationAddress = (void *)0x7FFFFFFEFFFFull;
}

/* ---- the write watch ---------------------------------------------------------------------- */
#ifndef __APPLE__
#ifndef UFFD_USER_MODE_ONLY
#define UFFD_USER_MODE_ONLY 1
#endif
#ifndef UFFD_FEATURE_WP_UNPOPULATED
#define UFFD_FEATURE_WP_UNPOPULATED (1 << 13)
#endif
#ifndef UFFD_FEATURE_WP_ASYNC
#define UFFD_FEATURE_WP_ASYNC (1 << 15)
#endif
/* (linux/fs.h of 6.7; the sysroot's headers predate it) */
struct rt_page_region { u64 start, end, categories; };
struct rt_pm_scan_arg {
    u64 size, flags, start, end, walk_end, vec, vec_len, max_pages, category_inverted, category_mask, category_anyof_mask, return_mask;
};
#define RT_PAGEMAP_SCAN _IOWR('f', 16, struct rt_pm_scan_arg)
#define RT_PM_SCAN_WP_MATCHING (1 << 0)
#define RT_PAGE_IS_WRITTEN (1 << 1)

enum { WW_NONE, WW_SCAN, WW_PROTECT };
static int ww_mode = -1;
static int uffd = -1, pagemap = -1;
static struct WatchArea { u64 base, size; u8 *committed, *dirty; } areas[4];   /* bitmaps: one bit a page */
static int nareas;

static int bit(const u8 *map, u64 i) { return map[i >> 3] >> (i & 7) & 1; }
static void set_bit(u8 *map, u64 i) { __atomic_fetch_or(&map[i >> 3], (u8)(1u << (i & 7)), __ATOMIC_RELAXED); }
static void clear_bit(u8 *map, u64 i) { __atomic_fetch_and(&map[i >> 3], (u8)~(1u << (i & 7)), __ATOMIC_RELAXED); }

static struct WatchArea *area_of(u64 lo, u64 hi) {
    for (int i = 0; i < nareas; i++)
        if (areas[i].base <= lo && hi <= areas[i].base + areas[i].size) return &areas[i];
    return NULL;
}

static int uffd_protect(u64 lo, u64 hi, int on) {
    struct uffdio_writeprotect wp = {{lo, hi - lo}, on ? UFFDIO_WRITEPROTECT_MODE_WP : 0};
    return ioctl(uffd, UFFDIO_WRITEPROTECT, &wp);
}

static long scan(u64 lo, u64 hi, int reset, struct rt_page_region *vec, u64 nvec, u64 max_pages, u64 *walk_end) {
    struct rt_pm_scan_arg a;
    memset(&a, 0, sizeof a);
    a.size = sizeof a;
    a.flags = reset ? RT_PM_SCAN_WP_MATCHING : 0;
    a.start = lo;
    a.end = hi;
    a.vec = (u64)(uintptr_t)vec;
    a.vec_len = nvec;
    a.max_pages = max_pages;
    a.category_mask = RT_PAGE_IS_WRITTEN;
    a.return_mask = RT_PAGE_IS_WRITTEN;
    long n = ioctl(pagemap, RT_PAGEMAP_SCAN, &a);
    *walk_end = a.walk_end;
    return n;
}

/* Whether the kernel's watch works here: two pages, one written (by a system call, as reads
 * into guest memory are), exactly that one reported, and nothing after a reset. */
static int scan_works(void) {
    u8 *p = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return 0;
    int ok = 0;
    struct uffdio_register reg = {{(u64)(uintptr_t)p, 2 * PAGE}, UFFDIO_REGISTER_MODE_WP, 0};
    if (!ioctl(uffd, UFFDIO_REGISTER, &reg) && !uffd_protect((u64)(uintptr_t)p, (u64)(uintptr_t)p + 2 * PAGE, 1)) {
        int fds[2];
        if (!pipe(fds)) {
            ssize_t w = write(fds[1], "watch", 5), r = read(fds[0], p + PAGE + 100, 5);
            close(fds[0]);
            close(fds[1]);
            struct rt_page_region v[4];
            u64 end;
            long n = scan((u64)(uintptr_t)p, (u64)(uintptr_t)p + 2 * PAGE, 1, v, 4, 0, &end);
            int one = w == 5 && r == 5 && n == 1 && v[0].start == (u64)(uintptr_t)p + PAGE && v[0].end == (u64)(uintptr_t)p + 2 * PAGE;
            n = scan((u64)(uintptr_t)p, (u64)(uintptr_t)p + 2 * PAGE, 0, v, 4, 0, &end);
            ok = one && n == 0;
        }
    }
    munmap(p, 2 * PAGE);
    return ok;
}

static void ww_init(void) {
    if (ww_mode >= 0) return;
    ww_mode = WW_PROTECT;
    const char *force = getenv("SSBU_WRITE_WATCH");   /* "protect": the fallback, for testing */
    if (force && !strcmp(force, "protect")) return;
    uffd = (int)syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
    if (uffd < 0) uffd = (int)syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK);
    if (uffd < 0) return;
    struct uffdio_api api = {UFFD_API, UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_UNPOPULATED, 0};
    pagemap = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (ioctl(uffd, UFFDIO_API, &api) || pagemap < 0 || !scan_works()) {
        close(uffd);
        uffd = -1;
        if (pagemap >= 0) close(pagemap);
        pagemap = -1;
        return;
    }
    ww_mode = WW_SCAN;
}

const char *rt_ww_mode(void) {
    ww_init();
    return ww_mode == WW_SCAN ? "kernel (userfaultfd + PAGEMAP_SCAN)" : "page protection (fallback)";
}

static void ww_register(u64 base, u64 size) {
    ww_init();
    if (nareas == 4) { fprintf(stderr, "[mem] too many write-watched areas\n"); return; }
    struct WatchArea *a = &areas[nareas];
    u64 pages = size / PAGE;
    a->committed = calloc(1, pages / 8 + 1);
    a->dirty = calloc(1, pages / 8 + 1);
    a->base = base;
    a->size = size;
    madvise((void *)(uintptr_t)base, size, MADV_NOHUGEPAGE);   /* (page-sized reports, not 2 MB ones) */
    if (ww_mode == WW_SCAN) {
        struct uffdio_register reg = {{base, size}, UFFDIO_REGISTER_MODE_WP, 0};
        if (ioctl(uffd, UFFDIO_REGISTER, &reg)) {
            fprintf(stderr, "[mem] userfaultfd registration failed (errno %d): using page protection\n", errno);
            ww_mode = WW_PROTECT;
        }
    }
    nareas++;
}

/* Newly committed pages start unwritten. */
static void ww_commit(u64 lo, u64 hi) {
    struct WatchArea *a = area_of(lo, hi);
    if (!a) { mprotect((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE); return; }
    mprotect((void *)(uintptr_t)lo, hi - lo, ww_mode == WW_PROTECT ? PROT_READ : PROT_READ | PROT_WRITE);
    u64 first = (lo - a->base) / PAGE, last = (hi - a->base) / PAGE;
    for (u64 i = first; i < last;) {   /* runs not committed before */
        if (bit(a->committed, i)) { i++; continue; }
        u64 j = i;
        while (j < last && !bit(a->committed, j)) { set_bit(a->committed, j); clear_bit(a->dirty, j); j++; }
        if (ww_mode == WW_SCAN) uffd_protect(a->base + i * PAGE, a->base + j * PAGE, 1);
        i = j;
    }
    if (ww_mode == WW_PROTECT)   /* (pages committed before keep their state: writable if written) */
        for (u64 i = first; i < last; i++)
            if (bit(a->dirty, i)) mprotect((void *)(uintptr_t)(a->base + i * PAGE), PAGE, PROT_READ | PROT_WRITE);
}

static void ww_decommit(u64 lo, u64 hi) {
    struct WatchArea *a = area_of(lo, hi);
    if (!a) return;
    for (u64 i = (lo - a->base) / PAGE; i < (hi - a->base) / PAGE; i++) { clear_bit(a->committed, i); clear_bit(a->dirty, i); }
}

/* (from the fault handler) A write to a protected page of a watched area: note it, allow it. */
int rt_ww_fault(void *address) {
    if (ww_mode != WW_PROTECT) return 0;
    u64 p = (u64)(uintptr_t)address & ~(PAGE - 1);
    struct WatchArea *a = area_of(p, p + PAGE);
    if (!a || !bit(a->committed, (p - a->base) / PAGE)) return 0;
    set_bit(a->dirty, (p - a->base) / PAGE);
    return mprotect((void *)(uintptr_t)p, PAGE, PROT_READ | PROT_WRITE) == 0;
}

void rt_ww_prepare_write(void *p, size_t n) {
    if (ww_mode != WW_PROTECT || !n) return;
    u64 lo = (u64)(uintptr_t)p & ~(PAGE - 1), hi = ((u64)(uintptr_t)p + n + PAGE - 1) & ~(PAGE - 1);
    for (struct WatchArea *a = area_of(lo, lo + PAGE); a && lo < hi; lo += PAGE)
        if (lo >= a->base && lo < a->base + a->size && bit(a->committed, (lo - a->base) / PAGE) && !bit(a->dirty, (lo - a->base) / PAGE)) {
            set_bit(a->dirty, (lo - a->base) / PAGE);
            mprotect((void *)(uintptr_t)lo, PAGE, PROT_READ | PROT_WRITE);
        }
}

UINT GetWriteWatch(DWORD flags, PVOID base, SIZE_T size, PVOID *addresses, ULONG_PTR *count, ULONG *granularity) {
    u64 lo = (u64)(uintptr_t)base & ~(PAGE - 1), hi = ((u64)(uintptr_t)base + size + PAGE - 1) & ~(PAGE - 1);
    struct WatchArea *a = area_of(lo, hi);
    if (granularity) *granularity = (ULONG)PAGE;
    if (!a || !count) { last_error = ERROR_INVALID_PARAMETER; return 1; }
    ULONG_PTR cap = *count, n = 0;
    if (ww_mode == WW_SCAN) {
        struct rt_page_region vec[64];
        u64 at = lo;
        while (at < hi && n < cap) {
            u64 end;
            long got = scan(at, hi, flags & WRITE_WATCH_FLAG_RESET, vec, 64, cap - n, &end);
            if (got < 0) { last_error = rt_win32_error(errno); return 1; }
            for (long k = 0; k < got; k++)
                for (u64 pg = vec[k].start; pg < vec[k].end && n < cap; pg += PAGE) addresses[n++] = (void *)(uintptr_t)pg;
            if (end >= hi || end <= at) break;   /* (else the vector filled up: go on from where it stopped) */
            at = end;
        }
    } else {
        for (u64 i = (lo - a->base) / PAGE; i < (hi - a->base) / PAGE && n < cap; i++)
            if (bit(a->dirty, i)) {
                addresses[n++] = (void *)(uintptr_t)(a->base + i * PAGE);
                if (flags & WRITE_WATCH_FLAG_RESET) {
                    clear_bit(a->dirty, i);
                    mprotect((void *)(uintptr_t)(a->base + i * PAGE), PAGE, PROT_READ);
                }
            }
    }
    *count = n;
    return 0;
}

UINT ResetWriteWatch(LPVOID base, SIZE_T size) {
    u64 lo = (u64)(uintptr_t)base & ~(PAGE - 1), hi = ((u64)(uintptr_t)base + size + PAGE - 1) & ~(PAGE - 1);
    struct WatchArea *a = area_of(lo, hi);
    if (!a) return 1;
    if (ww_mode == WW_SCAN) return uffd_protect(lo, hi, 1) ? 1 : 0;
    for (u64 i = (lo - a->base) / PAGE; i < (hi - a->base) / PAGE;) {   /* runs of written, committed pages */
        if (!bit(a->dirty, i) || !bit(a->committed, i)) { i++; continue; }
        u64 j = i;
        while (j < (hi - a->base) / PAGE && bit(a->dirty, j) && bit(a->committed, j)) clear_bit(a->dirty, j++);
        mprotect((void *)(uintptr_t)(a->base + i * PAGE), (j - i) * PAGE, PROT_READ);
        i = j;
    }
    return 0;
}
#endif

/* ---- files -------------------------------------------------------------------------------- */
const char *rt_unix_path(const char *path, char *buf, size_t n) {
    snprintf(buf, n, "%s", path);
    for (char *p = buf; *p; p++) if (*p == '\\') *p = '/';
    return buf;
}

int rt_fd_of(HANDLE h) {
    Obj *o = obj_of(h, H_FILE);
    return o ? o->fd : -1;
}

HANDLE CreateFileA(LPCSTR path, DWORD access, DWORD share, void *security, DWORD disposition, DWORD flags, HANDLE template_file) {
    (void)share; (void)security; (void)template_file;
    char buf[4096];
    int mode = (access & GENERIC_WRITE) ? ((access & GENERIC_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disposition) {
    case CREATE_NEW: mode |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS: mode |= O_CREAT | O_TRUNC; break;
    case OPEN_ALWAYS: mode |= O_CREAT; break;
    case TRUNCATE_EXISTING: mode |= O_TRUNC; break;
    }
    int fd = open(rt_unix_path(path, buf, sizeof buf), mode | O_CLOEXEC, 0644);
    if (fd < 0) {
        last_error = errno == EEXIST ? ERROR_FILE_EXISTS : rt_win32_error(errno);
        return INVALID_HANDLE_VALUE;
    }
    struct stat st;
    if (!fstat(fd, &st) && S_ISDIR(st.st_mode)) { close(fd); last_error = ERROR_ACCESS_DENIED; return INVALID_HANDLE_VALUE; }
#ifdef __APPLE__
    if (flags & FILE_FLAG_RANDOM_ACCESS) fcntl(fd, F_RDAHEAD, 0);   /* (no read-ahead: macOS's posix_fadvise) */
#else
    if (flags & FILE_FLAG_RANDOM_ACCESS) posix_fadvise(fd, 0, 0, POSIX_FADV_RANDOM);
#endif
    Obj *o = new_obj(H_FILE);
    o->fd = fd;
    return o;
}

#ifdef __APPLE__
int rt_ww_storable(const void *p, size_t n);   /* (runtime/macos/memory.c) */

/* The read through a host buffer, a megabyte at a time, copied into buf by stores: a store to a
 * protected page faults, and the fault handler lets it through (rt_ww_fault), however often the
 * renderer protects the pages again meanwhile. (Opening them first only spares those faults.) */
static ssize_t read_bounced(int fd, void *buf, size_t n, off_t offset, int positional) {
    size_t cap = n < (1u << 20) ? n : (1u << 20), done = 0;
    u8 *tmp = malloc(cap);
    if (!tmp) { errno = ENOMEM; return -1; }
    while (done < n) {
        size_t want = n - done < cap ? n - done : cap;
        ssize_t r = positional ? pread(fd, tmp, want, offset + (off_t)done) : read(fd, tmp, want);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && !done) { int err = errno; free(tmp); errno = err; return -1; }
        if (r <= 0) break;
        rt_ww_prepare_write((u8 *)buf + done, (size_t)r);
        memcpy((u8 *)buf + done, tmp, (size_t)r);
        done += (size_t)r;
        if ((size_t)r < want) break;   /* (the end of the file) */
    }
    free(tmp);
    return (ssize_t)done;
}
#endif

/* A read into memory the protecting write watch may have protected: a system call writing a
 * protected page fails with EFAULT rather than faulting. On Linux the pages are opened and the
 * read tried again. On macOS that is not enough: the renderer may protect them again before the
 * call reaches them (a reset of anything sharing their 16 KiB pages), and the call then fails
 * having filled the pages before (read() moving the file offset on by those) - so the read goes
 * back to where it started and through a host buffer. */
static ssize_t read_some(int fd, void *buf, size_t n, off_t offset, int positional) {
#ifdef __APPLE__   /* (macOS: always the protecting watch) */
    off_t start = positional ? offset : lseek(fd, 0, SEEK_CUR);
    ssize_t r = positional ? pread(fd, buf, n, offset) : read(fd, buf, n);
    if (r >= 0 || errno != EFAULT) return r;
    if (!positional && start >= 0) lseek(fd, start, SEEK_SET);
    if (!rt_ww_storable(buf, n)) { errno = EFAULT; return -1; }   /* (memory no store can go to: not the watch's doing) */
    return read_bounced(fd, buf, n, offset, positional);
#else
    ssize_t r = positional ? pread(fd, buf, n, offset) : read(fd, buf, n);
    if (r < 0 && errno == EFAULT && ww_mode == WW_PROTECT) {
        rt_ww_prepare_write(buf, n);
        r = positional ? pread(fd, buf, n, offset) : read(fd, buf, n);
    }
    return r;
#endif
}

BOOL ReadFile(HANDLE h, LPVOID buffer, DWORD size, LPDWORD read_count, LPOVERLAPPED overlapped) {
    Obj *o = obj_of(h, H_FILE);
    if (read_count) *read_count = 0;
    if (!o) { last_error = ERROR_INVALID_HANDLE; return FALSE; }
    u8 *p = buffer;
    off_t at = overlapped ? (off_t)((u64)overlapped->OffsetHigh << 32 | overlapped->Offset) : 0;
    DWORD done = 0;
    while (done < size) {
        ssize_t r = read_some(o->fd, p + done, size - done, at + done, overlapped != NULL);
        if (r < 0) { if (errno == EINTR) continue; return fail_errno(); }
        if (!r) break;
        done += (DWORD)r;
    }
    if (read_count) *read_count = done;
    if (overlapped && !done && size) { last_error = ERROR_HANDLE_EOF; return FALSE; }
    return TRUE;
}

BOOL WriteFile(HANDLE h, LPCVOID buffer, DWORD size, LPDWORD written, LPOVERLAPPED overlapped) {
    Obj *o = obj_of(h, H_FILE);
    if (written) *written = 0;
    if (!o) { last_error = ERROR_INVALID_HANDLE; return FALSE; }
    const u8 *p = buffer;
    off_t at = overlapped ? (off_t)((u64)overlapped->OffsetHigh << 32 | overlapped->Offset) : 0;
    DWORD done = 0;
    while (done < size) {
        ssize_t r = overlapped ? pwrite(o->fd, p + done, size - done, at + done) : write(o->fd, p + done, size - done);
        if (r < 0) { if (errno == EINTR) continue; return fail_errno(); }
        done += (DWORD)r;
    }
    if (written) *written = done;
    return TRUE;
}

BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER *size) {
    Obj *o = obj_of(h, H_FILE);
    struct stat st;
    if (!o || fstat(o->fd, &st)) return fail_errno();
    size->QuadPart = st.st_size;
    return TRUE;
}

BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER distance, LARGE_INTEGER *new_position, DWORD method) {
    Obj *o = obj_of(h, H_FILE);
    if (!o) { last_error = ERROR_INVALID_HANDLE; return FALSE; }
    off_t r = lseek(o->fd, (off_t)distance.QuadPart, method == FILE_END ? SEEK_END : method == FILE_CURRENT ? SEEK_CUR : SEEK_SET);
    if (r < 0) return fail_errno();
    if (new_position) new_position->QuadPart = r;
    return TRUE;
}

BOOL SetEndOfFile(HANDLE h) {
    Obj *o = obj_of(h, H_FILE);
    if (!o) { last_error = ERROR_INVALID_HANDLE; return FALSE; }
    off_t at = lseek(o->fd, 0, SEEK_CUR);
    return at < 0 || ftruncate(o->fd, at) ? fail_errno() : TRUE;
}

/* To the disk itself: macOS's fsync leaves the data in the drive's cache, F_FULLFSYNC has the
 * drive write it out (as FlushFileBuffers does on Windows); fsync where the file system has no
 * F_FULLFSYNC. */
static int full_fsync(int fd) {
#ifdef __APPLE__
    if (!fcntl(fd, F_FULLFSYNC)) return 0;
#endif
    return fsync(fd);
}

BOOL FlushFileBuffers(HANDLE h) {
    Obj *o = obj_of(h, H_FILE);
    if (!o) { last_error = ERROR_INVALID_HANDLE; return FALSE; }
    return full_fsync(o->fd) ? fail_errno() : TRUE;
}

#ifdef __APPLE__   /* (stat's times by their Darwin names; macOS keeps a real creation time) */
#define st_atim st_atimespec
#define st_mtim st_mtimespec
#define st_ctim st_birthtimespec
#endif

/* (Linux keeps no creation time: the change time stands in for it) */
BOOL GetFileTime(HANDLE h, FILETIME *created, FILETIME *accessed, FILETIME *written) {
    Obj *o = obj_of(h, H_FILE);
    struct stat st;
    if (!o || fstat(o->fd, &st)) return fail_errno();
    if (created) *created = filetime_of(st.st_ctim);
    if (accessed) *accessed = filetime_of(st.st_atim);
    if (written) *written = filetime_of(st.st_mtim);
    return TRUE;
}

BOOL SetFileTime(HANDLE h, const FILETIME *created, const FILETIME *accessed, const FILETIME *written) {
    (void)created;
    Obj *o = obj_of(h, H_FILE);
    if (!o) { last_error = ERROR_INVALID_HANDLE; return FALSE; }
    struct timespec t[2] = {{0, UTIME_OMIT}, {0, UTIME_OMIT}};
    if (accessed) t[0] = timespec_of(accessed);
    if (written) t[1] = timespec_of(written);
    return futimens(o->fd, t) ? fail_errno() : TRUE;
}

BOOL DeleteFileA(LPCSTR path) {
    char buf[4096];
    return unlink(rt_unix_path(path, buf, sizeof buf)) ? fail_errno() : TRUE;
}

BOOL CreateDirectoryA(LPCSTR path, void *security) {
    (void)security;
    char buf[4096];
    return mkdir(rt_unix_path(path, buf, sizeof buf), 0755) ? fail_errno() : TRUE;
}

int _mkdir(const char *path) {
    char buf[4096];
    return mkdir(rt_unix_path(path, buf, sizeof buf), 0755);
}

BOOL RemoveDirectoryA(LPCSTR path) {
    char buf[4096];
    return rmdir(rt_unix_path(path, buf, sizeof buf)) ? fail_errno() : TRUE;
}

/* rename(): atomic, and it replaces an existing file whatever the flags say. With
 * MOVEFILE_WRITE_THROUGH the file and the folder now holding its name are flushed before it
 * returns, so the move survives a power cut (the folder to the disk itself on macOS). */
BOOL MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags) {
    char a[4096], b[4096];
    if (rename(rt_unix_path(from, a, sizeof a), rt_unix_path(to, b, sizeof b))) return fail_errno();
    if (!(flags & MOVEFILE_WRITE_THROUGH)) return TRUE;
    int fd = open(b, O_RDONLY | O_CLOEXEC), ok = fd >= 0 && !fsync(fd);
    if (fd >= 0) close(fd);
    char *slash = strrchr(b, '/');
    if (slash == b) slash[1] = 0;
    else if (slash) *slash = 0;
    else snprintf(b, sizeof b, ".");
    fd = open(b, O_RDONLY | O_CLOEXEC);
    ok = ok && fd >= 0 && !full_fsync(fd);
    if (fd >= 0) close(fd);
    return ok ? TRUE : fail_errno();
}

DWORD GetFileAttributesA(LPCSTR path) {
    char buf[4096];
    struct stat st;
    if (stat(rt_unix_path(path, buf, sizeof buf), &st)) { last_error = rt_win32_error(errno); return INVALID_FILE_ATTRIBUTES; }
    return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}

/* Directory searches: "<dir>/<pattern>", with "." and ".." listed as Windows does. */
static BOOL find_next(Obj *o, WIN32_FIND_DATAA *data) {
    for (struct dirent *e; (e = readdir(o->dir));) {
        if (fnmatch(o->pattern, e->d_name, 0)) continue;
        memset(data, 0, sizeof *data);
        snprintf(data->cFileName, sizeof data->cFileName, "%s", e->d_name);
        char full[2048];
        snprintf(full, sizeof full, "%s/%s", o->dir_path, e->d_name);
        struct stat st;
        if (!stat(full, &st)) {
            data->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            data->nFileSizeHigh = (DWORD)((u64)st.st_size >> 32);
            data->nFileSizeLow = (DWORD)st.st_size;
#ifdef __APPLE__
            data->ftCreationTime = filetime_of(st.st_ctim);   /* (the birth time: above) */
            data->ftLastWriteTime = filetime_of(st.st_mtim);
#else
            data->ftCreationTime = data->ftLastWriteTime = filetime_of(st.st_mtim);   /* (no birth time in stat) */
#endif
            data->ftLastAccessTime = filetime_of(st.st_atim);
        } else data->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
        return TRUE;
    }
    last_error = ERROR_NO_MORE_FILES;
    return FALSE;
}

HANDLE FindFirstFileA(LPCSTR pattern, WIN32_FIND_DATAA *data) {
    char buf[4096];
    rt_unix_path(pattern, buf, sizeof buf);
    char *slash = strrchr(buf, '/');
    Obj *o = new_obj(H_FIND);
    if (slash) { *slash = 0; snprintf(o->dir_path, sizeof o->dir_path, "%s", buf[0] ? buf : "/"); snprintf(o->pattern, sizeof o->pattern, "%s", slash + 1); }
    else { snprintf(o->dir_path, sizeof o->dir_path, "."); snprintf(o->pattern, sizeof o->pattern, "%s", buf); }
    if (!strcmp(o->pattern, "*.*")) snprintf(o->pattern, sizeof o->pattern, "*");
    o->dir = opendir(o->dir_path);
    if (!o->dir) { last_error = rt_win32_error(errno) == ERROR_FILE_NOT_FOUND ? ERROR_PATH_NOT_FOUND : rt_win32_error(errno); obj_release(o); return INVALID_HANDLE_VALUE; }
    if (!find_next(o, data)) { obj_release(o); last_error = ERROR_FILE_NOT_FOUND; return INVALID_HANDLE_VALUE; }
    return o;
}

BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *data) {
    Obj *o = obj_of(h, H_FIND);
    return o ? find_next(o, data) : FALSE;
}

BOOL FindClose(HANDLE h) { return CloseHandle(h); }

/* ---- the process -------------------------------------------------------------------------- */
errno_t rand_s(unsigned int *value) {
#ifdef __APPLE__
    arc4random_buf(value, sizeof *value);
    return 0;
#else
    return getrandom(value, sizeof *value, 0) == (ssize_t)sizeof *value ? 0 : EIO;
#endif
}

void ExitProcess(UINT code) {
    fflush(NULL);
    _exit((int)code);
}

#ifdef __APPLE__   /* (the program's Mach-O header: where it is loaded) */
HMODULE GetModuleHandleW(LPCWSTR name) { return name ? NULL : (HMODULE)&_mh_execute_header; }
HMODULE GetModuleHandleA(LPCSTR name) { return name ? NULL : (HMODULE)&_mh_execute_header; }
#else
extern char __executable_start[];
HMODULE GetModuleHandleW(LPCWSTR name) { return name ? NULL : (HMODULE)__executable_start; }
HMODULE GetModuleHandleA(LPCSTR name) { return name ? NULL : (HMODULE)__executable_start; }
#endif

USHORT CaptureStackBackTrace(ULONG skip, ULONG count, PVOID *frames, PULONG hash) {
    void *all[128];
    int n = backtrace(all, 128);
    USHORT out = 0;
    for (int i = (int)skip + 1; i < n && out < count; i++) frames[out++] = all[i];
    if (hash) *hash = 0;
    return out;
}

void OutputDebugStringA(LPCSTR text) { fputs(text, stderr); }

#ifdef __APPLE__
BOOL IsDebuggerPresent(void) {   /* (the kernel's flag that something traces the process) */
    struct kinfo_proc info;
    size_t size = sizeof info;
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    memset(&info, 0, sizeof info);
    return sysctl(mib, 4, &info, &size, NULL, 0) == 0 && (info.kp_proc.p_flag & P_TRACED) != 0;
}
#else
BOOL IsDebuggerPresent(void) {
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    int tracer = 0;
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, "TracerPid:", 10)) { tracer = atoi(line + 10); break; }
    if (f) fclose(f);
    return tracer != 0;
}
#endif
