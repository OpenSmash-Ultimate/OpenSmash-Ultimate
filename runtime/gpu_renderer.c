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
static int tried;              /* (gpu_backend.c's choice tried the Vulkan back end) */
static char failed[600];       /* why it could not start */

int gfx_renderer(void) { return drawing; }

void gfx_renderer_chosen(int renderer, const char *why) {
    drawing = renderer;
    tried = 1;
    snprintf(failed, sizeof failed, "%s", why ? why : "");
    if (renderer == RENDERER_VULKAN)
        fprintf(stderr, "[gfx] renderer = %s: the Vulkan back end draws (docs/TECHNICAL.md, \"A second back end: Vulkan\")\n",
                display_renderer() == RENDERER_VULKAN ? "vulkan" : "auto (an AMD card)");
}

int gfx_renderer_start(void *window) {
#ifdef _WIN32
    if (display_renderer() == RENDERER_DEFAULT && tried) {   /* (auto: gpu_backend.c's choice tried Vulkan) */
        fprintf(stderr, "[gfx] renderer = auto: %s. " DEFAULT_WORDS " draws\n", failed);
        return drawing = RENDERER_DEFAULT;
    }
#endif
    if (display_renderer() != RENDERER_VULKAN) return drawing = RENDERER_DEFAULT;
    if (tried) {   /* (the Vulkan back end could not start: Direct3D 11 is starting instead) */
        fprintf(stderr, "[gfx] renderer = vulkan: the Vulkan back end cannot start here: %s. " DEFAULT_WORDS " draws\n", failed);
        return drawing = RENDERER_DEFAULT;
    }
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
