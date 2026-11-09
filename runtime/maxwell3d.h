/* The Maxwell 3D engine (class B197): register (method) indices, i.e. NVIDIA's documented
 * byte offsets (open-gpu-doc, clb197.h) divided by four. (j) is an array element. */
#pragma once

#define M_MME_INSTRUCTION_PTR      0x045
#define M_MME_INSTRUCTION          0x046
#define M_MME_START_PTR            0x047
#define M_MME_START                0x048
#define M_RASTER_ENABLE            0x0DF
#define M_CT(j)                    (0x200 + (j) * 0x10)   /* + 0 address hi, 1 lo, 2 width, 3 height, 4 format, 5 block size, 6 depth, 7 array pitch, 8 layer */
#define M_VIEWPORT_SCALE(j)        (0x280 + (j) * 8)      /* + 0..2 scale xyz, 3..5 offset xyz, 6 swizzle */
#define M_VIEWPORT_CLIP(j)         (0x300 + (j) * 4)      /* + 0 x | width << 16, 1 y | height << 16, 2 min z, 3 max z */
#define M_VERTEX_ARRAY_START       0x35D
#define M_VERTEX_ARRAY_COUNT       0x35E
#define M_VIEWPORT_Z_CLIP          0x35F                  /* bit 0: z clipped to [0, w] (else [-w, w]) */
#define M_COLOR_CLEAR(i)           (0x360 + (i))
#define M_Z_CLEAR                  0x364
#define M_STENCIL_CLEAR            0x368
#define M_SCISSOR(j)               (0x380 + (j) * 4)      /* + 0 enable, 1 x min | max << 16, 2 y min | max << 16 */
#define M_ZT_A                     0x3F8                  /* + 0 address hi, 1 lo, 2 format, 3 block size, 4 array pitch */
#define M_VERTEX_ID_BASE           0x446
#define M_VERTEX_ATTRIBUTE(i)      (0x458 + (i))
#define M_DRAW_ARRAY_INSTANCE_FIRST 0x485
#define M_DRAW_ARRAY_INSTANCE_NEXT 0x486
#define M_CT_SELECT                0x487
#define M_ZT_SIZE                  0x48A                  /* + 0 width, 1 height, 2 depth */
#define M_SAMPLER_BINDING          0x48D
#define M_DEPTH_TEST               0x4B3
#define M_FILL_MODE                0x4B4
#define M_BLEND_STATE_PER_TARGET   0x4B9
#define M_DEPTH_WRITE              0x4BA
#define M_ALPHA_TEST               0x4BB
#define M_DEPTH_FUNC               0x4C3
#define M_ALPHA_REF                0x4C4
#define M_ALPHA_FUNC               0x4C5
#define M_BLEND_CONST(i)           (0x4C7 + (i))
#define M_BLEND_SEPARATE_ALPHA     0x4CF
#define M_BLEND_COLOR_OP           0x4D0
#define M_BLEND_COLOR_SRC          0x4D1
#define M_BLEND_COLOR_DST          0x4D2
#define M_BLEND_ALPHA_OP           0x4D3
#define M_BLEND_ALPHA_SRC          0x4D4
#define M_BLEND_ALPHA_DST          0x4D6
#define M_BLEND_ENABLE(i)          (0x4D8 + (i))
#define M_STENCIL_TEST             0x4E0
#define M_STENCIL_OP_FAIL          0x4E1
#define M_STENCIL_OP_ZFAIL         0x4E2
#define M_STENCIL_OP_ZPASS         0x4E3
#define M_STENCIL_FUNC             0x4E4
#define M_STENCIL_REF              0x4E5
#define M_STENCIL_FUNC_MASK        0x4E6
#define M_STENCIL_MASK             0x4E7
#define M_WINDOW_ORIGIN            0x4EB
#define M_GLOBAL_BASE_VERTEX       0x50D
#define M_GLOBAL_BASE_INSTANCE     0x50E
#define M_POINT_SIZE               0x546
#define M_ZT_SELECT                0x54E
#define M_ANTI_ALIAS_ALPHA_CONTROL 0x54F                  /* bit 0 alpha to coverage, bit 4 alpha to one */
#define M_TEX_SAMPLER_POOL         0x557                  /* + 0 address hi, 1 lo, 2 maximum index */
#define M_TEX_HEADER_POOL          0x55D                  /* + 0 address hi, 1 lo, 2 maximum index */
#define M_BACK_STENCIL_REF         0x3D5
#define M_BACK_STENCIL_MASK        0x3D6   /* write mask */
#define M_BACK_STENCIL_FUNC_MASK   0x3D7
#define M_TWO_SIDED_STENCIL        0x565
#define M_BACK_STENCIL_OP_FAIL     0x566
#define M_BACK_STENCIL_OP_ZFAIL    0x567
#define M_BACK_STENCIL_OP_ZPASS    0x568
#define M_BACK_STENCIL_FUNC        0x569
#define M_SRGB_WRITE               0x56E
#define M_DEPTH_BIAS               0x56F
#define M_PROGRAM_REGION           0x582                  /* + 0 address hi, 1 lo */
#define M_END                      0x585
#define M_BEGIN                    0x586                  /* topology in bits 0-15; instance next/continue bits 26, 27 */
#define M_PRIMITIVE_RESTART        0x591
#define M_PRIMITIVE_RESTART_INDEX  0x592
#define M_INDEX_BUFFER             0x5F2                  /* + 0 address hi, 1 lo, 2 limit hi, 3 limit lo, 4 format, 5 first */
#define M_DRAW_INDEX_COUNT         0x5F8
#define M_VERTEX_STREAM_INSTANCED(i) (0x620 + (i))
#define M_CULL_ENABLE              0x646
#define M_FRONT_FACE               0x647
#define M_CULL_FACE                0x648
#define M_VIEWPORT_SCALE_OFFSET    0x64B
#define M_VIEWPORT_CLIP_CONTROL    0x64F
#define M_PRIMITIVE_TOPOLOGY       0x65C
#define M_CLEAR_SURFACE            0x674
#define M_CT_WRITE(i)              (0x680 + (i))
#define M_REPORT_SEMAPHORE         0x6C0                  /* + 0 address hi, 1 lo, 2 payload, 3 operation */
#define M_VERTEX_STREAM(j)         (0x700 + (j) * 4)      /* + 0 stride | enable << 12, 1 address hi, 2 lo, 3 divisor */
#define M_BLEND_PER_TARGET(j)      (0x780 + (j) * 8)      /* + 0 separate alpha, 1 color op, 2 src, 3 dst, 4 alpha op, 5 src, 6 dst */
#define M_VERTEX_STREAM_LIMIT(j)   (0x7C0 + (j) * 2)      /* + 0 hi, 1 lo */
#define M_PIPELINE(j)              (0x800 + (j) * 0x10)   /* + 0 enable | type << 4, 1 program offset, 3 register count */
#define M_CB_SIZE                  0x8E0
#define M_CB_ADDRESS               0x8E1                  /* + 0 hi, 1 lo */
#define M_CB_LOAD_OFFSET           0x8E3
#define M_CB_LOAD(i)               (0x8E4 + (i))
#define M_CB_BIND(stage)           (0x904 + (stage) * 8)  /* valid bit 0, slot bits 4-8 */
#define M_BINDLESS_TEXTURE         0x982                  /* constant buffer slot holding texture handles */

/* Shader stages, as numbered by the constant buffer bind groups. */
enum { STAGE_VERTEX, STAGE_TESS_CONTROL, STAGE_TESS_EVAL, STAGE_GEOMETRY, STAGE_FRAGMENT, STAGES };
