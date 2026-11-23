/* display.ini's motion (interpolate / extrapolate; the settings menu changes it at once): the in-between pictures of
 * frame_rate 120/240 drawn with the game's motion blended between its last two logic frames, so the characters and the
 * camera move at 120/240 frames a second while the logic, and every frame-exact timing with it, stays at 60.
 *
 * At 240 the game draws four pictures a logic frame (hle_frame_rate.c), and three of them from the same state: the
 * probe (gpu_interp_probe.c) showed that what moves with the logic is in a few of the constant buffers its draws and
 * compute launches read - each skeleton's bones (the skinning launches' palettes, 11-12 KiB), each object's and the
 * camera's matrices - and that almost all of it moves by small steps, while what changes every picture (a frame
 * counter, the UI, effects) is elsewhere. So the renderers (gpu_vk.c, gpu_d3d11.c) ask here for each such buffer:
 *
 *   - Each draw and launch is known by what it draws (its programs, its index or vertex data and count, its target, and
 *     the how-many-th such draw of the picture it is), so the same draw of the next picture finds its buffers'
 *     history here although the game writes them at new addresses and reorders its draws when the logic runs.
 *   - Each buffer's last two different contents are kept. A buffer that changed again before a logic frame's worth of
 *     pictures had passed changes faster than the logic (a counter, the UI, effects): it is left as it is.
 *   - On the pictures after a change, the buffer is drawn blended: interpolate shows prev + (last - prev) * a, a = the
 *     pictures since the change / the pictures a logic frame lasts (a quarter, a half, three quarters at 240), so the
 *     newest logic frame is fully shown one logic frame after it was made - as a picture at 60 shows it - and starts to
 *     show 4 ms after; extrapolate shows last + (last - prev) * a, no later than the game's own picture, guessing
 *     ahead (an overshoot on a sudden turn, put right at the next logic frame).
 *   - Only 32-bit words that are ordinary floats both times and moved by a small step blend; anything else (an
 *     integer, a flag, a jump: a teleport, a camera cut, a respawn) takes the newest value at once, and a change with
 *     many jumps is drawn as it is. Bank 0 (the storage buffers' addresses) and the bindless texture bank (texture
 *     handles) are never touched.
 *
 * The pictures are told apart as the probe does: the submissions the game had made when it queued each picture
 * (ipc_vi.c's QUEUE_BUFFER), reached on the GPU thread (gpu_queue.c). Only while frame_rate is above 60 now
 * (rt_refresh_mult: never online). */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "host.h"
#include "display.h"

void motion_frame(u64 mark);
void motion_job_done(u64 jobs_done);
int motion_active(void);
void motion_draw(u64 vs, u64 ps, u64 data, u32 count, u64 target);
void motion_dispatch(u64 program);
const void *motion_block(int compute, u32 bank, u32 tex_bank, u64 va, u32 size, void (*read)(u64, u32, void *));

extern volatile LONG rt_refresh_mult;      /* (kernel.c) the refreshes a 60 Hz frame now */
extern volatile float rt_passes_per_logic; /* (hle_frame_rate.c) pictures a logic frame lasts at the game's speed now */

enum { ENTRIES = 1 << 14, OCC = 1 << 14, MARKS = 64, MIN_SIZE = 48, MAX_SIZE = 0x4000, SWEEP_EVERY = 480, STALE = 240 };

typedef struct Entry {
    u64 key;
    u32 size, passes;        /* passes: pictures since the last change */
    int used, live, cut;     /* live: changes faster than the logic; cut: the last change was a jump */
    u64 seen;                /* the picture it was last read in */
    u8 *last, *prev;         /* (size bytes each) */
} Entry;
static Entry table[ENTRIES];
static u32 entries;
static u64 occ_key[OCC]; static u32 occ_n[OCC], occ_gen[OCC], gen = 1;
static u64 draw_now, pictures;
static int active;           /* this picture: blend (motion on and frame_rate above 60) */
static SRWLOCK marks_lock = SRWLOCK_INIT;
static u64 mark_at[MARKS], marks_in, marks_out;
static u8 scratch[MAX_SIZE], out[MAX_SIZE];
static u64 stat_blended, stat_live, stat_cut;

static u64 mix(u64 h, u64 v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); return h * 0xff51afd7ed558ccdull ^ (h >> 29); }

static void decide(void) { active = display_motion() != 0 && rt_refresh_mult > 1; }
int motion_active(void) { return active; }

/* (the game's thread, QUEUE_BUFFER) a picture queued */
void motion_frame(u64 mark) {
    AcquireSRWLockExclusive(&marks_lock);
    if (marks_in - marks_out < MARKS) mark_at[marks_in++ % MARKS] = mark;
    ReleaseSRWLockExclusive(&marks_lock);
}

static void sweep(void) {   /* entries no draw has read for a while: freed */
    for (u32 i = 0; i < ENTRIES; i++) {
        Entry *e = &table[i];
        if (e->used == 1 && pictures - e->seen > STALE) { free(e->last); free(e->prev); memset(e, 0, sizeof *e); e->used = -1; entries--; }
    }
}

static void picture_done(void) {
    pictures++;
    gen++;
    if (pictures % SWEEP_EVERY == 0) {
        sweep();
        static u64 said;
        if (active && pictures - said >= 240 * 60) {
            said = pictures;
            fprintf(stderr, "[motion] %s: %llu buffers blended, %llu left alone (faster than the logic), %llu jumps drawn as they are; %u kept\n",
                    display_motion() == 2 ? "extrapolate" : "interpolate", (unsigned long long)stat_blended, (unsigned long long)stat_live,
                    (unsigned long long)stat_cut, entries);
        }
    }
    int was = active;
    decide();
    if (was != active) fprintf(stderr, "[motion] %s\n", active ? (display_motion() == 2 ? "on: the in-between pictures extrapolated" : "on: the in-between pictures interpolated")
                                                              : "off");
}

/* (the GPU thread, before submission number jobs_done) the pictures whose submissions have all run */
void motion_job_done(u64 jobs_done) {
    static int first = 1;
    if (first) { first = 0; decide(); }
    for (;;) {
        AcquireSRWLockExclusive(&marks_lock);
        int ready = marks_out < marks_in && (i64)(jobs_done - mark_at[marks_out % MARKS]) >= 0;
        if (ready) marks_out++;
        ReleaseSRWLockExclusive(&marks_lock);
        if (!ready) return;
        picture_done();
    }
}

/* (the GPU thread) a draw: the buffers asked for next are its */
void motion_draw(u64 vs, u64 ps, u64 data, u32 count, u64 target) {
    if (!active) return;
    u64 id = mix(mix(mix(mix(vs, ps), data), count), target);
    u32 i = (u32)(id >> 13) & (OCC - 1);
    for (u32 probes = 0; occ_gen[i] == gen && occ_key[i] != id && probes < OCC; probes++) i = (i + 1) & (OCC - 1);
    if (occ_gen[i] != gen) { occ_gen[i] = gen; occ_key[i] = id; occ_n[i] = 0; }
    draw_now = mix(id, ++occ_n[i]);
}
void motion_dispatch(u64 program) { motion_draw(program, 0xC0C0, 0, 0, 0); }

static Entry *find(u64 key, int make) {
    u32 i = (u32)(key >> 17) & (ENTRIES - 1), free_at = ~0u;
    for (u32 probes = 0; probes < ENTRIES; probes++, i = (i + 1) & (ENTRIES - 1)) {
        Entry *e = &table[i];
        if (e->used == 1 && e->key == key) return e;
        if (e->used != 1 && free_at == ~0u) free_at = i;
        if (e->used == 0) break;   /* (never used: the key is not further on) */
    }
    if (!make || free_at == ~0u || entries >= ENTRIES * 3 / 4) return NULL;
    Entry *e = &table[free_at];
    memset(e, 0, sizeof *e);
    e->used = 1; e->key = key;
    entries++;
    return e;
}

/* A word moved by a small step: both ordinary floats (or zero), the difference small beside them. */
static int smooth(u32 a, u32 b) {
    if (((a & 0x7f800000u) == 0 && (a & 0x7fffffffu)) || ((b & 0x7f800000u) == 0 && (b & 0x7fffffffu))) return 0;   /* (an integer's bits) */
    float x, y;
    memcpy(&x, &a, 4); memcpy(&y, &b, 4);
    if (!isfinite(x) || !isfinite(y)) return 0;
    float m = fabsf(x) > fabsf(y) ? fabsf(x) : fabsf(y), d = fabsf(x - y);
    return m < 1e7f && (d <= 0.25f * m || d < 1e-3f);
}

/* (the GPU thread) one constant buffer of the current draw (compute 0) or launch (compute 1): the bytes to draw with
 * instead of the game's (valid until the next call), or NULL for the game's own. */
const void *motion_block(int compute, u32 bank, u32 tex_bank, u64 va, u32 size, void (*read)(u64, u32, void *)) {
    if (!active || !va || bank == 0 || (!compute && bank == tex_bank) || size < MIN_SIZE || size > MAX_SIZE || (size & 3)) return NULL;
    Entry *e = find(mix(draw_now, (u64)compute << 8 | bank), 1);
    if (!e) return NULL;
    read(va, size, scratch);
    e->seen = pictures;
    if (e->size != size || !e->last || !e->prev) {   /* new, or another size: its history starts here */
        free(e->last); free(e->prev);
        e->last = malloc(size); e->prev = malloc(size);
        if (!e->last || !e->prev) { free(e->last); free(e->prev); e->last = e->prev = NULL; e->size = 0; return NULL; }
        memcpy(e->last, scratch, size); memcpy(e->prev, scratch, size);
        e->size = size; e->passes = 0; e->live = 1; e->cut = 0;
        return NULL;
    }
    float per = rt_passes_per_logic > 1 ? rt_passes_per_logic : (float)rt_refresh_mult;
    const u32 *now = (const u32 *)scratch, *last = (const u32 *)e->last;
    u32 n = size / 4;
    if (memcmp(scratch, e->last, size)) {   /* a change: the game's newest state */
        u32 changed = 0, jumps = 0;
        for (u32 k = 0; k < n; k++) if (now[k] != last[k]) { changed++; jumps += !smooth(last[k], now[k]); }
        e->live = (float)(e->passes + 1) < per * 0.75f;   /* (changed again sooner than a logic frame lasts) */
        e->cut = jumps * 3 > changed;
        memcpy(e->prev, e->cut ? scratch : e->last, size);   /* (a cut: nothing to blend from) */
        memcpy(e->last, scratch, size);
        e->passes = 0;
    } else e->passes++;
    if (e->live) { stat_live++; return NULL; }
    if (e->cut) { stat_cut++; return NULL; }
    float a = (float)e->passes / per;
    if (a > 1) a = 1;
    int extrapolate = display_motion() == 2;
    if ((extrapolate && a <= 0) || (!extrapolate && a >= 1)) return NULL;   /* (the game's own picture) */
    const u32 *prev = (const u32 *)e->prev;
    u32 *o = (u32 *)out;
    for (u32 k = 0; k < n; k++) {
        if (prev[k] == last[k] || !smooth(prev[k], last[k])) { o[k] = last[k]; continue; }
        float p, l, v;
        memcpy(&p, &prev[k], 4); memcpy(&l, &last[k], 4);
        v = extrapolate ? l + (l - p) * a : p + (l - p) * a;
        memcpy(&o[k], &v, 4);
    }
    stat_blended++;
    return out;
}
