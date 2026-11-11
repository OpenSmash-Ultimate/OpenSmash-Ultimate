/* The sampling profiler behind F6, on macOS (debug_profile.c is the Windows one and
 * linux/profile.c Linux's; this writes the same report). For a few seconds, every millisecond,
 * each of the process's threads is stopped, its registers read and (while busy) its stack
 * followed by its frame pointers, and let go on - the Windows design, with no signal
 * (rt_thread_frames); then the busiest functions (named from ssbu.syms, which the build writes
 * beside the app), modules and threads go to build/profile_<time>.txt, and its top lines to the
 * log. A thread stopped just after a system call instruction (svc) counts as waiting.
 * Also, the same way: every thread's stack for freeze reports (profile_dump_threads) and the
 * hitch log (SSBU_HITCH=<ms>: what made each frame over that long late). */
#include <errno.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <windows.h>

#include "../debug_profile.h"

typedef unsigned long long U64;
typedef unsigned U32;

#define SLOTS (1u << 20)
typedef struct Sample { U64 pc; U32 count, name, thread; } Sample;
static Sample *table;
static U32 distinct;
static volatile LONG running;
static double duration;

typedef struct ThreadStat { RtTid tid; U32 samples, waiting, reported; char desc[64]; } ThreadStat;
static ThreadStat threads[512];
static U32 nthreads;

/* The loaded images' code (their __TEXT), sorted by address: the program's own, the system's. */
#define MODULES 2048
typedef struct Module { U64 base, size, slide; char name[64]; } Module;
static Module modules[MODULES];
static U32 nmodules, exe_module;

#define STACK_DEPTH 24
#define MAX_STACKS 65536
typedef struct Stack { U32 thread, depth, waiting; U64 pc[STACK_DEPTH]; } Stack;
static Stack *stacks;
static U32 nstacks;

static U64 *sym_va;
static U32 *sym_name, nsyms;
static char *sym_pool;

int profile_running(void) { return running != 0; }

static void add(U64 pc, U32 thread) {
    U32 h = (U32)(((pc ^ (U64)thread << 52) * 0x9E3779B97F4A7C15ull) >> 44) & (SLOTS - 1);
    while (table[h].pc && (table[h].pc != pc || table[h].thread != thread)) h = (h + 1) & (SLOTS - 1);
    if (!table[h].pc) { if (distinct >= SLOTS / 2) return; table[h].pc = pc; table[h].thread = thread; distinct++; }
    table[h].count++;
}

static int module_of(U64 pc) {   /* (a search: hundreds of images) */
    U32 lo = 0, hi = nmodules;
    while (lo < hi) {
        U32 mid = (lo + hi) / 2;
        if (modules[mid].base <= pc) lo = mid + 1; else hi = mid;
    }
    return lo && pc - modules[lo - 1].base < modules[lo - 1].size ? (int)lo - 1 : -1;
}

static int by_base(const void *a, const void *b) {
    U64 x = ((const Module *)a)->base, y = ((const Module *)b)->base;
    return x < y ? -1 : x > y;
}

static void list_modules(void) {
    nmodules = 0;
    U32 count = _dyld_image_count();
    for (U32 i = 0; i < count && nmodules < MODULES; i++) {
        const struct mach_header_64 *mh = (const struct mach_header_64 *)_dyld_get_image_header(i);
        if (!mh || mh->magic != MH_MAGIC_64) continue;
        U64 slide = (U64)_dyld_get_image_vmaddr_slide(i), lo = 0, size = 0;
        const struct load_command *lc = (const struct load_command *)(mh + 1);
        for (U32 k = 0; k < mh->ncmds; k++, lc = (const struct load_command *)((const char *)lc + lc->cmdsize))
            if (lc->cmd == LC_SEGMENT_64 && !strcmp(((const struct segment_command_64 *)lc)->segname, "__TEXT")) {
                lo = ((const struct segment_command_64 *)lc)->vmaddr + slide;
                size = ((const struct segment_command_64 *)lc)->vmsize;
            }
        if (!size) continue;
        Module *m = &modules[nmodules++];
        m->base = lo;
        m->size = size;
        m->slide = slide;
        const char *n = _dyld_get_image_name(i), *slash = n ? strrchr(n, '/') : NULL;
        snprintf(m->name, sizeof m->name, "%s", slash ? slash + 1 : n ? n : "?");
    }
    qsort(modules, nmodules, sizeof *modules, by_base);
    int m = module_of((U64)(uintptr_t)GetModuleHandleW(NULL));
    exe_module = m >= 0 ? (U32)m : 0;
}

/* ssbu.syms: "<address> <type> <name>" lines, sorted (nm -n), addresses as linked (the program
 * is loaded slid: its slide added) and C names with Mach-O's leading underscore. Beside the
 * program, or beside its app (the build's: build-mac/ssbu.syms), or in its Resources. */
static FILE *open_symbols(void) {
    char path[4096], *slash;
    snprintf(path, sizeof path - 32, "%s", rt_exe_path());
    if (!(slash = strrchr(path, '/'))) return NULL;
    strcpy(slash + 1, "ssbu.syms");
    FILE *f = fopen(path, "r");
    if (f) return f;
    *slash = 0;   /* <app>/Contents/MacOS */
    if ((slash = strrchr(path, '/')) && !strcmp(slash, "/MacOS")) {
        strcpy(slash, "/Resources/ssbu.syms");
        if ((f = fopen(path, "r"))) return f;
        *slash = 0;   /* <app>/Contents */
        if ((slash = strrchr(path, '/'))) *slash = 0;   /* <app> */
        if ((slash = strrchr(path, '/'))) {
            strcpy(slash + 1, "ssbu.syms");
            f = fopen(path, "r");
        }
    }
    return f;
}

static int load_symbols(void) {
    if (sym_va) return 1;
    FILE *map = open_symbols();
    if (!map) return 0;
    U32 cap = 1 << 18;
    size_t pool_used = 0, pool_cap = 1 << 22;
    sym_va = malloc(sizeof *sym_va * cap);
    sym_name = malloc(sizeof *sym_name * cap);
    sym_pool = malloc(pool_cap);
    char line[512];
    while (fgets(line, sizeof line, map)) {
        U64 va;
        char type, name[256];
        if (sscanf(line, "%llx %c %255s", &va, &type, name) != 3 || (type != 'T' && type != 't')) continue;
        if (nsyms && va < sym_va[nsyms - 1]) continue;
        if (nsyms == cap) {
            cap *= 2;
            sym_va = realloc(sym_va, sizeof *sym_va * cap);
            sym_name = realloc(sym_name, sizeof *sym_name * cap);
        }
        const char *plain = name[0] == '_' ? name + 1 : name;
        size_t len = strlen(plain) + 1;
        if (pool_used + len > pool_cap) { pool_cap *= 2; sym_pool = realloc(sym_pool, pool_cap); }
        memcpy(sym_pool + pool_used, plain, len);
        sym_va[nsyms] = va;
        sym_name[nsyms++] = (U32)pool_used;
        pool_used += len;
    }
    fclose(map);
    return 1;
}

static U32 symbol_index(U64 pc) {   /* the program's function at pc: its index + 1, or 0 */
    if (!nsyms || module_of(pc) != (int)exe_module) return 0;
    U64 va = pc - modules[exe_module].slide;
    if (va < sym_va[0]) return 0;
    U32 lo = 0, hi = nsyms;
    while (hi - lo > 1) {
        U32 mid = (lo + hi) / 2;
        if (sym_va[mid] <= va) lo = mid; else hi = mid;
    }
    return lo + 1;
}

static U32 symbol_of(U64 pc) {
    U32 i = symbol_index(pc);
    return i ? sym_name[i - 1] + 1 : 0;
}

static U64 symbol_start(U64 pc) {   /* the start of the program's function at pc, or 0 */
    U32 i = symbol_index(pc);
    return i ? sym_va[i - 1] + modules[exe_module].slide : 0;
}

static void thread_name(RtTid tid, char *out, size_t n) {   /* its description, or the system's name of it */
    const char *d = rt_thread_description(tid);
    if (d && *d) { snprintf(out, n, "%s", d); return; }
    thread_extended_info_data_t info;
    mach_msg_type_number_t count = THREAD_EXTENDED_INFO_COUNT;
    if (thread_info((thread_act_t)tid, THREAD_EXTENDED_INFO, (thread_info_t)&info, &count) == KERN_SUCCESS) snprintf(out, n, "%s", info.pth_name);
    else if (n) out[0] = 0;
}

static ThreadStat *thread_stat(RtTid tid) {
    for (U32 i = 0; i < nthreads; i++) if (threads[i].tid == tid) return &threads[i];
    if (nthreads == 512) return NULL;
    threads[nthreads] = (ThreadStat){.tid = tid};
    thread_name(tid, threads[nthreads].desc, sizeof threads[nthreads].desc);
    return &threads[nthreads++];
}

/* The process's threads but this one, as their ids (Mach ports: a send right each, for
 * release_threads). */
static U32 list_threads(RtTid *out, U32 max) {
    thread_act_array_t list;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &list, &count) != KERN_SUCCESS) return 0;
    RtTid self = pthread_mach_thread_np(pthread_self());
    U32 n = 0;
    for (mach_msg_type_number_t i = 0; i < count; i++) {
        if (list[i] != self && n < max) out[n++] = list[i];
        else mach_port_deallocate(mach_task_self(), list[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)list, sizeof *list * count);
    return n;
}

static void release_threads(RtTid *tids, U32 n) {
    for (U32 i = 0; i < n; i++) mach_port_deallocate(mach_task_self(), tids[i]);
}

/* ---- the report (as debug_profile.c's) ----------------------------------------------------- */
typedef struct Entry { const char *name; U64 pc; U32 count; } Entry;
static int by_count(const void *a, const void *b) { return (int)((const Entry *)b)->count - (int)((const Entry *)a)->count; }
static int by_pc(const void *a, const void *b) {
    U64 x = ((const Sample *)a)->pc, y = ((const Sample *)b)->pc;
    return x < y ? -1 : x > y;
}

static void busiest(FILE *f, const Sample *s, U32 n, const char *pool, Entry *e, int thread, U64 busy, U32 limit) {
    U32 ne = 0;
    for (U32 i = 0; i < n; i++) {
        if (thread >= 0 && s[i].thread != (U32)thread) continue;
        const char *name = s[i].name ? pool + s[i].name - 1 : NULL;
        if (name && ne && e[ne - 1].name && !strcmp(e[ne - 1].name, name)) { e[ne - 1].count += s[i].count; continue; }
        if (!name && ne && !e[ne - 1].name && e[ne - 1].pc == s[i].pc) { e[ne - 1].count += s[i].count; continue; }
        e[ne++] = (Entry){name, s[i].pc, s[i].count};
    }
    qsort(e, ne, sizeof *e, by_count);
    for (U32 i = 0; i < ne && i < limit; i++) {
        int m = module_of(e[i].pc);
        if (e[i].name) fprintf(f, "  %5.2f%%  %s\n", 100.0 * e[i].count / (busy ? busy : 1), e[i].name);
        else fprintf(f, "  %5.2f%%  %s+%#llx\n", 100.0 * e[i].count / (busy ? busy : 1), m >= 0 ? modules[m].name : "?",
                     m >= 0 ? e[i].pc - modules[m].base : e[i].pc);
    }
}

typedef struct Count { U32 key[3], count; } Count;
static int count_by_key(const void *a, const void *b) { return memcmp(((const Count *)a)->key, ((const Count *)b)->key, sizeof ((const Count *)a)->key); }
static int count_by_count(const void *a, const void *b) {
    U32 x = ((const Count *)a)->count, y = ((const Count *)b)->count;
    return x < y ? 1 : x > y ? -1 : 0;
}
static U32 frame_key(U64 pc) {
    U32 s = symbol_of(pc);
    if (s) return s;
    int m = module_of(pc);
    return m < 0 ? 0xFFFFFFFFu : 0x80000000u | (U32)m;
}
static const char *key_name(U32 key) {
    if (key == 0xFFFFFFFFu) return "?";
    if (key & 0x80000000u) return modules[key & 0x7FFFFFFFu].name;
    return sym_pool + key - 1;
}
static U32 tally(Count *c, U32 n) {
    if (!n) return 0;
    qsort(c, n, sizeof *c, count_by_key);
    U32 out = 0;
    for (U32 i = 0; i < n; i++) {
        if (out && !count_by_key(&c[out - 1], &c[i])) c[out - 1].count += c[i].count;
        else c[out++] = c[i];
    }
    qsort(c, out, sizeof *c, count_by_count);
    return out;
}

static int is_system_module(int m) {   /* the C library and the system calls under it */
    return m >= 0 && (!strncmp(modules[m].name, "libsystem_", 10) || !strcmp(modules[m].name, "libdyld.dylib") || !strcmp(modules[m].name, "dyld"));
}

static void call_paths(FILE *f) {
    U32 t = 0, t_busy = 0, n = 0;
    for (U32 i = 0; i < nthreads; i++)
        if (threads[i].samples - threads[i].waiting > t_busy) { t = i; t_busy = threads[i].samples - threads[i].waiting; }
    U32 nwait = 0;
    for (U32 i = 0; i < nstacks; i++) { n += stacks[i].thread == t && !stacks[i].waiting; nwait += stacks[i].thread == t && stacks[i].waiting; }
    if (!n) return;
    Count *c = malloc(sizeof *c * (size_t)(n + nwait) * STACK_DEPTH);
    U32 nc = 0;
    for (U32 i = 0; i < nstacks; i++) {
        if (stacks[i].thread != t || stacks[i].waiting) continue;
        U32 keys[STACK_DEPTH], nk = 0;
        for (U32 d = 0; d < stacks[i].depth; d++) {
            U32 k = frame_key(stacks[i].pc[d]), seen = 0;
            for (U32 j = 0; j < nk && !seen; j++) seen = keys[j] == k;
            if (!seen) { keys[nk++] = k; c[nc++] = (Count){{k, 0, 0}, 1}; }
        }
    }
    nc = tally(c, nc);
    fprintf(f, "\nThread %d %s: time in or under each function (%% of %u sampled stacks, at most %u frames deep):\n",
            (int)threads[t].tid, threads[t].desc, n, STACK_DEPTH);
    for (U32 i = 0; i < nc && i < 60; i++) fprintf(f, "  %5.1f%%  %s\n", 100.0 * c[i].count / n, key_name(c[i].key[0]));
    nc = 0;
    for (U32 i = 0; i < nstacks; i++)
        if (stacks[i].thread == t && !stacks[i].waiting) c[nc++] = (Count){{frame_key(stacks[i].pc[0]), 0, 0}, 1};
    nc = tally(c, nc);
    U32 top = nc < 12 ? nc : 12;
    Count *callee = malloc(sizeof *callee * (top ? top : 1)), *paths = c + nc;
    memcpy(callee, c, sizeof *callee * top);
    fprintf(f, "\nThread %d: how it reaches its busiest functions (%% of that function's samples):\n", (int)threads[t].tid);
    for (U32 j = 0; j < top; j++) {
        U32 np = 0;
        for (U32 i = 0; i < nstacks; i++) {
            const Stack *k = &stacks[i];
            if (k->thread != t || k->waiting || frame_key(k->pc[0]) != callee[j].key[0]) continue;
            Count p = {{0, 0, 0}, 1};
            for (U32 d = 1; d < 4 && d < k->depth; d++) p.key[d - 1] = frame_key(k->pc[d]);
            paths[np++] = p;
        }
        np = tally(paths, np);
        fprintf(f, "  %s (%.1f%% of the thread):\n", key_name(callee[j].key[0]), 100.0 * callee[j].count / n);
        for (U32 i = 0; i < np && i < 5; i++) {
            fprintf(f, "    %5.1f%%  <-", 100.0 * paths[i].count / callee[j].count);
            for (U32 d = 0; d < 3 && paths[i].key[d]; d++) fprintf(f, " %s%s", d ? "<- " : "", key_name(paths[i].key[d]));
            fprintf(f, "\n");
        }
    }
    /* the guest main thread's waits: from the first frame outside the C library, what it waited in */
    for (U32 m = 0; m < nthreads; m++) {
        if (!strstr(threads[m].desc, "main thread")) continue;
        U32 nw = 0;
        Count *w = malloc(sizeof *w * (nstacks ? nstacks : 1));
        for (U32 i = 0; i < nstacks; i++) {
            const Stack *k = &stacks[i];
            if (k->thread != m || !k->waiting) continue;
            U32 d = 0;
            while (d < k->depth) {
                int mod = module_of(k->pc[d]);
                if (mod >= 0 && !is_system_module(mod)) break;
                d++;
            }
            Count p = {{0, 0, 0}, 1};
            for (U32 j = 0; j < 3 && d + j < k->depth; j++) p.key[j] = frame_key(k->pc[d + j]);
            if (!p.key[0]) p.key[0] = 0xFFFFFFFFu;
            w[nw++] = p;
        }
        nw = tally(w, nw);
        fprintf(f, "\nThread %d %s: where it waits (%u of its samples were waiting; %% of those with stacks):\n",
                (int)threads[m].tid, threads[m].desc, threads[m].waiting);
        U32 sum = 0;
        for (U32 i = 0; i < nw; i++) sum += w[i].count;
        for (U32 i = 0; i < nw && i < 15; i++) {
            fprintf(f, "  %5.1f%%  in", 100.0 * w[i].count / (sum ? sum : 1));
            for (U32 d = 0; d < 3 && w[i].key[d]; d++) fprintf(f, " %s%s", d ? "<- " : "", key_name(w[i].key[d]));
            fprintf(f, "\n");
        }
        free(w);
    }
    free(callee);
    free(c);
}

static void report(U64 total, U64 busy) {
    char path[160];
    SYSTEMTIME st;
    GetLocalTime(&st);
    struct stat sb;
    snprintf(path, sizeof path, "%sprofile_%04u%02u%02u_%02u%02u%02u.txt", !stat("build", &sb) && S_ISDIR(sb.st_mode) ? "build/" : "",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "[profile] cannot write %s (errno %d)\n", path, errno); f = stderr; }
    fprintf(f, "Sampling profile: %.0f s, %llu samples (%llu busy, %llu in system calls - mostly waiting)\n\n", duration, total, busy, total - busy);
    fprintf(f, "Threads (samples, busy %%):\n");
    for (U32 i = 0; i < nthreads; i++)
        if (threads[i].samples > threads[i].waiting)
            fprintf(f, "  %6d %-32s %6u samples, %5.1f%% busy\n", (int)threads[i].tid, threads[i].desc, threads[i].samples,
                    100.0 * (threads[i].samples - threads[i].waiting) / threads[i].samples);
    Sample *s = malloc(sizeof *s * (distinct ? distinct : 1));
    U32 n = 0;
    for (U32 i = 0; i < SLOTS; i++) if (table[i].pc) s[n++] = table[i];
    qsort(s, n, sizeof *s, by_pc);
    fprintf(f, "\nBusy time by module:\n");
    U64 *per_module = calloc(nmodules + 1, sizeof *per_module);
    for (U32 i = 0; i < n; i++) {
        int m = module_of(s[i].pc);
        per_module[m < 0 ? nmodules : (U32)m] += s[i].count;
    }
    for (U32 m = 0; m <= nmodules; m++)
        if (per_module[m] * 1000 >= busy && busy)
            fprintf(f, "  %5.1f%%  %s\n", 100.0 * per_module[m] / busy, m < nmodules ? modules[m].name : "(no module)");
    if (!load_symbols()) fprintf(f, "\n(ssbu.syms not found beside the program: functions unnamed)\n");
    for (U32 i = 0; i < n; i++) s[i].name = symbol_of(s[i].pc);
    Entry *e = malloc(sizeof *e * (n ? n : 1));
    fprintf(f, "\nBusiest code (%% of busy samples):\n");
    busiest(f, s, n, sym_pool, e, -1, busy, 80);
    for (int round = 0; round < 4; round++) {
        U32 best = 0, best_busy = 0;
        for (U32 i = 0; i < nthreads; i++) {
            U32 b = threads[i].samples - threads[i].waiting;
            if (!threads[i].reported && b > best_busy) { best = i; best_busy = b; }
        }
        if (best_busy * 50 < busy) break;
        threads[best].reported = 1;
        fprintf(f, "\nThread %d %s (%.1f%% busy) - its busiest code (%% of its busy samples):\n", (int)threads[best].tid,
                threads[best].desc, 100.0 * best_busy / threads[best].samples);
        busiest(f, s, n, sym_pool, e, (int)best, best_busy, 25);
    }
    call_paths(f);
    if (f != stderr) {
        fclose(f);
        fprintf(stderr, "[profile] written to %s\n", path);
    }
    free(e); free(s); free(per_module);
}

/* ---- sampling ------------------------------------------------------------------------------
 * A shot: where a thread is, whether it is waiting in a system call, and (when asked) its stack.
 * One shot at a time in the process (rt_thread_frames), for F6, freeze reports and the hitch
 * log alike. */
typedef struct Shot { U64 pc, lr; int waiting, depth; void *frames[STACK_DEPTH + 8]; } Shot;

static int shoot(RtTid tid, int want_stack, Shot *out) {
    U64 pc = 0, lr = 0;
    int n = rt_thread_frames(tid, out->frames, want_stack ? STACK_DEPTH + 8 : 0, &pc, &lr);
    if (!n) return 0;
    out->pc = pc;
    out->lr = lr;
    out->depth = want_stack ? n : 0;
    int m = module_of(pc);   /* (the instruction before it is code of a loaded image: readable) */
    out->waiting = m >= 0 && pc - modules[m].base >= 4 && !(pc & 3) && *(const U32 *)(uintptr_t)(pc - 4) == 0xD4001001u;   /* (stopped just after `svc #0x80`) */
    return 1;
}

/* A shot's stack: each return address moved into its call instruction; the link register (in
 * the walk when the innermost function had not saved it) left out when it points back into
 * that same function. */
static int shot_stack(const Shot *s, U64 *pc, int max) {
    int n = 0, from = 1;
    if (!s->depth) { pc[0] = s->pc; return 1; }
    pc[n++] = (U64)s->frames[0];
    if (s->depth > 1 && (U64)s->frames[1] == s->lr && symbol_start(s->lr) && symbol_start(s->lr) == symbol_start(s->pc)) from = 2;
    for (int j = from; j < s->depth && n < max; j++) pc[n++] = (U64)s->frames[j] - 4;
    return n;
}

static DWORD WINAPI sampler(void *arg) {
    (void)arg;
    list_modules();
    load_symbols();
    memset(table, 0, sizeof *table * SLOTS);
    distinct = nstacks = nthreads = 0;
    RtTid tids[512];
    U32 nt = 0;
    U64 total = 0, busy = 0, t0 = GetTickCount64(), refreshed = 0;
    for (;;) {
        U64 now = GetTickCount64();
        if (now - t0 >= (U64)(duration * 1000)) break;
        if (!nt || now - refreshed > 250) {   /* threads come and go */
            release_threads(tids, nt);
            nt = list_threads(tids, 512);
            refreshed = now;
        }
        for (U32 i = 0; i < nt; i++) {
            ThreadStat *ts = thread_stat(tids[i]);
            Shot s;
            if (!shoot(tids[i], ts && nstacks < MAX_STACKS, &s)) continue;
            total++;
            busy += !s.waiting;
            if (!s.waiting) add(s.pc, ts ? (U32)(ts - threads) : 511);   /* (the busiest code is busy code: as Windows', which leaves the system-call stubs out) */
            if (ts) {
                ts->samples++;
                ts->waiting += s.waiting;
                if ((!s.waiting || strstr(ts->desc, "main thread")) && s.depth && nstacks < MAX_STACKS) {
                    Stack *k = &stacks[nstacks++];
                    k->thread = (U32)(ts - threads);
                    k->waiting = s.waiting;
                    U64 pc[STACK_DEPTH + 8];
                    int n = shot_stack(&s, pc, STACK_DEPTH);
                    k->depth = (U32)n;
                    memcpy(k->pc, pc, sizeof *pc * (size_t)n);
                }
            }
        }
        Sleep(1);
    }
    release_threads(tids, nt);
    report(total, busy);
    InterlockedExchange(&running, 0);
    return 0;
}

void profile_start(double seconds) {
    if (InterlockedCompareExchange(&running, 1, 0)) return;
    if (!table) {
        table = malloc(sizeof *table * SLOTS);
        stacks = malloc(sizeof *stacks * MAX_STACKS);
    }
    duration = seconds;
    fprintf(stderr, "[profile] sampling for %.0f s\n", seconds);
    CloseHandle(CreateThread(NULL, 0, sampler, NULL, 0, NULL));
}

/* ---- thread dumps, for freeze reports (as debug_profile.c's) ------------------------------- */
const char *rt_describe_host(U64 pc, char *buf, size_t n);   /* host.c: a recompiled function's guest place */

/* A code address for a report: the guest function of recompiled code, else the program's own
 * function (ssbu.syms) or module + offset. */
static void describe_pc(U64 pc, char *buf, size_t n) {
    char guest[96];
    U32 sym = symbol_of(pc);
    int m = module_of(pc);
    if (rt_describe_host(pc, guest, sizeof guest)) snprintf(buf, n, "%s", guest);
    else if (sym) snprintf(buf, n, "%s", sym_pool + sym - 1);
    else if (m >= 0) snprintf(buf, n, "%s+%#llx", modules[m].name, pc - modules[m].base);
    else snprintf(buf, n, "%#llx", pc);
}

/* A thread's CPU time so far, in seconds. */
static double cpu_seconds(RtTid tid) {
    thread_basic_info_data_t info;
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    if (thread_info((thread_act_t)tid, THREAD_BASIC_INFO, (thread_info_t)&info, &count) != KERN_SUCCESS) return 0;
    return info.user_time.seconds + info.system_time.seconds + (info.user_time.microseconds + info.system_time.microseconds) / 1e6;
}

/* (Windows only so far: rollback's failed-hold report names the threads but shows no stacks here) */
int profile_dump_some(const unsigned long *tids, unsigned n, void (*line)(const char *text)) { (void)tids; (void)n; (void)line; return 0; }

/* Every thread's stack but the caller's, a line at a time to line. */
int profile_dump_threads(void (*line)(const char *text)) {
    if (InterlockedCompareExchange(&running, 1, 0)) { line("  (the profiler is running: no stacks)\n"); return 0; }
    list_modules();
    load_symbols();
    RtTid tids[512];
    U32 nt = list_threads(tids, 512);
    char text[400], where[160];
    for (U32 i = 0; i < nt; i++) {
        Shot s;
        U64 pc[STACK_DEPTH + 8];
        int depth = shoot(tids[i], 1, &s) ? shot_stack(&s, pc, STACK_DEPTH + 8) : 0;
        char name[64];
        thread_name(tids[i], name, sizeof name);
        snprintf(text, sizeof text, "thread %d %s (CPU %.1f s)%s\n", (int)tids[i], name, cpu_seconds(tids[i]), depth ? "" : ": no stack");
        line(text);
        for (int f = 0; f < depth; f++) {
            describe_pc(pc[f], where, sizeof where);
            snprintf(text, sizeof text, "    %s\n", where);
            line(text);
        }
    }
    release_threads(tids, nt);
    InterlockedExchange(&running, 0);
    return 1;
}

/* ---- the hitch log (SSBU_HITCH=<ms>), as debug_profile.c's ----------------------------------
 * A sampler wakes about every millisecond and notes where three threads are: the guest's main
 * thread, the vsync thread (it presents), and the thread that last entered the renderer. At each
 * present the samples since the last one are the frame's: a frame over the threshold is logged
 * with the functions each thread spent it in (a sample is about a millisecond), the process's
 * page faults and the whole system's CPU use during it; a table of the causes so far every
 * minute. Nothing runs without SSBU_HITCH. (It pauses while F6 or a freeze report samples.) */
DWORD gfx_last_enter_thread(void);   /* the renderer's */

enum { ROLE_MAIN, ROLE_VSYNC, ROLE_RENDER, ROLES };
static const char *const ROLE_NAMES[ROLES] = {"main", "vsync", "renderer"};
typedef struct HitchSample { U64 key, via; unsigned char role, waiting; } HitchSample;   /* via: see hitch_sample */
#define HITCH_RING 8192
static HitchSample hitch_ring[HITCH_RING];
static U32 hitch_head, hitch_frame_start;
static pthread_mutex_t hitch_lock = PTHREAD_MUTEX_INITIALIZER;
static int hitch_ms = -1;
typedef struct HitchCause { U64 key, via; U32 role, frames; double ms; } HitchCause;
static HitchCause hitch_causes[512];
static U32 nhitch_causes, hitch_count;

/* The sample's key: the function (its start) of the innermost frame outside the C library; and
 * when that is a framework's or library's (a wait in Metal), via: the program's own function
 * that called into it (as debug_profile.c's). */
static void hitch_sample(RtTid tid, int role) {
    Shot s;
    if (!shoot(tid, 1, &s)) return;
    U64 pc[STACK_DEPTH + 8];
    int depth = shot_stack(&s, pc, STACK_DEPTH + 8);
    HitchSample h = {0, 0, (unsigned char)role, (unsigned char)s.waiting};
    int i = 0;
    for (; i < depth; i++) {
        if (is_system_module(module_of(pc[i]))) continue;
        U64 start = symbol_start(pc[i]);
        h.key = start ? start : pc[i];
        break;
    }
    if (i < depth && module_of(pc[i]) != (int)exe_module)
        for (int j = i + 1; j < depth; j++)
            if (module_of(pc[j]) == (int)exe_module) {
                U64 start = symbol_start(pc[j]);
                h.via = start ? start : pc[j];
                break;
            }
    pthread_mutex_lock(&hitch_lock);
    hitch_ring[hitch_head++ % HITCH_RING] = h;
    pthread_mutex_unlock(&hitch_lock);
}

static DWORD WINAPI hitch_sampler(void *unused) {
    (void)unused;
    SetThreadDescription(GetCurrentThread(), L"hitch sampler");
    list_modules();
    load_symbols();
    RtTid main_tid = 0, vsync_tid = 0;
    for (U32 tick = 0;; tick++) {
        if ((!main_tid || !vsync_tid) && tick % 500 == 0) {   /* find the two named threads */
            RtTid tids[512];
            U32 nt = list_threads(tids, 512);
            for (U32 i = 0; i < nt; i++) {
                const char *desc = rt_thread_description(tids[i]);
                if (!main_tid && !strcmp(desc, "guest main thread")) main_tid = tids[i];
                else if (!vsync_tid && !strcmp(desc, "vsync")) vsync_tid = tids[i];
                else mach_port_deallocate(mach_task_self(), tids[i]);   /* (the two found keep theirs) */
            }
        }
        if (!running) {
            RtTid render_tid = (RtTid)gfx_last_enter_thread();
            if (main_tid) hitch_sample(main_tid, ROLE_MAIN);
            if (vsync_tid) hitch_sample(vsync_tid, ROLE_VSYNC);
            if (render_tid && render_tid != main_tid && render_tid != vsync_tid) hitch_sample(render_tid, ROLE_RENDER);
        }
        Sleep(1);
    }
}

static void hitch_add_cause(U64 key, U64 via, U32 role, double ms) {
    for (U32 i = 0; i < nhitch_causes; i++)
        if (hitch_causes[i].key == key && hitch_causes[i].via == via && hitch_causes[i].role == role) { hitch_causes[i].frames++; hitch_causes[i].ms += ms; return; }
    if (nhitch_causes < 512) hitch_causes[nhitch_causes++] = (HitchCause){key, via, role, 1, ms};
}

/* "function", or "library function via the program's function" */
static void describe_hitch(U64 key, U64 via, char *buf, size_t n) {
    describe_pc(key, buf, n);
    if (via) {
        size_t len = strlen(buf);
        if (len + 6 < n) { memcpy(buf + len, " via ", 5); describe_pc(via, buf + len + 5, n - len - 5); }
    }
}

static int by_cause_ms(const void *a, const void *b) {
    double x = ((const HitchCause *)a)->ms, y = ((const HitchCause *)b)->ms;
    return x < y ? 1 : x > y ? -1 : 0;
}

/* The whole system's CPU time so far, and how much of it idle (in ticks). */
static void system_times(U64 *idle, U64 *total) {
    static mach_port_t host;
    if (!host) host = mach_host_self();
    host_cpu_load_info_data_t info;
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    *idle = *total = 0;
    if (host_statistics64(host, HOST_CPU_LOAD_INFO, (host_info64_t)&info, &count) != KERN_SUCCESS) return;
    *idle = info.cpu_ticks[CPU_STATE_IDLE];
    for (int i = 0; i < CPU_STATE_MAX; i++) *total += info.cpu_ticks[i];
}

/* (the renderer, at each present) */
void hitch_present(void) {
    if (hitch_ms < 0) {
        const char *v = getenv("SSBU_HITCH");
        hitch_ms = v ? atoi(v) : 0;
        if (hitch_ms > 0) {
            fprintf(stderr, "[hitch] logging frames over %d ms\n", hitch_ms);
            CloseHandle(CreateThread(NULL, 0, hitch_sampler, NULL, 0, NULL));
        }
    }
    if (hitch_ms <= 0) return;
    static LARGE_INTEGER freq, last;
    static U64 last_faults, last_idle, last_total, frame, last_table;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    double ms = last.QuadPart ? (now.QuadPart - last.QuadPart) * 1000.0 / freq.QuadPart : 0;
    last = now;
    frame++;
    struct rusage ru;
    U64 faults = getrusage(RUSAGE_SELF, &ru) ? 0 : (U64)ru.ru_minflt + (U64)ru.ru_majflt;
    U64 idle, total;
    system_times(&idle, &total);
    pthread_mutex_lock(&hitch_lock);
    U32 from = hitch_frame_start, to = hitch_head;
    hitch_frame_start = hitch_head;
    if (ms > hitch_ms && to - from > 0) {
        if (to - from > HITCH_RING) from = to - HITCH_RING;
        /* per role, the most sampled functions of the frame */
        struct { U64 key, via; U32 n, waiting; } top[ROLES][64];
        U32 ntop[ROLES] = {0}, total_role[ROLES] = {0};
        for (U32 i = from; i != to; i++) {
            const HitchSample *s = &hitch_ring[i % HITCH_RING];
            total_role[s->role]++;
            U32 j = 0;
            for (; j < ntop[s->role] && (top[s->role][j].key != s->key || top[s->role][j].via != s->via); j++) {}
            if (j == ntop[s->role]) {
                if (j == 64) continue;
                top[s->role][j].key = s->key; top[s->role][j].via = s->via; top[s->role][j].n = top[s->role][j].waiting = 0; ntop[s->role]++;
            }
            top[s->role][j].n++;
            top[s->role][j].waiting += s->waiting;
        }
        char text[2400], where[320];
        int len = snprintf(text, sizeof text, "[hitch] frame %llu: %.1f ms; page faults +%llu; system CPU %.0f%%", frame, ms,
                           faults - last_faults, total > last_total ? 100.0 * (1.0 - (double)(idle - last_idle) / (double)(total - last_total)) : 0.0);
        for (int r = 0; r < ROLES; r++) {
            if (!total_role[r]) continue;
            len += snprintf(text + len, sizeof text - len, "\n[hitch]   %s:", ROLE_NAMES[r]);
            for (int k = 0; k < 3; k++) {   /* the three biggest */
                U32 best = 0, bn = 0;
                for (U32 j = 0; j < ntop[r]; j++) if (top[r][j].n > bn) { bn = top[r][j].n; best = j; }
                if (!bn) break;
                describe_hitch(top[r][best].key, top[r][best].via, where, sizeof where);
                double est = ms * bn / total_role[r];   /* the role's samples cover the frame */
                len += snprintf(text + len, sizeof text - len, " %.1f ms %s%s;", est, where, top[r][best].waiting * 2 > top[r][best].n ? " (waiting)" : "");
                if (k == 0) hitch_add_cause(top[r][best].key, top[r][best].via, (U32)r, est);
                top[r][best].n = 0;
            }
        }
        hitch_count++;
        fprintf(stderr, "%s\n", text);
    }
    pthread_mutex_unlock(&hitch_lock);
    last_faults = faults; last_idle = idle; last_total = total;
    if (hitch_count && frame - last_table >= 3600 && nhitch_causes) {   /* every minute: the causes so far */
        last_table = frame;
        qsort(hitch_causes, nhitch_causes, sizeof *hitch_causes, by_cause_ms);
        fprintf(stderr, "[hitch] %u frames over %d ms so far; the largest part of each, by total time:\n", hitch_count, hitch_ms);
        char where[320];
        for (U32 i = 0; i < nhitch_causes && i < 10; i++) {
            describe_hitch(hitch_causes[i].key, hitch_causes[i].via, where, sizeof where);
            fprintf(stderr, "[hitch]   %7.1f ms in %4u frames  %s: %s\n", hitch_causes[i].ms, hitch_causes[i].frames, ROLE_NAMES[hitch_causes[i].role], where);
        }
    }
}
