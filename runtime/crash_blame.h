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
