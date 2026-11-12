/* Source-only UTF-8 layout checks: no renderer, fonts, or game files. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../runtime/ui_utf8.h"

int main(void) {
    const char *han = "\347\225\214\351\235\242\350\257\255\350\250\200";
    assert(ui_text_columns("Settings") == 8);
    assert(ui_text_columns(han) == 8);
    assert(ui_prefix_bytes(han, 3) == 3);
    assert(ui_prefix_bytes(han, 4) == 6);
    assert(ui_text_columns("a\n1234") == 4);
    const char *invalid = "\360\200\200\200";
    const char *p = invalid;
    assert(ui_utf8_next(&p) == 0xfffd && p == invalid + 1);
    p = "\342";
    assert(ui_utf8_next(&p) == 0xfffd && !*p);
    char out[128];
    ui_wrap(han, 4, out, sizeof out);
    assert(!strcmp(out, "\347\225\214\351\235\242\n\350\257\255\350\250\200"));
    ui_wrap("hello world", 7, out, sizeof out);
    assert(!strcmp(out, "hello\nworld"));
    char small[5];
    ui_wrap(han, 20, small, sizeof small);
    assert(strlen(small) == 3 && !memcmp(small, han, 3));
    puts("UTF-8 layout passed");
    return 0;
}
