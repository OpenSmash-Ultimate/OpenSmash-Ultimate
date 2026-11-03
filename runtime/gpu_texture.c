/* Textures: Maxwell texture headers (TIC) to host-ready pixel data.
 *
 * A texture header describes the format, dimensions, mip count and memory layout of an
 * image in GPU memory. Most images are block linear: 64-byte x 8-row GOBs (512 bytes),
 * stacked into blocks of 2^n GOBs vertically, blocks laid out row-major across the image.
 * Mip levels follow each other, each with its block height reduced to fit its size. This
 * file reads such images back into linear rows, one level at a time. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>

#include "gpu_texture.h"

#define DIV_UP(a, b) (((a) + (b) - 1) / (b))

/* Maxwell texture formats -> bytes per block, block size and the DXGI format (UNORM). */
static const struct { u8 format, bytes, bw, bh; DXGI_FORMAT dxgi, srgb; } FORMATS[] = {
    {0x01, 16, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0},
    {0x03, 8, 1, 1, DXGI_FORMAT_R16G16B16A16_UNORM, 0},
    {0x04, 8, 1, 1, DXGI_FORMAT_R32G32_FLOAT, 0},
    {0x07, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB},
    {0x08, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB},
    {0x09, 4, 1, 1, DXGI_FORMAT_R10G10B10A2_UNORM, 0},
    {0x0c, 4, 1, 1, DXGI_FORMAT_R16G16_UNORM, 0},
    {0x0f, 4, 1, 1, DXGI_FORMAT_R32_FLOAT, 0},
    {0x10, 16, 4, 4, DXGI_FORMAT_BC6H_SF16, 0},
    {0x11, 16, 4, 4, DXGI_FORMAT_BC6H_UF16, 0},
    {0x12, 2, 1, 1, DXGI_FORMAT_B4G4R4A4_UNORM, 0},
    {0x14, 2, 1, 1, DXGI_FORMAT_B5G5R5A1_UNORM, 0},
    {0x15, 2, 1, 1, DXGI_FORMAT_B5G6R5_UNORM, 0},
    {0x17, 16, 4, 4, DXGI_FORMAT_BC7_UNORM, DXGI_FORMAT_BC7_UNORM_SRGB},
    {0x18, 2, 1, 1, DXGI_FORMAT_R8G8_UNORM, 0},
    {0x1b, 2, 1, 1, DXGI_FORMAT_R16_UNORM, 0},
    {0x1d, 1, 1, 1, DXGI_FORMAT_R8_UNORM, 0},
    {0x20, 4, 1, 1, DXGI_FORMAT_R9G9B9E5_SHAREDEXP, 0},
    {0x21, 4, 1, 1, DXGI_FORMAT_R11G11B10_FLOAT, 0},
    {0x24, 8, 4, 4, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM_SRGB},
    {0x25, 16, 4, 4, DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC2_UNORM_SRGB},
    {0x26, 16, 4, 4, DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC3_UNORM_SRGB},
    {0x27, 8, 4, 4, DXGI_FORMAT_BC4_UNORM, 0},
    {0x28, 16, 4, 4, DXGI_FORMAT_BC5_UNORM, 0},
    {0x29, 4, 1, 1, DXGI_FORMAT_R24G8_TYPELESS, 0},
    {0x2b, 4, 1, 1, DXGI_FORMAT_R24G8_TYPELESS, 0},
    {0x2f, 4, 1, 1, DXGI_FORMAT_R32_FLOAT, 0},
    {0x3a, 2, 1, 1, DXGI_FORMAT_R16_UNORM, 0},
#ifdef __APPLE__
    /* ASTC, 16-byte blocks of w x h texels (the codes as yuzu's TextureFormat has them): Mii parts
     * (eyes, eyebrows, mouths) are 4x4. Direct3D has no format for them; Apple's GPUs read them. */
    {0x40, 16, 4, 4, DXGI_ASTC(4, 4, 0), DXGI_ASTC(4, 4, 1)},
    {0x41, 16, 5, 5, DXGI_ASTC(5, 5, 0), DXGI_ASTC(5, 5, 1)},
    {0x42, 16, 6, 6, DXGI_ASTC(6, 6, 0), DXGI_ASTC(6, 6, 1)},
    {0x44, 16, 8, 8, DXGI_ASTC(8, 8, 0), DXGI_ASTC(8, 8, 1)},
    {0x45, 16, 10, 10, DXGI_ASTC(10, 10, 0), DXGI_ASTC(10, 10, 1)},
    {0x46, 16, 12, 12, DXGI_ASTC(12, 12, 0), DXGI_ASTC(12, 12, 1)},
    {0x50, 16, 5, 4, DXGI_ASTC(5, 4, 0), DXGI_ASTC(5, 4, 1)},
    {0x51, 16, 6, 5, DXGI_ASTC(6, 5, 0), DXGI_ASTC(6, 5, 1)},
    {0x52, 16, 8, 6, DXGI_ASTC(8, 6, 0), DXGI_ASTC(8, 6, 1)},
    {0x53, 16, 10, 8, DXGI_ASTC(10, 8, 0), DXGI_ASTC(10, 8, 1)},
    {0x54, 16, 12, 10, DXGI_ASTC(12, 10, 0), DXGI_ASTC(12, 10, 1)},
    {0x55, 16, 8, 5, DXGI_ASTC(8, 5, 0), DXGI_ASTC(8, 5, 1)},
    {0x56, 16, 10, 5, DXGI_ASTC(10, 5, 0), DXGI_ASTC(10, 5, 1)},
    {0x57, 16, 10, 6, DXGI_ASTC(10, 6, 0), DXGI_ASTC(10, 6, 1)},
#endif
};

/* The component type adjusts the DXGI format for signed/integer/float variants. */
static DXGI_FORMAT typed(DXGI_FORMAT f, u32 type) {
    if (type == 1) {   /* SNORM */
        switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_SNORM;
        case DXGI_FORMAT_R8G8_UNORM: return DXGI_FORMAT_R8G8_SNORM;
        case DXGI_FORMAT_R8_UNORM: return DXGI_FORMAT_R8_SNORM;
        case DXGI_FORMAT_R16_UNORM: return DXGI_FORMAT_R16_SNORM;
        case DXGI_FORMAT_R16G16_UNORM: return DXGI_FORMAT_R16G16_SNORM;
        case DXGI_FORMAT_R16G16B16A16_UNORM: return DXGI_FORMAT_R16G16B16A16_SNORM;
        case DXGI_FORMAT_BC4_UNORM: return DXGI_FORMAT_BC4_SNORM;
        case DXGI_FORMAT_BC5_UNORM: return DXGI_FORMAT_BC5_SNORM;
        default: return f;
        }
    }
    if (type == 7) {   /* FLOAT */
        switch (f) {
        case DXGI_FORMAT_R16G16B16A16_UNORM: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R16G16_UNORM: return DXGI_FORMAT_R16G16_FLOAT;
        case DXGI_FORMAT_R16_UNORM: return DXGI_FORMAT_R16_FLOAT;
        default: return f;
        }
    }
    if (type == 4 || type == 3) {   /* UINT / SINT */
        int s = type == 3;
        switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM: return s ? DXGI_FORMAT_R8G8B8A8_SINT : DXGI_FORMAT_R8G8B8A8_UINT;
        case DXGI_FORMAT_R8_UNORM: return s ? DXGI_FORMAT_R8_SINT : DXGI_FORMAT_R8_UINT;
        case DXGI_FORMAT_R16_UNORM: return s ? DXGI_FORMAT_R16_SINT : DXGI_FORMAT_R16_UINT;
        case DXGI_FORMAT_R32_FLOAT: return s ? DXGI_FORMAT_R32_SINT : DXGI_FORMAT_R32_UINT;
        case DXGI_FORMAT_R32G32_FLOAT: return s ? DXGI_FORMAT_R32G32_SINT : DXGI_FORMAT_R32G32_UINT;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return s ? DXGI_FORMAT_R32G32B32A32_SINT : DXGI_FORMAT_R32G32B32A32_UINT;
        default: return f;
        }
    }
    return f;
}

int tic_decode(const u32 *tic, TextureDesc *d) {
    memset(d, 0, sizeof *d);
    u32 format = tic[0] & 0x7F;
    d->format = format;
    d->address = (u64)(tic[2] & 0xFFFF) << 32 | tic[1];
    d->header_version = (tic[2] >> 21) & 7;
    d->block_height = (tic[3] >> 3) & 7;
    d->block_depth = (tic[3] >> 6) & 7;
    d->levels = ((tic[3] >> 28) & 15) + 1;
    d->width = (tic[4] & 0xFFFF) + 1;
    d->srgb = (tic[4] >> 22) & 1;
    d->type = (tic[4] >> 23) & 15;
    d->height = (tic[5] & 0xFFFF) + 1;
    d->depth = ((tic[5] >> 16) & 0x3FFF) + 1;
    d->swizzle = (tic[0] >> 19) & 0xFFF;
    if (d->header_version == 2) {   /* pitch linear: pitch in 32-byte units, a single level */
        d->pitch = (tic[3] & 0xFFFF) << 5;
        d->levels = 1;
        d->block_height = d->block_depth = 0;
    }
    for (u32 i = 0; i < sizeof FORMATS / sizeof *FORMATS; i++)
        if (FORMATS[i].format == format) {
            d->bytes = FORMATS[i].bytes; d->bw = FORMATS[i].bw; d->bh = FORMATS[i].bh;
            d->dxgi = d->srgb && FORMATS[i].srgb ? FORMATS[i].srgb : typed(FORMATS[i].dxgi, (tic[0] >> 7) & 7);
            return 1;
        }
    return 0;
}

/* Byte offset of (x bytes, y rows) inside a GOB. */
static u32 gob_offset(u32 x, u32 y) {
    return ((x % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((x % 32) / 16) * 32 + (y % 2) * 16 + (x % 16);
}

/* Block-linear size of one level: width in bytes, height in block rows, block height log2. */
static u64 level_size(u32 wbytes, u32 rows, u32 bh) {
    return (u64)DIV_UP(wbytes, 64) * DIV_UP(rows, 8u << bh) * (512ull << bh);
}

static u32 level_block_height(u32 rows, u32 bh) {
    while (bh > 0 && DIV_UP(rows, 8) <= (1u << (bh - 1))) bh--;
    return bh;
}

u64 texture_layer_size(const TextureDesc *d) {
    if (d->header_version == 2) return (u64)d->pitch * DIV_UP(d->height, d->bh);   /* pitch linear: one level */
    u64 total = 0;
    for (u32 l = 0; l < d->levels; l++) {
        u32 w = DIV_UP(d->width >> l ? d->width >> l : 1, d->bw), h = DIV_UP(d->height >> l ? d->height >> l : 1, d->bh);
        total += level_size(w * d->bytes, h, level_block_height(h, d->block_height));
    }
    /* layers are aligned to the level-0 block */
    u64 block = 512ull << d->block_height;
    return (total + block - 1) / block * block;
}

/* Read level `level` of layer `layer` into linear rows (pitch = width in blocks * bytes). */
void texture_read(const TextureDesc *d, u64 guest, u32 layer, u32 level, u8 *out) {
    u32 w = DIV_UP(d->width >> level ? d->width >> level : 1, d->bw), h = DIV_UP(d->height >> level ? d->height >> level : 1, d->bh);
    u32 row = w * d->bytes;
    if (d->header_version == 2) {   /* pitch linear: one level */
        const u8 *src = P(u8, guest);
        for (u32 y = 0; y < h; y++) memcpy(out + (size_t)y * row, src + (size_t)y * d->pitch, row);
        return;
    }
    u64 base = guest + texture_layer_size(d) * layer;
    for (u32 l = 0; l < level; l++) {
        u32 lw = DIV_UP(d->width >> l ? d->width >> l : 1, d->bw), lh = DIV_UP(d->height >> l ? d->height >> l : 1, d->bh);
        base += level_size(lw * d->bytes, lh, level_block_height(lh, d->block_height));
    }
    u32 bh = level_block_height(h, d->block_height);
    u32 gobs_x = DIV_UP(row, 64), block_rows = 8u << bh;
    const u8 *src = P(u8, base);
    for (u32 y = 0; y < h; y++) {
        u32 by = y / block_rows, yin = y % block_rows;
        u64 line = (u64)by * gobs_x * (512ull << bh) + (yin / 8) * 512ull;
        for (u32 x = 0; x < row; x += 16) {   /* 16 contiguous bytes within a GOB */
            u64 off = line + (u64)(x / 64) * (512ull << bh) + gob_offset(x, yin);
            u32 n = row - x < 16 ? row - x : 16;
            memcpy(out + (size_t)y * row + x, src + off, n);
        }
    }
}

/* 3D textures: GOBs stack (1 << block_height) high and (1 << block_depth) deep in a block;
 * blocks run across a row, rows of blocks fill a slab of block_depth slices. */
u64 texture_3d_size(const TextureDesc *d) {
    u32 w = DIV_UP(d->width, d->bw) * d->bytes, h = DIV_UP(d->height, d->bh);
    u64 row = (u64)DIV_UP(w, 64) << (9 + d->block_height + d->block_depth);
    return row * DIV_UP(h, 8u << d->block_height) * DIV_UP(d->depth, 1u << d->block_depth);
}

void texture_read_3d(const TextureDesc *d, u64 guest, u8 *out) {
    u32 w = DIV_UP(d->width, d->bw), h = DIV_UP(d->height, d->bh), rowb = w * d->bytes;
    u32 bh = d->block_height, bd = d->block_depth;
    u64 row = (u64)DIV_UP(rowb, 64) << (9 + bh + bd), slab = row * DIV_UP(h, 8u << bh);
    const u8 *src = P(u8, guest);
    for (u32 z = 0; z < d->depth; z++)
        for (u32 y = 0; y < h; y++) {
            u64 base = (z >> bd) * slab + ((u64)(z & ((1u << bd) - 1)) << (9 + bh)) + (u64)((y / 8) >> bh) * row +
                       ((u64)((y / 8) & ((1u << bh) - 1)) << 9);
            for (u32 x = 0; x < rowb; x += 16) {
                u32 n = rowb - x < 16 ? rowb - x : 16;
                memcpy(out + ((size_t)z * h + y) * rowb + x, src + base + ((u64)(x / 64) << (9 + bh + bd)) + gob_offset(x, y % 8), n);
            }
        }
}
