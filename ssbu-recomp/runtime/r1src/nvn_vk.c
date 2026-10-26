/* Track R1 round 5: the Vulkan back end of ssbu_l1q.exe (the tree nvn_vk.c, which ssbu_l1q.exe was built from) with dirty-page marks on its compute write-backs into
 * guest memory (nvn_vk_draw.c cb_keep_back / cb_land, under R1_MARK). Those write-backs are in nvn_vk_draw.c, a build unit of
 * its own now (r1src/nvn_vk_draw.c); this file is the rest of the back end. Nothing else differs. */
#define R1_MARK 1
#include "../nvn_vk.c"
