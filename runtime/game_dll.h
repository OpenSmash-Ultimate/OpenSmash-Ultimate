/* The runtime/game split: a build given --game-dll (tools/game_dll.py) links the game's modules
 * (main, subsdk0, sdk and the NROs, as tools/recomp.py writes their C) into a library of their
 * own, which the runtime program loads at start (runtime/host.c), instead of linking both into
 * one program. So the runtime can be built once, and a player's PC compiles and links only the
 * game.
 *
 * The library exports one name, RT_GAME_ENTRY: a function that returns its RtGameTables. They
 * hold the module tables, the originals of the functions the runtime wraps (HLE_WRAP), the
 * library's hook slots (hookable.h), and the runtime functions the generated C calls (rt_lookup,
 * rt_svc, hle_<name>, hle_wrap_<...>, ...). Each of those is a small function in the library
 * that calls through a slot, which host.c fills by the function's name. So the library links
 * with these headers alone: no runtime source or object, and no import library. Calls between
 * the game's own functions stay inside the library and stay direct.
 *
 * RT_GAME_ABI is the version of this interface. host.c refuses a library built for another one
 * and says what to do, rather than run it. Change it with anything both sides must agree on:
 * these structures, RtModule, Cpu, RtHook, RtCache, a runtime function's parameters, what a name
 * in the imports means. The sizes are checked too, in case a change goes in without a new number,
 * and so is the instruction set (tools/isa.py), which both must be compiled for. The set of
 * HLE(...) and HLE_WRAP(...) functions is checked by name: the game's C is written for one set
 * (tools/recomp.py forwards each replaced function to its HLE), so the library must be built from
 * the same runtime sources as the program. */
#pragma once
#include "module.h"
#include "hookable.h"

#define RT_GAME_ABI 1
#define RT_GAME_ENTRY "rt_game_tables"
/* the library's file: beside the program (on macOS in the app's Contents/Frameworks) */
#ifdef _WIN32
#define RT_GAME_LIBRARY "ssbu_game.dll"
#define RT_GAME_EXPORT __declspec(dllexport)
#elif defined(__APPLE__)
#define RT_GAME_LIBRARY "ssbu_game.dylib"
#define RT_GAME_EXPORT __attribute__((visibility("default")))
#else
#define RT_GAME_LIBRARY "ssbu_game.so"
#define RT_GAME_EXPORT __attribute__((visibility("default")))
#endif
/* what this file is compiled for, as tools/isa.py names it (its flags define these macros) */
#if defined(__aarch64__) || defined(_M_ARM64)
#define RT_GAME_ISA "arm64"
#elif defined(__AVX2__)
#define RT_GAME_ISA "avx2"
#else
#define RT_GAME_ISA "x86-64-v2"
#endif

/* A runtime function, whatever its parameters: each is called through its own type. */
typedef void RtGameFn(void);

typedef struct RtGameImport { const char *name; RtGameFn **slot; } RtGameImport;   /* a runtime function the game calls */
typedef struct RtGameOrig { u64 addr; GuestFn *fn; } RtGameOrig;                    /* f_<address>_orig, of an HLE_WRAP */

typedef struct RtGameTables {
    u32 abi;                        /* RT_GAME_ABI: first, so that it can be read whatever else changed */
    u32 sizes[4];                   /* RT_GAME_SIZES, as the library was compiled */
    const char *isa;                /* RT_GAME_ISA, as the library was compiled */
    const RtModule *const *boot;    /* main, subsdk0 and sdk, in that order */
    const RtModule *const *nros;    /* the NROs compiled in, for nn::ro (module.h's rt_nro_modules) */
    u32 nro_count;
    u32 import_count;
    const RtGameImport *imports;    /* the runtime functions the game's C calls, by name */
    const RtGameOrig *origs;        /* the original of each function the runtime wraps by address */
    u32 orig_count;
    RtHook *const *hooks;           /* the library's hook slots, as its constructors listed them (newest first) */
} RtGameTables;
#define RT_GAME_SIZES {(u32)sizeof(RtGameTables), (u32)sizeof(RtModule), (u32)sizeof(Cpu), (u32)sizeof(RtHook)}
typedef const RtGameTables *RtGameEntry(void);

/* The program's side (tools/game_dll.py writes it into the runtime): each runtime function the
 * library may import, by name (the HLE(...) ones are hle_table's, host.h), and for an HLE_WRAP
 * the slot of its original, f_<address>_orig, which the runtime calls and the library has. */
typedef struct RtGameFunc { const char *name; RtGameFn *fn; u64 addr; GuestFn **orig; } RtGameFunc;
extern const RtGameFunc rt_game_funcs[];
extern const u32 rt_game_func_count;
