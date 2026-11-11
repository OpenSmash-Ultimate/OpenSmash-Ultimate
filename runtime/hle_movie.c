/* The game's movie player (its own code in main, around the SDK's movie library): a race that
 * the Switch's scheduling hides.
 *
 * The player's audio renderer takes the audio decoder's outputs from the player's
 * "MovieDecoderEventHandler" thread, copies them into PCM buffers and hands those to the audio
 * out callback. Its Close frees `pending` (the decoder output it is part-way through) and zeroes
 * that pointer, under the renderer's lock; the lists of PCM buffers are emptied by the call
 * after it (main+0x36f6080), without the lock. The renderer's output step
 * (main+0x36f5720) returns at once while it has no free buffer, and otherwise reads `pending`.
 *
 * Stop ends the event thread before those two calls. Starting a movie over (the player's Start
 * with the movie at its end, main+0x36f26f0) makes them while the event thread keeps running,
 * so a decoder output that arrives between them meets free buffers and no `pending`: the event
 * thread reads through the null pointer ("reading address 0x28", or 0x4, in the output step).
 * It shows a few seconds after the game starts, as the title screen's movie begins. On
 * the Switch the two calls follow each other within a microsecond; here Windows can preempt the
 * player's thread between them for a whole time slice, which happens often on a busy PC.
 *
 * The fix: Close first empties those lists itself, under the lock - what the next call does
 * anyway. Then no free buffer is listed while `pending` is gone: only Open lists buffers again,
 * after it has made a new `pending`. */
#include <stdio.h>

#include "host.h"

#define RENDERER_FREE 0x70   /* std::vector<movie::Buffer>: PCM buffers to fill */
#define RENDERER_FULL 0x88   /* std::vector<movie::Buffer>: PCM buffers filled, for the audio out callback */
#define RENDERER_LOCK 0xB8   /* std::mutex */
#define BUFFER_SIZE 0x40     /* sizeof(movie::Buffer) */

static void empty_buffers(Cpu *c, u64 vector) {
    u64 begin = ld64(vector), end = ld64(vector + 8);
    while (end != begin) {
        end -= BUFFER_SIZE;
        rt_callsym(c, "_ZN5movie6BufferD1Ev", 1, end);
        st64(vector + 8, end);
    }
}

/* The audio renderer's Close. */
/* signature: 16 words, sha256 86bd7e6ce224f325 */
HLE_WRAP(71036f6400) {
    u64 renderer = c->x[0], lr = c->x[30];
    rt_callsym(c, "_ZNSt3__15mutex4lockEv", 1, renderer + RENDERER_LOCK);
    empty_buffers(c, renderer + RENDERER_FREE);
    empty_buffers(c, renderer + RENDERER_FULL);
    rt_callsym(c, "_ZNSt3__15mutex6unlockEv", 1, renderer + RENDERER_LOCK);
    c->x[0] = renderer;
    c->x[30] = lr;
    f_71036f6400_orig(c);
}

/* ---- the player's pre-roll ---------------------------------------------------------------
 * The player's Start (main+0x36f26f0) waits, before it starts the audio and video renderers,
 * until both have something to show: the video renderer more than one picture ([[player+0x58]]
 * +0xB0), the audio one more than 8 PCM buffers, 16 when it cuts the decoder's output into
 * fixed-size frames ([[player+0x68]]+0x110, +0x50) - as the SDK's movie sample does. It polls
 * that every 10 ms (nn::os::SleepThread), at most 50 times, 125 for Vorbis audio, then starts
 * anyway, holding the player's lock all the while (the game's main thread waits on it).
 *
 * The opening movie never gets there: its input is read in the file's order (the extractor
 * hands each sample to the track it belongs to), and the next sample after the first video
 * pictures is one more picture, which the video decoder has no room for until the renderer
 * shows one; the audio behind it is not read. So no audio buffer comes, and Start waits its
 * full 1.25 s, then starts the renderers with no audio, as it would after any timeout; the audio
 * comes within a few ms of the first picture shown. Measured headless (2026-11-11): the video
 * renderer had 3 pictures and the audio one 0 buffers at each of the 125 polls, then 16 buffers
 * 5 ms after the first picture.
 *
 * The fix: once the video is ready, a poll that finds the audio buffers no more than three polls
 * (30 ms) ago does not sleep. Start then goes through the polls left at once and starts as its
 * timeout would have it start, without the wait. Audio that is still coming in keeps the
 * polls sleeping, and Start waits for it as the game means it to. */
#define PLAYER_VIDEO 0x58           /* the video renderer's holder: its first word the renderer */
#define PLAYER_AUDIO 0x68           /* the audio renderer's holder */
#define VIDEO_PICTURES 0xB0         /* i64: pictures the video renderer has received */
#define AUDIO_FIXED_FRAMES 0x50     /* u8: the decoder's output cut into fixed-size frames */
#define AUDIO_BUFFERS 0x110         /* i64: PCM buffers the audio renderer has filled */
#define PREROLL_POLL 10000000ull    /* ns: Start's sleep between two polls */
#define PREROLL_STILL 3             /* polls without a new audio buffer that end the wait */

typedef struct Preroll { u64 player; i64 audio; u32 polls, still; u64 since; } Preroll;
static _Thread_local Preroll preroll;

/* signature: 16 words, sha256 653ad22cdc9a960a */
HLE_WRAP(71036f26f0) {
    Preroll outer = preroll;   /* (Start does not call itself; kept all the same) */
    preroll = (Preroll){c->x[0], -1, 0, 0, rt_now_ns()};
    f_71036f26f0_orig(c);
    preroll = outer;
}

/* From SleepThread (kernel.c): 1 if this sleep is a poll of Start's pre-roll that has nothing
 * left to wait for, and is not slept. */
int movie_preroll_skip(u64 ns) {
    Preroll *p = &preroll;
    if (!p->player || ns != PREROLL_POLL) return 0;
    u64 video = ld64(p->player + PLAYER_VIDEO), audio = ld64(p->player + PLAYER_AUDIO);
    if (video && (video = ld64(video)) && (i64)ld64(video + VIDEO_PICTURES) <= 1) { p->still = 0; return 0; }
    if (!audio || !(audio = ld64(audio))) return 0;
    i64 buffers = (i64)ld64(audio + AUDIO_BUFFERS);
    p->polls++;
    if (buffers != p->audio) { p->audio = buffers; p->still = 0; return 0; }
    if (++p->still < PREROLL_STILL) return 0;
    if (p->still == PREROLL_STILL) {
        static int said;
        if (said++ < 4)
            fprintf(stderr, "[movie] pre-roll: no new audio for %d polls with the video ready (%lld audio buffers, %s needed): "
                    "started %.0f ms into it, not after its timeout\n", PREROLL_STILL, (long long)buffers,
                    *P(u8, audio + AUDIO_FIXED_FRAMES) ? "over 16" : "over 8", (rt_now_ns() - p->since) / 1e6);
    }
    return 1;
}
