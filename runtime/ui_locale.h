/* UI localization adapter. The game's own language is deliberately independent.
 * Header-only so menu harnesses and every native renderer use the same implementation. */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "settings.h"
#include "ui_catalog.generated.h"

static int ui_locale_index;
static char ui_locale_preference[32] = "auto";
static int ui_locale_initialized;

static int ui_locale_match(const char *a, const char *b) {
    while (*a && *b) {
        char x = *a++, y = *b++;
        if (x == '_') x = '-';
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return (!*a || *a == '.' || *a == '@') && !*b;
}

static int ui_locale_pick(const char *code) {
    if (!code || !*code) return 0;
    for (unsigned i = 0; i < UI_CATALOG_COUNT; i++) if (ui_locale_match(code, ui_catalogs[i].code)) return (int)i;
    /* Parent language fallback: zh-Hans/zh-SG -> zh-CN, en-GB -> en. */
    for (unsigned i = 0; i < UI_CATALOG_COUNT; i++) {
        size_t n = strcspn(ui_catalogs[i].code, "-");
        if (strlen(code) >= n && (!code[n] || code[n] == '-' || code[n] == '_' || code[n] == '.' || code[n] == '@')) {
            char base[32];
            if (n >= sizeof base) continue;
            memcpy(base, code, n); base[n] = 0;
            char target[32]; memcpy(target, ui_catalogs[i].code, n); target[n] = 0;
            if (ui_locale_match(base, target)) return (int)i;
        }
    }
    return 0;
}

static void ui_locale_select(const char *preference) {
    snprintf(ui_locale_preference, sizeof ui_locale_preference, "%s", preference && *preference ? preference : "auto");
    const char *code = getenv("SSBU_UI_LANGUAGE");
    if (!code || !*code) code = ui_locale_preference;
    if (ui_locale_match(code, "auto")) {
#ifdef _WIN32
        WCHAR locale[LOCALE_NAME_MAX_LENGTH];
        static char system_locale[LOCALE_NAME_MAX_LENGTH];
        if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH)) {
            WideCharToMultiByte(CP_UTF8, 0, locale, -1, system_locale, sizeof system_locale, NULL, NULL);
            code = system_locale;
        } else code = "en";
#else
        code = getenv("LC_ALL");
        if (!code || !*code) code = getenv("LC_MESSAGES");
        if (!code || !*code) code = getenv("LANG");
#endif
    }
    ui_locale_index = ui_locale_pick(code);
}

static void ui_locale_load(const char *path) {
    char code[32] = "auto";
    settings_ini_get(path, NULL, "ui_language", code, sizeof code);
    ui_locale_select(code);
    ui_locale_initialized = 1;
}

static const char *ui_text(const char *source) {
    if (!source) return NULL;
    const UiCatalog *cat = &ui_catalogs[ui_locale_index];
    unsigned low = 0, high = cat->count;
    while (low < high) {
        unsigned mid = low + (high - low) / 2;
        int cmp = strcmp(source, cat->messages[mid].source);
        if (!cmp) return cat->messages[mid].text;
        if (cmp < 0) high = mid; else low = mid + 1;
    }
    return source;
}
