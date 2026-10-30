/* The fight test switches' own logic (docs/TESTING-RUNS.md), apart from the runtime so that
 * tools/dev/fighttest.c can check it on made-up memory: SSBU_FIGHT_HASH's line, SSBU_DETERMINISTIC's
 * generator state, refresh report and sequence steps, SSBU_FIXED_SEED's numbers, and
 * SSBU_DEBUG_MATCH / SSBU_DEBUG_END. As ssbu-recomp has them (fighter_sdk/fighter_host.c,
 * runtime/debug_mode.c, os_threads.c, nnsdk.c); hle_fight_test.c runs them on the game. Guest
 * structures are reached through host pointers here, little-endian like the guest. */
#pragma once
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline uint32_t fight_get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline void fight_put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static inline void fight_put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

/* ---- hashes ---------------------------------------------------------------------------- */
#define FIGHT_FNV_BASIS 0xcbf29ce484222325ull
static inline uint64_t fight_fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    while (n--) { h ^= *b++; h *= 0x100000001b3ull; }
    return h;
}

/* A Hash40: the name's length << 32 | its CRC-32. */
static inline uint64_t fight_hash40(const char *s) {
    uint32_t crc = 0xffffffffu;
    size_t n = strlen(s);
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint8_t)s[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return (uint64_t)n << 32 | (uint32_t)~crc;
}

/* XXH64 (the public algorithm): the key a Lua constant's name has in the game's lookup. */
static inline uint64_t fight_rotl(uint64_t v, int n) { return v << n | v >> (64 - n); }
static inline uint64_t fight_xxh64(const void *data, size_t n, uint64_t seed) {
    const uint64_t P1 = 0x9E3779B185EBCA87ull, P2 = 0xC2B2AE3D27D4EB4Full, P3 = 0x165667B19E3779F9ull,
                   P4 = 0x85EBCA77C2B2AE63ull, P5 = 0x27D4EB2F165667C5ull;
    const uint8_t *p = (const uint8_t *)data, *end = p + n;
    uint64_t h, v[4], w;
    uint32_t w32;
    if (n >= 32) {
        v[0] = seed + P1 + P2; v[1] = seed + P2; v[2] = seed; v[3] = seed - P1;
        for (; p + 32 <= end; p += 32)
            for (int k = 0; k < 4; k++) { memcpy(&w, p + 8 * k, 8); v[k] = fight_rotl(v[k] + w * P2, 31) * P1; }
        h = fight_rotl(v[0], 1) + fight_rotl(v[1], 7) + fight_rotl(v[2], 12) + fight_rotl(v[3], 18);
        for (int k = 0; k < 4; k++) h = (h ^ fight_rotl(v[k] * P2, 31) * P1) * P1 + P4;
    } else {
        h = seed + P5;
    }
    h += n;
    for (; p + 8 <= end; p += 8) { memcpy(&w, p, 8); h ^= fight_rotl(w * P2, 31) * P1; h = fight_rotl(h, 27) * P1 + P4; }
    if (p + 4 <= end) { memcpy(&w32, p, 4); h ^= w32 * P1; h = fight_rotl(h, 23) * P2 + P3; p += 4; }
    for (; p < end; p++) { h ^= *p * P5; h = fight_rotl(h, 11) * P1; }
    h ^= h >> 33; h *= P2; h ^= h >> 29; h *= P3; h ^= h >> 32;
    return h;
}

/* ---- SSBU_FIGHT_HASH -------------------------------------------------------------------- */
/* The script random generators: 9 xorshift128 generators of 20 bytes each (4 state words and a
 * call count), the ones app::sv_math::rand picks by its Hash40 argument (ssbu-recomp). */
enum { FIGHT_RNGS = 9, FIGHT_RNG_WORDS = 5 * FIGHT_RNGS };

/* The sum of the 9 call counts and the FNV-1a of the 180 bytes (the basis when there are none). */
static inline void fight_rng_sum(const uint32_t *w, uint32_t *calls, uint64_t *hash) {
    *calls = 0;
    *hash = FIGHT_FNV_BASIS;
    if (!w) return;
    for (int i = 0; i < FIGHT_RNGS; i++) *calls += w[5 * i + 4];
    *hash = fight_fnv(*hash, w, 4 * FIGHT_RNG_WORDS);
}

/* What one line adds to the running hash: ssbu-recomp's record, zeroed and filled. */
typedef struct FightRec { long long f; int k, st; uint64_t mot; float mf, x, y, dmg; uint64_t rh; } FightRec;
_Static_assert(sizeof(FightRec) == 48, "ssbu-recomp's 48-byte record");

typedef struct FightFrame { long long f; int entry, st; uint64_t mot; float mf, x, y, dmg; uint32_t rngcalls; uint64_t rh; } FightFrame;

/* One fighter's fight frame: ssbu-recomp's line, field for field, into out (the running hash
 * *run taken on first). Returns the length written: the line's, or size - 1 where it was cut
 * short (357 bytes and its 0 hold the longest, every number at its widest). */
enum { FIGHT_LINE_MAX = 357 };
static inline int fight_hash_line(char *out, size_t size, uint64_t *run, const FightFrame *fr) {
    FightRec rec;
    memset(&rec, 0, sizeof rec);
    rec.f = fr->f; rec.k = fr->entry; rec.st = fr->st; rec.mot = fr->mot;
    rec.mf = fr->mf; rec.x = fr->x; rec.y = fr->y; rec.dmg = fr->dmg; rec.rh = fr->rh;
    *run = fight_fnv(*run, &rec, sizeof rec);
    uint32_t bx, by, bm, bd;
    memcpy(&bx, &fr->x, 4); memcpy(&by, &fr->y, 4); memcpy(&bm, &fr->mf, 4); memcpy(&bd, &fr->dmg, 4);
    int n = snprintf(out, size, "F %lld e %d st %x mot %llx mf %08x x %08x y %08x dmg %08x rngcalls %u rng %08x H %016llx  (%.3f %.3f frame %.2f %.2f%%)\n",
                     fr->f, fr->entry, (unsigned)fr->st, (unsigned long long)fr->mot, bm, bx, by, bd, fr->rngcalls,
                     (unsigned)(fr->rh ^ fr->rh >> 32), (unsigned long long)*run, (double)fr->x, (double)fr->y, (double)fr->mf,
                     (double)fr->dmg);
    return n < 0 || !size ? 0 : (size_t)n >= size ? (int)(size - 1) : n;
}

/* ---- SSBU_DETERMINISTIC ----------------------------------------------------------------- */
/* The generators' fixed state at the first battle frame, call counts 0 (ssbu-recomp's). */
static inline void fight_rng_fixed(uint32_t w[FIGHT_RNG_WORDS]) {
    for (unsigned i = 0; i < FIGHT_RNGS; i++) {
        w[5 * i] = 0x12345678u + 0x9E3779B9u * i;
        w[5 * i + 1] = 0x2468ACE1u ^ i;
        w[5 * i + 2] = 0x0F1E2D3Cu + i;
        w[5 * i + 3] = 0x89ABCDEFu - i;
        w[5 * i + 4] = 0;
    }
}

/* SSBU_DET_OFF's bits: leave the generators, count polls, leave the refresh report. */
enum { DET_OFF_RNG = 1, DET_OFF_STEPS = 2, DET_OFF_DROPS = 4 };

/* The frame end's report of dropped refreshes, after the call: the count it wrote through its
 * third argument (NULL: none given) and its result code (NULL: none). Cleared unless keep;
 * counted either way. */
typedef struct FightDrops { long long frame_ends, calls, frames; } FightDrops;
static inline void fight_frame_end(FightDrops *d, uint8_t *count, uint8_t *result, int keep) {
    d->frame_ends++;
    uint64_t n;
    if (!count || (memcpy(&n, count, 8), !n)) return;
    d->calls++;
    d->frames += (long long)n;
    if (keep) return;
    fight_put64(count, 0);
    if (result && (fight_get32(result) == 0xF0044007u || fight_get32(result) == 0xF0044005u)) fight_put32(result, 0);
}

/* SSBU_SEQ's ticks for one poll from the first fighter's fight frame and the game's ready-go
 * flag: the fight frames since the last poll while a fight runs; after it has ended (ready-go
 * cleared) one a poll again until the next one; a new fight's count starting lower is no step. */
typedef struct FightSteps { long long last, hist[4]; int was_on, ended; } FightSteps;
static inline long fight_steps(FightSteps *s, long long frame, int on) {
    long long n = frame - s->last;
    if (on) { s->was_on = 1; s->ended = 0; }
    else if (s->was_on) s->ended = 1;
    if (n < 0 && s->was_on) { s->last = frame; return s->ended ? 1 : 0; }
    if (s->ended) { s->last = frame; return 1; }
    s->last = frame;
    s->hist[n < 0 ? 0 : n > 3 ? 3 : n]++;
    return n < 0 ? 0 : n > 100000 ? 100000 : (long)n;
}

/* ---- SSBU_FIXED_SEED -------------------------------------------------------------------- */
#define FIXED_TICK 0x123456789ull      /* what the clock reads that seed generators give */
#define FIXED_TIME 1700000000ull       /* the same for the time of day (POSIX seconds) */
#define FIXED_GOLDEN 0x9E3779B97F4A7C15ull

/* Set and not empty: n, as strtoull reads it (any base). */
static inline int fixed_seed_parse(const char *text, uint64_t *n) {
    if (!text || !*text) return 0;
    *n = strtoull(text, NULL, 0);
    return 1;
}
static inline uint64_t fixed_mix(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
/* The random bytes' stream: splitmix64 from n * golden + 1, 8 bytes a step (the last cut). */
static inline uint64_t fixed_stream(uint64_t n) { return n * FIXED_GOLDEN + 1; }
static inline void fixed_fill(uint64_t *state, void *buf, uint64_t size) {
    for (uint64_t i = 0; i < size; i += 8) {
        uint64_t z = fixed_mix(*state += FIXED_GOLDEN);
        memcpy((uint8_t *)buf + i, &z, size - i < 8 ? (size_t)(size - i) : 8);
    }
}
/* The kernel's random entropy (svcGetInfo 11, sub-id sub): one number for each, as the console
 * keeps one set for the process. */
static inline uint64_t fixed_entropy(uint64_t n, uint64_t sub) { return fixed_mix(fixed_stream(n) + FIXED_GOLDEN * (sub + 1)); }

/* ---- SSBU_DEBUG_MATCH ------------------------------------------------------------------- */
/* "p1=<fighter>,p2=<fighter>|cpu|human,p3=..,p4=..,c1=<colour>..c4=<colour>,stage=<name>,
 * form=omega|battlefield|normal,hazards=on|off,cpu=<level>,items=on|off,stock=<n>,time=<min>"
 * (ssbu-recomp's debug match; c1..c4: a player's costume, 0-7, as ssbu-recomp's c1..c8). A
 * key left out keeps what it was (a list's lines add up); -1: not set. time is read and, as
 * there, not used. */
typedef struct DebugMatch {
    char name[4][32], stage[48];
    int p2type, form, hazards, cpu, items, time, stock, colour[4];
} DebugMatch;
static inline void dm_init(DebugMatch *m) {
    memset(m, 0, sizeof *m);
    m->p2type = m->form = m->hazards = m->cpu = m->items = m->time = m->stock = -1;
    for (int i = 0; i < 4; i++) m->colour[i] = -1;
}
static inline void dm_parse(DebugMatch *m, const char *text) {
    char buf[256];
    snprintf(buf, sizeof buf, "%s", text);
    for (char *t = buf, *next; t; t = next) {
        next = strchr(t, ',');
        if (next) *next++ = 0;
        char *eq = strchr(t, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = t, *v = eq + 1;
        if (!strcmp(k, "p2") && (!strcmp(v, "cpu") || !strcmp(v, "human"))) m->p2type = !strcmp(v, "cpu");
        else if ((k[0] == 'p') && k[1] >= '1' && k[1] <= '4' && !k[2]) snprintf(m->name[k[1] - '1'], sizeof m->name[0], "%s", v);
        else if (k[0] == 'c' && k[1] >= '1' && k[1] <= '4' && !k[2]) m->colour[k[1] - '1'] = atoi(v) & 7;
        else if (!strcmp(k, "stage")) snprintf(m->stage, sizeof m->stage, "%s", v);
        else if (!strcmp(k, "form")) m->form = !strcmp(v, "omega") ? 2 : !strcmp(v, "battlefield") ? 1 : 0;
        else if (!strcmp(k, "hazards")) m->hazards = !strcmp(v, "on");
        else if (!strcmp(k, "cpu")) m->cpu = atoi(v);
        else if (!strcmp(k, "items")) m->items = !strcmp(v, "on");
        else if (!strcmp(k, "time")) m->time = atoi(v);
        else if (!strcmp(k, "stock")) m->stock = atoi(v);
    }
}

/* A player's choice on the select screens (a panel state, 0x230 bytes taken) as the game's
 * record builder gets it: +5 rule kind (1 stock), +0xa stocks, +0x78 type (0 human, 1 CPU,
 * 3 none), +0x88 ui_chara id (0xc1 << 56 | hash40("ui_chara_<name>")), +0x90 colour, +0x91 CPU
 * level, +0x224 (all ones with a level). Changed before the game's function runs, so all it
 * derives is the game's. Players 3 and 4 copy player 2's state (as changed: p2) when the screens
 * left them none. Returns the type it was, as changed. */
enum { DM_STATE = 0x230 };
static inline void dm_chara(uint8_t *st, const char *name) {
    char nm[64];
    snprintf(nm, sizeof nm, "ui_chara_%s", name);
    fight_put64(st + 0x88, 0xc100000000000000ull | fight_hash40(nm));
}
static inline uint32_t dm_player(const DebugMatch *m, int idx, uint8_t *st, uint32_t type_was, const uint8_t *p2) {
    if (m->stock > 0 && type_was != 3 && st[5] == 1) st[0xa] = (uint8_t)m->stock;
    if ((idx == 2 || idx == 3) && m->name[idx][0] && p2) {
        if (type_was == 3 || fight_get32(st + 0x78) != 1) {
            memcpy(st, p2, DM_STATE);
            fight_put32(st + 0x78, 1);
        }
        dm_chara(st, m->name[idx]);
        st[0x90] = (uint8_t)idx;
        if (m->stock > 0 && st[5] == 1) st[0xa] = (uint8_t)m->stock;
        if (m->cpu > 0) { st[0x91] = (uint8_t)m->cpu; fight_put32(st + 0x224, 0xffffffffu); }
        type_was = 1;
    }
    if (idx >= 0 && idx < 2 && m->name[idx][0] && type_was != 3) {
        dm_chara(st, m->name[idx]);
        if (idx == 1 && m->p2type >= 0) fight_put32(st + 0x78, (uint32_t)m->p2type);
        if (m->cpu > 0 && fight_get32(st + 0x78) == 1) { st[0x91] = (uint8_t)m->cpu; fight_put32(st + 0x224, 0xffffffffu); }
    }
    if (idx >= 0 && idx < 4 && m->colour[idx] >= 0 && type_was != 3) st[0x90] = (uint8_t)m->colour[idx];
    return type_was;
}

/* The stage choice, which the game reads right after player 1's record: the current entry (+0
 * ui_stage id: 0x69 << 56 | hash40("ui_stage_<name>"), +8 form), the hazards byte, and the match
 * block's item appearance word (2 on, 0 off). fd and final_destination are end, battlefield is
 * battle_field. */
static inline void dm_stage(const DebugMatch *m, uint8_t *sel, uint8_t *hazards, uint8_t *items) {
    if (m->stage[0]) {
        const char *s = m->stage;
        char nm[80];
        if (!strcmp(s, "fd") || !strcmp(s, "final_destination")) s = "end";
        else if (!strcmp(s, "battlefield")) s = "battle_field";
        snprintf(nm, sizeof nm, "ui_stage_%s", s);
        fight_put64(sel, 0x6900000000000000ull | fight_hash40(nm));
    }
    if (m->form >= 0) fight_put32(sel + 8, (uint32_t)m->form);
    if (m->hazards >= 0) *hazards = (uint8_t)m->hazards;
    if (m->items >= 0) fight_put32(items, m->items ? 2u : 0u);
}

/* The next line of SSBU_DEBUG_MATCH_LIST at a new match set-up: a blank line keeps the match
 * as it was; at the end of the file the last one stays. Returns 1 when a line was taken. */
static inline int dm_next(DebugMatch *m, FILE *list) {
    char line[256];
    if (!list || !fgets(line, sizeof line, list)) return 0;
    line[strcspn(line, "\r\n")] = 0;
    if (!line[0]) return 0;
    dm_parse(m, line);
    return 1;
}

/* ---- SSBU_DEBUG_END --------------------------------------------------------------------- */
/* "<n>s": n seconds of fight, n * 60 frames; "<n>": n frames (an s right after the number, or
 * after spaces, makes it seconds; anything else after it is not read). 0: off (unset, under one
 * frame). More than a long long holds: as many as it holds, a run that does not end (rather
 * than a cast's undefined result, which on x86-64 ends it at once). */
static inline long long dm_end_parse(const char *text) {
    if (!text) return 0;
    char *end;
    double n = strtod(text, &end);
    while (*end == ' ') end++;
    if (*end == 's') n *= 60.0;
    if (!(n >= 1)) return 0;
    return n >= 9.2e18 ? LLONG_MAX : (long long)n;
}

/* One frame shown: the fight frames so far, counted while the game's ready-go flag is up and
 * from 1 again at each fight's start (0 while it is down; *ended 1 on the frame it went down). */
typedef struct DebugEnd { long long frames; int was_on; } DebugEnd;
static inline long long dm_end_frame(DebugEnd *d, int on, int *ended) {
    *ended = !on && d->was_on;
    if (!on) { d->was_on = 0; return 0; }
    if (!d->was_on) { d->was_on = 1; d->frames = 0; }
    return ++d->frames;
}
