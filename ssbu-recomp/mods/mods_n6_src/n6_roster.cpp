// n4_roster.cpp - TEST MOD for plan\phases\N4_own_fighter.md: mods_n3_src\n3_roster.cpp with
//   N3_NEWKIND=<name>[,<name2>..]  row n3t<k> gets fighter_kind_<name k> (several new kinds);
//   N4_UNLOCK_LOG=1                log every distinct (mode, x2, caller) of the availability question for a new id;
//   N4_UNLOCK_MODES=<bit mask>     answer "as the donor" only for the modes whose bit is set (default all: 0xffffffff);
//                                  for the other modes the new id gets N4_UNLOCK_ELSE (default 0 = not available).
//   N4_UNLOCK_CALLERS=<hex,hex..>  answer "as the donor" only to these callers (return addresses), whatever the mode.
// Build:  python fighter_sdk\tools\build_mod.py mods_n4_src\n4_roster.cpp mods_n4. The N3 text follows.
// n3_roster.cpp - TEST MOD for plan\phases\N3_new_kind.md, experiments (i) and (ii). NOT part of the port.
// Adds K rows to the game's character database (ui_chara_db) IN MEMORY: copies of one existing row with new ids
// ui_chara_n3t0 .. ui_chara_n3t<K-1>, placed last in the display order. Logs what the roster builder and the
// select-screen grid do with them. Nothing on disk is changed.
//   N3_ROWS=<K>            number of extra rows (default 0 = the mod does nothing but log)
//   N3_DONOR=<name>        the row that is copied (default mario; ui_chara_<name>)
//   N3_NEWKIND=<name>      row n3t0 gets fighter_kind = hash40("fighter_kind_<name>") (experiment (ii); see n3_kind.cpp)
// Build:  python fighter_sdk\tools\build_mod.py mods_n3_src\n3_roster.cpp mods_n3        Run: SSBU_MODS=mods_n3
//
// From N2 (read in the 13.0.4 binary): database object D = *(*(*(0x710532e730) + 8) + 0x168); inner = *(D + 8);
// inner + 0x18 -> parsed file (+0x20 hash table, +0x28 reference table); inner + 0x20 -> root struct;
// inner + 0x48 = hash of the root list; every accessor walks these pointers again on each call.
// Change against N2's mod (which gave up: "no memory below the database"): the list, ALL rows and the hash table are
// copied into one new block (any address), so every offset is inside the block and positive.
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <windows.h>
#include "hook.h"
#include "mod_ga.h"            // 13.0.5: game addresses through MOD_GA
#include "registry.h"

// N5: with the fighter registry (SSBU_CHARACTERS) row k is the registry's fighter k and its id is ui_chara_<name>;
// without it the rows are N4's ui_chara_n3t<k>.
static const SsbuRegistry *g_reg;
static void rid(char *id, size_t n, unsigned k) {
    if (g_reg && (int)k < g_reg->count) snprintf(id, n, "ui_chara_%s", g_reg->fighters[k].name);
    else snprintf(id, n, "ui_chara_n3t%u", k);
}

static const HookApi *api;
static HookOrig orig_roster, orig_grid;
static int patched;                 // 0 not yet, 1 done, -1 gave up
static int g_rows;
static uint64_t g_inner; static uint32_t g_first;      // set by try_patch: the database and the index of row n3t0
static char g_donor[48] = "mario", g_newkind[128 * 33];  // U1: 48 -> 300; KIND SLOTS: any number of names
#define NEWROWS 128                                      // KIND SLOTS: = the registry's NM_MAX (the 16 kind numbers are shared per match by n12_kind)
static char g_kinds[NEWROWS][32]; static int g_nkinds;        // N4: names of the new kinds, row k -> name k
static char g_row_donor[NEWROWS][32];                         // N6: the donor of row k (the manifest's "donor"); "" = g_donor
static const char *donor_of_row(int k) { return k >= 0 && k < NEWROWS && g_row_donor[k][0] ? g_row_donor[k] : g_donor; }
static int g_unlock_log; static uint32_t g_unlock_modes = 0xffffffffu; static int g_unlock_else;
static volatile long g_hide_new;                       // > 0 while a roster-builder call runs that must not see the new ids
static const uint64_t MASK40 = 0xffffffffffull;

static void logf(const char *fmt, ...) {
    char line[500]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    api->log(line);
}
static int readable(uint64_t a, size_t n) {
    MEMORY_BASIC_INFORMATION mi;
    if (a < 0x10000 || !VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi)) return 0;
    if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    return a + n <= (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize;
}
static uint64_t r64(uint64_t a) { uint64_t v; memcpy(&v, (void *)(uintptr_t)a, 8); return v; }
static uint32_t r32(uint64_t a) { uint32_t v; memcpy(&v, (void *)(uintptr_t)a, 4); return v; }
static int32_t ri32(uint64_t a) { int32_t v; memcpy(&v, (void *)(uintptr_t)a, 4); return v; }
static uint8_t r8(uint64_t a) { return *(uint8_t *)(uintptr_t)a; }
static uint32_t crc32(const char *s) {
    uint32_t c = 0xffffffffu;
    for (; *s; s++) { c ^= (uint8_t)*s; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1))); }
    return ~c;
}
static uint64_t hash40(const char *s) { return ((uint64_t)strlen(s) << 32) | crc32(s); }

// field of a struct param by hash: address of the value (its type byte), 0 if absent
static uint64_t field(uint64_t st, uint64_t hashes, uint64_t refs, uint64_t h) {
    if (r8(st) != 0x0c) return 0;
    uint32_t n = r32(st + 1); uint64_t e = refs + (int64_t)ri32(st + 5);
    for (uint32_t i = 0; i < n; i++)
        if (((r64(hashes + 8ull * r32(e + 8ull * i)) ^ h) & MASK40) == 0) { int32_t o = ri32(e + 8ull * i + 4); return o < 0 ? 0 : st + o; }
    return 0;
}
static uint64_t field_entry(uint64_t st, uint64_t hashes, uint64_t refs, uint64_t h) {      // address of the reference entry
    uint32_t n = r32(st + 1); uint64_t e = refs + (int64_t)ri32(st + 5);
    for (uint32_t i = 0; i < n; i++)
        if (((r64(hashes + 8ull * r32(e + 8ull * i)) ^ h) & MASK40) == 0) return e + 8ull * i;
    return 0;
}

static void try_patch(void) {
    if (patched || g_rows <= 0) return;
    uint64_t g = r64(MOD_GA(0x710532e730ull));
    if (!readable(g, 16)) return;
    uint64_t mgr = r64(g + 8);
    if (!readable(mgr + 0x168, 8)) return;
    uint64_t D = r64(mgr + 0x168);
    if (!readable(D, 16)) return;
    uint64_t inner = r64(D + 8);
    if (!readable(inner, 0x50)) return;
    uint64_t prc = r64(inner + 0x18), root = r64(inner + 0x20), root_hash = r64(inner + 0x48) & MASK40;
    if (!readable(prc, 0x30) || !readable(root, 9) || r8(root) != 0x0c) return;          // not loaded yet: try again at the next call
    uint64_t hashes = r64(prc + 0x20), refs = r64(prc + 0x28);
    if (!readable(hashes - 0x10, 0x18) || !readable(refs, 8)) return;
    patched = -1;
    if (memcmp((void *)(uintptr_t)(hashes - 0x10), "paracobn", 8) != 0) { logf("[n3] GIVE UP: no file header in front of the hash table (%llx)", (unsigned long long)hashes); return; }
    uint32_t nh = r32(hashes - 8) / 8;
    uint64_t list = field(root, hashes, refs, root_hash);
    if (!list || r8(list) != 0x0b) { logf("[n3] GIVE UP: root list not found"); return; }
    uint32_t n = r32(list + 1);
    char donor_id[80]; snprintf(donor_id, sizeof donor_id, "ui_chara_%s", g_donor);
    const uint64_t H_ID = hash40("ui_chara_id"), H_ORDER = hash40("disp_order"), H_KIND = hash40("fighter_kind"), H_DONOR = hash40(donor_id);
    uint64_t donor = 0, next = ~0ull, lo = ~0ull, hi = 0; int max_order = -1;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t row = list + (int64_t)ri32(list + 5 + 4ull * i);
        if (row < lo) lo = row;
        if (row > hi) hi = row;
        uint64_t id = field(row, hashes, refs, H_ID);
        if (id && r8(id) == 9 && ((r64(hashes + 8ull * r32(id + 1)) ^ H_DONOR) & MASK40) == 0) donor = row;
        uint64_t od = field(row, hashes, refs, H_ORDER);
        if (od && r8(od) == 2) { int v = (int8_t)r8(od + 1); if (v > max_order && v < 99) max_order = v; }
    }
    if (!donor) { logf("[n3] GIVE UP: the row %s was not found among %u rows", donor_id, n); return; }
    for (uint32_t i = 0; i < n; i++) { uint64_t row = list + (int64_t)ri32(list + 5 + 4ull * i); if (row > donor && row < next) next = row; }
    if (next == ~0ull || next - donor > 0x4000) { logf("[n3] GIVE UP: extent of the donor row unknown"); return; }
    size_t row_size = (size_t)(next - donor);
    if (lo <= list || !readable(lo, (size_t)(hi + row_size - lo))) { logf("[n3] GIVE UP: rows not behind the list or not readable (list %llx rows %llx..%llx)", (unsigned long long)list, (unsigned long long)lo, (unsigned long long)hi); return; }
    size_t K = (size_t)g_rows, old_bytes = (size_t)(hi + row_size - lo);
    size_t list_bytes = (5 + 4 * (n + K) + 15) & ~(size_t)15;
    size_t need = 0x10 + list_bytes + ((old_bytes + 15) & ~(size_t)15) + K * ((row_size + 15) & ~(size_t)15) + 8 * (nh + K + 8) + 0x100;
    uint8_t *mem = (uint8_t *)VirtualAlloc(nullptr, need, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!mem) { logf("[n3] GIVE UP: no memory"); return; }
    uint8_t *root2 = mem, *list2 = mem + 0x10, *old2 = list2 + list_bytes, *new2 = old2 + ((old_bytes + 15) & ~(size_t)15);
    uint8_t *hash2 = new2 + K * ((row_size + 15) & ~(size_t)15);
    memcpy(root2, (void *)(uintptr_t)root, 9);
    list2[0] = 0x0b; uint32_t n1 = n + (uint32_t)K; memcpy(list2 + 1, &n1, 4);
    memcpy(old2, (void *)(uintptr_t)lo, old_bytes);
    for (uint32_t i = 0; i < n; i++) {
        uint64_t row = list + (int64_t)ri32(list + 5 + 4ull * i);
        int32_t o32 = (int32_t)((old2 + (row - lo)) - list2); memcpy(list2 + 5 + 4ull * i, &o32, 4);
    }
    memcpy(hash2, (void *)(uintptr_t)hashes, 8ull * nh);
    uint64_t entry = field_entry(root, hashes, refs, root_hash);
    if (!entry) { logf("[n3] GIVE UP: root list entry not found"); return; }
    int kind_type = -1;
    for (size_t k = 0; k < K; k++) {
        uint8_t *row2 = new2 + k * ((row_size + 15) & ~(size_t)15);
        // N6: row k is a copy of ITS donor's row (per fighter), when that row is found and has the same extent as the
        // first donor's (the buffer was sized with it); else the first donor's row
        uint64_t donor_k = donor;
        if (strcmp(donor_of_row((int)k), g_donor) != 0) {
            char idk[80]; snprintf(idk, sizeof idk, "ui_chara_%s", donor_of_row((int)k));
            uint64_t hk = hash40(idk), found = 0, nextk = ~0ull;
            for (uint32_t i = 0; i < n; i++) {
                uint64_t row = list + (int64_t)ri32(list + 5 + 4ull * i);
                uint64_t id = field(row, hashes, refs, H_ID);
                if (id && r8(id) == 9 && ((r64(hashes + 8ull * r32(id + 1)) ^ hk) & MASK40) == 0) found = row;
            }
            for (uint32_t i = 0; found && i < n; i++) { uint64_t row = list + (int64_t)ri32(list + 5 + 4ull * i); if (row > found && row < nextk) nextk = row; }
            if (found && nextk != ~0ull && nextk - found == row_size) donor_k = found;
            logf("[n6] roster row %u (%s): donor row %s %s", (unsigned)k, k < (size_t)g_nkinds ? g_kinds[k] : "?", idk,
                 donor_k == found ? "found, copied" : found ? "found but of another size: the first donor's row is copied instead" : "NOT found: the first donor's row is copied instead");
        }
        memcpy(row2, (void *)(uintptr_t)donor_k, row_size);
        int32_t o32 = (int32_t)(row2 - list2); memcpy(list2 + 5 + 4ull * (n + k), &o32, 4);
        char id[64]; rid(id, sizeof id, (unsigned)k);
        uint64_t h = hash40(id); memcpy(hash2 + 8ull * (nh + k), &h, 8);
        uint64_t id2 = field((uint64_t)(uintptr_t)row2, hashes, refs, H_ID), od2 = field((uint64_t)(uintptr_t)row2, hashes, refs, H_ORDER);
        if (!id2 || !od2 || r8(od2) != 2) { logf("[n3] GIVE UP: fields of the copied row not found"); return; }
        uint32_t hi_idx = nh + (uint32_t)k; memcpy((void *)(uintptr_t)(id2 + 1), &hi_idx, 4);
        int order = max_order + 1 + (int)k; if (order > 126) order = 126;
        *(int8_t *)(uintptr_t)(od2 + 1) = (int8_t)order;
        if (k < (size_t)g_nkinds) {
            uint64_t fk = field((uint64_t)(uintptr_t)row2, hashes, refs, H_KIND);
            kind_type = fk ? r8(fk) : -2;
            if (fk && r8(fk) == 9) {
                char kn[80]; snprintf(kn, sizeof kn, "fighter_kind_%s", g_kinds[k]);
                uint64_t kh = hash40(kn); memcpy(hash2 + 8ull * (nh + K + k), &kh, 8);
                uint32_t ki = nh + (uint32_t)(K + k); memcpy((void *)(uintptr_t)(fk + 1), &ki, 4);
            }
        }
    }
    int32_t olist = (int32_t)(list2 - root2);
    // switch over: hash table, then the root list entry, then the root
    *(uint64_t *)(uintptr_t)(prc + 0x20) = (uint64_t)(uintptr_t)hash2;
    memcpy((void *)(uintptr_t)(entry + 4), &olist, 4);
    *(uint64_t *)(uintptr_t)(inner + 0x20) = (uint64_t)(uintptr_t)root2;
    g_inner = inner; g_first = n;
    patched = 1;
    logf("[n3] ui_chara_db patched: rows %u -> %u, new rows = copies of %s (%u bytes each), ids ui_chara_n3t0..%u (hash index %u..), disp_order from %d; fighter_kind of n3t0: %s (field type %d)",
         n, n1, donor_id, (unsigned)row_size, (unsigned)K - 1, nh, max_order + 1, g_newkind[0] ? g_newkind : "(the donor's)", kind_type);
}

// 0x7103269fd0(inner, hash40) -> row index, or -1: the database keeps a hash map id -> row index (bucket array at
// inner + 0x58, bucket count at + 0x60) that is built when the file is loaded, so it does not know the new rows.
// The lookup becomes ours for the new ids; every other id goes to the game's map.
static HookOrig orig_index;
static void my_index(Ctx *c, void *) {
    uint64_t inner = c->x[0], h = c->x[1] & MASK40;
    if (patched == 1 && inner == g_inner)
        for (int k = 0; k < g_rows; k++) {
            char id[64]; rid(id, sizeof id, (unsigned)k);
            if (h == hash40(id)) { c->x[0] = (uint32_t)(g_first + k); return; }
        }
    orig_index(c);
}
// 0x7101797640(ui_chara id, mode, x2) -> "is this fighter available": it asks the save data by id, which has no entry
// for a new id (run i2: the new row went to the list of the 75 locked fighters). A new row is available when its
// donor is: the question is asked for the donor's id.
static HookOrig orig_unlock;
static void my_unlock(Ctx *c, void *) {
    uint64_t h = c->x[0] & MASK40;
    static uint64_t ids[256]; static int nids;
    if (patched == 1 && !nids) { for (int k = 0; k < g_rows && k < 256; k++) { char id[80]; rid(id, sizeof id, (unsigned)k); ids[k] = hash40(id); } nids = g_rows < 256 ? g_rows : 256; }
    int is_new_id = 0, new_k = 0;
    for (int k = 0; k < nids; k++) if (h == ids[k]) { is_new_id = 1; new_k = k; break; }
    if (!is_new_id) { orig_unlock(c); return; }
    uint32_t mode = (uint32_t)c->x[1], x2 = (uint32_t)c->x[2]; uint64_t lr = c->x[30];
    int as_donor = mode < 32 ? (int)((g_unlock_modes >> mode) & 1) : 1;
    {   // N4_UNLOCK_CALLERS=<hex return address>[,<hex>..]: answer "as the donor" only to these callers
        static int ncall = -1; static uint64_t callers[16];
        if (ncall < 0) {
            ncall = 0;
            const char *e = getenv("N4_UNLOCK_CALLERS");
            char list[300]; snprintf(list, sizeof list, "%s", e ? e : "");
            for (char *p = strtok(list, ","); p && ncall < 16; p = strtok(nullptr, ",")) callers[ncall++] = strtoull(p, nullptr, 16);
        }
        if (ncall) { as_donor = 0; for (int i = 0; i < ncall; i++) if (callers[i] == lr) as_donor = 1; }
    }
    if (g_hide_new > 0) as_donor = 0;
    if (as_donor) {
        char id[80]; snprintf(id, sizeof id, "ui_chara_%s", donor_of_row(new_k)); c->x[0] = 0xc100000000000000ull | hash40(id);
        orig_unlock(c);
    } else c->x[0] = (uint64_t)g_unlock_else;
    if (g_unlock_log) {
        static struct { uint32_t mode, x2; uint64_t lr; long n; int ans; } seen[64]; static int nseen;
        int i; for (i = 0; i < nseen; i++) if (seen[i].mode == mode && seen[i].lr == lr && seen[i].x2 == x2) break;
        if (i == nseen && nseen < 64) { seen[i].mode = mode; seen[i].x2 = x2; seen[i].lr = lr; seen[i].n = 0; nseen++;
            logf("[n4] availability asked for a new id: mode %u, x2 %u, caller %llx -> answer %d (%s)", mode, x2, (unsigned long long)lr, (int)(c->x[0] & 1), as_donor ? "the donor's" : "fixed"); }
    }
}
static void my_roster(Ctx *c, void *) {            // 0x710326f0f0(out vector, w1..w6 options)
    uint64_t out = c->x[0]; unsigned mode = (unsigned)c->x[1]; uint64_t lr = c->x[30];
    try_patch();
    // N4_ROSTER_HIDE_FIRST=<n>: during the first n calls of the roster builder the new ids are "not available"
    // (run U2: the screens between the title and the main menu count the builder's OUTPUT; 79 extra entries there
    // pay out a milestone reward the test sequence cannot leave)
    static long calls; static int hide_first = -1;
    if (hide_first < 0) { const char *e = getenv("N4_ROSTER_HIDE_FIRST"); hide_first = e ? atoi(e) : 0; }
    long nr = ++calls;
    int hide = nr <= hide_first;
    {   // N4_ROSTER_HIDE_CALLERS=<hex return address>[,..]: the same, for the calls from these places
        static int nhc = -1; static uint64_t hc[16];
        if (nhc < 0) {
            nhc = 0;
            const char *e = getenv("N4_ROSTER_HIDE_CALLERS");
            char list[300]; snprintf(list, sizeof list, "%s", e ? e : "");
            for (char *p = strtok(list, ","); p && nhc < 16; p = strtok(nullptr, ",")) hc[nhc++] = strtoull(p, nullptr, 16);
        }
        for (int i = 0; i < nhc; i++) if (hc[i] == lr) hide = 1;
    }
    if (hide) g_hide_new++;
    orig_roster(c);
    if (hide) g_hide_new--;
    static int shown;
    if (shown < 40 && readable(out, 16)) { shown++; logf("[n3] roster builder call %ld from %llx: %lld entries (w1=%u, patched=%d%s)", nr, (unsigned long long)lr, (long long)((r64(out + 8) - r64(out)) / 8), mode, patched, hide ? ", new ids hidden" : ""); }
}
static void my_grid(Ctx *c, void *) {              // 0x71019eb840(this, x1, x2, flags, vector of roster ids)
    uint64_t self = c->x[0], vec = c->x[4]; long long in = readable(vec, 16) ? (long long)((r64(vec + 8) - r64(vec)) / 8) : -1;
    // N4_GRID_ADD=1: the list the select screen gets is the one built between title and main menu (caller
    // 0x7101779854), which is ALSO what the unlock-milestone screen counts (runs U3, U5, U7). To test the grid with
    // many icons without that reward, the new ids are hidden there and appended HERE to the grid's own input:
    // id = 0xc1 << 56 | database row << 40 | hash40 (the form of the entries that come in).
    static uint64_t ids2[512], vec2[3];
    const char *ga = getenv("N4_GRID_ADD");
    if (ga && atoi(ga) && patched == 1 && in >= 0 && in + g_rows <= 512) {
        uint64_t b = r64(vec);
        memcpy(ids2, (void *)(uintptr_t)b, (size_t)in * 8);
        for (int k = 0; k < g_rows; k++) {
            char id[64]; rid(id, sizeof id, (unsigned)k);
            ids2[in + k] = 0xc100000000000000ull | ((uint64_t)(g_first + k) << 40) | hash40(id);
        }
        vec2[0] = (uint64_t)(uintptr_t)ids2; vec2[1] = vec2[2] = (uint64_t)(uintptr_t)(ids2 + in + g_rows);
        logf("[n4] grid input: %lld entries (first %llx, last %llx) + %d new ids appended (first %llx) = %lld", in, (unsigned long long)ids2[0],
             (unsigned long long)(in ? ids2[in - 1] : 0), g_rows, (unsigned long long)ids2[in], in + g_rows);
        in += g_rows;
        c->x[4] = (uint64_t)(uintptr_t)vec2;
    }
    {   // N10, THE GUARD: the grid has layout rows for at most 91 entries (N4: with 92 it finds no layout row and the game
        // aborts). Until the layout is extended the entries behind the limit are NOT given to the grid (they are the
        // last of the display order: new fighters with "order": "last"); one warning per count. N10_GRID_MAX=<n>
        // changes the limit (for the test of the guard; 0 = no guard).
        static int gmax = -1; static long long warned = -1;
        if (gmax < 0) { const char *e = getenv("N10_GRID_MAX"); gmax = e && *e ? atoi(e) : 91; }
        uint64_t v = c->x[4];
        long long n = readable(v, 16) ? (long long)((r64(v + 8) - r64(v)) / 8) : -1;
        if (gmax > 0 && n > gmax && n <= 512) {
            static uint64_t ids3[512], vec3[3];
            memcpy(ids3, (void *)(uintptr_t)r64(v), (size_t)gmax * 8);
            vec3[0] = (uint64_t)(uintptr_t)ids3; vec3[1] = vec3[2] = (uint64_t)(uintptr_t)(ids3 + gmax);
            c->x[4] = (uint64_t)(uintptr_t)vec3;
            if (warned != n) { warned = n; logf("[n10] WARNING: the select screen was given %lld roster entries, its layout holds %d: the last %lld are NOT shown on the grid (first hidden id %llx); "
                                                "the fighters exist and can be started by other means (SSBU_DEBUG_MATCH)", n, gmax, n - gmax, (unsigned long long)(r64(r64(v) + 8ull * (uint64_t)gmax) & MASK40)); }
            in = gmax;
        }
    }
    orig_grid(c);
    if (!readable(self + 0x1d0, 0x48)) return;
    int rows[13], sum = 0; char txt[120]; int k = 0;
    for (int i = 0; i < 13; i++) { rows[i] = (int)r32(self + 0x1d4 + 4 * i); sum += rows[i]; k += snprintf(txt + k, sizeof txt - k, "%d,", rows[i]); }
    logf("[n3] grid returned: %lld roster entries in; layout rows %s sum %d; rows used %d; icon size %d", in, txt, sum, (int)r32(self + 0x208), (int)r32(self + 0x1d0));
}
extern "C" __declspec(dllexport) void mod_init(const HookApi *a) {
    api = a;
    const char *e = getenv("N3_ROWS"); g_rows = e ? atoi(e) : 0;
    if ((e = getenv("N3_DONOR")) && *e) snprintf(g_donor, sizeof g_donor, "%s", e);
    if ((e = getenv("N3_NEWKIND")) && *e) {
        snprintf(g_newkind, sizeof g_newkind, "%s", e);
        static char list[NEWROWS * 33]; snprintf(list, sizeof list, "%s", e);
        for (char *p = strtok(list, ","); p && g_nkinds < NEWROWS; p = strtok(nullptr, ",")) snprintf(g_kinds[g_nkinds++], 32, "%s", p);
    }
    g_reg = ssbu_registry_get();
    if (g_reg && g_reg->count > 0) {                    // N5: the manifests are the configuration
        g_rows = g_reg->count; g_nkinds = 0; g_newkind[0] = 0;
        snprintf(g_donor, sizeof g_donor, "%s", g_reg->fighters[0].donor);
        for (int k = 0; k < g_reg->count && k < NEWROWS; k++) {
            snprintf(g_row_donor[g_nkinds], 32, "%s", g_reg->fighters[k].donor);
            snprintf(g_kinds[g_nkinds++], 32, "%s", g_reg->fighters[k].name);
            size_t n = strlen(g_newkind); snprintf(g_newkind + n, sizeof g_newkind - n, "%s%s", k ? "," : "", g_reg->fighters[k].name);
        }
        logf("[n5] roster component: %d rows from the fighter registry: %s", g_rows, g_newkind);
    } else g_reg = nullptr;
    g_unlock_log = (e = getenv("N4_UNLOCK_LOG")) && atoi(e);
    if ((e = getenv("N4_UNLOCK_MODES")) && *e) g_unlock_modes = (uint32_t)strtoul(e, nullptr, 0);
    if ((e = getenv("N4_UNLOCK_ELSE")) && *e) g_unlock_else = atoi(e);
    if (!api->install(MOD_GA(0x710326f0f0ull), my_roster, nullptr, &orig_roster)) logf("[n3] roster hook failed: %s", api->error());
    if (!api->install(MOD_GA(0x71019eb840ull), my_grid, nullptr, &orig_grid)) logf("[n3] grid hook failed: %s", api->error());
    if (!api->install(MOD_GA(0x7103269fd0ull), my_index, nullptr, &orig_index)) logf("[n3] index hook failed: %s", api->error());
    if (!api->install(MOD_GA(0x7101797640ull), my_unlock, nullptr, &orig_unlock)) logf("[n3] unlock hook failed: %s", api->error());
    logf("[n3] roster mod loaded: %d extra ui_chara_db rows (copies of ui_chara_%s)", g_rows, g_donor);
}
