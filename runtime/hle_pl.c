/* nn::pl - the system's shared fonts, natively; and VR mode, which is never on.
 *
 * The firmware stores each system font as a .bfttf: an 8-byte header, then a TrueType font
 * XORed word by word (big-endian) with a key derived from the header's magic. The pl service
 * decodes them into memory shared with every application, back to back, and reports each
 * font as an offset just past its header - so the game sees plain TrueType. Here the same
 * files, taken from the firmware (dump/fonts, see tools/nca.py), are decoded the same way.
 *
 * A font the firmware's files lack (missing, or no font once decoded) comes from
 * third_party/fonts_open instead, each type on its own, so the firmware's always come first:
 * open fonts the yuzu project made for the same purpose (README.md there), plain TrueType put
 * behind the same header. They have Latin, Greek and Cyrillic letters but no Chinese, Japanese
 * or Korean ones. Nor do they have U+25A1, the box the game draws for a character a face's
 * fonts lack, which every firmware font has; the game crashes on a face without it
 * (ssbu-recomp/runtime/nnfontll.c found that). So an open font gets U+25A1 in its character
 * map here, drawn as its U+FFFD, else its U+25CA, else a space. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"

/* By nn::pl::SharedFontType: the firmware's <name>.bfttf, the open <name>.ttf. */
static const char *const names[] = {
    "nintendo_udsg-r_std_003", "nintendo_udsg-r_org_zh-cn_003", "nintendo_udsg-r_ext_zh-cn_003",
    "nintendo_udjxh-db_zh-tw_003", "nintendo_udsg-r_ko_003", "nintendo_ext_003",
};
static const char *const types[] = {
    "Standard", "ChineseSimple", "ChineseSimpleExtension", "ChineseTraditional", "Korean", "NintendoExtension",
};
#define FONTS (sizeof names / sizeof *names)
#define AREA_SIZE 0x1200000   /* 18 MB, as on the console */
#define DECODED_MAGIC 0x7F9A0218u   /* what the first header word decodes to */
#define FILE_KEY 0x49621806u   /* the key of the firmware's files: an open font's header has it too */
#define ALT_CHAR 0x25A1u
#define CHARS 0x110000u

enum { NONE, FIRMWARE, OPEN };
static struct { u64 address, size; int from; } font[FONTS];
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;

static u32 be16(const u8 *p) { return (u32)p[0] << 8 | p[1]; }
static u32 be32(const u8 *p) { return be16(p) << 16 | be16(p + 2); }
static void put16(u8 *p, u32 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void put32(u8 *p, u32 v) { put16(p, v >> 16); put16(p + 2, v); }
static int truetype(const u8 *p) { return be32(p) == 0x00010000u || be32(p) == 0x74727565u; }   /* (or "true") */

/* The file at `path` read to `to`, if it is there and fits in `room` bytes: its size, else -1 and why. */
static long read_file(const char *path, u8 *to, size_t room, const char **why) {
    FILE *f = fopen(path, "rb");
    if (!f) { *why = "is missing"; return -1; }
    long n = fseek(f, 0, SEEK_END) ? -1 : ftell(f);
    *why = NULL;
    if (n < 0 || fseek(f, 0, SEEK_SET)) *why = "cannot be read";
    else if ((size_t)n > room) *why = "does not fit in what is left of the font area";
    else if (fread(to, 1, (size_t)n, f) != (size_t)n) *why = "cannot be read";
    fclose(f);
    return *why ? -1 : n;
}

/* ---- U+25A1 for an open font ---------------------------------------------------------------- */
/* A character map subtable's characters into glyph[] (format 4: the BMP; format 12: all of
 * Unicode), read no further than `end`. */
static void read_format4(const u8 *s, const u8 *end, u16 *glyph) {
    if (end - s < 16) return;
    u32 segs = be16(s + 6) / 2;
    if ((size_t)(end - s) < 16 + 8 * (size_t)segs) return;
    const u8 *last = s + 14, *first = last + 2 * segs + 2, *delta = first + 2 * segs, *range = delta + 2 * segs;
    for (u32 i = 0; i < segs; i++) {
        u32 from = be16(first + 2 * i), to = be16(last + 2 * i), d = be16(delta + 2 * i), r = be16(range + 2 * i);
        for (u32 c = from; c <= to && c < 0xFFFF; c++) {
            u32 g = (c + d) & 0xFFFF;
            if (r) {   /* (an offset from this segment's own idRangeOffset into glyphIdArray) */
                size_t at = (size_t)(range + 2 * i - s) + r + 2 * (c - from);
                if (at + 2 > (size_t)(end - s)) break;
                g = be16(s + at);
                g = g ? (g + d) & 0xFFFF : 0;
            }
            if (g) glyph[c] = (u16)g;
        }
    }
}

static void read_format12(const u8 *s, const u8 *end, u16 *glyph) {
    if (end - s < 16) return;
    u32 groups = be32(s + 12);
    for (u32 i = 0; i < groups && 16 + 12 * ((size_t)i + 1) <= (size_t)(end - s); i++) {
        const u8 *g = s + 16 + 12 * (size_t)i;
        u32 from = be32(g), to = be32(g + 4), id = be32(g + 8);
        for (u32 c = from; c <= to && c < CHARS && (u64)id + (c - from) <= 0xFFFF; c++) glyph[c] = (u16)(id + (c - from));
    }
}

/* The characters of glyph[] (none at 0xFFFF) as a cmap table at `t`, or only measured when `t` is
 * NULL: a Windows Unicode BMP subtable (format 4) and a full one (format 12), whichever the font
 * engine asks for. Its size; 0 when format 4 cannot hold them. */
static u32 write_cmap(u8 *t, const u16 *glyph) {
    u32 segs = 1, groups = 0;   /* (format 4 ends with a segment for 0xFFFF) */
    for (u32 c = 0; c < CHARS; c++)
        if (glyph[c] && !(c && glyph[c - 1] && glyph[c] == glyph[c - 1] + 1)) { groups++; segs += c < 0xFFFF; }
    u32 len4 = 16 + 8 * segs, len12 = 16 + 12 * groups;
    if (len4 > 0xFFFF) return 0;
    if (!t) return 20 + len4 + len12;
    u8 *f4 = t + 20, *f12 = f4 + len4;
    put16(t, 0); put16(t + 2, 2);
    put16(t + 4, 3); put16(t + 6, 1); put32(t + 8, 20);
    put16(t + 12, 3); put16(t + 14, 10); put32(t + 16, 20 + len4);
    u32 most = 1, shift = 0;   /* (the largest power of two up to segs, and its log: the binary search's) */
    while (most * 2 <= segs) { most *= 2; shift++; }
    put16(f4, 4); put16(f4 + 2, len4); put16(f4 + 4, 0); put16(f4 + 6, 2 * segs);
    put16(f4 + 8, 2 * most); put16(f4 + 10, shift); put16(f4 + 12, 2 * (segs - most));
    u8 *last = f4 + 14, *first = last + 2 * segs + 2, *delta = first + 2 * segs, *range = delta + 2 * segs;
    put16(last + 2 * segs, 0);
    memset(range, 0, 2 * segs);
    put16(f12, 12); put16(f12 + 2, 0); put32(f12 + 4, len12); put32(f12 + 8, 0); put32(f12 + 12, groups);
    u32 s = 0, g = 0;
    for (u32 c = 0; c < CHARS; c++) {
        if (!glyph[c]) continue;
        u32 from = c;
        while (c + 1 < CHARS && glyph[c + 1] && glyph[c + 1] == glyph[c] + 1) c++;
        if (from < 0xFFFF) {
            put16(first + 2 * s, from); put16(last + 2 * s, c); put16(delta + 2 * s, (glyph[from] - from) & 0xFFFF);
            s++;
        }
        u8 *group = f12 + 16 + 12 * g++;
        put32(group, from); put32(group + 4, c); put32(group + 8, glyph[from]);
    }
    put16(first + 2 * s, 0xFFFF); put16(last + 2 * s, 0xFFFF); put16(delta + 2 * s, 1);
    return 20 + len4 + len12;
}

/* U+25A1 added to the font at `p` (n bytes, room for `room`) when its character map lacks it: a new
 * cmap table after the font, of its characters and U+25A1, which the table directory then names
 * (the old one stays, unused). The font's size now: n when nothing changed. */
static long add_alt_char(u8 *p, long n, size_t room) {
    u8 *entry = NULL;
    for (u32 i = 0; i < be16(p + 4) && 12 + 16 * ((long)i + 1) <= n; i++)
        if (!memcmp(p + 12 + 16 * i, "cmap", 4)) entry = p + 12 + 16 * i;
    if (!entry) return n;
    u32 at = be32(entry + 8), len = be32(entry + 12);
    if (at > (u32)n || len > (u32)n - at || len < 4) return n;
    const u8 *cmap = p + at, *end = cmap + len, *best = NULL;
    u32 best_rank = 0;   /* 2: a full Unicode subtable (format 12), 1: a BMP one (format 4) */
    for (u32 i = 0; i < be16(cmap + 2) && 4 + 8 * (i + 1) <= len; i++) {
        const u8 *rec = cmap + 4 + 8 * i;
        u32 platform = be16(rec), encoding = be16(rec + 2), off = be32(rec + 4);
        if (off > len - 2) continue;
        u32 format = be16(cmap + off), unicode = platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10));
        u32 rank = !unicode ? 0 : format == 12 ? 2 : format == 4 ? 1 : 0;
        if (rank > best_rank) { best = cmap + off; best_rank = rank; }
    }
    if (!best) return n;
    u16 *glyph = calloc(CHARS, sizeof *glyph);   /* by character */
    if (!glyph) return n;
    if (best_rank == 2) read_format12(best, end, glyph);
    else read_format4(best, end, glyph);
    glyph[0xFFFF] = 0;
    u16 stand_in = glyph[0xFFFD] ? glyph[0xFFFD] : glyph[0x25CA] ? glyph[0x25CA] : glyph[0x20];
    size_t start = ((size_t)n + 3) & ~(size_t)3;
    u32 size;
    if (!glyph[ALT_CHAR] && stand_in) {
        glyph[ALT_CHAR] = stand_in;
        if ((size = write_cmap(NULL, glyph)) && start + size <= room) {
            memset(p + n, 0, start - (size_t)n);
            write_cmap(p + start, glyph);
            u32 sum = 0;
            for (u32 k = 0; k < size; k += 4) sum += be32(p + start + k);
            put32(entry + 4, sum); put32(entry + 8, (u32)start); put32(entry + 12, size);
            n = (long)(start + size);
        }
    }
    free(glyph);
    return n;
}

/* ---- the fonts ------------------------------------------------------------------------------- */
/* A .bfttf decoded in place at `at`: its size with the header, else -1 and why. */
static long firmware_font(const char *path, u8 *at, size_t room, const char **why) {
    long n = read_file(path, at, room, why);
    if (n < 0) return -1;
    if (n < 12) { *why = "is too short for a font"; return -1; }
    u32 *w = (u32 *)at, key = _byteswap_ulong(w[0]) ^ DECODED_MAGIC;
    w[0] = _byteswap_ulong(DECODED_MAGIC);
    for (long k = 2; k < n / 4; k++) w[k] ^= _byteswap_ulong(key);
    if (!truetype(at + 8)) { *why = "is no TrueType font once decoded"; return -1; }
    return n;
}

/* An open .ttf at `at` behind the header a decoded .bfttf has, with U+25A1: its size with the
 * header, else -1 and why. */
static long open_font(const char *path, u8 *at, size_t room, const char **why) {
    if (room < 8) { *why = "does not fit in what is left of the font area"; return -1; }
    long n = read_file(path, at + 8, room - 8, why);
    if (n < 0) return -1;
    if (n < 12 || !truetype(at + 8)) { *why = "is no TrueType font"; return -1; }
    n = add_alt_char(at + 8, n, room - 8);
    put32(at, DECODED_MAGIC);
    put32(at + 4, (u32)n ^ FILE_KEY);
    return n + 8;
}

/* The fonts back to back from `area` (guest address `base`, `room` bytes): each type the firmware's
 * from the folder `firmware`, else the open one from `open_dir`, else none; font[] says where. */
static void pack_fonts(u8 *area, u64 base, size_t room, const char *firmware, const char *open_dir) {
    size_t used = 0;
    for (u32 i = 0; i < FONTS; i++) {
        char path[1024], open_path[1024];
        const char *why, *open_why;
        snprintf(path, sizeof path, "%s/%s.bfttf", firmware, names[i]);
        long n = firmware_font(path, area + used, room - used, &why);
        font[i].from = FIRMWARE;
        if (n < 0) {
            snprintf(open_path, sizeof open_path, "%s/%s.ttf", open_dir, names[i]);
            n = open_font(open_path, area + used, room - used, &open_why);
            font[i].from = n < 0 ? NONE : OPEN;
            if (n < 0) fprintf(stderr, "[pl] %s %s, and %s %s: that font is empty\n", path, why, open_path, open_why);
            else fprintf(stderr, "[pl] %s %s: %s instead\n", path, why, open_path);
        }
        if (n < 0) { font[i].address = font[i].size = 0; continue; }
        font[i].address = base + used + 8;
        font[i].size = (u64)n - 8;
        used += ((size_t)n + 3) & ~(size_t)3;
        if (used > room) used = room;
    }
    static const int order[] = {FIRMWARE, OPEN, NONE};   /* which came from where: a line for each */
    for (int k = 0; k < 3; k++) {
        char list[256];
        size_t len = 0;
        for (u32 i = 0; i < FONTS; i++)
            if (font[i].from == order[k]) len += (size_t)snprintf(list + len, sizeof list - len, "%s%s", len ? ", " : "", types[i]);
        if (!len) continue;
        if (order[k] == FIRMWARE) fprintf(stderr, "[pl] shared fonts from the firmware (%s): %s\n", firmware, list);
        else if (order[k] == OPEN) fprintf(stderr, "[pl] open shared fonts (%s; no Chinese, Japanese or Korean characters): %s\n", open_dir, list);
        else fprintf(stderr, "[pl] no shared font for %s\n", list);
    }
}

static BOOL CALLBACK load_fonts(INIT_ONCE *o, void *p, void **ctx) {
    (void)o; (void)p; (void)ctx;
    if (VirtualAlloc(G2H(SHARED_BASE), AREA_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != G2H(SHARED_BASE))
        rt_fatal("cannot map the shared font area");
    kernel_add_region(SHARED_BASE, AREA_SIZE, 6 /* shared */, 1 /* read */);
    char firmware[1024], open_dir[1024];
    snprintf(firmware, sizeof firmware, "%s/../fonts", rt_data_dir);
    snprintf(open_dir, sizeof open_dir, "%s/../../third_party/fonts_open", rt_data_dir);
    pack_fonts(G2H(SHARED_BASE), SHARED_BASE, AREA_SIZE, firmware, open_dir);
    return TRUE;
}

static u32 font_type(Cpu *c) {
    InitOnceExecuteOnce(&once, load_fonts, NULL, NULL);
    return (u32)c->x[0] < FONTS ? (u32)c->x[0] : 0;
}

HLE(_ZN2nn2pl21RequestSharedFontLoadENS0_14SharedFontTypeE) { font_type(c); }
HLE(_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE) { c->x[0] = font[font_type(c)].size ? 1 : 0; }
HLE(_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE) { c->x[0] = font[font_type(c)].address; }
HLE(_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE) { c->x[0] = font[font_type(c)].size; }

HLE(_ZN2nn2pl8IsVrModeEv) { c->x[0] = 0; }
HLE(_ZN2nn2pl11BeginVrModeEv) { (void)c; }
HLE(_ZN2nn2pl9EndVrModeEv) { (void)c; }
