/* UTF-8 layout shared by menu, Direct3D/Vulkan and Metal. No runtime/OS dependencies. */
#pragma once
#include <stddef.h>
#include <string.h>

static unsigned ui_utf8_next(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    unsigned c = *s;
    if (!c) return 0;
    (*p)++;
    if (c < 128) return c;
    int n = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
    if (!n) return 0xfffd;
    unsigned value = c & ((1u << (6 - n)) - 1);
    for (int i = 1; i <= n; i++) {
        if (!s[i] || (s[i] & 0xc0) != 0x80) return 0xfffd;
        value = (value << 6) | (s[i] & 63);
    }
    if ((n == 1 && value < 128) || (n == 2 && value < 0x800) || (n == 3 && value < 0x10000) ||
        value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return 0xfffd;
    *p += n;
    return value;
}

static int ui_columns(unsigned c) {
    if (c == '\n' || c == '\r' || (c >= 0x300 && c <= 0x36f)) return 0;
    return c >= 0x1100 && (c <= 0x115f || c == 0x2329 || c == 0x232a ||
        (c >= 0x2e80 && c <= 0xa4cf) || (c >= 0xac00 && c <= 0xd7a3) ||
        (c >= 0xf900 && c <= 0xfaff) || (c >= 0xfe10 && c <= 0xfe6f) ||
        (c >= 0xff01 && c <= 0xff60) || (c >= 0xffe0 && c <= 0xffe6) || c >= 0x1f000) ? 2 : 1;
}

static int ui_text_columns(const char *s) {
    int width = 0, line = 0;
    while (*s) {
        unsigned c = ui_utf8_next(&s);
        if (c == '\n') { if (line > width) width = line; line = 0; }
        else line += ui_columns(c);
    }
    return line > width ? line : width;
}

static size_t ui_prefix_bytes(const char *s, int cols) {
    const char *p = s;
    int width = 0;
    while (*p) {
        const char *before = p;
        unsigned c = ui_utf8_next(&p);
        width += ui_columns(c);
        if (width > cols || c == '\n') return (size_t)(before - s);
    }
    return (size_t)(p - s);
}

/* Whole codepoints only, even when a small byte buffer clips a translated paragraph. */
static void ui_wrap(const char *s, int cols, char *out, size_t size) {
    size_t o = 0;
    if (!size) return;
    if (cols < 2) cols = 2;
    while (*s && o + 1 < size) {
        size_t cut = ui_prefix_bytes(s, cols);
        if (!cut && *s != '\n') { const char *p = s; ui_utf8_next(&p); cut = (size_t)(p - s); }
        if (s[cut] && s[cut] != '\n') {
            size_t space = 0;
            for (size_t i = 0; i < cut; i++) if (s[i] == ' ') space = i;
            if (space) cut = space;
        }
        const char *p = s, *end = s + cut;
        while (p < end) {
            const char *start = p; ui_utf8_next(&p);
            size_t n = (size_t)(p - start);
            if (o + n >= size) { out[o] = 0; return; }
            memcpy(out + o, start, n); o += n;
        }
        s += cut;
        if (*s == '\n') s++;
        while (*s == ' ') s++;
        if (*s && o + 1 < size) out[o++] = '\n';
    }
    out[o] = 0;
}
