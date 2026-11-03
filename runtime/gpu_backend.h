/* The rendering back end, as seen from the GPU command processor (gpu.c): the 3D engine's
 * register file at the moments that produce pixels, and the frames the display shows.
 *
 * Windows and Linux hold more than one back end in one program: Direct3D 11 (gpu_d3d11.c, the
 * default) and Vulkan (gpu_vulkan.c, in development). The entry points below go through a table,
 * gfx_backend (gpu_backend.c), chosen once at the game's first GPU work (display.ini's renderer:
 * Direct3D 11, or Vulkan when it starts): each costs one indirect
 * call. macOS has one back end, macos/gpu_metal.m, which defines these names itself. */
#pragma once
#include "gpu.h"
#include "maxwell3d.h"

typedef struct CbBinding { u64 address; u32 size; } CbBinding;   /* a bound constant buffer (GPU VA) */

#ifndef __APPLE__
/* The back end's entry points (the names below without gfx_), and its state the rest of the
 * runtime asks about: the watchdog's lock owner and last thread in, the compiler threads' work for
 * a crash report, and the swap chain's output for display pacing (gpu_display.c). */
typedef struct GfxBackend {
    void (*clear)(const u32 *regs, u32 arg);
    void (*draw)(const u32 *regs, const CbBinding cb[STAGES][18], int indexed);
    void (*present)(u64 guest, u32 width, u32 height);
    void (*blit2d)(const u32 *regs);
    void (*dispatch)(const u32 *regs, const u32 *qmd);
    int (*copy_resident)(u64 dst_va, u64 src_va, u64 bytes);
    void (*resident_flush)(u64 va, u64 bytes);
    void (*resident_written)(u64 va, u64 bytes);
    void (*stream_wrote)(u64 va, u64 bytes);
    void (*guest_wrote)(u64 va, u64 bytes);
    void (*stream_end)(void);
    void (*gpu_unmapped)(u64 va, u64 size);
    void (*guest_unmapped)(u64 addr, u64 size);
    int (*draw_logging)(void);
    void (*status_lines)(void);
    void (*closing)(void);
    DWORD (*lock_owner)(void);
    DWORD (*last_enter_thread)(void);
    void (*report_compiles)(void (*line)(const char *text));
    void *(*display_output)(void);
} GfxBackend;
extern GfxBackend gfx_backend;   /* (gpu_backend.c) the back end drawing */

static inline void gfx_clear(const u32 *regs, u32 arg) { gfx_backend.clear(regs, arg); }
static inline void gfx_draw(const u32 *regs, const CbBinding cb[STAGES][18], int indexed) { gfx_backend.draw(regs, cb, indexed); }
static inline void gfx_present(u64 guest, u32 width, u32 height) { gfx_backend.present(guest, width, height); }
static inline void gfx_blit2d(const u32 *regs) { gfx_backend.blit2d(regs); }
static inline void gfx_dispatch(const u32 *regs, const u32 *qmd) { gfx_backend.dispatch(regs, qmd); }
static inline int gfx_copy_resident(u64 dst_va, u64 src_va, u64 bytes) { return gfx_backend.copy_resident(dst_va, src_va, bytes); }
static inline void gfx_resident_flush(u64 va, u64 bytes) { gfx_backend.resident_flush(va, bytes); }
static inline void gfx_resident_written(u64 va, u64 bytes) { gfx_backend.resident_written(va, bytes); }
static inline void gfx_stream_wrote(u64 va, u64 bytes) { gfx_backend.stream_wrote(va, bytes); }
static inline void gfx_guest_wrote(u64 va, u64 bytes) { gfx_backend.guest_wrote(va, bytes); }
static inline void gfx_stream_end(void) { gfx_backend.stream_end(); }
static inline void gfx_gpu_unmapped(u64 va, u64 size) { gfx_backend.gpu_unmapped(va, size); }
static inline void gfx_guest_unmapped(u64 addr, u64 size) { gfx_backend.guest_unmapped(addr, size); }
static inline int gfx_draw_logging(void) { return gfx_backend.draw_logging(); }
#else
/* The entry points, as every back end implements them (macOS' by these names; Direct3D 11's as
 * d3d11_*, gpu_d3d11.h). */
/* CLEAR_SURFACE with the 3D engine's registers (clear values, render target bindings). */
void gfx_clear(const u32 *regs, u32 arg);

/* A draw has been issued (VERTEX_END_GL), with the 3D engine's state as it stands. */
void gfx_draw(const u32 *regs, const CbBinding cb[STAGES][18], int indexed);

/* Show the frame whose pixels live at this guest address (a block-linear surface). */
void gfx_present(u64 guest, u32 width, u32 height);

/* The 2D engine's scaled copy between surfaces (PIXELS_FROM_MEMORY), with its registers. */
void gfx_blit2d(const u32 *regs);
/* A compute launch: the engine's registers (program region, texture pools) and its launch
 * descriptor (QMD): program offset, grid and workgroup size, shared memory, constant buffers. */
void gfx_dispatch(const u32 *regs, const u32 *qmd);
/* Compute results stay on the GPU (gpu_d3d11.c). The copy engine asks: copy [src, src + bytes)
 * to dst there if the source is such data (1 if done); bring a range back to guest memory
 * before reading it otherwise; and refresh the GPU's copy of a range it wrote in guest memory. */
int gfx_copy_resident(u64 dst_va, u64 src_va, u64 bytes);
void gfx_resident_flush(u64 va, u64 bytes);
void gfx_resident_written(u64 va, u64 bytes);
/* The command stream itself wrote guest memory (constant buffer loads, inline-to-memory). */
void gfx_stream_wrote(u64 va, u64 bytes);
/* Other writes of the GPU to guest memory (semaphores, copies into block-linear surfaces;
 * bytes 0: somewhere unknown). */
void gfx_guest_wrote(u64 va, u64 bytes);
void gfx_stream_end(void);   /* (after each submission) */
/* Memory taken away (hle_nv.c, once the GPU thread's queued work has run): the GPU mapping of
 * [va, va + size) changed or went, or guest memory [addr, addr + size) the GPU may read is
 * about to be decommitted. What the back end made from it and keeps is dropped. */
void gfx_gpu_unmapped(u64 va, u64 size);
void gfx_guest_unmapped(u64 addr, u64 size);
int gfx_draw_logging(void);   /* a frame's draws are being logged (debugging) */
#endif
void *gfx_window_handle(void);   /* the game's window (an HWND), to own dialogs */
void gfx_status_lines(void);   /* (host.c's [status]) the back end's own lines, if any */
void gfx_closing(void);        /* the game is about to end (Direct3D: see gpu_d3d11.c's "first draws") */
#ifdef __APPLE__
/* (macOS, gpu.c's copy engine) the picture the GPU drew at guest address `guest`, which only the
 * host has: its pixels at the game's size (4 bytes each, as the format keeps them in memory; rows
 * top down, malloc'd into *pixels), when a 4-byte colour surface starts there. */
int gfx_surface_pixels(u64 guest, u32 *width, u32 *height, u8 **pixels);
#endif
