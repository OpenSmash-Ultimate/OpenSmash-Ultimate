/* Presentation: the display's buffer queue hands over the buffer to show; the back end
 * draws the render target that lives at that buffer's memory.
 *
 * The buffer arrives as the driver flattened it: a parcel header (3 words), the Android
 * GraphicBuffer ("RFBG", width, height, stride, format, usage, ...) and then the native
 * handle the NVIDIA driver attaches - whose words include the nvmap handle of the memory
 * (word 32) and the buffer's byte offset in it (word 33), its width and height (26, 27). */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>

#include "gpu_backend.h"
#include "present.h"

void present_frame(const u8 *graphic_buffer, u32 size) {
    const u32 *w = (const u32 *)graphic_buffer;
    if (size < 34 * 4 || w[3] != 0x47424652) {
        static int warned;
        if (!warned++) fprintf(stderr, "[present] unexpected graphic buffer layout (%u bytes)\n", size);
        return;
    }
    u32 handle = w[32], offset = w[33];
    if (!handle || handle >= NVMAP_MAX || !gpu_maps[handle].addr) return;
    gfx_present(gpu_maps[handle].addr + offset, w[26], w[27]);
}
