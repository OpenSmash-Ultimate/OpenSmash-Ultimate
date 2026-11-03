/* A native mod for testing runtime/hle_native_mods.c in the game (Windows):
 *   clang-cl /LD /O1 tools/dev/native_mod_test.c /Fe<folder>\native_mod_test.dll
 *   SSBU_MODS=<folder>, a match started: the log gets "[mod] native_mod_test: ..." lines every 5 s with how often
 *   the fighters' frame line ran through the hook (installed by name before lua2cpp_common loads, so it waits for it).
 * Pass-through: the game plays as without it (the fight hash is the same). */
#include <stdint.h>
#include <stdio.h>
#include <windows.h>

typedef struct Ctx Ctx;
typedef void (*HookFn)(Ctx *c, void *user);
typedef void (*HookOrig)(Ctx *c);
typedef struct HookApi {
    uint32_t size, version;
    int (*install)(uint64_t address, HookFn replacement, void *user, HookOrig *orig);
    int (*install_name)(const char *name, HookFn replacement, void *user, HookOrig *orig);
    int (*remove)(int id);
    int (*enable)(int id);
    long long (*calls)(int id);
    uint64_t (*address)(const char *name);
    void (*call)(Ctx *c, uint64_t address);
    void (*log)(const char *text);
    const char *(*error)(void);
} HookApi;

static const HookApi *api;
static HookOrig line_orig;
static int line_id;
static volatile LONG mine;

static void line(Ctx *c, void *user) {
    (void)user;
    InterlockedIncrement(&mine);
    line_orig(c);
}

static DWORD WINAPI report(void *unused) {
    (void)unused;
    for (;;) {
        Sleep(5000);
        char text[160];
        snprintf(text, sizeof text, "native_mod_test: frame line %lld calls through the hook (%ld seen by the replacement)",
                 api->calls(line_id), (long)mine);
        api->log(text);
    }
}

__declspec(dllexport) void mod_init(const HookApi *a) {
    api = a;
    line_id = api->install_name("_ZN7lua2cpp16L2CFighterCommon31sys_line_system_control_fighterEv", line, NULL, &line_orig);
    char text[200];
    snprintf(text, sizeof text, "native_mod_test: hook %d (%s)", line_id, line_id ? "installed or waiting for its module" : api->error());
    api->log(text);
    CloseHandle(CreateThread(NULL, 0, report, NULL, 0, NULL));
}
