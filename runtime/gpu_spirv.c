/* SPIR-V for the Vulkan back end (gpu_spirv.h: the binding map). DXBC through vkd3d-shader: the
 * same DXBC the Direct3D 11 back end makes and caches (gpu_shader.c's HLSL, gpu_shader_cache.h's
 * files, tools/shader_aot.py's cache filled ahead of time). Or the HLSL through glslang, in
 * ssbu_glslang.dll (tools/glslang_shim.c), loaded here at the first ask. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_spirv.h"

#ifdef SSBU_HAVE_VKD3D
#include <vkd3d_types.h>
#undef VKD3D_IMPORT
#define VKD3D_IMPORT   /* (a static library: tools/vkd3d_lib.py) */
#include <vkd3d_shader.h>

/* (gpu_shader.h's stages, repeated so this file needs no runtime header) */
enum { STAGE_VERTEX, STAGE_PIXEL, STAGE_COMPUTE };

int spirv_have_vkd3d(void) { return 1; }

/* One binding a register of each kind, for one stage's visibility and set. */
static unsigned map_stage(struct vkd3d_shader_resource_binding *b, enum vkd3d_shader_visibility visibility, unsigned set) {
    /* (each binding for buffers and images alike: a register is one or the other in a program) */
    static const struct { enum vkd3d_shader_descriptor_type type; unsigned count, first; } KINDS[] = {
        {VKD3D_SHADER_DESCRIPTOR_TYPE_CBV, SPIRV_CBVS, SPIRV_BINDING_CBV(0)},
        {VKD3D_SHADER_DESCRIPTOR_TYPE_SRV, SPIRV_SRVS, SPIRV_BINDING_SRV(0)},
        {VKD3D_SHADER_DESCRIPTOR_TYPE_SAMPLER, SPIRV_SAMPLERS, SPIRV_BINDING_SAMPLER(0)},
        {VKD3D_SHADER_DESCRIPTOR_TYPE_UAV, SPIRV_UAVS, SPIRV_BINDING_UAV(0)},
    };
    unsigned n = 0;
    for (unsigned k = 0; k < sizeof KINDS / sizeof *KINDS; k++)
        for (unsigned r = 0; r < KINDS[k].count; r++, n++) {
            memset(&b[n], 0, sizeof b[n]);
            b[n].type = KINDS[k].type;
            b[n].register_index = r;
            b[n].shader_visibility = visibility;
            b[n].flags = VKD3D_SHADER_BINDING_FLAG_BUFFER | VKD3D_SHADER_BINDING_FLAG_IMAGE;
            b[n].binding.set = set;
            b[n].binding.binding = KINDS[k].first + r;
            b[n].binding.count = 1;
        }
    return n;
}

int spirv_from_dxbc(const void *dxbc, size_t size, int stage, SpirvCode *out, char *err, size_t err_size) {
    enum { PER_STAGE = SPIRV_CBVS + SPIRV_SRVS + SPIRV_SAMPLERS + SPIRV_UAVS };
    struct vkd3d_shader_resource_binding bindings[PER_STAGE];
    unsigned count = stage == STAGE_VERTEX ? map_stage(bindings, VKD3D_SHADER_VISIBILITY_VERTEX, SPIRV_SET_VERTEX)
                   : stage == STAGE_PIXEL ? map_stage(bindings, VKD3D_SHADER_VISIBILITY_PIXEL, SPIRV_SET_PIXEL)
                   : map_stage(bindings, VKD3D_SHADER_VISIBILITY_COMPUTE, SPIRV_SET_COMPUTE);
    struct vkd3d_shader_interface_info iface = {VKD3D_SHADER_STRUCTURE_TYPE_INTERFACE_INFO};
    iface.bindings = bindings;
    iface.binding_count = count;
    struct vkd3d_shader_spirv_target_info target = {VKD3D_SHADER_STRUCTURE_TYPE_SPIRV_TARGET_INFO};
    target.next = &iface;
    target.entry_point = "main";
    target.environment = VKD3D_SHADER_SPIRV_ENVIRONMENT_VULKAN_1_1;
    /* Shader Model 5's float rules (32-bit denormals flushed) need SPV_KHR_float_controls: the device
     * gets VK_KHR_shader_float_controls (core in Vulkan 1.2) */
    static const enum vkd3d_shader_spirv_extension EXTENSIONS[] = {VKD3D_SHADER_SPIRV_EXTENSION_KHR_FLOAT_CONTROLS};
    target.extensions = EXTENSIONS;
    target.extension_count = sizeof EXTENSIONS / sizeof *EXTENSIONS;
    /* buffers read or written as storage buffers (not texel buffers: no format, any size) */
    struct vkd3d_shader_compile_option options[] = {
        {VKD3D_SHADER_COMPILE_OPTION_API_VERSION, VKD3D_SHADER_API_VERSION_2_1},
        {VKD3D_SHADER_COMPILE_OPTION_STRIP_DEBUG, 1},
        {VKD3D_SHADER_COMPILE_OPTION_BUFFER_UAV, VKD3D_SHADER_COMPILE_OPTION_BUFFER_UAV_STORAGE_BUFFER},
    };
    struct vkd3d_shader_compile_info info = {VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO};
    info.next = &target;
    info.source.code = dxbc;
    info.source.size = size;
    info.source_type = VKD3D_SHADER_SOURCE_DXBC_TPF;
    info.target_type = VKD3D_SHADER_TARGET_SPIRV_BINARY;
    info.options = options;
    info.option_count = sizeof options / sizeof *options;
    info.log_level = VKD3D_SHADER_LOG_WARNING;
    struct vkd3d_shader_code code = {0};
    char *messages = NULL;
    int ret = vkd3d_shader_compile(&info, &code, &messages);
    if (err_size) snprintf(err, err_size, "%s", messages && *messages ? messages : ret < 0 ? "vkd3d-shader failed" : "");
    vkd3d_shader_free_messages(messages);
    out->code = NULL;
    out->size = 0;
    if (ret < 0 || !code.size) {
        /* (not freed: vkd3d-shader 2.1 frees a failed compile's code itself and leaves the pointer
         * in place, so freeing it again would free it twice) */
        if (err_size && !*err) snprintf(err, err_size, "vkd3d-shader made no code (%d)", ret);
        return 0;
    }
    if (!(out->code = malloc(code.size))) {
        vkd3d_shader_free_shader_code(&code);
        if (err_size) snprintf(err, err_size, "out of memory");
        return 0;
    }
    memcpy(out->code, code.code, code.size);
    out->size = code.size;
    vkd3d_shader_free_shader_code(&code);
    return 1;
}
#else
int spirv_have_vkd3d(void) { return 0; }

int spirv_from_dxbc(const void *dxbc, size_t size, int stage, SpirvCode *out, char *err, size_t err_size) {
    (void)dxbc; (void)size; (void)stage;
    out->code = NULL;
    out->size = 0;
    if (err_size) snprintf(err, err_size, "this build has no vkd3d-shader (tools/vkd3d_lib.py builds it for Windows)");
    return 0;
}
#endif

/* ---- glslang (ssbu_glslang.dll) ---------------------------------------------------------- */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef int (*HlslToSpirv)(int stage, const char *hlsl, unsigned **words, size_t *count, char *log, size_t log_size);
typedef void (*SpirvWordsFree)(unsigned *words);
static HlslToSpirv glslang_compile;
static SpirvWordsFree glslang_free;
static char glslang_why[300];
static INIT_ONCE glslang_once = INIT_ONCE_STATIC_INIT;

/* The DLL beside the program (not one found elsewhere on the search path). */
static BOOL CALLBACK glslang_load(PINIT_ONCE once, PVOID arg, PVOID *ctx) {
    (void)once; (void)arg; (void)ctx;
    char path[MAX_PATH + 32];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (!n || n >= MAX_PATH) { snprintf(glslang_why, sizeof glslang_why, "the program's folder is unknown"); return TRUE; }
    char *slash = strrchr(path, '\\');
    snprintf(slash ? slash + 1 : path, sizeof path - (size_t)(slash ? slash + 1 - path : 0), "ssbu_glslang.dll");
    HMODULE m = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) {
        DWORD e = GetLastError();
        snprintf(glslang_why, sizeof glslang_why, e == ERROR_MOD_NOT_FOUND && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES
                 ? "ssbu_glslang.dll needs the Visual C++ runtime (vcruntime140.dll, msvcp140.dll), which this PC lacks"
                 : "no ssbu_glslang.dll beside the program (tools/glslang_lib.py makes it; error %lu)", (unsigned long)e);
        return TRUE;
    }
    glslang_compile = (HlslToSpirv)(void (*)(void))GetProcAddress(m, "ssbu_hlsl_to_spirv");
    glslang_free = (SpirvWordsFree)(void (*)(void))GetProcAddress(m, "ssbu_spirv_free");
    if (!glslang_compile || !glslang_free) {
        glslang_compile = NULL;
        snprintf(glslang_why, sizeof glslang_why, "ssbu_glslang.dll is not this program's (its functions are missing)");
    }
    return TRUE;
}

int spirv_have_glslang(char *err, size_t err_size) {
    InitOnceExecuteOnce(&glslang_once, glslang_load, NULL, NULL);
    if (!glslang_compile && err_size) snprintf(err, err_size, "%s", glslang_why);
    return glslang_compile != NULL;
}

int spirv_from_hlsl(const char *hlsl, int stage, SpirvCode *out, char *err, size_t err_size) {
    out->code = NULL;
    out->size = 0;
    if (!spirv_have_glslang(err, err_size)) return 0;
    unsigned *words = NULL;
    size_t count = 0;
    if (!glslang_compile(stage, hlsl, &words, &count, err, err_size) || !words || !count) {
        if (words) glslang_free(words);
        if (err_size && !*err) snprintf(err, err_size, "glslang made no SPIR-V");
        return 0;
    }
    /* (copied: the DLL has a C runtime of its own, so its memory is given back to it) */
    if (!(out->code = malloc(count * 4))) {
        glslang_free(words);
        if (err_size) snprintf(err, err_size, "out of memory");
        return 0;
    }
    memcpy(out->code, words, count * 4);
    out->size = count * 4;
    glslang_free(words);
    return 1;
}
#else
int spirv_have_glslang(char *err, size_t err_size) {
    if (err_size) snprintf(err, err_size, "glslang's library is made for Windows only");
    return 0;
}

int spirv_from_hlsl(const char *hlsl, int stage, SpirvCode *out, char *err, size_t err_size) {
    (void)hlsl; (void)stage;
    out->code = NULL;
    out->size = 0;
    spirv_have_glslang(err, err_size);
    return 0;
}
#endif

int spirv_available(void) { return spirv_have_vkd3d() || spirv_have_glslang(NULL, 0); }

void spirv_free(SpirvCode *code) {
    free(code->code);
    code->code = NULL;
    code->size = 0;
}
