/* Which renderer draws the game: display.ini's renderer (or SSBU_RENDERER), read by gpu_display.c
 * (renderer.h's names), acted on here once, as the renderer starts.
 *
 * The default is the system's own back end: Direct3D 11 (gpu_d3d11.c) on Windows, the same through
 * DXVK on Linux, Metal on macOS. vulkan asks for the Vulkan back end behind gpu_backend.h
 * (gpu_vulkan.c; docs/TECHNICAL.md, "A second back end: Vulkan"), which is in development: until
 * it draws the game, asking for it checks Vulkan on this PC, says what was found in the log, and
 * the default renderer draws. */
#include <stdio.h>

#include "display.h"
#include "gpu_renderer.h"
#include "gpu_vulkan.h"

#ifdef _WIN32
#define DEFAULT_WORDS "Direct3D 11"
#else
#define DEFAULT_WORDS "DXVK (Direct3D 11 on Vulkan)"
#endif

static int drawing = RENDERER_DEFAULT;

int gfx_renderer(void) { return drawing; }

int gfx_renderer_start(void *window) {
    if (display_renderer() != RENDERER_VULKAN) return drawing = RENDERER_DEFAULT;
    char what[600];
    ULONGLONG t0 = GetTickCount64();
    int works = vk_probe(window, what, sizeof what);
    double ms = (double)(GetTickCount64() - t0);
    if (works)
        fprintf(stderr, "[gfx] renderer = vulkan: Vulkan works here: %s (checked in %.0f ms). The Vulkan renderer cannot draw the game "
                        "yet (docs/TECHNICAL.md, \"A second back end: Vulkan\"): " DEFAULT_WORDS " draws\n", what, ms);
    else
        fprintf(stderr, "[gfx] renderer = vulkan: Vulkan cannot be used here: %s. " DEFAULT_WORDS " draws\n", what);
    return drawing = RENDERER_DEFAULT;
}
