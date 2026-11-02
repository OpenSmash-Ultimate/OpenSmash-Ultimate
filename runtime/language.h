/* The game's language (nn::oe::GetDesiredLanguage, hle_env.c): one of the codes the game knows,
 * from a name such as Windows' display language ("it-IT"), a POSIX locale ("pt_BR.UTF-8") or a
 * code typed in display.ini. Kept apart, without the runtime, so tools/dev/langtest.c can test it.
 *
 * An exact code wins; otherwise the language part decides, with the region where the game has
 * two kinds of it (English, French, Spanish, Chinese). A language the game lacks is en-US. */
#pragma once
#include <string.h>

static const char *const LANGUAGE_CODES[] = {"ja", "en-US", "en-GB", "fr", "fr-CA", "de", "it", "es", "es-419",
                                             "zh-Hans", "zh-Hant", "ko", "nl", "ru"};
enum { LANGUAGE_COUNT = sizeof LANGUAGE_CODES / sizeof *LANGUAGE_CODES };

static int language_eq(const char *a, const char *b, size_t n) {   /* (ASCII, case aside; n 0: whole strings) */
    for (size_t i = 0; !n || i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
    return 1;
}

/* The code for a name (NULL or empty: en-US). '_' counts as '-', and a ".UTF-8" or "@..." tail
 * is left out. */
static const char *language_pick(const char *name) {
    char n[32];
    size_t k = 0;
    for (; name && name[k] && name[k] != '.' && name[k] != '@' && k < sizeof n - 1; k++) n[k] = name[k] == '_' ? '-' : name[k];
    n[k] = 0;
    if (!n[0]) return "en-US";
    for (int i = 0; i < LANGUAGE_COUNT; i++)
        if (language_eq(n, LANGUAGE_CODES[i], 0)) return LANGUAGE_CODES[i];
    const char *region = strchr(n, '-') ? strchr(n, '-') + 1 : "";
    if (language_eq(n, "zh", 2))
        return strstr(n, "TW") || strstr(n, "HK") || strstr(n, "MO") || strstr(n, "Hant") || strstr(n, "hant") ? "zh-Hant" : "zh-Hans";
    if (language_eq(n, "en", 2)) return !region[0] || language_eq(region, "US", 0) || language_eq(region, "CA", 0) ? "en-US" : "en-GB";
    if (language_eq(n, "fr", 2)) return language_eq(region, "CA", 0) ? "fr-CA" : "fr";
    if (language_eq(n, "es", 2)) return !region[0] || language_eq(region, "ES", 0) ? "es" : "es-419";
    if (!n[1] || (n[2] && n[2] != '-')) return "en-US";   /* (not a two-letter language) */
    for (int i = 0; i < LANGUAGE_COUNT; i++)
        if (language_eq(n, LANGUAGE_CODES[i], 2) && !LANGUAGE_CODES[i][2]) return LANGUAGE_CODES[i];
    return "en-US";
}
