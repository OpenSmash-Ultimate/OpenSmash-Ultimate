#pragma once
#include "host.h"
#include <string.h>

typedef struct ModAssetPayload { const u8 *bytes; u32 size; } ModAssetPayload;

static u32 asset_u32(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }
static u64 asset_u64(const u8 *p) { u64 v; memcpy(&v, p, 8); return v; }

static int asset_animation(const u8 *p, u32 size, ModAssetPayload *out) {
    if (size < 72 || memcmp(p, "HBSS", 4) || memcmp(p + 16, "MINA\2\0", 6) || p[22] > 1 || p[23]) return 0;
    u64 relative = asset_u64(p + 56), count = asset_u64(p + 64);
    if (!relative || !count || relative > size - 56 || count > size - 56 - relative) return 0;
    *out = (ModAssetPayload){p + 56 + relative, (u32)count};
    return 1;
}

static int asset_texture(const u8 *p, u32 size, ModAssetPayload *out) {
    if (size <= 0xb0) return 0;
    const u8 *footer = p + size - 0xb0;
    if (memcmp(footer + 0x40, " XNT", 4) || memcmp(footer + 0xa8, " XET\1\0\2\0", 8) ||
        !memchr(footer + 0x44, 0, 64) || asset_u32(footer + 0xa4) != size - 0xb0 ||
        !asset_u32(footer + 0x84) || !asset_u32(footer + 0x88) || !asset_u32(footer + 0x8c) ||
        !asset_u32(footer + 0xa0)) return 0;
    u32 levels = asset_u32(footer + 0x98);
    if (!levels || levels > 16) return 0;
    u64 total = 0;
    for (u32 i = 0; i < levels; i++) {
        u32 mip = asset_u32(footer + i * 4);
        if (!mip) return 0;
        total += mip;
    }
    /* Exporters can list linear mip sizes while the payload includes block-linear padding. */
    if (total > (size - 0xb0) / asset_u32(footer + 0xa0)) return 0;
    *out = (ModAssetPayload){p, size - 0xb0};
    return 1;
}

static int asset_pair(int texture, const u8 *old, u32 old_size, const u8 *next, u32 next_size,
                      ModAssetPayload *a, ModAssetPayload *b) {
    if (old_size != next_size) return 0;
    if (texture) {
        if (!asset_texture(old, old_size, a) || !asset_texture(next, next_size, b)) return 0;
        const u8 *x = old + a->size, *y = next + b->size;
        return !memcmp(x, y, 0x44) && !memcmp(x + 0x84, y + 0x84, 0x2c);
    }
    if (!asset_animation(old, old_size, a) || !asset_animation(next, next_size, b) ||
        a->size < 16 || a->size != b->size || a->bytes - old != b->bytes - next) return 0;
    u32 prefix = (u32)(a->bytes - old);
    return !memcmp(old, next, prefix) &&
           !memcmp(a->bytes + a->size, b->bytes + b->size, old_size - prefix - a->size);
}
