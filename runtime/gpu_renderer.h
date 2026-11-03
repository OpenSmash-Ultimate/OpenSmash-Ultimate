/* Which renderer draws the game (display.ini's renderer: renderer.h), acted on as the renderer
 * starts: gpu_renderer.c. */
#pragma once
#include <stddef.h>

/* (gpu_d3d11.c's start, once the game's window exists: an HWND; on Linux the SDL_Window) Acts on
 * display.ini's renderer and returns the renderer that draws (RENDERER_*). With the default asked
 * for, nothing is done and nothing logged: the default renderer starts as it always has. */
int gfx_renderer_start(void *window);
/* The renderer drawing, once started (RENDERER_DEFAULT until then). */
int gfx_renderer(void);
/* (gpu_backend.c's choice, made when display.ini asks for vulkan) Which renderer draws, and why the
 * Vulkan back end could not start (NULL when it did): gfx_renderer_start says so when Direct3D 11
 * starts instead. */
void gfx_renderer_chosen(int renderer, const char *why);
