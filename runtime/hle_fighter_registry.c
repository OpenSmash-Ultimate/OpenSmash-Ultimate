/* The fighter registry: new fighters with a kind and a script module of their own (ssbu-recomp's
 * fighter_sdk/newmodule_host.c, N5 - N13, on this runtime). OPT-IN: without SSBU_CHARACTERS=<folder>
 * nothing of this file runs and no hook is installed. Contains nothing of the game.
 *
 * 1. The registry. <folder>\<name>\fighter.json of every sub-folder = one new fighter (name, donor, dll,
 *    kind). Kinds are given in name order from 0x5E. A fighter's script library is
 *    SSBU_FIGHTER_BIN\<name>.dll when that folder has one, else <folder>\<name>\<dll>. SSBU_MODS defaults
 *    to registry_bin\ beside the program: the registry's components (the kind and roster DLLs, built with
 *    hook.h, loaded by hle_native_mods.c), which get the list through the exported ssbu_registry()
 *    (ssbu-recomp/fighter_sdk/include/registry.h). The kind tables themselves are code patches made
 *    when the game is built (tools/fighter_kinds.py).
 *
 * 2. A module without an NRO. The game's script module worker (main 0x71022b6620, the same in 13.0.4
 *    and 13.0.5) formats "prebuilt:/nro/<build>/lua2cpp_<name>.nro" with 0x7101ea04d0 (the call returns
 *    to 0x71022b67d4; x4 = request + 0x30, the name), looks the path up in the archive and loads the
 *    file; then request + 0x132 = 1 ("loaded"). A name the archive does not have would leave the
 *    requester waiting for ever. The hook on the formatter answers a registered name itself:
 *      - the donor's module not loaded yet: the name becomes the donor's, and the game loads the
 *        donor's image through this request (its class tables and constants are data of that image);
 *      - else: "loaded", and an empty path (the worker goes back to its queue).
 *    The module is a range of its own (host.c rt_module_clone, reserved, never committed) whose
 *    functions are, by offset, the fighter library's scripts (fighter_src_attach_clone), else the
 *    donor's: dispatch only, the donor's own code is not touched, so a real donor in the same match
 *    keeps its own scripts. An agent the donor's factory makes for a new fighter (the kind component
 *    calls agent_begin / agent_end around it) is moved into the new module: its class table is replaced
 *    by a copy whose donor-module entries are the new range's, and every script it registers
 *    (lib::L2CAgent::sv_set_function_hash, lua2cpp::L2CAgentBase::sv_set_status_func: hooked by name)
 *    is registered at the new range's address. N12: the fighter's own script names (functions only in
 *    its source) are registered too, and its `<name>_frame` runs once per fight frame. N7: the donor's
 *    create_agent_* factories answer with the library's own factory for the new names when it has one.
 *
 * 3. Paths under the new name. The formatter and the per-kind path builders make the engine's file
 *    paths. A path with a new fighter's name as a token is kept when the game's data has it (a mod can
 *    put the fighter's files there, tools/mods.py), else rewritten to the donor's name, so the donor's
 *    file answers. While a new fighter is set up (owner_set), a path built with the donor's name is
 *    answered with the fighter's own when the data has that one (N8). The roster row's picture paths and
 *    name text (N9, "display_name") likewise. Windows only, as the components. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"

#ifdef _WIN32
typedef Cpu Ctx;
typedef void (*HookFn)(Cpu *c, void *user);
typedef void (*ImportFn)(Cpu *c);
typedef struct HookApi {
    u32 size, version;
    int (*install)(u64 address, HookFn replacement, void *user, ImportFn *orig);
    int (*install_name)(const char *name, HookFn replacement, void *user, ImportFn *orig);
    int (*remove)(int id);
    int (*enable)(int id);
    long long (*calls)(int id);
    u64 (*address)(const char *name);
    void (*call)(Cpu *c, u64 address);
    void (*log)(const char *text);
    const char *(*error)(void);
} HookApi;
const void *native_mods_api(void);                                     /* (hle_native_mods.c) */
u64 rt_module_named(const char *name, u64 *image_size, u64 *text_size); /* (host.c) */
u64 rt_module_clone(const char *donor, const char *name, GuestFn ***code, GuestFn ***odd);
const RtSym *rt_module_exports(u64 base, u32 *count);
int rt_is_function(u64 addr);
int fighter_src_attach_clone(const char *name, const char *path, u64 base, u64 donor_base, u64 donor_size, GuestFn **code, GuestFn **odd,
                             u64 text_size);                            /* (hle_fighter_src.c) */
const char *fighter_src_function(int lib, int i, u32 *off);

/* ---- registry.h (ssbu-recomp/fighter_sdk/include): the components' view, version 4 */
typedef struct SsbuRegFighter {
    char name[32], donor[32], dll[64], folder[260];
    int kind, own_name;
} SsbuRegFighter;
typedef struct SsbuRegistry {
    u32 size, version;
    int count;
    const SsbuRegFighter *fighters;
    void (*start)(void);
    void (*agent_begin)(const char *name);
    void (*agent_end)(const char *name, u64 agent);
    void (*owner_set)(const char *name);
    void (*agent_begin_weapon)(const char *name);
    int (*article_agent)(void *ctx, const char *name, const char *article, int category);
} SsbuRegistry;

/* 13.0.5 addresses (the only build this runtime takes): each is ssbu-recomp's 13.0.4 one through the function
 * map ga_13_0_5.txt (code) or + 0x1000 (data, game_addr.h), and checked in the lifted 13.0.5 C (build/main):
 * the worker f_71022b6620 sets x4 = request + 0x30, calls f_7101ea04d0 with x30 = 0x71022b67d4, then writes
 * request + 0x132; it reads the archive's tables at 0x7105332000 + 0xf20. */
#define MAIN 0x7100000000ull
#define NM_MAX 8
#define NM_VT_SLOTS 96
#define NM_WORKER_RET (MAIN + 0x22b67d4)        /* 13.0.5 = 13.0.4 (lifted: f_71022b6620) */
#define NM_FORMATTER (MAIN + 0x1ea04d0)
#define NM_FSINFO (MAIN + 0x5332f20)            /* the archive's tables (13.0.4 0x7105331f20; the worker reads it) */
#define NM_PORTRAIT (MAIN + 0x3238a70)          /* 13.0.4 0x71032384c0 */
#define NM_NAMEID (MAIN + 0x3262310)            /* 13.0.4 0x7103261d60 */
#define NM_MSG_HAS (MAIN + 0x37a4570)           /* 13.0.4 0x71037a3fc0 */
#define NM_MSG_GET (MAIN + 0x37a4450)           /* 13.0.4 0x71037a3ea0 */
/* the other string builders of per-kind paths (13.0.4 0x710147f330, 0x71017df5f0, 0x71017dfe50, 0x710353e330):
 * the first three return the path in x0's buffer, the last takes it in x1 */
static const u64 g_nm_path_fn[] = {MAIN + 0x147f330, MAIN + 0x17df5b0, MAIN + 0x17dfe10, MAIN + 0x353e8e0};
#define NM_PATH_PRE 3

typedef struct NewMod {
    SsbuRegFighter f;
    char donor_module[48], module[48], dll_path[600];
    u64 base, dbase, dsize, dtext;         /* the new range; the donor module's */
    GuestFn **code, **odd;
    int lib;                               /* fighter_src library index, -1 */
    int active, own_count;
    u8 *is_new;                            /* per library function: only in the source */
    u64 *cat_hash;
    u64 vt_src[64], vt_dst[64];
    int vt_count;
    long requests, answered, substituted, agents, remapped, vt_clones, paths, paths_missing, paths_own;
    char display[64];
    u16 display16[64];
    long nameid_calls, text_calls, own_scripts, own_script_agents, frame_calls;
    int frame_at;
} NewMod;
static NewMod g_nm[NM_MAX];
static int g_nm_count, g_nm_started;
static SsbuRegFighter g_nm_list[NM_MAX];
static SRWLOCK g_nm_lock = SRWLOCK_INIT, g_nm_act_lock = SRWLOCK_INIT;
static __declspec(thread) NewMod *t_nm_current;
static __declspec(thread) int t_nm_weapon;
static __declspec(thread) u64 t_nm_cat_agent;
static __declspec(thread) NewMod *t_nm_owner;
static struct { u64 agent; NewMod *nm; } g_nm_agent[2048];
static ImportFn g_nm_o_fmt, g_nm_o_fnhash, g_nm_o_status, g_nm_o_frame;
static const HookApi *g_api;

static void nm_log(const char *fmt, ...) {
    char line[900];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s\n", line);
    fflush(stderr);
}
static u64 rd64(u64 a) { return *P(u64, a); }
static void wr64(u64 a, u64 v) { *P(u64, a) = v; }
static u32 rd32(u64 a) { return *P(u32, a); }

/* ---- manifests */
static int nm_json_str(const char *js, const char *key, char *out, size_t n) {
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(js, pat);
    out[0] = 0;
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == ':' || *p == '\r' || *p == '\n') p++;
    size_t k = 0;
    if (*p == '"') { for (p++; *p && *p != '"' && k + 1 < n; p++) out[k++] = *p; }
    else for (; *p && *p != ',' && *p != '}' && *p != '\r' && *p != '\n' && *p != ' ' && k + 1 < n; p++) out[k++] = *p;
    out[k] = 0;
    return 1;
}
static int nm_cmp_fighter(const void *a, const void *b) { return strcmp(((const NewMod *)a)->f.name, ((const NewMod *)b)->f.name); }
static int nm_exists(const char *path) { return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES; }

/* (hle_native_mods.c, before it reads SSBU_MODS; and ssbu_registry()) */
void fighter_registry_scan(void) {
    static LONG done;
    if (InterlockedExchange(&done, 1)) return;
    const char *root = getenv("SSBU_CHARACTERS");
    if (!root || !root[0]) return;
    char pat[MAX_PATH * 2];
    WIN32_FIND_DATAA fd;
    snprintf(pat, sizeof pat, "%s\\*", root);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) { nm_log("[registry] SSBU_CHARACTERS=%s: no such folder", root); return; }
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.' || g_nm_count >= NM_MAX) continue;
        char path[MAX_PATH * 2], js[8192], v[64];
        snprintf(path, sizeof path, "%s\\%s\\fighter.json", root, fd.cFileName);
        FILE *f = fopen(path, "rb");
        if (!f) continue;   /* a folder without a manifest is not a fighter */
        size_t n = fread(js, 1, sizeof js - 1, f);
        fclose(f);
        js[n] = 0;
        NewMod *nm = &g_nm[g_nm_count];
        memset(nm, 0, sizeof *nm);
        nm->lib = -1;
        if (!nm_json_str(js, "name", nm->f.name, sizeof nm->f.name) || !nm->f.name[0]) snprintf(nm->f.name, sizeof nm->f.name, "%.31s", fd.cFileName);
        if (!nm_json_str(js, "donor", nm->f.donor, sizeof nm->f.donor) || !nm->f.donor[0]) { nm_log("[registry] %s: no \"donor\": skipped", path); continue; }
        nm_json_str(js, "dll", nm->f.dll, sizeof nm->f.dll);
        nm_json_str(js, "display_name", nm->display, sizeof nm->display);
        for (int q = 0; q < 63 && nm->display[q]; q++) nm->display16[q] = (u8)nm->display[q];
        nm->f.own_name = !(nm_json_str(js, "own_name", v, sizeof v) && (v[0] == 'f' || v[0] == '0'));
        nm->f.kind = nm_json_str(js, "kind", v, sizeof v) && v[0] >= '0' && v[0] <= '9' ? (int)strtol(v, NULL, 0) : -1;
        snprintf(nm->f.folder, sizeof nm->f.folder, "%s\\%s", root, fd.cFileName);
        snprintf(nm->module, sizeof nm->module, "lua2cpp_%s", nm->f.name);
        snprintf(nm->donor_module, sizeof nm->donor_module, "lua2cpp_%s", nm->f.donor);
        g_nm_count++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    qsort(g_nm, (size_t)g_nm_count, sizeof g_nm[0], nm_cmp_fighter);
    const char *bin = getenv("SSBU_FIGHTER_BIN");
    for (int i = 0; i < g_nm_count; i++) {
        NewMod *nm = &g_nm[i];
        if (nm->f.kind < 0) nm->f.kind = 0x5e + i;   /* "auto": the first free kind numbers, in name order */
        nm->dll_path[0] = 0;
        if (bin && bin[0]) {
            snprintf(nm->dll_path, sizeof nm->dll_path, "%s\\%s.dll", bin, nm->f.name);
            if (!nm_exists(nm->dll_path)) nm->dll_path[0] = 0;
        }
        if (!nm->dll_path[0] && nm->f.dll[0]) {
            snprintf(nm->dll_path, sizeof nm->dll_path, "%s\\%s", nm->f.folder, nm->f.dll);
            if (!nm_exists(nm->dll_path)) { nm_log("[registry] %s: its script library %s is missing", nm->f.name, nm->dll_path); nm->dll_path[0] = 0; }
        }
        g_nm_list[i] = nm->f;
        nm_log("[registry] fighter %s: kind 0x%x, donor %s, script library %s, folder %s, own name %d", nm->f.name, nm->f.kind, nm->f.donor,
               nm->dll_path[0] ? nm->dll_path : "(none)", nm->f.folder, nm->f.own_name);
    }
    if (g_nm_count) {
        const char *mods = getenv("SSBU_MODS");
        if (!(mods && mods[0])) {
            char exe[MAX_PATH * 2];
            DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH);
            while (n && exe[n - 1] != '\\' && exe[n - 1] != '/') n--;
            snprintf(exe + n, sizeof exe - n, "registry_bin");
            _putenv_s("SSBU_MODS", exe);
        }
        nm_log("[registry] %d new fighter(s) from %s; components from %s", g_nm_count, root, getenv("SSBU_MODS"));
    } else nm_log("[registry] SSBU_CHARACTERS=%s: no sub-folder with a fighter.json: no new fighter", root);
}

/* ---- the module */
static NewMod *nm_find(const char *name) {
    for (int i = 0; i < g_nm_count; i++) if (!strcmp(g_nm[i].f.name, name)) return &g_nm[i];
    return NULL;
}
static void nm_factory_hooks(NewMod *nm);
static int nm_activate(NewMod *nm) {
    if (nm->active) return 1;
    AcquireSRWLockExclusive(&g_nm_act_lock);
    if (!nm->active) {
        nm->dbase = rt_module_named(nm->donor_module, &nm->dsize, &nm->dtext);
        if (nm->dbase) nm->base = rt_module_clone(nm->donor_module, nm->module, &nm->code, &nm->odd);
        if (nm->base) {
            if (nm->dll_path[0])
                nm->lib = fighter_src_attach_clone(nm->f.name, nm->dll_path, nm->base, nm->dbase, nm->dsize, nm->code, nm->odd, nm->dtext);
            u32 off;
            int n = 0;
            while (nm->lib >= 0 && fighter_src_function(nm->lib, n, &off)) n++;
            nm->own_count = n;
            nm->is_new = calloc((size_t)n + 1, 1);
            int news = 0;
            for (int i = 0; i < n; i++) {
                fighter_src_function(nm->lib, i, &off);
                nm->is_new[i] = (off & 3) != 0 || !rt_is_function(nm->dbase + off);
                news += nm->is_new[i];
            }
            nm_factory_hooks(nm);
            nm_log("[registry] module %s is OURS: no NRO; address range %#llx (+%#llx, reserved, not committed); %d of its functions come from %s "
                   "(%d of them only in the source), the others are the donor module %s's (at %#llx) at the same offset",
                   nm->module, (unsigned long long)nm->base, (unsigned long long)nm->dsize, n, nm->dll_path[0] ? nm->dll_path : "(no library)", news,
                   nm->donor_module, (unsigned long long)nm->dbase);
            __atomic_store_n(&nm->active, 1, __ATOMIC_RELEASE);
        }
    }
    ReleaseSRWLockExclusive(&g_nm_act_lock);
    return nm->active;
}

/* ---- agents */
static NewMod *nm_agent_lookup(u64 agent) {
    NewMod *nm = NULL;
    unsigned i = (unsigned)(agent >> 4) & 2047;
    AcquireSRWLockShared(&g_nm_lock);
    for (int k = 0; k < 2048 && g_nm_agent[i].agent; k++, i = (i + 1) & 2047)
        if (g_nm_agent[i].agent == agent) { nm = g_nm_agent[i].nm; break; }
    ReleaseSRWLockShared(&g_nm_lock);
    return nm;
}
static void nm_agent_set(u64 agent, NewMod *nm) {   /* nm == NULL: forget (the slot stays in the probe chain) */
    unsigned i = (unsigned)(agent >> 4) & 2047;
    AcquireSRWLockExclusive(&g_nm_lock);
    for (int k = 0; k < 2048; k++, i = (i + 1) & 2047) {
        if (g_nm_agent[i].agent == agent) { g_nm_agent[i].nm = nm; break; }
        if (!g_nm_agent[i].agent) { if (nm) { g_nm_agent[i].agent = agent; g_nm_agent[i].nm = nm; } break; }
    }
    ReleaseSRWLockExclusive(&g_nm_lock);
}
static u64 nm_remap(NewMod *nm, u64 a) {
    if (a - nm->dbase < nm->dtext && rt_is_function(a)) return a - nm->dbase + nm->base;
    return a;
}
static u64 nm_clone_vtable(NewMod *nm, u64 vt) {
    if (vt - nm->dbase >= nm->dsize) return vt;   /* a class of another module: nothing of the donor's in it */
    for (int i = 0; i < nm->vt_count; i++) if (nm->vt_src[i] == vt) return nm->vt_dst[i];
    u64 copy[NM_VT_SLOTS + 2] = {0}, lo = vt - 0x10, end = nm->dbase + nm->dsize;
    int moved = 0;
    for (int i = 0; i < NM_VT_SLOTS + 2 && lo + 8ull * i + 8 <= end; i++) {
        u64 v = rd64(lo + 8ull * i);
        copy[i] = i >= 2 ? nm_remap(nm, v) : v;
        moved += copy[i] != v;
    }
    u64 dst = rt_guest_copy(copy, sizeof copy) + 0x10;   /* (guest-visible, for the life of the process) */
    if (nm->vt_count < 64) { nm->vt_src[nm->vt_count] = vt; nm->vt_dst[nm->vt_count++] = dst; }
    nm->vt_clones++;
    if (nm->vt_clones <= 40)
        nm_log("[registry] %s: class table %s+%#llx copied for the new module: %d entries are now %s addresses", nm->module, nm->donor_module,
               (unsigned long long)(vt - nm->dbase), moved, nm->module);
    return dst;
}
static void nm_agent_begin(const char *name) {
    NewMod *nm = name ? nm_find(name) : NULL;
    t_nm_current = nm && nm_activate(nm) ? nm : NULL;
    t_nm_weapon = 0;
    t_nm_cat_agent = 0;
    if (nm && !t_nm_current) {
        static int said;
        if (!said++) nm_log("[registry] %s: the donor module %s is not loaded: the agent stays the donor's", nm->module, nm->donor_module);
    }
}
static void nm_agent_begin_weapon(const char *name) { nm_agent_begin(name); t_nm_weapon = 1; }
static void nm_agent_end(const char *name, u64 agent) {
    NewMod *nm = t_nm_current;
    t_nm_current = NULL;
    if (!name) { if (agent && g_nm_count) nm_agent_set(agent, NULL); return; }
    if (!nm || agent < 0x10000) return;
    u64 vt = rd64(agent), nv = nm_clone_vtable(nm, vt);
    if (nv != vt) wr64(agent, nv);
    nm_agent_set(agent, nm);
    if (nm->agents == 9 || (nm->agents && nm->agents % 64 == 0))
        nm_log("[registry] %s so far: %ld module requests (%ld answered directly, %ld loaded the donor image), %ld agents moved into the module, "
               "%ld class tables copied, %ld script registrations moved to its addresses, %ld paths with its name asked (%ld not files of the data)",
               nm->module, nm->requests, nm->answered, nm->substituted, nm->agents, nm->vt_clones, nm->remapped, nm->paths, nm->paths_missing);
    if (nm->agents++ < 24)
        nm_log("[registry] %s: agent %llx (class table %s+%#llx) belongs to the new module", nm->module, (unsigned long long)agent,
               vt - nm->dbase < nm->dsize ? nm->donor_module : "(other)", (unsigned long long)(vt - nm->dbase));
}

/* N12: the fighter's own script names (see newmodule_host.c): at the first registration made for a new
 * fighter's FIGHTER agent, every function only in its source with that registration's category prefix is
 * registered under the hash of its own name. SSBU_REG_OWNSCRIPTS=0: off. */
static const char *const g_nm_cat[4] = {"game_", "effect_", "sound_", "expression_"};
static u64 nm_hash40(const char *s) {
    u32 crc = 0xffffffffu;
    size_t n = 0;
    for (; s[n]; n++) {
        u8 ch = (u8)s[n];
        if (ch >= 'A' && ch <= 'Z') ch += 32;
        crc ^= ch;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ((u64)(n & 0xff) << 32) | (u32)~crc;
}
static int nm_cat_of_hash(NewMod *nm, u64 h) {
    if (nm->lib < 0) return -1;
    if (!nm->cat_hash) {
        u64 *t = calloc((size_t)nm->own_count + 1, sizeof *t);
        for (int i = 0; i < nm->own_count; i++) t[i] = nm_hash40(fighter_src_function(nm->lib, i, NULL));
        nm->cat_hash = t;
    }
    for (int i = 0; i < nm->own_count; i++)
        if (nm->cat_hash[i] == h) {
            const char *n = fighter_src_function(nm->lib, i, NULL);
            for (int k = 0; k < 4; k++) if (!strncmp(n, g_nm_cat[k], strlen(g_nm_cat[k]))) return k;
        }
    return -1;
}
static void nm_own_scripts(Ctx *c, NewMod *nm, u64 agent, int cat) {
    u64 keep[31];
    memcpy(keep, c->x, sizeof keep);
    int n = 0, k = 0;
    char names[400];
    names[0] = 0;
    for (int i = 0; i < nm->own_count; i++) {
        u32 off;
        const char *name = fighter_src_function(nm->lib, i, &off);
        if (!nm->is_new[i] || strncmp(name, g_nm_cat[cat], strlen(g_nm_cat[cat]))) continue;
        c->x[0] = agent; c->x[1] = nm->base + off; c->x[2] = nm_hash40(name);
        g_nm_o_fnhash(c);
        memcpy(c->x, keep, sizeof keep);
        n++; nm->own_scripts++;
        if (k < 360) k += snprintf(names + k, sizeof names - k, " %s", name);
    }
    if (nm->own_script_agents++ < 8)
        nm_log("[n12] %s: %s agent %llx: %d scripts of the fighter's OWN names registered by name (functions only in its source):%s", nm->module, g_nm_cat[cat],
               (unsigned long long)agent, n, n ? names : " (none)");
}
static void nm_h_fnhash(Ctx *c, void *user) {   /* lib::L2CAgent::sv_set_function_hash(this, function, hash) */
    (void)user;
    NewMod *nm = t_nm_current ? t_nm_current : g_nm_count ? nm_agent_lookup(c->x[0]) : NULL;
    if (nm && nm->active) { u64 a = nm_remap(nm, c->x[1]); if (a != c->x[1]) { c->x[1] = a; nm->remapped++; } }
    u64 agent = c->x[0], h = c->x[2] & 0xffffffffffull;
    g_nm_o_fnhash(c);
    static int on = -1;
    if (on < 0) { const char *e = getenv("SSBU_REG_OWNSCRIPTS"); on = !(e && e[0] == '0'); }
    if (on && nm && nm->active && t_nm_current == nm && !t_nm_weapon && t_nm_cat_agent != agent) {
        int cat = nm_cat_of_hash(nm, h);
        if (cat >= 0) {
            u64 x0 = c->x[0];
            t_nm_cat_agent = agent;
            nm_own_scripts(c, nm, agent, cat);
            c->x[0] = x0;
        }
    }
}
static void nm_h_status(Ctx *c, void *user) {   /* lua2cpp::L2CAgentBase::sv_set_status_func(this, kind, line, function) */
    (void)user;
    NewMod *nm = t_nm_current ? t_nm_current : g_nm_count ? nm_agent_lookup(c->x[0]) : NULL;
    if (nm && nm->active) { u64 a = nm_remap(nm, c->x[3]); if (a != c->x[3]) { c->x[3] = a; nm->remapped++; } }
    g_nm_o_status(c);
}
/* N12: `<name>_frame` of the fighter's library, once per fight frame from the frame line
 * lua2cpp::L2CFighterCommon::sys_line_system_control_fighter (its status agent in x0). SSBU_REG_FRAME=0: off. */
static void nm_h_frame(Ctx *c, void *user) {
    (void)user;
    NewMod *nm = g_nm_count ? nm_agent_lookup(c->x[0]) : NULL;
    if (nm && nm->active && nm->lib >= 0) {
        if (nm->frame_at == 0) {
            char want[64];
            snprintf(want, sizeof want, "%s_frame", nm->f.name);
            nm->frame_at = -1;
            for (int i = 0; i < nm->own_count; i++) if (!strcmp(fighter_src_function(nm->lib, i, NULL), want)) { nm->frame_at = i + 1; break; }
            nm_log("[n12] %s: per-frame routine %s %s", nm->module, want, nm->frame_at > 0 ? "found in the fighter's library: called once per fight frame" : "not in the fighter's library");
        }
        if (nm->frame_at > 0) {
            u64 keep[31];
            u32 off;
            memcpy(keep, c->x, sizeof keep);
            fighter_src_function(nm->lib, nm->frame_at - 1, &off);
            rt_lookup(nm->base + off)(c);   /* (x30: the frame line's caller, as it was) */
            memcpy(c->x, keep, sizeof keep);
            nm->frame_calls++;
        }
    }
    g_nm_o_frame(c);
}

/* N7: the new fighter's own agent factories. While an agent is made for a new fighter on this thread and its
 * library has a function at the donor factory's offset, the name hash becomes the new name's and the
 * library's factory runs; 0 from it = the donor's factory makes the agent. SSBU_REG_FACTORY=0: off. */
static struct { u64 addr; NewMod *nm; ImportFn orig; char sym[96]; } g_nm_fact[64];
static int g_nm_fact_count;
static long g_nm_fact_own, g_nm_fact_donor;
static u32 nm_crc_plain(const char *s) {
    u32 crc = 0xffffffffu;
    for (; *s; s++) { crc ^= (u8)*s; for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1))); }
    return ~crc;
}
static u64 nm_rename40(u64 h, const char *p, const char *p2) {
    unsigned len = (unsigned)(h >> 32) & 0xff, lp = (unsigned)strlen(p), lp2 = (unsigned)strlen(p2);
    u32 x = nm_crc_plain(p) ^ nm_crc_plain(p2);
    if (len < lp) return h;
    for (unsigned i = 0; i < (len - lp) * 8; i++) x = (x >> 1) ^ (0xedb88320u & (0u - (x & 1)));
    return ((u64)((len - lp + lp2) & 0xff) << 32) | ((u32)h ^ x);
}
static int nm_own_at(NewMod *nm, u32 off) {
    u32 o;
    for (int i = 0; nm->lib >= 0 && i < nm->own_count; i++) if (fighter_src_function(nm->lib, i, &o) && o == off) return 1;
    return 0;
}
static void nm_h_factory(Ctx *c, void *user) {
    int k = (int)(intptr_t)user;
    NewMod *nm = t_nm_current;
    static int on = -1;
    if (on < 0) { const char *e = getenv("SSBU_REG_FACTORY"); on = !(e && e[0] == '0'); }
    if (on && nm && nm->active && nm == g_nm_fact[k].nm && nm_own_at(nm, (u32)(g_nm_fact[k].addr - nm->dbase))) {
        u64 keep[31];
        memcpy(keep, c->x, sizeof keep);
        u64 own = g_nm_fact[k].addr - nm->dbase + nm->base, renamed = nm_rename40(keep[0] & 0xffffffffffull, nm->f.donor, nm->f.name);
        c->x[0] = renamed;
        rt_lookup(own)(c);
        if (c->x[0]) {
            if (g_nm_fact_own++ < 14)
                nm_log("[registry] %s: agent made by ITS OWN factory %s under its own name (hash %llx; the donor's name hash was %llx)", nm->module, g_nm_fact[k].sym,
                       (unsigned long long)renamed, (unsigned long long)(keep[0] & 0xffffffffffull));
            return;
        }
        memcpy(c->x, keep, sizeof keep);
        if (g_nm_fact_donor++ < 14)
            nm_log("[registry] %s: factory %s: the fighter's library does not make the agent with name hash %llx: the DONOR's factory makes it", nm->module,
                   g_nm_fact[k].sym, (unsigned long long)(keep[0] & 0xffffffffffull));
    }
    g_nm_fact[k].orig(c);
}
static void nm_factory_hooks(NewMod *nm) {
    u32 count = 0;
    const RtSym *ex = rt_module_exports(nm->dbase, &count);
    int fresh = 0, any = 0;
    for (u32 i = 0; ex && i < count; i++) {
        if (!strstr(ex[i].name, "create_agent_") || ex[i].addr - nm->dbase >= nm->dtext) continue;
        if (!nm_own_at(nm, (u32)(ex[i].addr - nm->dbase))) continue;   /* (only where the library has its own) */
        any++;
        int known = 0;
        for (int k = 0; k < g_nm_fact_count; k++) known |= g_nm_fact[k].addr == ex[i].addr;
        if (known || g_nm_fact_count >= 64) continue;
        int k = g_nm_fact_count++;
        g_nm_fact[k].addr = ex[i].addr; g_nm_fact[k].nm = nm;
        snprintf(g_nm_fact[k].sym, sizeof g_nm_fact[k].sym, "%.95s", ex[i].name);
        if (g_api->install(ex[i].addr, nm_h_factory, (void *)(intptr_t)k, &g_nm_fact[k].orig)) fresh++;
        else { g_nm_fact[k].addr = 0; nm_log("[registry] %s: the factory %s could not be hooked: %s", nm->module, ex[i].name, g_api->error()); }
    }
    if (any) nm_log("[registry] %s: %d of the donor's agent factories have the library's own: hooked (%d now)", nm->module, any, fresh);
}

/* ---- the formatter: module requests and paths */
static i64 nm_arc_file_index(u64 hash) {   /* the archive's own lookup, as the worker does it (0x71022b68cc): file index, -1, -2 = not ready */
    u64 fsinfo = rd64(NM_FSINFO);
    if (!fsinfo) return -2;
    u64 holder = rd64(fsinfo + 0x78);
    if (!holder) return -2;
    u64 tab = rd64(holder);
    if (!tab) return -2;
    u64 buckets = rd64(tab + 0x50), entries = rd64(tab + 0x58);
    if (!buckets || !entries) return -2;
    u32 nb = rd32(buckets + 4);
    if (!nb) return -2;
    u64 b = buckets + 8 + (hash % nb) * 8;
    u32 start = rd32(b), count = rd32(b + 4);
    for (u32 i = 0; i < count; i++) {
        u64 e = rd64(entries + 8ull * (start + i));
        if ((e & 0xffffffffffull) == hash) return (i64)(e >> 40);
    }
    return -1;
}
static int nm_has_file(u64 hash) { return nm_arc_file_index(hash) >= 0; }
static int nm_sorted_has(u64 table, u32 count, u64 hash) {   /* a lookup sorted by the low 40 bits (hash | index << 40) */
    u32 lo = 0, hi = count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        u64 v = rd64(table + 8ull * mid) & 0xffffffffffull;
        if (v == hash) return 1;
        if (v < hash) lo = mid + 1; else hi = mid;
    }
    return 0;
}
/* Step 3 (tools/mods.py's new fighters): whether the data has a path: 1 a file, 2 a directory (a load group:
 * fighter/<name>/c00, cmn/ai, c00/camera; the archive's directory index, archive + 0x70, count header + 0xc),
 * 3 a folder or file of the search tables (fighter/<name>/motion/body/c00, effect/fighter/<name>: path index
 * search + 0x20, count [search + 8] + 8, lifted 0x710353e330), 0 none. A directory or folder of a new fighter
 * the data does not have still falls back to the donor's. */
static int nm_has_path(u64 hash) {
    if (nm_arc_file_index(hash) >= 0) return 1;
    u64 fsinfo = rd64(NM_FSINFO), holder = fsinfo ? rd64(fsinfo + 0x78) : 0;
    if (!holder) return 0;
    u64 tab = rd64(holder), search = rd64(holder + 8);
    if (tab) {
        u64 hdr = rd64(tab + 0x40), dirs = rd64(tab + 0x70);
        if (hdr && dirs && nm_sorted_has(dirs, rd32(hdr + 0xc), hash)) return 2;
    }
    if (search) {
        u64 body = rd64(search + 8), index = rd64(search + 0x20);
        if (body && index && nm_sorted_has(index, rd32(body + 8), hash)) return 3;
    }
    return 0;
}
static const char *const nm_kind_text[] = {"none", "file", "directory", "folder"};
static int nm_token_char(char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'); }
static u64 g_nm_seen[4096];
static int g_nm_seen_count;
static int nm_seen(u64 h) {   /* 1 if seen before; remembers it */
    for (int k = 0; k < g_nm_seen_count; k++) if (g_nm_seen[k] == h) return 1;
    if (g_nm_seen_count < 4096) g_nm_seen[g_nm_seen_count++] = h;
    return 0;
}
static char *nm_stristr(char *s, const char *name) {
    size_t n = strlen(name);
    for (; *s; s++) if (!_strnicmp(s, name, n)) return s;
    return NULL;
}
static int nm_writable(void *p) {
    MEMORY_BASIC_INFORMATION mi;
    return VirtualQuery(p, &mi, sizeof mi) && mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD | PAGE_READONLY | PAGE_EXECUTE_READ));
}
/* A colour of a new fighter its data does not have: the path of another costume slot (a whole token c01..c07,
 * as in fighter/<name>/model/body/c03 or se_<name>_c03; not c03attack.. names) as c00's, when the data has that.
 * A mod with one costume then dresses every colour as itself, not as the donor's colours. Same length: in place.
 * SSBU_REG_SLOT0=0: off. */
static long g_nm_slot0;
static int nm_slot0(char *buf) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("SSBU_REG_SLOT0"); on = !(e && e[0] == '0'); }
    if (!on) return 0;
    char out[512];
    snprintf(out, sizeof out, "%s", buf);
    int swapped = 0;
    for (char *p = out; *p; p++)
        if ((p[0] == 'c' || p[0] == 'C') && p[1] == '0' && p[2] >= '1' && p[2] <= '7' && (p == out || !nm_token_char(p[-1])) && !nm_token_char(p[3])) {
            p[2] = '0';
            swapped++;
        }
    if (!swapped || !nm_has_path(nm_hash40(out))) return 0;
    memcpy(buf, out, strlen(out) + 1);
    g_nm_slot0++;
    return 1;
}
/* N8: the donor's name in a path while a new fighter is set up: the fighter's own path when the data has it
 * and the text can take it (as long, or the engine's fixed string: 256 bytes + a 16-bit length). SSBU_REG_OWNPATH=0: off. */
static long g_nm_ownpath, g_nm_ownpath_no;
static void nm_owner_set(const char *name) { t_nm_owner = name ? nm_find(name) : NULL; }
static int nm_owner_path(char *buf, u64 lr) {
    NewMod *nm = t_nm_owner;
    static int on = -1;
    if (on < 0) { const char *e = getenv("SSBU_REG_OWNPATH"); on = !(e && e[0] == '0'); }
    if (!nm || !on || !strchr(buf, '/')) return 0;
    size_t ln = strlen(nm->f.name), ld = strlen(nm->f.donor), n = strlen(buf);
    if (n >= 250 || nm_stristr(buf, nm->f.name)) return 0;
    char out[512];
    size_t k = 0;
    int swapped = 0;
    for (const char *p = buf; *p && k + ln + 2 < sizeof out;) {
        if (!_strnicmp(p, nm->f.donor, ld) && (p == buf || !nm_token_char(p[-1])) && !nm_token_char(p[ld])) {
            int cap = p[0] >= 'A' && p[0] <= 'Z';
            memcpy(out + k, nm->f.name, ln);
            if (cap && out[k] >= 'a' && out[k] <= 'z') out[k] -= 32;
            k += ln; p += ld; swapped++;
        } else out[k++] = *p++;
    }
    out[k] = 0;
    if (!swapped) return 0;
    u64 h = nm_hash40(out);
    if (!nm_has_path(h)) {
        if (!nm_slot0(out)) return 0;   /* (another colour: the fighter's own c00) */
        h = nm_hash40(out);
    }
    u16 *len = (u16 *)(buf + 256);
    int fixed = nm_writable(len) && *len == (u16)n, known = nm_seen(h);
    if (k > n && !fixed) {
        g_nm_ownpath_no++;
        if (!known) nm_log("[registry] path asked with the DONOR's name while %s is set up: %s (by %llx): the data has %s, but the text cannot be lengthened here", nm->f.name, buf,
                           (unsigned long long)lr, out);
        return 0;
    }
    g_nm_ownpath++;
    if (!known) nm_log("[registry] path asked with the DONOR's name while %s is set up: %s (by %llx) -> the fighter's OWN %s", nm->f.name, buf, (unsigned long long)lr, out);
    memcpy(buf, out, k + 1);
    if (fixed) *len = (u16)k;
    return 1;
}
static void nm_path(char *buf, u64 lr) {
    if (t_nm_owner && nm_owner_path(buf, lr)) return;
    for (int i = 0; i < g_nm_count; i++) {
        NewMod *nm = &g_nm[i];
        size_t ln = strlen(nm->f.name), ld = strlen(nm->f.donor);
        char *p = nm_stristr(buf, nm->f.name);
        if (!p || !strchr(buf, '/')) continue;
        char was[512];
        snprintf(was, sizeof was, "%s", buf);
        u64 h_new = nm_hash40(buf);
        int kind = nm_has_path(h_new);
        if (kind) {   /* the data has the path under the new name (a file, a directory, a folder) */
            if (!nm_seen(h_new)) {
                nm->paths++; nm->paths_own++;
                nm_log("[registry] path asked: %s (hash %llx, by %llx) -> OWN %s of %s in the data; so far %ld own, %ld donor fallbacks", buf, (unsigned long long)h_new,
                       (unsigned long long)lr, nm_kind_text[kind], nm->f.name, nm->paths_own, nm->paths - nm->paths_own);
            }
            return;
        }
        if (nm_slot0(buf)) {   /* another colour the data does not have: the fighter's own c00 */
            if (!nm_seen(h_new)) {
                nm->paths++; nm->paths_own++;
                nm_log("[registry] path asked: %s (hash %llx, by %llx) -> not in the data -> the fighter's OWN c00: %s", was, (unsigned long long)h_new, (unsigned long long)lr, buf);
            }
            return;
        }
        int swapped = 0;
        while (p) {
            int tok = (p == buf || !nm_token_char(p[-1])) && !nm_token_char(p[ln]);
            if (tok && ld <= ln) {
                int cap = p[0] >= 'A' && p[0] <= 'Z';
                memmove(p + ld, p + ln, strlen(p + ln) + 1);
                memcpy(p, nm->f.donor, ld);
                if (cap && p[0] >= 'a' && p[0] <= 'z') p[0] -= 32;
                swapped++;
                p = nm_stristr(p + ld, nm->f.name);
            } else p = nm_stristr(p + 1, nm->f.name);
        }
        if (!swapped) continue;
        u16 *len = (u16 *)(buf + 256);   /* the engine's fixed strings: 256 bytes of text, then a 16-bit length it appends at */
        if (nm_writable(len) && *len == (u16)strlen(was)) *len = (u16)strlen(buf);
        if (!nm_seen(h_new)) {
            i64 fi = nm_arc_file_index(nm_hash40(buf));
            nm->paths++;
            if (fi < 0) nm->paths_missing++;
            nm_log("[registry] path asked: %s (hash %llx, by %llx) -> not in the data -> DONOR FALLBACK: %s: %s", was, (unsigned long long)h_new, (unsigned long long)lr, buf,
                   fi >= 0 ? "found" : fi == -1 ? "NOT a file path of the data (a folder, a stream, or missing)" : "(archive tables not ready)");
        }
        return;
    }
}
static void nm_h_fmt(Ctx *c, void *user) {
    (void)user;
    u64 buf = c->x[0], lr = c->x[30];
    if (lr == NM_WORKER_RET && c->x[4] > 0x10000) {
        const char *name = P(const char, c->x[4]);
        u64 req = c->x[4] - 0x30;   /* (x4 = request + 0x30: lifted f_71022b6620) */
        NewMod *nm = nm_find(name);
        if (nm) {
            nm->requests++;
            if (!rt_module_named(nm->donor_module, NULL, NULL)) {
                c->x[4] = (u64)(uintptr_t)nm->f.donor;   /* the donor's image is a dependency: this request loads it */
                nm->substituted++;
                nm_log("[registry] module request lua2cpp_%s (request object %llx): answered by OUR module (no NRO of that name); its donor image %s is not loaded: "
                       "this request loads it", name, (unsigned long long)req, nm->donor_module);
                g_nm_o_fmt(c);
                return;
            }
            *P(u8, req + 0x132) = 1;   /* "loaded", as the worker writes it */
            *P(char, buf) = 0;         /* an empty path: the worker goes back to its queue */
            nm->answered++;
            nm_log("[registry] module request lua2cpp_%s (request object %llx): answered by OUR module (no NRO of that name); donor image %s already loaded", name,
                   (unsigned long long)req, nm->donor_module);
            return;
        }
        g_nm_o_fmt(c);
        return;
    }
    g_nm_o_fmt(c);
    if (buf > 0x10000) nm_path(P(char, buf), lr);
}
static ImportFn g_nm_o_path[sizeof g_nm_path_fn / sizeof g_nm_path_fn[0]];
static void nm_h_pathfmt(Ctx *c, void *user) {
    u64 buf = c->x[0], lr = c->x[30];
    int i = (int)(intptr_t)user;
    if (i >= NM_PATH_PRE) { if (c->x[1] > 0x10000) nm_path(P(char, c->x[1]), lr); g_nm_o_path[i](c); return; }
    g_nm_o_path[i](c);
    if (buf > 0x10000) nm_path(P(char, buf), lr);
}

/* N9: the picture files of a new roster row: the donor's picture path answered with the fighter's own
 * when the data has it (ui/replace/chara/chara_<type>/chara_<type>_<name>_<cc>.bntx). SSBU_REG_PORTRAIT=0: off. */
static ImportFn g_nm_o_portrait;
static long g_nm_portrait_calls, g_nm_portrait_own;
static void nm_h_portrait(Ctx *c, void *user) {
    (void)user;
    u64 id = c->x[0], lr = c->x[30];
    u32 colour = (u32)c->x[1], type = (u32)c->x[2];
    g_nm_o_portrait(c);
    static int on = -1;
    if (on < 0) { const char *e = getenv("SSBU_REG_PORTRAIT"); on = !(e && e[0] == '0'); }
    for (int i = 0; i < g_nm_count; i++) {
        NewMod *nm = &g_nm[i];
        char t[320], own[320] = "";
        snprintf(t, sizeof t, "ui_chara_%s", nm->f.name);
        if ((id & 0xffffffffffull) != nm_hash40(t)) continue;
        u64 got = c->x[0], hd = 0;
        int found = -1, cc = -1, asked_own = 0;
        for (int ty = 0; ty < 16 && found < 0; ty++)
            for (int col = 0; col < 16 && found < 0; col++)
                for (int patch = 0; patch < 2; patch++) {
                    char ask[320];
                    snprintf(t, sizeof t, "ui/%s/chara/chara_%d/chara_%d_%s_%02d.bntx", patch ? "replace_patch" : "replace", ty, ty, nm->f.donor, col);
                    hd = nm_hash40(t);
                    snprintf(own, sizeof own, "ui/replace/chara/chara_%d/chara_%d_%s_%02d.bntx", ty, ty, nm->f.name, col);
                    snprintf(ask, sizeof ask, "ui/%s/chara/chara_%d/chara_%d_%s_%02d.bntx", patch ? "replace_patch" : "replace", ty, ty, nm->f.name, col);
                    if (hd == (got & 0xffffffffffull)) { found = ty; cc = col; break; }
                    if (nm_hash40(ask) == (got & 0xffffffffffull)) { found = ty; cc = col; asked_own = 1; break; }
                }
        int has = 0;
        u64 h = 0;
        if (found >= 0) {
            h = nm_hash40(own);
            has = nm_has_file(h);
            if (!has && cc > 0) {   /* no picture of this colour: the fighter's own colour 00, when it has one */
                char own0[320];
                snprintf(own0, sizeof own0, "ui/replace/chara/chara_%d/chara_%d_%s_00.bntx", found, found, nm->f.name);
                if (nm_has_file(nm_hash40(own0))) { h = nm_hash40(own0); has = 1; }
            }
            if (asked_own && !has) { c->x[0] = (got & ~0xffffffffffull) | hd; got = c->x[0]; }   /* no own file: the donor's picture */
        }
        g_nm_portrait_calls++;
        static u64 seen[256];
        static int nseen;
        int known = 0;
        u64 key = ((u64)i << 48) | ((u64)type << 32) | ((u64)colour << 16) | (u64)(has != 0);
        for (int k = 0; k < nseen; k++) known |= seen[k] == key;
        if (!known && nseen < 256) {
            seen[nseen++] = key;
            if (found < 0) nm_log("[registry] picture path asked for ui_chara_%s (colour %u, type %u, by %llx): answer %llx is not a picture path of the donor %s: left as it is",
                                  nm->f.name, colour, type, (unsigned long long)lr, (unsigned long long)got, nm->f.donor);
            else nm_log("[registry] picture path asked for ui_chara_%s (colour %u, type %u, by %llx): the row's answer is the donor's chara_%d_%s_%02d.bntx -> %s", nm->f.name, colour,
                        type, (unsigned long long)lr, found, nm->f.donor, cc, has && on ? "the fighter's OWN file in the data" : "the donor's picture");
        }
        if (has && on) { c->x[0] = (got & ~0xffffffffffull) | h; g_nm_portrait_own++; }
        return;
    }
}

/* N9: the name text of a new roster row (manifest "display_name"): the row's name_id is answered with the
 * fighter's own name, and a label "nam_chr..._<name>" with the display name. */
static ImportFn g_nm_o_nameid, g_nm_o_msg[2];
static int nm_mem_ok(u64 a, size_t n) {
    MEMORY_BASIC_INFORMATION mi;
    if (a < 0x10000 || !VirtualQuery(G2H(a), &mi, sizeof mi)) return 0;
    if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    return (u64)(uintptr_t)G2H(a) + n <= (u64)(uintptr_t)mi.BaseAddress + mi.RegionSize;
}
static void nm_h_nameid(Ctx *c, void *user) {
    (void)user;
    u64 out = c->x[0], id = c->x[2], lr = c->x[30];
    g_nm_o_nameid(c);
    for (int i = 0; i < g_nm_count; i++) {
        NewMod *nm = &g_nm[i];
        char t[96];
        if (!nm->display[0]) continue;
        snprintf(t, sizeof t, "ui_chara_%s", nm->f.name);
        if ((id & 0xffffffffffull) != nm_hash40(t) || !nm_mem_ok(out, 0x48)) continue;
        char *text = P(char, out + 8);
        i32 *len = P(i32, out + 4);
        size_t was = strnlen(text, 0x40);
        if (was >= 0x40 || (i32)was != *len) {
            if (nm->nameid_calls++ < 3) nm_log("[registry] name_id of ui_chara_%s (by %llx): the answer does not have the expected shape: left as it is", nm->f.name, (unsigned long long)lr);
            return;
        }
        if (nm->nameid_calls++ < 4) nm_log("[registry] name_id asked for ui_chara_%s (by %llx): the row's \"%s\" -> the fighter's own name \"%s\" (display name \"%s\")", nm->f.name,
                                           (unsigned long long)lr, text, nm->f.name, nm->display);
        memcpy(text, nm->f.name, strlen(nm->f.name) + 1);
        *len = (i32)strlen(nm->f.name);
        u32 hs = 0x811c9dc5u;   /* out + 0: the string's own checksum, as the engine's append recomputes it */
        for (const char *q = nm->f.name; *q; q++) hs = (hs * 0x89u) ^ (u8)*q;
        *P(u32, out) = hs;
        return;
    }
}
static void nm_h_msg(Ctx *c, void *user) {
    int get = (int)(intptr_t)user;
    u64 label = c->x[1], lr = c->x[30];
    if (nm_mem_ok(label, 12) && !memcmp(P(const char, label), "nam_chr", 7)) {
        const char *s = P(const char, label);
        size_t n = strnlen(s, 96);
        for (int i = 0; i < g_nm_count && n < 96; i++) {
            NewMod *nm = &g_nm[i];
            size_t ln = strlen(nm->f.name);
            if (!nm->display[0] || n <= ln + 1 || s[n - ln - 1] != '_' || strcmp(s + n - ln, nm->f.name)) continue;
            if (nm->text_calls++ < 12) nm_log("[registry] message label %s %s (by %llx): the fighter's display name \"%s\"", s, get ? "text asked" : "existence asked",
                                              (unsigned long long)lr, nm->display);
            c->x[0] = get ? (u64)(uintptr_t)nm->display16 : 1;
            return;
        }
    }
    g_nm_o_msg[get](c);
}

/* registry.h start(): the components call it from their mod_init */
static void nm_start(void) {
    if (g_nm_started || !g_nm_count) return;
    g_nm_started = 1;
    g_api = native_mods_api();
    int a = g_api->install(NM_FORMATTER, nm_h_fmt, NULL, &g_nm_o_fmt);
    if (!a) nm_log("[registry] path formatter: %s", g_api->error());
    int b = g_api->install_name("_ZN3lib8L2CAgent20sv_set_function_hashEPvN3phx6Hash40E", nm_h_fnhash, NULL, &g_nm_o_fnhash);
    int d = g_api->install_name("_ZN7lua2cpp12L2CAgentBase18sv_set_status_funcERKN3lib8L2CValueES4_Pv", nm_h_status, NULL, &g_nm_o_status);
    int e = 0;
    for (int i = 0; i < (int)(sizeof g_nm_path_fn / sizeof g_nm_path_fn[0]); i++) e += !!g_api->install(g_nm_path_fn[i], nm_h_pathfmt, (void *)(intptr_t)i, &g_nm_o_path[i]);
    int pt = g_api->install(NM_PORTRAIT, nm_h_portrait, NULL, &g_nm_o_portrait);
    int any_display = 0;
    for (int i = 0; i < g_nm_count; i++) any_display |= g_nm[i].display[0] != 0;
    if (any_display) {
        int n1 = g_api->install(NM_NAMEID, nm_h_nameid, NULL, &g_nm_o_nameid);
        int n2 = g_api->install(NM_MSG_HAS, nm_h_msg, (void *)(intptr_t)0, &g_nm_o_msg[0]);
        int n3 = g_api->install(NM_MSG_GET, nm_h_msg, (void *)(intptr_t)1, &g_nm_o_msg[1]);
        nm_log("[registry] name text hooks (a manifest has \"display_name\"): name_id %s, label exists %s, label text %s", n1 ? "installed" : "FAILED", n2 ? "installed" : "FAILED",
               n3 ? "installed" : "FAILED");
    }
    int fr = 0;
    const char *ef = getenv("SSBU_REG_FRAME");
    if (!(ef && ef[0] == '0')) fr = g_api->install_name("_ZN7lua2cpp16L2CFighterCommon31sys_line_system_control_fighterEv", nm_h_frame, NULL, &g_nm_o_frame);
    nm_log("[registry] runtime hooks: path formatter %#llx %s, sv_set_function_hash %s, sv_set_status_func %s, %d of %d other path builders, picture path %s, "
           "per-frame line %s", (unsigned long long)NM_FORMATTER, a ? "installed" : "FAILED", b ? "installed" : "FAILED", d ? "installed" : "FAILED", e,
           (int)(sizeof g_nm_path_fn / sizeof g_nm_path_fn[0]), pt ? "installed" : "FAILED", fr ? "installed (or waiting for its module)" : "off");
}
/* version 4's own article agent: the kind component makes it itself (its own_article_agent) */
static int nm_article_agent(void *ctx, const char *name, const char *article, int category) {
    (void)ctx; (void)name; (void)article; (void)category;
    return 0;
}

__declspec(dllexport) const SsbuRegistry *ssbu_registry(void) {
    static SsbuRegistry r;
    fighter_registry_scan();
    r = (SsbuRegistry){sizeof(SsbuRegistry), 4, g_nm_count, g_nm_list, nm_start, nm_agent_begin, nm_agent_end, nm_owner_set, nm_agent_begin_weapon, nm_article_agent};
    return &r;
}
#else
void fighter_registry_scan(void) {}
#endif
