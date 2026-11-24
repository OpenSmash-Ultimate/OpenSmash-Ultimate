/* The multimedia engines behind host1x: NVDEC (video decode), VIC (colour conversion and
 * scaling) and NVJPG, driven by command buffers of host1x opcodes.
 *
 * A command buffer is a stream of headers, each followed by its data words:
 *   bits 31:28 mode - 0 set class (class id in bits 15:6), 1 incrementing, 2 non-incrementing
 *   (count in bits 15:0), 3 mask (one word per set bit of bits 15:0), 4 immediate (data in
 *   bits 15:0); bits 27:16 the register offset.
 * An engine is programmed through its THI registers: METHOD0 (0x10) takes a method index,
 * METHOD1 (0x11) that method's argument. Method arguments that are addresses are in 256-byte
 * units of the engines' own (SMMU) address space, which MapBuffer hands out per nvmap handle.
 *
 * The engines' work itself is not performed yet - movies show whatever their output surfaces
 * held. What they are asked to do is recorded: SSBU_LOG_HOST1X=n logs the first n executes of
 * each engine with the methods set; SSBU_DUMP_HOST1X=dir also saves, per execute, the method
 * registers and the structures they point at (for working out the decode offline). */
#define _CRT_SECURE_NO_WARNINGS
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "gpu.h"
#include "gpu_vp9.h"
#include "gpu_h264.h"
#ifdef SSBU_HAVE_VPX
#include <math.h>
#include <vpx/vpx_decoder.h>
#include <vpx/vp8dx.h>
#ifdef _WIN32
#include <process.h>
/* libvpx was built against the DLL C runtime: the linker finds its imports in the static one
 * only among what something else has pulled in already, which for these two is this. */
void *const vpx_crt_imports[] = {(void *)_beginthreadex, (void *)round};
#endif
#endif

enum { CLASS_HOST1X = 0x01, CLASS_VIC = 0x5D, CLASS_NVJPG = 0xC0, CLASS_NVDEC = 0xF0 };
enum { THI_METHOD0 = 0x10, THI_METHOD1 = 0x11 };
#define METHODS 0x800

/* ---- the engines' address space: a range per nvmap handle --------------------------- */
static struct { u64 dev, size; u32 handle; } maps[4096];
static u32 nmaps;
static u64 next_dev = 0x10000000;

u32 host1x_map(u32 handle) {
    for (u32 i = 0; i < nmaps; i++)
        if (maps[i].handle == handle) return (u32)maps[i].dev;
    u64 size = handle < NVMAP_MAX ? gpu_maps[handle].size : 0;
    u64 span = (size + 0xFFFF) & ~0xFFFFull;
    if (!span) span = 0x10000;
    if (next_dev + span > 0xFF000000ull) next_dev = 0x10000000;   /* (wraps: old mappings are long gone by then) */
    u32 slot = nmaps < 4096 ? nmaps++ : handle % 4096;
    maps[slot].dev = next_dev;
    maps[slot].size = size;
    maps[slot].handle = handle;
    next_dev += span;
    return (u32)maps[slot].dev;
}

/* The guest address of engine address dev (0 if unmapped); *left: bytes to the handle's end. */
static u64 to_guest(u64 dev, u64 *left) {
    for (u32 i = 0; i < nmaps; i++)
        if (dev >= maps[i].dev && dev < maps[i].dev + maps[i].size && gpu_maps[maps[i].handle].addr) {
            if (left) *left = maps[i].dev + maps[i].size - dev;
            return gpu_maps[maps[i].handle].addr + (dev - maps[i].dev);
        }
    if (left) *left = 0;
    return 0;
}

/* ---- engines -------------------------------------------------------------------------- */
typedef struct Engine {
    const char *name;
    u32 cls, method0, executes;
    u32 regs[METHODS];   /* method arguments, by method index */
} Engine;
static Engine engines[] = {{"nvdec", CLASS_NVDEC}, {"vic", CLASS_VIC}, {"nvjpg", CLASS_NVJPG}};

static Engine *engine_of(u32 cls) {
    for (u32 i = 0; i < sizeof engines / sizeof *engines; i++)
        if (engines[i].cls == cls) return &engines[i];
    return NULL;
}

/* Which methods hold addresses worth saving, and how much at most. */
static u64 dump_size(const Engine *e, u32 m) {
    if (e->cls == CLASS_NVDEC) {
        if (m == 0x101) return 0x400;                   /* picture setup */
        if (m == 0x102) {                               /* bitstream: as much as the picture setup says */
            u64 left, g = to_guest((u64)e->regs[0x101] << 8, &left);
            if (e->regs[0x80] == 3) return g && left >= H264_SETUP_STREAM_LEN + 4 ? ld32(g + H264_SETUP_STREAM_LEN) : 0;   /* (H.264's setup) */
            return g && left >= 0x34 ? ld32(g + 0x30) : 0;
        }
        if (m == 0x170) return 0x1000;                  /* VP9 probabilities */
        if (m == 0x109) return 0x100;                   /* status */
    }
    if (e->cls == CLASS_VIC && (m == 0x1C2 || m == 0x1C3)) return 0x1000;   /* config, filter structs */
    return 0;
}

static void record(Engine *e) {
    static int log_n = -1;
    static const char *dir = (const char *)1;
    if (log_n < 0) log_n = getenv("SSBU_LOG_HOST1X") ? atoi(getenv("SSBU_LOG_HOST1X")) : 0;
    if (dir == (const char *)1) dir = getenv("SSBU_DUMP_HOST1X");
    u32 n = e->executes++;
    if (n == 0) fprintf(stderr, "[host1x] first %s job\n", e->name);
    if ((int)n < log_n) {
        fprintf(stderr, "[host1x] %s execute %u:", e->name, n);
        for (u32 m = 0; m < METHODS; m++)
            if (e->regs[m] && m != 0xC0) fprintf(stderr, " %03x=%08x", m, e->regs[m]);
        fprintf(stderr, "\n");
        for (u32 m = 0; m < METHODS; m++) {
            if (!e->regs[m] || !dump_size(e, m)) continue;
            u64 left, g = to_guest((u64)e->regs[m] << 8, &left);
            fprintf(stderr, "[host1x]   %03x -> %#llx (%llu bytes to its end):", m, g, left);
            for (u32 i = 0; g && i < 64 && i < left; i += 4) fprintf(stderr, " %08x", ld32(g + i));
            fprintf(stderr, "\n");
        }
    }
    static u32 dumped;
    if (!dir || dumped >= 2000) return;
    dumped++;
    char path[512];
    snprintf(path, sizeof path, "%s/%s_%05u.bin", dir, e->name, n);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    /* "H1X1", method count, the methods; then per saved structure: method, size, guest bytes */
    u32 head[2] = {0x31583148, METHODS};
    fwrite(head, 4, 2, f);
    fwrite(e->regs, 4, METHODS, f);
    for (u32 m = 0; m < METHODS; m++) {
        u64 want = e->regs[m] ? dump_size(e, m) : 0, left;
        u64 g = want ? to_guest((u64)e->regs[m] << 8, &left) : 0;
        if (!g) continue;
        u32 size = (u32)(left < want ? left : want);
        fwrite(&m, 4, 1, f);
        fwrite(&size, 4, 1, f);
        fwrite(P(void, g), 1, size, f);
    }
    fclose(f);
}

/* ---- NVDEC: VP9 ------------------------------------------------------------------- */
/* Methods: 0x80 the codec (9: VP9), 0xC0 execute, 0x101 the picture setup, 0x102 the bitstream
 * (tile data only: the driver has parsed the headers), 0x10C-0x10F the luma surfaces of the
 * last, golden and altref references and of the frame decoded (0x11D-0x120 their chroma),
 * 0x170 the probabilities to decode with. Addresses in 256-byte units.
 *
 * The frame goes to libvpx whole: its headers are written back from the picture setup and the
 * probabilities (gpu_vp9.c). One fact is missing - whether the frame is shown (hidden ones are
 * altrefs, and it changes how the next frame predicts motion) - and the next job's setup has
 * it; so a frame waits for that job, or for a VIC job that shows it, before it is decoded. */
enum {   /* the picture setup */
    PIC_BITSTREAM_SIZE = 0x30,
    PIC_SIZE = 0x60,            /* s16 width, height (the references' at 0x48, 0x50, 0x58) */
    PIC_FLAGS = 0x68,
    PIC_SIGN_BIAS = 0x6C,       /* s8 per reference frame: intra, last, golden, altref */
    PIC_LF_LEVEL = 0x70, PIC_LF_SHARPNESS = 0x71,
    PIC_BASE_Q = 0x72, PIC_DQ_Y_DC = 0x73, PIC_DQ_UV_AC = 0x74, PIC_DQ_UV_DC = 0x75,
    PIC_TX_MODE = 0x77, PIC_ALLOW_HP = 0x78, PIC_INTERP_FILTER = 0x79, PIC_REFERENCE_MODE = 0x7A,
    PIC_TILE_COLS = 0x7E, PIC_TILE_ROWS = 0x7F,
    PIC_SEG = 0x80,             /* enabled, update map, temporal update, abs delta; u32 mask[8]; s16 data[8][4] */
    PIC_LF_DELTAS = 0xE4,       /* enabled; s8 ref[4]; s8 mode[2] */
};
enum { F_KEY = 1, F_LAST_KEY = 2, F_ERROR_RESILIENT = 8, F_LAST_SHOWN = 0x10, F_INTRA_ONLY = 0x20 };

/* The probabilities as the engine takes them: (offset, count) per field, in Vp9Probs order. */
static void probs_from_engine(const u8 *e, Vp9Probs *p) {
    vp9_default_probs(p);   /* (uv mode probabilities are not in there: never updated) */
    memcpy(p->tx8, e + 0x470, 2);
    memcpy(p->tx16, e + 0x472, 4);
    memcpy(p->tx32, e + 0x476, 6);
    for (int i = 0; i < 4 * 2 * 2 * 6 * 6; i++) memcpy(&p->coef[0][0][0][0][0][0] + 3 * i, e + 0x5A0 + 4 * i, 3);   /* 4 bytes per node triple */
    memcpy(p->skip, e + 0x537, 3);
    for (int i = 0; i < 7; i++) memcpy(p->inter_mode[i], e + 0x400 + 4 * i, 3);
    memcpy(p->interp_filter, e + 0x52A, 8);
    memcpy(p->is_inter, e + 0x41C, 4);
    memcpy(p->comp_mode, e + 0x532, 5);
    memcpy(p->single_ref, e + 0x580, 10);
    memcpy(p->comp_ref, e + 0x58A, 5);
    for (int i = 0; i < 4; i++) { memcpy(p->y_mode[i], e + 0x480 + 8 * i, 8); p->y_mode[i][8] = e[0x47C + i]; }
    for (int i = 0; i < 16; i++) memcpy(p->partition[i], e + 0x4E0 + 4 * i, 3);
    memcpy(p->mv_joint, e + 0x53B, 3);
    memcpy(p->mv_sign, e + 0x53E, 2);
    memcpy(p->mv_class0_bit, e + 0x540, 2);
    memcpy(p->mv_fr, e + 0x542, 6);
    memcpy(p->mv_class0_hp, e + 0x548, 2);
    memcpy(p->mv_hp, e + 0x54A, 2);
    memcpy(p->mv_class, e + 0x54C, 20);
    memcpy(p->mv_class0_fr, e + 0x560, 12);
    memcpy(p->mv_bits, e + 0x56C, 20);
}

#ifdef SSBU_HAVE_VPX
typedef struct Vp9Job {
    int valid;
    Vp9Frame f;
    Vp9Probs probs;
    u64 refs[3], current;   /* luma surfaces */
    u8 *tiles;
    u32 ntiles, cap;
} Vp9Job;
static Vp9Job pending;

static vpx_codec_ctx_t vpx;
static int vpx_state;                    /* 0 not yet, 1 ready, -1 failed */
static Vp9Slots slots;                   /* what libvpx's reference slots hold */
static Vp9Probs contexts[4];             /* libvpx's frame contexts, mirrored */

/* Decoded pictures, by their luma surface, for VIC. */
typedef struct Picture { u64 luma; u32 w, h, used; u8 *y, *u, *v; } Picture;
static Picture pictures[16];
static u32 picture_clock;

static Picture *picture(u64 luma) {
    for (u32 i = 0; i < 16; i++) if (pictures[i].y && pictures[i].luma == luma) return &pictures[i];
    return NULL;
}

/* A decoded picture kept for VIC under its luma surface: 4:2:0, the chroma planar (cstep 1) or
 * interleaved (cstep 2: v = u + 1, NV12). */
static void store_picture(u64 luma, u32 w, u32 h, const u8 *y, u32 ypitch, const u8 *u, const u8 *v, u32 cpitch, u32 cstep) {
    Picture *p = picture(luma);
    if (!p) {
        p = &pictures[0];
        for (u32 i = 1; i < 16; i++) if (pictures[i].used < p->used) p = &pictures[i];
    }
    u32 cw = (w + 1) / 2, ch = (h + 1) / 2;
    if (!p->y || p->w != w || p->h != h) {
        free(p->y);
        p->y = malloc((size_t)w * h + 2ull * cw * ch);
        if (!p->y) { p->w = p->h = 0; return; }
        p->u = p->y + (size_t)w * h;
        p->v = p->u + (size_t)cw * ch;
    }
    p->luma = luma; p->w = w; p->h = h; p->used = ++picture_clock;
    for (u32 r = 0; r < h; r++) memcpy(p->y + (size_t)r * w, y + (size_t)r * ypitch, w);
    for (u32 r = 0; r < ch; r++) {
        const u8 *ur = u + (size_t)r * cpitch, *vr = v + (size_t)r * cpitch;
        u8 *ud = p->u + (size_t)r * cw, *vd = p->v + (size_t)r * cw;
        if (cstep == 1) { memcpy(ud, ur, cw); memcpy(vd, vr, cw); }
        else for (u32 x = 0; x < cw; x++) { ud[x] = ur[cstep * x]; vd[x] = vr[cstep * x]; }
    }
}

static void keep_picture(u64 luma, const vpx_image_t *img) {
    store_picture(luma, img->d_w, img->d_h, img->planes[0], (u32)img->stride[0], img->planes[1], img->planes[2], (u32)img->stride[1], 1);
}

static void vp9_decode(Vp9Job *j, int show) {
    j->valid = 0;
    if (vpx_state == 0) {
        vpx_codec_dec_cfg_t cfg = {4, 0, 0};
        vpx_state = vpx_codec_dec_init(&vpx, vpx_codec_vp9_dx(), &cfg, 0) == VPX_CODEC_OK ? 1 : -1;
        if (vpx_state < 0) fprintf(stderr, "[nvdec] libvpx would not start\n");
    }
    if (vpx_state < 0) return;
    Vp9Frame f = j->f;
    f.show = (u8)show;
    int slot = vp9_assign_slots(&slots, &f, j->refs, j->current);
    /* probabilities: updates from a mirrored context of libvpx's */
    if (f.key || f.intra_only || f.error_resilient) {
        Vp9Probs def;
        vp9_default_probs(&def);
        for (u32 c = 0; c < 4; c++) contexts[c] = def;
        f.reset_context = 3;
        f.context_idx = 0;
    } else {
        int best = 0;
        for (int c = 1; c < 4; c++) if (vp9_context_score(&contexts[c], &j->probs) > vp9_context_score(&contexts[best], &j->probs)) best = c;
        f.context_idx = (u8)best;
    }
    f.refresh_context = !f.error_resilient;
    f.parallel = 1;   /* no adaptation in libvpx: every frame says what it decodes with */
    static u8 *frame;
    static u32 frame_cap;
    u32 need = j->ntiles + 0x10000;
    if (need > frame_cap) { frame = realloc(frame, need); frame_cap = need; }
    Vp9Probs after;
    u8 compressed[0x8000];
    u32 csize = vp9_write_compressed(&f, &contexts[f.context_idx], &j->probs, &after, compressed, sizeof compressed);
    u32 usize = vp9_write_uncompressed(&f, csize, frame, 256);
    memcpy(frame + usize, compressed, csize);
    memcpy(frame + usize + csize, j->tiles, j->ntiles);
    if (f.refresh_context) contexts[f.context_idx] = after;
    if (vpx_codec_decode(&vpx, frame, usize + csize + j->ntiles, NULL, 0) != VPX_CODEC_OK) {
        static int complaints;
        if (complaints++ < 10)
            fprintf(stderr, "[nvdec] VP9 frame (%s%s, %ux%u, %u bytes) not decoded: %s (%s)\n", f.key ? "key" : "inter", show ? "" : ", hidden",
                    f.width, f.height, j->ntiles, vpx_codec_error(&vpx), vpx_codec_error_detail(&vpx) ? vpx_codec_error_detail(&vpx) : "");
        return;
    }
    vpx_codec_iter_t it = NULL;
    while (vpx_codec_get_frame(&vpx, &it)) {}   /* (shown frames come out here; every frame is taken from its slot below) */
    vp9_ref_frame_t ref;
    memset(&ref, 0, sizeof ref);
    ref.idx = slot;
    if (vpx_codec_control(&vpx, VP9_GET_REFERENCE, &ref) == VPX_CODEC_OK && ref.img.fmt == VPX_IMG_FMT_I420) keep_picture(j->current, &ref.img);
}

/* ---- NVDEC: H.264 ------------------------------------------------------------------ */
/* Methods: 0x80 the codec (3: H.264), 0x101 the picture setup (gpu_h264.h: the parameter sets'
 * fields, and flags with the picture's surface index), 0x102 the bitstream (the slices, Annex B),
 * 0x10C + i / 0x11D + i surface i's luma / chroma. Seen in Sephiroth's Final Smash (a 1280x720
 * High profile movie with B pictures), which was black before.
 *
 * Each job's surface is kept under a tag, and a picture goes to the surface of the job it came
 * from: the decoder gives each picture as its job is decoded (gpu_h264.c: the SPS asks for no
 * reordering), as the engine does, and the game asks VIC for them in display order itself,
 * several jobs later. A decoder that held pictures back would still put them in the right
 * surfaces, only later: VIC says so when it is asked for one too early. */
static struct { u64 tag, luma; u32 w, h; } h264_jobs[64];
static u64 h264_tag;

static void h264_picture(void *user, u64 tag, u32 w, u32 h, const u8 *y, const u8 *uv, u32 pitch) {
    (void)user;
    for (u32 i = 0; i < 64; i++)
        if (h264_jobs[i].luma && h264_jobs[i].tag == tag) {
            u32 pw = w < h264_jobs[i].w ? w : h264_jobs[i].w, ph = h < h264_jobs[i].h ? h : h264_jobs[i].h;   /* (the decoder's frame is aligned) */
            store_picture(h264_jobs[i].luma, pw, ph, y, pitch, uv, uv + 1, pitch, 2);
            h264_jobs[i].luma = 0;
            return;
        }
}

static void h264_execute(Engine *e) {
    static int off;   /* 1: no decoder here */
    if (off) return;
    u64 left, bits_left;
    u64 pg = to_guest((u64)e->regs[0x101] << 8, &left);
    if (!pg || left < H264_SETUP_SIZE) return;
    u64 flags = ld64(pg + 0xB0);
    u32 idx = h264_surface_index(flags), size = ld32(pg + H264_SETUP_STREAM_LEN);
    u64 bg = to_guest((u64)e->regs[0x102] << 8, &bits_left);
    if (!bg || size > bits_left || idx > 16) return;
    size = h264_strip_end_units(P(u8, bg), size);
    static u8 *unit;
    static u32 cap;
    u32 head = 0, need = size + 256;   /* (room for the parameter sets) */
    if (need > cap) {
        u8 *grown = realloc(unit, need);
        if (!grown) return;
        unit = grown;
        cap = need;
    }
    static int skipping, fed;   /* skipping: a sequence whose headers could not be written, up to its end */
    if (h264_starts_idr(P(u8, bg), size)) {   /* a new sequence (or a new movie): the parameter sets first */
        /* The decoder holds back the last pictures of what came before (a movie that ended: the
         * game appends end units to every frame, so nothing says so sooner): out with them first. */
        if (fed) h264_drain(h264_picture, NULL);
        fed = 0;
        const char *why = NULL;
        head = h264_write_headers(P(u8, pg), unit, cap, &why);
        skipping = !head;
        if (!head) {
            static int complaints;
            if (complaints++ < 10)
                fprintf(stderr, "[nvdec] H.264: a stream with %s, which is not written here: it stays black\n", why ? why : "a setup too large");
        }
    }
    if (skipping) return;
    memcpy(unit + head, P(u8, bg), size);
    u32 slot = (u32)(h264_tag % 64);
    h264_jobs[slot].tag = h264_tag;
    h264_jobs[slot].luma = (u64)e->regs[0x10C + idx] << 8;
    h264_jobs[slot].w = ld32(pg + 0x64) * 16;
    h264_jobs[slot].h = ld32(pg + 0x68) * 16 * (ld32(pg + 0x60) & 1 ? 1 : 2);
    int r = h264_decode(unit, head + size, h264_tag++, h264_picture, NULL);
    fed = 1;
    if (r < 0) off = 1;
    else if (!r) {
        static int complaints;
        if (complaints++ < 10) fprintf(stderr, "[nvdec] H.264 frame (%u bytes) not decoded\n", size);
    }
}

static void nvdec_execute(Engine *e) {
    if (e->regs[0x80] == 3) {
        h264_execute(e);
        return;
    }
    if (e->regs[0x80] != 9) {
        static int said;
        if (!said++) fprintf(stderr, "[nvdec] codec %u is not decoded (only VP9 and H.264)\n", e->regs[0x80]);
        return;
    }
    u64 left, entropy_left;
    u64 pg = to_guest((u64)e->regs[0x101] << 8, &left), eg = to_guest((u64)e->regs[0x170] << 8, &entropy_left);
    if (!pg || left < 0x100 || !eg || entropy_left < 0xEA0) return;
    const u8 *pic = P(u8, pg);
    u32 flags = ld32(pg + PIC_FLAGS);
    if (pending.valid) vp9_decode(&pending, (flags & F_LAST_SHOWN) != 0);
    Vp9Job *j = &pending;
    Vp9Frame *f = &j->f;
    memset(f, 0, sizeof *f);
    f->bit_depth = 8;
    f->key = flags & F_KEY;
    f->intra_only = (flags & F_INTRA_ONLY) != 0;
    f->error_resilient = (flags & F_ERROR_RESILIENT) != 0;
    f->width = (u16)ld16(pg + PIC_SIZE);
    f->height = (u16)ld16(pg + PIC_SIZE + 2);
    for (u32 i = 0; i < 3; i++) f->sign_bias[i] = pic[PIC_SIGN_BIAS + 1 + i] & 1;
    f->allow_hp = pic[PIC_ALLOW_HP] & 1;
    f->interp_filter = pic[PIC_INTERP_FILTER] <= VP9_SWITCHABLE ? pic[PIC_INTERP_FILTER] : VP9_SWITCHABLE;
    f->reference_mode = pic[PIC_REFERENCE_MODE] <= 2 ? pic[PIC_REFERENCE_MODE] : 0;
    f->tx_mode = pic[PIC_TX_MODE] <= VP9_TX_MODE_SELECT ? pic[PIC_TX_MODE] : VP9_ALLOW_32X32;
    f->lf_level = pic[PIC_LF_LEVEL] & 63;
    f->lf_sharpness = pic[PIC_LF_SHARPNESS] & 7;
    f->lf_delta_enabled = pic[PIC_LF_DELTAS] & 1;
    f->lf_delta_update = f->lf_delta_enabled;   /* all of them, every frame */
    for (u32 i = 0; i < 4; i++) { f->lf_ref_update[i] = 1; f->lf_ref[i] = (i8)pic[PIC_LF_DELTAS + 1 + i]; }
    for (u32 i = 0; i < 2; i++) { f->lf_mode_update[i] = 1; f->lf_mode[i] = (i8)pic[PIC_LF_DELTAS + 5 + i]; }
    f->base_q = pic[PIC_BASE_Q];
    f->dq_y_dc = (i8)pic[PIC_DQ_Y_DC];
    f->dq_uv_ac = (i8)pic[PIC_DQ_UV_AC];
    f->dq_uv_dc = (i8)pic[PIC_DQ_UV_DC];
    f->tile_cols_log2 = pic[PIC_TILE_COLS];
    f->tile_rows_log2 = pic[PIC_TILE_ROWS];
    f->seg_enabled = pic[PIC_SEG] & 1;
    if (f->seg_enabled) {
        f->seg_update_map = pic[PIC_SEG + 1] & 1;
        f->seg_temporal = pic[PIC_SEG + 2] & 1;
        f->seg_abs = pic[PIC_SEG + 3] & 1;
        f->seg_update_data = 1;
        memset(f->seg_tree, 255, sizeof f->seg_tree);   /* (not in the setup) */
        memset(f->seg_pred, 255, sizeof f->seg_pred);
        for (u32 i = 0; i < 8; i++)
            for (u32 k = 0; k < 4; k++) {
                f->seg_on[i][k] = ld32(pg + PIC_SEG + 4 + 4 * i) >> k & 1;
                f->seg_data[i][k] = (i16)ld16(pg + PIC_SEG + 0x24 + 8 * i + 2 * k);
            }
    }
    probs_from_engine(P(u8, eg), &j->probs);
    static int checked;
    if (f->key && !checked++) {   /* the probabilities' layout: a key frame cannot change the inter ones from their defaults */
        Vp9Probs def;
        vp9_default_probs(&def);
        int same = !memcmp(def.inter_mode, j->probs.inter_mode, sizeof def.inter_mode) && !memcmp(def.partition, j->probs.partition, sizeof def.partition) &&
                   !memcmp(def.mv_joint, j->probs.mv_joint, sizeof def - offsetof(Vp9Probs, mv_joint));
        fprintf(stderr, "[nvdec] VP9 %ux%u; probability layout %s\n", f->width, f->height, same ? "as expected" : "NOT as expected");
    }
    for (u32 i = 0; i < 3; i++) j->refs[i] = (u64)e->regs[0x10C + i] << 8;
    j->current = (u64)e->regs[0x10F] << 8;
    u32 size = ld32(pg + PIC_BITSTREAM_SIZE);
    u64 bg = to_guest((u64)e->regs[0x102] << 8, &left);
    if (!bg || size > left) return;
    if (size > j->cap) { j->tiles = realloc(j->tiles, size); j->cap = size; }
    memcpy(j->tiles, P(void, bg), size);
    j->ntiles = size;
    j->valid = 1;
}
#endif

/* ---- VIC: the decoded picture, converted into the output surface -------------------- */
/* Methods: 0x100 + 3 * surface the input surfaces' luma, U, V (surface 0: the picture),
 * 0x1C2 the configuration (at +0x20: output format, layout, size), 0x1C8/0x1C9 the output's
 * luma (or RGB) and chroma. */
static void vic_execute(Engine *e) {
#ifdef SSBU_HAVE_VPX
    u64 in = (u64)e->regs[0x100] << 8;
    if (pending.valid && pending.current == in) vp9_decode(&pending, 1);   /* shown, then */
    for (u32 i = 0; i < 64; i++)
        if (h264_jobs[i].luma == in) {   /* an H.264 picture the decoder still holds back: this shows what the surface had */
            static u32 early;
            if (++early <= 10) fprintf(stderr, "[vic] H.264: the picture of job %llu asked for before the decoder gave it (%u so far)\n",
                                       (unsigned long long)h264_jobs[i].tag, early);
            break;
        }
    Picture *p = picture(in);
    u64 cg = to_guest(((u64)e->regs[0x1C2] << 8) + 0x20, NULL);
    if (!p || !cg) return;
    u64 cfg = ld64(cg);
    u32 format = cfg & 0x7F, kind = (cfg >> 11) & 0xF, w = (u32)((cfg >> 32) & 0x3FFF) + 1, h = (u32)((cfg >> 46) & 0x3FFF) + 1;
    if (kind) {
        static int said;
        if (!said++) fprintf(stderr, "[vic] block-linear output (kind %u) is not written\n", kind);
        return;
    }
    if (w > p->w) w = p->w;
    if (h > p->h) h = p->h;
    u64 left_y, left_c;
    u64 yg = to_guest((u64)e->regs[0x1C8] << 8, &left_y), uvg = to_guest((u64)e->regs[0x1C9] << 8, &left_c);
    u32 cw = (p->w + 1) / 2;
    if (format == 0x44) {   /* YUV 4:2:0, chroma interleaved (NV12), rows 256-byte aligned */
        u32 pitch = (w + 255) & ~255u;
        if (!yg || !uvg || (u64)pitch * h > left_y || (u64)pitch * (h / 2) > left_c) return;
        for (u32 r = 0; r < h; r++) memcpy(P(u8, yg + (u64)r * pitch), p->y + (size_t)r * p->w, w);
        for (u32 r = 0; r < h / 2; r++) {
            u8 *d = P(u8, uvg + (u64)r * pitch);
            const u8 *u = p->u + (size_t)r * cw, *v = p->v + (size_t)r * cw;
            for (u32 x = 0; x < w / 2; x++) { d[2 * x] = u[x]; d[2 * x + 1] = v[x]; }
        }
    } else if (format == 0x1F || format == 0x20 || format == 0x23) {   /* RGBA, BGRA, RGBX: BT.601, studio range */
        u32 pitch = (w * 4 + 255) & ~255u;
        if (!yg || (u64)pitch * h > left_y) return;
        for (u32 r = 0; r < h; r++) {
            u8 *d = P(u8, yg + (u64)r * pitch);
            for (u32 x = 0; x < w; x++) {
                int Y = (p->y[(size_t)r * p->w + x] - 16) * 298, U = p->u[(size_t)(r / 2) * cw + x / 2] - 128, V = p->v[(size_t)(r / 2) * cw + x / 2] - 128;
                int R = (Y + 409 * V + 128) >> 8, G = (Y - 100 * U - 208 * V + 128) >> 8, B = (Y + 516 * U + 128) >> 8;
                R = R < 0 ? 0 : R > 255 ? 255 : R; G = G < 0 ? 0 : G > 255 ? 255 : G; B = B < 0 ? 0 : B > 255 ? 255 : B;
                d[4 * x] = (u8)(format == 0x20 ? B : R); d[4 * x + 1] = (u8)G; d[4 * x + 2] = (u8)(format == 0x20 ? R : B); d[4 * x + 3] = 255;
            }
        }
    } else {
        static int said;
        if (!said++) fprintf(stderr, "[vic] output format %#x is not written\n", format);
    }
#else
    (void)e;
#endif
}

static void method(Engine *e, u32 m, u32 arg) {
    if (m < METHODS) e->regs[m] = arg;
    if (m != 0xC0) return;   /* Execute */
    record(e);
#ifdef SSBU_HAVE_VPX
    if (e->cls == CLASS_NVDEC) nvdec_execute(e);
#endif
    if (e->cls == CLASS_VIC) vic_execute(e);
}

/* ---- command buffers -------------------------------------------------------------- */
void host1x_run(u32 cls, const u32 *w, u32 n) {
    Engine *e = engine_of(cls);
    u32 offset = 0, count = 0, mask = 0, incrementing = 0;
    for (u32 i = 0; i < n; i++) {
        u32 v = w[i];
        u32 reg;
        if (mask) {   /* a word per set bit, lowest first */
            u32 bit = 0;
            while (!(mask >> bit & 1)) bit++;
            mask &= ~(1u << bit);
            reg = offset + bit;
        } else if (count) {
            count--;
            reg = offset;
            if (incrementing) offset++;
        } else {
            u32 mode = v >> 28;
            offset = (v >> 16) & 0xFFF;
            switch (mode) {
            case 0: cls = (v >> 6) & 0x3FF; e = engine_of(cls); mask = v & 0x3F; break;   /* set class */
            case 1: case 2: count = v & 0xFFFF; incrementing = mode == 1; break;
            case 3: mask = v & 0xFFFF; break;
            case 4: if (e && offset == THI_METHOD0) e->method0 = v & 0xFFFF;               /* immediate */
                    else if (e && offset == THI_METHOD1) method(e, e->method0, v & 0xFFFF);
                    break;
            default: break;   /* restart, gather, ...: not seen from this driver */
            }
            continue;
        }
        if (!e) continue;   /* host1x's own class: sync point increments and waits, done at submit */
        if (reg == THI_METHOD0) e->method0 = v;
        else if (reg == THI_METHOD1) method(e, e->method0, v);
    }
}

/* A Submit's command buffer: {nvmap handle, byte offset, word count}. */
void host1x_submit(u32 cls, u32 handle, u32 offset, u32 words) {
    if (handle >= NVMAP_MAX || !gpu_maps[handle].addr || (u64)offset + 4ull * words > gpu_maps[handle].size) return;
    host1x_run(cls, P(u32, gpu_maps[handle].addr + offset), words);
}
