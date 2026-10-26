/* Offline checks of the test-run tools (docs/TESTING-RUNS.md), on made-up sequences and numbers:
 * SSBU_SEQ's parser and step machine (runtime/hle_input_seq.c), the arithmetic of SSBU_SPEED and
 * SSBU_SKIP_MOVIES (runtime/testrun.h) and the game address table's mapping (runtime/game_addr.h).
 * Plain C, no runtime: any compiler.
 *
 *   clang -I runtime tools/dev/testruntest.c runtime/hle_input_seq.c -o testruntest && ./testruntest
 *
 * Prints "<n> checks, 0 failed" and exits 0 when every check passes; "testruntest <file>" only
 * reads the sequence in the file. tests/test_test_runs.py builds and runs it. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <string.h>

#include "../../runtime/game_addr.h"
#include "../../runtime/input_seq.h"
#include "../../runtime/testrun.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

enum { A = 1 << 0, B = 1 << 1, X = 1 << 2, ZL = 1 << 8, PLUS = 1 << 10, UP = 1 << 13 };
static Seq s;

/* ---- the parser ------------------------------------------------------------------------ */
static void bad(const char *text, const char *want) {
    char why[200] = "";
    int n = seq_parse(&s, text, why, sizeof why);
    CHECK(n == -1 && strstr(why, want), "'%s': %d, '%s' (wanted '%s')", text, n, why, want);
}

static void test_parse(void) {
    char why[200];
    CHECK(seq_parse(&s, "", why, sizeof why) == 0 && seq_finished(&s), "an empty sequence");
    CHECK(seq_parse(&s, NULL, why, sizeof why) == 0, "no sequence");
    CHECK(seq_parse(&s, "  # only a comment\n\t", why, sizeof why) == 0, "only a comment");
    int n = seq_parse(&s, "@movie 90:A 120:ZL+PLUS/3 # a comment: 5:B\n@draws:10-20 @draws:5-5/1 30 "
                          "@until:40-60/4:A/50 @untilmark:module:m_1:X/30/2 @untilmark:m:UP @module:m_1 7:SLEFT+SUP",
                      why, sizeof why);
    CHECK(n == 11, "the steps of a long sequence: %d (%s)", n, n < 0 ? why : "");
    if (n != 11) return;
    SeqStep *st = s.steps;
    CHECK(st[0].kind == SEQ_MARK && !strcmp(st[0].mark, "movie"), "@movie");
    CHECK(st[1].kind == SEQ_KEYS && st[1].wait == 90 && st[1].hold == SEQ_HOLD && st[1].buttons == A, "90:A");
    CHECK(st[2].wait == 120 && st[2].hold == 3 && st[2].buttons == (ZL | PLUS), "120:ZL+PLUS/3");
    CHECK(st[3].kind == SEQ_DRAWS && st[3].lo == 10 && st[3].hi == 20 && st[3].need == SEQ_RUN, "@draws:10-20 (the comment's 5:B not a step)");
    CHECK(st[4].lo == 5 && st[4].hi == 5 && st[4].need == 1, "@draws:5-5/1");
    CHECK(st[5].kind == SEQ_KEYS && st[5].wait == 30 && st[5].hold == 0 && !st[5].buttons, "30");
    CHECK(st[6].kind == SEQ_UNTIL && st[6].lo == 40 && st[6].hi == 60 && st[6].need == 4 && st[6].buttons == A && st[6].wait == 50 && st[6].hold == SEQ_HOLD,
          "@until:40-60/4:A/50");
    CHECK(st[7].kind == SEQ_UNTILMARK && !strcmp(st[7].mark, "module:m_1") && st[7].buttons == X && st[7].wait == 30 && st[7].hold == 2,
          "@untilmark with a mark holding ':'");
    CHECK(st[8].wait == SEQ_PERIOD && st[8].hold == SEQ_HOLD && st[8].buttons == UP, "@untilmark's defaults");
    CHECK(st[9].kind == SEQ_MARK && !strcmp(st[9].mark, "module:m_1"), "@module:m_1");
    CHECK(st[10].lx == -32767 && st[10].ly == 32767 && !st[10].buttons, "the stick");
    CHECK(!strcmp(st[2].text, "120:ZL+PLUS/3"), "a step's text");

    bad("5:Q", "step 1 (5:Q): unknown key");
    bad("10 5:A+", "step 2 (5:A+): expected a key");
    bad("5:A/0", "hold of 1 or more");
    bad("5:A/", "hold of 1 or more");
    bad("x", "expected a number of ticks");
    bad("5x", "unexpected text");
    bad("5:A/3/4", "unexpected text");
    bad("5:SLEFT+SRIGHT", "two opposite ways");
    bad("@", "a mark's name");
    bad("@draws", "expected ':'");
    bad("@draws:20-10", "low end is above");
    bad("@draws:11-10", "low end is above");
    bad("@draws:10", "expected <lo>-<hi>");
    bad("@draws:1-2/0", "count of 1 or more");
    bad("@until:1-2:A/8", "period must be longer");
    bad("@until:1-2", "expected ':' and the keys");
    bad("@untilmark:m:A/10/10", "period must be longer");
    bad("@untilmark:A", "expected @untilmark:<mark>:<keys>");
    bad("99999999999", "expected a number of ticks");
    bad("@a_mark_whose_name_runs_on_far_too_long_for_a_step", "longer than 47");
    char many[SEQ_STEPS * 2 + 8] = "";
    for (int i = 0; i <= SEQ_STEPS; i++) strcat(many, "1 ");
    bad(many, "more than 256 steps");
}

/* ---- the step machine ------------------------------------------------------------------ */
static const char *raised[8];
static int seen(const char *mark, void *user) {
    (void)user;
    for (int i = 0; i < 8 && raised[i]; i++) if (!strcmp(raised[i], mark)) return 1;
    return 0;
}

/* Ticks the sequence n times with that draw count; the keys of each tick in keys[] (if given). */
static int lines;
static void run(int n, long draws, uint64_t *keys) {
    for (int i = 0; i < n; i++) {
        lines += seq_tick(&s, draws, seen, NULL);
        if (keys) keys[i] = s.buttons;
    }
}

static void parse(const char *text) {
    char why[200];
    int n = seq_parse(&s, text, why, sizeof why);
    CHECK(n >= 0, "'%s' did not parse: %s", text, why);
    memset(raised, 0, sizeof raised);
    lines = 0;
}

static void test_holds(void) {
    uint64_t k[20];
    parse("0:A/3 2:B");
    run(14, -1, k);
    int ok = 1;
    for (int t = 0; t < 14; t++) ok &= k[t] == (t < 3 ? A : t < 5 ? 0 : t < 13 ? B : 0);
    CHECK(ok, "0:A/3 2:B: A for ticks 1-3, nothing for 4-5, B for 6-13, then nothing");
    CHECK(seq_finished(&s) && lines == 2, "and then ended, having said 2 lines (%d)", lines);

    parse("4 1:X/1 0 0 2:A/2");
    run(10, -1, k);
    CHECK(!k[0] && !k[3] && !k[4] && k[5] == X && !k[6] && !k[7] && k[8] == A && k[9] == A, "waits and holds of one tick; steps of none pass over");
    CHECK(seq_finished(&s), "ended on the last tick of its last hold");
    run(3, -1, k);
    CHECK(!k[0] && !k[1] && !k[2] && s.ticks == 13, "nothing after the end");

    parse("1:SUP/2");
    run(2, -1, NULL);
    CHECK(s.ly == 32767 && s.lx == 0 && !s.buttons, "a stick hold");
    run(2, -1, NULL);
    CHECK(s.ly == 0, "the stick let go");

    parse("2:A");
    run(3, -1, NULL);
    CHECK(strstr(s.said, "keys for 8 ticks") && strstr(s.said, "step 1 (2:A)") && strstr(s.said, "tick 3"), "what a press says: '%s'", s.said);
}

static void test_waits(void) {
    uint64_t k[40];
    parse("@go 0:A/1");
    run(5, -1, k);
    CHECK(!k[4] && !seq_finished(&s), "@go: waits for the mark");
    raised[0] = "go";
    run(2, -1, k);
    CHECK(!k[0] && k[1] == A, "the mark seen on one tick, the next step on the tick after");

    parse("@draws:10-12/3 0:A/1");
    long draws[] = {-1, 11, 12, 30, 10, 11, 12, 0};
    for (int i = 0; i < 8; i++) run(1, draws[i], &k[i]);
    CHECK(!k[0] && !k[5] && !k[6] && k[7] == A, "@draws: 3 ticks in a row in range, the run broken by 30 first");

    /* @until: presses at once, every 12, until 3 ticks in a row in range; a press is not cut short */
    parse("@until:5-5/3:B/12 0:A/1");
    run(30, 0, k);
    int ok = 1;
    for (int t = 0; t < 30; t++) ok &= k[t] == (t % 12 < 8 ? B : 0);
    CHECK(ok, "@until: B for 8 ticks every 12 while out of range");
    run(2, 5, k);   /* ticks 31-32: in range, but the press from tick 25 holds to 32 */
    CHECK(k[0] == B && k[1] == B, "a press under way is finished");
    run(1, 5, k);   /* tick 33: 3 in a row, no press */
    CHECK(!k[0] && strstr(s.said, "5..5 draw calls for 3 ticks"), "then the wait ends (%s)", s.said);
    run(1, 5, k);
    CHECK(k[0] == A, "and the next step starts");

    parse("@until:5-5/4:B/12");
    run(1, 0, k);
    run(4, 5, k);   /* in range for 4 ticks, but the press runs to tick 8 */
    CHECK(!seq_finished(&s), "@until: not ended inside a press");
    run(10, 0, k);  /* ticks 6-15, out of range again: the press ends after 8, the next at 13 */
    CHECK(k[2] == B && !k[3] && k[7] == B, "@until: the next press at its period");

    parse("@until:5-5/3:B/10");
    run(1, 5, k);
    CHECK(!k[0], "@until: no press at once when the count is already in range");
    run(2, 5, k);
    CHECK(!k[0] && !k[1] && seq_finished(&s), "@until: and the wait ends after 3 ticks, with no press");

    parse("@until:5-5/4:B/10 0:A/1");
    run(10, 0, k);   /* ticks 1-10: B from 1 to 8 */
    run(3, 5, k);    /* ticks 11-13: the next press is due at 11, but the count is in range (not yet for 4) */
    CHECK(!k[0] && !k[1] && !k[2], "@until: no press due while the count is in range");
    run(1, 0, k);    /* tick 14: out of range again */
    CHECK(k[0] == B, "@until: the press due comes on the first tick out of range");
    run(7, 0, k);    /* ticks 15-21: the rest of it */
    CHECK(k[6] == B, "@until: that press held for 8 ticks");
    run(4, 5, k);    /* ticks 22-25: in range for 4 (a press due at 24 does not come) */
    CHECK(!k[0] && !k[1] && !k[2] && !k[3] && s.index == 1, "@until: no press due at 24, and the wait ends at 25");
    run(1, 5, k);
    CHECK(k[0] == A, "and the next step starts at 26");

    parse("@untilmark:ok:X/10/2 0:A/1");
    run(25, -1, k);
    ok = 1;
    for (int t = 0; t < 25; t++) ok &= k[t] == (t % 10 < 2 ? X : 0);
    CHECK(ok, "@untilmark: X for 2 ticks every 10");
    raised[0] = "ok";
    run(2, -1, k);   /* tick 26 between presses: ends; tick 27: the next step */
    CHECK(!k[0] && k[1] == A, "@untilmark: ends when the mark is seen between presses");

    parse("@untilmark:ok:X");
    raised[0] = "ok";
    run(1, -1, k);
    CHECK(!k[0] && seq_finished(&s), "@untilmark: a mark seen already, no press");
}

/* ---- one poll's ticks: the steps hook ---------------------------------------------------- */
static int answer;
static int hook(void) { return answer; }
static int logged;
static void count_lines(const char *line, void *user) { (void)line; (void)user; logged++; }

static void test_polls(void) {
    parse("2:A/4 0:B/1");
    CHECK(seq_poll(&s, NULL, -1, seen, count_lines, NULL) == 1 && s.ticks == 1, "no hook: one tick a poll");
    answer = 0;
    CHECK(seq_poll(&s, hook, -1, seen, count_lines, NULL) == 0 && s.ticks == 1, "a poll of 0 ticks");
    answer = 2;
    seq_poll(&s, hook, -1, seen, count_lines, NULL);   /* ticks 2-3: A from 3 */
    CHECK(s.buttons == A && logged == 1, "2 ticks: the keys of the last");
    answer = 0;
    seq_poll(&s, hook, -1, seen, count_lines, NULL);
    CHECK(s.buttons == A, "a poll of 0 ticks keeps the keys the poll before gave");
    answer = 4;
    seq_poll(&s, hook, -1, seen, count_lines, NULL);   /* ticks 4-7: A to 6, B on 7 */
    CHECK(s.buttons == B && seq_finished(&s) && logged == 2, "4 ticks across two steps: B, the end, 2 lines (%d)", logged);
    answer = -3;
    CHECK(seq_poll(&s, hook, -1, seen, NULL, NULL) == 0, "a negative answer: 0 ticks");
    answer = 1 << 30;
    long long before = s.ticks;
    CHECK(seq_poll(&s, hook, -1, seen, NULL, NULL) == SEQ_MAX_TICKS && s.ticks == before + SEQ_MAX_TICKS, "a huge answer: at most SEQ_MAX_TICKS");
    CHECK(!s.buttons, "nothing after the end");
}

/* ---- SSBU_SPEED, SSBU_SKIP_MOVIES -------------------------------------------------------- */
static void test_speed(void) {
    double v;
    CHECK(speed_parse(NULL, &v) && v == 1, "unset: 1");
    CHECK(speed_parse("", &v) && v == 1, "empty: 1");
    CHECK(speed_parse("4", &v) && v == 4, "4");
    CHECK(speed_parse("0", &v) && v == 0, "0: uncapped");
    CHECK(speed_parse("0.5", &v) && v == 0.5, "0.5");
    CHECK(speed_parse("1", &v) && v == 1, "1");
    CHECK(!speed_parse("fast", &v) && v == 1, "words: no, and 1");
    CHECK(!speed_parse("-2", &v) && !speed_parse("2x", &v) && !speed_parse("nan", &v) && !speed_parse("inf", &v) && !speed_parse("5000", &v), "the rest: no");
    CHECK(speed_due(0, 1000, 10) == 1010, "the first refresh a step from now");
    CHECK(speed_due(1010, 1005, 10) == 1020, "a step after the last, when early");
    CHECK(speed_due(1020, 1055, 10) == 1030, "up to four steps behind: still one step on (catching up)");
    CHECK(speed_due(1020, 1070, 10) == 1030, "exactly four steps behind: still one step on");
    CHECK(speed_due(1030, 1100, 10) == 1100, "further behind: from now, no burst");
}

static void test_skip(void) {
    long keep;
    CHECK(skip_movies_parse(NULL, &keep) && keep == 0, "unset: off");
    CHECK(skip_movies_parse("0", &keep) && keep == 0, "0: off");
    CHECK(skip_movies_parse("1", &keep) && keep == 12, "1: 12");
    CHECK(skip_movies_parse("120", &keep) && keep == 120, "120");
    CHECK(!skip_movies_parse("yes", &keep) && keep == 0 && !skip_movies_parse("-1", &keep) && !skip_movies_parse("3s", &keep), "the rest: no, and off");

    static SkipMovies m;
    int first, n = 0;
    m.keep = 3;
    skip_open(&m, 0x1000);
    while (!skip_ended(&m, 0x1000, &first) && n < 10) { skip_read(&m, 0x1000); n++; }
    CHECK(n == 3 && first, "3 samples, then the end, said once");
    CHECK(skip_ended(&m, 0x1000, &first) && !first, "the end stays, said once");
    CHECK(!skip_ended(&m, 0x2000, &first), "an extractor never opened is left alone");
    CHECK(skip_seek(&m, 0x1000) == 1, "a seek after the end says so");
    n = 0;
    while (!skip_ended(&m, 0x1000, &first) && n < 10) { skip_read(&m, 0x1000); n++; }
    CHECK(n == 3 && first, "after a seek, 3 samples again");
    skip_open(&m, 0x1000);
    CHECK(!skip_ended(&m, 0x1000, &first), "a new stream on the same extractor starts again");
    skip_read(&m, 0x1000);
    CHECK(skip_seek(&m, 0x1000) == 0 && !skip_ended(&m, 0x1000, &first), "a seek before the end: from there");
    for (unsigned i = 0; i < SKIP_STREAMS + 2; i++) skip_open(&m, 0x5000 + i);
    CHECK(skip_find(&m, 0x5000 + SKIP_STREAMS + 1) && !skip_find(&m, 0x5000), "the oldest stream's place taken when all are in use");
    m.keep = 0;
    skip_open(&m, 0x9000);
    for (int i = 0; i < 100; i++) skip_read(&m, 0x9000);
    CHECK(!skip_ended(&m, 0x9000, &first), "off: never ends");
}

/* ---- the game address table ------------------------------------------------------------ */
static void test_game_addr(void) {
    CHECK(GAME_ADDR(FRAME_END) == 0x71038701e0ull && GAME_ADDR(MATCH_BLOCK) == 0x71052c51b0ull, "GAME_ADDR");
    int ok = 1;
    for (int i = 0; i < GA_COUNT; i++) ok &= game_addr_in(GAME_13_0_5, game_addresses[i].at_13_0_4) == game_addresses[i].at_13_0_5;
    CHECK(ok, "every row: its 13.0.4 address maps to its 13.0.5 one");
    CHECK(game_addr_in(GAME_13_0_4, 0x7101788260ull) == 0x7101788260ull, "13.0.4: itself");
    CHECK(game_addr_in(GAME_13_0_5, 0x7104000000ull) == 0x7104001000ull, "data: a page later");
    CHECK(game_addr_in(GAME_13_0_5, 0x71039c8000ull) == 0x71039c9000ull, "the first byte of read-only data");
    CHECK(game_addr_in(GAME_13_0_5, 0x71039c7ffcull) == 0, "code not in the table: unknown (0)");
    CHECK(game_addr_in(GAME_13_0_5, 0x7108001234ull) == 0x7108001234ull, "subsdk0: itself");
    CHECK(game_addr_in(GAME_13_0_5, 0x7107500000ull) == 0x7107500000ull, "past main: itself");
    CHECK(game_addr(0x71052c41b0ull + 0x588) == 0x71052c51b0ull + 0x588 && game_addr(0x7101788260ull) == 0x71017880c0ull, "game_addr: 13.0.5's");
}

/* testruntest <file>: only reads the sequence in the file (the documentation's example). */
static int parse_file(const char *path) {
    static char text[65536];
    FILE *f = fopen(path, "rb");
    size_t n = f ? fread(text, 1, sizeof text - 1, f) : 0;
    if (f) fclose(f);
    text[n] = 0;
    char why[200];
    int steps = seq_parse(&s, text, why, sizeof why);
    if (steps < 0) printf("%s: %s\n", path, why);
    else printf("%d steps\n", steps);
    return steps < 1;
}

int main(int argc, char **argv) {
    if (argc > 1) return parse_file(argv[1]);
    test_parse();
    test_holds();
    test_waits();
    test_polls();
    test_speed();
    test_skip();
    test_game_addr();
    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
