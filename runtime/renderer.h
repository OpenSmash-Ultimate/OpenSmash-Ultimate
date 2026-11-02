/* Which renderer draws the game: display.ini's renderer, SSBU_RENDERER (gpu_display.c reads them,
 * gpu_renderer.c acts on them at the renderer's start).
 *
 * The default is the system's own back end: Direct3D 11 on Windows (gpu_d3d11.c), the same
 * Direct3D 11 through DXVK on Linux, Metal on macOS (macos/gpu_metal.m). vulkan is the Vulkan back
 * end behind gpu_backend.h (gpu_vulkan.c; docs/TECHNICAL.md, "A second back end: Vulkan"), in
 * development: until it draws the game it is checked at the start and the default draws.
 *
 * Plain C, no system headers: tools/dev/renderertest.c checks the names on any system. */
#pragma once

typedef enum { RENDERER_DEFAULT = 0, RENDERER_VULKAN = 1 } Renderer;

#if defined(__APPLE__)
#define RENDERER_DEFAULT_NAME "metal"
#elif defined(_WIN32)
#define RENDERER_DEFAULT_NAME "d3d11"
#else
#define RENDERER_DEFAULT_NAME "dxvk"
#endif

static inline int renderer_lower_eq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
        if (x != *b) return 0;
    }
    return !*a && !*b;
}

/* A display.ini / SSBU_RENDERER value: RENDERER_*, or -1 for none of the names. Every system's
 * default name means the default here (a display.ini carried from another system keeps working):
 * the Linux build's Direct3D 11 is DXVK's, so d3d11 and dxvk are one renderer there. */
static inline int renderer_parse(const char *v) {
    static const char *const DEFAULTS[] = {"auto", "default", "d3d11", "dx11", "direct3d", "direct3d11", "dxvk", "metal"};
    if (!v) return -1;
    for (unsigned i = 0; i < sizeof DEFAULTS / sizeof *DEFAULTS; i++)
        if (renderer_lower_eq(v, DEFAULTS[i])) return RENDERER_DEFAULT;
    if (renderer_lower_eq(v, "vulkan") || renderer_lower_eq(v, "vk")) return RENDERER_VULKAN;
    return -1;
}

/* The name display.ini is written with (the settings menu, the default file). */
static inline const char *renderer_name(int r) { return r == RENDERER_VULKAN ? "vulkan" : RENDERER_DEFAULT_NAME; }
