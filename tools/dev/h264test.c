/* Offline check of the H.264 path: replays recorded NVDEC jobs (SSBU_DUMP_HOST1X files) through
 * the parameter set writer and the decoder, as runtime/gpu_host1x.c does: the headers before each
 * IDR picture, the end units stripped. Says each picture that comes out (the job it was decoded
 * from) and saves some as BMPs; with --stream it also writes the stream it fed, for any other
 * decoder (ffmpeg, PyAV) to compare with.
 *   h264test <dump dir> <out dir> [jobs to save, e.g. 0,60,120] [--stream]
 * Build (from the repo): clang-cl /O2 /I runtime tools/dev/h264test.c runtime/gpu_h264.c ole32.lib */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_h264.h"

typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;

typedef struct Job { u32 regs[0x800]; u8 *blob[0x800]; u32 blob_size[0x800]; } Job;

static int load(const char *path, Job *j) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    u32 head[2];
    memset(j, 0, sizeof *j);
    if (fread(head, 4, 2, f) != 2 || head[0] != 0x31583148 || head[1] > 0x800 || fread(j->regs, 4, head[1], f) != head[1]) { fclose(f); return 0; }
    u32 m, size;
    while (fread(&m, 4, 1, f) == 1 && fread(&size, 4, 1, f) == 1 && m < 0x800) {
        j->blob[m] = malloc(size ? size : 1);
        j->blob_size[m] = size;
        if (fread(j->blob[m], 1, size, f) != size) break;
    }
    fclose(f);
    return 1;
}
static void unload(Job *j) { for (u32 m = 0; m < 0x800; m++) free(j->blob[m]); }

static const char *out_dir;
static u32 save[64], nsave, pictures;
static long long last_tag = -1;
static u32 out_of_order, fed_jobs, most_behind;

static void save_bmp(const char *path, u32 w, u32 h, const u8 *y, const u8 *uv, u32 pitch) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    u32 row = w * 3, size = row * h;
    u8 head[54] = {'B', 'M'};
    *(u32 *)(head + 2) = 54 + size; *(u32 *)(head + 10) = 54; *(u32 *)(head + 14) = 40;
    *(int *)(head + 18) = (int)w; *(int *)(head + 22) = -(int)h; *(unsigned short *)(head + 26) = 1; *(unsigned short *)(head + 28) = 24;
    fwrite(head, 1, 54, f);
    u8 *line = malloc(row);
    for (u32 r = 0; r < h; r++) {
        for (u32 x = 0; x < w; x++) {   /* BT.601 studio range, as gpu_host1x.c's VIC */
            int Y = (y[(size_t)r * pitch + x] - 16) * 298, U = uv[(size_t)(r / 2) * pitch + (x & ~1u)] - 128, V = uv[(size_t)(r / 2) * pitch + (x & ~1u) + 1] - 128;
            int R = (Y + 409 * V + 128) >> 8, G = (Y - 100 * U - 208 * V + 128) >> 8, B = (Y + 516 * U + 128) >> 8;
            line[3 * x] = (u8)(B < 0 ? 0 : B > 255 ? 255 : B); line[3 * x + 1] = (u8)(G < 0 ? 0 : G > 255 ? 255 : G); line[3 * x + 2] = (u8)(R < 0 ? 0 : R > 255 ? 255 : R);
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
}

static void got(void *user, u64 tag, u32 w, u32 h, const u8 *y, const u8 *uv, u32 pitch) {
    (void)user;
    u32 behind = fed_jobs ? fed_jobs - 1 - (u32)tag : 0;   /* jobs fed after this picture's before it came out (0: the drain) */
    if (behind > most_behind) most_behind = behind;
    printf("picture %u: job %llu (%u jobs later), %ux%u, pitch %u\n", pictures, (unsigned long long)tag, behind, w, h, pitch);
    if ((long long)tag < last_tag) out_of_order++;
    last_tag = (long long)tag;
    for (u32 i = 0; i < nsave; i++)
        if (save[i] == pictures) {
            char path[1024];
            snprintf(path, sizeof path, "%s/h264_%03u.bmp", out_dir, pictures);
            save_bmp(path, w, h, y, uv, pitch);
        }
    pictures++;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: h264test <dump dir> <out dir> [pictures to save] [--stream]\n"); return 1; }
    out_dir = argv[2];
    int stream = 0;
    for (int a = 3; a < argc; a++) {
        if (!strcmp(argv[a], "--stream")) { stream = 1; continue; }
        for (char *p = argv[a]; *p && nsave < 64;) { save[nsave++] = (u32)strtoul(p, &p, 10); if (*p == ',') p++; else break; }
    }
    char path[1024];
    snprintf(path, sizeof path, "%s/fed.h264", out_dir);
    FILE *fed = stream ? fopen(path, "wb") : NULL;
    static Job j;
    u32 jobs = 0, refused = 0;
    static u8 buf[1 << 22];
    for (u32 n = 0;; n++) {
        snprintf(path, sizeof path, "%s/nvdec_%05u.bin", argv[1], n);
        if (!load(path, &j)) { if (n > 4096) break; continue; }
        const u8 *setup = j.blob[0x101], *bits = j.blob[0x102];
        if (j.regs[0x80] != 3 || !setup || j.blob_size[0x101] < H264_SETUP_SIZE || !bits) { unload(&j); continue; }
        u32 size = *(const u32 *)(setup + H264_SETUP_STREAM_LEN);
        if (size > j.blob_size[0x102]) size = j.blob_size[0x102];
        size = h264_strip_end_units(bits, size);
        u32 at = 0;
        if (h264_starts_idr(bits, size)) {
            if (jobs) h264_drain(got, NULL);   /* (as gpu_host1x.c: what the decoder held back, before a new sequence) */
            const char *why = NULL;
            at = h264_write_headers(setup, buf, sizeof buf, &why);
            if (!at) { printf("job %u: headers not written: %s\n", jobs, why ? why : "no room"); unload(&j); jobs++; continue; }
        }
        if (at + size > sizeof buf) { unload(&j); jobs++; continue; }
        memcpy(buf + at, bits, size);
        if (fed) fwrite(buf, 1, at + size, fed);
        fed_jobs = jobs + 1;
        int r = h264_decode(buf, at + size, jobs, got, NULL);
        if (r < 0) { printf("no decoder\n"); return 1; }
        if (!r) refused++;
        unload(&j);
        jobs++;
    }
    fed_jobs = 0;   /* (what the drain gives is not counted) */
    h264_drain(got, NULL);
    if (fed) fclose(fed);
    printf("%u jobs, %u pictures, %u refused, %u out of decoding order\n", jobs, pictures, refused, out_of_order);
    return 0;
}
