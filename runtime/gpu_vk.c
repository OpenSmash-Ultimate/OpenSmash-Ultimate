/* The rendering back end: Vulkan, in development (docs/TECHNICAL.md, "A second back end: Vulkan").
 *
 * A second implementation of gpu_backend.h beside gpu_d3d11.c, chosen at the first GPU work when
 * display.ini's renderer = vulkan (gpu_backend.c); if it cannot start, Direct3D 11 draws. As in
 * gpu_d3d11.c the game's render targets live on the host GPU, each a Vulkan image keyed by the guest
 * address the console would have rendered into, made the render scale's times the game's size; when
 * the display shows one, it is drawn into the window (16:9, centred) and presented.
 *
 * What it draws so far (the plan's step 4): the clears, the 2D engine's blits and the frames shown -
 * swap chain on the game's window, pacing (clock: mailbox; vrr and SSBU_SPEED: immediate; display:
 * the monitor's refreshes through DXGI, as Direct3D 11 waits for them), the window's size and F11,
 * frames queued as display.ini's latency says, SSBU_CAPTURE and F3 pictures. Draws and compute
 * launches are counted, not drawn yet (step 5); the overlay and the settings menu are not drawn
 * (step 7).
 *
 * Every image is kept in the GENERAL layout and every operation is followed by a full memory
 * barrier: simple and correct, to be narrowed once draws make it matter. The work of a frame is one
 * command buffer, submitted at the present. Every entry point takes the back end's lock, as
 * gpu_d3d11.c's do. */
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
#include "gpu_vulkan.h"
#include "gpu_window.h"

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

extern u64 gfx_stat_clears, gfx_stat_presented, gfx_stat_blits;   /* (gpu_d3d11.c's counters, printed by host.c: either back end counts) */

/* ---- the Vulkan functions used ------------------------------------------------------------ */
#define VK_INSTANCE_FUNCS(X) X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) \
    X(GetPhysicalDeviceFeatures) X(GetPhysicalDeviceQueueFamilyProperties) X(GetPhysicalDeviceMemoryProperties) \
    X(GetPhysicalDeviceFormatProperties) X(EnumerateDeviceExtensionProperties) X(CreateDevice) X(GetDeviceProcAddr) \
    X(CreateWin32SurfaceKHR) X(DestroySurfaceKHR) X(GetPhysicalDeviceSurfaceSupportKHR) \
    X(GetPhysicalDeviceSurfaceCapabilitiesKHR) X(GetPhysicalDeviceSurfaceFormatsKHR) X(GetPhysicalDeviceSurfacePresentModesKHR)
#define VK_DEVICE_FUNCS(X) X(GetDeviceQueue) X(DeviceWaitIdle) X(QueueSubmit) X(QueueWaitIdle) \
    X(CreateCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) X(ResetCommandBuffer) \
    X(CreateFence) X(WaitForFences) X(ResetFences) X(CreateSemaphore) X(DestroySemaphore) \
    X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(BindImageMemory) \
    X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) \
    X(AllocateMemory) X(FreeMemory) X(MapMemory) X(UnmapMemory) \
    X(CmdPipelineBarrier) X(CmdClearColorImage) X(CmdClearDepthStencilImage) X(CmdBlitImage) X(CmdCopyImageToBuffer) \
    X(CreateSwapchainKHR) X(DestroySwapchainKHR) X(GetSwapchainImagesKHR) X(AcquireNextImageKHR) X(QueuePresentKHR)
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

/* ---- frames: command buffers in flight ------------------------------------------------------ */
#define MAX_FRAMES 3
typedef struct Grave { VkImage image; VkBuffer buffer; VkDeviceMemory memory; } Grave;
typedef struct Frame {
    VkCommandBuffer cmd;
    VkFence done;
    VkSemaphore acquired;
    int recording, submitted;
    Grave *graves;   /* released once this frame's work is done */
    u32 ngraves, graves_cap;
} Frame;
static Frame frames[MAX_FRAMES];
static u32 nframes = 2, frame_at;
static VkCommandPool pool;

static u64 vk_stat_draws, vk_stat_dispatches, vk_stat_clears_skipped, vk_stat_blits_skipped, vk_stat_presents_skipped;

static void bury(VkImage image, VkBuffer buffer, VkDeviceMemory memory) {
    Frame *f = &frames[frame_at];
    if (f->ngraves == f->graves_cap) {
        u32 cap = f->graves_cap * 2 + 32;
        Grave *g = realloc(f->graves, sizeof *g * cap);
        if (!g) return;   /* (leaked rather than freed while the GPU may use it) */
        f->graves = g;
        f->graves_cap = cap;
    }
    f->graves[f->ngraves++] = (Grave){image, buffer, memory};
}

static void check(VkResult r, const char *doing) {
    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_OUT_OF_DATE_KHR) return;
    if (r == VK_ERROR_DEVICE_LOST)
        rt_fatal("the graphics device was lost while %s (Vulkan, VK_ERROR_DEVICE_LOST). Nothing can be drawn after that: start the game again.", doing);
    fprintf(stderr, "[vulkan] %s failed (%d)\n", doing, (int)r);
}

/* The frame's command buffer, begun the first time it is asked for (after the GPU is done with the
 * slot's previous work). */
static VkCommandBuffer cmd(void) {
    Frame *f = &frames[frame_at];
    if (f->recording) return f->cmd;
    if (f->submitted) {
        check(vkWaitForFences(device, 1, &f->done, VK_TRUE, UINT64_MAX), "waiting for a frame");
        f->submitted = 0;
    }
    for (u32 i = 0; i < f->ngraves; i++) {
        if (f->graves[i].image) vkDestroyImage(device, f->graves[i].image, NULL);
        if (f->graves[i].buffer) vkDestroyBuffer(device, f->graves[i].buffer, NULL);
        if (f->graves[i].memory) vkFreeMemory(device, f->graves[i].memory, NULL);
    }
    f->ngraves = 0;
    vkResetCommandBuffer(f->cmd, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    check(vkBeginCommandBuffer(f->cmd, &bi), "starting a frame's commands");
    f->recording = 1;
    return f->cmd;
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
                              VK_QUEUE_FAMILY_IGNORED, image, {aspect, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

static int memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < memory_props.memoryTypeCount; i++)
        if ((bits >> i & 1) && (memory_props.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
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
    VkDeviceMemory memory;
    u64 used;
} Surface;
static Surface surfaces[2048];
static u32 nsurfaces;
static u64 tick;

static void release_surface(Surface *s) {
    bury(s->image, VK_NULL_HANDLE, s->memory);
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
    VkImage image;
    if (vkCreateImage(device, &ici, NULL, &image) != VK_SUCCESS) return NULL;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(device, image, &mr);
    int type = memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, (uint32_t)type};
    VkDeviceMemory mem;
    if (type < 0 || vkAllocateMemory(device, &mai, NULL, &mem) != VK_SUCCESS) { vkDestroyImage(device, image, NULL); return NULL; }
    vkBindImageMemory(device, image, mem, 0);
    memset(s, 0, sizeof *s);
    s->guest = guest; s->width = width; s->height = height; s->format = format; s->depth = depth;
    s->hw = hw; s->hh = hh;
    s->storage = cf.storage; s->view = cf.view; s->klass = cf.klass; s->srgb = cf.srgb;
    s->image = image; s->memory = mem;
    s->used = ++tick;
    VkCommandBuffer c = cmd();
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
static void vk_clear(const u32 *regs, u32 arg) {
    enter();
    gfx_stat_clears++;
    u32 rt = (arg >> 6) & 0xF;
    if (arg & 0x3C && rt < 8) {   /* any of R, G, B, A */
        u64 va = (u64)RT(rt, 0) << 32 | RT(rt, 1);
        Surface *s = surface(gpu_va_to_guest(va), RT(rt, 2), RT(rt, 3), RT(rt, 4), 0);
        if (s && ((arg >> 2) & 0xF) == 0xF) {
            float c[4] = {f32_of(regs[0x360]), f32_of(regs[0x361]), f32_of(regs[0x362]), f32_of(regs[0x363])};
            VkClearColorValue v;
            if (s->klass == CLASS_SINT) for (int i = 0; i < 4; i++) v.int32[i] = (int32_t)c[i];
            else if (s->klass == CLASS_UINT) for (int i = 0; i < 4; i++) v.uint32[i] = c[i] <= 0 ? 0 : (uint32_t)c[i];
            else for (int i = 0; i < 4; i++) v.float32[i] = s->srgb && i < 3 ? to_srgb(c[i]) : c[i];   /* (stored plain: written as an sRGB view would) */
            VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkCommandBuffer cb = cmd();
            vkCmdClearColorImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
            sync_all(cb);
        } else if (s) vk_stat_clears_skipped++;   /* only some channels: a masked draw (step 5) */
    }
    if (arg & 3) {   /* depth, stencil */
        u64 va = (u64)regs[0x3F8] << 32 | regs[0x3F9];
        Surface *s = surface(gpu_va_to_guest(va), regs[0x48A], regs[0x48B], regs[0x3FA], 1);
        if (s) {
            VkImageAspectFlags a = (arg & 1 ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (arg & 2 && has_stencil(s->storage) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
            if (a) {
                VkClearDepthStencilValue z = {f32_of(regs[0x364]), regs[0x368] & 0xFF};
                VkImageSubresourceRange range = {a, 0, 1, 0, 1};
                VkCommandBuffer cb = cmd();
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
    VkCommandBuffer c = cmd();
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

/* A copy of the surface's whole image into a buffer the CPU can read, recorded now. */
static int readback(Surface *s, Readback *rb, const char *path) {
    if (s->storage != VK_FORMAT_R8G8B8A8_UNORM && s->storage != VK_FORMAT_B8G8R8A8_UNORM) return 0;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, (VkDeviceSize)s->hw * s->hh * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    if (vkCreateBuffer(device, &bci, NULL, &rb->buffer) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(device, rb->buffer, &mr);
    int type = memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, (uint32_t)type};
    if (type < 0 || vkAllocateMemory(device, &mai, NULL, &rb->memory) != VK_SUCCESS) { vkDestroyBuffer(device, rb->buffer, NULL); return 0; }
    vkBindBufferMemory(device, rb->buffer, rb->memory, 0);
    VkBufferImageCopy region = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {s->hw, s->hh, 1}};
    VkCommandBuffer c = cmd();
    vkCmdCopyImageToBuffer(c, s->image, VK_IMAGE_LAYOUT_GENERAL, rb->buffer, 1, &region);
    sync_all(c);
    rb->w = s->hw; rb->h = s->hh; rb->format = s->storage;
    snprintf(rb->path, sizeof rb->path, "%s", path);
    return 1;
}

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

/* SSBU_CAPTURE=<dir> (with SSBU_CAPTURE_MAX, SSBU_CAPTURE_EVERY), as gpu_d3d11.c: the path of this
 * frame's picture, or NULL. */
static const char *capture_path(char *path, size_t size) {
    static const char *dir;
    static int checked, count, most = 40;
    static u64 last, every = 3000000000ull;
    if (!checked++) {
        dir = getenv("SSBU_CAPTURE");
        if (getenv("SSBU_CAPTURE_MAX")) most = atoi(getenv("SSBU_CAPTURE_MAX"));
        if (getenv("SSBU_CAPTURE_EVERY")) every = (u64)(atof(getenv("SSBU_CAPTURE_EVERY")) * 1e9);
    }
    if (!dir || !*dir || count >= most || rt_now_ns() - last < every) return NULL;
    last = rt_now_ns();
    snprintf(path, size, "%s/vk_frame_%03d.bmp", dir, count++);
    return path;
}

/* ---- frames shown ------------------------------------------------------------------------------- */
static void submit(VkSemaphore wait, VkSemaphore signal) {
    Frame *f = &frames[frame_at];
    if (!f->recording) cmd();
    check(vkEndCommandBuffer(f->cmd), "ending a frame's commands");
    f->recording = 0;
    vkResetFences(device, 1, &f->done);
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, wait ? 1u : 0u, &wait, &stage, 1, &f->cmd, signal ? 1u : 0u, &signal};
    check(vkQueueSubmit(queue, 1, &si, f->done), "submitting a frame");
    f->submitted = 1;
}

static void vk_present(u64 guest, u32 width, u32 height) {
    (void)width; (void)height;
    enter();
    gfx_stat_presented++;
    unsigned bw, bh;
    if (display_take_size(&bw, &bh)) recreate = 1;   /* the window's new size (F11, a drag) */
    if (swapchain && wanted_mode() != present_mode) recreate = 1;
    if (recreate) {
        /* (the frame's work so far goes first: making a swap chain waits for the GPU) */
        if (frames[frame_at].recording) { submit(VK_NULL_HANDLE, VK_NULL_HANDLE); frame_at = (frame_at + 1) % nframes; }
        make_swapchain();
    }
    Frame *f = &frames[frame_at];
    VkCommandBuffer c = cmd();
    uint32_t index = 0;
    VkResult ar = swapchain ? vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, f->acquired, VK_NULL_HANDLE, &index) : VK_ERROR_OUT_OF_DATE_KHR;
    int showing = ar == VK_SUCCESS || ar == VK_SUBOPTIMAL_KHR;
    if (ar == VK_SUBOPTIMAL_KHR || ar == VK_ERROR_OUT_OF_DATE_KHR) recreate = 1;
    Surface *src = find_color(guest);
    Readback shot[2];
    int nshots = 0;
    char path[1024];
    LONG keys = (LONG)gfx_take_hotkeys();
    if (src && capture_path(path, sizeof path) && readback(src, &shot[nshots], path)) nshots++;
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
        image_layout(c, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue black = {{0, 0, 0, 1}}, grey = {{0.1f, 0.1f, 0.12f, 1}};
        vkCmdClearColorImage(c, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, src ? &black : &grey, 1, &range);
        if (src && (features_of(src->storage) & VK_FORMAT_FEATURE_BLIT_SRC_BIT) && src->klass == CLASS_FLOAT) {
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
    leave();
}

/* ---- what is not drawn yet (step 5), and the rest of the runtime's questions ------------------------ */
static void vk_draw(const u32 *regs, const CbBinding cb[STAGES][18], int indexed) { (void)regs; (void)cb; (void)indexed; vk_stat_draws++; }
static void vk_dispatch(const u32 *regs, const u32 *qmd) { (void)regs; (void)qmd; vk_stat_dispatches++; }
static int vk_copy_resident(u64 dst, u64 src, u64 bytes) { (void)dst; (void)src; (void)bytes; return 0; }
static void vk_range(u64 va, u64 bytes) { (void)va; (void)bytes; }
static void vk_nothing(void) {}
static int vk_draw_logging(void) { return 0; }
static DWORD vk_lock_owner(void) { return (DWORD)(uintptr_t)lock.OwningThread; }
static DWORD vk_last_enter_thread(void) { return last_enter_thread; }
static void vk_report_compiles(void (*line)(const char *text)) { (void)line; }

static void vk_status_lines(void) {
    fprintf(stderr, "[status] vulkan (%s): %llu frames shown (%llu with no window to show them in), %llu clears (%llu masked, not drawn), "
                    "%llu 2D blits (%llu between formats it cannot blit), %u surfaces; not drawn yet: %llu draws, %llu compute launches\n",
            card, gfx_stat_presented, vk_stat_presents_skipped, gfx_stat_clears, vk_stat_clears_skipped, gfx_stat_blits, vk_stat_blits_skipped,
            nsurfaces, vk_stat_draws, vk_stat_dispatches);
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

static void vk_closing(void) {}

static const GfxBackend VULKAN = {
    .clear = vk_clear,
    .draw = vk_draw,
    .present = vk_present,
    .blit2d = vk_blit2d,
    .dispatch = vk_dispatch,
    .copy_resident = vk_copy_resident,
    .resident_flush = vk_range,
    .resident_written = vk_range,
    .stream_wrote = vk_range,
    .guest_wrote = vk_range,
    .stream_end = vk_nothing,
    .gpu_unmapped = vk_range,
    .guest_unmapped = vk_range,
    .draw_logging = vk_draw_logging,
    .status_lines = vk_status_lines,
    .closing = vk_closing,
    .lock_owner = vk_lock_owner,
    .last_enter_thread = vk_last_enter_thread,
    .report_compiles = vk_report_compiles,
    .display_output = vk_display_output,
};

/* ---- the start ------------------------------------------------------------------------------------- */
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

const GfxBackend *vk_backend_start(char *why, size_t size) {
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
        return NULL;
    }

    /* the card: display.ini's gpu, else the fastest kind */
    VkPhysicalDevice devs[16];
    uint32_t ndev = 16;
    if (vkEnumeratePhysicalDevices(instance, &ndev, devs) < 0 || !ndev) { snprintf(why, size, "Vulkan lists no graphics card"); return NULL; }
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
    if (props.apiVersion < VK_API_VERSION_1_1) { snprintf(why, size, "%s has Vulkan %u.%u: 1.1 is needed", card, VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion)); return NULL; }
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
    if (family < 0) { snprintf(why, size, "%s: no queue that draws and shows pictures in the game's window", card); return NULL; }
    queue_family = (uint32_t)family;

    /* the device: swap chains, and float controls for the shaders (gpu_spirv.h) when it has them */
    VkExtensionProperties dext[512];
    uint32_t ndext = 512;
    vkEnumerateDeviceExtensionProperties(physical, NULL, &ndext, dext);
    if (!has_ext(dext, ndext, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) { snprintf(why, size, "%s: no swap chains", card); return NULL; }
    const char *want_ext[2] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    uint32_t nwant = 1;
    if (has_ext(dext, ndext, "VK_KHR_shader_float_controls")) want_ext[nwant++] = "VK_KHR_shader_float_controls";
    VkPhysicalDeviceFeatures have, enable;
    vkGetPhysicalDeviceFeatures(physical, &have);
    memset(&enable, 0, sizeof enable);
    enable.independentBlend = have.independentBlend;
    enable.dualSrcBlend = have.dualSrcBlend;
    enable.depthClamp = have.depthClamp;
    enable.samplerAnisotropy = have.samplerAnisotropy;
    enable.textureCompressionBC = have.textureCompressionBC;
    enable.fragmentStoresAndAtomics = have.fragmentStoresAndAtomics;
    enable.vertexPipelineStoresAndAtomics = have.vertexPipelineStoresAndAtomics;
    enable.imageCubeArray = have.imageCubeArray;
    enable.shaderClipDistance = have.shaderClipDistance;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, queue_family, 1, &priority};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, nwant, want_ext, &enable};
    if ((r = vkCreateDevice(physical, &dci, NULL, &device)) != VK_SUCCESS) { snprintf(why, size, "%s: vkCreateDevice failed (%d)", card, (int)r); return NULL; }
#define LOAD_D(name) vk##name = (PFN_vk##name)vkGetDeviceProcAddr(device, "vk" #name);
    VK_DEVICE_FUNCS(LOAD_D)
#undef LOAD_D
    vkGetDeviceQueue(device, queue_family, 0, &queue);

    /* frames: as many in flight as display.ini's latency */
    nframes = (u32)display_latency();
    if (nframes < 1) nframes = 1;
    if (nframes > MAX_FRAMES) nframes = MAX_FRAMES;
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, queue_family};
    if (vkCreateCommandPool(device, &pci, NULL, &pool) != VK_SUCCESS) { snprintf(why, size, "%s: no command pool", card); return NULL; }
    for (u32 i = 0; i < MAX_FRAMES; i++) {
        VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0};
        VkSemaphoreCreateInfo sem = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, NULL, 0};
        if (vkAllocateCommandBuffers(device, &cai, &frames[i].cmd) != VK_SUCCESS || vkCreateFence(device, &fci, NULL, &frames[i].done) != VK_SUCCESS ||
            vkCreateSemaphore(device, &sem, NULL, &frames[i].acquired) != VK_SUCCESS) {
            snprintf(why, size, "%s: cannot make the frames' command buffers", card);
            return NULL;
        }
    }

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
    fprintf(stderr, "[vulkan] drawing with %s (Vulkan %u.%u.%u)%s: %u frames in flight, render scale %.3g, present modes%s%s (in development: "
                    "clears, 2D blits and frames shown; draws not yet)\n",
            card, VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion),
            nwant > 1 ? ", float controls" : "", nframes, render_scale, have_immediate ? " immediate" : "", have_mailbox ? " mailbox" : "");
    return &VULKAN;
}

#else

const struct GfxBackend *vk_backend_start(char *why, size_t size) {
    snprintf(why, size, "this program has no Vulkan back end (Windows builds with the Vulkan headers have it)");
    return NULL;
}

#endif
