/* The back end drawing (gpu_backend.h): a table of its entry points, Direct3D 11's from the start
 * (gpu_d3d11.c), chosen once before the game's first GPU work. The entry points gpu.c and hle_nv.c
 * call are gpu_backend.h's inline functions over this table; the ones below are the rest of the
 * runtime's questions to the back end (the watchdog, crash reports, [status], display pacing, the
 * window's close), plain functions as their callers declare them. (macOS: macos/gpu_metal.m defines
 * all of them itself; this file is left out of the Mac program.) */
#ifndef __APPLE__
#include "gpu_d3d11.h"

GfxBackend gfx_backend = {
    .clear = d3d11_clear,
    .draw = d3d11_draw,
    .present = d3d11_present,
    .blit2d = d3d11_blit2d,
    .dispatch = d3d11_dispatch,
    .copy_resident = d3d11_copy_resident,
    .resident_flush = d3d11_resident_flush,
    .resident_written = d3d11_resident_written,
    .stream_wrote = d3d11_stream_wrote,
    .guest_wrote = d3d11_guest_wrote,
    .stream_end = d3d11_stream_end,
    .gpu_unmapped = d3d11_gpu_unmapped,
    .guest_unmapped = d3d11_guest_unmapped,
    .draw_logging = d3d11_draw_logging,
    .status_lines = d3d11_status_lines,
    .closing = d3d11_closing,
    .lock_owner = d3d11_lock_owner,
    .last_enter_thread = d3d11_last_enter_thread,
    .report_compiles = d3d11_report_compiles,
    .display_output = d3d11_display_output,
};

/* The Vulkan back end cannot draw the game yet (docs/TECHNICAL.md, "A second back end: Vulkan"):
 * Direct3D 11's table stays, and renderer = vulkan is answered at Direct3D 11's start
 * (gfx_renderer_start: Vulkan checked on this PC, the result logged). Once it draws, this is where
 * display.ini's renderer = vulkan puts its table in place, and Direct3D 11's stays if it cannot
 * start. */
void gfx_backend_choose(void) {}

void gfx_status_lines(void) { gfx_backend.status_lines(); }
void gfx_closing(void) { gfx_backend.closing(); }
DWORD gfx_lock_owner(void) { return gfx_backend.lock_owner(); }
DWORD gfx_last_enter_thread(void) { return gfx_backend.last_enter_thread(); }
void gfx_report_compiles(void (*line)(const char *text)) { gfx_backend.report_compiles(line); }
void *gfx_display_output(void) { return gfx_backend.display_output(); }
#endif
