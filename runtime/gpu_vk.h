/* The Vulkan back end behind gpu_backend.h (gpu_vk.c), in development (docs/TECHNICAL.md, "A second
 * back end: Vulkan"). Windows with SSBU_HAVE_VULKAN for now; elsewhere vk_backend_start says it is
 * not in this build. */
#pragma once
#include <stddef.h>

struct GfxBackend;
/* (gpu_backend.c, once, at the first GPU work, when display.ini's renderer = vulkan) Start it: the
 * game's window, a device, a swap chain. Its entry points, or NULL with `why` saying why (Direct3D
 * 11 then draws). */
const struct GfxBackend *vk_backend_start(char *why, size_t size);
