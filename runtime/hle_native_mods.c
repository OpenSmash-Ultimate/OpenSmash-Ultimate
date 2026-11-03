/* Native mods: DLLs that replace functions of the game through the fighter SDK's hook.h API
 * (ssbu-recomp/fighter_sdk/include/hook.h; ssbu-recomp's hook_host.c), on this runtime's hooks.
 *
 * A mod is a DLL exporting `void mod_init(const HookApi *api)`. It is loaded, and mod_init called once,
 * when the game mounts its data (right after the Skyline plugins load: hle_fs.c), from:
 *   - the folder SSBU_MODS names (the SDK's switch), and
 *   - the native\ folder of every mod switched on in mods.json (mods\<mod>\native\*.dll).
 * install() is rt_hook_function: the same patch the Skyline plugins' hooks use (the dispatch-table
 * slot, and the function's first bytes for direct calls: hle_detour.c), so a mod's hook is reached
 * however the game calls the function. Each hook runs through a small thunk of its own that counts
 * the calls and, once removed, calls the original instead: remove() and enable() write one word.
 * A name no loaded module exports yet is kept, and hooked when its module loads (native_mods_module_loaded).
 * The Ctx the mods see is this runtime's Cpu: x[0..30], sp as x[31], then v[0..31], as hook.h lays it out.
 * Windows only, as ssbu-recomp's. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"

#ifdef _WIN32
typedef void (*HookFn)(Cpu *c, void *user);
typedef void (*HookOrig)(Cpu *c);
typedef struct HookApi {
    u32 size, version;
    int (*install)(u64 address, HookFn replacement, void *user, HookOrig *orig);
    int (*install_name)(const char *name, HookFn replacement, void *user, HookOrig *orig);
    int (*remove)(int id);
    int (*enable)(int id);
    long long (*calls)(int id);
    u64 (*address)(const char *name);
    void (*call)(Cpu *c, u64 address);
    void (*log)(const char *text);
    const char *(*error)(void);
} HookApi;

#define HOOKS 256
typedef struct Hook {
    HookFn repl;
    void *user;
    GuestFn *orig;             /* what the function ran before this hook */
    HookOrig *orig_out;
    volatile LONG on;
    volatile LONG64 calls;
    char *name;                /* (a name not loaded yet: installed when its module loads) */
    int pending;
} Hook;
static Hook hooks[HOOKS];
static int nhooks;
static SRWLOCK hooks_lock = SRWLOCK_INIT;
static char last_error[200];

static void run(int i, Cpu *c) {
    Hook *h = &hooks[i];
    InterlockedIncrement64(&h->calls);
    if (h->on) h->repl(c, h->user);
    else h->orig(c);
}
/* One thunk per hook (a native function has no room for a closure): thunk_<g><k><j> is hook g*64 + k*8 + j. */
#define T(g, k, j) static void thunk_##g##k##j(Cpu *c) { run((g) * 64 + (k) * 8 + (j), c); }
#define T8(g, k) T(g, k, 0) T(g, k, 1) T(g, k, 2) T(g, k, 3) T(g, k, 4) T(g, k, 5) T(g, k, 6) T(g, k, 7)
#define T64(g) T8(g, 0) T8(g, 1) T8(g, 2) T8(g, 3) T8(g, 4) T8(g, 5) T8(g, 6) T8(g, 7)
T64(0) T64(1) T64(2) T64(3)
#define R(g, k, j) thunk_##g##k##j,
#define R8(g, k) R(g, k, 0) R(g, k, 1) R(g, k, 2) R(g, k, 3) R(g, k, 4) R(g, k, 5) R(g, k, 6) R(g, k, 7)
#define R64(g) R8(g, 0) R8(g, 1) R8(g, 2) R8(g, 3) R8(g, 4) R8(g, 5) R8(g, 6) R8(g, 7)
static GuestFn *const thunks[HOOKS] = {R64(0) R64(1) R64(2) R64(3)};

static int hook_at(int i, u64 address) {   /* (under hooks_lock) */
    char label[48];
    snprintf(label, sizeof label, "native mod hook %d", i + 1);
    u64 orig = rt_hook_function(address, rt_native(thunks[i], label));
    if (!orig) { snprintf(last_error, sizeof last_error, "%#llx is not the start of a function of the game", (unsigned long long)address); return 0; }
    hooks[i].orig = rt_lookup(orig);
    if (hooks[i].orig_out) *hooks[i].orig_out = (HookOrig)hooks[i].orig;
    hooks[i].on = 1;
    hooks[i].pending = 0;
    return 1;
}

static int new_hook(HookFn repl, void *user, HookOrig *orig) {
    if (!repl) { snprintf(last_error, sizeof last_error, "no replacement given"); return -1; }
    if (nhooks == HOOKS) { snprintf(last_error, sizeof last_error, "more than %d hooks", HOOKS); return -1; }
    int i = nhooks++;
    hooks[i] = (Hook){repl, user, NULL, orig, 0, 0, NULL, 0};
    return i;
}

static int api_install(u64 address, HookFn repl, void *user, HookOrig *orig) {
    AcquireSRWLockExclusive(&hooks_lock);
    int i = new_hook(repl, user, orig), id = i >= 0 && hook_at(i, address) ? i + 1 : 0;
    if (i >= 0 && !id) nhooks--;
    ReleaseSRWLockExclusive(&hooks_lock);
    return id;
}

static int api_install_name(const char *name, HookFn repl, void *user, HookOrig *orig) {
    if (!name || !*name) { snprintf(last_error, sizeof last_error, "no name given"); return 0; }
    AcquireSRWLockExclusive(&hooks_lock);
    int i = new_hook(repl, user, orig), id = 0;
    if (i >= 0) {
        u64 at = rt_symbol(name);
        if (at) id = hook_at(i, at) ? i + 1 : 0;
        else { hooks[i].name = _strdup(name); hooks[i].pending = 1; id = i + 1; }   /* (its module is not loaded yet) */
        if (!id) nhooks--;
    }
    ReleaseSRWLockExclusive(&hooks_lock);
    return id;
}

static Hook *by_id(int id) { return id > 0 && id <= nhooks ? &hooks[id - 1] : NULL; }
static int api_remove(int id) { Hook *h = by_id(id); if (!h || h->pending) return 0; InterlockedExchange(&h->on, 0); return 1; }
static int api_enable(int id) { Hook *h = by_id(id); if (!h || h->pending) return 0; InterlockedExchange(&h->on, 1); return 1; }
static long long api_calls(int id) { Hook *h = by_id(id); return h ? (long long)h->calls : -1; }
static u64 api_address(const char *name) { return name ? rt_symbol(name) : 0; }
static void api_call(Cpu *c, u64 address) { rt_call(c, address); }
static void api_log(const char *text) { fprintf(stderr, "[mod] %s\n", text ? text : ""); fflush(stderr); }
static const char *api_error(void) { return last_error; }

static const HookApi api = {sizeof(HookApi), 1, api_install, api_install_name, api_remove, api_enable, api_calls,
                            api_address, api_call, api_log, api_error};

/* (hle_fighter_registry.c) the same hooks for the runtime's own fighter registry */
const void *native_mods_api(void) { return &api; }

/* (host.c, after a module's init) the hooks by name its exports were waiting for */
void native_mods_module_loaded(void) {
    if (!nhooks) return;
    AcquireSRWLockExclusive(&hooks_lock);
    for (int i = 0; i < nhooks; i++) {
        if (!hooks[i].pending) continue;
        u64 at = rt_symbol(hooks[i].name);
        if (!at) continue;
        if (hook_at(i, at)) fprintf(stderr, "[mod] %s hooked as its module loaded\n", hooks[i].name);
        else fprintf(stderr, "[mod] %s: %s\n", hooks[i].name, last_error);
        hooks[i].pending = 0;
    }
    ReleaseSRWLockExclusive(&hooks_lock);
}

static int load_one(const char *path) {
    HMODULE m = LoadLibraryA(path);
    if (!m) { fprintf(stderr, "[mod] %s did not load (error %lu)\n", path, GetLastError()); return 0; }
    void (*init)(const HookApi *) = (void (*)(const HookApi *))(void *)GetProcAddress(m, "mod_init");
    if (!init) { fprintf(stderr, "[mod] %s has no mod_init: not a mod (left loaded)\n", path); return 0; }
    int before = nhooks;
    init(&api);
    fprintf(stderr, "[mod] %s: mod_init made %d hook%s\n", path, nhooks - before, nhooks - before == 1 ? "" : "s");
    return 1;
}

static int load_folder(const char *dir) {
    char pattern[1100];
    snprintf(pattern, sizeof pattern, "%s\\*.dll", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do {
        char path[1400];
        snprintf(path, sizeof path, "%s\\%s", dir, fd.cFileName);
        n += load_one(path);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

void fighter_registry_scan(void);   /* (hle_fighter_registry.c) */
/* (hle_fs.c, when the game mounts its data, after the Skyline plugins) */
int mods_enabled_names(char names[][128], int max);   /* (hle_mod_switch.c) the mods switched on in mods.json */
void native_mods_load(void) {
    static LONG done;
    if (InterlockedExchange(&done, 1)) return;
    int n = 0;
    fighter_registry_scan();   /* (hle_fighter_registry.c: SSBU_CHARACTERS; it may set SSBU_MODS) */
    const char *dir = getenv("SSBU_MODS");
    if (dir && *dir) n += load_folder(dir);
    static char names[256][128];
    int count = mods_enabled_names(names, 256);
    for (int i = 0; i < count; i++) {
        char folder[1100];
        snprintf(folder, sizeof folder, "%s\\..\\..\\mods\\%s\\native", rt_data_dir ? rt_data_dir : ".", names[i]);
        n += load_folder(folder);
    }
    if (n) fprintf(stderr, "[mod] %d native mod%s loaded, %d hook%s\n", n, n == 1 ? "" : "s", nhooks, nhooks == 1 ? "" : "s");
}
#else
void native_mods_module_loaded(void) {}
void native_mods_load(void) {}
#endif
