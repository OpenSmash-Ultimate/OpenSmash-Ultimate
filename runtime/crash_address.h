/* (v0.5.5, Windows) A crash at an address no loaded module holds - a return into a hook whose DLL was unloaded, code a
 * tool wrote into the process, a pointer into freed memory: the crash report (runtime/host.c crash_report) said
 * "? +0x7ff9..." under "host pc" and nothing that names it. crash_unknown_address prints, there, what VirtualQuery says
 * of that memory (free, reserved, committed; image, mapped or private; its protection; the file of an image or a
 * mapping still there) and the DLLs the process unloaded so far (RtlGetUnloadEventTraceEx: ntdll's ring of the last
 * 64 unloads), marking the one the faulting pc, the address it touched or a word of the stack shown falls in.
 * Lines go to CRASH_OUT(fmt, ...), which the includer defines (host.c: out, its report and stderr). Reads ntdll's ring
 * under __try: the caller keeps its own fault handler out of it (host.c: rt_probing). SSBU_CRASH_UNLOADED=0 (host.c):
 * none of this, as before. A header so that tests/test_crash_address.py can run it on a DLL it loads and unloads. */
#pragma once
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <psapi.h>

typedef unsigned long long CrashAddr;

static int crash_in_module(CrashAddr a) {
    HMODULE m;
    return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)a, &m);
}

static void crash_region_line(const char *what, CrashAddr a) {
    MEMORY_BASIC_INFORMATION m;
    if (!VirtualQuery((void *)(uintptr_t)a, &m, sizeof m)) {
        CRASH_OUT("  %s %#llx: not in the address space (VirtualQuery: error %lu)\n", what, a, GetLastError());
        return;
    }
    char file[MAX_PATH] = "", f[MAX_PATH];
    if (m.State != MEM_FREE && (m.Type == MEM_IMAGE || m.Type == MEM_MAPPED) && K32GetMappedFileNameA(GetCurrentProcess(), (void *)(uintptr_t)a, f, sizeof f))
        snprintf(file, sizeof file, ", file %s", strrchr(f, '\\') ? strrchr(f, '\\') + 1 : f);
    CRASH_OUT("  %s %#llx: %s%s, region %#llx +%#llx (allocated at %#llx), protection %#lx%s\n", what, a,
              m.State == MEM_FREE ? "free" : m.State == MEM_RESERVE ? "reserved" : "committed",
              m.State == MEM_FREE ? "" : m.Type == MEM_IMAGE ? " image" : m.Type == MEM_MAPPED ? " mapped" : " private",
              (CrashAddr)(uintptr_t)m.BaseAddress, (CrashAddr)m.RegionSize, (CrashAddr)(uintptr_t)m.AllocationBase,
              m.State == MEM_COMMIT ? m.Protect : 0ul, file);
}

typedef struct {   /* (the start of RTL_UNLOAD_EVENT_TRACE; an element is as long as the call says) */
    void *base;
    SIZE_T size;
    ULONG sequence, stamp, checksum;
    WCHAR name[32];
} CrashUnloadEvent;
typedef void(NTAPI *CrashUnloadTraceFn)(PULONG *element_size, PULONG *element_count, PVOID *trace);

#define CRASH_UNLOAD_MAX 128
/* pc: the faulting pc when no module holds it (else 0); data: the address an access touched when no module holds it
 * and it is not the game's (else 0); stack_lo..stack_hi: the stack words shown (stack_lo 0: none readable). With pc,
 * every unloaded module; with data alone, only one that held it or a stack word (a dangling heap pointer is common:
 * its one region line, no list). */
static void crash_unknown_address(CrashAddr pc, CrashAddr data, CrashAddr stack_lo, CrashAddr stack_hi) {
    if (pc) crash_region_line("the faulting pc", pc);
    if (data) crash_region_line("the address touched", data);
    CrashUnloadTraceFn trace_fn = (CrashUnloadTraceFn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetUnloadEventTraceEx");
    if (!trace_fn) { CRASH_OUT("unloaded modules: not known here (no RtlGetUnloadEventTraceEx)\n"); return; }
    PULONG psize = NULL, pcount = NULL;
    PVOID trace = NULL;
    trace_fn(&psize, &pcount, &trace);
    volatile int shown = 0, kept = 0;
    __try {   /* (ntdll's own memory, read in place: a ring of elements, the empty ones without a base) */
        const unsigned char *ring = trace ? *(const unsigned char *const *)trace : NULL;
        ULONG size = psize ? *psize : 0, count = pcount ? *pcount : 0;
        if (ring && size >= sizeof(CrashUnloadEvent) && count <= CRASH_UNLOAD_MAX) {
#define CRASH_EV(i) ((const CrashUnloadEvent *)(ring + (size_t)(i) * size))
            ULONG order[CRASH_UNLOAD_MAX];
            int n = 0;
            for (ULONG i = 0; i < count; i++) if (CRASH_EV(i)->base) order[n++] = i;
            for (int i = 1; i < n; i++)   /* (oldest first) */
                for (int j = i; j > 0 && CRASH_EV(order[j])->sequence < CRASH_EV(order[j - 1])->sequence; j--) {
                    ULONG t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
                }
            kept = n;
            /* (with the pc in no module, the whole list; else, for an address touched, only an unloaded module it is in) */
            if (pc) CRASH_OUT(n ? "unloaded modules (oldest first, the system keeps the last %lu):\n" : "unloaded modules: none\n", count);
            for (int i = 0; i < n; i++) {
                const CrashUnloadEvent *u = CRASH_EV(order[i]);
                CrashAddr b = (CrashAddr)(uintptr_t)u->base, end = b + u->size;
                char marks[200] = "";
                if (pc >= b && pc < end) snprintf(marks + strlen(marks), sizeof marks - strlen(marks), " <- the faulting pc (+%#llx)", pc - b);
                if (data >= b && data < end) snprintf(marks + strlen(marks), sizeof marks - strlen(marks), " <- the address touched (+%#llx)", data - b);
                if (stack_lo)
                    for (CrashAddr a = stack_lo; a < stack_hi; a += 8) {
                        CrashAddr w = *(const CrashAddr *)(uintptr_t)a;
                        if (w >= b && w < end) {
                            snprintf(marks + strlen(marks), sizeof marks - strlen(marks), " <- a stack word (%#llx: +%#llx)", w, w - b);
                            break;
                        }
                    }
                if (!pc && !marks[0]) continue;
                CRASH_OUT("  %s%.32ls %#llx-%#llx (time stamp %#lx, unload %lu)%s\n", pc ? "" : "unloaded module ", u->name, b, end, u->stamp, u->sequence, marks);
                shown++;
            }
#undef CRASH_EV
        } else CRASH_OUT("unloaded modules: not readable (%lu of %lu bytes)\n", count, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        CRASH_OUT("  (the unloaded modules' list could not be read: %d of %d shown)\n", shown, kept);
    }
}
