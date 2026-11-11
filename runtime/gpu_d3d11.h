/* The Direct3D 11 back end's entry points (gpu_d3d11.c), as gpu_backend.c's table holds them:
 * gpu_backend.h's gfx_* names, documented there. Windows, and Linux through DXVK. */
#pragma once
#include "gpu_backend.h"

void d3d11_clear(const u32 *regs, u32 arg);
void d3d11_draw(const u32 *regs, const CbBinding cb[STAGES][18], int indexed);
void d3d11_present(u64 guest, u32 width, u32 height);
int d3d11_present_again(void);   /* the last frame once more (gfx_present_again): 1, or 0 before any */
void d3d11_blit2d(const u32 *regs);
void d3d11_dispatch(const u32 *regs, const u32 *qmd);
int d3d11_copy_resident(u64 dst_va, u64 src_va, u64 bytes);
void d3d11_resident_flush(u64 va, u64 bytes);
void d3d11_resident_written(u64 va, u64 bytes);
void d3d11_stream_wrote(u64 va, u64 bytes);
void d3d11_guest_wrote(u64 va, u64 bytes);
void d3d11_stream_end(void);
void d3d11_gpu_unmapped(u64 va, u64 size);
void d3d11_guest_unmapped(u64 addr, u64 size);
int d3d11_draw_logging(void);
void d3d11_status_lines(void);
void d3d11_closing(void);
DWORD d3d11_lock_owner(void);
DWORD d3d11_last_enter_thread(void);
void d3d11_report_compiles(void (*line)(const char *text));
void *d3d11_display_output(void);
/* (renderer = auto: gpu_backend.c) The PCI vendor of the card Direct3D 11 would draw with, its name in `name`; 0 for
 * Windows' own renderer (gpu = warp) or none. */
unsigned d3d11_card_vendor(char *name, size_t size);
