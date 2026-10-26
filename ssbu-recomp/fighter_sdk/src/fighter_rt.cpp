// Run-time of a fighter DLL: binding with the host, the script adapters, L2CValue, the Lua stack protocol.
// Compiled into every fighters_bin\<name>.dll (.so, .dylib) together with that fighter's sources (build_fighter.py).
#include "fighter_sdk.h"

FsHost fs_host;
uint64_t fs_addr[S_COUNT + 1];

static const FsSym fs_syms[] = {
#include "fs_syms.inc"
};

// the scripts of this fighter: generated/bindings.inc (script -> offset of the original in the module)
// ACMD_BIND: an animation-command script; L2C_BIND: a function of the script classes (status scripts, state machine)
#define ACMD_BIND(fighter, name, off) extern "C" void fs_acmd_##name(Ctx *c);
#define L2C_BIND(fighter, name, off) extern "C" void fs_fn_##name(Ctx *c);
#include "bindings.inc"
#undef ACMD_BIND
#undef L2C_BIND
#define ACMD_BIND(fighter, name, off) { off, #name, fs_acmd_##name },
#define L2C_BIND(fighter, name, off) { off, #name, fs_fn_##name },
static const FsOverride fs_overrides[] = {
#include "bindings.inc"
    { 0xffffffffu, "", nullptr }
};
#undef ACMD_BIND
#undef L2C_BIND

extern "C" FS_EXPORT void fighter_bind(const FsHost *host, FsTable *t) {
    fs_host = *host;
    t->syms = fs_syms;
    t->sym_count = (int)(sizeof fs_syms / sizeof fs_syms[0]);
    t->addr = fs_addr;
    t->ovr = fs_overrides;
    t->ovr_count = (int)(sizeof fs_overrides / sizeof fs_overrides[0]) - 1;
}

// N13 (plan\phases\N13_own_articles.md): the host-side components (the kind component's own articles) find a fighter's
// own functions by name: i-th function's name and offset in the module (-1 past the end). The guest ADDRESS of an
// offset comes from fs_module_address, which a fighter source with status_local.h exports (FS_EXPORT_MODULE_ADDRESS).
extern "C" FS_EXPORT const char *fs_function_at(int i, int64_t *off) {
    int n = (int)(sizeof fs_overrides / sizeof fs_overrides[0]) - 1;
    if (i < 0 || i >= n) return nullptr;
    if (off) *off = fs_overrides[i].off;
    return fs_overrides[i].name;
}

#ifdef FS_SCRIBBLE
// S6: see fighter_sdk.h. 4 KB below the caller's frame: where the frames of the argument helpers were.
__attribute__((noinline)) void fs_scribble() {
    volatile uint8_t pad[4096];
    for (int i = 0; i < 4096; i++) pad[i] = 0xA5;
}
#endif

// ---------------------------------------------------------------- Lua constants
LuaConst::operator int32_t() const { return ((const int32_t *)(uintptr_t)fs_addr[S_const_value_table])[index]; }

// ---------------------------------------------------------------- L2CValue: simple types natively, the rest by the game's own code
int64_t L2CValue::as_integer() const {
    if (type == L2C_INT) return (int64_t)raw;
    return gcall<int64_t>(S_lv_as_integer, this);
}
float L2CValue::as_number() const {
    if (type == L2C_NUM) return num;
    if (type == L2C_INT) return (float)(int64_t)raw;
    return gcall<float>(S_lv_as_number, this);
}
bool L2CValue::as_bool() const {
    if (type == L2C_BOOL) return (raw & 1) != 0;
    return gcall<bool>(S_lv_as_bool, this);
}
Hash40 L2CValue::as_hash() const {
    if (type == L2C_HASH) return Hash40{raw};
    return gcall<Hash40>(S_lv_as_hash, this);
}
L2CValue::operator bool() const {
    if (type == L2C_BOOL) return (raw & 1) != 0;
    return gcall<bool>(S_lv_bool, this);
}
bool L2CValue::operator==(const L2CValue &o) const {
    if (type == L2C_BOOL && o.type == L2C_BOOL) return (raw & 1) == (o.raw & 1);
    return gcall<bool>(S_lv_eq, this, &o);
}

bool L2CValue::operator<(const L2CValue &o) const { return gcall<bool>(S_lv_lt, this, &o); }
bool L2CValue::operator<=(const L2CValue &o) const { return gcall<bool>(S_lv_le, this, &o); }
static L2CValue fs_lv_op(int sym, const L2CValue *a, const L2CValue *b) {
    L2CValue out;
    Ctx *c = fs_ctx();
    c->x[0] = (uint64_t)(uintptr_t)a; c->x[1] = (uint64_t)(uintptr_t)b; c->x[8] = (uint64_t)(uintptr_t)&out;
    fs_host.call(c, sym);
    return out;
}
L2CValue L2CValue::operator+(const L2CValue &o) const { return fs_lv_op(S_lv_add, this, &o); }
L2CValue L2CValue::operator-(const L2CValue &o) const { return fs_lv_op(S_lv_sub, this, &o); }
L2CValue L2CValue::operator*(const L2CValue &o) const { return fs_lv_op(S_lv_mul, this, &o); }
L2CValue L2CValue::operator/(const L2CValue &o) const { return fs_lv_op(S_lv_div, this, &o); }
L2CValue L2CValue::operator%(const L2CValue &o) const { return fs_lv_op(S_lv_mod, this, &o); }
L2CValue L2CValue::operator-() const { return fs_lv_op(S_lv_neg, this, nullptr); }
L2CValue L2CValue::lnot() const { return fs_lv_op(S_lv_not, this, nullptr); }
L2CValue L2CValue::operator&(const L2CValue &o) const { return fs_lv_op(S_lv_and, this, &o); }
L2CValue L2CValue::operator|(const L2CValue &o) const { return fs_lv_op(S_lv_or, this, &o); }
L2CValue L2CValue::operator<<(const L2CValue &o) const { return fs_lv_op(S_lv_shl, this, &o); }
L2CValue L2CValue::operator>>(const L2CValue &o) const { return fs_lv_op(S_lv_shr, this, &o); }
L2CValue &L2CValue::operator[](int32_t index) const { return *(L2CValue *)(uintptr_t)gcall<uint64_t>(S_lv_ix_int, this, index); }
L2CValue &L2CValue::operator[](Hash40 key) const { return *(L2CValue *)(uintptr_t)gcall<uint64_t>(S_lv_ix_hash, this, key); }
L2CValue &L2CValue::operator[](const L2CValue &key) const { return *(L2CValue *)(uintptr_t)gcall<uint64_t>(S_lv_ix_value, this, &key); }
L2CValue &L2CValue::operator=(const L2CValue &o) {
    if (simple() && o.simple()) { type = o.type; pad = o.pad; raw = o.raw; }
    else gcall<uint64_t>(S_lv_assign, this, &o);
    return *this;
}

// ---------------------------------------------------------------- functions of the script classes
void fs_return(Ctx *c, const L2CValue &r) {
    uint64_t *out = (uint64_t *)(uintptr_t)c->x[8];
    const uint64_t *in = (const uint64_t *)&r;
    if (!r.simple()) {
        // S3: a table / string / function value is reference counted by the game, and the caller will release what
        // it gets. `r` may be a plain copy of a table element (nothing was counted): hand it over with the game's own
        // assignment, which counts. (If `r` already owned a count this keeps one too many: a leak, never a
        // premature free. Found when lifted callers started to consume results of source functions.)
        out[0] = 0; out[1] = 0;
        gcall<uint64_t>(S_lv_assign, (L2CValue *)out, &r);
        return;
    }
    out[0] = in[0]; out[1] = in[1];
}
L2CValue fs_call_pointer(L2CAgent *agent, const L2CValue &fn, const L2CValue *args, int count) {
    L2CValue out;
    Ctx *c = fs_ctx();
    c->x[0] = (uint64_t)(uintptr_t)agent;
    for (int i = 0; i < count && i < 7; i++) c->x[1 + i] = (uint64_t)(uintptr_t)&args[i];
    c->x[8] = (uint64_t)(uintptr_t)&out;
    fs_host.call_addr(c, fn.raw);
    return out;
}

// ---------------------------------------------------------------- the Lua stack protocol
void fs_lua_clear(L2CAgent *agent) { gcall<void>(S_clear_lua_stack, agent); }
void fs_lua_push(L2CAgent *agent, const L2CValue &v) { gcall<void>(S_push_lua_stack, agent, &v); }
L2CValue fs_lua_pop(L2CAgent *agent) { return gcall_value(S_pop_lua_stack, agent, (int32_t)1); }

LuaCall fs_lua_call(L2CAgent *agent, int sym, const L2CValue *args, int count) {
    fs_lua_clear(agent);
    for (int i = 0; i < count; i++) fs_lua_push(agent, args[i]);
    gcall<void>(sym, agent->lua_state);
    return LuaCall{agent};
}
L2CValue LuaCall::pop() const { return fs_lua_pop(agent); }

// ---------------------------------------------------------------- adapters: what the game calls
// A registered ACMD function is `void f(L2CAgent *agent, lib::utility::Variadic &result)`; the original is a thunk that
// asks the result format, runs the script, and pushes the script's return value (always 0) when a format was given.
void fs_run_acmd(Ctx *c, AcmdBody body) {
    L2CAgent *agent = (L2CAgent *)(uintptr_t)c->x[0];
    void *variadic = (void *)(uintptr_t)c->x[1];
    uint64_t format = gcall<uint64_t>(S_get_format, variadic);
    body(agent, agent->boma);
    if (format) {
        L2CValue result((int32_t)0);
        gcall<void>(S_push_variadic, &result, (uint64_t)0, format, variadic);
    }
}
