/* The rendering back end: Vulkan, in development (docs/TECHNICAL.md, "A second back end: Vulkan").
 *
 * A second implementation of gpu_backend.h beside gpu_d3d11.c, chosen at the first GPU work when
 * display.ini's renderer = vulkan (gpu_backend.c); if it cannot start, Direct3D 11 draws. As in
 * gpu_d3d11.c the game's render targets live on the host GPU, each a Vulkan image keyed by the guest
 * address the console would have rendered into, made the render scale's times the game's size; when
 * the display shows one, it is drawn into the window (16:9, centred) and presented.
 *
 * What it draws (the plan's steps 4 to 6): the clears, the 2D engine's blits, the draws (see "draws":
 * the same shaders, parameters and state as gpu_d3d11.c's, in dynamic renderings), compute launches
 * (their results kept on the GPU: "resident regions"), and the frames shown - swap chain on the game's
 * window, pacing (clock: mailbox; vrr and SSBU_SPEED: immediate; display: the monitor's refreshes
 * through DXGI, as Direct3D 11 waits for them), the window's size and F11, frames queued as
 * display.ini's latency says, SSBU_CAPTURE and F3 pictures. The overlay and the settings menu are not
 * drawn (step 7); masked clears are counted, not drawn.
 *
 * Every image is kept in the GENERAL layout and every operation outside a rendering is followed by a
 * full memory barrier, as is the end of each rendering: simple and correct, to be narrowed later. The
 * work of a frame is one command buffer, submitted at the present. Every entry point takes the back
 * end's lock, as gpu_d3d11.c's do; shaders and pipelines are made by threads of their own (the makers). */
#define _CRT_SECURE_NO_WARNINGS
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_vk.h"

#if defined(SSBU_HAVE_VULKAN) && defined(_WIN32)
#define COBJMACROS
#include <dxgi.h>

#include "display.h"
#include "gpu_backend.h"
#include "gpu_shader.h"
#include "gpu_shader_cache.h"
#include "gpu_spirv.h"
#include "gpu_texture.h"
#include "gpu_vulkan.h"
#include "gpu_watch.h"
#include "gpu_window.h"
#include "debug_profile.h"
#include "gpu_menu.h"
#include "gpu_overlay.h"
#include "net.h"
#include "pad.h"

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

extern u64 gfx_stat_clears, gfx_stat_presented, gfx_stat_blits;   /* (gpu_d3d11.c's counters, printed by host.c: either back end counts) */

/* ---- the Vulkan functions used ------------------------------------------------------------ */
#define VK_INSTANCE_FUNCS(X) X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) \
    X(GetPhysicalDeviceFeatures) X(GetPhysicalDeviceQueueFamilyProperties) X(GetPhysicalDeviceMemoryProperties) \
    X(GetPhysicalDeviceFormatProperties) X(EnumerateDeviceExtensionProperties) X(CreateDevice) X(GetDeviceProcAddr) \
    X(CreateWin32SurfaceKHR) X(DestroySurfaceKHR) X(GetPhysicalDeviceSurfaceSupportKHR) \
    X(GetPhysicalDeviceSurfaceCapabilitiesKHR) X(GetPhysicalDeviceSurfaceFormatsKHR) X(GetPhysicalDeviceSurfacePresentModesKHR) \
    X(GetPhysicalDeviceFeatures2)
#define VK_DEVICE_FUNCS(X) X(GetDeviceQueue) X(DeviceWaitIdle) X(QueueSubmit) X(QueueWaitIdle) \
    X(CreateCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) X(ResetCommandBuffer) \
    X(CreateFence) X(WaitForFences) X(ResetFences) X(CreateSemaphore) X(DestroySemaphore) \
    X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(BindImageMemory) \
    X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) \
    X(AllocateMemory) X(FreeMemory) X(MapMemory) X(UnmapMemory) \
    X(CmdPipelineBarrier) X(CmdClearColorImage) X(CmdClearDepthStencilImage) X(CmdBlitImage) X(CmdCopyImage) X(CmdCopyImageToBuffer) \
    X(CreateSwapchainKHR) X(DestroySwapchainKHR) X(GetSwapchainImagesKHR) X(AcquireNextImageKHR) X(QueuePresentKHR) \
    X(CreateImageView) X(DestroyImageView) X(CreateBufferView) X(DestroyBufferView) X(CreateSampler) X(CreateShaderModule) \
    X(DestroyShaderModule) X(CreateGraphicsPipelines) X(CreatePipelineCache) X(CreateDescriptorSetLayout) X(CreatePipelineLayout) \
    X(CreateDescriptorPool) X(ResetDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CmdBindPipeline) \
    X(CmdBindDescriptorSets) X(CmdBindIndexBuffer) X(CmdDraw) X(CmdDrawIndexed) X(CmdSetViewport) X(CmdSetScissor) \
    X(CmdSetBlendConstants) X(CmdSetStencilReference) X(CmdSetStencilCompareMask) X(CmdSetStencilWriteMask) \
    X(CmdCopyBufferToImage) X(CmdBeginRenderingKHR) X(CmdEndRenderingKHR) X(GetPipelineCacheData) \
    X(CreateComputePipelines) X(CmdDispatch) X(CmdCopyBuffer) X(CreateDescriptorUpdateTemplate) X(UpdateDescriptorSetWithTemplate) \
    X(DestroyDevice) X(DestroyCommandPool) X(DestroyFence) X(DestroySampler) X(DestroyPipelineCache) X(DestroyPipelineLayout) \
    X(DestroyDescriptorSetLayout)
#define DECLARE(name) static PFN_vk##name vk##name;
VK_INSTANCE_FUNCS(DECLARE)
VK_DEVICE_FUNCS(DECLARE)
#undef DECLARE
static PFN_vkCreateInstance vkCreateInstance_;

static VkInstance instance;
static VkPhysicalDevice physical;
static VkPhysicalDeviceMemoryProperties memory_props;
static VkDevice device;
static VkQueue queue;
static uint32_t queue_family;
static VkSurfaceKHR vk_surface;
static HWND window;
static CRITICAL_SECTION lock;
static volatile DWORD last_enter_thread;
static char card[300];

/* ---- device memory: images share big chunks of it (drivers allow few allocations: 4,096) ------- */
static int memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < memory_props.memoryTypeCount; i++)
        if ((bits >> i & 1) && (memory_props.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}

#define CHUNK_SIZE (64ull << 20)
typedef struct Range { VkDeviceSize off, size; } Range;
typedef struct Chunk { VkDeviceMemory mem; uint32_t type; Range *free; u32 nfree, cap; u8 *map; } Chunk;   /* (free: sorted by offset) */
static Chunk chunks[512];
static u32 nchunks;
typedef struct Alloc { VkDeviceMemory mem; VkDeviceSize off, size; int chunk; u8 *ptr; } Alloc;   /* (chunk -1: memory of its own; ptr: where the CPU sees it, if it can) */
static u64 vk_stat_device_bytes;
static VkDeviceSize granularity = 1024;   /* (buffers and images share chunks: bufferImageGranularity) */

static int range_insert(Chunk *c, u32 at, Range r) {
    if (c->nfree == c->cap) {
        u32 cap = c->cap * 2 + 16;
        Range *n = realloc(c->free, sizeof *n * cap);
        if (!n) return 0;
        c->free = n;
        c->cap = cap;
    }
    memmove(&c->free[at + 1], &c->free[at], sizeof *c->free * (c->nfree - at));
    c->free[at] = r;
    c->nfree++;
    return 1;
}

static int chunk_take(Chunk *c, VkDeviceSize size, VkDeviceSize align, VkDeviceSize *off) {
    if (!align) align = 1;
    for (u32 i = 0; i < c->nfree; i++) {
        Range r = c->free[i];
        VkDeviceSize start = (r.off + align - 1) / align * align, end = start + size, rend = r.off + r.size;
        if (end > rend) continue;
        if (start > r.off) {   /* (the alignment's gap stays free) */
            if (end < rend && !range_insert(c, i + 1, (Range){end, rend - end})) return 0;
            c->free[i].size = start - r.off;
        } else if (end < rend) c->free[i] = (Range){end, rend - end};
        else { memmove(&c->free[i], &c->free[i + 1], sizeof *c->free * (c->nfree - i - 1)); c->nfree--; }
        *off = start;
        return 1;
    }
    return 0;
}

static void chunk_give(Chunk *c, VkDeviceSize off, VkDeviceSize size) {
    u32 i = 0;
    while (i < c->nfree && c->free[i].off < off) i++;
    if (i > 0 && c->free[i - 1].off + c->free[i - 1].size == off) {
        c->free[i - 1].size += size;
        if (i < c->nfree && c->free[i - 1].off + c->free[i - 1].size == c->free[i].off) {
            c->free[i - 1].size += c->free[i].size;
            memmove(&c->free[i], &c->free[i + 1], sizeof *c->free * (c->nfree - i - 1));
            c->nfree--;
        }
        return;
    }
    if (i < c->nfree && off + size == c->free[i].off) { c->free[i].off = off; c->free[i].size += size; return; }
    range_insert(c, i, (Range){off, size});
}

/* Memory for an image or buffer: device local where there is some; host: memory the CPU writes (a->ptr). */
static int dev_alloc_in(const VkMemoryRequirements *mr, Alloc *a, int host) {
    VkMemoryPropertyFlags want = host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    int type = memory_type(mr->memoryTypeBits, want);
    if (type < 0 && !host) type = memory_type(mr->memoryTypeBits, 0);
    memset(a, 0, sizeof *a);
    if (type < 0) return 0;
    int mappable = (memory_props.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    VkDeviceSize align = mr->alignment > granularity ? mr->alignment : granularity;
    a->size = mr->size;
    if (mr->size > CHUNK_SIZE / 4) {   /* big ones: their own */
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr->size, (uint32_t)type};
        if (vkAllocateMemory(device, &mai, NULL, &a->mem) != VK_SUCCESS) { a->mem = VK_NULL_HANDLE; return 0; }
        if (mappable && vkMapMemory(device, a->mem, 0, VK_WHOLE_SIZE, 0, (void **)&a->ptr) != VK_SUCCESS) a->ptr = NULL;
        if (host && !a->ptr) { vkFreeMemory(device, a->mem, NULL); a->mem = VK_NULL_HANDLE; return 0; }
        a->chunk = -1;
        vk_stat_device_bytes += mr->size;
        return 1;
    }
    for (u32 i = 0; i < nchunks; i++)
        if (chunks[i].type == (uint32_t)type && chunk_take(&chunks[i], mr->size, align, &a->off)) {
            a->mem = chunks[i].mem;
            a->chunk = (int)i;
            a->ptr = chunks[i].map ? chunks[i].map + a->off : NULL;
            return 1;
        }
    if (nchunks == sizeof chunks / sizeof *chunks) return 0;
    Chunk *c = &chunks[nchunks];
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, CHUNK_SIZE, (uint32_t)type};
    if (vkAllocateMemory(device, &mai, NULL, &c->mem) != VK_SUCCESS) return 0;
    c->type = (uint32_t)type;
    c->nfree = 0;
    c->map = NULL;
    if (mappable && vkMapMemory(device, c->mem, 0, VK_WHOLE_SIZE, 0, (void **)&c->map) != VK_SUCCESS) c->map = NULL;
    if ((host && !c->map) || !range_insert(c, 0, (Range){0, CHUNK_SIZE}) || !chunk_take(c, mr->size, align, &a->off)) {
        vkFreeMemory(device, c->mem, NULL);
        return 0;
    }
    vk_stat_device_bytes += CHUNK_SIZE;
    a->mem = c->mem;
    a->chunk = (int)nchunks++;
    a->ptr = c->map ? c->map + a->off : NULL;
    return 1;
}
static int dev_alloc(const VkMemoryRequirements *mr, Alloc *a) { return dev_alloc_in(mr, a, 0); }

static void dev_free(Alloc *a) {
    if (!a->mem) return;
    if (a->chunk < 0) { vkFreeMemory(device, a->mem, NULL); vk_stat_device_bytes -= a->size; }
    else chunk_give(&chunks[a->chunk], a->off, a->size);
    memset(a, 0, sizeof *a);
}

/* An image in device memory, in the GENERAL layout once the command buffer runs (from `c`). */
static VkImage make_image(const VkImageCreateInfo *ici, Alloc *a) {
    VkImage image;
    if (vkCreateImage(device, ici, NULL, &image) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(device, image, &mr);
    if (!dev_alloc(&mr, a) || vkBindImageMemory(device, image, a->mem, a->off) != VK_SUCCESS) {
        dev_free(a);
        vkDestroyImage(device, image, NULL);
        return VK_NULL_HANDLE;
    }
    return image;
}

/* ---- frames: command buffers in flight ------------------------------------------------------ */
/* Each frame has its own upload memory (blocks the CPU writes: buffers, indices, texture data, per-draw
 * parameters), its buffer views and descriptor sets: all of it taken again once the GPU is done with
 * the frame. */
#define MAX_FRAMES 3
#define BLOCK_SIZE (32ull << 20)
#define MAX_BLOCKS 64
#define MAX_POOLS 64
typedef struct Grave { VkImage image; VkBuffer buffer; VkImageView view; Alloc alloc; VkBufferView bview; } Grave;
typedef struct Block { VkBuffer buf; VkDeviceMemory mem; u8 *map; VkDeviceSize size, used; } Block;
typedef struct Frame {
    VkCommandBuffer cmd;
    VkFence done;
    VkSemaphore acquired;
    int recording, submitted;
    Grave *graves;   /* released once this frame's work is done */
    u32 ngraves, graves_cap;
    Block blocks[MAX_BLOCKS];
    u32 nblocks;
    VkBufferView *views;   /* (made for this frame's buffers) */
    u32 nviews, views_cap;
    VkDescriptorPool pools[MAX_POOLS];
    u32 npools, pool_at;
    u64 serial;   /* (frames begun before it, and it) */
} Frame;
static Frame frames[MAX_FRAMES];
static u32 nframes = 2, frame_at;
static u64 frame_serial;
static VkCommandPool pool;

static u64 vk_stat_draws, vk_stat_drawn, vk_stat_skip_shader, vk_stat_skip_target, vk_stat_skip_topology, vk_stat_skip_other;
static u64 vk_stat_dispatches, vk_stat_dispatched, vk_stat_clears_skipped, vk_stat_blits_skipped, vk_stat_presents_skipped;
static u64 vk_stat_local_blocks, vk_ns_draw, vk_ns_dispatch, vk_ns_wait, vk_ns_present;
static u64 vk_stat_pipelines, vk_stat_shaders, vk_stat_shader_failures, vk_stat_textures, vk_stat_upload_bytes, vk_stat_reused, vk_stat_passes;

static VkCommandBuffer cmd(void);
/* Something the GPU may still use, destroyed once the frame recording now is done. The frame is begun first: a
 * slot not yet begun frees its graves as it begins, after waiting for its own last work only, while the frames
 * after it may still use what was buried (the validation layer: buffers destroyed in use by command buffers). */
static void bury_grave(Grave g) {
    if (!frames[frame_at].recording) cmd();
    Frame *f = &frames[frame_at];
    if (f->ngraves == f->graves_cap) {
        u32 cap = f->graves_cap * 2 + 32;
        Grave *n = realloc(f->graves, sizeof *n * cap);
        if (!n) return;   /* (leaked rather than freed while the GPU may use it) */
        f->graves = n;
        f->graves_cap = cap;
    }
    f->graves[f->ngraves++] = g;
}
static void bury_view(VkImageView v) { if (v) bury_grave((Grave){VK_NULL_HANDLE, VK_NULL_HANDLE, v, {0}}); }

static void check(VkResult r, const char *doing) {
    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_OUT_OF_DATE_KHR) return;
    if (r == VK_ERROR_DEVICE_LOST)
        rt_fatal("the graphics device was lost while %s (Vulkan, VK_ERROR_DEVICE_LOST). Nothing can be drawn after that: start the game again.", doing);
    fprintf(stderr, "[vulkan] %s failed (%d)\n", doing, (int)r);
}

/* The frame's command buffer, begun the first time it is asked for (after the GPU is done with the
 * slot's previous work, whose graves, upload memory, views and descriptor sets are then taken back). */
static VkCommandBuffer cmd(void) {
    Frame *f = &frames[frame_at];
    if (f->recording) return f->cmd;
    if (f->submitted) {
        u64 t0 = rt_clock_ns();
        check(vkWaitForFences(device, 1, &f->done, VK_TRUE, UINT64_MAX), "waiting for a frame");
        vk_ns_wait += rt_clock_ns() - t0;
        f->submitted = 0;
    }
    for (u32 i = 0; i < f->nviews; i++) vkDestroyBufferView(device, f->views[i], NULL);
    f->nviews = 0;
    for (u32 i = 0; i < f->ngraves; i++) {
        Grave *g = &f->graves[i];
        if (g->view) vkDestroyImageView(device, g->view, NULL);
        if (g->bview) vkDestroyBufferView(device, g->bview, NULL);
        if (g->image) vkDestroyImage(device, g->image, NULL);
        if (g->buffer) vkDestroyBuffer(device, g->buffer, NULL);
        dev_free(&g->alloc);
    }
    f->ngraves = 0;
    for (u32 i = 0; i < f->nblocks; i++) f->blocks[i].used = 0;
    for (u32 i = 0; i < f->npools; i++) vkResetDescriptorPool(device, f->pools[i], 0);
    f->pool_at = 0;
    f->serial = ++frame_serial;
    vkResetCommandBuffer(f->cmd, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    check(vkBeginCommandBuffer(f->cmd, &bi), "starting a frame's commands");
    f->recording = 1;
    return f->cmd;
}

/* `size` bytes of the frame's upload memory, aligned: where the CPU writes them, and where the GPU
 * finds them (buffer, offset). NULL if there is no more. */
static VkDeviceSize texel_align = 256, ubo_align = 256;
static u8 *arena(VkDeviceSize size, VkDeviceSize align, VkBuffer *buf, VkDeviceSize *off) {
    cmd();
    Frame *f = &frames[frame_at];
    if (!align) align = 16;
    for (u32 i = 0; i < f->nblocks; i++) {
        Block *b = &f->blocks[i];
        VkDeviceSize o = (b->used + align - 1) / align * align;
        if (o + size <= b->size) { b->used = o + size; *buf = b->buf; *off = o; return b->map + o; }
    }
    if (f->nblocks == MAX_BLOCKS) return NULL;
    Block *b = &f->blocks[f->nblocks];
    VkDeviceSize bs = BLOCK_SIZE;
    while (bs < size) bs *= 2;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, bs,
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                  VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    if (vkCreateBuffer(device, &bci, NULL, &b->buf) != VK_SUCCESS) return NULL;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(device, b->buf, &mr);
    /* (the card's own memory the CPU can write, where it has some: the draws read it there rather than
     * across the bus; else the system's. SSBU_VK_SYSMEM=1: the system's) */
    static int sysmem = -1;
    if (sysmem < 0) sysmem = getenv("SSBU_VK_SYSMEM") && atoi(getenv("SSBU_VK_SYSMEM")) != 0;
    VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    int local = sysmem ? -1 : memory_type(mr.memoryTypeBits, host | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    int type = memory_type(mr.memoryTypeBits, host);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, (uint32_t)local};
    if (local < 0 || vkAllocateMemory(device, &mai, NULL, &b->mem) != VK_SUCCESS) {
        mai.memoryTypeIndex = (uint32_t)type;
        if (type < 0 || vkAllocateMemory(device, &mai, NULL, &b->mem) != VK_SUCCESS) { vkDestroyBuffer(device, b->buf, NULL); return NULL; }
    } else vk_stat_local_blocks++;
    vkBindBufferMemory(device, b->buf, b->mem, 0);
    if (vkMapMemory(device, b->mem, 0, VK_WHOLE_SIZE, 0, (void **)&b->map) != VK_SUCCESS) {
        vkDestroyBuffer(device, b->buf, NULL);
        vkFreeMemory(device, b->mem, NULL);
        return NULL;
    }
    b->size = bs;
    b->used = size;
    f->nblocks++;
    *buf = b->buf;
    *off = 0;
    return b->map;
}

/* A view of upload memory as the shaders' raw buffers (ByteAddressBuffer: a uniform texel buffer of
 * 32-bit words in SPIR-V), destroyed with the frame. */
static VkBufferView texel_view(VkBuffer buf, VkDeviceSize off, VkDeviceSize size) {
    Frame *f = &frames[frame_at];
    if (f->nviews == f->views_cap) {
        u32 cap = f->views_cap * 2 + 1024;
        VkBufferView *n = realloc(f->views, sizeof *n * cap);
        if (!n) return VK_NULL_HANDLE;
        f->views = n;
        f->views_cap = cap;
    }
    VkBufferViewCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, NULL, 0, buf, VK_FORMAT_R32_UINT, off, size};
    VkBufferView v;
    if (vkCreateBufferView(device, &ci, NULL, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
    f->views[f->nviews++] = v;
    return v;
}

/* A view of upload memory, kept: the frames' upload blocks live as long as the device, and the same
 * (block, offset, size) comes back frame after frame (the arena hands out the same places in the same
 * order), so a view made once is used again instead of made per draw. A view pushed out of its slot is
 * destroyed with this frame (frames before it that used it are done by then: the queue runs in order). */
#define ARENA_VIEWS (1u << 16)
static struct { VkBuffer buf; VkDeviceSize off, size; VkBufferView view; } arena_views[ARENA_VIEWS];
static u64 vk_stat_views_made, vk_stat_views_kept;
static VkBufferView arena_view(VkBuffer buf, VkDeviceSize off, VkDeviceSize size) {
    u64 h = ((u64)(uintptr_t)buf * 0x9E3779B97F4A7C15ull) ^ (off * 0xC2B2AE3D27D4EB4Full) ^ (size * 0x165667B19E3779F9ull);
    u32 k = (u32)(h >> 48) & (ARENA_VIEWS - 1);
    if (arena_views[k].view && arena_views[k].buf == buf && arena_views[k].off == off && arena_views[k].size == size) {
        vk_stat_views_kept++;
        return arena_views[k].view;
    }
    VkBufferViewCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, NULL, 0, buf, VK_FORMAT_R32_UINT, off, size};
    VkBufferView v;
    if (vkCreateBufferView(device, &ci, NULL, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
    vk_stat_views_made++;
    if (arena_views[k].view) {   /* (the old one: destroyed with this frame, as texel_view's are) */
        Frame *f = &frames[frame_at];
        if (f->nviews == f->views_cap) {
            u32 cap = f->views_cap * 2 + 1024;
            VkBufferView *n = realloc(f->views, sizeof *n * cap);
            if (n) { f->views = n; f->views_cap = cap; }
        }
        if (f->nviews < f->views_cap) f->views[f->nviews++] = arena_views[k].view;   /* (else leaked, never freed in use) */
    }
    arena_views[k].buf = buf; arena_views[k].off = off; arena_views[k].size = size; arena_views[k].view = v;
    return v;
}

/* A descriptor set of the frame's pools (more pools as it needs them). */
static VkDescriptorSet new_set(VkDescriptorSetLayout layout) {
    cmd();
    Frame *f = &frames[frame_at];
    for (;;) {
        if (f->pool_at < f->npools) {
            VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, f->pools[f->pool_at], 1, &layout};
            VkDescriptorSet s;
            if (vkAllocateDescriptorSets(device, &ai, &s) == VK_SUCCESS) return s;
            f->pool_at++;
            continue;
        }
        if (f->npools == MAX_POOLS) return VK_NULL_HANDLE;
        enum { SETS = 2048 };
        VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, SETS}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, SETS}, {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, SETS * 24},
                                        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, SETS * 8}, {VK_DESCRIPTOR_TYPE_SAMPLER, SETS * 8},
                                        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, SETS * 8}};
        VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, SETS, sizeof sizes / sizeof *sizes, sizes};
        if (vkCreateDescriptorPool(device, &pci, NULL, &f->pools[f->npools]) != VK_SUCCESS) return VK_NULL_HANDLE;
        f->npools++;
    }
}

/* Everything before is done and seen by everything after. */
static void sync_all(VkCommandBuffer c) {
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                          VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

static void image_layout(VkCommandBuffer c, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                              VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, from, to, VK_QUEUE_FAMILY_IGNORED,
                              VK_QUEUE_FAMILY_IGNORED, image, {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

/* Draws go into a rendering (dynamic rendering) kept open while the draws after it use the same
 * attachments; anything else recorded (clears, copies, blits, uploads) ends it first: outside(). */
static struct { int active; u32 ncolor; VkImageView color[8], depth; VkImage images[9]; } rp;
static void end_rendering(void) {
    if (!rp.active) return;
    VkCommandBuffer c = frames[frame_at].cmd;
    vkCmdEndRenderingKHR(c);
    sync_all(c);
    rp.active = 0;
}
static VkCommandBuffer outside(void) {
    end_rendering();
    return cmd();
}

/* ---- formats ---------------------------------------------------------------------------------- */
enum { CLASS_FLOAT, CLASS_SINT, CLASS_UINT };
typedef struct ColorFormat { VkFormat view, storage; int klass, srgb; } ColorFormat;

/* Maxwell render target formats (colour), as gpu_d3d11.c's color_format: 8-bit colour is stored
 * plain (mutable: viewed as sRGB or plain by each use, the bytes kept across). */
static ColorFormat color_format(u32 f) {
    ColorFormat c = {VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, CLASS_FLOAT, 0};
    switch (f) {
    case 0xC0: case 0xC3: c.view = VK_FORMAT_R32G32B32A32_SFLOAT; break;
    case 0xC1: case 0xC4: c.view = VK_FORMAT_R32G32B32A32_SINT; c.klass = CLASS_SINT; break;
    case 0xC2: case 0xC5: c.view = VK_FORMAT_R32G32B32A32_UINT; c.klass = CLASS_UINT; break;
    case 0xC6: c.view = VK_FORMAT_R16G16B16A16_UNORM; break;
    case 0xC7: c.view = VK_FORMAT_R16G16B16A16_SNORM; break;
    case 0xC8: c.view = VK_FORMAT_R16G16B16A16_SINT; c.klass = CLASS_SINT; break;
    case 0xC9: c.view = VK_FORMAT_R16G16B16A16_UINT; c.klass = CLASS_UINT; break;
    case 0xCA: case 0xCE: c.view = VK_FORMAT_R16G16B16A16_SFLOAT; break;
    case 0xCB: c.view = VK_FORMAT_R32G32_SFLOAT; break;
    case 0xCC: c.view = VK_FORMAT_R32G32_SINT; c.klass = CLASS_SINT; break;
    case 0xCD: c.view = VK_FORMAT_R32G32_UINT; c.klass = CLASS_UINT; break;
    case 0xCF: case 0xE6: c.view = c.storage = VK_FORMAT_B8G8R8A8_UNORM; break;
    case 0xD0: case 0xE7: c.view = VK_FORMAT_B8G8R8A8_SRGB; c.storage = VK_FORMAT_B8G8R8A8_UNORM; c.srgb = 1; break;
    case 0xD1: c.view = VK_FORMAT_A2B10G10R10_UNORM_PACK32; break;
    case 0xD2: c.view = VK_FORMAT_A2B10G10R10_UINT_PACK32; c.klass = CLASS_UINT; break;
    case 0xD5: case 0xF9: c.view = c.storage = VK_FORMAT_R8G8B8A8_UNORM; break;
    case 0xD6: case 0xFA: c.view = VK_FORMAT_R8G8B8A8_SRGB; c.storage = VK_FORMAT_R8G8B8A8_UNORM; c.srgb = 1; break;
    case 0xD7: c.view = VK_FORMAT_R8G8B8A8_SNORM; break;
    case 0xD8: c.view = VK_FORMAT_R8G8B8A8_SINT; c.klass = CLASS_SINT; break;
    case 0xD9: c.view = VK_FORMAT_R8G8B8A8_UINT; c.klass = CLASS_UINT; break;
    case 0xDA: c.view = VK_FORMAT_R16G16_UNORM; break;
    case 0xDB: c.view = VK_FORMAT_R16G16_SNORM; break;
    case 0xDC: c.view = VK_FORMAT_R16G16_SINT; c.klass = CLASS_SINT; break;
    case 0xDD: c.view = VK_FORMAT_R16G16_UINT; c.klass = CLASS_UINT; break;
    case 0xDE: c.view = VK_FORMAT_R16G16_SFLOAT; break;
    case 0xE0: c.view = VK_FORMAT_B10G11R11_UFLOAT_PACK32; break;
    case 0xE3: c.view = VK_FORMAT_R32_SINT; c.klass = CLASS_SINT; break;
    case 0xE4: c.view = VK_FORMAT_R32_UINT; c.klass = CLASS_UINT; break;
    case 0xE5: c.view = VK_FORMAT_R32_SFLOAT; break;
    case 0xE8: c.view = VK_FORMAT_R5G6B5_UNORM_PACK16; break;
    case 0xE9: case 0xF8: c.view = VK_FORMAT_A1R5G5B5_UNORM_PACK16; break;
    case 0xEA: c.view = VK_FORMAT_R8G8_UNORM; break;
    case 0xEB: c.view = VK_FORMAT_R8G8_SNORM; break;
    case 0xEC: c.view = VK_FORMAT_R8G8_SINT; c.klass = CLASS_SINT; break;
    case 0xED: c.view = VK_FORMAT_R8G8_UINT; c.klass = CLASS_UINT; break;
    case 0xEE: c.view = VK_FORMAT_R16_UNORM; break;
    case 0xEF: c.view = VK_FORMAT_R16_SNORM; break;
    case 0xF0: c.view = VK_FORMAT_R16_SINT; c.klass = CLASS_SINT; break;
    case 0xF1: c.view = VK_FORMAT_R16_UINT; c.klass = CLASS_UINT; break;
    case 0xF2: c.view = VK_FORMAT_R16_SFLOAT; break;
    case 0xF3: c.view = VK_FORMAT_R8_UNORM; break;
    case 0xF4: c.view = VK_FORMAT_R8_SNORM; break;
    case 0xF5: c.view = VK_FORMAT_R8_SINT; c.klass = CLASS_SINT; break;
    case 0xF6: c.view = VK_FORMAT_R8_UINT; c.klass = CLASS_UINT; break;
    }
    if (c.storage == VK_FORMAT_UNDEFINED) c.storage = c.view;
    return c;
}

static VkFormatFeatureFlags features_of(VkFormat f) {
    static VkFormat known[64];
    static VkFormatFeatureFlags have[64];
    static u32 n;
    for (u32 i = 0; i < n; i++) if (known[i] == f) return have[i];
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(physical, f, &p);
    if (n < 64) { known[n] = f; have[n] = p.optimalTilingFeatures; n++; }
    return p.optimalTilingFeatures;
}

/* Maxwell depth formats; Z24S8 as D32S8 where the card has no D24S8 (AMD). */
static VkFormat depth_format(u32 f) {
    switch (f) {
    case 0x0A: return VK_FORMAT_D32_SFLOAT;
    case 0x13: return VK_FORMAT_D16_UNORM;
    case 0x14: case 0x15: case 0x16:
        return features_of(VK_FORMAT_D24_UNORM_S8_UINT) & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT ? VK_FORMAT_D24_UNORM_S8_UINT
                                                                                                        : VK_FORMAT_D32_SFLOAT_S8_UINT;
    case 0x19: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    }
    return VK_FORMAT_UNDEFINED;
}
static int has_stencil(VkFormat f) { return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT; }

static void warn_format(const char *what, u32 f) {
    static u32 seen[64], n;
    for (u32 i = 0; i < n; i++) if (seen[i] == (f | (u32)(what[0] << 24))) return;
    if (n < 64) seen[n++] = f | (u32)(what[0] << 24);
    fprintf(stderr, "[vulkan] unsupported %s format %#x\n", what, f);
}

/* ---- surfaces: render targets on the host ---------------------------------------------------- */
static float render_scale = 1.0f;

typedef struct Surface {
    u64 guest;
    u32 width, height, format, depth;   /* the game's size and format: what it is known by */
    u32 hw, hh;                         /* the host image's size (scaled) */
    VkFormat storage, view;
    int klass, srgb;
    VkImage image;
    Alloc alloc;
    u64 used;
    VkImageView att, smp[4];            /* (made when first asked for) the attachment's view (of format att_format); sampled:
                                         * plain, sRGB, and those as 2D arrays */
    VkFormat att_format;
} Surface;
static Surface surfaces[2048];
static u32 nsurfaces;
static u64 tick;
static u64 tex_gen;   /* (counts textures and surfaces made and let go, and memory taken away: tex_memo's entries hold while it is the same) */

static void release_surface(Surface *s) {
    tex_gen++;
    bury_view(s->att);
    for (int i = 0; i < 4; i++) bury_view(s->smp[i]);
    bury_grave((Grave){s->image, VK_NULL_HANDLE, VK_NULL_HANDLE, s->alloc});
    memset(s, 0, sizeof *s);
}

static void scaled_size(u32 w, u32 h, u32 *hw, u32 *hh) {
    *hw = w; *hh = h;
    if (render_scale == 1.0f) return;
    float x = (float)w * render_scale + 0.5f, y = (float)h * render_scale + 0.5f;
    if (x < 1) x = 1;
    if (y < 1) y = 1;
    if (x > 16384 || y > 16384) return;
    *hw = (u32)x; *hh = (u32)y;
}

static VkImageAspectFlags aspect_of(const Surface *s) {
    if (!s->depth) return VK_IMAGE_ASPECT_COLOR_BIT;
    return VK_IMAGE_ASPECT_DEPTH_BIT | (has_stencil(s->storage) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
}

/* The host image standing for the render target at `guest`, made on first use. */
static Surface *surface(u64 guest, u32 width, u32 height, u32 format, int depth) {
    if (!guest || !width || !height || width > 16384 || height > 16384) return NULL;
    Surface *free_slot = NULL;
    ColorFormat cf = {VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, CLASS_FLOAT, 0};
    if (depth) cf.view = cf.storage = depth_format(format);
    else cf = color_format(format);
    for (u32 i = 0; i < nsurfaces; i++) {
        Surface *s = &surfaces[i];
        if (!s->image) { if (!free_slot) free_slot = s; continue; }
        if (s->guest == guest && s->depth == depth) {
            if (s->width == width && s->height == height && (s->format == format || (cf.view && cf.storage == s->storage))) {
                s->format = format; s->view = cf.view ? cf.view : s->view; s->srgb = cf.srgb;   /* (the same bytes, now sRGB or plain) */
                s->used = ++tick;
                return s;
            }
            release_surface(s);   /* the memory now holds a different surface */
            if (!free_slot) free_slot = s;
        }
    }
    if (cf.view == VK_FORMAT_UNDEFINED) { warn_format(depth ? "depth" : "color", format); return NULL; }
    Surface *s = free_slot;
    if (!s) {
        if (nsurfaces < sizeof surfaces / sizeof *surfaces) s = &surfaces[nsurfaces++];
        else {   /* the least recently used goes */
            s = &surfaces[0];
            for (u32 i = 1; i < nsurfaces; i++) if (surfaces[i].used < s->used) s = &surfaces[i];
            release_surface(s);
        }
    }
    u32 hw, hh;
    scaled_size(width, height, &hw, &hh);
    VkFormatFeatureFlags have = features_of(cf.storage);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (depth && (have & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (!depth && (have & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, cf.storage != cf.view || cf.srgb || cf.storage == VK_FORMAT_R8G8B8A8_UNORM ||
                             cf.storage == VK_FORMAT_B8G8R8A8_UNORM ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0,
                             VK_IMAGE_TYPE_2D, cf.storage, {hw, hh, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, usage,
                             VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    Alloc mem;
    VkImage image = make_image(&ici, &mem);
    if (!image) return NULL;
    memset(s, 0, sizeof *s);
    s->guest = guest; s->width = width; s->height = height; s->format = format; s->depth = depth;
    s->hw = hw; s->hh = hh;
    s->storage = cf.storage; s->view = cf.view; s->klass = cf.klass; s->srgb = cf.srgb;
    s->image = image; s->alloc = mem;
    s->used = ++tick;
    tex_gen++;
    VkCommandBuffer c = outside();
    image_layout(c, image, aspect_of(s), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    /* (a new render target reads as zero, as Direct3D 11's do) */
    VkImageSubresourceRange range = {aspect_of(s), 0, 1, 0, 1};
    if (depth) { VkClearDepthStencilValue z = {0, 0}; vkCmdClearDepthStencilImage(c, image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &range); }
    else { VkClearColorValue zero; memset(&zero, 0, sizeof zero); vkCmdClearColorImage(c, image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range); }
    sync_all(c);
    return s;
}

static Surface *find_color(u64 guest) {
    Surface *found = NULL;
    for (u32 i = 0; i < nsurfaces; i++)
        if (surfaces[i].image && !surfaces[i].depth && surfaces[i].guest == guest) found = &surfaces[i];
    return found;
}

/* ---- the lock --------------------------------------------------------------------------------- */
static void enter(void) {
    EnterCriticalSection(&lock);
    last_enter_thread = GetCurrentThreadId();
}
static void leave(void) { LeaveCriticalSection(&lock); }

static float f32_of(u32 bits) { float f; memcpy(&f, &bits, 4); return f; }
static float to_srgb(float x) {
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    return x <= 0.0031308f ? x * 12.92f : 1.055f * powf(x, 1.0f / 2.4f) - 0.055f;
}

#define RT(i, field) regs[0x200 + (i) * 0x10 + (field)]

/* ---- the 3D engine's clears ------------------------------------------------------------------- */
static int clear_channels(Surface *s, const float color[4], u32 mask);   /* (below: it draws) */
static void vk_clear(const u32 *regs, u32 arg) {
    enter();
    gfx_stat_clears++;
    u32 rt = (arg >> 6) & 0xF;
    if (arg & 0x3C && rt < 8) {   /* any of R, G, B, A */
        u64 va = (u64)RT(rt, 0) << 32 | RT(rt, 1);
        Surface *s = surface(gpu_va_to_guest(va), RT(rt, 2), RT(rt, 3), RT(rt, 4), 0);
        static int drawn_clears = -1;   /* SSBU_VK_DRAWN_CLEARS=1 (testing): every colour clear drawn as the masked ones are */
        if (drawn_clears < 0) drawn_clears = getenv("SSBU_VK_DRAWN_CLEARS") && atoi(getenv("SSBU_VK_DRAWN_CLEARS")) != 0;
        if (s && ((arg >> 2) & 0xF) == 0xF && !(drawn_clears && s->klass == CLASS_FLOAT)) {
            float c[4] = {f32_of(regs[0x360]), f32_of(regs[0x361]), f32_of(regs[0x362]), f32_of(regs[0x363])};
            VkClearColorValue v;
            if (s->klass == CLASS_SINT) for (int i = 0; i < 4; i++) v.int32[i] = (int32_t)c[i];
            else if (s->klass == CLASS_UINT) for (int i = 0; i < 4; i++) v.uint32[i] = c[i] <= 0 ? 0 : (uint32_t)c[i];
            else for (int i = 0; i < 4; i++) v.float32[i] = s->srgb && i < 3 ? to_srgb(c[i]) : c[i];   /* (stored plain: written as an sRGB view would) */
            VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkCommandBuffer cb = outside();
            vkCmdClearColorImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
            sync_all(cb);
        } else if (s) {   /* only some channels: a masked draw */
            float c[4] = {f32_of(regs[0x360]), f32_of(regs[0x361]), f32_of(regs[0x362]), f32_of(regs[0x363])};
            if (!clear_channels(s, c, (arg >> 2) & 0xF)) vk_stat_clears_skipped++;
        }
    }
    if (arg & 3) {   /* depth, stencil */
        u64 va = (u64)regs[0x3F8] << 32 | regs[0x3F9];
        Surface *s = surface(gpu_va_to_guest(va), regs[0x48A], regs[0x48B], regs[0x3FA], 1);
        if (s) {
            VkImageAspectFlags a = (arg & 1 ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (arg & 2 && has_stencil(s->storage) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
            if (a) {
                VkClearDepthStencilValue z = {f32_of(regs[0x364]), regs[0x368] & 0xFF};
                VkImageSubresourceRange range = {a, 0, 1, 0, 1};
                VkCommandBuffer cb = outside();
                vkCmdClearDepthStencilImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &range);
                sync_all(cb);
            }
        }
    }
    leave();
}

/* ---- the 2D engine's blit: source rectangle scaled into the destination's -------------------- */
static void vk_blit2d(const u32 *regs) {
    enter();
    gfx_stat_blits++;
    u64 src_g = gpu_va_to_guest((u64)regs[0x94] << 32 | regs[0x95]), dst_g = gpu_va_to_guest((u64)regs[0x88] << 32 | regs[0x89]);
    Surface *src = find_color(src_g);
    Surface *dst = src ? surface(dst_g, regs[0x86], regs[0x87], regs[0x80], 0) : NULL;
    if (!src || !dst || src == dst) { leave(); return; }   /* (sources only in guest memory: not yet, as in gpu_d3d11.c) */
    double du = regs[0x231] + regs[0x230] / 4294967296.0, dv = regs[0x233] + regs[0x232] / 4294967296.0;
    double sx = regs[0x235] + regs[0x234] / 4294967296.0, sy = (i32)regs[0x237] + regs[0x236] / 4294967296.0;
    u32 dx = regs[0x22C], dy = regs[0x22D], dw = regs[0x22E], dh = regs[0x22F];
    int linear = (regs[0x223] >> 4) & 1;
    VkFormatFeatureFlags fs = features_of(src->storage), fd = features_of(dst->storage);
    if (!(fs & VK_FORMAT_FEATURE_BLIT_SRC_BIT) || !(fd & VK_FORMAT_FEATURE_BLIT_DST_BIT) || (src->klass != CLASS_FLOAT) != (dst->klass != CLASS_FLOAT)) {
        vk_stat_blits_skipped++;
        leave();
        return;
    }
    if (linear && !(fs & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) linear = 0;
    /* (in the game's pixels; each image's own are its scale times those) */
    float ssx = (float)src->hw / (float)src->width, ssy = (float)src->hh / (float)src->height;
    float dsx = (float)dst->hw / (float)dst->width, dsy = (float)dst->hh / (float)dst->height;
    VkImageBlit b;
    memset(&b, 0, sizeof b);
    b.srcSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.dstSubresource = b.srcSubresource;
    b.srcOffsets[0] = (VkOffset3D){(int32_t)lround(sx * ssx), (int32_t)lround(sy * ssy), 0};
    b.srcOffsets[1] = (VkOffset3D){(int32_t)lround((sx + dw * du) * ssx), (int32_t)lround((sy + dh * dv) * ssy), 1};
    b.dstOffsets[0] = (VkOffset3D){(int32_t)lround(dx * dsx), (int32_t)lround(dy * dsy), 0};
    b.dstOffsets[1] = (VkOffset3D){(int32_t)lround((dx + dw) * dsx), (int32_t)lround((dy + dh) * dsy), 1};
    for (int k = 0; k < 2; k++) {   /* (inside both images) */
        if (b.srcOffsets[k].x < 0) b.srcOffsets[k].x = 0;
        if (b.srcOffsets[k].y < 0) b.srcOffsets[k].y = 0;
        if (b.srcOffsets[k].x > (int32_t)src->hw) b.srcOffsets[k].x = (int32_t)src->hw;
        if (b.srcOffsets[k].y > (int32_t)src->hh) b.srcOffsets[k].y = (int32_t)src->hh;
        if (b.dstOffsets[k].x > (int32_t)dst->hw) b.dstOffsets[k].x = (int32_t)dst->hw;
        if (b.dstOffsets[k].y > (int32_t)dst->hh) b.dstOffsets[k].y = (int32_t)dst->hh;
    }
    if (b.srcOffsets[0].x == b.srcOffsets[1].x || b.srcOffsets[0].y == b.srcOffsets[1].y || b.dstOffsets[0].x == b.dstOffsets[1].x ||
        b.dstOffsets[0].y == b.dstOffsets[1].y) { leave(); return; }
    VkCommandBuffer c = outside();
    vkCmdBlitImage(c, src->image, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &b, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    sync_all(c);
    leave();
}

/* ---- the swap chain ------------------------------------------------------------------------------ */
static VkSwapchainKHR swapchain;
static VkImage swap_images[8];
static VkSemaphore swap_ready[8];   /* (per image: its frame's work is done, it may be shown) */
static u32 nswap, swap_w, swap_h;
static VkPresentModeKHR present_mode;
static int have_immediate, have_mailbox, recreate = 1, shown;
static VkFormat swap_format = VK_FORMAT_B8G8R8A8_UNORM;

static VkPresentModeKHR wanted_mode(void) {
    if ((display_present_flags() & 0x200u) && have_immediate) return VK_PRESENT_MODE_IMMEDIATE_KHR;   /* vrr, SSBU_SPEED: at once */
    if (have_mailbox) return VK_PRESENT_MODE_MAILBOX_KHR;   /* clock / display pacing: the newest frame at the next refresh */
    return have_immediate ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
}

static const char *mode_name(VkPresentModeKHR m) {
    return m == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" : m == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "fifo";
}

static void destroy_swapchain(void) {
    if (!swapchain) return;
    vkDeviceWaitIdle(device);
    for (u32 i = 0; i < nswap; i++) vkDestroySemaphore(device, swap_ready[i], NULL);
    vkDestroySwapchainKHR(device, swapchain, NULL);
    swapchain = VK_NULL_HANDLE;
    nswap = 0;
}

/* (Re)made for the window's size now; 0 when the window has no size (minimized). */
static int make_swapchain(void) {
    destroy_swapchain();
    recreate = 0;
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, vk_surface, &caps) != VK_SUCCESS) return 0;
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) {
        RECT r;
        GetClientRect(window, &r);
        ext.width = (uint32_t)(r.right - r.left);
        ext.height = (uint32_t)(r.bottom - r.top);
    }
    if (!ext.width || !ext.height) return 0;
    uint32_t count = caps.minImageCount + 1;
    if (count < 3) count = 3;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    present_mode = wanted_mode();
    VkSwapchainCreateInfoKHR sci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, NULL, 0, vk_surface, count, swap_format,
                                    VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, ext, 1, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                    VK_SHARING_MODE_EXCLUSIVE, 0, NULL, caps.currentTransform, VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                    present_mode, VK_TRUE, VK_NULL_HANDLE};
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkResult r = vkCreateSwapchainKHR(device, &sci, NULL, &swapchain);
    if (r != VK_SUCCESS) {
        fprintf(stderr, "[vulkan] cannot make the swap chain (%ux%u, %s): %d\n", ext.width, ext.height, mode_name(present_mode), (int)r);
        swapchain = VK_NULL_HANDLE;
        return 0;
    }
    nswap = 8;
    vkGetSwapchainImagesKHR(device, swapchain, &nswap, swap_images);
    VkSemaphoreCreateInfo sem = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, NULL, 0};
    for (u32 i = 0; i < nswap; i++) vkCreateSemaphore(device, &sem, NULL, &swap_ready[i]);
    swap_w = ext.width;
    swap_h = ext.height;
    static u32 told_w, told_h;
    static VkPresentModeKHR told_mode = (VkPresentModeKHR)-1;
    if (told_w != swap_w || told_h != swap_h || told_mode != present_mode) {
        fprintf(stderr, "[vulkan] swap chain %ux%u, %u images, %s\n", swap_w, swap_h, nswap, mode_name(present_mode));
        told_w = swap_w; told_h = swap_h; told_mode = present_mode;
    }
    return 1;
}

/* ---- pictures to files: SSBU_CAPTURE, F3 --------------------------------------------------------- */
typedef struct Readback { VkBuffer buffer; VkDeviceMemory memory; u32 w, h; VkFormat format; char path[1024]; } Readback;

/* A copy of an image (8-bit RGBA or BGRA, in the general layout) into a buffer the CPU can read, recorded now. */
static int readback_image(VkImage image, VkFormat format, u32 w, u32 h, Readback *rb, const char *path) {
    if (format != VK_FORMAT_R8G8B8A8_UNORM && format != VK_FORMAT_B8G8R8A8_UNORM) return 0;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    if (vkCreateBuffer(device, &bci, NULL, &rb->buffer) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(device, rb->buffer, &mr);
    int type = memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, (uint32_t)type};
    if (type < 0 || vkAllocateMemory(device, &mai, NULL, &rb->memory) != VK_SUCCESS) { vkDestroyBuffer(device, rb->buffer, NULL); return 0; }
    vkBindBufferMemory(device, rb->buffer, rb->memory, 0);
    VkBufferImageCopy region = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    VkCommandBuffer c = outside();
    vkCmdCopyImageToBuffer(c, image, VK_IMAGE_LAYOUT_GENERAL, rb->buffer, 1, &region);
    sync_all(c);
    rb->w = w; rb->h = h; rb->format = format;
    snprintf(rb->path, sizeof rb->path, "%s", path);
    return 1;
}
static int readback(Surface *s, Readback *rb, const char *path) { return readback_image(s->image, s->storage, s->hw, s->hh, rb, path); }

/* (once the frame's work is done) the buffer as a 24-bit BMP, then let go. */
static void readback_save(Readback *rb) {
    const u8 *px = NULL;
    if (vkMapMemory(device, rb->memory, 0, VK_WHOLE_SIZE, 0, (void **)&px) == VK_SUCCESS) {
        FILE *f = fopen(rb->path, "wb");
        u32 row = (rb->w * 3 + 3) & ~3u, size = 54 + row * rb->h;
        if (f) {
            u8 h[54] = {'B', 'M'};
            memcpy(h + 2, &size, 4);
            h[10] = 54; h[14] = 40;
            memcpy(h + 18, &rb->w, 4);
            memcpy(h + 22, &rb->h, 4);
            h[26] = 1; h[28] = 24;
            fwrite(h, 1, 54, f);
            u8 *line = calloc(1, row);
            int bgra = rb->format == VK_FORMAT_B8G8R8A8_UNORM;
            for (u32 y = rb->h; line && y-- > 0;) {   /* (bottom up) */
                const u8 *p = px + (size_t)y * rb->w * 4;
                for (u32 x = 0; x < rb->w; x++) {
                    line[x * 3 + 0] = p[x * 4 + (bgra ? 0 : 2)];
                    line[x * 3 + 1] = p[x * 4 + 1];
                    line[x * 3 + 2] = p[x * 4 + (bgra ? 2 : 0)];
                }
                fwrite(line, 1, row, f);
            }
            free(line);
            fclose(f);
            fprintf(stderr, "[vulkan] picture %ux%u saved: %s\n", rb->w, rb->h, rb->path);
        }
        vkUnmapMemory(device, rb->memory);
    }
    vkDestroyBuffer(device, rb->buffer, NULL);
    vkFreeMemory(device, rb->memory, NULL);
}

/* SSBU_CAPTURE=<dir> (with SSBU_CAPTURE_MAX, SSBU_CAPTURE_EVERY, and SSBU_CAPTURE_FROM / SSBU_CAPTURE_TO, a span of
 * a test fight's frames), as gpu_d3d11.c: the path of this frame's picture, or NULL. The same fight frame captured
 * under both back ends compares their pictures. */
extern volatile long long test_fight_frame;
static const char *capture_path(char *path, size_t size) {
    static const char *dir;
    static int checked, count, most = 40;
    static u64 last, every = 3000000000ull;
    static long long from, to;
    if (!checked++) {
        dir = getenv("SSBU_CAPTURE");
        if (getenv("SSBU_CAPTURE_MAX")) most = atoi(getenv("SSBU_CAPTURE_MAX"));
        if (getenv("SSBU_CAPTURE_EVERY")) every = (u64)(atof(getenv("SSBU_CAPTURE_EVERY")) * 1e9);
        if (getenv("SSBU_CAPTURE_FROM")) from = atoll(getenv("SSBU_CAPTURE_FROM"));
        if (getenv("SSBU_CAPTURE_TO")) to = atoll(getenv("SSBU_CAPTURE_TO"));
    }
    if (!dir || !*dir || count >= most || rt_now_ns() - last < every) return NULL;
    if ((from && test_fight_frame < from) || (to && test_fight_frame > to)) return NULL;
    last = rt_now_ns();
    snprintf(path, size, "%s/vk_frame_%03d.bmp", dir, count);
    if (test_fight_frame) fprintf(stderr, "[capture] vk_frame_%03d.bmp at fight frame %lld\n", count, test_fight_frame);
    count++;
    return path;
}

/* ---- frames shown ------------------------------------------------------------------------------- */
static void submit(VkSemaphore wait, VkSemaphore signal) {
    Frame *f = &frames[frame_at];
    end_rendering();
    if (!f->recording) cmd();
    check(vkEndCommandBuffer(f->cmd), "ending a frame's commands");
    f->recording = 0;
    vkResetFences(device, 1, &f->done);
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, wait ? 1u : 0u, &wait, &stage, 1, &f->cmd, signal ? 1u : 0u, &signal};
    check(vkQueueSubmit(queue, 1, &si, f->done), "submitting a frame");
    f->submitted = 1;
}

static void pipeline_cache_tick(void);
static void residents_retire(void);
static void vk_present_(u64 guest, u32 width, u32 height);
static void trial_passed(void);
static int compose(VkCommandBuffer c, Surface *src, int show_overlay, const char *detail);
static const char *vk_detail(int details);
static VkImage win_image;
static u32 win_w, win_h;
static int depth_sampling;
static void vk_present(u64 guest, u32 width, u32 height) {
    u64 t0 = rt_clock_ns();
    vk_present_(guest, width, height);
    InterlockedAdd64((volatile LONG64 *)&vk_ns_present, (LONG64)(rt_clock_ns() - t0));
}
void hitch_present(void);   /* debug_profile.c (SSBU_HITCH's frame accounting, as gpu_d3d11.c's present) */
static void vk_present_(u64 guest, u32 width, u32 height) {
    (void)width; (void)height;
    enter();
    hitch_present();
    gfx_stat_presented++;
    if (gfx_stat_presented == 600) trial_passed();   /* (renderer = auto: past its first seconds) */
    unsigned bw, bh;
    if (display_take_size(&bw, &bh)) recreate = 1;   /* the window's new size (F11, a drag) */
    if (swapchain && wanted_mode() != present_mode) recreate = 1;
    if (recreate) {
        /* (the frame's work so far goes first: making a swap chain waits for the GPU) */
        if (frames[frame_at].recording) { submit(VK_NULL_HANDLE, VK_NULL_HANDLE); frame_at = (frame_at + 1) % nframes; }
        make_swapchain();
    }
    Frame *f = &frames[frame_at];
    VkCommandBuffer c = outside();
    uint32_t index = 0;
    VkResult ar = swapchain ? vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, f->acquired, VK_NULL_HANDLE, &index) : VK_ERROR_OUT_OF_DATE_KHR;
    int showing = ar == VK_SUCCESS || ar == VK_SUBOPTIMAL_KHR;
    if (ar == VK_SUBOPTIMAL_KHR || ar == VK_ERROR_OUT_OF_DATE_KHR) recreate = 1;
    Surface *src = find_color(guest);
    Readback shot[4];
    int nshots = 0;
    char path[1024], window_path[1024] = "";
    LONG keys = (LONG)gfx_take_hotkeys();
    /* the overlay (F1, F2 its details) and the settings menu (Esc), as gpu_d3d11.c has them */
    static int overlay = -1, details;
    if (overlay < 0) overlay = getenv("SSBU_OVERLAY") ? atoi(getenv("SSBU_OVERLAY")) != 0 : display_overlay();
    if (keys & 1) overlay = !overlay;
    if (keys & 2) { details = !details; overlay = 1; }
    int menu_shot = menu_frame(&overlay, (keys & 4) != 0) & MENU_SHOT;
    if (menu_shot) keys &= ~4;
    if (keys & 16) {   /* F5: depth buffers sampled as textures, or not */
        if (depth_sampling < 0) depth_sampling = !getenv("SSBU_NO_DEPTH_SAMPLING");
        depth_sampling = !depth_sampling;
        tex_gen++;
        fprintf(stderr, "[gfx] depth sampling %s (F5)\n", depth_sampling ? "on" : "off");
    }
    static double profile_at = -1;   /* SSBU_PROFILE_AT: the second to start one at (debug runs), as gpu_d3d11.c */
    if (profile_at == -1) profile_at = getenv("SSBU_PROFILE_AT") ? atof(getenv("SSBU_PROFILE_AT")) : -2;
    if (profile_at >= 0 && rt_now_ns() > (u64)(profile_at * 1e9)) { profile_at = -2; keys |= 32; }
    if (keys & 32) profile_start(10.0);   /* F6: the sampling profiler */
    if (src && capture_path(path, sizeof path) && readback(src, &shot[nshots], path)) {
        nshots++;
        if (getenv("SSBU_CAPTURE_WINDOW")) {   /* (the window's picture too, overlay and menu included) */
            snprintf(window_path, sizeof window_path, "%s", path);
            char *f = strstr(window_path, "vk_frame_");
            if (f) memcpy(f, "vk_windw_", 9);
        }
    }
    if (src && (keys & 4)) {   /* F3: the picture as the game drew it */
        CreateDirectoryA("screenshots", NULL);
        SYSTEMTIME st;
        GetLocalTime(&st);
        snprintf(path, sizeof path, "screenshots/ssbu_%04u%02u%02u_%02u%02u%02u_%03u.bmp", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                 st.wSecond, st.wMilliseconds);
        if (readback(src, &shot[nshots], path)) nshots++;
    }
    if (showing) {
        VkImage img = swap_images[index];
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        int composed = compose(c, src, overlay, vk_detail(details));
        if (composed && window_path[0] && readback_image(win_image, swap_format, win_w, win_h, &shot[nshots], window_path)) nshots++;
        if (composed && menu_shot) {   /* F3 with the settings menu open: the window as it is, menu and all */
            CreateDirectoryA("screenshots", NULL);
            SYSTEMTIME st;
            GetLocalTime(&st);
            snprintf(path, sizeof path, "screenshots/ssbu_%04u%02u%02u_%02u%02u%02u_%03u_menu.bmp", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                     st.wSecond, st.wMilliseconds);
            if (readback_image(win_image, swap_format, win_w, win_h, &shot[nshots], path)) nshots++;
        }
        c = cmd();
        image_layout(c, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue black = {{0, 0, 0, 1}}, grey = {{0.1f, 0.1f, 0.12f, 1}};
        if (composed) {   /* the window's picture into the swap chain image */
            VkImageCopy ic = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {win_w, win_h, 1}};
            vkCmdCopyImage(c, win_image, VK_IMAGE_LAYOUT_GENERAL, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
        } else vkCmdClearColorImage(c, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, src ? &black : &grey, 1, &range);
        if (!composed && src && (features_of(src->storage) & VK_FORMAT_FEATURE_BLIT_SRC_BIT) && src->klass == CLASS_FLOAT) {
            /* the picture into the window: 16:9, centred, black around it (bilinear) */
            unsigned fx, fy, fw, fh;
            display_fit(swap_w, swap_h, &fx, &fy, &fw, &fh);
            sync_all(c);
            VkImageBlit b;
            memset(&b, 0, sizeof b);
            b.srcSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            b.dstSubresource = b.srcSubresource;
            b.srcOffsets[1] = (VkOffset3D){(int32_t)src->hw, (int32_t)src->hh, 1};
            b.dstOffsets[0] = (VkOffset3D){(int32_t)fx, (int32_t)fy, 0};
            b.dstOffsets[1] = (VkOffset3D){(int32_t)(fx + fw), (int32_t)(fy + fh), 1};
            vkCmdBlitImage(c, src->image, VK_IMAGE_LAYOUT_GENERAL, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
        }
        image_layout(c, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        submit(f->acquired, swap_ready[index]);
        VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 1, &swap_ready[index], 1, &swapchain, &index, NULL};
        display_before_present();   /* (gpu_display.c: the loading animation stops) */
        VkResult pr = vkQueuePresentKHR(queue, &pi);
        if (pr == VK_SUBOPTIMAL_KHR || pr == VK_ERROR_OUT_OF_DATE_KHR) recreate = 1;
        else check(pr, "showing a frame");
        if ((pr == VK_SUCCESS || pr == VK_SUBOPTIMAL_KHR) && !shown++) display_show_window();
    } else {
        vk_stat_presents_skipped++;
        submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
        if (!shown) display_show_window();   /* (a window with no size yet: shown, so it gets one) */
    }
    if (nshots) {
        check(vkWaitForFences(device, 1, &f->done, VK_TRUE, UINT64_MAX), "waiting for a picture");
        for (int i = 0; i < nshots; i++) readback_save(&shot[i]);
    }
    frame_at = (frame_at + 1) % nframes;
    residents_retire();
    pipeline_cache_tick();
    leave();
}

/* ---- draws (the plan's step 5) ---------------------------------------------------------------------
 * The same decisions as gpu_d3d11.c's d3d11_draw: the same shaders (gpu_shader.c's HLSL, compiled by fxc
 * to DXBC and kept in the same disk cache, then gpu_spirv.c's SPIR-V), the same per-draw parameters
 * (DrawParams: vertex pulling, the clip-space fix-up, the render scale), the same constant buffer,
 * vertex stream and storage buffer contents, the same textures and samplers and the same fixed-function
 * state. Where Direct3D 11 needs a trick Vulkan has the thing itself: two-sided stencil with references
 * and masks of its own (the dynamic state has front and back), block-compressed textures of any size.
 * Buffers go into each frame's upload memory, as few times as it can: the same range once a submission
 * (note_wrote's counts say whether the GPU's own commands wrote it since), a constant buffer slot's bytes
 * compared with what it had last, 64 KB constant buffers read in shared 128 KB windows, constant buffers
 * of the write-watched area kept in buffers of their own (gpu_d3d11.c's kept ones). Shaders and
 * pipelines are made by the makers, the draw skipped meanwhile as gpu_d3d11.c's are. 1D textures
 * get 1D images of their own, depth textures in guest memory 32-bit float depth images. */
extern u64 gpu_stat_batches;   /* (gpu.c: submissions so far) */

static VkPhysicalDeviceLimits limits;
static VkPhysicalDeviceFeatures enabled;
static int have_mirror_clamp;
static VkDescriptorSetLayout gfx_set_layout, cs_set_layout;
static VkPipelineLayout gfx_layout, cs_layout;
static VkPipelineCache pipeline_cache;
static char pipeline_cache_path[1024];
static u32 limits_vendor, limits_device;
static VkBuffer dummy_buffer;
static VkBufferView dummy_texels;
static Alloc dummy_buffer_mem;
static VkImage dummy_images[3];      /* 1D, 2D (six layers), 3D */
static Alloc dummy_image_mem[3];
static VkImageView dummy_views[8];   /* by TEX_* (TEX_ARRAY_3D: none) */
static VkSampler default_sampler, default_compare;

/* Copy guest GPU memory [va, va + size) into dst (zeros where unmapped), as gpu_d3d11.c's read_gpu. */
static void read_gpu(u64 va, u32 size, void *dst) {
    u64 g = gpu_va_to_guest(va);
    if (!size) return;
    if (g && gpu_va_to_guest(va + size - 1) == g + size - 1) { memcpy(dst, P(void, g), size); return; }
    for (u32 done = 0; done < size;) {
        u32 n = 0x1000 - (u32)((va + done) & 0xFFF);
        if (n > size - done) n = size - done;
        u64 p = gpu_va_to_guest(va + done);
        if (p) memcpy((u8 *)dst + done, P(void, p), n);
        else memset((u8 *)dst + done, 0, n);
        done += n;
    }
}

static void kept_written(u64 lo, u64 hi);
/* Writes of the GPU's own commands counted by (hashed) 64 KB block of guest memory, and those of no known
 * place: an upload is the same while the counts over its blocks are (a log of the last writes, as
 * gpu_d3d11.c's, overflows: a frame's constant buffer loads are many thousands). */
static u32 block_gen[1 << 16];
static u64 any_gen;
#define KEPT_BLOCK(block) ((u32)((u64)(block) * 0x9E3779B97F4A7C15ull >> 48))
static u64 writes_over(u64 g, u64 size) {
    u64 h = any_gen;
    for (u64 b = g >> 16; b <= (g + size - 1) >> 16; b++) h = h * 0x100000001B3ull + block_gen[KEPT_BLOCK(b)];
    return h;
}
static int watched(u64 g, u64 size);
/* What the GPU's own commands wrote to guest memory (constant buffer loads, inline writes, copies,
 * semaphores), by guest address (gpu_d3d11.c keeps a log of them): within one submission nothing else
 * changes what its draws read, so the same bytes from the same place are uploaded once unless one of
 * these writes touched them since. */
static void note_wrote(u64 va, u64 bytes) {   /* bytes == 0: anywhere */
    u64 lo = 0, hi = ~0ull, g = bytes ? gpu_va_to_guest(va) : 0;
    if (g && gpu_va_to_guest(va + bytes - 1) == g + bytes - 1) { lo = g; hi = g + bytes; }
    kept_written(lo, hi);
    if (hi - lo > (16ull << 20)) any_gen++;   /* (somewhere unknown, or much: everything) */
    else for (u64 b = lo >> 16; b <= (hi - 1) >> 16; b++) block_gen[KEPT_BLOCK(b)]++;
}

/* Constant buffers of 4 KB or more stay on the GPU between draws (gpu_d3d11.c's "kept" buffers: NVN binds
 * 64 KB for each program's own constants, and copying them again for every submission was most of a
 * draw's time): a buffer of their own, holding guest memory as it was when made, for as long as nothing
 * writes it - the CPU (the write watch, asked at its first use in a submission) or the GPU's own commands
 * (note_wrote's writes, which mark the kept buffers they touch). A changed one is made again (the old
 * one buried: the GPU may be reading it); one that keeps changing (twice within KEPT_SETTLE frames) is
 * uploaded with the rest for a while instead. Only memory in the write-watched alias area is kept. */
#define KEPT_SETS 1024
#define KEPT_WAYS 4
#define KEPT_SETTLE 8
#define KEPT_PAUSE 60
typedef struct Kept { u64 va, guest, seen, used, checked, made, paused_to; u32 size; int stale; VkBuffer buf; Alloc mem; VkBufferView view; u8 *shadow; } Kept;
/* (shadow: a copy of what it holds, compared with guest memory when a write may have changed it - the write
 * watch's pages are shared with other data the CPU rewrites each frame) */
static Kept kept[KEPT_SETS * KEPT_WAYS];
static u16 kept_blocks[1 << 16];   /* kept buffers touching each (hashed) 64 KB block of guest memory */
static u64 vk_stat_kept_made, vk_stat_kept_hits, vk_stat_kept_stale;

static void kept_blocks_add(const Kept *e, int d) {
    for (u64 b = e->guest >> 16; b <= (e->guest + e->size - 1) >> 16; b++) kept_blocks[KEPT_BLOCK(b)] += (u16)d;
}
static void kept_drop(Kept *e) {
    if (!e->buf) return;
    kept_blocks_add(e, -1);
    bury_grave((Grave){VK_NULL_HANDLE, e->buf, VK_NULL_HANDLE, e->mem, e->view});
    free(e->shadow);
    e->shadow = NULL;
    e->buf = VK_NULL_HANDLE;
    e->view = VK_NULL_HANDLE;
    memset(&e->mem, 0, sizeof e->mem);
}
/* (note_wrote) guest memory [lo, hi) written by the GPU's commands: kept buffers over it are stale */
static void kept_written(u64 lo, u64 hi) {
    if (hi - lo > (1ull << 32)) return;   /* (somewhere unknown) */
    int any = 0;
    for (u64 b = lo >> 16; b <= (hi - 1) >> 16 && !any; b++) any = kept_blocks[KEPT_BLOCK(b)] != 0;
    if (!any) return;
    for (u32 i = 0; i < KEPT_SETS * KEPT_WAYS; i++)
        if (kept[i].buf && kept[i].guest < hi && lo < kept[i].guest + kept[i].size) kept[i].stale = 1;
}

/* The kept buffer for [va, va + size), or NULL (then uploaded with the rest). */
static VkBufferView kept_cb(u64 va, u32 size) {
    if (size < 0x1000) return VK_NULL_HANDLE;
    u64 g = gpu_va_to_guest(va);
    if (!g || gpu_va_to_guest(va + size - 1) != g + size - 1 || !watched(g, size)) return VK_NULL_HANDLE;
    Kept *set = &kept[((va >> 4) * 0x9E3779B97F4A7C15ull >> 54) * KEPT_WAYS], *e = NULL, *lru = &set[0];
    for (u32 w = 0; w < KEPT_WAYS && !e; w++) {
        if (set[w].va == va && set[w].size == size && set[w].guest == g) e = &set[w];
        else if (set[w].used < lru->used) lru = &set[w];
    }
    if (e && e->buf && !e->stale && e->checked != gpu_stat_batches) {   /* first use in this submission: has the CPU written it? */
        e->checked = gpu_stat_batches;
        if (watch_written(g, size, &e->seen)) e->stale = 1;
    }
    if (e && e->buf && e->stale && e->shadow && !memcmp(e->shadow, P(void, g), size)) e->stale = 0;   /* written, but the same bytes */
    if (e && e->buf && !e->stale) { e->used = ++tick; vk_stat_kept_hits++; return e->view; }
    if (e && e->buf) {   /* changed */
        vk_stat_kept_stale++;
        kept_drop(e);
        if (gfx_stat_presented - e->made < KEPT_SETTLE) e->paused_to = gfx_stat_presented + KEPT_PAUSE;
    }
    if (e && gfx_stat_presented < e->paused_to) { e->used = ++tick; return VK_NULL_HANDLE; }
    if (!e) { e = lru; kept_drop(e); memset(e, 0, sizeof *e); }
    u32 bytes = (size + 15) & ~15u;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, bytes, VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer buf;
    if (vkCreateBuffer(device, &bci, NULL, &buf) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(device, buf, &mr);
    Alloc mem;
    if (!dev_alloc_in(&mr, &mem, 1) || vkBindBufferMemory(device, buf, mem.mem, mem.off) != VK_SUCCESS) {
        dev_free(&mem);
        vkDestroyBuffer(device, buf, NULL);
        return VK_NULL_HANDLE;
    }
    e->va = va; e->guest = g; e->size = size; e->stale = 0; e->checked = gpu_stat_batches; e->made = gfx_stat_presented; e->used = ++tick;
    watch_written(g, size, &e->seen);   /* (before reading: a write meanwhile is news next time) */
    if ((e->shadow = malloc(size)) != NULL) memcpy(e->shadow, P(void, g), size);   /* (then the buffer from the copy: the same bytes) */
    memcpy(mem.ptr, e->shadow ? e->shadow : P(u8, g), size);
    if (bytes > size) memset(mem.ptr + size, 0, bytes - size);
    VkBufferViewCreateInfo vci = {VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, NULL, 0, buf, VK_FORMAT_R32_UINT, 0, bytes};
    VkBufferView view;
    if (vkCreateBufferView(device, &vci, NULL, &view) != VK_SUCCESS) {
        bury_grave((Grave){VK_NULL_HANDLE, buf, VK_NULL_HANDLE, mem, VK_NULL_HANDLE});
        return VK_NULL_HANDLE;
    }
    e->buf = buf; e->mem = mem; e->view = view;
    kept_blocks_add(e, 1);
    vk_stat_kept_made++;
    vk_stat_upload_bytes += bytes;
    return view;
}

/* Guest GPU memory [va, va + size) as a raw buffer for a shader: copied into the frame's upload memory
 * (the same range again in the same submission, unwritten since: the view made for it). */
static u64 vk_stat_cb_copies[2], vk_stat_cb_bytes[2];
typedef struct Upload { u64 va, serial, batch, gen; u32 size; VkBufferView view; } Upload;   /* (gen: writes_over when made) */
static Upload uploads[8192];
static VkBufferView upload(u64 va, u32 size) {
    if (!va || !size) return dummy_texels;
    u32 padded = (size + 15) & ~15u;
    if (padded > limits.maxTexelBufferElements * 4ull) padded = size = limits.maxTexelBufferElements * 4u;
    u64 g = gpu_va_to_guest(va);
    const u8 *src = g && gpu_va_to_guest(va + size - 1) == g + size - 1 ? P(const u8, g) : NULL;
    Upload *u = &uploads[((va >> 4) * 0x9E3779B97F4A7C15ull ^ size) >> 51 & 8191];
    cmd();
    if (src && u->view && u->va == va && u->size == size && u->serial == frames[frame_at].serial && u->batch == gpu_stat_batches &&
        u->gen == writes_over(g, size)) {
        vk_stat_reused++;
        return u->view;
    }
    VkBuffer buf;
    VkDeviceSize off;
    u8 *dst = arena(padded, texel_align, &buf, &off);
    if (!dst) return dummy_texels;
    if (src) memcpy(dst, src, size);
    else read_gpu(va, size, dst);
    if (padded > size) memset(dst + size, 0, padded - size);
    vk_stat_upload_bytes += padded;
    VkBufferView v = arena_view(buf, off, padded);
    if (!v) return dummy_texels;
    *u = (Upload){va, frames[frame_at].serial, gpu_stat_batches, src ? writes_over(g, size) : 0, size, src ? v : VK_NULL_HANDLE};
    return v;
}

/* ---- resident regions: what compute launches write stays on the GPU (step 6) ------------------------
 * As gpu_d3d11.c's resident regions, simpler: every storage buffer of a launch, in the write-watched alias
 * area, becomes (or is part of) a region - a device buffer holding the guest memory there, taken again
 * from guest memory when the CPU writes it (the write watch, asked once a submission), and written by the
 * launches; draws then read their vertex streams and storage buffers from it where it covers them (the
 * fighters' skinned vertices). gpu_d3d11.c learns which buffers a program writes (reading results back
 * for its first runs) and brings results back to guest memory when the copy engine reads them; not here
 * yet: results stay on the GPU, guest memory keeps what it had. Regions that overlap are merged. */
typedef struct Resident { u64 va, size, guest, seen, asked, used; VkDeviceSize bytes; VkBuffer buf; Alloc mem; u64 written; } Resident;   /* (written: the frame a launch last wrote it) */
#define MAX_RESIDENTS 512
static Resident residents[MAX_RESIDENTS];
static u32 nresidents;   /* (slots in use up to here; a free one has no buffer) */
static u64 vk_stat_residents_made, vk_stat_resident_refills, vk_stat_skip_compute, vk_stat_flushes, vk_stat_residents_retired,
    vk_stat_learned, vk_stat_resident_copies, vk_stat_resident_flushed;
static VkDeviceSize ssbo_align = 256;

static VkBuffer make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, Alloc *a) {
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer b;
    if (vkCreateBuffer(device, &bci, NULL, &b) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(device, b, &mr);
    if (!dev_alloc(&mr, a) || vkBindBufferMemory(device, b, a->mem, a->off) != VK_SUCCESS) {
        dev_free(a);
        vkDestroyBuffer(device, b, NULL);
        return VK_NULL_HANDLE;
    }
    return b;
}

static void resident_drop(Resident *r) {
    if (!r->buf) return;
    bury_grave((Grave){VK_NULL_HANDLE, r->buf, VK_NULL_HANDLE, r->mem});
    memset(r, 0, sizeof *r);
}

/* Guest memory [r->va + off, + n) into the region's buffer (recorded now, outside renderings). */
static void resident_fill(Resident *r, VkDeviceSize off, u64 n) {
    VkBuffer src;
    VkDeviceSize soff;
    u8 *p = arena(n, 16, &src, &soff);
    if (!p) return;
    read_gpu(r->va + off, (u32)n, p);
    VkCommandBuffer c = outside();
    VkBufferCopy bc = {soff, off, n};
    vkCmdCopyBuffer(c, src, r->buf, 1, &bc);
    sync_all(c);
    vk_stat_upload_bytes += n;
}

/* Has the CPU written the region's memory since it was taken (asked once a submission)? A region the
 * launches keep writing is the GPU's (its pages are shared with data the CPU rewrites: taking them again
 * put the guest's stale vertices over the skinned ones - Donkey Kong drawn pale); one no launch wrote for
 * a few frames is the CPU's again, taken from guest memory. */
#define RESIDENT_OWNED 3
#define RESIDENT_IDLE 60
static void resident_check(Resident *r) {
    r->used = ++tick;
    if (r->asked == gpu_stat_batches) return;
    r->asked = gpu_stat_batches;
    if (watch_written(r->guest, r->size, &r->seen) && gfx_stat_presented - r->written > RESIDENT_OWNED) {
        resident_fill(r, 0, r->size);
        vk_stat_resident_refills++;
    }
}

static Resident *resident_covering(u64 va, u64 size) {
    for (u32 i = 0; i < nresidents; i++)
        if (residents[i].buf && residents[i].va <= va && va + size <= residents[i].va + residents[i].size) return &residents[i];
    return NULL;
}

/* The region holding [va, va + size), made (over any it overlaps, whose GPU data it keeps) if there is
 * none: NULL where memory is not one watched piece. */
static Resident *resident_acquire(u64 va, u64 size) {
    Resident *r = resident_covering(va, size);
    if (r) { resident_check(r); return r; }
    u64 lo = va, hi = va + size;
    for (int again = 1; again;) {   /* (the union of every region it touches) */
        again = 0;
        for (u32 i = 0; i < nresidents; i++) {
            Resident *o = &residents[i];
            if (!o->buf || o->va >= hi || o->va + o->size <= lo) continue;
            if (o->va < lo) { lo = o->va; again = 1; }
            if (o->va + o->size > hi) { hi = o->va + o->size; again = 1; }
        }
    }
    u64 g = gpu_va_to_guest(lo);
    if (!g || gpu_va_to_guest(hi - 1) != g + (hi - lo) - 1 || !watched(g, hi - lo) || hi - lo > (256ull << 20)) return NULL;
    Resident *n = NULL, *lru = NULL;
    for (u32 i = 0; i < MAX_RESIDENTS && !n; i++) {
        if (i == nresidents) { n = &residents[nresidents++]; break; }
        if (!residents[i].buf) n = &residents[i];
        else if (!lru || residents[i].used < lru->used) lru = &residents[i];
    }
    if (!n) { resident_drop(lru); n = lru; }
    Resident made = {lo, hi - lo, g, 0, gpu_stat_batches, ++tick, ((hi - lo + 15) & ~15ull) + 256, VK_NULL_HANDLE, {0}, gfx_stat_presented};
    made.buf = make_buffer(made.bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &made.mem);
    if (!made.buf) return NULL;
    *n = made;
    watch_written(g, hi - lo, &n->seen);   /* (before reading: a write meanwhile is news next time) */
    resident_fill(n, 0, hi - lo);
    VkCommandBuffer c = outside();
    for (u32 i = 0; i < nresidents; i++) {   /* the regions it replaces: their GPU data over the guest's */
        Resident *o = &residents[i];
        if (o == n || !o->buf || o->va < lo || o->va + o->size > hi) continue;
        VkBufferCopy bc = {0, o->va - lo, o->size};
        vkCmdCopyBuffer(c, o->buf, n->buf, 1, &bc);
        resident_drop(o);
    }
    sync_all(c);
    vk_stat_residents_made++;
    return n;
}

/* A view of a region's data from va, as a raw buffer: starting `skip` bytes before va where va is not
 * aligned as Vulkan wants views to start (the shader is told: offsets, or the buffer's address, move). */
static VkBufferView resident_texels(Resident *r, u64 va, u64 size, u32 *skip) {
    VkDeviceSize off = va - r->va, at = off / texel_align * texel_align, range = (off - at + size + 15) & ~15ull;
    if (at + range > r->bytes) range = (r->bytes - at) & ~3ull;
    *skip = (u32)(off - at);
    if (range / 4 > limits.maxTexelBufferElements) range = (VkDeviceSize)limits.maxTexelBufferElements * 4;
    return texel_view(r->buf, at, range);
}

/* Compute inputs (what a launch only reads: bind poses, skin weights): device buffers of guest memory, taken
 * again when the write watch says the CPU wrote them (gpu_d3d11.c's input buffers). */
#define MAX_INPUTS 128
static Resident inputs[MAX_INPUTS];
static Resident *input_buffer(u64 va, u64 size) {
    u64 g = gpu_va_to_guest(va);
    if (!g || !size || !watched(g, size) || gpu_va_to_guest(va + size - 1) != g + size - 1) return NULL;
    Resident *b = NULL, *lru = &inputs[0];
    for (u32 i = 0; i < MAX_INPUTS && !b; i++) {
        if (inputs[i].buf && inputs[i].va == va && inputs[i].size == size && inputs[i].guest == g) b = &inputs[i];
        else if (inputs[i].used < lru->used) lru = &inputs[i];
    }
    if (b) {
        b->used = ++tick;
        /* (asked every launch: asking once a submission, as resident_check does, drew the fighters pale again -
         * the CPU rewrites these inputs between launches of one submission) */
        if (watch_written(g, size, &b->seen)) resident_fill(b, 0, size);
        return b;
    }
    b = lru;
    resident_drop(b);
    Resident made = {va, size, g, 0, 0, ++tick, ((size + 15) & ~15ull) + 256, VK_NULL_HANDLE, {0}, 0};
    made.buf = make_buffer(made.bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, &made.mem);
    if (!made.buf) return NULL;
    *b = made;
    watch_written(g, size, &b->seen);
    resident_fill(b, 0, size);
    return b;
}

/* The frame's work so far submitted and waited for (results to read back: learning, flushes). The
 * frame's upload memory may be read until cmd() begins again. */
static void submit(VkSemaphore wait, VkSemaphore signal);
static void flush_wait(void) {
    Frame *f = &frames[frame_at];
    if (!f->recording) return;
    submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
    u64 t0 = rt_clock_ns();
    check(vkWaitForFences(device, 1, &f->done, VK_TRUE, UINT64_MAX), "waiting for results");
    vk_ns_wait += rt_clock_ns() - t0;
    vk_stat_flushes++;
}

/* Words to guest memory, through GPU addresses (as gpu_d3d11.c writes results back while learning). */
static void write_gpu(u64 va, const u8 *src, u64 n) {
    for (u64 done = 0; done < n;) {
        u64 k = 0x1000 - ((va + done) & 0xFFF);
        if (k > n - done) k = n - done;
        u64 g = gpu_va_to_guest(va + done);
        if (g) { memcpy(P(void, g), src + done, k); gpu_rb_wrote(g, k); }
        done += k;
    }
}

/* Once a frame: regions no launch wrote for a while are given up (memory a scene leaves is handed out
 * again; the guest's copy is the data then). */
static void residents_retire(void) {
    for (u32 i = 0; i < nresidents; i++)
        if (residents[i].buf && gfx_stat_presented - residents[i].written > RESIDENT_IDLE) { resident_drop(&residents[i]); vk_stat_residents_retired++; }
}

static void residents_unmapped(u64 lo, u64 hi, int guest) {
    for (u32 i = 0; i < nresidents; i++) {
        Resident *r = &residents[i];
        u64 a = guest ? r->guest : r->va;
        if (r->buf && a < hi && lo < a + r->size) resident_drop(r);
    }
    for (u32 i = 0; i < MAX_INPUTS; i++) {
        Resident *r = &inputs[i];
        u64 a = guest ? r->guest : r->va;
        if (r->buf && a < hi && lo < a + r->size) resident_drop(r);
    }
}

/* ---- the makers: threads that make shaders and pipelines (gpu_d3d11.c's compiler threads) ------------- */
/* A draw whose shaders or pipeline are not made yet is skipped, as Direct3D 11's are while its shaders
 * compile: the game's frame does not wait for a driver's compiler (vkd3d-shader's SPIR-V of the bigger
 * programs takes the driver a second or more). Big stacks: drivers' compilers recurse. */
typedef struct Job { struct Job *next; void (*run)(struct Job *job); } Job;
static SRWLOCK jobs_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE jobs_cv = CONDITION_VARIABLE_INIT;
static Job *jobs_head, *jobs_tail;
static volatile LONG jobs_pending;
static u32 nmakers;

static DWORD WINAPI maker_main(void *unused) {
    (void)unused;
    for (;;) {
        AcquireSRWLockExclusive(&jobs_lock);
        while (!jobs_head) SleepConditionVariableSRW(&jobs_cv, &jobs_lock, INFINITE, 0);
        Job *j = jobs_head;
        jobs_head = j->next;
        if (!jobs_head) jobs_tail = NULL;
        ReleaseSRWLockExclusive(&jobs_lock);
        j->run(j);
        InterlockedDecrement(&jobs_pending);
    }
}

static void job_add(Job *j) {
    j->next = NULL;
    InterlockedIncrement(&jobs_pending);
    AcquireSRWLockExclusive(&jobs_lock);
    if (jobs_tail) jobs_tail->next = j;
    else jobs_head = j;
    jobs_tail = j;
    ReleaseSRWLockExclusive(&jobs_lock);
    WakeConditionVariable(&jobs_cv);
}

static void makers_start(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    nmakers = si.dwNumberOfProcessors / 2;
    if (nmakers < 1) nmakers = 1;
    if (nmakers > 6) nmakers = 6;
    for (u32 i = 0; i < nmakers; i++) {
        HANDLE t = CreateThread(NULL, 64ull << 20, maker_main, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        if (!t) continue;
        SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);
        CloseHandle(t);
    }
}

/* SSBU_VK_WAIT=1: draws wait for their shaders and pipelines instead (every draw drawn: pictures to
 * compare with Direct3D 11's, which waits the same way with SSBU_SHADER_WAIT... when it has it). */
static int wait_makes(void) {
    static int v = -1;
    if (v < 0) v = getenv("SSBU_VK_WAIT") && atoi(getenv("SSBU_VK_WAIT")) != 0;
    return v;
}
static void wait_for(volatile LONG *state) {
    while (!*state) SwitchToThread();
}

/* What each stage's constant buffer slot was given last in this frame, with a copy of the bytes (gpu_d3d11.c's
 * last_upload): the same bytes from the same place again - most of a frame's 64 KB program constants, outside
 * the write-watched area, so not kept - get the same view, compared rather than copied again. */
/* SSBU_VK_MEMO=0: what a submission's draws found once is looked at again each time (constant buffers compared,
 * shaders fingerprinted, textures looked up), as before: for measuring, and if a picture ever differs.
 * SSBU_VK_MEMO=check: both, every time, and the status lines count the answers that differed (the test). */
static u64 vk_stat_memo_checked, vk_stat_memo_wrong[3];   /* (constant buffers, shaders, textures) */
static int memo_on(void) {   /* 0 off, 1 on, 2 checked */
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("SSBU_VK_MEMO");
        on = v && !strcmp(v, "check") ? 2 : !(v && *v == '0');
    }
    return on;
}
typedef struct LastCb { u64 va, serial, batch, gen, g; u32 size, cap, run; VkBufferView view; u8 *copy; } LastCb;   /* (run: the bytes mapped, the rest zeros;
 * batch, gen: the submission it was last found the same in, and writes_over then) */
static LastCb last_cb[2][18];
static u64 vk_stat_cb_compared, vk_stat_cb_same, vk_stat_cb_unchanged;
/* Big ones whose place changes from draw to draw (a ring of per-draw constants, each draw binding 64 KB
 * from its own start): GPU memory in 128 KB windows from 64 KB boundaries, read page by page as read_gpu
 * does (zeros where unmapped), copied once a submission - unless a page's mapping changed or the GPU's own
 * commands wrote it since (the window's signature: each page's guest address and write count) - and each
 * draw a view into the window holding its binding. */
#define WINDOWS 2048
#define WINDOW (128u << 10)
typedef struct Window { u64 base, serial, batch, sig; VkBuffer buf; VkDeviceSize off; } Window;
static Window windows[WINDOWS];
static u64 vk_stat_window_hits, vk_stat_window_copies;
static VkBufferView upload_window(u64 va, u32 size) {
    u64 base = va & ~0xFFFFull;
    if (size > 0x10000 || (va - base) % texel_align) return VK_NULL_HANDLE;
    u64 sig = any_gen;
    for (u64 o = 0; o < WINDOW; o += 0x1000) {
        u64 g = gpu_va_to_guest(base + o);
        sig = (sig ^ g) * 0x100000001B3ull + (g ? block_gen[KEPT_BLOCK(g >> 16)] : 0);
    }
    Window *w = &windows[(base >> 16) * 0x9E3779B97F4A7C15ull >> 53 & (WINDOWS - 1)];
    cmd();
    if (!(w->buf && w->base == base && w->serial == frames[frame_at].serial && w->batch == gpu_stat_batches && w->sig == sig)) {
        u8 *dst = arena(WINDOW, texel_align, &w->buf, &w->off);
        if (!dst) { w->buf = VK_NULL_HANDLE; return VK_NULL_HANDLE; }
        read_gpu(base, WINDOW, dst);
        w->base = base; w->serial = frames[frame_at].serial; w->batch = gpu_stat_batches; w->sig = sig;
        vk_stat_window_copies++;
        vk_stat_upload_bytes += WINDOW;
    } else vk_stat_window_hits++;
    return arena_view(w->buf, w->off + (va - base), (size + 15) & ~15u);
}

/* Big ones (4 KB or more) by their place, for the frame: a slot takes turns between several. */
#define BIG_CBS 1024
static LastCb big_cb[BIG_CBS];
static VkBufferView upload_slot(int stage, u32 slot, u64 va, u32 size) {
    LastCb *l = size >= 0x1000 ? &big_cb[((va >> 4) * 0x9E3779B97F4A7C15ull ^ size) >> 54 & (BIG_CBS - 1)]
                               : &last_cb[stage == SHADER_VERTEX ? 0 : 1][slot];
    /* (found the same in this submission and unwritten since: as below, without looking through its pages first -
     * a mapping that changes meanwhile counts as a write: note_wrote(0, 0)) */
    if (l->view && l->run && l->va == va && l->size == size && l->batch == gpu_stat_batches && l->serial == frames[frame_at].serial && memo_on() == 1 &&
        l->gen == writes_over(l->g, l->run)) {
        vk_stat_cb_unchanged++;
        return l->view;
    }
    u64 g = size ? gpu_va_to_guest(va) : 0;
    /* the bytes mapped in one piece from va; a 64 KB binding often runs past what is mapped, which reads as
     * zeros: then the mapped part is compared */
    u32 run = 0;
    if (g) {
        run = 0x1000 - (u32)(va & 0xFFF);
        while (run < size && gpu_va_to_guest(va + run) == g + run) run += 0x1000;
        if (run > size) run = size;
        for (u32 o = run; o < size && run; o += 0x1000)
            if (gpu_va_to_guest(va + o)) run = 0;   /* (mapped elsewhere: not compared) */
    }
    const u8 *src = run ? P(const u8, g) : NULL;
    cmd();
    u64 gen = src ? writes_over(g, run) : 0;
    if (src && l->view && l->va == va && l->size == size && l->run == run && l->serial == frames[frame_at].serial) {
        /* (in the submission it was found the same in, unwritten by the GPU's commands since: the same bytes,
         * as upload() takes them - not compared again) */
        if (l->batch == gpu_stat_batches && l->gen == gen && memo_on()) {
            if (memo_on() == 2) { vk_stat_memo_checked++; if (memcmp(l->copy, src, run)) vk_stat_memo_wrong[0]++; }
            vk_stat_cb_unchanged++;
            return l->view;
        }
        vk_stat_cb_compared++;
        if (!memcmp(l->copy, src, run)) { vk_stat_cb_same++; l->batch = gpu_stat_batches; l->gen = gen; l->g = g; return l->view; }
    }
    VkBufferView v = size >= 0x1000 ? upload_window(va, size) : VK_NULL_HANDLE;
    if (!v) {
        u64 r0 = vk_stat_reused;
        v = upload(va, size);
        if (vk_stat_reused == r0) { vk_stat_cb_copies[size >= 0x1000]++; vk_stat_cb_bytes[size >= 0x1000] += size; }
    }
    if (!src || v == dummy_texels) { l->view = VK_NULL_HANDLE; return v; }
    if (l->cap < size) {
        free(l->copy);
        l->cap = size;
        if (!(l->copy = malloc(size))) { l->cap = 0; l->view = VK_NULL_HANDLE; return v; }
    }
    memcpy(l->copy, src, run);
    l->va = va; l->size = size; l->run = run; l->serial = frames[frame_at].serial; l->view = v;
    l->batch = gpu_stat_batches; l->gen = gen; l->g = g;
    return v;
}

/* ---- shaders ---------------------------------------------------------------------------------------- */
typedef struct VShader {
    Job job;            /* (first: the job is the shader) */
    char *hlsl;         /* (while it is made) */
    u64 hash, table_hash;
    u32 vs_outputs;
    int stage;
    volatile LONG state;   /* 0 being made, 1 ready, 2 failed */
    VkPipeline compute;    /* (a compute program's pipeline, made with it) */
    u32 written_mask, learn_runs;   /* compute: storage buffers it was seen to write, over its first runs */
    VkShaderModule module;
    VkDescriptorUpdateTemplate tmpl;   /* (stage_set's writes for this program, made at its first set; 0 with tmpl_failed: none) */
    int tmpl_failed;
    int glslang;        /* its SPIR-V is glslang's: SV_VertexID is the vertex index itself (see vk_draw) */
    struct WarmShader *adopt, *publish;   /* the warm start's (below): its module taken once loaded; ours given to it */
    char name[64];
    ShaderInfo info;
} VShader;
static VShader **vshaders;
static u32 nvshaders, vshaders_cap;
typedef struct VSlot { u64 g, key, print; u32 size; VShader *s; u64 tables_at, batch, gen; } VSlot;   /* (batch, gen: the submission
 * its fingerprint was last found the same in, and writes_over then: not taken again in it) */
#define VSLOTS 16384
static VSlot vslots[VSLOTS];

typedef struct CbufSource { const CbBinding *cb; } CbufSource;
static u32 read_cbuf_word(void *user, u32 bank, u32 offset) {
    const CbufSource *src = user;
    u32 v = 0;
    if (bank < 18 && src->cb[bank].address && offset + 4 <= src->cb[bank].size) read_gpu(src->cb[bank].address + offset, 4, &v);
    return v;
}
static u64 table_hash(const ShaderInfo *info, CbufSource *src) {
    u64 h = 0;
    for (u32 t = 0; t < info->ntables; t++)
        for (u32 e = 0; e < info->table[t].count; e++)
            h = (h ^ read_cbuf_word(src, info->table[t].bank, info->table[t].offset + 4 * e)) * 1099511628211ull;
    return h;
}
static u64 tables_place(const ShaderInfo *info, const CbBinding *cb) {
    if (!info->ntables) return 0;
    for (u32 t = 0; t < info->ntables; t++) if (info->table[t].bank != 1) return 0;
    return cb[1].address ? cb[1].address ^ (u64)cb[1].size << 48 : 0;
}
static u64 fingerprint(const u8 *p, u32 size) {
    u64 h = size;
    for (u32 i = 0; i < 32; i++) {
        u64 v;
        memcpy(&v, p + (((u64)size - 8) * i / 31 & ~7ull), 8);
        h = (h ^ v) * 1099511628211ull;
    }
    return h;
}

static const char *cache_dir(void) {
    static char dir[1024];
    if (!dir[0]) {   /* (gpu_d3d11.c's: SSBU_SHADER_CACHE, else beside the modules) */
        if (getenv("SSBU_SHADER_CACHE")) snprintf(dir, sizeof dir, "%s", getenv("SSBU_SHADER_CACHE"));
        else snprintf(dir, sizeof dir, "%s/../shadercache", rt_data_dir);
        CreateDirectoryA(dir, NULL);
    }
    return dir;
}

/* Which translator makes the SPIR-V (chosen once, at the first shader): glslang from the HLSL when its
 * library loads (no fxc, no DXBC: gpu_spirv.h), else vkd3d-shader from the DXBC when the build has it.
 * SSBU_VK_SPIRV=vkd3d or glslang asks for one; the other is taken if it is missing. */
static int use_glslang(void) {
    static int v = -1;
    if (v < 0) {
        const char *want = getenv("SSBU_VK_SPIRV");
        char why[300] = "";
        int glslang = spirv_have_glslang(why, sizeof why), vkd3d = spirv_have_vkd3d();
        v = glslang && (!vkd3d || !want || _stricmp(want, "vkd3d") != 0);
        fprintf(stderr, "[vulkan] shaders: %s%s%s\n", v ? "glslang (HLSL to SPIR-V, ssbu_glslang.dll)" : "vkd3d-shader (DXBC to SPIR-V)",
                !glslang ? "; no glslang: " : "", !glslang ? why : "");
    }
    return v;
}

/* glslang for a game program of this stage (SHADER_*): SSBU_VK_GLSLANG_STAGES=<mask> (bit 0 vertex, 1 pixel,
 * 2 compute; debugging, with both translators in the build) gives the others to vkd3d-shader. */
static int glslang_for(int stage) {
    static int mask = -1;
    if (mask < 0) mask = getenv("SSBU_VK_GLSLANG_STAGES") ? atoi(getenv("SSBU_VK_GLSLANG_STAGES")) & 7 : 7;
    return use_glslang() && (spirv_have_vkd3d() ? mask >> stage & 1 : 1);
}

/* A file's bytes (malloc'd; *size), or NULL. */
static void *read_file(const char *path, size_t *size) {
    void *data = NULL;
    *size = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0 && (data = malloc((size_t)n)) && fread(data, 1, (size_t)n, f) == (size_t)n) *size = (size_t)n;
    else { free(data); data = NULL; }
    fclose(f);
    return data;
}

/* bytes written to `path` through a file of this process's, then renamed (another process may write it too) */
static void write_file(const char *path, const void *data, size_t size) {
    char tmp[1240];
    snprintf(tmp, sizeof tmp, "%s.vk%lu.tmp", path, GetCurrentProcessId());
    FILE *o = fopen(tmp, "wb");
    if (o) {
        int ok = fwrite(data, 1, size, o) == size;
        if (fclose(o) || !ok || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) DeleteFileA(tmp);
    }
}

/* ---- the warm start's programs ---------------------------------------------------------------------- */
/* The pipelines the game drew with are listed in a file (further down: the warm list), and made again at the
 * next start by threads of their own, before the game asks for them: a first draw then finds its pipeline
 * made, and waits for no compiler. A listed pipeline names its programs by the keys of their SPIR-V files
 * (shader_cache_file_spv); the warm threads load those into this table, and a draw's new program (find_shader)
 * takes its module from here instead of making its own - the same module a listed pipeline was made with,
 * which is what a pipeline is found by (PipeKey). glslang's programs only: vkd3d-shader's are not listed. */
typedef struct WarmKey { u64 hash, table_hash; u32 vs_outputs, stage; } WarmKey;   /* (no padding: hashed and compared as bytes) */
enum { WARM_LISTED, WARM_LOADING, WARM_READY, WARM_NONE, WARM_GAME };   /* (GAME: a draw makes it, and gives it: READY or NONE after) */
typedef struct WarmShader { WarmKey k; u32 used; volatile LONG state; VkShaderModule module; VkPipeline compute; } WarmShader;
static WarmShader *warm_shaders;   /* by key, open addressing; not changed once the warm threads run (the states are) */
static u32 warm_shader_slots;
static u64 vk_stat_warm_taken;
static volatile LONG64 vk_stat_warm_loaded, vk_stat_warm_pipelines;
static u32 desc_hash(const void *d, size_t size);

static WarmShader *warm_find(const WarmKey *k) {
    if (!warm_shader_slots) return NULL;
    for (u32 h = desc_hash(k, sizeof *k) & (warm_shader_slots - 1);; h = (h + 1) & (warm_shader_slots - 1)) {
        WarmShader *w = &warm_shaders[h];
        if (!w->used) return NULL;
        if (!memcmp(&w->k, k, sizeof *k)) return w;
    }
}
static void warm_list_compute(const WarmKey *k);

/* The program's SPIR-V by glslang: the disk cache's (shader_cache_file_spv), else made from the HLSL and kept there. */
static int spirv_glslang(VShader *s, const char *hlsl, SpirvCode *spv) {
    char path[1200];
    shader_cache_file_spv(path, sizeof path, cache_dir(), s->stage, s->hash, s->vs_outputs, s->table_hash, spirv_glslang_version());
    size_t size;
    void *code = read_file(path, &size);
    if (code && size >= 20 && size % 4 == 0 && *(const u32 *)code == 0x07230203) {
        spv->code = code;
        spv->size = size;
        return 1;
    }
    free(code);
    char err[600];
    if (!spirv_from_hlsl(hlsl, s->stage, spv, err, sizeof err)) {
        fprintf(stderr, "[vulkan] %s: glslang failed: %.500s\n", s->name, err);
        return 0;
    }
    write_file(path, spv->code, spv->size);
    return 1;
}

/* The program's SPIR-V by vkd3d-shader: its DXBC (the disk cache's, else fxc's, then kept there as gpu_d3d11.c
 * keeps it) translated. */
static int spirv_vkd3d(VShader *s, const char *hlsl, SpirvCode *spv);

/* The program as SPIR-V (spirv_glslang or spirv_vkd3d), as a shader module (and a compute program's pipeline). */
static void shader_make(VShader *s, const char *hlsl) {
    SpirvCode spv;
    if (!(glslang_for(s->stage) ? spirv_glslang(s, hlsl, &spv) : spirv_vkd3d(s, hlsl, &spv))) {
        InterlockedExchange(&s->state, 2);
        return;
    }
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, spv.size, spv.code};
    VkShaderModule module;
    int made = vkCreateShaderModule(device, &ci, NULL, &module) == VK_SUCCESS;
    spirv_free(&spv);
    if (made) s->module = module;
    if (made && s->stage == SHADER_COMPUTE) {
        VkComputePipelineCreateInfo cpi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
                                           {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", NULL},
                                           cs_layout, VK_NULL_HANDLE, -1};
        made = vkCreateComputePipelines(device, pipeline_cache, 1, &cpi, NULL, &s->compute) == VK_SUCCESS;
        if (!made) fprintf(stderr, "[vulkan] %s: cannot make its pipeline\n", s->name);
    }
    InterlockedExchange(&s->state, made ? 1 : 2);   /* (the module written before: a full barrier) */
}

static int spirv_vkd3d(VShader *s, const char *hlsl, SpirvCode *spv) {
    char path[1200];
    shader_cache_file(path, sizeof path, cache_dir(), s->stage, s->hash, s->vs_outputs, s->table_hash);
    size_t size = 0;
    void *dxbc = read_file(path, &size);
    ID3DBlob *blob = NULL;
    if (!dxbc) {
        ID3DBlob *errors = NULL;
        HRESULT hr = shader_cache_compile(hlsl, s->name, s->stage, &blob, &errors);
        if (FAILED(hr) || !blob) {
            fprintf(stderr, "[vulkan] %s: fxc failed: %.300s\n", s->name, errors ? (const char *)ID3D10Blob_GetBufferPointer(errors) : "?");
            if (errors) ID3D10Blob_Release(errors);
            if (blob) ID3D10Blob_Release(blob);
            return 0;
        }
        if (errors) ID3D10Blob_Release(errors);
        dxbc = ID3D10Blob_GetBufferPointer(blob);
        size = ID3D10Blob_GetBufferSize(blob);
        write_file(path, dxbc, size);
    }
    char err[600];
    int ok = spirv_from_dxbc(dxbc, size, s->stage, spv, err, sizeof err);
    if (blob) ID3D10Blob_Release(blob);
    else free(dxbc);
    if (!ok) fprintf(stderr, "[vulkan] %s: no SPIR-V: %.400s\n", s->name, err);
    return ok;
}

static void shader_job(Job *j) {
    VShader *s = (VShader *)j;
    shader_make(s, s->hlsl);
    free(s->hlsl);
    s->hlsl = NULL;
    InterlockedIncrement64((volatile LONG64 *)(s->state == 1 ? &vk_stat_shaders : &vk_stat_shader_failures));
    if (s->publish) {   /* (a listed program the game asked for before the warm threads came to it: they use this one) */
        s->publish->module = s->module;
        s->publish->compute = s->compute;
        InterlockedExchange(&s->publish->state, s->state == 1 ? WARM_READY : WARM_NONE);
    }
    if (s->state == 1 && s->stage == SHADER_COMPUTE && s->glslang) warm_list_compute(&(WarmKey){s->hash, s->table_hash, s->vs_outputs, SHADER_COMPUTE});
}

/* A new program the warm start has listed: its module (and a compute program's pipeline) taken from the warm
 * table once a warm thread has loaded it - asked again at each draw until then (find_shader_ex); made by a
 * maker after all if the warm thread found no file for it. The drawing thread's. */
static void warm_adopt(VShader *s) {
    WarmShader *w = s->adopt;
    LONG state = w->state;
    if (state == WARM_LOADING) return;
    s->adopt = NULL;
    if (state != WARM_READY) { job_add(&s->job); return; }
    s->module = w->module;
    s->compute = w->compute;
    free(s->hlsl);
    s->hlsl = NULL;
    vk_stat_warm_taken++;
    InterlockedIncrement64((volatile LONG64 *)&vk_stat_shaders);
    InterlockedExchange(&s->state, 1);
}

/* The shader for the program at guest address g (as gpu_d3d11.c's find_shader_ex), made by a maker if it
 * is new. alpha_tested: the pixel shader's variant with the alpha test. A compute program has no header:
 * it is translated as if it had one (never read); local and shared: its workgroup and shared memory. */
static VShader *find_shader_new(u64 g, int stage, u32 vs_outputs, const CbBinding *stage_cb, int alpha_tested, const u32 *local, u32 shared) {
    const u8 *p = stage == SHADER_COMPUTE ? P(u8, g) - 0x50 : P(u8, g);
    u32 skip = stage == SHADER_COMPUTE ? 0x50 : 0;
    u64 key = (u64)stage | (u64)vs_outputs << 4 | (stage == SHADER_COMPUTE ? ((u64)local[0] ^ (u64)local[1] << 11 ^ (u64)local[2] << 22 ^ (u64)shared << 33) << 36 : 0)
            | (u64)(alpha_tested != 0) << 63;
    VSlot *slot = &vslots[((g >> 4) * 0x9E3779B97F4A7C15ull ^ key) >> 50 & (VSLOTS - 1)];
    u64 gen = slot->s && slot->g == g && slot->key == key ? writes_over(g - skip, slot->size) : 0;
    int same_batch = slot->s && slot->g == g && slot->key == key && slot->batch == gpu_stat_batches && slot->gen == gen && memo_on();
    if (same_batch && memo_on() == 2) { vk_stat_memo_checked++; if (slot->print != fingerprint(p + skip, slot->size - skip)) vk_stat_memo_wrong[1]++; }
    if (slot->s && slot->g == g && slot->key == key && (same_batch || slot->print == fingerprint(p + skip, slot->size - skip))) {
        VShader *s = slot->s;
        slot->batch = gpu_stat_batches;
        slot->gen = gen;
        if (!s->info.ntables) return s;
        u64 place = tables_place(&s->info, stage_cb);
        if (place && slot->tables_at == place) return s;
        CbufSource src = {stage_cb};
        if (s->table_hash == table_hash(&s->info, &src)) { slot->tables_at = place; return s; }
    }
    u32 size = shader_size(p, 0x40000);
    u64 h = shader_program_hash(p + skip, size - skip);
    if (stage == SHADER_COMPUTE) h = (h ^ ((u64)local[0] | (u64)local[1] << 16 | (u64)local[2] << 32 | (u64)shared << 40)) * 1099511628211ull;
    u64 program_hash = h;
    h = shader_cache_hash(h, p, size, stage);
    if (alpha_tested) h = (h ^ 0x616C706861743032ull) * 1099511628211ull;
    CbufSource src = {stage_cb};
    for (u32 i = 0; i < nvshaders; i++) {
        VShader *s = vshaders[i];
        if (s->hash != h || s->stage != stage || (stage == SHADER_PIXEL && s->vs_outputs != vs_outputs)) continue;
        if (s->info.ntables && s->table_hash != table_hash(&s->info, &src)) continue;
        *slot = (VSlot){g, key, fingerprint(p + skip, size - skip), size, s, 0};
        return s;
    }
    if (nvshaders == vshaders_cap) {
        VShader **n = realloc(vshaders, sizeof *n * (vshaders_cap * 2 + 64));
        if (!n) return NULL;
        vshaders = n;
        vshaders_cap = vshaders_cap * 2 + 64;
    }
    VShader *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    vshaders[nvshaders++] = s;
    s->hash = h; s->stage = stage; s->vs_outputs = vs_outputs;
    *slot = (VSlot){g, key, fingerprint(p + skip, size - skip), size, s, 0};
    snprintf(s->name, sizeof s->name, "%s_%016llx%s", stage == SHADER_VERTEX ? "vs" : stage == SHADER_PIXEL ? "ps" : "cs", program_hash,
             alpha_tested ? "_alphatest" : "");
    int glslang = glslang_for(stage);   /* (its own dialect of the HLSL: gpu_shader.h) */
    s->glslang = glslang;
    char *hlsl = glslang ? (alpha_tested ? shader_translate_alpha_tested(SHADER_HLSL_GLSLANG, p, size, vs_outputs, &s->info, read_cbuf_word, &src)
                                         : shader_translate(SHADER_HLSL_GLSLANG, p, size, stage, vs_outputs, local, shared, &s->info, read_cbuf_word, &src))
               : stage == SHADER_COMPUTE ? shader_hlsl_cs(p, size, local, shared, &s->info, read_cbuf_word, &src)
               : alpha_tested            ? shader_hlsl_alpha_tested(p, size, vs_outputs, &s->info, read_cbuf_word, &src)
                                         : shader_hlsl_ex(p, size, stage, vs_outputs, &s->info, read_cbuf_word, &src);
    s->table_hash = table_hash(&s->info, &src);
    if (s->info.unsupported[0]) fprintf(stderr, "[gfx] %s: %s\n", s->name, s->info.unsupported);
    if (!hlsl) { s->state = 2; vk_stat_shader_failures++; return s; }
    static const char *dump;   /* SSBU_DUMP_HLSL=<dir>: each program's HLSL, as gpu_d3d11.c writes it (debugging) */
    static int dump_checked;
    if (!dump_checked++) dump = getenv("SSBU_DUMP_HLSL");
    if (dump) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s%s.hlsl", dump, s->name, glslang ? "_glslang" : "");
        FILE *f = fopen(path, "w");
        if (f) { fputs(hlsl, f); fclose(f); }
    }
    s->hlsl = hlsl;
    s->job.run = shader_job;
    char cached_path[1200];
    shader_cache_file(cached_path, sizeof cached_path, cache_dir(), s->stage, s->hash, s->vs_outputs, s->table_hash);
    int cached = glslang || GetFileAttributesA(cached_path) != INVALID_FILE_ATTRIBUTES;
    /* (a program of the warm list: a warm thread's module, or ours given to the warm threads) */
    WarmShader *w = glslang ? warm_find(&(WarmKey){s->hash, s->table_hash, s->vs_outputs, (u32)s->stage}) : NULL;
    if (w && InterlockedCompareExchange(&w->state, WARM_GAME, WARM_LISTED) == WARM_LISTED) s->publish = w;
    else if (w && (w->state == WARM_LOADING || w->state == WARM_READY)) s->adopt = w;
    if (s->adopt) warm_adopt(s);
    else job_add(&s->job);
    if (wait_makes())
        while (!s->state) { if (s->adopt) warm_adopt(s); SwitchToThread(); }
    /* a program compiled before (its DXBC in the disk cache: only the SPIR-V and the module to make, milliseconds) is
     * waited for a little, as gpu_d3d11.c does: a skipped draw is an effect missing the first time it is used (the
     * owner's report 2026-11-09), a skipped launch leaves its outputs (skinned vertices) unwritten for a frame.
     * Compute programs were waited for already; every stage now. One compiled from scratch (fxc) is not waited for;
     * with glslang there is no fxc (its SPIR-V is made from the HLSL in milliseconds): every program is waited for. */
    if (cached || stage == SHADER_COMPUTE)
        for (u64 until = GetTickCount64() + 250; s->state == 0 && GetTickCount64() < until;) { if (s->adopt) warm_adopt(s); SwitchToThread(); }
    return s;
}
static VShader *find_shader_ex(u64 g, int stage, u32 vs_outputs, const CbBinding *stage_cb, int alpha_tested, const u32 *local, u32 shared) {
    VShader *s = find_shader_new(g, stage, vs_outputs, stage_cb, alpha_tested, local, shared);
    if (s && s->adopt && !s->state) warm_adopt(s);
    return s;
}
static VShader *find_shader(u64 g, int stage, u32 vs_outputs, const CbBinding *stage_cb, int alpha_tested) {
    return find_shader_ex(g, stage, vs_outputs, stage_cb, alpha_tested, NULL, 0);
}

/* ---- textures ----------------------------------------------------------------------------------------- */
/* Texture formats (gpu_texture.c names them as DXGI formats), and how a view reads them. */
static VkFormat texture_format(DXGI_FORMAT f, VkComponentMapping *map) {
    *map = (VkComponentMapping){VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
    case DXGI_FORMAT_R8G8B8A8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
    case DXGI_FORMAT_R8G8B8A8_SINT: return VK_FORMAT_R8G8B8A8_SINT;
    case DXGI_FORMAT_R8G8B8A8_UINT: return VK_FORMAT_R8G8B8A8_UINT;
    case DXGI_FORMAT_B5G6R5_UNORM: return VK_FORMAT_R5G6B5_UNORM_PACK16;        /* (the same bits: blue lowest) */
    case DXGI_FORMAT_B5G5R5A1_UNORM: return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
    case DXGI_FORMAT_B4G4R4A4_UNORM:   /* blue in bits 0-3 .. alpha 12-15: read through R4G4B4A4 (red 12-15 .. alpha 0-3) */
        *map = (VkComponentMapping){VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_R};
        return VK_FORMAT_R4G4B4A4_UNORM_PACK16;
    case DXGI_FORMAT_R10G10B10A2_UNORM: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case DXGI_FORMAT_R11G11B10_FLOAT: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case DXGI_FORMAT_R9G9B9E5_SHAREDEXP: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case DXGI_FORMAT_R16G16B16A16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
    case DXGI_FORMAT_R16G16B16A16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
    case DXGI_FORMAT_R16G16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
    case DXGI_FORMAT_R16G16_UNORM: return VK_FORMAT_R16G16_UNORM;
    case DXGI_FORMAT_R16G16_SNORM: return VK_FORMAT_R16G16_SNORM;
    case DXGI_FORMAT_R16_FLOAT: return VK_FORMAT_R16_SFLOAT;
    case DXGI_FORMAT_R16_UNORM: return VK_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R16_SNORM: return VK_FORMAT_R16_SNORM;
    case DXGI_FORMAT_R16_SINT: return VK_FORMAT_R16_SINT;
    case DXGI_FORMAT_R16_UINT: return VK_FORMAT_R16_UINT;
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case DXGI_FORMAT_R32G32B32A32_SINT: return VK_FORMAT_R32G32B32A32_SINT;
    case DXGI_FORMAT_R32G32B32A32_UINT: return VK_FORMAT_R32G32B32A32_UINT;
    case DXGI_FORMAT_R32G32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
    case DXGI_FORMAT_R32G32_SINT: return VK_FORMAT_R32G32_SINT;
    case DXGI_FORMAT_R32G32_UINT: return VK_FORMAT_R32G32_UINT;
    case DXGI_FORMAT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
    case DXGI_FORMAT_R32_SINT: return VK_FORMAT_R32_SINT;
    case DXGI_FORMAT_R32_UINT: return VK_FORMAT_R32_UINT;
    case DXGI_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_UNORM;
    case DXGI_FORMAT_R8G8_SNORM: return VK_FORMAT_R8G8_SNORM;
    case DXGI_FORMAT_R8_UNORM: return VK_FORMAT_R8_UNORM;
    case DXGI_FORMAT_R8_SNORM: return VK_FORMAT_R8_SNORM;
    case DXGI_FORMAT_R8_SINT: return VK_FORMAT_R8_SINT;
    case DXGI_FORMAT_R8_UINT: return VK_FORMAT_R8_UINT;
    case DXGI_FORMAT_BC1_UNORM: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case DXGI_FORMAT_BC1_UNORM_SRGB: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case DXGI_FORMAT_BC2_UNORM: return VK_FORMAT_BC2_UNORM_BLOCK;
    case DXGI_FORMAT_BC2_UNORM_SRGB: return VK_FORMAT_BC2_SRGB_BLOCK;
    case DXGI_FORMAT_BC3_UNORM: return VK_FORMAT_BC3_UNORM_BLOCK;
    case DXGI_FORMAT_BC3_UNORM_SRGB: return VK_FORMAT_BC3_SRGB_BLOCK;
    case DXGI_FORMAT_BC4_UNORM: return VK_FORMAT_BC4_UNORM_BLOCK;
    case DXGI_FORMAT_BC4_SNORM: return VK_FORMAT_BC4_SNORM_BLOCK;
    case DXGI_FORMAT_BC5_UNORM: return VK_FORMAT_BC5_UNORM_BLOCK;
    case DXGI_FORMAT_BC5_SNORM: return VK_FORMAT_BC5_SNORM_BLOCK;
    case DXGI_FORMAT_BC6H_UF16: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
    case DXGI_FORMAT_BC6H_SF16: return VK_FORMAT_BC6H_SFLOAT_BLOCK;
    case DXGI_FORMAT_BC7_UNORM: return VK_FORMAT_BC7_UNORM_BLOCK;
    case DXGI_FORMAT_BC7_UNORM_SRGB: return VK_FORMAT_BC7_SRGB_BLOCK;
    default: return VK_FORMAT_UNDEFINED;   /* (R24G8: depth in guest memory; not yet) */
    }
}

static VkImageView make_view(VkImage image, VkImageViewType type, VkFormat format, VkImageAspectFlags aspect, u32 levels, u32 layers,
                             const VkComponentMapping *map) {
    VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, image, type, format,
                                {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
                                {aspect, 0, levels, 0, layers}};
    if (map) ci.components = *map;
    VkImageView v;
    return vkCreateImageView(device, &ci, NULL, &v) == VK_SUCCESS ? v : VK_NULL_HANDLE;
}

typedef struct Texture {
    u64 guest, size, sample_hash, seen, used, asked;
    u32 tic[8];
    int watched, stale, is3d, cube, is1d;
    u32 levels, layers;
    VkImageAspectFlags aspect;   /* (colour; depth for a depth texture read from guest memory) */
    VkImage image;
    Alloc alloc;
    VkFormat format;
    VkComponentMapping map;
    VkImageView views[8];   /* by TEX_* the shaders declare (made when first asked for) */
} Texture;
#define MAX_TEXTURES 4096
static Texture textures[MAX_TEXTURES];
static u32 ntextures;
#define TEXTURE_INDEX 16384
static u16 texture_index[TEXTURE_INDEX];
static u32 texture_index_adds;
static int depth_sampling = -1;

int tex_decode_rgba8(int nvn_format, const void *src, int w, int h, uint8_t *dst);   /* (ssbu-recomp/runtime/tex_decode.c) */
static u64 vk_stat_cpu_decoded, vk_stat_textures_1d, vk_stat_depth_textures;
/* BC7 often cannot store alpha 0: a clear texel is then (1,1,1,1), which the alpha test keeps (gpu_d3d11.c) */
static int punch_clear_texels(u8 *rgba, u32 n) {
    int punched = 0;
    for (u32 i = 0; i < n; i++) {
        u8 *p = rgba + (size_t)i * 4;
        if (p[3] && p[0] <= 1 && p[1] <= 1 && p[2] <= 1 && p[3] <= 1) { p[3] = 0; punched++; }
    }
    return punched;
}
static int watched(u64 g, u64 size) { return g >= ALIAS_BASE && g + size <= ALIAS_BASE + ALIAS_SIZE; }
static u64 sample_hash(u64 g, u64 size) {
    u64 h = 1469598103934665603ull;
    const u8 *p = P(u8, g);
    u64 step = size / 64 ? size / 64 : 1;
    for (u64 i = 0; i < size; i += step) { u64 v; memcpy(&v, p + (i & ~7ull), 8); h = (h ^ v) * 1099511628211ull; }
    return h;
}
static void warn_texture(const TextureDesc *d) {
    static u32 seen[64], n;
    u32 key = d->format | d->type << 8 | d->header_version << 12;
    for (u32 i = 0; i < n; i++) if (seen[i] == key) return;
    if (n < 64) seen[n++] = key;
    fprintf(stderr, "[vulkan] unsupported texture: format %#x, type %u, layout %u (%ux%u)\n", d->format, d->type, d->header_version, d->width, d->height);
}
static u32 texture_key(u64 g, const u32 *tic) {
    u64 h = g * 0x9E3779B97F4A7C15ull;
    for (u32 i = 0; i < 8; i++) h = (h ^ tic[i]) * 0x100000001B3ull;
    return (u32)(h >> 40) & (TEXTURE_INDEX - 1);
}
static Texture *texture_find(u64 g, const u32 *tic) {
    for (u32 k = texture_key(g, tic), n = 0; texture_index[k] && n < TEXTURE_INDEX; k = (k + 1) & (TEXTURE_INDEX - 1), n++) {
        Texture *t = &textures[texture_index[k] - 1];
        if (t->image && t->guest == g && !memcmp(t->tic, tic, 32)) return t;
    }
    return NULL;
}
static void texture_index_put(Texture *t) {
    u32 k = texture_key(t->guest, t->tic);
    while (texture_index[k]) k = (k + 1) & (TEXTURE_INDEX - 1);
    texture_index[k] = (u16)(t - textures + 1);
}
static void texture_index_add(Texture *t) {
    if (++texture_index_adds >= TEXTURE_INDEX / 2) {
        memset(texture_index, 0, sizeof texture_index);
        texture_index_adds = 0;
        for (u32 i = 0; i < ntextures; i++)
            if (textures[i].image && &textures[i] != t) { texture_index_put(&textures[i]); texture_index_adds++; }
    }
    texture_index_put(t);
}
static void texture_drop(Texture *t) {
    tex_gen++;
    for (int i = 0; i < 8; i++) bury_view(t->views[i]);
    if (t->image) bury_grave((Grave){t->image, VK_NULL_HANDLE, VK_NULL_HANDLE, t->alloc});
    memset(t->views, 0, sizeof t->views);
    t->image = VK_NULL_HANDLE;
    memset(&t->alloc, 0, sizeof t->alloc);
}

/* The view a shader declaring a texture of type `type` (TEX_*) reads it through, or NULL. */
static VkImageView texture_view(Texture *t, u32 type) {
    type &= 7;
    if (t->views[type]) return t->views[type];
    VkImageViewType vt;
    u32 layers = t->layers;
    if (t->is1d != (type == TEX_1D || type == TEX_ARRAY_1D)) return VK_NULL_HANDLE;
    switch (type) {
    case TEX_1D: vt = VK_IMAGE_VIEW_TYPE_1D; layers = 1; break;
    case TEX_ARRAY_1D: vt = VK_IMAGE_VIEW_TYPE_1D_ARRAY; break;
    case TEX_2D: if (t->is3d) return VK_NULL_HANDLE; vt = VK_IMAGE_VIEW_TYPE_2D; layers = 1; break;
    case TEX_ARRAY_2D: if (t->is3d) return VK_NULL_HANDLE; vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY; break;
    case TEX_3D: if (!t->is3d) return VK_NULL_HANDLE; vt = VK_IMAGE_VIEW_TYPE_3D; break;
    case TEX_CUBE: if (!t->cube) return VK_NULL_HANDLE; vt = VK_IMAGE_VIEW_TYPE_CUBE; layers = 6; break;
    case TEX_ARRAY_CUBE: if (!t->cube || !enabled.imageCubeArray) return VK_NULL_HANDLE; vt = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY; layers = t->layers / 6 * 6; break;
    default: return VK_NULL_HANDLE;
    }
    return t->views[type] = make_view(t->image, vt, t->format, t->aspect, t->levels, layers, &t->map);
}

/* A render target's view for sampling: plain or sRGB (the texture header says), as a 2D image or an
 * array of one. Depth: its depth alone. */
static VkFormat other_gamma(VkFormat f, int srgb) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: return srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: return srgb ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;
    default: return f;
    }
}
static VkImageView surface_sampled(Surface *s, int srgb, u32 type) {
    int k = (srgb ? 1 : 0) | (type == TEX_ARRAY_2D ? 2 : 0);
    if (type != TEX_2D && type != TEX_ARRAY_2D) return VK_NULL_HANDLE;
    if (!s->smp[k])
        s->smp[k] = make_view(s->image, type == TEX_ARRAY_2D ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
                              s->depth ? s->storage : other_gamma(s->storage, srgb), s->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
                              1, 1, NULL);
    return s->smp[k];
}
static VkImageView surface_attachment(Surface *s) {
    if (s->att && s->att_format == s->view) return s->att;
    bury_view(s->att);
    s->att_format = s->view;
    return s->att = make_view(s->image, VK_IMAGE_VIEW_TYPE_2D, s->view, aspect_of(s), 1, 1, NULL);
}

/* The texture a header describes, as a draw reads it (gpu_d3d11.c's get_texture): a render target at
 * its address, else the texture cache's (made from guest memory, again when the write watch or a
 * sample of its bytes says it changed). *rt: the render target it is, if one. */
static Texture *got_texture;   /* (get_texture: the cache's texture it gave a view of, else NULL) */
static VkImageView get_texture(const u32 *tic, u32 shader_type, float *scale, Surface **rt_out) {
    TextureDesc d;
    *scale = 1.0f;
    *rt_out = NULL;
    got_texture = NULL;
    int ok = tic_decode(tic, &d);
    u64 g = gpu_va_to_guest(d.address);
    if (!g) return VK_NULL_HANDLE;
    if (depth_sampling < 0) depth_sampling = !getenv("SSBU_NO_DEPTH_SAMPLING");
    int depth_tic = d.format == 0x29 || d.format == 0x2A || d.format == 0x2B || d.format == 0x2F || d.format == 0x30 || d.format == 0x3A;
    Surface *rt = NULL;
    for (u32 i = 0; i < nsurfaces; i++) {
        Surface *s = &surfaces[i];
        if (!s->image || s->guest != g) continue;
        if (s->depth && (!depth_sampling || !depth_tic || !ok || s->width != d.width || s->height != d.height)) continue;
        if (!rt || s->used > rt->used) rt = s;
    }
    if (rt) {
        *scale = (float)rt->hw / (float)rt->width;
        *rt_out = rt;
        return surface_sampled(rt, (int)d.srgb, shader_type & 7);
    }
    if (!ok || d.type == TIC_1D_BUFFER || d.bytes == 0) { warn_texture(&d); return VK_NULL_HANDLE; }
    VkComponentMapping map;
    VkFormat format = texture_format(d.dxgi, &map);
    /* Depth in guest memory (24-bit depth, 8 stencil): a 32-bit float depth image, the words converted on the
     * CPU - read as Direct3D 11 reads it through R24_UNORM_X8 (depth, 0, 0, 1), compares included */
    int depth24 = d.dxgi == DXGI_FORMAT_R24G8_TYPELESS;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    if (depth24) {
        format = VK_FORMAT_D32_SFLOAT;
        aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
        map = (VkComponentMapping){VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE};
    }
    if (!format || !(features_of(format) & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) { warn_texture(&d); return VK_NULL_HANDLE; }
    /* A program declaring a 1D texture reads a 1D image (Vulkan's views of a 2D one cannot be 1D): one of its
     * own, the texels' first row, kept beside any 2D one of the same header (its key marked). */
    u32 type7 = shader_type & 7;
    int want1d = type7 == TEX_1D || type7 == TEX_ARRAY_1D;
    u32 tk[8];
    memcpy(tk, tic, 32);
    if (want1d) {
        if (d.type == TIC_3D || d.bw > 1 || depth24) { warn_texture(&d); return VK_NULL_HANDLE; }
        tk[7] ^= 0x31443144u;
        tic = tk;
    }
    u32 layers = d.type == TIC_CUBE ? 6 : d.type == TIC_CUBE_ARRAY ? 6 * d.depth : d.type == TIC_2D_ARRAY || d.type == TIC_1D_ARRAY ? d.depth : 1;
    int is3d = d.type == TIC_3D;
    if (is3d) layers = 1;
    if (!layers) layers = 1;
    u64 size = is3d ? texture_3d_size(&d) : texture_layer_size(&d) * layers;
    int watch = watched(g, size);
    u64 sh = watch ? 0 : sample_hash(g, size);
    Texture *free_slot = NULL, *lru = NULL, *hit = texture_find(g, tic);
    if (hit) {
        if (watch && !hit->stale && hit->asked != gfx_stat_presented) {   /* (asked once a frame) */
            hit->asked = gfx_stat_presented;
            if (watch_written(hit->guest, hit->size, &hit->seen)) hit->stale = 1;
        }
        if (watch ? !hit->stale : hit->sample_hash == sh) { hit->used = ++tick; got_texture = hit; return texture_view(hit, shader_type); }
        free_slot = hit;
    } else
        for (u32 i = 0; i < ntextures; i++) {
            Texture *t = &textures[i];
            if (!t->image) { if (!free_slot) free_slot = t; continue; }
            if (!lru || t->used < lru->used) lru = t;
        }
    Texture *t = free_slot;
    if (!t) t = ntextures < MAX_TEXTURES ? &textures[ntextures++] : lru;
    texture_drop(t);
    if (t != hit) { t->guest = g; memcpy(t->tic, tic, 32); texture_index_add(t); }
    if (watch) watch_written(g, size, &t->seen);   /* (before reading: a write meanwhile is news next frame) */
    u32 levels = is3d ? 1 : d.levels, max_levels = 1;
    while ((d.width >> max_levels) || (!want1d && (d.height >> max_levels))) max_levels++;
    if (levels > max_levels) levels = max_levels;
    if (!levels) levels = 1;
    int cube = (d.type == TIC_CUBE || d.type == TIC_CUBE_ARRAY) && layers % 6 == 0 && d.width == d.height;
    /* A stock icon is BC7 sRGB with a clear margin (black, alpha 0), which the GPU samples with alpha 1 (a
     * black square): small sRGB block textures are decoded on the CPU (tex_decode.c) and the clear texels
     * mended, as gpu_d3d11.c does - kept above 512 only when a texel needed mending. */
    u8 **rgba = NULL;
    int nvn = want1d ? 0 : d.dxgi == DXGI_FORMAT_BC1_UNORM_SRGB ? 0x46 : d.dxgi == DXGI_FORMAT_BC2_UNORM_SRGB ? 0x47
            : d.dxgi == DXGI_FORMAT_BC3_UNORM_SRGB ? 0x48 : d.dxgi == DXGI_FORMAT_BC7_UNORM_SRGB ? 0x4E : 0;
    if (nvn && !is3d && d.width <= 1024 && d.height <= 1024 && (rgba = calloc(levels * layers, sizeof *rgba)) != NULL) {
        int ok = 1, punched = 0;
        for (u32 i = 0; i < levels * layers && ok; i++) {
            u32 l = i % levels, w = (d.width >> l) ? d.width >> l : 1, h = (d.height >> l) ? d.height >> l : 1;
            u8 *blk = malloc((size_t)((w + d.bw - 1) / d.bw) * ((h + d.bh - 1) / d.bh) * d.bytes + 64);
            rgba[i] = malloc((size_t)w * h * 4);
            if (!blk || !rgba[i]) ok = 0;
            else {
                texture_read(&d, g, i / levels, l, blk);
                if (tex_decode_rgba8(nvn, blk, (int)w, (int)h, rgba[i])) ok = 0;
                else punched += punch_clear_texels(rgba[i], w * h);
            }
            free(blk);
        }
        if (!ok || ((d.width > 512 || d.height > 512) && !punched)) {
            for (u32 i = 0; i < levels * layers; i++) free(rgba[i]);
            free(rgba);
            rgba = NULL;
        } else {
            format = VK_FORMAT_R8G8B8A8_SRGB;
            map = (VkComponentMapping){VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
            vk_stat_cpu_decoded++;
        }
    }
    if (want1d) cube = 0;
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
                             is3d ? VK_IMAGE_TYPE_3D : want1d ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D, format,
                             {d.width, want1d ? 1 : d.height, is3d ? d.depth : 1}, levels, layers,
                             VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    /* the texels, in the frame's upload memory: level after level of each layer (whole blocks a row) */
    u32 nregions = is3d ? 1 : levels * layers;
    VkBufferImageCopy *regions = calloc(nregions, sizeof *regions);
    VkDeviceSize total = 0;
    for (u32 i = 0; i < nregions; i++) {
        u32 l = i % levels;
        u32 w = (d.width >> l) ? d.width >> l : 1, h = (d.height >> l) ? d.height >> l : 1;
        u64 bytes = is3d ? texture_3d_size(&d) : rgba ? (u64)w * h * 4 : (u64)((w + d.bw - 1) / d.bw) * ((h + d.bh - 1) / d.bh) * d.bytes;
        regions[i].bufferOffset = total;
        regions[i].imageSubresource = (VkImageSubresourceLayers){aspect, l, is3d ? 0 : i / levels, 1};
        regions[i].imageExtent = (VkExtent3D){w, want1d ? 1 : h, is3d ? d.depth : 1};   /* (1D: the first row) */
        total += (bytes + 64 + 15) & ~15ull;   /* (texture_read may write a little past the end) */
    }
    VkBuffer buf;
    VkDeviceSize off;
    u8 *data = regions && total < (1ull << 31) ? arena(total, 16, &buf, &off) : NULL;
    if (data) t->image = make_image(&ici, &t->alloc);
    if (!data || !t->image) {
        for (u32 i = 0; rgba && i < nregions; i++) free(rgba[i]);
        free(rgba);
        free(regions);
        t->image = VK_NULL_HANDLE;
        t->guest = 0;
        warn_texture(&d);
        return VK_NULL_HANDLE;
    }
    for (u32 i = 0; i < nregions; i++) {
        if (is3d) texture_read_3d(&d, g, data + regions[i].bufferOffset);
        else if (rgba) { memcpy(data + regions[i].bufferOffset, rgba[i], (size_t)regions[i].imageExtent.width * regions[i].imageExtent.height * 4); free(rgba[i]); }
        else texture_read(&d, g, i / levels, i % levels, data + regions[i].bufferOffset);
        if (depth24) {   /* 24-bit depth words (stencil above) to floats */
            u32 *wd = (u32 *)(data + regions[i].bufferOffset);
            for (u64 j = 0, nw = (u64)regions[i].imageExtent.width * regions[i].imageExtent.height; j < nw; j++) {
                float f = (float)(wd[j] & 0xFFFFFF) / 16777215.0f;
                memcpy(&wd[j], &f, 4);
            }
        }
        regions[i].bufferOffset += off;
    }
    free(rgba);
    vk_stat_upload_bytes += total;
    VkCommandBuffer c = outside();
    image_layout(c, t->image, aspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdCopyBufferToImage(c, buf, t->image, VK_IMAGE_LAYOUT_GENERAL, nregions, regions);
    sync_all(c);
    free(regions);
    t->guest = g; t->size = size; t->watched = watch; t->stale = 0; t->asked = gfx_stat_presented;
    memcpy(t->tic, tic, 32); t->sample_hash = sh; t->used = ++tick;
    t->is1d = want1d; t->aspect = aspect;
    if (depth24) vk_stat_depth_textures++;
    if (want1d) vk_stat_textures_1d++;
    t->is3d = is3d; t->cube = cube; t->levels = levels; t->layers = layers; t->format = format; t->map = map;
    vk_stat_textures++;
    tex_gen++;
    got_texture = t;
    return texture_view(t, shader_type);
}

/* get_texture's answers within a submission, by header and the program's texture type: a frame's draws ask for
 * the same few hundred textures thousands of times, and each answer decoded the header, looked through the
 * render targets, sized the texels and hashed a sample of them. Within one submission nothing but the GPU's own
 * commands writes the memory its draws read (upload() relies on it too), so a texture of the cache found there
 * stays the answer while no texture or render target is made or let go (tex_gen) and no command wrote its bytes
 * (writes_over). Render targets are not kept here: which one an address names follows their use. */
#define TEX_MEMO 4096
typedef struct TexMemo { u32 tic[8], type; u64 batch, gen, wgen; Texture *t; VkImageView view; } TexMemo;
static TexMemo tex_memo[TEX_MEMO];
static u64 vk_stat_tex_memo;
static VkImageView texture_of(const u32 *tic, u32 type, float *scale, Surface **rt_out) {
    u64 h = 1469598103934665603ull ^ type;
    for (u32 i = 0; i < 8; i++) h = (h ^ tic[i]) * 0x100000001B3ull;
    TexMemo *m = &tex_memo[(h >> 40) & (TEX_MEMO - 1)];
    if (!memo_on()) return get_texture(tic, type, scale, rt_out);
    if (m->batch == gpu_stat_batches && m->gen == tex_gen && m->type == type && !memcmp(m->tic, tic, 32) && m->t->image &&
        m->wgen == writes_over(m->t->guest, m->t->size)) {
        *scale = 1.0f;
        *rt_out = NULL;
        m->t->used = ++tick;
        vk_stat_tex_memo++;
        if (memo_on() == 2) {
            vk_stat_memo_checked++;
            VkImageView v = get_texture(tic, type, scale, rt_out);
            if (v != m->view || *rt_out || *scale != 1.0f) vk_stat_memo_wrong[2]++;
            return v;
        }
        return m->view;
    }
    VkImageView v = get_texture(tic, type, scale, rt_out);
    if (v && got_texture && !*rt_out && got_texture->size <= (4u << 20)) {   /* (a bigger one: its sample hashed instead, as before) */
        memcpy(m->tic, tic, 32);
        m->type = type; m->batch = gpu_stat_batches; m->gen = tex_gen; m->t = got_texture; m->view = v;
        m->wgen = writes_over(got_texture->guest, got_texture->size);
    } else m->batch = 0;
    return v;
}

/* ---- samplers, by their 32-byte descriptors (TSC), as gpu_d3d11.c's get_sampler ----------------------- */
#define SAMPLER_SLOTS 4096
static struct { u32 tsc[8]; VkSampler sampler; } samplers[SAMPLER_SLOTS];
static u32 nsamplers;
static u32 desc_hash(const void *d, size_t size) {   /* (of whole 32-bit words) */
    const u8 *p = d;
    u64 h = 1469598103934665603ull;
    for (size_t i = 0; i + 4 <= size; i += 4) { u32 w; memcpy(&w, p + i, 4); h = (h ^ w) * 0x100000001B3ull; }
    return (u32)(h >> 32 ^ h);
}
static VkSamplerAddressMode wrap_mode(u32 m) {
    switch (m) {
    case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 3: case 6: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 5: case 7: return have_mirror_clamp ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
}
static VkSampler get_sampler(const u32 *tsc) {
    u32 k = desc_hash(tsc, 32) & (SAMPLER_SLOTS - 1);
    for (u32 n = 0; n < SAMPLER_SLOTS && samplers[k].sampler; n++, k = (k + 1) & (SAMPLER_SLOTS - 1))
        if (!memcmp(samplers[k].tsc, tsc, 32)) return samplers[k].sampler;
    u32 mag = tsc[1] & 3, min = (tsc[1] >> 4) & 3, mip = (tsc[1] >> 6) & 3, aniso = (tsc[0] >> 20) & 7, cmp = (tsc[0] >> 9) & 1;
    VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = mag == 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.minFilter = min == 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = mip == 3 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if (aniso && enabled.samplerAnisotropy) {   /* (Direct3D's anisotropic filter: linear in every way) */
        ci.anisotropyEnable = VK_TRUE;
        ci.maxAnisotropy = (float)(1u << aniso);
        if (ci.maxAnisotropy > limits.maxSamplerAnisotropy) ci.maxAnisotropy = limits.maxSamplerAnisotropy;
        ci.magFilter = ci.minFilter = VK_FILTER_LINEAR;
        ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    }
    ci.addressModeU = wrap_mode(tsc[0] & 7);
    ci.addressModeV = wrap_mode((tsc[0] >> 3) & 7);
    ci.addressModeW = wrap_mode((tsc[0] >> 6) & 7);
    ci.compareEnable = cmp ? VK_TRUE : VK_FALSE;
    ci.compareOp = (VkCompareOp)((tsc[0] >> 10) & 7);   /* (Direct3D's function less one) */
    i32 bias = (i32)(tsc[1] << 7) >> 19;
    ci.mipLodBias = bias / 256.0f;
    if (ci.mipLodBias > limits.maxSamplerLodBias) ci.mipLodBias = limits.maxSamplerLodBias;
    if (ci.mipLodBias < -limits.maxSamplerLodBias) ci.mipLodBias = -limits.maxSamplerLodBias;
    ci.minLod = (tsc[2] & 0xFFF) / 256.0f;
    ci.maxLod = mip == 1 ? 0.0f : ((tsc[2] >> 12) & 0xFFF) / 256.0f;
    if (ci.maxLod < ci.minLod) ci.maxLod = ci.minLod;
    /* the border: Vulkan's three fixed colours, the nearest */
    float b[4];
    memcpy(b, &tsc[4], 16);
    ci.borderColor = b[3] < 0.5f ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
                   : b[0] + b[1] + b[2] >= 1.5f ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    VkSampler s;
    if (vkCreateSampler(device, &ci, NULL, &s) != VK_SUCCESS) return default_sampler;
    if (nsamplers < SAMPLER_SLOTS * 3 / 4) {
        memcpy(samplers[k].tsc, tsc, 32);
        samplers[k].sampler = s;
        nsamplers++;
    }
    return s;
}

/* ---- fixed-function state, as gpu_d3d11.c translates it -------------------------------------------- */
static u32 compare_func(u32 f) {   /* Direct3D's (NEVER 1 .. ALWAYS 8) */
    if (f >= 0x200 && f <= 0x207) return f - 0x200 + 1;
    if (f >= 1 && f <= 8) return f;
    return 8;
}
static VkStencilOp stencil_op(u32 op) {
    switch (op) {
    case 1: case 0x1E00: return VK_STENCIL_OP_KEEP;
    case 2: case 0: return VK_STENCIL_OP_ZERO;
    case 3: case 0x1E01: return VK_STENCIL_OP_REPLACE;
    case 4: case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 5: case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 6: case 0x150A: return VK_STENCIL_OP_INVERT;
    case 7: case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 8: case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    }
    return VK_STENCIL_OP_KEEP;
}
static VkBlendFactor blend_factor(u32 f) {
    switch (f) {
    case 0x1: case 0x4000: return VK_BLEND_FACTOR_ZERO;
    case 0x2: case 0x4001: return VK_BLEND_FACTOR_ONE;
    case 0x3: case 0x4300: return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x4: case 0x4301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x5: case 0x4302: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x6: case 0x4303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x7: case 0x4304: return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x8: case 0x4305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x9: case 0x4306: return VK_BLEND_FACTOR_DST_COLOR;
    case 0xA: case 0x4307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0xB: case 0x4308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0xE: case 0xC001: case 0xC003: return VK_BLEND_FACTOR_CONSTANT_COLOR;   /* (Direct3D's blend factor: all four) */
    case 0xF: case 0xC002: case 0xC004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x10: case 0xC900: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 0x11: case 0xC901: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 0x12: case 0xC902: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 0x13: case 0xC903: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    }
    return VK_BLEND_FACTOR_ONE;
}
static VkBlendFactor alpha_factor(VkBlendFactor b) {   /* (as Direct3D has them for alpha: the same results) */
    switch (b) {
    case VK_BLEND_FACTOR_SRC_COLOR: return VK_BLEND_FACTOR_SRC_ALPHA;
    case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case VK_BLEND_FACTOR_DST_COLOR: return VK_BLEND_FACTOR_DST_ALPHA;
    case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case VK_BLEND_FACTOR_SRC1_COLOR: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    default: return b;
    }
}
static int dual_source(VkBlendFactor b) { return b >= VK_BLEND_FACTOR_SRC1_COLOR && b <= VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA; }
static VkBlendOp blend_op(u32 op) {
    switch (op) {
    case 2: case 0x800A: return VK_BLEND_OP_SUBTRACT;
    case 3: case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: case 0x8007: return VK_BLEND_OP_MIN;
    case 5: case 0x8008: return VK_BLEND_OP_MAX;
    }
    return VK_BLEND_OP_ADD;
}
/* Maxwell topologies Direct3D 11 draws (gpu_d3d11.c's topology: the same ones), -1 for the rest. */
static int topology(u32 t, int *restart) {
    *restart = 0;
    switch (t) {
    case 0: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case 1: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case 3: *restart = 1; return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case 4: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case 5: *restart = 1; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case 10: return enabled.geometryShader ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY : -1;
    case 11: *restart = 1; return enabled.geometryShader ? VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY : -1;
    case 12: return enabled.geometryShader ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY : -1;
    case 13: *restart = 1; return enabled.geometryShader ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY : -1;
    }
    return -1;
}

/* ---- pipelines, by shaders and state ----------------------------------------------------------------- */
typedef struct PipeKey {
    VkShaderModule vs, ps;
    u32 topo, restart, fill, cull, front;
    u32 depth_test, depth_write, depth_func, stencil_test;
    u32 sfront[4], sback[4];   /* fail, depth fail, pass, function */
    u32 ncolor;
    VkFormat color[8], depth;
    u32 blend[8][8];           /* enable, colour op, src, dst, alpha op, src, dst; write mask */
} PipeKey;
#define PIPE_SLOTS 32768
typedef struct Pipe {
    Job job;
    PipeKey key;
    VkPipeline pipe;
    volatile LONG state;    /* (as a shader's) */
    WarmKey wvs, wps;       /* its programs, for the warm list (listable) */
    u8 listable, warm;      /* made by a draw from glslang's programs: listed once made; made by the warm start */
    u8 waited, used;        /* (the drawing thread's: it was waited for once; a warm one was drawn with) */
} Pipe;
static Pipe *pipes[PIPE_SLOTS];   /* (pipes_lock: the drawing thread and the warm threads put pipelines in) */
static SRWLOCK pipes_lock = SRWLOCK_INIT;
static u32 npipes;
static void warm_list_pipe(const Pipe *e);
static u64 vk_stat_pipeline_failures, vk_stat_skip_pipeline;
static volatile LONG64 pipeline_ns;   /* (the makers' time making pipelines) */

static void pipe_make(Pipe *e) {
    const PipeKey *k = &e->key;
    VkPipelineShaderStageCreateInfo st[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, k->vs, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, k->ps, "main", NULL}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};   /* (vertex pulling: none) */
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, (VkPrimitiveTopology)k->topo,
                                                 k->restart ? VK_TRUE : VK_FALSE};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = k->fill ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.cullMode = k->cull;
    rs.frontFace = k->front ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = k->depth_test;
    ds.depthWriteEnable = k->depth_write;
    ds.depthCompareOp = (VkCompareOp)(k->depth_func - 1);
    ds.stencilTestEnable = k->stencil_test;
    ds.front = (VkStencilOpState){(VkStencilOp)k->sfront[0], (VkStencilOp)k->sfront[2], (VkStencilOp)k->sfront[1], (VkCompareOp)(k->sfront[3] - 1), 0xFF, 0xFF, 0};
    ds.back = (VkStencilOpState){(VkStencilOp)k->sback[0], (VkStencilOp)k->sback[2], (VkStencilOp)k->sback[1], (VkCompareOp)(k->sback[3] - 1), 0xFF, 0xFF, 0};
    ds.maxDepthBounds = 1.0f;
    VkPipelineColorBlendAttachmentState at[8];
    memset(at, 0, sizeof at);
    for (u32 i = 0; i < k->ncolor; i++) {
        const u32 *b = k->blend[i];
        at[i] = (VkPipelineColorBlendAttachmentState){b[0], (VkBlendFactor)b[2], (VkBlendFactor)b[3], (VkBlendOp)b[1],
                                                      (VkBlendFactor)b[5], (VkBlendFactor)b[6], (VkBlendOp)b[4], b[7]};
    }
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_LOGIC_OP_COPY, k->ncolor, at};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                            VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, sizeof dyn / sizeof *dyn, dyn};
    VkPipelineRenderingCreateInfoKHR ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR, NULL, 0, k->ncolor, k->color, k->depth,
                                           has_stencil(k->depth) ? k->depth : VK_FORMAT_UNDEFINED};
    VkGraphicsPipelineCreateInfo pci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri, 0, k->ps ? 2u : 1u, st, &vi, &ia, NULL, &vp, &rs, &ms,
                                        &ds, &cb, &dy, gfx_layout, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, -1};
    VkPipeline p;
    u64 t0 = rt_clock_ns();
    VkResult r = vkCreateGraphicsPipelines(device, pipeline_cache, 1, &pci, NULL, &p);
    InterlockedAdd64(&pipeline_ns, (LONG64)(rt_clock_ns() - t0));
    if (r != VK_SUCCESS) {
        static volatile LONG told;
        if (InterlockedIncrement(&told) <= 20) fprintf(stderr, "[vulkan] cannot make a pipeline (%d)\n", (int)r);
        InterlockedIncrement64((volatile LONG64 *)&vk_stat_pipeline_failures);
        InterlockedExchange(&e->state, 2);
        return;
    }
    e->pipe = p;
    InterlockedIncrement64(e->warm ? &vk_stat_warm_pipelines : (volatile LONG64 *)&vk_stat_pipelines);
    InterlockedExchange(&e->state, 1);
    if (e->listable) warm_list_pipe(e);
}
static void pipe_job(Job *j) { pipe_make((Pipe *)j); }

/* The pipeline for these shaders and this state, or NULL while a maker makes it (or if it cannot be made). */
static u64 vk_stat_warm_used;
static int warm_list_on(void);
static VkPipeline get_pipeline(const PipeKey *k, const VShader *vs, const VShader *ps) {
    u32 h = desc_hash(k, sizeof *k) & (PIPE_SLOTS - 1);
    Pipe *e = NULL;
    int made = 0;
    AcquireSRWLockExclusive(&pipes_lock);
    for (u32 n = 0; n < PIPE_SLOTS && pipes[h]; n++, h = (h + 1) & (PIPE_SLOTS - 1))
        if (!memcmp(&pipes[h]->key, k, sizeof *k)) { e = pipes[h]; break; }
    if (!e && npipes < PIPE_SLOTS * 3 / 4 && (e = calloc(1, sizeof *e)) != NULL) {   /* (full: 24,576 pipelines) */
        e->key = *k;
        e->job.run = pipe_job;
        e->waited = 1;
        if (vs->glslang && (!ps || ps->glslang) && warm_list_on()) {
            e->listable = 1;
            e->wvs = (WarmKey){vs->hash, vs->table_hash, vs->vs_outputs, SHADER_VERTEX};
            e->wps = ps ? (WarmKey){ps->hash, ps->table_hash, ps->vs_outputs, SHADER_PIXEL} : (WarmKey){0, 0, 0, ~0u};
        }
        pipes[h] = e;
        npipes++;
        made = 1;
    }
    ReleaseSRWLockExclusive(&pipes_lock);
    if (!e) return VK_NULL_HANDLE;
    if (made) job_add(&e->job);
    else if (e->warm && !e->used) { e->used = 1; vk_stat_warm_used++; }
    if (e->state == 0 && wait_makes()) wait_for(&e->state);
    /* a new pipeline is waited for a little (its shaders are ready: the driver makes it in milliseconds, less with the
     * pipeline cache warm), rather than its first draws skipped - an effect missing the first time (2026-11-09); so is
     * one a warm thread is making now, once */
    if (e->state == 0 && (made || !e->waited)) {
        e->waited = 1;
        for (u64 until = GetTickCount64() + 100; e->state == 0 && GetTickCount64() < until;) SwitchToThread();
    }
    return e->state == 1 ? e->pipe : VK_NULL_HANDLE;
}

/* ---- the warm list ------------------------------------------------------------------------------------ */
/* "<shader cache>/vulkan_<vendor>_<device>.warm": a header, then a record for each pipeline the game drew
 * with whose programs are glslang's, and each of its compute programs, in the order of their first use.
 * It is for the SPIR-V files of one translator and one ssbu_glslang.dll (the header says which: another
 * starts the list again, as it makes its SPIR-V again). Records are added as pipelines are made, each
 * with a checksum: a torn or doubled one is left out when the list is read, and the file written again
 * without it. The list holds WARM_RECORDS at most; SSBU_VK_WARM=0 makes nothing from it at the start
 * (it is still kept). */
#define WARM_LIST_VERSION 1   /* (bump when PipeKey's fields change their meaning) */
#define WARM_RECORDS 16384
typedef struct WarmHeader { char magic[8]; u32 record_size, key_size, spv_version, library, list_version, zero; } WarmHeader;
typedef struct WarmRecord { u32 kind, sum; WarmKey vs, ps; PipeKey key; } WarmRecord;   /* kind 1: a pipeline (key's modules zero); 2: a compute program (vs) */
static SRWLOCK warm_list_lock = SRWLOCK_INIT;
static FILE *warm_file;
static u64 warm_seen[WARM_RECORDS * 4];   /* (the records listed, by their hashes: none twice) */
static u32 warm_nseen;
static WarmRecord *warm_records;          /* (the list as read at the start) */
static u32 warm_nrecords;
static int warm_listing = -1;

static int warm_list_on(void) { return warm_listing > 0; }

/* (1: the record is new, and now seen; warm_list_lock held, or no other thread yet) */
static int warm_see(const WarmRecord *r) {
    const u8 *p = (const u8 *)r;
    u64 h = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof *r; i++) h = (h ^ p[i]) * 1099511628211ull;
    if (!h) h = 1;
    u32 slots = sizeof warm_seen / sizeof *warm_seen;
    for (u32 at = (u32)(h >> 20) & (slots - 1);; at = (at + 1) & (slots - 1)) {
        if (warm_seen[at] == h) return 0;
        if (!warm_seen[at]) {
            if (warm_nseen >= WARM_RECORDS) return 0;   /* (the list is full) */
            warm_seen[at] = h;
            warm_nseen++;
            return 1;
        }
    }
}

static void warm_list_add(WarmRecord *r) {
    r->sum = desc_hash(&r->vs, sizeof *r - offsetof(WarmRecord, vs));
    AcquireSRWLockExclusive(&warm_list_lock);
    if (warm_file && warm_see(r)) {
        fwrite(r, sizeof *r, 1, warm_file);
        fflush(warm_file);
    }
    ReleaseSRWLockExclusive(&warm_list_lock);
}
static void warm_list_pipe(const Pipe *e) {
    WarmRecord r;
    memset(&r, 0, sizeof r);
    r.kind = 1;
    r.vs = e->wvs;
    r.ps = e->wps;
    memcpy(&r.key, &e->key, sizeof r.key);
    r.key.vs = r.key.ps = VK_NULL_HANDLE;
    warm_list_add(&r);
}
static void warm_list_compute(const WarmKey *k) {
    if (!warm_list_on()) return;
    WarmRecord r;
    memset(&r, 0, sizeof r);
    r.kind = 2;
    r.vs = *k;
    r.ps.stage = ~0u;
    warm_list_add(&r);
}

static int warm_record_ok(const WarmRecord *r) {
    if (r->sum != desc_hash(&r->vs, sizeof *r - offsetof(WarmRecord, vs))) return 0;
    if (r->kind == 2) return r->vs.stage == SHADER_COMPUTE;
    return r->kind == 1 && r->vs.stage == SHADER_VERTEX && (r->ps.stage == SHADER_PIXEL || r->ps.stage == ~0u) && r->key.ncolor <= 8;
}

static void warm_shader_put(const WarmKey *k) {
    for (u32 h = desc_hash(k, sizeof *k) & (warm_shader_slots - 1);; h = (h + 1) & (warm_shader_slots - 1)) {
        WarmShader *w = &warm_shaders[h];
        if (w->used && !memcmp(&w->k, k, sizeof *k)) return;
        if (!w->used) { w->k = *k; w->used = 1; return; }
    }
}

/* The list read (its good records, once each: warm_records, and the table of their programs), the file put
 * right if it was not, and opened for more. */
static void warm_list_open(void) {
    warm_listing = 0;
    if (!use_glslang()) return;
    char path[1100];
    snprintf(path, sizeof path, "%s/vulkan_%04x_%04x.warm", cache_dir(), limits_vendor, limits_device);
    WarmHeader head = {"OSVKWRM", sizeof(WarmRecord), sizeof(PipeKey), SHADER_CACHE_SPV_VERSION, spirv_glslang_version(), WARM_LIST_VERSION, 0};
    size_t size = 0;
    u8 *data = read_file(path, &size);
    int rewrite = !data;
    u32 n = 0;
    if (data && (size < sizeof head || memcmp(data, &head, sizeof head))) { size = 0; rewrite = 1; }
    else if (data) {
        u32 listed = (u32)((size - sizeof head) / sizeof(WarmRecord));
        rewrite = (size - sizeof head) % sizeof(WarmRecord) != 0;
        WarmRecord *rec = (WarmRecord *)(void *)(data + sizeof head);   /* (malloc's alignment, and a header of 32 bytes) */
        for (u32 i = 0; i < listed; i++)
            if (warm_record_ok(&rec[i]) && warm_see(&rec[i])) rec[n++] = rec[i];
            else rewrite = 1;
        warm_records = rec;
        warm_nrecords = n;
    }
    if (rewrite) {
        size_t bytes = sizeof head + (size_t)n * sizeof(WarmRecord);
        u8 *out = malloc(bytes);
        if (out) {
            memcpy(out, &head, sizeof head);
            if (n) memcpy(out + sizeof head, warm_records, (size_t)n * sizeof(WarmRecord));
            write_file(path, out, bytes);
            free(out);
        }
    }
    if (!n) { free(data); warm_records = NULL; }
    warm_file = fopen(path, "ab");
    warm_listing = warm_file != NULL;
    if (!n) return;
    /* the programs the records name */
    u32 slots = 64;
    while (slots < n * 4) slots *= 2;   /* (two programs a record at most) */
    warm_shaders = calloc(slots, sizeof *warm_shaders);
    if (!warm_shaders) { warm_nrecords = 0; return; }
    warm_shader_slots = slots;
    for (u32 i = 0; i < n; i++) {
        warm_shader_put(&warm_records[i].vs);
        if (warm_records[i].ps.stage != ~0u) warm_shader_put(&warm_records[i].ps);
    }
}

/* ---- the warm start: the list's pipelines made before the game asks for them ------------------------------- */
/* The warm threads take the records in turn (the order the game first used them in): each loads its programs'
 * SPIR-V files into the warm table (a program once), makes a compute program's pipeline with it, and makes the
 * record's pipeline, put among the game's (pipes) for the draw that asks for it. The driver has most of them
 * in its pipeline cache; what it has not, it compiles here instead of during play. Fewer and slower threads
 * than the makers, which the game waits for. Each pipeline holds driver memory in this process: the warm
 * start stops once the process has grown by SSBU_SHADER_PREWARM_MB (Direct3D 11's prewarm budget: a third of
 * the PC's memory by default; 0: no limit) - the rest are made at first use, as before. */
static volatile LONG warm_at, warm_running, warm_stopped, warm_left;
static u64 warm_budget, warm_bytes0, warm_t0;

static u64 private_bytes(void) {   /* (as gpu_d3d11.c's) */
    typedef struct { DWORD cb, faults; SIZE_T peak_ws, ws, a, b, c, d, pagefile, peak_pagefile, private_usage; } Counters;
    static BOOL (WINAPI *get)(HANDLE, Counters *, DWORD);
    if (!get) get = (BOOL (WINAPI *)(HANDLE, Counters *, DWORD))(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32GetProcessMemoryInfo");
    Counters c = {sizeof c};
    return get && get(GetCurrentProcess(), &c, sizeof c) ? c.private_usage : 0;
}

static void warm_load(WarmShader *w) {
    char path[1200];
    shader_cache_file_spv(path, sizeof path, cache_dir(), (int)w->k.stage, w->k.hash, w->k.vs_outputs, w->k.table_hash, spirv_glslang_version());
    size_t size;
    void *code = read_file(path, &size);
    int made = 0;
    if (code && size >= 20 && size % 4 == 0 && *(const u32 *)code == 0x07230203) {
        VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, size, code};
        VkShaderModule module;
        made = vkCreateShaderModule(device, &ci, NULL, &module) == VK_SUCCESS;
        if (made && w->k.stage == SHADER_COMPUTE) {
            VkComputePipelineCreateInfo cpi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
                                               {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", NULL},
                                               cs_layout, VK_NULL_HANDLE, -1};
            made = vkCreateComputePipelines(device, pipeline_cache, 1, &cpi, NULL, &w->compute) == VK_SUCCESS;
            if (!made) { vkDestroyShaderModule(device, module, NULL); w->compute = VK_NULL_HANDLE; }
        }
        if (made) w->module = module;
    }
    free(code);
    if (made) InterlockedIncrement64(&vk_stat_warm_loaded);
    InterlockedExchange(&w->state, made ? WARM_READY : WARM_NONE);   /* (the module written before: a full barrier) */
}

/* The record's program, loaded: by this thread, by another, or by the game (waited for, a little). */
static WarmShader *warm_ready(const WarmKey *k) {
    WarmShader *w = warm_find(k);
    if (!w) return NULL;
    if (InterlockedCompareExchange(&w->state, WARM_LOADING, WARM_LISTED) == WARM_LISTED) warm_load(w);
    for (u64 until = GetTickCount64() + 2000; (w->state == WARM_LOADING || w->state == WARM_GAME) && GetTickCount64() < until;) Sleep(1);
    return w->state == WARM_READY ? w : NULL;
}

/* "<list>.warming": there while the warm threads work. One found at a start means the last warm start did not
 * finish - the driver crashed on a record, or the game was closed in those seconds - so the list is started
 * again rather than given to the driver once more: one start without warmed pipelines, not a crash at each. */
static char warm_marker[1200];

static DWORD WINAPI warm_main(void *unused) {
    (void)unused;
    for (;;) {
        LONG i = InterlockedIncrement(&warm_at) - 1;
        if (i < 0 || (u32)i >= warm_nrecords) break;
        if (!warm_stopped && warm_budget && (i & 15) == 0 && private_bytes() > warm_bytes0 + warm_budget) InterlockedExchange(&warm_stopped, 1);
        if (warm_stopped) { InterlockedIncrement(&warm_left); continue; }
        const WarmRecord *r = &warm_records[i];
        WarmShader *vs = warm_ready(&r->vs), *ps = r->ps.stage != ~0u ? warm_ready(&r->ps) : NULL;
        if (r->kind != 1) { if (!vs) InterlockedIncrement(&warm_left); continue; }
        if (!vs || (r->ps.stage != ~0u && !ps)) { InterlockedIncrement(&warm_left); continue; }
        PipeKey key;
        memcpy(&key, &r->key, sizeof key);
        key.vs = vs->module;
        key.ps = ps ? ps->module : VK_NULL_HANDLE;
        u32 h = desc_hash(&key, sizeof key) & (PIPE_SLOTS - 1);
        Pipe *e = calloc(1, sizeof *e);   /* (made before the lock the draws take: this thread runs below them) */
        if (!e) { InterlockedIncrement(&warm_left); continue; }
        e->key = key;
        e->warm = 1;
        int found = 0, put = 0;
        AcquireSRWLockExclusive(&pipes_lock);
        for (u32 n = 0; n < PIPE_SLOTS && pipes[h] && !found; n++, h = (h + 1) & (PIPE_SLOTS - 1))
            found = !memcmp(&pipes[h]->key, &key, sizeof key);
        if (!found && npipes < PIPE_SLOTS / 2) {   /* (room left for the game's own) */
            pipes[h] = e;
            npipes++;
            put = 1;
        }
        ReleaseSRWLockExclusive(&pipes_lock);
        if (put) pipe_make(e);
        else {
            free(e);
            if (!found) InterlockedIncrement(&warm_left);
        }
    }
    if (!InterlockedDecrement(&warm_running)) {
        DeleteFileA(warm_marker);
        fprintf(stderr, "[vulkan] warm start: %lld programs loaded and %lld pipelines made of %u listed in %.1f s (%ld left out%s; at %.1f s)\n",
                (long long)vk_stat_warm_loaded, (long long)vk_stat_warm_pipelines, warm_nrecords, (rt_clock_ns() - warm_t0) / 1e9, (long)warm_left,
                warm_stopped ? ": the memory budget, SSBU_SHADER_PREWARM_MB" : "", rt_now_ns() / 1e9);
    }
    return 0;
}

/* The start's last step (the makers run): the list opened, and the warm threads started on it. */
static void warm_start(void) {
    char list[1100];
    int again = 0;
    snprintf(list, sizeof list, "%s/vulkan_%04x_%04x.warm", cache_dir(), limits_vendor, limits_device);
    snprintf(warm_marker, sizeof warm_marker, "%s.warming", list);
    if (GetFileAttributesA(warm_marker) != INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "[vulkan] warm start: the last one did not finish (%s): the list is started again, nothing is warmed this time\n", warm_marker);
        /* (A list that can't be removed - read-only, held open - keeps its marker, and no start warms from it.) */
        if (DeleteFileA(list) || GetLastError() == ERROR_FILE_NOT_FOUND) DeleteFileA(warm_marker);
        else again = 1;
    }
    warm_list_open();
    if (!warm_nrecords) return;
    if (again) {
        fprintf(stderr, "[vulkan] warm start: the list could not be removed (%s), %u records left unused
", list, warm_nrecords);
        warm_shader_slots = 0;
        return;
    }
    if (getenv("SSBU_VK_WARM") && !atoi(getenv("SSBU_VK_WARM"))) {
        fprintf(stderr, "[vulkan] warm start: off (SSBU_VK_WARM=0), %u pipelines and programs listed\n", warm_nrecords);
        warm_shader_slots = 0;   /* (no draw looks its programs up) */
        return;
    }
    MEMORYSTATUSEX m = {sizeof m};
    warm_budget = 1024ull << 20;
    if (GlobalMemoryStatusEx(&m) && m.ullTotalPhys / 3 > warm_budget) warm_budget = m.ullTotalPhys / 3;
    if (getenv("SSBU_SHADER_PREWARM_MB")) warm_budget = strtoull(getenv("SSBU_SHADER_PREWARM_MB"), NULL, 10) << 20;
    warm_bytes0 = private_bytes();
    warm_t0 = rt_clock_ns();
    u32 n = nmakers / 2;
    if (n < 1) n = 1;
    if (n > 3) n = 3;
    InterlockedExchange(&warm_running, (LONG)n);
    FILE *mark = fopen(warm_marker, "wb");
    if (mark) fclose(mark);
    else fprintf(stderr, "[vulkan] warm start: no marker file (%s): one that does not finish will not be noticed
", warm_marker);
    for (u32 i = 0; i < n; i++) {
        HANDLE t = CreateThread(NULL, 64ull << 20, warm_main, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        if (!t) {
            if (!InterlockedDecrement(&warm_running)) { warm_shader_slots = 0; DeleteFileA(warm_marker); }   /* (no thread at all: nothing is loaded) */
            continue;
        }
        SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);
        CloseHandle(t);
    }
}

/* ---- the draw -------------------------------------------------------------------------------------- */
static u32 vk_debug(void) {   /* (SSBU_GFX_DEBUG, as gpu_d3d11.c reads it) */
    static int checked;
    static u32 v;
    if (!checked++ && getenv("SSBU_GFX_DEBUG")) v = (u32)strtoul(getenv("SSBU_GFX_DEBUG"), NULL, 0);
    return v;
}
static u32 instance_counter;
static float f32_of(u32 bits);
static u32 f32_bits(float f) { u32 u; memcpy(&u, &f, 4); return u; }

typedef struct StageBinds {
    VkBufferView cb[18], vb[32], gm[16];
    VkImageView tex[32];
    VkSampler smp[32];
    Surface *sampled[32];   /* (render targets among the textures) */
} StageBinds;

/* A stage's constant buffers, storage buffers and textures (gpu_d3d11.c's bind_resources). */
static u64 vk_ns_res[3];   /* (constant buffers, storage buffers, textures) */
/* The parts of a draw are timed in one draw of every TIMING_EVERY (a clock read is some 25 ns, a draw's parts
 * take a dozen: timing every draw cost more than some of the parts), the times scaled up. */
#define TIMING_EVERY 32
static int vk_timing;
static void stage_resources(const u32 *regs, const CbBinding cb[STAGES][18], int stage, const VShader *s, DrawParams *params, StageBinds *b) {
    int mstage = stage == SHADER_VERTEX ? STAGE_VERTEX : STAGE_FRAGMENT;
    u64 t0 = vk_timing ? rt_clock_ns() : 0, t1;
    for (u32 k = 0; k < 18; k++) {
        if (!(s->info.cbuf_mask >> k & 1)) continue;
        u32 size = cb[mstage][k].size;
        if (size > 0x10000) size = 0x10000;
        if (size > s->info.cbuf_extent[k]) size = s->info.cbuf_extent[k];   /* only what the program can read */
        if (!(b->cb[k] = kept_cb(cb[mstage][k].address, size))) b->cb[k] = upload_slot(stage, k, cb[mstage][k].address, size);
    }
    if (vk_timing) { t1 = rt_clock_ns(); vk_ns_res[0] += (t1 - t0) * TIMING_EVERY; t0 = t1; }
    for (u32 k = 0; k < s->info.sb_count && k < 16; k++) {   /* storage buffers: their descriptors are in constant buffer 0 */
        u32 desc[4] = {0};
        if (cb[mstage][0].address) read_gpu(cb[mstage][0].address + s->info.sb_offset[k], 16, desc);
        u64 va = (u64)desc[1] << 32 | desc[0];
        u64 end = (va + desc[2] + 0xFFF) & ~0xFFFull;
        u32 size = desc[2] && end - va <= (16u << 20) ? (u32)(end - va) : desc[2] ? 16u << 20 : 0;
        params->sb[stage][k][0] = desc[0];
        params->sb[stage][k][1] = desc[1];
        params->sb[stage][k][2] = size;
        Resident *res = nresidents && va && size && (va & 3) == 0 ? resident_covering(va, size) : NULL;   /* written by compute: the GPU's copy */
        u32 skip;
        if (res) resident_check(res);
        if (res && (b->gm[k] = resident_texels(res, va, size, &skip))) {   /* (the view may start a little before va) */
            params->sb[stage][k][0] = (u32)(va - skip);
            params->sb[stage][k][1] = (u32)((va - skip) >> 32);
            params->sb[stage][k][2] = size + skip;
            continue;
        }
        b->gm[k] = upload(va, size);
    }
    if (vk_timing) { t1 = rt_clock_ns(); vk_ns_res[1] += (t1 - t0) * TIMING_EVERY; t0 = t1; }
    u32 tex_cb = regs[M_BINDLESS_TEXTURE] & 0x1F;
    u64 tic_pool = (u64)regs[M_TEX_HEADER_POOL] << 32 | regs[M_TEX_HEADER_POOL + 1];
    u64 tsc_pool = (u64)regs[M_TEX_SAMPLER_POOL] << 32 | regs[M_TEX_SAMPLER_POOL + 1];
    for (u32 k = 0; k < s->info.ntex && k < 32; k++) {
        u32 handle = 0, tic[8], tsc[8];
        if (tex_cb < 18 && cb[mstage][tex_cb].address) read_gpu(cb[mstage][tex_cb].address + s->info.tex[k].handle_offset, 4, &handle);
        u32 tic_index = handle & 0xFFFFF, tsc_index = regs[M_SAMPLER_BINDING] & 1 ? tic_index : (handle >> 20) & 0xFFF;
        read_gpu(tic_pool + tic_index * 32ull, 32, tic);
        read_gpu(tsc_pool + tsc_index * 32ull, 32, tsc);
        float scale;
        b->tex[k] = texture_of(tic, s->info.tex[k].type, &scale, &b->sampled[k]);
        /* (Direct3D 11 binds 16 samplers: the rest are its default one) */
        b->smp[k] = k < 16 ? get_sampler(tsc) : s->info.tex[k].shadow ? default_compare : default_sampler;
        params->swz[stage][k] = (tic[0] >> 19) & 0xFFF;
        if (scale != 1.0f) params->swz[stage][k] |= (u32)(scale * 4096.0f + 0.5f) << 16;
    }
    if (vk_timing) vk_ns_res[2] += (rt_clock_ns() - t0) * TIMING_EVERY;
}

/* The stage's descriptor set (binding map: gpu_spirv.h), with what its program declares. */
/* What the frame's command buffer has bound and set (graphics), so a draw sets only what changed; other
 * graphics recording (masked clears, the overlay) forgets it: bound.serial = 0. */
static struct {
    u64 serial;
    int have;
    VkPipeline pipe;
    VkViewport vp;
    VkRect2D scissor;
    u32 blend[4], stencil[6];
    VkBuffer ibuf;
    VkDeviceSize ioff;
    VkIndexType itype;
    VkDescriptorSet sets[2];   /* (the vertex and pixel programs' sets, bound with the parameters at poff) */
    u32 poff;
} bound;

/* Sets already written this frame, by what they hold (the program, the parameters' buffer - its offset is
 * the bind's - and every view, image and sampler): a draw binding the same as an earlier one (a material's
 * draws, the same constants compared equal) takes that set instead of a new one written. */
#define SET_SLOTS 2048
#define SET_KEY (2 + 18 + 32 + 16 + 64)
typedef struct SetSlot { u64 serial; u32 n; VkDescriptorSet set; u64 key[SET_KEY]; } SetSlot;
static SetSlot *set_slots;
typedef union TmplSlot { VkDescriptorBufferInfo b; VkBufferView v; VkDescriptorImageInfo i; } TmplSlot;   /* (an update template's data, one a descriptor) */
static u64 vk_stat_sets_made, vk_stat_sets_kept, vk_stat_index_reused, vk_stat_params_kept, vk_stat_binds_kept;
static VkDescriptorSet stage_set(VShader *s, const StageBinds *b, VkBuffer pbuf) {
    u64 key[SET_KEY];
    u32 nk = 0;
    key[nk++] = (u64)(uintptr_t)s;
    key[nk++] = (u64)(uintptr_t)pbuf;
    for (u32 k = 0; k < 18; k++)
        if (s->info.cbuf_mask >> k & 1) key[nk++] = (u64)(uintptr_t)b->cb[k];
    if (s->stage == SHADER_VERTEX)
        for (u32 k = 0; k < 32; k++)
            if (s->info.inputs >> k & 1) key[nk++] = (u64)(uintptr_t)b->vb[k];
    for (u32 k = 0; k < s->info.sb_count && k < 16; k++) key[nk++] = (u64)(uintptr_t)b->gm[k];
    for (u32 k = 0; k < s->info.ntex && k < 32; k++) { key[nk++] = (u64)(uintptr_t)b->tex[k]; key[nk++] = (u64)(uintptr_t)b->smp[k]; }
    u64 h = 1469598103934665603ull;
    for (u32 i = 0; i < nk; i++) h = (h ^ key[i]) * 0x100000001B3ull;
    if (!set_slots) set_slots = calloc(SET_SLOTS, sizeof *set_slots);
    u64 serial = frames[frame_at].serial;
    SetSlot *slot = set_slots ? &set_slots[(h >> 40) & (SET_SLOTS - 1)] : NULL;
    if (slot && slot->serial == serial && slot->n == nk && !memcmp(slot->key, key, nk * sizeof *key)) {
        vk_stat_sets_kept++;
        return slot->set;
    }
    VkDescriptorSet set = new_set(gfx_set_layout);
    if (!set) return VK_NULL_HANDLE;
    vk_stat_sets_made++;
    if (slot) { slot->serial = serial; slot->n = nk; slot->set = set; memcpy(slot->key, key, nk * sizeof *key); }
    /* (through the program's update template: the same writes in one call the driver need not take apart,
     * measured some 10% of a draw's time in the character select screen's 3,200 draws a frame) */
    if (!s->tmpl && !s->tmpl_failed && memo_on()) {
        VkDescriptorUpdateTemplateEntry e[1 + 18 + 32 + 16 + 64];
        u32 ne = 0;
#define ENTRY(binding, type) (e[ne] = (VkDescriptorUpdateTemplateEntry){binding, 0, 1, type, ne * sizeof(TmplSlot), sizeof(TmplSlot)}, ne++)
        ENTRY(SPIRV_BINDING_CBV(0), VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC);
        for (u32 k = 0; k < 18; k++)
            if (s->info.cbuf_mask >> k & 1) ENTRY(SPIRV_BINDING_SRV(k), VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER);
        if (s->stage == SHADER_VERTEX)
            for (u32 k = 0; k < 32; k++)
                if (s->info.inputs >> k & 1) ENTRY(SPIRV_BINDING_SRV(18 + k), VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER);
        for (u32 k = 0; k < s->info.sb_count && k < 16; k++) ENTRY(SPIRV_BINDING_SRV(96 + k), VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER);
        for (u32 k = 0; k < s->info.ntex && k < 32; k++) {
            ENTRY(SPIRV_BINDING_SRV(64 + k), VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
            ENTRY(SPIRV_BINDING_SAMPLER(k), VK_DESCRIPTOR_TYPE_SAMPLER);
        }
#undef ENTRY
        VkDescriptorUpdateTemplateCreateInfo ti = {VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO, NULL, 0, ne, e,
                                                   VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET, gfx_set_layout, VK_PIPELINE_BIND_POINT_GRAPHICS, VK_NULL_HANDLE, 0};
        if (vkCreateDescriptorUpdateTemplate(device, &ti, NULL, &s->tmpl) != VK_SUCCESS) { s->tmpl = VK_NULL_HANDLE; s->tmpl_failed = 1; }
    }
    if (s->tmpl && memo_on()) {
        TmplSlot d[1 + 18 + 32 + 16 + 64];
        u32 nd = 0;
        d[nd++].b = (VkDescriptorBufferInfo){pbuf, 0, sizeof(DrawParams)};
        for (u32 k = 0; k < 18; k++)
            if (s->info.cbuf_mask >> k & 1) d[nd++].v = b->cb[k] ? b->cb[k] : dummy_texels;
        if (s->stage == SHADER_VERTEX)
            for (u32 k = 0; k < 32; k++)
                if (s->info.inputs >> k & 1) d[nd++].v = b->vb[k] ? b->vb[k] : dummy_texels;
        for (u32 k = 0; k < s->info.sb_count && k < 16; k++) d[nd++].v = b->gm[k] ? b->gm[k] : dummy_texels;
        for (u32 k = 0; k < s->info.ntex && k < 32; k++) {
            VkImageView v = b->tex[k] ? b->tex[k] : dummy_views[s->info.tex[k].type & 7];
            if (!v) v = dummy_views[TEX_2D];
            d[nd++].i = (VkDescriptorImageInfo){VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_GENERAL};
            d[nd++].i = (VkDescriptorImageInfo){b->smp[k] ? b->smp[k] : default_sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        }
        vkUpdateDescriptorSetWithTemplate(device, set, s->tmpl, d);
        return set;
    }
    VkWriteDescriptorSet w[1 + 18 + 32 + 16 + 64];
    VkDescriptorImageInfo ii[64];
    u32 n = 0, ni = 0;
    VkDescriptorBufferInfo params = {pbuf, 0, sizeof(DrawParams)};
    w[n++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, SPIRV_BINDING_CBV(0), 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, NULL, &params, NULL};
    for (u32 k = 0; k < 18; k++)
        if (s->info.cbuf_mask >> k & 1)
            w[n++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, SPIRV_BINDING_SRV(k), 0, 1,
                                            VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, NULL, NULL, b->cb[k] ? &b->cb[k] : &dummy_texels};
    if (s->stage == SHADER_VERTEX)
        for (u32 k = 0; k < 32; k++)
            if (s->info.inputs >> k & 1)
                w[n++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, SPIRV_BINDING_SRV(18 + k), 0, 1,
                                                VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, NULL, NULL, b->vb[k] ? &b->vb[k] : &dummy_texels};
    for (u32 k = 0; k < s->info.sb_count && k < 16; k++)
        w[n++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, SPIRV_BINDING_SRV(96 + k), 0, 1,
                                        VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, NULL, NULL, b->gm[k] ? &b->gm[k] : &dummy_texels};
    for (u32 k = 0; k < s->info.ntex && k < 32; k++) {
        VkImageView v = b->tex[k] ? b->tex[k] : dummy_views[s->info.tex[k].type & 7];
        if (!v) v = dummy_views[TEX_2D];
        ii[ni] = (VkDescriptorImageInfo){VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_GENERAL};
        w[n++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, SPIRV_BINDING_SRV(64 + k), 0, 1,
                                        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &ii[ni++], NULL, NULL};
        ii[ni] = (VkDescriptorImageInfo){b->smp[k] ? b->smp[k] : default_sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        w[n++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, SPIRV_BINDING_SAMPLER(k), 0, 1,
                                        VK_DESCRIPTOR_TYPE_SAMPLER, &ii[ni++], NULL, NULL};
    }
    vkUpdateDescriptorSets(device, n, w, 0, NULL);
    return set;
}

static u64 vk_ns_phase[8];   /* (time in a draw's parts: shaders, targets, state and pipeline, streams, resources, indices, sets, recording) */
#define PHASE(k) do { if (vk_timing) { u64 tn_ = rt_clock_ns(); vk_ns_phase[k] += (tn_ - tp) * TIMING_EVERY; tp = tn_; } } while (0)
static void vk_draw_(const u32 *regs, const CbBinding cb[STAGES][18], int indexed);
static void vk_draw(const u32 *regs, const CbBinding cb[STAGES][18], int indexed) {
    u64 t0 = rt_clock_ns();
    vk_draw_(regs, cb, indexed);
    InterlockedAdd64((volatile LONG64 *)&vk_ns_draw, (LONG64)(rt_clock_ns() - t0));
}
static volatile DWORD draw_thread;   /* (the thread drawing: its run time in the status lines) */
static void vk_draw_(const u32 *regs, const CbBinding cb[STAGES][18], int indexed) {
    enter();
    if (!draw_thread) draw_thread = GetCurrentThreadId();
    vk_timing = (vk_stat_draws & (TIMING_EVERY - 1)) == 0;
    u64 tp = vk_timing ? rt_clock_ns() : 0;
    vk_stat_draws++;
    u32 begin = regs[M_BEGIN];
    if (begin & (1u << 26)) instance_counter++;
    else if (!(begin & (1u << 27))) instance_counter = 0;
    int restart, topo = topology(begin & 0xFFFF, &restart);
    u64 region = (u64)regs[M_PROGRAM_REGION] << 32 | regs[M_PROGRAM_REGION + 1];
    u64 vs_g = gpu_va_to_guest(region + regs[M_PIPELINE(1) + 1]);
    u64 ps_g = regs[M_PIPELINE(5)] & 1 ? gpu_va_to_guest(region + regs[M_PIPELINE(5) + 1]) : 0;
    VShader *vs = vs_g ? find_shader(vs_g, SHADER_VERTEX, 0, cb[STAGE_VERTEX], 0) : NULL;
    /* (the pixel shader asked for once the vertex shader is translated: both are made at the same time) */
    VShader *ps_e = vs && vs->state != 2 && ps_g ? find_shader(ps_g, SHADER_PIXEL, vs->info.outputs, cb[STAGE_FRAGMENT], 0) : NULL;
    /* alpha to coverage on targets of one sample: the alpha test >= 0.5 (gpu_d3d11.c) */
    int a2c = (regs[M_ANTI_ALIAS_ALPHA_CONTROL] & 1) && !(regs[M_ALPHA_TEST] & 1) && !(vk_debug() & 64);
    u32 alpha_func = regs[M_ALPHA_TEST] & 1 && !(vk_debug() & 64) ? compare_func(regs[M_ALPHA_FUNC]) : a2c ? 7 : 8;
    VShader *pa_e = alpha_func != 8 && ps_e && ps_e->state != 2 && (ps_e->info.color_mask & 1)
                  ? find_shader(ps_g, SHADER_PIXEL, vs->info.outputs, cb[STAGE_FRAGMENT], 1) : NULL;
    VShader *ps = pa_e && pa_e->state == 1 ? pa_e : ps_e && ps_e->state == 1 ? ps_e : NULL;
    if (!vs || vs->state != 1 || (ps_g && !ps) || topo < 0) {
        if (topo < 0) vk_stat_skip_topology++; else vk_stat_skip_shader++;
        leave();
        return;
    }

    PHASE(0);
    /* render targets */
    Surface *rts[8] = {0}, *zs = NULL;
    u32 sel = regs[M_CT_SELECT], count = sel & 15, rt_w = 0, rt_h = 0, area_w = 16384, area_h = 16384;
    float rt_sx = 1.0f, rt_sy = 1.0f;
    if (count > 8) count = 8;
    for (u32 i = 0; i < count; i++) {
        u32 ct = (sel >> (4 + 3 * i)) & 7;
        u64 va = (u64)regs[M_CT(ct)] << 32 | regs[M_CT(ct) + 1];
        if (!va || !regs[M_CT(ct) + 4]) continue;
        Surface *sf = surface(gpu_va_to_guest(va), regs[M_CT(ct) + 2], regs[M_CT(ct) + 3], regs[M_CT(ct) + 4], 0);
        if (!sf || !(features_of(sf->view) & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) continue;
        rts[i] = sf;
        if (!rt_w) { rt_w = sf->width; rt_h = sf->height; rt_sx = (float)sf->hw / sf->width; rt_sy = (float)sf->hh / sf->height; }
    }
    if (regs[M_ZT_SELECT] & 1) {
        u64 va = (u64)regs[M_ZT_A] << 32 | regs[M_ZT_A + 1];
        Surface *sf = surface(gpu_va_to_guest(va), regs[M_ZT_SIZE], regs[M_ZT_SIZE + 1], regs[M_ZT_A + 2], 1);
        if (sf && (features_of(sf->storage) & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
            zs = sf;
            if (!rt_w) { rt_w = sf->width; rt_h = sf->height; rt_sx = (float)sf->hw / sf->width; rt_sy = (float)sf->hh / sf->height; }
        }
    }
    if (!rt_w) { vk_stat_skip_target++; leave(); return; }
    for (u32 i = 0; i < count; i++)
        if (rts[i]) { if (rts[i]->hw < area_w) area_w = rts[i]->hw; if (rts[i]->hh < area_h) area_h = rts[i]->hh; }
    if (zs) { if (zs->hw < area_w) area_w = zs->hw; if (zs->hh < area_h) area_h = zs->hh; }

    /* viewport and the clip-space fix-up (as gpu_d3d11.c's; Vulkan's viewport upside down: Direct3D's y) */
    DrawParams params;
    memset(&params, 0, sizeof params);
    float sx = f32_of(regs[M_VIEWPORT_SCALE(0)]), sy = f32_of(regs[M_VIEWPORT_SCALE(0) + 1]), sz = f32_of(regs[M_VIEWPORT_SCALE(0) + 2]);
    float ox = f32_of(regs[M_VIEWPORT_SCALE(0) + 3]), oy = f32_of(regs[M_VIEWPORT_SCALE(0) + 4]), oz = f32_of(regs[M_VIEWPORT_SCALE(0) + 5]);
    int lower_left = regs[M_WINDOW_ORIGIN] & 1;
    if (lower_left) { oy = (float)rt_h - oy; sy = -sy; }
    int minus_one = !(regs[M_VIEWPORT_Z_CLIP] & 1);
    float zmin = minus_one ? oz - sz : oz, zmax = oz + sz;
    if (zmin < 0) zmin = 0;
    if (zmin > 1) zmin = 1;
    if (zmax < 0) zmax = 0;
    if (zmax > 1) zmax = 1;
    float vx = ox - fabsf(sx), vy = oy - fabsf(sy), vw = 2 * fabsf(sx), vh = 2 * fabsf(sy);
    if (vw < 1) { vx = 0; vw = (float)rt_w; }
    if (vh < 1) { vy = 0; vh = (float)rt_h; }
    vx *= rt_sx; vw *= rt_sx;
    vy *= rt_sy; vh *= rt_sy;
    params.fix[0] = sx < 0 ? -1.0f : 1.0f;
    params.fix[1] = sy < 0 ? 1.0f : -1.0f;
    params.fix[2] = minus_one ? 0.5f : 1.0f;
    params.fix[3] = minus_one ? 0.5f : 0.0f;
    params.frag[0] = lower_left ? 1.0f : 0.0f;
    params.frag[1] = (float)rt_h;
    params.base[2] = f32_bits(1.0f / rt_sx);
    params.base[3] = f32_bits(1.0f / rt_sy);
    if (pa_e && ps == pa_e) { params.frag[2] = f32_of(alpha_test_mask(alpha_func)); params.frag[3] = a2c ? 0.5f : f32_of(regs[M_ALPHA_REF]); }
    int flipped = (!lower_left) != ((regs[M_WINDOW_ORIGIN] >> 4) & 1);
    VkRect2D scissor = {{0, 0}, {area_w, area_h}};
    if (regs[M_SCISSOR(0)] & 1) {
        u32 h = regs[M_SCISSOR(0) + 1], v = regs[M_SCISSOR(0) + 2];
        i32 l = (i32)(h & 0xFFFF), t = (i32)(v & 0xFFFF), r = (i32)(h >> 16), bm = (i32)(v >> 16);
        if (lower_left) { i32 tt = (i32)rt_h - bm; bm = (i32)rt_h - t; t = tt; }
        l = (i32)floorf((float)l * rt_sx); t = (i32)floorf((float)t * rt_sy);
        r = (i32)ceilf((float)r * rt_sx); bm = (i32)ceilf((float)bm * rt_sy);
        if (l < 0) l = 0;
        if (t < 0) t = 0;
        if (r > (i32)area_w) r = (i32)area_w;
        if (bm > (i32)area_h) bm = (i32)area_h;
        scissor = (VkRect2D){{l, t}, {r > l ? (u32)(r - l) : 0, bm > t ? (u32)(bm - t) : 0}};
    }

    PHASE(1);
    /* the pipeline's state */
    PipeKey key;
    memset(&key, 0, sizeof key);
    key.vs = vs->module;
    key.ps = ps ? ps->module : VK_NULL_HANDLE;
    key.topo = (u32)topo;
    key.restart = (u32)restart;
    key.fill = regs[M_FILL_MODE] == 0x1B01 && enabled.fillModeNonSolid;
    if ((regs[M_CULL_ENABLE] & 1) && !(vk_debug() & 4)) {
        u32 face = regs[M_CULL_FACE];
        key.cull = face == 0x404 ? VK_CULL_MODE_FRONT_BIT : face == 0x405 ? VK_CULL_MODE_BACK_BIT : 0;
    }
    key.front = (regs[M_FRONT_FACE] == 0x901) != flipped;
    VkFormat zfmt = zs ? zs->storage : VK_FORMAT_UNDEFINED;
    key.depth = zfmt;
    key.depth_test = (regs[M_DEPTH_TEST] & 1) && zs && !(vk_debug() & 2);
    key.depth_write = key.depth_test && (regs[M_DEPTH_WRITE] & 1);
    key.depth_func = compare_func(regs[M_DEPTH_FUNC]);
    key.stencil_test = (regs[M_STENCIL_TEST] & 1) && zs && has_stencil(zfmt) && !(vk_debug() & 2);
    key.sfront[0] = stencil_op(regs[M_STENCIL_OP_FAIL]); key.sfront[1] = stencil_op(regs[M_STENCIL_OP_ZFAIL]);
    key.sfront[2] = stencil_op(regs[M_STENCIL_OP_ZPASS]); key.sfront[3] = compare_func(regs[M_STENCIL_FUNC]);
    memcpy(key.sback, key.sfront, sizeof key.sback);
    if (regs[M_TWO_SIDED_STENCIL] & 1) {
        key.sback[0] = stencil_op(regs[M_BACK_STENCIL_OP_FAIL]); key.sback[1] = stencil_op(regs[M_BACK_STENCIL_OP_ZFAIL]);
        key.sback[2] = stencil_op(regs[M_BACK_STENCIL_OP_ZPASS]); key.sback[3] = compare_func(regs[M_BACK_STENCIL_FUNC]);
    }
    if (!key.depth_test) key.depth_func = 8;
    if (!key.stencil_test) { memset(key.sfront, 0, sizeof key.sfront); memset(key.sback, 0, sizeof key.sback); key.sfront[3] = key.sback[3] = 8; }
    u32 ncolor = 0;
    for (u32 i = 0; i < count; i++) if (rts[i]) ncolor = i + 1;
    key.ncolor = ncolor;
    for (u32 i = 0; i < ncolor; i++) {
        u32 *bl = key.blend[i];
        key.color[i] = rts[i] ? rts[i]->view : VK_FORMAT_UNDEFINED;
        u32 mask = regs[M_CT_WRITE(i)];
        bl[7] = (mask & 1 ? 1 : 0) | (mask & 0x10 ? 2 : 0) | (mask & 0x100 ? 4 : 0) | (mask & 0x1000 ? 8 : 0);
        if (vk_debug() & 1) bl[7] = 15;
        bl[0] = (regs[M_BLEND_ENABLE(i)] & 1) && rts[i] && (features_of(rts[i]->view) & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) && !(vk_debug() & 8);
        if (!rts[i]) bl[7] = 0;
        if (!bl[0]) continue;
        u32 base = regs[M_BLEND_STATE_PER_TARGET] & 1 ? M_BLEND_PER_TARGET(i) + 1 : 0, cop, csrc, cdst, aop, asrc, adst;
        if (base) { cop = regs[base]; csrc = regs[base + 1]; cdst = regs[base + 2]; aop = regs[base + 3]; asrc = regs[base + 4]; adst = regs[base + 5]; }
        else {
            cop = regs[M_BLEND_COLOR_OP]; csrc = regs[M_BLEND_COLOR_SRC]; cdst = regs[M_BLEND_COLOR_DST];
            aop = regs[M_BLEND_ALPHA_OP]; asrc = regs[M_BLEND_ALPHA_SRC]; adst = regs[M_BLEND_ALPHA_DST];
        }
        bl[1] = blend_op(cop); bl[2] = blend_factor(csrc); bl[3] = blend_factor(cdst);
        bl[4] = blend_op(aop); bl[5] = alpha_factor(blend_factor(asrc)); bl[6] = alpha_factor(blend_factor(adst));
        if (!enabled.dualSrcBlend)   /* (no second source: as if it were zero) */
            for (int j = 2; j <= 6; j++)
                if (j != 4 && dual_source((VkBlendFactor)bl[j]))
                    bl[j] = bl[j] == VK_BLEND_FACTOR_SRC1_COLOR || bl[j] == VK_BLEND_FACTOR_SRC1_ALPHA ? VK_BLEND_FACTOR_ZERO : VK_BLEND_FACTOR_ONE;
    }

    /* (draws after one another mostly have the same state: the last one's pipeline without the lookup) */
    static PipeKey last_key;
    static VkPipeline last_pipe;
    VkPipeline pipe = last_pipe && memo_on() && !memcmp(&last_key, &key, sizeof key) ? last_pipe : get_pipeline(&key, vs, ps);
    if (!pipe) { vk_stat_skip_pipeline++; leave(); return; }
    if (pipe != last_pipe) { last_key = key; last_pipe = pipe; }

    PHASE(2);
    /* vertex streams: each attribute the shader reads gets its stream's data (t18 + n) */
    StageBinds vb, pb;
    /* (not cleared, 2.6 KB a draw: each binding a program declares is written below before it is read) */
    VkBufferView stream_view[32] = {0};
    u32 stream_skip[32] = {0};   /* (how far before the stream its view starts: added to its attributes' offsets) */
    for (u32 n = 0; n < 32; n++) {
        if (!(vs->info.inputs >> n & 1)) continue;
        vb.vb[n] = VK_NULL_HANDLE;
        u32 fmt = regs[M_VERTEX_ATTRIBUTE(n)], j = fmt & 0x1F;
        u32 st = regs[M_VERTEX_STREAM(j)];
        u64 start = (u64)regs[M_VERTEX_STREAM(j) + 1] << 32 | regs[M_VERTEX_STREAM(j) + 2];
        u64 limit = (u64)regs[M_VERTEX_STREAM_LIMIT(j)] << 32 | regs[M_VERTEX_STREAM_LIMIT(j) + 1];
        params.attr[n][0] = fmt;
        params.attr[n][1] = st & 0xFFF;
        params.attr[n][2] = regs[M_VERTEX_STREAM_INSTANCED(j)] & 1 ? (regs[M_VERTEX_STREAM(j) + 3] ? regs[M_VERTEX_STREAM(j) + 3] : 1) : 0;
        params.attr[n][3] = (st >> 12) & 1 && start;
        if (!params.attr[n][3]) continue;
        u64 size = limit >= start ? limit - start + 1 : 0;
        if (size > (64u << 20)) size = 64u << 20;
        u32 off = (fmt >> 7) & 0x3FFF;
        if (stream_view[j] && off + stream_skip[j] <= 0x3FFF) {   /* another attribute of the same stream */
            vb.vb[n] = stream_view[j];
            params.attr[n][0] = fmt + (stream_skip[j] << 7);
            continue;
        }
        /* a stream in data the GPU keeps (compute results) is read from there */
        Resident *res = nresidents && (start & 3) == 0 ? resident_covering(start, size) : NULL;
        if (res) {
            u32 skip;
            resident_check(res);
            VkBufferView v = resident_texels(res, start, size, &skip);
            if (v && off + skip <= 0x3FFF) {
                vb.vb[n] = stream_view[j] = v;
                stream_skip[j] = skip;
                params.attr[n][0] = fmt + (skip << 7);
                continue;
            }
        }
        stream_skip[j] = 0;
        /* (NOT a kept buffer, as the big constant buffers are: tried, 174 -> 41 GB uploaded in a run, and Donkey Kong
         * was drawn pale again at the fight's start - stale vertices; copied once a submission instead) */
        vb.vb[n] = stream_view[j] = upload(start, (u32)size);
    }
    params.base[0] = regs[M_GLOBAL_BASE_VERTEX];
    params.base[1] = regs[M_GLOBAL_BASE_INSTANCE] + instance_counter;
    PHASE(3);
    stage_resources(regs, cb, SHADER_VERTEX, vs, &params, &vb);
    if (ps) stage_resources(regs, cb, SHADER_PIXEL, ps, &params, &pb);

    /* a render target this draw also samples reads as nothing (Direct3D unbinds it); one an open rendering
     * wrote reads what it wrote (the rendering ends first) */
    int ends = 0;
    for (int st = 0; st < 2; st++) {
        StageBinds *b = st ? &pb : &vb;
        u32 ntex = st ? (ps ? ps->info.ntex : 0) : vs->info.ntex;
        for (u32 k = 0; k < ntex && k < 32; k++) {
            Surface *s = b->sampled[k];
            if (!s) continue;
            int own = s == zs;
            for (u32 i = 0; i < ncolor; i++) own |= rts[i] == s;
            if (own) { b->tex[k] = VK_NULL_HANDLE; continue; }
            for (u32 i = 0; rp.active && i < 9; i++) ends |= rp.images[i] == s->image;
        }
    }


    PHASE(4);
    /* the indices: copied after one another into the frame's upload memory (8-bit ones widened to 16) */
    VkBuffer ibuf = VK_NULL_HANDLE;
    VkDeviceSize ioff = 0;
    u32 isz = 0;
    if (indexed) {
        u32 fmt = regs[M_INDEX_BUFFER + 4], first = regs[M_INDEX_BUFFER + 5], n = regs[M_DRAW_INDEX_COUNT];
        isz = fmt == 0 ? 1 : fmt == 1 ? 2 : 4;
        u64 va = ((u64)regs[M_INDEX_BUFFER] << 32 | regs[M_INDEX_BUFFER + 1]) + (u64)first * isz;
        if (!n || n > (16u << 20)) { vk_stat_skip_other++; leave(); return; }
        u32 bytes = n * (isz == 1 ? 2 : isz);
        /* the same indices from the same place in one submission (nothing but the GPU's own commands writes
         * memory meanwhile, and writes_over counts those): the copy made for an earlier draw */
        static struct { u64 va, serial, batch, gen; u32 bytes, isz; VkBuffer buf; VkDeviceSize off; } ix[4096];
        u64 ig = gpu_va_to_guest(va), raw = (u64)n * isz;
        int whole = ig && gpu_va_to_guest(va + raw - 1) == ig + raw - 1;
        u64 gen = whole ? writes_over(ig, raw) : 0;
        cmd();
        u32 slot_i = (u32)(((va >> 1) * 0x9E3779B97F4A7C15ull ^ bytes) >> 52) & 4095;
        if (whole && ix[slot_i].va == va && ix[slot_i].bytes == bytes && ix[slot_i].isz == isz && ix[slot_i].serial == frames[frame_at].serial &&
            ix[slot_i].batch == gpu_stat_batches && ix[slot_i].gen == gen) {
            ibuf = ix[slot_i].buf;
            ioff = ix[slot_i].off;
            vk_stat_index_reused++;
            goto indices_done;
        }
        u8 *dst = arena(bytes, 4, &ibuf, &ioff);
        if (!dst) { vk_stat_skip_other++; leave(); return; }
        if (whole) {
            ix[slot_i].va = va; ix[slot_i].bytes = bytes; ix[slot_i].isz = isz; ix[slot_i].serial = frames[frame_at].serial;
            ix[slot_i].batch = gpu_stat_batches; ix[slot_i].gen = gen; ix[slot_i].buf = ibuf; ix[slot_i].off = ioff;
        }
        if (isz == 1) {
            static u8 tmp8[1 << 16];
            for (u32 done = 0; done < n;) {
                u32 k = n - done > sizeof tmp8 ? (u32)sizeof tmp8 : n - done;
                read_gpu(va + done, k, tmp8);
                for (u32 i = 0; i < k; i++) ((u16 *)dst)[done + i] = tmp8[i];
                done += k;
            }
        } else read_gpu(va, n * isz, dst);
        vk_stat_upload_bytes += bytes;
    indices_done:;
    }
    /* The parameters (1.3 KB) into the frame's upload memory - unless they are the last draw's in this frame:
     * then its copy, and its offset (its descriptor sets and their binding may then stay as they are) */
    static DrawParams last_params;
    static u64 last_params_serial;
    static VkBuffer last_pbuf;
    static VkDeviceSize last_poff;
    VkBuffer pbuf;
    VkDeviceSize poff;
    cmd();
    if (last_params_serial == frames[frame_at].serial && memo_on() && !memcmp(&last_params, &params, sizeof params)) {
        pbuf = last_pbuf;
        poff = last_poff;
        vk_stat_params_kept++;
    } else {
        u8 *pdst = arena(sizeof params, ubo_align, &pbuf, &poff);
        if (!pdst) { vk_stat_skip_other++; leave(); return; }
        memcpy(pdst, &params, sizeof params);
        last_params = params;
        last_params_serial = frames[frame_at].serial;
        last_pbuf = pbuf;
        last_poff = poff;
    }
    PHASE(5);
    VkDescriptorSet sets[2] = {stage_set(vs, &vb, pbuf), ps ? stage_set(ps, &pb, pbuf) : VK_NULL_HANDLE};
    if (!sets[0] || (ps && !sets[1])) { vk_stat_skip_other++; leave(); return; }

    PHASE(6);
    /* the rendering: the open one if it has these attachments, else a new one */
    VkImageView colors[8] = {0}, depth = zs ? surface_attachment(zs) : VK_NULL_HANDLE;
    for (u32 i = 0; i < ncolor; i++) colors[i] = rts[i] ? surface_attachment(rts[i]) : VK_NULL_HANDLE;
    if (rp.active && (ends || rp.ncolor != ncolor || rp.depth != depth || memcmp(rp.color, colors, sizeof colors))) end_rendering();
    VkCommandBuffer c = cmd();
    if (!rp.active) {
        VkRenderingAttachmentInfoKHR ca[8], da;
        memset(ca, 0, sizeof ca);
        for (u32 i = 0; i < ncolor; i++)
            ca[i] = (VkRenderingAttachmentInfoKHR){VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR, NULL, colors[i], VK_IMAGE_LAYOUT_GENERAL,
                                                    VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED,
                                                    VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE};
        da = (VkRenderingAttachmentInfoKHR){VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR, NULL, depth, VK_IMAGE_LAYOUT_GENERAL,
                                             VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED,
                                             VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE};
        VkRenderingInfoKHR ri = {VK_STRUCTURE_TYPE_RENDERING_INFO_KHR, NULL, 0, {{0, 0}, {area_w, area_h}}, 1, 0, ncolor, ca,
                                 depth ? &da : NULL, depth && has_stencil(zfmt) ? &da : NULL};
        vkCmdBeginRenderingKHR(c, &ri);
        rp.active = 1;
        rp.ncolor = ncolor;
        rp.depth = depth;
        memcpy(rp.color, colors, sizeof colors);
        memset(rp.images, 0, sizeof rp.images);
        for (u32 i = 0; i < ncolor; i++) rp.images[i] = rts[i] ? rts[i]->image : VK_NULL_HANDLE;
        rp.images[8] = zs ? zs->image : VK_NULL_HANDLE;
        vk_stat_passes++;
    }
    /* (state the command buffer already has is not set again: bound) */
    if (bound.serial != frames[frame_at].serial) { memset(&bound, 0, sizeof bound); bound.serial = frames[frame_at].serial; }
    if (bound.pipe != pipe) { vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe); bound.pipe = pipe; }
    uint32_t dyn_off[2] = {(uint32_t)poff, (uint32_t)poff};
    if (!bound.have || !memo_on() || bound.sets[0] != sets[0] || bound.poff != (u32)poff || (ps && bound.sets[1] != sets[1])) {
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, gfx_layout, 0, ps ? 2 : 1, sets, ps ? 2 : 1, dyn_off);
        bound.sets[0] = sets[0];
        bound.sets[1] = ps ? sets[1] : VK_NULL_HANDLE;   /* (with no pixel program the one bound before stays: not known as bound) */
        bound.poff = (u32)poff;
    } else vk_stat_binds_kept++;
    VkViewport vp = {vx, vy + vh, vw, -vh, zmin, zmax};
    if (!bound.have || memcmp(&bound.vp, &vp, sizeof vp)) { vkCmdSetViewport(c, 0, 1, &vp); bound.vp = vp; }
    if (!bound.have || memcmp(&bound.scissor, &scissor, sizeof scissor)) { vkCmdSetScissor(c, 0, 1, &scissor); bound.scissor = scissor; }
    u32 blend_bits[4] = {regs[M_BLEND_CONST(0)], regs[M_BLEND_CONST(1)], regs[M_BLEND_CONST(2)], regs[M_BLEND_CONST(3)]};
    if (!bound.have || memcmp(bound.blend, blend_bits, sizeof blend_bits)) {
        float blend_const[4] = {f32_of(blend_bits[0]), f32_of(blend_bits[1]), f32_of(blend_bits[2]), f32_of(blend_bits[3])};
        vkCmdSetBlendConstants(c, blend_const);
        memcpy(bound.blend, blend_bits, sizeof blend_bits);
    }
    /* Stencil references and masks: Vulkan has a set for each face. Which is which as gpu_d3d11.c draws
     * its two passes: WINDOW_ORIGIN.FLIP_Y gives the faces Direct3D (and here Vulkan) calls front the
     * hardware's back state - when the back state differs; else the front's for both. */
    u32 fref = regs[M_STENCIL_REF] & 0xFF, fwrite = regs[M_STENCIL_MASK] & 0xFF, fread = regs[M_STENCIL_FUNC_MASK] & 0xFF;
    u32 bref = regs[M_BACK_STENCIL_REF] & 0xFF, bwrite = regs[M_BACK_STENCIL_MASK] & 0xFF, bread = regs[M_BACK_STENCIL_FUNC_MASK] & 0xFF;
    int split = key.stencil_test && (regs[M_TWO_SIDED_STENCIL] & 1) && (bref != fref || bwrite != fwrite || bread != fread);
    if (!split) { bref = fref; bwrite = fwrite; bread = fread; }
    static int force_swap = -2;
    if (force_swap == -2) force_swap = getenv("SSBU_STENCIL_SWAP") ? atoi(getenv("SSBU_STENCIL_SWAP")) != 0 : -1;
    int swap_faces = split && (force_swap >= 0 ? force_swap : (int)((regs[M_WINDOW_ORIGIN] >> 4) & 1));
    u32 stencil[6] = {swap_faces ? bref : fref, swap_faces ? fref : bref, swap_faces ? bwrite : fwrite, swap_faces ? fwrite : bwrite,
                      swap_faces ? bread : fread, swap_faces ? fread : bread};
    if (!bound.have || memcmp(bound.stencil, stencil, sizeof stencil)) {
        vkCmdSetStencilReference(c, VK_STENCIL_FACE_FRONT_BIT, stencil[0]);
        vkCmdSetStencilReference(c, VK_STENCIL_FACE_BACK_BIT, stencil[1]);
        vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_FRONT_BIT, stencil[2]);
        vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_BACK_BIT, stencil[3]);
        vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_FRONT_BIT, stencil[4]);
        vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_BACK_BIT, stencil[5]);
        memcpy(bound.stencil, stencil, sizeof stencil);
    }
    bound.have = 1;
    if (indexed) {
        VkIndexType itype = isz == 4 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
        if (bound.ibuf != ibuf || bound.ioff != ioff || bound.itype != itype) {
            vkCmdBindIndexBuffer(c, ibuf, ioff, itype);
            bound.ibuf = ibuf; bound.ioff = ioff; bound.itype = itype;
        }
        vkCmdDrawIndexed(c, regs[M_DRAW_INDEX_COUNT], 1, 0, vs->glslang ? 0 : (int32_t)regs[M_GLOBAL_BASE_VERTEX], 0);
    } else vkCmdDraw(c, regs[M_VERTEX_ARRAY_COUNT], 1, vs->glslang ? 0 : regs[M_VERTEX_ARRAY_START], 0);
    /* (Direct3D's SV_VertexID leaves out the base vertex and the first vertex - vkd3d-shader's SPIR-V reads it as
     * VertexIndex - BaseVertex - while glslang's is VertexIndex: for its programs the draw gives no offset, as the
     * vertices are pulled by the program, not by the input assembler, so the offset only ever showed in the index) */
    PHASE(7);
    vk_stat_drawn++;
    leave();
}

/* The rest of the runtime's notices: writes of the GPU's own commands (note_wrote, above). */
static void vk_wrote(u64 va, u64 bytes) {
    enter();
    note_wrote(va, bytes);
    leave();
}
/* Memory taken away: textures made from it are made again at their next use (if any). */
static void vk_guest_unmapped(u64 addr, u64 size) {
    enter();
    tex_gen++;
    residents_unmapped(addr, addr + size, 1);
    for (u32 i = 0; i < KEPT_SETS * KEPT_WAYS; i++)
        if (kept[i].buf && kept[i].guest < addr + size && addr < kept[i].guest + kept[i].size) kept_drop(&kept[i]);
    for (u32 i = 0; i < ntextures; i++)
        if (textures[i].image && textures[i].guest < addr + size && addr < textures[i].guest + textures[i].size) textures[i].stale = 1, textures[i].sample_hash = ~0ull;
    note_wrote(0, 0);
    leave();
}
static void vk_gpu_unmapped(u64 va, u64 size) {
    enter();
    tex_gen++;
    note_wrote(0, 0);   /* (what was uploaded by GPU address may have moved: nothing taken again unlooked) */
    residents_unmapped(va, va + size, 0);
    for (u32 i = 0; i < KEPT_SETS * KEPT_WAYS; i++)
        if (kept[i].buf && kept[i].va < va + size && va < kept[i].va + kept[i].size) kept_drop(&kept[i]);
    note_wrote(0, 0);
    leave();
}

/* ---- compute launches (the plan's step 6), as gpu_d3d11.c's d3d11_dispatch -------------------------- */
static void vk_dispatch_(const u32 *regs, const u32 *q);
static void vk_dispatch(const u32 *regs, const u32 *q) {
    u64 t0 = rt_clock_ns();
    vk_dispatch_(regs, q);
    InterlockedAdd64((volatile LONG64 *)&vk_ns_dispatch, (LONG64)(rt_clock_ns() - t0));
}
static void vk_dispatch_(const u32 *regs, const u32 *q) {
    enter();
    vk_stat_dispatches++;
    u64 region = (u64)regs[M_PROGRAM_REGION] << 32 | regs[M_PROGRAM_REGION + 1];
    u64 g = gpu_va_to_guest(region + q[8]);
    u32 local[3] = {q[18] >> 16, q[19] & 0xFFFF, q[19] >> 16};
    u32 grid[3] = {q[12] & 0x7FFFFFFF, q[13] & 0xFFFF, q[13] >> 16};
    u32 shared = q[17] & 0x3FFFF;
    CbBinding cb[18];
    memset(cb, 0, sizeof cb);
    for (u32 i = 0; i < 8; i++)
        if (q[20] >> i & 1) { cb[i].address = (u64)(q[30 + 2 * i] & 0xFF) << 32 | q[29 + 2 * i]; cb[i].size = q[30 + 2 * i] >> 15; }
    if (!g || !grid[0] || !grid[1] || !grid[2] || !local[0]) { leave(); return; }
    VShader *cs = find_shader_ex(g, SHADER_COMPUTE, 0, cb, 0, local, shared);
    if (!cs || cs->state != 1 || !cs->compute) { vk_stat_skip_compute++; leave(); return; }

    VkBufferView cbv[18] = {0};
    for (u32 b = 0; b < 18; b++) {
        if (!(cs->info.cbuf_mask >> b & 1)) continue;
        u32 size = cb[b].size > 0x10000 ? 0x10000 : cb[b].size;
        if (size > cs->info.cbuf_extent[b]) size = cs->info.cbuf_extent[b];
        cbv[b] = upload(cb[b].address, size);
    }
    DrawParams params;
    memset(&params, 0, sizeof params);
    VkDescriptorBufferInfo sb[16];
    for (u32 k = 0; k < 16; k++) sb[k] = (VkDescriptorBufferInfo){dummy_buffer, 0, 256};
    /* As gpu_d3d11.c: a program that stores is learned over its first 8 launches - every buffer from guest
     * memory, the results read back, written to guest memory, and the buffers that changed noted; from then
     * on what it writes is a resident region, what it only reads an input buffer (or the region's data) */
    static int no_resident = -1;
    if (no_resident < 0) no_resident = getenv("SSBU_NO_RESIDENT") != NULL;
    int learned = (cs->learn_runs >= 8 || !cs->info.stores) && !no_resident;
    u8 *learn_at[16] = {0}, *before[16] = {0};
    u64 learn_va[16] = {0};
    u32 learn_size[16] = {0};
    for (u32 k = 0; k < cs->info.sb_count && k < 16; k++) {
        u32 desc[4] = {0};
        if (cb[0].address) read_gpu(cb[0].address + cs->info.sb_offset[k], 16, desc);
        u64 va = (u64)desc[1] << 32 | desc[0];
        u32 size = desc[2] > (64u << 20) ? 64u << 20 : desc[2];
        params.sb[0][k][0] = desc[0];
        params.sb[0][k][1] = desc[1];
        params.sb[0][k][2] = size;
        if (!va || !size) continue;
        int writes = learned && (cs->written_mask >> k & 1);
        Resident *r = !learned ? NULL : writes ? resident_acquire(va, size) : resident_covering(va, size);
        if (r && writes) r->written = gfx_stat_presented;
        if (r) resident_check(r);
        if (!r && learned && !writes && (r = input_buffer(va, size)) != NULL) {   /* only read: cached */
            sb[k] = (VkDescriptorBufferInfo){r->buf, 0, (size + 3) & ~3ull};
            continue;
        }
        if (r) {   /* (the view may start a little before va) */
            VkDeviceSize off = va - r->va, at = off / ssbo_align * ssbo_align;
            u32 skip = (u32)(off - at);
            params.sb[0][k][0] = (u32)(va - skip);
            params.sb[0][k][1] = (u32)((va - skip) >> 32);
            params.sb[0][k][2] = size + skip;
            VkDeviceSize range = (skip + size + 3) & ~3ull;
            if (at + range > r->bytes) range = r->bytes - at;
            sb[k] = (VkDescriptorBufferInfo){r->buf, at, range};
            continue;
        }
        /* outside the watched area: guest memory for this launch alone (its results are lost) */
        VkBuffer buf;
        VkDeviceSize boff;
        u32 bytes = (size + 15) & ~15u;
        u8 *p = arena(bytes, ssbo_align, &buf, &boff);
        if (!p) continue;
        read_gpu(va, size, p);
        if (bytes > size) memset(p + size, 0, bytes - size);
        sb[k] = (VkDescriptorBufferInfo){buf, boff, bytes};
        if (!learned && cs->info.stores && (before[k] = malloc(size)) != NULL) {
            memcpy(before[k], p, size);
            learn_at[k] = p; learn_va[k] = va; learn_size[k] = size;
        }
    }
    VkBuffer pbuf;
    VkDeviceSize poff;
    u8 *pdst = arena(sizeof params, ubo_align, &pbuf, &poff);
    VkDescriptorSet set = pdst ? new_set(cs_set_layout) : VK_NULL_HANDLE;
    if (!set) { vk_stat_skip_compute++; leave(); return; }
    memcpy(pdst, &params, sizeof params);
    VkWriteDescriptorSet w[1 + 18 + 16 + 64];
    VkDescriptorImageInfo ii[64];
    VkDescriptorBufferInfo pinfo = {pbuf, poff, sizeof(DrawParams)};
    u32 n = 0, ni = 0;
    w[n++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, SPIRV_BINDING_CBV(0), 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &pinfo, NULL};
    for (u32 b = 0; b < 18; b++)
        if (cs->info.cbuf_mask >> b & 1)
            w[n++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, SPIRV_BINDING_SRV(b), 0, 1,
                                            VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, NULL, NULL, cbv[b] ? &cbv[b] : &dummy_texels};
    for (u32 k = 0; k < cs->info.sb_count && k < 16; k++)
        w[n++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, SPIRV_BINDING_UAV(k), 0, 1,
                                        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &sb[k], NULL};
    for (u32 k = 0; k < cs->info.ntex && k < 32; k++) {   /* (gpu_d3d11.c binds no textures for compute: none here either) */
        VkImageView v = dummy_views[cs->info.tex[k].type & 7] ? dummy_views[cs->info.tex[k].type & 7] : dummy_views[TEX_2D];
        ii[ni] = (VkDescriptorImageInfo){VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_GENERAL};
        w[n++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, SPIRV_BINDING_SRV(64 + k), 0, 1,
                                        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &ii[ni++], NULL, NULL};
        ii[ni] = (VkDescriptorImageInfo){cs->info.tex[k].shadow ? default_compare : default_sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        w[n++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, SPIRV_BINDING_SAMPLER(k), 0, 1,
                                        VK_DESCRIPTOR_TYPE_SAMPLER, &ii[ni++], NULL, NULL};
    }
    vkUpdateDescriptorSets(device, n, w, 0, NULL);
    VkCommandBuffer c = outside();
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, cs->compute);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, cs_layout, 0, 1, &set, 0, NULL);
    vkCmdDispatch(c, grid[0], grid[1], grid[2]);
    sync_all(c);
    vk_stat_dispatched++;
    if (!learned && cs->info.stores) {   /* learning: the results, read back */
        flush_wait();
        for (u32 k = 0; k < 16; k++) {
            if (!before[k]) continue;
            const u32 *now = (const u32 *)learn_at[k], *was = (const u32 *)before[k];
            int changed = 0;
            for (u32 i = 0; i < learn_size[k] / 4; i++) {
                if (now[i] == was[i]) continue;
                changed = 1;
                write_gpu(learn_va[k] + 4ull * i, (const u8 *)&now[i], 4);
            }
            if (changed) cs->written_mask |= 1u << k;
            free(before[k]);
        }
        if (++cs->learn_runs == 8) vk_stat_learned++;
    }
    leave();
}

/* ---- masked clears: a full-target triangle of the clear colour through the channel mask (gpu_d3d11.c's
 * clear_channels), its two small programs compiled at the first one (glslang; or fxc, then vkd3d-shader) ---- */
static VkShaderModule clear_vs, clear_ps;
static int clear_ready;   /* 0 not tried, 1 ready, -1 failed */
static struct { VkFormat format; u32 mask; VkPipeline pipe; } clear_pipes[64];
static u32 nclear_pipes;
static u64 vk_stat_masked_clears;

static VkShaderModule small_module(const char *hlsl, const char *target, int stage) {
    ID3DBlob *code = NULL, *errors = NULL;
    VkShaderModule m = VK_NULL_HANDLE;
    if (use_glslang()) {   /* (glslang: from the HLSL itself) */
        SpirvCode spv;
        char err[600];
        if (spirv_from_hlsl(hlsl, stage, &spv, err, sizeof err)) {
            VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, spv.size, spv.code};
            if (vkCreateShaderModule(device, &ci, NULL, &m) != VK_SUCCESS) m = VK_NULL_HANDLE;
            spirv_free(&spv);
        } else fprintf(stderr, "[vulkan] a %s program of the back end's own: glslang failed: %.500s\n", target, err);
        return m;
    }
    if (SUCCEEDED(D3DCompile(hlsl, strlen(hlsl), "clear", NULL, NULL, "main", target, 0, 0, &code, &errors)) && code) {
        SpirvCode spv;
        char err[300];
        if (spirv_from_dxbc(ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), stage, &spv, err, sizeof err)) {
            VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, spv.size, spv.code};
            if (vkCreateShaderModule(device, &ci, NULL, &m) != VK_SUCCESS) m = VK_NULL_HANDLE;
            spirv_free(&spv);
        }
    }
    if (code) ID3D10Blob_Release(code);
    if (errors) ID3D10Blob_Release(errors);
    return m;
}

static VkPipeline clear_pipeline(VkFormat format, u32 mask) {
    for (u32 i = 0; i < nclear_pipes; i++)
        if (clear_pipes[i].format == format && clear_pipes[i].mask == mask) return clear_pipes[i].pipe;
    if (!clear_ready) {
        clear_vs = small_module("float4 main(uint id : SV_VertexID) : SV_Position {\n"
                                "  return float4(id == 1 ? 3.0f : -1.0f, id == 2 ? 3.0f : -1.0f, 0.0f, 1.0f); }\n", "vs_5_0", SHADER_VERTEX);
        clear_ps = small_module("cbuffer Clear : register(b0) { float4 color; };\n"
                                "float4 main(float4 p : SV_Position) : SV_Target { return color; }\n", "ps_5_0", SHADER_PIXEL);
        clear_ready = clear_vs && clear_ps ? 1 : -1;
        if (clear_ready < 0) fprintf(stderr, "[vulkan] masked clears: their programs could not be made\n");
    }
    if (clear_ready < 0 || nclear_pipes == 64) return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo st[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, clear_vs, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, clear_ps, "main", NULL}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState at = {VK_FALSE, 0, 0, 0, 0, 0, 0, mask};
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_LOGIC_OP_COPY, 1, &at};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn};
    VkPipelineRenderingCreateInfoKHR ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR, NULL, 0, 1, &format, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED};
    VkGraphicsPipelineCreateInfo pci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri, 0, 2, st, &vi, &ia, NULL, &vp, &rs, &ms,
                                        &ds, &cb, &dy, gfx_layout, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, -1};
    VkPipeline p;
    if (vkCreateGraphicsPipelines(device, pipeline_cache, 1, &pci, NULL, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
    clear_pipes[nclear_pipes].format = format;
    clear_pipes[nclear_pipes].mask = mask;
    clear_pipes[nclear_pipes++].pipe = p;
    return p;
}

/* Only some of a colour target's channels cleared (mask: R 1, G 2, B 4, A 8, as Vulkan's write mask). */
static int clear_channels(Surface *s, const float color[4], u32 mask) {
    if (s->klass != CLASS_FLOAT || !(features_of(s->view) & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) return 0;
    VkPipeline pipe = clear_pipeline(s->view, mask);
    VkBuffer buf;
    VkDeviceSize off;
    float *c4 = pipe ? (float *)arena(256, ubo_align, &buf, &off) : NULL;
    VkDescriptorSet set = c4 ? new_set(gfx_set_layout) : VK_NULL_HANDLE;
    if (!set) return 0;
    memcpy(c4, color, 16);   /* (an sRGB target's view encodes it, as Direct3D's clear does) */
    VkDescriptorBufferInfo bi = {buf, 0, 256};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, SPIRV_BINDING_CBV(0), 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, NULL, &bi, NULL};
    uint32_t dyn_off = (uint32_t)off;
    bound.serial = 0;
    vkUpdateDescriptorSets(device, 1, &w, 0, NULL);
    VkImageView view = surface_attachment(s);
    VkCommandBuffer c = outside();
    VkRenderingAttachmentInfoKHR ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR, NULL, view, VK_IMAGE_LAYOUT_GENERAL, VK_RESOLVE_MODE_NONE,
                                       VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfoKHR ri = {VK_STRUCTURE_TYPE_RENDERING_INFO_KHR, NULL, 0, {{0, 0}, {s->hw, s->hh}}, 1, 0, 1, &ca, NULL, NULL};
    vkCmdBeginRenderingKHR(c, &ri);
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, gfx_layout, SPIRV_SET_PIXEL, 1, &set, 1, &dyn_off);
    VkViewport v = {0, 0, (float)s->hw, (float)s->hh, 0, 1};
    VkRect2D r = {{0, 0}, {s->hw, s->hh}};
    vkCmdSetViewport(c, 0, 1, &v);
    vkCmdSetScissor(c, 0, 1, &r);
    vkCmdDraw(c, 3, 1, 0, 0);
    vkCmdEndRenderingKHR(c);
    sync_all(c);
    vk_stat_masked_clears++;
    return 1;
}


/* ---- the window's picture (the plan's step 7): the game's picture fitted into it - shrunk through the
 * same filter as gpu_d3d11.c's psdown when it is bigger than its place (render scale), else bilinear -
 * then the overlay and the settings menu (gpu_overlay.c's quads, gpu_menu.c's drawing) over it, in an
 * image of the swap chain's format and size, copied into the swap chain image shown. ------------------ */
static Alloc win_alloc;
static VkImageView win_unorm, win_srgb;
static VkImage atlas_image;
static Alloc atlas_alloc;
static VkImageView atlas_view;
static int compose_state;   /* 0 not tried, 1 ready, -1 failed (then the plain blit, as before) */
static VkShaderModule full_vs, down_ps, ovl_vs, ovl_ps;
static VkPipeline down_pipe, ovl_pipe;
static VkFormat win_srgb_format;

static const char FULL_VS[] =
    "struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V main(uint id : SV_VertexID) { V o; o.uv = float2((id << 1) & 2, id & 2);\n"
    "  o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n";
static const char DOWN_PS[] =   /* (gpu_d3d11.c's psdown; the texture at t64, the binding map's first) */
    "Texture2D t : register(t64); SamplerState s : register(s0);\n"
    "struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "cbuffer D : register(b0) { float4 dsize; float4 dtaps; };\n"
    "float4 main(V i) : SV_Target { float2 c = i.uv * dsize.xy - dtaps.zw * 0.5f; float3 sum = 0;\n"
    "  [loop] for (int y = 0; y < (int)dtaps.y; y++) [loop] for (int x = 0; x < (int)dtaps.x; x++)\n"
    "    sum += t.SampleLevel(s, (c + dtaps.zw * (float2(x, y) + 0.5f) / dtaps.xy) * dsize.zw, 0).rgb;\n"
    "  return float4(sum / (dtaps.x * dtaps.y), 1); }\n";
static const char OVL_VS[] =   /* (gpu_overlay.c's, its vertices read from a raw buffer: 20 bytes each) */
    "cbuffer C : register(b0) { float4 inv_size; };\n"
    "ByteAddressBuffer vtx : register(t18);\n"
    "struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; float4 c : COLOR0; };\n"
    "O main(uint id : SV_VertexID) { uint4 w = vtx.Load4(id * 20); uint col = vtx.Load(id * 20 + 16); O o;\n"
    "  o.p = float4(asfloat(w.xy) * inv_size.xy * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = asfloat(w.zw);\n"
    "  o.c = float4(col & 255, (col >> 8) & 255, (col >> 16) & 255, col >> 24) / 255.0f; return o; }\n";
static const char OVL_VS_WORDS[] =   /* (the same for glslang: the raw buffer as Buffer<uint>, a uniform texel buffer as above) */
    "cbuffer C : register(b0) { float4 inv_size; };\n"
    "Buffer<uint> vtx : register(t18);\n"
    "struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; float4 c : COLOR0; };\n"
    "O main(uint id : SV_VertexID) { uint i = id * 5u; uint4 w = uint4(vtx[i], vtx[i + 1u], vtx[i + 2u], vtx[i + 3u]); uint col = vtx[i + 4u]; O o;\n"
    "  o.p = float4(asfloat(w.xy) * inv_size.xy * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = asfloat(w.zw);\n"
    "  o.c = float4(col & 255, (col >> 8) & 255, (col >> 16) & 255, col >> 24) / 255.0f; return o; }\n";
static const char OVL_PS[] =
    "Texture2D t : register(t64); SamplerState s : register(s0);\n"
    "struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; float4 c : COLOR0; };\n"
    "float4 main(O i) : SV_Target { return float4(i.c.rgb, i.c.a * t.Sample(s, i.uv).r); }\n";

static VkPipeline small_pipeline(VkShaderModule vs, VkShaderModule ps, VkFormat format, int blend) {
    VkPipelineShaderStageCreateInfo st[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, ps, "main", NULL}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState at = {VK_FALSE, 0, 0, 0, 0, 0, 0, 15};
    if (blend)   /* (gpu_overlay.c's blend state) */
        at = (VkPipelineColorBlendAttachmentState){VK_TRUE, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD,
                                                   VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD, 15};
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_LOGIC_OP_COPY, 1, &at};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn};
    VkPipelineRenderingCreateInfoKHR ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR, NULL, 0, 1, &format, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED};
    VkGraphicsPipelineCreateInfo pci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri, 0, 2, st, &vi, &ia, NULL, &vp, &rs, &ms,
                                        &ds, &cb, &dy, gfx_layout, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, -1};
    VkPipeline p;
    return vkCreateGraphicsPipelines(device, pipeline_cache, 1, &pci, NULL, &p) == VK_SUCCESS ? p : VK_NULL_HANDLE;
}

/* The programs, pipelines and glyph atlas, once (recorded into the frame's commands: the atlas upload). */
static int compose_ready(VkCommandBuffer c) {
    if (compose_state) return compose_state > 0;
    compose_state = -1;
    win_srgb_format = other_gamma(swap_format, 1);
    full_vs = small_module(FULL_VS, "vs_5_0", SHADER_VERTEX);
    down_ps = small_module(DOWN_PS, "ps_5_0", SHADER_PIXEL);
    ovl_vs = small_module(use_glslang() ? OVL_VS_WORDS : OVL_VS, "vs_5_0", SHADER_VERTEX);
    ovl_ps = small_module(OVL_PS, "ps_5_0", SHADER_PIXEL);
    if (!full_vs || !down_ps || !ovl_vs || !ovl_ps) { fprintf(stderr, "[vulkan] the window's programs could not be made: no overlay or menu\n"); return 0; }
    if (win_srgb_format != swap_format) down_pipe = small_pipeline(full_vs, down_ps, win_srgb_format, 0);
    ovl_pipe = small_pipeline(ovl_vs, ovl_ps, swap_format, 1);
    unsigned aw, ah;
    const unsigned char *cov = overlay_coverage(&aw, &ah);
    if (!ovl_pipe || !cov) { fprintf(stderr, "[vulkan] the overlay's pipeline or glyphs could not be made\n"); return 0; }
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8_UNORM, {aw, ah, 1}, 1, 1,
                             VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkBuffer buf;
    VkDeviceSize off;
    u8 *p = arena((VkDeviceSize)aw * ah, 16, &buf, &off);
    if (!p || !(atlas_image = make_image(&ici, &atlas_alloc))) return 0;
    memcpy(p, cov, (size_t)aw * ah);
    image_layout(c, atlas_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    VkBufferImageCopy r = {off, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {aw, ah, 1}};
    vkCmdCopyBufferToImage(c, buf, atlas_image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    sync_all(c);
    if (!(atlas_view = make_view(atlas_image, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, NULL))) return 0;
    compose_state = 1;
    return 1;
}

/* The window's image at the swap chain's size (made again when that changes). */
static int win_picture(VkCommandBuffer c) {
    if (win_image && win_w == swap_w && win_h == swap_h) return 1;
    if (win_image) {
        bury_view(win_unorm);
        bury_view(win_srgb);
        bury_grave((Grave){win_image, VK_NULL_HANDLE, VK_NULL_HANDLE, win_alloc});
        win_image = VK_NULL_HANDLE; win_unorm = win_srgb = VK_NULL_HANDLE;
    }
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, VK_IMAGE_TYPE_2D, swap_format,
                             {swap_w, swap_h, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    if (!(win_image = make_image(&ici, &win_alloc))) return 0;
    win_unorm = make_view(win_image, VK_IMAGE_VIEW_TYPE_2D, swap_format, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, NULL);
    if (win_srgb_format != swap_format) win_srgb = make_view(win_image, VK_IMAGE_VIEW_TYPE_2D, win_srgb_format, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, NULL);
    image_layout(c, win_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    win_w = swap_w; win_h = swap_h;
    return win_unorm != VK_NULL_HANDLE;
}

/* One pass into the window's image through `view`, drawing `count` vertices with `pipe`, the vertex
 * program's set (or none) and the pixel program's; viewport (x, y, w, h). */
static void win_pass(VkCommandBuffer c, VkImageView view, VkPipeline pipe, VkDescriptorSet vset, VkDescriptorSet pset, const uint32_t *offs,
                     float x, float y, float w, float h, u32 count) {
    VkRenderingAttachmentInfoKHR ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR, NULL, view, VK_IMAGE_LAYOUT_GENERAL, VK_RESOLVE_MODE_NONE,
                                       VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfoKHR ri = {VK_STRUCTURE_TYPE_RENDERING_INFO_KHR, NULL, 0, {{0, 0}, {win_w, win_h}}, 1, 0, 1, &ca, NULL, NULL};
    vkCmdBeginRenderingKHR(c, &ri);
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    if (vset) vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, gfx_layout, SPIRV_SET_VERTEX, 1, &vset, 1, &offs[0]);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, gfx_layout, SPIRV_SET_PIXEL, 1, &pset, 1, &offs[1]);
    VkViewport v = {x, y + h, w, -h, 0, 1};   /* (upside down: Direct3D's y, as the draws') */
    VkRect2D r = {{0, 0}, {win_w, win_h}};
    vkCmdSetViewport(c, 0, 1, &v);
    vkCmdSetScissor(c, 0, 1, &r);
    vkCmdDraw(c, count, 1, 0, 0);
    vkCmdEndRenderingKHR(c);
    sync_all(c);
    bound.serial = 0;   /* (the draws' state is no longer bound) */
}

/* A set with the parameters' buffer (bytes at its dynamic offset), and optionally a raw buffer at t18 and
 * a texture at t64 with the default (linear, clamped) sampler. */
static VkDescriptorSet win_set(VkBuffer pbuf, VkBufferView raw, VkImageView tex) {
    VkDescriptorSet set = new_set(gfx_set_layout);
    if (!set) return VK_NULL_HANDLE;
    VkWriteDescriptorSet w[4];
    u32 n = 0;
    VkDescriptorBufferInfo bi = {pbuf, 0, 256};
    w[n++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, SPIRV_BINDING_CBV(0), 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, NULL, &bi, NULL};
    if (raw) w[n++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, SPIRV_BINDING_SRV(18), 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, NULL, NULL, &raw};
    VkDescriptorImageInfo ti = {VK_NULL_HANDLE, tex, VK_IMAGE_LAYOUT_GENERAL}, si = {default_sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    if (tex) {
        w[n++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, SPIRV_BINDING_SRV(64), 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &ti, NULL, NULL};
        w[n++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, SPIRV_BINDING_SAMPLER(0), 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, &si, NULL, NULL};
    }
    vkUpdateDescriptorSets(device, n, w, 0, NULL);
    return set;
}

static u64 vk_stat_downscaled;
/* The window's picture, recorded into c: 1 when the window's image holds it (to be copied into the swap
 * chain image), 0 when it cannot be made (then the plain blit). */
static int compose(VkCommandBuffer c, Surface *src, int show_overlay, const char *detail) {
    if (!compose_ready(c) || !win_picture(c)) return 0;
    VkClearColorValue black = {{0, 0, 0, 1}}, grey = {{0.1f, 0.1f, 0.12f, 1}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(c, win_image, VK_IMAGE_LAYOUT_GENERAL, src ? &black : &grey, 1, &range);
    sync_all(c);
    unsigned fx, fy, fw, fh;
    display_fit(win_w, win_h, &fx, &fy, &fw, &fh);
    if (src && src->klass == CLASS_FLOAT) {
        VkImageView srgb_src = (src->hw > fw || src->hh > fh) && down_pipe && win_srgb ? surface_sampled(src, 1, TEX_2D) : VK_NULL_HANDLE;
        VkBuffer pbuf;
        VkDeviceSize poff;
        float *k = srgb_src ? (float *)arena(256, ubo_align, &pbuf, &poff) : NULL;
        VkDescriptorSet set = k ? win_set(pbuf, VK_NULL_HANDLE, srgb_src) : VK_NULL_HANDLE;
        if (set) {   /* a picture bigger than its place: each window pixel the average of the picture's under it, in linear light */
            float rx = (float)src->hw / (float)fw, ry = (float)src->hh / (float)fh;
            float kk[8] = {(float)src->hw, (float)src->hh, 1.0f / (float)src->hw, 1.0f / (float)src->hh, ceilf(rx), ceilf(ry), rx, ry};
            memcpy(k, kk, sizeof kk);
            uint32_t offs[2] = {0, (uint32_t)poff};
            win_pass(c, win_srgb, down_pipe, VK_NULL_HANDLE, set, offs, (float)fx, (float)fy, (float)fw, (float)fh, 3);
            vk_stat_downscaled++;
        } else if (features_of(src->storage) & VK_FORMAT_FEATURE_BLIT_SRC_BIT) {   /* as big as its place or smaller: bilinear */
            VkImageBlit b;
            memset(&b, 0, sizeof b);
            b.srcSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            b.dstSubresource = b.srcSubresource;
            b.srcOffsets[1] = (VkOffset3D){(int32_t)src->hw, (int32_t)src->hh, 1};
            b.dstOffsets[0] = (VkOffset3D){(int32_t)fx, (int32_t)fy, 0};
            b.dstOffsets[1] = (VkOffset3D){(int32_t)(fx + fw), (int32_t)(fy + fh), 1};
            vkCmdBlitImage(c, src->image, VK_IMAGE_LAYOUT_GENERAL, win_image, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_LINEAR);
            sync_all(c);
        }
    }
    /* the overlay and the menu (gpu_overlay.c's quads), over the picture's place */
    const OverlayVertex *v;
    u32 nv = overlay_quads(fw, fh, show_overlay, detail, &v);
    if (nv) {
        VkBuffer vbuf, pbuf;
        VkDeviceSize voff, poff;
        VkDeviceSize vbytes = ((VkDeviceSize)nv * sizeof *v + 15) & ~15ull;
        u8 *vp = arena(vbytes, texel_align, &vbuf, &voff);
        float *k = vp ? (float *)arena(256, ubo_align, &pbuf, &poff) : NULL;
        VkBufferView raw = k ? texel_view(vbuf, voff, vbytes) : VK_NULL_HANDLE;
        VkDescriptorSet vset = raw ? win_set(pbuf, raw, VK_NULL_HANDLE) : VK_NULL_HANDLE;
        VkDescriptorSet pset = vset ? win_set(pbuf, VK_NULL_HANDLE, atlas_view) : VK_NULL_HANDLE;
        if (pset) {
            memcpy(vp, v, (size_t)nv * sizeof *v);
            float inv[4] = {1.0f / (float)fw, 1.0f / (float)fh, 0, 0};
            memcpy(k, inv, sizeof inv);
            uint32_t offs[2] = {(uint32_t)poff, (uint32_t)poff};
            win_pass(c, win_unorm, ovl_pipe, vset, pset, offs, (float)fx, (float)fy, (float)fw, (float)fh, nv);
        }
    }
    return 1;
}

/* The overlay's details (F2), twice a second: this renderer's counters. */
static const char *vk_detail(int details) {
    static char detail[1400], hint[640];
    static u64 last_ns, last_presented, last_drawn, last_dispatched, last_upload, last_skipped, last_draw_ns, last_waits;
    u64 now = rt_now_ns();
    if (now - last_ns >= 500000000ull) {
        double secs = (now - last_ns) / 1e9;
        u64 frames = gfx_stat_presented - last_presented ? gfx_stat_presented - last_presented : 1;
        u64 skipped = vk_stat_skip_shader + vk_stat_skip_pipeline + vk_stat_skip_target + vk_stat_skip_topology + vk_stat_skip_other;
        snprintf(detail, sizeof detail,
                 "Vulkan: %s\n"
                 "renderer %.1f ms/frame  uploads %.1f MB/frame\n"
                 "draws %llu/frame  compute %llu/frame  GPU waits %llu  skipped %.0f/s\n"
                 "shaders %llu  pipelines %llu  textures %u  surfaces %u\n"
                 "GPU-resident buffers %u  depth sampling %s\n"
                 "Esc settings  F1 overlay  F2 details  F3 screenshot",
                 card, (vk_ns_draw + vk_ns_dispatch + vk_ns_present - last_draw_ns) / 1e6 / frames,
                 (vk_stat_upload_bytes - last_upload) / 1048576.0 / frames, (vk_stat_drawn - last_drawn) / frames,
                 (vk_stat_dispatched - last_dispatched) / frames, vk_stat_flushes - last_waits, (skipped - last_skipped) / secs,
                 vk_stat_shaders, vk_stat_pipelines, ntextures, nsurfaces, nresidents, depth_sampling != 0 ? "on" : "off");
        char net[512];
        net_status(net, sizeof net);
        snprintf(hint, sizeof hint, "F2 details  Esc settings%s%s%s", depth_sampling == 0 ? "  depth sampling OFF" : "", net[0] ? "\n" : "", net);
        if (net[0]) { size_t l = strlen(detail); snprintf(detail + l, sizeof detail - l, "\n%s", net); }
        char pads[256];
        pad_describe(pads, sizeof pads);
        { size_t l = strlen(detail); snprintf(detail + l, sizeof detail - l, "\npads: %s", pads[0] ? pads : "none (keyboard only)"); }
        last_ns = now; last_presented = gfx_stat_presented; last_drawn = vk_stat_drawn; last_dispatched = vk_stat_dispatched;
        last_upload = vk_stat_upload_bytes; last_skipped = skipped; last_draw_ns = vk_ns_draw + vk_ns_dispatch + vk_ns_present; last_waits = vk_stat_flushes;
    }
    return details ? detail : hint;
}

/* ---- the rest of the runtime's questions ------------------------ */
/* The copy engine: [src, src + bytes) to dst on the GPU when the source is a region's (1: done). */
static int vk_copy_resident(u64 dst, u64 src, u64 bytes) {
    enter();
    Resident *s = nresidents && bytes ? resident_covering(src, bytes) : NULL;
    Resident *d = s ? resident_acquire(dst, bytes) : NULL;
    s = d ? resident_covering(src, bytes) : NULL;   /* (acquiring may have merged it) */
    if (!s || !d || d == s) { leave(); return 0; }
    VkCommandBuffer c = outside();
    VkBufferCopy bc = {src - s->va, dst - d->va, bytes};
    vkCmdCopyBuffer(c, s->buf, d->buf, 1, &bc);
    sync_all(c);
    d->written = gfx_stat_presented;
    vk_stat_resident_copies++;
    leave();
    return 1;
}
/* Before the copy engine reads [va, va + bytes): regions' data there back to guest memory. */
static void vk_resident_flush(u64 va, u64 bytes) {
    enter();
    for (u32 i = 0; i < nresidents && bytes; i++) {
        Resident *r = &residents[i];
        if (!r->buf || r->va >= va + bytes || va >= r->va + r->size) continue;
        u64 lo = va > r->va ? va : r->va, hi = va + bytes < r->va + r->size ? va + bytes : r->va + r->size;
        VkBuffer buf;
        VkDeviceSize off;
        u8 *p = arena(hi - lo, 16, &buf, &off);
        if (!p) continue;
        VkCommandBuffer c = outside();
        VkBufferCopy bc = {lo - r->va, off, hi - lo};
        vkCmdCopyBuffer(c, r->buf, buf, 1, &bc);
        sync_all(c);
        flush_wait();
        write_gpu(lo, p, hi - lo);
        watch_written(r->guest, r->size, &r->seen);   /* (its own writes are no news) */
        vk_stat_resident_flushed++;
    }
    leave();
}
/* The copy engine wrote guest memory [va, va + bytes): regions there take it. */
static void vk_resident_written(u64 va, u64 bytes) {
    enter();
    note_wrote(va, bytes);
    for (u32 i = 0; i < nresidents && bytes; i++) {
        Resident *r = &residents[i];
        if (!r->buf || r->va >= va + bytes || va >= r->va + r->size) continue;
        u64 lo = va > r->va ? va : r->va, hi = va + bytes < r->va + r->size ? va + bytes : r->va + r->size;
        resident_fill(r, lo - r->va, hi - lo);
    }
    leave();
}
static void vk_nothing(void) {}
static int vk_draw_logging(void) { return 0; }
static DWORD vk_lock_owner(void) { return (DWORD)(uintptr_t)lock.OwningThread; }
static DWORD vk_last_enter_thread(void) { return last_enter_thread; }
static void vk_report_compiles(void (*line)(const char *text)) { (void)line; }

static void vk_status_lines(void) {
    /* the drawing thread's own run time (preemption left out: a steadier measure than the times above on a busy
     * computer) per draw since the last status */
    static HANDLE dh;
    static ULONG64 last_cycles, last_draws;
    if (!dh && draw_thread) dh = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, draw_thread);
    ULONG64 cycles = 0;
    if (dh && QueryThreadCycleTime(dh, &cycles) && vk_stat_draws > last_draws)
        fprintf(stderr, "[status] vulkan drawing thread: %.0f cycles a draw (%.2f G cycles for %llu draws)\n", (double)(cycles - last_cycles) / (double)(vk_stat_draws - last_draws),
                (cycles - last_cycles) / 1e9, vk_stat_draws - last_draws);
    last_cycles = cycles;
    last_draws = vk_stat_draws;
    if (memo_on() == 2)
        fprintf(stderr, "[status] vulkan SSBU_VK_MEMO=check: %llu answers taken again checked, %llu differed (constant buffers %llu, shaders %llu, textures %llu)\n",
                vk_stat_memo_checked, vk_stat_memo_wrong[0] + vk_stat_memo_wrong[1] + vk_stat_memo_wrong[2], vk_stat_memo_wrong[0], vk_stat_memo_wrong[1], vk_stat_memo_wrong[2]);
    fprintf(stderr, "[status] vulkan (%s): %llu frames shown (%llu with no window to show them in), %llu clears (%llu masked drawn, %llu masked not drawn), "
                    "%llu 2D blits (%llu between formats it cannot blit), %u surfaces, %u textures (%llu made, %llu decoded on the CPU), %llu MB device memory; "
                    "%llu draws: %llu drawn in %llu renderings, skipped %llu shader / %llu pipeline / %llu target / %llu topology / %llu other; "
                    "%llu shaders (%llu failed), %llu pipelines (%llu failed; %.1f s making them), %llu MB uploaded (%llu reused); %llu compute launches (%llu run, %llu skipped), %u resident regions (%llu made, %llu taken again, %llu given up, %llu copies, %llu flushed), %llu programs learned, %llu waits for results; "
                    "time in draws %.1f s, launches %.1f s, presents %.1f s (of it waiting for the GPU %.1f s), %llu upload blocks in the card's memory\n",
            card, gfx_stat_presented, vk_stat_presents_skipped, gfx_stat_clears, vk_stat_masked_clears, vk_stat_clears_skipped, gfx_stat_blits, vk_stat_blits_skipped,
            nsurfaces, ntextures, vk_stat_textures, vk_stat_cpu_decoded, vk_stat_device_bytes >> 20, vk_stat_draws, vk_stat_drawn, vk_stat_passes, vk_stat_skip_shader, vk_stat_skip_pipeline,
            vk_stat_skip_target, vk_stat_skip_topology, vk_stat_skip_other, vk_stat_shaders, vk_stat_shader_failures, vk_stat_pipelines, vk_stat_pipeline_failures, pipeline_ns / 1e9,
            vk_stat_upload_bytes >> 20, vk_stat_reused, vk_stat_dispatches, vk_stat_dispatched, vk_stat_skip_compute, nresidents,
            vk_stat_residents_made, vk_stat_resident_refills, vk_stat_residents_retired, vk_stat_resident_copies, vk_stat_resident_flushed,
            vk_stat_learned, vk_stat_flushes, vk_ns_draw / 1e9, vk_ns_dispatch / 1e9, vk_ns_present / 1e9, vk_ns_wait / 1e9,
            vk_stat_local_blocks);
    if (warm_nrecords)
        fprintf(stderr, "[status] vulkan warm start: %u pipelines and programs listed, %lld programs loaded (%llu taken by draws), %lld pipelines made (%llu drawn with), %ld left out%s\n",
                warm_nrecords, (long long)vk_stat_warm_loaded, vk_stat_warm_taken, (long long)vk_stat_warm_pipelines, vk_stat_warm_used, (long)warm_left,
                warm_running ? ", running" : "");
    fprintf(stderr, "[status] vulkan draws' parts (s): shaders %.1f, targets %.1f, state %.1f, streams %.1f, resources %.1f, indices %.1f, sets %.1f, "
                    "recording %.1f (resources: constant buffers %.1f, storage buffers %.1f, textures %.1f); "
                    "kept constant buffers: %llu made, %llu used, %llu found changed; constant buffers copied: %llu small (%llu MB), %llu big (%llu MB), %llu the same as the slot's last (of %llu compared; %llu not compared, unchanged in their submission); windows: %llu copied, %llu used again; buffer views: %llu made, %llu used again; descriptor sets: %llu written, %llu used again; parameters: %llu the last draw's, sets left bound %llu; indices used again %llu; textures: %llu found again in their submission, %llu 1D, %llu depth from guest memory; %llu frames shrunk into the window\n", vk_ns_phase[0] / 1e9, vk_ns_phase[1] / 1e9, vk_ns_phase[2] / 1e9, vk_ns_phase[3] / 1e9, vk_ns_phase[4] / 1e9,
            vk_ns_phase[5] / 1e9, vk_ns_phase[6] / 1e9, vk_ns_phase[7] / 1e9, vk_ns_res[0] / 1e9, vk_ns_res[1] / 1e9, vk_ns_res[2] / 1e9, vk_stat_kept_made, vk_stat_kept_hits, vk_stat_kept_stale, vk_stat_cb_copies[0], vk_stat_cb_bytes[0] >> 20, vk_stat_cb_copies[1], vk_stat_cb_bytes[1] >> 20, vk_stat_cb_same, vk_stat_cb_compared, vk_stat_cb_unchanged, vk_stat_window_copies, vk_stat_window_hits,
            vk_stat_views_made, vk_stat_views_kept, vk_stat_sets_made, vk_stat_sets_kept, vk_stat_params_kept, vk_stat_binds_kept, vk_stat_index_reused,
            vk_stat_tex_memo, vk_stat_textures_1d, vk_stat_depth_textures, vk_stat_downscaled);
}

/* The output (monitor) the window is on, referenced, for display pacing (gpu_display.c waits for its
 * refreshes): DXGI's, which needs no Direct3D device; NULL if none matches. */
static void *vk_display_output(void) {
    if (!window) return NULL;
    HMONITOR mon = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    IDXGIFactory1 *factory;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory))) return NULL;
    IDXGIOutput *found = NULL;
    IDXGIAdapter1 *a;
    for (UINT i = 0; !found && IDXGIFactory1_EnumAdapters1(factory, i, &a) == S_OK; i++) {
        IDXGIOutput *o;
        for (UINT j = 0; !found && IDXGIAdapter1_EnumOutputs(a, j, &o) == S_OK; j++) {
            DXGI_OUTPUT_DESC d;
            if (SUCCEEDED(IDXGIOutput_GetDesc(o, &d)) && d.Monitor == mon) found = o;
            else IDXGIOutput_Release(o);
        }
        IDXGIAdapter1_Release(a);
    }
    IDXGIFactory1_Release(factory);
    return found;
}

/* The pipeline cache to its file (written aside, then put in place): by a maker at most once a minute while
 * new pipelines are made, and at the end. */
static void pipeline_cache_save(void) {
    u64 t0 = rt_clock_ns();
    size_t size = 0;
    if (!pipeline_cache || vkGetPipelineCacheData(device, pipeline_cache, &size, NULL) != VK_SUCCESS || !size) return;
    void *data = malloc(size);
    if (!data) return;
    if (vkGetPipelineCacheData(device, pipeline_cache, &size, data) == VK_SUCCESS) {
        char tmp[1100];
        snprintf(tmp, sizeof tmp, "%s.%lu.tmp", pipeline_cache_path, GetCurrentProcessId());
        FILE *f = fopen(tmp, "wb");
        if (f) {
            int ok = fwrite(data, 1, size, f) == size;
            if (fclose(f) || !ok || !MoveFileExA(tmp, pipeline_cache_path, MOVEFILE_REPLACE_EXISTING)) DeleteFileA(tmp);
        }
    }
    free(data);
    fprintf(stderr, "[vulkan] pipeline cache saved: %.1f MB in %.0f ms (at %.1f s)\n", size / 1048576.0, (rt_clock_ns() - t0) / 1e6, rt_now_ns() / 1e9);
}
static volatile LONG saving;
static void save_job(Job *j) { (void)j; pipeline_cache_save(); InterlockedExchange(&saving, 0); }
static void pipeline_cache_tick(void) {
    static u64 saved, last;
    static Job job = {NULL, save_job};
    u64 made = vk_stat_pipelines + (u64)vk_stat_warm_pipelines, now = rt_now_ns();   /* (the warm threads' too: a lost cache file made again) */
    if (made == saved || now - last < 60000000000ull || saving) return;   /* (a minute apart: the file is 100 MB and more) */
    saved = made;
    last = now;
    InterlockedExchange(&saving, 1);
    job_add(&job);
}

static void vk_closing(void) {
    /* (Closed while it warms: the threads make no more, and the marker goes - a close is not a warm start that died.) */
    if (warm_running) { InterlockedExchange(&warm_stopped, 1); DeleteFileA(warm_marker); }
    if (!saving) pipeline_cache_save();
}

static const GfxBackend VULKAN = {
    .clear = vk_clear,
    .draw = vk_draw,
    .present = vk_present,
    .blit2d = vk_blit2d,
    .dispatch = vk_dispatch,
    .copy_resident = vk_copy_resident,
    .resident_flush = vk_resident_flush,
    .resident_written = vk_resident_written,
    .stream_wrote = vk_wrote,
    .guest_wrote = vk_wrote,
    .stream_end = vk_nothing,
    .gpu_unmapped = vk_gpu_unmapped,
    .guest_unmapped = vk_guest_unmapped,
    .draw_logging = vk_draw_logging,
    .status_lines = vk_status_lines,
    .closing = vk_closing,
    .lock_owner = vk_lock_owner,
    .last_enter_thread = vk_last_enter_thread,
    .report_compiles = vk_report_compiles,
    .display_output = vk_display_output,
};

/* ---- the start ------------------------------------------------------------------------------------- */
/* What draws need made once: the descriptor set layout of the binding map (gpu_spirv.h: the same layout
 * for the vertex program's set 0 and the pixel program's set 1), the pipeline layout and cache, and
 * stand-ins for what a draw lacks (Direct3D reads zeros there): a raw buffer, an image of each type,
 * samplers. */
static int draw_objects(void) {
    VkDescriptorSetLayoutBinding b[1 + 18 + 32 + 32 + 16 + 32];
    u32 n = 0;
    b[n++] = (VkDescriptorSetLayoutBinding){SPIRV_BINDING_CBV(0), VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_ALL_GRAPHICS, NULL};
    for (u32 k = 0; k < 18 + 32; k++)   /* constant buffers t0-17, vertex streams t18-49 */
        b[n++] = (VkDescriptorSetLayoutBinding){SPIRV_BINDING_SRV(k), VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_ALL_GRAPHICS, NULL};
    for (u32 k = 0; k < 32; k++)        /* textures t64-95 */
        b[n++] = (VkDescriptorSetLayoutBinding){SPIRV_BINDING_SRV(64 + k), VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_ALL_GRAPHICS, NULL};
    for (u32 k = 0; k < 16; k++)        /* storage buffers t96-111 */
        b[n++] = (VkDescriptorSetLayoutBinding){SPIRV_BINDING_SRV(96 + k), VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_ALL_GRAPHICS, NULL};
    for (u32 k = 0; k < 32; k++)        /* samplers s0-31 */
        b[n++] = (VkDescriptorSetLayoutBinding){SPIRV_BINDING_SAMPLER(k), VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_ALL_GRAPHICS, NULL};
    VkDescriptorSetLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, n, b};
    if (vkCreateDescriptorSetLayout(device, &lci, NULL, &gfx_set_layout) != VK_SUCCESS) return 0;
    VkDescriptorSetLayout sets[2] = {gfx_set_layout, gfx_set_layout};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 2, sets, 0, NULL};
    if (vkCreatePipelineLayout(device, &pli, NULL, &gfx_layout) != VK_SUCCESS) return 0;
    /* the pipeline cache: the last run's, from the shader cache's folder (the driver checks it is its own) */
    snprintf(pipeline_cache_path, sizeof pipeline_cache_path, "%s/vulkan_%04x_%04x.pipelines", cache_dir(), limits_vendor, limits_device);
    void *initial = NULL;
    size_t initial_size = 0;
    FILE *f = fopen(pipeline_cache_path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size > 0 && (initial = malloc((size_t)size)) && fread(initial, 1, (size_t)size, f) == (size_t)size) initial_size = (size_t)size;
        fclose(f);
    }
    /* compute: set 0 - no vertex streams; storage buffers (u0-15) written */
    n = 0;
    b[n++] = (VkDescriptorSetLayoutBinding){SPIRV_BINDING_CBV(0), VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    for (u32 k = 0; k < 18; k++)
        b[n++] = (VkDescriptorSetLayoutBinding){SPIRV_BINDING_SRV(k), VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    for (u32 k = 0; k < 32; k++)
        b[n++] = (VkDescriptorSetLayoutBinding){SPIRV_BINDING_SRV(64 + k), VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    for (u32 k = 0; k < 32; k++)
        b[n++] = (VkDescriptorSetLayoutBinding){SPIRV_BINDING_SAMPLER(k), VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    for (u32 k = 0; k < 16; k++)
        b[n++] = (VkDescriptorSetLayoutBinding){SPIRV_BINDING_UAV(k), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    lci.bindingCount = n;
    if (vkCreateDescriptorSetLayout(device, &lci, NULL, &cs_set_layout) != VK_SUCCESS) return 0;
    VkPipelineLayoutCreateInfo cli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &cs_set_layout, 0, NULL};
    if (vkCreatePipelineLayout(device, &cli, NULL, &cs_layout) != VK_SUCCESS) return 0;

    VkPipelineCacheCreateInfo pcci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, NULL, 0, initial_size, initial};
    if (vkCreatePipelineCache(device, &pcci, NULL, &pipeline_cache) != VK_SUCCESS) {
        pcci.initialDataSize = 0;
        if (vkCreatePipelineCache(device, &pcci, NULL, &pipeline_cache) != VK_SUCCESS) pipeline_cache = VK_NULL_HANDLE;
    }
    if (initial_size) fprintf(stderr, "[vulkan] pipeline cache: %.1f MB from %s\n", initial_size / 1048576.0, pipeline_cache_path);
    free(initial);

    /* a raw buffer of zeros */
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 256, VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    if (vkCreateBuffer(device, &bci, NULL, &dummy_buffer) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(device, dummy_buffer, &mr);
    int type = memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, (uint32_t)type};
    if (type < 0 || vkAllocateMemory(device, &mai, NULL, &dummy_buffer_mem.mem) != VK_SUCCESS) return 0;
    dummy_buffer_mem.chunk = -1;
    vkBindBufferMemory(device, dummy_buffer, dummy_buffer_mem.mem, 0);
    void *zeros;
    if (vkMapMemory(device, dummy_buffer_mem.mem, 0, VK_WHOLE_SIZE, 0, &zeros) != VK_SUCCESS) return 0;
    memset(zeros, 0, 256);
    vkUnmapMemory(device, dummy_buffer_mem.mem);
    VkBufferViewCreateInfo bvi = {VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, NULL, 0, dummy_buffer, VK_FORMAT_R32_UINT, 0, 256};
    if (vkCreateBufferView(device, &bvi, NULL, &dummy_texels) != VK_SUCCESS) return 0;

    /* images of zeros: 1D, 2D (six layers, a cube too), 3D */
    static const VkImageType types[3] = {VK_IMAGE_TYPE_1D, VK_IMAGE_TYPE_2D, VK_IMAGE_TYPE_3D};
    VkImage *img = dummy_images;
    VkCommandBuffer c = cmd();
    for (int i = 0; i < 3; i++) {
        VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, i == 1 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0, types[i],
                                 VK_FORMAT_R8G8B8A8_UNORM, {1, 1, 1}, 1, i == 1 ? 6u : 1u, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
        if (!(img[i] = make_image(&ici, &dummy_image_mem[i]))) return 0;
        image_layout(c, img[i], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        VkClearColorValue zero;
        memset(&zero, 0, sizeof zero);
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
        vkCmdClearColorImage(c, img[i], VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    }
    sync_all(c);
    dummy_views[TEX_1D] = make_view(img[0], VK_IMAGE_VIEW_TYPE_1D, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, NULL);
    dummy_views[TEX_ARRAY_1D] = make_view(img[0], VK_IMAGE_VIEW_TYPE_1D_ARRAY, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, NULL);
    dummy_views[TEX_2D] = make_view(img[1], VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, NULL);
    dummy_views[TEX_ARRAY_2D] = make_view(img[1], VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 1, 6, NULL);
    dummy_views[TEX_CUBE] = make_view(img[1], VK_IMAGE_VIEW_TYPE_CUBE, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 1, 6, NULL);
    if (enabled.imageCubeArray)
        dummy_views[TEX_ARRAY_CUBE] = make_view(img[1], VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 1, 6, NULL);
    dummy_views[TEX_3D] = make_view(img[2], VK_IMAGE_VIEW_TYPE_3D, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, NULL);
    if (!dummy_views[TEX_2D]) return 0;

    /* Direct3D's default sampler: linear, clamped (and a comparing one: never) */
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, NULL, 0, VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR,
                               VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               0, VK_FALSE, 1, VK_FALSE, VK_COMPARE_OP_NEVER, 0, 1000, VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, VK_FALSE};
    if (vkCreateSampler(device, &sci, NULL, &default_sampler) != VK_SUCCESS) return 0;
    sci.compareEnable = VK_TRUE;
    if (vkCreateSampler(device, &sci, NULL, &default_compare) != VK_SUCCESS) return 0;
    return 1;
}

/* A start that failed after the instance was made, undone: what the device holds freed, the device, the
 * surface and the instance destroyed, so that the back end started in its place (gpu_backend.c: Direct3D 11)
 * does not share the card's memory and the window with a Vulkan device nothing draws with. Nothing was drawn
 * yet, and the makers and the warm threads are not started until the start is sure (its last step). NULL. */
static const GfxBackend *start_undo(void) {
    if (device) {
        vkDeviceWaitIdle(device);
        if (pool) vkDestroyCommandPool(device, pool, NULL);   /* (and the frames' command buffers with it) */
        pool = VK_NULL_HANDLE;
        for (u32 i = 0; i < MAX_FRAMES; i++) {
            if (frames[i].done) vkDestroyFence(device, frames[i].done, NULL);
            if (frames[i].acquired) vkDestroySemaphore(device, frames[i].acquired, NULL);
        }
        memset(frames, 0, sizeof frames);
        frame_at = 0;
        frame_serial = 0;
        if (default_sampler) vkDestroySampler(device, default_sampler, NULL);
        if (default_compare) vkDestroySampler(device, default_compare, NULL);
        default_sampler = default_compare = VK_NULL_HANDLE;
        for (u32 i = 0; i < sizeof dummy_views / sizeof *dummy_views; i++)
            if (dummy_views[i]) vkDestroyImageView(device, dummy_views[i], NULL);
        memset(dummy_views, 0, sizeof dummy_views);
        for (u32 i = 0; i < 3; i++) {
            if (dummy_images[i]) vkDestroyImage(device, dummy_images[i], NULL);
            dummy_images[i] = VK_NULL_HANDLE;
            dev_free(&dummy_image_mem[i]);
        }
        if (dummy_texels) vkDestroyBufferView(device, dummy_texels, NULL);
        if (dummy_buffer) vkDestroyBuffer(device, dummy_buffer, NULL);
        if (dummy_buffer_mem.mem) vkFreeMemory(device, dummy_buffer_mem.mem, NULL);
        dummy_texels = VK_NULL_HANDLE;
        dummy_buffer = VK_NULL_HANDLE;
        memset(&dummy_buffer_mem, 0, sizeof dummy_buffer_mem);
        for (u32 i = 0; i < nchunks; i++) {
            vkFreeMemory(device, chunks[i].mem, NULL);
            free(chunks[i].free);
        }
        memset(chunks, 0, sizeof chunks);
        nchunks = 0;
        vk_stat_device_bytes = 0;
        if (pipeline_cache) vkDestroyPipelineCache(device, pipeline_cache, NULL);
        if (cs_layout) vkDestroyPipelineLayout(device, cs_layout, NULL);
        if (gfx_layout) vkDestroyPipelineLayout(device, gfx_layout, NULL);
        if (cs_set_layout) vkDestroyDescriptorSetLayout(device, cs_set_layout, NULL);
        if (gfx_set_layout) vkDestroyDescriptorSetLayout(device, gfx_set_layout, NULL);
        pipeline_cache = VK_NULL_HANDLE;
        cs_layout = gfx_layout = VK_NULL_HANDLE;
        cs_set_layout = gfx_set_layout = VK_NULL_HANDLE;
        vkDestroyDevice(device, NULL);
        device = VK_NULL_HANDLE;
        queue = VK_NULL_HANDLE;
    }
    if (instance) {
        if (vk_surface && vkDestroySurfaceKHR) vkDestroySurfaceKHR(instance, vk_surface, NULL);
        vk_surface = VK_NULL_HANDLE;
        if (vkDestroyInstance) vkDestroyInstance(instance, NULL);
        instance = VK_NULL_HANDLE;
    }
    return NULL;
}

static int kind_rank(VkPhysicalDeviceType t) {
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 4;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return 0;
    default: return 1;
    }
}

static int name_has(const char *hay, const char *needle) {
    for (; *hay; hay++) {
        size_t i = 0;
        while (needle[i] && hay[i] && (hay[i] | 32) == (needle[i] | 32)) i++;
        if (!needle[i]) return 1;
    }
    return 0;
}

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

/* renderer = auto's trial (gpu_vk.h): vulkan_auto_trial.txt in the shader cache's folder, holding the number of auto
 * starts in a row that did not reach their 600th frame ("strikes N"). Written before anything of Vulkan's is made
 * (a driver or a layer crashing in vkCreateInstance or vkCreateDevice counts too), put back as it was when the start
 * declines or fails (Direct3D 11 then draws, and that is no strike), taken away at the 600th frame shown. Two strikes:
 * auto takes Direct3D 11 until renderer = vulkan, which takes the file away, or a run of it reaches its 600th frame. */
#define TRIAL_STRIKES 2
static char trial_path[1100];
static int trial_open, trial_before;   /* (a trial running; the strikes before it) */
static void trial_path_make(void) { snprintf(trial_path, sizeof trial_path, "%s/vulkan_auto_trial.txt", cache_dir()); }
static int trial_read(void) {
    FILE *f = fopen(trial_path, "r");
    int n = 0;
    if (!f) return 0;
    if (fscanf(f, "strikes %d", &n) != 1 || n < 1) n = 1;   /* (an older or broken file: one) */
    fclose(f);
    return n;
}
static int trial_write(int strikes) {
    FILE *f = fopen(trial_path, "w");
    if (!f) return 0;
    fprintf(f, "strikes %d\n"
               "renderer = auto started the Vulkan renderer this many times in a row without it showing 600 frames (a crash,\n"
               "or the game closed early). At %d, auto starts Direct3D 11 instead; renderer = vulkan (display.ini or the\n"
               "settings menu) tries Vulkan again. The file goes when Vulkan shows its 600th frame.\n", strikes, TRIAL_STRIKES);
    return !fclose(f);
}
static void trial_passed(void) {   /* (vk_present_, at the 600th frame shown) */
    if (!trial_open) return;
    trial_open = 0;
    DeleteFileA(trial_path);
}
static void trial_undo(void) {   /* (the start declined or failed: no strike) */
    if (!trial_open) return;
    trial_open = 0;
    if (trial_before) trial_write(trial_before);
    else DeleteFileA(trial_path);
}

static const GfxBackend *start_backend(char *why, size_t size, int automatic);

const GfxBackend *vk_backend_start(char *why, size_t size, int automatic) {
    trial_path_make();
    if (!automatic) DeleteFileA(trial_path);   /* (asked for by name: auto tries it again after this) */
    else {
        trial_before = trial_read();
        if (trial_before >= TRIAL_STRIKES) {
            snprintf(why, size, "the Vulkan renderer stopped before showing its first 600 frames the last %d times (%s; a crash, or the game "
                     "closed early): Direct3D 11. Settings > Display > Renderer = vulkan tries Vulkan again", trial_before, trial_path);
            return NULL;
        }
        if (trial_write(trial_before + 1)) trial_open = 1;
        else fprintf(stderr, "[vulkan] renderer = auto: %s could not be written: a crash of the Vulkan renderer will not be noticed next time\n", trial_path);
    }
    const GfxBackend *b = start_backend(why, size, automatic);
    if (!b) {
        trial_undo();
        start_undo();   /* (whatever return left something: the device, the surface on the window Direct3D 11 now draws in, the instance) */
    }
    return b;
}

static const GfxBackend *start_backend(char *why, size_t size, int automatic) {
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)vk_loader(why, size);
    if (!gipa) return NULL;
    InitializeCriticalSection(&lock);
    window = (HWND)gfx_window_open();
    if (!window) { snprintf(why, size, "the game's window could not be made"); return NULL; }
    vkCreateInstance_ = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    if (!vkCreateInstance_) { snprintf(why, size, "the Vulkan loader lacks vkCreateInstance"); return NULL; }
    const char *iext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "OpenSmash", 1, "OpenSmash runtime", 1, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 2, iext};
    VkResult r = vkCreateInstance_(&ici, NULL, &instance);
    if (r != VK_SUCCESS) { snprintf(why, size, "vkCreateInstance failed (%d)", (int)r); return NULL; }
#define LOAD_I(name) vk##name = (PFN_vk##name)gipa(instance, "vk" #name);
    VK_INSTANCE_FUNCS(LOAD_I)
#undef LOAD_I
    VkWin32SurfaceCreateInfoKHR wci = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, NULL, 0, GetModuleHandleW(NULL), window};
    if (!vkCreateWin32SurfaceKHR || vkCreateWin32SurfaceKHR(instance, &wci, NULL, &vk_surface) != VK_SUCCESS) {
        snprintf(why, size, "no Vulkan surface on the game's window");
        return start_undo();
    }

    /* the card: display.ini's gpu, else the fastest kind */
    VkPhysicalDevice devs[16];
    uint32_t ndev = 16;
    if (vkEnumeratePhysicalDevices(instance, &ndev, devs) < 0 || !ndev) { snprintf(why, size, "Vulkan lists no graphics card"); return start_undo(); }
    const char *want = display_gpu();
    int any = !want || !*want || !_stricmp(want, "auto") || !_stricmp(want, "warp"), best = -1, rank = -1, named = -1;
    for (uint32_t i = 0; i < ndev; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        if (kind_rank(p.deviceType) > rank) { rank = kind_rank(p.deviceType); best = (int)i; }
        if (!any && named < 0 && p.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU && name_has(p.deviceName, want)) named = (int)i;
    }
    physical = devs[named >= 0 ? named : best];
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical, &props);
    snprintf(card, sizeof card, "%s", props.deviceName);
    /* (auto: Vulkan on AMD's cards alone - gpu_backend.c asked Direct3D 11's choice of card first; this is Vulkan's;
     * SSBU_VK_AUTO_VENDOR=<hex PCI vendor> names another, to test auto elsewhere) */
    unsigned auto_vendor = getenv("SSBU_VK_AUTO_VENDOR") ? (unsigned)strtoul(getenv("SSBU_VK_AUTO_VENDOR"), NULL, 16) : 0x1002;
    if (automatic && props.vendorID != auto_vendor) {
        snprintf(why, size, "%s is not an AMD card, Direct3D 11 is the default for it (renderer = vulkan to draw with Vulkan)", card);
        return start_undo();
    }
    if (props.apiVersion < VK_API_VERSION_1_1) { snprintf(why, size, "%s has Vulkan %u.%u: 1.1 is needed", card, VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion)); return start_undo(); }
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_props);

    /* a queue that draws and shows pictures in the window */
    VkQueueFamilyProperties fams[32];
    uint32_t nfam = 32;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &nfam, fams);
    int family = -1;
    for (uint32_t i = 0; i < nfam && family < 0; i++) {
        VkBool32 shows = VK_FALSE;
        if ((fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (fams[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
            vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, vk_surface, &shows) == VK_SUCCESS && shows)
            family = (int)i;
    }
    if (family < 0) { snprintf(why, size, "%s: no queue that draws and shows pictures in the game's window", card); return start_undo(); }
    queue_family = (uint32_t)family;

    /* the device: swap chains, and float controls for the shaders (gpu_spirv.h) when it has them */
    VkExtensionProperties dext[512];
    uint32_t ndext = 512;
    vkEnumerateDeviceExtensionProperties(physical, NULL, &ndext, dext);
    if (!has_ext(dext, ndext, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) { snprintf(why, size, "%s: no swap chains", card); return start_undo(); }
    /* draws go into dynamic renderings (VK_KHR_dynamic_rendering: core in Vulkan 1.3; drivers of
     * Vulkan 1.1 cards have it too, as an extension) */
    /* (and the draw parameters: vkd3d-shader's SPIR-V reads BaseVertex and BaseInstance - Vulkan 1.1's feature,
     * which a device must be given to use them) */
    VkPhysicalDeviceShaderDrawParametersFeatures drawp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES};
    VkPhysicalDeviceDynamicRenderingFeaturesKHR dynamic = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR, &drawp};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &dynamic};
    if (vkGetPhysicalDeviceFeatures2) vkGetPhysicalDeviceFeatures2(physical, &f2);
    if (!has_ext(dext, ndext, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) || !dynamic.dynamicRendering) {
        snprintf(why, size, "%s: no dynamic rendering (VK_KHR_dynamic_rendering), which the Vulkan renderer draws with", card);
        return start_undo();
    }
    if (!spirv_available()) {
        char g[300] = "";
        spirv_have_glslang(g, sizeof g);
        snprintf(why, size, "no shader translator for Vulkan: %s (and no vkd3d-shader in this build)", g);
        return start_undo();
    }
    const char *want_ext[8] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME};
    uint32_t nwant = 2;
    if (has_ext(dext, ndext, VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME)) want_ext[nwant++] = VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME;
    if (has_ext(dext, ndext, VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME)) want_ext[nwant++] = VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME;
    if ((have_mirror_clamp = has_ext(dext, ndext, VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME)) != 0)
        want_ext[nwant++] = VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME;
    int float_controls = has_ext(dext, ndext, "VK_KHR_shader_float_controls");
    if (float_controls) want_ext[nwant++] = "VK_KHR_shader_float_controls";
    VkPhysicalDeviceFeatures have, enable;
    vkGetPhysicalDeviceFeatures(physical, &have);
    memset(&enable, 0, sizeof enable);
    enable.robustBufferAccess = have.robustBufferAccess;   /* (raw buffers read past their end read zeros, as in Direct3D) */
    enable.fillModeNonSolid = have.fillModeNonSolid;
    enable.geometryShader = have.geometryShader;           /* (topologies with adjacency need it) */
    enable.independentBlend = have.independentBlend;
    enable.dualSrcBlend = have.dualSrcBlend;
    enable.depthClamp = have.depthClamp;
    enable.samplerAnisotropy = have.samplerAnisotropy;
    enable.shaderImageGatherExtended = have.shaderImageGatherExtended;   /* (gathers with offsets: the SPIR-V declares it) */
    enable.textureCompressionBC = have.textureCompressionBC;
    enable.fragmentStoresAndAtomics = have.fragmentStoresAndAtomics;
    enable.vertexPipelineStoresAndAtomics = have.vertexPipelineStoresAndAtomics;
    enable.imageCubeArray = have.imageCubeArray;
    enable.shaderClipDistance = have.shaderClipDistance;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, queue_family, 1, &priority};
    enabled = enable;
    VkPhysicalDeviceShaderDrawParametersFeatures drawp_on = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES, NULL, VK_TRUE};
    VkPhysicalDeviceDynamicRenderingFeaturesKHR dynamic_on = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR,
                                                              drawp.shaderDrawParameters ? &drawp_on : NULL, VK_TRUE};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &dynamic_on, 0, 1, &qci, 0, NULL, nwant, want_ext, &enable};
    if ((r = vkCreateDevice(physical, &dci, NULL, &device)) != VK_SUCCESS) { snprintf(why, size, "%s: vkCreateDevice failed (%d)", card, (int)r); return start_undo(); }
#define LOAD_D(name) vk##name = (PFN_vk##name)vkGetDeviceProcAddr(device, "vk" #name);
    VK_DEVICE_FUNCS(LOAD_D)
#undef LOAD_D
    if (!vkCmdBeginRenderingKHR || !vkCmdEndRenderingKHR) { snprintf(why, size, "%s: the driver lacks vkCmdBeginRenderingKHR", card); return start_undo(); }
    vkGetDeviceQueue(device, queue_family, 0, &queue);
    limits = props.limits;
    limits_vendor = props.vendorID;
    limits_device = props.deviceID;
    texel_align = limits.minTexelBufferOffsetAlignment > 16 ? limits.minTexelBufferOffsetAlignment : 16;
    ubo_align = limits.minUniformBufferOffsetAlignment > 16 ? limits.minUniformBufferOffsetAlignment : 16;
    ssbo_align = limits.minStorageBufferOffsetAlignment > 16 ? limits.minStorageBufferOffsetAlignment : 16;
    granularity = limits.bufferImageGranularity > 256 ? limits.bufferImageGranularity : 256;

    /* frames: as many in flight as display.ini's latency */
    nframes = (u32)display_latency();
    if (nframes < 1) nframes = 1;
    if (nframes > MAX_FRAMES) nframes = MAX_FRAMES;
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, queue_family};
    if (vkCreateCommandPool(device, &pci, NULL, &pool) != VK_SUCCESS) { snprintf(why, size, "%s: no command pool", card); return start_undo(); }
    for (u32 i = 0; i < MAX_FRAMES; i++) {
        VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0};
        VkSemaphoreCreateInfo sem = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, NULL, 0};
        if (vkAllocateCommandBuffers(device, &cai, &frames[i].cmd) != VK_SUCCESS || vkCreateFence(device, &fci, NULL, &frames[i].done) != VK_SUCCESS ||
            vkCreateSemaphore(device, &sem, NULL, &frames[i].acquired) != VK_SUCCESS) {
            snprintf(why, size, "%s: cannot make the frames' command buffers", card);
            return start_undo();
        }
    }

    if (!draw_objects()) { snprintf(why, size, "%s: cannot make the drawing's descriptor layouts or stand-in resources", card); return start_undo(); }

    /* present modes: immediate is what vrr needs (tearing) */
    VkPresentModeKHR modes[16];
    uint32_t nmodes = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical, vk_surface, &nmodes, modes);
    for (uint32_t i = 0; i < nmodes; i++) {
        have_immediate |= modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR;
        have_mailbox |= modes[i] == VK_PRESENT_MODE_MAILBOX_KHR;
    }
    display_set_tearing(have_immediate);
    VkSurfaceFormatKHR sf[64];
    uint32_t nsf = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, vk_surface, &nsf, sf);
    int bgra = 0;
    for (uint32_t i = 0; i < nsf; i++) bgra |= sf[i].format == VK_FORMAT_B8G8R8A8_UNORM;
    swap_format = bgra || !nsf ? VK_FORMAT_B8G8R8A8_UNORM : sf[0].format;
    /* The render scale (display.ini render_scale; auto: the picture's height in the window now over
     * the game's 1080), fixed from here on, as gpu_d3d11.c's */
    float scale = display_render_scale();
    if (scale == 0) {
        scale = 1;
        VkSurfaceCapabilitiesKHR caps;
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, vk_surface, &caps) == VK_SUCCESS && caps.currentExtent.width != 0xFFFFFFFFu) {
            unsigned fx, fy, fw, fh;
            display_fit(caps.currentExtent.width, caps.currentExtent.height, &fx, &fy, &fw, &fh);
            scale = (float)fh / 1080.0f;
            if (scale < 1) scale = 1;
            if (scale > 4) scale = 4;
        }
    }
    if (scale < 0.5f || scale > 4) scale = 1;
    render_scale = scale;
    fprintf(stderr, "[vulkan] drawing with %s (Vulkan %u.%u.%u)%s: %u frames in flight, render scale %.3g, present modes%s%s\n",
            card, VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion),
            float_controls ? ", float controls" : "", nframes, render_scale, have_immediate ? " immediate" : "", have_mailbox ? " mailbox" : "");
    makers_start();
    warm_start();
    return &VULKAN;
}

#else

const struct GfxBackend *vk_backend_start(char *why, size_t size, int automatic) {
    (void)automatic;
    snprintf(why, size, "this program has no Vulkan back end (Windows builds with the Vulkan headers have it)");
    return NULL;
}

#endif
