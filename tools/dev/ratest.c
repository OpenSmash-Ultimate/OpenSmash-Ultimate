/* Run-ahead's schedule (runtime/runahead.h) on a toy game, with no game: tests/test_runahead.py.
 *
 * The toy runs as the game's main loop does between two safe points (a segment): it draws the state
 * it starts from (the last logic frame), reads the controller of the frame shown under way, and runs a
 * logic frame. Savestates are a copy of its state. A run without run-ahead is the reference; runs with
 * 1, 2 and 3 frames of run-ahead must give the same real logic frames, show each frame the reference's
 * picture n frames sooner (always with n = 1, which predicts nothing; with n > 1 wherever the controls
 * held stay the same over the frames predicted), and draw a press n frames sooner than the reference
 * (the toy answers a press in the logic frame that reads it, as the game does). A pause stops run-ahead
 * and it comes back; a refused save is a normal frame, a refused load keeps the predicted frames.
 *
 *   clang-cl /O2 tools/dev/ratest.c /Fe:build/ratest.exe   (clang -O2 elsewhere); prints "<n> checks, <f> failed" */
#include <stdio.h>
#include <string.h>

#include "../../runtime/runahead.h"

typedef struct Toy { unsigned key; int x, act; unsigned h; } Toy;   /* key: the logic frame counter */

#define HOSTS 2400
#define MAXN 4

static int checks, failed;
static void check(int ok, const char *what, int a, int b) {
    checks++;
    if (!ok && ++failed <= 20) printf("FAILED: %s (%d, %d)\n", what, a, b);
}

/* the controls in frame shown h: presses of a few frames, a hold, a pause from 900 to 960 */
static int pad(long long h) {
    if (h >= 900 && h < 960) return 8;            /* (paused: the toy's logic stands still) */
    if (h % 97 < 4) return 1;                     /* a short press */
    if (h % 233 >= 100 && h % 233 < 140) return 2;   /* a hold */
    if (h % 61 == 30) return 4;                   /* one frame */
    return 0;
}

static void logic(Toy *t, int in) {
    if (in & 8) return;                           /* (the pause: no logic frame) */
    t->key++;
    if (in & 1 && !t->act) t->act = 1;            /* a move begins in the frame that reads the press */
    else if (t->act) t->act = t->act >= 12 ? 0 : t->act + 1;
    t->x += (in & 2 ? 3 : 0) + (in & 4 ? -1 : 0);
    t->h = t->h * 2654435761u ^ (unsigned)(t->x * 31 + t->act * 7 + in);
}

typedef struct Run {
    Toy real[HOSTS * 2]; int have[HOSTS * 2];     /* real states by logic frame */
    Toy pic[HOSTS];                               /* the state each frame shown draws */
    long long segs;
    RaSched s;
} Run;
static Run runs[MAXN + 1];

/* refuse: 1 a save refused at frame shown 500, 2 a load refused at frame shown 700 */
static void play(Run *r, int n, int refuse) {
    memset(r, 0, sizeof *r);
    r->s.n = n;
    Toy t = {0}, saved = {0};
    long long shown = 0;
    while (shown < HOSTS) {
        int hide = 0, act = ra_point(&r->s, t.key, &hide);   /* (the safe point) */
        if (act == RA_SAVE) {
            int ok = !(refuse == 1 && shown == 500);
            if (ok) saved = t;
            if (t.key < HOSTS * 2) { r->real[t.key] = t; r->have[t.key] = 1; }
            ra_saved(&r->s, ok, &hide);
        } else if (act == RA_LOAD) {
            if (refuse == 2 && shown == 700) ra_refused(&r->s, &hide);
            else { t = saved; ra_loaded(&r->s, t.key, &hide); }
        } else if (r->s.phase != RA_AHEAD && t.key < HOSTS * 2) {
            if (r->have[t.key] && memcmp(&r->real[t.key], &t, sizeof t)) check(0, "a real logic frame came out two ways", (int)t.key, n);
            r->real[t.key] = t; r->have[t.key] = 1;
        }
        long long host = shown;
        if (!hide) r->pic[shown++] = t;
        logic(&t, pad(host));                     /* (the segment: live controls of the frame shown under way) */
        r->segs++;
    }
}

/* the first frame shown from `from` on whose picture differs from the one before `from` */
static long long first_change(const Run *r, long long from) {
    for (long long h = from; h < HOSTS; h++)
        if (r->pic[h].act != r->pic[from - 1].act) return h;
    return -1;
}

int main(void) {
    play(&runs[0], 0, 0);
    for (int n = 1; n <= 3; n++) {
        Run *r = &runs[n], *ref = &runs[0];
        play(r, n, 0);
        int same = 1, firstbad = -1;
        for (int k = 0; k < HOSTS * 2; k++)
            if (r->have[k] && ref->have[k] && memcmp(&r->real[k], &ref->real[k], sizeof(Toy))) { same = 0; if (firstbad < 0) firstbad = k; }
        check(same, "the real logic frames are the reference's", firstbad, n);
        /* the picture of frame shown h is the reference's of h + n (the pipeline's frame and n - 1 predicted),
         * except where the controls change within the frames predicted, and around the pause */
        int wrong = 0, unexplained = 0, compared = 0;
        for (long long h = 1; h + n < HOSTS; h++) {
            if (h >= 890 && h < 975) continue;
            compared++;
            if (r->pic[h].h == ref->pic[h + n].h && r->pic[h].key == ref->pic[h + n].key) continue;
            wrong++;
            int changes = 0;
            for (int k = 1; k < n; k++) changes |= pad(h + k) != pad(h);
            if (!changes) unexplained++;
        }
        check(unexplained == 0, "every picture is the reference's n frames on, but where a press began or ended in the frames predicted", unexplained, n);
        if (n == 1) check(wrong == 0, "run-ahead 1 predicts nothing: every picture the reference's next", wrong, compared);
        /* press to picture: the reference 1 frame shown (the pipeline), run-ahead 0 */
        int presses = 0, ref_late = 0, ra_late = 0;
        for (long long h = 2; h < HOSTS - 20; h++)
            if ((pad(h) & 1) && !(pad(h - 1) & 1) && !ref->pic[h - 1].act && !r->pic[h - 1].act && h < 880) {
                presses++;
                ref_late += first_change(ref, h) - h != 1;
                ra_late += first_change(r, h) - h != 0;
            }
        check(presses > 5 && ref_late == 0, "a press reaches the reference's picture 1 frame shown later", ref_late, presses);
        check(ra_late == 0, "a press reaches run-ahead's picture in its own frame shown", ra_late, n);
        /* n + 1 segments per frame shown while it runs (the pause and the first frame aside) */
        long long extra = r->segs - HOSTS;
        check(extra >= (long long)n * (HOSTS - 120) && extra <= (long long)n * HOSTS, "n more segments per frame shown", (int)extra, n);
        check(r->s.paused >= 1, "the pause stops run-ahead", (int)r->s.paused, n);
        check(r->s.cycles > HOSTS - 120, "run-ahead runs again after the pause", (int)r->s.cycles, n);
        printf("run-ahead %d: %lld segments for %d frames shown, %d of %d pictures predicted differently, %lld frames ahead, paused %lld\n",
               n, r->segs, HOSTS, wrong, compared, r->s.cycles, r->s.paused);
    }
    /* refusals */
    Run *r = &runs[MAXN];
    play(r, 2, 1);
    check(r->s.save_failed == 1, "a refused save is counted", (int)r->s.save_failed, 1);
    check(r->s.cycles > HOSTS - 120, "and run-ahead goes on", (int)r->s.cycles, 1);
    for (int k = 0; k < HOSTS * 2; k++)
        if (k < 500 && r->have[k] && runs[0].have[k] && memcmp(&r->real[k], &runs[0].real[k], sizeof(Toy))) { check(0, "real frames before a refused save as the reference's", k, 1); break; }
    play(r, 2, 2);
    check(r->s.load_refused == 1, "a refused load is counted", (int)r->s.load_refused, 2);
    check(r->s.cycles > HOSTS - 120, "and run-ahead goes on from the predicted frames", (int)r->s.cycles, 2);
    printf("%d checks, %d failed\n", checks, failed);
    return failed != 0;
}
