/* Offline checks of a pair's path choice (runtime/net_path.c: our direct path first, Epic's P2P when the direct one
 * is dead or clearly slower, hysteresis, no move mid-fight) and of reading the lobby's /gsp/records answer
 * (runtime/net_results.c). tests/test_eos_paths.py builds and runs it; exit code 0 when every check passes. */
#include <stdio.h>
#include <string.h>

#include "../../runtime/net_path.h"
#include "../../runtime/net_results.h"

/* (what net_results.c reaches outside itself; never called here) */
int account_nsa_hex(char out[17]) { out[0] = 0; return 0; }
size_t online_results_snapshot(char *out, size_t cap) { (void)out; (void)cap; return 0; }
void online_results_fetched(const uint8_t *records, int n) { (void)records; (void)n; }

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* both paths answering every 250 ms with these round trips from `from` to `to` (ms); the path at the end */
static int run(PathPick *p, uint64_t from, uint64_t to, double direct, double eos, int fight) {
    int cur = p->cur;
    for (uint64_t t = from; t <= to; t += 250) {
        if (direct > 0) path_answer(p, PATH_DIRECT, direct, t);
        if (eos > 0) path_answer(p, PATH_EOS, eos, t);
        cur = path_update(p, t, fight);
    }
    return cur;
}

int main(void) {
    PathPick p;

    /* nothing answers yet: Epic's (where packets wait) */
    path_init(&p);
    CHECK(path_update(&p, 1000, 0) == PATH_EOS, "start: Epic's");

    /* Epic's answers first, then the direct path: direct at once (the first choice), even a little slower */
    path_init(&p);
    CHECK(run(&p, 1000, 2000, 0, 40, 0) == PATH_EOS, "only Epic's answers: Epic's");
    CHECK(run(&p, 2250, 2250, 42, 40, 0) == PATH_DIRECT, "the direct path answers: direct");
    CHECK(p.why == PATH_WHY_UP, "why: the direct path answers (%s)", path_why(p.why));

    /* Epic's clearly quicker (direct 100, Epic's 60): Epic's, but only after PATH_HOLD_MS */
    CHECK(run(&p, 2500, 4000, 100, 60, 0) == PATH_DIRECT, "clearly quicker for 1.5 s: still direct");
    CHECK(run(&p, 4250, 6500, 100, 60, 0) == PATH_EOS, "clearly quicker for over 3 s: Epic's");
    CHECK(p.why == PATH_WHY_QUICKER, "why: quicker (%s)", path_why(p.why));

    /* a small gain is no reason: direct 50, Epic's 45 (10 %, 5 ms) stays direct; 43.5 (13 %) too */
    path_init(&p);
    CHECK(run(&p, 0, 10000, 50, 45, 0) == PATH_DIRECT, "Epic's 10 %% quicker: direct");
    CHECK(run(&p, 10250, 20000, 50, 43.5, 0) == PATH_DIRECT, "Epic's 13 %% quicker: direct");
    /* 15 % but under 5 ms: direct 20, Epic's 16.5 */
    path_init(&p);
    CHECK(run(&p, 0, 10000, 20, 16.5, 0) == PATH_DIRECT, "15 %% but 3.5 ms: direct");

    /* never mid-fight while the path lives: Epic's clearly quicker during a fight stays direct, moves after it */
    path_init(&p);
    run(&p, 0, 1000, 30, 0, 0);
    CHECK(p.cur == PATH_DIRECT, "direct before the fight");
    CHECK(run(&p, 1250, 20000, 100, 50, 1) == PATH_DIRECT, "a fight: no move");
    CHECK(run(&p, 20250, 24000, 100, 50, 0) == PATH_EOS, "after the fight: Epic's");

    /* ...but a dead path moves at once, fight or not */
    path_init(&p);
    run(&p, 0, 1000, 30, 60, 0);
    CHECK(p.cur == PATH_DIRECT, "direct");
    CHECK(run(&p, 1250, 3000, 0, 60, 1) == PATH_EOS, "direct dead mid-fight: Epic's at once");
    CHECK(p.why == PATH_WHY_DEAD, "why: dead (%s)", path_why(p.why));
    /* the direct path back: direct again after the hold (not mid-fight) */
    CHECK(run(&p, 3250, 4000, 30, 60, 0) == PATH_EOS, "direct back for 0.75 s: Epic's");
    CHECK(run(&p, 4250, 8000, 30, 60, 0) == PATH_DIRECT, "direct back for 3 s: direct");

    /* hysteresis: a round trip that wavers around the line does not flap */
    path_init(&p);
    run(&p, 0, 500, 50, 60, 0);
    int moves = p.moves;
    for (uint64_t t = 750; t < 60000; t += 250) {
        int quick = (t / 1000) % 2;   /* (Epic's clearly quicker one second, not the next) */
        path_answer(&p, PATH_DIRECT, 50, t);
        path_answer(&p, PATH_EOS, quick ? 30 : 60, t);
        path_update(&p, t, 0);
    }
    CHECK(p.moves == moves, "wavering: no move (%d moves)", p.moves - moves);

    /* Epic's dies while it is in use and direct lives: direct at once */
    path_init(&p);
    run(&p, 0, 500, 100, 40, 0);
    run(&p, 750, 5000, 100, 40, 0);
    CHECK(p.cur == PATH_EOS, "Epic's quicker: Epic's");
    CHECK(run(&p, 5250, 7000, 100, 0, 1) == PATH_DIRECT, "Epic's dead mid-fight: direct at once");

    /* /gsp/records */
    uint8_t recs[25 * 4];
    char json[256], a[51], b[51];
    memset(a, '0', 50); memset(b, '0', 50);
    a[50] = b[50] = 0;
    a[0] = '2'; a[1] = 'b'; a[49] = '1';
    b[1] = '1'; b[49] = '2';
    snprintf(json, sizeof json, "{\"records\":[\"%s\",\"zz\",\"%s\"]}", a, b);
    int n = results_parse_records(json, recs, 25, 4);
    CHECK(n == 2, "two records read (%d)", n);
    CHECK(n == 2 && recs[0] == 0x2b && recs[24] == 1 && recs[25] == 1 && recs[49] == 2, "their bytes");
    CHECK(results_parse_records("{\"records\":[]}", recs, 25, 4) == 0, "none");
    CHECK(results_parse_records("{\"error\":\"x\"}", recs, 25, 4) == 0, "not an answer");

    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
