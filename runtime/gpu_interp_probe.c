/* SSBU_INTERP_PROBE=<file> (a research tool, Direct3D 11): what the game's graphics data does from one picture to the
 * next, for drawing the in-between pictures of frame_rate 120/240 (hle_frame_rate.c) between two logic frames.
 *
 * At 240 the game's main loop draws four pictures a logic frame, and only one of them follows a logic frame: the others
 * are drawn from the same state (SSBU_PRESENT_HASH's pictures change once in four). To blend the in-between ones, the
 * renderer needs to know which of the constant buffers its draws read are the ones that move with the logic - the
 * camera's, each model's, each skeleton's - and how. This file records, for every picture: each draw's and each compute
 * dispatch's constant buffers (stage, program, bank, address, size, a hash of the bytes, and the bytes themselves up to
 * 64 KiB a buffer), and compares them with the picture before, buffer by buffer in draw order. A line per picture:
 *
 *   F <picture> t<logic frames the game ran in it> draws <n> disp <n> cbs <n> order <same>/<n> changed <cbs> <words>
 *     | vs <changed by bank 0..17> | ps <...> | cs <...>
 *
 * and every 960th picture, the next 8 in detail: a line per buffer that changed (at most 400 a picture), with how many
 * of its 32-bit words changed and the first three changes as floats:
 *
 *   D <picture> <index> <vs|ps|cs> <program> b<bank> va <va> size <bytes> words <n> | +<off> <before> -> <after> ...
 *
 * The picture boundary is the game's own: the submissions it had made when it queued the picture (ipc_vi.c's
 * QUEUE_BUFFER, gpu_queue_mark), reached on the GPU thread (gpu_queue.c), so a picture's lines hold its draws exactly.
 * The logic frames are the game's frame scheduler's count for that pass (main + 0x523d000, hle_frame_rate.c). */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"

void interp_probe_frame(u64 mark, u32 ticks);
void interp_probe_job_done(u64 jobs_done);
int interp_probe_on(void);
void interp_probe_cb(int kind, u64 program, const char *name, u32 bank, u64 va, u32 size, void (*read)(u64, u32, void *));
void interp_probe_draw(void);
void interp_probe_dispatch(void);

enum { KIND_VS, KIND_PS, KIND_CS, KINDS };
static const char *const KIND_NAME[KINDS] = {"vs", "ps", "cs"};
enum { RECS = 1 << 18, ARENA = 96 << 20, MARKS = 64, DETAIL_EVERY = 960, DETAIL_FRAMES = 8, DETAIL_LINES = 400 };

typedef struct Rec { u64 key, va, hash; u32 size, off; char name[24]; } Rec;   /* off: in the arena, ~0u none */
typedef struct Frame { Rec *rec; u32 n; u8 *arena; size_t used; u32 draws, disp; } Frame;
static Frame frames[2];
static int cur;
static FILE *out;
static int state = -1;   /* -1 not decided, 0 off, 1 on */
static SRWLOCK marks_lock = SRWLOCK_INIT;
static u64 mark_at[MARKS];
static u32 mark_ticks[MARKS];
static u64 marks_in, marks_out;   /* (a ring: pushed by the game's thread, taken by the GPU thread) */
static u64 pictures;

int interp_probe_on(void) {
    if (state < 0) {
        const char *p = getenv("SSBU_INTERP_PROBE");
        state = 0;
        if (p && *p && (out = fopen(p, "w")) != NULL) {
            for (int i = 0; i < 2; i++) {
                frames[i].rec = malloc(sizeof(Rec) * RECS);
                frames[i].arena = malloc(ARENA);
            }
            if (frames[0].rec && frames[1].rec && frames[0].arena && frames[1].arena) state = 1;
            else fprintf(stderr, "[probe] SSBU_INTERP_PROBE: out of memory\n");
            if (state) fprintf(stderr, "[probe] SSBU_INTERP_PROBE: the constant buffers of every picture into %s\n", p);
        }
    }
    return state;
}

static u64 fnv(const u8 *d, u32 n) {
    u64 h = 1469598103934665603ull;
    for (u32 i = 0; i < n; i++) h = (h ^ d[i]) * 1099511628211ull;
    return h;
}

/* (the GPU thread) one constant buffer a draw or dispatch reads */
void interp_probe_cb(int kind, u64 program, const char *name, u32 bank, u64 va, u32 size, void (*read)(u64, u32, void *)) {
    if (state != 1 || kind < 0 || kind >= KINDS) return;
    Frame *f = &frames[cur];
    if (f->n >= RECS) return;
    Rec *r = &f->rec[f->n++];
    r->key = program ^ (u64)kind << 56 ^ (u64)bank << 48;
    r->va = va;
    r->size = size > 0x10000 ? 0x10000 : size;
    r->off = ~0u;
    r->hash = 0;
    snprintf(r->name, sizeof r->name, "%s", name ? name : "?");
    if (!va || !r->size) return;
    if (f->used + r->size <= ARENA) {
        u8 *d = f->arena + f->used;
        read(va, r->size, d);
        r->hash = fnv(d, r->size);
        r->off = (u32)f->used;
        f->used += (r->size + 15) & ~15u;
    } else {
        static u8 tmp[0x10000];
        read(va, r->size, tmp);
        r->hash = fnv(tmp, r->size);
    }
}
void interp_probe_draw(void) { if (state == 1) frames[cur].draws++; }
void interp_probe_dispatch(void) { if (state == 1) frames[cur].disp++; }

/* (the game's thread, QUEUE_BUFFER) a picture queued: everything submitted so far is its */
void interp_probe_frame(u64 mark, u32 ticks) {
    if (interp_probe_on() != 1) return;
    AcquireSRWLockExclusive(&marks_lock);
    if (marks_in - marks_out < MARKS) { mark_at[marks_in % MARKS] = mark; mark_ticks[marks_in % MARKS] = ticks; marks_in++; }
    ReleaseSRWLockExclusive(&marks_lock);
}

static void finish(u32 ticks) {
    Frame *f = &frames[cur], *p = &frames[cur ^ 1];
    u32 same = 0, changed = 0, by[KINDS][18] = {{0}};
    u64 words = 0;
    int detail = pictures % DETAIL_EVERY < DETAIL_FRAMES && pictures >= DETAIL_EVERY;
    u32 lines = 0;
    for (u32 i = 0; i < f->n && i < p->n; i++) {
        const Rec *a = &p->rec[i], *b = &f->rec[i];
        if (a->key != b->key) continue;
        same++;
        if (a->hash == b->hash && a->size == b->size) continue;
        changed++;
        int kind = (int)(b->key >> 56) & 0xFF;
        u32 bank = (u32)(b->key >> 48) & 0xFF;
        if (kind < KINDS && bank < 18) by[kind][bank]++;
        if (a->off == ~0u || b->off == ~0u) continue;
        const u32 *x = (const u32 *)(p->arena + a->off), *y = (const u32 *)(f->arena + b->off);
        u32 n = (a->size < b->size ? a->size : b->size) / 4, w = 0, shown = 0;
        char buf[512];
        int len = 0;
        for (u32 k = 0; k < n; k++) {
            if (x[k] == y[k]) continue;
            w++;
            if (detail && shown < 3) {
                float fx, fy;
                memcpy(&fx, &x[k], 4); memcpy(&fy, &y[k], 4);
                len += snprintf(buf + len, sizeof buf - (size_t)len, " +%#x %g -> %g", k * 4, fx, fy);
                shown++;
            }
        }
        words += w;
        if (detail && lines < DETAIL_LINES) {
            fprintf(out, "D %llu %u %s %s b%u va %#llx size %#x words %u |%s\n", (unsigned long long)pictures, i,
                    kind < KINDS ? KIND_NAME[kind] : "?", b->name, bank, (unsigned long long)b->va, b->size, w, len ? buf : "");
            lines++;
        }
    }
    fprintf(out, "F %llu t%u draws %u disp %u cbs %u order %u/%u changed %u %llu", (unsigned long long)pictures, ticks, f->draws,
            f->disp, f->n, same, f->n < p->n ? f->n : p->n, changed, (unsigned long long)words);
    for (int k = 0; k < KINDS; k++) {
        fprintf(out, " | %s", KIND_NAME[k]);
        for (u32 b = 0; b < 18; b++) fprintf(out, " %u", by[k][b]);
    }
    fputc('\n', out);
    if (pictures % 240 == 0) fflush(out);
    pictures++;
    cur ^= 1;
    frames[cur].n = 0; frames[cur].used = 0; frames[cur].draws = 0; frames[cur].disp = 0;
}

/* (the GPU thread, before running submission number jobs_done) the pictures whose submissions have all run */
void interp_probe_job_done(u64 jobs_done) {
    if (state != 1) return;
    for (;;) {
        u32 ticks;
        AcquireSRWLockExclusive(&marks_lock);
        int ready = marks_out < marks_in && (i64)(jobs_done - mark_at[marks_out % MARKS]) >= 0;
        if (ready) { ticks = mark_ticks[marks_out % MARKS]; marks_out++; }
        ReleaseSRWLockExclusive(&marks_lock);
        if (!ready) return;
        finish(ticks);
    }
}
