// Fighter SDK of the native port: what a fighter's C++ source is written against.
//
//   ACMD(mario, game_attack11) { frame(agent, 2); if (is_excute(agent)) { ATTACK(agent, ...); } }
//
// The game's own types are used in place (guest memory is mapped at its real address, a guest pointer is a host
// pointer). Fixed-width types only: the game is LP64, Windows is LLP64 - never `long`.
// Every game function is called by NAME through gcall(): see lua_bind.h (generated) for the wrappers.
#pragma once
#include <stdint.h>

// what the host looks up in a fighter's library by name (GetProcAddress, dlsym; macOS builds hide everything else)
#ifdef _WIN32
#define FS_EXPORT __declspec(dllexport)
#else
#define FS_EXPORT __attribute__((visibility("default")))
#endif

// ---------------------------------------------------------------- the lifted code's register context (runtime.h)
struct Ctx {
    uint64_t x[32];                        // x[31] = sp
    struct { uint64_t d[2]; } v[32];
    // (more fields follow in the host; never allocated here)
};

// ---------------------------------------------------------------- interface with the host (fighter_host.c)
struct FsHost {
    void (*call)(Ctx *, int sym);          // call the game function behind symbol slot `sym`
    void (*log)(const char *);
    Ctx *(*ctx)();                         // register context of the script running on this fiber
    void (*call_addr)(Ctx *, uint64_t);    // call a script function through a pointer taken from a table
};
struct FsOverride { uint32_t off; const char *name; void (*fn)(Ctx *); };
struct FsSym { const char *name; const char *spec; };
struct FsTable { const FsSym *syms; int sym_count; uint64_t *addr; const FsOverride *ovr; int ovr_count; };

extern FsHost fs_host;
extern uint64_t fs_addr[];                 // address of every symbol slot (data symbols are read through it)
inline Ctx *fs_ctx() { return fs_host.ctx(); }

// ---------------------------------------------------------------- hash40
struct Hash40 { uint64_t v; };

// S10: the game's small vectors. In the script modules a phx::Vector2f / 3f is a 128-bit register stored whole on the
// stack and handed over by address; a function that returns one returns it in the vector register (all four lanes).
// Vector3f{x, y, z} / Vector2f{x, y}: the lanes not named are 0, as in the game's code (movi #0, then the lanes set).
struct alignas(16) Vector2f { float v[4]; };
struct alignas(16) Vector3f { float v[4]; };
struct alignas(16) Vector4f { float v[4]; };

constexpr uint32_t fs_crc32_byte(uint32_t c) {
    for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    return c;
}
constexpr Hash40 hash40(const char *s) {
    uint32_t crc = 0xFFFFFFFFu;
    uint64_t n = 0;
    for (; s[n]; n++) crc = fs_crc32_byte((crc ^ (uint8_t)s[n]) & 0xFF) ^ (crc >> 8);
    return Hash40{(uint64_t)(crc ^ 0xFFFFFFFFu) | (n << 32)};
}

// ---------------------------------------------------------------- Lua constants (lua_const.h: FIGHTER_STATUS_KIND_...)
struct LuaConst {
    int32_t index;                         // index in lua2cpp::L2CAgentGeneratedBase::const_value_table__
    operator int32_t() const;
};
struct NilT {};
static const NilT NIL{};                   // "argument left out" (lib::L2CValue::NIL)

// ---------------------------------------------------------------- game objects used in place
struct lua_State;
struct BattleObject;
struct BattleObjectModuleAccessor;

struct L2CAgent;                           // defined below, after L2CValue
struct LuaCall;

enum : uint32_t { L2C_VOID = 0, L2C_BOOL = 1, L2C_INT = 2, L2C_NUM = 3, L2C_POINTER = 4, L2C_TABLE = 5, L2C_INNER_FUNC = 6, L2C_HASH = 7, L2C_STRING = 8 };

struct L2CValue {                          // lib::L2CValue, 16 bytes
    uint32_t type;
    uint32_t pad;
    union { uint64_t raw; float num; void *ptr; };

    L2CValue() : type(L2C_VOID), pad(0), raw(0) {}
    L2CValue(NilT) : type(L2C_VOID), pad(0), raw(0) {}
    L2CValue(bool b) : type(L2C_BOOL), pad(0), raw(b ? 1 : 0) {}
    L2CValue(int32_t i) : type(L2C_INT), pad(0), raw((uint64_t)(int64_t)i) {}
    L2CValue(uint32_t i) : type(L2C_INT), pad(0), raw(i) {}
    L2CValue(int64_t i) : type(L2C_INT), pad(0), raw((uint64_t)i) {}
    L2CValue(uint64_t i) : type(L2C_INT), pad(0), raw(i) {}
    L2CValue(float f) : type(L2C_NUM), pad(0), raw(0) { num = f; }
    L2CValue(Hash40 h) : type(L2C_HASH), pad(0), raw(h.v & 0xFFFFFFFFFFull) {}
    L2CValue(void *p) : type(L2C_POINTER), pad(0), raw(0) { ptr = p; }
    L2CValue(LuaConst c) : type(L2C_INT), pad(0), raw((uint64_t)(int64_t)(int32_t)c) {}
    L2CValue(const LuaCall &call);         // the value a script call left on the Lua stack

    bool simple() const { return type <= L2C_POINTER || type == L2C_HASH; }
    int64_t as_integer() const;
    float as_number() const;
    bool as_bool() const;
    Hash40 as_hash() const;
    struct AnyPointer { void *p; template <class T> operator T *() const { return (T *)p; } };
    AnyPointer as_pointer() const { return AnyPointer{type == L2C_POINTER ? ptr : nullptr}; }   // S9: the game's returns 0 unless the value is a pointer (14 offline differences, S8)
    explicit operator bool() const;
    bool operator==(const L2CValue &o) const;
    bool operator<(const L2CValue &o) const;
    bool operator<=(const L2CValue &o) const;
    // arithmetic is the game's own (lib::L2CValue::operator+ ...): same int / number rules as the original scripts
    L2CValue operator+(const L2CValue &o) const;
    L2CValue operator-(const L2CValue &o) const;
    L2CValue operator*(const L2CValue &o) const;
    L2CValue operator/(const L2CValue &o) const;
    L2CValue operator%(const L2CValue &o) const;
    L2CValue operator&(const L2CValue &o) const;
    L2CValue operator|(const L2CValue &o) const;
    L2CValue operator<<(const L2CValue &o) const;
    L2CValue operator>>(const L2CValue &o) const;
    L2CValue operator-() const;
    L2CValue lnot() const;                 // the game's L2CValue::operator! (Lua `not`): an L2CValue, not a C++ bool
    // table access: a reference to the element inside the game's table (agent->global_table[...])
    L2CValue &operator[](int32_t index) const;
    L2CValue &operator[](Hash40 key) const;
    L2CValue &operator[](const L2CValue &key) const;
    // assignment into game memory goes through the game's own operator= when a table/string is involved
    L2CValue &operator=(const L2CValue &o);
    L2CValue(const L2CValue &o) = default;
};

// The script object of a fighter, weapon or item (lib::L2CAgent; from +0xc8 on: lua2cpp::L2CFighterBase).
struct L2CAgent {
    void *vtable;
    lua_State *lua_state;                  // +0x08
    uint8_t pad10[0x28];
    BattleObject *battle_object;           // +0x38
    BattleObjectModuleAccessor *boma;      // +0x40
    uint8_t pad48[0x80];
    L2CValue global_table;                 // +0xc8: per-fighter table of status values and "uniq" check functions
};

// a function of the script classes: `L2CValue Class::name(L2CValue...)`, this = the agent
L2CValue fs_call_pointer(struct L2CAgent *agent, const L2CValue &fn, const L2CValue *args, int count);
template <class... A> inline L2CValue call_function(struct L2CAgent *agent, const L2CValue &fn, A... a) {
    L2CValue args[sizeof...(a) + 1] = {L2CValue(a)...};
    return fs_call_pointer(agent, fn, args, (int)sizeof...(a));
}
inline void *function_address(int slot) { return (void *)(uintptr_t)fs_addr[slot]; }

// ---------------------------------------------------------------- native C++ -> game function
struct GCall {
    Ctx *c;
    int xi, fi;
    uint64_t more[4];                      // ninth and later integer arguments: they go on the game's stack
    explicit GCall() : c(fs_ctx()), xi(0), fi(0) {}
    void word(uint64_t v) { if (xi < 8) c->x[xi] = v; else more[(xi - 8) & 3] = v; xi++; }
    void put(uint64_t v) { word(v); }
    void put(int64_t v) { word((uint64_t)v); }
    void put(int32_t v) { word((uint64_t)(int64_t)v); }
    void put(uint32_t v) { word(v); }
    void put(bool v) { word(v ? 1 : 0); }
    void put(Hash40 v) { word(v.v); }
    void put(LuaConst v) { word((uint64_t)(int64_t)(int32_t)v); }
    void put(float v) { c->v[fi].d[0] = 0; c->v[fi].d[1] = 0; *(float *)&c->v[fi] = v; fi++; }
    void put(const L2CValue *v) { word((uint64_t)(uintptr_t)v); }
    void put(const L2CValue &v) { word((uint64_t)(uintptr_t)&v); }
    void put(const Vector2f &v) { word((uint64_t)(uintptr_t)&v); }      // S10: by address (the object lives in the caller's expression)
    void put(const Vector3f &v) { word((uint64_t)(uintptr_t)&v); }
    void put(const Vector4f &v) { word((uint64_t)(uintptr_t)&v); }
    template <class T> void put(T *v) { word((uint64_t)(uintptr_t)v); }
    void all() {}
    // S4: arguments travel BY REFERENCE down to put(). They used to be taken by value here and in gcall(): the
    // pointer put(const L2CValue &) hands to the game then pointed at a by-value copy that was already dead when
    // the game function ran (seen in a match: a landing function passed garbage as a status kind and as a motion).
#ifdef FS_TEST_BYVALUE      // S6, never in a real build: the S4 bug put back on purpose, to prove that the FS_SCRIBBLE test sees it
    template <class A, class... R> void all(A a, R... r) { put(a); all(r...); }
#else
    template <class A, class... R> void all(const A &a, const R &... r) { put(a); all(r...); }
#endif
    void call(int sym) {
        if (xi <= 8) { fs_host.call(c, sym); return; }
        uint64_t sp = c->x[31];
        uint64_t *cell = (uint64_t *)(uintptr_t)(sp - 32);
        for (int i = 0; i < 4; i++) cell[i] = i < xi - 8 ? more[i] : 0;
        c->x[31] = sp - 32;
        fs_host.call(c, sym);
        c->x[31] = sp;
    }
};

template <class R> struct GRet;
template <> struct GRet<void> { static void get(Ctx *) {} };
template <> struct GRet<uint64_t> { static uint64_t get(Ctx *c) { return c->x[0]; } };
template <> struct GRet<int64_t> { static int64_t get(Ctx *c) { return (int64_t)c->x[0]; } };
template <> struct GRet<int32_t> { static int32_t get(Ctx *c) { return (int32_t)c->x[0]; } };
template <> struct GRet<uint32_t> { static uint32_t get(Ctx *c) { return (uint32_t)c->x[0]; } };
template <> struct GRet<bool> { static bool get(Ctx *c) { return (c->x[0] & 1) != 0; } };
template <> struct GRet<float> { static float get(Ctx *c) { return *(float *)&c->v[0]; } };
template <> struct GRet<Hash40> { static Hash40 get(Ctx *c) { return Hash40{c->x[0]}; } };
template <> struct GRet<Vector4f> { static Vector4f get(Ctx *c) { Vector4f r; const float *p = (const float *)&c->v[0]; for (int i = 0; i < 4; i++) r.v[i] = p[i]; return r; } };      // S10: a vector result, all lanes

// S6, test builds only (-DFS_SCRIBBLE, build_fighter.py with SSBU_FIGHTER_SCRIBBLE=1): between handing the arguments
// to GCall and calling the game, the stack below this frame is overwritten, so a pointer to an argument copy that is
// already dead (the S4 bug: arguments taken by value) reaches the game as garbage in the offline test too, instead of
// passing on a quiet stack.
#ifdef FS_SCRIBBLE
void fs_scribble();
#define FS_SCRIBBLE_NOW() fs_scribble()
#else
#define FS_SCRIBBLE_NOW() ((void)0)
#endif
template <class R, class... A> inline R gcall(int sym, const A &... a) {
    GCall g;
    g.all(a...);
    FS_SCRIBBLE_NOW();
    g.call(sym);
    return GRet<R>::get(g.c);
}
// function returning an L2CValue: the caller's buffer goes in x8
template <class... A> inline L2CValue gcall_value(int sym, const A &... a) {
    L2CValue out;
    GCall g;
    g.all(a...);
    FS_SCRIBBLE_NOW();
    g.c->x[8] = (uint64_t)(uintptr_t)&out;
    g.call(sym);
    return out;
}

// ---------------------------------------------------------------- script calls (the Lua stack protocol of lua2cpp)
// MACRO(agent, a, b, c) = clear_lua_stack, push a, b, c, app::sv_animcmd::MACRO(lua_State), [pop the result]
struct LuaCall {
    L2CAgent *agent;
    L2CValue pop() const;                  // take the result the call left
    operator bool() const { return (bool)pop(); }
};
LuaCall fs_lua_call(L2CAgent *agent, int sym, const L2CValue *args, int count);
void fs_lua_clear(L2CAgent *agent);
void fs_lua_push(L2CAgent *agent, const L2CValue &v);
L2CValue fs_lua_pop(L2CAgent *agent);

inline L2CValue::L2CValue(const LuaCall &call) { *this = call.pop(); }

// ---------------------------------------------------------------- defining scripts
typedef void (*AcmdBody)(L2CAgent *agent, BattleObjectModuleAccessor *boma);
void fs_run_acmd(Ctx *c, AcmdBody body);

// An ACMD script ("animation command": what a move does, frame by frame). The game runs the whole function again
// every time the script advances: frame()/wait() say when the following is_excute() block is due.
#define ACMD(fighter, name) \
    static void name##_script(L2CAgent *agent, BattleObjectModuleAccessor *boma); \
    extern "C" void fs_acmd_##name(Ctx *c) { fs_run_acmd(c, name##_script); } \
    static void name##_script(L2CAgent *agent, BattleObjectModuleAccessor *boma)

// A function of the script classes (status scripts, the shared state machine L2CFighterCommon::status_*, sub_*):
//   L2C_FUNCTION(common, status_end_Dash) { ...; return L2CValue(0); }          agent, boma are in scope
//   L2C_FUNCTION1(common, name, a0) / L2C_FUNCTION2(common, name, a0, a1)       with L2CValue arguments
//   L2C_VOID_FUNCTION(common, name) ...                                          functions without a result
void fs_return(Ctx *c, const L2CValue &r);
#define FS_AGENT(c) ((L2CAgent *)(uintptr_t)(c)->x[0])
#define FS_ARG(c, i) (*(const L2CValue *)(uintptr_t)(c)->x[i])
#define L2C_FUNCTION(fighter, name) \
    static L2CValue name##_fn(L2CAgent *agent, BattleObjectModuleAccessor *boma); \
    extern "C" void fs_fn_##name(Ctx *c) { L2CAgent *a = FS_AGENT(c); uint64_t out = c->x[8]; L2CValue r = name##_fn(a, a->boma); c->x[8] = out; fs_return(c, r); } \
    static L2CValue name##_fn(L2CAgent *agent, BattleObjectModuleAccessor *boma)
#define L2C_FUNCTION1(fighter, name, a0) \
    static L2CValue name##_fn(L2CAgent *agent, BattleObjectModuleAccessor *boma, const L2CValue &a0); \
    extern "C" void fs_fn_##name(Ctx *c) { L2CAgent *a = FS_AGENT(c); uint64_t out = c->x[8]; L2CValue r = name##_fn(a, a->boma, FS_ARG(c, 1)); c->x[8] = out; fs_return(c, r); } \
    static L2CValue name##_fn(L2CAgent *agent, BattleObjectModuleAccessor *boma, const L2CValue &a0)
#define L2C_FUNCTION2(fighter, name, a0, a1) \
    static L2CValue name##_fn(L2CAgent *agent, BattleObjectModuleAccessor *boma, const L2CValue &a0, const L2CValue &a1); \
    extern "C" void fs_fn_##name(Ctx *c) { L2CAgent *a = FS_AGENT(c); uint64_t out = c->x[8]; L2CValue r = name##_fn(a, a->boma, FS_ARG(c, 1), FS_ARG(c, 2)); c->x[8] = out; fs_return(c, r); } \
    static L2CValue name##_fn(L2CAgent *agent, BattleObjectModuleAccessor *boma, const L2CValue &a0, const L2CValue &a1)
#define L2C_VOID_FUNCTION(fighter, name) \
    static void name##_fn(L2CAgent *agent, BattleObjectModuleAccessor *boma); \
    extern "C" void fs_fn_##name(Ctx *c) { L2CAgent *a = FS_AGENT(c); name##_fn(a, a->boma); } \
    static void name##_fn(L2CAgent *agent, BattleObjectModuleAccessor *boma)
#define L2C_VOID_FUNCTION1(fighter, name, a0) \
    static void name##_fn(L2CAgent *agent, BattleObjectModuleAccessor *boma, const L2CValue &a0); \
    extern "C" void fs_fn_##name(Ctx *c) { L2CAgent *a = FS_AGENT(c); name##_fn(a, a->boma, FS_ARG(c, 1)); } \
    static void name##_fn(L2CAgent *agent, BattleObjectModuleAccessor *boma, const L2CValue &a0)
#define L2C_VOID_FUNCTION2(fighter, name, a0, a1) \
    static void name##_fn(L2CAgent *agent, BattleObjectModuleAccessor *boma, const L2CValue &a0, const L2CValue &a1); \
    extern "C" void fs_fn_##name(Ctx *c) { L2CAgent *a = FS_AGENT(c); name##_fn(a, a->boma, FS_ARG(c, 1), FS_ARG(c, 2)); } \
    static void name##_fn(L2CAgent *agent, BattleObjectModuleAccessor *boma, const L2CValue &a0, const L2CValue &a1)

// S3: three to seven L2CValue arguments (x1..x7 point to them)
#define FS_L2C_N(name, DECL, PASS) \
    static L2CValue name##_fn(L2CAgent *agent, BattleObjectModuleAccessor *boma DECL); \
    extern "C" void fs_fn_##name(Ctx *c) { L2CAgent *a = FS_AGENT(c); uint64_t out = c->x[8]; L2CValue r = name##_fn(a, a->boma PASS); c->x[8] = out; fs_return(c, r); } \
    static L2CValue name##_fn(L2CAgent *agent, BattleObjectModuleAccessor *boma DECL)
#define FS_L2C_VOID_N(name, DECL, PASS) \
    static void name##_fn(L2CAgent *agent, BattleObjectModuleAccessor *boma DECL); \
    extern "C" void fs_fn_##name(Ctx *c) { L2CAgent *a = FS_AGENT(c); name##_fn(a, a->boma PASS); } \
    static void name##_fn(L2CAgent *agent, BattleObjectModuleAccessor *boma DECL)
#define FS_V const L2CValue &
#define FS_COMMA ,
#define FS_D3(a, b, d) FS_COMMA FS_V a FS_COMMA FS_V b FS_COMMA FS_V d
#define FS_P3 FS_COMMA FS_ARG(c, 1) FS_COMMA FS_ARG(c, 2) FS_COMMA FS_ARG(c, 3)
#define L2C_FUNCTION3(fighter, name, a0, a1, a2) FS_L2C_N(name, FS_D3(a0, a1, a2), FS_P3)
#define L2C_VOID_FUNCTION3(fighter, name, a0, a1, a2) FS_L2C_VOID_N(name, FS_D3(a0, a1, a2), FS_P3)
#define L2C_FUNCTION4(fighter, name, a0, a1, a2, a3) FS_L2C_N(name, FS_D3(a0, a1, a2) FS_COMMA FS_V a3, FS_P3 FS_COMMA FS_ARG(c, 4))
#define L2C_VOID_FUNCTION4(fighter, name, a0, a1, a2, a3) FS_L2C_VOID_N(name, FS_D3(a0, a1, a2) FS_COMMA FS_V a3, FS_P3 FS_COMMA FS_ARG(c, 4))
#define L2C_FUNCTION5(fighter, name, a0, a1, a2, a3, a4) FS_L2C_N(name, FS_D3(a0, a1, a2) FS_COMMA FS_V a3 FS_COMMA FS_V a4, FS_P3 FS_COMMA FS_ARG(c, 4) FS_COMMA FS_ARG(c, 5))
#define L2C_VOID_FUNCTION5(fighter, name, a0, a1, a2, a3, a4) FS_L2C_VOID_N(name, FS_D3(a0, a1, a2) FS_COMMA FS_V a3 FS_COMMA FS_V a4, FS_P3 FS_COMMA FS_ARG(c, 4) FS_COMMA FS_ARG(c, 5))
#define L2C_FUNCTION6(fighter, name, a0, a1, a2, a3, a4, a5) FS_L2C_N(name, FS_D3(a0, a1, a2) FS_D3(a3, a4, a5), FS_P3 FS_COMMA FS_ARG(c, 4) FS_COMMA FS_ARG(c, 5) FS_COMMA FS_ARG(c, 6))
#define L2C_VOID_FUNCTION6(fighter, name, a0, a1, a2, a3, a4, a5) FS_L2C_VOID_N(name, FS_D3(a0, a1, a2) FS_D3(a3, a4, a5), FS_P3 FS_COMMA FS_ARG(c, 4) FS_COMMA FS_ARG(c, 5) FS_COMMA FS_ARG(c, 6))

// S10 (part D): "changed on purpose". FS_MODIFIED(name); at file scope next to an edited function: the offline test
// and the real-call differential do not compare it with the game's original and report it as MODIFIED instead of
// differing; every other function is still compared. (A function that does not exist in the game at all needs no
// mark: build_fighter.py gives it an address of its own and the tests report it as NEW.)
#define FS_MODIFIED(name) extern "C" FS_EXPORT const char fs_modified_##name = 1
// N13: once, in a source that includes status_local.h: the guest address of an offset of this fighter's module, for
// the host components (an own article's agent is made by the kind component from the DLL's functions)
#define FS_EXPORT_MODULE_ADDRESS() extern "C" FS_EXPORT uint64_t fs_module_address(uint32_t off) { return (uint64_t)(uintptr_t)fs_module_function(off); }

// S11 (part 4): NEW WORK VARIABLES by name.
//   FS_NEW_WORK_VAR(STATUS, FLOAT, MECH_CHARGE_FLOAT_CHARGE);       at file scope, in a source or header of the fighter
//   WorkModule::set_float(boma, 0.0f, MECH_CHARGE_FLOAT_CHARGE);    used like any game constant
// build_fighter.py gives every declared name a free id (build product fs_new_work.h, force-included) and refuses the
// build with the exact limit when there is no room. What the game has (main 13.0.4, WorkModule accessors at
// 71004e4420 / 4600 / 4620 / 4900; sizes read from Mario's module in a match, tests\logs\s11_w1.log):
//   work module = accessor + 0x50; + 0x18 = array of AREAS indexed by id bits 24..27: 0 = INSTANCE (lives as long as
//   the fighter), 1 = STATUS (cleared at a status change unless a KEEP flag is given to change_status).
//   area: + 0x08 float count, + 0x10 floats (4 bytes each); + 0x18 int count, + 0x20 ints (8 bytes each);
//         + 0x28 flag WORD count, + 0x30 flag bits (32 per word). index = id & 0xFFFFFF; id bits 28..: 0 float, 1 int, 2 flag.
//   capacity:  instance  120 floats, 270 ints, 288 flags      status  40 floats, 32 ints, 96 flags
//   The accessors do NOT check the index: an id beyond the capacity reads and writes foreign memory.
// The storage is not enlarged by the SDK. fs_new_work_fits(boma) (call it once in the install function) compares the
// highest new index with the counts of THIS agent's areas and returns false if one is out of range.
#define FS_NEW_WORK_VAR(scope, type, name) static const int32_t fs_new_work_decl_##name = name
#ifdef __cplusplus
static inline bool fs_new_work_fits_n(BattleObjectModuleAccessor *boma, const int32_t need[6]) {
    const uint8_t *wm = *(const uint8_t *const *)((const uint8_t *)boma + 0x50);
    const uint8_t *const *areas = *(const uint8_t *const *const *)(wm + 0x18);
    for (int a = 0; a < 2; a++) {
        const uint8_t *ar = areas[a];
        uint64_t nf = *(const uint64_t *)(ar + 0x08), ni = *(const uint64_t *)(ar + 0x18), nw = *(const uint64_t *)(ar + 0x28);
        if ((uint64_t)need[a * 3] > nf || (uint64_t)need[a * 3 + 1] > ni || (uint64_t)need[a * 3 + 2] > nw * 32) return false;
    }
    return true;
}
#define fs_new_work_fits(boma) fs_new_work_fits_n((boma), FS_NEW_WORK_CHECK)
#endif

#include "lua_const.h"
#include "lua_bind.h"

// S21: a STRING value (type 8). The game's lib::L2CValue::L2CValue(char const*) (main 0x71037341c0) allocates a
// std::string the value owns; ~L2CValue (0x7103733f20) frees it; the copy constructor (0x7103733fe0) and operator=
// make a new one. L2CString is such a value for the length of one statement, built and destroyed by those very
// functions: `local::f(agent, L2CString("INITIALIZE"))` = what the original does around its call. It is passed as a
// `const L2CValue &`; a plain L2CValue copy of it (bits only, see L2CValue) must not outlive the statement: the
// generator never writes one into a variable or a returned value. Only where gen_sdk.py made the two slots.
#if defined(FS_HAVE_S_lv_ctor_string) && defined(FS_HAVE_S_lv_dtor)
struct L2CString : L2CValue {
    explicit L2CString(const char *s) : L2CValue() { gcall<void>(S_lv_ctor_string, (const L2CValue *)this, s); }
    ~L2CString() { gcall<void>(S_lv_dtor, (const L2CValue *)this); }
    L2CString(const L2CString &) = delete;
    L2CString &operator=(const L2CString &) = delete;
};
#endif
// S21: lib::L2CValue::length() (main 0x7103735ea0): of a string its size, of a table the length of its array part
#ifdef FS_HAVE_S_lv_length
inline uint64_t fs_lv_length(const L2CValue &v) { return gcall<uint64_t>(S_lv_length, &v); }
#endif
