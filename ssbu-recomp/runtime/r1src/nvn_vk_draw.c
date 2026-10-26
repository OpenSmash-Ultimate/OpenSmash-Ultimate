/* Track R1 round 5: the Vulkan back end's drawing part with dirty-page marks on its compute write-backs into guest memory
 * (cb_keep_back / cb_land, under R1_MARK). It was part of nvn_vk.c (r1src/nvn_vk.c) until it got a build unit of its own.
 * Nothing else differs from the tree file. */
#define R1_MARK 1
#include "../nvn_vk_draw.c"
