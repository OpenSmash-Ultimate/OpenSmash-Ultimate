/* The arithmetic of two test-run switches (docs/TESTING-RUNS.md), apart from the runtime so that
 * tools/dev/testruntest.c can check it: SSBU_SPEED's pacing (gpu_display.c) and SSBU_SKIP_MOVIES'
 * sample budget (hle_movie_skip.c). */
#pragma once
#include <stdint.h>
#include <stdlib.h>

/* ---- SSBU_SPEED ------------------------------------------------------------------------ */
#define SPEED_MAX 1000.0

/* Its value: 1 (normal speed) when unset or empty, else n >= 0 times 60 refreshes a second
 * (0: uncapped). 0 for anything else (the caller says so and keeps 1). */
static inline int speed_parse(const char *text, double *speed) {
    *speed = 1;
    if (!text || !*text) return 1;
    char *end;
    double v = strtod(text, &end);
    if (end == text || *end || !(v >= 0 && v <= SPEED_MAX)) return 0;
    *speed = v;
    return 1;
}

/* When the next refresh is due, from when the last one was (0: none yet), the time now and the
 * time between two (in one unit): a step after the last, but not a burst to catch up after a
 * stall (a load) - more than four steps behind, it is now. */
static inline int64_t speed_due(int64_t last_due, int64_t now, int64_t step) {
    int64_t due = (last_due ? last_due : now) + step;
    return now - due > 4 * step ? now : due;
}

/* ---- SSBU_SKIP_MOVIES ---------------------------------------------------------------- */
/* Its value: the samples a movie's stream gives before it reports its end; 0 = off (unset, empty
 * or 0). 1 is the default, 12: ssbu-recomp found that an end before any sample, or before the
 * decoders have started, is not one the game gets through. 0 for anything else (the caller says
 * so and keeps it off). */
static inline int skip_movies_parse(const char *text, long *keep) {
    *keep = 0;
    if (!text || !*text) return 1;
    char *end;
    long v = strtol(text, &end, 10);
    if (end == text || *end || v < 0 || v > 1000000000L) return 0;
    *keep = v == 1 ? 12 : v;
    return 1;
}

/* Each movie::Extractor's samples: counted from its SetDataSource, and from its last SeekTo (the
 * game seeks to the start again when its movie began late; ssbu-recomp found that a budget spent
 * before such a seek left the stream with no sample the game would take after it). */
enum { SKIP_STREAMS = 16 };
typedef struct SkipStream { uint64_t extractor, read, base; int ended; } SkipStream;
typedef struct SkipMovies { long keep; unsigned next; SkipStream s[SKIP_STREAMS]; } SkipMovies;

static inline SkipStream *skip_find(SkipMovies *m, uint64_t extractor) {
    for (int i = 0; i < SKIP_STREAMS; i++)
        if (m->s[i].extractor == extractor && extractor) return &m->s[i];
    return NULL;
}
/* SetDataSource: a stream begins on this extractor (the oldest one's place is taken when all are
 * in use: an extractor's memory may have been freed and used again since). */
static inline void skip_open(SkipMovies *m, uint64_t extractor) {
    SkipStream *s = skip_find(m, extractor);
    if (!s) s = &m->s[m->next++ % SKIP_STREAMS];
    *s = (SkipStream){extractor, 0, 0, 0};
}
/* SeekTo: the budget starts again. Returns 1 if the stream had already been ended here (then the
 * decoders may have taken the end, and the movie may wait for ever: ssbu-recomp re-armed its own
 * decoders there, which this runtime cannot). */
static inline int skip_seek(SkipMovies *m, uint64_t extractor) {
    SkipStream *s = skip_find(m, extractor);
    if (!s) return 0;
    int was = s->ended;
    s->base = s->read;
    s->ended = 0;
    return was;
}
/* Before Read and GetTrackIndexForAvailableData: 1 when they are to report the end of the
 * stream instead; *first is 1 the first time (to log it). */
static inline int skip_ended(SkipMovies *m, uint64_t extractor, int *first) {
    SkipStream *s = skip_find(m, extractor);
    *first = 0;
    if (!m->keep || !s || s->read - s->base < (uint64_t)m->keep) return 0;
    *first = !s->ended;
    s->ended = 1;
    return 1;
}
/* After a Read that gave a sample. */
static inline void skip_read(SkipMovies *m, uint64_t extractor) {
    SkipStream *s = skip_find(m, extractor);
    if (s) s->read++;
}
