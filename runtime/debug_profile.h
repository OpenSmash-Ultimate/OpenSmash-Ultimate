/* The sampling profiler behind the F6 debug key, thread dumps and the hitch log (debug_profile.c). */
#pragma once

void profile_start(double seconds);   /* sample every thread for this long, then write a report */
int profile_running(void);
/* Every thread's stack but the caller's, a line at a time (freeze reports); 0 if the profiler is busy. */
int profile_dump_threads(void (*line)(const char *text));
int profile_dump_some(const unsigned long *tids, unsigned n, void (*line)(const char *text));   /* (only the threads with these ids) */
void hitch_present(void);             /* at each present: SSBU_HITCH's frame accounting */
