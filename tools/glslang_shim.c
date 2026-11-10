/* ssbu_glslang.dll: the Vulkan back end's shader translator when the program has no vkd3d-shader -
 * gpu_shader.c's HLSL (its SHADER_HLSL_GLSLANG dialect) to SPIR-V through glslang's HLSL front end,
 * with gpu_spirv.h's binding map. gpu_spirv.c loads it at run time (spirv_from_hlsl).
 *
 * Why a library of its own: glslang is C++, and Khronos' Windows release of it (the one
 * tools/glslang_lib.py fetches, pinned by its SHA-256) is built against the DLL C runtime (/MD),
 * while the program is C with the static one. This file and that release make the DLL; the program
 * only calls the two functions below, and without the DLL (or the Visual C++ runtime it needs) the
 * Vulkan back end says it has no translator and Direct3D 11 draws. ssbu-recomp's renderer did the
 * same (its vk_glslang_shim.c, GLSL there). Built by tools/glslang_lib.py (in tools/: the runtime's build compiles every runtime/*.c).
 *
 * Thread-safe: glslang's process start is made once; each call has its own shader and program. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include <glslang/Include/glslang_c_interface.h>
#include <glslang/build_info.h>
#include <glslang/Public/resource_limits_c.h>

#include "../runtime/gpu_spirv.h"

static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK start(PINIT_ONCE o, PVOID arg, PVOID *ctx) {
    (void)o; (void)arg; (void)ctx;
    return glslang_initialize_process() != 0;
}

__declspec(dllexport) void ssbu_spirv_free(uint32_t *words) { free(words); }

/* What made the SPIR-V: this file's revision (bump SHIM_REVISION when what it asks of glslang or
 * rewrites in its SPIR-V changes) and glslang's version, as revision << 24 | major << 16 |
 * minor << 8 | patch. The cache's file names carry it (gpu_shader_cache.h's
 * shader_cache_file_spv), so another DLL never reads this one's files. Never 0. */
#define SHIM_REVISION 1
__declspec(dllexport) unsigned ssbu_glslang_version(void) {
    return (unsigned)SHIM_REVISION << 24 | (GLSLANG_VERSION_MAJOR & 0xFF) << 16 | (GLSLANG_VERSION_MINOR & 0xFF) << 8 | (GLSLANG_VERSION_PATCH & 0xFF);
}

/* stage: 0 vertex, 1 pixel, 2 compute (gpu_shader.h's SHADER_*). 1 and a malloc'd SPIR-V (free it with
 * ssbu_spirv_free; count in 32-bit words), or 0 with glslang's messages in log. */
__declspec(dllexport) int ssbu_hlsl_to_spirv(int stage, const char *hlsl, uint32_t **words, size_t *count, char *log, size_t log_size) {
    *words = NULL;
    *count = 0;
    if (log_size) log[0] = 0;
    if (!InitOnceExecuteOnce(&once, start, NULL, NULL)) {
        if (log_size) snprintf(log, log_size, "glslang could not start");
        return 0;
    }
    glslang_input_t in;
    memset(&in, 0, sizeof in);
    in.language = GLSLANG_SOURCE_HLSL;
    in.stage = stage == 0 ? GLSLANG_STAGE_VERTEX : stage == 1 ? GLSLANG_STAGE_FRAGMENT : GLSLANG_STAGE_COMPUTE;
    in.client = GLSLANG_CLIENT_VULKAN;
    in.client_version = GLSLANG_TARGET_VULKAN_1_1;
    in.target_language = GLSLANG_TARGET_SPV;
    in.target_language_version = GLSLANG_TARGET_SPV_1_3;
    in.code = hlsl;
    in.default_version = 100;
    in.default_profile = GLSLANG_NO_PROFILE;
    /* (HLSL's constant buffer packing: Params is uint4 and float4 arrays, the same in either packing) */
    in.messages = GLSLANG_MSG_READ_HLSL_BIT | GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT | GLSLANG_MSG_HLSL_OFFSETS_BIT;
    in.resource = glslang_default_resource();
    glslang_shader_t *sh = glslang_shader_create(&in);
    if (!sh) {
        if (log_size) snprintf(log, log_size, "glslang_shader_create failed");
        return 0;
    }
    glslang_shader_set_entry_point(sh, "main");
    glslang_shader_set_source_entry_point(sh, "main");
    /* gpu_spirv.h's map: b# as they are, t# from 32 (textures and the Buffer<uint>s alike), s# from 160,
     * u# from 192 (the compute programs' RWStructuredBuffers are SSBOs to glslang: from 192 too); one descriptor set a stage */
    unsigned set = stage == 1 ? SPIRV_SET_PIXEL : stage == 0 ? SPIRV_SET_VERTEX : SPIRV_SET_COMPUTE;
    glslang_shader_shift_binding(sh, GLSLANG_RESOURCE_TYPE_UBO, SPIRV_BINDING_CBV(0));
    glslang_shader_shift_binding(sh, GLSLANG_RESOURCE_TYPE_TEXTURE, SPIRV_BINDING_SRV(0));
    glslang_shader_shift_binding(sh, GLSLANG_RESOURCE_TYPE_SSBO, SPIRV_BINDING_UAV(0));   /* (RWStructuredBuffer: an SSBO, not a UAV, to glslang) */
    glslang_shader_shift_binding(sh, GLSLANG_RESOURCE_TYPE_SAMPLER, SPIRV_BINDING_SAMPLER(0));
    glslang_shader_shift_binding(sh, GLSLANG_RESOURCE_TYPE_UAV, SPIRV_BINDING_UAV(0));
    glslang_shader_shift_binding(sh, GLSLANG_RESOURCE_TYPE_IMAGE, SPIRV_BINDING_UAV(0));
    char set_text[8];
    snprintf(set_text, sizeof set_text, "%u", set);
    const char *sets[] = {set_text};
    glslang_shader_set_resource_set_binding(sh, sets, 1);
    int ok = 0;
    glslang_program_t *pr = NULL;
    if (!glslang_shader_preprocess(sh, &in) || !glslang_shader_parse(sh, &in)) {
        if (log_size) snprintf(log, log_size, "%s%s", glslang_shader_get_info_log(sh), glslang_shader_get_info_debug_log(sh));
        goto done;
    }
    pr = glslang_program_create();
    glslang_program_add_shader(pr, sh);
    if (!glslang_program_link(pr, in.messages) || !glslang_program_map_io(pr)) {
        if (log_size) snprintf(log, log_size, "%s%s", glslang_program_get_info_log(pr), glslang_program_get_info_debug_log(pr));
        goto done;
    }
    glslang_spv_options_t opt;
    memset(&opt, 0, sizeof opt);
    opt.disable_optimizer = false;   /* (HLSL's legalisation: opaque arguments, copies of resources) */
    opt.strip_debug_info = true;
    glslang_program_SPIRV_generate_with_options(pr, in.stage, &opt);
    size_t n = glslang_program_SPIRV_get_size(pr);
    const char *msg = glslang_program_SPIRV_get_messages(pr);
    if (msg && msg[0] && log_size) snprintf(log, log_size, "%s", msg);
    if (n && (*words = malloc(n * sizeof **words))) {
        glslang_program_SPIRV_get(pr, *words);
        *count = n;
        ok = 1;
        /* glslang types a Buffer<uint> as an image of format R32ui; read through a uniform texel buffer (sampled, not
         * storage) the format is the view's, and NVIDIA's driver read zeros through such images: Unknown, as
         * vkd3d-shader declares them. (OpTypeImage: result, sampled type, dim, depth, arrayed, ms, sampled, format) */
        /* And HLSL's min, max and clamp as Direct3D has them (and vkd3d-shader's SPIR-V): a NaN operand gives the other
         * one - GLSL.std.450's NMin, NMax, NClamp - where glslang writes FMin, FMax, FClamp, whose result with a NaN is
         * undefined (the Maxwell instructions behind them, FMNMX, take the number). */
        uint32_t glsl_std = 0;
        for (size_t i = 5; i < n;) {
            uint32_t op = (*words)[i] & 0xFFFF, len = (*words)[i] >> 16;
            if (!len || i + len > n) break;
            if (op == 25 && len >= 9 && (*words)[i + 3] == 5 && (*words)[i + 7] == 1) (*words)[i + 8] = 0;   /* Dim Buffer, Sampled 1 */
            if (op == 11 && len >= 6 && !memcmp(&(*words)[i + 2], "GLSL.std.450", 13)) glsl_std = (*words)[i + 1];   /* OpExtInstImport */
            if (op == 12 && len >= 5 && glsl_std && (*words)[i + 3] == glsl_std) {   /* OpExtInst type result set instruction */
                uint32_t *inst = &(*words)[i + 4];
                if (*inst == 37) *inst = 79;        /* FMin -> NMin */
                else if (*inst == 40) *inst = 80;   /* FMax -> NMax */
                else if (*inst == 43) *inst = 81;   /* FClamp -> NClamp */
            }
            i += len;
        }
    } else if (log_size && !log[0]) snprintf(log, log_size, n ? "out of memory" : "glslang made no SPIR-V");
done:
    if (pr) glslang_program_delete(pr);
    glslang_shader_delete(sh);
    return ok;
}
