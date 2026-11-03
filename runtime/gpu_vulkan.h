/* The Vulkan back end, in development (docs/TECHNICAL.md, "A second back end: Vulkan"): for now
 * the check of Vulkan on this PC that display.ini's renderer = vulkan asks for (gpu_renderer.c). */
#pragma once
#include <stddef.h>

/* Whether this PC can run the Vulkan back end: the system's Vulkan loader, an instance, the card
 * display.ini's gpu names (else the fastest), a graphics queue that can present to `window` (the
 * game's HWND; NULL: not checked), and a device made with it, then everything made let go again.
 * 1 if all of it worked, 0 if not; either way `what` says what was found or what failed, and
 * the cards found are logged ("[vulkan] ..." lines). SSBU_VULKAN_LOADER names another loader
 * (a path) to load. */
int vk_probe(void *window, char *what, size_t size);
/* The system's Vulkan loader, loaded once and kept: its vkGetInstanceProcAddr (a
 * PFN_vkGetInstanceProcAddr), or NULL with `why` saying why (gpu_vk.c's back end starts from it). */
void *vk_loader(char *why, size_t size);
