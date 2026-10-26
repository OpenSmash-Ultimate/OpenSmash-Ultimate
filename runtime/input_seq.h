/* SSBU_SEQ: an input sequence for player 1 counted in the game's frames, not in seconds
 * (docs/TESTING-RUNS.md), so a test run does not depend on how fast this PC loads.
 *
 * The game asks for player 1's pad once a frame; each ask (a poll) moves the sequence on by one
 * step tick, or by what the steps hook says (input_seq_set_steps). The parser and the step
 * machine below (hle_input_seq.c) are plain C with nothing of the runtime in them, so
 * tools/dev/testruntest.c drives them with made-up sequences; hle_hid.c runs one on the polls.
 *
 * Steps, separated by spaces or new lines ('#' starts a comment that runs to the end of the
 * line). Keys are the names SSBU_INPUT takes (A B X Y L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN
 * LSTICK RSTICK, and SLEFT SUP SRIGHT SDOWN for the left stick), joined by '+'.
 *   <n>                         nothing for n ticks
 *   <n>:<keys>[/<h>]            nothing for n ticks, then the keys for h ticks (8 if not given)
 *   @<mark>                     wait until something raised that mark (input_seq_mark)
 *   @draws:<lo>-<hi>[/<n>]      wait until the frames shown had lo..hi draw calls each for n
 *                               ticks in a row (20 if not given): a screen known by its draw count
 *   @until:<lo>-<hi>[/<n>]:<keys>[/<p>]
 *                               press the keys (8 ticks) until the draw count has been in lo..hi
 *                               for n ticks in a row: at once, and p ticks after a press began
 *                               (200 if not given), each on a tick the count is out of range; a
 *                               press under way is finished first
 *   @untilmark:<mark>:<keys>[/<p>[/<h>]]
 *                               press the keys (h ticks, 8 if not given) at once and again every
 *                               p ticks (200) until the mark has been raised; a press under way
 *                               is finished first
 * A step that waits ends on the tick its condition is met, and the next step starts on the tick
 * after. Holds are exact: "0:A/3 2:B" is A for ticks 1-3, nothing for 4-5, B for 6-13. */
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { SEQ_STEPS = 256, SEQ_TEXT = 48, SEQ_HOLD = 8, SEQ_RUN = 20, SEQ_PERIOD = 200 };
#define SEQ_MAX_N 10000000L   /* (any count in a step: some 46 hours of frames) */

typedef enum { SEQ_KEYS, SEQ_MARK, SEQ_DRAWS, SEQ_UNTIL, SEQ_UNTILMARK } SeqKind;
typedef struct SeqStep {
    SeqKind kind;
    long wait, hold;          /* KEYS: ticks of nothing, then of the keys; UNTIL(MARK): wait = the period */
    long lo, hi, need;        /* DRAWS, UNTIL: lo..hi draw calls for need ticks in a row */
    uint64_t buttons;         /* the keys: NpadButton bits, and the left stick */
    int lx, ly;
    char mark[SEQ_TEXT];
    char text[SEQ_TEXT];      /* as written, for the log */
} SeqStep;

typedef struct Seq {
    SeqStep steps[SEQ_STEPS];
    int count, index;         /* the steps, and the one under way (count: the sequence has ended) */
    long pos, run, pressed;   /* the step's ticks so far; ticks in a row in range; the tick its last press began (0 none) */
    long long ticks;
    uint64_t buttons;         /* the keys of the last tick: what the poll gives the game */
    int lx, ly;
    char said[160];           /* what the last tick did, when seq_tick returned 1 */
} Seq;

typedef int SeqSeenFn(const char *mark, void *user);        /* has `mark` been raised? */
typedef void SeqLogFn(const char *line, void *user);
typedef int SeqStepsFn(void);                                /* ticks for this poll */
enum { SEQ_MAX_TICKS = 1000 };

/* Reads a sequence into s, ready to run. Returns its steps (0: none, as for NULL, "" or only
 * comments), or -1 with `why` naming the step. */
int seq_parse(Seq *s, const char *text, char *why, size_t why_size);
/* One tick, with the draw calls of the last frame shown (-1: none yet). Returns 1 when s->said
 * says what it did (a press began, a wait ended). */
int seq_tick(Seq *s, long draws, SeqSeenFn *seen, void *user);
int seq_finished(const Seq *s);
/* One of player 1's polls: as many ticks as `steps` says (NULL: 1; kept to 0..SEQ_MAX_TICKS),
 * each line said given to `log`. Returns the ticks. The keys are then the last tick's, so a poll
 * of 0 ticks gives what the poll before gave. */
long seq_poll(Seq *s, SeqStepsFn *steps, long draws, SeqSeenFn *seen, SeqLogFn *log, void *user);

/* ---- the runtime's side (hle_hid.c) ---------------------------------------------------- */
/* SSBU_SEQ is set and has steps (an empty one is as unset). It is read at the first call, which
 * main makes at start-up through movie_test_start: a sequence that does not parse stops the
 * program there. */
int input_seq_on(void);
/* Any thread: something the sequence can wait for happened (@<what>). "movie" when a movie's
 * stream is opened (hle_movie_skip.c), "module:<name>" when the game has loaded that module
 * (host.c: the name its "[ro] loaded" line gives). */
void input_seq_mark(const char *what);
/* The hook the sequence asks at each of player 1's polls for the ticks to move on by (seq_poll);
 * NULL, the default, is 1 a poll. */
void input_seq_set_steps(SeqStepsFn *steps_per_poll);
/* The vsync thread, at each frame shown: the draw calls the GPU had run once that frame was
 * drawn (gpu_queue_draws), for @draws. */
void input_seq_presented(uint64_t draws_total);
