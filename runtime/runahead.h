/* Run-ahead's schedule (hle_rb_core.c, "Run-ahead"; docs/TECHNICAL.md). Plain C with nothing of
 * the runtime in it, so tools/dev/ratest.c runs it on a toy game (tests/test_runahead.py).
 *
 * The game's main loop is cut at the safe point (hle_rb_core.c): a SEGMENT runs from one safe point
 * to the next - the render preparation of the last logic frame, the pad poll, one logic frame, the
 * frame end and the submission of the picture just prepared. So a segment shows the logic frame of
 * the segment BEFORE it, and a press read in segment j is first drawn in segment j+1: one frame of
 * the game's own pipeline, on top of what the game's logic takes to answer.
 *
 * Run-ahead of n frames (1-3), at each fight frame shown:
 *   R  a real segment, hidden: its poll reads the controllers now, its logic frame is the real one.
 *   -> SAVE (the state after R: the real game)
 *   P  n - 1 segments, hidden, each a logic frame further with the same controls (predicted),
 *   S  the segment shown: it draws the last logic frame (R's when n is 1: nothing predicted);
 *      its own logic frame is thrown away
 *   -> LOAD (back to the save), and the next frame shown starts with R again.
 * A frame shown then has the logic frame its poll read n frames sooner than without run-ahead:
 * n = 1 takes out the pipeline's frame and predicts nothing; n = 2 also shows one logic frame run
 * ahead on the controls held now (wrong only for the frame a press or release begins). A segment
 * costs one more game frame per frame of run-ahead, plus a save and a load.
 *
 * Hidden segments: the caller's rb_fast (not shown, not paced). Predicted segments (P and S) must
 * not move a test's input sequence on or count as fight frames (rb_ra_predicting). */
#pragma once

enum { RA_IDLE, RA_REAL, RA_AHEAD };
enum { RA_NOTHING, RA_SAVE, RA_LOAD };

typedef struct RaSched {
    int n;                   /* frames ahead wanted (0: off); read at each start of a frame shown */
    int phase;               /* RA_IDLE: normal frames; RA_REAL: R runs; RA_AHEAD: P / S run, a load to come */
    int left;                /* (RA_AHEAD) segments until the load */
    unsigned key0;           /* the logic frame R started from */
    unsigned last_key;
    int have_last;
    long long cycles, paused, save_failed, load_refused;
} RaSched;

/* At a safe point in a fight, the logic frame `key`: what to do now (RA_SAVE: then ra_saved; RA_LOAD:
 * then ra_loaded where the save returns, or ra_refused), and in *hide whether the next segment is hidden. */
static inline int ra_point(RaSched *s, unsigned key, int *hide) {
    int act = RA_NOTHING;
    *hide = 0;
    switch (s->phase) {
    case RA_IDLE:   /* (only once the logic moves: not while paused, nor at the first safe point) */
        if (s->n > 0 && s->have_last && key != s->last_key) { s->phase = RA_REAL; s->key0 = key; *hide = 1; }
        break;
    case RA_REAL:
        if (key == s->key0) { s->phase = RA_IDLE; s->paused++; break; }   /* R ran no logic frame (pause): a normal frame */
        act = RA_SAVE;
        break;
    case RA_AHEAD:
        if (--s->left <= 0) act = RA_LOAD;
        else *hide = s->left > 1;
        break;
    default:
        break;
    }
    s->last_key = key;
    s->have_last = 1;
    return act;
}

/* After RA_SAVE: saved (ok) or not; not saved, this frame is a normal one (R's picture, real). */
static inline void ra_saved(RaSched *s, int ok, int *hide) {
    if (!ok) { s->phase = RA_IDLE; s->save_failed++; *hide = 0; return; }
    s->phase = RA_AHEAD;
    s->left = s->n > 0 ? s->n : 1;
    *hide = s->left > 1;
}

/* Back at the save after RA_LOAD, the logic frame `key` restored: R again (or normal frames, run-ahead off). */
static inline void ra_loaded(RaSched *s, unsigned key, int *hide) {
    s->cycles++;
    s->last_key = key;
    s->have_last = 1;
    if (s->n <= 0) { s->phase = RA_IDLE; *hide = 0; return; }
    s->phase = RA_REAL; s->key0 = key; *hide = 1;
}

/* RA_LOAD refused: the game goes on from the predicted frames (they become real). */
static inline void ra_refused(RaSched *s, int *hide) { s->phase = RA_IDLE; s->load_refused++; *hide = 0; }

/* The fight is over (or another mode took the safe point): normal frames from the next fight. */
static inline void ra_stop(RaSched *s) { s->phase = RA_IDLE; s->have_last = 0; }

/* The segment running now is a predicted one (P or S). */
static inline int ra_predicting(const RaSched *s) { return s->phase == RA_AHEAD; }
