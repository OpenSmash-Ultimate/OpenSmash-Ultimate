/* Movies in test runs (docs/TESTING-RUNS.md): SSBU_SKIP_MOVIES, and SSBU_SEQ's mark "movie".
 *
 * SSBU_SKIP_MOVIES=<n>: every movie's stream reports its end after n samples (1: 12), and the
 * game ends the movie by its own end-of-stream path. ssbu-recomp found that an end before any
 * sample is not one the game gets through (testrun.h has the count).
 *
 * The movies play through the SDK's movie library, the game's own copy in subsdk0 (hle_movie.c).
 * The game drives a movie::Extractor - { vtable, impl } - through its vtable, the exported
 * _ZTVN5movie9ExtractorE + 0x10, with the slots below as ssbu-recomp read them off subsdk0. Its
 * feed threads take 0x2000 from GetTrackIndexForAvailableData or Read as "no more data" and
 * send each decoder an end-of-stream buffer. Once the modules are loaded, those slots get
 * wrappers here, provided each holds the function its export name says (else none is touched,
 * and the log says so). The wrappers call the library's own functions, and only answer 0x2000
 * themselves once the samples are spent. With SSBU_SEQ, SetDataSource raises the mark "movie".
 *
 * Not ported: ssbu-recomp's decoders took up their inputs again after the game seeked a stream
 * that had already ended early (it does that, without a flush, when a movie started late on a
 * busy PC); here the decoders are the game's, and such a movie may wait for ever. The log says
 * when it happens; a larger n lets the movie start before its end. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>

#include "host.h"
#include "input_seq.h"
#include "testrun.h"

enum {   /* the movie::Extractor vtable's slots (byte offsets) */
    EXT_SOURCE_PATH = 0x10,     /* SetDataSource(const char *) */
    EXT_SOURCE_MEMORY = 0x18,   /* SetDataSource(const void *, int) */
    EXT_SEEK = 0x40,            /* SeekTo(int64) */
    EXT_TRACK_FOR_DATA = 0x48,  /* GetTrackIndexForAvailableData(size_t *) */
    EXT_READ = 0x50,            /* Read(Buffer *) */
    EXT_SOURCE_STREAM = 0x78,   /* SetDataSource(ClientStreamReader *, const char *) */
    EXT_SLOTS = 0x80 / 8
};
#define NO_MORE_DATA 0x2000

static SkipMovies skip;
static SRWLOCK skip_lock = SRWLOCK_INIT;
static u64 library[EXT_SLOTS];   /* the library's own function in each slot taken */

static void call_library(Cpu *c, int slot) { rt_lookup(library[slot / 8])(c); }   /* (x30 kept: returns to the game) */

static void set_source(Cpu *c, int slot) {
    AcquireSRWLockExclusive(&skip_lock);
    skip_open(&skip, c->x[0]);
    ReleaseSRWLockExclusive(&skip_lock);
    input_seq_mark("movie");
    call_library(c, slot);
}
static void source_path(Cpu *c) { set_source(c, EXT_SOURCE_PATH); }
static void source_memory(Cpu *c) { set_source(c, EXT_SOURCE_MEMORY); }
static void source_stream(Cpu *c) { set_source(c, EXT_SOURCE_STREAM); }

static void seek(Cpu *c) {
    AcquireSRWLockExclusive(&skip_lock);
    int ended = skip_seek(&skip, c->x[0]);
    ReleaseSRWLockExclusive(&skip_lock);
    if (ended) fprintf(stderr, "[movie] SSBU_SKIP_MOVIES: the game seeks a stream that ended early; if the movie stops here, a larger count lets it start first\n");
    call_library(c, EXT_SEEK);
}

/* Spent: the call answers "no more data" without the library. */
static int spent(u64 extractor) {
    int first;
    AcquireSRWLockExclusive(&skip_lock);
    int end = skip_ended(&skip, extractor, &first);
    ReleaseSRWLockExclusive(&skip_lock);
    if (first) fprintf(stderr, "[movie] SSBU_SKIP_MOVIES: end of the stream after %ld samples\n", skip.keep);
    return end;
}

static void track_for_data(Cpu *c) {
    if (spent(c->x[0])) { c->x[0] = NO_MORE_DATA; return; }
    call_library(c, EXT_TRACK_FOR_DATA);
}

static void read_sample(Cpu *c) {
    u64 extractor = c->x[0];
    if (spent(extractor)) { c->x[0] = NO_MORE_DATA; return; }
    call_library(c, EXT_READ);
    if ((u32)c->x[0] == 0) {
        AcquireSRWLockExclusive(&skip_lock);
        skip_read(&skip, extractor);
        ReleaseSRWLockExclusive(&skip_lock);
    }
}

void movie_test_start(void) {
    static const struct { int slot; const char *name; GuestFn *fn; } TAKEN[] = {
        {EXT_SOURCE_PATH, "_ZN5movie9Extractor13SetDataSourceE", source_path},
        {EXT_SOURCE_MEMORY, "_ZN5movie9Extractor13SetDataSourceE", source_memory},
        {EXT_SEEK, "_ZN5movie9Extractor6SeekToE", seek},
        {EXT_TRACK_FOR_DATA, "_ZN5movie9Extractor29GetTrackIndexForAvailableDataE", track_for_data},
        {EXT_READ, "_ZN5movie9Extractor4ReadE", read_sample},
        {EXT_SOURCE_STREAM, "_ZN5movie9Extractor13SetDataSourceE", source_stream},
    };
    const char *v = getenv("SSBU_SKIP_MOVIES");
    long keep;
    int seq = input_seq_on();   /* (main's call: a sequence that does not parse stops the program now) */
    if (!skip_movies_parse(v, &keep)) fprintf(stderr, "[movie] SSBU_SKIP_MOVIES is a number of samples (1: 12), not '%s': movies play\n", v);
    if (!keep && !seq) return;
    u64 vtable = rt_symbol("_ZTVN5movie9ExtractorE");
    const char *why = vtable ? NULL : "no module exports movie::Extractor's vtable";
    for (u32 i = 0; i < sizeof TAKEN / sizeof *TAKEN && !why; i++)
        if (!rt_symbol_at(ld64(vtable + 0x10 + TAKEN[i].slot), TAKEN[i].name)) why = "its vtable is not as expected";
    if (why) {
        fprintf(stderr, "[movie] %s: %s, movies are left alone\n", keep ? "SSBU_SKIP_MOVIES" : "SSBU_SEQ's mark movie", why);
        return;
    }
    skip.keep = keep;
    for (u32 i = 0; i < sizeof TAKEN / sizeof *TAKEN; i++) {
        u64 at = vtable + 0x10 + TAKEN[i].slot;
        library[TAKEN[i].slot / 8] = ld64(at);
        st64(at, rt_native(TAKEN[i].fn, TAKEN[i].name));
    }
    if (keep) fprintf(stderr, "[movie] SSBU_SKIP_MOVIES: each movie's stream ends after %ld samples\n", keep);
}
