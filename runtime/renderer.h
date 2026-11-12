/* Which renderer draws the game: display.ini's renderer, SSBU_RENDERER (gpu_display.c reads them,
 * gpu_backend.c and gpu_renderer.c act on them at the renderer's start).
 *
 * The default is the system's own back end - Direct3D 11 through DXVK on Linux, Metal on macOS -
 * and on Windows "auto": the Vulkan back end (gpu_vk.c; docs/TECHNICAL.md, "A second back end:
 * Vulkan") on an AMD, NVIDIA or Intel card, where it draws the game without Direct3D 11's first-use
 * stutter (and, on Intel's, without its driver's wrong colours: #81), and Direct3D 11 (gpu_d3d11.c)
 * on any other card and whenever Vulkan cannot start. vulkan and
 * direct3d11 choose one: direct3d11 (or d3d11, dx11, direct3d) is Direct3D 11 whatever the card.
 * Policy change: "d3d11" (the value every default display.ini before auto was written with) used to
 * count as auto, to move those players to Vulkan without their editing it; a name that says Direct3D
 * 11 and draws with Vulkan was counterintuitive, so it is Direct3D 11 again, and those files keep
 * Direct3D 11 until their player picks auto.
 *
 * Plain C, no system headers: tools/dev/renderertest.c checks the names on any system. */
#pragma once

typedef enum { RENDERER_DEFAULT = 0, RENDERER_VULKAN = 1, RENDERER_D3D11 = 2 } Renderer;

#if defined(__APPLE__)
#define RENDERER_DEFAULT_NAME "metal"
#elif defined(_WIN32)
#define RENDERER_DEFAULT_NAME "auto"
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
    static const char *const DEFAULTS[] = {"auto", "default", "dxvk", "metal"};
    static const char *const D3D11[] = {"direct3d11", "d3d11", "direct3d", "dx11"};
    if (!v) return -1;
    for (unsigned i = 0; i < sizeof DEFAULTS / sizeof *DEFAULTS; i++)
        if (renderer_lower_eq(v, DEFAULTS[i])) return RENDERER_DEFAULT;
    for (unsigned i = 0; i < sizeof D3D11 / sizeof *D3D11; i++)
#ifdef _WIN32
        if (renderer_lower_eq(v, D3D11[i])) return RENDERER_D3D11;
#else
        if (renderer_lower_eq(v, D3D11[i])) return RENDERER_DEFAULT;   /* (Linux: DXVK's Direct3D 11 is the default) */
#endif
    if (renderer_lower_eq(v, "vulkan") || renderer_lower_eq(v, "vk")) return RENDERER_VULKAN;
    return -1;
}

/* The name display.ini is written with (the settings menu, the default file). */
static inline const char *renderer_name(int r) { return r == RENDERER_VULKAN ? "vulkan" : r == RENDERER_D3D11 ? "direct3d11" : RENDERER_DEFAULT_NAME; }
