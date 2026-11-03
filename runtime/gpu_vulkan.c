/* The Vulkan back end, in development (docs/TECHNICAL.md, "A second back end: Vulkan").
 *
 * It will be a second implementation of gpu_backend.h beside gpu_d3d11.c, as macos/gpu_metal.m is
 * for Metal. For now it is the check that display.ini's renderer = vulkan asks for at the
 * renderer's start (gpu_renderer.c): whether this PC can run it - the system's Vulkan loader, an
 * instance, the card display.ini's gpu names, a graphics queue that presents to the game's window,
 * a device - with everything it made let go again before Direct3D 11 makes its own.
 *
 * Built with SSBU_HAVE_VULKAN (tools/cc.py, when the headers in third_party/vulkan-headers are
 * there). The program links no Vulkan library: the loader is the system's, loaded at run time
 * (vulkan-1.dll, which graphics drivers install; libvulkan.so.1 elsewhere), so the program still
 * starts on a PC without Vulkan. Without SSBU_HAVE_VULKAN (the Linux and macOS builds for now)
 * vk_probe says the program has none. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "display.h"
#include "gpu_vulkan.h"

#ifndef SSBU_HAVE_VULKAN

int vk_probe(void *window, char *what, size_t size) {
    (void)window;
    snprintf(what, size, "this program was built without Vulkan");
    return 0;
}

void *vk_loader(char *why, size_t size) {
    snprintf(why, size, "this program was built without Vulkan");
    return NULL;
}

#else

#define VK_NO_PROTOTYPES
#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>
#ifndef _WIN32
#include <dlfcn.h>
#endif

#ifdef _WIN32
#define LOADER_NAME "vulkan-1.dll"
#else
#define LOADER_NAME "libvulkan.so.1"
#endif

/* ---- the loader: loaded once and kept (a driver may not take being unloaded well) ---------- */
static PFN_vkGetInstanceProcAddr get_instance_proc;

static PFN_vkGetInstanceProcAddr load_loader(char *why, size_t size) {
    static int tried;
    static char failed[200];
    if (tried) {
        if (!get_instance_proc) snprintf(why, size, "%s", failed);
        return get_instance_proc;
    }
    tried = 1;
    const char *name = getenv("SSBU_VULKAN_LOADER");
    if (!name || !*name) name = LOADER_NAME;
#ifdef _WIN32
    HMODULE lib = LoadLibraryA(name);
    if (lib) get_instance_proc = (PFN_vkGetInstanceProcAddr)(void (*)(void))GetProcAddress(lib, "vkGetInstanceProcAddr");
#else
    void *lib = dlopen(name, RTLD_NOW | RTLD_LOCAL);
    if (lib) get_instance_proc = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#endif
    if (!lib) snprintf(failed, sizeof failed, "no Vulkan loader (%s): the graphics driver installs it", name);
    else if (!get_instance_proc) snprintf(failed, sizeof failed, "%s has no vkGetInstanceProcAddr", name);
    if (!get_instance_proc) snprintf(why, size, "%s", failed);
    return get_instance_proc;
}

void *vk_loader(char *why, size_t size) { return (void *)load_loader(why, size); }

/* ---- the functions the check uses ---------------------------------------------------------- */
typedef struct Vk {
    PFN_vkEnumerateInstanceVersion EnumerateInstanceVersion;
    PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties;
    PFN_vkCreateInstance CreateInstance;
    PFN_vkDestroyInstance DestroyInstance;
    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceFeatures GetPhysicalDeviceFeatures;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties;
    PFN_vkCreateDevice CreateDevice;
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
    PFN_vkDestroySurfaceKHR DestroySurfaceKHR;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR GetPhysicalDeviceSurfaceSupportKHR;
#ifdef _WIN32
    PFN_vkCreateWin32SurfaceKHR CreateWin32SurfaceKHR;
#endif
} Vk;

static int has_extension(const VkExtensionProperties *list, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(list[i].extensionName, name)) return 1;
    return 0;
}

static const char *kind_name(VkPhysicalDeviceType t) {
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return "software";
    default: return "other";
    }
}

/* Fastest kind first, as display.ini's gpu = auto picks: discrete, integrated, virtual, other, software. */
static int kind_rank(VkPhysicalDeviceType t) {
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 4;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return 0;
    default: return 1;
    }
}

static void driver_version(const VkPhysicalDeviceProperties *p, char *out, size_t size) {
    uint32_t v = p->driverVersion;
    if (p->vendorID == 0x10DE)   /* NVIDIA: 10.8.8.6 bits */
        snprintf(out, size, "%u.%u", v >> 22, (v >> 14) & 0xFF);
#ifdef _WIN32
    else if (p->vendorID == 0x8086)   /* Intel on Windows: 18.14 bits */
        snprintf(out, size, "%u.%u", v >> 14, v & 0x3FFF);
#endif
    else snprintf(out, size, "%u.%u.%u", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
}

static int contains_lower(const char *hay, const char *needle) {
    char h[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE], n[64];
    size_t i;
    for (i = 0; hay[i] && i + 1 < sizeof h; i++) h[i] = (char)(hay[i] >= 'A' && hay[i] <= 'Z' ? hay[i] + 32 : hay[i]);
    h[i] = 0;
    for (i = 0; needle[i] && i + 1 < sizeof n; i++) n[i] = (char)(needle[i] >= 'A' && needle[i] <= 'Z' ? needle[i] + 32 : needle[i]);
    n[i] = 0;
    return strstr(h, n) != NULL;
}

/* The device features the back end will ask for (for the draws gpu_d3d11.c makes with Direct3D 11's
 * feature level 11): those this card lacks, in words, into `out` ("" when it has them all). */
static void missing_features(const VkPhysicalDeviceFeatures *f, char *out, size_t size) {
    static const struct { size_t at; const char *name; } WANTED[] = {
        {offsetof(VkPhysicalDeviceFeatures, independentBlend), "independentBlend"},
        {offsetof(VkPhysicalDeviceFeatures, dualSrcBlend), "dualSrcBlend"},
        {offsetof(VkPhysicalDeviceFeatures, depthClamp), "depthClamp"},
        {offsetof(VkPhysicalDeviceFeatures, samplerAnisotropy), "samplerAnisotropy"},
        {offsetof(VkPhysicalDeviceFeatures, textureCompressionBC), "textureCompressionBC"},
        {offsetof(VkPhysicalDeviceFeatures, fragmentStoresAndAtomics), "fragmentStoresAndAtomics"},
        {offsetof(VkPhysicalDeviceFeatures, imageCubeArray), "imageCubeArray"},
    };
    out[0] = 0;
    for (size_t i = 0; i < sizeof WANTED / sizeof *WANTED; i++) {
        VkBool32 on;
        memcpy(&on, (const char *)f + WANTED[i].at, sizeof on);
        if (!on) {
            size_t l = strlen(out);
            snprintf(out + l, size - l, "%s%s", l ? ", " : "", WANTED[i].name);
        }
    }
}

int vk_probe(void *window, char *what, size_t size) {
    what[0] = 0;
    PFN_vkGetInstanceProcAddr gipa = load_loader(what, size);
    if (!gipa) return 0;
    Vk vk;
    memset(&vk, 0, sizeof vk);
#define GLOBAL(name) vk.name = (PFN_vk##name)gipa(NULL, "vk" #name)
    GLOBAL(EnumerateInstanceVersion);
    GLOBAL(EnumerateInstanceExtensionProperties);
    GLOBAL(CreateInstance);
#undef GLOBAL
    if (!vk.CreateInstance || !vk.EnumerateInstanceExtensionProperties) { snprintf(what, size, "the Vulkan loader lacks vkCreateInstance"); return 0; }
    uint32_t loader_version = VK_API_VERSION_1_0;
    if (vk.EnumerateInstanceVersion && vk.EnumerateInstanceVersion(&loader_version) != VK_SUCCESS) loader_version = VK_API_VERSION_1_0;
    if (loader_version < VK_API_VERSION_1_1) {
        snprintf(what, size, "the Vulkan loader is version %u.%u: the Vulkan renderer needs 1.1 or newer", VK_API_VERSION_MAJOR(loader_version),
                 VK_API_VERSION_MINOR(loader_version));
        return 0;
    }

    /* The instance, with the window surface extensions when they are there (a window to check) */
    uint32_t next = 0;
    vk.EnumerateInstanceExtensionProperties(NULL, &next, NULL);
    VkExtensionProperties *iext = calloc(next ? next : 1, sizeof *iext);
    if (!iext) { snprintf(what, size, "out of memory"); return 0; }
    vk.EnumerateInstanceExtensionProperties(NULL, &next, iext);
    const char *want_ext[2];
    uint32_t nwant = 0;
    int surfaces = 0;
#ifdef _WIN32
    surfaces = has_extension(iext, next, VK_KHR_SURFACE_EXTENSION_NAME) && has_extension(iext, next, VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
    if (surfaces) { want_ext[nwant++] = VK_KHR_SURFACE_EXTENSION_NAME; want_ext[nwant++] = VK_KHR_WIN32_SURFACE_EXTENSION_NAME; }
#endif
    free(iext);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "OpenSmash", 1, "OpenSmash runtime", 1, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, nwant, want_ext};
    VkInstance inst = VK_NULL_HANDLE;
    VkResult vr = vk.CreateInstance(&ici, NULL, &inst);
    if (vr != VK_SUCCESS) {
        snprintf(what, size, "vkCreateInstance failed (%d): no Vulkan driver for this graphics card%s", (int)vr,
                 vr == VK_ERROR_INCOMPATIBLE_DRIVER ? " (incompatible driver)" : "");
        return 0;
    }
#define INSTANCE(name) vk.name = (PFN_vk##name)gipa(inst, "vk" #name)
    INSTANCE(DestroyInstance);
    INSTANCE(EnumeratePhysicalDevices);
    INSTANCE(GetPhysicalDeviceProperties);
    INSTANCE(GetPhysicalDeviceFeatures);
    INSTANCE(GetPhysicalDeviceQueueFamilyProperties);
    INSTANCE(EnumerateDeviceExtensionProperties);
    INSTANCE(CreateDevice);
    INSTANCE(GetDeviceProcAddr);
    INSTANCE(DestroySurfaceKHR);
    INSTANCE(GetPhysicalDeviceSurfaceSupportKHR);
#ifdef _WIN32
    INSTANCE(CreateWin32SurfaceKHR);
#endif
#undef INSTANCE
    int ok = 0;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice *devs = NULL;
    VkDevice device = VK_NULL_HANDLE;
    if (!vk.DestroyInstance || !vk.EnumeratePhysicalDevices || !vk.GetPhysicalDeviceProperties || !vk.GetPhysicalDeviceFeatures ||
        !vk.GetPhysicalDeviceQueueFamilyProperties || !vk.EnumerateDeviceExtensionProperties || !vk.CreateDevice || !vk.GetDeviceProcAddr) {
        snprintf(what, size, "the Vulkan instance lacks core functions");
        if (vk.DestroyInstance) vk.DestroyInstance(inst, NULL);
        return 0;
    }

    /* The game's window as a surface, to ask which queue can show pictures in it */
#ifdef _WIN32
    if (window && surfaces && vk.CreateWin32SurfaceKHR && vk.GetPhysicalDeviceSurfaceSupportKHR && vk.DestroySurfaceKHR) {
        VkWin32SurfaceCreateInfoKHR sci = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, NULL, 0, GetModuleHandleW(NULL), (HWND)window};
        if ((vr = vk.CreateWin32SurfaceKHR(inst, &sci, NULL, &surface)) != VK_SUCCESS) {
            snprintf(what, size, "vkCreateWin32SurfaceKHR on the game's window failed (%d)", (int)vr);
            goto done;
        }
    }
#endif
    if (window && surface == VK_NULL_HANDLE) {
        snprintf(what, size, "this Vulkan driver cannot draw into a window (no %s)",
#ifdef _WIN32
                 VK_KHR_WIN32_SURFACE_EXTENSION_NAME
#else
                 "window surface support in this build"
#endif
        );
        goto done;
    }

    /* The cards: logged, and one chosen as display.ini's gpu says (auto: the fastest kind) */
    uint32_t ndev = 0;
    if (vk.EnumeratePhysicalDevices(inst, &ndev, NULL) != VK_SUCCESS || !ndev) { snprintf(what, size, "Vulkan lists no graphics card"); goto done; }
    devs = calloc(ndev, sizeof *devs);
    if (!devs) { snprintf(what, size, "out of memory"); goto done; }
    vk.EnumeratePhysicalDevices(inst, &ndev, devs);
    const char *want = display_gpu();
    int any = !want || !*want || !_stricmp(want, "auto") || !_stricmp(want, "warp");
    int best = -1, named = -1, best_rank = -1;
    char list[1024] = "";
    for (uint32_t i = 0; i < ndev; i++) {
        VkPhysicalDeviceProperties p;
        vk.GetPhysicalDeviceProperties(devs[i], &p);
        int again = 0;   /* (a card can be listed twice, e.g. by two of its drivers' entries) */
        for (uint32_t j = 0; j < i && !again; j++) {
            VkPhysicalDeviceProperties q;
            vk.GetPhysicalDeviceProperties(devs[j], &q);
            again = q.vendorID == p.vendorID && q.deviceID == p.deviceID && !strcmp(q.deviceName, p.deviceName);
        }
        if (again) continue;
        char drv[32];
        driver_version(&p, drv, sizeof drv);
        size_t l = strlen(list);
        snprintf(list + l, sizeof list - l, "%s%s (%s, vendor %#x, Vulkan %u.%u.%u, driver %s)", l ? "; " : "", p.deviceName, kind_name(p.deviceType),
                 p.vendorID, VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion), drv);
        int rank = kind_rank(p.deviceType);
        if (rank > best_rank) { best_rank = rank; best = (int)i; }
        if (!any && named < 0 && p.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU && contains_lower(p.deviceName, want)) named = (int)i;
    }
    fprintf(stderr, "[vulkan] loader %u.%u.%u; graphics cards: %s\n", VK_API_VERSION_MAJOR(loader_version), VK_API_VERSION_MINOR(loader_version),
            VK_API_VERSION_PATCH(loader_version), list);
    if (!any && named < 0) fprintf(stderr, "[vulkan] display.ini: no graphics card's name contains '%s': using the fastest\n", want);
    if (!_stricmp(want ? want : "", "warp")) fprintf(stderr, "[vulkan] display.ini: gpu = warp is Direct3D's software renderer: using the fastest\n");
    VkPhysicalDevice pd = devs[named >= 0 ? named : best];
    VkPhysicalDeviceProperties props;
    vk.GetPhysicalDeviceProperties(pd, &props);
    char drv[32];
    driver_version(&props, drv, sizeof drv);
    char card[400];
    snprintf(card, sizeof card, "%s (%s, Vulkan %u.%u.%u, driver %s)", props.deviceName, kind_name(props.deviceType), VK_API_VERSION_MAJOR(props.apiVersion),
             VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion), drv);
    if (props.apiVersion < VK_API_VERSION_1_1) { snprintf(what, size, "%s: the Vulkan renderer needs Vulkan 1.1 or newer", card); goto done; }

    /* A queue family that draws (and shows pictures in the window, when there is one) */
    uint32_t nfam = 0;
    vk.GetPhysicalDeviceQueueFamilyProperties(pd, &nfam, NULL);
    VkQueueFamilyProperties fams[32];
    if (nfam > 32) nfam = 32;
    vk.GetPhysicalDeviceQueueFamilyProperties(pd, &nfam, fams);
    int family = -1;
    for (uint32_t i = 0; i < nfam && family < 0; i++) {
        if (!(fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !fams[i].queueCount) continue;
        VkBool32 shows = VK_TRUE;
        if (surface != VK_NULL_HANDLE && vk.GetPhysicalDeviceSurfaceSupportKHR(pd, i, surface, &shows) != VK_SUCCESS) shows = VK_FALSE;
        if (shows) family = (int)i;
    }
    if (family < 0) {
        snprintf(what, size, "%s: no queue that draws%s", card, surface != VK_NULL_HANDLE ? " and shows pictures in the game's window" : "");
        goto done;
    }

    /* Swap chains, and the features the draws will want */
    uint32_t ndext = 0;
    vk.EnumerateDeviceExtensionProperties(pd, NULL, &ndext, NULL);
    VkExtensionProperties *dext = calloc(ndext ? ndext : 1, sizeof *dext);
    if (!dext) { snprintf(what, size, "out of memory"); goto done; }
    vk.EnumerateDeviceExtensionProperties(pd, NULL, &ndext, dext);
    int swapchain = has_extension(dext, ndext, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    free(dext);
    if (!swapchain) { snprintf(what, size, "%s: no %s", card, VK_KHR_SWAPCHAIN_EXTENSION_NAME); goto done; }
    VkPhysicalDeviceFeatures have, enable;
    vk.GetPhysicalDeviceFeatures(pd, &have);
    char lacks[300];
    missing_features(&have, lacks, sizeof lacks);
    memset(&enable, 0, sizeof enable);
    enable.independentBlend = have.independentBlend;
    enable.dualSrcBlend = have.dualSrcBlend;
    enable.depthClamp = have.depthClamp;
    enable.samplerAnisotropy = have.samplerAnisotropy;
    enable.textureCompressionBC = have.textureCompressionBC;
    enable.fragmentStoresAndAtomics = have.fragmentStoresAndAtomics;
    enable.imageCubeArray = have.imageCubeArray;

    /* The device itself, made and let go */
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, (uint32_t)family, 1, &priority};
    const char *dev_ext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 1, dev_ext, &enable};
    if ((vr = vk.CreateDevice(pd, &dci, NULL, &device)) != VK_SUCCESS) {
        snprintf(what, size, "%s: vkCreateDevice failed (%d)", card, (int)vr);
        device = VK_NULL_HANDLE;
        goto done;
    }
    PFN_vkDestroyDevice destroy_device = (PFN_vkDestroyDevice)vk.GetDeviceProcAddr(device, "vkDestroyDevice");
    if (destroy_device) destroy_device(device, NULL);
    device = VK_NULL_HANDLE;
    snprintf(what, size, "%s: a device made%s%s%s", card, surface != VK_NULL_HANDLE ? ", it shows pictures in the game's window" : "",
             lacks[0] ? "; it lacks " : "", lacks);
    ok = 1;

done:
    free(devs);
    if (surface != VK_NULL_HANDLE) vk.DestroySurfaceKHR(inst, surface, NULL);
    vk.DestroyInstance(inst, NULL);
    return ok;
}

#endif
