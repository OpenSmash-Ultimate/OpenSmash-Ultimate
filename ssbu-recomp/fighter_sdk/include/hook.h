// hook.h - the hook table of the native port: replace ANY function of the game at run time.
//
// Every function of the main executable and of every script module (lua2cpp_*) can be replaced, by its address in
// the game (13.0.4: main at 0x7100000000 + offset, modules at their address in nro_registry.c) or by its exported
// name. The replacement is reached however the game calls the function: directly, through a pointer, through an
// import. With no hook installed the game runs exactly the code it ran before (see fighter_sdk\hook_host.c).
//
// A mod is a DLL in the folder named by SSBU_MODS=<folder> that exports
//     extern "C" MOD_EXPORT void mod_init(const HookApi *api);
// The port calls it once, before the game starts. Build:  python fighter_sdk\tools\build_mod.py <mod.cpp>
// (the port loads mods on Windows only: hook_host.c)
//
// A replacement gets the register context of the calling game thread: arguments in c->x[0..7] (integers, pointers;
// a function that returns an L2CValue gets the result buffer in c->x[8]) and c->v[0..7] (floats); results go to
// c->x[0] / c->v[0]. `orig(c)` runs the function as it was before this hook (the original, or an older hook).
#pragma once
#include <stdint.h>

#ifdef _WIN32
#define MOD_EXPORT __declspec(dllexport)
#else
#define MOD_EXPORT __attribute__((visibility("default")))
#endif

#ifndef SSBU_CTX_DEFINED
#define SSBU_CTX_DEFINED
struct Ctx {
    uint64_t x[32];                        // x[31] = sp
    struct { uint64_t d[2]; } v[32];
    // (more fields follow in the host; never allocate a Ctx)
};
#endif

typedef void (*HookFn)(Ctx *c, void *user);        // a replacement; `user` is the pointer given at installation
typedef void (*HookOrig)(Ctx *c);                  // what the function did before the hook

struct HookApi {
    uint32_t size, version;                        // sizeof(HookApi), 1
    // Replace the function at a game address. Returns an id > 0, or 0 (see error()). *orig receives "the function
    // before this hook". The first hook on a function rewrites 5 bytes of its code: install from mod_init, or
    // when no game thread can be entering that function; every later call below is safe at any time.
    int (*install)(uint64_t address, HookFn replacement, void *user, HookOrig *orig);
    // The same by exported name (mangled, e.g. "_ZN7lua2cpp16L2CFighterCommon15status_end_DashEv"). A name of a
    // module that is not loaded yet is kept and installed when the module is loaded: *orig is written at that
    // moment, so it must be a variable that stays (a static).
    int (*install_name)(const char *name, HookFn replacement, void *user, HookOrig *orig);
    int (*remove)(int id);                         // the function runs what it ran before this hook (one atomic word)
    int (*enable)(int id);                         // undo remove()
    long long (*calls)(int id);                    // how often the function was called since the hook was installed
    uint64_t (*address)(const char *name);         // address of an exported name, 0 if no loaded module has it
    void (*call)(Ctx *c, uint64_t address);        // call any game function: arguments and results in c
    void (*log)(const char *text);                 // one line in the port's log
    const char *(*error)(void);                    // why the last install returned 0
};
