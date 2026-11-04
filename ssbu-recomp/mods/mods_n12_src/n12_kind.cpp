// n4_kind.cpp - TEST MOD for plan\phases\N4_own_fighter.md. NOT part of the port. Successor of mods_n3_src\n3_kind.cpp.
// New fighter kinds 0x5E.. whose engine-side things are a donor's (Mario), with the WEAPONS' OWNER RESOLVED FROM THE
// LIVE OWNER instead of the static table weapon kind -> fighter kind. Needs the game DLL built with patches\kinds.txt.
//   N3_NEWKIND=<name>[,<name2>...]   switches the mod on: kind 0x5E = <name>, 0x5F = <name2> ... (hash fighter_kind_<name>)
//   N4_A0_WORKAROUND=1               no donor dependency and no live owner for the weapons (to show what breaks)
//   N4_OWNNAME=1                     the name rows of the new kinds become their own strings, the kind-hash row their own hash
//   N4_WEAPON_LOG=<n>                log the first n weapon inits in full (default 12)
// Build:  python fighter_sdk\tools\build_mod.py mods_n4_src\n4_kind.cpp mods_n4        Run: SSBU_MODS=mods_n4
//
// Addresses of 13.0.4. Beyond n3_kind.cpp:
//   weapon init             0x71033b5640 (weapon object, init description; weapon kind at description + 0x9c)
//       reads the STATIC owner kind from the int table 0x710455d7e4[weapon kind] and takes A[owner] + 0x70
//   weapon agent dispatchers 0x71033abee0, 0x71033acde0, 0x71033add40, 0x71033aeca0 (weapon object; kind at + 0xc)
//       jump by the same table to the owner's script module
//   These five functions are the only code that refers to that table. The hooks run the original with the table
//   row of the weapon kind set to the LIVE owner's kind for the duration of the call (under one lock).
//   fireball article function 0x7100cb9de0 (article, owner's module accessor: battle object id at + 8): counted per owner kind
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <windows.h>
#define PSAPI_VERSION 2
#include <psapi.h>                 // N13: K32EnumProcessModules (kernel32): the fighter DLL's private copy
#include "hook.h"
#include "mod_ga.h"            // 13.0.5: game addresses through MOD_GA
#include "registry.h"

// N5 (plan\phases\N5_own_module.md): this is n4_kind.cpp as a COMPONENT OF THE FIGHTER REGISTRY. With
// SSBU_CHARACTERS the new kinds are the registry's fighters (names from the manifests, own names on), and every
// agent a donor factory makes for a new kind is handed to the runtime (agent_begin / agent_end), which moves it into
// the new fighter's own script module. Without a registry it behaves as n4_kind.cpp (N3_NEWKIND, N4_OWNNAME).
static const SsbuRegistry *g_reg;

static const HookApi *api;
static const uint64_t MASK40 = 0xffffffffffull;
// N6 (task 3): THE DONOR IS PER NEW KIND, from the manifest's "donor" (N5 had DONOR = 0 = Mario built in). The donor's
// kind number is found by its name in the engine's own name table (0x7104f80e20: one string pointer per kind, the
// table whose rows 0x5E.. this component writes): dk(new kind) = that row index. Without a registry (N3_NEWKIND) the
// donor is N3_DONOR, default mario.
// U1 (2026-10-16): MAXNEW 4 -> 8 = the registry's NM_MAX (fighter_sdk\newmodule_host.c); own weapon kinds stay
// 640 + 8 * fighter + article < 640 + NEW_WK_MAX (64). The game DLL must be the kinds-patched one (patches\kinds.txt,
// 0x72 kinds = at most 20 new): ssbu_game_p15n3.dll. An exe linked against an unpatched DLL (ssbu_game_eh_o1.dll:
// ssbu_tutto.exe, ssbu_n13.exe) reads B / A / container 2 outside the singleton: garbage rows, no parameter objects,
// abort() in the fighter init (run both3).
static const int FIRST = 0x5E, MAXNEW = 16;   // U16: 8 -> 16 = the spare rows of the engine's kind tables (0x5E-0x6D; 0x6E.. are the special kinds)
static const uint64_t NAME_TABLE = MOD_GA(0x7104f80e20ull);
static int g_donor[MAXNEW] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
static char g_donor_name[MAXNEW][32];
static const uint64_t NEW_B = 0x19a8;              // array B of the patched singleton (patches\kinds.txt)
static const uint64_t OWNER_TABLE = MOD_GA(0x710455d7e4ull);   // int32 per weapon kind (617 rows)
static const int WEAPON_KINDS = 617;
static int g_nnew;
static uint64_t g_hash[MAXNEW];
static char g_name[MAXNEW][32], g_upper[MAXNEW][32], g_sound[MAXNEW][32];
static volatile LONG g_data_done;
static int g_a0_workaround, g_ownname, g_weapon_log = 12;
static CRITICAL_SECTION g_lock;

static void logf(const char *fmt, ...) {
    char line[500]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    api->log(line);
}
static uint32_t crc32(const char *s) {
    uint32_t c = 0xffffffffu;
    for (; *s; s++) { c ^= (uint8_t)*s; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1))); }
    return ~c;
}
static uint64_t hash40(const char *s) { return ((uint64_t)strlen(s) << 32) | crc32(s); }
// 2026-10-16 (owner: fights at 30-40 fps with big stutters in the two-new-fighter build; main thread 18-26 ms per
// frame against ~3 in the plain build): readable() was a VirtualQuery on every call, and it runs inside hooks the
// fight calls thousands of times per frame (h_cb on the donor's 146 callbacks probes up to four arguments each call,
// h_wparam on every get_param of every object once an own article exists, h_kindfield). VirtualQuery takes the
// process's address-space lock (contention with the renderer's GetWriteWatch too). Now every region VirtualQuery
// reports - readable or not - is remembered for the current system tick (about 16 ms) and answered from the memo;
// the memo is emptied at each new tick, so a region freed or decommitted is seen again within one tick.
typedef struct { uint64_t lo, hi; int ok; } RdRange;
static SRWLOCK g_rd_lock = SRWLOCK_INIT;
static RdRange g_rd[128];
static int g_rd_n;
static ULONGLONG g_rd_tick;
static int readable(uint64_t a, size_t n) {
    if (a < 0x10000) return 0;
    ULONGLONG t = GetTickCount64();
    int hit = -1;
    AcquireSRWLockShared(&g_rd_lock);
    if (t == g_rd_tick)
        for (int k = 0; k < g_rd_n; k++)
            if (a >= g_rd[k].lo && a < g_rd[k].hi) { hit = g_rd[k].ok && a + n <= g_rd[k].hi; break; }
    ReleaseSRWLockShared(&g_rd_lock);
    if (hit >= 0) return hit;
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi)) return 0;
    int ok = mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
    uint64_t lo = (uint64_t)(uintptr_t)mi.BaseAddress, hi = lo + mi.RegionSize;
    AcquireSRWLockExclusive(&g_rd_lock);
    if (t != g_rd_tick) { g_rd_n = 0; g_rd_tick = t; }
    if (g_rd_n < 128) { g_rd[g_rd_n].lo = lo; g_rd[g_rd_n].hi = hi; g_rd[g_rd_n].ok = ok; g_rd_n++; }
    ReleaseSRWLockExclusive(&g_rd_lock);
    return ok && a + n <= hi;
}
static int is_new(uint32_t kind) { return kind >= (uint32_t)FIRST && kind < (uint32_t)(FIRST + g_nnew); }
static void set_row(uint64_t table, int row, uint64_t value, const char *what) {
    uint64_t *t = (uint64_t *)(uintptr_t)table; DWORD old;
    if (!readable(table, 8 * (row + 1))) { logf("[n4] kind: table %s %llx not readable", what, (unsigned long long)table); return; }
    uint64_t was = t[row];
    VirtualProtect(&t[row], 8, PAGE_READWRITE, &old);
    t[row] = value;
    logf("[n4] kind: table %s %llx row 0x%x: %llx -> %llx", what, (unsigned long long)table, row, (unsigned long long)was, (unsigned long long)value);
}
// the donor's kind of new kind FIRST + n: the row of the engine's name table whose string is the donor's name
static void resolve_donors(void) {
    static volatile LONG done;
    if (done) return;
    if (!readable(NAME_TABLE, 8 * FIRST)) return;
    int all = 1;
    for (int n = 0; n < g_nnew; n++) {
        if (g_donor[n] >= 0) continue;
        for (int k = 0; k < FIRST; k++) {
            uint64_t s = ((uint64_t *)(uintptr_t)NAME_TABLE)[k];
            if (readable(s, 32) && strcmp((const char *)(uintptr_t)s, g_donor_name[n]) == 0) { g_donor[n] = k; break; }
        }
        if (g_donor[n] >= 0) logf("[n6] kind 0x%x (%s): donor %s = kind 0x%x (row of the engine's name table %llx)", FIRST + n, g_name[n], g_donor_name[n], g_donor[n], (unsigned long long)NAME_TABLE);
        else all = 0;
    }
    if (all) InterlockedExchange(&done, 1);
}
static int dk(uint32_t kind) {                     // donor kind of a new kind
    int n = (int)kind - FIRST;
    if (n < 0 || n >= g_nnew) return 0;
    if (g_donor[n] < 0) {
        resolve_donors();
        if (g_donor[n] < 0) {
            static long said;
            if (said++ < 4) logf("[n6] kind 0x%x (%s): the donor name %s is NOT in the engine's name table: kind 0 (mario) is used", kind, g_name[n], g_donor_name[n]);
            return 0;
        }
    }
    return g_donor[n];
}
static void ensure_data(void) {
    if (InterlockedCompareExchange(&g_data_done, 1, 0)) return;
    static const uint64_t tab[4] = { MOD_GA(0x7104f80e20ull), MOD_GA(0x7104f811d0ull), MOD_GA(0x7104f80a70ull), MOD_GA(0x710453b1c0ull) };
    static const char *what[4] = { "names", "sound names", "upper-case names", "kind hashes" };
    resolve_donors();
    for (int n = 0; n < g_nnew; n++) {
        const int DONOR = dk(FIRST + n);
        for (int t = 0; t < 4; t++) {
            uint64_t v = ((uint64_t *)(uintptr_t)tab[t])[DONOR];
            // N4_OWNNAME is a bit mask: 1 name (files, script module), 2 sound name, 4 upper-case name, 8 kind hash.
            // The strings live in this DLL: host addresses, which the recompiled game reads as they are.
            if (t == 0 && (g_ownname & 1)) v = (uint64_t)(uintptr_t)g_name[n];
            if (t == 1 && (g_ownname & 2)) v = (uint64_t)(uintptr_t)g_sound[n];
            if (t == 2 && (g_ownname & 4)) v = (uint64_t)(uintptr_t)g_upper[n];
            if (t == 3 && (g_ownname & 8)) v = g_hash[n];
            set_row(tab[t], FIRST + n, v, what[t]);
        }
    }
    DWORD old;
    VirtualProtect((void *)(uintptr_t)OWNER_TABLE, 4 * WEAPON_KINDS, PAGE_READWRITE, &old);
}
// N5: the weapon agent dispatchers (0x71033abee0 ..) take the owner kind of the weapon kind from the same table that
// the weapon init is lent (the live owner), build the agent name "<owner's name>_<weapon>" from it and jump by it:
// for a new kind that is a name no factory knows and no case of the jump table, so the weapon got NO agents at all
// (found in N5: a new kind's fireball spawned, flew, and never ran a script or hit anything). During the dispatcher
// the row is the weapon kind's static owner again; the agent the donor's factory returns is then moved to the live
// owner's module by the registry.
static int static_owner(int wk) {
    static int32_t keep[617]; static volatile LONG have;
    if (!InterlockedCompareExchange(&have, 1, 0)) memcpy(keep, (void *)(uintptr_t)OWNER_TABLE, sizeof keep);
    return wk >= 0 && wk < WEAPON_KINDS ? keep[wk] : -1;
}

// ---- live fighters and weapons ---------------------------------------------------------------------------------
struct Seen { uint64_t obj; int kind; uint32_t id; };   // (id: the object id at + 8, read once while the fighter is live)
static Seen g_fighter[16], g_weapon[512];
static int g_nfighter, g_nweapon;
static __declspec(thread) int t_init_kind = -1;        // kind of the fighter init running on this thread
static int trace_kind(void) { return t_init_kind; }
static int entry_kind(int slot) {                     // FighterManager -> entry[slot] + 0x88 (tests\DEBUG_MODE.md)
    if (slot < 0 || slot > 7) return -1;
    uint64_t mgr = *(uint64_t *)(uintptr_t)MOD_GA(0x71052b84f8ull);
    if (!readable(mgr, 8)) return -2;
    uint64_t impl = *(uint64_t *)(uintptr_t)mgr;
    if (!readable(impl + 0x20 + 8 * slot, 8)) return -3;
    uint64_t e = *(uint64_t *)(uintptr_t)(impl + 0x20 + 8 * slot);
    if (!readable(e + 0x88, 4)) return -4;
    return *(int32_t *)(uintptr_t)(e + 0x88);
}
static void remember(Seen *tab, int *n, int cap, uint64_t obj, int kind) {
    for (int i = 0; i < *n; i++) if (tab[i].obj == obj) { tab[i].kind = kind; return; }
    for (int i = 0; i < *n; i++) if (tab[i].obj == 0) { tab[i].obj = obj; tab[i].kind = kind; tab[i].id = 0; return; }   // (a slot freed by a newer fighter of its id)
    if (*n < cap) { tab[*n].obj = obj; tab[*n].kind = kind; tab[*n].id = 0; (*n)++; }
}
static int recall(Seen *tab, int n, uint64_t obj) {
    for (int i = 0; i < n; i++) if (tab[i].obj == obj) return tab[i].kind;
    return -1;
}

static HookOrig o_hash, o_record, o_callbacks, o_disp[9], o_build[2], o_get[2], o_loader, o_init, o_setup, o_winit, o_wdisp[4], o_fireball;
static long g_n_hash, g_n_record, g_n_callbacks, g_n_disp[9], g_n_build[2], g_n_get[2], g_n_init, g_n_winit, g_n_wdisp[4], g_n_swap, g_n_fire[0x80];

static void h_hash(Ctx *c, void *) {               // hash40 -> kind
    ensure_data();
    uint64_t h = c->x[0] & MASK40;
    for (int n = 0; n < g_nnew; n++)
        if (h == g_hash[n]) { if (!g_n_hash++) logf("[n4] kind: hash -> kind: %s -> 0x%x", g_name[n], FIRST + n); c->x[0] = FIRST + n; return; }
    o_hash(c);
}
static void h_w0(Ctx *c, HookOrig o, long *n, const char *what) {      // functions taking the kind in w0
    if (is_new((uint32_t)c->x[0])) {
        int d = dk((uint32_t)c->x[0]);
        if ((*n)++ < 2) logf("[n4] kind: %s asked for kind 0x%x -> the donor's (kind 0x%x)", what, (unsigned)c->x[0], d);
        c->x[0] = (uint64_t)d;
    }
    o(c);
}
// ---- N12 task 4 (step 2 of the work order): THE NEW FIGHTER'S OWN DESCRIPTION RECORD WITH APPENDED ARTICLE ROWS.
// Read from the image with our own tools (plan\phases\N12_fighter_extensions.md section 4): the record returned by
// 0x710064b730(kind) is static data; record + 0 = the fighter kind, record + 8 -> the article list header
// { rows, count }; a row is 0x20 bytes { int32 weapon kind, int32 max alive, three function pointers (the native
// article functions; the first is e.g. the fireball's 0x7100cb9de0) }; the row index is the generate-article id.
// A fighter without articles points to a shared empty header {0, 0} (Marth: 0x710453d8c8).
// fighter.json "articles": [ {"name": "<article>", "template": "<owner>_<article>", "max": n}, ... ]: each entry is
// one more row {NEW weapon kind, max, the template row's three functions}; generate id = donor's count + index.
// New weapon kinds: 640 + n (NEW_WK_BASE; the game's weapon kinds are 0 .. 614: the owner table and both name tables
// have 615 rows, the description lookup's bound is 614). The record handed out for the new kind is a COPY of the
// donor's (RECORD_COPY bytes) whose + 8 points to the own header. N12_ARTICLES=1: on (default off until the
// weapon-kind lookups are table-driven; WRITTEN, see the phase file for what is run).
static const int NEW_WK_BASE = 640, NEW_WK_MAX = 8 * MAXNEW, RECORD_COPY = 0x400;
static uint64_t game_call2(Ctx *c, uint64_t addr, uint64_t a0, uint64_t a1);
struct NewArticle { char name[32], tmpl[64]; int max, wk, tmpl_wk, owner_kind, resolved, keep_on_hit; uint64_t fn[3]; };
static NewArticle g_art[MAXNEW][8]; static int g_nart[MAXNEW];
static uint8_t *g_own_record[MAXNEW]; static int g_articles_on = -1;
static int weapon_kind_of(const char *owner, const char *weapon) {
    for (int wk = 0; wk < 615; wk++) {
        uint64_t o = ((uint64_t *)(uintptr_t)MOD_GA(0x7105188240ull))[wk], w = ((uint64_t *)(uintptr_t)MOD_GA(0x7105185bd0ull))[wk];
        if (readable(o, 1) && readable(w, 1) && !strcmp((const char *)(uintptr_t)o, owner) && !strcmp((const char *)(uintptr_t)w, weapon)) return wk;
    }
    return -1;
}
static void read_articles(int k) {
    char path[600]; snprintf(path, sizeof path, "%s\\fighter.json", g_reg->fighters[k].folder);
    FILE *f = fopen(path, "rb"); if (!f) return;
    static char js[32768]; size_t n = fread(js, 1, sizeof js - 1, f); fclose(f); js[n] = 0;
    const char *p = strstr(js, "\"articles\""); if (!p) return;
    p = strchr(p, ':'); if (!p) return;
    while (*p == ':' || *p == ' ' || *p == '\r' || *p == '\n' || *p == '\t') p++;
    if (*p != '[') return;                              // "articles": "donor" (the donor's list) - nothing to add
    for (p++; *p && *p != ']' && g_nart[k] < 8; ) {
        const char *o = strchr(p, '{'), *e = o ? strchr(o, '}') : nullptr, *close = strchr(p, ']');
        if (!o || !e || (close && close < o)) break;        // run n12b: without this a '{' AFTER the list made a phantom article
        char obj[400]; size_t ol = (size_t)(e - o) < sizeof obj - 1 ? (size_t)(e - o) : sizeof obj - 1; memcpy(obj, o, ol); obj[ol] = 0;
        NewArticle *a = &g_art[k][g_nart[k]]; memset(a, 0, sizeof *a); a->max = 1;
        const char *q;
        if ((q = strstr(obj, "\"name\"")) && (q = strchr(q + 6, '"'))) sscanf(q + 1, "%31[^\"]", a->name);
        if ((q = strstr(obj, "\"template\"")) && (q = strchr(q + 10, '"'))) sscanf(q + 1, "%63[^\"]", a->tmpl);
        if ((q = strstr(obj, "\"max\"")) && (q = strchr(q, ':'))) a->max = atoi(q + 1);
        // "keep_on_hit": true - the template's native code must not remove the weapon when its hit box connects
        // (it does so unless its is_penetration parameter is set): the article's own status code decides. Waluigi's
        // dice block breaks with its rolled number's effect on any hit - shield, fighter - as the package's plugin
        // does (its status main replaces the template's, so nothing native removes it there).
        if ((q = strstr(obj, "\"keep_on_hit\"")) && (q = strchr(q, ':'))) { q++; while (*q == ' ') q++; a->keep_on_hit = !strncmp(q, "true", 4) || *q == '1'; }
        a->wk = NEW_WK_BASE + k * 8 + g_nart[k];
        g_nart[k]++;
        p = e + 1;
    }
}
static int articles_on(void) {
    if (g_articles_on < 0) { const char *e = getenv("N12_ARTICLES"); g_articles_on = e && e[0] == '1'; }
    return g_articles_on;
}
static void resolve_articles(int n) {                  // template weapon kind and its owner kind, by the names
    for (int i = 0; i < g_nart[n]; i++) {
        NewArticle *a = &g_art[n][i];
        if (a->resolved) continue;
        char owner[64]; snprintf(owner, sizeof owner, "%s", a->tmpl);
        char *us = strchr(owner, '_'); const char *weapon = us ? us + 1 : "";
        if (us) *us = 0;
        a->tmpl_wk = weapon_kind_of(owner, weapon);
        a->owner_kind = a->tmpl_wk >= 0 ? ((int32_t *)(uintptr_t)OWNER_TABLE)[a->tmpl_wk] : -1;
        a->resolved = 1;
    }
}
static void build_own_record(int n, uint64_t donor_record) {
    if (g_own_record[n] || !g_nart[n] || !readable(donor_record, RECORD_COPY)) return;
    uint64_t dh = *(uint64_t *)(uintptr_t)(donor_record + 8);
    uint64_t drows = readable(dh, 16) ? *(uint64_t *)(uintptr_t)dh : 0; int dcount = readable(dh, 16) ? (int)*(uint64_t *)(uintptr_t)(dh + 8) : 0;
    int total = dcount + g_nart[n];
    uint8_t *rec = (uint8_t *)VirtualAlloc(nullptr, RECORD_COPY + 16 + 0x20 * (size_t)total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!rec) return;
    memcpy(rec, (void *)(uintptr_t)donor_record, RECORD_COPY);
    uint64_t *hdr = (uint64_t *)(rec + RECORD_COPY); uint8_t *rows = rec + RECORD_COPY + 16;
    if (dcount > 0 && readable(drows, 0x20 * (size_t)dcount)) memcpy(rows, (void *)(uintptr_t)drows, 0x20 * (size_t)dcount);
    char shown[400]; int sk = 0; shown[0] = 0;
    for (int i = 0; i < g_nart[n]; i++) {
        NewArticle *a = &g_art[n][i];
        uint8_t *r = rows + 0x20 * (dcount + i);
        memset(r, 0, 0x20);
        // N12_ROW_TEMPLATE_KIND=1 (diagnosis, runs n12k): the row carries the TEMPLATE's weapon kind instead of the own one
        static int tk = -1; if (tk < 0) { const char *e = getenv("N12_ROW_TEMPLATE_KIND"); tk = e && e[0] == '1'; }
        *(int32_t *)r = tk && a->tmpl_wk >= 0 ? a->tmpl_wk : a->wk; *(int32_t *)(r + 4) = a->max;
        memcpy(r + 8, a->fn, 24);
        if (sk < 360) sk += snprintf(shown + sk, sizeof shown - sk, " [id %d: %s = weapon kind %d, max %d, template %s = weapon kind %d of owner kind %d, functions %llx %llx %llx]", dcount + i, a->name, a->wk, a->max,
                                     a->tmpl, a->tmpl_wk, a->owner_kind, (unsigned long long)a->fn[0], (unsigned long long)a->fn[1], (unsigned long long)a->fn[2]);
    }
    hdr[0] = (uint64_t)(uintptr_t)rows; hdr[1] = (uint64_t)total;
    *(uint64_t *)(rec + 8) = (uint64_t)(uintptr_t)hdr;
    g_own_record[n] = rec;
    logf("[n12] kind 0x%x (%s): OWN description record %p (copy of the donor's %llx), article list: %d of the donor + %d own:%s", FIRST + n, g_name[n], (void *)rec, (unsigned long long)donor_record,
         dcount, g_nart[n], shown);
}
static void h_record(Ctx *c, void *) {
    uint32_t kind = (uint32_t)c->x[0];
    h_w0(c, o_record, &g_n_record, "description record");
    if (!articles_on() || !is_new(kind)) return;
    int n = (int)kind - FIRST;
    if (!g_own_record[n]) {
        // the template rows' functions: the template owner's record, read through the same (original) lookup
        resolve_articles(n);
        for (int i = 0; i < g_nart[n]; i++) {
            NewArticle *a = &g_art[n][i];
            int wk = a->tmpl_wk, ok_ = a->owner_kind;
            if (ok_ < 0) continue;
            uint64_t keep[31]; memcpy(keep, c->x, sizeof keep);
            uint64_t trec = game_call2(c, MOD_GA(0x710064b730ull), (uint64_t)ok_, 0);
            memcpy(c->x, keep, sizeof keep);
            uint64_t th = readable(trec + 8, 8) ? *(uint64_t *)(uintptr_t)(trec + 8) : 0;
            uint64_t trows = readable(th, 16) ? *(uint64_t *)(uintptr_t)th : 0; int tc = readable(th, 16) ? (int)*(uint64_t *)(uintptr_t)(th + 8) : 0;
            for (int r = 0; r < tc && readable(trows + 0x20ull * r, 0x20); r++)
                if (*(int32_t *)(uintptr_t)(trows + 0x20ull * r) == wk) { memcpy(a->fn, (void *)(uintptr_t)(trows + 0x20ull * r + 8), 24); break; }
        }
        build_own_record(n, c->x[0]);
    }
    if (g_own_record[n]) c->x[0] = (uint64_t)(uintptr_t)g_own_record[n];
}
// ---- N7: THE GENERAL WRAP OF A DONOR'S NATIVE CALLBACKS (plan\phases\N7_own_files2.md, task 3).
// A donor's callbacks object is a C++ object of the game; its functions (the entries of its class table) are native
// code that may compare the fighter's kind field with the donor's own constant (N6: one such compare made a clone of
// link crash). When the callbacks getter is asked for a NEW kind, every function of the donor object's class table is
// hooked once with h_cb: when one of its first four arguments is the fighter object of a new kind, or that fighter's
// module accessor, the fighter's kind field [object + 0xc] holds the DONOR's kind for the duration of the call, and
// the new kind again afterwards. A real donor fighter passes through untouched (pointer compares only).
// What this does not cover: native code reached some other way (article functions: the weapon table row is lent,
// N4), and code that runs INSIDE a wrapped callback sees the donor's kind too (by design).  N7_CBWRAP=0: off.
struct NewLive { uint64_t obj, acc; uint32_t id; };
static NewLive g_live[16]; static volatile LONG g_nlive;
static HookOrig o_cb[512]; static uint64_t g_cb_addr[512]; static int g_ncb; static long g_n_cb_swap, g_n_cb_logged; static int g_cbwrap = 1;
static uint64_t g_cb_done_vt[8]; static int g_ncb_vt;
#define N8_CB_VT_DEFAULT ""          /* N9: no class given = the battle-object shape rule. (N8's default "any" was a bug: strtoull read it as the hex number 0xa, a class that never matches) */
static struct { int fn, arg; uint64_t vt; long calls; } g_obs[64]; static int g_n_obs;       /* N8: what the callbacks receive */
static uint64_t g_cbvt[8]; static int g_n_cbvt;                                             /* N8: the classes whose kind field is presented as the donor's */
static void h_cb(Ctx *c, void *u) {
    int i = (int)(intptr_t)u;
    /* Run n7l1: matching the argument against the pointer the fighter init got found nothing (the callbacks get another
     * object of the fighter, as N6's one-function hook did: it read [x1 + 0xc]). So the test is on the argument itself:
     * a readable object whose kind field (+ 0xc) is a new kind and which either carries the object id (+ 8) of a live
     * new-kind fighter or arrives while the init of that new kind runs on this thread. */
    /* N8: the rule that needs no guess. Run n8k1 (observe mode, N7_CBWRAP=2, with N6's one-function hook working)
     * logged what the donor's callbacks receive: see plan\phases\N8_first_frame.md section 5. The object is recognised
     * by its CLASS (the first word = the class table of the engine's fighter battle object, learned from the object
     * 0x7100c280f0 receives, or given by N8_CB_VT) and its kind field + 0xc holding a new kind. */
    LONG n = g_nlive;
    for (int a = 0; a < 4; a++) {
        uint64_t v = c->x[a];
        if (v < 0x10000 || (v & 7) || (v >> 47) || !readable(v, 16)) continue;
        uint32_t *p = (uint32_t *)(uintptr_t)(v + 0xc);
        uint32_t was = *p, id = *(uint32_t *)(uintptr_t)(v + 8);
        if (!is_new(was)) continue;
        uint64_t vt = *(uint64_t *)(uintptr_t)v;
        if (g_cbwrap == 2 || g_n_obs < 24) {            /* observe: every distinct (function, argument, class) once */
            int seen = 0;
            for (int k = 0; k < g_n_obs; k++) if (g_obs[k].fn == i && g_obs[k].arg == a && g_obs[k].vt == vt) { seen = 1; g_obs[k].calls++; break; }
            if (!seen && g_n_obs < 64) {
                g_obs[g_n_obs].fn = i; g_obs[g_n_obs].arg = a; g_obs[g_n_obs].vt = vt; g_obs[g_n_obs].calls = 1; g_n_obs++;
                int64_t rel = 0; int which = -1;
                for (LONG f = 0; f < n; f++) if (g_live[f].obj && v >= g_live[f].obj - 0x4000 && v < g_live[f].obj + 0x40000) { rel = (int64_t)(v - g_live[f].obj); which = (int)f; }
                logf("[n8obs] callback %llx (table function %d) argument %d = object %llx: class table %llx, + 8 = %x, kind field + 0xc = 0x%x, + 0x10 = %x; relative to the live new fighter object %d: %+lld%s",
                     (unsigned long long)g_cb_addr[i], i, a, (unsigned long long)v, (unsigned long long)vt, id, was, *(uint32_t *)(uintptr_t)(v + 0x10), which, (long long)rel,
                     which < 0 ? " (not near any)" : "");
            }
        }
        if (g_cbwrap == 2) continue;
        int mine = 0;
        for (int k = 0; k < g_n_cbvt; k++) if (g_cbvt[k] == vt) mine = 1;
        if (!g_n_cbvt) {
            /* no class given: THE SHAPE OF THE ENGINE'S BATTLE OBJECT. [lifted] 0x7100c280f0 reads its x1 as: + 0xc the
             * kind (compared with three constants), + 0x20 a pointer whose + 0x50 / + 0x100 are modules = the module
             * accessor; reference\skyline-smash (cpp.rs, BattleObject / BattleObjectModuleAccessor) gives the same
             * layout: + 8 object id, + 0xc kind, + 0x20 module accessor, and the accessor carries the SAME object id at
             * + 8. So: a new kind at + 0xc AND an accessor at + 0x20 with the object's id. (Player 1's id is 0: no
             * "id != 0" test, which is what made N7's rule blind.) */
            uint64_t acc = readable(v, 0x28) ? *(uint64_t *)(uintptr_t)(v + 0x20) : 0;
            mine = acc > 0x10000 && !(acc & 7) && !(acc >> 47) && readable(acc, 16) && *(uint32_t *)(uintptr_t)(acc + 8) == id;
        }
        if (!mine) continue;
        g_n_cb_swap++;
        if (g_n_cb_logged < 8) { g_n_cb_logged++; logf("[n7] donor native callback %llx (class-table function %d) called for the new kind 0x%x (argument %d = object %llx, object id %x): the kind field is the donor's (0x%x) for the call",
                                                       (unsigned long long)g_cb_addr[i], i, was, a, (unsigned long long)v, id, dk(was)); }
        else if (g_n_cb_swap % 20000 == 0) logf("[n7] donor native callbacks: %ld calls so far ran with the donor's kind for a new kind's fighter", g_n_cb_swap);
        *p = (uint32_t)dk(was); o_cb[i](c); *p = was;
        return;
    }
    o_cb[i](c);
}
static void wrap_callbacks(uint64_t object, uint32_t kind) {
    if (!g_cbwrap || !readable(object, 8)) return;
    uint64_t vt = *(uint64_t *)(uintptr_t)object;
    EnterCriticalSection(&g_lock);
    for (int k = 0; k < g_ncb_vt; k++) if (g_cb_done_vt[k] == vt) { LeaveCriticalSection(&g_lock); return; }
    if (g_ncb_vt < 8) g_cb_done_vt[g_ncb_vt++] = vt;
    int entries = 0, fresh = 0, known = 0, failed = 0;
    for (int s = 0; s < 400 && readable(vt + 8ull * s, 8); s++) {
        uint64_t e = *(uint64_t *)(uintptr_t)(vt + 8ull * s);
        if (e < 0x7100000000ull || e >= 0x7104000000ull) break;      /* the end of the class table (the next table's header) */
        entries++;
        int have = 0;
        for (int k = 0; k < g_ncb; k++) if (g_cb_addr[k] == e) { have = 1; break; }
        if (have) { known++; continue; }
        if (g_ncb >= 512) { failed++; continue; }
        g_cb_addr[g_ncb] = e;
        if (api->install(e, h_cb, (void *)(intptr_t)g_ncb, &o_cb[g_ncb])) { g_ncb++; fresh++; } else failed++;
    }
    LeaveCriticalSection(&g_lock);
    logf("[n7] callbacks object %llx of the donor of kind 0x%x (donor kind 0x%x): class table %llx with %d functions: %d wrapped now, %d wrapped before (shared with another table), %d not hookable; %d wrapped functions in all",
         (unsigned long long)object, kind, dk(kind), (unsigned long long)vt, entries, fresh, known, failed, g_ncb);
}
static void h_callbacks(Ctx *c, void *) {
    uint32_t kind = (uint32_t)c->x[0];
    h_w0(c, o_callbacks, &g_n_callbacks, "callbacks object");
    if (is_new(kind)) wrap_callbacks(c->x[0], kind);
}
// N6: 0x71006cdd20(w0 = fighter kind, x1, x2 = the fighter's module accessor): a 95-way jump table by kind
// (`cmp w0, #94 ; b.hi default`, table 0x71044be758) that sets up per-kind things of the fighter's modules. N2 listed
// it as not examined; Mario's case never mattered. For the donor link the default taken for 0x5F left the collision
// objects of its shield group unmade, and the donor's callback 0x7100c28280 (shield module, group 1) then read a null
// pointer (run l_a: HOST EXCEPTION reading 0xc in 0x71004c6b20, return address 0x7100c282ac). The donor's case runs.
static HookOrig o_setup2; static long g_n_setup2;
static void h_setup2(Ctx *c, void *) { h_w0(c, o_setup2, &g_n_setup2, "per-kind module setup 0x71006cdd20"); }
// N6_TRACE=<hex address>[,..]: debugging aid: log the callers of up to 8 game functions (return address, x0..x3, the
// kind of the fighter init running on the thread), the first N6_TRACE_MAX (default 60) calls of each.
static HookOrig o_trace[8]; static uint64_t g_trace_addr[8]; static long g_n_trace[8]; static long g_trace_max = 60;
static int trace_kind(void);
static void h_trace(Ctx *c, void *u) {
    int i = (int)(intptr_t)u;
    if (g_n_trace[i]++ < g_trace_max)
        logf("[n6trace] %llx called from %llx: x0 %llx x1 %llx x2 %llx x3 %llx, fighter init on this thread: kind 0x%x", (unsigned long long)g_trace_addr[i], (unsigned long long)c->x[30],
             (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3], trace_kind());
    // N12: N12_TRACE_DEREF=1: x1 points to {object, control}: the object, its class table and slot 2 of it
    static int deref = -1; if (deref < 0) deref = getenv("N12_TRACE_DEREF") != nullptr;
    if (deref && g_n_trace[i] <= g_trace_max && readable(c->x[1], 16)) {
        uint64_t o = *(uint64_t *)(uintptr_t)c->x[1], ctl = *(uint64_t *)(uintptr_t)(c->x[1] + 8);
        uint64_t vt = readable(o, 8) ? *(uint64_t *)(uintptr_t)o : 0, s2 = readable(vt + 16, 8) ? *(uint64_t *)(uintptr_t)(vt + 16) : 0;
        logf("[n6trace]    x1 -> object %llx (control %llx), class table %llx, slot 2 %llx; x3 %llx x4 %llx", (unsigned long long)o, (unsigned long long)ctl, (unsigned long long)vt,
             (unsigned long long)s2, (unsigned long long)c->x[3], (unsigned long long)c->x[4]);
    }
    // N13_TRACE_DUMP=<bytes>: the first <bytes> of the block x1 points to, as 32-bit words (N13: the weapon activation's
    // description, whose + 60 is the LIFE the activation writes)
    static long dump = -1; if (dump < 0) { const char *e = getenv("N13_TRACE_DUMP"); dump = e ? strtol(e, nullptr, 0) : 0; }
    if (dump > 0 && g_n_trace[i] <= g_trace_max && readable(c->x[1], (size_t)dump)) {
        char t[1200]; int k = 0; t[0] = 0;
        for (long o = 0; o + 4 <= dump && k < 1100; o += 4) k += snprintf(t + k, sizeof t - k, "%s%08x", o % 32 ? " " : (o ? " | " : ""), *(uint32_t *)(uintptr_t)(c->x[1] + o));
        logf("[n13trace]    x1 block (%ld bytes): %s", dump, t);
        // x0 = the weapon object: its per-kind object at + 0x3bb0 (15280), class table and slot 6 (+ 48)
        uint64_t po = readable(c->x[0] + 0x3bb0, 8) ? *(uint64_t *)(uintptr_t)(c->x[0] + 0x3bb0) : 0, pv = readable(po, 8) ? *(uint64_t *)(uintptr_t)po : 0;
        logf("[n13trace]    x0 + 0x3bb0 -> %llx, class table %llx, slot 6 %llx; x0 kind field %d", (unsigned long long)po, (unsigned long long)pv,
             (unsigned long long)(readable(pv + 48, 8) ? *(uint64_t *)(uintptr_t)(pv + 48) : 0), readable(c->x[0] + 0xc, 4) ? *(int32_t *)(uintptr_t)(c->x[0] + 0xc) : -1);
    }
    // N6_TRACE_SET=1: x0 is a collision set (0x71004c5780 ..): its group count [x0+0x48] and, per 0x2d0-byte group
    // element at [x0+0x50], first index / count (+8, +0xc) and the first collision object pointer (+0x30)
    static int set_dump = -1; if (set_dump < 0) set_dump = getenv("N6_TRACE_SET") != nullptr;
    if (set_dump && g_n_trace[i] <= g_trace_max && readable(c->x[0] + 0x48, 16)) {
        uint32_t ng = *(uint32_t *)(uintptr_t)(c->x[0] + 0x48); uint64_t el = *(uint64_t *)(uintptr_t)(c->x[0] + 0x50);
        char t[300]; int k = 0; t[0] = 0;
        for (uint32_t g = 0; g < ng && g < 6 && readable(el + 0x2d0ull * g, 0x38); g++) {
            uint8_t *e2 = (uint8_t *)(uintptr_t)(el + 0x2d0ull * g);
            k += snprintf(t + k, sizeof t - k, " [%u: first %d count %d object %llx]", g, *(int32_t *)(e2 + 8), *(int32_t *)(e2 + 12), *(unsigned long long *)(e2 + 0x30));
        }
        logf("[n6trace]    set %llx: %u groups, elements at %llx:%s", (unsigned long long)c->x[0], ng, (unsigned long long)el, t);
    }
    o_trace[i](c);
}
// N6: THE DONOR'S NATIVE CODE COMPARES THE FIGHTER'S KIND WITH ITS OWN. Found with the donor link (run l_a): its
// callback 0x7100c280f0(this, x1 = fighter object) is shared by link (2), younglink (23) and toonlink (46); it reads
// the kind at [x1 + 0xc], takes A[that kind] by a constant offset and SKIPS the set-up of the shield group for any
// other kind (0x7100c28164..0x7100c28178 -> 0x7100c28230); the next callback 0x7100c28280 then uses the collision
// object that was never taken from the pool (null). Mario's callbacks have no such compare. Not general yet: only
// the functions listed here (N6_KINDFIELD=<hex,..>, default 7100c280f0) run with the kind field [x1 + 0xc] set to the
// donor's kind for the call; the general fix is a wrapper on every entry of the donor's callbacks object.
static HookOrig o_kf[8]; static long g_n_kf[8]; static uint64_t g_kf_addr[8];
static void h_kindfield(Ctx *c, void *u) {
    int i = (int)(intptr_t)u;
    uint32_t *p = (uint32_t *)(uintptr_t)(c->x[1] + 0xc);
    if (readable(c->x[1] + 0xc, 4) && is_new(*p)) {
        uint32_t was = *p;
        if (g_n_kf[i]++ < 3) logf("[n6] donor native function %llx called for a fighter object with kind 0x%x: the kind field is the donor's (0x%x) for the call", (unsigned long long)g_kf_addr[i], was, dk(was));
        if (g_n_kf[i] <= 3 && readable(c->x[1], 0x40)) {       /* N8: WHAT the object is (run n8k1) */
            const uint64_t *w = (const uint64_t *)(uintptr_t)c->x[1];
            int64_t rel = 0; int which = -1;
            for (LONG f = 0; f < g_nlive; f++) if (g_live[f].obj) { rel = (int64_t)(c->x[1] - g_live[f].obj); which = (int)f; if (rel > -0x4000 && rel < 0x40000) break; }
            logf("[n8obs] %llx receives x0 %llx (class table %llx), x1 %llx: words %llx %llx %llx %llx %llx %llx; the live new fighter object %d is %llx (x1 - object = %+lld), its accessor %llx; return address %llx",
                 (unsigned long long)g_kf_addr[i], (unsigned long long)c->x[0], readable(c->x[0], 8) ? *(unsigned long long *)(uintptr_t)c->x[0] : 0ull, (unsigned long long)c->x[1],
                 (unsigned long long)w[0], (unsigned long long)w[1], (unsigned long long)w[2], (unsigned long long)w[3], (unsigned long long)w[4], (unsigned long long)w[5],
                 which, which >= 0 ? (unsigned long long)g_live[which].obj : 0ull, (long long)rel, which >= 0 ? (unsigned long long)g_live[which].acc : 0ull, (unsigned long long)c->x[30]);
        }
        *p = (uint32_t)dk(was); o_kf[i](c); *p = was;
        return;
    }
    o_kf[i](c);
}
static void h_get(Ctx *c, void *u) { int i = (int)(intptr_t)u; h_w0(c, o_get[i], &g_n_get[i], i ? "getter 0x710033c510" : "getter 0x710033ba50"); }
static void h_disp(Ctx *c, void *u) {              // dispatchers 0..4: kind at [x0 + 0xc]; 5..8: kind in w0
    int i = (int)(intptr_t)u;
    if (i >= 5) {
        uint32_t kind = (uint32_t)c->x[0];
        if (g_reg && is_new(kind)) g_reg->agent_begin(g_name[kind - FIRST]);
        h_w0(c, o_disp[i], &g_n_disp[i], "script dispatcher (share)");
        if (g_reg) g_reg->agent_end(is_new(kind) ? g_name[kind - FIRST] : nullptr, c->x[0]);
        return;
    }
    uint32_t *k = (uint32_t *)(uintptr_t)(c->x[0] + 0xc);
    uint32_t kind = *k;
    if (is_new(kind)) {
        const int DONOR = dk(kind);
        if (g_n_disp[i]++ < 2) logf("[n4] kind: script dispatcher %d asked for kind 0x%x -> the donor's factory (kind 0x%x)%s", i, kind, DONOR, g_reg ? ", the agent goes to the new fighter's own module" : "");
        if (g_reg) g_reg->agent_begin(g_name[kind - FIRST]);
        *k = DONOR; o_disp[i](c); *k = kind;
        if (g_reg) g_reg->agent_end(g_name[kind - FIRST], c->x[0]);
        return;
    }
    o_disp[i](c);
    if (g_reg) g_reg->agent_end(nullptr, c->x[0]);
}
// ---- N5: THE FIGHTER'S OWN PARAMETER FILES, read by the parameter-object builders from its folder.
// The builders parse the donor's two per-fighter parameter files out of the archive's "loaded data" buffers (the
// path of the new name is answered with the donor's file: one file, one buffer, shared with a real donor). When
// <fighter folder>\fighter\<name>\param\<file> exists, the loaded-data entry of the donor's file points to OUR copy
// of that file for the duration of the builder call made for the new kind (under the lock that serialises the
// builders), and to the archive's buffer again afterwards: the new kind's parameter object is built from its own
// file, the donor's from the archive's. Tables (reference\smash-arc; offsets as in loose_files.c): filesystem info
// *(0x7105331f20): +0x08 loaded file paths (8 bytes: loaded data index, flags), +0x10 loaded datas (0x18 bytes: +0
// buffer), +0x78 -> { archive tables }: +0x50 buckets, +0x58 hash -> file path index entries.
static const char *const OWN_PARAM[2] = { "vl.prc", "expression_vl.prc" };
static struct { uint8_t *data; long size; int tried; } g_own[MAXNEW][2];
static struct { uint64_t *slot; uint64_t was; } g_lent[2]; static int g_nlent;
static long g_n_own;
static int64_t arc_file_index(const char *path) {
    uint64_t h = hash40(path);
    uint64_t fsinfo = *(uint64_t *)(uintptr_t)MOD_GA(0x7105331f20ull);
    if (!readable(fsinfo + 0x78, 8)) return -1;
    uint64_t holder = *(uint64_t *)(uintptr_t)(fsinfo + 0x78);
    if (!readable(holder, 8)) return -1;
    uint64_t tab = *(uint64_t *)(uintptr_t)holder;
    if (!readable(tab + 0x50, 16)) return -1;
    uint64_t buckets = *(uint64_t *)(uintptr_t)(tab + 0x50), entries = *(uint64_t *)(uintptr_t)(tab + 0x58);
    if (!readable(buckets, 8)) return -1;
    uint32_t nb = *(uint32_t *)(uintptr_t)(buckets + 4);
    if (!nb) return -1;
    uint32_t *b = (uint32_t *)(uintptr_t)(buckets + 8 + (h % nb) * 8);
    for (uint32_t k = 0; k < b[1]; k++) {
        uint64_t e = *(uint64_t *)(uintptr_t)(entries + 8ull * (b[0] + k));
        if ((e & MASK40) == h) return (int64_t)(e >> 40);
    }
    return -1;
}
static void own_param_begin(uint32_t kind) {
    g_nlent = 0;
    if (!g_reg || !is_new(kind)) return;
    int n = (int)kind - FIRST;
    const SsbuRegFighter *f = &g_reg->fighters[n];
    for (int k = 0; k < 2; k++) {
        if (!g_own[n][k].tried) {
            g_own[n][k].tried = 1;
            char path[600]; snprintf(path, sizeof path, "%s\\fighter\\%s\\param\\%s", f->folder, f->name, OWN_PARAM[k]);
            FILE *fp = fopen(path, "rb");
            if (fp) {
                fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
                uint8_t *d = (uint8_t *)VirtualAlloc(nullptr, (size_t)sz + 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
                if (d && sz > 8 && fread(d, 1, (size_t)sz, fp) == (size_t)sz) { g_own[n][k].data = d; g_own[n][k].size = sz; }
                fclose(fp);
            }
        }
        if (!g_own[n][k].data) continue;
        char donor[200]; snprintf(donor, sizeof donor, "fighter/%s/param/%s", f->donor, OWN_PARAM[k]);
        int64_t fi = arc_file_index(donor);
        uint64_t fsinfo = *(uint64_t *)(uintptr_t)MOD_GA(0x7105331f20ull);
        if (fi < 0 || !readable(fsinfo + 8, 16)) continue;
        uint64_t lfp = *(uint64_t *)(uintptr_t)(fsinfo + 8), ld = *(uint64_t *)(uintptr_t)(fsinfo + 0x10);
        if (!readable(lfp + 8ull * fi, 8)) continue;
        uint32_t ldi = *(uint32_t *)(uintptr_t)(lfp + 8ull * fi);
        if (ldi == 0xffffffffu || !readable(ld + 0x18ull * ldi, 0x18)) continue;
        uint64_t *slot = (uint64_t *)(uintptr_t)(ld + 0x18ull * ldi);
        // both must be parameter files (the same 8-byte magic at the start), or nothing is lent
        if (!readable(*slot, 8) || memcmp((void *)(uintptr_t)*slot, g_own[n][k].data, 8) != 0) {
            if (g_n_own++ < 4) logf("[n5] own parameter file %s of %s: the archive's buffer of %s is not there or not the same kind of file: NOT lent", OWN_PARAM[k], f->name, donor);
            continue;
        }
        g_lent[g_nlent].slot = slot; g_lent[g_nlent].was = *slot; g_nlent++;
        *slot = (uint64_t)(uintptr_t)g_own[n][k].data;
        if (g_n_own++ < 8) logf("[n5] kind 0x%x: parameter builder reads %s\\fighter\\%s\\param\\%s (%ld bytes) instead of the archive's %s (file path index 0x%llx, loaded data %u, buffer %llx): for this call only",
                                kind, f->folder, f->name, OWN_PARAM[k], g_own[n][k].size, donor, (unsigned long long)fi, ldi, (unsigned long long)g_lent[g_nlent - 1].was);
    }
}
static void own_param_end(void) {
    for (int k = 0; k < g_nlent; k++) *g_lent[k].slot = g_lent[k].was;
    g_nlent = 0;
}
// ---- N12 (own articles, TEST STAND-IN, N12_ARTGROUP=1): run n12r showed the weapon init asks the OWNER fighter's
// parameter object for the article's group by hash ("param_fireball", class-table slot of Mario's 0x7100cbf1c0, found
// at 0x7104fe4f40) - for a new fighter that is the donor's class, which has no such group. Here the donor class's
// getter answers, for the new fighter's object and the hashes "param_<weapon>" / "map_collision_<weapon>" of its
// articles' templates, with the TEMPLATE OWNER's object's answer (the template's values: test only). The clean form is
// the fighter's own parameter object carrying these structs, parsed from its own vl.prc (phase file 4.4g).
static HookOrig o_group; static uint64_t g_group_fn, g_tmpl_getter; static int g_group_slot = -1; static long g_n_group;
static uint64_t g_objN[MAXNEW], g_objT[MAXNEW][8], g_hashT[MAXNEW][8][2];
static void h_group(Ctx *c, void *) {
    for (int n = 0; n < g_nnew; n++) {
        // run n12s: never asked with x0 == the object at A[new kind] + 0x10; any object of the donor's class asking for
        // an article group of this fighter's templates is answered (the donor itself has no such group)
        if (!g_objN[n]) continue;
        uint64_t h = c->x[1] & MASK40;
        if (c->x[0] != g_objN[n] && g_n_group < 3) {
            for (int t = 0; t < g_nart[n]; t++) if (h == g_hashT[n][t][0] || h == g_hashT[n][t][1])
                logf("[n12] parameter group %llx asked of object %llx (not A[0x%x] + 0x10 = %llx)", (unsigned long long)h, (unsigned long long)c->x[0], FIRST + n, (unsigned long long)g_objN[n]);
        }
        for (int t = 0; t < g_nart[n]; t++)
            if (g_objT[n][t] && (h == g_hashT[n][t][0] || h == g_hashT[n][t][1])) {
                uint64_t vt = *(uint64_t *)(uintptr_t)g_objT[n][t], fn = *(uint64_t *)(uintptr_t)(vt + 8ull * g_group_slot);
                if (g_n_group++ < 6) logf("[n12] parameter group %llx asked of %s's object: answered by the template owner's object %llx (getter %llx) - TEST stand-in", (unsigned long long)h, g_name[n],
                                          (unsigned long long)g_objT[n][t], (unsigned long long)fn);
                c->x[0] = g_objT[n][t];
                api->call(c, fn);
                return;
            }
    }
    o_group(c);
}
static void install_group_hook(int n) {
    const char *e = getenv("N12_ARTGROUP");
    if (!(e && e[0] == '1') || !articles_on()) return;
    uint8_t *P = *(uint8_t **)(uintptr_t)MOD_GA(0x71052bb3b0ull);
    if (!P) return;
    resolve_articles(n);
    g_objN[n] = *(uint64_t *)(P + 0x60 + 0x38 * (FIRST + n) + 0x10);
    for (int t = 0; t < g_nart[n]; t++) {
        int ok_ = g_art[n][t].owner_kind;
        if (ok_ < 0 || ok_ >= FIRST) continue;
        g_objT[n][t] = *(uint64_t *)(P + 0x60 + 0x38 * ok_ + 0x10);
        const char *w = strchr(g_art[n][t].tmpl, '_');
        char s1[80], s2[80]; snprintf(s1, sizeof s1, "param_%s", w ? w + 1 : ""); snprintf(s2, sizeof s2, "map_collision_%s", w ? w + 1 : "");
        g_hashT[n][t][0] = hash40(s1); g_hashT[n][t][1] = hash40(s2);
        if (g_group_slot < 0 && readable(g_objT[n][t], 8)) {           // the slot of Mario's getter in his class table
            uint64_t vt = *(uint64_t *)(uintptr_t)g_objT[n][t];
            for (int s = 0; s < 32 && readable(vt + 8ull * s, 8); s++) if (*(uint64_t *)(uintptr_t)(vt + 8ull * s) == MOD_GA(0x7100cbf1c0ull)) g_group_slot = s;
        }
    }
    if (g_group_slot < 0 || !readable(g_objN[n], 8)) { logf("[n12] parameter group hook: slot %d, object %llx: not installed", g_group_slot, (unsigned long long)g_objN[n]); return; }
    uint64_t fn = *(uint64_t *)(uintptr_t)(*(uint64_t *)(uintptr_t)g_objN[n] + 8ull * g_group_slot);
    if (!g_group_fn) { g_group_fn = fn; api->install(fn, h_group, nullptr, &o_group); }
    logf("[n12] parameter group hook: %s's object %llx, class-table slot %d = %llx; template owner object %llx", g_name[n], (unsigned long long)g_objN[n], g_group_slot, (unsigned long long)fn,
         (unsigned long long)g_objT[n][0]);
}
static void h_build(Ctx *c, void *u) {             // (this, kind, x2): the case builds the kind's parameter object and stores it in A[kind]
    int i = (int)(intptr_t)u;
    uint32_t kind = (uint32_t)c->x[1];
    if (!is_new(kind)) { o_build[i](c); return; }
    const int DONOR = dk(kind);
    uint8_t *P = (uint8_t *)(uintptr_t)c->x[0], *a0 = P + 0x60 + 0x38 * DONOR, *an = P + 0x60 + 0x38 * kind, keep[0x38];
    uint64_t arg[8]; memcpy(arg, c->x, sizeof arg);
    // THE DONOR AS A DEPENDENCY. The donor's native code (its callbacks object, e.g. 0x7100cb9620) reads the donor's
    // own row of array A with a CONSTANT offset (singleton + 0x70 = A[0] + 0x10), whatever fighter it runs for. A new
    // kind that uses a donor's callbacks therefore needs the donor's parameter objects to exist. The game's caller
    // (0x7100607b2c..) builds a kind's objects when the row's use counter (row + 0) is 0 and then counts one user;
    // we do the same for the donor: build once, count one user, so that a real donor fighter arriving later shares
    // the objects instead of building them a second time (N3's workaround did not count: run ii5).
    // N6: the constant offset is the DONOR's row (A[donor] + 0x10), so the dependency is per donor kind.
    static int dep_pending_k[FIRST];
    int &dep_pending = dep_pending_k[DONOR];
    uint32_t *cnt = (uint32_t *)a0;
    if (!g_a0_workaround && ((i == 0 && *cnt == 0 && *(uint64_t *)(a0 + 0x18) == 0) || (i == 1 && dep_pending))) {
        c->x[1] = DONOR; o_build[i](c);
        memcpy(c->x, arg, sizeof arg);
        if (i == 0) dep_pending = 1;
        else { dep_pending = 0; (*cnt)++; logf("[n4] kind: donor kind %d loaded as a dependency of kind 0x%x: A[%d] = %llx | %llx, use counter %u", DONOR, kind, DONOR,
                                               *(unsigned long long *)(a0 + 0x10), *(unsigned long long *)(a0 + 0x28), *cnt); }
    }
    // N12 (own articles, stage 1): the TEMPLATE's owner is a dependency the same way (run n12c: the weapon init of the
    // article pool read the owner's parameter object A[owner] + 0x10 before that fighter was set up: null). Built here
    // once with one user counted, so the owner arriving later in the match shares it. Needs that owner's files loaded
    // (true when it is in the match; otherwise an open point, see the phase file).
    if (articles_on() && !g_a0_workaround && !(getenv("N13_LIVEPARAM") && getenv("N13_LIVEPARAM")[0] == '1')) {
        static int tdep_pending[FIRST];
        int n = (int)kind - FIRST;
        resolve_articles(n);
        for (int t = 0; t < g_nart[n]; t++) {
            int ok_ = g_art[n][t].owner_kind;
            if (ok_ < 0 || ok_ >= FIRST || ok_ == DONOR) continue;
            uint8_t *ao = P + 0x60 + 0x38 * ok_; uint32_t *oc = (uint32_t *)ao;
            if ((i == 0 && *oc == 0 && *(uint64_t *)(ao + 0x18) == 0) || (i == 1 && tdep_pending[ok_])) {
                c->x[1] = (uint64_t)ok_; o_build[i](c);
                memcpy(c->x, arg, sizeof arg);
                if (i == 0) tdep_pending[ok_] = 1;
                else { tdep_pending[ok_] = 0; (*oc)++; logf("[n12] kind: template owner kind %d loaded as a dependency of kind 0x%x (article %s): A[%d] = %llx | %llx, use counter %u", ok_, kind,
                                                            g_art[n][t].name, ok_, *(unsigned long long *)(ao + 0x10), *(unsigned long long *)(ao + 0x28), *oc); }
            }
        }
    }
    EnterCriticalSection(&g_lock);
    memcpy(keep, a0, 0x38); memcpy(a0, an, 0x38);          // the donor's case works on row 0: lend it the new row for the call
    own_param_begin(kind);                                 // N5: the fighter's OWN parameter files, when its folder has them
    c->x[1] = DONOR; o_build[i](c);
    own_param_end();
    memcpy(an, a0, 0x38); memcpy(a0, keep, 0x38);
    LeaveCriticalSection(&g_lock);
    if (g_n_build[i]++ < 6) logf("[n4] kind: parameter object builder %d ran the donor's case for kind 0x%x: A[0x%x] = %llx %llx | %llx %llx ; A[donor 0x%x] = %llx | %llx", i, kind, kind,
                              *(unsigned long long *)(an + 0x10), *(unsigned long long *)(an + 0x18), *(unsigned long long *)(an + 0x28), *(unsigned long long *)(an + 0x30),
                              DONOR, *(unsigned long long *)(a0 + 0x10), *(unsigned long long *)(a0 + 0x28));
    if (i == 1) install_group_hook((int)kind - FIRST);      // N12 test stand-in (N12_ARTGROUP=1)
}
// A REAL ROW PER NEW KIND in the three row vectors the loader fills from fighter_param.prc / fighter_param_motion.prc
// (holders at singleton + 0, + 0x10, + 0x20: begin at holder + 8, end at + 0x10; rows of 0x628, 0x218, 0x68 bytes; the
// row's first 8 bytes are its fighter_kind hash: read in 0x710070a464..0x710070a540). N4_ROW=1: each vector gets one
// more row per new kind (a copy of the donor's row with the new kind's hash) in a buffer from the game's own
// allocator (0x710392dce0(align, size)), and B[new kind] = the new indexes.
// N4_ROW_FIND=<hex bits> N4_ROW_SET=<hex bits>: in the new 0x628-byte row every 32-bit word equal to FIND becomes SET.
static int g_row, g_row_off[8], g_row_noff; static uint32_t g_row_find, g_row_set;
static uint64_t game_call2(Ctx *c, uint64_t addr, uint64_t a0, uint64_t a1) {
    uint64_t keep[31]; memcpy(keep, c->x, sizeof keep);
    c->x[0] = a0; c->x[1] = a1;
    api->call(c, addr);
    uint64_t r = c->x[0];
    memcpy(c->x, keep, sizeof keep);
    return r;
}
// ---- N12 task 1: THE ROW FROM THE MANIFEST. fighter.json "parameter_row": { "set": { "<name>": <number>, ... } }:
// each name is looked up in fighter_param_row.txt (name, f32|i32, byte offset; next to this DLL) and its value
// written into the new kind's 0x628-byte row right after the row is made. A name the table does not have is logged
// and skipped. N12_ROWSET=0: off.
struct RowField { char name[48]; int is_int; int off; };
static RowField g_rf[256]; static int g_nrf = -1;
static void load_row_table(void) {
    if (g_nrf >= 0) return;
    g_nrf = 0;
    char path[MAX_PATH]; HMODULE me = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(void *)&load_row_table, &me);
    DWORD n = GetModuleFileNameA(me, path, MAX_PATH);
    while (n && path[n - 1] != '\\' && path[n - 1] != '/') n--;
    snprintf(path + n, sizeof path - n, "fighter_param_row.txt");
    FILE *f = fopen(path, "r");
    if (!f) { logf("[n12] row table %s not found: parameter_row.set is not applied", path); return; }
    char line[200];
    while (fgets(line, sizeof line, f) && g_nrf < 256) {
        char nm[64], ty[8], of[16];
        if (line[0] == '#' || sscanf(line, "%63s %7s %15s", nm, ty, of) != 3) continue;
        snprintf(g_rf[g_nrf].name, sizeof g_rf[g_nrf].name, "%s", nm);
        g_rf[g_nrf].is_int = ty[0] == 'i'; g_rf[g_nrf].off = (int)strtol(of, nullptr, 0);
        g_nrf++;
    }
    fclose(f);
    logf("[n12] row table %s: %d named fields", path, g_nrf);
}
static void apply_row_set(int k, uint8_t *r, size_t row) {
    if (!g_reg || k >= g_reg->count) return;
    { const char *e = getenv("N12_ROWSET"); if (e && e[0] == '0') return; }
    char path[600]; snprintf(path, sizeof path, "%s\\fighter.json", g_reg->fighters[k].folder);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    static char js[32768]; size_t n = fread(js, 1, sizeof js - 1, f); fclose(f); js[n] = 0;
    const char *p = strstr(js, "\"parameter_row\"");
    if (p) p = strstr(p, "\"set\"");
    if (p) p = strchr(p + 5, '{');
    if (!p) return;
    load_row_table();
    int done = 0, unknown = 0; char shown[900]; int sk = 0; shown[0] = 0;
    for (p++; *p && *p != '}'; ) {
        const char *q = strchr(p, '"'); if (!q) break;
        const char *qe = strchr(q + 1, '"'); if (!qe) break;
        char key[64]; size_t kl = (size_t)(qe - q - 1); if (kl > 63) kl = 63; memcpy(key, q + 1, kl); key[kl] = 0;
        const char *v = qe + 1; while (*v == ' ' || *v == ':' || *v == '\t' || *v == '\r' || *v == '\n') v++;
        char *ve; double val = strtod(v, &ve);
        p = ve; while (*p == ' ' || *p == ',' || *p == '\r' || *p == '\n' || *p == '\t') p++;
        if (ve == v) break;
        int fi = -1;
        for (int i = 0; i < g_nrf; i++) if (!strcmp(g_rf[i].name, key)) { fi = i; break; }
        if (fi < 0 || (size_t)g_rf[fi].off + 4 > row) { unknown++; logf("[n12] %s: parameter_row.set \"%s\": not in the row table: skipped", g_reg->fighters[k].name, key); continue; }
        uint32_t was, now; memcpy(&was, r + g_rf[fi].off, 4);
        if (g_rf[fi].is_int) { int32_t iv = (int32_t)val; memcpy(&now, &iv, 4); } else { float fv = (float)val; memcpy(&now, &fv, 4); }
        memcpy(r + g_rf[fi].off, &now, 4);
        done++;
        if (sk < 820) sk += snprintf(shown + sk, sizeof shown - sk, " %s(+0x%x)=%g", key, g_rf[fi].off, val);
        (void)was;
    }
    logf("[n12] %s: parameter_row.set from %s: %d values written into the new 0x628 row, %d unknown names:%s", g_reg->fighters[k].name, path, done, unknown, shown);
}
static int add_rows(Ctx *c, uint8_t *P, int holder_off, size_t row, int32_t *idx_out) {
    uint64_t holder = *(uint64_t *)(P + holder_off);
    if (!readable(holder + 8, 0x18)) return 0;
    uint64_t *v = (uint64_t *)(uintptr_t)(holder + 8);              // begin, end, capacity end
    uint8_t *b = (uint8_t *)(uintptr_t)v[0], *e = (uint8_t *)(uintptr_t)v[1];
    size_t n = (size_t)(e - b) / row;
    int donor_row[MAXNEW];
    for (int k = 0; k < g_nnew; k++) {                  // N6: the donor's row of EACH new kind
        int donor = -1;
        uint64_t dh = ((uint64_t *)(uintptr_t)MOD_GA(0x710453bc58ull))[dk(FIRST + k)] & MASK40;       // the loader's own list of kind hashes
        for (size_t i = 0; i < n; i++) if ((*(uint64_t *)(b + i * row) & MASK40) == dh) { donor = (int)i; break; }
        if (donor < 0) { logf("[n6] kind: row vector of 0x%x-byte rows: no row with the kind hash %llx of the donor %s: no rows added", (unsigned)row, (unsigned long long)dh, g_donor_name[k]); return 0; }
        donor_row[k] = donor;
    }
    uint8_t *nb = (uint8_t *)(uintptr_t)game_call2(c, MOD_GA(0x710392dce0ull), 0x10, (n + g_nnew) * row);
    if (!nb) return 0;
    memcpy(nb, b, n * row);
    for (int k = 0; k < g_nnew; k++) {
        uint8_t *r = nb + (n + k) * row;
        memcpy(r, nb + donor_row[k] * row, row);
        uint64_t h = *(uint64_t *)r; h = (h & ~MASK40) | g_hash[k]; memcpy(r, &h, 8);
        idx_out[k] = (int32_t)(n + k);
        if (row == 0x628) apply_row_set(k, r, row);             // N12: fighter.json parameter_row.set
        if (row == 0x628 && g_row_find)
            for (size_t o = 8; o + 4 <= row; o += 4)
                if (*(uint32_t *)(r + o) == g_row_find) { *(uint32_t *)(r + o) = g_row_set; if (k == 0 && g_row_noff < 8) g_row_off[g_row_noff++] = (int)o; }
    }
    v[0] = (uint64_t)(uintptr_t)nb; v[1] = (uint64_t)(uintptr_t)(nb + (n + g_nnew) * row);
    if (v[2] >= (uint64_t)(uintptr_t)e && v[2] - (uint64_t)(uintptr_t)e < 0x100000) v[2] = v[1];      // capacity end, if this is one
    // the old buffer is NOT given back (who allocated it was not checked): a leak of n rows, once per run
    {
        char rows[1200]; int rk = 0; rows[0] = 0;   // U16: was 120 - with 10+ new kinds rk ran past the end and the next snprintf wrote behind it
        for (int k = 0; k < g_nnew && rk < (int)sizeof rows - 64; k++) rk += snprintf(rows + rk, sizeof rows - rk, "%s%d (%s)", k ? ", " : "", donor_row[k], g_donor_name[k]);
        logf("[n4] kind: row vector of 0x%x-byte rows: %d -> %d rows (donor's row %s copied with the new kind hash; new buffer %p from the game's allocator)",
             (unsigned)row, (int)n, (int)n + g_nnew, rows, (void *)nb);
    }
    return 1;
}
static void h_loader(Ctx *c, void *) {             // loads fighter_param: writes B[0..0x5D]; the rows of the new kinds are ours
    uint8_t *P = (uint8_t *)(uintptr_t)c->x[0];
    ensure_data();
    o_loader(c);
    int32_t *b = (int32_t *)(P + NEW_B);
    for (int n = 0; n < g_nnew; n++) memcpy(b + 3 * (FIRST + n), b + 3 * dk(FIRST + n), 12);
    if (g_row) {
        int32_t i0[MAXNEW], i1[MAXNEW], i2[MAXNEW];
        // The third table (0x68-byte rows) is a KIND x KIND matrix: each row holds four vectors with 3 floats per
        // OPPONENT kind, indexed with B[opponent] (accessor 0x7100720f90). A row index 94 there means element 282 of
        // vectors that have 282 elements: std::out_of_range (run H1). Growing it needs a column per new kind in
        // every row; until then a new kind keeps the donor's row AND column there (N4_ROW=2 shows the failure).
        int ok0 = add_rows(c, P, 0x00, 0x628, i0), ok1 = add_rows(c, P, 0x10, 0x218, i1), ok2 = g_row >= 2 ? add_rows(c, P, 0x20, 0x68, i2) : 0;
        for (int n = 0; n < g_nnew; n++) {
            if (ok0) b[3 * (FIRST + n) + 0] = i0[n];
            if (ok1) b[3 * (FIRST + n) + 1] = i1[n];
            if (ok2) b[3 * (FIRST + n) + 2] = i2[n];
        }
        char offs[120]; int k = 0; offs[0] = 0;
        for (int i = 0; i < g_row_noff; i++) k += snprintf(offs + k, sizeof offs - k, " +0x%x", g_row_off[i]);
        for (int n = 0; n < g_nnew; n++) {
            int K = FIRST + n, D = dk(K);
            logf("[n4] kind: OWN ROWS: B[0x%x] = %d %d %d (the donor's, kind 0x%x: %d %d %d); edit %08x -> %08x made in the new 0x628 row at:%s", K,
                 b[3 * K], b[3 * K + 1], b[3 * K + 2], D, b[3 * D], b[3 * D + 1], b[3 * D + 2], g_row_find, g_row_set, g_row_noff ? offs : " (nowhere)");
        }
    }
    logf("[n4] kind: parameter loader done: B[0x%x] = %d %d %d, B[0] = %d %d %d (B[1] = %d %d %d)", FIRST, b[3 * FIRST], b[3 * FIRST + 1], b[3 * FIRST + 2], b[0], b[1], b[2], b[3], b[4], b[5]);
}
// read-back of the edited value through the game's own index: singleton -> row vector -> row B[kind]
static void row_readback(int kind, const char *when) {
    uint8_t *P = *(uint8_t **)(uintptr_t)MOD_GA(0x71052bb3b0ull);
    if (!P || !g_row_noff) return;
    int32_t idx = ((int32_t *)(P + NEW_B))[3 * kind];
    uint64_t holder = *(uint64_t *)P, beg = *(uint64_t *)(uintptr_t)(holder + 8), end = *(uint64_t *)(uintptr_t)(holder + 0x10);
    uint64_t r = beg + (uint64_t)idx * 0x628;
    if (idx < 0 || r + 0x628 > end) { logf("[n4] row read-back %s: kind 0x%x has row index %d outside the vector", when, kind, idx); return; }
    uint32_t w = *(uint32_t *)(uintptr_t)(r + g_row_off[0]);
    logf("[n4] row read-back %s: kind 0x%x -> row %d of %d (fighter_kind hash %llx), value at +0x%x = %08x (%g)", when, kind, idx, (int)((end - beg) / 0x628),
         (unsigned long long)(*(uint64_t *)(uintptr_t)r & MASK40), g_row_off[0], w, *(float *)&w);
}
static void h_setup(Ctx *c, void *) {              // second per-kind container: rows 0x5E.. become empty lists
    o_setup(c);
    uint64_t *p = *(uint64_t **)(uintptr_t)MOD_GA(0x71052bb3a8ull);
    if (!p) return;
    int done = 0;
    for (int k = 0x5e; k < 0x72; k++) {
        uint64_t *row = p + 3 * k;
        if (row[0] == 0 && row[1] == 0) { row[0] = row[1] = (uint64_t)(uintptr_t)row; row[2] = 0; done++; }
    }
    if (done) logf("[n4] kind: second per-kind container %p: %d rows from 0x5e made empty lists", (void *)p, done);
}
static void h_init(Ctx *c, void *) {               // fighter init: x0 = fighter object, x1 = init description, kind at +0x9c
    uint64_t d = c->x[1], obj = c->x[0];
    int kind = readable(d + 0x9c, 4) ? *(int32_t *)(uintptr_t)(d + 0x9c) : -99;
    int slot = readable(d + 0xa8, 4) ? *(int32_t *)(uintptr_t)(d + 0xa8) : -1;
    remember(g_fighter, &g_nfighter, 16, obj, kind);
    int was = t_init_kind; t_init_kind = kind;
    {   /* N7: the live new-kind fighters, for the wrap of the donor's callbacks: this object leaves the list when it is
         * initialised again as another kind; a new kind's object and its module accessor (object + 0x20, checked by
         * the object id both carry at + 8) enter it BEFORE the init runs (callbacks are called inside it) */
        LONG n = g_nlive; int at = -1;
        for (LONG f = 0; f < n; f++) if (g_live[f].obj == obj) at = (int)f;
        if (is_new((uint32_t)kind)) {
            if (at < 0 && n < 16) { at = (int)n; g_live[at].acc = 0; g_live[at].obj = obj; InterlockedExchange(&g_nlive, n + 1); }
        } else if (at >= 0) g_live[at].obj = 0, g_live[at].acc = 0;
    }
    /* N8: the registry learns which new fighter is being set up on this thread (paths built with the donor's name) */
    int own_set = g_reg && g_reg->size >= offsetof(SsbuRegistry, owner_set) + sizeof(void *) && g_reg->owner_set && is_new((uint32_t)kind);
    if (own_set) g_reg->owner_set(g_name[kind - FIRST]);
    o_init(c);
    if (own_set) g_reg->owner_set(nullptr);
    t_init_kind = was;
    {   // (2026-11-03, crashes n12_kind +0x45d4: h_fireball read + 8 of every remembered object, also of past matches'
        // freed ones): the id is read here, once, from the live object; the newest fighter of an id replaces older ones
        uint32_t id = readable(obj + 8, 4) ? *(uint32_t *)(uintptr_t)(obj + 8) : 0;
        for (int i = 0; i < g_nfighter; i++) {
            if (g_fighter[i].obj == obj) g_fighter[i].id = id;
            else if (g_fighter[i].id == id) g_fighter[i].id = 0, g_fighter[i].obj = 0;
        }
    }
    if (is_new((uint32_t)kind) && readable(obj + 0x20, 8)) {
        uint64_t acc = *(uint64_t *)(uintptr_t)(obj + 0x20);
        int ok = readable(acc + 8, 4) && *(uint32_t *)(uintptr_t)(acc + 8) == *(uint32_t *)(uintptr_t)(obj + 8);
        for (LONG f = 0; f < g_nlive; f++) if (g_live[f].obj == obj) { g_live[f].acc = ok ? acc : 0; g_live[f].id = *(uint32_t *)(uintptr_t)(obj + 8); }
        static long said; if (said++ < 4) logf("[n7] new kind 0x%x: fighter object %llx, module accessor %llx (%s): the donor's wrapped callbacks see the donor's kind for this fighter", kind,
                                               (unsigned long long)obj, (unsigned long long)acc, ok ? "same object id" : "NOT recognised: only the object pointer is matched");
    }
    if (g_n_init++ < 16) logf("[n4] kind: fighter init 0x71006867e0 returned: object %llx kind 0x%x, slot %d, object id %x, object kind field %x",
                              (unsigned long long)obj, kind, slot, *(uint32_t *)(uintptr_t)(obj + 8), *(uint32_t *)(uintptr_t)(obj + 0xc));
    if (g_row && kind >= 0 && kind < 0x72) row_readback(kind, "after the fighter init");
}

// ---- weapons: the live owner -----------------------------------------------------------------------------------
static int with_owner(Ctx *c, HookOrig o, int wk, int live) {      // run o with OWNER_TABLE[wk] = live
    int32_t *t = (int32_t *)(uintptr_t)OWNER_TABLE;
    static_owner(0);                                    // N5: the table as the game has it, before the first row is lent
    if (wk < 0 || wk >= WEAPON_KINDS || live < 0 || t[wk] == live || !is_new((uint32_t)live)) { o(c); return 0; }
    EnterCriticalSection(&g_lock);
    int32_t was = t[wk];
    t[wk] = live; o(c); t[wk] = was;
    LeaveCriticalSection(&g_lock);
    g_n_swap++;
    return 1;
}
// N12 (diagnosis, N12_LIFE_LOG=1): the LIFE (instance int 5) of the own article's weapons, read every frame line
// call from the weapon object's module accessor (+ 0x20) -> work module (+ 0x50) -> areas (+ 0x18) -> area 0 -> ints
// (+ 0x20, 8 bytes each), logged when it changes.
static uint64_t g_life_w[8]; static int64_t g_life_v[8]; static int g_nlife; static long g_life_frames; static HookOrig o_lifeframe;
static uint64_t g_ownw[32]; static int g_nownw, g_ownw_next, g_ownw_art[32][2];      // weapon objects of own articles (dispatcher hook), their fighter / article index
static uint64_t g_ownagent[64]; static int g_ownagent_disp[64], g_nownagent, g_ownagent_seen[64]; static int64_t g_ownagent_life[64];
static int64_t work_int(uint64_t w, int index, int *ok);
static int64_t weapon_life(uint64_t w, int *ok) { return work_int(w, 5, ok); }
static int64_t work_int(uint64_t w, int index, int *ok) {
    *ok = 0;
    uint64_t acc = readable(w + 0x20, 8) ? *(uint64_t *)(uintptr_t)(w + 0x20) : 0;
    uint64_t wm = readable(acc + 0x50, 8) ? *(uint64_t *)(uintptr_t)(acc + 0x50) : 0;
    uint64_t areas = readable(wm + 0x18, 8) ? *(uint64_t *)(uintptr_t)(wm + 0x18) : 0;
    uint64_t a0 = readable(areas, 8) ? *(uint64_t *)(uintptr_t)areas : 0;
    uint64_t ints = readable(a0 + 0x20, 8) ? *(uint64_t *)(uintptr_t)(a0 + 0x20) : 0;
    if (!readable(ints + 8ull * index, 8)) return 0;
    *ok = 1;
    return *(int64_t *)(uintptr_t)(ints + 8ull * index);
}
// run n12x: the direct read above is WRONG (it returned two floats 1.0 for the fighter's counter); the game's own
// getter is used instead: WorkModule::get_int(module accessor = object + 0x20, id)
static int64_t game_work_int(Ctx *c, uint64_t obj, uint32_t id, int *ok) {
    static uint64_t fn; if (!fn) fn = api->address("_ZN3app8lua_bind24WorkModule__get_int_implEPNS_26BattleObjectModuleAccessorEi");
    uint64_t acc = readable(obj + 0x20, 8) ? *(uint64_t *)(uintptr_t)(obj + 0x20) : 0;
    *ok = fn && acc && readable(acc + 8, 4) && *(uint32_t *)(uintptr_t)(acc + 8) == *(uint32_t *)(uintptr_t)(obj + 8);
    return *ok ? (int64_t)(int32_t)game_call2(c, fn, acc, id) : 0;
}
// 4.4i next step: the accessor taken the way the hash log takes a fighter's (agent + 0x40); the own article's agents
// are recorded in h_wdisp. The getter's address and the reads are logged separately.
// who writes LIFE: the work module's set_int (class-table slot + 0xa0, fh_hook's research note) hooked once; every
// call with the id 0x10000005 in x1 or x2 is logged with its return address
static HookOrig o_setint; static long g_n_setint;
static void h_setint(Ctx *c, void *) {
    if (((uint32_t)c->x[1] == 0x10000005u || (uint32_t)c->x[2] == 0x10000005u) && g_n_setint++ < 200)
        logf("[n12life] work set_int(%llx, x1 %llx, x2 %llx) from %llx", (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[30]);
    o_setint(c);
}
static void h_lifeframe(Ctx *c, void *) {
    g_life_frames++;
    static int setint_done;
    if (!setint_done) {
        setint_done = 1;
        uint64_t ag = c->x[0], bm = readable(ag + 0x40, 8) ? *(uint64_t *)(uintptr_t)(ag + 0x40) : 0;
        uint64_t wm = readable(bm + 0x50, 8) ? *(uint64_t *)(uintptr_t)(bm + 0x50) : 0, vt = readable(wm, 8) ? *(uint64_t *)(uintptr_t)wm : 0;
        uint64_t si = readable(vt + 0xa0, 8) ? *(uint64_t *)(uintptr_t)(vt + 0xa0) : 0;
        logf("[n12life] work module %llx class table %llx: set_int %llx hook %s", (unsigned long long)wm, (unsigned long long)vt, (unsigned long long)si,
             si && api->install(si, h_setint, nullptr, &o_setint) ? "installed" : "NOT installed");
    }
    static uint64_t fn; static int said;
    if (!fn) fn = api->address("_ZN3app8lua_bind24WorkModule__get_int_implEPNS_26BattleObjectModuleAccessorEi");
    if (!said++) logf("[n12life] WorkModule::get_int_impl by name: %llx", (unsigned long long)fn);
    if (!fn) { o_lifeframe(c); return; }
    uint64_t agent = c->x[0], boma = readable(agent + 0x40, 8) ? *(uint64_t *)(uintptr_t)(agent + 0x40) : 0;
    if (g_life_frames % 400 == 0 && boma && readable(boma + 8, 4)) {        // the reader checked on a fighter's work int (the hash log's own counter id)
        int64_t v = (int32_t)game_call2(c, fn, boma, 0x1000010Au);
        logf("[n12life] reader check: frame-line call %ld, fighter accessor %llx (object id %x): work int 0x1000010a = %lld", g_life_frames, (unsigned long long)boma, *(uint32_t *)(uintptr_t)(boma + 8), (long long)v);
    }
    for (int i = 0; i < g_nownagent && i < 64; i++) {
        uint64_t a = g_ownagent[i], wb = readable(a + 0x40, 8) ? *(uint64_t *)(uintptr_t)(a + 0x40) : 0;
        if (!wb || !readable(wb + 8, 4)) continue;
        int64_t v = (int32_t)game_call2(c, fn, wb, 0x10000005u);
        if (v != g_ownagent_life[i] || !g_ownagent_seen[i]) {
            logf("[n12life] frame-line call %ld: own article agent %llx (dispatcher %d), accessor %llx id %x: LIFE %lld -> %lld", g_life_frames, (unsigned long long)a, g_ownagent_disp[i],
                 (unsigned long long)wb, *(uint32_t *)(uintptr_t)(wb + 8), (long long)g_ownagent_life[i], (long long)v);
            g_ownagent_life[i] = v; g_ownagent_seen[i] = 1;
        }
    }
    o_lifeframe(c);
}
static void forget_own_weapon(uint64_t w);
static void h_winit(Ctx *c, void *) {              // weapon init (weapon object, description)
    ensure_data();
    uint64_t w = c->x[0], d = c->x[1];
    int wk = *(int32_t *)(uintptr_t)(d + 0x9c);
    int32_t a8 = *(int32_t *)(uintptr_t)(d + 0xa8);
    // N12 (STAGE 1 of own articles, see the phase file 4.5): an own weapon kind (640 +) is not known to the engine's
    // per-weapon-kind lookups yet (run n12b: the pool made at the fighter init read a null there). For the init the
    // description presents the TEMPLATE's kind: the weapon made is the template's (its kind, agents, parameters);
    // what is own is the article row (generate id, max alive) of the fighter's own record.
    int own_wk = -1;
    if (wk >= NEW_WK_BASE && wk < NEW_WK_BASE + NEW_WK_MAX) {
        int fi = (wk - NEW_WK_BASE) / 8, ai = (wk - NEW_WK_BASE) % 8;
        int t = fi < MAXNEW && ai < g_nart[fi] ? g_art[fi][ai].tmpl_wk : -1;
        static long said;
        if (said++ < 6) logf("[n12] weapon init for the OWN weapon kind %d (%s's %s): the template's kind %d is presented for the init (stage 1)", wk, fi < MAXNEW ? g_name[fi] : "?", t >= 0 ? g_art[fi][ai].name : "?", t);
        if (t >= 0) { own_wk = wk; *(int32_t *)(uintptr_t)(d + 0x9c) = t; wk = t; }
    }
    // 2026-11-03 (owner: Mario's own fireball became invisible and harmless): a weapon object made AGAIN as an ordinary
    // weapon (the next match's pools reuse the memory of this one's) must not stay in the own-article tables, or it gets
    // the new fighter's agents and parameters. Every init of a non-own weapon takes its object out of them.
    if (own_wk < 0) forget_own_weapon(w);
    int stat = (wk >= 0 && wk < WEAPON_KINDS) ? ((int32_t *)(uintptr_t)OWNER_TABLE)[wk] : -1;
    int by_slot = entry_kind(a8 >= 0 ? (a8 & 7) : -1), by_init = t_init_kind;
    int live = by_init >= 0 ? by_init : by_slot;
    remember(g_weapon, &g_nweapon, 512, w, live);
    // stage 1: an own article's weapon takes its parameters from the TEMPLATE's owner (its param_<article> lives in
    // that owner's parameter file), so the owner row is NOT lent to the live owner for it
    // N13 (N13_LIVEPARAM=1): an own article's weapon init asks the LIVE owner's parameter object (the new fighter's),
    // as any weapon of a fighter does; its values come from the fighter's own file (h_wparam), so the template's owner
    // need not be in the match (run n13w1: building Mario's parameter objects as a dependency without his files loaded
    // trapped). Default: off (stage 1 of N12: the template owner's object).
    static int liveparam = -1; if (liveparam < 0) { const char *e = getenv("N13_LIVEPARAM"); liveparam = e && e[0] == '1'; }
    int sw = g_a0_workaround ? (o_winit(c), 0) : (own_wk >= 0 && !liveparam) ? (o_winit(c), 0) : with_owner(c, o_winit, wk, live);
    if (own_wk >= 0) *(int32_t *)(uintptr_t)(d + 0x9c) = own_wk;
    if (own_wk >= 0 && getenv("N12_LIFE_LOG") && g_nlife < 8) {
        int ok; g_life_w[g_nlife] = w; g_life_v[g_nlife] = game_work_int(c, w, 0x10000005u, &ok);
        logf("[n12life] own article weapon %llx after its init: life %lld (%s)", (unsigned long long)w, (long long)g_life_v[g_nlife], ok ? "read" : "NOT readable");
        if (!g_nlife && !o_lifeframe) api->install_name("_ZN7lua2cpp16L2CFighterCommon31sys_line_system_control_fighterEv", h_lifeframe, nullptr, &o_lifeframe);
        g_nlife++;
    }
    if (g_n_winit++ < g_weapon_log) {
        // 0x71033babd0 stores the owner's first parameter object (shared pointer) at weapon + 0x38d0 + 8
        uint8_t *P = *(uint8_t **)(uintptr_t)MOD_GA(0x71052bb3b0ull);
        unsigned long long got = *(unsigned long long *)(uintptr_t)(w + 0x38d8);
        unsigned long long a_stat = (P && stat >= 0) ? *(unsigned long long *)(P + 0x60 + 0x38 * stat + 0x10) : 0;
        unsigned long long a_live = (P && live >= 0) ? *(unsigned long long *)(P + 0x60 + 0x38 * live + 0x10) : 0;
        logf("[n4] weapon init: object %llx weapon kind 0x%x, static owner kind 0x%x, live owner (the fighter init running on this thread) 0x%x%s; the weapon's parameter object %llx; A[static owner] %llx, A[live owner] %llx -> %s",
             (unsigned long long)w, wk, stat, live, sw ? " (table row lent)" : "", got, a_stat, a_live,
             got == a_live ? (a_live == a_stat ? "same object" : "THE LIVE OWNER'S") : got == a_stat ? "the static owner's" : "neither");
    }
    (void)by_slot;
}
// ---- N13: THE TWO BOUNDED PER-WEAPON-KIND LOOKUPS (plan\phases\N13_own_articles.md section 2). The weapon set-up
// 0x710339e810 (weapon object, x1, w2 = weapon kind) stores at weapon + 0x3bb0 the per-kind native object of
// 0x71033be790(kind) (table 0x710529c400[kind] for kind < 615, ELSE one shared default object 0x710532e9a8) and at
// + 0x3bb8 the description of 0x71033aa1e0(kind) (jump table, bound 614, else the default record 0x710455cba8).
// It is called with the OWN kind (640 +) - the weapon init only lends the template's kind for its own duration - so an
// own article got the default object, whose class-table slot 6 (0x71033be290) does not fill the life of the
// generate description: + 60 stayed 0, the activation 0x71033a07d0 wrote LIFE 0 and the fireball's main loop removed
// the weapon on its first frame (runs n13a - n13c; Mario's object 0x71051e2bb8, slot 6 0x7103440050, wrote 70).
// Both lookups answer an own kind with its TEMPLATE's kind (the N4 "lend" pattern; the weapon IS the template's in
// the engine, what is own is its agent, statuses, scripts and parameters). N13_WKLOOK=0: off.
static HookOrig o_wklook[2]; static long g_n_wklook[2];
static void h_wklook(Ctx *c, void *u) {
    int i = (int)(intptr_t)u, wk = (int32_t)c->x[0];
    if (wk >= NEW_WK_BASE && wk < NEW_WK_BASE + NEW_WK_MAX) {
        int fi = (wk - NEW_WK_BASE) / 8, ai = (wk - NEW_WK_BASE) % 8;
        if (fi < MAXNEW) resolve_articles(fi);
        int t = fi < MAXNEW && ai < g_nart[fi] ? g_art[fi][ai].tmpl_wk : -1;
        if (g_n_wklook[i]++ < 6) logf("[n13] weapon-kind lookup %s asked for the OWN kind %d (caller %llx): answered for the template's kind %d", i ? "0x71033aa1e0 (description)" : "0x71033be790 (per-kind object)",
                                      wk, (unsigned long long)c->x[30], t);
        if (t >= 0) c->x[0] = (uint64_t)(uint32_t)t;
    }
    o_wklook[i](c);
}
// ---- N13: THE ARTICLE'S OWN PARAMETERS (phase file N13 section 4). The template's native weapon code reads its
// values through the WEAPON's work module, by group and key (Mario's fireball, per-kind object slot 6 0x7103440050:
// get_param_int("param_fireball", "life") -> generate description + 60 -> LIFE; work-module class table 0x7104f89490,
// slots + 584 / + 592 int, + 616 float), and those land in the TEMPLATE owner's parameter file. For a weapon of an own
// article both the template's group name ("param_<template weapon>") and the article's own ("param_<article>") are
// answered from the fighter's OWN vl.prc, list "param_<article>" entry 0 (the package's data: Waluigi's vl.prcxml adds
// exactly that list), for every key it has; any other group or key goes to the game as before. The weapons are
// recognised by their module accessor (the dispatchers' x1), recorded in h_wdisp. N13_OWNPARAM=0: off.
struct OwnPar { uint64_t key; int is_float; int32_t i; float f; };
static OwnPar g_opar[MAXNEW][8][40]; static int g_nopar[MAXNEW][8], g_opar_tried[MAXNEW][8];
static uint64_t g_opar_grp[MAXNEW][8][2];
// (2026-11-03, the owner's crash in his third match: host exception reading a freed module accessor's + 0x50 inside
// this hook. The weapon pools are made again for every match; the tables were 16 long, filled after two matches,
// kept the old matches' freed objects and stopped taking the new ones. Now rings of 32, and the work module of each
// accessor is read once when it is recorded, so the hook only compares pointers - nothing freed is ever read.)
static uint64_t g_ownacc[32], g_ownacc_wm[32], g_ownacc_obj[32]; static int g_ownacc_art[32][2], g_nownacc, g_ownacc_next;
static void forget_own_weapon(uint64_t w) {
    for (int k = 0; k < g_nownw; k++) if (g_ownw[k] == w) g_ownw[k] = 0;
    for (int k = 0; k < g_nownacc; k++) if (g_ownacc_obj[k] == w) { g_ownacc[k] = 0; g_ownacc_wm[k] = 0; g_ownacc_obj[k] = 0; }
}
static HookOrig o_wparam[4]; static long g_n_wparam_own, g_n_wparam_miss;
static void load_own_params(int fi, int ai) {
    if (g_opar_tried[fi][ai]) return;
    g_opar_tried[fi][ai] = 1;
    NewArticle *a = &g_art[fi][ai];
    const char *w = strchr(a->tmpl, '_');
    char g1[80], g2[80]; snprintf(g1, sizeof g1, "param_%s", w ? w + 1 : a->tmpl); snprintf(g2, sizeof g2, "param_%s", a->name);
    g_opar_grp[fi][ai][0] = hash40(g1); g_opar_grp[fi][ai][1] = hash40(g2);
    if (!g_reg || fi >= g_reg->count) return;
    char path[600]; snprintf(path, sizeof path, "%s\\fighter\\%s\\param\\vl.prc", g_reg->fighters[fi].folder, g_reg->fighters[fi].name);
    FILE *fp = fopen(path, "rb"); if (!fp) { logf("[n13] own parameters of %s's %s: %s not readable", g_name[fi], a->name, path); return; }
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    uint8_t *d = (uint8_t *)malloc((size_t)n + 16); size_t got = d ? fread(d, 1, (size_t)n, fp) : 0; fclose(fp);
    if (!d || got != (size_t)n || n < 16 || memcmp(d, "paracobn", 8) != 0) { free(d); logf("[n13] own parameters: %s is not a parameter file", path); return; }
    // the format (fighter_sdk\tools\brawl\prc.py): "paracobn", u32 hash table size, u32 reference table size, the hash
    // table (u64), the reference table, the tree; struct = 12, u32 count, u32 reference offset of {u32 hash index, u32
    // offset from the struct's type byte} entries; list = 11, u32 count, u32 offsets from the list's type byte
    uint32_t hs = *(uint32_t *)(d + 8), rs = *(uint32_t *)(d + 12);
    const uint64_t *hashes = (const uint64_t *)(d + 16); const uint8_t *ref = d + 16 + hs; size_t root = 16 + (size_t)hs + rs;
    #define PRC_OK(p, k) ((size_t)(p) + (k) <= (size_t)n)
    if (!PRC_OK(root, 9) || d[root] != 12) { free(d); return; }
    uint32_t cnt = *(uint32_t *)(d + root + 1), ro = *(uint32_t *)(d + root + 5);
    size_t grp = 0;
    for (uint32_t i = 0; i < cnt && PRC_OK(16 + hs + ro + 8ull * i, 8); i++) {
        const uint32_t *e = (const uint32_t *)(ref + ro + 8ull * i);
        if ((e[0] + 1ull) * 8 <= hs && (hashes[e[0]] & MASK40) == g_opar_grp[fi][ai][1]) grp = root + e[1];
    }
    if (grp && PRC_OK(grp, 9) && d[grp] == 11 && *(uint32_t *)(d + grp + 1) > 0) grp += *(uint32_t *)(d + grp + 5);      // a list: its entry 0
    if (!grp || !PRC_OK(grp, 9) || d[grp] != 12) { free(d); logf("[n13] own parameters: %s has no list or struct %s: the template's values stay", path, g2); return; }
    uint32_t fc = *(uint32_t *)(d + grp + 1), fro = *(uint32_t *)(d + grp + 5);
    char shown[400]; int sk = 0; shown[0] = 0;
    for (uint32_t i = 0; i < fc && g_nopar[fi][ai] < 40 && PRC_OK(16 + hs + fro + 8ull * i, 8); i++) {
        const uint32_t *e = (const uint32_t *)(ref + fro + 8ull * i);
        size_t p = grp + e[1];
        if ((e[0] + 1ull) * 8 > hs || !PRC_OK(p, 5)) continue;
        OwnPar *o = &g_opar[fi][ai][g_nopar[fi][ai]];
        o->key = hashes[e[0]] & MASK40; o->is_float = d[p] == 8;
        if (d[p] == 8) o->f = *(float *)(d + p + 1);
        else if (d[p] == 6 || d[p] == 7) o->i = *(int32_t *)(d + p + 1);
        else if (d[p] == 1 || d[p] == 3) o->i = d[p + 1];
        else if (d[p] == 2) o->i = (int8_t)d[p + 1];
        else if (d[p] == 4) o->i = *(int16_t *)(d + p + 1);
        else if (d[p] == 5) o->i = *(uint16_t *)(d + p + 1);
        else continue;
        g_nopar[fi][ai]++;
        if (sk < 360) sk += snprintf(shown + sk, sizeof shown - sk, " %llx=%s%g", (unsigned long long)o->key, o->is_float ? "" : "(int)", o->is_float ? (double)o->f : (double)o->i);
    }
    #undef PRC_OK
    free(d);
    logf("[n13] own parameters of %s's %s: %d values of %s entry 0 (%s), answered for the groups %s and %s:%s", g_name[fi], a->name, g_nopar[fi][ai], g2, path, g1, g2, shown);
}
static void h_wparam(Ctx *c, void *u) {             // work module get_param (x0 work module, x1 group, x2 key)
    int i = (int)(intptr_t)u;
    if (g_nownacc) {
        uint64_t wm = c->x[0], grp = c->x[1] & MASK40, key = c->x[2] & MASK40;
        for (int k = 0; k < g_nownacc; k++) {
            if (g_ownacc_wm[k] != wm) continue;
            int fi = g_ownacc_art[k][0], ai = g_ownacc_art[k][1];

            load_own_params(fi, ai);
            // N13 (run n13w11): the package's own script asks the group by the article's bare name (Waluigi's dice block:
            // get_param_int(weapon, hash40("diceblock"), hash40("life"))); the game has no such group (it answered 0
            // and the dice broke at once). Answered from the same own list - an interpretation, see the phase file 5.
            static int bare = -1; if (bare < 0) { const char *e = getenv("N13_BARENAME"); bare = !(e && e[0] == '0'); }
            if (grp != g_opar_grp[fi][ai][0] && grp != g_opar_grp[fi][ai][1] && !(bare && grp == hash40(g_art[fi][ai].name))) {
                if (g_n_wparam_miss++ < 8) logf("[n13] own article %s's weapon asked its work module for group %llx key %llx: not the article's group, the game answers", g_art[fi][ai].name,
                                                 (unsigned long long)grp, (unsigned long long)key);
                break;
            }
            static const uint64_t K_PENETRATION = hash40("is_penetration");
            if (g_art[fi][ai].keep_on_hit && key == K_PENETRATION) {
                if (i >= 2) { float f = 1.0f; memset(&c->v[0], 0, 16); memcpy(&c->v[0], &f, 4); } else c->x[0] = 1;
                static int said; if (said++ < 2) logf("[n13] own article %s: is_penetration answered 1 (fighter.json keep_on_hit): its own status code ends it on a hit", g_art[fi][ai].name);
                return;
            }
            for (int p = 0; p < g_nopar[fi][ai]; p++) {
                OwnPar *o = &g_opar[fi][ai][p];
                if (o->key != key) continue;
                if (i >= 2) { float f = o->is_float ? o->f : (float)o->i; memset(&c->v[0], 0, 16); memcpy(&c->v[0], &f, 4); }
                else c->x[0] = (uint64_t)(uint32_t)(o->is_float ? (int32_t)o->f : o->i);
                if (g_n_wparam_own++ < 16) logf("[n13] own article %s's weapon: parameter %llx.%llx answered from the fighter's own file: %g", g_art[fi][ai].name, (unsigned long long)grp,
                                                 (unsigned long long)key, o->is_float ? (double)o->f : (double)o->i);
                return;
            }
            if (g_n_wparam_miss++ < 8) logf("[n13] own article %s's weapon: key %llx of group %llx not in the own file: the game answers", g_art[fi][ai].name, (unsigned long long)key, (unsigned long long)grp);
            break;
        }
    }
    o_wparam[i](c);
}
// ---- N13: THE ARTICLE'S OWN FILES. The weapon resource request 0x71017eef20(manager, w1 weapon kind, ...) builds its
// paths with 0x71017e0840(out, w1 weapon kind, ...) from the two name tables 0x7105188240 (owner name) and
// 0x7105185bd0 (weapon name) by kind - 615 rows, no bound for an own kind. Run n13w9 (Waluigi, Mario not in the match):
// the request trapped on a null resource. For an own kind the call is made with the template's kind and, for its
// duration, that row's two names are the fighter's and the article's: "fighter/<fighter>/model/<article>/..." - the
// fighter's own files (the package's), which the registry serves from its folder. N13_OWNFILES=0: off.
static HookOrig o_wpath, o_wstart; static long g_n_wpath, g_n_wpath_t;
// run n13w10: at the generate the requests come with the TEMPLATE's kind (the weapon object's kind field holds it since
// the dispatchers, see h_wdisp). The weapon start 0x710339fee0 (x0 = the weapon object) marks, on its thread, that
// an own article's weapon is being started; inside it the template's kind is that article.
static __declspec(thread) int t_own_fi = -1, t_own_ai = -1;
static void h_wstart(Ctx *c, void *) {
    int fi = -1, ai = -1;
    for (int k = 0; k < g_nownw; k++) if (g_ownw[k] == c->x[0]) { fi = g_ownw_art[k][0]; ai = g_ownw_art[k][1]; }
    int pf = t_own_fi, pa = t_own_ai;
    t_own_fi = fi; t_own_ai = ai;
    o_wstart(c);
    t_own_fi = pf; t_own_ai = pa;
}
// ---- 2026-11-04 (owner: Mario's fireball flew as Waluigi's dice, or popped at his hand next to Phantom, while the new
// fighter had not thrown yet). The weapon resource manager keeps its loaded files in a map keyed by
// kind | a << 25 | b << 17 | type (0x71017ee9a0 get: w2 kind; 0x71017eef20 request: w1 kind). An own article's weapon
// start presented the TEMPLATE's kind (21, the object's kind field), so its model / motion files went into the map under
// Mario's fireball key: the first of the two to be set up won and the other got its files (run mfw2: Waluigi's dice
// start 10060000 asked key 21 before Mario's five fireballs). Inside an own article's start the two calls get the OWN
// kind (640 +): its own key (the fighter's preload already asked for it, 0x71017eec78 with 648) and, through h_wpath,
// its own paths; Mario's key holds Mario's files. N13_OWNRESKEY=0: off.
static HookOrig o_reskey[2]; static long g_n_reskey;
static void h_reskey(Ctx *c, void *u) {
    int i = (int)(intptr_t)u, r = i ? 1 : 2;
    if (t_own_fi >= 0 && t_own_fi < MAXNEW && t_own_ai >= 0 && t_own_ai < g_nart[t_own_fi] && (int32_t)c->x[r] == g_art[t_own_fi][t_own_ai].tmpl_wk) {
        if (g_n_reskey++ < 8) logf("[n13] weapon resource %s for %s's %s: the template's kind %d -> the own kind %d (own key, own files)", i ? "request 0x71017eef20" : "get 0x71017ee9a0",
                                   g_name[t_own_fi], g_art[t_own_fi][t_own_ai].name, (int)(int32_t)c->x[r], g_art[t_own_fi][t_own_ai].wk);
        c->x[r] = (uint64_t)(uint32_t)g_art[t_own_fi][t_own_ai].wk;
    }
    o_reskey[i](c);
}
// ... and the start's own read of that map: 0x710339fee0 + 0x4c0 builds the key from the kind it read at its top (w22 =
// object + 0xc, the template's) and looks it up with the map find 0x71017def60 (out, map, key) - run mfw3: with only
// the two calls above it found nothing under the template's key and the start read a null (crash). Inside an own
// article's start a key whose kind bits (0 .. 13; bits 14 and 16 are the get's type flag) are the template's is the
// own one.
static HookOrig o_resfind; static long g_n_resfind;
static void h_resfind(Ctx *c, void *) {
    if (t_own_fi >= 0 && t_own_fi < MAXNEW && t_own_ai >= 0 && t_own_ai < g_nart[t_own_fi] && (int)(c->x[2] & 0x3fff) == g_art[t_own_fi][t_own_ai].tmpl_wk) {
        uint64_t k = (c->x[2] & ~0x3fffull) | (uint64_t)(uint32_t)g_art[t_own_fi][t_own_ai].wk;
        if (g_n_resfind++ < 4) logf("[n13] weapon resource map find for %s's %s: key %llx -> %llx (own kind)", g_name[t_own_fi], g_art[t_own_fi][t_own_ai].name, (unsigned long long)c->x[2], (unsigned long long)k);
        c->x[2] = k;
    }
    o_resfind(c);
}
static void h_wpath(Ctx *c, void *) {
    int wk = (int32_t)c->x[1];
    if (t_own_fi >= 0 && t_own_fi < MAXNEW && t_own_ai < g_nart[t_own_fi] && g_art[t_own_fi][t_own_ai].tmpl_wk == wk) wk = g_art[t_own_fi][t_own_ai].wk;
    if (wk >= NEW_WK_BASE && wk < NEW_WK_BASE + NEW_WK_MAX) {
        int fi = (wk - NEW_WK_BASE) / 8, ai = (wk - NEW_WK_BASE) % 8;
        if (fi < MAXNEW) resolve_articles(fi);
        int t = fi < MAXNEW && ai < g_nart[fi] ? g_art[fi][ai].tmpl_wk : -1;
        if (t >= 0) {
            uint64_t *on = (uint64_t *)(uintptr_t)MOD_GA(0x7105188240ull), *wn = (uint64_t *)(uintptr_t)MOD_GA(0x7105185bd0ull);
            static int prot; DWORD old;
            if (!prot) { prot = 1; VirtualProtect(on, 8 * 615, PAGE_READWRITE, &old); VirtualProtect(wn, 8 * 615, PAGE_READWRITE, &old); }
            EnterCriticalSection(&g_lock);
            uint64_t ko = on[t], kw = wn[t];
            on[t] = (uint64_t)(uintptr_t)g_name[fi]; wn[t] = (uint64_t)(uintptr_t)g_art[fi][ai].name;
            c->x[1] = (uint64_t)(uint32_t)t;
            o_wpath(c);
            on[t] = ko; wn[t] = kw;
            LeaveCriticalSection(&g_lock);
            if (g_n_wpath++ < 6) logf("[n13] weapon file path for the OWN kind %d (caller %llx): built as %s / %s (template's row %d lent for the call)", wk, (unsigned long long)c->x[30], g_name[fi], g_art[fi][ai].name, t);
            return;
        }
    }
    if (g_nnew && g_n_wpath_t < 6) for (int n = 0; n < g_nnew; n++) for (int a = 0; a < g_nart[n]; a++)
        if (g_art[n][a].resolved && g_art[n][a].tmpl_wk == wk && g_n_wpath_t++ < 6)
            logf("[n13] weapon file path asked for the TEMPLATE's kind %d (caller %llx): the game's own names", wk, (unsigned long long)c->x[30]);
    o_wpath(c);
}
// 0x71017e88d0(out, x1, fighter kind, file type): builds the path of one of the kind's files and looks it up; the
// first word of `out` is what 0x710353e4e0 turns into a file index (0xffffff = the archive has no such path).
static HookOrig o_path; static long g_n_path;
static void h_path(Ctx *c, void *) {
    uint64_t out = c->x[0]; uint32_t kind = (uint32_t)c->x[2], type = (uint32_t)c->x[3];
    o_path(c);
    if (is_new(kind) && g_n_path++ < 40)
        logf("[n4] file path asked for kind 0x%x, file type 0x%x (caller %llx): result words %x %x %x %x", kind, type, (unsigned long long)c->x[30],
             *(uint32_t *)(uintptr_t)out, *(uint32_t *)(uintptr_t)(out + 4), *(uint32_t *)(uintptr_t)(out + 8), *(uint32_t *)(uintptr_t)(out + 12));
}
// ---- N13: THE ARTICLE'S OWN AGENT, made here (phase file N13 section 3). A game fighter's module makes its weapon
// agents in its create_agent_* factories: operator new, the shared module's BASE constructor (lua2cpp::L2CWeaponCommon
// for the status agent, L2CFighterAnimcmdBase for game / effect / sound), a class table whose own entries are the
// article's install (status agent, slot 9) and the destructors, and one sv_set_function_hash per script (Mario's
// fireball: tests\n6_s_factories.py list mario). For a NEW fighter's article whose DLL has `<article>__install`, the
// same is done from the DLL: the class table is a copy of the base class's own table (its destructors are the base's,
// right for an object the base constructor made) with slot 9 = the DLL's install; the scripts are the DLL's
// `<article>__<game_|effect_|sound_><name>` under hash40("<prefix><name>"). No other fighter's module is involved.
// The DLL is asked by name (fs_function_at, SDK runtime) and for guest addresses (fs_module_address,
// FS_EXPORT_MODULE_ADDRESS in its source). Returns 1 with the agent in c->x[0]. (The same written in the runtime,
// registry v4 nm_article_agent, needs a relinked exe: kept as source, not used - run n13w1..w8, section 7.)
typedef const char *(*FsFnAt)(int, int64_t *);
typedef uint64_t (*FsModAddr)(uint32_t);
static int own_article_agent(Ctx *c, int fi, int ai, int cat) {
    static long said, made;
    if (!g_reg || fi >= g_reg->count || cat < 0 || cat > 3) return 0;
    // the runtime loads a private copy "<stem>.run<pid>_<serial>.dll" (fighter_host.c): the newest one with the exports
    HMODULE dll = GetModuleHandleA(g_reg->fighters[fi].dll);
    if (!dll) {
        char stem[64]; snprintf(stem, sizeof stem, "%s", g_reg->fighters[fi].dll); char *dot = strrchr(stem, '.'); if (dot) *dot = 0;
        char pre[96]; snprintf(pre, sizeof pre, "%s.run%lu_", stem, (unsigned long)GetCurrentProcessId());
        HMODULE mods[1024]; DWORD need = 0; long best = -1;
        if (K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need))
            for (DWORD m = 0; m < need / sizeof(HMODULE) && m < 1024; m++) {
                char bn[MAX_PATH]; if (!K32GetModuleBaseNameA(GetCurrentProcess(), mods[m], bn, sizeof bn)) continue;
                if (_strnicmp(bn, pre, strlen(pre)) != 0) continue;
                long serial = atol(bn + strlen(pre));
                if (serial > best && GetProcAddress(mods[m], "fs_function_at")) { best = serial; dll = mods[m]; }
            }
    }
    FsFnAt fn_at = dll ? (FsFnAt)(void *)GetProcAddress(dll, "fs_function_at") : nullptr;
    FsModAddr mod_addr = dll ? (FsModAddr)(void *)GetProcAddress(dll, "fs_module_address") : nullptr;
    if (!fn_at || !mod_addr) { if (said++ < 4) logf("[n13] %s: the DLL %s has no fs_function_at / fs_module_address: the template's agents are kept", g_name[fi], g_reg->fighters[fi].dll); return 0; }
    const char *art = g_art[fi][ai].name; size_t la = strlen(art);
    char want[96]; snprintf(want, sizeof want, "%s__install", art);
    int64_t inst = -1, off; const char *n;
    for (int i = 0; (n = fn_at(i, &off)); i++) if (strcmp(n, want) == 0) inst = off;
    if (inst < 0) { if (said++ < 4) logf("[n13] %s: article %s: the DLL has no %s: the template's agents are kept", g_name[fi], art, want); return 0; }
    static const char *const ctor_name[2] = { "_ZN7lua2cpp15L2CWeaponCommonC2EPN3app12BattleObjectEPNS1_26BattleObjectModuleAccessorEP9lua_State",
                                              "_ZN7lua2cpp21L2CFighterAnimcmdBaseC2EPN3app12BattleObjectEPNS1_26BattleObjectModuleAccessorEP9lua_State" };
    static const char *const vt_name[2] = { "_ZTVN7lua2cpp15L2CWeaponCommonE", "_ZTVN7lua2cpp21L2CFighterAnimcmdBaseE" };
    static const uint32_t obj_size[2] = { 0x118, 0xc8 }, nslots[2] = { 15, 9 };   // Mario's fireball agents: the same sizes and slot counts
    static uint64_t *table[2][MAXNEW][8];
    int k = cat ? 1 : 0;
    (void)vt_name;
    uint64_t a_new = api->address("_Znwm"), a_ctor = api->address(ctor_name[k]);
    uint64_t a_hash = api->address("_ZN3lib8L2CAgent20sv_set_function_hashEPvN3phx6Hash40E");
    if (!a_new || !a_ctor || !a_hash) {
        if (said++ < 4) logf("[n13] %s: article %s: a game symbol is missing (new %llx, constructor %llx, script registration %llx): the template's agents are kept",
                             g_name[fi], art, (unsigned long long)a_new, (unsigned long long)a_ctor, (unsigned long long)a_hash);
        return 0;
    }
    uint64_t obj = c->x[1], acc = c->x[2], lua = c->x[3], keep[31];
    memcpy(keep, c->x, sizeof keep);
    c->x[0] = obj_size[k]; api->call(c, a_new);
    uint64_t agent = c->x[0];
    if (!agent) { memcpy(c->x, keep, sizeof keep); return 0; }
    c->x[0] = agent; c->x[1] = obj; c->x[2] = acc; c->x[3] = lua; api->call(c, a_ctor);
    // the class table: the BASE class's own (its constructor just stored it; the vtable symbols are not exported,
    // run n13w10), copied with its two header words; slot 9 of the status agent = the DLL's install
    if (!table[k][fi][ai]) {
        uint64_t vt = *(uint64_t *)(uintptr_t)agent;
        if (!readable(vt - 16, 8ull * (nslots[k] + 2))) { memcpy(c->x, keep, sizeof keep); logf("[n13] %s: article %s: base class table %llx not readable", g_name[fi], art, (unsigned long long)vt); return 0; }
        uint64_t *t = (uint64_t *)calloc(nslots[k] + 2, 8);
        for (uint32_t s = 0; s < nslots[k] + 2; s++) t[s] = *(uint64_t *)(uintptr_t)(vt - 16 + 8ull * s);
        if (!k) t[2 + 9] = mod_addr((uint32_t)inst);
        // 2026-10-16 (owner: "the match ends, the screen stays black, then it closes"): the base classes are abstract, so
        // their DELETING destructor (slot 1, what the game calls when it frees the weapon's agents at the end of a match)
        // is a trap word (0xe7ffdefe at 0x7200333780, run res1). Slot 1 = the complete destructor (slot 0): the agent is
        // destroyed as the base would, its 0x118 / 0xc8 bytes are not given back (a few KB a match).
        {   uint64_t d1 = t[2 + 0], d0 = t[2 + 1];
            int d0_trap = readable(d0, 4) && *(uint32_t *)(uintptr_t)d0 == 0xe7ffdefeu, d1_trap = readable(d1, 4) && *(uint32_t *)(uintptr_t)d1 == 0xe7ffdefeu;
            if (d0_trap && !d1_trap) t[2 + 1] = d1;
            logf("[n13] %s: article %s: %s agent destructors: complete %llx%s, deleting %llx%s%s", g_name[fi], art, k ? "animation-command" : "status",
                 (unsigned long long)d1, d1_trap ? " (a TRAP)" : "", (unsigned long long)d0, d0_trap ? " (a TRAP)" : "",
                 d0_trap && !d1_trap ? ": slot 1 = the complete destructor (the agent's memory is kept)" : "");
        }
        table[k][fi][ai] = t;
        logf("[n13] %s: article %s: %s agent class table made from the base class's %llx (%u slots%s; base slot 9 was %llx)", g_name[fi], art, k ? "animation-command" : "status",
             (unsigned long long)vt, nslots[k], k ? "" : "; slot 9 = the DLL's install", (unsigned long long)*(uint64_t *)(uintptr_t)(vt + 72));
    }
    *(uint64_t *)(uintptr_t)agent = (uint64_t)(uintptr_t)(table[k][fi][ai] + 2);
    int scripts = 0;
    if (k) {
        static const char *const pre[4] = { "", "game_", "effect_", "sound_" };
        for (int i = 0; (n = fn_at(i, &off)); i++) {
            if (strncmp(n, art, la) != 0 || n[la] != '_' || n[la + 1] != '_' || strncmp(n + la + 2, pre[cat], strlen(pre[cat])) != 0) continue;
            c->x[0] = agent; c->x[1] = mod_addr((uint32_t)off); c->x[2] = hash40(n + la + 2);
            api->call(c, a_hash);
            scripts++;
        }
    }
    memcpy(c->x, keep, sizeof keep);
    c->x[0] = agent;
    if (made++ < 12) logf("[n13] %s: article %s: OWN %s agent %llx made (base class of the shared module; %s; %d scripts of the DLL registered)", g_name[fi], art,
                          cat == 0 ? "status" : cat == 1 ? "game" : cat == 2 ? "effect" : "sound", (unsigned long long)agent, k ? "no install" : "install = the DLL's", scripts);
    return 1;
}
static void h_wdisp(Ctx *c, void *u) {             // weapon agent dispatcher (weapon object, ...)
    int i = (int)(intptr_t)u;
    uint64_t w = c->x[0];
    int wk = *(int32_t *)(uintptr_t)(w + 0xc);
    // N12 stage 1 (run n12f: the pool's weapons reached the dispatchers with the OWN kind 640 in the object's kind
    // field, so the agent name was built from rows 640 of the name tables): the object's kind field becomes the
    // template's kind, for good - every per-kind reader of the engine then sees a kind it knows.
    if (wk >= NEW_WK_BASE && wk < NEW_WK_BASE + NEW_WK_MAX) {
        int fi = (wk - NEW_WK_BASE) / 8, ai = (wk - NEW_WK_BASE) % 8;
        int t = fi < MAXNEW && ai < g_nart[fi] ? g_art[fi][ai].tmpl_wk : -1;
        static long said;
        if (said++ < 4) logf("[n12] weapon agent dispatcher %d: weapon object %llx has the OWN kind %d: its kind field becomes the template's %d (stage 1)", i, (unsigned long long)w, wk, t);
        if (t >= 0) { *(int32_t *)(uintptr_t)(w + 0xc) = t; wk = t; int slot = -1; for (int k = 0; k < g_nownw; k++) if (g_ownw[k] == w) slot = k;
            if (slot < 0) { slot = g_ownw_next; g_ownw_next = (g_ownw_next + 1) % 32; if (g_nownw < 32) g_nownw++; }
            g_ownw_art[slot][0] = fi; g_ownw_art[slot][1] = ai; g_ownw[slot] = w; }
    }
    int own_obj = 0, own_fi = -1, own_ai = -1;
    for (int k = 0; k < g_nownw; k++) if (g_ownw[k] == w) { own_obj = 1; own_fi = g_ownw_art[k][0]; own_ai = g_ownw_art[k][1]; }
    int live = recall(g_weapon, g_nweapon, w);
    // N5: the dispatchers run INSIDE the weapon init, which runs inside the owner's fighter init on this thread (the
    // object in x0 is not the one the weapon init got: nothing was ever recalled in N4): the live owner is that fighter
    if (live < 0 && t_init_kind >= 0) live = t_init_kind;
    // N13: THE ARTICLE'S OWN AGENT. Dispatcher i (0 status, 1 game, 2 effect, 3 sound) would make the TEMPLATE owner's
    // agent (its module's factory). For an own article whose fighter DLL brings `<article>__install`, the registry makes
    // the agent from the base class with the DLL's install and scripts (registry version 4); the factory's arguments
    // are the dispatcher's (x0 battle object, x1 module accessor, x2 lua state) moved up by one. N13_OWNAGENT=0: off.
    if (own_obj && own_fi >= 0 && readable(c->x[1] + 0x50, 8)) {   // N13: the weapon's module accessor, for its own parameters
        int slot = -1;
        for (int k = 0; k < g_nownacc; k++) if (g_ownacc[k] == c->x[1]) slot = k;
        if (slot < 0) { slot = g_ownacc_next; g_ownacc_next = (g_ownacc_next + 1) % 32; if (g_nownacc < 32) g_nownacc++; }
        g_ownacc_art[slot][0] = own_fi; g_ownacc_art[slot][1] = own_ai;
        g_ownacc_wm[slot] = *(uint64_t *)(uintptr_t)(c->x[1] + 0x50);
        g_ownacc_obj[slot] = w;
        g_ownacc[slot] = c->x[1];
    }
    static int ownagent = -1; if (ownagent < 0) { const char *e = getenv("N13_OWNAGENT"); ownagent = !(e && e[0] == '0'); }
    if (ownagent && own_obj && own_fi >= 0 && own_fi < g_nnew && own_ai < g_nart[own_fi] && g_reg) {
        uint64_t keep[4]; memcpy(keep, c->x, sizeof keep);
        c->x[3] = keep[2]; c->x[2] = keep[1]; c->x[1] = keep[0]; c->x[0] = 0;
        if (own_article_agent(c, own_fi, own_ai, i)) {
            if (g_nownagent < 64) { g_ownagent[g_nownagent] = c->x[0]; g_ownagent_disp[g_nownagent++] = i; }
            return;
        }
        memcpy(c->x, keep, sizeof keep);
    }
    { static long seen; if (seen++ < 8) logf("[n5] weapon agent dispatcher %d called: x0 %llx (weapon kind field 0x%x), x1 %llx, recorded live owner 0x%x, %d weapons recorded",
                                              i, (unsigned long long)w, wk, (unsigned long long)c->x[1], live, g_nweapon); }
    if (is_new((uint32_t)live)) {
        // the agent comes from the donor's script module: the table row stays the donor's for this call
        if (g_n_wdisp[i]++ < 2) logf("[n4] weapon agent dispatcher %d: weapon kind 0x%x of live owner 0x%x -> the donor module's factory%s", i, wk, live,
                                     g_reg ? ", the agent goes to the live owner's own module" : "");
        if (g_reg) {
            int32_t *t = (int32_t *)(uintptr_t)OWNER_TABLE;
            int so = static_owner(wk);
            if (g_reg->size >= offsetof(SsbuRegistry, agent_begin_weapon) + sizeof(void *) && g_reg->agent_begin_weapon) g_reg->agent_begin_weapon(g_name[live - FIRST]);
            else g_reg->agent_begin(g_name[live - FIRST]);
            EnterCriticalSection(&g_lock);              // the weapon init above us on this thread holds it already
            int32_t was = (wk >= 0 && wk < WEAPON_KINDS) ? t[wk] : 0;
            if (so >= 0) t[wk] = so;
            o_wdisp[i](c);
            if (so >= 0) t[wk] = was;
            LeaveCriticalSection(&g_lock);
            if (own_obj && g_nownagent < 64) { g_ownagent[g_nownagent] = c->x[0]; g_ownagent_disp[g_nownagent++] = i; }   // N12: the own article's agents (life log)
            g_reg->agent_end(g_name[live - FIRST], c->x[0]);
            return;
        }
    }
    o_wdisp[i](c);
    // N12 life log, control: the TEMPLATE's own weapons of a game fighter (same kind as the own articles' templates)
    if (getenv("N12_LIFE_LOG") && i == 0 && g_nownagent < 64) {
        int tmpl = 0;
        for (int n = 0; n < g_nnew; n++) { resolve_articles(n); for (int t = 0; t < g_nart[n]; t++) if (g_art[n][t].tmpl_wk == wk) tmpl = 1; }
        if (tmpl) { g_ownagent[g_nownagent] = c->x[0]; g_ownagent_disp[g_nownagent++] = 100 + i; }     // dispatcher 100 = control (game owner)
    }
    if (g_reg) g_reg->agent_end(nullptr, c->x[0]);
}
// ---- N5: the computer player. The eight AI functions N2 lists switch on a fighter kind they read from memory:
//   0x71002c5cf0, 0x71002c9900                 kind at [x0 + 0xc04]
//   0x710034b310, 0x7100351630, 0x710035fc50    kind at [x0 + 0x28]
//   0x7100362df0, 0x710036c1c0, 0x71003719c0    kind at [[[x0 - 8] + 0x168] + 0x28]   (script functions: x0 = lua state)
// A new kind takes the default branch everywhere (N4: the CPU stands still). Here the kind FIELD of such an object
// is given the donor's kind when one of these functions is entered with it: the AI then uses the donor's rows for
// the new fighter. N5_AI=0 switches it off. What else reads these fields was not examined.
static HookOrig o_ai[8]; static long g_n_ai[8]; static int g_ai = 1;
static void h_ai(Ctx *c, void *u) {
    int i = (int)(intptr_t)u;
    uint64_t p = 0;
    if (i < 2) p = c->x[0] + 0xc04;
    else if (i < 5) p = c->x[0] + 0x28;
    else if (readable(c->x[0] - 8, 8)) {
        uint64_t a = *(uint64_t *)(uintptr_t)(c->x[0] - 8);
        if (readable(a + 0x168, 8)) p = *(uint64_t *)(uintptr_t)(a + 0x168) + 0x28;
    }
    if (g_ai && p && readable(p, 4) && is_new(*(uint32_t *)(uintptr_t)p)) {
        uint32_t was = *(uint32_t *)(uintptr_t)p;
        const int DONOR = dk(was);
        *(uint32_t *)(uintptr_t)p = DONOR;
        if (g_n_ai[i]++ < 3) logf("[n5] AI function %d: the kind field at %llx was 0x%x -> the donor's kind %d (written once per object)", i, (unsigned long long)p, was, DONOR);
    }
    o_ai[i](c);
}
// ---- N5: Kirby's copy resources. 0x71017f0bd0(x0, x1 -> fighter kind, ...) takes a row of the pointer table
// 0x710509ece0[kind] (8 bytes per kind; row -> the kind's copy-ability name strings) and builds resource paths from
// it (0x71017f06e0); the table has no row for a new kind (N4 run r2: the crash while loading). For the call the
// kind the function reads is the donor's: Kirby gets the DONOR's copy ability for a new fighter. N5_KIRBY=0: off.
static HookOrig o_kirby; static long g_n_kirby; static int g_kirby = 1;
static void h_kirby(Ctx *c, void *) {
    uint32_t *p = (uint32_t *)(uintptr_t)c->x[1];
    if (g_kirby && readable(c->x[1], 4) && is_new(*p)) {
        uint32_t was = *p;
        if (g_n_kirby++ < 4) logf("[n5] copy-ability resources asked for kind 0x%x (caller %llx): the donor's row of the table is used", was, (unsigned long long)c->x[30]);
        *p = (uint32_t)dk(was); o_kirby(c); *p = was;
        return;
    }
    o_kirby(c);
}
// ---- Kirby's copy of a new fighter (2026-11-04). Kirby copies by kind: app::FighterSpecializer_Kirby::copy_setup
// (accessor, slot, FighterKind, bool, bool) at 0x7100ba0e80 reads two per-kind pointer tables that have rows for
// the 94 real kinds only, 0x7104fcd098[kind] (copy description) and 0x7104fcd388[kind] (the copy's motion remap,
// also read by 0x71006e2680 / 0x71006e27f0 / 0x71006e29f0 / 0x7100ba4090 and four FighterMotionModuleImpl lua binds),
// and writes the kind into Kirby's work int 0x100000fc. For a new kind the rows are other data (row 0x5e+n of the
// first table is row n of the second): run kirby3 crashed in the weapon init called by copy_setup. Tried and dropped
// (runs kirby4-12): the donor's kind (the copy's files are the victim's kirbycopy folder, loaded for the kinds in the
// match only: the motion module found nothing for the donor's kind and crashed); the new kind with the donor's rows
// swapped in during each of those calls (the copy's hat article then crashed in its init, 0x710341b040).
// What is done: a new fighter is swallowed as another Kirby is (kind 6: copy_setup's own "no ability" path, which
// does not touch the tables nor the work int): no crash, no copy ability. N5_KIRBY=0: off.
static HookOrig o_copysetup; static long g_n_copysetup;
static void h_copysetup(Ctx *c, void *) {
    uint32_t kind = (uint32_t)c->x[2];
    if (is_new(kind)) {
        if (g_n_copysetup++ < 8) logf("[n12] Kirby swallowed new kind 0x%x (%s): no copy ability (copy_setup gets kind 6, as for another Kirby)", kind, g_name[kind - FIRST]);
        c->x[2] = 6;
    }
    o_copysetup(c);
}
static void h_fireball(Ctx *c, void *) {           // (article, owner's module accessor)
    uint64_t acc = c->x[1];
    uint32_t id = readable(acc + 8, 4) ? *(uint32_t *)(uintptr_t)(acc + 8) : 0;
    int kind = 0x7f;
    for (int i = 0; i < g_nfighter; i++) if (id && g_fighter[i].id == id) kind = g_fighter[i].kind & 0x7f;   // (2026-11-03 crash +0x45d4: the remembered objects of past matches are freed: their ids were read at their init, never again)
    long n = ++g_n_fire[kind];
    if (n <= 3 || n % 10 == 0) {
        uint64_t vt = readable(c->x[0], 8) ? *(uint64_t *)(uintptr_t)c->x[0] : 0;
        logf("[n4] fireball article function: owner object id %x, owner kind 0x%x: number %ld of that kind (article %llx, class table %llx, slot 2 %llx; caller %llx)", id, kind, n,
             (unsigned long long)c->x[0], (unsigned long long)vt, (unsigned long long)(readable(vt + 16, 8) ? *(uint64_t *)(uintptr_t)(vt + 16) : 0), (unsigned long long)c->x[30]);
    }
    if (n == 1 && kind < 0x72 && getenv("N4_PARAM_DUMP")) {
        // read-back for the loose-file proof: the kind's first parameter object (A[kind] + 0x10; the donor's class, 0x1c8 bytes)
        uint8_t *P = *(uint8_t **)(uintptr_t)MOD_GA(0x71052bb3b0ull);
        uint64_t obj = P ? *(uint64_t *)(P + 0x60 + 0x38 * kind + 0x10) : 0;
        for (int off = 0; obj && readable(obj, 0x1c8) && off < 0x1c8; off += 0x40) {
            char line[400]; int k = 0;
            for (int j = 0; j < 0x40 && off + j < 0x1c8; j += 4) k += snprintf(line + k, sizeof line - k, " %08x", *(uint32_t *)(uintptr_t)(obj + off + j));
            logf("[n4] parameter object of kind 0x%x +0x%03x:%s", kind, off, line);
        }
        // the object is a row of vectors {begin, end, capacity}: count the 32-bit words of their contents that carry
        // the test mark (low 12 bits 0x5a5, tests\n4_prc_mark.py) and show the first ones
        int words = 0, marked = 0, shown = 0; char first[400]; int fk = 0; first[0] = 0;
        for (int off = 8; obj && readable(obj, 0x1c8) && off + 16 <= 0x1c8; off += 8) {
            uint64_t b = *(uint64_t *)(uintptr_t)(obj + off), e = *(uint64_t *)(uintptr_t)(obj + off + 8);
            if (b < 0x10000 || e <= b || e - b > 0x4000 || !readable(b, (size_t)(e - b))) continue;
            for (uint64_t p = b; p + 4 <= e; p += 4) {
                uint32_t w = *(uint32_t *)(uintptr_t)p; words++;
                if ((w & 0xfff) == 0x5a5) { marked++; if (shown++ < 10) fk += snprintf(first + fk, sizeof first - fk, " [+0x%x][%d]=%08x (%g)", off, (int)(p - b) / 4, w, *(float *)(uintptr_t)p); }
            }
            off += 16;
        }
        logf("[n4] parameter object of kind 0x%x: %d words in its vectors, %d carry the mark 5a5:%s", kind, words, marked, first);
    }
    // N12 (own articles, stage 1; N12_ARTWRAP=0: off): the template's native article function run for a NEW owner
    // sees the owner's kind field as the TEMPLATE OWNER's for the call only (the callback-wrap idea, scoped to it)
    if (articles_on() && is_new((uint32_t)kind) && !(getenv("N12_ARTWRAP") && getenv("N12_ARTWRAP")[0] == '0')) {
        int n = kind - FIRST, tk = -1;
        for (int t = 0; t < g_nart[n]; t++) if (g_art[n][t].fn[0] == MOD_GA(0x7100cb9de0ull)) tk = g_art[n][t].owner_kind;
        uint64_t obj = 0;
        for (int i = 0; i < g_nfighter; i++) if (id && g_fighter[i].id == id) obj = g_fighter[i].obj;
        if (tk >= 0 && obj && readable(obj + 0xc, 4)) {
            uint32_t *p = (uint32_t *)(uintptr_t)(obj + 0xc), was = *p;
            static long said; if (said++ < 4) logf("[n12] template article function 7100cb9de0 for the new owner 0x%x: the owner's kind field is the template owner's (%d) for the call", was, tk);
            *p = (uint32_t)tk; o_fireball(c); *p = was;
            return;
        }
    }
    o_fireball(c);
}
// ---- U1 (2026-10-16): THE NEW FIGHTER'S COLOUR. The select screens give a player a colour 0..7 (the CPU of the test
// menus gets 3); a new fighter's folder brings only the colours it has (model\body\c00 for Waluigi and Phantom). For
// a missing colour the paths fighter/<name>/.../c03 fell back to the DONOR's c03 (run u1both: ganon's c03 model
// with phantomg's own c00 group, then a null read in the file loader 0x710355f8f0 under 0x710060b6b0). The game's
// player-record function 0x7101788260 (x0 panel state, x1 player, x3 record: + 8 fighter kind, + 0x19 colour; see
// debug_mode.c) is followed here: a new kind whose folder has no model\body\c<colour> plays colour 0.
// N12_COLOUR0=0: off.
static HookOrig o_precord; static long g_n_colour;
static int colour_present(int n, int colour) {
    if (!g_reg || n < 0 || n >= g_reg->count) return 1;
    char path[600]; snprintf(path, sizeof path, "%s\\fighter\\%s\\model\\body\\c%02d", g_reg->fighters[n].folder, g_reg->fighters[n].name, colour);
    DWORD at = GetFileAttributesA(path);
    return at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY);
}
static void h_precord(Ctx *c, void *) {
    uint64_t rec = c->x[3]; int idx = (int)c->x[1];
    o_precord(c);
    if (!readable(rec, 0x20)) return;
    int kind = (int)(int8_t)*(uint8_t *)(uintptr_t)(rec + 8), colour = *(uint8_t *)(uintptr_t)(rec + 0x19);
    if (!is_new((uint32_t)kind) || colour == 0 || colour_present(kind - FIRST, colour)) return;
    *(uint8_t *)(uintptr_t)(rec + 0x19) = 0;
    if (g_n_colour++ < 8) logf("[u1] player %d: new fighter %s (kind 0x%x) chosen with colour %d, which its folder does not have (no model\\body\\c%02d): colour 0", idx, g_name[kind - FIRST], kind, colour, colour);
}
extern "C" __declspec(dllexport) void mod_init(const HookApi *a) {
    api = a;
    const char *e = getenv("N3_NEWKIND");
    char reglist[200] = "";
    g_reg = ssbu_registry_get();
    if (g_reg && g_reg->count > 0) {
        for (int k = 0; k < g_reg->count && k < MAXNEW; k++) {
            size_t n = strlen(reglist); snprintf(reglist + n, sizeof reglist - n, "%s%s", k ? "," : "", g_reg->fighters[k].name);
            snprintf(g_donor_name[k], 32, "%s", g_reg->fighters[k].donor);      // N6: the donor of THIS fighter
        }
        e = reglist;
        g_reg->start();
    } else g_reg = nullptr;
    if (!e || !*e) { api->log("[n4] kind mod: no fighter registry and N3_NEWKIND not set, nothing installed"); return; }
    InitializeCriticalSection(&g_lock);
    char list[200]; snprintf(list, sizeof list, "%s", e);
    for (char *p = strtok(list, ","); p && g_nnew < MAXNEW; p = strtok(nullptr, ",")) {
        char kn[80]; snprintf(kn, sizeof kn, "fighter_kind_%s", p);
        g_hash[g_nnew] = hash40(kn);
        snprintf(g_name[g_nnew], 32, "%s", p); snprintf(g_upper[g_nnew], 32, "%s", p); snprintf(g_sound[g_nnew], 32, "%s", p);
        for (char *q = g_upper[g_nnew]; *q; q++) if (*q >= 'a' && *q <= 'z') *q -= 32;
        if (g_sound[g_nnew][0] >= 'a' && g_sound[g_nnew][0] <= 'z') g_sound[g_nnew][0] -= 32;
        if (!g_donor_name[g_nnew][0]) { const char *d = getenv("N3_DONOR"); snprintf(g_donor_name[g_nnew], 32, "%s", d && *d ? d : "mario"); }
        g_nnew++;
    }
    resolve_donors();                                   // when the game's data is mapped already; else at the first hook
    if (g_reg) for (int k = 0; k < g_nnew && k < g_reg->count; k++) read_articles(k);      // N12: fighter.json "articles"
    g_a0_workaround = (e = getenv("N4_A0_WORKAROUND")) && atoi(e);
    g_ownname = (e = getenv("N4_OWNNAME")) ? atoi(e) : g_reg ? (g_reg->fighters[0].own_name ? 15 : 14) : 0;
    g_row = (e = getenv("N4_ROW")) ? atoi(e) : g_reg ? 1 : 0;
    if ((e = getenv("N4_ROW_FIND"))) g_row_find = (uint32_t)strtoul(e, nullptr, 16);
    if ((e = getenv("N4_ROW_SET"))) g_row_set = (uint32_t)strtoul(e, nullptr, 16);
    if ((e = getenv("N4_WEAPON_LOG"))) g_weapon_log = atoi(e);
    static const uint64_t disp[9] = { MOD_GA(0x710064bbd0ull), MOD_GA(0x710064c310ull), MOD_GA(0x710064c930ull), MOD_GA(0x710064cf50ull), MOD_GA(0x710064d570ull), MOD_GA(0x710064db90ull), MOD_GA(0x710064e2a0ull), MOD_GA(0x710064e8b0ull), MOD_GA(0x710064eec0ull) };
    static const uint64_t wdisp[4] = { MOD_GA(0x71033abee0ull), MOD_GA(0x71033acde0ull), MOD_GA(0x71033add40ull), MOD_GA(0x71033aeca0ull) };
    int ok = 0, all = 0;
    all++; ok += !!api->install(MOD_GA(0x710066cd40ull), h_hash, nullptr, &o_hash);
    all++; ok += !!api->install(MOD_GA(0x710064b730ull), h_record, nullptr, &o_record);
    all++; ok += !!api->install(MOD_GA(0x710068d530ull), h_callbacks, nullptr, &o_callbacks);
    for (int i = 0; i < 9; i++) { all++; ok += !!api->install(disp[i], h_disp, (void *)(intptr_t)i, &o_disp[i]); }
    all++; ok += !!api->install(MOD_GA(0x710070c580ull), h_build, (void *)0, &o_build[0]);
    all++; ok += !!api->install(MOD_GA(0x7100721ca0ull), h_build, (void *)1, &o_build[1]);
    all++; ok += !!api->install(MOD_GA(0x710033ba50ull), h_get, (void *)0, &o_get[0]);
    all++; ok += !!api->install(MOD_GA(0x710033c510ull), h_get, (void *)1, &o_get[1]);
    all++; ok += !!api->install(MOD_GA(0x7100709d60ull), h_loader, nullptr, &o_loader);
    all++; ok += !!api->install(MOD_GA(0x71006867e0ull), h_init, nullptr, &o_init);
    all++; ok += !!api->install(MOD_GA(0x710066ee40ull), h_setup, nullptr, &o_setup);
    all++; ok += !!api->install(MOD_GA(0x71033b5640ull), h_winit, nullptr, &o_winit);
    for (int i = 0; i < 4; i++) { all++; ok += !!api->install(wdisp[i], h_wdisp, (void *)(intptr_t)i, &o_wdisp[i]); }
    all++; ok += !!api->install(MOD_GA(0x7100cb9de0ull), h_fireball, nullptr, &o_fireball);
    all++; ok += !!api->install(MOD_GA(0x71017e88d0ull), h_path, nullptr, &o_path);
    if (!((e = getenv("N12_COLOUR0")) && e[0] == '0')) { all++; ok += !!api->install(MOD_GA(0x7101788260ull), h_precord, nullptr, &o_precord); }      // U1: colour of a new fighter
    if (!((e = getenv("N13_WKLOOK")) && e[0] == '0'))       // N13: the two bounded per-weapon-kind lookups (phase file N13 section 2)
        for (int i = 0; i < 2; i++) { all++; ok += !!api->install(i ? MOD_GA(0x71033aa1e0ull) : MOD_GA(0x71033be790ull), h_wklook, (void *)(intptr_t)i, &o_wklook[i]); }
    if (!((e = getenv("N13_OWNFILES")) && e[0] == '0')) {
        all++; ok += !!api->install(MOD_GA(0x71017e0840ull), h_wpath, nullptr, &o_wpath);
        all++; ok += !!api->install(MOD_GA(0x710339fee0ull), h_wstart, nullptr, &o_wstart);
        if (!((e = getenv("N13_OWNRESKEY")) && e[0] == '0'))
            { for (int i = 0; i < 2; i++) { all++; ok += !!api->install(MOD_GA(i ? 0x71017eef20ull : 0x71017ee9a0ull), h_reskey, (void *)(intptr_t)i, &o_reskey[i]); }
              all++; ok += !!api->install(MOD_GA(0x71017def60ull), h_resfind, nullptr, &o_resfind); }
    }
    if (!((e = getenv("N13_OWNPARAM")) && e[0] == '0') && readable(MOD_GA(0x7104f89490ull) + 624, 8)) {      // N13: work-module parameter getters (int, int, float)
        // + 624 (class slot + 0x270, 0x71004e53e0 in 13.0.5): the float getter the template's NATIVE weapon code calls
        // directly (Mario's fireball start 0x7103440120 and bounce 0x71034402a0 read speed / angle / gravity / wall angle
        // through it): without it the dice block's native start set speed 0 (it stayed where it was made) and never bounced
        static const int slot[4] = { 584, 592, 616, 624 };
        for (int i = 0; i < 4; i++) { all++; ok += !!api->install(*(uint64_t *)(uintptr_t)(MOD_GA(0x7104f89490ull) + slot[i]), h_wparam, (void *)(intptr_t)i, &o_wparam[i]); }
        logf("[n13] own article parameters: work-module getters %llx %llx %llx %llx hooked", *(unsigned long long *)(uintptr_t)(MOD_GA(0x7104f89490ull) + 584),
             *(unsigned long long *)(uintptr_t)(MOD_GA(0x7104f89490ull) + 592), *(unsigned long long *)(uintptr_t)(MOD_GA(0x7104f89490ull) + 616),
             *(unsigned long long *)(uintptr_t)(MOD_GA(0x7104f89490ull) + 624));
    } else logf("[n13] own article parameters: work-module class table not readable at start: NOT hooked");
    {
        e = getenv("N6_KINDFIELD");
        g_cbwrap = !(getenv("N7_CBWRAP") && getenv("N7_CBWRAP")[0] == '0');
        if (getenv("N7_CBWRAP") && getenv("N7_CBWRAP")[0] == '2') g_cbwrap = 2;         /* N8: observe only (log what arrives, change nothing) */
        {   /* N8: the classes (first word of the object) whose kind field is presented as the donor's; N8_CB_VT=<hex,..>, "any" = every object with a new kind at + 0xc */
            const char *ve = getenv("N8_CB_VT");
            char vl[300]; snprintf(vl, sizeof vl, "%s", ve ? ve : N8_CB_VT_DEFAULT);
            for (char *p = strtok(vl, ","); p && g_n_cbvt < 8; p = strtok(nullptr, ",")) { uint64_t v = strtoull(p, nullptr, 16); if (v) g_cbvt[g_n_cbvt++] = v; }
            logf("[n8] donor callbacks wrap: mode %d (0 off, 1 the donor's kind is presented, 2 observe only); %d object class(es) recognised%s", g_cbwrap, g_n_cbvt, g_n_cbvt ? "" : " (none given: the battle-object shape: a new kind at + 0xc and a module accessor at + 0x20 that carries the object's id)");
        }
        /* N7: no default any more (N6: 7100c280f0): the general wrap of the donor's callbacks covers it */
        char kl[300]; snprintf(kl, sizeof kl, "%s", e ? e : ""); int nk = 0;
        for (char *p = strtok(kl, ","); p && nk < 8; p = strtok(nullptr, ","), nk++) {
            g_kf_addr[nk] = strtoull(p, nullptr, 16);
            if (!g_kf_addr[nk]) continue;
            all++; ok += !!api->install(g_kf_addr[nk], h_kindfield, (void *)(intptr_t)nk, &o_kf[nk]);
        }
    }
    if ((e = getenv("N6_TRACE_MAX"))) g_trace_max = atol(e);
    if ((e = getenv("N6_TRACE")) && *e) {
        char tl[300]; snprintf(tl, sizeof tl, "%s", e); int nt = 0;
        for (char *p = strtok(tl, ","); p && nt < 8; p = strtok(nullptr, ","), nt++) {
            g_trace_addr[nt] = strtoull(p, nullptr, 16);
            if (!api->install(g_trace_addr[nt], h_trace, (void *)(intptr_t)nt, &o_trace[nt])) logf("[n6trace] hook on %llx failed", (unsigned long long)g_trace_addr[nt]);
        }
    }
    if (!((e = getenv("N6_SETUP2")) && e[0] == '0')) { all++; ok += !!api->install(MOD_GA(0x71006cdd20ull), h_setup2, nullptr, &o_setup2); }
    static const uint64_t ai[8] = { MOD_GA(0x71002c5cf0ull), MOD_GA(0x71002c9900ull), MOD_GA(0x710034b310ull), MOD_GA(0x7100351630ull), MOD_GA(0x710035fc50ull), MOD_GA(0x7100362df0ull), MOD_GA(0x710036c1c0ull), MOD_GA(0x71003719c0ull) };
    g_kirby = !((e = getenv("N5_KIRBY")) && e[0] == '0');
    if (g_kirby) { all++; ok += !!api->install(MOD_GA(0x71017f0bd0ull), h_kirby, nullptr, &o_kirby); }
    if (g_kirby) { all++; ok += !!api->install(MOD_GA(0x7100ba0e80ull), h_copysetup, nullptr, &o_copysetup); }
    g_ai = !((e = getenv("N5_AI")) && e[0] == '0');
    if (g_ai) for (int i = 0; i < 8; i++) { all++; ok += !!api->install(ai[i], h_ai, (void *)(intptr_t)i, &o_ai[i]); }
    char donors[1200]; int dn = 0; donors[0] = 0;   // U16: was 200 (overran with 16 new kinds)
    for (int k = 0; k < g_nnew && dn < (int)sizeof donors - 80; k++) dn += snprintf(donors + dn, sizeof donors - dn, "%s%s<-%s", k ? ", " : "", g_name[k], g_donor_name[k]);
    logf("[n4] kind mod loaded (n7_kind: donor per kind, donor callbacks wrapped): %d new kind(s) from 0x%x (first: %s, hash %llx), donors: %s; weapons: %s; own names %d; %d of %d hooks",
         g_nnew, FIRST, g_name[0], (unsigned long long)g_hash[0], donors, g_a0_workaround ? "N3 workaround (A[0])" : "live owner", g_ownname, ok, all);
}
