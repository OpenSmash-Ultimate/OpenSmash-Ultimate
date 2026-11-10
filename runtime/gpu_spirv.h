/* The shaders as SPIR-V for the Vulkan back end (gpu_spirv.c; docs/TECHNICAL.md, "A second back
 * end: Vulkan"), by either of two translators: vkd3d-shader from their DXBC (gpu_shader.c's HLSL,
 * compiled and cached as the Direct3D 11 back end has it), where the build has it (SSBU_HAVE_VKD3D:
 * tools/vkd3d_lib.py on Windows; elsewhere spirv_from_dxbc says it cannot); or glslang from the
 * HLSL itself (spirv_from_hlsl), through ssbu_glslang.dll beside the program (tools/glslang_lib.py,
 * which the Windows build makes): no vkd3d-shader and no fxc needed.
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

/* Whether there is a translator: vkd3d-shader in this build (DXBC), or glslang's library beside
 * the program (HLSL: spirv_from_hlsl). */
int spirv_available(void);
int spirv_have_vkd3d(void);
/* glslang's library, ssbu_glslang.dll (tools/glslang_lib.py: Windows), loaded at the first ask:
 * 1 if it is there and loads; 0 otherwise, `err` (if given) says why. */
int spirv_have_glslang(char *err, size_t err_size);
/* That library's version, which its SPIR-V files are named by (gpu_shader_cache.h's
 * shader_cache_file_spv): the shim's revision and glslang's version in one number
 * (tools/glslang_shim.c's ssbu_glslang_version). 0 without the library, and for one from before
 * it said its version. */
unsigned spirv_glslang_version(void);
/* gpu_shader.c's HLSL of its SHADER_HLSL_GLSLANG dialect to SPIR-V 1.3 for Vulkan 1.1, through
 * glslang's HLSL front end, with the same binding map and descriptor kinds as spirv_from_dxbc's
 * (its Buffer<uint>s uniform texel buffers, as vkd3d-shader makes the ByteAddressBuffers' DXBC),
 * entry point "main"; no fxc, no DXBC. 1 if done (*out: spirv_free), 0 if not (`err`). */
int spirv_from_hlsl(const char *hlsl, int stage, SpirvCode *out, char *err, size_t err_size);
/* DXBC of `stage` (gpu_shader.h's SHADER_VERTEX, SHADER_PIXEL, SHADER_COMPUTE) to SPIR-V for Vulkan
 * 1.1 with the binding map above, entry point "main". 1 if done (*out holds it: spirv_free);
 * 0 if not, `err` says why (vkd3d-shader's messages, cut to fit). */
int spirv_from_dxbc(const void *dxbc, size_t size, int stage, SpirvCode *out, char *err, size_t err_size);
void spirv_free(SpirvCode *code);
