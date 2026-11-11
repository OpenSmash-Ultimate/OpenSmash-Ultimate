/* The back end drawing (gpu_backend.h): a table of its entry points, chosen once, at the game's
 * first GPU work. The entry points gpu.c and hle_nv.c call are gpu_backend.h's inline functions over
 * this table; the ones at the end are the rest of the runtime's questions to the back end (the
 * watchdog, crash reports, [status], display pacing, the window's close), plain functions as their
 * callers declare them. (macOS: macos/gpu_metal.m defines all of them itself; this file is left out
 * of the Mac program.)
 *
 * Until the choice the table holds starters: the entry points that draw (or can start the back
 * end) make the choice, then call the chosen back end's; the questions answer as a back end that has
 * not started does (nothing kept, nothing drawn) without making it. The choice: display.ini's
 * renderer (gpu_display.c) - Direct3D 11 (gpu_d3d11.c) by default, as before; vulkan starts the
 * Vulkan back end (gpu_vk.c), and Direct3D 11 draws if it cannot start, saying why. After the choice
 * each entry point is one indirect call into the chosen back end. */
#ifndef __APPLE__
#include <stdio.h>
#include <stdlib.h>

#include "display.h"
#include "gpu_d3d11.h"
#include "gpu_renderer.h"
#include "gpu_vk.h"

static const GfxBackend D3D11 = {
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

/* ---- the choice ------------------------------------------------------------------------------ */
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK choose(PINIT_ONCE o, PVOID p, PVOID *c) {
    (void)o; (void)p; (void)c;
    const GfxBackend *b = &D3D11;
    int asked = display_renderer();
#ifdef _WIN32
    int automatic = asked == RENDERER_DEFAULT;   /* (auto: Vulkan on an AMD or NVIDIA card) */
#else
    int automatic = 0;
#endif
    char why[600];
#ifdef _WIN32
    if (automatic) {   /* (the card Direct3D 11 would take, through DXGI: on another vendor's, Vulkan is not even loaded) */
        const char *test = getenv("SSBU_VK_AUTO_VENDOR");   /* (testing: another vendor taken for AMD's) */
        unsigned amd = test && *test ? (unsigned)strtoul(test, NULL, 16) : 0x1002;
        char name[128];
        unsigned vendor = d3d11_card_vendor(name, sizeof name);
        /* (NVIDIA too since 2026-11-11: Vulkan keeps its own pipeline cache, so a second start has every pipeline in about
         * 2 s where Direct3D 11 compiled them again for 200-265 s whenever the driver's cache had dropped them - the owner:
         * a recompiled game must handle its shaders as a PC game does. Intel and others stay on Direct3D 11.) */
        if (vendor != amd && vendor != 0x10de) {
            if (vendor) snprintf(why, sizeof why, "%s is not an AMD or NVIDIA card, Direct3D 11 is the default for it (renderer = vulkan to draw with Vulkan)", name);
            else snprintf(why, sizeof why, "no graphics card of AMD's or NVIDIA's (gpu = warp, or none listed): Direct3D 11");
            gfx_renderer_chosen(RENDERER_DEFAULT, why);
            automatic = 0;
        }
    }
#endif
    if (asked == RENDERER_VULKAN || automatic) {
        const GfxBackend *v = vk_backend_start(why, sizeof why, automatic);
        if (v) { b = v; gfx_renderer_chosen(RENDERER_VULKAN, NULL); }
        else gfx_renderer_chosen(RENDERER_DEFAULT, why);
    }
    /* (pointer by pointer: each is one aligned store, so a thread reading the table meanwhile sees
     * a starter, which waits for the choice, or the chosen back end's function) */
    void *const *from = (void *const *)b;
    void *volatile *to = (void *volatile *)&gfx_backend;
    for (size_t i = 0; i < sizeof gfx_backend / sizeof(void *); i++) to[i] = from[i];
    return TRUE;
}
static void start(void) { InitOnceExecuteOnce(&once, choose, NULL, NULL); }

/* the entry points that draw: the choice, then the chosen back end's */
static void start_clear(const u32 *regs, u32 arg) { start(); gfx_backend.clear(regs, arg); }
static void start_draw(const u32 *regs, const CbBinding cb[STAGES][18], int indexed) { start(); gfx_backend.draw(regs, cb, indexed); }
static void start_present(u64 guest, u32 width, u32 height) { start(); gfx_backend.present(guest, width, height); }
static void start_blit2d(const u32 *regs) { start(); gfx_backend.blit2d(regs); }
static void start_dispatch(const u32 *regs, const u32 *qmd) { start(); gfx_backend.dispatch(regs, qmd); }
static int start_copy_resident(u64 dst_va, u64 src_va, u64 bytes) { start(); return gfx_backend.copy_resident(dst_va, src_va, bytes); }
static void start_resident_flush(u64 va, u64 bytes) { start(); gfx_backend.resident_flush(va, bytes); }
static void start_resident_written(u64 va, u64 bytes) { start(); gfx_backend.resident_written(va, bytes); }
static void start_stream_wrote(u64 va, u64 bytes) { start(); gfx_backend.stream_wrote(va, bytes); }
static void start_guest_wrote(u64 va, u64 bytes) { start(); gfx_backend.guest_wrote(va, bytes); }
static void start_stream_end(void) { start(); gfx_backend.stream_end(); }
/* the questions: as a back end not started yet (Direct3D 11's answers before its start) */
static void unstarted_unmapped(u64 va, u64 size) { (void)va; (void)size; }   /* (nothing kept yet) */
static int unstarted_draw_logging(void) { return 0; }
static void unstarted_nothing(void) {}
static DWORD unstarted_thread(void) { return 0; }
static void unstarted_report_compiles(void (*line)(const char *text)) { (void)line; }
static void *unstarted_display_output(void) { return NULL; }

GfxBackend gfx_backend = {
    .clear = start_clear,
    .draw = start_draw,
    .present = start_present,
    .blit2d = start_blit2d,
    .dispatch = start_dispatch,
    .copy_resident = start_copy_resident,
    .resident_flush = start_resident_flush,
    .resident_written = start_resident_written,
    .stream_wrote = start_stream_wrote,
    .guest_wrote = start_guest_wrote,
    .stream_end = start_stream_end,
    .gpu_unmapped = unstarted_unmapped,
    .guest_unmapped = unstarted_unmapped,
    .draw_logging = unstarted_draw_logging,
    .status_lines = unstarted_nothing,
    .closing = unstarted_nothing,
    .lock_owner = unstarted_thread,
    .last_enter_thread = unstarted_thread,
    .report_compiles = unstarted_report_compiles,
    .display_output = unstarted_display_output,
};

void gfx_status_lines(void) { gfx_backend.status_lines(); }
void gfx_closing(void) { gfx_backend.closing(); }
DWORD gfx_lock_owner(void) { return gfx_backend.lock_owner(); }
DWORD gfx_last_enter_thread(void) { return gfx_backend.last_enter_thread(); }
void gfx_report_compiles(void (*line)(const char *text)) { gfx_backend.report_compiles(line); }
void *gfx_display_output(void) { return gfx_backend.display_output(); }
/* (gpu_menu.c's software keyboard, while the game waits for it) the last frame once more, with the overlay
 * and the menu: the back end drawing's (Vulkan's too: it was Direct3D 11's alone, so with Vulkan drawing
 * the keyboard fell back to the Windows text box). 1, or 0 before any frame. */
int gfx_present_again(void) {
    if (gfx_backend.present == start_present) return 0;   /* (no back end chosen: nothing shown yet) */
    if (gfx_backend.present == d3d11_present) return d3d11_present_again();
    return vk_present_again();
}
#endif
