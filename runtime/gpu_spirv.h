/* The shaders' DXBC (gpu_shader.c's HLSL, compiled and cached as the Direct3D 11 back end has it)
 * turned into SPIR-V for the Vulkan back end in development, by vkd3d-shader (gpu_spirv.c;
 * docs/TECHNICAL.md, "A second back end: Vulkan"). Built with it where the build has vkd3d-shader
 * (SSBU_HAVE_VKD3D: tools/vkd3d_lib.py on Windows); elsewhere spirv_from_dxbc says it cannot.
 *
 * The binding map: the HLSL's registers as Vulkan descriptor bindings, one binding a register, in
 * one descriptor set a stage (the vertex program's set 0, the pixel program's set 1, a compute
 * program's set 0). Constant buffers (b#) are uniform buffers; buffers only read (t# as
 * ByteAddressBuffer) uniform texel buffers, read through an R32_UINT view (vkd3d-shader's SPIR-V for
 * a raw SRV); buffers written (u# as RWByteAddressBuffer / RWStructuredBuffer) storage buffers;
 * textures (t#) sampled images; samplers (s#) samplers. The SPIR-V is 1.3 (Vulkan 1.1) and may use
 * SPV_KHR_float_controls (Shader Model 5 flushes 32-bit denormals): the device needs
 * VK_KHR_shader_float_controls. Plain C, no system headers: tools/dev/spirvtest.c
 * checks it on the shader cache. */
#pragma once
#include <stddef.h>

enum {
    SPIRV_SET_VERTEX = 0, SPIRV_SET_PIXEL = 1, SPIRV_SET_COMPUTE = 0,
    SPIRV_CBVS = 32, SPIRV_SRVS = 128, SPIRV_SAMPLERS = 32, SPIRV_UAVS = 32,   /* registers mapped of each kind */
};
#define SPIRV_BINDING_CBV(n) (n)                                                    /* b0..b31  ->  0..31 */
#define SPIRV_BINDING_SRV(n) (SPIRV_CBVS + (n))                                     /* t0..t127 -> 32..159 */
#define SPIRV_BINDING_SAMPLER(n) (SPIRV_CBVS + SPIRV_SRVS + (n))                    /* s0..s31  -> 160..191 */
#define SPIRV_BINDING_UAV(n) (SPIRV_CBVS + SPIRV_SRVS + SPIRV_SAMPLERS + (n))       /* u0..u31  -> 192..223 */

typedef struct SpirvCode { void *code; size_t size; } SpirvCode;   /* (size in bytes) */

/* Whether this build has the translator (vkd3d-shader). */
int spirv_available(void);
/* DXBC of `stage` (gpu_shader.h's SHADER_VERTEX, SHADER_PIXEL, SHADER_COMPUTE) to SPIR-V for Vulkan
 * 1.1 with the binding map above, entry point "main". 1 if done (*out holds it: spirv_free);
 * 0 if not, `err` says why (vkd3d-shader's messages, cut to fit). */
int spirv_from_dxbc(const void *dxbc, size_t size, int stage, SpirvCode *out, char *err, size_t err_size);
void spirv_free(SpirvCode *code);
