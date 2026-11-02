/* Offline checks of runtime/language.h: the game's language code for system language names and
 * codes typed by hand. Built and run by tests/test_language.py (any C compiler: no runtime).
 * Prints "<n> checks, 0 failed" and exits 0 when every check passes. */
#include <stdio.h>
#include <string.h>
#include "../../runtime/language.h"

int main(void) {
    static const char *const cases[][2] = {
        {"en-US", "en-US"}, {"en-GB", "en-GB"}, {"en-AU", "en-GB"}, {"en-CA", "en-US"}, {"en", "en-US"},
        {"it-IT", "it"}, {"it", "it"}, {"de-AT", "de"}, {"nl-BE", "nl"}, {"ru-RU", "ru"}, {"ja-JP", "ja"}, {"ko-KR", "ko"},
        {"fr-FR", "fr"}, {"fr-CA", "fr-CA"}, {"fr-BE", "fr"}, {"es-ES", "es"}, {"es", "es"}, {"es-MX", "es-419"}, {"es-AR", "es-419"},
        {"zh-CN", "zh-Hans"}, {"zh-Hans-CN", "zh-Hans"}, {"zh-TW", "zh-Hant"}, {"zh-HK", "zh-Hant"}, {"zh-Hant", "zh-Hant"},
        {"pt-BR", "en-US"}, {"pl-PL", "en-US"}, {"", "en-US"}, {"auto", "en-US"}, {"xyz", "en-US"},
        {"it_IT.UTF-8", "it"}, {"de_DE@euro", "de"}, {"C", "en-US"}, {"EN-gb", "en-GB"}, {"ES-419", "es-419"},
    };
    int checks = 0, failed = 0;
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        const char *got = language_pick(cases[i][0]);
        checks++;
        if (strcmp(got, cases[i][1])) { failed++; printf("FAIL \"%s\": %s (wanted %s)\n", cases[i][0], got, cases[i][1]); }
    }
    checks++;
    if (strcmp(language_pick(NULL), "en-US")) { failed++; printf("FAIL NULL\n"); }
    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
