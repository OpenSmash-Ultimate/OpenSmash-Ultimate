/* DXBC to SPIR-V for the Vulkan back end in development, through vkd3d-shader (gpu_spirv.h: the
 * binding map). The same DXBC the Direct3D 11 back end makes and caches (gpu_shader.c's HLSL,
 * gpu_shader_cache.h's files, tools/shader_aot.py's cache filled ahead of time), so the
 * translator, the cache and the prewarm serve both back ends. */
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

int spirv_available(void) { return 1; }

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
int spirv_available(void) { return 0; }

int spirv_from_dxbc(const void *dxbc, size_t size, int stage, SpirvCode *out, char *err, size_t err_size) {
    (void)dxbc; (void)size; (void)stage;
    out->code = NULL;
    out->size = 0;
    if (err_size) snprintf(err, err_size, "this build has no vkd3d-shader (tools/vkd3d_lib.py builds it for Windows)");
    return 0;
}
#endif

void spirv_free(SpirvCode *code) {
    free(code->code);
    code->code = NULL;
    code->size = 0;
}
