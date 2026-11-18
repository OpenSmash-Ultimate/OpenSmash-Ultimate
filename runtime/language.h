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

/* The text language the game keeps in its system save (system_data.bin, the byte at
 * LANGUAGE_SAVE_OFFSET): written once, when the save is made, from the language the game was
 * given then; from then on the game shows its text in that language, whatever it is given. It
 * is the language without the region: the game's table at main+0x4742e80 (13.0.5) gives it
 * for each of its 14 languages, and new saves made with each code here agreed. -1: none. */
enum { LANGUAGE_SAVE_OFFSET = 0x3C6098, LANGUAGE_SAVE_FILE_SIZE = 5982968 };
static inline int language_save_value(const char *code) {
    static const struct { const char *code; int value; } SAVE[] = {
        {"ja", 0}, {"en-US", 1}, {"en-GB", 1}, {"fr", 2}, {"fr-CA", 2}, {"es", 3}, {"es-419", 3},
        {"de", 4}, {"it", 5}, {"nl", 6}, {"ru", 7}, {"zh-Hans", 8}, {"zh-Hant", 9}, {"ko", 10}};
    for (size_t i = 0; code && i < sizeof SAVE / sizeof *SAVE; i++)
        if (!strcmp(code, SAVE[i].code)) return SAVE[i].value;
    return -1;
}

/* Whether a name is a language the game has (language_pick's en-US is then not its fallback). */
static inline int language_known(const char *name) {
    static const char *const HAS[] = {"ja", "en", "fr", "de", "it", "es", "zh", "ko", "nl", "ru"};
    if (!name || !name[0] || !name[1] || (name[2] && name[2] != '-' && name[2] != '_')) return 0;
    for (size_t i = 0; i < sizeof HAS / sizeof *HAS; i++)
        if (language_eq(name, HAS[i], 2)) return 1;
    return 0;
}