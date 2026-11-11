/* The display service (vi), natively: displays, layers, the vsync event, and the buffer
 * queue behind each layer.
 *
 * The SDK's display library talks to vi:u / vi:s / vi:m; a layer comes back as a "native
 * window" parcel naming a binder object. From then on the graphics driver drives that
 * window through Android's IGraphicBufferProducer protocol, sent as binder transactions to
 * the relay service (IHOSBinderDriver). The queue's consumer is a native vsync thread: 60
 * times a second it presents the oldest queued buffer and returns the previous one, which
 * paces the game exactly as the console's compositor does. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "ipc.h"
#include "present.h"
#include "display.h"
#include "gpu.h"   /* (gpu_queue_mark / gpu_queue_wait) */
#include "input_seq.h"

void test_fight_presented(void);   /* (hle_fight_test.c) SSBU_DEBUG_END's count of fight frames */

#define DISPLAY_ID 1
#define LAYER_ID 1
#define BINDER_ID 1
#define WIDTH 1920
#define HEIGHT 1080
#define SLOTS 64

enum { FREE, DEQUEUED, QUEUED, ACQUIRED };
enum {   /* IGraphicBufferProducer transactions */
    REQUEST_BUFFER = 1, SET_BUFFER_COUNT = 2, DEQUEUE_BUFFER = 3, DETACH_BUFFER = 4, DETACH_NEXT_BUFFER = 5,
    ATTACH_BUFFER = 6, QUEUE_BUFFER = 7, CANCEL_BUFFER = 8, QUERY = 9, CONNECT = 10, DISCONNECT = 11,
    ALLOCATE_BUFFERS = 13, SET_PREALLOCATED_BUFFER = 14, GET_BUFFER_HISTORY = 17
};

typedef struct Slot { int state, has_buffer; u8 buffer[0x400]; u32 buffer_size; u64 queued_at, gpu_mark; int keep; } Slot;
static struct {
    Slot slots[SLOTS];
    u32 buffer_event, vsync_event;
    CONDITION_VARIABLE freed, queued;   /* (queued: SSBU_SPEED's consumer waits on it, and rollback's catch-up frames) */
    SRWLOCK lock;
    int started;
    u64 frames;
} queue = {.lock = SRWLOCK_INIT};

u64 present_frames(void) { return queue.frames; }

/* Who holds the queue's lock, and since when (freeze reports: an SRW lock has no owner field). */
static volatile DWORD queue_holder;
static volatile u64 queue_held_since;
static void queue_lock(void) {
    AcquireSRWLockExclusive(&queue.lock);
    queue_held_since = GetTickCount64();
    queue_holder = GetCurrentThreadId();
}
static void queue_unlock(void) {
    queue_holder = 0;
    ReleaseSRWLockExclusive(&queue.lock);
}
DWORD present_queue_holder(u64 *since_ms) { DWORD t = queue_holder; *since_ms = queue_held_since; return t; }

/* ---- the consumer: vsync -------------------------------------------------------- */
extern volatile LONG rb_fast;   /* (kernel.c; hle_rb_core.c sets it) frames still to run again after a load */
volatile u64 rb_vsync_gpu_wait_qpc, rb_vsync_gpu_waits;   /* (the profile: this thread's GPU waits in fast mode) */
/* (M6: refreshes to leave out, the net mode's holds for the frame advantage: the refresh comes and goes with
 * nothing signalled or shown, so the game's frame end waits one more and the game really is a frame later. A
 * Sleep(16) at the safe point did nothing of the kind: the vsync event stays signalled until the game takes it,
 * so a frame of ~6 ms work plus 16 ms asleep still made the next refresh, and the game ahead stayed ahead:
 * twopc15, 276 holds and PC 1 still 4-6 frames ahead the whole match.) */
volatile LONG rb_vsync_skip;
volatile LONG64 rb_vsync_skipped;
/* (rollback: a catch-up's last frame, the game's frame now, is kept to be shown at the next refresh as on
 * its first pass; until then the vsync thread is out of fast mode. hle_rb_core.c says when a catch-up is a
 * rollback's - run-ahead's hidden frames are fast too - and which slot each of its frames had.) */
int rb_vi_catchup(void);
int rb_dequeue_pick(u64 freemask, int dflt);
static volatile int keep_pending;
static u8 shown[sizeof queue.slots[0].buffer];
static u32 shown_size;
static u64 shown_mark;
static int speed_mode, report_draws;   /* SSBU_SPEED (not 1), SSBU_SEQ: decided at start */

/* SSBU_LAT_TRACE=<file> (tests: tools/latency_trace.py): the times, in ms, of each refresh signalled to
 * the game ("R"), each frame it queues ("Q"), each frame shown ("S <ms before the present> <ms after>
 * <frames shown> <fight frame>") and each change of player 1's buttons as the game reads them ("P <ms>
 * <frames shown> <fight frame> <buttons>"): with SSBU_PRESENT_HASH of the same run and of one without
 * the press, the delay from a press to the first picture that shows it. */
extern volatile long long test_fight_frame;
static FILE *lat_tf;
static SRWLOCK lat_lock = SRWLOCK_INIT;
static double lat_ms(void) { LARGE_INTEGER t, f; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f); return (double)t.QuadPart * 1000.0 / (double)f.QuadPart; }
static void lat_open(void) {
    const char *t = getenv("SSBU_LAT_TRACE");
    if (t && *t && !(lat_tf = fopen(t, "w"))) fprintf(stderr, "[latency] SSBU_LAT_TRACE: cannot write %s\n", t);
}
static void lat_line(const char *fmt, ...) {
    if (!lat_tf) return;
    va_list a;
    va_start(a, fmt);
    AcquireSRWLockExclusive(&lat_lock);
    vfprintf(lat_tf, fmt, a);
    ReleaseSRWLockExclusive(&lat_lock);
    va_end(a);
}
/* (hle_hid.c: player 1's buttons as the game reads them) */
void vi_lat_poll(u64 buttons) {
    static u64 last = ~0ull;
    if (!lat_tf || buttons == last) return;
    last = buttons;
    lat_line("P %.3f %llu %lld %llx\n", lat_ms(), (unsigned long long)queue.frames, test_fight_frame, (unsigned long long)buttons);
}

/* Under the queue's lock: the oldest queued buffer becomes the one shown (copied to `shown`) and
 * the one shown before is free again. Returns its slot, or -1 if none is queued. */
static int take_oldest(void) {
    int oldest = -1;
    for (int i = 0; i < SLOTS; i++)
        if (queue.slots[i].state == QUEUED && (oldest < 0 || queue.slots[i].queued_at < queue.slots[oldest].queued_at)) oldest = i;
    if (oldest >= 0) {
        for (int i = 0; i < SLOTS; i++) if (queue.slots[i].state == ACQUIRED) queue.slots[i].state = FREE;
        queue.slots[oldest].state = ACQUIRED;
        if (queue.slots[oldest].keep) { queue.slots[oldest].keep = 0; keep_pending = 0; }
        queue.frames++;
        shown_size = queue.slots[oldest].buffer_size;
        memcpy(shown, queue.slots[oldest].buffer, shown_size);
        shown_mark = queue.slots[oldest].gpu_mark;
    }
    return oldest;
}

/* Shown outside the queue's lock: a present can wait for the graphics card (frames behind), and
 * meanwhile the game queues its next frame and goes on with the one after. */
static LONG64 show_waited;   /* (low_latency: the last show's wait for the card, in performance-counter ticks) */
static void show(void) {
    LARGE_INTEGER w0, w1;
    QueryPerformanceCounter(&w0);
    gpu_queue_wait(shown_mark);   /* the frame drawn: what was submitted before it was queued has run */
    QueryPerformanceCounter(&w1);
    show_waited = w1.QuadPart - w0.QuadPart;
    if (report_draws) input_seq_presented(gpu_queue_draws(shown_mark));
    double p0 = lat_tf ? lat_ms() : 0;
    present_frame(shown, shown_size);
    test_fight_presented();
    if (lat_tf) lat_line("S %.3f %.3f %llu %lld\n", p0, lat_ms(), (unsigned long long)queue.frames, test_fight_frame);
    kernel_signal_event(queue.buffer_event);
    WakeAllConditionVariable(&queue.freed);
}

/* SSBU_SPEED (docs/TESTING-RUNS.md): no clock of refreshes. Each frame the game queues is shown
 * at its time - n x 60 a second, or at once when uncapped - and makes exactly one refresh, so
 * the game counts one refresh per frame shown however fast this PC is. With nothing queued there
 * is no refresh, however long that lasts (a load, a shader compile): the game would count one as
 * a dropped frame and run an extra logic frame. ssbu-recomp ran no vsync thread at all in this
 * mode and the game still booted to a fight (ssbu-recomp/tests/DEBUG_MODE.md). Meanwhile the
 * loop only wakes to write the second's line. */
static void speed_loop(void) {
    u64 frames = 0, second = GetTickCount64();
    for (;;) {
        queue_lock();
        int queued = 0;
        for (;;) {
            for (int i = 0; i < SLOTS && !queued; i++) queued = queue.slots[i].state == QUEUED;
            u64 waited = GetTickCount64() - second;
            if (queued || waited >= 1000) break;
            queue_holder = 0;   /* (asleep, not holding it) */
            SleepConditionVariableSRW(&queue.queued, &queue.lock, (DWORD)(1000 - waited), 0);
            queue_held_since = GetTickCount64();
            queue_holder = GetCurrentThreadId();
        }
        queue_unlock();
        if (queued) {
            display_speed_tick();   /* (only this thread takes a queued buffer: it is still there) */
            queue_lock();
            int oldest = take_oldest();
            queue_unlock();
            if (oldest >= 0) {
                show();
                frames++;
                kernel_signal_event(queue.vsync_event);   /* the frame's one refresh */
            }
        }
        u64 now = GetTickCount64();
        if (now - second >= 1000) {
            fprintf(stderr, "[display] speed: %llu frames shown in the last %.2f s (%.0f a second)\n", (unsigned long long)frames,
                    (now - second) / 1000.0, frames * 1000.0 / (double)(now - second));
            frames = 0;
            second = now;
        }
    }
}

/* (rollback: hle_rb_core.c) A frame of a catch-up after a load is drawn and not shown: the GPU
 * waited for (timed, for the rollback profile), the buffer given back. */
static void show_fast(void) {
    LARGE_INTEGER g0, g1;
    QueryPerformanceCounter(&g0);
    gpu_queue_wait(shown_mark);
    QueryPerformanceCounter(&g1);
    rb_vsync_gpu_wait_qpc += (u64)(g1.QuadPart - g0.QuadPart);
    rb_vsync_gpu_waits++;
    kernel_signal_event(queue.buffer_event);
    WakeAllConditionVariable(&queue.freed);
}

/* ---- low_latency (display.ini; ssbu-recomp's latency = low) -----------------------------
 * The game starts a frame when the refresh is signalled, reads the controller, and queues the frame.
 * With low_latency the signal comes later than the refresh, by what the frame doesn't need: a frame
 * period less the longest release-to-queue time of the last 64 frames and a margin for the graphics
 * card (SSBU_LOW_LATENCY_MARGIN ms, default 4). The controller is read that much closer to the
 * refresh that shows the frame. The refresh count the game reads is the same, so its logic does not
 * change. A frame that was released late and then missed its refresh (nothing queued, or the card
 * still drawing it) turns the delay off for a second. Not with SSBU_SPEED or in a rollback's catch-up. */
static LONG64 ll_freq, ll_spans[64], ll_release, ll_margin;
static int ll_next, ll_count, ll_backoff, ll_delayed;
static volatile LONG64 ll_late_frames, ll_delayed_frames, ll_delay_sum;

static LONG64 qpc_now(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

static void ll_queued(void) {   /* (QUEUE_BUFFER, under the queue's lock) */
    if (!ll_release) return;
    ll_spans[ll_next] = qpc_now() - ll_release;
    ll_next = (ll_next + 1) % 64;
    if (ll_count < 64) ll_count++;
    ll_release = 0;
}

static LONG64 ll_delay(void) {   /* ticks to hold the signal past the refresh */
    if (!ll_freq) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        ll_freq = f.QuadPart;
        const char *m = getenv("SSBU_LOW_LATENCY_MARGIN");
        ll_margin = (LONG64)((m && atof(m) >= 0 ? atof(m) : 4.0) * ll_freq / 1000);
        if (display_low_latency()) fprintf(stderr, "[display] low_latency: the game starts each frame as late as it can (margin %.1f ms)\n", (double)ll_margin * 1000 / ll_freq);
    }
    if (!display_low_latency() || speed_mode || ll_count < 16) return 0;
    if (ll_backoff > 0) { ll_backoff--; return 0; }
    LONG64 most = 0;
    for (int i = 0; i < ll_count; i++) if (ll_spans[i] > most) most = ll_spans[i];
    LONG64 d = ll_freq / 60 - most - ll_margin;
    return d > ll_freq / 2000 ? d : 0;   /* (under half a millisecond: not worth it) */
}

static void ll_wait_until(LONG64 at) {
    for (;;) {
        LONG64 left = at - qpc_now();
        if (left <= 0) return;
        LONG64 ms = left * 1000 / ll_freq;
        if (ms >= 2) Sleep((DWORD)(ms - 1)); else SwitchToThread();
    }
}

static DWORD WINAPI vsync_thread(LPVOID unused) {
    (void)unused;
    SetThreadDescription(GetCurrentThread(), L"vsync");
    timeBeginPeriod(1);
    if (speed_mode) speed_loop();
    for (;;) {
        /* (rollback build, M4: while rb_fast, the frames a load runs again are neither paced nor
         * shown: each is taken off the queue as soon as it is drawn, and the next refresh is now) */
        int fast = rb_fast > 0 && !keep_pending;
        if (!fast) display_wait_tick();   /* the next refresh (display.ini's pacing) */
        if (!fast && rb_vsync_skip > 0) { InterlockedDecrement(&rb_vsync_skip); InterlockedIncrement64(&rb_vsync_skipped); continue; }
        queue_lock();
        int oldest = take_oldest();
        /* (rollback, fast and nothing queued: wait for the game's next frame - up to 2 ms, then a refresh
         * all the same - rather than signal refreshes in a spin: thousands of them, each through the
         * kernel's lock, and the game counted them as dropped refreshes) */
        if (fast && oldest < 0) {
            queue_holder = 0;   /* (asleep, not holding it) */
            BOOL woken = SleepConditionVariableSRW(&queue.queued, &queue.lock, 2, 0);
            queue_held_since = GetTickCount64();
            queue_holder = GetCurrentThreadId();
            if (woken) { queue_unlock(); continue; }
        }
        queue_unlock();
        LONG64 tick = qpc_now();
        if (oldest >= 0) {
            if (fast) show_fast();
            else {
                show();
                if (ll_delayed && show_waited > ll_freq / 1000) { ll_backoff = 60; ll_late_frames++; }   /* (the card was still drawing it) */
            }
        } else if (!fast && ll_delayed) { ll_backoff = 60; ll_late_frames++; }   /* (released late, and not queued in time) */
        LONG64 d = fast ? 0 : ll_delay();
        ll_delayed = d > 0;
        if (d) { ll_wait_until(tick + d); ll_delayed_frames++; ll_delay_sum += d; }
        static u64 ll_said;
        if (display_low_latency() && !fast && GetTickCount64() - ll_said >= 60000) {
            if (ll_said) fprintf(stderr, "[display] low_latency: %lld frames started later than the refresh (%.1f ms on average), %lld missed it and turned it off for a second\n",
                                 (long long)ll_delayed_frames, ll_delayed_frames ? (double)ll_delay_sum * 1000 / ll_freq / (double)ll_delayed_frames : 0.0,
                                 (long long)ll_late_frames);
            ll_said = GetTickCount64();
        }
        queue_lock();
        ll_release = fast ? 0 : qpc_now();
        queue_unlock();
        if (lat_tf && !fast) lat_line("R %.3f\n", lat_ms());
        kernel_signal_event(queue.vsync_event);
    }
}

static void start(void) {
    if (queue.started) return;
    queue.started = 1;
    InitializeConditionVariable(&queue.freed);
    InitializeConditionVariable(&queue.queued);
    queue.buffer_event = kernel_create_event();
    queue.vsync_event = kernel_create_event();
    double speed = display_speed();
    speed_mode = speed != 1;
    report_draws = input_seq_on();
    lat_open();
    if (speed_mode) fprintf(stderr, "[display] SSBU_SPEED=%g: one refresh per frame shown and none without one, %s\n", speed, speed > 0 ? "paced at n x 60 a second" : "uncapped");
    CreateThread(NULL, 0, vsync_thread, NULL, 0, NULL);
}

/* (rollback build, M2) The buffer queue's slot states for a savestate. The game's memory says which
 * buffers it has dequeued and queued; after a load the queue must say the same, or the buffers it
 * forgot stay dequeued (two loads left none free: DEQUEUE_BUFFER said "would block" for good) and
 * stale queued ones are shown between new frames. The registered buffers themselves don't change. */
typedef struct ViSlot { int state; u64 queued_at, gpu_mark; } ViSlot;
void *vi_rb_save(void) {
    ViSlot *s = malloc(sizeof *s * SLOTS);
    queue_lock();
    for (int i = 0; i < SLOTS; i++) { s[i].state = queue.slots[i].state; s[i].queued_at = queue.slots[i].queued_at; s[i].gpu_mark = queue.slots[i].gpu_mark; }
    queue_unlock();
    return s;
}

void vi_rb_load(const void *p) {
    const ViSlot *s = p;
    queue_lock();
    for (int i = 0; i < SLOTS; i++) { queue.slots[i].state = s[i].state; queue.slots[i].queued_at = s[i].queued_at; queue.slots[i].gpu_mark = s[i].gpu_mark; queue.slots[i].keep = 0; }
    keep_pending = 0;
    /* (a rollback's load: the frames the save had queued, and the one shown, were shown on the first pass. Let
     * go now - a catch-up presents nothing - rather than left queued: their slots were free again for the
     * catch-up's frames on the first pass, and the oldest of them was shown after the catch-up, a picture
     * from before the rollback. No refreshes for them: the game counted those as late frames and ran extra.) */
    if (rb_vi_catchup())
        for (int i = 0; i < SLOTS; i++) if (queue.slots[i].state == QUEUED || queue.slots[i].state == ACQUIRED) queue.slots[i].state = FREE;
    queue_unlock();
    if (queue.started) { kernel_signal_event(queue.buffer_event); WakeAllConditionVariable(&queue.freed); }
}

/* ---- parcels --------------------------------------------------------------------- */
/* A flattened parcel: { u32 data size, data offset, objects size, objects offset } + data. */
typedef struct In { const u8 *d; u32 size, pos; } In;
typedef struct Out { u8 d[0x800]; u32 size; } Out;

static u32 rd32(In *p) { u32 v = 0; if (p->pos + 4 <= p->size) memcpy(&v, p->d + p->pos, 4); p->pos += 4; return v; }
static void skip_token(In *p) {   /* strict-mode policy, then the interface name as String16 */
    rd32(p);
    i32 len = (i32)rd32(p);
    if (len > 0) p->pos += ((u32)(len + 1) * 2 + 3) & ~3u;
}
static void wr(Out *o, const void *v, u32 n) { memcpy(o->d + o->size, v, n); o->size += (n + 3) & ~3u; }
static void w32(Out *o, u32 v) { wr(o, &v, 4); }

static u32 parcel_out(Out *o, u64 dst, u64 cap) {
    u32 total = 16 + o->size;
    if (!dst || total > cap) return 0;
    u32 h[4] = {o->size, 16, 0, 16 + o->size};
    memcpy(P(void, dst), h, 16);
    memcpy(P(void, dst + 16), o->d, o->size);
    return total;
}

static void queue_output(Out *o) {   /* QueueBufferOutput: width, height, transform hint, pending */
    u32 pending = 0;
    for (int i = 0; i < SLOTS; i++) pending += queue.slots[i].state == QUEUED;
    w32(o, WIDTH); w32(o, HEIGHT); w32(o, 0); w32(o, pending);
}

static void transact(u32 code, In *in, Out *out, Cpu *c) {
    int fast_freed = 0;   /* (rollback build: a frame of a catch-up queued, freed at once) */
    (void)c;
    skip_token(in);
    queue_lock();
    switch (code) {
    case CONNECT:
        queue_output(out);
        w32(out, 0);
        kernel_signal_event(queue.buffer_event);
        break;
    case DISCONNECT: case SET_BUFFER_COUNT: case ALLOCATE_BUFFERS:
        w32(out, 0);
        break;
    case SET_PREALLOCATED_BUFFER: {
        u32 slot = rd32(in);
        if (slot < SLOTS) {
            Slot *s = &queue.slots[slot];
            s->buffer_size = in->size - in->pos < sizeof s->buffer ? in->size - in->pos : (u32)sizeof s->buffer;
            memcpy(s->buffer, in->d + in->pos, s->buffer_size);
            s->has_buffer = s->buffer_size >= 4 && *(u32 *)s->buffer != 0;
            s->state = FREE;
            kernel_signal_event(queue.buffer_event);   /* a buffer is available */
        }
        w32(out, 0);
        break;
    }
    case REQUEST_BUFFER: {   /* the buffer exactly as it was registered */
        u32 slot = rd32(in);
        if (slot < SLOTS && queue.slots[slot].has_buffer) wr(out, queue.slots[slot].buffer, queue.slots[slot].buffer_size);
        else w32(out, 0);
        w32(out, 0);
        break;
    }
    case DEQUEUE_BUFFER: {   /* never blocks: "would block" sends the client back to the event */
        int slot = -1;
        u64 freemask = 0;
        for (int i = 0; i < SLOTS; i++)
            if (queue.slots[i].has_buffer && queue.slots[i].state == FREE) { freemask |= 1ull << i; if (slot < 0) slot = i; }
        if (slot >= 0) slot = rb_dequeue_pick(freemask, slot);   /* (a catch-up's frame: its first pass's slot) */
        if (slot < 0) {
            w32(out, 0xFFFFFFFF);
            w32(out, 0);
            w32(out, (u32)-11);
            break;
        }
        queue.slots[slot].state = DEQUEUED;
        w32(out, (u32)slot);
        w32(out, 1);                       /* a fence follows: flattened, 0x24 bytes, no file descriptors */
        w32(out, 0x24); w32(out, 0);
        u8 fence[0x24] = {0};              /* no fences: the buffer is free now */
        wr(out, fence, sizeof fence);
        w32(out, 0);
        break;
    }
    case QUEUE_BUFFER: {
        u32 slot = rd32(in);
        if (slot < SLOTS) {
            queue.slots[slot].state = QUEUED;
            queue.slots[slot].queued_at = rt_now_ns();
            ll_queued();
            if (lat_tf && rb_fast <= 0) lat_line("Q %.3f\n", lat_ms());
            queue.slots[slot].gpu_mark = gpu_queue_mark();
            /* (rollback build, M4: a frame of a catch-up is not shown. Freed here and the game told at
             * once - buffer free, a refresh - rather than through the vsync thread: the game's frame end
             * waited 0.9 ms a re-run frame for that round trip. SSBU_RB_QFREE=0: through the thread.) */
            static int qfree = -1;
            if (qfree < 0) { const char *e = getenv("SSBU_RB_QFREE"); qfree = !(e && *e == '0'); }
            queue.slots[slot].keep = 0;
            if (rb_fast == 1 && rb_vi_catchup()) {   /* (a catch-up's last frame: shown at the next refresh) */
                queue.slots[slot].keep = 1;
                keep_pending = 1;
                WakeConditionVariable(&queue.queued);
            } else if (qfree && rb_fast > 0) {
                for (int i = 0; i < SLOTS; i++) if (queue.slots[i].state == ACQUIRED) queue.slots[i].state = FREE;
                queue.slots[slot].state = FREE;
                queue.frames++;
                fast_freed = 1;
            } else if (speed_mode || rb_fast > 0) WakeConditionVariable(&queue.queued);
        }
        queue_output(out);
        w32(out, 0);
        break;
    }
    case CANCEL_BUFFER: {
        u32 slot = rd32(in);
        if (slot < SLOTS) queue.slots[slot].state = FREE;
        kernel_signal_event(queue.buffer_event);
        w32(out, 0);
        break;
    }
    case QUERY: {
        u32 what = rd32(in), value = 0;
        switch (what) {
        case 0: case 6: value = WIDTH; break;
        case 1: case 7: value = HEIGHT; break;
        case 2: value = 1; break;      /* RGBA 8888 */
        case 3: value = 1; break;      /* minimum undequeued buffers */
        case 5: value = 1; break;      /* a surface */
        }
        w32(out, value);
        w32(out, 0);
        break;
    }
    case GET_BUFFER_HISTORY:
        w32(out, 0);
        w32(out, 0);
        break;
    default:
        fprintf(stderr, "[vi] buffer queue: transaction %u not implemented\n", code);
        w32(out, 0);
    }
    queue_unlock();
    if (fast_freed) {
        kernel_signal_event(queue.buffer_event);
        WakeAllConditionVariable(&queue.freed);
        kernel_signal_event(queue.vsync_event);
    }
}

/* ---- IHOSBinderDriver ------------------------------------------------------------ */
static void binder(IpcObject *self, Ipc *ipc) {
    switch (ipc->command) {
    case 0: case 3: {   /* TransactParcel(id, code, flags) in -> out, and its "auto" variant */
        u32 code = ipc_u32(ipc, 4);
        u64 in_size, out_size;
        u64 in_addr = ipc_in_buffer(ipc, 0, &in_size), out_addr = ipc_out_buffer(ipc, 0, &out_size);
        In in = {0};
        if (in_addr && in_size >= 16) {
            const u32 *h = P(u32, in_addr);
            in.d = P(u8, in_addr + h[1]);
            in.size = h[0];
        }
        Out out = {0};
        transact(code, &in, &out, ipc->cpu);
        ipc_wrote(ipc, 0, parcel_out(&out, out_addr, out_size));
        return;
    }
    case 1: return;   /* AdjustRefcount */
    case 2:           /* GetNativeHandle(id, type): the "buffer released" event */
        ipc_copy_handle(ipc, queue.buffer_event);
        return;
    }
    ipc_unknown(ipc, self);
}

/* ---- the display services -------------------------------------------------------- */
/* The parcel naming the layer's binder object: a 0x28-byte flat binder (type, process,
 * 64-bit binder id, the relay service's name) at offset 0, listed in the object table so
 * Parcel::readStrongBinder accepts it. */
static u32 native_window(u64 dst, u64 cap) {
    u32 w[10] = {2, 1, BINDER_ID, 0, 0, 0, 0, 0, 0, 0};
    memcpy(&w[6], "dispdrv", 8);
    u32 total = 16 + sizeof w + 4;
    if (!dst || cap < total) return 0;
    u32 h[4] = {sizeof w, 16, 4, 16 + sizeof w};
    memcpy(P(void, dst), h, 16);
    memcpy(P(void, dst + 16), w, sizeof w);
    st32(dst + 16 + sizeof w, 0);   /* object table: the binder is at data offset 0 */
    return total;
}

static void display_service(IpcObject *self, Ipc *ipc);

static void root(IpcObject *self, Ipc *ipc) {
    if (ipc->command <= 2) {   /* GetDisplayService, whichever root this is */
        ipc_object(ipc, ipc_new_object("vi:IApplicationDisplayService", display_service, NULL));
        return;
    }
    ipc_unknown(ipc, self);
}

static u32 native_window(u64 dst, u64 cap);

static void sub_service(IpcObject *self, Ipc *ipc) {
    switch (ipc->command) {
    case 2010: IPC_PUSH(ipc, (u64)LAYER_ID); return;   /* CreateManagedLayer */
    case 2012: {   /* CreateStrayLayer(flags, display) -> layer, native window parcel */
        u64 size, b = ipc_out_buffer(ipc, 0, &size);
        u32 n = native_window(b, size);
        ipc_wrote(ipc, 0, n);
        IPC_PUSH(ipc, (u64)LAYER_ID);
        IPC_PUSH(ipc, (u64)n);
        return;
    }
    case 3200: {   /* GetDisplayMode: width, height, refresh rate, mode */
        u32 mode[4] = {WIDTH, HEIGHT, 0, 0};
        f32 hz = 60.0f;
        memcpy(&mode[2], &hz, 4);
        ipc_push(ipc, mode, 16);
        return;
    }
    }
    if (ipc->command < 2000 || ipc->command >= 9000) ipc_unknown(ipc, self);
    /* the rest are layer and display property setters: accepted */
}

static void display_service(IpcObject *self, Ipc *ipc) {
    start();
    switch (ipc->command) {
    case 100: case 103:   /* GetRelayService / GetIndirectDisplayTransactionService */
        ipc_object(ipc, ipc_new_object("vi:IHOSBinderDriver", binder, NULL));
        return;
    case 101: ipc_object(ipc, ipc_new_object("vi:ISystemDisplayService", sub_service, NULL)); return;
    case 102: ipc_object(ipc, ipc_new_object("vi:IManagerDisplayService", sub_service, NULL)); return;
    case 1000: {   /* ListDisplays -> count; DisplayInfo { name[0x40]; bool; pad; u64 layers, width, height } */
        u64 size, b = ipc_out_buffer(ipc, 0, &size);
        if (b && size >= 0x60) {
            memset(P(void, b), 0, 0x60);
            memcpy(P(void, b), "Default", 8);
            st8(b + 0x40, 1);
            st64(b + 0x48, 1); st64(b + 0x50, WIDTH); st64(b + 0x58, HEIGHT);
            ipc_wrote(ipc, 0, 0x60);
        }
        IPC_PUSH(ipc, (u64)1);
        return;
    }
    case 1010: case 1011: IPC_PUSH(ipc, (u64)DISPLAY_ID); return;   /* OpenDisplay / OpenDefaultDisplay */
    case 1020: case 1101: case 2021: case 2031: case 2101: return;   /* close / enable / close layer ... */
    case 1102: IPC_PUSH(ipc, (u64)WIDTH); IPC_PUSH(ipc, (u64)HEIGHT); return;   /* GetDisplayResolution */
    case 2020: {   /* OpenLayer(name, layer, applet) -> native window parcel */
        u64 size, b = ipc_out_buffer(ipc, 0, &size);
        u32 n = native_window(b, size);
        ipc_wrote(ipc, 0, n);
        IPC_PUSH(ipc, (u64)n);
        return;
    }
    case 2030: {   /* CreateStrayLayer(flags, display) -> layer, native window parcel */
        u64 size, b = ipc_out_buffer(ipc, 0, &size);
        u32 n = native_window(b, size);
        ipc_wrote(ipc, 0, n);
        IPC_PUSH(ipc, (u64)LAYER_ID);
        IPC_PUSH(ipc, (u64)n);
        return;
    }
    case 2102: IPC_PUSH(ipc, (u64)ipc_u32(ipc, 0)); return;   /* ConvertScalingMode */
    case 5202: case 5203: ipc_copy_handle(ipc, queue.vsync_event); return;   /* GetDisplayVsyncEvent */
    }
    ipc_unknown(ipc, self);
}

IpcObject *service_vi(const char *name) {
    if (!strcmp(name, "vi:u") || !strcmp(name, "vi:s") || !strcmp(name, "vi:m"))
        return ipc_new_object(_strdup(name), root, NULL);
    return NULL;
}
