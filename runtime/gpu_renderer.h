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
