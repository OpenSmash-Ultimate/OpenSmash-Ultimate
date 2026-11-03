/* Native lib_paramconfig path for config_param.toml (tools/paramconfig.py).
 *
 * The standalone libparam_config.nro is not loaded (its main hangs here). Movesets that link
 * paramconfig into their own plugin.nro still install their own hooks on top of these; a miss
 * in the plugin's table falls through to this table (toml-only mods like Vegito), then the game.
 *
 * Hooks: main+0x4e53a0 get_param_int, +0x4e53b0 get_param_int64, +0x4e53e0 get_param_float
 * (lib_paramconfig 6.5 / game 13.0.3; same low-main region as 13.0.5). */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fight_test.h"
#include "hle_paramconfig.h"
#include "host.h"

#define MAIN_BASE 0x7100000000ull
#define GET_PARAM_INT   0x4e53a0ull
#define GET_PARAM_INT64 0x4e53b0ull
#define GET_PARAM_FLOAT 0x4e53e0ull

enum { T_INT = 0, T_FLOAT = 1, T_ATTR_MUL = 2, T_INT_MUL = 3 };
enum { CAT_WEAPON = 1 };
enum { INVALID_COLOR = -2 };

typedef struct {
    i32 kind, slot, type;
    u64 param, sub;
    union { i32 i; float f; } v;
} Entry;

static Entry *entries;
static u32 entry_n;
static u64 orig_int, orig_int64, orig_float;
static int color_work = 0x1000000b;   /* FIGHTER_INSTANCE_WORK_ID_INT_COLOR fallback */
static int color_ready;

static void resolve_color_work(Cpu *c) {
    if (color_ready) return;
    color_ready = 1;
    static const char NAME[] = "FIGHTER_INSTANCE_WORK_ID_INT_COLOR";
    u64 fn = rt_symbol("_ZN3lib18lua_bind_get_valueIiEEbmRT_");
    if (!fn) return;
    Cpu saved = *c;
    u64 sp = (c->sp - 0x40) & ~15ull, out = sp + 0x20;
    st32(out, 0);
    c->sp = sp;
    c->x[0] = fight_xxh64(NAME, sizeof NAME - 1, ~0ull);
    c->x[1] = out;
    rt_call(c, fn);
    if (c->x[0] & 1) color_work = (int)(i32)ld32(out);
    *c = saved;
}

static u64 call_i(Cpu *c, const char *name, u64 boma, u64 a1) {
    u64 fn = rt_symbol(name);
    if (!fn) return 0;
    Cpu saved = *c;
    c->x[0] = boma;
    c->x[1] = a1;
    rt_call(c, fn);
    u64 r = c->x[0];
    *c = saved;
    return r;
}

static int get_kind(Cpu *c, u64 boma) {
    /* Moveset plugins import the const-pointer form (EPKNS), not EPNS. */
    static const char *const names[] = {
        "_ZN3app7utility8get_kindEPKNS_26BattleObjectModuleAccessorE",
        "_ZN3app8lua_bind28utility__get_kind_implEPKNS_26BattleObjectModuleAccessorE",
        "_ZN3app8lua_bind28utility__get_kind_implEPNS_26BattleObjectModuleAccessorE",
        0};
    for (int i = 0; names[i]; i++) {
        u64 fn = rt_symbol(names[i]);
        if (!fn) continue;
        Cpu saved = *c;
        c->x[0] = boma;
        rt_call(c, fn);
        int r = (int)(i32)c->x[0];
        *c = saved;
        return r;
    }
    return 0;
}

static int get_category(Cpu *c, u64 boma) {
    static const char *const names[] = {
        "_ZN3app7utility12get_categoryEPKNS_26BattleObjectModuleAccessorE",
        "_ZN3app8lua_bind32utility__get_category_implEPKNS_26BattleObjectModuleAccessorE",
        "_ZN3app8lua_bind32utility__get_category_implEPNS_26BattleObjectModuleAccessorE",
        0};
    for (int i = 0; names[i]; i++) {
        u64 fn = rt_symbol(names[i]);
        if (!fn) continue;
        Cpu saved = *c;
        c->x[0] = boma;
        rt_call(c, fn);
        int r = (int)(i32)c->x[0];
        *c = saved;
        return r;
    }
    return 0;
}

static int work_int(Cpu *c, u64 boma, int id) {
    return (int)(i32)call_i(c,
        "_ZN3app8lua_bind24WorkModule__get_int_implEPNS_26BattleObjectModuleAccessorEi",
        boma, (u64)(u32)id);
}

/* ParamModule* -> BattleObjectModuleAccessor* at +8 (lib_paramconfig). */
static u64 boma_of(u64 module) {
    return module ? *P(u64, module + 8) : 0;
}

static int costume_slot(Cpu *c, u64 boma) {
    if (!boma) return INVALID_COLOR;
    resolve_color_work(c);
    if (get_category(c, boma) == CAT_WEAPON) {
        /* WEAPON_INSTANCE_WORK_ID_INT_ACTIVATE_FOUNDER_ID: resolve once if exported as a name */
        static int founder = -1;
        if (founder < 0) {
            static const char NAME[] = "WEAPON_INSTANCE_WORK_ID_INT_ACTIVATE_FOUNDER_ID";
            u64 fn = rt_symbol("_ZN3lib18lua_bind_get_valueIiEEbmRT_");
            founder = 0x10000001;   /* common fallback */
            if (fn) {
                Cpu saved = *c;
                u64 sp = (c->sp - 0x40) & ~15ull, out = sp + 0x20;
                st32(out, 0);
                c->sp = sp;
                c->x[0] = fight_xxh64(NAME, sizeof NAME - 1, ~0ull);
                c->x[1] = out;
                rt_call(c, fn);
                if (c->x[0] & 1) founder = (int)(i32)ld32(out);
                *c = saved;
            }
        }
        u32 owner_id = (u32)work_int(c, boma, founder);
        /* Without a full sv_battle_object path, use the weapon's own color if set; else skip. */
        (void)owner_id;
    }
    return work_int(c, boma, color_work);
}

static int lookup_int(i32 kind, i32 slot, u64 param, u64 sub, i32 *out) {
    for (u32 i = 0; i < entry_n; i++) {
        Entry *e = &entries[i];
        if (e->type != T_INT || e->kind != kind || e->param != param || e->sub != sub) continue;
        if (e->slot != -1 && e->slot != slot) continue;
        *out = e->v.i;
        return 1;
    }
    return 0;
}

static int lookup_float(i32 kind, i32 slot, u64 param, u64 sub, int type, float *out) {
    for (u32 i = 0; i < entry_n; i++) {
        Entry *e = &entries[i];
        if (e->type != type || e->kind != kind || e->param != param || e->sub != sub) continue;
        if (e->slot != -1 && e->slot != slot) continue;
        *out = e->v.f;
        return 1;
    }
    return 0;
}

static int lookup_mul(i32 kind, i32 slot, u64 param, u64 sub, int type, float *out) {
    return lookup_float(kind, slot, param, sub, type, out);
}

static int prepare(Cpu *c, u64 module, i32 *kind, i32 *slot) {
    u64 boma = boma_of(module);
    if (!boma) return 0;
    *slot = costume_slot(c, boma);
    if (*slot == INVALID_COLOR) return 0;
    int cat = get_category(c, boma);
    *kind = get_kind(c, boma);
    if (cat == CAT_WEAPON) *kind = -*kind;
    return 1;
}

static void hook_int(Cpu *c) {
    u64 module = c->x[0], param = c->x[1], sub = c->x[2];
    rt_call(c, orig_int);
    i32 original = (i32)c->x[0];
    if (!entry_n) return;
    i32 kind, slot;
    if (!prepare(c, module, &kind, &slot)) { c->x[0] = (u64)(u32)original; return; }
    float mul;
    if (lookup_mul(kind, slot, param, sub, T_INT_MUL, &mul)) {
        c->x[0] = (u64)(u32)(i32)((float)original * mul);
        return;
    }
    i32 neu;
    if (lookup_int(kind, slot, param, sub, &neu)) {
        c->x[0] = (u64)(u32)neu;
        return;
    }
    c->x[0] = (u64)(u32)original;
}

static void hook_int64(Cpu *c) {
    u64 module = c->x[0], param = c->x[1], sub = c->x[2];
    rt_call(c, orig_int64);
    i64 original = (i64)c->x[0];
    if (!entry_n) return;
    i32 kind, slot;
    if (!prepare(c, module, &kind, &slot)) { c->x[0] = (u64)original; return; }
    float mul;
    if (lookup_mul(kind, slot, param, sub, T_INT_MUL, &mul)) {
        c->x[0] = (u64)(i64)((float)original * mul);
        return;
    }
    i32 neu;
    if (lookup_int(kind, slot, param, sub, &neu)) {
        c->x[0] = (u64)(i64)neu;
        return;
    }
    c->x[0] = (u64)original;
}

static void hook_float(Cpu *c) {
    u64 module = c->x[0], param = c->x[1], sub = c->x[2];
    rt_call(c, orig_float);
    float original;
    memcpy(&original, &c->v[0], 4);
    if (!entry_n) return;
    i32 kind, slot;
    if (!prepare(c, module, &kind, &slot)) {
        memcpy(&c->v[0], &original, 4);
        return;
    }
    float mul;
    if (lookup_mul(kind, slot, param, sub, T_ATTR_MUL, &mul)) {
        float r = original * mul;
        memcpy(&c->v[0], &r, 4);
        return;
    }
    float neu;
    if (lookup_float(kind, slot, param, sub, T_FLOAT, &neu)) {
        memcpy(&c->v[0], &neu, 4);
        return;
    }
    memcpy(&c->v[0], &original, 4);
}

/* Parse {"entries":[[kind,slot,type,param,sub,value],...]} written by tools/paramconfig.py. */
static int parse_num(const char **pp, double *out) {
    char *end;
    *out = strtod(*pp, &end);
    if (end == *pp) return 0;
    *pp = end;
    return 1;
}

static void skip_ws(const char **pp) {
    while (**pp == ' ' || **pp == '\t' || **pp == '\n' || **pp == '\r') (*pp)++;
}

static void load_dump(void) {
    char path[1200];
    snprintf(path, sizeof path, "%s/../skyline/paramconfig.json", rt_data_dir);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 32 << 20) { fclose(f); return; }
    char *js = malloc((size_t)sz + 1);
    if (!js) { fclose(f); return; }
    if (fread(js, 1, (size_t)sz, f) != (size_t)sz) { free(js); fclose(f); return; }
    js[sz] = 0;
    fclose(f);
    const char *p = strstr(js, "\"entries\"");
    if (!p || !(p = strchr(p, '['))) { free(js); return; }
    p++;
    Entry *list = NULL;
    u32 n = 0, cap = 0;
    for (;;) {
        skip_ws(&p);
        if (*p == ']' || !*p) break;
        if (*p == ',') { p++; continue; }
        if (*p != '[') break;
        p++;
        double vals[6];
        int ok = 1;
        for (int i = 0; i < 6; i++) {
            skip_ws(&p);
            if (!parse_num(&p, &vals[i])) { ok = 0; break; }
            skip_ws(&p);
            if (i < 5 && *p == ',') p++;
        }
        skip_ws(&p);
        if (*p == ']') p++;
        if (!ok) break;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            list = realloc(list, cap * sizeof *list);
            if (!list) { n = 0; break; }
        }
        Entry *e = &list[n++];
        e->kind = (i32)vals[0];
        e->slot = (i32)vals[1];
        e->type = (i32)vals[2];
        e->param = (u64)vals[3];
        e->sub = (u64)vals[4];
        if (e->type == T_INT) e->v.i = (i32)vals[5];
        else e->v.f = (float)vals[5];
    }
    free(js);
    entries = list;
    entry_n = n;
    if (n) fprintf(stderr, "[paramconfig] %u override(s) from dump/skyline/paramconfig.json\n", n);
}

void paramconfig_install(Cpu *c) {
    (void)c;
    load_dump();
    if (!entry_n) return;   /* (no overrides: every get_param stays the game's own call) */
    orig_int = rt_hook_function(MAIN_BASE + GET_PARAM_INT, rt_native(hook_int, "paramconfig get_param_int"));
    orig_int64 = rt_hook_function(MAIN_BASE + GET_PARAM_INT64, rt_native(hook_int64, "paramconfig get_param_int64"));
    orig_float = rt_hook_function(MAIN_BASE + GET_PARAM_FLOAT, rt_native(hook_float, "paramconfig get_param_float"));
    if (!orig_int || !orig_int64 || !orig_float)
        fprintf(stderr, "[paramconfig] get_param hook missed (int=%s int64=%s float=%s): overrides inactive\n",
                orig_int ? "ok" : "no", orig_int64 ? "ok" : "no", orig_float ? "ok" : "no");
    else if (entry_n)
        fprintf(stderr, "[paramconfig] get_param hooks installed\n");
}
