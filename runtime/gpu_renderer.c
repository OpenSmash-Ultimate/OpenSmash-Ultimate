/* Which renderer draws the game: display.ini's renderer (or SSBU_RENDERER), read by gpu_display.c
 * (renderer.h's names), acted on here once, as the renderer starts.
 *
 * The default is the system's own back end: Direct3D 11 (gpu_d3d11.c) on Windows, the same through
 * DXVK on Linux, Metal on macOS. vulkan asks for the Vulkan back end behind gpu_backend.h
 * (docs/TECHNICAL.md, "A second back end: Vulkan"), which is in development: until it draws the
 * game, asking for it is answered in the log and the default renderer draws. */
#include <stdio.h>

#include "display.h"
#include "gpu_renderer.h"

#ifdef _WIN32
#define DEFAULT_WORDS "Direct3D 11"
#else
#define DEFAULT_WORDS "DXVK (Direct3D 11 on Vulkan)"
#endif

static int drawing = RENDERER_DEFAULT;

int gfx_renderer(void) { return drawing; }

int gfx_renderer_start(void *window) {
    (void)window;
    if (display_renderer() != RENDERER_VULKAN) return drawing = RENDERER_DEFAULT;
    fprintf(stderr, "[gfx] renderer = vulkan: this program has no Vulkan renderer yet; " DEFAULT_WORDS " draws\n");
    return drawing = RENDERER_DEFAULT;
}
