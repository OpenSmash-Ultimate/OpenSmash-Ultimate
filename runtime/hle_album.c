/* nn::album's screenshots: the pictures the game saves (the Vault's snapshots) kept as files, and
 * listed and loaded back for the Vault's album. Ported from ssbu-recomp's runtime/nnalbum.c (its
 * screenshot half; the movie half, the replays' video maker, stays "not available" in
 * hle_offline.c).
 *
 * Where: album/ in the port's folder, beside save/ (under SSBU_DATA_ROOT when that is set, as the saves).
 * Files, as ssbu-recomp names them (an ssbu-recomp album folder copied here reads the same):
 *   <yyyymmdd>_<hhmmss>_<nn>.bmp            the picture, a 32-bit BMP (the console keeps a JPEG; the
 *                                           game only ever sees decoded RGBA8)
 *   <yyyymmdd>_<hhmmss>_<nn>.userdata.bin   the application data that came with it (up to 0x400 bytes)
 * A .bmp of another name, or of another kind, is passed over.
 *
 * What the SDK does (from ssbu-recomp's reading of sdk.elf 13.0.4, the [sdk] addresses there):
 *  - nn::album is a thin layer over nn::capsrv (caps:u). AlbumFileEntry is capsrv's
 *    ApplicationAlbumEntry: +0 u64 size, +8 u64 hash, +0x10 {u16 year, u8 month, day, hour,
 *    minute, second, id}, +0x18 storage, +0x19 content type (0 screenshot, 3 extra movie), +0x1f 1.
 *    GetAlbumFileCreatedTime (the SDK's own code, lifted) reads the date from those bytes.
 *  - GetAlbumScreenshotFileList(int *count, AlbumFileEntry *, int max): the album's screenshots;
 *    a failure is not reported (count 0, success).
 *  - LoadAlbumImage / LoadAlbumThumbnailImage (all their forms): the outputs are cleared first;
 *    width / height, the picture (1280x720 RGBA8, a thumbnail 320x180), the application data
 *    (min(its size, the caller's room) copied, its full size stored), AlbumFileAttribute {0, 0};
 *    the work buffer is always cleared; capsrv's Result goes through ConvertCapsrvResult.
 *  - SaveScreenshot / SaveScreenshotWithUserData / SaveAndShareScreenshot(data, size, ...): the
 *    picture is RGBA8, 1280x720 (or 1920x1080), its size says which.
 * The entry's hash (a keyed hash on the console) is ssbu-recomp's FNV-1a over the date and the
 * content type; an entry whose hash does not match is "file not found".
 *
 * Tests: tools/dev/albumtest.c (tests/test_album.py) calls the storage functions below with host
 * pointers; the HLE() entries only turn guest addresses into them. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>

#include "host.h"

#define CAPS_RESULT(desc) RESULT(206, desc)
#define CONTENT_SCREENSHOT 0
#define CONTENT_EXTRA_MOVIE 3
enum { ALBUM_W = 1280, ALBUM_H = 720, THUMB_W = 320, THUMB_H = 180, USERDATA_MAX = 0x400 };

typedef struct AlbumDate { u16 year; u8 month, day, hour, minute, second, id; } AlbumDate;
typedef struct AlbumEntry { u64 size, hash; AlbumDate date; u8 storage, content, pad[5], one; } AlbumEntry;   /* 0x20 bytes */
_Static_assert(sizeof(AlbumEntry) == 0x20, "AlbumFileEntry");

/* ---- the folder ------------------------------------------------------------------------- */
static const char *album_dir(void) {
    static char dir[512];
    if (!dir[0]) {
        const char *root = getenv("SSBU_DATA_ROOT");   /* (hle_fs.c's data_root: test runs) */
        if (root && *root) snprintf(dir, sizeof dir, "%s/album", root);
        else snprintf(dir, sizeof dir, "%s/../../album", rt_data_dir ? rt_data_dir : ".");
    }
    return dir;
}

static void entry_base(char *out, size_t n, const AlbumDate *d) {
    snprintf(out, n, "%s/%04d%02d%02d_%02d%02d%02d_%02d", album_dir(), d->year, d->month, d->day, d->hour, d->minute, d->second, d->id);
}

static u64 entry_hash(const AlbumDate *d, int content) {
    u64 h = 0xcbf29ce484222325ull;
    const u8 *p = (const u8 *)d;
    for (int i = 0; i < 8; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return (h ^ (u64)content) * 0x100000001b3ull;
}

static void make_entry(AlbumEntry *e, const AlbumDate *d, int content, u64 size) {
    memset(e, 0, sizeof *e);
    e->size = size;
    e->hash = entry_hash(d, content);
    e->date = *d;
    e->content = (u8)content;
    e->one = 1;
}

static u64 file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return ~0ull;
    long long n = fseek(f, 0, SEEK_END) ? -1 : _ftelli64(f);
    fclose(f);
    return n < 0 ? ~0ull : (u64)n;
}

/* "<yyyymmdd>_<hhmmss>_<nn>.bmp" -> its date; 0 when the name is not one of the album's */
static int parse_name(const char *name, AlbumDate *d) {
    int v[7];
    if (strlen(name) != 22 || _stricmp(name + 18, ".bmp")) return 0;
    for (int i = 0; i < 18; i++)
        if (i == 8 || i == 15 ? name[i] != '_' : name[i] < '0' || name[i] > '9') return 0;
    if (sscanf(name, "%4d%2d%2d_%2d%2d%2d_%2d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]) != 7) return 0;
    if (v[0] < 1970 || v[0] >= 3000 || v[1] < 1 || v[1] > 12 || v[2] < 1 || v[2] > 31 || v[3] > 23 || v[4] > 59 || v[5] > 59) return 0;
    d->year = (u16)v[0]; d->month = (u8)v[1]; d->day = (u8)v[2]; d->hour = (u8)v[3]; d->minute = (u8)v[4]; d->second = (u8)v[5];
    d->id = (u8)v[6];
    return 1;
}

static int date_cmp(const AlbumDate *x, const AlbumDate *y) {
    return x->year != y->year ? (int)x->year - (int)y->year : memcmp(&x->month, &y->month, 6);
}
static int entry_cmp(const void *a, const void *b) { return date_cmp(&((const AlbumEntry *)a)->date, &((const AlbumEntry *)b)->date); }

/* ---- pictures --------------------------------------------------------------------------- */
static void put16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v) { put16(p, v); put16(p + 2, v >> 16); }
static u32 get16(const u8 *p) { return p[0] | (u32)p[1] << 8; }
static u32 get32(const u8 *p) { return get16(p) | get16(p + 2) << 16; }

/* A top-down RGBA8 picture as a 32-bit BMP (BGRA, bottom-up), written whole or not at all. 0: written. */
static int write_bmp(const char *path, const u8 *rgba, int w, int h) {
    u8 head[54] = {'B', 'M'};
    u32 bytes = (u32)w * (u32)h * 4;
    put32(head + 2, 54 + bytes);
    put32(head + 10, 54);
    put32(head + 14, 40);
    put32(head + 18, (u32)w);
    put32(head + 22, (u32)h);
    put16(head + 26, 1);
    put16(head + 28, 32);
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return 1;
    int bad = fwrite(head, sizeof head, 1, f) != 1;
    u8 *row = malloc((size_t)w * 4);
    for (int y = h - 1; y >= 0 && row && !bad; y--) {
        const u8 *s = rgba + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            row[x * 4] = s[x * 4 + 2]; row[x * 4 + 1] = s[x * 4 + 1]; row[x * 4 + 2] = s[x * 4]; row[x * 4 + 3] = s[x * 4 + 3];
        }
        bad = fwrite(row, 4, (size_t)w, f) != (size_t)w;
    }
    bad |= !row;
    free(row);
    bad |= fclose(f) != 0;
    if (bad || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) { remove(tmp); return 1; }
    return 0;
}

/* A 32-bit uncompressed BMP (either way up) as top-down RGBA8 (malloc), or NULL. */
static u8 *read_bmp(const char *path, int *w, int *h) {
    FILE *f = fopen(path, "rb");
    u8 head[54], *px = NULL;
    if (!f) return NULL;
    if (fread(head, sizeof head, 1, f) == 1 && head[0] == 'B' && head[1] == 'M' && get32(head + 14) >= 40 && get16(head + 28) == 32 &&
        get32(head + 30) == 0) {
        int ww = (int)get32(head + 18), hh = (int)get32(head + 22), up = hh > 0;
        if (hh < 0) hh = -hh;
        if (ww > 0 && ww <= 4096 && hh > 0 && hh <= 4096 && !fseek(f, (long)get32(head + 10), SEEK_SET)) {
            px = malloc((size_t)ww * hh * 4);
            for (int y = 0; px && y < hh; y++) {
                u8 *row = px + (size_t)(up ? hh - 1 - y : y) * ww * 4;
                if (fread(row, 4, (size_t)ww, f) != (size_t)ww) { free(px); px = NULL; break; }
                for (int x = 0; x < ww; x++) { u8 t = row[x * 4]; row[x * 4] = row[x * 4 + 2]; row[x * 4 + 2] = t; }
            }
            *w = ww;
            *h = hh;
        }
    }
    fclose(f);
    return px;
}

/* Each destination pixel the mean of the source pixels it covers. */
static void scale_rgba(const u8 *src, int sw, int sh, u8 *dst, int dw, int dh) {
    if (sw == dw && sh == dh) { memcpy(dst, src, (size_t)sw * sh * 4); return; }
    for (int y = 0; y < dh; y++) {
        int y0 = (int)((i64)y * sh / dh), y1 = (int)((i64)(y + 1) * sh / dh);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((i64)x * sw / dw), x1 = (int)((i64)(x + 1) * sw / dw);
            if (x1 <= x0) x1 = x0 + 1;
            unsigned acc[4] = {0, 0, 0, 0}, cnt = (unsigned)((x1 - x0) * (y1 - y0));
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    const u8 *p = src + ((size_t)yy * sw + xx) * 4;
                    for (int k = 0; k < 4; k++) acc[k] += p[k];
                }
            u8 *q = dst + ((size_t)y * dw + x) * 4;
            for (int k = 0; k < 4; k++) q[k] = (u8)((acc[k] + cnt / 2) / cnt);
        }
    }
}

/* ---- saving ------------------------------------------------------------------------------ */
/* The picture (and its application data) into the album. 0, or a capsrv-style Result. *out the
 * entry it is listed as (may be NULL). */
static u32 album_save(const u8 *rgba, u64 size, const void *user, u64 user_size, const char *what, AlbumEntry *out) {
    int w, h;
    if (size == (u64)ALBUM_W * ALBUM_H * 4) w = ALBUM_W, h = ALBUM_H;
    else if (size == 1920ull * 1080 * 4) w = 1920, h = 1080;
    else {
        fprintf(stderr, "[album] %s: %llu bytes are neither a 1280x720 nor a 1920x1080 RGBA8 picture: not saved\n", what, (unsigned long long)size);
        return CAPS_RESULT(8);
    }
    if (!rgba) return CAPS_RESULT(8);
    SYSTEMTIME t;
    GetLocalTime(&t);
    CreateDirectoryA(album_dir(), NULL);
    AlbumDate d = {t.wYear, (u8)t.wMonth, (u8)t.wDay, (u8)t.wHour, (u8)t.wMinute, (u8)t.wSecond, 0};
    char base[560], path[600];
    for (;; d.id++) {   /* (the first number that second has free) */
        if (d.id == 100) { fprintf(stderr, "[album] %s: 100 pictures in one second already: not saved\n", what); return CAPS_RESULT(8); }
        entry_base(base, sizeof base, &d);
        snprintf(path, sizeof path, "%s.bmp", base);
        if (file_size(path) == ~0ull) break;
    }
    if (write_bmp(path, rgba, w, h)) { fprintf(stderr, "[album] %s: cannot write %s\n", what, path); return CAPS_RESULT(8); }
    if (user && user_size) {
        char upath[600];
        snprintf(upath, sizeof upath, "%s.userdata.bin", base);
        FILE *f = fopen(upath, "wb");
        if (f) {
            fwrite(user, 1, (size_t)(user_size < USERDATA_MAX ? user_size : USERDATA_MAX), f);
            fclose(f);
        }
    }
    if (out) make_entry(out, &d, CONTENT_SCREENSHOT, file_size(path));
    fprintf(stderr, "[album] %s: %dx%d picture saved to %s\n", what, w, h, path);
    return 0;
}

/* ---- the list ---------------------------------------------------------------------------- */
/* The album's screenshots, oldest first: the newest `max` when there are more. */
static int album_list(AlbumEntry *out, int max) {
    char pattern[600];
    WIN32_FIND_DATAA fd;
    AlbumEntry *all = NULL;
    int n = 0, cap = 0;
    if (!out || max <= 0) return 0;
    snprintf(pattern, sizeof pattern, "%s/*.bmp", album_dir());
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        AlbumDate d;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !parse_name(fd.cFileName, &d)) continue;
        if (n == cap) {
            void *more = realloc(all, (size_t)(cap = cap ? cap * 2 : 64) * sizeof *all);
            if (!more) break;
            all = more;
        }
        make_entry(&all[n++], &d, CONTENT_SCREENSHOT, ((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (all) qsort(all, (size_t)n, sizeof *all, entry_cmp);
    int first = n > max ? n - max : 0;
    if (all) memcpy(out, all + first, (size_t)(n - first) * sizeof *all);
    free(all);
    return n - first;
}

/* ---- loading ----------------------------------------------------------------------------- */
/* [sdk] 0x50aa0 ConvertCapsrvResult */
static u32 convert_capsrv_result(u32 r) {
    static const u32 table[7] = {0xdcd6, 0xccd6, 0xd6d6, 0xdcd6, 0xced6, 0xdcd6, 0xd2d6};
    if (!r) return 0;
    u32 module = r & 0x1ff, desc = (r >> 9) & 0x1fff;
    if (module != 206) return 0xcad6;
    if (desc >= 21 && desc < 28 && ((0x57 >> (desc - 21)) & 1)) return table[desc - 21];
    if (desc >= 10 && desc < 20) return 0xd6d6;
    if (desc == 26 || desc == 30) return 0xded6;
    return 0xcad6;
}

/* The entry's file (without its extension) when it is one of the album's screenshots, else a capsrv Result. */
static u32 entry_file(const AlbumEntry *e, char *base, size_t n) {
    char path[600];
    if (!e) return CAPS_RESULT(23);
    if (e->storage > 1) return CAPS_RESULT(13);
    if (e->date.month < 1 || e->date.month > 12 || e->date.day < 1 || e->date.day > 31 || e->date.hour > 23 || e->date.minute > 59 ||
        e->date.second > 59)
        return CAPS_RESULT(12);
    if (e->content != CONTENT_SCREENSHOT || e->hash != entry_hash(&e->date, e->content)) return CAPS_RESULT(23);
    entry_base(base, n, &e->date);
    snprintf(path, sizeof path, "%s.bmp", base);
    return file_size(path) == ~0ull ? CAPS_RESULT(23) : 0;
}

/* Load...ForApplication and the album layer above it: the picture (thumb: 320x180, else 1280x720)
 * into buf, its size, attribute and application data. Every pointer may be NULL. The Result is
 * the SDK's (converted). */
static u32 album_load(i32 *out_w, i32 *out_h, u64 *out_attr, u64 *out_user_size, void *user, u64 user_cap, void *buf, u64 buf_size, void *work,
                      u64 work_size, const AlbumEntry *entry, int thumb) {
    char base[560], path[600];
    int w = thumb ? THUMB_W : ALBUM_W, h = thumb ? THUMB_H : ALBUM_H, sw = 0, sh = 0;
    u8 app[USERDATA_MAX], *px = NULL;
    u32 app_size = 0;
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    u32 r = entry_file(entry, base, sizeof base);
    if (!r && (!buf || buf_size < (u64)w * h * 4)) r = CAPS_RESULT(30);   /* (the read buffer is too small) */
    if (!r) {
        snprintf(path, sizeof path, "%s.bmp", base);
        if (!(px = read_bmp(path, &sw, &sh))) r = CAPS_RESULT(14);        /* (not a picture this can read) */
    }
    if (!r) {
        scale_rgba(px, sw, sh, (u8 *)buf, w, h);
        snprintf(path, sizeof path, "%s.userdata.bin", base);
        FILE *f = fopen(path, "rb");
        if (f) { app_size = (u32)fread(app, 1, sizeof app, f); fclose(f); }
        if (out_w) *out_w = w;
        if (out_h) *out_h = h;
        if (user && app_size) memcpy(user, app, app_size < user_cap ? app_size : (size_t)user_cap);
        if (out_user_size) *out_user_size = app_size;
        if (out_attr) { out_attr[0] = 0; out_attr[1] = 0; }
    }
    free(px);
    if (work && work_size) memset(work, 0, (size_t)work_size);
    return convert_capsrv_result(r);
}

/* ---- nn::album ----------------------------------------------------------------------------- */
#define G(T, a) ((a) ? P(T, a) : (T *)NULL)   /* a guest pointer, NULL for 0 */
#define STK(i) ld64(c->sp + 8 * (i))           /* the i-th argument passed on the stack */

HLE(_ZN2nn5album10InitializeEv) { c->x[0] = 0; }

/* (const void *data, size_t size, ImageSize, AlbumReportOption) */
HLE(_ZN2nn5album14SaveScreenshotEPKvmNS0_9ImageSizeENS0_17AlbumReportOptionE) {
    c->x[0] = album_save(G(u8, c->x[0]), c->x[1], NULL, 0, "nn::album::SaveScreenshot", NULL);
}
/* (data, size, ImageSize, ImageOrientation, AlbumReportOption, const void *userData, size_t userDataSize) */
HLE(_ZN2nn5album26SaveScreenshotWithUserDataEPKvmNS0_9ImageSizeENS0_16ImageOrientationENS0_17AlbumReportOptionES2_m) {
    c->x[0] = album_save(G(u8, c->x[0]), c->x[1], G(void, c->x[5]), c->x[6], "nn::album::SaveScreenshotWithUserData", NULL);
}
/* (data, size, ImageSize, AlbumReportOption, const account::Uid &, const char *text): saved, not shared */
HLE(_ZN2nn5album22SaveAndShareScreenshotEPKvmNS0_9ImageSizeENS0_17AlbumReportOptionERKNS_7account3UidEPKc) {
    c->x[0] = album_save(G(u8, c->x[0]), c->x[1], NULL, 0, "nn::album::SaveAndShareScreenshot", NULL);
}

/* (int *count, AlbumFileEntry *entries, int max): always a success */
HLE(_ZN2nn5album26GetAlbumScreenshotFileListEPiPNS0_14AlbumFileEntryEi) {
    int n = album_list(G(AlbumEntry, c->x[1]), (int)(i32)c->x[2]);
    if (c->x[0]) st32(c->x[0], (u32)n);
    c->x[0] = 0;
}

/* (int *w, int *h, void *buf, size_t, void *work, size_t, const AlbumFileEntry &) */
HLE(_ZN2nn5album14LoadAlbumImageEPiS1_PvmS2_mRKNS0_14AlbumFileEntryE) {
    c->x[0] = album_load(G(i32, c->x[0]), G(i32, c->x[1]), NULL, NULL, NULL, 0, G(void, c->x[2]), c->x[3], G(void, c->x[4]), c->x[5],
                         G(AlbumEntry, c->x[6]), 0);
}
/* (int *w, int *h, size_t *userDataSize, void *userData, size_t, void *buf, size_t, void *work, [sp] size_t, [sp+8] entry) */
HLE(_ZN2nn5album14LoadAlbumImageEPiS1_PmPvmS3_mS3_mRKNS0_14AlbumFileEntryE) {
    c->x[0] = album_load(G(i32, c->x[0]), G(i32, c->x[1]), NULL, G(u64, c->x[2]), G(void, c->x[3]), c->x[4], G(void, c->x[5]), c->x[6],
                         G(void, c->x[7]), STK(0), G(AlbumEntry, STK(1)), 0);
}
/* the same, the thumbnail */
HLE(_ZN2nn5album23LoadAlbumThumbnailImageEPiS1_PmPvmS3_mS3_mRKNS0_14AlbumFileEntryE) {
    c->x[0] = album_load(G(i32, c->x[0]), G(i32, c->x[1]), NULL, G(u64, c->x[2]), G(void, c->x[3]), c->x[4], G(void, c->x[5]), c->x[6],
                         G(void, c->x[7]), STK(0), G(AlbumEntry, STK(1)), 1);
}
/* (int *w, int *h, AlbumFileAttribute *, void *buf, size_t, void *work, size_t, entry) */
HLE(_ZN2nn5album23LoadAlbumThumbnailImageEPiS1_PNS0_18AlbumFileAttributeEPvmS4_mRKNS0_14AlbumFileEntryE) {
    c->x[0] = album_load(G(i32, c->x[0]), G(i32, c->x[1]), G(u64, c->x[2]), NULL, NULL, 0, G(void, c->x[3]), c->x[4], G(void, c->x[5]), c->x[6],
                         G(AlbumEntry, c->x[7]), 1);
}
/* (int *w, int *h, AlbumFileAttribute *, size_t *userDataSize, void *userData, size_t, void *buf, size_t, [sp] work,
 *  [sp+8] size_t, [sp+16] entry) */
HLE(_ZN2nn5album23LoadAlbumThumbnailImageEPiS1_PNS0_18AlbumFileAttributeEPmPvmS5_mS5_mRKNS0_14AlbumFileEntryE) {
    c->x[0] = album_load(G(i32, c->x[0]), G(i32, c->x[1]), G(u64, c->x[2]), G(u64, c->x[3]), G(void, c->x[4]), c->x[5], G(void, c->x[6]),
                         c->x[7], G(void, STK(0)), STK(1), G(AlbumEntry, STK(2)), 1);
}
