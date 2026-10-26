/* The part of the Windows API the runtime uses, for Linux (runtime/linux/win32.c) and macOS.
 *
 * The runtime was written against Win32; rather than rewrite every lock, thread, file and
 * memory call, Linux builds get this header in place of <windows.h>: the same names, with
 * the same meaning, on POSIX threads, futexes, mmap and file descriptors. Types come from
 * DXVK's windows_base.h, which its Direct3D headers need anyway (DWORD and LONG are 32-bit,
 * as on Windows). Only what the runtime calls is here.
 *
 * macOS builds use it too, with the same win32.c; what differs there (memory with 16 KiB pages,
 * arm64 fibers, Mach thread ids, the profiler) is in runtime/macos/, whose include folder comes
 * first and has small stand-ins for DXVK's two headers. */
#pragma once
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <wchar.h>
#include <sys/types.h>

#include "windows_base.h"   /* (DXVK's; macOS: runtime/macos/include's) */
#include "unknwn.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- types ---------------------------------------------------------------------------- */
typedef int64_t LONG64;
typedef uint64_t ULONG64, DWORD64, DWORD_PTR;
typedef LONG64 *PLONG64;
typedef DWORD *PDWORD;
typedef ULONG *PULONG;
typedef ULONG_PTR *PULONG_PTR;
typedef SIZE_T *PSIZE_T;
typedef BOOL *PBOOL, *LPBOOL;
typedef BYTE BOOLEAN;
typedef const WCHAR *PCWSTR;
typedef WCHAR *PWCHAR;
typedef UINT MMRESULT;
typedef LONG_PTR LRESULT, LPARAM;
typedef UINT_PTR WPARAM;
typedef int errno_t;

#define CALLBACK
#define APIENTRY
#define WINAPIV
#define WINBASEAPI
#define FORCEINLINE static inline __attribute__((always_inline))
#define DECLSPEC_NORETURN __attribute__((noreturn))
/* __declspec(thread), __declspec(noinline), __declspec(align(n)) ... */
#define __declspec(x) RT_DECLSPEC_##x
#define RT_DECLSPEC_thread __thread
#define RT_DECLSPEC_noinline __attribute__((noinline))
#define RT_DECLSPEC_noreturn __attribute__((noreturn))
#define RT_DECLSPEC_align(n) __attribute__((aligned(n)))
#define RT_DECLSPEC_selectany __attribute__((weak))
#define RT_DECLSPEC_dllexport __attribute__((visibility("default")))
#define RT_DECLSPEC_dllimport

#define INFINITE 0xFFFFFFFFu
#define STILL_ACTIVE 259

#define LOWORD(l) ((WORD)((DWORD_PTR)(l) & 0xffff))
#define HIWORD(l) ((WORD)(((DWORD_PTR)(l) >> 16) & 0xffff))
#define MAKEWORD(a, b) ((WORD)(((BYTE)(a)) | ((WORD)((BYTE)(b))) << 8))

/* ---- errors ---------------------------------------------------------------------------- */
#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_PATH_NOT_FOUND 3
#define ERROR_ACCESS_DENIED 5
#define ERROR_INVALID_HANDLE 6
#define ERROR_NOT_ENOUGH_MEMORY 8
#define ERROR_INVALID_PARAMETER 87
#define ERROR_NO_MORE_FILES 18
#define ERROR_SHARING_VIOLATION 32   /* (never on POSIX: nothing locks a file against others) */
#define ERROR_LOCK_VIOLATION 33
#define ERROR_HANDLE_EOF 38
#define ERROR_FILE_EXISTS 80
#define ERROR_DISK_FULL 112
#define ERROR_DIR_NOT_EMPTY 145
#define ERROR_ALREADY_EXISTS 183
#define ERROR_INVALID_ADDRESS 487
DWORD GetLastError(void);
void SetLastError(DWORD error);
DWORD rt_win32_error(int err);   /* errno -> the nearest Win32 error */

/* ---- atomics --------------------------------------------------------------------------- */
#define RT_SC __ATOMIC_SEQ_CST
static inline LONG InterlockedIncrement(volatile LONG *p) { return __atomic_add_fetch(p, 1, RT_SC); }
static inline LONG InterlockedDecrement(volatile LONG *p) { return __atomic_sub_fetch(p, 1, RT_SC); }
static inline LONG64 InterlockedIncrement64(volatile LONG64 *p) { return __atomic_add_fetch(p, 1, RT_SC); }
static inline LONG64 InterlockedDecrement64(volatile LONG64 *p) { return __atomic_sub_fetch(p, 1, RT_SC); }
static inline LONG InterlockedExchange(volatile LONG *p, LONG v) { return __atomic_exchange_n(p, v, RT_SC); }
static inline LONG64 InterlockedExchange64(volatile LONG64 *p, LONG64 v) { return __atomic_exchange_n(p, v, RT_SC); }
static inline LONG InterlockedExchangeAdd(volatile LONG *p, LONG v) { return __atomic_fetch_add(p, v, RT_SC); }
static inline LONG InterlockedAdd(volatile LONG *p, LONG v) { return __atomic_add_fetch(p, v, RT_SC); }
static inline LONG64 InterlockedExchangeAdd64(volatile LONG64 *p, LONG64 v) { return __atomic_fetch_add(p, v, RT_SC); }
static inline LONG64 InterlockedAdd64(volatile LONG64 *p, LONG64 v) { return __atomic_add_fetch(p, v, RT_SC); }
static inline LONG InterlockedOr(volatile LONG *p, LONG v) { return __atomic_fetch_or(p, v, RT_SC); }
static inline LONG InterlockedAnd(volatile LONG *p, LONG v) { return __atomic_fetch_and(p, v, RT_SC); }
static inline LONG64 InterlockedOr64(volatile LONG64 *p, LONG64 v) { return __atomic_fetch_or(p, v, RT_SC); }
static inline LONG InterlockedCompareExchange(volatile LONG *p, LONG exchange, LONG comparand) {
    __atomic_compare_exchange_n(p, &comparand, exchange, 0, RT_SC, RT_SC);
    return comparand;
}
static inline LONG64 InterlockedCompareExchange64(volatile LONG64 *p, LONG64 exchange, LONG64 comparand) {
    __atomic_compare_exchange_n(p, &comparand, exchange, 0, RT_SC, RT_SC);
    return comparand;
}
static inline PVOID InterlockedCompareExchangePointer(PVOID volatile *p, PVOID exchange, PVOID comparand) {
    __atomic_compare_exchange_n(p, &comparand, exchange, 0, RT_SC, RT_SC);
    return comparand;
}
static inline PVOID InterlockedExchangePointer(PVOID volatile *p, PVOID v) { return __atomic_exchange_n(p, v, RT_SC); }
#define MemoryBarrier() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#if defined(__aarch64__)
#define YieldProcessor() __asm__ __volatile__("isb" ::: "memory")   /* (arm64's spin-wait pause: yield is a no-op on Apple's cores) */
#else
#define YieldProcessor() __builtin_ia32_pause()
#endif

/* ---- locks and condition variables ------------------------------------------------------- */
typedef struct SRWLOCK { pthread_rwlock_t l; } SRWLOCK, *PSRWLOCK;
#define SRWLOCK_INIT {PTHREAD_RWLOCK_INITIALIZER}
static inline void InitializeSRWLock(SRWLOCK *l) { pthread_rwlock_init(&l->l, NULL); }
static inline void AcquireSRWLockExclusive(SRWLOCK *l) { pthread_rwlock_wrlock(&l->l); }
static inline void ReleaseSRWLockExclusive(SRWLOCK *l) { pthread_rwlock_unlock(&l->l); }
static inline void AcquireSRWLockShared(SRWLOCK *l) { pthread_rwlock_rdlock(&l->l); }
static inline void ReleaseSRWLockShared(SRWLOCK *l) { pthread_rwlock_unlock(&l->l); }
static inline BOOLEAN TryAcquireSRWLockExclusive(SRWLOCK *l) { return pthread_rwlock_trywrlock(&l->l) == 0; }
static inline BOOLEAN TryAcquireSRWLockShared(SRWLOCK *l) { return pthread_rwlock_tryrdlock(&l->l) == 0; }

#ifdef __APPLE__
/* Recursive, as on Windows. Darwin's mutex keeps its owner to itself, so the holder's thread id
 * (GetCurrentThreadId's, 0 when free) is noted here, with the depth of its recursion: read
 * only, by other threads, for freeze reports. */
typedef struct CRITICAL_SECTION {
    pthread_mutex_t m;
    volatile DWORD OwningThread;
    int depth;
} CRITICAL_SECTION, *LPCRITICAL_SECTION;
void InitializeCriticalSection(CRITICAL_SECTION *cs);
static inline BOOL InitializeCriticalSectionAndSpinCount(CRITICAL_SECTION *cs, DWORD spin) { (void)spin; InitializeCriticalSection(cs); return TRUE; }
static inline void EnterCriticalSection(CRITICAL_SECTION *cs) {
    pthread_mutex_lock(&cs->m);
    if (!cs->depth++) cs->OwningThread = (DWORD)pthread_mach_thread_np(pthread_self());
}
static inline void LeaveCriticalSection(CRITICAL_SECTION *cs) {
    if (!--cs->depth) cs->OwningThread = 0;
    pthread_mutex_unlock(&cs->m);
}
static inline BOOL TryEnterCriticalSection(CRITICAL_SECTION *cs) {
    if (pthread_mutex_trylock(&cs->m)) return FALSE;
    if (!cs->depth++) cs->OwningThread = (DWORD)pthread_mach_thread_np(pthread_self());
    return TRUE;
}
static inline void DeleteCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_destroy(&cs->m); }
#else
/* Recursive, as on Windows. OwningThread is glibc's __owner field of the mutex (the holder's
 * thread id, 0 when free): read only, for freeze reports. */
typedef union CRITICAL_SECTION {
    pthread_mutex_t m;
    struct { int glibc_lock_, glibc_count_, OwningThread; };
} CRITICAL_SECTION, *LPCRITICAL_SECTION;
void InitializeCriticalSection(CRITICAL_SECTION *cs);
static inline BOOL InitializeCriticalSectionAndSpinCount(CRITICAL_SECTION *cs, DWORD spin) { (void)spin; InitializeCriticalSection(cs); return TRUE; }
static inline void EnterCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_lock(&cs->m); }
static inline void LeaveCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_unlock(&cs->m); }
static inline BOOL TryEnterCriticalSection(CRITICAL_SECTION *cs) { return pthread_mutex_trylock(&cs->m) == 0; }
static inline void DeleteCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_destroy(&cs->m); }
#endif

/* A condition variable is a futex sequence number: works with any lock, wakes spuriously at
 * worst - as Windows' may. */
typedef struct CONDITION_VARIABLE { volatile int seq; } CONDITION_VARIABLE, *PCONDITION_VARIABLE;
#define CONDITION_VARIABLE_INIT {0}
#define CONDITION_VARIABLE_LOCKMODE_SHARED 0x1
static inline void InitializeConditionVariable(CONDITION_VARIABLE *cv) { cv->seq = 0; }
void WakeConditionVariable(CONDITION_VARIABLE *cv);
void WakeAllConditionVariable(CONDITION_VARIABLE *cv);
BOOL SleepConditionVariableCS(CONDITION_VARIABLE *cv, CRITICAL_SECTION *cs, DWORD ms);
BOOL SleepConditionVariableSRW(CONDITION_VARIABLE *cv, SRWLOCK *l, DWORD ms, ULONG flags);

typedef struct INIT_ONCE { volatile int done; pthread_mutex_t m; } INIT_ONCE, *PINIT_ONCE, *LPINIT_ONCE;
#define INIT_ONCE_STATIC_INIT {0, PTHREAD_MUTEX_INITIALIZER}
typedef BOOL (CALLBACK *PINIT_ONCE_FN)(PINIT_ONCE once, PVOID param, PVOID *context);
BOOL InitOnceExecuteOnce(PINIT_ONCE once, PINIT_ONCE_FN fn, PVOID param, LPVOID *context);
BOOL InitOnceBeginInitialize(LPINIT_ONCE once, DWORD flags, PBOOL pending, LPVOID *context);
BOOL InitOnceComplete(LPINIT_ONCE once, DWORD flags, LPVOID context);

/* ---- handles: threads, events, files ----------------------------------------------------- */
typedef DWORD (WINAPI *LPTHREAD_START_ROUTINE)(LPVOID param);
typedef DWORD (WINAPI *PTHREAD_START_ROUTINE)(LPVOID param);
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x00010000
#define CREATE_SUSPENDED 0x4
#define THREAD_PRIORITY_LOWEST (-2)
#define THREAD_PRIORITY_BELOW_NORMAL (-1)
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_TIME_CRITICAL 15
HANDLE CreateThread(void *security, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID param, DWORD flags, DWORD *id);
__attribute__((noreturn)) void ExitThread(DWORD code);
HANDLE GetCurrentThread(void);
HANDLE GetCurrentProcess(void);
DWORD GetCurrentThreadId(void);
DWORD GetCurrentProcessId(void);
DWORD GetThreadId(HANDLE thread);
BOOL DuplicateHandle(HANDLE src_process, HANDLE src, HANDLE dst_process, HANDLE *dst, DWORD access, BOOL inherit, DWORD options);
HRESULT SetThreadDescription(HANDLE thread, PCWSTR description);
BOOL SetThreadPriority(HANDLE thread, int priority);
BOOL SetThreadStackGuarantee(PULONG size);
BOOL SwitchToThread(void);
void Sleep(DWORD ms);
DWORD WaitForSingleObject(HANDLE h, DWORD ms);
BOOL CloseHandle(HANDLE h);
HANDLE CreateEventA(void *security, BOOL manual_reset, BOOL initial, const char *name);
HANDLE CreateEventW(void *security, BOOL manual_reset, BOOL initial, const WCHAR *name);
BOOL SetEvent(HANDLE h);
BOOL ResetEvent(HANDLE h);
/* Waitable timers: one-shot, reset by the wait that sees them due (one waiting thread at a time:
 * the one that sets it, as the runtime uses them). */
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2
#define TIMER_ALL_ACCESS 0x1F0003
typedef void (CALLBACK *PTIMERAPCROUTINE)(LPVOID arg, DWORD low, DWORD high);
HANDLE CreateWaitableTimerExW(void *security, LPCWSTR name, DWORD flags, DWORD access);
BOOL SetWaitableTimer(HANDLE timer, const LARGE_INTEGER *due, LONG period, PTIMERAPCROUTINE fn, LPVOID arg, BOOL resume);
/* Linux: the thread behind a handle (0 if none), and a thread's full description (pthread names
 * are cut at 15 characters). A thread id is the kernel's (gettid), as GetCurrentThreadId's; on
 * macOS it is the thread's Mach port, which thread_suspend and task_threads use too. */
#ifdef __APPLE__
typedef unsigned int RtTid;   /* (mach_port_t) */
#else
typedef pid_t RtTid;
#endif
RtTid rt_thread_tid(HANDLE thread);
pthread_t rt_thread_pthread(HANDLE thread);
const char *rt_thread_description(RtTid tid);

/* ---- fibers ------------------------------------------------------------------------------- */
typedef void (WINAPI *LPFIBER_START_ROUTINE)(LPVOID param);
#define FIBER_FLAG_FLOAT_SWITCH 0x1
LPVOID ConvertThreadToFiber(LPVOID param);
LPVOID ConvertThreadToFiberEx(LPVOID param, DWORD flags);
LPVOID CreateFiber(SIZE_T stack, LPFIBER_START_ROUTINE fn, LPVOID param);
LPVOID CreateFiberEx(SIZE_T commit, SIZE_T reserve, DWORD flags, LPFIBER_START_ROUTINE fn, LPVOID param);
void SwitchToFiber(LPVOID fiber);
void DeleteFiber(LPVOID fiber);
BOOL IsThreadAFiber(void);
PVOID GetFiberData(void);
PVOID GetCurrentFiber(void);

/* ---- time --------------------------------------------------------------------------------- */
typedef struct SYSTEMTIME { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME, *LPSYSTEMTIME;
typedef struct FILETIME { DWORD dwLowDateTime, dwHighDateTime; } FILETIME, *LPFILETIME;
BOOL QueryPerformanceCounter(LARGE_INTEGER *t);
BOOL QueryPerformanceFrequency(LARGE_INTEGER *f);
ULONGLONG GetTickCount64(void);
DWORD GetTickCount(void);
void GetLocalTime(SYSTEMTIME *t);
void GetSystemTime(SYSTEMTIME *t);
void GetSystemTimeAsFileTime(FILETIME *t);
static inline MMRESULT timeBeginPeriod(UINT ms) { (void)ms; return 0; }
static inline MMRESULT timeEndPeriod(UINT ms) { (void)ms; return 0; }
static inline errno_t localtime_s(struct tm *out, const time_t *t) { return localtime_r(t, out) ? 0 : errno; }
static inline errno_t gmtime_s(struct tm *out, const time_t *t) { return gmtime_r(t, out) ? 0 : errno; }
static inline errno_t _get_timezone(long *seconds_west) { tzset(); *seconds_west = timezone; return 0; }   /* (standard time, as MSVC's) */

/* ---- memory ------------------------------------------------------------------------------- */
#define MEM_COMMIT 0x00001000
#define MEM_RESERVE 0x00002000
#define MEM_DECOMMIT 0x00004000
#define MEM_RELEASE 0x00008000
#define MEM_FREE 0x00010000
#define MEM_PRIVATE 0x00020000
#define MEM_WRITE_WATCH 0x00200000
#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define PAGE_EXECUTE 0x10
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_GUARD 0x100
#define WRITE_WATCH_FLAG_RESET 0x01
typedef struct MEMORY_BASIC_INFORMATION {
    PVOID BaseAddress, AllocationBase;
    DWORD AllocationProtect;
    SIZE_T RegionSize;
    DWORD State, Protect, Type;
} MEMORY_BASIC_INFORMATION, *PMEMORY_BASIC_INFORMATION;
LPVOID VirtualAlloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect);
BOOL VirtualFree(LPVOID address, SIZE_T size, DWORD type);
BOOL VirtualProtect(LPVOID address, SIZE_T size, DWORD protect, PDWORD old);
SIZE_T VirtualQuery(LPCVOID address, MEMORY_BASIC_INFORMATION *info, SIZE_T length);
UINT GetWriteWatch(DWORD flags, PVOID base, SIZE_T size, PVOID *addresses, ULONG_PTR *count, ULONG *granularity);
UINT ResetWriteWatch(LPVOID base, SIZE_T size);
/* Linux: let a system call write [p, p + n) of watched memory (only the fallback watch, which
 * protects pages, needs this; system calls fail rather than fault on protected pages). */
void rt_ww_prepare_write(void *p, size_t n);
const char *rt_ww_mode(void);
#ifdef __APPLE__
/* macOS (runtime/macos/memory.c): the guest's part of the address space, [RT_MEMBASE,
 * RT_MEMBASE + 2^39) with an inaccessible moat each side, reserved before anything else takes
 * any of it - first thing in main. 1 when done (or already), 0 (and the reason on stderr) if
 * the system holds some of it. */
int rt_guest_window(void);
#endif
typedef struct SYSTEM_INFO {
    WORD wProcessorArchitecture, wReserved;
    DWORD dwPageSize;
    LPVOID lpMinimumApplicationAddress, lpMaximumApplicationAddress;
    DWORD_PTR dwActiveProcessorMask;
    DWORD dwNumberOfProcessors, dwProcessorType, dwAllocationGranularity;
    WORD wProcessorLevel, wProcessorRevision;
} SYSTEM_INFO, *LPSYSTEM_INFO;
void GetSystemInfo(SYSTEM_INFO *info);
static inline void *_aligned_malloc(size_t size, size_t align) {
    void *p = NULL;
    return posix_memalign(&p, align < sizeof(void *) ? sizeof(void *) : align, size ? size : 1) ? NULL : p;
}
static inline void _aligned_free(void *p) { free(p); }

/* ---- files -------------------------------------------------------------------------------- */
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 0x1
#define FILE_SHARE_WRITE 0x2
#define FILE_SHARE_DELETE 0x4
#define FILE_READ_ATTRIBUTES 0x80
#define FILE_WRITE_ATTRIBUTES 0x100
#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5
#define FILE_ATTRIBUTE_READONLY 0x1
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FILE_FLAG_RANDOM_ACCESS 0x10000000
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define INVALID_FILE_SIZE ((DWORD)0xFFFFFFFF)
#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2
#define MOVEFILE_REPLACE_EXISTING 0x1   /* (rename always replaces) */
#define MOVEFILE_WRITE_THROUGH 0x8      /* the move is on the disk when MoveFileExA returns */
typedef struct OVERLAPPED {
    ULONG_PTR Internal, InternalHigh;
    union { struct { DWORD Offset, OffsetHigh; }; PVOID Pointer; };
    HANDLE hEvent;
} OVERLAPPED, *LPOVERLAPPED;
typedef struct WIN32_FIND_DATAA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    CHAR cFileName[MAX_PATH];
    CHAR cAlternateFileName[14];
} WIN32_FIND_DATAA, *LPWIN32_FIND_DATAA;
HANDLE CreateFileA(LPCSTR path, DWORD access, DWORD share, void *security, DWORD disposition, DWORD flags, HANDLE template_file);
BOOL ReadFile(HANDLE h, LPVOID buffer, DWORD size, LPDWORD read, LPOVERLAPPED overlapped);
BOOL WriteFile(HANDLE h, LPCVOID buffer, DWORD size, LPDWORD written, LPOVERLAPPED overlapped);
BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER *size);
BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER distance, LARGE_INTEGER *new_position, DWORD method);
BOOL SetEndOfFile(HANDLE h);
BOOL FlushFileBuffers(HANDLE h);
BOOL GetFileTime(HANDLE h, FILETIME *created, FILETIME *accessed, FILETIME *written);
BOOL SetFileTime(HANDLE h, const FILETIME *created, const FILETIME *accessed, const FILETIME *written);
BOOL DeleteFileA(LPCSTR path);
BOOL CreateDirectoryA(LPCSTR path, void *security);
BOOL RemoveDirectoryA(LPCSTR path);
BOOL MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags);
DWORD GetFileAttributesA(LPCSTR path);
HANDLE FindFirstFileA(LPCSTR pattern, WIN32_FIND_DATAA *data);
BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *data);
BOOL FindClose(HANDLE h);
int rt_fd_of(HANDLE h);   /* Linux: the descriptor behind a file handle, or -1 */
/* A Windows path for Linux: backslashes become slashes (into buf, which it returns). */
const char *rt_unix_path(const char *path, char *buf, size_t n);

/* ---- the C library's Windows names -------------------------------------------------------- */
#define _strdup strdup
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define _snprintf snprintf
#define _vsnprintf vsnprintf
#define _fseeki64 fseeko
#define _ftelli64 ftello
#define _byteswap_ushort __builtin_bswap16
#define _byteswap_ulong __builtin_bswap32
#define _byteswap_uint64 __builtin_bswap64
#define _rotl(v, n) ((unsigned)(((unsigned)(v) << ((n) & 31)) | ((unsigned)(v) >> ((32 - (n)) & 31))))
#define _rotr(v, n) ((unsigned)(((unsigned)(v) >> ((n) & 31)) | ((unsigned)(v) << ((32 - (n)) & 31))))
int _mkdir(const char *path);
errno_t rand_s(unsigned int *value);

/* ---- the process -------------------------------------------------------------------------- */
__attribute__((noreturn)) void ExitProcess(UINT code);
HMODULE GetModuleHandleW(LPCWSTR name);
HMODULE GetModuleHandleA(LPCSTR name);
USHORT CaptureStackBackTrace(ULONG skip, ULONG count, PVOID *frames, PULONG hash);
void OutputDebugStringA(LPCSTR text);
BOOL IsDebuggerPresent(void);   /* (a tracer is attached: /proc/self/status; macOS: P_TRACED) */
#ifdef __APPLE__
/* macOS (runtime/macos/platform.c): a thread's frame-pointer chain from its registers (pc
 * first, then each return address; lr when the innermost function has not saved it yet), read
 * without ever faulting - for a stopped thread, a fault's context or a fiber's stack alike;
 * another thread's, stopped for it (by its thread id; its pc and lr too, when asked; 0 frames
 * if it could not be stopped); and the program's own path (resolved). */
int rt_frame_walk(uint64_t pc, uint64_t lr, uint64_t fp, void **frames, int max);
int rt_thread_frames(unsigned thread_id, void **frames, int max, uint64_t *pc, uint64_t *lr);
const char *rt_exe_path(void);
#endif
typedef struct _EXCEPTION_POINTERS EXCEPTION_POINTERS, *PEXCEPTION_POINTERS;   /* (only pointed to: no SEH on Linux) */
/* The game window's keyboard and focus, from the window system (runtime/linux/window.c;
 * macOS: runtime/macos/window.m). */
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_F1 0x70
#define VK_F6 0x75
SHORT GetAsyncKeyState(int vk);
HWND GetForegroundWindow(void);

#ifdef __cplusplus
}
#endif
