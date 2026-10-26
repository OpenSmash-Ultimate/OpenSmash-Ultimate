/* nn::pl: the system's shared fonts, read from plain TrueType files on the host.
 *
 * Each file is looked up in %SSBU_FONTS% (else ..\sysdata\fonts): the user's own firmware fonts
 * (extract-tools/fw_fonts.py); then in fonts_open\ beside the exe: the open-source replacements the runtime
 * ships (Open Sans, Source Sans Pro, the yuzu project's extension font; ..\third_party\fonts_open\README.md). They lack
 * many characters (no CJK, few symbols); nnfontll.c draws their .notdef box where the game needs U+25A1.
 * Only a file found in neither place stops the game (code 7), as a console always has its fonts.
 *
 * SharedFontType: 0 Standard, 1 ChineseSimple, 2 ChineseSimpleExtension, 3 ChineseTraditional,
 *                 4 Korean, 5 NintendoExtension. SharedFontLoadState: 0 Loading, 1 Loaded. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "runtime.h"
#include "hostrt.h"

#define A0 (c->x[0])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define FONT_TYPES 7

static const char *const font_files[FONT_TYPES] = {
    "nintendo_udsg-r_std_003.ttf", "nintendo_udsg-r_org_zh-cn_003.ttf", "nintendo_udsg-r_ext_zh-cn_003.ttf",
    "nintendo_udjxh-db_zh-tw_003.ttf", "nintendo_udsg-r_ko_003.ttf", "nintendo_ext_003.ttf", "nintendo_ext2_003.ttf",
};
static struct { void *data; size_t size; int tried; } g_fonts[FONT_TYPES];
static CRITICAL_SECTION g_lock;

static void load_font(unsigned type) {
    if (type >= FONT_TYPES) return;
    EnterCriticalSection(&g_lock);
    if (!g_fonts[type].tried) {
        g_fonts[type].tried = 1;
        char path[MAX_PATH], dirs[2][MAX_PATH], exe[MAX_PATH];
        int ndirs = 0;
        const char *env = getenv("SSBU_FONTS");
        snprintf(dirs[ndirs++], MAX_PATH, "%s", env ? env : "..\\sysdata\\fonts");
        DWORD n = GetModuleFileNameA(NULL, exe, sizeof exe);       /* the open-source fonts shipped with the runtime: fonts_open\ beside the exe */
        char *slash = n && n < sizeof exe ? strrchr(exe, '\\') : NULL;
        if (slash) { *slash = 0; snprintf(dirs[ndirs++], MAX_PATH, "%s\\fonts_open", exe); }
        FILE *f = NULL;
        for (int i = 0; i < ndirs && !f; i++) {
            snprintf(path, sizeof path, "%s\\%s", dirs[i], font_files[type]);
            f = fopen(path, "rb");
        }
        if (!f) snprintf(path, sizeof path, "%s\\%s", dirs[0], font_files[type]);
        if (f) {
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            fseek(f, 0, SEEK_SET);
            void *data = _aligned_malloc((size_t)size + 16, 4096);
            if (data && fread(data, 1, (size_t)size, f) == (size_t)size) { g_fonts[type].data = data; g_fonts[type].size = (size_t)size; }
            fclose(f);
        }
        printf("nn::pl: shared font %u %s (%s, %zu bytes)\n", type, g_fonts[type].data ? "loaded" : "NOT FOUND", path, g_fonts[type].size);
        if (!g_fonts[type].data) {                        /* close4: a console always has its system fonts; "loaded, size 0" was a made-up state */
            printf("nn::pl: %s is neither in %s nor in fonts_open\\ beside the exe. The runtime ships open-source fonts in "
                   "..\\third_party\\fonts_open (copied beside the exe by scripts/setup.py): copy that folder there again, or give your own "
                   "firmware's fonts (tools/extract/fw_fonts.py, SSBU_FONTS=<folder>): stopping\n", font_files[type], dirs[0]);
            fflush(stdout);
            report_and_exit(7);
        }
    }
    LeaveCriticalSection(&g_lock);
}

/* Whether p points into one of the shared font files (nnfontll.c: tells the system's fonts from the game's own). */
int pl_shared_font_contains(const void *p) {
    for (int i = 0; i < FONT_TYPES; i++)
        if (g_fonts[i].data && (const char *)p >= (const char *)g_fonts[i].data && (const char *)p < (const char *)g_fonts[i].data + g_fonts[i].size) return 1;
    return 0;
}

static void pl_RequestSharedFontLoad(Ctx *c) { load_font((unsigned)A0); }
static void pl_GetSharedFontLoadState(Ctx *c) { load_font((unsigned)A0); RET(1); }
static void pl_GetSharedFontAddress(Ctx *c) { unsigned t = (unsigned)A0; load_font(t); RET(t < FONT_TYPES ? (uint64_t)(uintptr_t)g_fonts[t].data : 0); }
static void pl_GetSharedFontSize(Ctx *c) { unsigned t = (unsigned)A0; load_font(t); RET(t < FONT_TYPES ? g_fonts[t].size : 0); }
static void pl_IsVrMode(Ctx *c) { RET(0); }
static void nothing(Ctx *c) { (void)c; }

static const ImportDef pl_imports[] = {
    {"_ZN2nn2pl21RequestSharedFontLoadENS0_14SharedFontTypeE", pl_RequestSharedFontLoad},
    {"_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE", pl_GetSharedFontLoadState},
    {"_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE", pl_GetSharedFontAddress},
    {"_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE", pl_GetSharedFontSize},
    {"_ZN2nn2pl8IsVrModeEv", pl_IsVrMode}, {"_ZN2nn2pl11BeginVrModeEv", nothing}, {"_ZN2nn2pl9EndVrModeEv", nothing},
};

void register_pl_imports(void) {
    InitializeCriticalSection(&g_lock);
    register_imports(pl_imports, (int)(sizeof pl_imports / sizeof pl_imports[0]));
}
