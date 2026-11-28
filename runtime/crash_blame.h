/* (crash reports, runtime/host.c crash_blame: SSBU_CRASH_BLAME, default on, 0 = off as before) Whether a guest module is
 * one the game does not have: a Skyline plugin, a mod's or one of the standard ones (ARCropolis, Smashline...). The
 * game's own modules are main, sdk, subsdk<n> and rtld, and its NROs, which are all lua2cpp_<name> (seen in 13.0.5's
 * romfs, nro/release). `name` is n characters, not ended. A header so that tests/test_crash_blame.py can run it. */
#pragma once
#include <string.h>

static inline int plugin_module(const char *name, size_t n) {
    if (!n || (n >= 2 && name[0] == '0' && name[1] == 'x') || memchr(name, ':', n)) return 0;   /* (an address, "native:...") */
    if ((n == 4 && !memcmp(name, "main", 4)) || (n == 3 && !memcmp(name, "sdk", 3)) || (n == 4 && !memcmp(name, "rtld", 4))) return 0;
    if ((n >= 6 && !memcmp(name, "subsdk", 6)) || (n >= 8 && !memcmp(name, "lua2cpp_", 8))) return 0;
    return 1;
}

/* Known crashes (host.c crash_blame, SSBU_CRASH_BLAME_KNOWN: default on, 0 = off as before): a crash whose faulting frames
 * are all the game's (so no plugin is named above) but that field reports show only with one plugin loaded. The report's
 * first line then ends " - with plugin <module> (a known crash with it loaded: <frame>)", which the launcher reads
 * (OpenSmash.Core CrashBlame.cs) as it reads " - in plugin" and suggests turning off the mod behind it.
 * The CSK Collection: a read of a null pointer (+0x20, or 0) at a match's loading or right after a match, on guest thread
 * 11 or the main thread, with main+0x35c2890 or main+0x35c7aa0 among the innermost guest frames (mostly
 * main+0x35c2890 < main+0x17dde90 < main+0x17eeee0; also main+0x35c7aa0 < main+0x35e0e50 < main+0x35c2890, and
 * main+0x3602380 < main+0x36026c0 < main+0x35c7aa0): about 45% of 0.5.4's crash reports, every one of them with the
 * plugin loaded, which about half of all sessions have. The frames are guest functions' starts, as rt_describe writes
 * them; the plugin's own copies (the_csk_collection_, the name tools/skyline.py gives a second build) count too. */
#define KNOWN_DEPTH 5   /* (innermost guest frames looked at) */
typedef struct { const char *plugin; const char *frames[3]; } KnownCrash;
static const KnownCrash known_crashes[] = {
    {"the_csk_collection", {"main+0x35c2890", "main+0x35c7aa0", NULL}},
};

/* The known crash a report matches, or NULL. described: the innermost guest frames as rt_describe writes them (n of
 * them, only the first KNOWN_DEPTH looked at); near_null: the fault is a read below 0x1000; loaded(plugin): the name of
 * the loaded module that is that plugin or a copy of it, NULL if none. *module and *frame: what matched. */
static inline const KnownCrash *known_crash(const char *const *described, int n, int near_null, const char *(*loaded)(const char *plugin),
                                            const char **module, const char **frame) {
    if (!near_null) return NULL;
    for (size_t k = 0; k < sizeof known_crashes / sizeof *known_crashes; k++) {
        const KnownCrash *c = &known_crashes[k];
        const char *hit = NULL;
        for (int i = 0; i < n && i < KNOWN_DEPTH && !hit; i++)
            for (int j = 0; j < 3 && c->frames[j] && !hit; j++)
                if (described[i] && !strcmp(described[i], c->frames[j])) hit = c->frames[j];
        const char *m = hit ? loaded(c->plugin) : NULL;
        if (!m) continue;
        *module = m;
        *frame = hit;
        return c;
    }
    return NULL;
}
