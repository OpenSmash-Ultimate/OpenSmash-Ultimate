/* Maxwell texture headers and image layouts (see gpu_texture.c). */
#pragma once
#include "host.h"
#include <dxgiformat.h>

#ifdef __APPLE__
/* (macOS) ASTC, which Direct3D has no format for and Apple's GPUs read as it is: this stands for
 * it in TextureDesc.dxgi - blocks of w x h texels, linear or sRGB (macos/gpu_metal.m). */
#define DXGI_ASTC(w, h, srgb) ((DXGI_FORMAT)(0x4000 | (w) << 8 | (h) << 4 | (srgb)))
#define DXGI_IS_ASTC(f) (((u32)(f) & 0xF000) == 0x4000)
#endif

typedef struct TextureDesc {
    u64 address;                  /* GPU virtual address */
    u32 format, type, header_version, swizzle, srgb;
    u32 width, height, depth, levels, pitch;
    u32 block_height, block_depth;
    u32 bytes, bw, bh;            /* bytes per block, block width/height in texels */
    DXGI_FORMAT dxgi;
} TextureDesc;

/* Texture types in a header. */
enum { TIC_1D, TIC_2D, TIC_3D, TIC_CUBE, TIC_1D_ARRAY, TIC_2D_ARRAY, TIC_1D_BUFFER, TIC_2D_NO_MIPMAP, TIC_CUBE_ARRAY };

int tic_decode(const u32 *tic, TextureDesc *d);           /* 0 when the format is not supported */
u64 texture_layer_size(const TextureDesc *d);
void texture_read(const TextureDesc *d, u64 guest, u32 layer, u32 level, u8 *out);
u64 texture_3d_size(const TextureDesc *d);                /* level 0 of a 3D texture, every slice */
void texture_read_3d(const TextureDesc *d, u64 guest, u8 *out);   /* level 0, slice after slice */
