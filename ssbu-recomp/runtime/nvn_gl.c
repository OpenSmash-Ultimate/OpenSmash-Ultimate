/* NVN on OpenGL: the game's graphics calls drawn for real.
 *
 * How it fits together
 *   - One render thread owns the OpenGL context (4.6 compatibility, NVIDIA). Guest threads hand it work with
 *     gl_run() and wait for it.
 *   - Shaders: only about 20 of the game's ~8200 shaders carry NV_gpu_program5 assembly text in their control
 *     section; all of them carry Maxwell machine code. The machine code is translated to GLSL (mx_translate.c,
 *     tested offline by mxtest.exe on every dumped stage) and used as separable programs in one pipeline. The assembly-text path
 *     (glProgramStringARB) is kept as a fallback for pairs where both stages have text.
 *   - Hardware constant buffers as the machine code sees them: 1 = constants stored after the shader's code
 *     (also jump tables), 2 = texture handles (8 bytes per unit from byte 32), 3 + n = uniform buffer n bound
 *     by the game. They are bound as storage blocks (binding 20 * stage + index); the game's storage buffers
 *     (LDG / STG in machine code) as storage blocks at 40 + 16 * stage + index. Alpha test is done by the
 *     translated fragment shader (uniform at location 0), not by fixed function.
 *   - Command buffers are recorded into host lists and replayed at nvnQueueSubmitCommands.
 *   - The NVN entry points, command recording, the guest objects and the render thread's job queue are the front end
 *     (nvn_front.c, seam step S3); this file is the OpenGL back end (rb_gl) and what only it uses.
 *   - Buffers are guest memory: at draw time the bound ranges are copied into a streaming GL buffer.
 *   - Textures are created lazily; texels come from the pool memory (Tegra block-linear layout) and are
 *     re-read when that memory changes. Samplers + textures become bindless handles.
 *   - Window origin is upper-left and depth is 0..1 (glClipControl), so row 0 of every GL texture is the
 *     top row, as in NVN; only the final blit to the window is flipped.
 *
 * Debug aids (environment): SSBU_SHOT=<n> / SSBU_SHOT_EVERY=<k> save presented frames as frame_<n>.bmp (in SSBU_SHOT_DIR if set, from frame SSBU_SHOT_FROM on);
 * SSBU_RTDUMP=<n> saves every colour target at frame n; SSBU_DRAWSHOT=<n> the target after each draw
 * (with SSBU_DRAWSHOT_CLEAR, cleared before each); SSBU_DRAWLOG=<n> prints state, uniforms and textures of
 * every draw of frame n and writes the textures as tex_*.rgba; SSBU_SHADERDUMP=<dir> writes shader text,
 * machine code and generated GLSL; SSBU_NOSCISSOR, SSBU_SKIPDRAW=<i>, SSBU_FRAGDEBUG (magenta fragments),
 * SSBU_GLDEBUG (driver messages, error check after every command); SSBU_NOCOMPUTE skips every compute dispatch (fighters then
 * stay in their bind pose); SSBU_DRAWLOG_MINDISP=<n> makes SSBU_DRAWLOG count frames with at least n dispatches (the draw log
 * lists each dispatch with its buffers and marks vertex buffers written by compute); every 300 frames a line with the frame
 * rate. SSBU_NVN_MISSING=warn (nvn_stub.c): a call of an NVN function without an implementation does not stop the game.
 *
 * Compute (nvnCommandBufferDispatchCompute): see the block above compute_dispatch. The five compute programs built into the
 * executable carry NV assembly text and run through GL_NV_compute_program5; the others are Maxwell machine code without a
 * program header (control +0x714 = 5, work group size at control +0x748) translated by mx_translate.c. Storage buffers a
 * dispatch writes are copied back to guest memory before the next draw / copy and at the end of the batch.
 * The whole API surface is audited in plan\phases\G1_nvn_audit.md (python nvn_audit.py).
 *
 * NVN enum values and argument orders: public nvn.h (open-ead/nnheaders), checked against the trace.
 * Maxwell encodings: envytools envydis/gm107.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <GL/gl.h>
#include "runtime.h"
#include "hostrt.h"
#include "mx_translate.h"
#include "gfx_options.h"
#include "nvn_front.h"
#include "nvn_front_gl.h"
#include "tex_decode.h"
#include "nvn_sdk.h"
#include "nvn_c4.h"

#pragma comment(lib, "opengl32.lib")

/* ask laptop drivers for the discrete GPU */
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

/* ---------------------------------------------------------------- OpenGL beyond 1.1 */
#define GLF(X) \
    X(void, GenProgramsARB, (GLsizei, GLuint *)) \
    X(void, DeleteProgramsARB, (GLsizei, const GLuint *)) \
    X(void, BindProgramARB, (GLenum, GLuint)) \
    X(void, ProgramStringARB, (GLenum, GLenum, GLsizei, const void *)) \
    X(void, GenBuffers, (GLsizei, GLuint *)) \
    X(void, BindBuffer, (GLenum, GLuint)) \
    X(void, BufferData, (GLenum, ptrdiff_t, const void *, GLenum)) \
    X(void, BufferSubData, (GLenum, ptrdiff_t, ptrdiff_t, const void *)) \
    X(void, BindBufferRange, (GLenum, GLuint, GLuint, ptrdiff_t, ptrdiff_t)) \
    X(void, GenFramebuffers, (GLsizei, GLuint *)) \
    X(void, DeleteFramebuffers, (GLsizei, const GLuint *)) \
    X(void, BindFramebuffer, (GLenum, GLuint)) \
    X(void, FramebufferTexture, (GLenum, GLenum, GLuint, GLint)) \
    X(GLenum, CheckFramebufferStatus, (GLenum)) \
    X(void, DrawBuffers, (GLsizei, const GLenum *)) \
    X(void, BlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
    X(void, TexStorage1D, (GLenum, GLsizei, GLenum, GLsizei)) \
    X(void, TexStorage2D, (GLenum, GLsizei, GLenum, GLsizei, GLsizei)) \
    X(void, TexStorage3D, (GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLsizei)) \
    X(void, TexStorage2DMultisample, (GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLboolean)) \
    X(void, TexStorage3DMultisample, (GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLsizei, GLboolean)) \
    X(void, TexSubImage3D, (GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
    X(void, CompressedTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void *)) \
    X(void, CompressedTexSubImage3D, (GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLsizei, const void *)) \
    X(void, TextureView, (GLuint, GLenum, GLuint, GLenum, GLuint, GLuint, GLuint, GLuint)) \
    X(void, GenSamplers, (GLsizei, GLuint *)) \
    X(void, SamplerParameteri, (GLuint, GLenum, GLint)) \
    X(void, SamplerParameterf, (GLuint, GLenum, GLfloat)) \
    X(void, SamplerParameterfv, (GLuint, GLenum, const GLfloat *)) \
    X(uint64_t, GetTextureSamplerHandleARB, (GLuint, GLuint)) \
    X(uint64_t, GetTextureHandleARB, (GLuint)) \
    X(void, MakeTextureHandleResidentARB, (uint64_t)) \
    X(void, MakeTextureHandleNonResidentARB, (uint64_t)) \
    X(void, VertexAttribFormat, (GLuint, GLint, GLenum, GLboolean, GLuint)) \
    X(void, VertexAttribIFormat, (GLuint, GLint, GLenum, GLuint)) \
    X(void, VertexAttribBinding, (GLuint, GLuint)) \
    X(void, BindVertexBuffer, (GLuint, GLuint, ptrdiff_t, GLsizei)) \
    X(void, VertexBindingDivisor, (GLuint, GLuint)) \
    X(void, EnableVertexAttribArray, (GLuint)) \
    X(void, DisableVertexAttribArray, (GLuint)) \
    X(void, DrawElementsInstancedBaseVertexBaseInstance, (GLenum, GLsizei, GLenum, const void *, GLsizei, GLint, GLuint)) \
    X(void, DrawArraysInstancedBaseInstance, (GLenum, GLint, GLsizei, GLsizei, GLuint)) \
    X(void, BlendFuncSeparatei, (GLuint, GLenum, GLenum, GLenum, GLenum)) \
    X(void, BlendEquationSeparatei, (GLuint, GLenum, GLenum)) \
    X(void, Enablei, (GLenum, GLuint)) \
    X(void, Disablei, (GLenum, GLuint)) \
    X(void, ColorMaski, (GLuint, GLboolean, GLboolean, GLboolean, GLboolean)) \
    X(void, BlendColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, StencilFuncSeparate, (GLenum, GLenum, GLint, GLuint)) \
    X(void, StencilOpSeparate, (GLenum, GLenum, GLenum, GLenum)) \
    X(void, StencilMaskSeparate, (GLenum, GLuint)) \
    X(void, ClearBufferfv, (GLenum, GLint, const GLfloat *)) \
    X(void, ViewportIndexedf, (GLuint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, ScissorIndexed, (GLuint, GLint, GLint, GLsizei, GLsizei)) \
    X(void, DepthRangeIndexed, (GLuint, GLdouble, GLdouble)) \
    X(void, ClipControl, (GLenum, GLenum)) \
    X(void, PrimitiveRestartIndex, (GLuint)) \
    X(void, PatchParameteri, (GLenum, GLint)) \
    X(void, DispatchCompute, (GLuint, GLuint, GLuint)) \
    X(void, CreateBuffers, (GLsizei, GLuint *)) \
    X(void, DeleteBuffers, (GLsizei, const GLuint *)) \
    X(void, NamedBufferData, (GLuint, ptrdiff_t, const void *, GLenum)) \
    X(void, NamedBufferSubData, (GLuint, ptrdiff_t, ptrdiff_t, const void *)) \
    X(void, GetNamedBufferSubData, (GLuint, ptrdiff_t, ptrdiff_t, void *)) \
    X(void, MemoryBarrier, (GLbitfield)) \
    X(void, BindImageTexture, (GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum)) \
    X(void, DebugMessageCallback, (void *, const void *))     X(GLuint, CreateShaderProgramv, (GLenum, GLsizei, const char *const *))     X(void, GenProgramPipelines, (GLsizei, GLuint *))     X(void, BindProgramPipeline, (GLuint))     X(void, UseProgramStages, (GLuint, GLbitfield, GLuint))     X(void, GetProgramiv, (GLuint, GLenum, GLint *))     X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei *, char *))     X(void, DeleteProgram, (GLuint))     X(void, ProgramUniform2ui, (GLuint, GLint, GLuint, GLuint))     X(void, PolygonOffsetClamp, (GLfloat, GLfloat, GLfloat))     X(void, ClearBufferiv, (GLenum, GLint, const GLint *))     X(void, ClearBufferuiv, (GLenum, GLint, const GLuint *))     X(void, SampleMaski, (GLuint, GLbitfield))     X(void, GetTextureSubImage, (GLuint, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, GLsizei, void *))     X(void, GetCompressedTextureSubImage, (GLuint, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLsizei, void *))     X(void, GenQueries, (GLsizei, GLuint *))     X(void, BeginQuery, (GLenum, GLuint))     X(void, EndQuery, (GLenum))     X(void, GetQueryObjectuiv, (GLuint, GLenum, GLuint *))
#define X(ret, name, args) static ret (APIENTRY *gl##name) args;
GLF(X)
#undef X
/* OpenGL entry points that only some NVN features need: looked up without stopping when one is absent (gl_has reports a use). */
#define GLO(X) \
    X(GLuint, CreateShader, (GLenum)) \
    X(void, ShaderSource, (GLuint, GLsizei, const char *const *, const GLint *)) \
    X(void, CompileShader, (GLuint)) \
    X(void, GetShaderiv, (GLuint, GLenum, GLint *)) \
    X(void, AttachShader, (GLuint, GLuint)) \
    X(void, DetachShader, (GLuint, GLuint)) \
    X(void, DeleteShader, (GLuint)) \
    X(GLuint, CreateProgram, (void)) \
    X(void, LinkProgram, (GLuint)) \
    X(void, ProgramParameteri, (GLuint, GLenum, GLint)) \
    X(void, GetProgramBinary, (GLuint, GLsizei, GLsizei *, GLenum *, void *)) \
    X(void, ProgramBinary, (GLuint, GLenum, const void *, GLsizei)) \
    X(void, UseProgram, (GLuint)) \
    X(void, DepthBoundsEXT, (GLdouble, GLdouble)) \
    X(void, ConservativeRasterParameterfNV, (GLenum, GLfloat)) \
    X(void, SubpixelPrecisionBiasNV, (GLuint, GLuint)) \
    X(void, ViewportSwizzleNV, (GLuint, GLenum, GLenum, GLenum, GLenum)) \
    X(void, PatchParameterfv, (GLenum, const GLfloat *)) \
    X(void, TextureBarrier, (void)) \
    X(void, InvalidateFramebuffer, (GLenum, GLsizei, const GLenum *)) \
    X(void, FramebufferTextureLayer, (GLenum, GLenum, GLuint, GLint, GLint)) \
    X(void *, FenceSync, (GLenum, GLbitfield)) \
    X(GLenum, ClientWaitSync, (void *, GLbitfield, uint64_t)) \
    X(void, WaitSync, (void *, GLbitfield, uint64_t)) \
    X(void, DeleteSync, (void *)) \
    X(void, PushDebugGroup, (GLenum, GLuint, GLsizei, const char *)) \
    X(void, PopDebugGroup, (void)) \
    X(void, DebugMessageInsert, (GLenum, GLenum, GLuint, GLenum, GLsizei, const char *)) \
    X(void, ObjectLabel, (GLenum, GLuint, GLsizei, const char *)) \
    X(void, DebugMessageControl, (GLenum, GLenum, GLenum, GLsizei, const GLuint *, GLboolean)) \
    X(void, CoverageModulationNV, (GLenum)) \
    X(void, CoverageModulationTableNV, (GLsizei, const GLfloat *)) \
    X(void, FragmentCoverageColorNV, (GLuint)) \
    X(void, RasterSamplesEXT, (GLuint, GLboolean)) \
    X(void, FramebufferParameteri, (GLenum, GLenum, GLint)) \
    X(void, FramebufferSampleLocationsfvNV, (GLenum, GLuint, GLsizei, const GLfloat *)) \
    X(void, MinSampleShading, (GLfloat)) \
    X(void, GetQueryObjectui64v, (GLuint, GLenum, uint64_t *)) \
    X(GLenum, GetGraphicsResetStatus, (void)) \
    X(void, SamplerParameterIiv, (GLuint, GLenum, const GLint *)) \
    X(void, SamplerParameterIuiv, (GLuint, GLenum, const GLuint *)) \
    X(void, BlendParameteriNV, (GLenum, GLint)) \
    X(void, BlendEquationi, (GLuint, GLenum))
#define X(ret, name, args) static ret (APIENTRY *gl##name) args;
GLO(X)
#undef X
static int gl_has_(const void *fn, const char *name) {
    static int nlog;
    if (fn) return 1;
    if (nlog < 30) { nlog++; printf("nvn_gl: this OpenGL driver lacks %s: the NVN feature that needs it has no effect\n", name); fflush(stdout); }
    return 0;
}
#define gl_has(fn, name) gl_has_((const void *)(uintptr_t)(fn), name)

enum {
    GL_VERTEX_PROGRAM_ARB_ = 0x8620, GL_FRAGMENT_PROGRAM_ARB_ = 0x8804, GL_GEOMETRY_PROGRAM_NV_ = 0x8C26,
    GL_TESS_CONTROL_PROGRAM_NV_ = 0x891E, GL_TESS_EVALUATION_PROGRAM_NV_ = 0x891F, GL_COMPUTE_PROGRAM_NV_ = 0x90FB,
    GL_PROGRAM_FORMAT_ASCII_ARB_ = 0x8875, GL_PROGRAM_ERROR_POSITION_ARB_ = 0x864B, GL_PROGRAM_ERROR_STRING_ARB_ = 0x8874,
    GL_MAX_PROGRAM_PARAMETER_BUFFER_BINDINGS_NV_ = 0x8DA0,
    GL_ARRAY_BUFFER_ = 0x8892, GL_ELEMENT_ARRAY_BUFFER_ = 0x8893, GL_STREAM_DRAW_ = 0x88E0, GL_SHADER_STORAGE_BUFFER_ = 0x90D2,
    GL_FRAMEBUFFER_ = 0x8D40, GL_READ_FRAMEBUFFER_ = 0x8CA8, GL_DRAW_FRAMEBUFFER_ = 0x8CA9, GL_COLOR_ATTACHMENT0_ = 0x8CE0,
    GL_DEPTH_ATTACHMENT_ = 0x8D00, GL_STENCIL_ATTACHMENT_ = 0x8D20, GL_DEPTH_STENCIL_ATTACHMENT_ = 0x821A,
    GL_FRAMEBUFFER_COMPLETE_ = 0x8CD5, GL_FRAMEBUFFER_SRGB_ = 0x8DB9,
    GL_TEXTURE_3D_ = 0x806F, GL_TEXTURE_1D_ARRAY_ = 0x8C18, GL_TEXTURE_2D_ARRAY_ = 0x8C1A, GL_TEXTURE_2D_MULTISAMPLE_ = 0x9100,
    GL_TEXTURE_2D_MULTISAMPLE_ARRAY_ = 0x9102, GL_TEXTURE_RECTANGLE_ = 0x84F5, GL_TEXTURE_CUBE_MAP_ = 0x8513,
    GL_TEXTURE_CUBE_MAP_POSITIVE_X_ = 0x8515, GL_TEXTURE_CUBE_MAP_ARRAY_ = 0x9009, GL_TEXTURE_BUFFER_ = 0x8C2A,
    GL_TEXTURE_MAX_LEVEL_ = 0x813D, GL_TEXTURE_SWIZZLE_RGBA_ = 0x8E46, GL_DEPTH_STENCIL_TEXTURE_MODE_ = 0x90EA,
    GL_TEXTURE_WRAP_R_ = 0x8072, GL_TEXTURE_MIN_LOD_ = 0x813A, GL_TEXTURE_MAX_LOD_ = 0x813B, GL_TEXTURE_LOD_BIAS_ = 0x8501,
    GL_TEXTURE_COMPARE_MODE_ = 0x884C, GL_TEXTURE_COMPARE_FUNC_ = 0x884D, GL_COMPARE_REF_TO_TEXTURE_ = 0x884E,
    GL_TEXTURE_MAX_ANISOTROPY_ = 0x84FE, GL_TEXTURE_CUBE_MAP_SEAMLESS_ = 0x884F,
    GL_UPPER_LEFT_ = 0x8CA2, GL_LOWER_LEFT_ = 0x8CA1, GL_ZERO_TO_ONE_ = 0x935F, GL_NEGATIVE_ONE_TO_ONE_ = 0x935E,
    GL_MULTISAMPLE_ = 0x809D, GL_SAMPLE_ALPHA_TO_COVERAGE_ = 0x809E, GL_PRIMITIVE_RESTART_ = 0x8F9D, GL_PATCH_VERTICES_ = 0x8E72,
    GL_DEPTH_CLAMP_ = 0x864F, GL_RASTERIZER_DISCARD_ = 0x8C89, GL_DEBUG_OUTPUT_ = 0x92E0, GL_DEBUG_OUTPUT_SYNCHRONOUS_ = 0x8242,
    GL_BGR_ = 0x80E0, GL_BGRA_ = 0x80E1, GL_UNPACK_ROW_LENGTH_ = 0x0CF2, GL_DEPTH_STENCIL_ = 0x84F9, GL_STENCIL_INDEX_ = 0x1901,
};

static HDC g_dc;
static void gl_housekeeping(void);
static LONG64 g_frame_draws, g_frame_skipped, g_frame_clears;
static int g_debug;
static const char *g_cur_op = "";

/* SSBU_GLSTATS=1: one more line every 300 frames with per-frame counts (render-thread waits of guest threads, compute
 * read-backs, objects created, shader work, and how many per-draw GL settings repeat the value of the previous draw). */
static LONG64 g_gs_draws, g_gs_prog_same, g_gs_attr_set, g_gs_attr_same, g_gs_vbuf_bind, g_gs_buf_bind;
static GLuint g_gs_last_prog[2];
static uint32_t g_gs_last_attr[16];
/* P3 round 4, SSBU_FDLOG=1 (diagnostics): every message of the driver is kept with its time (ring of 64) and counted
 * per id; hitch_draw prints the ones that arrived during a slow draw call, nvn_gl_report the table. */
static int g_fdlog;
static unsigned g_fd_prim, g_fd_poly, g_fd_dclamp, g_fd_rdisc, g_fd_count;
static struct { LONG64 t; GLuint id; GLenum type, sev; char text[232]; } g_fd_ring[64];
static unsigned g_fd_ring_n;
static struct { GLuint id; GLenum type, sev; LONG64 n; char text[200]; } g_fd_ids[64];
static void fd_note(GLenum type, GLuint id, GLenum severity, const char *message) {
    unsigned k = g_fd_ring_n++ & 63;
    g_fd_ring[k].t = pf_now(); g_fd_ring[k].id = id; g_fd_ring[k].type = type; g_fd_ring[k].sev = severity;
    snprintf(g_fd_ring[k].text, sizeof g_fd_ring[k].text, "%s", message ? message : "");
    for (int i = 0; i < 64; i++) {
        if (g_fd_ids[i].n && g_fd_ids[i].id != id) continue;
        if (!g_fd_ids[i].n) { g_fd_ids[i].id = id; g_fd_ids[i].type = type; g_fd_ids[i].sev = severity; snprintf(g_fd_ids[i].text, sizeof g_fd_ids[i].text, "%s", message ? message : ""); }
        g_fd_ids[i].n++;
        break;
    }
}
static void APIENTRY debug_cb(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const char *message, const void *user) {
    static int n;
    (void)source; (void)length; (void)user;
    if (g_fdlog) fd_note(type, id, severity, message);
    if (severity == 0x826B) return;                       /* notifications */
    if (type != 0x824C && !g_debug) return;               /* errors always; the rest only with SSBU_GLDEBUG */
    logf_limited(&n, g_debug ? 3000 : 60, "GL debug (type %x id %u) during %s / %s: %s\n", type, id, g_cur_op, (const char *)g_hitch_rt, message);
}

/* ---------------------------------------------------------------- formats
 * The OpenGL internal format, pixel format and type of an NVN texture format, for the formats fmt_get (nvn_front.c)
 * lists; bytes per block, block size and kind are fmt_get's. Unknown format: 0 and the zeros fmt_get leaves. */
typedef struct { uint16_t internal, format, type; uint8_t bpb, bw, bh, kind; } GlFmt;
static const uint16_t k_int12[4][12] = {
    {0x8229, 0x8F94, 0x8232, 0x8231, 0x822D, 0x822A, 0x8F98, 0x8234, 0x8233, 0x822E, 0x8236, 0x8235},
    {0x822B, 0x8F95, 0x8238, 0x8237, 0x822F, 0x822C, 0x8F99, 0x823A, 0x8239, 0x8230, 0x823C, 0x823B},
    {0x8051, 0x8F96, 0x8D7D, 0x8D8F, 0x881B, 0x8054, 0x8F9A, 0x8D77, 0x8D89, 0x8815, 0x8D71, 0x8D83},
    {0x8058, 0x8F97, 0x8D7C, 0x8D8E, 0x881A, 0x805B, 0x8F9B, 0x8D76, 0x8D88, 0x8814, 0x8D70, 0x8D82}};
static const uint16_t k_type12[12] = {0x1401, 0x1400, 0x1401, 0x1400, 0x140B, 0x1403, 0x1402, 0x1403, 0x1402, 0x1406, 0x1405, 0x1404};
static const uint8_t k_isint12[12] = {0, 0, 1, 1, 0, 0, 0, 1, 1, 0, 1, 1};
static const uint8_t k_norm12[12] = {1, 1, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0};
static const uint16_t k_pix[4] = {0x1903, 0x8227, 0x1907, 0x1908}, k_pixint[4] = {0x8D94, 0x8228, 0x8D98, 0x8D99};

static int gl_fmt(int nf, GlFmt *g) {
    Fmt f;
    int known = fmt_get(nf, &f);
    g->internal = g->format = g->type = 0; g->bpb = f.bpb; g->bw = f.bw; g->bh = f.bh; g->kind = f.kind;
    if (!known) return 0;
#define SET(i, fo, ty) (g->internal = (i), g->format = (fo), g->type = (ty), 1)
    if ((nf >= 0x01 && nf <= 0x30) || (nf >= 0x6D && nf <= 0x78)) {
        int comps = nf >= 0x6D ? 4 : (nf - 1) / 12 + 1, i = nf >= 0x6D ? nf - 0x6D : (nf - 1) % 12;
        return SET(k_int12[comps - 1][i], k_isint12[i] ? k_pixint[comps - 1] : k_pix[comps - 1], k_type12[i]);
    }
    if (nf >= 0x79 && nf <= 0x94) return SET((nf >= 0x87 ? 0x93D0 : 0x93B0) + (nf - 0x79) % 14, 0, 0);
    switch (nf) {
    case 0x31: return SET(0x8D48, GL_STENCIL_INDEX_, 0x1401);
    case 0x32: return SET(0x81A5, 0x1902, 0x1403);
    case 0x33: return SET(0x81A6, 0x1902, 0x1405);
    case 0x34: return SET(0x8CAC, 0x1902, 0x1406);
    case 0x35: return SET(0x88F0, GL_DEPTH_STENCIL_, 0x84FA);
    case 0x36: return SET(0x8CAD, GL_DEPTH_STENCIL_, 0x8DAD);
    case 0x37: case 0x38: return SET(0x8C43, 0x1908, 0x1401);
    case 0x39: return SET(0x8056, 0x1908, 0x8033);
    case 0x3A: return SET(0x8050, 0x1908, 0x8034);
    case 0x3B: return SET(0x8057, 0x1908, 0x8034);
    case 0x3C: return SET(0x8D62, 0x1907, 0x8363);
    case 0x3D: return SET(0x8059, 0x1908, 0x8368);
    case 0x3E: return SET(0x906F, 0x8D99, 0x8368);
    case 0x3F: return SET(0x8C3A, 0x1907, 0x8C3B);
    case 0x40: return SET(0x8C3D, 0x1907, 0x8C3E);
    case 0x41: return SET(0x83F0, 0, 0);
    case 0x42: return SET(0x83F1, 0, 0);
    case 0x43: return SET(0x83F2, 0, 0);
    case 0x44: return SET(0x83F3, 0, 0);
    case 0x45: return SET(0x8C4C, 0, 0);
    case 0x46: return SET(0x8C4D, 0, 0);
    case 0x47: return SET(0x8C4E, 0, 0);
    case 0x48: return SET(0x8C4F, 0, 0);
    case 0x49: return SET(0x8DBB, 0, 0);
    case 0x4A: return SET(0x8DBC, 0, 0);
    case 0x4B: return SET(0x8DBD, 0, 0);
    case 0x4C: return SET(0x8DBE, 0, 0);
    case 0x4D: return SET(0x8E8C, 0, 0);
    case 0x4E: return SET(0x8E8D, 0, 0);
    case 0x4F: return SET(0x8E8E, 0, 0);
    case 0x50: return SET(0x8E8F, 0, 0);
    case 0x95: return SET(0x8D62, GL_BGR_, 0x8363);
    case 0x99: case 0x9A: return SET(0x8058, GL_BGRA_, 0x1401);
    case 0x9B: case 0x9C: return SET(0x8C43, GL_BGRA_, 0x1401);
    }
    return 0;
#undef SET
}

/* Vertex attribute format: component count, GL type, normalised, integer. */
static int vfmt_get(int nf, int *size, GLenum *type, int *norm, int *integer) {
    static const uint16_t t6[6] = {0x1401, 0x1400, 0x1403, 0x1402, 0x1405, 0x1404};
    if ((nf >= 0x01 && nf <= 0x30) || (nf >= 0x6D && nf <= 0x78)) {
        int i = nf >= 0x6D ? nf - 0x6D : (nf - 1) % 12;
        *size = nf >= 0x6D ? 4 : (nf - 1) / 12 + 1; *type = k_type12[i]; *norm = k_norm12[i]; *integer = k_isint12[i];
        return 1;
    }
    if (nf >= 0x51 && nf <= 0x68) { *size = (nf - 0x51) / 6 + 1; *type = t6[(nf - 0x51) % 6]; *norm = 0; *integer = 0; return 1; }
    switch (nf) {
    case 0x3D: *size = 4; *type = 0x8368; *norm = 1; *integer = 0; return 1;
    case 0x3E: *size = 4; *type = 0x8368; *norm = 0; *integer = 1; return 1;
    case 0x69: *size = 4; *type = 0x8D9F; *norm = 1; *integer = 0; return 1;
    case 0x6A: *size = 4; *type = 0x8D9F; *norm = 0; *integer = 1; return 1;
    case 0x6B: *size = 4; *type = 0x8368; *norm = 0; *integer = 0; return 1;
    case 0x6C: *size = 4; *type = 0x8D9F; *norm = 0; *integer = 0; return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- textures on the render thread */
static GLuint g_fbo_read, g_dummy_tex;
static uint64_t g_dummy_handle;
static void fbo_purge(GLuint tex);

static GLenum gl_target(int t, int samples) {
    static const GLenum m[11] = {GL_TEXTURE_1D, GL_TEXTURE_2D, GL_TEXTURE_3D_, GL_TEXTURE_1D_ARRAY_, GL_TEXTURE_2D_ARRAY_, GL_TEXTURE_2D_MULTISAMPLE_,
                                 GL_TEXTURE_2D_MULTISAMPLE_ARRAY_, GL_TEXTURE_RECTANGLE_, GL_TEXTURE_CUBE_MAP_, GL_TEXTURE_CUBE_MAP_ARRAY_, GL_TEXTURE_BUFFER_};
    if (samples > 1 && t == 1) return GL_TEXTURE_2D_MULTISAMPLE_;
    if (samples > 1 && t == 4) return GL_TEXTURE_2D_MULTISAMPLE_ARRAY_;
    return t >= 0 && t < 11 ? m[t] : GL_TEXTURE_2D;
}
static void set_swizzle(GLenum tg, const uint8_t *swz) {
    static const GLint m[6] = {GL_ZERO, GL_ONE, GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA};
    GLint v[4];
    if (swz[0] == 2 && swz[1] == 3 && swz[2] == 4 && swz[3] == 5) return;
    for (int i = 0; i < 4; i++) v[i] = m[swz[i] < 6 ? swz[i] : 0];
    glTexParameteriv(tg, GL_TEXTURE_SWIZZLE_RGBA_, v);
}

static void tex_realize(Tex *t) {
    static int nlog, nbad;
    GlFmt f;
    TexBuilder *b = &t->b;
    if (!gl_fmt(b->format, &f)) {
        g_st.unknown_fmt++;
        logf_limited(&nbad, 20, "nvn_gl: texture format 0x%x is not mapped; using RGBA8\n", b->format);
        gl_fmt(0x25, &f);
    }
    { const TexFormat *tf = tex_format(b->format); if (tf && tf->sw_decode && tf->gl_sw_internal) f.internal = tf->gl_sw_internal; }
    GLenum tg = gl_target(b->target, b->samples);
    int w = b->width > 0 ? b->width : 1, h = b->height > 0 ? b->height : 1, d = b->depth > 0 ? b->depth : 1, levels = b->levels > 0 ? b->levels : 1, maxdim = w > h ? w : h, maxl = 1;
    if (b->target == 2 && d > maxdim) maxdim = d;
    while ((maxdim >> maxl) > 0) maxl++;
    if (levels > maxl) levels = maxl;
    t->glevels = levels;
    while (glGetError()) {}
    glGenTextures(1, &t->o.gl);
    glBindTexture(tg, t->o.gl);
    switch (tg) {
    case GL_TEXTURE_1D: glTexStorage1D(tg, levels, f.internal, w); break;
    case GL_TEXTURE_2D: case GL_TEXTURE_RECTANGLE_: case GL_TEXTURE_CUBE_MAP_: case GL_TEXTURE_1D_ARRAY_:
        glTexStorage2D(tg, levels, f.internal, w, tg == GL_TEXTURE_1D_ARRAY_ ? d : h); break;
    case GL_TEXTURE_3D_: case GL_TEXTURE_2D_ARRAY_: case GL_TEXTURE_CUBE_MAP_ARRAY_: glTexStorage3D(tg, levels, f.internal, w, h, d); break;
    case GL_TEXTURE_2D_MULTISAMPLE_: glTexStorage2DMultisample(tg, b->samples, f.internal, w, h, GL_TRUE); t->glevels = 1; break;
    case GL_TEXTURE_2D_MULTISAMPLE_ARRAY_: glTexStorage3DMultisample(tg, b->samples, f.internal, w, h, d, GL_TRUE); t->glevels = 1; break;
    default: glTexStorage2D(GL_TEXTURE_2D, levels, f.internal, w, h); break;
    }
    if (tg != GL_TEXTURE_2D_MULTISAMPLE_ && tg != GL_TEXTURE_2D_MULTISAMPLE_ARRAY_) {
        glTexParameteri(tg, GL_TEXTURE_MIN_FILTER, levels > 1 ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST);
        glTexParameteri(tg, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        set_swizzle(tg, b->swz);
        if (b->dsmode == 1) glTexParameteri(tg, GL_DEPTH_STENCIL_TEXTURE_MODE_, GL_STENCIL_INDEX_);
    }
    GLenum e = glGetError();
    g_st.tex_created++;
    if (b->format >= 0x01 && b->format <= 0x0c && !(b->flags & 0x10)) {       /* candidates for a glyph cache: block-linear single-channel textures */
        static int n1;
        logf_limited(&n1, 12, "nvn_gl: single-channel texture %dx%dx%d format 0x%x levels %d flags 0x%x swizzle %d%d%d%d -> gl %u\n", b->width, b->height, b->depth, b->format, b->levels, b->flags,
                     b->swz[0], b->swz[1], b->swz[2], b->swz[3], t->o.gl);
    }
    if (e || g_debug) logf_limited(&nlog, 80, "nvn_gl: texture target %d format 0x%x %dx%dx%d levels %d samples %d flags 0x%x -> gl %u%s (error 0x%x)\n", b->target, b->format,
                                   b->width, b->height, b->depth, b->levels, b->samples, b->flags, t->o.gl, e ? " FAILED" : "", e);
}

/* One level (one layer / face, or all slices of a 3D level) of tightly packed texels into the GL texture. */
static void tex_put(Tex *t, const GlFmt *f, int level, int layer, int x, int y, int z, int w, int h, int d, const void *data) {
    GLenum tg = gl_target(t->b.target, t->b.samples);
    GLsizei size = (GLsizei)(((size_t)(w + f->bw - 1) / f->bw) * ((size_t)(h + f->bh - 1) / f->bh) * f->bpb * (size_t)d);
    glBindTexture(tg, t->o.gl);
    switch (tg) {
    case GL_TEXTURE_1D: glTexSubImage1D(tg, level, x, w, f->format, f->type, data); break;
    case GL_TEXTURE_CUBE_MAP_: tg = GL_TEXTURE_CUBE_MAP_POSITIVE_X_ + (GLenum)(layer % 6);    /* fall through */
    case GL_TEXTURE_2D: case GL_TEXTURE_RECTANGLE_: case GL_TEXTURE_1D_ARRAY_:
        if (f->kind == 4) glCompressedTexSubImage2D(tg, level, x, y, w, h, f->internal, size, data);
        else glTexSubImage2D(tg, level, x, y, w, h, f->format, f->type, data);
        break;
    case GL_TEXTURE_3D_: case GL_TEXTURE_2D_ARRAY_: case GL_TEXTURE_CUBE_MAP_ARRAY_:
        if (tg != GL_TEXTURE_3D_) z = layer;
        if (f->kind == 4) glCompressedTexSubImage3D(tg, level, x, y, z, w, h, d, f->internal, size, data);
        else glTexSubImage3D(tg, level, x, y, z, w, h, d, f->format, f->type, data);
        break;
    }
}

static void tex_upload_memory(Tex *t, const GlFmt *f) {
    static uint8_t *tmp;
    static size_t tmp_cap;
    static int nlog;
    TexBuilder *b = &t->b;
    const uint8_t *src = tex_source(t);
    if (b->flags & 0x10) {                         /* linear: plain rows with a stride */
        int64_t stride = b->stride ? b->stride : (int64_t)b->width * f->bpb;
        if (f->kind == 4) return;
        glPixelStorei(GL_UNPACK_ROW_LENGTH_, (GLint)(stride / f->bpb));
        tex_put(t, f, 0, 0, 0, 0, 0, b->width, b->height, 1, src);
        glPixelStorei(GL_UNPACK_ROW_LENGTH_, 0);
        g_st.tex_bytes += stride * b->height;
        return;
    }
    TexLayout L;
    const TexFormat *tf = tex_format(b->format);
    GlFmt sw = *f;
    tex_lay(b, &L);
    if (tf && tf->sw_decode) { sw.kind = 0; sw.format = 0x1908; sw.type = 0x1401; sw.bw = sw.bh = 1; sw.bpb = 4; }     /* decoded in software to RGBA8 */
    for (int layer = 0; layer < L.layers; layer++) {
        for (int l = 0; l < t->glevels && l < L.levels; l++) {
            const uint8_t *p = tex_level_ptr(&L, src, l, layer);
            size_t lin = L.lvl_linear_size[l], need = lin + (tf && tf->sw_decode ? (size_t)L.lvl_w[l] * L.lvl_h[l] * 4 : 0);
            if (!b->packaged && t->limit && p + L.lvl_size[l] > t->limit) {
                logf_limited(&nlog, 20, "nvn_gl: texture %dx%d format 0x%x level %d layer %d runs past its memory pool; not uploaded\n", b->width, b->height, b->format, l, layer);
                return;
            }
            if (need > tmp_cap) { tmp_cap = need * 2; tmp = realloc(tmp, tmp_cap); }
            tex_deswizzle_level(&L, l, p, tmp);
            if (tf && tf->sw_decode) {
                tex_decode_rgba8(b->format, tmp, L.lvl_w[l], L.lvl_h[l], tmp + lin);
                tex_put(t, &sw, l, layer, 0, 0, 0, L.lvl_w[l], L.lvl_h[l], 1, tmp + lin);
            } else tex_put(t, f, l, layer, 0, 0, 0, L.lvl_w[l], L.lvl_h[l], L.lvl_d[l], tmp);
            g_st.tex_bytes += (LONG64)lin;
        }
    }
}

/* Make the GL texture exist and hold what the guest memory holds. */
static void tex_sync(Tex *t) {
    GlFmt f;
    if (!t->o.gl) tex_realize(t);
    if (t->label_dirty && t->o.gl) { t->label_dirty = 0; if (glObjectLabel) glObjectLabel(0x1702 /* TEXTURE */, t->o.gl, -1, nvn_label_get(t->guest)); }
    if (t->checked == g_serial) return;
    t->checked = g_serial;
    const uint8_t *src = tex_source(t);
    if (t->gpu_written || !src || t->b.samples > 1 || !gl_fmt(t->b.format, &f) || (f.kind >= 1 && f.kind <= 3)) return;
    if (!t->b.packaged && t->limit && src + t->data_size > t->limit) return;
    if (t->b.packaged && t->uploaded) return;      /* packaged data is consumed once at creation */
    LONG64 pf0 = pf_now();
    HS_IN("texture check (write watch, hash)", 0);
    uint64_t newest = ww_newest(src, t->data_size);
    if (newest) {
        if (t->uploaded && !t->dirty && t->ww_hashed && newest <= t->ww_seen) { g_ww_clean++; pf_add(PF_TEXHASH, pf0); HS_OUT(); return; }      /* not written since the last look */
        t->ww_seen = newest;
        g_ww_dirty++;
    } else g_ww_unwatched++;
    uint64_t h = mem_hash(src, t->data_size, newest != 0 || (t->b.flags & 0x10) != 0);
    int same = t->uploaded && !t->dirty && h == t->hash && (!newest || t->ww_hashed);
    t->ww_hashed = newest != 0;                    /* t->hash is the hash of every word (needed to compare the next time) */
    pf_add(PF_TEXHASH, pf0); g_pf_hash_bytes += (LONG64)t->data_size;
    HS_OUT();
    if (same) return;
    while (glGetError()) {}
    pf0 = pf_now();
    { HS_IN("texture upload (deswizzle, decode, glTexSubImage)", 0); tex_upload_memory(t, &f); HS_OUT(); }
    pf_add(PF_TEXUP, pf0);
    GLenum e = glGetError();
    static int nlog;
    if (e) logf_limited(&nlog, 20, "nvn_gl: upload of texture %dx%d format 0x%x target %d failed with GL error 0x%x\n", t->b.width, t->b.height, t->b.format, t->b.target, e);
    t->hash = h; t->uploaded = 1; t->dirty = 0;
    tex_release_packaged(t);
    g_st.tex_uploads++;
}

static void eview_base(Tex *t, EView *e) {
    memset(e, 0, sizeof *e);
    e->target = t->b.target; e->format = t->b.format; e->nlvl = t->glevels; e->nlay = tex_layers(&t->b); e->dsmode = t->b.dsmode;
    memcpy(e->swz, t->b.swz, 4);
}
/* The GL texture object that stands for `t` seen through `v` (the texture itself when the view changes nothing). */
static TexObj *tex_view_obj(Tex *t, const View *v) {
    EView base, e;
    eview_base(t, &base);
    e = base;
    if (!v || !(v->set & 0x80)) return &t->o;
    if (v->set & 1) { e.lvl0 = v->lvl0; e.nlvl = v->nlvl; }
    if (v->set & 2) { e.lay0 = v->lay0; e.nlay = v->nlay; }
    if ((v->set & 4) && v->format) e.format = v->format;
    if (v->set & 8) memcpy(e.swz, v->swz, 4);
    if (v->set & 16) e.dsmode = v->dsmode;
    if (v->set & 32) e.target = v->target;
    if (e.lvl0 < 0 || e.lvl0 >= base.nlvl) e.lvl0 = 0;
    if (e.nlvl <= 0 || e.lvl0 + e.nlvl > base.nlvl) e.nlvl = base.nlvl - e.lvl0;
    if (e.lay0 < 0 || e.lay0 >= base.nlay) e.lay0 = 0;
    if (e.nlay <= 0 || e.lay0 + e.nlay > base.nlay) e.nlay = base.nlay - e.lay0;
    if (t->b.target == 2 || t->b.samples > 1) { e.target = base.target; e.lay0 = 0; e.nlay = base.nlay; }      /* no layer views of 3D / multisample */
    if ((e.target == 1 || e.target == 7 || e.target == 0) && e.nlay > 1) e.nlay = 1;
    if (e.target == 8 && e.nlay != 6) e.target = e.nlay == 1 ? 1 : 4;
    if (e.target == 9 && e.nlay % 6) e.target = 4;
    if (!memcmp(&e, &base, sizeof e)) return &t->o;
    for (VNode *n = t->views; n; n = n->next) if (!memcmp(&n->e, &e, sizeof e)) return &n->o;
    VNode *n = calloc(1, sizeof *n);
    GlFmt f;
    static int nlog;
    n->e = e; n->next = t->views; t->views = n;
    if (!gl_fmt(e.format, &f)) gl_fmt(t->b.format, &f);
    GLenum tg = gl_target(e.target, t->b.samples);
    while (glGetError()) {}
    glGenTextures(1, &n->o.gl);
    glTextureView(n->o.gl, tg, t->o.gl, f.internal, (GLuint)e.lvl0, (GLuint)e.nlvl, (GLuint)e.lay0, (GLuint)e.nlay);
    GLenum err = glGetError();
    if (err) {
        logf_limited(&nlog, 30, "nvn_gl: view (target %d format 0x%x levels %d+%d layers %d+%d) of texture (target %d format 0x%x %dx%d) refused: GL error 0x%x\n",
                     e.target, e.format, e.lvl0, e.nlvl, e.lay0, e.nlay, t->b.target, t->b.format, t->b.width, t->b.height, err);
        glDeleteTextures(1, &n->o.gl);
        n->o.gl = t->o.gl; n->o.alias = 1;
        return &n->o;
    }
    glBindTexture(tg, n->o.gl);
    if (tg != GL_TEXTURE_2D_MULTISAMPLE_ && tg != GL_TEXTURE_2D_MULTISAMPLE_ARRAY_) {
        glTexParameteri(tg, GL_TEXTURE_MIN_FILTER, e.nlvl > 1 ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST);
        glTexParameteri(tg, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        set_swizzle(tg, e.swz);
        if (e.dsmode == 1) glTexParameteri(tg, GL_DEPTH_STENCIL_TEXTURE_MODE_, GL_STENCIL_INDEX_);
    }
    g_st.views++;
    return &n->o;
}

static void texobj_free(TexObj *o) {
    for (int i = 0; i < o->nh; i++) glMakeTextureHandleNonResidentARB(o->h[i].h);
    free(o->h);
    o->h = NULL; o->nh = 0;
    if (o->gl && !o->alias) { fbo_purge(o->gl); glDeleteTextures(1, &o->gl); }
    o->gl = 0;
}
static void tex_free_gl(Tex *t) {
    for (VNode *n = t->views, *next; n; n = next) { next = n->next; texobj_free(&n->o); free(n); }
    t->views = NULL;
    texobj_free(&t->o);
    tex_release_packaged(t);
}

/* GL sampler objects are shared by value and never deleted. */
static struct { SamplerParams p; GLuint gl; } g_samplers[512];
static int g_sampler_count;
static GLuint sampler_gl(const SamplerParams *p) {
    static const GLenum minf[6] = {GL_NEAREST, GL_LINEAR, GL_NEAREST_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_NEAREST, GL_NEAREST_MIPMAP_LINEAR, GL_LINEAR_MIPMAP_LINEAR};
    static const GLenum wrap[8] = {GL_CLAMP, GL_REPEAT, 0x8742, 0x8743, 0x8912, 0x812D, 0x8370, 0x812F};
    for (int i = 0; i < g_sampler_count; i++) if (!memcmp(&g_samplers[i].p, p, sizeof *p)) return g_samplers[i].gl;
    GLuint s = 0;
    glGenSamplers(1, &s);
    glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, (GLint)minf[p->min < 6 ? p->min : 1]);
    glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, p->mag ? GL_LINEAR : GL_NEAREST);
    glSamplerParameteri(s, GL_TEXTURE_WRAP_S, (GLint)wrap[p->ws & 7]);
    glSamplerParameteri(s, GL_TEXTURE_WRAP_T, (GLint)wrap[p->wt & 7]);
    glSamplerParameteri(s, GL_TEXTURE_WRAP_R_, (GLint)wrap[p->wr & 7]);
    glSamplerParameterf(s, GL_TEXTURE_MIN_LOD_, p->lodmin);
    glSamplerParameterf(s, GL_TEXTURE_MAX_LOD_, p->lodmax);
    glSamplerParameterf(s, GL_TEXTURE_LOD_BIAS_, p->bias);
    {   /* ARB_bindless_texture: a sampler used for a texture handle may only have one of four border colours,
         * (0,0,0,0) (0,0,0,1) (1,1,1,0) (1,1,1,1); anything else makes glGetTextureSamplerHandleARB fail with
         * INVALID_OPERATION and the texture is then not bound at all. The nearest allowed colour is used instead. */
        float b[4] = {p->border[0], p->border[1], p->border[2], p->border[3]};
        float mx = b[0] > b[1] ? b[0] : b[1];         /* a depth or one-channel texture only looks at red: (1,0,0,0) must stay 1 */
        if (b[2] > mx) mx = b[2];
        float rgb = mx >= 0.5f ? 1.0f : 0.0f, al = b[3] >= 0.5f ? 1.0f : 0.0f;
        if (b[0] != rgb || b[1] != rgb || b[2] != rgb || b[3] != al) {
            static int said;
            if (said++ < 4) printf("nvn_gl: sampler border colour (%g, %g, %g, %g) is not usable with bindless handles: using (%g, %g, %g, %g)\n", b[0], b[1], b[2], b[3], rgb, rgb, rgb, al);
            b[0] = b[1] = b[2] = rgb; b[3] = al;
        }
        if (p->border_kind && gl_has(glSamplerParameterIiv, "glSamplerParameterIiv")) {       /* integer border colour (SetBorderColori / ui): the same four values only */
            GLint bi[4];
            memcpy(bi, p->border, 16);
            GLint rgbi = bi[0] || bi[1] || bi[2], ali = bi[3] != 0;
            bi[0] = bi[1] = bi[2] = rgbi; bi[3] = ali;
            if (p->border_kind == 1) glSamplerParameterIuiv(s, GL_TEXTURE_BORDER_COLOR, (const GLuint *)bi); else glSamplerParameterIiv(s, GL_TEXTURE_BORDER_COLOR, bi);
        } else glSamplerParameterfv(s, GL_TEXTURE_BORDER_COLOR, b);
    }
    if (p->aniso > 1.0f) glSamplerParameterf(s, GL_TEXTURE_MAX_ANISOTROPY_, p->aniso);
    if (p->reduction == 1 || p->reduction == 2) glSamplerParameteri(s, 0x9366 /* TEXTURE_REDUCTION_MODE_ARB */, p->reduction == 1 ? 0x8007 : 0x8008);   /* min / max instead of average */
    if (p->cmode == 1) {
        glSamplerParameteri(s, GL_TEXTURE_COMPARE_MODE_, GL_COMPARE_REF_TO_TEXTURE_);
        glSamplerParameteri(s, GL_TEXTURE_COMPARE_FUNC_, GL_NEVER + (p->cfunc ? p->cfunc - 1 : 7));
    }
    if (g_sampler_count < 512) { g_samplers[g_sampler_count].p = *p; g_samplers[g_sampler_count++].gl = s; }
    return s;
}

/* NVN texture handle (texture ID | sampler ID << 20, bit 32 set: dev_texture_handle, as [sdk 0x41bc80]) -> resident GL bindless handle. */
static uint64_t resolve_handle(TexPool *tp, SamplerPool *sp, uint64_t nh, uint32_t *dims, int *isint) {
    uint32_t tid = (uint32_t)nh & 0xfffff, sid = (uint32_t)(nh >> 20) & 0xfff;   /* real layout [sdk 0x41bc80]; DrawTexture splits it the same way [0x4382f0] */
    Tex *t = NULL;
    View v;
    SamplerParams p;
    int have_sampler = 0;
    if (!nh || !tp) return g_dummy_handle;
    AcquireSRWLockShared(&g_pool_lock);
    if (tid < (uint32_t)tp->count) { t = tp->e[tid].tex; v = tp->e[tid].view; }
    if (sp && sid < (uint32_t)sp->count && sp->used[sid]) { p = sp->e[sid]; have_sampler = 1; }
    ReleaseSRWLockShared(&g_pool_lock);
    if (!t || t->dead) return g_dummy_handle;
    tex_sync(t);
    TexObj *o = tex_view_obj(t, &v);
    {   /* what TXQ reports, and whether the shader must use an integer sampler */
        GlFmt f;
        int l0 = (v.set & 0x81) == 0x81 ? v.lvl0 : 0, nl = (v.set & 0x81) == 0x81 && v.nlvl > 0 ? v.nlvl : t->glevels - l0, is3d = t->b.target == 2;
        int layers = (v.set & 0x82) == 0x82 && v.nlay > 0 ? v.nlay : tex_layers(&t->b);
        dims[0] = (uint32_t)lvl(t->b.width, l0); dims[1] = (uint32_t)lvl(t->b.height, l0);
        dims[2] = is3d ? (uint32_t)lvl(t->b.depth, l0) : (uint32_t)(t->b.target == 9 ? layers / 6 : layers);
        dims[3] = (uint32_t)(nl > 0 ? nl : 1) | ((uint32_t)is3d << 16);
        if (gl_fmt((v.set & 0x84) == 0x84 && v.format ? v.format : t->b.format, &f) && f.kind == 0 && (f.format == 0x8D94 || f.format == 0x8228 || f.format == 0x8D98 || f.format == 0x8D99))
            *isint = f.type == 0x1400 || f.type == 0x1402 || f.type == 0x1404 ? 2 : 1;
    }
    GLuint smp = have_sampler ? sampler_gl(&p) : 0;
    for (int i = 0; i < o->nh; i++) if (o->h[i].smp == smp) return o->h[i].h;
    uint64_t h = smp ? glGetTextureSamplerHandleARB(o->gl, smp) : glGetTextureHandleARB(o->gl);
    if (!h) return g_dummy_handle;
    glMakeTextureHandleResidentARB(h);
    o->h = realloc(o->h, sizeof *o->h * (size_t)(o->nh + 1));
    o->h[o->nh].smp = smp; o->h[o->nh++].h = h;
    g_st.handles++;
    return h;
}
/* The same lookup for nvnCommandBufferDrawTexture, which needs the OpenGL names: 0 = no live texture under that ID (resolve_handle
 * gives the 1x1 dummy there; a DrawTexture of it is reported instead). */
static int resolve_names(TexPool *tp, SamplerPool *sp, uint64_t nh, GLuint *tex, GLuint *smp) {
    uint32_t tid = (uint32_t)nh & 0xfffff, sid = (uint32_t)(nh >> 20) & 0xfff;   /* real layout [sdk 0x41bc80]; DrawTexture splits it the same way [0x4382f0] */
    Tex *t = NULL;
    View v;
    SamplerParams p;
    int have_sampler = 0;
    if (!tp) return 0;
    AcquireSRWLockShared(&g_pool_lock);
    if (tid < (uint32_t)tp->count) { t = tp->e[tid].tex; v = tp->e[tid].view; }
    if (sp && sid < (uint32_t)sp->count && sp->used[sid]) { p = sp->e[sid]; have_sampler = 1; }
    ReleaseSRWLockShared(&g_pool_lock);
    if (!t || t->dead) return 0;
    tex_sync(t);
    *tex = tex_view_obj(t, &v)->gl;
    *smp = have_sampler ? sampler_gl(&p) : 0;
    return 1;
}

/* ---------------------------------------------------------------- framebuffers */
typedef struct { GLuint color[8], depth; int n, dkind; } FboKey;
static struct { FboKey k; GLuint fbo; int w, h, used; } g_fbos[256];
static int g_fbo_next;
static void fbo_purge(GLuint tex) {
    for (int i = 0; i < 256; i++) {
        if (!g_fbos[i].used) continue;
        int hit = g_fbos[i].k.depth == tex;
        for (int j = 0; j < 8; j++) hit |= g_fbos[i].k.color[j] == tex;
        if (hit) { glDeleteFramebuffers(1, &g_fbos[i].fbo); g_fbos[i].used = 0; }
    }
}
static GLuint fbo_get(const FboKey *k) {
    static int nlog;
    for (int i = 0; i < 256; i++) if (g_fbos[i].used && !memcmp(&g_fbos[i].k, k, sizeof *k)) return g_fbos[i].fbo;
    int slot = -1;
    for (int i = 0; i < 256 && slot < 0; i++) if (!g_fbos[i].used) slot = i;
    if (slot < 0) { slot = g_fbo_next++ & 255; glDeleteFramebuffers(1, &g_fbos[slot].fbo); }
    GLuint fbo = 0;
    GLenum bufs[8];
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER_, fbo);
    for (int i = 0; i < 8; i++) {
        bufs[i] = i < k->n && k->color[i] ? GL_COLOR_ATTACHMENT0_ + (GLenum)i : GL_NONE;
        if (i < k->n && k->color[i]) glFramebufferTexture(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_ + (GLenum)i, k->color[i], 0);
    }
    if (k->depth) glFramebufferTexture(GL_FRAMEBUFFER_, k->dkind == 3 ? GL_DEPTH_STENCIL_ATTACHMENT_ : k->dkind == 2 ? GL_STENCIL_ATTACHMENT_ : GL_DEPTH_ATTACHMENT_, k->depth, 0);
    glDrawBuffers(8, bufs);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER_);
    g_st.fbo_made++;
    if (st != GL_FRAMEBUFFER_COMPLETE_) {
        g_st.fbo_bad++;
        logf_limited(&nlog, 20, "nvn_gl: framebuffer incomplete (status 0x%x): %d colour targets (first gl %u), depth gl %u kind %d\n", st, k->n, k->color[0], k->depth, k->dkind);
    }
    g_fbos[slot].k = *k; g_fbos[slot].fbo = fbo; g_fbos[slot].used = 1;
    return fbo;
}

/* ---------------------------------------------------------------- streaming buffer: guest memory -> GL */
#define ARENA_SIZE ((size_t)96 << 20)
static GLuint g_arena;
static size_t g_arena_pos;
static struct { uint64_t addr, size; size_t off; uint32_t gen; } g_acache[8192];
static uint32_t g_arena_gen = 1;

static size_t arena_put_raw(const void *p, size_t n) {
    g_arena_pos = (g_arena_pos + 255) & ~(size_t)255;
    if (g_arena_pos + n > ARENA_SIZE) { HS_IN("buffer streaming: new 96 MB store (glBufferData)", 0); glBufferData(GL_ARRAY_BUFFER_, (ptrdiff_t)ARENA_SIZE, NULL, GL_STREAM_DRAW_); HS_OUT(); g_arena_pos = 0; g_arena_gen++; }
    size_t off = g_arena_pos;
    LONG64 pf0 = pf_now();
    HS_IN("buffer streaming (glBufferSubData)", 0);
    glBufferSubData(GL_ARRAY_BUFFER_, (ptrdiff_t)off, (ptrdiff_t)n, p);
    HS_OUT();
    pf_add(PF_ARENA, pf0); g_pf_arena_bytes += (LONG64)n;
    g_arena_pos += n;
    g_st.arena_bytes += (LONG64)n;
    return off;
}
/* Offset in the streaming buffer of a copy of guest memory [addr, addr+size), made at most once per batch. */
static size_t arena_put(uint64_t addr, uint64_t size) {
    static int nlog;
    if (size > ARENA_SIZE / 4) { logf_limited(&nlog, 10, "nvn_gl: buffer range of %llu bytes clamped to %llu\n", (unsigned long long)size, (unsigned long long)(ARENA_SIZE / 4)); size = ARENA_SIZE / 4; }
    unsigned h = (unsigned)(((addr >> 4) ^ (size * 0x9E3779B1u)) * 2654435761u) >> 19;
    for (int probe = 0; probe < 8; probe++) {
        unsigned i = (h + (unsigned)probe) & 8191;
        if (g_acache[i].gen == g_arena_gen && g_acache[i].addr == addr && g_acache[i].size == size) return g_acache[i].off;
        if (g_acache[i].gen != g_arena_gen) { h = i; break; }
    }
    size_t off = arena_put_raw(P(addr), (size_t)size);      /* may start a new generation */
    h &= 8191;
    g_acache[h].addr = addr; g_acache[h].size = size; g_acache[h].off = off; g_acache[h].gen = g_arena_gen;
    return off;
}

/* ---------------------------------------------------------------- programs */
static const GLenum k_prog_target[6] = {GL_VERTEX_PROGRAM_ARB_, GL_FRAGMENT_PROGRAM_ARB_, GL_GEOMETRY_PROGRAM_NV_, GL_TESS_CONTROL_PROGRAM_NV_, GL_TESS_EVALUATION_PROGRAM_NV_, GL_COMPUTE_PROGRAM_NV_};
static const GLenum k_pbuf_target[6] = {0x8DA2, 0x8DA4, 0x8DA3, 0x8C74, 0x8C75, 0x90FC};
#define TEXTURE_HANDLE_BUFFER 14           /* program.buffer[14] of a stage holds the 64-bit handles set with BindTexture, 8 bytes per unit */
static int g_max_pbuf;

static int stage_ready(Prog *p, int s) {
    static int nlog;
    Stage *st = &p->st[s];
    if (st->state == 2) return 1;
    if (st->state != 1) return 0;
    GLint pos = -1;
    glGenProgramsARB(1, &st->gl);
    glBindProgramARB(k_prog_target[s], st->gl);
    while (glGetError()) {}
    glProgramStringARB(k_prog_target[s], GL_PROGRAM_FORMAT_ASCII_ARB_, st->len, st->text);
    glGetIntegerv(GL_PROGRAM_ERROR_POSITION_ARB_, &pos);
    if (pos != -1 || glGetError()) {
        st->state = 3;
        g_st.prog_fail++;
        if (nlog < 12) {
            char path[64];
            snprintf(path, sizeof path, "shader_fail_%02d.txt", nlog);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(st->text, 1, (size_t)st->len, f); fclose(f); }
        }
        logf_limited(&nlog, 40, "nvn_gl: program #%d %s stage REJECTED at %d (%d buffers): %s\n", p->serial, k_stage_name[s], pos, st->nbuf, (const char *)glGetString(GL_PROGRAM_ERROR_STRING_ARB_));
        return 0;
    }
    st->state = 2;
    g_st.prog_ok++;
    free(st->text);
    st->text = NULL;
    return 1;
}

/* A shader that came without assembly text: Maxwell machine code (program header + 64-bit instruction words). */
static void dump_binary(Prog *p, int s) {
    const char *dir = getenv("SSBU_SHADERDUMP");
    char path[260];
    if (!dir) return;
    snprintf(path, sizeof path, "%s/b%05d_%s.bin", dir, p->serial, k_stage_name[s]);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(p->st[s].ctl, 1, 32, f); fwrite(p->st[s].code, 1, (size_t)p->st[s].code_len, f); fclose(f); }
}
static GLuint g_pipeline;
/* The GLSL program for a stage given as machine code; 0 when it cannot be translated or the driver refuses it. */
/* key: integer vertex inputs (all, signed) and integer textures (all, signed) as seen at this draw; a stage is translated
 * again when the bits it depends on change. */
/* P3: translated vertex / fragment stages are shared and kept (SSBU_NO_SHADERCACHE=1 turns this off).
 *   level 1: (stage, machine code, constants, the four key words) -> the translation's result and its GL program:
 *            a program object the game creates again from the same shader binary, or a stage that alternates between two
 *            keys, costs a table look-up instead of a translation and a driver compile;
 *   level 2: generated GLSL text -> GL program: two keys that lead to the same text share the program.
 * Shared GL programs are never deleted (a few thousand at most: the game has about 6,900 distinct stages). Per-draw state
 * of a program (the alpha-test uniform) is set at every draw that uses it, so sharing does not change what is drawn.
 * Entries are found by 64-bit hashes of the contents (plus length, stage and key compared exactly). */
typedef struct ShEnt { uint64_t h; int stage, code_len; uint32_t key[4]; GLuint prog; uint32_t cbufs, ssbos, in_attr, tex_units, ssbo_written; int uses_alpha; struct ShEnt *next; } ShEnt;
typedef struct ShSrc { uint64_t h; size_t len; GLuint prog; struct ShSrc *next; char *text; } ShSrc;
static ShSrc *g_prog_src[65536];                 /* P3 round 3: GL stage program -> its text record (pair programs) */
static GLuint g_pair_cur;                        /* the linked pair program in use (glUseProgram), 0 = none */
static void pair_off(void);
static ShEnt *g_shc[4096];
static ShSrc *g_shs[4096];
static int g_shc_off = -1;
static LONG64 g_shc_hit, g_shc_src_hit, g_shc_compiled;
static uint64_t sh_hash(uint64_t h, const void *data, size_t n) {
    const uint8_t *b = data;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, b + i, 8); h = (h ^ w) * 0x100000001B3ULL; h ^= h >> 31; }
    for (; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ULL;
    return h;
}
/* P3: the driver's compiled form of a translated stage is kept on disk. The driver needs 100-300 ms to compile one of
 * these stages (measured: about 500 new stages in a 200 s run with a battle, 8-60 ms per frame on average while they
 * arrive), which is what makes the frame time jump whenever something new comes on screen. With the binary on disk the
 * second and later runs load it in about a millisecond.
 *   file: <dir>/<hash of the GLSL text>_<text length>_<hash of GL_VENDOR, GL_RENDERER, GL_VERSION>.bin = format, size, bytes
 *   The key is the generated TEXT, so a change of the translator can never pick up an old binary; another driver or GPU
 *   gets other file names, and a binary the driver refuses (glProgramBinary leaves the program unlinked) is compiled again
 *   and rewritten. Files are written under a temporary name and renamed, so several running copies can share the folder.
 *   SSBU_SHADER_CACHE=<dir> chooses the folder (default: ..\\cache\\shaders, i.e. Documents\\SSBU\\cache\\shaders), SSBU_SHADER_CACHE=0
 *   turns it off (every stage is then compiled with glCreateShaderProgramv, as before). */
static LONG64 g_shd_loaded, g_shd_saved, g_shd_refused;
static volatile LONG64 g_shw_queued, g_shw_done, g_shw_compiled, g_shw_present, g_shw_failed;
static int g_shd_mode = -1;
static char g_shd_dir[200];
static uint64_t g_shd_drv;
static void shd_workers_start(void);
static void prec_load(void);
/* Render thread, once, after the context exists. */
static void shd_init(void) {
    const char *e = getenv("SSBU_SHADER_CACHE");
    g_shd_mode = !(e && !strcmp(e, "0")) && glCreateShader && glShaderSource && glCompileShader && glGetShaderiv && glAttachShader && glDetachShader && glDeleteShader &&
                 glCreateProgram && glLinkProgram && glProgramParameteri && glGetProgramBinary && glProgramBinary;
    if (!g_shd_mode) return;
    const char *v[3] = {(const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION)};
    snprintf(g_shd_dir, sizeof g_shd_dir, "%s", e && e[0] ? e : "../cache/shaders");      /* Documents\\SSBU\\cache\\shaders when run from recomp-study */
    if (!(e && e[0])) CreateDirectoryA("../cache", NULL);
    CreateDirectoryA(g_shd_dir, NULL);
    g_shd_drv = 0x243F6A8885A308D3ULL;
    for (int i = 0; i < 3; i++) if (v[i]) g_shd_drv = sh_hash(g_shd_drv, v[i], strlen(v[i]));
    printf("nvn_gl: shader binaries are kept in %s (SSBU_SHADER_CACHE=0 turns this off)%c", g_shd_dir, 10);
    shd_workers_start();
    prec_load();
}
static void shd_path(char *path, size_t size, uint64_t h, size_t len) {
    snprintf(path, size, "%s/%016llx_%zx_%016llx.bin", g_shd_dir, (unsigned long long)h, len, (unsigned long long)g_shd_drv);
}
/* Compile + link as a separable program (what glCreateShaderProgramv does, plus the hint that the binary will be asked
 * for) and write the binary to `path`. Returns the program, 0 when the shader does not compile. Any thread with a context. */
static GLuint shd_compile_save(GLenum type, const char *src, const char *path, GLint *ok, int *saved) {
    char tmp[360];
    GLint cs = 0, n = 0;
    GLuint sh = glCreateShader(type), q;
    *ok = 0; *saved = 0;
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, 0x8B81 /* COMPILE_STATUS */, &cs);
    if (!cs) { glDeleteShader(sh); return 0; }
    q = glCreateProgram();
    glProgramParameteri(q, 0x8258 /* PROGRAM_SEPARABLE */, 1);
    glProgramParameteri(q, 0x8257 /* PROGRAM_BINARY_RETRIEVABLE_HINT */, 1);
    glAttachShader(q, sh);
    glLinkProgram(q);
    glDetachShader(q, sh);
    glDeleteShader(sh);
    glGetProgramiv(q, 0x8B82, ok);
    if (!*ok) return q;
    glGetProgramiv(q, 0x8741 /* PROGRAM_BINARY_LENGTH */, &n);
    if (n > 0) {
        uint32_t head[2] = {0, 0};
        GLsizei got = 0;
        GLenum fmt = 0;
        void *data = malloc((size_t)n);
        while (glGetError()) {}
        glGetProgramBinary(q, n, &got, &fmt, data);
        if (!glGetError() && got > 0) {
            snprintf(tmp, sizeof tmp, "%s.%lu.%lu.tmp", path, (unsigned long)GetCurrentProcessId(), (unsigned long)GetCurrentThreadId());
            FILE *f = fopen(tmp, "wb");
            if (f) {
                head[0] = fmt; head[1] = (uint32_t)got;
                int good = fwrite(head, 4, 2, f) == 2 && fwrite(data, 1, (size_t)got, f) == (size_t)got;
                fclose(f);
                if (good && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) *saved = 1; else DeleteFileA(tmp);
            }
        }
        free(data);
    }
    return q;
}
/* Render thread: the program for this text, from disk when its binary is there. */
static GLuint shader_build(GLenum type, const char *src, size_t len, uint64_t h, GLint *ok) {
    char path[320];
    int saved = 0;
    *ok = 0;
    if (g_shd_mode <= 0) { GLuint q = glCreateShaderProgramv(type, 1, &src); glGetProgramiv(q, 0x8B82, ok); return q; }
    shd_path(path, sizeof path, h, len);
    FILE *f = fopen(path, "rb");
    if (f) {
        uint32_t head[2] = {0, 0};
        if (fread(head, 4, 2, f) == 2 && head[1] > 0 && head[1] < (64u << 20)) {
            void *data = malloc(head[1]);
            if (fread(data, 1, head[1], f) == head[1]) {
                GLuint q = glCreateProgram();
                while (glGetError()) {}
                HS_IN("shader binary from disk (glProgramBinary)", 0);
                glProgramParameteri(q, 0x8258 /* PROGRAM_SEPARABLE */, 1);
                glProgramBinary(q, head[0], data, (GLsizei)head[1]);
                glGetProgramiv(q, 0x8B82, ok);
                HS_OUT();
                while (glGetError()) {}
                if (*ok) { free(data); fclose(f); g_shd_loaded++; return q; }
                glDeleteProgram(q);
                g_shd_refused++;
            }
            free(data);
        }
        fclose(f);
    }
    GLuint q;
    { HS_IN(type == 0x8B31 ? "shader compile + link, vertex stage (driver)" : type == 0x91B9 ? "shader compile + link, compute stage (driver)" : "shader compile + link, fragment stage (driver)", 0); q = shd_compile_save(type, src, path, ok, &saved); HS_OUT(); }
    if (!q) { q = glCreateShaderProgramv(type, 1, &src); glGetProgramiv(q, 0x8B82, ok); return q; }     /* does not compile: the old path reports it (its log holds the compiler's messages) */
    g_shd_saved += saved;
    return q;
}

/* ---- compiling ahead of the first draw (P3)
 * The game hands over a program's shaders (nvnProgramSetShaders) when it loads a fighter, a stage or an effect, usually
 * long before the first draw that uses it. Worker threads, each with a small OpenGL context of its own (nothing shared
 * with the render thread's context), translate those stages at once and put the driver's binary into the disk cache; when
 * the first draw comes, the render thread finds the binary there. Nothing is drawn differently: a stage whose binary is
 * not ready is compiled by the render thread exactly as before. The workers assume the common case for what a stage's
 * text can depend on (no integer vertex inputs or textures); a draw with another combination compiles its own text.
 * SSBU_SHADER_WORKERS=<n> (default 3, 0 = none). */
typedef struct ShJob { struct ShJob *next; int stage, code_len; uint32_t cdata_size; uint32_t key[4]; uint8_t data[1]; } ShJob;
/* P3 round 2: which text variants of a stage were needed. A stage's GLSL depends on which vertex inputs and textures hold
 * integers at the draw (the four key words); the workers used to compile the variant with none only, so a stage first
 * drawn with another combination was compiled by the render thread in the middle of a frame (0.3-1.3 s for a vertex
 * stage, hitch log). Every key the render thread had to compile for is appended to <cache>/variants.txt as
 * "<hash of code and constants> <stage> <key words>"; at the next start the workers compile those variants as well, as
 * soon as the game hands the stage over. Nothing is drawn differently: this only decides what is compiled ahead. */
typedef struct ShVar { uint64_t h; int stage; uint32_t key[4]; struct ShVar *next; } ShVar;
static ShVar *g_shv[4096];
static CRITICAL_SECTION g_shw_lock;
static LONG64 g_shv_loaded, g_shv_added, g_shv_queued;
static volatile LONG64 g_shw_held, g_shw_held_ticks;
static void shv_put(uint64_t h, int stage, const uint32_t *key) {
    ShVar *e = calloc(1, sizeof *e);
    e->h = h; e->stage = stage; memcpy(e->key, key, 16); e->next = g_shv[h & 4095]; g_shv[h & 4095] = e;
}
static int shv_has(uint64_t h, int stage, const uint32_t *key) {
    for (ShVar *e = g_shv[h & 4095]; e; e = e->next) if (e->h == h && e->stage == stage && !memcmp(e->key, key, 16)) return 1;
    return 0;
}
static void shv_load(void) {                       /* render thread, before the workers start */
    char path[260], line[200];
    snprintf(path, sizeof path, "%s/variants.txt", g_shd_dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        unsigned long long h; int stage; uint32_t k[4];
        if (sscanf(line, "%llx %d %x %x %x %x", &h, &stage, &k[0], &k[1], &k[2], &k[3]) == 6 && stage >= 0 && stage < 2 && !shv_has(h, stage, k)) { shv_put(h, stage, k); g_shv_loaded++; }
    }
    fclose(f);
}
static void shv_record(uint64_t h, int stage, const uint32_t *key) {      /* render thread: this key had to be compiled here */
    char path[260];
    if (g_shd_mode <= 0 || !(key[0] | key[1] | key[2] | key[3])) return;
    EnterCriticalSection(&g_shw_lock);
    int known = shv_has(h, stage, key);
    if (!known) shv_put(h, stage, key);
    LeaveCriticalSection(&g_shw_lock);
    if (known) return;
    snprintf(path, sizeof path, "%s/variants.txt", g_shd_dir);
    FILE *f = fopen(path, "a");
    if (f) { fprintf(f, "%016llx %d %x %x %x %x\n", (unsigned long long)h, stage, key[0], key[1], key[2], key[3]); fclose(f); g_shv_added++; }
}
static CRITICAL_SECTION g_shw_lock;
static ShJob *g_shw_head, *g_shw_tail;
static HANDLE g_shw_sem;
static uint64_t g_shw_seen[16384];
static int g_shw_n;
/* ---- P3 round 4: the driver's work at the first draw of a shader pair, done ahead by the workers
 * What the driver does (measured with tools_p3/p4lab.c, P3_performance.md section 13): a program loaded with
 * glProgramBinary (or linked) is not ready to run; at the FIRST draw call that uses a (vertex, fragment) combination the
 * driver generates its code, 40-190 ms, without any debug message, under a lock that also stops the draw calls of every
 * other context of the process. It keeps the result in its own cache: in memory for the process (keyed by the CONTENT
 * of the two programs: other program objects, other contexts and other threads get it), and on disk in a file per
 * executable NAME (a copy of the exe under another name starts cold, the same name in another folder or relinked is
 * warm). Our program binaries do not contain it (read back after the draw and loaded under a new name: cold again).
 * So: a worker thread, in its small context of its own, loads the two programs and draws ONE point with the rasterizer
 * off into its own 8x8 window. Nothing of the game's frame is involved; when the render thread later draws with its
 * own program objects of the same binaries, the driver finds the pair in its memory cache (1 ms instead of 65).
 * Which pairs: (1) every pair a draw used in an earlier run (<cache>/pairs.txt), queued at start; (2) the vertex and
 * fragment stage of a program the game hands over (nvnProgramSetShaders) after start-up, or always with
 * SSBU_PAIR_WARM=2; the caller is held until its program is ready, as the stage compile already did.
 * When: the driver's lock means this work cannot hide behind the game (a 65 ms stall per cold pair on the render
 * thread), so while COLD work is pending (a warm draw or a compile took more than 8 ms) the render thread holds the
 * next present until the workers are done: the cost is paid as loading time, before the frames that need it.
 * With a warm driver cache a pair costs the workers about 2 ms and nothing is held. */
typedef struct PwSeen { uint64_t vh, fh; uint32_t vl, fl, mask; struct PwSeen *next; } PwSeen;
static PwSeen *g_pw_seen[4096];
static uint64_t g_pw_qseen[16384];
static CRITICAL_SECTION g_pw_stripe[64];
static volatile LONG g_pw_cold, g_shw_ctx_failed;
static volatile LONG64 g_pw_named, g_pw_progs, g_pw_warmed, g_pw_cold_n, g_pw_ticks, g_pw_missing, g_pw_dup, g_pw_gate_ticks, g_pw_gates, g_pw_compiled;
static int pw_mode(void) { static int m = -1; if (m < 0) { const char *e = getenv("SSBU_PAIR_WARM"); m = e ? atoi(e) : 1; } return m; }
static int pw_seen(uint64_t vh, uint32_t vl, uint64_t fh, uint32_t fl, uint32_t mask) {
    unsigned k = (unsigned)((vh ^ fh * 0x9E3779B97F4A7C15ULL) >> 20) & 4095;
    int found = 0;
    EnterCriticalSection(&g_shw_lock);
    for (PwSeen *e = g_pw_seen[k]; e && !found; e = e->next) found = e->vh == vh && e->fh == fh && e->vl == vl && e->fl == fl && e->mask == mask;
    if (!found) { PwSeen *e = calloc(1, sizeof *e); e->vh = vh; e->vl = vl; e->fh = fh; e->fl = fl; e->mask = mask; e->next = g_pw_seen[k]; g_pw_seen[k] = e; }
    LeaveCriticalSection(&g_shw_lock);
    return found;
}
static void pw_cold_mark(LONG64 t0) {
    static LONG64 freq;
    if (!freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart; }
    if ((pf_now() - t0) * 125 > freq) InterlockedExchange(&g_pw_cold, 1);          /* more than 8 ms */
}
/* Worker: the program for a stage text, from its binary; compiled (and saved) when there is none and the text is given. */
static GLuint pw_load(int stage, uint64_t th, size_t tl, const char *src) {
    char path[320];
    GLuint q = 0;
    GLint ok = 0;
    shd_path(path, sizeof path, th, tl);
    EnterCriticalSection(&g_pw_stripe[th & 63]);
    FILE *f = fopen(path, "rb");
    if (f) {
        uint32_t head[2] = {0, 0};
        if (fread(head, 4, 2, f) == 2 && head[1] > 0 && head[1] < (64u << 20)) {
            void *data = malloc(head[1]);
            if (fread(data, 1, head[1], f) == head[1]) {
                q = glCreateProgram();
                glProgramParameteri(q, 0x8258 /* PROGRAM_SEPARABLE */, 1);
                glProgramBinary(q, head[0], data, (GLsizei)head[1]);
                glGetProgramiv(q, 0x8B82, &ok);
                if (!ok) { glDeleteProgram(q); q = 0; }
            }
            free(data);
        }
        fclose(f);
    }
    if (!q && src) {
        int saved = 0;
        LONG64 t0 = pf_now();
        q = shd_compile_save(stage == 0 ? 0x8B31 : 0x8B30, src, path, &ok, &saved);
        if (q && !ok) { glDeleteProgram(q); q = 0; }
        if (saved) { InterlockedIncrement64(&g_shw_compiled); InterlockedIncrement64(&g_pw_compiled); }
        pw_cold_mark(t0);
    }
    LeaveCriticalSection(&g_pw_stripe[th & 63]);
    while (glGetError()) {}
    return q;
}
static char *pw_translate(int stage, const uint8_t *code, int code_len, const uint8_t *cdata, uint32_t cdata_size, uint32_t *in_attr) {
    MxIn in;
    MxOut out;
    memset(&in, 0, sizeof in);
    in.stage = stage; in.code = code; in.code_len = code_len;
    if (cdata_size) { in.cdata = cdata; in.cdata_size = cdata_size; }
    in.binding_base = (unsigned)stage * 20; in.ssbo_base = 40 + (unsigned)stage * 16;
    in.fragdebug = getenv("SSBU_FRAGDEBUG") != NULL;
    if (!mx_translate(&in, &out)) return NULL;
    if (in_attr) *in_attr = out.in_attr;
    return out.glsl;
}
/* <cache>/progs.txt: one line per program (hash of the machine code of its stages, as pw_queue_prog computes it) whose
 * pair a worker has made ready and written to pairs.txt. Read at start into the table pw_queue_prog looks at. */
static void pw_prog_known(const ShJob *j) {
    char path[260];
    uint64_t h;
    if (j->stage != 3 || getenv("SSBU_NO_PAIR_LIST")) return;
    memcpy(&h, &j->key[2], 8);
    snprintf(path, sizeof path, "%s/progs.txt", g_shd_dir);
    EnterCriticalSection(&g_shw_lock);
    FILE *f = fopen(path, "a");
    if (f) { fprintf(f, "%016llx\n", (unsigned long long)h); fclose(f); }
    LeaveCriticalSection(&g_shw_lock);
}
static LONG64 g_pw_progs_known;
static void pw_progs_load(void) {                  /* render thread, start, only when pairs.txt was read */
    char path[260], line[64];
    snprintf(path, sizeof path, "%s/progs.txt", g_shd_dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    EnterCriticalSection(&g_shw_lock);
    while (fgets(line, sizeof line, f)) {
        unsigned long long h = 0;
        if (sscanf(line, "%llx", &h) != 1 || !h) continue;
        unsigned k = (unsigned)(h >> 20) & 16383;
        for (int probe = 0; probe < 64; probe++, k = (k + 1) & 16383) { if (g_pw_qseen[k] == h) break; if (!g_pw_qseen[k]) { g_pw_qseen[k] = h; g_pw_progs_known++; break; } }
    }
    LeaveCriticalSection(&g_shw_lock);
    fclose(f);
}
/* Worker: one pair job. stage 2 = named by the two text hashes (data: vh, fh, vl, fl); stage 3 = the machine code of a
 * program's vertex stage (code_len, cdata_size) and fragment stage (key[0], key[1]; 0 = none: depth-only). */
static void pw_job(ShJob *j, GLuint pipe, GLuint empty_fs) {
    uint64_t vh = 0, fh = 0;
    uint32_t vl = 0, fl = 0, mask = 0;             /* mask: the vertex arrays that are enabled for the warm draw */
    char *vt = NULL, *ft = NULL;
    if (j->stage == 2) { memcpy(&vh, j->data, 8); memcpy(&fh, j->data + 8, 8); memcpy(&vl, j->data + 16, 4); memcpy(&fl, j->data + 20, 4); memcpy(&mask, j->data + 24, 4); }
    else {
        const uint8_t *vc = j->data, *vd = vc + j->code_len, *fc = vd + j->cdata_size, *fd = fc + j->key[0];
        vt = pw_translate(0, vc, j->code_len, vd, j->cdata_size, &mask);      /* a program handed over: every input the vertex stage reads comes from an array */
        mask &= 0xffff;
        if (j->key[0]) ft = pw_translate(1, fc, (int)j->key[0], fd, j->key[1], NULL);
        if (!vt || (j->key[0] && !ft)) { free(vt); free(ft); InterlockedIncrement64(&g_pw_missing); return; }
        vl = (uint32_t)strlen(vt); vh = sh_hash(0x9E3779B97F4A7C15ULL ^ 0, vt, vl);
        if (ft) { fl = (uint32_t)strlen(ft); fh = sh_hash(0x9E3779B97F4A7C15ULL ^ 1, ft, fl); }
    }
    if (pw_seen(vh, vl, fh, fl, mask)) { free(vt); free(ft); InterlockedIncrement64(&g_pw_dup); pw_prog_known(j); return; }
    if (mask & 0x10000) {                          /* a compute program: one work group with no buffer and no image bound (reads give 0, writes go nowhere) */
        GLuint cs = pw_load(5, vh, vl, NULL);
        if (cs) {
            static LONG64 freq;
            if (!freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart; }
            glUseProgramStages(pipe, 0x20 /* COMPUTE_SHADER_BIT */, cs);
            LONG64 t0 = pf_now();
            glDispatchCompute(1, 1, 1);
            glFinish();
            LONG64 dt = pf_now() - t0;
            InterlockedExchangeAdd64(&g_pw_ticks, dt); InterlockedIncrement64(&g_pw_warmed);
            if (dt * 125 > freq) { InterlockedIncrement64(&g_pw_cold_n); InterlockedExchange(&g_pw_cold, 1); }
            if (g_fdlog) printf("fd-warm: %016llx %x %016llx %x mask %x kind %d %.1f ms frame %lld%c", (unsigned long long)vh, vl, 0ull, 0u, mask, j->stage, (double)dt * 1000.0 / (double)freq, (long long)g_st.frames, 10);
            glUseProgramStages(pipe, 0x20, 0);
            glDeleteProgram(cs);
        } else InterlockedIncrement64(&g_pw_missing);
        while (glGetError()) {}
        return;
    }
    GLuint vs = pw_load(0, vh, vl, vt), fs = fl ? pw_load(1, fh, fl, ft) : empty_fs;
    free(vt); free(ft);
    if (vs && fs) {
        glUseProgramStages(pipe, 1, vs);
        glUseProgramStages(pipe, 2, fs);
        for (GLuint k = 0; k < 16; k++) { if (mask >> k & 1) glEnableVertexAttribArray(k); else glDisableVertexAttribArray(k); }
        LONG64 t0 = pf_now();
        glDrawArrays(GL_POINTS, 0, 1);
        glFinish();
        LONG64 dt = pf_now() - t0;
        static LONG64 freq;
        if (!freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart; }
        InterlockedExchangeAdd64(&g_pw_ticks, dt); InterlockedIncrement64(&g_pw_warmed);
        if (dt * 125 > freq) { InterlockedIncrement64(&g_pw_cold_n); InterlockedExchange(&g_pw_cold, 1); }
        if (g_fdlog) printf("fd-warm: %016llx %x %016llx %x mask %x kind %d %.1f ms frame %lld%c", (unsigned long long)vh, vl, (unsigned long long)fh, fl, mask, j->stage, (double)dt * 1000.0 / (double)freq, (long long)g_st.frames, 10);
        glUseProgramStages(pipe, 1, 0);
        glUseProgramStages(pipe, 2, 0);
        if (j->stage == 3 && !getenv("SSBU_NO_PAIR_LIST")) {          /* into <cache>/pairs.txt (same line format as prec_note) */
            char path[260];
            snprintf(path, sizeof path, "%s/pairs.txt", g_shd_dir);
            EnterCriticalSection(&g_shw_lock);
            FILE *f = fopen(path, "a");
            if (f) { fprintf(f, "%016llx %x %016llx %x %x\n", (unsigned long long)vh, (unsigned)vl, (unsigned long long)fh, (unsigned)fl, (unsigned)mask); fclose(f); }
            LeaveCriticalSection(&g_shw_lock);
            pw_prog_known(j);
        }
    } else InterlockedIncrement64(&g_pw_missing);
    if (vs) glDeleteProgram(vs);
    if (fs && fs != empty_fs) glDeleteProgram(fs);
    while (glGetError()) {}
}
static void pw_enqueue(ShJob *j) {
    j->next = NULL;
    EnterCriticalSection(&g_shw_lock);
    if (g_shw_tail) g_shw_tail->next = j; else g_shw_head = j;
    g_shw_tail = j;
    LeaveCriticalSection(&g_shw_lock);
    InterlockedIncrement64(&g_shw_queued);
    ReleaseSemaphore(g_shw_sem, 1, NULL);
}
/* Start-up (prec_load): a pair that a draw used in an earlier run. */
static void pw_queue_named(uint64_t vh, uint32_t vl, uint64_t fh, uint32_t fl, uint32_t mask) {
    if (!pw_mode() || g_shd_mode <= 0 || !g_shw_n) return;
    ShJob *j = calloc(1, sizeof *j + 28);
    j->stage = 2;
    memcpy(j->data, &vh, 8); memcpy(j->data + 8, &fh, 8); memcpy(j->data + 16, &vl, 4); memcpy(j->data + 20, &fl, 4); memcpy(j->data + 24, &mask, 4);
    g_pw_named++;
    pw_enqueue(j);
}
static volatile LONG64 g_pw_gate_t0;
/* boot.c: milliseconds presents were held for shader preparation so far (not counted by the run-time limit). */
unsigned nvn_gl_held_ms(void) {
    LARGE_INTEGER f;
    LONG64 t0 = g_pw_gate_t0, ticks = g_pw_gate_ticks + (t0 ? pf_now() - t0 : 0);
    QueryPerformanceFrequency(&f);
    return (unsigned)(ticks * 1000 / f.QuadPart);
}
/* Render thread, before a present: hold it while cold shader work is pending (see above). */
static void pw_gate(void) {
    static int off = -1;
    if (off < 0) { const char *e = getenv("SSBU_PAIR_GATE"); off = (e && !strcmp(e, "0")) || !pw_mode(); }
    if (off || !g_pw_cold) return;
    if (g_shw_queued == g_shw_done || g_shw_ctx_failed) { InterlockedExchange(&g_pw_cold, 0); return; }
    LONG64 t0 = pf_now(), last = t0, freq, start_done = g_shw_done;
    char title[200] = "", text[300];
    g_pw_gate_t0 = t0;
    { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart; }
    HS_IN("shader preparation (present held while the workers prepare programs)", 0);
    while (g_shw_queued != g_shw_done) {
        Sleep(2);
        if (pf_now() - last > freq) {            /* once per second while it lasts: the log and the window title */
            if (!title[0] && !GetWindowTextA(g_hwnd, title, sizeof title)) strcpy(title, " ");
            last = pf_now();
            snprintf(text, sizeof text, "%s - preparing shaders: %lld to go", title, (long long)(g_shw_queued - g_shw_done));
            SetWindowTextA(g_hwnd, text);
            printf("nvn_gl: preparing shaders, present held for %.0f s so far: %lld jobs to go (%lld pairs made ready, %lld of them not in the driver's cache)%c", (double)(last - t0) / (double)freq,
                   (long long)(g_shw_queued - g_shw_done), (long long)g_pw_warmed, (long long)g_pw_cold_n, 10);
            fflush(stdout);
        }
    }
    HS_OUT();
    if (title[0]) SetWindowTextA(g_hwnd, title);
    InterlockedExchange(&g_pw_cold, 0);
    g_pw_gate_ticks += pf_now() - t0; g_pw_gates++;
    g_pw_gate_t0 = 0;
    printf("nvn_gl: shader preparation held the present of frame %lld for %.2f s (%lld jobs)%c", (long long)g_st.frames + 1, (double)(pf_now() - t0) / (double)freq, (long long)(g_shw_done - start_done), 10);
}
static DWORD WINAPI shd_worker(LPVOID arg) {
#ifndef _WIN32
    (void)arg; InterlockedIncrement(&g_shw_ctx_failed); return 0;      /* Linux build: no OpenGL renderer */
#else
    PIXELFORMATDESCRIPTOR pfd;
    (void)arg;
    HWND w = CreateWindowExA(0, "STATIC", "", WS_POPUP, 0, 0, 8, 8, NULL, NULL, NULL, NULL);
    HDC dc = w ? GetDC(w) : NULL;
    memset(&pfd, 0, sizeof pfd);
    pfd.nSize = sizeof pfd; pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32;
    if (!dc || !SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd)) return 0;
    HGLRC rc = wglCreateContext(dc);
    if (!rc || !wglMakeCurrent(dc, rc)) { InterlockedIncrement(&g_shw_ctx_failed); return 0; }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    GLuint pw_pipe = 0, pw_empty = 0;                  /* P3 round 4: the pair jobs draw one discarded point through this pipeline */
    if (!getenv("SSBU_GL_DRIVER_THREAD")) { glEnable(GL_DEBUG_OUTPUT_); glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS_); }
    {   static const char *const src[1] = {"#version 450\nvoid main() {}\n"};
        static const float zero[64];
        GLuint vb = 0;
        glGenProgramPipelines(1, &pw_pipe);
        glBindProgramPipeline(pw_pipe);
        glEnable(GL_RASTERIZER_DISCARD_);
        pw_empty = glCreateShaderProgramv(0x8B30, 1, src);
        glGenBuffers(1, &vb);                          /* every vertex array reads this small buffer of zeros */
        glBindBuffer(GL_ARRAY_BUFFER_, vb);
        glBufferData(GL_ARRAY_BUFFER_, (ptrdiff_t)sizeof zero, zero, GL_STREAM_DRAW_);
        glBindVertexBuffer(0, vb, 0, 16);
        for (GLuint k = 0; k < 16; k++) { glVertexAttribFormat(k, 4, 0x1406 /* FLOAT */, 0, 0); glVertexAttribBinding(k, 0); }
    }
    for (;;) {
        WaitForSingleObject(g_shw_sem, INFINITE);
        EnterCriticalSection(&g_shw_lock);
        ShJob *j = g_shw_head;
        if (j) { g_shw_head = j->next; if (!g_shw_head) g_shw_tail = NULL; }
        LeaveCriticalSection(&g_shw_lock);
        if (!j) continue;
        if (j->stage >= 2) { pw_job(j, pw_pipe, pw_empty); InterlockedIncrement64(&g_shw_done); free(j); continue; }
        MxIn in;
        MxOut out;
        memset(&in, 0, sizeof in);
        in.stage = j->stage; in.code = j->data; in.code_len = j->code_len;
        if (j->cdata_size) { in.cdata = j->data + j->code_len; in.cdata_size = j->cdata_size; }
        in.binding_base = (unsigned)j->stage * 20; in.ssbo_base = 40 + (unsigned)j->stage * 16;
        in.int_attrs = j->key[0]; in.sint_attrs = j->key[1]; in.int_tex = j->key[2]; in.sint_tex = j->key[3];
        in.fragdebug = getenv("SSBU_FRAGDEBUG") != NULL;
        if (mx_translate(&in, &out)) {
            char path[320];
            size_t len = strlen(out.glsl);
            shd_path(path, sizeof path, sh_hash(0x9E3779B97F4A7C15ULL ^ (uint64_t)j->stage, out.glsl, len), len);
            if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) InterlockedIncrement64(&g_shw_present);
            else {
                GLint ok = 0;
                int saved = 0;
                GLuint q = shd_compile_save(j->stage == 0 ? 0x8B31 : 0x8B30, out.glsl, path, &ok, &saved);
                if (q) glDeleteProgram(q);
                InterlockedExchange(&g_pw_cold, 1);            /* a compile: presents wait for the workers (pw_gate) */
                if (saved) InterlockedIncrement64(&g_shw_compiled); else InterlockedIncrement64(&g_shw_failed);
                while (glGetError()) {}
            }
            free(out.glsl);
        } else InterlockedIncrement64(&g_shw_failed);
        InterlockedIncrement64(&g_shw_done);
        free(j);
    }
#endif
}
static void shd_workers_start(void) {
    const char *e = getenv("SSBU_SHADER_WORKERS");
    g_shw_n = e ? atoi(e) : 3;
    if (g_shw_n < 0) g_shw_n = 0;
    if (g_shw_n > 8) g_shw_n = 8;
    InitializeCriticalSection(&g_shw_lock);
    for (int i = 0; i < 64; i++) InitializeCriticalSection(&g_pw_stripe[i]);
    if (!getenv("SSBU_NO_SHADER_VARIANTS")) shv_load();
    g_shw_sem = CreateSemaphoreA(NULL, 0, 0x7fffffff, NULL);
    for (int i = 0; i < g_shw_n; i++) CloseHandle(CreateThread(NULL, 0, shd_worker, NULL, 0, NULL));
}
/* Guest thread, nvnProgramSetShaders: a vertex or fragment stage given as machine code. */
static void shd_prewarm(int stage, const uint8_t *code, int code_len, const uint8_t *cdata, uint32_t cdata_size) {
    if (g_shd_mode <= 0 || !g_shw_n || stage > 1 || code_len <= 0) return;
    uint64_t h = sh_hash(0xcbf29ce484222325ULL ^ (uint64_t)stage, code, (size_t)code_len);
    if (cdata) h = sh_hash(h ^ cdata_size, cdata, cdata_size);
    if (!h) h = 1;
    ShJob *j = malloc(sizeof *j + (size_t)code_len + cdata_size);
    j->next = NULL; j->stage = stage; j->code_len = code_len; j->cdata_size = cdata ? cdata_size : 0;
    memset(j->key, 0, sizeof j->key);
    memcpy(j->data, code, (size_t)code_len);
    if (cdata) memcpy(j->data + code_len, cdata, cdata_size);
    EnterCriticalSection(&g_shw_lock);
    unsigned k = (unsigned)(h >> 20) & 16383;
    int seen = 0, extra = 0;
    for (int probe = 0; probe < 64; probe++, k = (k + 1) & 16383) { if (g_shw_seen[k] == h) { seen = 1; break; } if (!g_shw_seen[k]) { g_shw_seen[k] = h; break; } }
    if (!seen) {
        if (g_shw_tail) g_shw_tail->next = j; else g_shw_head = j;
        g_shw_tail = j;
        for (ShVar *e = g_shv[h & 4095]; e; e = e->next) {         /* the variants recorded in earlier runs (variants.txt) */
            if (e->h != h || e->stage != stage) continue;
            ShJob *v = malloc(sizeof *v + (size_t)code_len + cdata_size);
            memcpy(v, j, sizeof *v + (size_t)code_len + (cdata ? cdata_size : 0));
            v->next = NULL; memcpy(v->key, e->key, 16);
            g_shw_tail->next = v; g_shw_tail = v;
            extra++;
        }
    }
    LeaveCriticalSection(&g_shw_lock);
    if (seen) { free(j); return; }
    InterlockedExchangeAdd64(&g_shw_queued, 1 + extra);
    g_shv_queued += extra;
    ReleaseSemaphore(g_shw_sem, 1 + extra, NULL);
    /* P3 round 2: nvnProgramSetShaders holds its caller while every worker is busy compiling (not during start-up, where
     * the game hands over 6,800 stages at once and nothing of them is drawn yet). A stage or fighter loaded later brings
     * stages that were never compiled on this PC; without this the game finished loading while the workers were still
     * compiling, and the first draws compiled the same stages again on the render thread, in the middle of the battle
     * (7 vertex stages, 5.3 s, tests/logs/p3b_r2a.log). On the console the program is ready when this call returns; here
     * the loading takes as long as the compiler needs, once per stage per PC. SSBU_SHADER_NOWAIT=1 = never hold. */
    {
        static int nowait = -1;
        if (nowait < 0) nowait = getenv("SSBU_SHADER_NOWAIT") != NULL;
        if (!nowait && g_st.frames >= 300 && GetCurrentThreadId() != g_render_tid && g_shw_queued - g_shw_done >= g_shw_n) {
            LONG64 t0 = pf_now(), freq = 0;
            { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart; }
            while (g_shw_queued - g_shw_done >= g_shw_n && pf_now() - t0 < 30 * freq) Sleep(1);
            InterlockedExchangeAdd64(&g_shw_held_ticks, pf_now() - t0); InterlockedIncrement64(&g_shw_held);
        }
    }
}
/* ---- P3 round 3: linked program pairs (SSBU_PAIR=1; default off until measured)
 * The two stages of a draw are separable programs in one pipeline object, and this driver does its last piece of work
 * for a (vertex, fragment) combination inside the first draw call that uses it (50-90 ms; it keeps the result in a
 * cache of its own that depends on the name of the executable). Here the same two TEXTS are also linked into one
 * program; its binary goes into our cache folder under the hash of both texts (file <hash>_<total length>_<driver>.bin,
 * same folder and driver key as the stage binaries, nothing in it depends on the executable). A draw uses the linked
 * program when it is there (same shaders, same bindings: every binding is fixed by a layout qualifier in the text; the
 * only uniform, the alpha test at location 0, is set on whichever program is in use); when the binary is not on disk
 * yet the draw goes through the pipeline exactly as before and the pair is linked for the next time. */
static int pair_mode(void) { static int m = -1; if (m < 0) { const char *e = getenv("SSBU_PAIR"); m = e ? atoi(e) : 0; } return m; }
typedef struct { uint64_t key; GLuint prog; int state; } PairEnt;      /* state 0 = free, 1 = ready, 2 = no linked program (pipeline) */
static PairEnt g_pairs[16384];
static LONG64 g_pair_loaded, g_pair_linked, g_pair_failed, g_pair_draws, g_pair_refused;
static void pair_off(void) { if (g_pair_cur) { glUseProgram(0); g_pair_cur = 0; } }
static GLuint pair_link(const char *vs, const char *fs, const char *path, int *saved) {
    GLint ok = 0, n = 0;
    GLuint sh[2] = {glCreateShader(0x8B31), glCreateShader(0x8B30)}, q = glCreateProgram();
    const char *src[2] = {vs, fs};
    *saved = 0;
    for (int i = 0; i < 2; i++) { glShaderSource(sh[i], 1, &src[i], NULL); glCompileShader(sh[i]); glAttachShader(q, sh[i]); }
    glProgramParameteri(q, 0x8257 /* PROGRAM_BINARY_RETRIEVABLE_HINT */, 1);
    glLinkProgram(q);
    for (int i = 0; i < 2; i++) { glDetachShader(q, sh[i]); glDeleteShader(sh[i]); }
    glGetProgramiv(q, 0x8B82, &ok);
    if (!ok) {
        static int nlog;
        char log[600] = "";
        glGetProgramInfoLog(q, sizeof log - 1, NULL, log);
        logf_limited(&nlog, 6, "nvn_gl: pair program not linked (the pipeline is used for this pair): %s\n", log);
        glDeleteProgram(q);
        return 0;
    }
    glGetProgramiv(q, 0x8741 /* PROGRAM_BINARY_LENGTH */, &n);
    if (n > 0) {
        char tmp[360];
        uint32_t head[2] = {0, 0};
        GLsizei got = 0;
        GLenum fmt = 0;
        void *data = malloc((size_t)n);
        while (glGetError()) {}
        glGetProgramBinary(q, n, &got, &fmt, data);
        if (!glGetError() && got > 0) {
            snprintf(tmp, sizeof tmp, "%s.%lu.%lu.tmp", path, (unsigned long)GetCurrentProcessId(), (unsigned long)GetCurrentThreadId());
            FILE *f = fopen(tmp, "wb");
            if (f) {
                head[0] = fmt; head[1] = (uint32_t)got;
                int good = fwrite(head, 4, 2, f) == 2 && fwrite(data, 1, (size_t)got, f) == (size_t)got;
                fclose(f);
                if (good && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) *saved = 1; else DeleteFileA(tmp);
            }
        }
        free(data);
    }
    return q;
}
/* Render thread, at a draw: the linked program for these two stage programs, 0 = use the pipeline. */
static GLuint pair_get(GLuint vs, GLuint fs, const char *fs_text_fixed) {
    uint64_t key = ((uint64_t)vs << 32) | fs;
    unsigned h = (unsigned)((key * 0x9E3779B97F4A7C15ULL) >> 50);
    PairEnt *e = NULL;
    for (int probe = 0; probe < 64; probe++, h = (h + 1) & 16383) { if (g_pairs[h].state && g_pairs[h].key == key) return g_pairs[h].prog; if (!g_pairs[h].state) { e = &g_pairs[h]; break; } }
    if (!e || g_shd_mode <= 0) return 0;
    e->key = key; e->state = 2; e->prog = 0;
    ShSrc *a = vs < 65536 ? g_prog_src[vs] : NULL, *b = fs_text_fixed ? NULL : fs < 65536 ? g_prog_src[fs] : NULL;
    const char *ft = fs_text_fixed ? fs_text_fixed : b ? b->text : NULL;
    if (!a || !a->text || !ft) return 0;
    size_t flen = strlen(ft);
    char path[320];
    uint64_t ph = sh_hash(sh_hash(0x50414953ULL, a->text, a->len) ^ a->len, ft, flen);
    shd_path(path, sizeof path, ph, a->len + flen);
    FILE *f = fopen(path, "rb");
    if (f) {
        uint32_t head[2] = {0, 0};
        if (fread(head, 4, 2, f) == 2 && head[1] > 0 && head[1] < (64u << 20)) {
            void *data = malloc(head[1]);
            if (fread(data, 1, head[1], f) == head[1]) {
                GLint ok = 0;
                GLuint q = glCreateProgram();
                while (glGetError()) {}
                HS_IN("pair program binary from disk (glProgramBinary)", 0);
                glProgramBinary(q, head[0], data, (GLsizei)head[1]);
                glGetProgramiv(q, 0x8B82, &ok);
                HS_OUT();
                while (glGetError()) {}
                if (ok) { e->prog = q; e->state = 1; g_pair_loaded++; } else { glDeleteProgram(q); g_pair_refused++; }
            }
            free(data);
        }
        fclose(f);
        if (e->state == 1) return e->prog;
    }
    if (pair_mode() == 1) {            /* 1: link here, at the first draw (measurement of the idea); 2: only use what is on disk */
        int saved = 0;
        /* Both stage texts name their buffer blocks CB<n> / SB<n> (mx_translate.c) with different bindings; in ONE program
         * blocks of the same name are the same block, so the fragment stage's are renamed FB<n> / TB<n> for the link
         * (block names are not used anywhere else in the text; the members c<n> / s<n> belong to each stage's own block). */
        char *ft2 = _strdup(ft);
        for (char *p = ft2; (p = strstr(p, " buffer CB")) != NULL; p += 9) p[8] = 'F';
        for (char *p = ft2; (p = strstr(p, " buffer SB")) != NULL; p += 9) p[8] = 'T';
        /* ... and so are their members: the translator writes them only as c<n>[ and s<n>[ (whole identifiers), and no
         * identifier of the text starts with K or Q; an unnamed block's member is a global name of the whole program. */
        for (char *p = ft2 + 1; *p; p++) {
            if ((*p != 'c' && *p != 's') || p[1] < '0' || p[1] > '9') continue;
            char b = p[-1];
            if ((b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9') || b == '_' || b == '.') continue;
            char *e = p + 1;
            while (*e >= '0' && *e <= '9') e++;
            if (*e == '[') *p = *p == 'c' ? 'K' : 'Q';
        }
        HS_IN("pair program compile + link (driver)", 0);
        GLuint q = pair_link(a->text, ft2, path, &saved);
        HS_OUT();
        free(ft2);
        while (glGetError()) {}
        if (q) { e->prog = q; e->state = 1; g_pair_linked++; } else g_pair_failed++;
    }
    return e->prog;
}
/* ---- P3 round 4: the list of shader pairs that were drawn (<cache>/pairs.txt)
 * One line per (vertex text, fragment text, enabled arrays) combination a draw used: "<vs hash> <vs length> <fs hash>
 * <fs length> <mask>": the names the two stage binaries have in the cache folder (fragment 0 0 = the empty fragment
 * shader of a depth-only pass) and which of the vertex attribute arrays the vertex stage reads were enabled (the
 * driver's code for a pair depends on exactly that, p4lab mode M). Read at start (prec_load), appended to when a draw
 * uses a combination that is not in it. */
typedef struct PairRec { uint64_t vh, fh; uint32_t vl, fl, mask; struct PairRec *next; } PairRec;
static PairRec *g_prec[4096];
static LONG64 g_prec_loaded, g_prec_added;
static int g_prec_off = -1;
static int prec_has(uint64_t vh, uint32_t vl, uint64_t fh, uint32_t fl, uint32_t mask, int add) {
    unsigned k = (unsigned)((vh ^ fh * 0x9E3779B97F4A7C15ULL) >> 20) & 4095;
    for (PairRec *e = g_prec[k]; e; e = e->next) if (e->vh == vh && e->fh == fh && e->vl == vl && e->fl == fl && e->mask == mask) return 1;
    if (add) { PairRec *e = calloc(1, sizeof *e); e->vh = vh; e->vl = vl; e->fh = fh; e->fl = fl; e->mask = mask; e->next = g_prec[k]; g_prec[k] = e; }
    return 0;
}
/* Render thread, at a draw through the pipeline: note the pair of GL stage programs (fs_empty: depth-only pass). */
static void prec_note(GLuint vs, GLuint fs, int fs_empty, uint32_t mask) {
    static uint64_t seen[1 << 15];
    if (g_prec_off < 0) g_prec_off = getenv("SSBU_NO_PAIR_LIST") != NULL || g_shd_mode <= 0;
    if (g_prec_off || fs >= 65536) return;
    uint64_t pair = ((uint64_t)vs << 32) | (uint64_t)(mask & 0xffff) << 16 | fs | 1ull << 63;
    unsigned h = (unsigned)((pair * 0x9E3779B97F4A7C15ULL) >> 49);
    for (int probe = 0; probe < 64; probe++, h = (h + 1) & 0x7fff) { if (seen[h] == pair) return; if (!seen[h]) { seen[h] = pair; break; } }
    ShSrc *a = vs < 65536 ? g_prog_src[vs] : NULL, *b = fs_empty ? NULL : fs < 65536 ? g_prog_src[fs] : NULL;
    if (!a || (!b && !fs_empty)) return;
    if (prec_has(a->h, (uint32_t)a->len, b ? b->h : 0, b ? (uint32_t)b->len : 0, mask, 1)) return;
    char path[260];
    snprintf(path, sizeof path, "%s/pairs.txt", g_shd_dir);
    FILE *f = fopen(path, "a");
    if (f) { fprintf(f, "%016llx %x %016llx %x %x\n", (unsigned long long)a->h, (unsigned)a->len, (unsigned long long)(b ? b->h : 0), b ? (unsigned)b->len : 0, (unsigned)mask); fclose(f); g_prec_added++; }
}
/* Render thread: a compute program was built (its first dispatch follows). List line: <hash> <length> 0 0 10000. */
static void prec_note_compute(uint64_t h, size_t len) {
    char path[260];
    if (g_prec_off < 0) g_prec_off = getenv("SSBU_NO_PAIR_LIST") != NULL || g_shd_mode <= 0;
    if (g_prec_off || prec_has(h, (uint32_t)len, 0, 0, 0x10000, 1)) return;
    snprintf(path, sizeof path, "%s/pairs.txt", g_shd_dir);
    FILE *f = fopen(path, "a");
    if (f) { fprintf(f, "%016llx %x %016llx %x %x\n", (unsigned long long)h, (unsigned)len, 0ull, 0u, 0x10000u); fclose(f); g_prec_added++; }
}
/* Render thread, start: the stage program of a listed pair is loaded from its binary now, not at its first draw. */
static LONG64 g_pre_n, g_pre_ticks;
static void prec_preload(int stage, uint64_t h, uint32_t len) {
    static int off = -1;
    if (off < 0) { const char *e = getenv("SSBU_PAIR_PRELOAD"); off = (e && !strcmp(e, "0")) || g_shc_off > 0 || getenv("SSBU_NO_SHADERCACHE") != NULL; }
    if (off || !len) return;
    for (ShSrc *e = g_shs[h & 4095]; e; e = e->next) if (e->h == h && e->len == len) return;
    LONG64 t0 = pf_now();
    GLuint q = pw_load(stage, h, len, NULL);
    g_pre_ticks += pf_now() - t0;
    if (!q) return;
    ShSrc *e = calloc(1, sizeof *e);
    e->h = h; e->len = len; e->prog = q; e->next = g_shs[h & 4095]; g_shs[h & 4095] = e;
    if (q < 65536) g_prog_src[q] = e;
    g_pre_n++; g_shd_loaded++;
}
static void prec_load(void) {                      /* render thread, once (shd_init) */
    char path[260], line[200];
    if (g_prec_off < 0) g_prec_off = getenv("SSBU_NO_PAIR_LIST") != NULL || g_shd_mode <= 0;
    if (g_prec_off) return;
    snprintf(path, sizeof path, "%s/pairs.txt", g_shd_dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        unsigned long long vh, fh; unsigned vl, fl, mask;
        if (sscanf(line, "%llx %x %llx %x %x", &vh, &vl, &fh, &fl, &mask) == 5 && !prec_has(vh, vl, fh, fl, mask, 1)) {
            g_prec_loaded++;
            pw_queue_named(vh, vl, fh, fl, mask);
            if (!(mask & 0x10000)) { prec_preload(0, vh, vl); prec_preload(1, fh, fl); }
        }
    }
    fclose(f);
    pw_progs_load();
    { LARGE_INTEGER q; QueryPerformanceFrequency(&q); printf("nvn_gl: %lld programs known by their machine code (progs.txt)%c", (long long)g_pw_progs_known, 10); }
    { LARGE_INTEGER q; QueryPerformanceFrequency(&q); printf("nvn_gl: %lld listed shader pairs queued for the workers; %lld stage binaries loaded ahead in %.0f ms%c", (long long)g_prec_loaded, (long long)g_pre_n, (double)g_pre_ticks * 1000.0 / (double)q.QuadPart, 10); }
}
static int stage_glsl(Prog *p, int s, const uint32_t *key) {
    static int nfail, nlink;
    Stage *st = &p->st[s];
    MxIn in;
    MxOut out;
    if (!st->code || s > 1) return 0;
    if (!st->dumped) { st->dumped = 1; g_st.binary_used++; dump_binary(p, s); }
    if (st->glsl_state == 2) return 0;
    uint32_t dep_a = s == 0 ? st->glsl_dep : 0;
    if (st->glsl_state == 1 && ((key[0] ^ st->glsl_key[0]) & dep_a) == 0 && ((key[1] ^ st->glsl_key[1]) & dep_a) == 0 &&
        ((key[2] ^ st->glsl_key[2]) & st->glsl_tex) == 0 && ((key[3] ^ st->glsl_key[3]) & st->glsl_tex) == 0) return 1;
    if (g_shc_off < 0) g_shc_off = getenv("SSBU_NO_SHADERCACHE") != NULL;
    if (st->glsl && g_shc_off) glDeleteProgram(st->glsl);
    st->glsl = 0;
    memset(&in, 0, sizeof in);
    in.stage = s; in.code = st->code; in.code_len = st->code_len;
    if (st->ctl[3] && st->ctl[4]) { in.cdata = st->code - 0x30 + st->ctl[4]; in.cdata_size = st->ctl[3]; }
    uint64_t h1 = 0, h0 = 0;
    if (!g_shc_off) {
        h1 = sh_hash(0xcbf29ce484222325ULL ^ (uint64_t)s, st->code, (size_t)st->code_len);
        if (in.cdata) h1 = sh_hash(h1 ^ in.cdata_size, in.cdata, in.cdata_size);
        h0 = h1 ? h1 : 1;                           /* the workers' name for this stage (shd_prewarm) */
        h1 = sh_hash(h1, key, 16);
        for (ShEnt *e = g_shc[h1 & 4095]; e; e = e->next)
            if (e->h == h1 && e->stage == s && e->code_len == st->code_len && !memcmp(e->key, key, 16)) {
                st->glsl = e->prog; st->glsl_state = 1;
                memcpy(st->glsl_key, key, sizeof st->glsl_key);
                st->glsl_cbufs = e->cbufs; st->glsl_ssbos = e->ssbos; st->glsl_dep = e->in_attr; st->glsl_tex = e->tex_units; st->glsl_alpha = e->uses_alpha; st->glsl_ssbo_w = e->ssbo_written;
                g_shc_hit++;
                return 1;
            }
    }
    in.int_attrs = key[0]; in.sint_attrs = key[1]; in.int_tex = key[2]; in.sint_tex = key[3];
    in.binding_base = (unsigned)s * 20; in.ssbo_base = 40 + (unsigned)s * 16;
    in.fragdebug = getenv("SSBU_FRAGDEBUG") != NULL;
    LONG64 pf0 = pf_now();
    int xl;
    { HS_IN("shader translate (machine code to GLSL)", 0); xl = mx_translate(&in, &out); HS_OUT(); }
    pf_add(PF_XLATE, pf0);
    if (!xl) {
        st->glsl_state = 2;
        g_st.glsl_fail++;
        logf_limited(&nfail, 60, "nvn_gl: program #%d %s stage: machine code not translated: %s\n", p->serial, k_stage_name[s], out.why);
        return 0;
    }
    char *src = out.glsl;
    const char *dir = getenv("SSBU_SHADERDUMP");
    if (dir) {
        char path[260];
        snprintf(path, sizeof path, "%s/b%05d_%s.glsl", dir, p->serial, k_stage_name[s]);
        FILE *f = fopen(path, "wb");
        if (f) { fputs(src, f); fclose(f); }
    }
    const char *srcs[1] = {src};
    GLint ok = 0;
    size_t src_len = strlen(src);
    uint64_t h2 = g_shc_off ? 0 : sh_hash(0x9E3779B97F4A7C15ULL ^ (uint64_t)s, src, src_len);
    ShSrc *hit = NULL;
    for (ShSrc *e = g_shc_off ? NULL : g_shs[h2 & 4095]; e && !hit; e = e->next) if (e->h == h2 && e->len == src_len) hit = e;
    if (hit) { st->glsl = hit->prog; ok = 1; g_shc_src_hit++; }
    else {
        pf0 = pf_now();
        LONG64 saved0 = g_shd_saved;
        st->glsl = g_shc_off ? glCreateShaderProgramv(s == 0 ? 0x8B31 : 0x8B30, 1, srcs) : shader_build(s == 0 ? 0x8B31 : 0x8B30, src, src_len, h2, &ok);
        if (g_shc_off) glGetProgramiv(st->glsl, 0x8B82, &ok);
        pf_add(PF_GLCOMPILE, pf0);
        if (g_shd_saved != saved0 && h0) shv_record(h0, s, key);
        g_shc_compiled++;
        if (ok && !g_shc_off) { ShSrc *e = calloc(1, sizeof *e); e->h = h2; e->len = src_len; e->prog = st->glsl; e->next = g_shs[h2 & 4095]; g_shs[h2 & 4095] = e;
            if (st->glsl < 65536) { if (pair_mode()) e->text = _strdup(src); g_prog_src[st->glsl] = e; } }
    }
    if (!ok) {
        char log[2000] = "";
        glGetProgramInfoLog(st->glsl, sizeof log - 1, NULL, log);
        if (nlink < 8) {
            char path[64];
            snprintf(path, sizeof path, "shader_fail_glsl_%02d.txt", nlink);
            FILE *f = fopen(path, "wb");
            if (f) { fputs(src, f); fclose(f); }
        }
        logf_limited(&nlink, 20, "nvn_gl: program #%d %s stage: the driver refuses the translated GLSL:\n%s\n", p->serial, k_stage_name[s], log);
        glDeleteProgram(st->glsl);
        st->glsl = 0; st->glsl_state = 2;
        g_st.glsl_fail++;
        free(src);
        return 0;
    }
    free(src);
    st->glsl_state = 1;
    memcpy(st->glsl_key, key, sizeof st->glsl_key);
    st->glsl_cbufs = out.cbufs; st->glsl_ssbos = out.ssbos; st->glsl_dep = out.in_attr; st->glsl_tex = out.tex_units; st->glsl_alpha = out.uses_alpha; st->glsl_ssbo_w = out.ssbo_written;
    if (out.ssbo_written) { static int nw; logf_limited(&nw, 5, "nvn_gl: program #%d %s stage writes storage buffers (mask 0x%x): results are not copied back to guest memory\n", p->serial, k_stage_name[s], out.ssbo_written); }
    if (!g_shc_off) {
        ShEnt *e = calloc(1, sizeof *e);
        e->h = h1; e->stage = s; e->code_len = st->code_len; memcpy(e->key, key, 16); e->prog = st->glsl;
        e->cbufs = out.cbufs; e->ssbos = out.ssbos; e->in_attr = out.in_attr; e->tex_units = out.tex_units; e->ssbo_written = out.ssbo_written; e->uses_alpha = out.uses_alpha;
        e->next = g_shc[h1 & 4095]; g_shc[h1 & 4095] = e;
    }
    g_st.glsl_ok++;
    return 1;
}

static void gl_housekeeping(void) {
    if (!g_dead_n) return;
    EnterCriticalSection(&g_dead_lock);
    for (int i = 0; i < g_dead_n; i++) {
        if (g_dead[i].kind == K_TEX) tex_free_gl(g_dead[i].p);
        else if (g_dead[i].kind == K_PROG) {
            Prog *p = g_dead[i].p;
            for (int s = 0; s < 6; s++) { if (p->st[s].gl) glDeleteProgramsARB(1, &p->st[s].gl); if (p->st[s].glsl && (s > 1 || g_shc_off > 0)) glDeleteProgram(p->st[s].glsl); free(p->st[s].text); memset(&p->st[s], 0, sizeof p->st[s]); }
        }
    }
    g_dead_n = 0;
    LeaveCriticalSection(&g_dead_lock);
}
/* ---------------------------------------------------------------- GPU state as left by the commands executed so far */
static struct {
    TexPool *tpool; SamplerPool *spool;
    Prog *prog[6];
    Range ubo[6][16], ssbo[6][32], vbuf[32];
    uint64_t nvn_img[6][8];                       /* image handles (nvnCommandBufferBindImage) */
    uint64_t nvn_tex[6][32], gl_tex[6][112];      /* constant buffer 2 as the shaders see it (mx_translate.h): 32 bytes reserved, one 64-bit handle per unit,
                                                     8 image handle slots (bytes 288..351, not read: images go to image units), then from byte 384
                                                     four words per unit: width, height, depth or layers, levels | is3d << 16 */
    uint32_t tex_int[6], tex_sint[6];             /* units holding integer formats (and which are signed) */
    int tex_dirty[6]; size_t tex_off[6]; uint32_t tex_gen[6];
    uint32_t vattr[32]; int nattr;
    struct { uint32_t stride, divisor; } vstream[32];
    uint64_t ds;
    uint32_t chanmask;
    int sref[2], svmask[2], swmask[2], stencil_dirty;
    float alpha_ref; int alpha_func;
    int have_fbo, fbo_w, fbo_h; GLuint fbo;
    Tex *color0, *color[8];
    int64_t row_stride, image_stride;
    int attr_enabled[16];
    uint8_t ms[24]; unsigned ms_ext;              /* the bound multisample state, and which of its extensions are on in OpenGL */
    int sc0[4], render_off, debug_depth, sample_shading;
} S;

static GLenum nvn_func(unsigned f) { return GL_NEVER + (f >= 1 && f <= 8 ? f - 1 : 7); }
static GLenum blend_func(unsigned f) {
    if (f >= 3 && f <= 0xB) return 0x300 + f - 3;
    switch (f) { case 1: return GL_ZERO; case 2: return GL_ONE; case 0x10: return 0x88F9; case 0x11: return 0x88FA; case 0x12: return 0x8589; case 0x13: return 0x88FB;
                 case 0x61: return 0x8001; case 0x62: return 0x8002; case 0x63: return 0x8003; case 0x64: return 0x8004; }
    return GL_ONE;
}
/* NVNdrawPrimitive has the OpenGL values for 0..9 (points .. polygon); adjacency 10..13 and patches 14 map to the GL enums.
 * Winding: glClipControl(GL_UPPER_LEFT) keeps OpenGL's rule (facing is decided as with a lower-left origin), and NVN is taken to
 * behave the same; SSBU_FLIPFACE inverts it for experiments (SSBU_DRAWLOG shows the samples each draw produces). */
static GLenum nvn_prim(unsigned p) {
    static const GLenum m[15] = {GL_POINTS, GL_LINES, GL_LINE_LOOP, GL_LINE_STRIP, GL_TRIANGLES, GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN, GL_QUADS, GL_QUAD_STRIP, GL_POLYGON,
                                 0x000A, 0x000B, 0x000C, 0x000D, 0x000E};
    return p < 15 ? m[p] : GL_TRIANGLES;
}
static GLenum blend_eq(unsigned e) { static const GLenum m[6] = {0x8006, 0x8006, 0x800A, 0x800B, 0x8007, 0x8008}; return m[e < 6 ? e : 1]; }
static GLenum stencil_op(unsigned o) { static const GLenum m[9] = {GL_KEEP, GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, 0x8507, 0x8508}; return m[o < 9 ? o : 1]; }

static void apply_stencil(void) {
    if (!S.stencil_dirty) return;
    S.stencil_dirty = 0;
    glStencilFuncSeparate(GL_FRONT, nvn_func((unsigned)(S.ds >> 8) & 15), S.sref[0], (GLuint)S.svmask[0]);
    glStencilFuncSeparate(GL_BACK, nvn_func((unsigned)(S.ds >> 12) & 15), S.sref[1], (GLuint)S.svmask[1]);
    glStencilMaskSeparate(GL_FRONT, (GLuint)S.swmask[0]);
    glStencilMaskSeparate(GL_BACK, (GLuint)S.swmask[1]);
}
static void apply_chanmask(void) {
    for (unsigned i = 0; i < 8; i++) { unsigned m = S.chanmask >> (i * 4); glColorMaski(i, m & 1, (m >> 1) & 1, (m >> 2) & 1, (m >> 3) & 1); }
}

static void set_targets(int n, const TargetRef *col, const TargetRef *depth) {
    FboKey k;
    GlFmt f;
    memset(&k, 0, sizeof k);
    S.have_fbo = 0; S.color0 = NULL; S.fbo_w = S.fbo_h = 0;
    memset(S.color, 0, sizeof S.color);
    for (int i = 0; i < n && i < 8; i++) {
        Tex *t = col[i].tex;
        if (!t || t->dead) continue;
        tex_sync(t);
        t->gpu_written = 1;
        k.color[i] = tex_view_obj(t, &col[i].view)->gl;
        k.n = i + 1;
        if (!S.fbo_w) { int l = (col[i].view.set & 0x81) == 0x81 ? col[i].view.lvl0 : 0; S.fbo_w = lvl(t->b.width, l); S.fbo_h = lvl(t->b.height, l); }
        if (i == 0) S.color0 = t;
        S.color[i] = t;
    }
    if (depth && depth->tex && !depth->tex->dead) {
        Tex *t = depth->tex;
        tex_sync(t);
        t->gpu_written = 1;
        k.depth = tex_view_obj(t, &depth->view)->gl;
        k.dkind = gl_fmt(t->b.format, &f) ? f.kind : 1;
        if (!S.fbo_w) { S.fbo_w = t->b.width; S.fbo_h = t->b.height; }
    }
    if (!k.n && !k.depth) { glBindFramebuffer(GL_FRAMEBUFFER_, 0); return; }
    S.fbo = fbo_get(&k);
    S.have_fbo = 1;
    glBindFramebuffer(GL_FRAMEBUFFER_, S.fbo);
}

/* Everything a draw needs that is not plain GL state: programs, their buffers, texture handles, vertex streams. */
static void bind_handle_table(int s, GLenum target, GLuint index, int skip) {
    if (S.tex_dirty[s] || S.tex_gen[s] != g_arena_gen) {
        S.tex_off[s] = arena_put_raw(S.gl_tex[s], sizeof S.gl_tex[s]);
        S.tex_gen[s] = g_arena_gen; S.tex_dirty[s] = 0;
    }
    glBindBufferRange(target, index, g_arena, (ptrdiff_t)S.tex_off[s] + skip, skip ? (ptrdiff_t)256 : (ptrdiff_t)sizeof S.gl_tex[s]);
}
/* Debug aid SSBU_DRAWLOG=<frame>: state of every draw of that frame, and how many samples each draw produced. */
static LONG64 g_drawlog_frame = -1, g_disp_frames;
static int g_drawlog_mindisp_n;     /* with SSBU_DRAWLOG_MIN=<n>: the first frame from <frame> on that follows a frame of at least n draws */
static int drawlog_now(void) {
    static int drawlog = -2, mindraws, mindisp;
    if (drawlog == -2) {
        drawlog = getenv("SSBU_DRAWLOG") ? atoi(getenv("SSBU_DRAWLOG")) : -1; mindraws = getenv("SSBU_DRAWLOG_MIN") ? atoi(getenv("SSBU_DRAWLOG_MIN")) : 0;
        mindisp = getenv("SSBU_DRAWLOG_MINDISP") ? atoi(getenv("SSBU_DRAWLOG_MINDISP")) : 0;
    }
    if (drawlog < 0) return 0;
    if (mindisp) {                             /* SSBU_DRAWLOG_MINDISP=<n>: SSBU_DRAWLOG then counts the frames that had at least n compute dispatches */
        if (g_drawlog_frame < 0 && g_disp_frames >= drawlog) g_drawlog_frame = g_st.frames;
        return g_st.frames == g_drawlog_frame;
    }
    if (!mindraws) return g_st.frames == drawlog;
    if (g_drawlog_frame < 0 && g_st.frames >= drawlog && g_prev_frame_draws >= mindraws) g_drawlog_frame = g_st.frames;
    return g_st.frames == g_drawlog_frame;
}
static GLuint g_dbg_query;
static struct { GLuint q; int active; uint64_t acc; } g_counter[17];
static void drawlog_begin(void) { if (!drawlog_now() || g_counter[1].active) return; if (!g_dbg_query) glGenQueries(1, &g_dbg_query); glBeginQuery(0x8914, g_dbg_query); }
static void drawlog_end(void) {
    GLuint n = 0;
    if (!drawlog_now() || g_counter[1].active) return;
    glEndQuery(0x8914);
    glGetQueryObjectuiv(g_dbg_query, 0x8866, &n);
    printf("  -> %u samples passed\n", n);
}
static int g_dbg_flipface;
static struct { uint64_t addr, size; int prog, n; } g_crange[1024];       /* storage ranges written by this frame's compute dispatches */
static int g_ncrange;
static int crange_hit(uint64_t addr, uint64_t size);
static void compute_flush(void);
static int g_dbg_noscissor, g_dbg_skipdraw = -1, g_frame_draw_index;      /* debug aids: SSBU_NOSCISSOR=1, SSBU_SKIPDRAW=<n-th draw of every frame> */
/* Audit measurement (fake audit, row "unbound or dead texture slots sample a 1x1 dummy texture"): draws whose bound
 * programs reference a texture unit (MxOut.tex_units of the translated stage) that holds the dummy texture; split by cause:
 * the game bound handle 0 to the unit / never bound it, or the handle names a texture that is gone or was never registered. */
static long long g_dummy_draws, g_dummy_unbound, g_dummy_dead;
static void count_dummy_samples(void) {
    int hit = 0;
    for (int s = 0; s < 6; s++) {
        uint32_t m = S.prog[s] ? S.prog[s]->st[s].glsl_tex : 0;
        for (int u = 0; m && u < 32; u++)
            if ((m >> u & 1) && S.gl_tex[s][4 + u] == g_dummy_handle) { hit = 1; if (S.nvn_tex[s][u]) g_dummy_dead++; else g_dummy_unbound++; }
    }
    g_dummy_draws += hit;
}
static int draw_prepare(void) {
    static int nlog;
    uint32_t ints = 0, sints = 0;
    GLuint pn_vs = 0, pn_fs = 0;                /* the pair of GLSL stage programs of this draw (prec_note) */
    int pn_empty = 0;
    compute_flush();
    if (!S.have_fbo || S.render_off) return 0;
    if (g_frame_draw_index++ == g_dbg_skipdraw) return 0;
    for (int i = 0; i < 16 && i < S.nattr; i++) {
        int size, norm, integer = 0;
        GLenum type;
        if ((S.vattr[i] & 0xff) && vfmt_get((int)(S.vattr[i] & 0xff), &size, &type, &norm, &integer) && integer) {
            ints |= 1u << i;
            if (type == 0x1400 || type == 0x1402 || type == 0x1404 || type == 0x8D9F) sints |= 1u << i;
        }
    }
    uint32_t key0[4] = {ints, sints, S.tex_int[0], S.tex_sint[0]}, key1[4] = {0, 0, S.tex_int[1], S.tex_sint[1]};
    /* preferred: both stages translated from machine code to GLSL (separable programs in one pipeline) */
    /* A program with a vertex stage only is a depth-only pass (shadow maps, depth pre-pass): NVN then writes depth and
     * stencil and no colour. Here it runs with an empty fragment shader. */
    int depth_only = S.prog[0] && !S.prog[1];
    if (S.prog[0] && !S.prog[2] && !S.prog[3] && !S.prog[4] && stage_glsl(S.prog[0], 0, key0) && (depth_only || stage_glsl(S.prog[1], 1, key1))) {
        static GLuint empty_fs;
        if (depth_only && !empty_fs) {
            static const char *const src[1] = {"#version 450\nvoid main() {}\n"};
            empty_fs = glCreateShaderProgramv(0x8B30, 1, src);
            printf("nvn_gl: first draw with a vertex stage only (program #%d): depth-only pass, empty fragment shader %u\n", S.prog[0]->serial, empty_fs);
        }
        for (int s = 0; s < 5; s++) glDisable(k_prog_target[s]);
        GLuint pair = pair_mode() ? pair_get(S.prog[0]->st[0].glsl, depth_only ? empty_fs : S.prog[1]->st[1].glsl, depth_only ? "#version 450\nvoid main() {}\n" : NULL) : 0;
        if (pair) { if (g_pair_cur != pair) { glUseProgram(pair); g_pair_cur = pair; } g_pair_draws++; }
        else {
        pair_off();
        glBindProgramPipeline(g_pipeline);
        glUseProgramStages(g_pipeline, 1, S.prog[0]->st[0].glsl);
        glUseProgramStages(g_pipeline, 2, depth_only ? empty_fs : S.prog[1]->st[1].glsl);
        }
        {   GLuint now[2] = {S.prog[0]->st[0].glsl, depth_only ? empty_fs : S.prog[1]->st[1].glsl};
            g_gs_draws++; g_gs_prog_same += now[0] == g_gs_last_prog[0] && now[1] == g_gs_last_prog[1];
            g_gs_last_prog[0] = now[0]; g_gs_last_prog[1] = now[1];
            pn_vs = now[0]; pn_fs = now[1]; pn_empty = depth_only; }
        glDisable(GL_ALPHA_TEST);
        for (int s = 0; s < (depth_only ? 1 : 2); s++)
            if (S.prog[s]->label_dirty) {          /* nvnProgramSetDebugLabel: the label goes to the OpenGL programs of its stages */
                Prog *lp = S.prog[s];
                lp->label_dirty = 0;
                for (int q = 0; q < 6; q++) if (lp->st[q].glsl && glObjectLabel) glObjectLabel(0x82E2 /* PROGRAM */, lp->st[q].glsl, -1, nvn_label_get(lp->guest));
            }
        {   /* nvnProgramSetSampleShading of the fragment program: 1 = run the fragment shader per sample, 2 = never, 0 = as the shader says */
            int ss = depth_only ? 0 : S.prog[1]->sample_shading;
            if (ss != S.sample_shading) { S.sample_shading = ss; if (ss == 1 && gl_has(glMinSampleShading, "glMinSampleShading")) { glEnable(0x8C36 /* SAMPLE_SHADING */); glMinSampleShading(1.0f); } else glDisable(0x8C36); }
        }
        if (!depth_only && S.prog[1]->st[1].glsl_alpha) { uint32_t ref; memcpy(&ref, &S.alpha_ref, 4); glProgramUniform2ui(pair ? pair : S.prog[1]->st[1].glsl, 0, (GLuint)S.alpha_func, ref); }
        for (int s = 0; s < (depth_only ? 1 : 2); s++) {
            Stage *st = &S.prog[s]->st[s];
            for (unsigned n = 0; n < 16; n++)
                if ((st->glsl_ssbos >> n & 1) && S.ssbo[s][n].addr && S.ssbo[s][n].size) {
                    size_t off = arena_put(S.ssbo[s][n].addr, S.ssbo[s][n].size);
                    glBindBufferRange(GL_SHADER_STORAGE_BUFFER_, 40 + (GLuint)s * 16 + n, g_arena, (ptrdiff_t)off, (ptrdiff_t)S.ssbo[s][n].size);
                }
            /* hardware constant buffers: 1 = constants stored after the shader's code, 2 = texture handles, 3.. = the uniform buffers bound by the game */
            for (unsigned n = 1; n < 18; n++) {
                Range r = {0, 0};
                if (!(st->glsl_cbufs >> n & 1)) continue;
                if (n == 2) { bind_handle_table(s, GL_SHADER_STORAGE_BUFFER_, (GLuint)s * 20 + n, 0); continue; }
                if (n == 1) { r.addr = (uint64_t)(uintptr_t)(st->code - 0x30 + st->ctl[4]); r.size = st->ctl[3]; } else r = S.ubo[s][n - 3];
                if (r.addr && r.size) {
                    size_t off = arena_put(r.addr, r.size);
                    glBindBufferRange(GL_SHADER_STORAGE_BUFFER_, (GLuint)s * 20 + n, g_arena, (ptrdiff_t)off, (ptrdiff_t)r.size);
                    g_gs_buf_bind++;
                }
            }
        }
        g_st.glsl_draws++;
    } else {
    pair_off();
    glBindProgramPipeline(0);
    if (S.alpha_func && S.alpha_func != 8) { glEnable(GL_ALPHA_TEST); glAlphaFunc(nvn_func((unsigned)S.alpha_func), S.alpha_ref); } else glDisable(GL_ALPHA_TEST);
    if (!S.prog[0] || !stage_ready(S.prog[0], 0) || !S.prog[1] || !stage_ready(S.prog[1], 1)) {
        logf_limited(&nlog, 10, "nvn_gl: draw skipped: no usable vertex/fragment program (vertex #%d state %d, fragment #%d state %d)\n", S.prog[0] ? S.prog[0]->serial : -1,
                     S.prog[0] ? S.prog[0]->st[0].state : -1, S.prog[1] ? S.prog[1]->serial : -1, S.prog[1] ? S.prog[1]->st[1].state : -1);
        return 0;
    }
    uint32_t storage = 0;
    for (int s = 0; s < 5; s++) {
        Prog *p = S.prog[s];
        if (!p || !stage_ready(p, s)) { glDisable(k_prog_target[s]); continue; }
        Stage *st = &p->st[s];
        glEnable(k_prog_target[s]);
        glBindProgramARB(k_prog_target[s], st->gl);
        for (int i = 0; i < st->nbuf; i++) {
            int n = st->map[i];
            if (n == TEXTURE_HANDLE_BUFFER) {
                bind_handle_table(s, k_pbuf_target[s], (GLuint)i, 32);
            } else if (n < 16 && S.ubo[s][n].addr && S.ubo[s][n].size) {
                size_t off = arena_put(S.ubo[s][n].addr, S.ubo[s][n].size);
                glBindBufferRange(k_pbuf_target[s], (GLuint)i, g_arena, (ptrdiff_t)off, (ptrdiff_t)S.ubo[s][n].size);
            }
        }
        for (uint32_t m = st->storage & ~storage, n = 0; m; m >>= 1, n++)
            if ((m & 1) && S.ssbo[s][n].addr && S.ssbo[s][n].size) {
                size_t off = arena_put(S.ssbo[s][n].addr, S.ssbo[s][n].size);
                glBindBufferRange(GL_SHADER_STORAGE_BUFFER_, n, g_arena, (ptrdiff_t)off, (ptrdiff_t)S.ssbo[s][n].size);
                storage |= 1u << n;
            }
    }
    }
    for (int i = 0; i < 16; i++) {
        uint32_t a = i < S.nattr ? S.vattr[i] : 0;
        int fmt = (int)(a & 0xff), stream = (int)((a >> 8) & 31), size, norm, integer, on = 0;
        GLenum type;
        if (fmt && S.vbuf[stream].addr && S.vbuf[stream].size && vfmt_get(fmt, &size, &type, &norm, &integer)) {
            if (integer) glVertexAttribIFormat((GLuint)i, size, type, a >> 14);
            else glVertexAttribFormat((GLuint)i, size, type, (GLboolean)norm, a >> 14);
            glVertexAttribBinding((GLuint)i, (GLuint)stream);
            g_gs_attr_set++; g_gs_attr_same += g_gs_last_attr[i] == a; g_gs_last_attr[i] = a;
            on = 1;
        }
        if (on != S.attr_enabled[i]) { if (on) glEnableVertexAttribArray((GLuint)i); else glDisableVertexAttribArray((GLuint)i); S.attr_enabled[i] = on; }
    }
    if (pn_vs) {
        uint32_t mask = 0;
        for (int i = 0; i < 16; i++) mask |= (uint32_t)(S.attr_enabled[i] != 0) << i;
        prec_note(pn_vs, pn_fs, pn_empty, mask & S.prog[0]->st[0].glsl_dep);
    }
    for (int i = 0; i < 16; i++)
        if (S.vbuf[i].addr && S.vbuf[i].size) {
            size_t off = arena_put(S.vbuf[i].addr, S.vbuf[i].size);
            glBindVertexBuffer((GLuint)i, g_arena, (ptrdiff_t)off, (GLsizei)S.vstream[i].stride);
            g_gs_vbuf_bind++;
            glVertexBindingDivisor((GLuint)i, S.vstream[i].divisor);
        }
    apply_stencil();
    if (g_ncrange) {
        int hit = 0;
        for (int i = 0; i < 16 && i < S.nattr; i++) {
            int stream = (int)((S.vattr[i] >> 8) & 31);
            if ((S.vattr[i] & 0xff) && S.vbuf[stream].addr && crange_hit(S.vbuf[stream].addr, S.vbuf[stream].size) >= 0) hit = 1;
        }
        g_st.draws_from_compute += hit;
    }
    if (drawlog_now()) {                    /* debug: what the draws of one frame look like */
        GLfloat vp[4];
        GLint sc[4], dfunc = 0;
        GLboolean cm[4];
        glGetFloatv(GL_VIEWPORT, vp); glGetIntegerv(GL_SCISSOR_BOX, sc); glGetBooleanv(GL_COLOR_WRITEMASK, cm); glGetIntegerv(GL_DEPTH_FUNC, &dfunc);
        printf("draw: fbo %u (%dx%d) viewport %.0f,%.0f %.0fx%.0f scissor %d,%d %dx%d vs #%d fs #%d depth %d(func %x) stencil %d blend %d cull %d alpha %d mask %d%d%d%d\n", S.fbo, S.fbo_w, S.fbo_h,
               vp[0], vp[1], vp[2], vp[3], sc[0], sc[1], sc[2], sc[3], S.prog[0] ? S.prog[0]->serial : -1, S.prog[1] ? S.prog[1]->serial : -1, glIsEnabled(GL_DEPTH_TEST), dfunc,
               glIsEnabled(GL_STENCIL_TEST), glIsEnabled(GL_BLEND), glIsEnabled(GL_CULL_FACE), S.alpha_func, cm[0], cm[1], cm[2], cm[3]);
        for (int s = 0; s < 2; s++) {
            printf("  %s buffers:", k_stage_name[s]);
            for (int n = 0; n < 16; n++) if (S.ubo[s][n].addr) printf(" c%d=%llx+%llu", n, (unsigned long long)S.ubo[s][n].addr, (unsigned long long)S.ubo[s][n].size);
            for (int n = 0; n < 16; n++)
                if (S.ssbo[s][n].addr) {
                    const float *f = P(S.ssbo[s][n].addr);
                    printf(" storage%d=%llx+%llu%s [", n, (unsigned long long)S.ssbo[s][n].addr, (unsigned long long)S.ssbo[s][n].size, S.prog[s] && (S.prog[s]->st[s].glsl_ssbos >> n & 1) ? "(used)" : "");
                    for (unsigned k = 0; k < 16 && k * 4 < S.ssbo[s][n].size; k++) printf(" %g", f[k]);
                    printf(" ]");
                }
            printf("  textures:");
            for (int n = 0; n < 8; n++) if (S.nvn_tex[s][n]) printf(" %d:%llx%s", n, (unsigned long long)S.nvn_tex[s][n], S.gl_tex[s][4 + n] == g_dummy_handle ? "(dummy)" : "");
            printf("\n");
            for (int n = 0; n < 16; n++)
                if (S.ubo[s][n].addr && S.prog[s] && (S.prog[s]->st[s].glsl_cbufs >> (n + 3) & 1)) {
                    const float *f = P(S.ubo[s][n].addr);
                    printf("    c%d:", n);
                    static int full = -1;              /* SSBU_DRAWLOG_FULL=1: every float of the buffer, not the first 24 */
                    if (full < 0) full = getenv("SSBU_DRAWLOG_FULL") != NULL;
                    for (unsigned k = 0; k < (full ? 1024u : 24u) && k * 4 < S.ubo[s][n].size; k++) printf(" %g", f[k]);
                    printf("\n");
                }
            for (int n = 0; n < 8; n++) {
                uint32_t tid = (uint32_t)S.nvn_tex[s][n] & 0xfffff;
                Tex *t = S.tpool && tid && tid < (uint32_t)S.tpool->count ? S.tpool->e[tid].tex : NULL;
                static uint8_t seen[0x4000];
                if (!t) continue;
                printf("    texture %d: id 0x%x target %d format 0x%x %dx%dx%d levels %d flags 0x%x gl %u uploaded %d rendered %d view set 0x%x format 0x%x\n", n, tid, t->b.target, t->b.format, t->b.width,
                       t->b.height, t->b.depth, t->b.levels, t->b.flags, t->o.gl, t->uploaded, t->gpu_written, S.tpool->e[tid].view.set, S.tpool->e[tid].view.format);
                {   uint32_t sid = (uint32_t)(S.nvn_tex[s][n] >> 20) & 0xfff;
                    if (S.spool && sid < (uint32_t)S.spool->count) {
                        const SamplerParams *sp = &S.spool->e[sid];
                        printf("      sampler 0x%x: min %d mag %d wrap %d %d %d lod %g..%g border %g %g %g %g (kind %d)\n", sid, sp->min, sp->mag, sp->ws, sp->wt, sp->wr, sp->lodmin, sp->lodmax, sp->border[0], sp->border[1], sp->border[2], sp->border[3], sp->border_kind);
                    }
                }
                if (tid < sizeof seen && !seen[tid] && t->o.gl && t->b.target == 1 && t->b.samples <= 1 && t->b.width <= 4096) {
                    char path[64];
                    uint8_t *px = malloc((size_t)t->b.width * t->b.height * 4);
                    seen[tid] = 1;
                    glBindTexture(GL_TEXTURE_2D, t->o.gl);
                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
                    while (glGetError()) {}
                    snprintf(path, sizeof path, "tex_%04x_%dx%d.rgba", tid, t->b.width, t->b.height);
                    FILE *fp = fopen(path, "wb");
                    if (fp) { fwrite(px, 1, (size_t)t->b.width * t->b.height * 4, fp); fclose(fp); }
                    free(px);
                }
            }
        }
        for (int i = 0; i < 16 && i < S.nattr; i++) {
            uint32_t a = S.vattr[i];
            int stream = (int)((a >> 8) & 31);
            if (!(a & 0xff)) continue;
            printf("  attr %d: format 0x%x stream %d offset %u stride %u buffer %llx+%llu", i, a & 0xff, stream, a >> 14, S.vstream[stream].stride, (unsigned long long)S.vbuf[stream].addr, (unsigned long long)S.vbuf[stream].size);
            if (S.vbuf[stream].addr && S.vbuf[stream].size >= (a >> 14) + 16) { const float *f = (const float *)((const uint8_t *)P(S.vbuf[stream].addr) + (a >> 14)); printf("  first: %g %g %g %g", f[0], f[1], f[2], f[3]); }
            { int k = S.vbuf[stream].addr ? crange_hit(S.vbuf[stream].addr, S.vbuf[stream].size) : -1; if (k >= 0) printf("  <- storage %d written by compute program #%d (%llx+%llu)", g_crange[k].n, g_crange[k].prog, (unsigned long long)g_crange[k].addr, (unsigned long long)g_crange[k].size); }
            printf("\n");
        }
    }
    return 1;
}

/* ---------------------------------------------------------------- compute
 * nvnCommandBufferDispatchCompute(cmdbuf, groupsX, groupsY, groupsZ) runs the program bound to the compute stage with the
 * uniform buffers, storage buffers, textures and images bound to that stage. The game skins its fighter meshes this way:
 * per mesh one dispatch reads bind-pose vertices and bone matrices and writes posed vertices into a storage buffer that later
 * draws bind as a vertex buffer. Storage buffers are guest memory: a buffer the program writes is copied into a GL buffer,
 * the dispatch runs, and the result is copied back to guest memory, where the CPU and later draws read it.
 * Debug aids: SSBU_NOCOMPUTE=1 skips every dispatch (the state before this was implemented); the draw log (SSBU_DRAWLOG)
 * also lists every dispatch of the frame with its buffers, and marks vertex buffers that a dispatch of the frame wrote. */
static int g_dbg_nocompute;
static struct { int serial; LONG64 calls, run, groups; } g_chist[64];
/* Storage buffers written by dispatches live in GL buffers of their own from the first dispatch that writes them until the next
 * command that reads guest memory (a draw, a copy, the end of the batch): compute_flush() then copies the written ones back to
 * guest memory, once, however many dispatches wrote into them (a fighter's meshes are skinned by ~10 dispatches into one buffer). */
typedef struct { uint64_t addr, size; GLuint buf; int dirty, live; } CBuf;
static CBuf g_cbuf[64];
static int g_cbuf_live;
static CBuf *cbuf_find(uint64_t addr, uint64_t size) {
    for (int k = 0; k < 64; k++) if (g_cbuf[k].live && g_cbuf[k].addr == addr && g_cbuf[k].size == size) return &g_cbuf[k];
    return NULL;
}
static int cbuf_conflict(uint64_t addr, uint64_t size) {      /* overlaps a buffer held in GL without being that buffer */
    for (int k = 0; k < 64; k++)
        if (g_cbuf[k].live && addr < g_cbuf[k].addr + g_cbuf[k].size && g_cbuf[k].addr < addr + size && !(g_cbuf[k].addr == addr && g_cbuf[k].size == size)) return 1;
    return 0;
}
static void compute_flush(void) {
    int any = 0;
    if (!g_cbuf_live) return;
    for (int k = 0; k < 64; k++) {
        CBuf *b = &g_cbuf[k];
        if (!b->live) continue;
        if (b->dirty) { LONG64 pf0 = pf_now(); HS_IN("compute read-back (glGetNamedBufferSubData)", 0); glGetNamedBufferSubData(b->buf, 0, (ptrdiff_t)b->size, P(b->addr)); HS_OUT(); pf_add(PF_CFLUSH, pf0); g_pf_cflush_bytes += (LONG64)b->size; g_st.compute_bytes += (LONG64)b->size; any = 1; }
        b->live = b->dirty = 0;
    }
    g_cbuf_live = 0;
    nvn_front_ccmp_landed();
    if (any) g_arena_gen++;                      /* guest memory changed: later draws must stream it again */
}
static CBuf *cbuf_get(uint64_t addr, uint64_t size) {
    CBuf *b = cbuf_find(addr, size);
    if (b) return b;
    for (int k = 0; k < 64 && !b; k++) if (!g_cbuf[k].live) b = &g_cbuf[k];
    if (!b) { compute_flush(); b = &g_cbuf[0]; }
    if (!b->buf) glCreateBuffers(1, &b->buf);
    glNamedBufferData(b->buf, (ptrdiff_t)size, P(addr), 0x88EA /* DYNAMIC_COPY */);
    b->addr = addr; b->size = size; b->live = 1; b->dirty = 0;
    g_cbuf_live++;
    return b;
}
static int crange_hit(uint64_t addr, uint64_t size) {
    for (int k = 0; k < g_ncrange; k++) if (addr < g_crange[k].addr + g_crange[k].size && g_crange[k].addr < addr + size) return k;
    return -1;
}
static void mem_stats(const char *what, uint64_t addr, uint64_t size) {
    const uint8_t *p = P(addr);
    const float *f = P(addr);
    size_t nz = 0, n = size > (8u << 20) ? (8u << 20) : (size_t)size;
    for (size_t q = 0; q < n; q++) nz += p[q] != 0;
    printf("    %s %llx+%llu: %zu of %zu bytes non-zero, hash %016llx, first:", what, (unsigned long long)addr, (unsigned long long)size, nz, n, (unsigned long long)mem_hash(p, n, 1));
    for (unsigned k = 0; k < 12 && k * 4 + 4 <= size; k++) printf(" %g", f[k]);
    printf("\n");
}
#define WMASK_BINDING 90                   /* storage block that collects which storage buffers a translated compute program wrote */
static Tex *image_tex(int stage, int k, View *v) {            /* NVN image handle = texture ID in the texture pool (nvnDeviceGetImageHandle) */
    uint32_t tid = (uint32_t)S.nvn_img[stage][k];
    Tex *t = NULL;
    if (!S.tpool || !tid) return NULL;
    AcquireSRWLockShared(&g_pool_lock);
    if (tid < (uint32_t)S.tpool->count) { t = S.tpool->e[tid].tex; *v = S.tpool->e[tid].view; }
    ReleaseSRWLockShared(&g_pool_lock);
    return t && !t->dead ? t : NULL;
}
/* The GLSL program of a compute stage given as machine code (translated again when the integer-ness of a texture it samples or
 * the format of an image it loads changes); 0 when it cannot be translated or the driver refuses it. */
static int stage_glsl_compute(Prog *p) {
    static int nfail, nlink;
    Stage *st = &p->st[5];
    MxIn in;
    MxOut out;
    uint16_t ifmt[8];
    if (!st->code || st->glsl_state == 2) return 0;
    for (int k = 0; k < 8; k++) {
        View v;
        GlFmt f;
        Tex *t = image_tex(5, k, &v);
        ifmt[k] = t && gl_fmt((v.set & 0x84) == 0x84 && v.format ? v.format : t->b.format, &f) ? f.internal : 0;
    }
    if (st->glsl_state == 1 && ((S.tex_int[5] ^ st->glsl_key[2]) & st->glsl_tex) == 0 && ((S.tex_sint[5] ^ st->glsl_key[3]) & st->glsl_tex) == 0) {
        int same = 1;
        for (int k = 0; k < 8; k++) if ((st->glsl_images >> k & 1) && st->glsl_img_key[k] != ifmt[k]) same = 0;
        if (same) return 1;
    }
    if (st->glsl) { glDeleteProgram(st->glsl); st->glsl = 0; }
    memset(&in, 0, sizeof in);
    in.stage = MX_COMPUTE; in.code = st->code; in.code_len = st->code_len;
    if (st->ctl[3] && st->ctl[4]) { in.cdata = st->code + st->ctl[4]; in.cdata_size = st->ctl[3]; }
    in.int_tex = S.tex_int[5]; in.sint_tex = S.tex_sint[5];
    in.binding_base = 0; in.ssbo_base = 40;        /* the bindings of the vertex stage: every draw and every dispatch binds what it uses */
    memcpy(in.local_size, st->local, sizeof in.local_size);
    in.smem_bytes = st->smem_bytes; in.lmem_bytes = st->lmem_bytes;
    in.image_base = 0; memcpy(in.image_format, ifmt, sizeof ifmt);
    in.track_writes = 1; in.wmask_binding = WMASK_BINDING;
    if (!mx_translate(&in, &out)) {
        st->glsl_state = 2;
        g_st.glsl_fail++;
        logf_limited(&nfail, 60, "nvn_gl: program #%d compute stage: machine code not translated: %s\n", p->serial, out.why);
        return 0;
    }
    const char *dir = getenv("SSBU_SHADERDUMP");
    if (dir) {
        char path[260];
        snprintf(path, sizeof path, "%s/b%05d_compute.glsl", dir, p->serial);
        FILE *f = fopen(path, "wb");
        if (f) { fputs(out.glsl, f); fclose(f); }
    }
    const char *srcs[1] = {out.glsl};
    GLint ok = 0;
    {   /* P3 round 2: compute stages go through the binary cache as well (a first dispatch cost 0.1-3.3 s, hitch log) */
        size_t clen = strlen(out.glsl);
        HS_IN("shader compile or load, compute stage", 0);
        if (g_shd_mode > 0) st->glsl = shader_build(0x91B9 /* COMPUTE_SHADER */, out.glsl, clen, sh_hash(0x9E3779B97F4A7C15ULL ^ 5u, out.glsl, clen), &ok);
        if (g_shd_mode > 0 && ok) prec_note_compute(sh_hash(0x9E3779B97F4A7C15ULL ^ 5u, out.glsl, clen), clen);
        else { st->glsl = glCreateShaderProgramv(0x91B9, 1, srcs); glGetProgramiv(st->glsl, 0x8B82, &ok); }
        HS_OUT();
    }
    if (!ok) {
        char log[2000] = "";
        glGetProgramInfoLog(st->glsl, sizeof log - 1, NULL, log);
        if (nlink < 4) {
            char path[64];
            snprintf(path, sizeof path, "shader_fail_glsl_c%d.txt", nlink);
            FILE *f = fopen(path, "wb");
            if (f) { fputs(out.glsl, f); fclose(f); }
        }
        logf_limited(&nlink, 20, "nvn_gl: program #%d compute stage: the driver refuses the translated GLSL:\n%s\n", p->serial, log);
        glDeleteProgram(st->glsl);
        st->glsl = 0; st->glsl_state = 2;
        g_st.glsl_fail++;
        free(out.glsl);
        return 0;
    }
    free(out.glsl);
    st->glsl_state = 1;
    st->glsl_key[2] = S.tex_int[5]; st->glsl_key[3] = S.tex_sint[5];
    memcpy(st->glsl_img_key, ifmt, sizeof ifmt); memcpy(st->glsl_img_fmt, out.image_gl_format, sizeof st->glsl_img_fmt);
    st->glsl_cbufs = out.cbufs; st->glsl_ssbos = out.ssbos; st->glsl_ssbo_w = out.ssbo_written; st->glsl_tex = out.tex_units;
    st->glsl_images = out.images; st->glsl_images_w = out.images_written; st->glsl_images_r = out.images_read;
    g_st.glsl_ok++;
    return 1;
}
static GLuint g_wmask_buf;
static void compute_bind_glsl(Prog *p) {
    static int nlog;
    Stage *st = &p->st[5];
    pair_off();
    glBindProgramPipeline(g_pipeline);
    glUseProgramStages(g_pipeline, 0x20 /* COMPUTE_SHADER_BIT */, st->glsl);
    for (unsigned n = 0; n < 18; n++) {
        Range r = {0, 0};
        if (!(st->glsl_cbufs >> n & 1)) continue;
        if (n == 0) { logf_limited(&nlog, 10, "nvn_gl: compute program #%d reads the driver's constant buffer 0 outside the storage buffer descriptors: not provided\n", p->serial); continue; }
        if (n == 2) { bind_handle_table(5, GL_SHADER_STORAGE_BUFFER_, n, 0); continue; }
        if (n == 1) { r.addr = (uint64_t)(uintptr_t)(st->code + st->ctl[4]); r.size = st->ctl[3]; } else r = S.ubo[5][n - 3];
        if (r.addr && r.size) {
            size_t off = arena_put(r.addr, r.size);
            glBindBufferRange(GL_SHADER_STORAGE_BUFFER_, n, g_arena, (ptrdiff_t)off, (ptrdiff_t)r.size);
        }
    }
    for (int k = 0; k < 8; k++) {
        View v;
        GlFmt f;
        Tex *t;
        if (!(st->glsl_images >> k & 1)) continue;
        if (!(t = image_tex(5, k, &v))) { logf_limited(&nlog, 10, "nvn_gl: compute program #%d uses image %d but none is bound\n", p->serial, k); continue; }
        tex_sync(t);
        int level = (v.set & 0x81) == 0x81 ? v.lvl0 : 0, w = (st->glsl_images_w >> k & 1) != 0;
        GLenum fmt = st->glsl_img_fmt[k] ? st->glsl_img_fmt[k] : gl_fmt(t->b.format, &f) ? f.internal : 0x8058;
        glBindImageTexture((GLuint)k, tex_view_obj(t, &v)->gl, level, GL_TRUE, 0, w ? ((st->glsl_images_r >> k & 1) ? 0x88BA : 0x88B9) : 0x88B8, fmt);
        if (w) { t->gpu_written = 1; t->uploaded = 1; }
    }
    if (st->glsl_ssbo_w) {
        static const uint32_t zero;
        if (!g_wmask_buf) glCreateBuffers(1, &g_wmask_buf);
        glNamedBufferData(g_wmask_buf, 4, &zero, 0x88EA);
        glBindBufferRange(GL_SHADER_STORAGE_BUFFER_, WMASK_BINDING, g_wmask_buf, 0, 4);
    }
}
static void compute_dispatch(unsigned gx, unsigned gy, unsigned gz) {
    static int nlog;
    Prog *p = S.prog[5];
    int log = drawlog_now(), h;
    g_st.dispatches++; g_frame_dispatch++;
    for (h = 0; h < 63 && g_chist[h].serial && g_chist[h].serial != (p ? p->serial : -1); h++) {}
    g_chist[h].serial = p ? p->serial : -1; g_chist[h].calls++; g_chist[h].groups += (LONG64)gx * gy * gz;
    if (!p) { logf_limited(&nlog, 10, "nvn_gl: DispatchCompute with no compute program bound\n"); return; }
    Stage *st = &p->st[5];
    int text = st->state == 1 || st->state == 2, glsl = 0;
    if (st->state == 4 && g_chist[h].calls <= 2) log = 1;         /* the first dispatches of every machine-code program are always described */
    uint32_t used = 0, written = 0;
    { static int mc = -1; if (mc < 0) mc = getenv("SSBU_COMPUTE_MC") != NULL; if (mc && text && st->code) text = 0; }      /* run the translated machine code instead of the text */
    if (text && !g_dbg_nocompute && !stage_ready(p, 5)) text = 0;
    if (text) { used = st->storage; written = st->storage_w; }
    else if (!g_dbg_nocompute && stage_glsl_compute(p)) { glsl = 1; used = st->glsl_ssbos; written = st->glsl_ssbo_w; }
    else if (st->state == 1 || st->state == 2) { used = st->storage; written = st->storage_w; }
    if (log) {
        printf("dispatch %lld: compute program #%d (%s) groups %u x %u x %u, storage used 0x%x written 0x%x\n", (long long)g_frame_dispatch - 1, p->serial,
               st->state == 1 || st->state == 2 ? "assembly text" : st->code ? "machine code" : "nothing usable", gx, gy, gz, used, written);
        for (int n = 0; n < 16; n++)
            if (S.ubo[5][n].addr) {
                const uint32_t *w = P(S.ubo[5][n].addr);
                printf("    uniform %d %llx+%llu:", n, (unsigned long long)S.ubo[5][n].addr, (unsigned long long)S.ubo[5][n].size);
                for (unsigned k = 0; k < 20 && k * 4 + 4 <= S.ubo[5][n].size; k++) printf(w[k] < 0x100000 ? " %u" : " %08x", w[k]);
                printf("\n");
            }
        for (int n = 0; n < 32; n++) if (S.ssbo[5][n].addr) { char tag[40]; snprintf(tag, sizeof tag, "storage %d%s before", n, written >> n & 1 ? " (written)" : ""); mem_stats(tag, S.ssbo[5][n].addr, S.ssbo[5][n].size); }
        for (int n = 0; n < 8; n++) if (S.nvn_tex[5][n]) printf("    texture %d: %llx\n", n, (unsigned long long)S.nvn_tex[5][n]);
        for (int n = 0; n < 8; n++) if (S.nvn_img[5][n]) printf("    image %d: %llx\n", n, (unsigned long long)S.nvn_img[5][n]);
    }
    for (int n = 0; n < 32; n++)
        if ((written >> n & 1) && S.ssbo[5][n].addr && g_ncrange < 1024) { g_crange[g_ncrange].addr = S.ssbo[5][n].addr; g_crange[g_ncrange].size = S.ssbo[5][n].size; g_crange[g_ncrange].prog = p->serial; g_crange[g_ncrange++].n = n; }
    if (g_dbg_nocompute) return;
    if (!text && !glsl) { logf_limited(&nlog, 20, "nvn_gl: DispatchCompute skipped: compute program #%d cannot be used (state %d, machine code %d bytes)\n", p->serial, st->state, st->code_len); g_st.dispatch_skipped++; return; }
    if (!gx || !gy || !gz) return;
    /* storage buffers: read-only ones are streamed like any other buffer (unless an earlier dispatch wrote them and they are still
     * held in GL); written ones get a GL buffer of their own (see compute_flush) */
    CBuf *held[32];
    memset(held, 0, sizeof held);
    for (int n = 0; n < 32; n++) {
        Range r = S.ssbo[5][n];
        if ((used >> n & 1) && r.addr && r.size && cbuf_conflict(r.addr, r.size)) { compute_flush(); break; }
    }
    for (int n = 0; n < 16; n++) if (S.ubo[5][n].addr && cbuf_conflict(S.ubo[5][n].addr, S.ubo[5][n].size)) { compute_flush(); break; }
    for (int n = 0; n < 16; n++) if (S.ubo[5][n].addr && cbuf_find(S.ubo[5][n].addr, S.ubo[5][n].size)) { compute_flush(); break; }
    for (int n = 0; n < 32; n++) {
        Range r = S.ssbo[5][n];
        GLuint binding = text ? (GLuint)n : 40 + (GLuint)n;
        if (!(used >> n & 1) || !r.addr || !r.size) continue;
        held[n] = written >> n & 1 ? cbuf_get(r.addr, r.size) : cbuf_find(r.addr, r.size);
        if (held[n]) glBindBufferRange(GL_SHADER_STORAGE_BUFFER_, binding, held[n]->buf, 0, (ptrdiff_t)r.size);
        else {
            size_t off = arena_put(r.addr, r.size);
            glBindBufferRange(GL_SHADER_STORAGE_BUFFER_, binding, g_arena, (ptrdiff_t)off, (ptrdiff_t)r.size);
        }
    }
    if (text) {
        pair_off();
        glBindProgramPipeline(0);
        glEnable(GL_COMPUTE_PROGRAM_NV_);
        glBindProgramARB(GL_COMPUTE_PROGRAM_NV_, st->gl);
        for (int i = 0; i < st->nbuf; i++) {
            int n = st->map[i];
            /* a parameter buffer binding of an assembly program must start at offset 0 of its buffer object (the driver answers
             * INVALID_VALUE "<offset> must be zero" otherwise), so each one gets a buffer of its own instead of a range of the stream */
            static GLuint param[16];
            if (!param[i]) glCreateBuffers(1, &param[i]);
            if (n == TEXTURE_HANDLE_BUFFER) glNamedBufferData(param[i], 256, (const uint8_t *)S.gl_tex[5] + 32, GL_STREAM_DRAW_);
            else if (n < 16 && S.ubo[5][n].addr && S.ubo[5][n].size) glNamedBufferData(param[i], (ptrdiff_t)S.ubo[5][n].size, P(S.ubo[5][n].addr), GL_STREAM_DRAW_);
            else continue;
            glBindBufferRange(k_pbuf_target[5], (GLuint)i, param[i], 0, n == TEXTURE_HANDLE_BUFFER ? 256 : (ptrdiff_t)S.ubo[5][n].size);
        }
    } else {
        glDisable(GL_COMPUTE_PROGRAM_NV_);
        compute_bind_glsl(p);
    }
    nvn_front_ccmp_dispatch(S.ubo[5], S.ssbo[5], written, st->code, st->code_len, gx, gy, gz);      /* SSBU_COMPUTE_CMP */
    glDispatchCompute(gx, gy, gz);
    if (glsl && st->glsl_images_w) glMemoryBarrier(0xffffffffu);
    if (written) {
        glMemoryBarrier(0x200 /* BUFFER_UPDATE_BARRIER_BIT */);
        if (glsl) {                              /* a translated program reports which storage buffers it really wrote */
            uint32_t wm = 0;
            glGetNamedBufferSubData(g_wmask_buf, 0, 4, &wm);
            written &= wm;
            if (log) printf("    storage buffers written by this dispatch: 0x%x\n", wm);
        }
        for (int n = 0; n < 32; n++) if ((written >> n & 1) && held[n]) held[n]->dirty = 1;
        if (log) {
            compute_flush();
            for (int n = 0; n < 32; n++) if ((written >> n & 1) && held[n]) { char tag[40]; snprintf(tag, sizeof tag, "storage %d after ", n); mem_stats(tag, S.ssbo[5][n].addr, S.ssbo[5][n].size); }
        }
    }
    if (text) glDisable(GL_COMPUTE_PROGRAM_NV_);
    g_chist[h].run++;
    g_st.dispatch_run++;
}

static void fence_now(SyncObj *o, int flags);
/* ---- counters (nvnCommandBufferResetCounter / ReportCounter): OpenGL queries that run from one report to the next */
static GLenum counter_target(int type) {
    static const GLenum m[16] = {0, 0x8914 /* SAMPLES_PASSED */, 0x82EE /* VERTICES_SUBMITTED */, 0x82EF /* PRIMITIVES_SUBMITTED */, 0x82F0 /* VERTEX_SHADER_INVOCATIONS */,
                                 0x82F1 /* TESS_CONTROL_SHADER_PATCHES */, 0x82F2 /* TESS_EVALUATION_SHADER_INVOCATIONS */, 0x887F /* GEOMETRY_SHADER_INVOCATIONS */,
                                 0x82F4 /* FRAGMENT_SHADER_INVOCATIONS */, 0, 0x82F3 /* GEOMETRY_SHADER_PRIMITIVES_EMITTED */, 0x82F6 /* CLIPPING_INPUT_PRIMITIVES */,
                                 0x82F7 /* CLIPPING_OUTPUT_PRIMITIVES */, 0x8C87 /* PRIMITIVES_GENERATED */, 0x8C88 /* TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN */, 0};
    return type >= 0 && type < 16 ? m[type] : 0;
}
static void counter_stop(int type) {
    uint64_t r = 0;
    if (type < 0 || type >= 17 || !g_counter[type].active) return;
    glEndQuery(counter_target(type));
    if (gl_has(glGetQueryObjectui64v, "glGetQueryObjectui64v")) glGetQueryObjectui64v(g_counter[type].q, 0x8866 /* QUERY_RESULT */, &r);
    g_counter[type].acc += r; g_counter[type].active = 0;
}
static void counter_start(int type) {
    if (!counter_target(type)) return;
    if (!g_counter[type].q) glGenQueries(1, &g_counter[type].q);
    glBeginQuery(counter_target(type), g_counter[type].q);
    g_counter[type].active = 1;
}
/* What a multisample state asks for beyond the two enables; some of it is state of the bound framebuffer. */
static void apply_multisample(void) {
    const uint8_t *b = S.ms;
    static int warned;
    unsigned fl = b[3];
    if (b[5]) { if (gl_has(glRasterSamplesEXT, "glRasterSamplesEXT")) { glEnable(0x9327 /* RASTER_MULTISAMPLE_EXT */); glRasterSamplesEXT(b[5], GL_TRUE); } } else if (S.ms_ext & 1) glDisable(0x9327);
    if (((fl >> 3) & 3) || (S.ms_ext & 2)) { static const GLenum m[4] = {GL_NONE, GL_RGB, GL_ALPHA, GL_RGBA}; if (gl_has(glCoverageModulationNV, "glCoverageModulationNV")) glCoverageModulationNV(m[(fl >> 3) & 3]); }
    if (fl & 1) { if (gl_has(glFragmentCoverageColorNV, "glFragmentCoverageColorNV")) { glEnable(0x92DD /* FRAGMENT_COVERAGE_TO_COLOR_NV */); glFragmentCoverageColorNV(fl >> 5); } } else if (S.ms_ext & 4) glDisable(0x92DD);
    if ((fl & 2) && S.have_fbo) {
        GLfloat loc[32];
        for (int i = 0; i < 16; i++) { loc[2 * i] = (float)(b[8 + i] & 15) / 16.0f; loc[2 * i + 1] = (float)(b[8 + i] >> 4) / 16.0f; }
        if (gl_has(glFramebufferSampleLocationsfvNV, "glFramebufferSampleLocationsfvNV") && gl_has(glFramebufferParameteri, "glFramebufferParameteri")) {
            glFramebufferParameteri(GL_FRAMEBUFFER_, 0x9342 /* FRAMEBUFFER_PROGRAMMABLE_SAMPLE_LOCATIONS_NV */, 1);
            glFramebufferParameteri(GL_FRAMEBUFFER_, 0x9343 /* FRAMEBUFFER_SAMPLE_LOCATION_PIXEL_GRID_NV */, (fl >> 2) & 1);
            glFramebufferSampleLocationsfvNV(GL_FRAMEBUFFER_, 0, b[4] ? b[4] : 1, loc);
        }
    } else if ((S.ms_ext & 8) && S.have_fbo && glFramebufferParameteri) glFramebufferParameteri(GL_FRAMEBUFFER_, 0x9342, 0);
    S.ms_ext = (b[5] ? 1u : 0) | (((fl >> 3) & 3) ? 2u : 0) | ((fl & 1) ? 4u : 0) | ((fl & 2) ? 8u : 0);
    (void)warned;
}

static void copy_region(const uint8_t *p, int *r) { memcpy(r, p, 24); }
static GLuint temp_fbo(GLenum target, GLuint slot_fbo, Tex *t, const View *v) {
    GlFmt f;
    int kind = gl_fmt(t->b.format, &f) ? f.kind : 0;
    glBindFramebuffer(target, slot_fbo);
    glFramebufferTexture(target, GL_COLOR_ATTACHMENT0_, 0, 0);
    glFramebufferTexture(target, GL_DEPTH_STENCIL_ATTACHMENT_, 0, 0);
    glFramebufferTexture(target, kind == 3 ? GL_DEPTH_STENCIL_ATTACHMENT_ : kind == 1 ? GL_DEPTH_ATTACHMENT_ : kind == 2 ? GL_STENCIL_ATTACHMENT_ : GL_COLOR_ATTACHMENT0_, tex_view_obj(t, v)->gl, 0);
    return (GLuint)(kind == 3 ? GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT : kind == 1 ? GL_DEPTH_BUFFER_BIT : kind == 2 ? GL_STENCIL_BUFFER_BIT : GL_COLOR_BUFFER_BIT);
}
static GLuint g_fbo_tmp[2];
static void blit_tex(Tex *src, const View *sv, const int *sr, Tex *dst, const View *dv, const int *dr, int linear) {
    tex_sync(src); tex_sync(dst);
    dst->gpu_written = 1;
    GLuint mask = temp_fbo(GL_READ_FRAMEBUFFER_, g_fbo_tmp[0], src, sv);
    temp_fbo(GL_DRAW_FRAMEBUFFER_, g_fbo_tmp[1], dst, dv);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_FRAMEBUFFER_SRGB_);
    glBlitFramebuffer(sr[0], sr[1], sr[0] + sr[3], sr[1] + sr[4], dr[0], dr[1], dr[0] + dr[3], dr[1] + dr[4], mask, linear && mask == GL_COLOR_BUFFER_BIT ? GL_LINEAR : GL_NEAREST);
    glEnable(GL_FRAMEBUFFER_SRGB_);
    glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER_, S.have_fbo ? S.fbo : 0);
    g_st.copies++;
}

static void replay(uint64_t handle, int depth);
static void save_bmp(const char *path, Tex *t);
#include "nvn_dt.h"
static void *dt_host(uint64_t a) { return P(a); }
/* Debug aid SSBU_DRAWSHOT=<frame>: the colour target after every draw of that frame, as draw_<n>.bmp. */
static void draw_shot(void) {
    static int want = -2;
    Tex fake;
    char path[64];
    if (want == -2) want = getenv("SSBU_DRAWSHOT") ? atoi(getenv("SSBU_DRAWSHOT")) : -1;
    if ((g_st.frames != want && !(g_draw_tag && getenv("SSBU_DRAWSHOT_BOX"))) || !S.fbo_w) return;      /* round 17: with SSBU_DRAWTAG the box aid follows the tagged fight frame */
    {   /* SSBU_DRAWSHOT_BOX=x,y,w,h (in 1920x1080 picture coordinates): no files; one line per draw that CHANGED the mean
         * colour of that box of its colour target (finds the draws that paint a given place of the screen) */
        static int box[4] = {-1, 0, 0, 0};
        static unsigned last_fbo; static double last[4];
        if (box[0] == -1) { const char *e = getenv("SSBU_DRAWSHOT_BOX"); box[0] = -2; if (e && sscanf(e, "%d,%d,%d,%d", &box[0], &box[1], &box[2], &box[3]) != 4) box[0] = -2; }
        if (box[0] >= 0) {
            int x = box[0] * S.fbo_w / 1920, y = box[1] * S.fbo_h / 1080, w = box[2] * S.fbo_w / 1920, h = box[3] * S.fbo_h / 1080;
            if (w < 1 || h < 1 || x + w > S.fbo_w || y + h > S.fbo_h) return;
            uint8_t *px = malloc((size_t)w * h * 4);
            double m[4] = {0, 0, 0, 0};
            glBindFramebuffer(GL_READ_FRAMEBUFFER_, S.fbo);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(x, y, w, h, 0x1908 /* GL_RGBA */, GL_UNSIGNED_BYTE, px);
            for (int i = 0; i < w * h; i++) for (int k = 0; k < 4; k++) m[k] += px[i * 4 + k];
            for (int k = 0; k < 4; k++) m[k] /= (double)(w * h);
            free(px);
            if (last_fbo != S.fbo || m[0] != last[0] || m[1] != last[1] || m[2] != last[2] || m[3] != last[3])
                printf("  BOX: draw %d (DT %d) fbo %u (%dx%d): mean RGBA of the box %.2f %.2f %.2f %.2f%s\n", g_frame_draw_index - 1, g_dt_idx - 1, S.fbo, S.fbo_w, S.fbo_h, m[0], m[1], m[2], m[3], last_fbo != S.fbo ? "  (first draw seen on this target)" : "  CHANGED");
            last_fbo = S.fbo; memcpy(last, m, sizeof last);
            return;
        }
    }
    memset(&fake, 0, sizeof fake);
    fake.b.width = S.fbo_w; fake.b.height = S.fbo_h;
    snprintf(path, sizeof path, "draw_%02d_fbo%u.bmp", g_frame_draw_index - 1, S.fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER_, S.fbo);
    save_bmp(path, &fake);
}
/* P3 round 2, EXPERIMENT, off by default (SSBU_FIRSTDRAW_WARM=1 turns it on): the first draw with a pair of GLSL
 * programs is preceded by one point drawn with the rasterizer off. Result: it does NOT remove the cost, it only moves it.
 * The driver spends 50-90 ms inside the first draw call of 48-93 of the 420-520 program pairs of a run (3.4-6.2 s per
 * run); with the point, that time is spent in the point instead (tests/logs/p3b_r2c.log: 48 points over 4 ms, 3,979 ms).
 * One run (p3b_warm.log) had only 5 slow pairs, with or without the point: the cost belongs to pairs the DRIVER has
 * never linked on this PC (its own disk cache), not to the first draw of a run. Kept as a measuring aid: the end
 * report counts the slow first draws. What the point does: the vertex stage runs once on vertex 0 of the bound
 * buffers; nothing reaches the framebuffer. It is not done while a counter other than "samples passed" is running or
 * when the vertex stage writes storage buffers. */
static LONG64 g_fdw_n, g_fdw_slow, g_fdw_skipped, g_fdw_ticks;
static void first_draw_warm(void) {
    static uint64_t seen[1 << 16];
    static int off = -1;
    if (off < 0) off = getenv("SSBU_FIRSTDRAW_WARM") == NULL;
    if (off || !g_gs_last_prog[0]) return;
    uint64_t pair = ((uint64_t)g_gs_last_prog[0] << 32) | g_gs_last_prog[1] | 1ull << 63;
    unsigned h = (unsigned)((pair * 0x9E3779B97F4A7C15ULL) >> 48);
    for (int probe = 0; probe < 64; probe++, h = (h + 1) & 0xffff) { if (seen[h] == pair) return; if (!seen[h]) { seen[h] = pair; break; } }
    if (!S.prog[0] || S.prog[0]->st[0].glsl != g_gs_last_prog[0] || S.prog[0]->st[0].glsl_ssbo_w) { g_fdw_skipped++; return; }      /* not the GLSL path, or a vertex stage with side effects */
    for (int t = 0; t < 17; t++) if (t != 1 && g_counter[t].active) { g_fdw_skipped++; return; }
    LONG64 t0 = pf_now(), freq;
    { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart; }
    HS_IN("first draw of a program pair: discarded point (driver)", 0);
    glEnable(GL_RASTERIZER_DISCARD_);
    glDrawArrays(GL_POINTS, 0, 1);
    glDisable(GL_RASTERIZER_DISCARD_);
    HS_OUT();
    LONG64 dt = pf_now() - t0;
    g_fdw_n++; g_fdw_ticks += dt; g_fdw_slow += dt * 250 >= freq;
}
/* SSBU_HITCH: a draw call that took more than 4 ms, and whether it was the first draw with this pair of programs. */
static void hitch_draw(LONG64 t0) {
    static uint64_t seen[1 << 16];
    static LONG64 freq, n_first, n_first_slow, n_old_slow;
    static double ms_first, ms_old;
    if (!freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart; }
    double ms = (double)(pf_now() - t0) * 1000.0 / (double)freq;
    uint64_t pair = ((uint64_t)g_gs_last_prog[0] << 32) | g_gs_last_prog[1] | 1ull << 63;
    unsigned h = (unsigned)((pair * 0x9E3779B97F4A7C15ULL) >> 48);
    int first = 1;
    for (int probe = 0; probe < 64; probe++, h = (h + 1) & 0xffff) { if (seen[h] == pair) { first = 0; break; } if (!seen[h]) { seen[h] = pair; break; } }
    n_first += first;
    if (ms < 4.0) return;
    if (first) { n_first_slow++; ms_first += ms; } else { n_old_slow++; ms_old += ms; }
    if (g_fdlog) {
        int any = 0;
        uint32_t mask = 0;
        ShSrc *a = g_gs_last_prog[0] < 65536 ? g_prog_src[g_gs_last_prog[0]] : NULL, *b = g_gs_last_prog[1] < 65536 ? g_prog_src[g_gs_last_prog[1]] : NULL;
        for (int i = 0; i < 16; i++) mask |= (uint32_t)(S.attr_enabled[i] != 0) << i;
        printf("fd-slow: %016llx %x %016llx %x mask %x (enabled %x) %.1f ms frame %lld %s%c", (unsigned long long)(a ? a->h : 0), a ? (unsigned)a->len : 0, (unsigned long long)(b ? b->h : 0), b ? (unsigned)b->len : 0,
               S.prog[0] ? mask & S.prog[0]->st[0].glsl_dep : 0, mask, ms, (long long)g_st.frames + 1, first ? "first" : "later", 10);
        {   int nc = 0; for (int i = 0; i < 8; i++) nc += S.color[i] != NULL;
            printf("fd-state: prim %u count %u poly %x depth clamp %u discard %u sample shading %d alpha %d func %d ms %02x %02x %02x %02x %02x %02x colour targets %d (format %x) %dx%d ds %llx dep %x key ints %x texint %x %x nattr %d%c",
                   g_fd_prim, g_fd_count, g_fd_poly, g_fd_dclamp, g_fd_rdisc, S.sample_shading, S.prog[1] ? S.prog[1]->st[1].glsl_alpha : -1, S.alpha_func, S.ms[0], S.ms[1], S.ms[2], S.ms[3], S.ms[4], S.ms[5], nc,
                   S.color0 ? S.color0->b.format : 0, S.fbo_w, S.fbo_h, (unsigned long long)S.ds, S.prog[0] ? S.prog[0]->st[0].glsl_dep : 0, S.prog[0] ? S.prog[0]->st[0].glsl_key[0] : 0, S.tex_int[0], S.tex_int[1], S.nattr, 10); }
        for (unsigned i = 0; i < 64; i++) if (g_fd_ring[i].t >= t0 && g_fd_ring[i].text[0]) { any++; printf("fdlog: frame %lld draw %.1f ms: id %u type %x severity %x: %s%c", (long long)g_st.frames + 1, ms, g_fd_ring[i].id, g_fd_ring[i].type, g_fd_ring[i].sev, g_fd_ring[i].text, 10); }
        if (!any) printf("fdlog: frame %lld draw %.1f ms: NO driver message during this draw call%c", (long long)g_st.frames + 1, ms, 10);
    }
    printf("hitch-draw: frame %lld %.1f ms, programs vs %u fs %u, %s | so far: %lld pairs, slow first draws %lld (%.0f ms), slow later draws %lld (%.0f ms)%c", (long long)g_st.frames + 1, ms, g_gs_last_prog[0],
           g_gs_last_prog[1], first ? "FIRST draw with this pair" : "pair drawn before", (long long)n_first, (long long)n_first_slow, ms_first, (long long)n_old_slow, ms_old, 10);
}
static void exec(const Cmd *m, int depth) {
    const uint8_t *blob = BLOB(m);
    const uint64_t *a = m->a;
    g_cur_op = k_op_name[m->op < OP_COUNT ? m->op : 0];
    if (g_cbuf_live && (m->op == OP_COPY_BUF_TEX || m->op == OP_COPY_BUF_BUF || m->op == OP_UPDATE_UBO || m->op == OP_CLEAR_BUF || m->op == OP_COPY_TEX_BUF)) compute_flush();
    switch (m->op) {
    case OP_TEXPOOL: S.tpool = P(a[0]); break;
    case OP_SMPPOOL: S.spool = P(a[0]); break;
    case OP_PROGRAM:
        for (int s = 0; s < 6; s++) if (a[1] & (1u << s)) { Prog *p = P(a[0]); S.prog[s] = p && !p->dead && p->st[s].state ? p : NULL; }
        break;
    case OP_UBO: case OP_SSBO: {
        int stage = (int)a[0] % 6, first = (int)a[1], count = (int)a[2], max = m->op == OP_UBO ? 16 : 32;
        Range *dst = m->op == OP_UBO ? S.ubo[stage] : S.ssbo[stage];
        for (int i = 0; i < count && first + i < max; i++) memcpy(&dst[first + i], blob + i * 16, 16);
        break;
    }
    case OP_TEXTURES: {
        int stage = (int)a[0] % 6, first = (int)a[1], count = (int)a[2];
        for (int i = 0; i < count && first + i < 32; i++) {
            uint64_t h;
            memcpy(&h, blob + i * 8, 8);
            S.nvn_tex[stage][first + i] = h;
            uint32_t dims[4] = {0, 0, 0, 0};
            int isint = 0;
            unsigned u = (unsigned)(first + i);
            { LONG64 pf0 = pf_now(); HS_IN("texture handle (view, sampler, bindless handle)", 0); S.gl_tex[stage][4 + u] = resolve_handle(S.tpool, S.spool, h, dims, &isint); HS_OUT(); pf_add(PF_HANDLE, pf0); }
            memcpy(&S.gl_tex[stage][48 + 2 * u], dims, 16);
            S.tex_int[stage] = (S.tex_int[stage] & ~(1u << u)) | ((uint32_t)(isint != 0) << u);
            S.tex_sint[stage] = (S.tex_sint[stage] & ~(1u << u)) | ((uint32_t)(isint == 2) << u);
        }
        S.tex_dirty[stage] = 1;
        break;
    }
    case OP_VBUF:
        for (int i = 0; i < (int)a[1] && (int)a[0] + i < 32; i++) memcpy(&S.vbuf[a[0] + i], blob + i * 16, 16);
        break;
    case OP_VATTR:
        S.nattr = (int)a[0] < 32 ? (int)a[0] : 32;
        memcpy(S.vattr, blob, (size_t)S.nattr * 4);
        break;
    case OP_VSTREAM:
        for (int i = 0; i < (int)a[0] && i < 32; i++) memcpy(&S.vstream[i], blob + i * 8, 8);
        break;
    case OP_BLEND: {
        const uint8_t *b = (const uint8_t *)&a[0];
        glBlendFuncSeparatei(b[0] & 7, blend_func(b[1]), blend_func(b[2]), blend_func(b[3]), blend_func(b[4]));
        glBlendEquationSeparatei(b[0] & 7, blend_eq(b[5]), blend_eq(b[6]));
        if (b[7]) {                            /* advanced blend equation (NV_blend_equation_advanced) instead of the functions above */
            static const uint16_t adv[0x33] = {0, 0, 0, 0, 0, 0 /* ZERO */, 0x9286, 0x9287, 0x9288, 0x9289, 0x928A, 0x928B, 0x928C, 0x928D, 0x928E, 0x928F, 0x1506, 0x9291, 0x92B1, 0x92B2, 0x9292,
                                               0x9294, 0x9295, 0x9296, 0x9297, 0x9298, 0x9299, 0x929A, 0x929B, 0x929C, 0x929E, 0x929F, 0x92B3, 0x92A0, 0x92A1, 0x150A, 0x92A3, 0x92B4, 0x92A4,
                                               0x92A5, 0x92A6, 0x92A7, 0x92A8, 0x92A9, 0x1903, 0x1904, 0x1905, 0x92AD, 0x92AE, 0x92AF, 0x92B0};
            if (b[7] < 0x33 && gl_has(glBlendEquationi, "glBlendEquationi") && gl_has(glBlendParameteriNV, "glBlendParameteriNV")) {
                glBlendParameteriNV(0x9280 /* BLEND_PREMULTIPLIED_SRC_NV */, (b[0] >> 5) & 1);
                glBlendParameteriNV(0x9281 /* BLEND_OVERLAP_NV */, 0x9282 + ((b[0] >> 3) & 3));
                glBlendEquationi(b[0] & 7, adv[b[7]]);
            }
        }
        break;
    }
    case OP_COLOR: {
        const uint8_t *b = (const uint8_t *)&a[0];
        for (unsigned i = 0; i < 8; i++) { if (b[0] & (1u << i)) glEnablei(GL_BLEND, i); else glDisablei(GL_BLEND, i); }
        if (b[1] != 3) { glEnable(GL_COLOR_LOGIC_OP); glLogicOp(GL_CLEAR + (b[1] & 15)); } else glDisable(GL_COLOR_LOGIC_OP);
        S.alpha_func = b[2];
        break;
    }
    case OP_CHANMASK: S.chanmask = (uint32_t)a[0]; apply_chanmask(); break;
    case OP_MULTISAMPLE: {
        const uint8_t *b = (const uint8_t *)&a[0];
        if (b[0]) glEnable(GL_MULTISAMPLE_); else glDisable(GL_MULTISAMPLE_);
        if (b[1]) glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE_); else glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE_);
        memcpy(S.ms, b, 24);
        if (b[3] || b[5] || S.ms_ext) apply_multisample();
        break;
    }
    case OP_POLYGON: {
        const uint8_t *b = (const uint8_t *)&a[0];
        static const GLenum face[4] = {0, GL_FRONT, GL_BACK, GL_FRONT_AND_BACK};
        if (b[0] & 3) { glEnable(GL_CULL_FACE); glCullFace(face[b[0] & 3]); } else glDisable(GL_CULL_FACE);
        glFrontFace((b[1] != 0) != (g_dbg_flipface != 0) ? GL_CCW : GL_CW);       /* see the note on winding at nvn_prim */
        glPolygonMode(GL_FRONT_AND_BACK, GL_POINT + (b[2] < 3 ? b[2] : 2));
        g_fd_poly = b[0] | (unsigned)b[2] << 8;
        if (b[3] & 1) glEnable(GL_POLYGON_OFFSET_POINT); else glDisable(GL_POLYGON_OFFSET_POINT);
        if (b[3] & 2) glEnable(GL_POLYGON_OFFSET_LINE); else glDisable(GL_POLYGON_OFFSET_LINE);
        if (b[3] & 4) glEnable(GL_POLYGON_OFFSET_FILL); else glDisable(GL_POLYGON_OFFSET_FILL);
        break;
    }
    case OP_DEPTHSTENCIL: {
        uint64_t d = a[0];
        S.ds = d;
        if (d & 1) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        glDepthMask((d >> 1) & 1);
        glDepthFunc(nvn_func((unsigned)(d >> 4) & 15));
        if (d & 4) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
        glStencilOpSeparate(GL_FRONT, stencil_op((unsigned)(d >> 16) & 15), stencil_op((unsigned)(d >> 20) & 15), stencil_op((unsigned)(d >> 24) & 15));
        glStencilOpSeparate(GL_BACK, stencil_op((unsigned)(d >> 28) & 15), stencil_op((unsigned)(d >> 32) & 15), stencil_op((unsigned)(d >> 36) & 15));
        S.stencil_dirty = 1;
        break;
    }
    case OP_VIEWPORTS:
        for (int i = 0; i < (int)a[1] && (int)a[0] + i < 16; i++) { float v[4]; memcpy(v, blob + i * 16, 16); glViewportIndexedf((GLuint)a[0] + (GLuint)i, v[0], v[1], v[2], v[3]); }
        break;
    case OP_SCISSORS:
        if (g_dbg_noscissor) break;
        for (int i = 0; i < (int)a[1] && (int)a[0] + i < 16; i++) {
            int v[4];
            memcpy(v, blob + i * 16, 16);
            glScissorIndexed((GLuint)a[0] + (GLuint)i, v[0], v[1], v[2] < 0 ? 0 : v[2], v[3] < 0 ? 0 : v[3]);
            if ((int)a[0] + i == 0) { S.sc0[0] = v[0]; S.sc0[1] = v[1]; S.sc0[2] = v[2] < 0 ? 0 : v[2]; S.sc0[3] = v[3] < 0 ? 0 : v[3]; }
        }
        break;
    case OP_DEPTHRANGES:
        for (int i = 0; i < (int)a[1] && (int)a[0] + i < 16; i++) { float v[2]; memcpy(v, blob + i * 8, 8); glDepthRangeIndexed((GLuint)a[0] + (GLuint)i, v[0], v[1]); }
        break;
    case OP_TARGETS: set_targets((int)a[0], (const TargetRef *)blob, (const TargetRef *)blob + 8); if (S.ms_ext & 8) apply_multisample(); break;
    case OP_STENCIL_REF: case OP_STENCIL_VMASK: case OP_STENCIL_MASK: {
        int *dst = m->op == OP_STENCIL_REF ? S.sref : m->op == OP_STENCIL_VMASK ? S.svmask : S.swmask;
        if (a[0] & 1) dst[0] = (int)a[1];
        if (a[0] & 2) dst[1] = (int)a[1];
        S.stencil_dirty = 1;
        break;
    }
    case OP_BLENDCOLOR: { float v[4]; memcpy(v, blob, 16); glBlendColor(v[0], v[1], v[2], v[3]); break; }
    case OP_ALPHAREF: memcpy(&S.alpha_ref, &a[0], 4); break;                       /* applied per draw (draw_prepare) */
    case OP_POINTSIZE: { float v; memcpy(&v, &a[0], 4); glPointSize(v); break; }
    case OP_LINEWIDTH: { float v; memcpy(&v, &a[0], 4); glLineWidth(v); break; }
    case OP_POLYOFFSET: { float v[3]; memcpy(v, &a[0], 12); glPolygonOffsetClamp(v[0], v[1], v[2]); break; }      /* (factor, units, clamp) */
    case OP_SAMPLEMASK: if ((uint32_t)a[0] == 0xffffffffu) glDisable(0x8E51); else { glEnable(0x8E51); glSampleMaski(0, (GLbitfield)a[0]); } break;
    case OP_RASTDISCARD: g_fd_rdisc = a[0] & 1; if (a[0] & 1) glEnable(GL_RASTERIZER_DISCARD_); else glDisable(GL_RASTERIZER_DISCARD_); break;
    case OP_DEPTHCLAMP: g_fd_dclamp = a[0] & 1; if (a[0] & 1) glEnable(GL_DEPTH_CLAMP_); else glDisable(GL_DEPTH_CLAMP_); break;
    case OP_PRIMRESTART: if (a[0] & 1) { glEnable(GL_PRIMITIVE_RESTART_); glPrimitiveRestartIndex((GLuint)a[1]); } else glDisable(GL_PRIMITIVE_RESTART_); break;
    case OP_PATCHSIZE: glPatchParameteri(GL_PATCH_VERTICES_, (GLint)a[0]); break;
    case OP_CLEARCOLOR: {
        float v[4];
        unsigned i = (unsigned)a[0] & 7, mask = (unsigned)a[2];
        if (!S.have_fbo || S.render_off) break;
        memcpy(v, blob, 16);
        glColorMaski(i, mask & 1, (mask >> 1) & 1, (mask >> 2) & 1, (mask >> 3) & 1);
        if (a[5] == 1) glClearBufferiv(GL_COLOR, (GLint)i, (const GLint *)v);              /* ClearColori / ClearColorui say what the four words are */
        else if (a[5] == 2) glClearBufferuiv(GL_COLOR, (GLint)i, (const GLuint *)v);
        else
        {
            GlFmt f;
            int isint = S.color[i] && gl_fmt(S.color[i]->b.format, &f) && f.kind == 0 && (f.format == 0x8D94 || f.format == 0x8228 || f.format == 0x8D98 || f.format == 0x8D99);
            if (!isint) glClearBufferfv(GL_COLOR, (GLint)i, v);      /* NVN passes the same four words for ClearColori / ClearColorui */
            else if (f.type == 0x1400 || f.type == 0x1402 || f.type == 0x1404) glClearBufferiv(GL_COLOR, (GLint)i, (const GLint *)v);
            else glClearBufferuiv(GL_COLOR, (GLint)i, (const GLuint *)v);
        }
        apply_chanmask();
        g_st.clears++; g_frame_clears++;
        break;
    }
    case OP_CLEARDS: {
        float depth;
        GLbitfield bits = (a[0] & 1 ? GL_DEPTH_BUFFER_BIT : 0) | ((uint32_t)a[2] ? GL_STENCIL_BUFFER_BIT : 0);
        if (!S.have_fbo || !bits || S.render_off) break;
        memcpy(&depth, &a[3], 4);
        glDepthMask(GL_TRUE);
        glStencilMaskSeparate(GL_FRONT_AND_BACK, (GLuint)a[2]);
        glClearDepth(depth);
        glClearStencil((GLint)a[1]);
        glClear(bits);
        glDepthMask((S.ds >> 1) & 1);
        S.stencil_dirty = 1;
        g_st.clears++; g_frame_clears++;
        break;
    }
    case OP_DRAW_ARRAYS: {
        LONG64 pf0 = pf_now();
        int ok;
        { HS_IN("draw state (pipeline, bindings)", 0); ok = draw_prepare(); HS_OUT(); }
        pf_add(PF_PREP, pf0);
        if (g_draw_tag) dt_line("gl", g_draw_tag, g_dt_idx++, ok, (unsigned)a[0], -1, (int)a[2], 0, (long long)a[1], (unsigned)a[3], (unsigned)a[4], S.prog, S.nattr, S.vattr, S.vbuf, (const uint32_t *)S.vstream, S.ubo, S.ssbo, dt_host);
        if (!ok) { g_st.draws_skipped++; g_frame_skipped++; break; }
        if (drawlog_now()) printf("  DrawArrays mode %d first %d count %d base instance %d instances %d%c", (int)a[0], (int)a[1], (int)a[2], (int)a[3], (int)a[4], 10);
        drawlog_begin();
        first_draw_warm();
        pf0 = pf_now();
        g_fd_prim = (unsigned)a[0]; g_fd_count = (unsigned)a[2];
        { HS_IN("draw call (driver)", 0); glDrawArraysInstancedBaseInstance(nvn_prim((unsigned)a[0]), (GLint)a[1], (GLsizei)a[2], (GLsizei)a[4], (GLuint)a[3]); HS_OUT(); }
        pf_add(PF_DRAWCALL, pf0);
        if (g_hitch_ms) hitch_draw(pf0);
        drawlog_end();
        draw_shot();
        g_st.draws++; g_frame_draws++; count_dummy_samples();
        break;
    }
    case OP_DRAW_ELEMENTS: {
        static const GLenum itype[3] = {GL_UNSIGNED_BYTE, GL_UNSIGNED_SHORT, GL_UNSIGNED_INT};
        unsigned t = (unsigned)a[1] < 3 ? (unsigned)a[1] : 1;
        int count = (int)a[2];
        LONG64 pf0 = pf_now();
        int ok;
        { HS_IN("draw state (pipeline, bindings)", 0); ok = count > 0 && a[3] && draw_prepare(); HS_OUT(); }
        if (g_draw_tag) dt_line("gl", g_draw_tag, g_dt_idx++, ok, (unsigned)a[0], (int)t, count, a[3], (long long)(int32_t)a[4], (unsigned)a[5], (unsigned)a[6], S.prog, S.nattr, S.vattr, S.vbuf, (const uint32_t *)S.vstream, S.ubo, S.ssbo, dt_host);
        if (!ok) { pf_add(PF_PREP, pf0); g_st.draws_skipped++; g_frame_skipped++; break; }
        size_t off = arena_put(a[3], (uint64_t)count << t);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER_, g_arena);
        pf_add(PF_PREP, pf0);
        if (drawlog_now()) printf("  DrawElements mode %d type %d count %d base vertex %d base instance %d instances %d%c", (int)a[0], (int)t, count, (int)a[4], (int)a[5], (int)a[6], 10);
        if (getenv("SSBU_DRAWSHOT_CLEAR") && getenv("SSBU_DRAWSHOT") && g_st.frames == atoi(getenv("SSBU_DRAWSHOT"))) { glDisable(GL_SCISSOR_TEST); glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT); glEnable(GL_SCISSOR_TEST); }
        drawlog_begin();
        first_draw_warm();
        pf0 = pf_now();
        g_fd_prim = (unsigned)a[0]; g_fd_count = (unsigned)count;
        { HS_IN("draw call (driver)", 0); glDrawElementsInstancedBaseVertexBaseInstance(nvn_prim((unsigned)a[0]), count, itype[t], (const void *)off, (GLsizei)a[6], (GLint)a[4], (GLuint)a[5]); HS_OUT(); }
        pf_add(PF_DRAWCALL, pf0);
        if (g_hitch_ms) hitch_draw(pf0);
        drawlog_end();
        draw_shot();
        g_st.draws++; g_frame_draws++; count_dummy_samples();
        break;
    }
    case OP_COPY_BUF_TEX: {                    /* a0 buffer address, a1 texture, a4 flags, a5 / a6 row / image stride of its command buffer; blob: view, region */
        /* PR #14 (xtlg511): replay with the strides recorded in the command (a5/a6), not the running S.row_stride/S.image_stride */
        Tex *t = P(a[1]);
        View v;
        GlFmt f;
        int r[6];
        if (!t || t->dead || !a[0]) break;
        memcpy(&v, blob, sizeof v);
        copy_region(blob + 24, r);
        if (!t->o.gl) tex_realize(t);
        if (!gl_fmt(t->b.format, &f)) break;
        int level = (v.set & 0x81) == 0x81 ? v.lvl0 : 0, layer = (v.set & 0x82) == 0x82 ? v.lay0 : 0;
        {   /* one line per distinct destination (format, size): shows what is updated this way (movie frames, glyph caches, ...) */
            static struct { int fmt, w, h; } seen[32];
            static int nseen;
            int k;
            for (k = 0; k < nseen && !(seen[k].fmt == t->b.format && seen[k].w == t->b.width && seen[k].h == t->b.height); k++) {}
            if (k == nseen && nseen < 32) {
                size_t nz = 0, bytes = (size_t)r[3] * (size_t)r[4] * f.bpb / (size_t)(f.bw * f.bh);
                for (size_t q = 0; q < bytes; q++) nz += ((const uint8_t *)P(a[0]))[q] != 0;
                seen[nseen].fmt = t->b.format; seen[nseen].w = t->b.width; seen[nseen++].h = t->b.height;
                printf("nvn_gl: CopyBufferToTexture into %dx%dx%d format 0x%x target %d levels %d flags 0x%x swizzle %d%d%d%d: region %d,%d,%d %dx%dx%d level %d layer %d, row stride %lld, %zu of %zu source bytes non-zero\n",
                       t->b.width, t->b.height, t->b.depth, t->b.format, t->b.target, t->b.levels, t->b.flags, t->b.swz[0], t->b.swz[1], t->b.swz[2], t->b.swz[3],
                       r[0], r[1], r[2], r[3], r[4], r[5], level, layer, (long long)a[5], nz, bytes);     /* PR #14 (xtlg511): the command's own row stride */
            }
        }
        /* PR #14 (xtlg511): the strides of the command buffer that recorded this copy (a5 / a6), not the running S state */
        if (a[5] && f.kind != 4) glPixelStorei(GL_UNPACK_ROW_LENGTH_, (GLint)((int64_t)a[5] / f.bpb));      /* nvnCommandBufferSetCopyRowStride */
        if (a[6] && a[5] && f.kind != 4) glPixelStorei(0x806E /* UNPACK_IMAGE_HEIGHT */, (GLint)((int64_t)a[6] / (int64_t)a[5]));   /* SetCopyImageStride */
        if (t->b.target != 2 && t->b.target != 4 && t->b.target != 9 && t->b.target != 8) layer = 0;
        else if (t->b.target != 2) layer += r[2];
        tex_put(t, &f, level, layer, r[0], r[1], r[2], r[3], r[4], r[5] > 0 ? r[5] : 1, P(a[0]));
        glPixelStorei(GL_UNPACK_ROW_LENGTH_, 0); glPixelStorei(0x806E, 0);
        t->gpu_written = 1; t->uploaded = 1;
        g_st.copies++;
        break;
    }
    case OP_COPY_TEX_TEX: {                    /* a0 src, a3 dst, a6 flags; blob: src view, src region, dst view, dst region */
        Tex *s = P(a[0]), *d = P(a[3]);
        View sv, dv;
        int sr[6], dr[6];
        if (!s || !d || s->dead || d->dead) break;
        memcpy(&sv, blob, sizeof sv); copy_region(blob + 24, sr);
        memcpy(&dv, blob + 48, sizeof dv); copy_region(blob + 72, dr);
        if (a[6] & 4) { dr[0] += dr[3]; dr[3] = -dr[3]; }      /* NVN_COPY_FLAGS_MIRROR_X / MIRROR_Y */
        if (a[6] & 8) { dr[1] += dr[4]; dr[4] = -dr[4]; }
        { LONG64 pf0 = pf_now(); blit_tex(s, &sv, sr, d, &dv, dr, (int)(a[6] & 1)); pf_add(PF_COPY, pf0); }
        break;
    }
    case OP_DOWNSAMPLE: {
        Tex *s = P(a[0]), *d = P(a[1]);
        if (!s || !d || s->dead || d->dead) break;
        int sr[6] = {0, 0, 0, s->b.width, s->b.height, 1}, dr[6] = {0, 0, 0, d->b.width, d->b.height, 1};
        blit_tex(s, NULL, sr, d, NULL, dr, 0);
        break;
    }
    case OP_COPY_TEX_BUF: {                    /* a0 texture, a3 buffer address, a5 / a6 row / image stride of its command buffer; blob: view, region */
        Tex *t = P(a[0]);
        View v;
        GlFmt f;
        int r[6];
        if (!t || t->dead || !a[3] || !t->o.gl || !gl_fmt(t->b.format, &f)) break;
        memcpy(&v, blob, sizeof v);
        copy_region(blob + 24, r);
        int level = (v.set & 0x81) == 0x81 ? v.lvl0 : 0, layer = (v.set & 0x82) == 0x82 ? v.lay0 : 0, d = r[5] > 0 ? r[5] : 1;
        int z = t->b.target == 2 ? r[2] : layer + r[2];
        size_t bytes = ((size_t)(r[3] + f.bw - 1) / f.bw) * ((size_t)(r[4] + f.bh - 1) / f.bh) * f.bpb * (size_t)d;
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        compute_flush();
        /* PR #14 (xtlg511): the strides of the command buffer that recorded this copy (a5 / a6), not the running S state */
        if (a[5] && f.kind != 4) { glPixelStorei(0x0D02 /* PACK_ROW_LENGTH */, (GLint)((int64_t)a[5] / f.bpb)); bytes = (size_t)a[5] * (size_t)r[4] * (size_t)d; }
        if (a[6] && a[5] && f.kind != 4) { glPixelStorei(0x806C /* PACK_IMAGE_HEIGHT */, (GLint)((int64_t)a[6] / (int64_t)a[5])); bytes = (size_t)a[6] * (size_t)d; }
        if (f.kind == 4) glGetCompressedTextureSubImage(t->o.gl, level, r[0], r[1], z, r[3], r[4], d, (GLsizei)bytes, P(a[3]));
        else glGetTextureSubImage(t->o.gl, level, r[0], r[1], z, r[3], r[4], d, f.format, f.type, (GLsizei)bytes, P(a[3]));
        glPixelStorei(0x0D02, 0); glPixelStorei(0x806C, 0);
        g_arena_gen++;
        g_st.copies++;
        break;
    }
    case OP_COPY_BUF_BUF: if (a[0] && a[1]) memmove(P(a[1]), P(a[0]), (size_t)a[2]); g_arena_gen++; break;
    case OP_UPDATE_UBO: if (a[0]) memcpy((uint8_t *)P(a[0]) + a[2], blob, (size_t)a[3]); g_arena_gen++; break;
    case OP_CLEAR_BUF: if (a[0]) { uint32_t v = (uint32_t)a[2]; for (uint64_t i = 0; i + 4 <= a[1]; i += 4) memcpy((uint8_t *)P(a[0]) + i, &v, 4); } g_arena_gen++; break;
    case OP_ROWSTRIDE: S.row_stride = (int64_t)a[0]; break;      /* PR #14 (xtlg511): no longer read by the copies (they carry their own strides); kept as state */
    case OP_IMAGESTRIDE: S.image_stride = (int64_t)a[0]; break;
    case OP_DEPTHBOUNDS: {                     /* depth bounds test: fragments pass when the depth already in the buffer is inside [near, far] */
        float v[2];
        memcpy(v, &a[1], 8);
        if (!gl_has(glDepthBoundsEXT, "glDepthBoundsEXT")) break;
        if (a[0] & 1) { glEnable(0x8890 /* DEPTH_BOUNDS_TEST_EXT */); glDepthBoundsEXT(v[0], v[1]); } else glDisable(0x8890);
        break;
    }
    case OP_CONSRASTER: if (a[0] & 1) glEnable(0x9346 /* CONSERVATIVE_RASTERIZATION_NV */); else glDisable(0x9346); break;
    case OP_CONSDILATE: { float v; memcpy(&v, &a[0], 4); if (gl_has(glConservativeRasterParameterfNV, "glConservativeRasterParameterfNV")) glConservativeRasterParameterfNV(0x9379 /* CONSERVATIVE_RASTER_DILATE_NV */, v); break; }
    case OP_SUBPIXEL: if (gl_has(glSubpixelPrecisionBiasNV, "glSubpixelPrecisionBiasNV")) glSubpixelPrecisionBiasNV((GLuint)a[0], (GLuint)a[1]); break;
    case OP_VPSWIZZLE:
        if (!gl_has(glViewportSwizzleNV, "glViewportSwizzleNV")) break;
        for (int i = 0; i < (int)a[1] && (int)a[0] + i < 16; i++) { int v[4]; memcpy(v, blob + i * 16, 16); glViewportSwizzleNV((GLuint)a[0] + (GLuint)i, 0x9350 + (GLenum)(v[0] & 7), 0x9350 + (GLenum)(v[1] & 7), 0x9350 + (GLenum)(v[2] & 7), 0x9350 + (GLenum)(v[3] & 7)); }
        break;
    case OP_TESSLEVELS: if (gl_has(glPatchParameterfv, "glPatchParameterfv")) glPatchParameterfv(a[0] ? 0x8E74 /* PATCH_DEFAULT_OUTER_LEVEL */ : 0x8E73, (const GLfloat *)blob); break;
    case OP_BARRIER:                           /* NVNbarrierBits: 2 / 4 order fragments (rendering to a texture that is being sampled), 8 indirect data, 0x10 texture memory changed */
        if ((a[0] & 6) && gl_has(glTextureBarrier, "glTextureBarrier")) glTextureBarrier();
        if (a[0] & 8) compute_flush();
        if (a[0] & 0x10) g_serial++;
        break;
    case OP_DISCARD: {                         /* the contents of an attachment are no longer needed */
        GLenum att[2] = {GL_DEPTH_ATTACHMENT_, GL_STENCIL_ATTACHMENT_};
        if (!S.have_fbo || !gl_has(glInvalidateFramebuffer, "glInvalidateFramebuffer")) break;
        if (a[0] == ~0ull) glInvalidateFramebuffer(GL_FRAMEBUFFER_, 2, att);
        else { att[0] = GL_COLOR_ATTACHMENT0_ + (GLenum)(a[0] & 7); if (S.color[a[0] & 7]) glInvalidateFramebuffer(GL_FRAMEBUFFER_, 1, att); }
        break;
    }
    case OP_CLEARTEX: {                        /* a0 texture, a4 channel mask, a5 0 float / 1 int / 2 uint, a6 whole level; blob: view, region, colour */
        Tex *t = P(a[0]);
        View v;
        int r[6];
        unsigned mask = (unsigned)a[4];
        if (!t || t->dead || !gl_has(glFramebufferTextureLayer, "glFramebufferTextureLayer")) break;
        memcpy(&v, blob, sizeof v); copy_region(blob + 24, r);
        tex_sync(t);
        t->gpu_written = 1; t->uploaded = 1;
        int level = (v.set & 0x81) == 0x81 ? v.lvl0 : 0, layer = (v.set & 0x82) == 0x82 ? v.lay0 : 0, layered = t->b.target == 2 || t->b.target == 3 || t->b.target == 4 || t->b.target == 8 || t->b.target == 9;
        if (a[6]) { r[0] = r[1] = r[2] = 0; r[3] = lvl(t->b.width, level); r[4] = lvl(t->b.height, level); r[5] = t->b.target == 2 ? lvl(t->b.depth, level) : 1; }
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER_, g_fbo_tmp[1]);
        glFramebufferTexture(GL_DRAW_FRAMEBUFFER_, GL_DEPTH_STENCIL_ATTACHMENT_, 0, 0);
        glScissorIndexed(0, r[0], r[1], r[3], r[4]);
        glColorMaski(0, mask & 1, (mask >> 1) & 1, (mask >> 2) & 1, (mask >> 3) & 1);
        for (int z = 0; z < (r[5] > 0 ? r[5] : 1); z++) {
            if (layered) glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, t->o.gl, level, (t->b.target == 2 ? 0 : layer) + r[2] + z);
            else glFramebufferTexture(GL_DRAW_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, t->o.gl, level);
            if (a[5] == 1) glClearBufferiv(GL_COLOR, 0, (const GLint *)(blob + 48)); else if (a[5] == 2) glClearBufferuiv(GL_COLOR, 0, (const GLuint *)(blob + 48)); else glClearBufferfv(GL_COLOR, 0, (const GLfloat *)(blob + 48));
        }
        glScissorIndexed(0, S.sc0[0], S.sc0[1], S.sc0[2], S.sc0[3]);
        apply_chanmask();
        glBindFramebuffer(GL_FRAMEBUFFER_, S.have_fbo ? S.fbo : 0);
        g_st.clears++;
        break;
    }
    case OP_DOWNSAMPLE_VIEW: {
        Tex *s = P(a[0]), *d = P(a[1]);
        View sv, dv;
        if (!s || !d || s->dead || d->dead) break;
        memcpy(&sv, blob, sizeof sv); memcpy(&dv, blob + 24, sizeof dv);
        int sl = (sv.set & 0x81) == 0x81 ? sv.lvl0 : 0, dl = (dv.set & 0x81) == 0x81 ? dv.lvl0 : 0;
        int sr[6] = {0, 0, 0, lvl(s->b.width, sl), lvl(s->b.height, sl), 1}, dr[6] = {0, 0, 0, lvl(d->b.width, dl), lvl(d->b.height, dl), 1};
        blit_tex(s, &sv, sr, d, &dv, dr, 0);
        break;
    }
    case OP_DRAW_INDIRECT: {                   /* a0 mode, a1 index type, a2 index buffer, a3 indirect data, a4 draws (or the maximum), a5 stride, a6 bit 0 elements, bit 1 count read from memory */
        static const GLenum itype[3] = {GL_UNSIGNED_BYTE, GL_UNSIGNED_SHORT, GL_UNSIGNED_INT};
        unsigned t = (unsigned)a[1] < 3 ? (unsigned)a[1] : 1, n = (unsigned)a[4], stride = (unsigned)a[5];
        int elements = (int)(a[6] & 1);
        compute_flush();                       /* the indirect data may have been written by a dispatch */
        if (a[6] & 2) { uint64_t param; memcpy(&param, blob, 8); uint32_t cnt = param ? rd32(param) : 0; if (cnt < n) n = cnt; }
        if (!stride) stride = elements ? 20 : 16;
        for (unsigned k = 0; k < n && a[3]; k++) {
            uint32_t d[5];                     /* arrays: count, instances, first, base instance; elements: count, instances, first index, base vertex, base instance */
            memcpy(d, P(a[3] + (uint64_t)k * stride), elements ? 20 : 16);
            if (!d[0] || !d[1]) continue;
            if (!draw_prepare()) { g_st.draws_skipped++; g_frame_skipped++; continue; }
            if (elements) {
                if (!a[2]) continue;
                size_t off = arena_put(a[2] + ((uint64_t)d[2] << t), (uint64_t)d[0] << t);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER_, g_arena);
                glDrawElementsInstancedBaseVertexBaseInstance(nvn_prim((unsigned)a[0]), (GLsizei)d[0], itype[t], (const void *)off, (GLsizei)d[1], (GLint)d[3], d[4]);
            } else glDrawArraysInstancedBaseInstance(nvn_prim((unsigned)a[0]), (GLint)d[2], (GLsizei)d[0], (GLsizei)d[1], d[3]);
            g_st.draws++; g_frame_draws++; count_dummy_samples();
        }
        break;
    }
    case OP_DISPATCH_INDIRECT: {
        uint32_t g[3] = {0, 0, 0};
        compute_flush();
        if (a[0]) memcpy(g, P(a[0]), 12);
        compute_dispatch(g[0], g[1], g[2]);
        break;
    }
    case OP_RENDER_ENABLE:                     /* a1 = 0: a0 is the switch; otherwise mode + 1 and a2 the address of two counters (16 bytes each) to compare */
        if (!a[1]) S.render_off = !(a[0] & 1);
        else { compute_flush(); int eq = a[2] && rd64(a[2]) == rd64(a[2] + 16); S.render_off = a[1] == 1 ? !eq : eq; }
        break;
    case OP_COUNTER_RESET: counter_stop((int)a[0]); if (a[0] < 17) g_counter[a[0]].acc = 0; counter_start((int)a[0]); break;
    case OP_COUNTER_REPORT: {                  /* (type, address of an NVNcounterData {counter, timestamp}) */
        uint64_t d[2] = {0, now_ns()};
        int type = (int)a[0];
        if (type > 0 && type < 16 && counter_target(type)) { counter_stop(type); d[0] = g_counter[type].acc; counter_start(type); }
        if (a[1]) { compute_flush(); memcpy(P(a[1]), d, 16); g_arena_gen++; }
        break;
    }
    case OP_REPORT_VALUE: if (a[1]) { uint64_t d[2] = {(uint32_t)a[0], now_ns()}; compute_flush(); memcpy(P(a[1]), d, 16); g_arena_gen++; } break;
    case OP_FENCE: if (a[0]) fence_now(P(a[0]), (int)a[2]); break;
    case OP_WAITSYNC: { SyncObj *o = P(a[0]); if (o && o->s && !o->dead) glWaitSync(o->s, 0, 0xFFFFFFFFFFFFFFFFull); break; }
    case OP_EVENT_SIGNAL: event_signal_at(a[0], (int)a[1], (uint32_t)a[4]); break;
    case OP_EVENT_WAIT: {                      /* the queue stops until the event's value equals (mode 0) or has reached (mode 1, wrapping) the value */
        static int nlog;
        uint32_t want = (uint32_t)a[2];
        DWORD t0 = GetTickCount();
        while (a[0]) {
            uint32_t v = rd32(a[0]);
            if (a[1] == 0 ? v == want : (int32_t)(v - want) >= 0) break;
            if (GetTickCount() - t0 > 5000) { logf_limited(&nlog, 10, "nvn_gl: WaitEvent: the event at %llx still holds %u after 5 s (waiting for %s %u); continuing\n", (unsigned long long)a[0], v, a[1] ? ">=" : "==", want); break; }
            Sleep(0);
        }
        break;
    }
    case OP_DRAW_TEXTURE: {                    /* nvn_c4.c: glDrawTextureNV with the depth, stencil and depth bounds tests off */
        GLuint tex = 0, smp = 0;
        static int nlog;
        if (!S.have_fbo || S.render_off) break;
        if (!resolve_names(S.tpool, S.spool, a[0], &tex, &smp)) { logf_limited(&nlog, 20, "nvn_gl: DrawTexture with handle 0x%llx: no such texture in the bound texture pool; nothing to draw\n", (unsigned long long)a[0]); break; }
        if (!nvn_c4_draw_texture_gl(tex, smp, (const NvnC4DrawTexture *)blob, S.alpha_func, S.alpha_ref)) {
            printf("nvn: stopping: nvnCommandBufferDrawTexture with a texture view that is not 2D or rectangle: what the GPU does is not established\n");
            fflush(stdout);
            report_and_exit(6);
        }
        g_st.draws++; g_frame_draws++;
        break;
    }
    case OP_COVMODTABLE:
        if (!gl_has(glCoverageModulationTableNV, "glCoverageModulationTableNV")) break;
        if (a[0]) { glEnable(0x9331 /* COVERAGE_MODULATION_TABLE_NV */); glCoverageModulationTableNV(16, (const GLfloat *)blob); } else glDisable(0x9331);
        break;
    case OP_DEBUG:
        if (!gl_has(glPushDebugGroup, "glPushDebugGroup")) break;
        if (a[0] == 0) { if (S.debug_depth < 60) { glPushDebugGroup(0x824A /* DEBUG_SOURCE_APPLICATION */, (GLuint)a[1], -1, (const char *)blob); S.debug_depth++; } }
        else if (a[0] == 1) { if (S.debug_depth > 0) { glPopDebugGroup(); S.debug_depth--; } }
        else glDebugMessageInsert(0x824A, 0x8268 /* DEBUG_TYPE_MARKER */, (GLuint)a[1], 0x826B /* DEBUG_SEVERITY_NOTIFICATION */, -1, (const char *)blob);
        break;
    case OP_DISPATCH: { LONG64 pf0 = pf_now(); HS_IN("compute dispatch", 0); compute_dispatch((unsigned)a[0], (unsigned)a[1], (unsigned)a[2]); HS_OUT(); pf_add(PF_DISPATCH, pf0); break; }
    case OP_IMAGES: {
        int stage = (int)a[0] % 6, first = (int)a[1], count = (int)a[2];
        for (int i = 0; i < count && first + i < 8; i++) memcpy(&S.nvn_img[stage][first + i], blob + i * 8, 8);
        break;
    }
    case OP_CALL:
        for (int i = 0; i < (int)a[0]; i++) { uint64_t h; memcpy(&h, blob + i * 8, 8); if (depth < 8) replay(h, depth + 1); }
        break;
    }
    if (g_debug) {
        static int nlog;
        GLenum e = glGetError();
        if (e) { g_st.gl_errors++; logf_limited(&nlog, 60, "nvn_gl: GL error 0x%x after %s\n", e, g_cur_op); }
    }
}
static void replay(uint64_t handle, int depth) {
    size_t len = 0;
    uint8_t *data = list_lookup(handle, &len);
    static int nlog;
    if (!data) { g_st.stale++; logf_limited(&nlog, 10, "nvn_gl: command handle %llx is unknown or expired\n", (unsigned long long)handle); return; }
    for (size_t off = 0; off + sizeof(Cmd) <= len;) {
        const Cmd *m = (const Cmd *)(data + off);
        if (m->size < sizeof(Cmd)) break;
        exec(m, depth);
        off += m->size;
    }
}

/* ---------------------------------------------------------------- render-thread jobs */
static void job_submit(void *arg) {
    SubmitJob *j = arg;
    static int nlog;
    g_serial++;
    g_arena_gen++;                               /* guest memory may have changed since the last batch */
    LONG64 pf0 = pf_now();
    for (int i = 0; i < j->count; i++) replay(j->handles[i], 0);
    compute_flush();                             /* the guest may read what compute wrote as soon as the batch has been submitted */
    pf_add(PF_SUBMIT, pf0);
    g_cur_op = "";
    GLenum e = glGetError();
    if (e) { g_st.gl_errors++; logf_limited(&nlog, 30, "nvn_gl: GL error 0x%x in submitted batch %lld\n", e, (long long)g_st.submits); while (glGetError()) {} }
    g_st.submits++;
}

static void job_submit_async(void *arg) { SubmitJob *j = arg; j->handles = (const uint64_t *)(j + 1); job_submit(j); }

static void save_bmp(const char *path, Tex *t) {
    int w = t->b.width, h = t->b.height;
    uint8_t *px = malloc((size_t)w * h * 3), head[54] = {'B', 'M'};
    uint32_t row = ((uint32_t)w * 3 + 3) & ~3u, size = 54 + row * (uint32_t)h, v;
    static const uint8_t pad[4];
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_BGR_, GL_UNSIGNED_BYTE, px);
    FILE *f = fopen(path, "wb");
    if (!f) { free(px); return; }
    memcpy(head + 2, &size, 4);
    v = 54; memcpy(head + 10, &v, 4);
    v = 40; memcpy(head + 14, &v, 4);
    memcpy(head + 18, &w, 4); memcpy(head + 22, &h, 4);
    head[26] = 1; head[28] = 24;
    fwrite(head, 1, 54, f);
    for (int y = h - 1; y >= 0; y--) { fwrite(px + (size_t)y * w * 3, 1, (size_t)w * 3, f); fwrite(pad, 1, row - (uint32_t)w * 3, f); }   /* GL row 0 is the top row */
    fclose(f);
    free(px);
    printf("nvn_gl: saved %s (%dx%d)\n", path, w, h);
    fflush(stdout);
}
/* SSBU_GLSTATS=1: one line every 300 frames with per-frame counts (pf_frame_mark, nvn_front.c). */
static void rb_gl_frame_stats(void) {
    static int on = -1;
    static LONG64 z[16];
    if (on < 0) on = getenv("SSBU_GLSTATS") != NULL;
    if (on) {
        LONG64 v[16] = {g_gs_draws, g_gs_prog_same, g_gs_attr_set, g_gs_attr_same, g_gs_vbuf_bind, g_gs_buf_bind, g_st.tex_created, g_st.tex_uploads, g_st.fbo_made, g_st.views,
                        g_st.handles, g_shc_compiled, g_shd_loaded, g_shc_hit, g_pfn[PF_CFLUSH], g_pfn[PF_WAIT_SUBMIT] + g_pfn[PF_WAIT_PRESENT] + g_pfn[PF_WAIT_OTHER]};
        double d[16];
        for (int k = 0; k < 16; k++) { d[k] = (double)(v[k] - z[k]) / 300.0; z[k] = v[k]; }
        printf("nvn_glstats: per frame: guest waits for the render thread %.1f | compute read-backs (glGetNamedBufferSubData) %.1f | created: textures %.3f, uploads %.3f, framebuffers %.3f, views %.3f, bindless handles %.3f | "
               "shader stages: built %.3f (from disk %.3f), reused %.3f | draws %.1f: same two programs as the previous draw %.1f; vertex attribute formats set %.1f, unchanged since last set %.1f; vertex buffer binds %.1f; uniform/constant buffer binds %.1f\n",
               d[15], d[14], d[6], d[7], d[8], d[9], d[10], d[11], d[12], d[13], d[0], d[1], d[2], d[3], d[4], d[5]);
    }
}
static void job_present(void *arg) {
    pair_off();
    pw_gate();
    PresentJob *j = arg;
    static int shot = -2, every, shot_from;
    Tex *t = j->win && j->index >= 0 && j->index < j->win->count ? j->win->tex[j->index] : NULL;
    RECT r;
    LONG64 frame = g_st.frames + 1;
    LONG64 pf0 = pf_now();
    if (shot == -2) { shot = getenv("SSBU_SHOT") ? atoi(getenv("SSBU_SHOT")) : -1; every = getenv("SSBU_SHOT_EVERY") ? atoi(getenv("SSBU_SHOT_EVERY")) : 0; shot_from = getenv("SSBU_SHOT_FROM") ? atoi(getenv("SSBU_SHOT_FROM")) : 0; }
    GetClientRect(g_hwnd, &r);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_FRAMEBUFFER_SRGB_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER_, 0);
    if (t && !t->dead && t->o.gl) {
        int sr[4] = {0, 0, t->b.width, t->b.height};
        if (j->win->crop[2] > 0 && j->win->crop[3] > 0) memcpy(sr, j->win->crop, sizeof sr);
        glBindFramebuffer(GL_READ_FRAMEBUFFER_, g_fbo_read);
        glFramebufferTexture(GL_READ_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, t->o.gl, 0);
        void display_fit(int sw, int sh, int dw, int dh, int out[4]);      /* PR #24, display.c: the picture keeps its 16:9 shape */
        int d4[4];
        display_fit(16, 9, (int)r.right, (int)r.bottom, d4);
        if (d4[2] != r.right || d4[3] != r.bottom) { glColorMaski(0, 1, 1, 1, 1); glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT); }      /* the game's next draw restores its own mask */
        glBlitFramebuffer(sr[0], sr[1], sr[0] + sr[2], sr[1] + sr[3], d4[0], r.bottom - d4[1], d4[0] + d4[2], r.bottom - d4[1] - d4[3], GL_COLOR_BUFFER_BIT, GL_LINEAR);
        if (g_drawlog_frame >= 0 && frame == g_drawlog_frame + 1) save_bmp("frame_drawlog.bmp", t);      /* the frame the draw log describes */
        { int step = nvn_front_take_step_shot(); if (step) { char path[300]; const char *dir = getenv("SSBU_SHOT_DIR"); snprintf(path, sizeof path, "%s%sstep_%03d.bmp", dir ? dir : "", dir ? "/" : "", step - 1); save_bmp(path, t); } }
        nvn_front_draw_tag_clear();
        if (j->shot_fight) { char path[300]; const char *dir = getenv("SSBU_SHOT_DIR"); snprintf(path, sizeof path, "%s%sfight_%05d.bmp", dir ? dir : "", dir ? "/" : "", j->shot_fight); save_bmp(path, t); printf("shot: fight frame %d saved at presented frame %lld (OpenGL)\n", j->shot_fight, (long long)frame); }
        if (frame == shot || (every > 0 && frame >= shot_from && frame % every == 0)) {
            char path[300];
            const char *dir = getenv("SSBU_SHOT_DIR");                /* default: the current directory */
            snprintf(path, sizeof path, "%s%sframe_%lld.bmp", dir ? dir : "", dir ? "/" : "", (long long)frame);
            save_bmp(path, t);
        }
        if (getenv("SSBU_RTDUMP") && frame == atoi(getenv("SSBU_RTDUMP"))) {       /* debug: every colour target in use, as rt_<n>.bmp */
            for (int k = 0; k < 256; k++) {
                Tex fake;
                char path[64];
                GLint w = 0, h = 0;
                if (!g_fbos[k].used || !g_fbos[k].k.color[0]) continue;
                glBindFramebuffer(GL_READ_FRAMEBUFFER_, g_fbos[k].fbo);
                glBindTexture(GL_TEXTURE_2D, g_fbos[k].k.color[0]);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
                if (w <= 0 || h <= 0) continue;
                memset(&fake, 0, sizeof fake);
                fake.b.width = w; fake.b.height = h;
                snprintf(path, sizeof path, "rt_%03d_gl%u.bmp", k, g_fbos[k].k.color[0]);
                save_bmp(path, &fake);
            }
            glBindFramebuffer(GL_READ_FRAMEBUFFER_, g_fbo_read);
        }
    } else {
        glClearColor(0, 0, 0, 1);                 /* nothing has been drawn into the presented texture yet */
        glClear(GL_COLOR_BUFFER_BIT);
    }
#ifdef _WIN32
    { LONG64 pf1 = pf_now(); HS_IN("SwapBuffers", 0); SwapBuffers(g_dc); HS_OUT(); pf_add(PF_SWAP, pf1); }
#endif
    pf_add(PF_PRESENT, pf0);
    pf_frame_mark(frame);
    g_joblog_cur = frame;
    glEnable(GL_FRAMEBUFFER_SRGB_);
    glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER_, S.have_fbo ? S.fbo : 0);
    g_st.frames = frame;
    if (frame <= 3 || frame % 600 == 0 || g_frame_draws != 0 && g_logn < 40 && (g_logn++, 1))
        printf("nvn_gl: frame %lld: %lld draws (%lld skipped), %lld clears | totals: %lld lists, %lld textures, %lld uploads (%lld MB), %lld programs ok, %lld rejected, %lld GL errors\n",
               (long long)frame, (long long)g_frame_draws, (long long)g_frame_skipped, (long long)g_frame_clears, (long long)g_st.lists, (long long)g_st.tex_created,
               (long long)g_st.tex_uploads, (long long)(g_st.tex_bytes >> 20), (long long)g_st.prog_ok, (long long)g_st.prog_fail, (long long)g_st.gl_errors);
    fflush(stdout);
    {   /* frame rate over the last 300 presented frames */
        static ULONGLONG t0;
        static LONG64 d0, c0;
        if (frame % 300 == 0) {
            ULONGLONG now = GetTickCount64();
            if (t0 && now > t0) printf("nvn_gl: frames %lld..%lld: %.1f fps, %lld draws and %lld dispatches per frame\n", (long long)frame - 300, (long long)frame, 300000.0 / (double)(now - t0),
                                       (long long)(g_st.draws - d0) / 300, (long long)(g_st.dispatches - c0) / 300);
            t0 = now; d0 = g_st.draws; c0 = g_st.dispatches;
            if (frame % 3000 == 0 || g_dummy_draws) { static long long last = -1; if (g_dummy_draws != last) { last = g_dummy_draws; printf("nvn_gl: dummy texture: %lld of %lld draws reference a unit that holds it (%lld units unbound, %lld units with a dead or unknown handle)\n", g_dummy_draws, (long long)g_st.draws, g_dummy_unbound, g_dummy_dead); } }
        }
    }
    g_prev_frame_draws = g_frame_draws;
    if (g_frame_dispatch >= (g_drawlog_mindisp_n > 0 ? g_drawlog_mindisp_n : 1)) g_disp_frames++;
    g_frame_dispatch = 0; g_ncrange = 0;
    input_frame_draws((int)g_frame_draws);        /* nnhid.c: scripted input can wait for a screen by its draw count */
    g_frame_draws = g_frame_skipped = g_frame_clears = 0;
    g_frame_draw_index = 0;
    list_collect();
}

static void job_write_texels(void *arg) {
    WriteJob *j = arg;
    Tex *t = j->tex;
    GlFmt f;
    static int nlog;
    if (!t->o.gl) tex_realize(t);
    if (!gl_fmt(t->b.format, &f)) return;
    int level = j->has_view && (j->view.set & 0x81) == 0x81 ? j->view.lvl0 : 0, layer = j->has_view && (j->view.set & 0x82) == 0x82 ? j->view.lay0 : 0;
    const int *r = j->region;
    while (glGetError()) {}
    if (j->row_stride && f.kind != 4) glPixelStorei(GL_UNPACK_ROW_LENGTH_, (GLint)(j->row_stride / f.bpb));
    if (t->b.target == 4 || t->b.target == 9 || t->b.target == 8) layer += r[2];
    tex_put(t, &f, level, layer, r[0], r[1], r[2], r[3], r[4], r[5] > 0 ? r[5] : 1, j->data);
    glPixelStorei(GL_UNPACK_ROW_LENGTH_, 0);
    GLenum e = glGetError();
    if (e) logf_limited(&nlog, 10, "nvn_gl: WriteTexels on texture %dx%dx%d format 0x%x region %d,%d,%d %dx%dx%d failed: GL error 0x%x\n", t->b.width, t->b.height, t->b.depth, t->b.format,
                        r[0], r[1], r[2], r[3], r[4], r[5], e);
    t->gpu_written = 1; t->uploaded = 1;
    g_st.copies++;
}
/* The render thread's start in OpenGL mode (render_thread, nvn_front.c): the context, the functions beyond 1.1, the
 * state every batch starts from. The front end then runs the job loop. */
static void rb_gl_init(void) {
#ifdef _WIN32
    PIXELFORMATDESCRIPTOR pfd;
#else
    int pfd = 0;
#endif
#ifndef _WIN32
    (void)pfd;
    printf("nvn_gl: the OpenGL renderer is not part of the Linux build\n"); fflush(stdout); ExitProcess(5);
#else
    g_dc = GetDC(g_hwnd);
    memset(&pfd, 0, sizeof pfd);
    pfd.nSize = sizeof pfd; pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32;
    SetPixelFormat(g_dc, ChoosePixelFormat(g_dc, &pfd), &pfd);
    HGLRC rc = wglCreateContext(g_dc);
    if (!rc || !wglMakeCurrent(g_dc, rc)) { printf("nvn_gl: cannot create an OpenGL context (error %lu)\n", GetLastError()); fflush(stdout); ExitProcess(5); }
    int missing = 0;
#define X(ret, name, args) gl##name = (void *)wglGetProcAddress("gl" #name); if (!gl##name) { printf("nvn_gl: the driver lacks gl" #name "\n"); missing++; }
    GLF(X)
#undef X
#define X(ret, name, args) gl##name = (void *)wglGetProcAddress("gl" #name);
    GLO(X)
#undef X
    printf("nvn_gl: %s | %s | OpenGL %s\n", (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
    if (missing) { printf("nvn_gl: %d required OpenGL functions are missing; this backend needs an NVIDIA driver\n", missing); fflush(stdout); ExitProcess(5); }
    BOOL (WINAPI *swap_interval)(int) = (void *)wglGetProcAddress("wglSwapIntervalEXT");
    if (swap_interval) swap_interval(0);
    g_debug = getenv("SSBU_GLDEBUG") != NULL;
    g_dbg_noscissor = getenv("SSBU_NOSCISSOR") != NULL;
    g_dbg_flipface = getenv("SSBU_FLIPFACE") != NULL;
    g_dbg_nocompute = getenv("SSBU_NOCOMPUTE") != NULL;
    g_drawlog_mindisp_n = getenv("SSBU_DRAWLOG_MINDISP") ? atoi(getenv("SSBU_DRAWLOG_MINDISP")) : 0;
    if (getenv("SSBU_SKIPDRAW")) g_dbg_skipdraw = atoi(getenv("SSBU_SKIPDRAW"));
    glEnable(GL_DEBUG_OUTPUT_);
    /* P3 (hitch log, tests/logs/p3b_hitch0.log against p3b_hitch_dbg.log): without synchronous debug output this driver
     * hands the GL calls of the render thread to a thread of its own, and every call that returns something (texture
     * handles, program status, glGetError, buffer read-back) then waits for everything queued before it: 0.1-2 s frames
     * whenever a new shader or texture arrives (65 s of frames over 25 ms in a 170 s run; 13 s with it on). Synchronous
     * debug output is a debugging state of the context: it changes when the callback is called, not what is drawn; the
     * callback prints errors only unless SSBU_GLDEBUG is set, as before. SSBU_GL_DRIVER_THREAD=1 = the old behaviour. */
    if (g_debug || !getenv("SSBU_GL_DRIVER_THREAD")) glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS_);
    g_fdlog = getenv("SSBU_FDLOG") != NULL;
    if (g_fdlog && glDebugMessageControl) glDebugMessageControl(0x1100 /* DONT_CARE */, 0x1100, 0x1100, 0, NULL, 1);     /* every severity, low ones too */
    glDebugMessageCallback((void *)debug_cb, NULL);
    glGetIntegerv(GL_MAX_PROGRAM_PARAMETER_BUFFER_BINDINGS_NV_, &g_max_pbuf);
    printf("nvn_gl: parameter buffer bindings per program: %d; window origin mode %d, depth mode %d\n", g_max_pbuf, g_origin_mode, g_depth_mode);
    glClipControl(g_origin_mode ? GL_UPPER_LEFT_ : GL_LOWER_LEFT_, g_depth_mode ? GL_ZERO_TO_ONE_ : GL_NEGATIVE_ONE_TO_ONE_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glEnable(GL_FRAMEBUFFER_SRGB_);
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS_);
    glEnable(GL_SCISSOR_TEST);
    for (GLuint i = 0; i < 16; i++) glScissorIndexed(i, 0, 0, 16384, 16384);
    glGenBuffers(1, &g_arena);
    glBindBuffer(GL_ARRAY_BUFFER_, g_arena);
    glBufferData(GL_ARRAY_BUFFER_, (ptrdiff_t)ARENA_SIZE, NULL, GL_STREAM_DRAW_);
    glGenFramebuffers(1, &g_fbo_read);
    glGenFramebuffers(2, g_fbo_tmp);
    glGenProgramPipelines(1, &g_pipeline);
    static const uint8_t black[4] = {0, 0, 0, 255};
    { void album_movie_frame_source_attach(void); album_movie_frame_source_attach(); }   /* close4: queue_present feeds the movie maker (job_present_movie) */
    glGenTextures(1, &g_dummy_tex);
    glBindTexture(GL_TEXTURE_2D, g_dummy_tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, 0x8058, 1, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, black);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    g_dummy_handle = glGetTextureHandleARB(g_dummy_tex);
    glMakeTextureHandleResidentARB(g_dummy_handle);
    for (int s = 0; s < 6; s++) { for (int i = 4; i < 36; i++) S.gl_tex[s][i] = g_dummy_handle; for (int i = 0; i < 32; i++) { uint32_t d[4] = {1, 1, 1, 1}; memcpy(&S.gl_tex[s][48 + 2 * i], d, 16); } S.tex_dirty[s] = 1; }
    S.chanmask = 0xffffffffu; S.svmask[0] = S.svmask[1] = 0xff; S.swmask[0] = S.swmask[1] = 0xff;
    S.sc0[2] = S.sc0[3] = 16384;
    S.ds = 0x1111118822ULL;                     /* depth func LESS, stencil funcs ALWAYS, ops KEEP */
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    SwapBuffers(g_dc);
#endif
    fflush(stdout);
    shd_init();
    hitch_set_render();
    nvn_c4_gl_selftest_hook();                  /* SSBU_NVN_GLTEST: OpenGL self-test of nvn_c4.c, then exit */
}
/* The nvn_gl: lines of the exit report (nvn_gl_report, nvn_front.c; printed in Vulkan mode too, as before). */
static void rb_gl_report(FILE *out) {
    fprintf(out, "\nnvn_gl: %lld frames, %lld batches, %lld command lists (%lld unknown handles), %lld draws (+%lld skipped), %lld clears, %lld copies\n", (long long)g_st.frames,
            (long long)g_st.submits, (long long)g_st.lists, (long long)g_st.stale, (long long)g_st.draws, (long long)g_st.draws_skipped, (long long)g_st.clears, (long long)g_st.copies);
    fprintf(out, "nvn_gl: textures %lld (uploads %lld, %lld MB, views %lld, bindless handles %lld, unmapped formats %lld), framebuffers %lld (%lld incomplete)\n", (long long)g_st.tex_created,
            (long long)g_st.tex_uploads, (long long)(g_st.tex_bytes >> 20), (long long)g_st.views, (long long)g_st.handles, (long long)g_st.unknown_fmt, (long long)g_st.fbo_made, (long long)g_st.fbo_bad);
    fprintf(out, "nvn_gl: nvnProgramSetShaders calls %ld, stages compiled %lld, rejected %lld, GL errors %lld, streamed %lld MB\n", (long)g_setshaders_calls, (long long)g_st.prog_ok,
            (long long)g_st.prog_fail, (long long)g_st.gl_errors, (long long)(g_st.arena_bytes >> 20));
    fprintf(out, "nvn_gl: shader cache: %lld stages compiled by the driver, %lld taken from the cache by machine code, %lld by identical GLSL text; texture memory checks: %lld unwritten (write watch), %lld written and hashed, %lld not watched (hashed)\n",
            (long long)g_shc_compiled, (long long)g_shc_hit, (long long)g_shc_src_hit, (long long)g_ww_clean, (long long)g_ww_dirty, (long long)g_ww_unwatched);
    fprintf(out, "nvn_gl: shader binaries on disk: %lld loaded, %lld compiled and saved by the render thread, %lld refused by the driver (compiled again)\n", (long long)g_shd_loaded, (long long)g_shd_saved, (long long)g_shd_refused);
    for (int i = 0; g_fdlog && i < 64 && g_fd_ids[i].n; i++) fprintf(out, "fdlog-id: %u type %x severity %x: %lld messages, first: %s\n", g_fd_ids[i].id, g_fd_ids[i].type, g_fd_ids[i].sev, (long long)g_fd_ids[i].n, g_fd_ids[i].text);
    { LARGE_INTEGER f; QueryPerformanceFrequency(&f);
      fprintf(out, "nvn_gl: shader pairs made ready by the workers (SSBU_PAIR_WARM=%d): %lld from pairs.txt + %lld programs handed over; %lld warm draws, %lld of them over 8 ms (not in the driver's cache), %.1f s in all; %lld known already, %lld without binaries or not translated, %lld stages compiled for it\n",
              pw_mode(), (long long)g_pw_named, (long long)g_pw_progs, (long long)g_pw_warmed, (long long)g_pw_cold_n, (double)g_pw_ticks / (double)f.QuadPart, (long long)g_pw_dup, (long long)g_pw_missing, (long long)g_pw_compiled);
      fprintf(out, "nvn_gl: presents held for shader preparation: %lld times, %.2f s in all\n", (long long)g_pw_gates, (double)g_pw_gate_ticks / (double)f.QuadPart); }
    fprintf(out, "nvn_gl: shader pairs drawn (pairs.txt): %lld known at start, %lld added in this run\n", (long long)g_prec_loaded, (long long)g_prec_added);
    if (pair_mode()) fprintf(out, "nvn_gl: linked pair programs (SSBU_PAIR=%d): %lld loaded from disk, %lld linked here, %lld not linkable, %lld binaries refused, %lld draws used one\n", pair_mode(), (long long)g_pair_loaded, (long long)g_pair_linked, (long long)g_pair_failed, (long long)g_pair_refused, (long long)g_pair_draws);
    { LARGE_INTEGER f; QueryPerformanceFrequency(&f); fprintf(out, "nvn_gl: first draws of a program pair preceded by a discarded point: %lld (%lld over 4 ms, %.0f ms in all), not done %lld (counter running or vertex stage with side effects)\n", (long long)g_fdw_n, (long long)g_fdw_slow, (double)g_fdw_ticks * 1000.0 / (double)f.QuadPart, (long long)g_fdw_skipped); }
    { LARGE_INTEGER f; QueryPerformanceFrequency(&f); fprintf(out, "nvn_gl: nvnProgramSetShaders held its caller %lld times, %.1f s in all, while all workers were compiling\n", (long long)g_shw_held, (double)g_shw_held_ticks / (double)f.QuadPart); }
    fprintf(out, "nvn_gl: shader variants (variants.txt): %lld known at start, %lld of them queued for the workers, %lld added in this run\n", (long long)g_shv_loaded, (long long)g_shv_queued, (long long)g_shv_added);
    fprintf(out, "nvn_gl: shader workers (%d): %lld stages queued, %lld done (%lld compiled and saved, %lld already on disk, %lld not translated or not saved)\n", g_shw_n, (long long)g_shw_queued, (long long)g_shw_done,
            (long long)g_shw_compiled, (long long)g_shw_present, (long long)g_shw_failed);
    fprintf(out, "nvn_gl: shaders given with NV assembly text: %ld; with Maxwell machine code only: %ld (translated to GLSL: %lld stages, not translatable yet: %lld; draws through GLSL: %lld)\n", (long)g_with_text,
            (long)g_without_text, (long long)g_st.glsl_ok, (long long)g_st.glsl_fail, (long long)g_st.glsl_draws);
    fprintf(out, "nvn_gl: compute dispatches %lld (run %lld, skipped %lld), %lld MB copied back to guest memory, draws using a vertex buffer written by compute in the same frame: %lld\n",
            (long long)g_st.dispatches, (long long)g_st.dispatch_run, (long long)g_st.dispatch_skipped, (long long)(g_st.compute_bytes >> 20), (long long)g_st.draws_from_compute);
    for (int h = 0; h < 64 && g_chist[h].serial; h++)
        fprintf(out, "nvn_gl:   compute program #%d: %lld dispatches (%lld run), %lld groups\n", g_chist[h].serial, (long long)g_chist[h].calls, (long long)g_chist[h].run, (long long)g_chist[h].groups);
    for (int s = 0; s < 6; s++) {
        int any = 0;
        for (int i = 0; i < 32; i++) any |= g_bufhist[s][i] != 0;
        if (!any) continue;
        fprintf(out, "nvn_gl: program.buffer[n] uses in %s programs (n:count):", k_stage_name[s]);
        for (int i = 0; i < 32; i++) if (g_bufhist[s][i]) fprintf(out, " %d:%ld", i, (long)g_bufhist[s][i]);
        fprintf(out, "\n");
    }
    fprintf(out, "nvn_gl: program.storage[n] uses (n:count):");
    for (int i = 0; i < 32; i++) if (g_storhist[i]) fprintf(out, " %d:%ld", i, (long)g_storhist[i]);
    fprintf(out, "\n");
}
/* ---------------------------------------------------------------- what the NVN entry points (nvn_front.c) hand to OpenGL */
/* nvnProgramSetShaders, guest thread (P3 round 4): one job that makes the program's vertex and fragment stage ready (binaries
 * compiled when missing) and has the driver do its first-draw work for the pair. Returns 0 when it does not apply (the
 * stages are then handed to the workers one by one, as before). After start-up the caller is held until the workers
 * have nothing left, so a fighter or stage that is loaded is ready when its loading ends. */
static int pw_queue_prog(Prog *p) {
    int m = pw_mode();
    Stage *v = &p->st[0], *f = &p->st[1];
    if (!m || g_shd_mode <= 0 || !g_shw_n || g_shw_ctx_failed || (m < 2 && g_st.frames < 300) || GetCurrentThreadId() == g_render_tid) return 0;
    if (!(v->code && v->code_len > 0 && v->glsl_state == 0) || p->st[2].state || p->st[3].state || p->st[4].state) return 0;
    int has_f = f->code && f->code_len > 0 && f->glsl_state == 0;
    if (f->state && !has_f) return 0;
    uint32_t vc = v->ctl[3] && v->ctl[4] ? v->ctl[3] : 0, fc = has_f && f->ctl[3] && f->ctl[4] ? f->ctl[3] : 0;
    size_t need = (size_t)v->code_len + vc + (has_f ? (size_t)f->code_len + fc : 0);
    ShJob *j = calloc(1, sizeof *j + need);
    uint8_t *d = j->data;
    j->stage = 3; j->code_len = v->code_len; j->cdata_size = vc; j->key[0] = has_f ? (uint32_t)f->code_len : 0; j->key[1] = fc;
    memcpy(d, v->code, (size_t)v->code_len); d += v->code_len;
    if (vc) { memcpy(d, v->code - 0x30 + v->ctl[4], vc); d += vc; }
    if (has_f) { memcpy(d, f->code, (size_t)f->code_len); d += f->code_len; if (fc) memcpy(d, f->code - 0x30 + f->ctl[4], fc); }
    uint64_t h = sh_hash(0x5041495257ULL ^ (uint64_t)j->code_len ^ ((uint64_t)j->key[0] << 32), j->data, need);
    if (!h) h = 1;
    memcpy(&j->key[2], &h, 8);
    int seen = 0;
    EnterCriticalSection(&g_shw_lock);
    unsigned k = (unsigned)(h >> 20) & 16383;
    for (int probe = 0; probe < 64; probe++, k = (k + 1) & 16383) { if (g_pw_qseen[k] == h) { seen = 1; break; } if (!g_pw_qseen[k]) { g_pw_qseen[k] = h; break; } }
    LeaveCriticalSection(&g_shw_lock);
    if (seen) free(j); else { InterlockedIncrement64(&g_pw_progs); pw_enqueue(j); }
    static int nowait = -1;
    if (nowait < 0) nowait = getenv("SSBU_SHADER_NOWAIT") != NULL;
    if (!nowait && g_st.frames >= 300 && g_shw_queued != g_shw_done) {
        LONG64 t0 = pf_now(), freq = 0;
        { LARGE_INTEGER q; QueryPerformanceFrequency(&q); freq = q.QuadPart; }
        while (g_shw_queued != g_shw_done && pf_now() - t0 < 30 * freq) Sleep(1);
        InterlockedExchangeAdd64(&g_shw_held_ticks, pf_now() - t0); InterlockedIncrement64(&g_shw_held);
    }
    return 1;
}
/* nvnProgramSetShaders in OpenGL mode (prog_set_shaders_, nvn_front.c), guest thread. */
static void rb_gl_program_set(Prog *p) {
    if (!pw_queue_prog(p))
    for (int s = 0; s < 2; s++) {              /* P3: have the driver's binary ready before the first draw */
        Stage *st = &p->st[s];
        if (st->code && st->code_len > 0 && st->glsl_state == 0)
            shd_prewarm(s, st->code, st->code_len, st->ctl[3] && st->ctl[4] ? st->code - 0x30 + st->ctl[4] : NULL, st->ctl[3]);
    }
}
/* PresentJob to the movie maker's window (queue_present, nvn_front.c): the picture is read back for the encoder. */
int album_movie_push_frame(const uint8_t *rgba, int w, int h, int stride, int bottom_up);
void album_movie_frame_source_attach(void);
static void job_present_movie(void *arg) {
    PresentJob *j = arg;
    Tex *t = j->win && j->index >= 0 && j->index < j->win->count ? j->win->tex[j->index] : NULL;
    if (!t || t->dead || !t->o.gl || t->b.width <= 0 || t->b.height <= 0) return;
    int w = t->b.width, h = t->b.height;
    uint8_t *px = malloc((size_t)w * h * 4);
    if (!px) return;
    glBindFramebuffer(GL_READ_FRAMEBUFFER_, g_fbo_read);
    glFramebufferTexture(GL_READ_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, t->o.gl, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);                /* GL row 0 is the top row of an NVN texture (see save_bmp) */
    album_movie_push_frame(px, w, h, w * 4, 0);
    free(px);
    glBindFramebuffer(GL_FRAMEBUFFER_, S.have_fbo ? S.fbo : 0);
}
static void job_housekeeping(void *arg) { (void)arg; gl_housekeeping(); }
static void job_sync_drop(void *arg) { SyncObj *o = arg; if (o->s) glDeleteSync(o->s); o->s = NULL; }
static void fence_now(SyncObj *o, int flags) {
    if (o->dead) return;
    if (o->s) glDeleteSync(o->s);
    o->s = glFenceSync(0x9117 /* SYNC_GPU_COMMANDS_COMPLETE */, 0);
    if (flags & 1) glFlush();                           /* NVN_SYNC_FLAG_FLUSH_FOR_CPU */
}
static void job_fence(void *arg) { SyncJob *j = arg; fence_now(j->o, j->flags); }
static void job_sync_wait(void *arg) {
    SyncJob *j = arg;
    if (!j->o->s) { j->result = 3; return; }            /* never fenced: NVN_SYNC_WAIT_RESULT_FAILED */
    GLenum r = glClientWaitSync(j->o->s, 1 /* SYNC_FLUSH_COMMANDS_BIT */, j->timeout);
    j->result = r == 0x911A ? 0 : r == 0x911C ? 1 : r == 0x911B ? 2 : 3;       /* already signaled, condition satisfied, timeout expired, failed */
}
static void job_server_wait(void *arg) { SyncJob *j = arg; if (j->o->s) glWaitSync(j->o->s, 0, 0xFFFFFFFFFFFFFFFFull); j->result = j->o->s != NULL; }
static void job_flush(void *arg) { (void)arg; glFlush(); }
static void job_finish(void *arg) { (void)arg; glFinish(); }
static void job_reset_status(void *arg) { *(int *)arg = glGetGraphicsResetStatus ? (int)glGetGraphicsResetStatus() : 0; }
static void job_grid(void *arg) { GLint *wh = arg; wh[0] = wh[1] = 1; glGetIntegerv(0x933E /* SAMPLE_LOCATION_PIXEL_GRID_WIDTH_NV */, &wh[0]); glGetIntegerv(0x933F, &wh[1]); while (glGetError()) {} }
static void job_read_texels(void *arg) {
    ReadJob *j = arg;
    Tex *t = j->tex;
    GlFmt f;
    static uint8_t *tmp;
    static size_t tmp_cap;
    static int nlog;
    if (!gl_fmt(t->b.format, &f)) return;
    tex_sync(t);
    while (glGetError()) {}
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    int level = j->has_view && (j->view.set & 0x81) == 0x81 ? j->view.lvl0 : 0, layer0 = j->has_view && (j->view.set & 0x82) == 0x82 ? j->view.lay0 : 0;
    GLenum fo = f.kind == 1 ? 0x1902 : f.kind == 2 ? GL_STENCIL_INDEX_ : f.format;
    if (!j->to_pool) {
        const int *r = j->region;
        int d = r[5] > 0 ? r[5] : 1, z = t->b.target == 2 ? r[2] : layer0 + r[2];
        size_t rows = (size_t)(r[4] + f.bh - 1) / f.bh, rowb = j->row_stride ? (size_t)j->row_stride : ((size_t)(r[3] + f.bw - 1) / f.bw) * f.bpb;
        size_t bytes = (j->image_stride ? (size_t)j->image_stride : rowb * rows) * (size_t)d;
        if (j->row_stride && f.kind != 4) glPixelStorei(0x0D02 /* PACK_ROW_LENGTH */, (GLint)(j->row_stride / f.bpb));
        if (j->image_stride && j->row_stride && f.kind != 4) glPixelStorei(0x806C /* PACK_IMAGE_HEIGHT */, (GLint)(j->image_stride / j->row_stride));
        if (f.kind == 4) glGetCompressedTextureSubImage(t->o.gl, level, r[0], r[1], z, r[3], r[4], d, (GLsizei)bytes, j->out);
        else glGetTextureSubImage(t->o.gl, level, r[0], r[1], z, r[3], r[4], d, fo, f.type, (GLsizei)bytes, j->out);
        glPixelStorei(0x0D02, 0); glPixelStorei(0x806C, 0);
    } else if (t->mem && t->gpu_written && !t->b.packaged) {
        const TexFormat *tf = tex_format(t->b.format);
        TexLayout L;
        if (tf && tf->sw_decode) { logf_limited(&nlog, 5, "nvn_gl: InvalidateTexels on a texture of format 0x%x that is kept decoded: its pool memory is not rewritten\n", t->b.format); return; }
        if (t->b.samples > 1) { logf_limited(&nlog, 5, "nvn_gl: InvalidateTexels on a multisample texture: its pool memory is not rewritten\n"); return; }
        if (t->b.flags & 0x10) {
            int64_t stride = t->b.stride ? t->b.stride : (int64_t)t->b.width * f.bpb;
            if (f.kind == 4 || t->mem + stride * t->b.height > t->limit) return;
            glPixelStorei(0x0D02, (GLint)(stride / f.bpb));
            glGetTextureSubImage(t->o.gl, 0, 0, 0, 0, t->b.width, t->b.height, 1, fo, f.type, (GLsizei)(stride * t->b.height), t->mem);
            glPixelStorei(0x0D02, 0);
        } else {
            tex_lay(&t->b, &L);
            if (level >= L.levels || level >= t->glevels) return;
            for (int layer = 0; layer < L.layers; layer++) {
                uint8_t *dst = (uint8_t *)(uintptr_t)tex_level_ptr(&L, t->mem, level, layer);
                size_t lin = L.lvl_linear_size[level];
                if (t->limit && dst + L.lvl_size[level] > t->limit) return;
                if (lin > tmp_cap) { tmp_cap = lin * 2; tmp = realloc(tmp, tmp_cap); }
                int is3d = t->b.target == 2;
                if (f.kind == 4) glGetCompressedTextureSubImage(t->o.gl, level, 0, 0, is3d ? 0 : layer, L.lvl_w[level], L.lvl_h[level], is3d ? L.lvl_d[level] : 1, (GLsizei)lin, tmp);
                else glGetTextureSubImage(t->o.gl, level, 0, 0, is3d ? 0 : layer, L.lvl_w[level], L.lvl_h[level], is3d ? L.lvl_d[level] : 1, fo, f.type, (GLsizei)lin, tmp);
                tex_swizzle_level(&L, level, tmp, dst);
            }
        }
        t->hash = mem_hash(t->mem, t->data_size, t->ww_hashed || (t->b.flags & 0x10) != 0);
    }
    GLenum e = glGetError();
    if (e) logf_limited(&nlog, 10, "nvn_gl: reading texels of texture %dx%dx%d format 0x%x failed: GL error 0x%x\n", t->b.width, t->b.height, t->b.depth, t->b.format, e);
}
/* Seam step S2: the OpenGL back end as an RBackend (nvn_front.h); rb_vk is in nvn_vk_draw.c. */
static void rb_gl_idle(void) { gl_housekeeping(); }
const RBackend rb_gl = {"gl", job_submit, job_submit_async, job_present, job_present_movie, job_fence, job_sync_wait, job_server_wait, job_sync_drop, job_flush, job_finish,
                        job_write_texels, job_read_texels, job_housekeeping, job_reset_status, job_grid, rb_gl_idle,
                        rb_gl_init, rb_gl_report, rb_gl_frame_stats, rb_gl_program_set};
