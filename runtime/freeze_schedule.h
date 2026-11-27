/* When the freeze watchdog (debug_watchdog.c) writes its reports: a header of its own, so that tests can run it
 * (tests/test_freeze_schedule.py).
 *
 * Until 0.5.3 the first report came after 5 s with no frame presented, again at 30 s and 120 s. In the crash
 * reports of 26-27/11 (32 freeze reports from 0.5.1-0.5.3), all but two were first reports of 5.0-5.9 s, and the
 * logs of most of those say "frames are presented again" within the next second or so: stalls (a long load, a
 * driver that took its time), not hangs. The real hangs went on to their 30 s report, or were seen at 82 s.
 * SSBU_FREEZE_CONFIRM=<seconds> (default 15): the first report only after that long with no frame, the second at
 * 30 s (or twice the first), the third at 120 s (or twice the second). A stall of 5 s or more that ends before
 * the first report is one line in the log, with its length, and no report. SSBU_FREEZE_CONFIRM=0 (or 5): as
 * before - reports at 5, 30 and 120 s, no stall lines. */
#pragma once
#include <stdlib.h>

#define FREEZE_STALL_MS 5000ull   /* (what counts as a stall worth a log line: the old first report's 5 s) */

typedef struct {
    unsigned long long at[3];   /* the reports' times, ms with no frame */
    int old;                    /* SSBU_FREEZE_CONFIRM=0: the old schedule and lines */
    int reports;                /* written in this freeze */
    unsigned long long from;    /* (a stall) the last frame before it; 0: none going on */
} FreezeSchedule;

static inline void freeze_schedule_init(FreezeSchedule *s, const char *env) {
    long sec = env && *env ? strtol(env, NULL, 10) : 15;
    if (sec < 0 || sec > 600) sec = 15;
    s->old = sec == 0 || sec == 5;
    unsigned long long first = s->old ? 5000 : (unsigned long long)sec * 1000;
    s->at[0] = first;
    s->at[1] = first < 30000 ? 30000 : first * 2;
    s->at[2] = s->at[1] < 120000 ? 120000 : s->at[1] * 2;
    s->reports = 0;
    s->from = 0;
}

enum { FREEZE_NONE, FREEZE_REPORT, FREEZE_RESUMED, FREEZE_STALL_ENDED };

/* One look (every 500 ms): `now` the clock, `last` the last frame presented (ms). FREEZE_REPORT: write a report
 * (*len: how long there was no frame; the first of the freeze when s->reports is 1 after the call);
 * FREEZE_RESUMED: frames again after a report (*len: how long they stopped); FREEZE_STALL_ENDED: frames again after
 * a stall of FREEZE_STALL_MS or more with no report (*len: its length; never with the old schedule). */
static inline int freeze_schedule_tick(FreezeSchedule *s, unsigned long long now, unsigned long long last, unsigned long long *len) {
    unsigned long long since = now > last ? now - last : 0;
    if (since < FREEZE_STALL_MS) {
        int was = s->reports, stalled = s->from != 0;
        unsigned long long from = s->from;
        s->reports = 0;
        s->from = 0;
        *len = stalled && last > from ? last - from : 0;
        if (was) return FREEZE_RESUMED;
        return stalled && !s->old ? FREEZE_STALL_ENDED : FREEZE_NONE;
    }
    if (!s->from) s->from = last;
    if (s->reports < 3 && since >= s->at[s->reports]) {
        s->reports++;
        *len = since;
        return FREEZE_REPORT;
    }
    return FREEZE_NONE;
}
