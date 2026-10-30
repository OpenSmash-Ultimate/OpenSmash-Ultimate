/* Offline checks of the fight test switches' logic (runtime/fight_test.h, docs/TESTING-RUNS.md)
 * on made-up numbers and structures: the refresh report, the sequence's steps across a fight's
 * start and end, the hash line's length, the debug match's parsing, list and writes, and
 * SSBU_DEBUG_END's count. Plain C,
 * no runtime: any compiler.
 *
 *   clang -I runtime tools/dev/fighttest.c -o fighttest && ./fighttest
 *
 * Prints "<n> checks, 0 failed" and exits 0 when every check passes. "fighttest dump" prints the
 * numbers tests/test_fight_test.py works out on its own instead: the hash line and its FNV-1a,
 * the generators' fixed state, Hash40 and XXH64 values, and SSBU_FIXED_SEED's streams. */
#define _CRT_SECURE_NO_WARNINGS
#include <float.h>
#include <stdio.h>
#include <string.h>

#include "../../runtime/fight_test.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint64_t get64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

/* ---- SSBU_DETERMINISTIC ------------------------------------------------------------------ */
static void test_frame_end(void) {
    FightDrops d = {0};
    uint8_t count[8], result[4];
    fight_put64(count, 3); fight_put32(result, 0xF0044007u);
    fight_frame_end(&d, count, result, 0);
    CHECK(get64(count) == 0 && fight_get32(result) == 0, "a report of 3 dropped refreshes cleared, and its result");
    fight_put64(count, 2); fight_put32(result, 0xF0044005u);
    fight_frame_end(&d, count, result, 0);
    CHECK(get64(count) == 0 && fight_get32(result) == 0, "the other result code too");
    fight_put64(count, 1); fight_put32(result, 0x1234u);
    fight_frame_end(&d, count, result, 0);
    CHECK(get64(count) == 0 && fight_get32(result) == 0x1234u, "another result is left");
    fight_put64(count, 4); fight_put32(result, 0xF0044007u);
    fight_frame_end(&d, count, result, 1);
    CHECK(get64(count) == 4 && fight_get32(result) == 0xF0044007u, "SSBU_DET_OFF=4 leaves the report");
    fight_put64(count, 0); fight_put32(result, 0xF0044007u);
    fight_frame_end(&d, count, result, 0);
    CHECK(fight_get32(result) == 0xF0044007u, "no refresh dropped: nothing changed");
    fight_frame_end(&d, NULL, NULL, 0);
    CHECK(d.frame_ends == 6 && d.calls == 4 && d.frames == 10, "counted: %lld frame ends, %lld reports, %lld refreshes", d.frame_ends, d.calls, d.frames);
}

static void test_steps(void) {
    /* the first fighter's fight frame and the ready-go flag at each poll, and the ticks it gives */
    static const struct { long long frame; int on; long want; } POLLS[] = {
        {1, 0, 1},     /* the first battle frame, before "go" */
        {3, 1, 2},     /* the fight runs: two fight frames since the last poll */
        {3, 1, 0},     /* none: no tick */
        {4, 1, 1},
        {200000, 1, 100000},   /* (kept to 100000, as ssbu-recomp) */
        {200003, 0, 1},        /* the fight has ended: one a poll */
        {200003, 0, 1},
        {5, 0, 1},             /* a lower count after the end: still one */
        {7, 1, 2},             /* the next fight: fight frames again */
        {6, 1, 0},             /* a lower count during a fight: none */
    };
    FightSteps s = {0};
    for (unsigned i = 0; i < sizeof POLLS / sizeof *POLLS; i++) {
        long got = fight_steps(&s, POLLS[i].frame, POLLS[i].on);
        CHECK(got == POLLS[i].want, "poll %u (frame %lld, ready-go %d): %ld ticks, wanted %ld", i + 1, POLLS[i].frame, POLLS[i].on, got, POLLS[i].want);
    }
    FightSteps t = {0};
    fight_steps(&t, -2, 0);   /* (before any fight: a lower count is no tick, counted as 0) */
    CHECK(t.hist[0] == 1, "a poll with no new frame is counted");
}

/* ---- SSBU_FIGHT_HASH --------------------------------------------------------------------- */
static void test_line_length(void) {
    /* every number at its widest: the longest line there is */
    FightFrame wide = {-9223372036854775807ll - 1, -2147483647 - 1, -1, ~0ull, -FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX, 4294967295u, ~0ull};
    char line[FIGHT_LINE_MAX + 1], cut[320];
    uint64_t run = FIGHT_FNV_BASIS, run2 = FIGHT_FNV_BASIS;
    int n = fight_hash_line(line, sizeof line, &run, &wide);
    CHECK(n == FIGHT_LINE_MAX && strlen(line) == FIGHT_LINE_MAX && line[n - 1] == '\n', "the widest line: %d bytes, whole", n);
    n = fight_hash_line(cut, sizeof cut, &run2, &wide);
    CHECK(n == (int)sizeof cut - 1 && strlen(cut) == sizeof cut - 1 && !memcmp(cut, line, sizeof cut - 1) && run2 == run,
          "a buffer too small: the length written (%d), not the length wanted", n);
    CHECK(fight_hash_line(cut, 0, &run2, &wide) == 0, "no buffer: nothing written");
}

/* ---- SSBU_DEBUG_MATCH -------------------------------------------------------------------- */
static void test_parse(void) {
    DebugMatch m;
    dm_init(&m);
    CHECK(m.p2type == -1 && m.form == -1 && m.items == -1 && m.stock == -1 && !m.name[0][0] && !m.stage[0], "nothing set");
    dm_parse(&m, "p1=mario,p2=link,p2=cpu,p3=kirby,p4=fox,stage=fd,form=omega,hazards=off,cpu=9,items=on,stock=2,time=3");
    CHECK(!strcmp(m.name[0], "mario") && !strcmp(m.name[1], "link") && !strcmp(m.name[2], "kirby") && !strcmp(m.name[3], "fox"), "the four fighters");
    CHECK(m.p2type == 1 && !strcmp(m.stage, "fd") && m.form == 2 && m.hazards == 0 && m.cpu == 9 && m.items == 1 && m.stock == 2 && m.time == 3,
          "the rest: p2type %d stage %s form %d hazards %d cpu %d items %d stock %d time %d", m.p2type, m.stage, m.form, m.hazards, m.cpu, m.items, m.stock, m.time);
    dm_parse(&m, "p2=human,form=battlefield,hazards=on,items=off,,junk,p5=x");
    CHECK(m.p2type == 0 && m.form == 1 && m.hazards == 1 && m.items == 0 && !strcmp(m.name[0], "mario"), "a second text adds to the first; junk is ignored");
    dm_parse(&m, "form=normal");
    CHECK(m.form == 0, "any other form is the normal one");
}

static void test_player(void) {
    DebugMatch m;
    uint8_t st[4][DM_STATE];
    memset(st, 0, sizeof st);
    for (int i = 0; i < 4; i++) { st[i][5] = 1; st[i][0xa] = 3; fight_put64(st[i] + 0x88, 0x1111); }
    fight_put32(st[1] + 0x78, 0);
    fight_put32(st[2] + 0x78, 3);
    fight_put32(st[3] + 0x78, 3);
    st[1][0x40] = 0x5a;   /* something of player 2's own, which player 3 copies */
    dm_init(&m);
    dm_parse(&m, "p1=mario,p2=link,p2=cpu,p3=kirby,cpu=7,stock=2");
    CHECK(dm_player(&m, 0, st[0], 0, NULL) == 0, "player 1: the type it was");
    CHECK(get64(st[0] + 0x88) == (0xc100000000000000ull | fight_hash40("ui_chara_mario")) && st[0][0xa] == 2, "player 1: the fighter and the stocks");
    CHECK(fight_get32(st[0] + 0x78) == 0 && st[0][0x91] == 0, "player 1: still a human, no level");
    dm_player(&m, 1, st[1], 0, NULL);
    CHECK(get64(st[1] + 0x88) == (0xc100000000000000ull | fight_hash40("ui_chara_link")), "player 2's fighter");
    CHECK(fight_get32(st[1] + 0x78) == 1 && st[1][0x91] == 7 && fight_get32(st[1] + 0x224) == 0xffffffffu, "player 2 made a CPU of level 7");
    CHECK(dm_player(&m, 2, st[2], 3, st[1]) == 1, "player 3 had no panel: a CPU now");
    CHECK(st[2][0x40] == 0x5a && fight_get32(st[2] + 0x78) == 1 && st[2][0x90] == 2, "player 3: player 2's state, colour 2");
    CHECK(get64(st[2] + 0x88) == (0xc100000000000000ull | fight_hash40("ui_chara_kirby")) && st[2][0x91] == 7 && st[2][0xa] == 2, "player 3's fighter, level, stocks");
    uint8_t before[DM_STATE];
    memcpy(before, st[3], DM_STATE);
    dm_player(&m, 3, st[3], 3, st[1]);
    CHECK(!memcmp(before, st[3], DM_STATE), "player 4: not asked for, left as it was");
    uint8_t none[DM_STATE];
    memset(none, 0, sizeof none);
    fight_put32(none + 0x78, 3);
    none[5] = 1;
    dm_player(&m, 0, none, 3, NULL);
    CHECK(get64(none + 0x88) == 0 && none[0xa] == 0, "player 1 with no panel: left alone");
    DebugMatch only;
    dm_init(&only);
    dm_parse(&only, "p2=cpu");
    memcpy(before, st[1], DM_STATE);
    dm_player(&only, 1, st[1], 1, NULL);
    CHECK(!memcmp(before, st[1], DM_STATE), "p2=cpu alone changes nothing (as in ssbu-recomp: only with p2's fighter)");
    DebugMatch col;   /* c1..c4: a player's costume */
    dm_init(&col);
    dm_parse(&col, "p1=dedede,c1=0,p2=dedede,c2=4,p3=kirby,c3=6");
    CHECK(col.colour[0] == 0 && col.colour[1] == 4 && col.colour[2] == 6 && col.colour[3] == -1, "c1..c4 parsed, c4 not set");
    st[0][0x90] = 5;
    dm_player(&col, 0, st[0], 0, NULL);
    CHECK(st[0][0x90] == 0, "player 1: colour 0");
    dm_player(&col, 1, st[1], 0, NULL);
    CHECK(st[1][0x90] == 4, "player 2: colour 4");
    fight_put32(st[2] + 0x78, 3);
    dm_player(&col, 2, st[2], 3, st[1]);
    CHECK(st[2][0x90] == 6, "player 3, made from player 2's state: its own colour 6");
    memset(none, 0, sizeof none);
    fight_put32(none + 0x78, 3);
    dm_player(&col, 0, none, 3, NULL);
    CHECK(none[0x90] == 0 && get64(none + 0x88) == 0, "a player with no panel: no colour either");
}

static void test_stage(void) {
    DebugMatch m;
    uint8_t sel[0x20], hazards = 7, items[4];
    memset(sel, 0xee, sizeof sel);
    fight_put32(items, 0x55);
    dm_init(&m);
    dm_stage(&m, sel, &hazards, items);
    CHECK(sel[0] == 0xee && hazards == 7 && fight_get32(items) == 0x55, "nothing asked: nothing changed");
    dm_parse(&m, "stage=fd,form=omega,hazards=off,items=on");
    dm_stage(&m, sel, &hazards, items);
    CHECK(get64(sel) == (0x6900000000000000ull | fight_hash40("ui_stage_end")) && fight_get32(sel + 8) == 2, "fd is end, omega form");
    CHECK(hazards == 0 && fight_get32(items) == 2 && sel[0xc] == 0xee, "hazards off, items on (2), the rest left");
    dm_parse(&m, "stage=battlefield,items=off");
    dm_stage(&m, sel, &hazards, items);
    CHECK(get64(sel) == (0x6900000000000000ull | fight_hash40("ui_stage_battle_field")) && fight_get32(items) == 0, "battlefield is battle_field; items off");
    dm_parse(&m, "stage=final_destination");
    dm_stage(&m, sel, &hazards, items);
    CHECK(get64(sel) == (0x6900000000000000ull | fight_hash40("ui_stage_end")), "final_destination is end too");
    dm_parse(&m, "stage=pictochat2");
    dm_stage(&m, sel, &hazards, items);
    CHECK(get64(sel) == (0x6900000000000000ull | fight_hash40("ui_stage_pictochat2")), "any other name as it is");
}

static void test_list(void) {
    FILE *f = tmpfile();
    const char *named = NULL;
    if (!f) f = fopen(named = "fighttest-list.txt", "w+");   /* (where tmpfile has no folder it may write to) */
    CHECK(f != NULL, "a temporary file");
    if (!f) return;
    fputs("p1=mario,stage=battlefield\n\r\np1=link,items=off\r\n", f);
    rewind(f);
    DebugMatch m;
    dm_init(&m);
    CHECK(dm_next(&m, f) == 1 && !strcmp(m.name[0], "mario") && !strcmp(m.stage, "battlefield"), "the first match");
    CHECK(dm_next(&m, f) == 0 && !strcmp(m.name[0], "mario"), "a blank line: the same match");
    CHECK(dm_next(&m, f) == 1 && !strcmp(m.name[0], "link") && !strcmp(m.stage, "battlefield") && m.items == 0, "the third: what it says, the rest kept");
    CHECK(dm_next(&m, f) == 0 && !strcmp(m.name[0], "link"), "the end of the list: the last match stays");
    CHECK(dm_next(&m, NULL) == 0, "no list");
    fclose(f);
    if (named) remove(named);
}

/* ---- SSBU_DEBUG_END ---------------------------------------------------------------------- */
static void test_end(void) {
    CHECK(dm_end_parse("2s") == 120 && dm_end_parse("1.5s") == 90 && dm_end_parse("90") == 90, "seconds of fight are 60 frames each");
    CHECK(dm_end_parse(NULL) == 0 && dm_end_parse("") == 0 && dm_end_parse("0") == 0 && dm_end_parse("-3s") == 0 && dm_end_parse("x") == 0 &&
          dm_end_parse("0.5") == 0 && dm_end_parse("nan") == 0, "off");
    CHECK(dm_end_parse("5 frames") == 5 && dm_end_parse("5 s") == 300 && dm_end_parse("5sec") == 300, "seconds: an s right after the number");
    CHECK(dm_end_parse("inf") == LLONG_MAX && dm_end_parse("1e30") == LLONG_MAX && dm_end_parse("1e19s") == LLONG_MAX &&
          dm_end_parse("9.3e18") == LLONG_MAX && dm_end_parse("1e18") == (long long)1e18, "past a long long: a run that does not end");
    DebugEnd d = {0};
    int ended;
    CHECK(dm_end_frame(&d, 0, &ended) == 0 && !ended, "no fight yet");
    CHECK(dm_end_frame(&d, 1, &ended) == 1 && dm_end_frame(&d, 1, &ended) == 2 && dm_end_frame(&d, 1, &ended) == 3 && !ended, "fight frames 1, 2, 3");
    CHECK(dm_end_frame(&d, 0, &ended) == 0 && ended && d.frames == 3, "the fight ends after 3 (kept for the log)");
    CHECK(dm_end_frame(&d, 0, &ended) == 0 && !ended, "and stays ended");
    CHECK(dm_end_frame(&d, 1, &ended) == 1, "the next fight counts from 1");
}

/* ---- SSBU_FIXED_SEED --------------------------------------------------------------------- */
static void test_seed(void) {
    uint64_t n = 99;
    CHECK(fixed_seed_parse("42", &n) && n == 42, "42");
    CHECK(fixed_seed_parse("0x10", &n) && n == 16, "hex");
    CHECK(fixed_seed_parse("0", &n) && n == 0, "0 is a seed too");
    CHECK(!fixed_seed_parse("", &n) && !fixed_seed_parse(NULL, &n), "unset or empty: off");
    uint64_t a = fixed_stream(1), b = fixed_stream(1);
    uint8_t x[13], y[16];
    fixed_fill(&a, x, sizeof x);
    fixed_fill(&b, y, sizeof y);
    CHECK(!memcmp(x, y, sizeof x), "the same seed, the same bytes");
    CHECK(a == b, "13 bytes take two steps, as 16 do");
    uint64_t c = fixed_stream(2);
    fixed_fill(&c, y, 8);
    CHECK(memcmp(x, y, 8) != 0, "another seed, other bytes");
    CHECK(fixed_entropy(5, 0) == fixed_entropy(5, 0) && fixed_entropy(5, 0) != fixed_entropy(5, 1) && fixed_entropy(5, 0) != fixed_entropy(6, 0),
          "the entropy: one number for each seed and sub-id");
}

/* ---- the numbers tests/test_fight_test.py works out on its own ----------------------------- */
static void hex(const void *p, size_t n) {
    for (size_t i = 0; i < n; i++) printf("%02x", ((const uint8_t *)p)[i]);
}
static int dump(void) {
    uint32_t w[FIGHT_RNG_WORDS];
    fight_rng_fixed(w);
    printf("rng_fixed");
    for (int i = 0; i < FIGHT_RNG_WORDS; i++) printf(" %08x", w[i]);
    printf("\n");
    for (int i = 0; i < FIGHT_RNG_WORDS; i++) w[i] = 0x01010101u * (uint32_t)i + 7;
    uint32_t calls;
    uint64_t rh, run = FIGHT_FNV_BASIS;
    fight_rng_sum(w, &calls, &rh);
    printf("rng_sum %u %016llx\n", calls, (unsigned long long)rh);
    fight_rng_sum(NULL, &calls, &rh);
    printf("rng_none %u %016llx\n", calls, (unsigned long long)rh);
    fight_rng_sum(w, &calls, &rh);
    FightFrame frames[] = {{1, 0, 0x1a, 0x123456789abcull, 1.5f, -12.25f, 30.0f, 0.0f, calls, rh},
                           {1, 1, 0x0, 0x0ull, 0.0f, 3.140625f, -0.5f, 12.5f, 0, FIGHT_FNV_BASIS},
                           {2, -1, 0x7fffffff, ~0ull, -1.0f, 100000.0f, 1e-3f, 999.9f, 4000000000u, 0xfedcba9876543210ull}};
    for (unsigned i = 0; i < sizeof frames / sizeof *frames; i++) {
        char line[320];
        fight_hash_line(line, sizeof line, &run, &frames[i]);
        printf("line %s", line);
    }
    static const char *const NAMES[] = {"", "a", "ui_chara_mario", "ui_stage_end", "ui_stage_battle_field"};
    for (unsigned i = 0; i < sizeof NAMES / sizeof *NAMES; i++) printf("hash40 '%s' %016llx\n", NAMES[i], (unsigned long long)fight_hash40(NAMES[i]));
    static const char *const DATA[] = {"", "a", "abc", "Nobody inspects the spammish repetition",
                                       "FIGHTER_INSTANCE_WORK_ID_INT_ENTRY_ID", "0123456789abcdef0123456789abcdef!"};
    for (unsigned i = 0; i < sizeof DATA / sizeof *DATA; i++)
        for (int s = 0; s < 2; s++) printf("xxh64 %d '%s' %016llx\n", s, DATA[i], (unsigned long long)fight_xxh64(DATA[i], strlen(DATA[i]), s ? ~0ull : 0));
    static const uint64_t SEEDS[] = {1, 42, 0};
    for (unsigned i = 0; i < sizeof SEEDS / sizeof *SEEDS; i++) {
        uint8_t b[21];
        uint64_t st = fixed_stream(SEEDS[i]);
        fixed_fill(&st, b, sizeof b);
        printf("fill %llu ", (unsigned long long)SEEDS[i]);
        hex(b, sizeof b);
        fixed_fill(&st, b, 3);
        printf(" ");
        hex(b, 3);
        printf("\nentropy %llu %016llx %016llx\n", (unsigned long long)SEEDS[i], (unsigned long long)fixed_entropy(SEEDS[i], 0),
               (unsigned long long)fixed_entropy(SEEDS[i], 3));
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "dump")) return dump();
    test_frame_end();
    test_steps();
    test_line_length();
    test_parse();
    test_player();
    test_stage();
    test_list();
    test_end();
    test_seed();
    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
