/* nn::ro's bookkeeping as the console's rtld keeps it, for Skyline plugins that walk it (Smashline:
 * its nro_hook, its unwind patches find nnSdk by an address): a ModuleObject for each loaded module
 * - the boot modules on the auto-load list, the NROs the game loads on the manual one - which the
 * SDK's nn::ro::detail::g_pAutoLoadList and g_pManualLoadList point at, filled from each module's
 * dynamic section (the module's own memory: its image is the console's). Lists are rtld's: a head
 * {front, back}, entries {next, prev, ...} back to the head.
 *
 * And nn::ro::LoadModuleInternal: the game's LoadModule (hle_ro.c) goes through its dispatch entry,
 * which is native code here (rtld_load_internal) - so a plugin that hooks it (A64HookFunction on the
 * SDK's function) sees every module load, with the nn::ro::Module filled as rtld fills it. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <string.h>

#include "host.h"

u32 rt_module_count(void);                     /* (host.c) */
u64 rt_module_base(u32 i);
u64 rt_module_size(u32 i);

enum {   /* ModuleObject (skyline-rs' rtld crate, repr(C)) */
    MO_NEXT = 0x0, MO_PREV = 0x8, MO_RELA_PLT = 0x10, MO_RELA = 0x18, MO_BASE = 0x20, MO_DYNAMIC = 0x28, MO_IS_RELA = 0x30,
    MO_RELA_PLT_SIZE = 0x38, MO_INIT = 0x40, MO_FINI = 0x48, MO_BUCKET = 0x50, MO_CHAIN = 0x58, MO_DYNSTR = 0x60,
    MO_DYNSYM = 0x68, MO_DYNSTR_SIZE = 0x70, MO_GOT = 0x78, MO_RELA_DYN_SIZE = 0x80, MO_REL_DYN_SIZE = 0x88,
    MO_REL_COUNT = 0x90, MO_RELA_COUNT = 0x98, MO_NCHAIN = 0xA0, MO_NBUCKET = 0xA8, MO_GOT_STUB = 0xB0, MO_SONAME = 0xB8,
    MO_NRO_SIZE = 0xC0, MO_SIZE = 0xD0,
};
#define MO(o, f) (*P(u64, (o) + (f)))

static u64 auto_list, manual_list;      /* the heads: {front, back} */
static struct { u64 base, object; } objects[512];
static u32 object_n;
static int ready;

static void link_back(u64 head, u64 o) {
    u64 back = *P(u64, head + 8);
    MO(o, MO_NEXT) = head;
    MO(o, MO_PREV) = back;
    *P(u64, back == head ? head : back + MO_NEXT) = o;   /* (the head's front when the list was empty) */
    *P(u64, head + 8) = o;
}

static u64 make_object(u64 base, u64 size) {
    static const u8 zero[MO_SIZE];
    u64 o = rt_guest_copy(zero, MO_SIZE);
    u64 mod0 = base + *P(u32, base + 4);
    u64 dyn = mod0 + (u64)(i64)*P(i32, mod0 + 4);
    MO(o, MO_BASE) = base;
    MO(o, MO_DYNAMIC) = dyn;
    MO(o, MO_NRO_SIZE) = size;
    u64 hash = 0;
    for (u64 d = dyn; *P(u64, d); d += 16) {
        u64 tag = *P(u64, d), v = *P(u64, d + 8);
        switch (tag) {
        case 2: MO(o, MO_RELA_PLT_SIZE) = v; break;                       /* DT_PLTRELSZ */
        case 3: MO(o, MO_GOT) = base + v; break;                          /* DT_PLTGOT */
        case 4: hash = base + v; break;                                    /* DT_HASH */
        case 5: MO(o, MO_DYNSTR) = base + v; break;                       /* DT_STRTAB */
        case 6: MO(o, MO_DYNSYM) = base + v; break;                       /* DT_SYMTAB */
        case 7: MO(o, MO_RELA) = base + v; *P(u8, o + MO_IS_RELA) = 1; break;   /* DT_RELA */
        case 8: MO(o, MO_RELA_DYN_SIZE) = v; break;                       /* DT_RELASZ */
        case 10: MO(o, MO_DYNSTR_SIZE) = v; break;                        /* DT_STRSZ */
        case 12: MO(o, MO_INIT) = base + v; break;
        case 13: MO(o, MO_FINI) = base + v; break;
        case 14: MO(o, MO_SONAME) = v; break;
        case 17: MO(o, MO_RELA) = base + v; break;                        /* DT_REL */
        case 18: MO(o, MO_REL_DYN_SIZE) = v; break;
        case 23: MO(o, MO_RELA_PLT) = base + v; break;                    /* DT_JMPREL */
        case 0x6ffffff9: MO(o, MO_RELA_COUNT) = v; break;
        case 0x6ffffffa: MO(o, MO_REL_COUNT) = v; break;
        }
    }
    if (hash) {
        u32 nbucket = *P(u32, hash), nchain = *P(u32, hash + 4);
        MO(o, MO_NBUCKET) = nbucket;
        MO(o, MO_NCHAIN) = nchain;
        MO(o, MO_BUCKET) = hash + 8;
        MO(o, MO_CHAIN) = hash + 8 + 4ull * nbucket;
    }
    if (object_n < 512) { objects[object_n].base = base; objects[object_n++].object = o; }
    return o;
}

/* The object of the module loaded at base (0: none). */
u64 rtld_object(u64 base) {
    for (u32 i = 0; i < object_n; i++)
        if (objects[i].base == base) return objects[i].object;
    return 0;
}

/* A module the game loaded (host.c, after it is linked): on the manual-load list. */
void rtld_module_loaded(u64 base, u64 size) {
    if (!ready || rtld_object(base)) return;
    link_back(manual_list, make_object(base, size));
}

static u64 load_internal_original;
void ro_load_module_native(Cpu *c);   /* (hle_ro.c: what LoadModule did) */
static void load_internal(Cpu *c) { ro_load_module_native(c); }

/* From the plugins' loading on (hle_skyline.c): the lists, with every module loaded so far, and
 * LoadModuleInternal's dispatch entry (the SDK's own would run the console's loader). */
void rtld_init(void) {
    if (ready) return;
    u64 heads[4] = {0, 0, 0, 0};
    auto_list = rt_guest_copy(heads, sizeof heads);
    manual_list = auto_list + 16;
    *P(u64, auto_list) = *P(u64, auto_list + 8) = auto_list;
    *P(u64, manual_list) = *P(u64, manual_list + 8) = manual_list;
    ready = 1;
    u32 n = rt_module_count();
    for (u32 i = 0; i < n; i++)
        link_back(i < 3 ? auto_list : manual_list, make_object(rt_module_base(i), rt_module_size(i)));
    u64 a = rt_symbol("_ZN2nn2ro6detail15g_pAutoLoadListE"), m = rt_symbol("_ZN2nn2ro6detail17g_pManualLoadListE");
    if (a) *P(u64, a) = auto_list;
    if (m) *P(u64, m) = manual_list;
    u64 internal = rt_symbol("_ZN2nn2ro18LoadModuleInternalEPNS0_6ModuleEPKvPvmib");
    if (internal) load_internal_original = rt_hook_function(internal, rt_native(load_internal, "nn::ro::LoadModuleInternal"));
    fprintf(stderr, "[rtld] %u modules on nn::ro's lists; LoadModuleInternal %s\n", n, load_internal_original ? "native" : "not found");
}

/* The game's LoadModule (hle_ro.c): through LoadModuleInternal's dispatch entry once the lists exist. */
int rtld_load_module(Cpu *c) {
    if (!ready || !load_internal_original) return 0;
    u64 internal = rt_symbol("_ZN2nn2ro18LoadModuleInternalEPNS0_6ModuleEPKvPvmib");
    c->x[5] = 1;   /* (bool check: as LoadModule passes it) */
    rt_call(c, internal);
    return 1;
}

/* nn::ro::LookupModuleSymbol(*address, const Module *, name): the name in that module alone. */
u64 rt_module_export(u64 base, const char *symbol);   /* (host.c) */
static void lookup_module_symbol(Cpu *c) {
    u64 object = *P(u64, c->x[1]), base = 0;
    for (u32 i = 0; i < object_n && !base; i++)
        if (objects[i].object == object || objects[i].base == object) base = objects[i].base;
    u64 at = base ? rt_module_export(base, P(char, c->x[2])) : 0;
    if (at) *P(u64, c->x[0]) = at;
    c->x[0] = at ? 0 : RESULT(22, 1100);
}
void rtld_plugin_shims(void) {
    u64 at = rt_symbol("_ZN2nn2ro18LookupModuleSymbolEPmPKNS0_6ModuleEPKc");
    if (at) rt_hook_function(at, rt_native(lookup_module_symbol, "nn::ro::LookupModuleSymbol"));
}
