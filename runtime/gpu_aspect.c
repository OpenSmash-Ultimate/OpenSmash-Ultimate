/* A picture wider than 16:9: the renderers' shared decisions (gpu_aspect.h, docs/ULTRAWIDE.md). Every
 * 16:9-shaped render target is made wider by the picture's widening (aspect_widening), so the scene shows
 * more at the sides; each draw on one is then squeezed back, clipped and moved here (aspect_draw), and
 * Direct3D 11 and Vulkan apply the same answer. */
#ifndef __APPLE__
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "display.h"
#include "gpu_aspect.h"

int aspect_native(void);   /* (host.c: the game draws the 3D wide itself) */
int hud_wide_on(void);     /* (host.c: in a fight's scene the HUD reaches the picture's edges) */
int game_fight_on(void);   /* (hle_fight_test.c) */
extern u64 gfx_stat_presented;   /* (the back end's: frames shown) */

float aspect_widening(u32 w, u32 h) {
    return (u64)w * 9 == (u64)h * 16 ? display_aspect() / (16.0f / 9) : 1.0f;
}

/* (research, a wider picture: F9 steps through the kinds of squeezed 2D draw - vertex and pixel program -
 * drawn in the last second, leaving one unsqueezed: which kind the P1/CPU tags are) */
static const void *pick_vs[64], *pick_ps[64];
static const char *pick_vs_name[64], *pick_ps_name[64];
static u64 pick_seen[64];
static int npick, pick = -1;

void aspect_pick_next(void) {
    int k = pick;
    do k = k + 1 >= npick ? -1 : k + 1;
    while (k >= 0 && gfx_stat_presented - pick_seen[k] > 60);
    pick = k;
    if (k < 0) fprintf(stderr, "[aspect] F9: every 2D draw squeezed\n");
    else fprintf(stderr, "[aspect] F9: not squeezed: %s %s\n", pick_vs_name[k], pick_ps_name[k] ? pick_ps_name[k] : "(no ps)");
}

void aspect_draw(const AspectDraw *d, AspectDecision *o) {
    o->fix0 = 1.0f; o->clip = 0; o->shift = 0; o->shift_by = 0;
    if (d->wide <= 1.0f) return;
    /* On a widened target every draw is squeezed back to the game's 16:9 shape (x times 16:9 over the
     * aspect: 3D then keeps its proportions and shows more at the sides; the HUD sits in the middle 16:9,
     * over the fighters its tags follow), except a full-screen pass, which must cover the whole width: one
     * that reads a widened target with no depth test (tone mapping, bloom, the lighting passes). 3D effects
     * that read the scene (soft particles) test depth. A full-screen pass may test depth with ALWAYS to
     * write it: the half-size depth copy the smoke and other effects are tested against.
     * ponytail: per-draw heuristic, no shader change (a new vertex shader costs every player ~1 h of
     * compiling); a fade without a texture is squeezed too - check one if bars show */
    int depth_tests = d->depth_on && !d->depth_always;
    /* (a wider picture: the game draws 3D wide itself; only 2D that is not a full-screen pass is squeezed) */
    int squeeze = aspect_native() ? !d->depth_on && !d->reads_wide : !(d->reads_wide && !depth_tests);
    if (squeeze && !d->depth_on && aspect_native()) {   /* (the F9 picker) */
        int k = 0;
        while (k < npick && !(pick_vs[k] == d->vs && pick_ps[k] == d->ps)) k++;
        if (k == npick && npick < 64) { pick_vs[k] = d->vs; pick_ps[k] = d->ps; pick_vs_name[k] = d->vs_name; pick_ps_name[k] = d->ps_name; npick++; }
        if (k < 64) pick_seen[k] = gfx_stat_presented;
        if (k == pick) squeeze = 0;
    }
    if (!squeeze) return;
    o->fix0 = 1.0f / d->wide;
    if (d->depth_on) return;   /* (3D keeps the full width) */
    /* A 2D draw (no depth test: the HUD, menus) stays inside the middle 16:9: the game parks hidden panels
     * just off its screen, which the squeeze would bring into view. Its scissor (in the game's pixels) is
     * squeezed towards the middle with it. In a fight the HUD's panels sit where their layouts anchor them,
     * out to the picture's edges (host.c, pane_place): there the 2D reaches the whole picture, ±960 x the
     * widening. */
    float cx = (float)d->rt_w / 2, reach = hud_wide_on() ? cx * d->wide : cx;
    float l = cx - reach, r = cx + reach;
    if (d->scissor & 1) { l = (float)(d->scissor_lr & 0xFFFF); r = (float)(d->scissor_lr >> 16); }
    if (l <= 0 && r >= (float)d->rt_w) { l = cx - reach; r = cx + reach; }   /* (the game's whole screen: in a fight, the whole picture) */
    /* (a scissor's left is unsigned, so one past the screen's left edge comes as 0, where its right past the other edge
     * stays past it: a mask moved out to the left (pane_place) was cut at the 16:9 edge - the vs screen's left background
     * panel, the waiting room's P1 slot (owner, 2026-11-15). 0 reaches as far as the right does: the picture's edge in a
     * fight, the screen's elsewhere) */
    if (l <= 0 || l < cx - reach) l = cx - reach;
    if (r > cx + reach) r = cx + reach;   /* (a scissor past the screen still stops at its edge) */
    o->clip = 1;
    o->clip_l = (int)floorf((cx + (l - cx) / d->wide) * d->rt_sx);
    o->clip_r = (int)ceilf((cx + (r - cx) / d->wide) * d->rt_sx);
    /* A magnifying bubble at a side of the 16:9 view is moved out to the same side of the picture, as far
     * from its edge as it was from the 16:9 one (docs/ULTRAWIDE-CONTEXT.md). Known by its portrait - a
     * program of its own, the fighter's picture - and the two draws right after it at the same place (ring,
     * arrow). The element's x, in the HUD's pixels from the middle, is in its vertex bank 3 at +0x4c, or
     * +0x5c (the ring). When the bubble shows stays the game's. Not in the wide view: there the fighter is
     * still in the picture past the 16:9 edge, and a bubble at the picture's edge looks like the fighter
     * jumped there (owner, 2026-11-11). Fights only: menus draw replay thumbnails and selection frames with
     * the same programs, and the left tile sat where a bubble would - its frame was moved out to the
     * picture's edge (owner, 2026-11-12). */
    static int bubble_left;
    static float bubble_t, anchor;
    if (!aspect_native() || display_view() == VIEW_WIDE || !game_fight_on() || !d->vcb3 || d->vcb3_size < 0x60) return;
    float row[8];
    d->read(d->vcb3 + 0x40, sizeof row, row);
    int portrait = d->vs_name && !strncmp(d->vs_name, "vs_545fbedf170d65dd", 19);
    if (portrait) {
        bubble_left = 0;
        float x = row[3], w = d->wide;
        if (fabsf(x) > 960 * 0.55f && fabsf(x) < 1200) {
            float sgn = x < 0 ? -1.0f : 1.0f, target = sgn * (1 - (960 - fabsf(x)) / (960 * w));
            bubble_t = target - x / (960 * w);
            bubble_left = 3;
            anchor = x;
        }
    }
    if (bubble_left <= 0) return;
    if (portrait || fabsf(row[3] - anchor) < 120 || fabsf(row[7] - anchor) < 120) {
        bubble_left--;
        o->shift = 1;
        o->shift_by = bubble_t;
        o->clip = 0;
    } else bubble_left = 0;
}
#endif
