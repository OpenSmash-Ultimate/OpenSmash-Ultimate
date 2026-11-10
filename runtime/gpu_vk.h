/* The Vulkan back end behind gpu_backend.h (gpu_vk.c), in development (docs/TECHNICAL.md, "A second
 * back end: Vulkan"). Windows with SSBU_HAVE_VULKAN for now; elsewhere vk_backend_start says it is
 * not in this build. */
#pragma once
#include <stddef.h>

struct GfxBackend;
/* (gpu_backend.c, once, at the first GPU work, when display.ini's renderer = vulkan, or auto on
 * Windows) Start it: the game's window, a device, a swap chain. Its entry points, or NULL with `why`
 * saying why (Direct3D 11 then draws). automatic (renderer = auto): only on an AMD card, and not
 * after two auto starts in a row that stopped before its 600th frame (a crash or a kill:
 * vulkan_auto_trial.txt in the shader cache's folder counts them, gpu_vk.c's trial; renderer =
 * vulkan takes it away). */
const struct GfxBackend *vk_backend_start(char *why, size_t size, int automatic);
