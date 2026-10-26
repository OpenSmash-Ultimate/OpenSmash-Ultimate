/* What the files of the Vulkan back end share. nvn_vk.c, nvn_vk_draw.c, nvn_vk_m4.c (with nvn_vk_prewarm.h), nvn_vk_menu.c,
 * nvn_vk_flash.c, nvn_vk_wd.c and nvn_vk_store.c are each a build unit of their own: what one of them defines and another
 * uses is declared here, everything else of a file is static. */
#ifndef NVN_VK_INT_H
#define NVN_VK_INT_H
#include <stddef.h>
#include <stdint.h>
#include <windows.h>
/* Session 2: the Khronos headers (reference\upstream\Vulkan-Headers\include, -I in build.py) replace our own vk_min.h subset. */
#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#define VK_NO_PROTOTYPES
#ifdef __APPLE__
#define VK_ENABLE_BETA_EXTENSIONS                       /* VK_KHR_portability_subset (MoltenVK) is declared in vulkan_beta.h */
#endif
#include <vulkan/vulkan.h>
#include "nvn_front.h"                                  /* R holds the front end's records (TexPool, Prog, Range, TargetRef) */
LONG64 tk(void);                                        /* nvn_vk_draw.c: performance counter ticks */
uint64_t fnv(uint64_t h, const void *p, size_t n);      /* nvn_vk_m4.c */
extern int g_m4_quit;                                   /* nvn_vk_m4.c: the game is closing */
extern volatile int g_store_pct;                        /* nvn_vk_m4.c: how much of data.arc the store has read */
int vks_build(const char *dir);                         /* nvn_vk_store.c: the container store, from data.arc */
/* ---------------------------------------------------------------- the loader's functions
 * VKF: the loader's and the instance's (nvn_vk.c nvn_vk_init loads them); VKD: the device's (nvn_vk_draw.c vkd_init).
 * The pointers are pfn_<function>, defined in those two files: a global vkCreateDevice of ours would have the name of
 * the loader's own export. The defines after the tables keep the Vulkan name at every call. A function added to VKF
 * or VKD needs its define too; without it, its calls do not compile. */
#ifdef _WIN32
#define VKF_SURFACE(X) X(VkResult, vkCreateWin32SurfaceKHR, (VkInstance, const VkWin32SurfaceCreateInfoKHR *, const VkAllocationCallbacks *, VkSurfaceKHR *))
#else
#define VKF_SURFACE(X)
#endif
#define VKF(X) \
    X(VkResult, vkEnumerateInstanceLayerProperties, (uint32_t *, VkLayerProperties *)) \
    X(VkResult, vkEnumerateInstanceExtensionProperties, (const char *, uint32_t *, VkExtensionProperties *)) \
    X(VkResult, vkCreateInstance, (const VkInstanceCreateInfo *, const VkAllocationCallbacks *, VkInstance *)) \
    X(void, vkDestroyInstance, (VkInstance, const VkAllocationCallbacks *)) \
    X(PFN_vkVoidFunction, vkGetInstanceProcAddr, (VkInstance, const char *)) \
    X(VkResult, vkEnumeratePhysicalDevices, (VkInstance, uint32_t *, VkPhysicalDevice *)) \
    X(void, vkGetPhysicalDeviceProperties, (VkPhysicalDevice, VkPhysicalDeviceProperties *)) \
    X(void, vkGetPhysicalDeviceQueueFamilyProperties, (VkPhysicalDevice, uint32_t *, VkQueueFamilyProperties *)) \
    X(VkResult, vkEnumerateDeviceExtensionProperties, (VkPhysicalDevice, const char *, uint32_t *, VkExtensionProperties *)) \
    VKF_SURFACE(X) \
    X(void, vkDestroySurfaceKHR, (VkInstance, VkSurfaceKHR, const VkAllocationCallbacks *)) \
    X(VkResult, vkGetPhysicalDeviceSurfaceSupportKHR, (VkPhysicalDevice, uint32_t, VkSurfaceKHR, VkBool32 *)) \
    X(VkResult, vkGetPhysicalDeviceSurfaceCapabilitiesKHR, (VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR *)) \
    X(VkResult, vkGetPhysicalDeviceSurfaceFormatsKHR, (VkPhysicalDevice, VkSurfaceKHR, uint32_t *, VkSurfaceFormatKHR *)) \
    X(VkResult, vkGetPhysicalDeviceSurfacePresentModesKHR, (VkPhysicalDevice, VkSurfaceKHR, uint32_t *, VkPresentModeKHR *)) \
    X(VkResult, vkCreateDevice, (VkPhysicalDevice, const VkDeviceCreateInfo *, const VkAllocationCallbacks *, VkDevice *)) \
    X(void, vkDestroyDevice, (VkDevice, const VkAllocationCallbacks *)) \
    X(void, vkGetDeviceQueue, (VkDevice, uint32_t, uint32_t, VkQueue *)) \
    X(VkResult, vkDeviceWaitIdle, (VkDevice)) \
    X(VkResult, vkCreateSwapchainKHR, (VkDevice, const VkSwapchainCreateInfoKHR *, const VkAllocationCallbacks *, VkSwapchainKHR *)) \
    X(void, vkDestroySwapchainKHR, (VkDevice, VkSwapchainKHR, const VkAllocationCallbacks *)) \
    X(VkResult, vkGetSwapchainImagesKHR, (VkDevice, VkSwapchainKHR, uint32_t *, VkImage *)) \
    X(VkResult, vkAcquireNextImageKHR, (VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t *)) \
    X(VkResult, vkQueuePresentKHR, (VkQueue, const VkPresentInfoKHR *)) \
    X(VkResult, vkQueueSubmit, (VkQueue, uint32_t, const VkSubmitInfo *, VkFence)) \
    X(VkResult, vkCreateCommandPool, (VkDevice, const VkCommandPoolCreateInfo *, const VkAllocationCallbacks *, VkCommandPool *)) \
    X(void, vkDestroyCommandPool, (VkDevice, VkCommandPool, const VkAllocationCallbacks *)) \
    X(VkResult, vkAllocateCommandBuffers, (VkDevice, const VkCommandBufferAllocateInfo *, VkCommandBuffer *)) \
    X(VkResult, vkBeginCommandBuffer, (VkCommandBuffer, const VkCommandBufferBeginInfo *)) \
    X(VkResult, vkEndCommandBuffer, (VkCommandBuffer)) \
    X(VkResult, vkResetCommandBuffer, (VkCommandBuffer, VkFlags)) \
    X(void, vkCmdPipelineBarrier, (VkCommandBuffer, VkFlags, VkFlags, VkFlags, uint32_t, const void *, uint32_t, const void *, uint32_t, const VkImageMemoryBarrier *)) \
    X(void, vkCmdClearColorImage, (VkCommandBuffer, VkImage, VkImageLayout, const VkClearColorValue *, uint32_t, const VkImageSubresourceRange *)) \
    X(void, vkCmdCopyImage, (VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t, const VkImageCopy *)) \
    X(VkResult, vkCreateSemaphore, (VkDevice, const VkSemaphoreCreateInfo *, const VkAllocationCallbacks *, VkSemaphore *)) \
    X(void, vkDestroySemaphore, (VkDevice, VkSemaphore, const VkAllocationCallbacks *)) \
    X(VkResult, vkCreateFence, (VkDevice, const VkFenceCreateInfo *, const VkAllocationCallbacks *, VkFence *)) \
    X(void, vkDestroyFence, (VkDevice, VkFence, const VkAllocationCallbacks *)) \
    X(VkResult, vkWaitForFences, (VkDevice, uint32_t, const VkFence *, VkBool32, uint64_t)) \
    X(VkResult, vkResetFences, (VkDevice, uint32_t, const VkFence *))
#define X(ret, name, args) extern ret (VKAPI_PTR *pfn_##name) args;
VKF(X)
#undef X
#define VKD(X) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkDestroySampler) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetDepthBias) \
    X(vkCmdSetBlendConstants) X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdSetStencilReference) X(vkCmdSetLineWidth) X(vkCmdSetCullMode) X(vkCmdSetFrontFace) \
    X(vkCmdSetPrimitiveTopology) X(vkCmdSetDepthTestEnable) X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp) X(vkCmdSetStencilTestEnable) X(vkCmdSetStencilOp) \
    X(vkCmdSetDepthBiasEnable) X(vkCmdSetPrimitiveRestartEnable) X(vkCmdSetRasterizerDiscardEnable) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) X(vkCmdDraw) X(vkCmdDrawIndexed) \
    X(vkCmdClearAttachments) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdBlitImage) X(vkCmdResolveImage) X(vkCmdClearDepthStencilImage) X(vkGetFenceStatus) \
    X(vkCreateComputePipelines) X(vkCmdDispatch) X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) X(vkGetQueryPoolResults) X(vkCreatePipelineCache) X(vkGetPipelineCacheData) X(vkDestroyPipelineCache) X(vkCmdCopyBuffer)
#define X(n) extern PFN_##n pfn_##n;
VKD(X)
#undef X
#ifdef _WIN32
#define vkCreateWin32SurfaceKHR pfn_vkCreateWin32SurfaceKHR
#endif
#define vkEnumerateInstanceLayerProperties pfn_vkEnumerateInstanceLayerProperties
#define vkEnumerateInstanceExtensionProperties pfn_vkEnumerateInstanceExtensionProperties
#define vkCreateInstance pfn_vkCreateInstance
#define vkDestroyInstance pfn_vkDestroyInstance
#define vkGetInstanceProcAddr pfn_vkGetInstanceProcAddr
#define vkEnumeratePhysicalDevices pfn_vkEnumeratePhysicalDevices
#define vkGetPhysicalDeviceProperties pfn_vkGetPhysicalDeviceProperties
#define vkGetPhysicalDeviceQueueFamilyProperties pfn_vkGetPhysicalDeviceQueueFamilyProperties
#define vkEnumerateDeviceExtensionProperties pfn_vkEnumerateDeviceExtensionProperties
#define vkDestroySurfaceKHR pfn_vkDestroySurfaceKHR
#define vkGetPhysicalDeviceSurfaceSupportKHR pfn_vkGetPhysicalDeviceSurfaceSupportKHR
#define vkGetPhysicalDeviceSurfaceCapabilitiesKHR pfn_vkGetPhysicalDeviceSurfaceCapabilitiesKHR
#define vkGetPhysicalDeviceSurfaceFormatsKHR pfn_vkGetPhysicalDeviceSurfaceFormatsKHR
#define vkGetPhysicalDeviceSurfacePresentModesKHR pfn_vkGetPhysicalDeviceSurfacePresentModesKHR
#define vkCreateDevice pfn_vkCreateDevice
#define vkDestroyDevice pfn_vkDestroyDevice
#define vkGetDeviceQueue pfn_vkGetDeviceQueue
#define vkDeviceWaitIdle pfn_vkDeviceWaitIdle
#define vkCreateSwapchainKHR pfn_vkCreateSwapchainKHR
#define vkDestroySwapchainKHR pfn_vkDestroySwapchainKHR
#define vkGetSwapchainImagesKHR pfn_vkGetSwapchainImagesKHR
#define vkAcquireNextImageKHR pfn_vkAcquireNextImageKHR
#define vkQueuePresentKHR pfn_vkQueuePresentKHR
#define vkQueueSubmit pfn_vkQueueSubmit
#define vkCreateCommandPool pfn_vkCreateCommandPool
#define vkDestroyCommandPool pfn_vkDestroyCommandPool
#define vkAllocateCommandBuffers pfn_vkAllocateCommandBuffers
#define vkBeginCommandBuffer pfn_vkBeginCommandBuffer
#define vkEndCommandBuffer pfn_vkEndCommandBuffer
#define vkResetCommandBuffer pfn_vkResetCommandBuffer
#define vkCmdPipelineBarrier pfn_vkCmdPipelineBarrier
#define vkCmdClearColorImage pfn_vkCmdClearColorImage
#define vkCmdCopyImage pfn_vkCmdCopyImage
#define vkCreateSemaphore pfn_vkCreateSemaphore
#define vkDestroySemaphore pfn_vkDestroySemaphore
#define vkCreateFence pfn_vkCreateFence
#define vkDestroyFence pfn_vkDestroyFence
#define vkWaitForFences pfn_vkWaitForFences
#define vkResetFences pfn_vkResetFences
#define vkCreateBuffer pfn_vkCreateBuffer
#define vkDestroyBuffer pfn_vkDestroyBuffer
#define vkGetBufferMemoryRequirements pfn_vkGetBufferMemoryRequirements
#define vkAllocateMemory pfn_vkAllocateMemory
#define vkFreeMemory pfn_vkFreeMemory
#define vkBindBufferMemory pfn_vkBindBufferMemory
#define vkMapMemory pfn_vkMapMemory
#define vkCreateImage pfn_vkCreateImage
#define vkDestroyImage pfn_vkDestroyImage
#define vkGetImageMemoryRequirements pfn_vkGetImageMemoryRequirements
#define vkBindImageMemory pfn_vkBindImageMemory
#define vkCreateImageView pfn_vkCreateImageView
#define vkDestroyImageView pfn_vkDestroyImageView
#define vkCreateSampler pfn_vkCreateSampler
#define vkDestroySampler pfn_vkDestroySampler
#define vkCreateShaderModule pfn_vkCreateShaderModule
#define vkDestroyShaderModule pfn_vkDestroyShaderModule
#define vkCreateDescriptorSetLayout pfn_vkCreateDescriptorSetLayout
#define vkDestroyDescriptorSetLayout pfn_vkDestroyDescriptorSetLayout
#define vkCreatePipelineLayout pfn_vkCreatePipelineLayout
#define vkDestroyPipelineLayout pfn_vkDestroyPipelineLayout
#define vkCreateGraphicsPipelines pfn_vkCreateGraphicsPipelines
#define vkDestroyPipeline pfn_vkDestroyPipeline
#define vkCreateDescriptorPool pfn_vkCreateDescriptorPool
#define vkDestroyDescriptorPool pfn_vkDestroyDescriptorPool
#define vkResetDescriptorPool pfn_vkResetDescriptorPool
#define vkAllocateDescriptorSets pfn_vkAllocateDescriptorSets
#define vkUpdateDescriptorSets pfn_vkUpdateDescriptorSets
#define vkCmdBeginRendering pfn_vkCmdBeginRendering
#define vkCmdEndRendering pfn_vkCmdEndRendering
#define vkCmdBindPipeline pfn_vkCmdBindPipeline
#define vkCmdBindDescriptorSets pfn_vkCmdBindDescriptorSets
#define vkCmdPushConstants pfn_vkCmdPushConstants
#define vkCmdSetViewport pfn_vkCmdSetViewport
#define vkCmdSetScissor pfn_vkCmdSetScissor
#define vkCmdSetDepthBias pfn_vkCmdSetDepthBias
#define vkCmdSetBlendConstants pfn_vkCmdSetBlendConstants
#define vkCmdSetStencilCompareMask pfn_vkCmdSetStencilCompareMask
#define vkCmdSetStencilWriteMask pfn_vkCmdSetStencilWriteMask
#define vkCmdSetStencilReference pfn_vkCmdSetStencilReference
#define vkCmdSetLineWidth pfn_vkCmdSetLineWidth
#define vkCmdSetCullMode pfn_vkCmdSetCullMode
#define vkCmdSetFrontFace pfn_vkCmdSetFrontFace
#define vkCmdSetPrimitiveTopology pfn_vkCmdSetPrimitiveTopology
#define vkCmdSetDepthTestEnable pfn_vkCmdSetDepthTestEnable
#define vkCmdSetDepthWriteEnable pfn_vkCmdSetDepthWriteEnable
#define vkCmdSetDepthCompareOp pfn_vkCmdSetDepthCompareOp
#define vkCmdSetStencilTestEnable pfn_vkCmdSetStencilTestEnable
#define vkCmdSetStencilOp pfn_vkCmdSetStencilOp
#define vkCmdSetDepthBiasEnable pfn_vkCmdSetDepthBiasEnable
#define vkCmdSetPrimitiveRestartEnable pfn_vkCmdSetPrimitiveRestartEnable
#define vkCmdSetRasterizerDiscardEnable pfn_vkCmdSetRasterizerDiscardEnable
#define vkCmdBindVertexBuffers pfn_vkCmdBindVertexBuffers
#define vkCmdBindIndexBuffer pfn_vkCmdBindIndexBuffer
#define vkCmdDraw pfn_vkCmdDraw
#define vkCmdDrawIndexed pfn_vkCmdDrawIndexed
#define vkCmdClearAttachments pfn_vkCmdClearAttachments
#define vkCmdCopyBufferToImage pfn_vkCmdCopyBufferToImage
#define vkCmdCopyImageToBuffer pfn_vkCmdCopyImageToBuffer
#define vkCmdBlitImage pfn_vkCmdBlitImage
#define vkCmdResolveImage pfn_vkCmdResolveImage
#define vkCmdClearDepthStencilImage pfn_vkCmdClearDepthStencilImage
#define vkGetFenceStatus pfn_vkGetFenceStatus
#define vkCreateComputePipelines pfn_vkCreateComputePipelines
#define vkCmdDispatch pfn_vkCmdDispatch
#define vkCreateQueryPool pfn_vkCreateQueryPool
#define vkDestroyQueryPool pfn_vkDestroyQueryPool
#define vkCmdResetQueryPool pfn_vkCmdResetQueryPool
#define vkCmdWriteTimestamp pfn_vkCmdWriteTimestamp
#define vkGetQueryPoolResults pfn_vkGetQueryPoolResults
#define vkCreatePipelineCache pfn_vkCreatePipelineCache
#define vkGetPipelineCacheData pfn_vkGetPipelineCacheData
#define vkDestroyPipelineCache pfn_vkDestroyPipelineCache
#define vkCmdCopyBuffer pfn_vkCmdCopyBuffer
/* ---------------------------------------------------------------- nvn_vk.c
 * V: the instance, the device and its queue, the surface, the swapchain and its images. */
extern struct VDevice {
    HWND hwnd;
    HMODULE loader;
    VkInstance instance;
    VkDebugUtilsMessengerEXT messenger;
    VkPhysicalDevice gpu;
    VkPhysicalDeviceProperties props;
    uint32_t family;
    VkDevice device;
    VkQueue queue;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkSurfaceFormatKHR format;
    VkPresentModeKHR mode;
    VkExtent2D extent;
    VkImage images[16];
    uint32_t image_count;
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkSemaphore acquired, rendered;
    VkFence fence;
    int fence_pending, alive, validate;
} V;
#define P(a) ((void *)(uintptr_t)(a))                     /* guest addresses are host addresses (as in nvn_gl.c) */
/* ---------------------------------------------------------------- nvn_vk_draw.c
 * D: the features and extensions of the device (vkd_device_info). R: the drawing part's state: the command buffers in
 * flight, the stream ring, the pass, the NVN state as the commands so far left it. */
extern struct VFeatures {
    VkPhysicalDeviceFeatures2 f2; VkPhysicalDeviceVulkan11Features f11; VkPhysicalDeviceVulkan12Features f12; VkPhysicalDeviceVulkan13Features f13;
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vids; VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc; VkPhysicalDeviceProvokingVertexFeaturesEXT pv;
    VkPhysicalDeviceIndexTypeUint8FeaturesEXT iu8; VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT plr; VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT vad;
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3;
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl;
    VkPhysicalDeviceFaultFeaturesEXT fault;
#ifdef VK_KHR_portability_subset
    VkPhysicalDevicePortabilitySubsetFeaturesKHR port; int has_port;
#endif
    const char *exts[16]; uint32_t next;
    int has_vids, has_cbc, has_pv, has_iu8, has_plr, has_vad, has_push, has_eds3, debug;
    int has_gpl, ext_gpl, ext_so, has_fault, has_ckpt;
    VkPhysicalDeviceMemoryProperties mp;
} D;
typedef struct { int block; uint64_t off, size; void *map; VkDeviceMemory mem; } VMem;
#define NSLOT 8
typedef struct { VkCommandBuffer cmd; VkFence fence; VkDescriptorPool pool[16]; int npool, ipool, pending; uint64_t serial; LONG64 ts_frame; } Slot;
typedef struct VTex VTex;
extern struct VRender {
    Slot slot[NSLOT]; int cur, recording; uint64_t serial, done;      /* serial: number of the last submission; done: every submission up to here has finished */
    VkCommandBuffer cmd;
    /* stream ring */
    VkBuffer ring; VMem ring_mem; uint64_t ring_pos, ring_size; uint32_t gen;
    struct { uint64_t addr, size, off; uint32_t gen; } acache[8192];
    VkBuffer zero; VMem zero_mem;
    /* pass */
    int in_pass, pass_w, pass_h;
    int pass_rs; float pass_sx, pass_sy;           /* track O: the pass draws into scaled images (pass_w, pass_h are the images'; sx, sy = image / game) */
    /* NVN state as left by the commands executed so far */
    TexPool *tpool; SamplerPool *spool;
    Prog *prog[6];
    Range ubo[6][16], ssbo[6][32], vbuf[32];
    uint64_t nvn_tex[6][32], c2[6][112];
    uint32_t tex_int[6], tex_sint[6];
    uint32_t vattr[32]; int nattr;
    struct { uint32_t stride, divisor; } vstream[32];
    uint64_t ds; uint32_t chanmask, sample_mask;
    int sref[2], svmask[2], swmask[2];
    float alpha_ref; int alpha_func;
    uint8_t blend[8][8], blend_on, logic, poly[4], ms[24];
    float viewport[4], drange[2], blendc[4], point_size, line_width, polyoff[3];
    int scissor[4], have_viewport, have_scissor, rast_discard, depth_clamp, restart, render_off;
    int ncol; TargetRef col[8], depth; int have_targets;
    int64_t row_stride, image_stride;
    int dyn_dirty, vi_dirty, dyn3_dirty, dyn3_samples;      /* dyn3: the extended-dynamic-state-3 group (blend, masks, polygon mode, multisample) */
    void *pipe_bound;
    LONG64 frame_draws, frame_skipped, frame_clears, pipelines, modules, uploads, upload_bytes, images, images_live, views, ring_wraps, feedback, flushes, sets;
    LONG64 sets_reused, tset_collisions, dummy_kind;
    LONG64 dispatches, cb_bytes, cb_syncs, cb_draws, cb_made, cb_waits, cb_wait_ticks, cb_guest_words, cb_conflict_words, readbacks;
} R;
#define VKC(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) vk_fatal(#call, r_); } while (0)
void ckpt_op(const char *what, uint64_t obj, int a, int b, int c);             /* PR #32, device loss report: a marker before a command that is not a draw */
#define CKPT_OP(what, obj, a, b, c) do { if (D.has_ckpt) ckpt_op(what, (uint64_t)(uintptr_t)(obj), (int)(a), (int)(b), (int)(c)); } while (0)
/* V_novids (2026-10-21): a GPU without VK_EXT_vertex_input_dynamic_state keeps its own pipeline list: its keys name a
 * vertex layout (nvk_vl_*), those of the other GPUs do not (the layout of pipes.bin and its keys are unchanged there).
 * macOS keeps the layout inside the key (MAC2) and its pipes.bin. */
#ifdef __APPLE__
#define VK_PIPES_NOVI "pipes.bin"
#define VK_VL_FILE "the keys themselves"
#else
#define VK_PIPES_NOVI "pipes_novi.bin"
#define VK_VL_FILE "vi_layouts.bin"
#endif
#define VK_PIPES_BIN (D.has_vids ? "pipes.bin" : VK_PIPES_NOVI)
typedef struct VView { EView e; int vtype, attach; VkImageView view; struct VView *next; } VView;
struct VTex {
    uint32_t magic; Tex *t; VkImage img; VMem mem; VkFormat fmt; VkImageLayout layout; VkFlags aspect;
    int w, h, d, levels, layers, samples, expand, decode, is3d, feedback, bpb, bw, bh;
    int rs; float sx, sy;                          /* track O: rs = 1 when the image is scaled from the game's texture, smaller or larger (w, h are the image's; sx, sy = image / game; PR #8) */
    VView *views; VTex *next, *prev;
    VTex *shadow; int shadow_ok;                   /* 2026-10-21 (AMD render bugs): a copy of this depth target for the shaders that sample it while it is bound (depth_copy_on) */
};
#define RS_ON (rs_scale() != 1.0f)
enum { DIM_1D, DIM_2D, DIM_3D, DIM_CUBE, DIM_1DA, DIM_2DA, DIM_CUBEA };
/* MAC2 / V_novids (2026-10-21). A GPU without VK_EXT_vertex_input_dynamic_state (MoltenVK; an RX 550 / Polaris on AMD's
 * Windows driver): the vertex layout of a draw (per location: format, binding, offset; per binding: instance rate,
 * divisor) is part of the pipeline, the strides stay dynamic (VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE, Vulkan 1.3).
 * Where the layout lives in the key:
 *   - macOS: inside it (PKEY_VI = NvkVl vi: the MAC2 fields under one name, the same bytes), in pipes.bin as before;
 *   - elsewhere: the three pad bytes of the key hold the NUMBER of the layout (nvk_vl_id: 24 bits of a hash of the
 *     layout, the next free number on a collision), the layouts are in ../cache/vk/vi_layouts.bin, the keys in
 *     pipes_novi.bin. On a GPU that has the extension the pad bytes stay zero: PKey, pipes.bin and the prepared markers
 *     are those of before, to the byte. nvn_vk_m4.c only names PKEY_VI / NVK_VI_PIPE / VK_PIPES_BIN / nvk_vl_rec_ok on
 *     lines it already had (its WD_SLEEP uses __LINE__). */
typedef struct { uint32_t na, nb, rate; uint32_t loc[16], fmt[16], bind[16], off[16], div[17]; } NvkVl;
#ifdef __APPLE__
#define PKEY_VI NvkVl vi;
#else
#define PKEY_VI
#endif
typedef struct {
    VkVertexInputBindingDescription b[17];
    VkVertexInputAttributeDescription a[16];
    VkVertexInputBindingDivisorDescriptionEXT dd[17];
    VkPipelineVertexInputDivisorStateCreateInfoEXT di;
    VkPipelineVertexInputStateCreateInfo vi;
} NvkVi;
void nvk_vi_pipe(const void *key, VkDynamicState *dyn, VkGraphicsPipelineCreateInfo *gi, NvkVi *s);
int nvk_vl_rec_ok(const void *key);                /* a key of the pipeline list whose layout is known (else it is not prewarmed) */
#define NVK_VI_PIPE(k, dyn, gi) NvkVi nvk_vi_; nvk_vi_pipe(k, dyn, gi, &nvk_vi_);
/* ---------------------------------------------------------------- nvn_vk_m4.c
 * The stages and the pipelines the workers make (VStage, PKey, Pipe), the freeze watchdog's wait registry (WdWait),
 * the kinds of hold (H_*) and the look-ahead over a command list (Scan). */
typedef struct { uint8_t unit, dim, shadow; uint16_t binding; } VSmp;
typedef struct VStage {
    uint64_t h, hb;                                /* h: this variant (code, constants, canonical key, forced outputs); hb: code and constants only */
    int stage, code_len, ok; volatile LONG state, cp_state;      /* state: 1 being made, 2 made (ok says how it went); cp_state: compute pipeline 0 none, 1 being made, 2 made */
    uint32_t key[4], fo, in_attr, out_attr, id, cbufs, ssbos, ssbo_w, images, dep, tex_units; int uses_alpha, nsmp, cp_recorded, from_rec, fragonly, temp;      /* fragonly (session 9): handed over by a program without a vertex stage */
    VkPipeline cp; VkPipelineLayout cl; VkDescriptorSetLayout cdl[2];      /* compute stage: its pipeline */
    uint32_t local[3];
    VSmp smp[64];
    VkShaderModule mod; struct VStage *next, *next_h;
    VkPipeline lib, lib2; volatile LONG lib_state, lib2_state;      /* session 9 (SSBU_VK_GPL=2): the stage's pipeline library; lib2: a vertex stage with the empty fragment shader state */
} VStage;
typedef struct { volatile LONG tid; const char *fn; int line; LONG64 since; } WdWait;
enum { H_STAGE_DRAW, H_STAGE_WAIT, H_PIPE_DRAW, H_PIPE_WAIT, H_N };
typedef struct { uint32_t vs, fs, chanmask, sample_mask; int32_t cfmt[8], dfmt; uint8_t topo_class, ncolor, samples, poly, a2c, dclamp, logic, sshade, blend_on, pad[3]; uint8_t blend[8][8]; PKEY_VI } PKey;
typedef struct Pipe { PKey k; VkPipeline p, p_fast; /* p_fast: the pipeline linked at once, after the optimised one took its place */ VkPipelineLayout layout; VkDescriptorSetLayout dl[2]; int nb[2], push0; VStage *vs, *fs; volatile LONG state; uint8_t recorded, predicted, used, libbed, lib_how; struct Pipe *next; } Pipe;      /* libbed: linked from libraries (layout and set layouts belong to the pair library); lib_how: 0 the pair library was there, 1 compiled by this build, 2 waited for */      /* state: 0 queued for a worker, 1 being made, 2 made */
typedef struct { Prog *prog[2]; int ncol, nattr, sig_ok, attr_new; TargetRef col[8], depth; uint32_t vattr[32], ints, sints, sig_tag; uint8_t logic; PKey sig; } Scan;
/* ---------------------------------------------------------------- what one file defines and the others use
 * Each file's other functions and objects stay static. Under the name of the file that defines them, in its order. */
/* nvn_vk.c */
extern volatile LONG64 g_vk_frames, g_vk_recreated, g_vk_skipped, g_vk_errors;
const char *vk_result_name(VkResult r);
void vk_fatal(const char *what, VkResult r);
int vk_ok(const char *what, VkResult r);
void nvn_vk_missing(const char *what);
int has_name(const VkExtensionProperties *e, uint32_t n, const char *name);
extern int g_mu_shot_src;                               /* U1: the swapchain images can be read back (SSBU_MENU_TEST only) */
void swapchain_destroy(void);
int swapchain_create(void);
void *vk_sym(const char *name);
int nvn_vk_init(void *hwnd);
void nvn_vk_report(FILE *out);
/* nvn_vk_draw.c */
void vkd_fail(const char *fmt, ...);
int vk_gpl_mode(void);
void vkd_device_info(VkDeviceCreateInfo *di, const VkExtensionProperties *dext, uint32_t ndext);
VMem vmem_alloc(VkMemoryRequirements rq, VkFlags want, int cls);
void vmem_free(VMem m);
VkBuffer vbuf_new(uint64_t size, VkFlags usage, VMem *mem);
extern LONG64 g_tm[12];
extern LONG64 g_battle_frames;
extern LONG64 g_fu_fr_n, g_fu_fr_linked, g_fu_fr_made, g_fu_fr_tk, g_fu_fr_newsig;
VkFlags fmt_aspect(VkFormat f);
extern LONG64 g_rs_stages, g_rs_fcs, g_rs_tf;
float rs_scale(void);
void image_barrier(VkImage img, VkFlags aspect, VkImageLayout from, VkImageLayout to);
extern LONG64 g_ovl_presents, g_ovl_frames;             /* extra presents of a held loading screen; normal frames drawn with the thin bar */
void vkd_lost_report(void);                             /* PR #32 (nvn_vk.c vk_fatal): what the driver and the markers say about the loss */
void compute_pipe_make(VStage *v);
int pkey_targets_of(PKey *k, int ncol, const TargetRef *col, const TargetRef *depth);
void pkey_targets(PKey *k);
void present_progress(float frac);                      /* session 11: the held loading screen presented again with the progress bar */
void vkd_init(void);
void vkd_shutdown(void);
/* nvn_vk_m4.c, with nvn_vk_prewarm.h */
extern CRITICAL_SECTION g_sh_lock;
extern CONDITION_VARIABLE g_jq_cv;
extern WdWait g_wd_wait[64];
extern volatile LONG64 g_wd_beat;
extern volatile LONG g_wd_where;
extern volatile LONG g_pcsave_want;
extern volatile LONG64 g_wd_try;
extern VkPipelineCache g_pcache;
extern LONG64 g_pipe_miss;
VkResult pw_compute(const VkComputePipelineCreateInfo *info, VkPipeline *out);
extern VStage *g_vstage[4096];
extern uint32_t g_vstage_ids;
extern LONG64 g_hold_n[H_N], g_hold_tk[H_N], g_hold_max;
void hold_note(int what, LONG64 ticks);
int holdlog_on(void);
extern volatile LONG g_in_fight;
extern volatile LONG64 g_fight_last;
int fight_recent(void);
extern LONG64 g_jq_queued, g_jq_done, g_bq_queued, g_bq_done;
const char *vkc_file(const char *name);
extern uint64_t g_spv_unused;
extern int g_offline;
VkShaderModule module_from_glsl(int stage, const char *glsl, const char *what, uint64_t *spv_hash);
extern LONG64 g_skip_frames, g_skip_fr_n;
extern int g_skip_flag;
VStage *vstage_get(Prog *p, int s, const uint32_t *key, uint32_t fo);
extern Pipe *g_pipes[1024];
VkBlendFactor blend_factor(unsigned f);
VkBlendOp blend_op(unsigned e);
extern CRITICAL_SECTION g_gpl_lock;
extern DWORD g_vk_render_tid;
extern LONG64 g_gpl_dlink;
extern VkDescriptorSetLayout g_udl[2];
void gpl_destroy(void);
void piperec_append(const PKey *k, VStage *vs, VStage *fs, int compute);
extern LONG64 g_pq_queued, g_pq_done, g_pq_running;
extern int g_prep_state;
extern HANDLE g_prep_thread;
extern volatile LONG g_prep_urgent;
extern LONG64 g_prep_fight_frames;
extern int g_prep_nfiles, g_prep_read;
extern int g_scan_on;
extern struct VRtLog { int w, h, ncol, cf, df, lv; long long n, fight, first; } g_rtlog[256];      /* SSBU_VK_RTLOG=1: render-target sizes */
extern int g_rtlog_n;
Pipe *pipe_get(const PKey *k, VStage *vs, VStage *fs);
int compute_pipe_ensure(VStage *v);
extern HANDLE g_pw_thread[16];
extern int g_pw_nthread;
extern LONG64 g_bar_n, g_bar_timeout;
void m4_barrier(LONG64 frame, LONG64 draws);
void nvn_vk_program_set(Prog *prog);
void prewarm_start(void);
void scan_begin(Scan *s);
void scan_list(uint64_t handle, int depth, Scan *s);
void prewarm_wait(void);
void m4_report(void);
void m4_stop(void);
extern char g_prep_marker[240];
extern volatile LONG64 g_gap_prev, g_gap_max;
extern int g_prep_limited;
void pcache_save_at(int where, int marker);
void pcache_save(void);
extern int g_prep_marked;
extern int g_prep_loadall;
/* nvn_vk_menu.c */
void menu_vk_views_drop(void);                          /* U1: its views of the swapchain images */
void menu_vk_draw(uint32_t index);                      /* U1: the settings menu */
/* nvn_vk_flash.c (session 13: the white-flash detector) */
void fl_pass(int w, int h, int fmt, int win, int ncol, int depth);
void fl_capture(VTex *vt, const int *s4, LONG64 frame, int layout_before);
void fl_shutdown(void);
/* nvn_vk_wd.c (session 12: the freeze watchdog) */
void wd_start(void);
void wd_stop(void);
#endif
