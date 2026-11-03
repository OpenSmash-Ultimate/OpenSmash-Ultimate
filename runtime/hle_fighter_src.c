/* Fighters as C++ source (the fighter SDK, ssbu-recomp/fighter_sdk): a fighter library replaces script functions
 * of a script module with C++ that computes the same thing (ssbu-recomp's fighter_host.c, its run mode).
 *
 * When the game loads a script module lua2cpp_<name>, <fighters folder>\<name>.dll is loaded if it is there
 * (SSBU_FIGHTER_BIN, else fighters_bin\ beside the game's folders; tools: fighter_sdk/tools/build_fighter.py).
 * Its fighter_bind(host, table) hands over the game symbols it calls (bound here by name: rt_symbol) and its
 * scripts by their offset in the module. Each script is hooked at module base + offset (rt_hook_function: the
 * dispatch slot and, for direct calls, the function's first bytes), through a small stub that tells the
 * dispatcher which library and script it is: however the game reaches the function, the source runs.
 * SSBU_FIGHTER_SRC=0: no library is loaded. ssbu-recomp's differential test (SSBU_FIGHTER_DIFF) is not here.
 *
 * The library calls back through FsHost: call(c, slot) runs the game function behind a symbol slot; ctx() is the
 * register context of the script running on this fiber; call_addr(c, address) calls through a pointer. The script
 * running on a fiber (which module, the return address it was called with) is per fiber (FLS): a script waits in
 * the game's frame()/wait() by switching fibers. A call the source makes sees the return address the source
 * function was called with, as after a tail call, so the game's unwinder walks the frames it knows: an exception
 * that ends a script (L2CExceptionShutdown, when the motion changes) passes the source frame by.
 * The Ctx the library sees is this runtime's Cpu (x[0..30], sp as x[31], v[0..31]). Windows only, as ssbu-recomp's. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"

#ifdef _WIN32
typedef struct FsHost { void (*call)(Cpu *, int); void (*log)(const char *); Cpu *(*ctx)(void); void (*call_addr)(Cpu *, u64); } FsHost;
typedef struct FsOverride { u32 off; const char *name; void (*fn)(Cpu *); } FsOverride;
typedef struct FsSym { const char *name; const char *spec; } FsSym;
typedef struct FsTable { const FsSym *syms; int sym_count; u64 *addr; const FsOverride *ovr; int ovr_count; } FsTable;
typedef void (*FsBindFn)(const FsHost *, FsTable *);

typedef struct FsMod { char name[64]; u64 base; HMODULE dll; FsTable t; u8 *stubs; } FsMod;
typedef struct FsRun { FsMod *mod; Cpu *ctx; u64 lr; } FsRun;   /* the script running on this fiber */

#define MODS 64
static FsMod *mods[MODS];
static int nmods;
static DWORD fls = FLS_OUT_OF_INDEXES;
static u64 installed[1 << 14];   /* (addresses hooked once: a module loaded again keeps its hooks) */
static int ninstalled;

static FsRun *running(void) { return fls == FLS_OUT_OF_INDEXES ? NULL : (FsRun *)FlsGetValue(fls); }

static void fs_call(Cpu *c, int slot) {
    FsRun *r = running();
    if (!r || !r->mod || slot < 0 || slot >= r->mod->t.sym_count || !r->mod->t.addr[slot]) {
        fprintf(stderr, "[fighter] a source script called symbol slot %d, which is not bound\n", slot);
        return;
    }
    c->x[30] = r->lr;   /* (not rt_call, which sets it to 0: the game's unwinder walks frames by it) */
    rt_lookup(r->mod->t.addr[slot])(c);
}
static void fs_log(const char *s) { fprintf(stderr, "[fighter] %s\n", s ? s : ""); }
static Cpu *fs_ctx(void) { FsRun *r = running(); return r ? r->ctx : NULL; }
static void fs_call_addr(Cpu *c, u64 addr) { FsRun *r = running(); c->x[30] = r ? r->lr : 0; rt_lookup(addr)(c); }
static const FsHost host = {fs_call, fs_log, fs_ctx, fs_call_addr};

/* (from a stub: rcx = the context, rdx = library << 16 | script) a script of a library runs */
static void dispatch(Cpu *c, u64 which) {
    FsMod *m = mods[which >> 16];
    const FsOverride *o = &m->t.ovr[which & 0xFFFF];
    FsRun *outer = running(), here = {m, c, c->x[30]};
    FlsSetValue(fls, &here);
    o->fn(c);
    FlsSetValue(fls, outer);
}

/* mov rdx, which; mov rax, dispatch; jmp rax (22 bytes; 32 apart) */
static GuestFn *make_stub(u8 *at, u64 which) {
    u64 target = (u64)(uintptr_t)dispatch;
    at[0] = 0x48; at[1] = 0xBA; memcpy(at + 2, &which, 8);
    at[10] = 0x48; at[11] = 0xB8; memcpy(at + 12, &target, 8);
    at[20] = 0xFF; at[21] = 0xE0;
    return (GuestFn *)(void *)at;
}

static int was_installed(u64 addr) {
    for (int i = 0; i < ninstalled; i++) if (installed[i] == addr) return 1;
    if (ninstalled < (int)(sizeof installed / sizeof *installed)) installed[ninstalled++] = addr;
    return 0;
}

static const char *folder(char *buf, size_t n) {
    const char *e = getenv("SSBU_FIGHTER_BIN");
    if (e && *e) snprintf(buf, n, "%s", e);
    else snprintf(buf, n, "%s\\..\\..\\fighters_bin", rt_data_dir ? rt_data_dir : ".");
    return buf;
}

/* (host.c, after a module's init) a script module's fighter library, if it has one */
void fighter_src_module_loaded(const char *module, u64 base) {
    const char *off = getenv("SSBU_FIGHTER_SRC");
    if ((off && off[0] == '0') || strncmp(module, "lua2cpp_", 8)) return;
    const char *name = module + 8;
    for (int i = 0; i < nmods; i++)
        if (!strcmp(mods[i]->name, name)) {   /* loaded again: the hooks stand (same addresses) */
            if (mods[i]->base != base) fprintf(stderr, "[fighter] %s loaded again at %#llx, not %#llx: its source is not reached\n", name,
                                               (unsigned long long)base, (unsigned long long)mods[i]->base);
            return;
        }
    char dir[900], path[1100];
    snprintf(path, sizeof path, "%s\\%s.dll", folder(dir, sizeof dir), name);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return;
    if (nmods == MODS) { fprintf(stderr, "[fighter] more than %d fighter libraries: %s left out\n", MODS, path); return; }
    if (fls == FLS_OUT_OF_INDEXES) fls = FlsAlloc(NULL);
    HMODULE dll = LoadLibraryA(path);
    FsBindFn bind = dll ? (FsBindFn)(void *)GetProcAddress(dll, "fighter_bind") : NULL;
    if (!bind) { fprintf(stderr, "[fighter] %s is not a fighter library (no fighter_bind)\n", path); return; }
    FsMod *m = calloc(1, sizeof *m);
    snprintf(m->name, sizeof m->name, "%s", name);
    m->base = base;
    m->dll = dll;
    bind(&host, &m->t);
    int missing = 0;
    for (int s = 0; s < m->t.sym_count; s++) {
        m->t.addr[s] = rt_symbol(m->t.syms[s].name);
        if (!m->t.addr[s]) missing++;
    }
    int idx = nmods;
    mods[nmods++] = m;
    m->stubs = VirtualAlloc(NULL, (size_t)m->t.ovr_count * 32 + 32, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    int hooked = 0, refused = 0;
    for (int i = 0; m->stubs && i < m->t.ovr_count && i < 0x10000; i++) {
        u64 at = base + m->t.ovr[i].off;
        if (was_installed(at)) { hooked++; continue; }
        char label[96];
        snprintf(label, sizeof label, "%s source: %.60s", name, m->t.ovr[i].name);
        GuestFn *stub = make_stub(m->stubs + 32 * i, (u64)idx << 16 | (u64)i);
        if (rt_hook_function(at, rt_native(stub, _strdup(label)))) hooked++;
        else refused++;
    }
    FlushInstructionCache(GetCurrentProcess(), m->stubs, (size_t)m->t.ovr_count * 32);
    fprintf(stderr, "[fighter] %s: %d scripts run as C++ source (%d not functions of the module), %d game symbols bound (%d missing)\n", name,
            hooked, refused, m->t.sym_count - missing, missing);
}
#else
void fighter_src_module_loaded(const char *module, u64 base) { (void)module; (void)base; }
#endif
