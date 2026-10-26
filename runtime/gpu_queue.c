/* The GPU's own thread.
 *
 * The console's GPU runs command lists while the CPU goes on: the graphics driver submits them
 * (SubmitGpfifo) and gets a fence back - a sync point value - and waits for that value only where
 * it needs the results. Running each submission inside the ioctl (translating it and drawing it
 * through Direct3D) put all the rendering on the thread that submits, the game's main thread: in
 * an 8-player match about 60% of its time, so the game's own work and the rendering added up
 * instead of overlapping.
 *
 * Here a submission is copied into a queue and the ioctl returns its fence at once. The "gpu"
 * thread runs the queue in order; when a submission has run, its sync point reaches its fence and
 * whoever waits for it (EventWait) wakes. What must see the GPU's work done first waits here
 * too: the display shows a queued buffer once everything submitted before it has run
 * (gpu_queue_wait), and anything that takes away memory the GPU may still read - unmapping GPU
 * addresses, decommitting guest memory - drains the queue first.
 *
 * SSBU_GPU_THREAD=0: no thread; submissions run in the ioctl, as before. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>

#include "gpu.h"
#include "input_seq.h"

#define JOBS 4096        /* submissions queued at most (the submitter waits beyond) */
#define INLINE 16        /* GPFIFO entries kept in the job itself; more are allocated */

typedef struct Job { u32 channel, count, syncpoint, fence; u64 *entries; u64 inline_entries[INLINE]; } Job;

static Job jobs[JOBS];
static u64 queued, done;   /* submissions so far: queued, and run (jobs[n % JOBS] is number n) */
static SRWLOCK lock = SRWLOCK_INIT;
static CONDITION_VARIABLE work = CONDITION_VARIABLE_INIT, progress = CONDITION_VARIABLE_INIT;
static int enabled = -1;   /* -1 not decided yet */
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
extern u64 gpu_stat_draws;   /* (gpu.c) */
static int count_draws;      /* SSBU_SEQ: drawn_by[n % JOBS] = draw calls run once submission n had */
static u64 drawn_by[JOBS];

static DWORD WINAPI gpu_thread(LPVOID unused) {
    (void)unused;
    SetThreadDescription(GetCurrentThread(), L"gpu");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    for (;;) {
        AcquireSRWLockExclusive(&lock);
        while (done == queued) SleepConditionVariableSRW(&work, &lock, INFINITE, 0);
        Job *j = &jobs[done % JOBS];
        ReleaseSRWLockExclusive(&lock);
        gpu_submit(j->channel, j->entries, j->count);   /* (the job stays put until `done` passes it) */
        AcquireSRWLockExclusive(&lock);
        u32 id = j->syncpoint;
        if (id < SYNCPT_MAX && (i32)(j->fence - gpu_syncpt_min[id]) > 0) gpu_syncpt_min[id] = j->fence;
        if (j->entries != j->inline_entries) free(j->entries);
        if (count_draws) drawn_by[done % JOBS] = gpu_stat_draws;
        done++;
        WakeAllConditionVariable(&progress);
        ReleaseSRWLockExclusive(&lock);
    }
}

static BOOL CALLBACK start(PINIT_ONCE o, PVOID p, PVOID *ctx) {
    (void)o; (void)p; (void)ctx;
    const char *env = getenv("SSBU_GPU_THREAD");
    enabled = !(env && *env == '0');
    count_draws = input_seq_on();
    if (enabled) {
        HANDLE t = CreateThread(NULL, 0, gpu_thread, NULL, 0, NULL);
        if (t) CloseHandle(t);
        else enabled = 0;
    }
    fprintf(stderr, "[gpu] %s\n", enabled ? "submissions run on the GPU thread" : "submissions run in the submitting thread (SSBU_GPU_THREAD=0)");
    return TRUE;
}

int gpu_threaded(void) {
    InitOnceExecuteOnce(&once, start, NULL, NULL);
    return enabled;
}

void gpu_queue_submit(u32 channel, const u64 *entries, u32 count, u32 syncpoint, u32 fence) {
    AcquireSRWLockExclusive(&lock);
    while (queued - done >= JOBS) SleepConditionVariableSRW(&progress, &lock, INFINITE, 0);
    Job *j = &jobs[queued % JOBS];
    j->channel = channel;
    j->count = count;
    j->syncpoint = syncpoint;
    j->fence = fence;
    j->entries = count <= INLINE ? j->inline_entries : malloc(sizeof *entries * count);
    if (!j->entries) { j->entries = j->inline_entries; j->count = count = 0; }
    memcpy(j->entries, entries, sizeof *entries * count);   /* (the driver reuses its array) */
    queued++;
    WakeConditionVariable(&work);
    ReleaseSRWLockExclusive(&lock);
}

u64 gpu_queue_mark(void) {
    AcquireSRWLockShared(&lock);
    u64 n = queued;
    ReleaseSRWLockShared(&lock);
    return n;
}

void gpu_queue_wait(u64 mark) {
    if (enabled != 1) return;
    AcquireSRWLockExclusive(&lock);
    while ((i64)(mark - done) > 0) SleepConditionVariableSRW(&progress, &lock, INFINITE, 0);
    ReleaseSRWLockExclusive(&lock);
}

void gpu_queue_drain(void) { gpu_queue_wait(gpu_queue_mark()); }

/* SSBU_SEQ's draw count (input_seq.h): the draw calls run once the first `mark` submissions had
 * run. Without the GPU thread, those run so far: submissions run as they come, so the frames the
 * game has drawn since may be in it. */
u64 gpu_queue_draws(u64 mark) {
    if (enabled != 1 || !count_draws || !mark) return gpu_stat_draws;
    AcquireSRWLockShared(&lock);
    u64 n = done >= mark && done - mark < JOBS ? drawn_by[(mark - 1) % JOBS] : gpu_stat_draws;
    ReleaseSRWLockShared(&lock);
    return n;
}

int gpu_syncpt_wait(u32 id, u32 threshold, u32 timeout_ms) {
    if (id >= SYNCPT_MAX) return 1;
    u64 until = timeout_ms == ~0u ? ~0ull : GetTickCount64() + timeout_ms;
    AcquireSRWLockExclusive(&lock);
    int reached;
    while (!(reached = (i32)(gpu_syncpt_min[id] - threshold) >= 0)) {
        u64 now = GetTickCount64();
        if (now >= until) break;
        /* (sync point increments inside a command list don't wake this: the end of the
         * submission does, and the wait looks again every few milliseconds regardless) */
        SleepConditionVariableSRW(&progress, &lock, until - now < 4 ? (DWORD)(until - now) : 4, 0);
    }
    ReleaseSRWLockExclusive(&lock);
    return reached;
}
