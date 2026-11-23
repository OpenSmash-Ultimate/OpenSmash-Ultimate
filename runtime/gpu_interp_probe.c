/* SSBU_INTERP_PROBE=<file> (a research tool, Direct3D 11): what the game's graphics data does from one picture to the
 * next, for drawing the in-between pictures of frame_rate 120/240 (hle_frame_rate.c) between two logic frames.
 *
 * At 240 the game's main loop draws four pictures a logic frame and only one of them follows a logic frame. To blend
 * the in-between ones the renderer must pair each draw with the same draw of the picture before and know which of its
 * constant buffers move with the logic. The game writes its constant buffers at a new address every picture and its
 * draw order changes when the logic runs, so a draw is known here by what it draws: its two programs, its index (or
 * vertex) data and count, and its target, and the how-many-th such draw of the picture it is. Every picture, each
 * draw's and each compute dispatch's constant buffers (stage, bank, size, bytes up to 64 KiB) are compared with the
 * same draw's in the picture before:
 *
 *   F <picture> t<logic frames> draws <n> paired <n> new <n> gone <n> disp <n> cbs <n> changed <cbs> words <n>
 *     smooth <n> jump <n> | <stage>b<bank>:<cbs changed>/<words> ... (the 12 banks with the most changed words)
 *
 * A changed 32-bit word is "smooth" when both are finite floats whose difference is small beside their size (a value
 * moving: blendable), else a "jump" (a flag, an index, a teleport, a value that appeared). Every 960th picture, the next
 * 8 in detail, a line per changed buffer (at most 400 a picture):
 *
 *   D <picture> <draw> <stage> <program> b<bank> size <bytes> words <n> smooth <n> jump <n> | +<off> <before> -> <after> ...
 *
 * The picture boundary is the game's own: the submissions it had made when it queued the picture (ipc_vi.c's
 * QUEUE_BUFFER, gpu_queue_mark), reached on the GPU thread (gpu_queue.c). The logic frames are the game's frame
 * scheduler's count at that moment (main + 0x523d000, hle_frame_rate.c). */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "host.h"

void interp_probe_frame(u64 mark, u32 ticks);
void interp_probe_job_done(u64 jobs_done);
int interp_probe_on(void);
void interp_probe_cb(int kind, u64 program, const char *name, u32 bank, u64 va, u32 size, void (*read)(u64, u32, void *));
void interp_probe_draw(u64 vs, u64 ps, u64 data, u32 count, u64 target);
void interp_probe_dispatch(u64 program);

enum { KIND_VS, KIND_PS, KIND_CS, KINDS };
static const char *const KIND_NAME[KINDS] = {"vs", "ps", "cs"};
enum { RECS = 1 << 18, DRAWS = 1 << 15, ARENA = 96 << 20, MARKS = 64, DETAIL_EVERY = 960, DETAIL_FRAMES = 8, DETAIL_LINES = 400,
       HASH = 1 << 19 };

typedef struct Rec { u64 draw; u32 kind, bank, size, off; u64 hash; char name[24]; } Rec;   /* off: in the arena, ~0u none */
typedef struct Frame { Rec *rec; u32 n; u8 *arena; size_t used; u64 *draw_ids; u32 draws, disp; } Frame;
static Frame frames[2];
static int cur;
static FILE *out;
static int state = -1;   /* -1 not decided, 0 off, 1 on */
static SRWLOCK marks_lock = SRWLOCK_INIT;
static u64 mark_at[MARKS];
static u32 mark_ticks[MARKS];
static u64 marks_in, marks_out;   /* (a ring: pushed by the game's thread, taken by the GPU thread) */
static u64 pictures, draw_now;    /* (the draw the next buffers are read for) */
static u64 *occ_key; static u32 *occ_n, *occ_gen; static u32 gen = 1;   /* (this picture's draws of each kind, counted) */
static u64 *ht_key; static u32 *ht_val, *ht_gen;                       /* (the picture before's buffers, by draw, stage and bank) */
static u64 *dt_key; static u32 *dt_gen;                                  /* (a set of draw ids) */

static u64 mix(u64 h, u64 v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); return h * 0xff51afd7ed558ccdull ^ (h >> 29); }

int interp_probe_on(void) {
    if (state < 0) {
        const char *p = getenv("SSBU_INTERP_PROBE");
        state = 0;
        if (p && *p && (out = fopen(p, "w")) != NULL) {
            int ok = 1;
            for (int i = 0; i < 2; i++) {
                frames[i].rec = malloc(sizeof(Rec) * RECS);
                frames[i].arena = malloc(ARENA);
                frames[i].draw_ids = malloc(sizeof(u64) * DRAWS);
                ok &= frames[i].rec && frames[i].arena && frames[i].draw_ids;
            }
            occ_key = calloc(HASH, 8); occ_n = calloc(HASH, 4); occ_gen = calloc(HASH, 4);
            ht_key = calloc(HASH, 8); ht_val = calloc(HASH, 4); ht_gen = calloc(HASH, 4);
            dt_key = calloc(HASH, 8); dt_gen = calloc(HASH, 4);
            ok &= occ_key && occ_n && occ_gen && ht_key && ht_val && ht_gen && dt_key && dt_gen;
            if (ok) state = 1;
            else fprintf(stderr, "[probe] SSBU_INTERP_PROBE: out of memory\n");
            if (state) fprintf(stderr, "[probe] SSBU_INTERP_PROBE (2): the constant buffers of every picture, draws paired by what they draw, into %s\n", p);
        }
    }
    return state;
}

static u64 fnv(const u8 *d, u32 n) {
    u64 h = 1469598103934665603ull;
    for (u32 i = 0; i < n; i++) h = (h ^ d[i]) * 1099511628211ull;
    return h;
}

/* (the GPU thread) a draw: the buffers read next are its */
void interp_probe_draw(u64 vs, u64 ps, u64 data, u32 count, u64 target) {
    if (state != 1) return;
    u64 id = mix(mix(mix(mix(vs, ps), data), count), target);
    u32 i = (u32)(id >> 13) & (HASH - 1);
    while (occ_gen[i] == gen && occ_key[i] != id) i = (i + 1) & (HASH - 1);
    if (occ_gen[i] != gen) { occ_gen[i] = gen; occ_key[i] = id; occ_n[i] = 0; }
    draw_now = mix(id, ++occ_n[i]);   /* (the n-th draw of this kind in the picture) */
    Frame *f = &frames[cur];
    if (f->draws < DRAWS) f->draw_ids[f->draws] = draw_now;
    f->draws++;
}
void interp_probe_dispatch(u64 program) {
    if (state != 1) return;
    interp_probe_draw(program, 0xC0C0, 0, 0, 0);   /* (a dispatch: known by its program and how many came before) */
    frames[cur].draws--;
    frames[cur].disp++;
}

/* (the GPU thread) one constant buffer the current draw or dispatch reads */
void interp_probe_cb(int kind, u64 program, const char *name, u32 bank, u64 va, u32 size, void (*read)(u64, u32, void *)) {
    (void)program;
    if (state != 1 || kind < 0 || kind >= KINDS) return;
    Frame *f = &frames[cur];
    if (f->n >= RECS) return;
    Rec *r = &f->rec[f->n++];
    r->draw = draw_now; r->kind = (u32)kind; r->bank = bank;
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

/* (the game's thread, QUEUE_BUFFER) a picture queued: everything submitted so far is its */
void interp_probe_frame(u64 mark, u32 ticks) {
    if (interp_probe_on() != 1) return;
    AcquireSRWLockExclusive(&marks_lock);
    if (marks_in - marks_out < MARKS) { mark_at[marks_in % MARKS] = mark; mark_ticks[marks_in % MARKS] = ticks; marks_in++; }
    ReleaseSRWLockExclusive(&marks_lock);
}

static int smooth(u32 a, u32 b) {
    float x, y;
    memcpy(&x, &a, 4); memcpy(&y, &b, 4);
    if (!isfinite(x) || !isfinite(y)) return 0;
    if ((a & 0x7f800000u) == 0 && a) return 0;   /* (a denormal: an integer's bits, not a float) */
    if ((b & 0x7f800000u) == 0 && b) return 0;
    float m = fabsf(x) > fabsf(y) ? fabsf(x) : fabsf(y);
    return fabsf(x - y) <= 0.25f * m + 1e-3f && m < 1e7f;
}

typedef struct BankStat { u32 key, cbs, size; u64 words; } BankStat;
static int by_words(const void *a, const void *b) {
    u64 x = ((const BankStat *)a)->words, y = ((const BankStat *)b)->words;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void finish(u32 ticks) {
    Frame *f = &frames[cur], *p = &frames[cur ^ 1];
    u32 g = gen;
    /* the picture before's buffers by (draw, stage, bank), and its draws */
    for (u32 i = 0; i < p->n; i++) {
        u64 k = mix(p->rec[i].draw, (u64)p->rec[i].kind << 8 | p->rec[i].bank);
        u32 h = (u32)(k >> 13) & (HASH - 1);
        while (ht_gen[h] == g && ht_key[h] != k) h = (h + 1) & (HASH - 1);
        ht_gen[h] = g; ht_key[h] = k; ht_val[h] = i;
    }
    u32 pd = p->draws < DRAWS ? p->draws : DRAWS, fd = f->draws < DRAWS ? f->draws : DRAWS, paired = 0;
    for (u32 i = 0; i < pd; i++) {
        u64 k = p->draw_ids[i];
        u32 h = (u32)(k >> 13) & (HASH - 1);
        while (dt_gen[h] == g && dt_key[h] != k) h = (h + 1) & (HASH - 1);
        dt_gen[h] = g; dt_key[h] = k;
    }
    for (u32 i = 0; i < fd; i++) {
        u64 k = f->draw_ids[i];
        u32 h = (u32)(k >> 13) & (HASH - 1);
        while (dt_gen[h] == g && dt_key[h] != k) h = (h + 1) & (HASH - 1);
        if (dt_gen[h] == g) paired++;
    }
    static BankStat banks[KINDS * 32];
    memset(banks, 0, sizeof banks);
    u32 changed = 0;
    u64 words = 0, sm = 0, jp = 0;
    int detail = pictures % DETAIL_EVERY < DETAIL_FRAMES && pictures >= DETAIL_EVERY;
    u32 lines = 0;
    for (u32 i = 0; i < f->n; i++) {
        const Rec *b = &f->rec[i];
        u64 k = mix(b->draw, (u64)b->kind << 8 | b->bank);
        u32 h = (u32)(k >> 13) & (HASH - 1);
        while (ht_gen[h] == g && ht_key[h] != k) h = (h + 1) & (HASH - 1);
        if (ht_gen[h] != g) continue;   /* (a draw the picture before had not) */
        const Rec *a = &p->rec[ht_val[h]];
        if (a->hash == b->hash && a->size == b->size) continue;
        changed++;
        BankStat *bs = &banks[(b->kind < KINDS ? b->kind : 0) * 32 + (b->bank & 31)];
        bs->key = (b->kind << 8) | b->bank; bs->cbs++; if (b->size > bs->size) bs->size = b->size;
        if (a->off == ~0u || b->off == ~0u) continue;
        const u32 *x = (const u32 *)(p->arena + a->off), *y = (const u32 *)(f->arena + b->off);
        u32 n = (a->size < b->size ? a->size : b->size) / 4, w = 0, s = 0, shown = 0;
        char buf[512];
        int len = 0;
        for (u32 j = 0; j < n; j++) {
            if (x[j] == y[j]) continue;
            w++;
            s += smooth(x[j], y[j]);
            if (detail && shown < 3) {
                float fx, fy;
                memcpy(&fx, &x[j], 4); memcpy(&fy, &y[j], 4);
                len += snprintf(buf + len, sizeof buf - (size_t)len, " +%#x %g -> %g", j * 4, fx, fy);
                shown++;
            }
        }
        words += w; sm += s; jp += w - s; bs->words += w;
        if (detail && lines < DETAIL_LINES) {
            fprintf(out, "D %llu %08x %s %s b%u size %#x words %u smooth %u jump %u |%s\n", (unsigned long long)pictures, (u32)(b->draw >> 32),
                    KIND_NAME[b->kind < KINDS ? b->kind : 0], b->name, b->bank, b->size, w, s, w - s, len ? buf : "");
            lines++;
        }
    }
    fprintf(out, "F %llu t%u draws %u paired %u new %u gone %u disp %u cbs %u changed %u words %llu smooth %llu jump %llu |",
            (unsigned long long)pictures, ticks, f->draws, paired, fd - paired, pd > paired ? pd - paired : 0, f->disp, f->n, changed,
            (unsigned long long)words, (unsigned long long)sm, (unsigned long long)jp);
    qsort(banks, sizeof banks / sizeof *banks, sizeof *banks, by_words);
    for (int i = 0; i < 12 && banks[i].cbs; i++)
        fprintf(out, " %sb%u:%u/%llu(%#x)", KIND_NAME[(banks[i].key >> 8) < KINDS ? banks[i].key >> 8 : 0], banks[i].key & 0xFF, banks[i].cbs,
                (unsigned long long)banks[i].words, banks[i].size);
    fputc('\n', out);
    if (pictures % 240 == 0) fflush(out);
    pictures++;
    gen++;   /* (every table empty again: entries of older generations are free) */
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
