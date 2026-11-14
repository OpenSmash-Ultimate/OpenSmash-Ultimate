/* A picture wider than 16:9 (docs/ULTRAWIDE.md): the decisions both renderers take, in one place so they
 * cannot drift (gpu_aspect.c). Each back end only applies them: Direct3D 11 (gpu_d3d11.c) and Vulkan
 * (gpu_vk.c). macOS (Metal) stays at 16:9 (display_shape_locked). */
#pragma once
#include "recomp.h"

/* How much wider than the game made it a w x h render target is drawn: the picture's widening for the
 * 16:9-shaped ones (the scene, the HUD; not shadow maps or other square ones), else 1. */
float aspect_widening(u32 w, u32 h);

/* One draw, as the back end sees it. */
typedef struct AspectDraw {
    float wide;          /* the first colour target's widening (aspect_widening; 1: none) */
    u32 rt_w;            /* that target's width, in the game's pixels */
    float rt_sx;         /* host pixels per game pixel across it (render scale times widening) */
    int depth_on;        /* the depth test on, with a depth target */
    int depth_always;    /* ... and its function ALWAYS (a pass that writes depth: the half-size depth copy) */
    int reads_wide;      /* the draw samples a widened target */
    u32 scissor;         /* the game's scissor 0: regs[M_SCISSOR(0)] (bit 0 on) */
    u32 scissor_lr;      /* ... its left | right << 16, regs[M_SCISSOR(0) + 1] */
    const void *vs, *ps; /* the programs (the F9 picker tells kinds apart by them) */
    const char *vs_name, *ps_name;
    u64 vcb3;            /* the vertex stage's constant bank 3 (GPU VA, 0: none), and its size */
    u32 vcb3_size;
    void (*read)(u64 va, u32 size, void *dst);   /* the back end's read of GPU memory */
} AspectDraw;

/* What the back end does with it. */
typedef struct AspectDecision {
    float fix0;          /* params.fix[0] times this: 1, or the squeeze (1 / widening) */
    int clip;            /* 1: the scissor's x range is [clip_l, clip_r), host pixels (2D held to the picture's reach) */
    int clip_l, clip_r;
    int shift;           /* 1: the viewport's x moved by shift * its width / 2, the scissor the whole target (a bubble) */
    float shift_by;
} AspectDecision;

void aspect_draw(const AspectDraw *d, AspectDecision *o);
/* F9 (research, a wider picture): the next kind of squeezed 2D draw left unsqueezed. */
void aspect_pick_next(void);
