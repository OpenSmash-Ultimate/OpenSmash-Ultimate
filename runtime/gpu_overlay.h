/* The debug overlay: frame rate, a frame time graph and statistics, drawn over each
 * presented frame (gpu_overlay.c); and the settings menu's drawing (gpu_menu.c). */
#pragma once
#ifndef __APPLE__   /* (macOS: the Metal renderer's overlay, macos/gpu_metal_overlay.m, has the ovl_ calls below) */
#include <d3d11.h>

/* Called once per presented frame, after the picture is in rtv (at x, y, w x h): notes the
 * frame's time and, if show, draws the overlay over the picture; detail (lines separated by
 * '\n', or NULL) goes under the graph. The settings menu, while open, is drawn instead of the
 * overlay (menu_draw). Leaves the pipeline state changed. */
void overlay_present(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11RenderTargetView *rtv, unsigned x, unsigned y, unsigned w,
                     unsigned h, int show, const char *detail);

/* A message in the middle of the picture, over the game and the overlay: text (NULL or "" clears
 * it) and, when progress is 0..1, a bar under it. The renderer sets it each frame while the
 * start-up shader preparation runs, and for a few seconds after it finishes. */
void overlay_message(const char *text, float progress);

/* The same overlay and menu for another renderer (runtime/gpu_vk.c): overlay_quads notes the frame's time
 * and returns the vertices overlay_present would draw for a picture w x h (a triangle list; 0: none);
 * overlay_coverage the glyph atlas they sample (one byte a texel: coverage, the colour's alpha). */
typedef struct OverlayVertex { float x, y, u, v; unsigned color; } OverlayVertex;
unsigned overlay_quads(unsigned w, unsigned h, int show, const char *detail, const OverlayVertex **verts);
const unsigned char *overlay_coverage(unsigned *w, unsigned *h);
#endif

/* For menu_draw, in the picture's pixels (0, 0 its top left): a rectangle, and a line of text
 * whose letters are `size` pixels high (UTF-8; ASCII advances size / 2, CJK size; '\n' starts a line).
 * Colours are 0xAABBGGRR (OVL_RGBA). ovl_text returns the y below the text. */
#define OVL_RGBA(r, g, b, a) ((unsigned)(r) | (unsigned)(g) << 8 | (unsigned)(b) << 16 | (unsigned)(a) << 24)
void ovl_rect(float x, float y, float w, float h, unsigned color);
float ovl_text(float x, float y, float size, unsigned color, const char *s);
