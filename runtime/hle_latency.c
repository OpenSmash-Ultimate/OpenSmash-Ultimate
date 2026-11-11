/* The game's own buffered frame (less-lag's "frame index"), chosen by SSBU_FRAME_INDEX.
 *
 * Idea from the community's Skyline mods that cut the game's offline delay: "less-lag" (Bluejay's
 * ssbusync, archived at github.com/The-Braxeo/ssbu-less-lag) and its successor in
 * project-ultelier/smash-ultelier's sync-guest, used by saad-script's ssbu-online-deluxe (AGPL-3.0).
 * Reimplemented here from what their hook does; no code taken.
 *
 * FUN 0x710386b0c0 (13.0.4: 0x710386ab10) picks the window texture the game's renderer draws the
 * frame into: entry (count + 2) % 3 of a ring of three at this+0xa0 (count at ring+0x30, entries
 * 0x20 bytes apart from ring+0x50), the instruction `add w9, w9, #2` at +0x3c. ssbusync's inline
 * hook there ("index mode") makes it + 1 ("one behind") or + 0 ("immediate"); vanilla is + 2 ("two
 * behind"). Here the wrap shifts the count the function reads for the time of the call and puts it
 * back after: the function itself only reads it.
 *
 * SSBU_FRAME_INDEX=1 (or unset): one behind, the default since 2026-11-11 - measured with
 * tools/latency_trace.py (Direct3D 11, same fight, 3 presses), a press reaches the screen 65 ms after
 * the game reads it instead of 82 (one frame less), the pictures the same, only a frame sooner;
 * 2: the game's own; 0: immediate (measured slower: 108 ms, the GPU waits on the texture it shows). */
#include <stdio.h>
#include <stdlib.h>

#include "host.h"

static int frame_index_shift(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("SSBU_FRAME_INDEX");
        int n = e && *e ? atoi(e) : 1;
        if (n < 0 || n > 2) n = 1;
        v = 2 - n;   /* (how far the count is moved back: the game adds 2) */
        if (v) fprintf(stderr, "[latency] frame index: %d behind (the game's own: 2)\n", n);
    }
    return v;
}

/* signature: 16 words, sha256 e5a536ff7e787dcd */
HLE_WRAP(710386b0c0) {
    int shift = frame_index_shift();
    u64 ring = shift ? ld64(c->x[0] + 0xa0) : 0;
    if (!ring) { f_710386b0c0_orig(c); return; }
    u32 count = ld32(ring + 0x30);
    st32(ring + 0x30, count + 3 - (u32)shift);   /* ((count + 3 - shift + 2) % 3 = (count + 2 - shift) % 3) */
    f_710386b0c0_orig(c);
    if (ld32(ring + 0x30) == count + 3 - (u32)shift) st32(ring + 0x30, count);
}
