/* NVN front end: the NVN entry points the game calls, command recording into host lists, the guest objects, the texture
 * source logic (formats, layouts, which memory the game wrote) and the render thread's job queue. Nothing here calls
 * OpenGL or Vulkan: the jobs go to the back end chosen at start (g_rb, nvn_front.h; rb_gl in nvn_gl.c, rb_vk in
 * nvn_vk_draw.c). Moved out of nvn_gl.c without renames (plan\phases\V1_vulkan.md, seam step S3). What the OpenGL back
 * end still reads of it directly, and the calls it makes into that back end besides rb_gl, are in nvn_front_gl.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "runtime.h"
#include "hostrt.h"
#include "mx_translate.h"
#include "gfx_options.h"
#include "nvn_front.h"
#include "nvn_front_gl.h"
#include "tex_decode.h"
#include "nvn_sdk.h"
#include "nvn_c4.h"
static void *volatile g_window_sync;                                  /* the sync object of the last nvnWindowAcquireTexture */
/* V1 (plan\phases\V1_vulkan.md, seam step S0): SSBU_RENDERER=vk. The render thread then starts nvn_vk.c instead of an
 * OpenGL context and its jobs go through vk_route (end of this file); nothing else in this file changes behaviour. */
void nvn_vk_present_black(void);
void nvn_vk_flush(int finish);
void nvn_vk_shutdown(void);
void nvn_vk_missing(const char *what);
static int g_vk;
const RBackend *g_rb = &rb_gl;                 /* seam step S2: chosen once in nvn_gl_start (SSBU_RENDERER) */
static void vk_shutdown_at_exit(void);
/* ---------------------------------------------------------------- the render thread */
typedef struct Job { void (*fn)(void *); void *arg; HANDLE done; struct Job *next; } Job;
static CRITICAL_SECTION g_job_lock;
static Job *g_job_head, *g_job_tail;
static HANDLE g_job_event, g_ready_event;
DWORD g_render_tid;
HWND g_hwnd;
static __declspec(thread) HANDLE t_done;
static void (*pf_job_submit)(void *), (*pf_job_present)(void *);
static LONG64 g_joblog_frame;
LONG64 g_joblog_cur;
static void joblog(void (*fn)(void *), LONG64 t);
enum { PF_WAIT_SUBMIT_ = 13, PF_WAIT_PRESENT_ = 14, PF_WAIT_OTHER_ = 15 };

static void gl_run(void (*fn)(void *), void *arg) {
    if (GetCurrentThreadId() == g_render_tid) { fn(arg); return; }
    if (!t_done) t_done = CreateEventA(NULL, FALSE, FALSE, NULL);
    Job j = {fn, arg, t_done, NULL};
    LONG64 pf0 = pf_now();
    if (g_joblog_frame > 0 && g_joblog_cur >= g_joblog_frame && g_joblog_cur < g_joblog_frame + 3) joblog(fn, pf0);
    EnterCriticalSection(&g_job_lock);
    if (g_job_tail) g_job_tail->next = &j; else g_job_head = &j;
    g_job_tail = &j;
    LeaveCriticalSection(&g_job_lock);
    SetEvent(g_job_event);
    hitch_wait_render(1);
    WaitForSingleObject(j.done, INFINITE);
    hitch_wait_render(0);
    pf_add(fn == pf_job_submit ? PF_WAIT_SUBMIT_ : fn == pf_job_present ? PF_WAIT_PRESENT_ : PF_WAIT_OTHER_, pf0);
}

/* P3: nvnQueueSubmitCommands / FenceSync / Flush do not wait for the render thread (SSBU_SYNC_SUBMIT=1: they wait, as before).
 * Measured in a battle: the game's main thread calls, per frame, SyncWait x2, SubmitCommands, FenceSync, Flush, then works
 * for about 8 ms and calls PresentTexture; the replay of the submitted commands takes 6-9 ms, during which the main
 * thread used to stand still. NVN itself executes submitted commands later, on the GPU, and the game waits for its
 * fence (SyncWait) before reusing what the commands refer to, so this is the order of events the game was written for.
 * What keeps the result the same as with a waiting submit:
 *   - the jobs run in the order they were issued (one queue); every other job (PresentTexture, SyncWait, texel
 *     reads and writes, Finish, ...) still waits, and so runs after everything issued before it;
 *   - the handles given to SubmitCommands are copied; the command lists themselves were copied when recorded;
 *   - entry points that change or release a host record a queued command may name (programs, textures, pools, samplers,
 *     windows, queues) first wait until the queue is empty (gl_barrier); FinalizeCommandHandle is queued in order;
 *   - guest memory the commands refer to (vertex / uniform / storage buffers, texture storage) is read when the commands
 *     run, as on the console's GPU: the game must not change it before the fence signals, and that rule is the game's own. */
static volatile LONG g_async_pending;
static int g_sync_submit = -1;
static void job_nop(void *arg) { (void)arg; }
static int gl_async_on(void) {
    if (g_sync_submit < 0) g_sync_submit = getenv("SSBU_SYNC_SUBMIT") != NULL;
    return !g_sync_submit;
}
static void gl_post(void (*fn)(void *), const void *arg, size_t size) {       /* run fn(copy of arg) on the render thread, in order, without waiting */
    if (GetCurrentThreadId() == g_render_tid) { fn((void *)arg); return; }
    Job *j = malloc(sizeof *j + size + 8);
    if (size) memcpy(j + 1, arg, size);
    j->fn = fn; j->arg = j + 1; j->done = NULL; j->next = NULL;
    InterlockedIncrement(&g_async_pending);
    EnterCriticalSection(&g_job_lock);
    if (g_job_tail) g_job_tail->next = j; else g_job_head = j;
    g_job_tail = j;
    LeaveCriticalSection(&g_job_lock);
    SetEvent(g_job_event);
}
static void gl_barrier(void) { if (g_async_pending) gl_run(job_nop, NULL); }   /* everything queued so far has run when this returns */

/* ---------------------------------------------------------------- statistics and diagnostics */
struct NvnStats g_st;                  /* the members: nvn_front_gl.h */
int g_logn;
volatile LONG g_setshaders_calls, g_with_text, g_without_text;
static const char *const k_pf_name[PF_N] = {"submit", "prep", "drawcall", "arena", "texhash", "texupload", "handle", "dispatch", "cflush", "copy", "present", "swap",
                                            "otherjob", "wait-submit", "wait-present", "wait-other", "pace", "shader-translate", "shader-compile"};
volatile LONG64 g_pf[PF_N], g_pfn[PF_N];
LONG64 g_pf_arena_bytes, g_pf_cflush_bytes, g_pf_hash_bytes;
void nvn_pf_pace(LONG64 ticks) { InterlockedExchangeAdd64(&g_pf[PF_PACE], ticks); InterlockedIncrement64(&g_pfn[PF_PACE]); }     /* display.c */

void logf_limited(int *counter, int limit, const char *fmt, ...) {
    if (*counter >= limit) return;
    (*counter)++;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

/* ---------------------------------------------------------------- formats
 * What the front end and every back end need of an NVN texture format: bytes per block, block size, kind. The formats
 * of the APIs are the back ends' (OpenGL: gl_fmt in nvn_gl.c; Vulkan: tex_vkfmt in nvn_vk_draw.c), for the formats listed here. */
static const uint8_t k_size12[12] = {1, 1, 1, 1, 2, 2, 2, 2, 2, 4, 4, 4};
static const uint8_t k_astc[14][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6}, {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};

int fmt_get(int nf, Fmt *f) {
    memset(f, 0, sizeof *f);
    f->bw = f->bh = 1;
#define SET(b, k) (f->bpb = (b), f->kind = (k), 1)
#define BC(b) (f->bw = f->bh = 4, SET(b, 4))
    if ((nf >= 0x01 && nf <= 0x30) || (nf >= 0x6D && nf <= 0x78)) {
        int comps = nf >= 0x6D ? 4 : (nf - 1) / 12 + 1, i = nf >= 0x6D ? nf - 0x6D : (nf - 1) % 12;
        return SET(comps * k_size12[i], 0);
    }
    if (nf >= 0x79 && nf <= 0x94) {
        int i = (nf - 0x79) % 14;
        f->bw = k_astc[i][0]; f->bh = k_astc[i][1];
        return SET(16, 4);
    }
    switch (nf) {
    case 0x31: return SET(1, 2);
    case 0x32: return SET(2, 1);
    case 0x33: return SET(4, 1);
    case 0x34: return SET(4, 1);
    case 0x35: return SET(4, 3);
    case 0x36: return SET(8, 3);
    case 0x37: case 0x38: return SET(4, 0);
    case 0x39: return SET(2, 0);
    case 0x3A: return SET(2, 0);
    case 0x3B: return SET(2, 0);
    case 0x3C: return SET(2, 0);
    case 0x3D: return SET(4, 0);
    case 0x3E: return SET(4, 0);
    case 0x3F: return SET(4, 0);
    case 0x40: return SET(4, 0);
    case 0x41: return BC(8);
    case 0x42: return BC(8);
    case 0x43: return BC(16);
    case 0x44: return BC(16);
    case 0x45: return BC(8);
    case 0x46: return BC(8);
    case 0x47: return BC(16);
    case 0x48: return BC(16);
    case 0x49: return BC(8);
    case 0x4A: return BC(8);
    case 0x4B: return BC(16);
    case 0x4C: return BC(16);
    case 0x4D: return BC(16);
    case 0x4E: return BC(16);
    case 0x4F: return BC(16);
    case 0x50: return BC(16);
    case 0x95: return SET(2, 0);
    case 0x99: case 0x9A: return SET(4, 0);
    case 0x9B: case 0x9C: return SET(4, 0);
    }
    return 0;
#undef SET
#undef BC
}

/* ---------------------------------------------------------------- guest objects
 * Small state objects and builders keep their contents in the guest's own memory (the game copies them
 * around freely); textures, programs, command buffers, pools and windows keep a pointer to a host struct
 * at +0 and a tag at +8. Host structs of textures and programs are never freed (recorded command lists
 * and pool entries may still name them); they are only marked dead. */
static void *hobj(uint64_t g, uint32_t kind) { return g && rd32(g + 8) == kind ? P(rd64(g)) : NULL; }
static void *hnew(uint64_t g, uint32_t kind, size_t size) {
    void *p = calloc(1, size);
    wr64(g, (uint64_t)(uintptr_t)p);
    wr32(g + 8, kind);
    return p;
}

/* SamplerParams, View, TexBuilder, TexObj, EView, VNode, Tex, TexPoolEnt, TexPool, SamplerPool, Stage, Prog, Cmd, CmdBuf, Window:
 * nvn_front.h (seam step S1) */

SRWLOCK g_pool_lock = SRWLOCK_INIT;
static Tex *volatile g_all_tex;            /* every texture, newest first (never unlinked: host records of textures are not freed) */
/* PR #30 (v1Ta44): flush notifications query only live texture ranges. History remains intact for renderer references and
 * prewarming. Sorted starts plus prefix maxima include overlapping/aliased textures without scanning every historical record. */
typedef struct { Tex *tex; uint64_t begin, end, prefix_end; } TexRange;
static SRWLOCK g_tex_range_lock = SRWLOCK_INIT;
static TexRange *g_tex_ranges;
static size_t g_tex_range_count, g_tex_range_cap;
static int g_tex_range_fallback;
static uint64_t tex_range_end(uint64_t begin, uint64_t size) { return size > UINT64_MAX - begin ? UINT64_MAX : begin + size; }
static size_t tex_range_lower(uint64_t addr) {
    size_t lo = 0, hi = g_tex_range_count;
    while (lo < hi) { size_t mid = lo + (hi - lo) / 2; if (g_tex_ranges[mid].begin < addr) lo = mid + 1; else hi = mid; }
    return lo;
}
static void tex_range_prefix(size_t first) {
    uint64_t end = first ? g_tex_ranges[first - 1].prefix_end : 0;
    for (size_t i = first; i < g_tex_range_count; i++) {
        if (g_tex_ranges[i].end > end) end = g_tex_ranges[i].end;
        g_tex_ranges[i].prefix_end = end;
    }
}
static void tex_range_add(Tex *t) {
    if (!t->mem || !t->storage) return;
    AcquireSRWLockExclusive(&g_tex_range_lock);
    if (g_tex_range_fallback) { ReleaseSRWLockExclusive(&g_tex_range_lock); return; }
    if (g_tex_range_count == g_tex_range_cap) {
        size_t cap = g_tex_range_cap ? g_tex_range_cap * 2 : 256;
        TexRange *r = realloc(g_tex_ranges, cap * sizeof *r);
        if (!r) {                         /* never drop invalidations because the optional index could not grow */
            free(g_tex_ranges); g_tex_ranges = NULL; g_tex_range_count = g_tex_range_cap = 0; g_tex_range_fallback = 1;
            ReleaseSRWLockExclusive(&g_tex_range_lock);
            printf("nvn_gl: the live-texture range index could not grow: flush notifications walk every texture again\n");
            return;
        }
        g_tex_ranges = r; g_tex_range_cap = cap;
    }
    uint64_t begin = (uint64_t)(uintptr_t)t->mem;
    size_t i = tex_range_lower(begin);
    memmove(g_tex_ranges + i + 1, g_tex_ranges + i, (g_tex_range_count - i) * sizeof *g_tex_ranges);
    g_tex_ranges[i].tex = t; g_tex_ranges[i].begin = begin; g_tex_ranges[i].end = tex_range_end(begin, t->storage);
    g_tex_range_count++; tex_range_prefix(i);
    ReleaseSRWLockExclusive(&g_tex_range_lock);
}
static void tex_range_remove(Tex *t) {
    if (!t->mem || !t->storage) return;
    AcquireSRWLockExclusive(&g_tex_range_lock);
    uint64_t begin = (uint64_t)(uintptr_t)t->mem;
    for (size_t i = tex_range_lower(begin); i < g_tex_range_count && g_tex_ranges[i].begin == begin; i++) {
        if (g_tex_ranges[i].tex != t) continue;
        g_tex_range_count--;
        memmove(g_tex_ranges + i, g_tex_ranges + i + 1, (g_tex_range_count - i) * sizeof *g_tex_ranges);
        tex_range_prefix(i);
        break;
    }
    if (!g_tex_range_count) { free(g_tex_ranges); g_tex_ranges = NULL; g_tex_range_cap = 0; }
    ReleaseSRWLockExclusive(&g_tex_range_lock);
}
int g_origin_mode = 1, g_depth_mode = 1;        /* what the game asks for; applied by the render thread */

/* ---------------------------------------------------------------- Tegra block-linear layout
 * Rules, storage sizes and de-swizzling come from tex_decode.c (tex_layout / tex_deswizzle_level), which was
 * checked against the game's packaged textures; in particular 3D textures use block height 1 and a block depth. */
void tex_lay(const TexBuilder *b, TexLayout *L) {
    tex_layout(L, b->target, b->format, b->width, b->height, b->depth, b->levels, b->flags & ~0x50, 0, b->has_layout && b->layout[0] <= 4 ? b->layout[0] : -1);
}
int tex_layers(const TexBuilder *b) {
    switch (b->target) {
    case 3: case 4: case 6: case 9: return b->depth > 0 ? b->depth : 1;     /* arrays; a cube array counts faces */
    case 8: return 6;
    }
    return 1;
}
int lvl(int v, int l) { v >>= l; return v > 0 ? v : 1; }
/* Size of the whole texture in pool memory (reserved) and of the part that holds texels (for bounds checks). */
static size_t tex_storage_size2(const TexBuilder *b, size_t *data_size) {
    Fmt f;
    TexLayout L;
    size_t s;
    if (!fmt_get(b->format, &f)) { if (data_size) *data_size = 0x1000; return 0x1000; }
    if (b->flags & 0x10) {
        s = (size_t)(b->stride ? b->stride : (int64_t)b->width * f.bpb) * (size_t)(b->height > 0 ? b->height : 1);
        if (data_size) *data_size = s;
        return s;
    }
    tex_lay(b, &L);
    s = L.size;
    if (data_size) *data_size = L.data_size;
    if (b->samples > 1) { s *= (size_t)b->samples; if (data_size) *data_size *= (size_t)b->samples; }
    return s;
}
static size_t tex_storage_size(const TexBuilder *b) { return tex_storage_size2(b, NULL); }

uint32_t g_serial = 1;              /* one per submitted batch: texture memory is re-checked once per batch */
/* PR #41: this renderer consumes packaged texels once, at the deferred first upload. Capture its source before publishing
 * the texture so that later hashing and upload do not depend on the caller keeping the original storage mapped. */
static int tex_capture_packaged(Tex *t) {
    if (!t->b.packaged || !t->data_size) return 1;
    t->packaged_copy = malloc(t->data_size);
    if (!t->packaged_copy) return 0;
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), P(t->b.packaged), t->packaged_copy, t->data_size, &got) || got != t->data_size) {
        free(t->packaged_copy); t->packaged_copy = NULL; return 0;
    }
    return 1;
}
const uint8_t *tex_source(const Tex *t) {
    return t->packaged_copy ? t->packaged_copy : t->b.packaged ? (const uint8_t *)P(t->b.packaged) : t->mem;
}
void tex_release_packaged(Tex *t) {
    free(t->packaged_copy); t->packaged_copy = NULL;
}

uint64_t mem_hash(const uint8_t *p, size_t n, int full) {
    uint64_t h = 0x9E3779B97F4A7C15ULL ^ n, w;
    size_t words = n / 8, step = full || words <= 8192 ? 1 : words / 4096;
    for (size_t i = 0; i < words; i += step) { memcpy(&w, p + i * 8, 8); h = (h ^ w) * 0x100000001B3ULL; h ^= h >> 29; }
    return h;
}

/* ---- which texture memory did the game write? (P3)
 * A texture's texels are guest memory, and NVN has no call that says "I changed them", so every texture bound in a batch
 * used to be hashed in every batch (35 MB per frame in a battle; large block-linear ones were only sampled, so a small
 * change could be missed). The game's heap is reserved with MEM_WRITE_WATCH (nnsdk.c, nn::os::AllocateAddressRegion):
 * GetWriteWatch names the pages written since the previous question. Rule:
 *   - no page of the texture's range written since this texture was last looked at -> the GL copy is current, nothing to do;
 *   - otherwise the whole range is hashed (every word) and uploaded if the hash changed, as before.
 * Asking resets the answer for those pages, and two textures can share a page (small textures) or the whole range (two
 * NVN textures on the same storage), so every written page gets a stamp in a table and a texture compares the newest stamp
 * of its pages with the stamp it saw last. Memory that is not write-watched (the image's own data, host heap) is hashed
 * as before. SSBU_NO_WRITEWATCH=1 restores the old behaviour everywhere. Render thread only. */
#define WW_SLOTS (1u << 20)
static struct { uint64_t page; uint64_t stamp; } *g_ww_tab;
static uint64_t g_ww_stamp, g_ww_used, g_ww_floor;       /* g_ww_floor: stamp given to every page when the table had to be emptied */
static int g_ww_off = -1;
LONG64 g_ww_clean, g_ww_dirty, g_ww_unwatched;
static uint64_t *ww_slot(uint64_t page, int insert) {
    for (unsigned h = (unsigned)((page * 0x9E3779B97F4A7C15ULL) >> 44);; h = (h + 1) & (WW_SLOTS - 1)) {
        if (g_ww_tab[h].page == page) return &g_ww_tab[h].stamp;
        if (!g_ww_tab[h].page) {
            if (!insert) return NULL;
            g_ww_tab[h].page = page; g_ww_used++;
            return &g_ww_tab[h].stamp;
        }
    }
}
/* Newest write stamp of the pages of [p, p+n), or 0 when the memory is not write-watched. Never returns less than 1 otherwise. */
uint64_t ww_newest(const uint8_t *p, size_t n) {
    static void *pages[16384];
    ULONG_PTR count = 16384;
    DWORD gran = 0;
    if (g_ww_off < 0) { g_ww_off = getenv("SSBU_NO_WRITEWATCH") != NULL; g_ww_tab = calloc(WW_SLOTS, sizeof *g_ww_tab); g_ww_stamp = 1; }
    if (g_ww_off || !n || GetWriteWatch(WRITE_WATCH_FLAG_RESET, (void *)p, n, pages, &count, &gran)) return 0;
    uint64_t first = (uint64_t)(uintptr_t)p >> 12, last = ((uint64_t)(uintptr_t)p + n - 1) >> 12, newest = g_ww_floor ? g_ww_floor : 1;
    if (g_ww_used > WW_SLOTS / 2) { memset(g_ww_tab, 0, WW_SLOTS * sizeof *g_ww_tab); g_ww_used = 0; g_ww_floor = ++g_ww_stamp; newest = g_ww_floor; }      /* forget: everything counts as written once */
    if (count) {
        uint64_t stamp = ++g_ww_stamp;
        if (count == 16384) {                      /* more written pages than the list holds: stamp the whole range */
            ResetWriteWatch((void *)p, n);
            for (uint64_t pg = first; pg <= last; pg++) *ww_slot(pg, 1) = stamp;
        } else for (ULONG_PTR i = 0; i < count; i++) *ww_slot((uint64_t)(uintptr_t)pages[i] >> 12, 1) = stamp;
    }
    for (uint64_t pg = first; pg <= last; pg++) { uint64_t *s = ww_slot(pg, 0); if (s && *s > newest) newest = *s; }
    return newest;
}
/* Round 16 (nvn_vk_draw.c, SSBU_VK_CBKEEP): the same question for any guest range: has the CPU written a page of it since
 * the stamp the caller remembers? 0 = the memory is not write-watched. Render thread only. */
uint64_t nvn_front_mem_newest(const void *p, size_t n) { return ww_newest(p, n); }

/* ---------------------------------------------------------------- deferred deletion (objects finalized on guest threads) */
CRITICAL_SECTION g_dead_lock;
struct NvnDead *g_dead;
int g_dead_n;
static int g_dead_cap;
static void defer_free(int kind, void *p) {
    EnterCriticalSection(&g_dead_lock);
    if (g_dead_n == g_dead_cap) { g_dead_cap = g_dead_cap ? g_dead_cap * 2 : 256; g_dead = realloc(g_dead, sizeof *g_dead * (size_t)g_dead_cap); }
    g_dead[g_dead_n].kind = kind; g_dead[g_dead_n++].p = p;
    LeaveCriticalSection(&g_dead_lock);
}

/* ---------------------------------------------------------------- command lists */
/* the OP_* list: nvn_front.h */
const char *const k_op_name[OP_COUNT] = {
    "nop", "SetTexturePool", "SetSamplerPool", "BindProgram", "BindUniformBuffer", "BindStorageBuffer", "BindTexture", "BindVertexBuffer", "BindVertexAttribState",
    "BindVertexStreamState", "BindBlendState", "BindColorState", "BindChannelMaskState", "BindMultisampleState", "BindPolygonState", "BindDepthStencilState",
    "SetViewports", "SetScissors", "SetDepthRanges", "SetRenderTargets", "SetStencilRef", "SetStencilValueMask", "SetStencilMask", "SetBlendColor", "SetAlphaRef",
    "SetPointSize", "SetLineWidth", "SetPolygonOffsetClamp", "SetSampleMask", "SetRasterizerDiscard", "SetDepthClamp", "SetPrimitiveRestart", "SetPatchSize",
    "ClearColor", "ClearDepthStencil", "DrawArrays", "DrawElements", "CopyBufferToTexture", "CopyTextureToTexture", "Downsample", "CopyBufferToBuffer",
    "UpdateUniformBuffer", "ClearBuffer", "CallCommands", "SetCopyRowStride", "CopyTextureToBuffer", "DispatchCompute", "BindImage", "SetCopyImageStride", "SetDepthBounds", "SetConservativeRasterEnable", "SetConservativeRasterDilate",
    "SetSubpixelPrecisionBias", "SetViewportSwizzles", "SetTessellationLevels", "Barrier", "Discard", "ClearTexture", "DownsampleTextureView", "DrawIndirect",
    "DispatchComputeIndirect", "SetRenderEnable", "ResetCounter", "ReportCounter", "ReportValue", "FenceSync", "WaitSync", "SignalEvent", "WaitEvent",
    "BindCoverageModulationTable", "DebugGroup", "DrawTexture"};

typedef struct { uint8_t *data; size_t len; uint32_t gen, model_cmd, model_ctrl, dispatches; int finalized; LONG64 created, used; } List;
static List *g_lists;
static int g_list_count, g_list_cap, *g_list_free, g_list_free_n;
static SRWLOCK g_list_lock = SRWLOCK_INIT;

/* Track R1 round 5 (run-ahead, r1_state.c): every command buffer's host record is listed, so that a restore of the game's
 * state can put the recordings in progress back as they were at the save (the commands recorded so far are host data:
 * without this a speculative pass's commands would stay in the buffer and be submitted again by the real pass, or the
 * commands recorded before the save would be gone after the speculative pass ended the buffer). */
static CmdBuf *g_all_cb[512]; static int g_all_cb_n; static SRWLOCK g_all_cb_lock = SRWLOCK_INIT;
static CmdBuf *cb_new(uint64_t g) {
    CmdBuf *cb = hnew(g, K_CMD, sizeof(CmdBuf));
    AcquireSRWLockExclusive(&g_all_cb_lock);
    int i; for (i = 0; i < g_all_cb_n && g_all_cb[i]; i++) {}
    if (i < 512) { g_all_cb[i] = cb; if (i == g_all_cb_n) g_all_cb_n++; }
    ReleaseSRWLockExclusive(&g_all_cb_lock);
    return cb;
}
static void cb_forget(CmdBuf *cb) { AcquireSRWLockExclusive(&g_all_cb_lock); for (int i = 0; i < g_all_cb_n; i++) if (g_all_cb[i] == cb) g_all_cb[i] = NULL; ReleaseSRWLockExclusive(&g_all_cb_lock); }
typedef struct { CmdBuf *cb, copy; uint8_t *data; size_t cap; } CbSave;
static CbSave g_cbs[512]; static int g_ncbs;
/* (R3 pub1) r1_ring.c: the save area's addresses, without a linker map (the prebuilt runtime package has none) */
void nvn_gl_r1_cb_area(void **cbs, void **ncbs, void **all) { *cbs = g_cbs; *ncbs = &g_ncbs; *all = g_all_cb; }
size_t nvn_gl_r1_cb_size(void) { return sizeof(CbSave); }      /* (R3 pub1) the entry size r1_ring.c expects (RG_CBS_SIZE) */
size_t nvn_r1_cb_save(void) {                      /* the guest threads are held */
    size_t bytes = 0;
    AcquireSRWLockShared(&g_all_cb_lock);
    g_ncbs = g_all_cb_n;
    for (int i = 0; i < g_ncbs; i++) {
        CbSave *s = &g_cbs[i]; CmdBuf *cb = g_all_cb[i];
        s->cb = cb;
        if (!cb) continue;
        s->copy = *cb;
        if (cb->len) { if (s->cap < cb->len) { s->cap = cb->len + 65536; s->data = realloc(s->data, s->cap); } memcpy(s->data, cb->data, cb->len); bytes += cb->len; }
    }
    ReleaseSRWLockShared(&g_all_cb_lock);
    return bytes;
}
void nvn_r1_cb_restore(void) {
    AcquireSRWLockShared(&g_all_cb_lock);
    for (int i = 0; i < g_ncbs; i++) {
        CbSave *s = &g_cbs[i]; CmdBuf *cb = s->cb;
        if (!cb || g_all_cb[i] != cb) continue;           /* finalized meanwhile */
        uint8_t *d = cb->data; size_t cap = cb->cap;       /* (the saved record's buffer may belong to a list now: never reused) */
        *cb = s->copy;
        if (s->copy.len && cap < s->copy.len) { cap = s->copy.len + 65536; d = realloc(d, cap); }
        if (s->copy.len) memcpy(d, s->data, s->copy.len);
        cb->data = d; cb->cap = cap;
    }
    ReleaseSRWLockShared(&g_all_cb_lock);
}
static CmdBuf *cb_get(uint64_t g) {
    CmdBuf *cb = hobj(g, K_CMD);
    return cb ? cb : cb_new(g);
}
/* Appends a command with x1..x7 as its arguments and room for `blob` more bytes; returns it (valid until the next append). */
static Cmd *rec(Ctx *c, uint32_t op, size_t blob) {
    CmdBuf *cb = cb_get(A0);
    size_t need = sizeof(Cmd) + ((blob + 7) & ~(size_t)7);
    if (cb->len + need > cb->cap) { cb->cap = (cb->cap + need) * 2 + 4096; cb->data = realloc(cb->data, cb->cap); }
    {   /* Command memory as the game sees it. The real encoding is the driver's own; the model is 8 bytes per command plus its
         * array payload (control memory: 32 bytes per recording, charged by EndRecording). When the memory the game added is
         * used up, its callback is asked for more, as NVN does (NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY = 0). */
        uint64_t use = 8 + ((blob + 7) & ~(size_t)7);
        if (cb->cmd_size && cb->cmd_used + use > cb->cmd_size && cb->callback && !cb->callback_off && !cb->in_callback) {
            uint64_t sx[19];
            V128 sv[8];
            memcpy(sx, c->x, sizeof sx); memcpy(sv, c->v, sizeof sv);
            cb->in_callback = 1;
            c->x[1] = 0; c->x[2] = (use + 0xff) & ~0xffull; c->x[3] = cb->callback_data;
            call_guest(c, cb->callback);
            cb->in_callback = 0;
            memcpy(c->x, sx, sizeof sx); memcpy(c->v, sv, sizeof sv);
            if (cb->len + need > cb->cap) { cb->cap = (cb->cap + need) * 2 + 4096; cb->data = realloc(cb->data, cb->cap); }
        }
        cb->cmd_used += use; cb->rec_cmd += use;
    }
    Cmd *m = (Cmd *)(cb->data + cb->len);
    cb->len += need;
    m->op = op; m->size = (uint32_t)need;
    for (int i = 0; i < 7; i++) m->a[i] = c->x[i + 1];
    return m;
}

static uint64_t list_register(uint8_t *data, size_t len, uint32_t model_cmd, uint32_t model_ctrl, uint32_t dispatches) {
    AcquireSRWLockExclusive(&g_list_lock);
    int i;
    if (g_list_free_n) i = g_list_free[--g_list_free_n];
    else {
        if (g_list_count == g_list_cap) {
            g_list_cap = g_list_cap ? g_list_cap * 2 : 1024;
            g_lists = realloc(g_lists, sizeof *g_lists * (size_t)g_list_cap);
            g_list_free = realloc(g_list_free, sizeof *g_list_free * (size_t)g_list_cap);
            memset(g_lists + g_list_count, 0, sizeof *g_lists * (size_t)(g_list_cap - g_list_count));
        }
        i = g_list_count++;
    }
    g_lists[i].data = data; g_lists[i].len = len; g_lists[i].created = g_st.frames; g_lists[i].used = -1;
    g_lists[i].model_cmd = model_cmd; g_lists[i].model_ctrl = model_ctrl; g_lists[i].dispatches = dispatches; g_lists[i].finalized = 0;
    uint64_t h = ((uint64_t)(g_lists[i].gen & 0x7fffffff) << 32) | (uint32_t)(i + 1);
    ReleaseSRWLockExclusive(&g_list_lock);
    InterlockedIncrement64(&g_st.lists);
    return h;
}
uint8_t *list_lookup(uint64_t h, size_t *len) {
    uint8_t *data = NULL;
    uint32_t i = (uint32_t)h - 1;
    AcquireSRWLockShared(&g_list_lock);
    if (h && i < (uint32_t)g_list_count && g_lists[i].data && (g_lists[i].gen & 0x7fffffff) == (uint32_t)(h >> 32)) { data = g_lists[i].data; *len = g_lists[i].len; g_lists[i].used = g_st.frames; }
    ReleaseSRWLockShared(&g_list_lock);
    return data;
}
/* Render thread, once per frame: drop lists not submitted for a while (NVN has no call that frees a command handle). */
void list_collect(void) {
    AcquireSRWLockExclusive(&g_list_lock);
    for (int i = 0; i < g_list_count; i++) {
        List *l = &g_lists[i];
        if (!l->data) continue;
        LONG64 last = l->used >= 0 ? l->used : l->created;
        if (l->finalized || g_st.frames - last > 120) { free(l->data); l->data = NULL; l->gen++; g_list_free[g_list_free_n++] = i; }      /* finalized by the game, or (a handle the game never finalized) unused for two seconds */
    }
    ReleaseSRWLockExclusive(&g_list_lock);
}

LONG64 g_prev_frame_draws, g_frame_dispatch;            /* written by both back ends / by the OpenGL compute path */
typedef struct { uint64_t cmd_size, compute_size, control_size, flush_threshold; int flags, priority; volatile LONG64 cmd_used, control_used, compute_used; } QueueObj;
/* Round 17, TEST ONLY (nvn_dt.h): SSBU_DRAWTAG=1 with SSBU_SHOT_FIGHT: the submissions the game queues between the pad poll
 * that reached a wanted fight frame and the present that is saved are "tagged": g_draw_tag = that fight frame while the
 * render thread replays them (set by a job queued before the submission, cleared by the back end at the present). */
int g_draw_tag, g_dt_idx;                 /* render thread */
volatile LONG g_drawtag_next;                    /* game thread: the fight frame the next submission belongs to, 0 = not tagged */
static int drawtag_env(void) { static int on = -1; if (on < 0) on = getenv("SSBU_DRAWTAG") != NULL; return on; }
static void job_drawtag(void *arg) { int n = *(int *)arg; if (g_draw_tag != n) { g_draw_tag = n; g_dt_idx = 0; printf("DT begin: the commands of fight frame %d (%s)\n", n, g_vk ? "Vulkan" : "OpenGL"); } }
int nvn_front_draw_tag(void) { return g_draw_tag; }
int nvn_front_draw_tag_next(void) { return g_dt_idx++; }
void nvn_front_draw_tag_clear(void) { if (g_draw_tag) { printf("DT end: fight frame %d, %d draw commands\n", g_draw_tag, g_dt_idx); g_draw_tag = 0; } }

/* Frame times (present to present) of the last 300 frames and CPU time per thread, printed with the frame-rate line. */
#include <tlhelp32.h>
#ifndef _WIN32
#include <dirent.h>
#endif
static double g_pf_ft[300];
static struct { DWORD tid; ULONGLONG cpu; } g_pf_thr[512];
static int g_pf_nthr;
static DWORD g_pf_main_tid;
const char *guest_thread_name_of(DWORD tid);                             /* os_threads.c */
static int pf_cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
#ifdef _WIN32
static void pf_threads(double frames) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    DWORD pid = GetCurrentProcessId();
    struct { DWORD tid; double ms; } top[512];
    int n = 0;
    double total = 0;
    if (snap == INVALID_HANDLE_VALUE) return;
    te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE h = OpenThread(0x0800 /* THREAD_QUERY_LIMITED_INFORMATION */, FALSE, te.th32ThreadID);
        FILETIME c, e, k, u;
        if (!h) continue;
        if (GetThreadTimes(h, &c, &e, &k, &u)) {
            ULONGLONG cpu = (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime), prev = 0;
            int i;
            for (i = 0; i < g_pf_nthr && g_pf_thr[i].tid != te.th32ThreadID; i++) {}
            if (i < g_pf_nthr) prev = g_pf_thr[i].cpu; else if (g_pf_nthr < 512) { g_pf_thr[g_pf_nthr].tid = te.th32ThreadID; i = g_pf_nthr++; }
            if (i < 512) g_pf_thr[i].cpu = cpu;
            if (n < 512) { top[n].tid = te.th32ThreadID; top[n].ms = (double)(cpu - prev) / 10000.0 / frames; total += top[n].ms; n++; }
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    printf("nvn_perf: cpu ms/frame by thread (all %.1f):", total);
    for (int k = 0; k < 10; k++) {
        int best = -1;
        for (int i = 0; i < n; i++) if (top[i].ms > 0.05 && (best < 0 || top[i].ms > top[best].ms)) best = i;
        if (best < 0) break;
        const char *name = top[best].tid == g_render_tid ? "render(host)" : top[best].tid == g_pf_main_tid ? "main" : guest_thread_name_of(top[best].tid);
        printf(" %s[%lu] %.1f", name ? name : "host", (unsigned long)top[best].tid, top[best].ms);
        top[best].ms = 0;
    }
    printf("\n");
}
#else
static void pf_threads(double frames) {                 /* Linux build: /proc/self/task/<tid>/stat, utime + stime */
    struct { DWORD tid; double ms; } top[512];
    int n = 0;
    double total = 0;
    long hz = sysconf(_SC_CLK_TCK);
    DIR *d = opendir("/proc/self/task");
    struct dirent *de;
    if (!d) return;
    while ((de = readdir(d))) {
        DWORD tid = (DWORD)strtoul(de->d_name, NULL, 10);
        char sp[96], buf[512];
        if (!tid) continue;
        snprintf(sp, sizeof sp, "/proc/self/task/%lu/stat", (unsigned long)tid);
        FILE *f = fopen(sp, "r");
        if (!f) continue;
        size_t k = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[k] = 0;
        const char *rp = strrchr(buf, ')');
        unsigned long long ut = 0, st = 0;
        if (!rp || sscanf(rp + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) != 2) continue;
        ULONGLONG cpu = (ut + st) * 10000000ull / (ULONGLONG)(hz > 0 ? hz : 100), prev = 0;
        int i;
        for (i = 0; i < g_pf_nthr && g_pf_thr[i].tid != tid; i++) {}
        if (i < g_pf_nthr) prev = g_pf_thr[i].cpu; else if (g_pf_nthr < 512) { g_pf_thr[g_pf_nthr].tid = tid; i = g_pf_nthr++; }
        if (i < 512) g_pf_thr[i].cpu = cpu;
        if (n < 512) { top[n].tid = tid; top[n].ms = (double)(cpu - prev) / 10000.0 / frames; total += top[n].ms; n++; }
    }
    closedir(d);
    printf("nvn_perf: cpu ms/frame by thread (all %.1f):", total);
    for (int k = 0; k < 10; k++) {
        int best = -1;
        for (int i = 0; i < n; i++) if (top[i].ms > 0 && (best < 0 || top[i].ms > top[best].ms)) best = i;
        if (best < 0 || top[best].ms < 0.05) break;
        const char *g = guest_thread_name_of(top[best].tid);
        printf(" %s %.1f", g ? g : top[best].tid == g_render_tid ? "render" : "host", top[best].ms);
        top[best].ms = 0;
    }
    printf("\n");
}
#endif
static DWORD WINAPI pf_threads_thread(LPVOID arg) { for (;;) { WaitForSingleObject((HANDLE)arg, INFINITE); pf_threads(300.0); } }
void pf_frame_mark(LONG64 frame) {
    static LONG64 last, freq, snap[PF_N], snapn[PF_N], b0[3];
    LONG64 now = pf_now();
    if (!freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart; }
    if (last) g_pf_ft[frame % 300] = (double)(now - last) * 1000.0 / (double)freq;
    if (last && g_hitch_ms) hitch_frame((long long)frame, (double)(now - last) * 1000.0 / (double)freq, g_frame_dispatch > 0);
    last = now;
    if (frame % 300) return;
    double ft[300], sum = 0;
    memcpy(ft, g_pf_ft, sizeof ft);
    qsort(ft, 300, sizeof ft[0], pf_cmp_d);
    for (int i = 0; i < 300; i++) sum += ft[i];
    printf("nvn_perf: frames %lld..%lld frame time ms: avg %.2f min %.2f median %.2f p99 %.2f max %.2f | fps avg %.1f, 1%% low %.1f\n", (long long)frame - 300, (long long)frame,
           sum / 300, ft[0], ft[150], ft[296], ft[299], sum > 0 ? 300000.0 / sum : 0.0, ft[296] > 0 ? 1000.0 / ft[296] : 0.0);
    printf("nvn_perf: ms/frame (calls/frame):");
    for (int k = 0; k < PF_N; k++) {
        LONG64 t = g_pf[k], c = g_pfn[k];
        printf(" %s %.2f (%.1f)", k_pf_name[k], (double)(t - snap[k]) * 1000.0 / (double)freq / 300.0, (double)(c - snapn[k]) / 300.0);
        snap[k] = t; snapn[k] = c;
    }
    printf(" | MB/frame: streamed %.2f, read back %.2f, hashed %.2f\n", (double)(g_pf_arena_bytes - b0[0]) / 300.0 / 1048576.0, (double)(g_pf_cflush_bytes - b0[1]) / 300.0 / 1048576.0,
           (double)(g_pf_hash_bytes - b0[2]) / 300.0 / 1048576.0);
    b0[0] = g_pf_arena_bytes; b0[1] = g_pf_cflush_bytes; b0[2] = g_pf_hash_bytes;
    rb_gl.frame_stats();                        /* SSBU_GLSTATS=1 (nvn_gl.c): with either renderer, as before */
    if (g_rb != &rb_gl && g_rb->frame_stats) g_rb->frame_stats();
    {   /* P3 round 2: the snapshot of all threads (CreateToolhelp32Snapshot) took 30-70 ms on the render thread: one long
         * frame in every 300 (hitch log, "present" scope). It is taken by a helper thread now. */
        static HANDLE ev;
        if (!ev) { ev = CreateEventA(NULL, FALSE, FALSE, NULL); CloseHandle(CreateThread(NULL, 0, pf_threads_thread, ev, 0, NULL)); }
        SetEvent(ev);
    }
}
static void joblog(void (*fn)(void *), LONG64 t) {
    static LONG64 freq;
    if (!freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart; }
    printf("joblog: frame %lld t=%.3f ms thread %lu %s%c", (long long)g_joblog_cur, (double)(t % (freq * 100)) * 1000.0 / (double)freq, (unsigned long)GetCurrentThreadId(),
           fn == rb_gl.submit ? "SubmitCommands" : fn == rb_gl.present ? "PresentTexture" : fn == rb_gl.fence ? "FenceSync" : fn == rb_gl.sync_wait ? "SyncWait" : fn == rb_gl.flush ? "Flush" :
           fn == rb_gl.finish ? "Finish" : "other", 10);
}

/* The render thread's job loop (both back ends). */
static void render_loop(void) {
    for (;;) {
        WaitForSingleObject(g_job_event, INFINITE);
        for (;;) {
            EnterCriticalSection(&g_job_lock);
            Job *j = g_job_head;
            if (j) { g_job_head = j->next; if (!g_job_head) g_job_tail = NULL; }
            LeaveCriticalSection(&g_job_lock);
            if (!j) break;
            HANDLE done = j->done;
            if (g_rb->idle) g_rb->idle();
            g_hitch_rt = j->fn == g_rb->submit || j->fn == g_rb->submit_async ? "submitted commands (outside the named scopes)" : j->fn == g_rb->present ? "present (blit)" : j->fn == g_rb->sync_wait ? "SyncWait job" : "other job (texel read/write, finish, barrier, ...)";
            j->fn(j->arg);                       /* a member of g_rb (seam step S2), or a job of the front end / of the back end itself */
            g_hitch_rt = "idle";
            if (done) SetEvent(done); else { free(j); InterlockedDecrement(&g_async_pending); }      /* gl_post: nobody waits */
        }
    }
}

static DWORD WINAPI render_thread(LPVOID arg) {
    (void)arg;
    pf_job_submit = g_rb->submit; pf_job_present = g_rb->present;
    g_joblog_frame = getenv("SSBU_JOBLOG") ? atoi(getenv("SSBU_JOBLOG")) : 0;
    g_render_tid = GetCurrentThreadId();
    {   /* Round 17, MEASUREMENT: SSBU_RENDER_PRIO=1 above normal, 2 highest (default: normal, as before). The game's own
         * threads run above normal when the game asked for a priority under 16 (os_threads.c host_priority). */
        const char *e = getenv("SSBU_RENDER_PRIO");
        int p = e ? atoi(e) : 0;
        if (p > 0) { SetThreadPriority(GetCurrentThread(), p >= 2 ? THREAD_PRIORITY_HIGHEST : THREAD_PRIORITY_ABOVE_NORMAL); printf("nvn: render thread priority %s (SSBU_RENDER_PRIO)\n", p >= 2 ? "highest" : "above normal"); }
    }
    g_rb->init();                               /* OpenGL: the context, the shader cache, the self-test hook (nvn_gl.c); Vulkan: the device (nvn_vk.c) */
    SetEvent(g_ready_event);
    render_loop();
    return 0;
}

/* Called by display.c once the window exists. */
void nvn_gl_start(void *hwnd) {
    g_hwnd = hwnd;
    {   /* V1: which back end. SSBU_RENDERER=gl (default) | vk */
        const char *e = getenv("SSBU_RENDERER");
        if (e && e[0] && strcmp(e, "gl") && strcmp(e, "vk")) { printf("nvn: SSBU_RENDERER=%s is not a renderer (gl, vk)\n", e); fflush(stdout); ExitProcess(5); }
        /* Track V session 4: VULKAN IS THE DEFAULT. SSBU_RENDERER=gl|vk wins; else renderer = vulkan | opengl of config\graphics.ini. */
        g_vk = e && e[0] ? !strcmp(e, "vk") : !gfx_options()->renderer;
#ifndef _WIN32
        if (!g_vk) { printf("nvn: the OpenGL renderer is not part of the Linux build: Vulkan instead\n"); g_vk = 1; }
#endif
        g_rb = g_vk ? &rb_vk : &rb_gl;
        printf("nvn: renderer: %s (%s)\n", g_vk ? "Vulkan (nvn_vk.c)" : "OpenGL (nvn_gl.c)", e && e[0] ? "SSBU_RENDERER" : "graphics.ini; the default is Vulkan");
    }
    InitializeCriticalSection(&g_job_lock);
    InitializeCriticalSection(&g_dead_lock);
    g_job_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_ready_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    { const char *e = getenv("SSBU_STACKS_AT"); void nvn_vk_stacks_at(int seconds); if (e && atoi(e) > 0) nvn_vk_stacks_at(atoi(e)); }      /* session 13, TEST: every thread's call stack once, that many seconds from here, with either renderer (nvn_vk_wd.c) */
    CreateThread(NULL, 16 << 20, render_thread, NULL, 0, NULL);
    WaitForSingleObject(g_ready_event, INFINITE);
}

void nvn_gl_report(FILE *out) {
    rb_gl.report(out);                          /* the nvn_gl: lines with either renderer, as before: the first ones are the front end's counters */
    hitch_report(out);
    if (g_vk) { g_rb->report(out); vk_shutdown_at_exit(); }
}

/* ================================================================ the NVN entry points */
static void n_nop(Ctx *c) { (void)c; }
static void n_true(Ctx *c) { RET(1); }
static void n_zero(Ctx *c) { RET(0); }

/* ---- device */
static void dev_origin_mode(Ctx *c) { g_origin_mode = (int)A1; }
static void dev_depth_mode(Ctx *c) { g_depth_mode = (int)A1; }
/* The real values (close5; until then this port gave textureID | samplerID << 32 and the bare ID for the other three):
 *   nvnDeviceGetTextureHandle [sdk 0x41bc80]         sign-extended 32-bit (textureID | samplerID << 20), then bit 32 set
 *   nvnDeviceGetTexelFetchHandle [0x41bc70]          sign-extended textureID, bit 32 set
 *   nvnDeviceGetImageHandle [0x41bc00]               sign-extended textureID */
static void dev_texture_handle(Ctx *c) { RET((uint64_t)(int64_t)(int32_t)((uint32_t)A1 | (uint32_t)A2 << 20) | 0x100000000ull); }   /* (device, textureID, samplerID) */
static void dev_texel_fetch_handle(Ctx *c) { RET((uint64_t)(int64_t)(int32_t)(uint32_t)A1 | 0x100000000ull); }
static void dev_image_handle(Ctx *c) { RET((uint64_t)(int64_t)(int32_t)(uint32_t)A1); }

/* ---- programs */
static volatile LONG g_prog_serial;
static void prog_initialize(Ctx *c) {
    Prog *p = hnew(A0, K_PROG, sizeof(Prog));
    p->serial = (int)InterlockedIncrement(&g_prog_serial);
    p->guest = A0;
    RET(1);
}
static void prog_finalize(Ctx *c) {
    gl_barrier();
    Prog *p = hobj(A0, K_PROG);
    if (!p) return;
    InterlockedExchange(&p->dead, 1);
    wr32(A0 + 8, 0);
    nvn_label_drop(A0);
    defer_free(K_PROG, p);
}
/* NV assembly text of a stage (nvnProgramSetShaders): program.buffer[n] and program.storage[n] uses per stage, for the
 * exit report of the OpenGL back end (rb_gl_report), and the stage names. */
LONG g_bufhist[6][32], g_storhist[32];
const char *const k_stage_name[6] = {"vertex", "fragment", "geometry", "tess-control", "tess-eval", "compute"};
static int stage_of(const char *text, int len) {
    static const char *const tag[6] = {"!!NVvp", "!!NVfp", "!!NVgp", "!!NVtcp", "!!NVtep", "!!NVcp"};
    for (int i = 0; i < 6; i++) if (len > 8 && !memcmp(text, tag[i], strlen(tag[i]))) return i;
    return -1;
}
/* Keeps a copy of the program text with program.buffer[n] renumbered to slots 0,1,2.. in order of appearance.
 * Runs with either renderer: the stage is then marked as given with text (state 1), which the caller relies on. */
static void prog_add_stage(Prog *p, const char *text, int len, int serial) {
    static int nlog;
    len = (int)strnlen(text, (size_t)len);
    int s = stage_of(text, len);
    if (s < 0) { logf_limited(&nlog, 10, "nvn_gl: program #%d: shader text of unknown kind '%.12s'\n", serial, text); return; }
    Stage *st = &p->st[s];
    char *out = malloc((size_t)len + 64);
    int o = 0;
    st->nbuf = 0; st->storage = 0;
    for (int i = 0; i < len;) {
        if (len - i > 16 && !memcmp(text + i, "program.buffer[", 15)) {
            int n = 0, slot;
            i += 15;
            while (i < len && text[i] >= '0' && text[i] <= '9') n = n * 10 + (text[i++] - '0');
            for (slot = 0; slot < st->nbuf && st->map[slot] != n; slot++) {}
            if (slot == st->nbuf && st->nbuf < 16) st->map[st->nbuf++] = (uint8_t)n;
            o += sprintf(out + o, "program.buffer[%d", slot);
            InterlockedIncrement(&g_bufhist[s][n & 31]);
            continue;
        }
        if (len - i > 17 && !memcmp(text + i, "program.storage[", 16)) {
            int n = atoi(text + i + 16);
            st->storage |= 1u << (n & 31);
            InterlockedIncrement(&g_storhist[n & 31]);
        }
        out[o++] = text[i++];
    }
    out[o] = 0;
    st->storage_w = 0;
    {   /* storage buffers the program writes: STB / ATOMB / CSWAPB lines name "sbo_bufK", declared as STORAGE sbo_bufK[] = { program.storage[n] } */
        int8_t decl[32];
        memset(decl, -1, sizeof decl);
        for (const char *q = out; (q = strstr(q, "STORAGE sbo_buf")) != NULL; q += 15) {
            int k = atoi(q + 15);
            const char *r = strstr(q, "program.storage[");
            if (r && k >= 0 && k < 32) decl[k] = (int8_t)(atoi(r + 16) & 31);
        }
        for (const char *q = out; q && *q; q = strchr(q, 10), q = q ? q + 1 : NULL) {
            if (strncmp(q, "STB", 3) && strncmp(q, "ATOM", 4) && strncmp(q, "CSWAP", 5)) continue;
            const char *eol = strchr(q, 10), *r = strstr(q, "sbo_buf");
            if (r && (!eol || r < eol)) { int k = atoi(r + 7); if (k >= 0 && k < 32 && decl[k] >= 0) st->storage_w |= 1u << decl[k]; }
        }
    }
    free(st->text);
    st->text = out; st->len = o; st->state = 1;
    const char *dir = getenv("SSBU_SHADERDUMP");
    if (dir) {
        char path[260];
        snprintf(path, sizeof path, "%s/p%05d_%s.txt", dir, serial, k_stage_name[s]);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(text, 1, (size_t)len, f); fclose(f); }
    }
}
/* Debug aid SSBU_SHADERDUMP_ALL=<dir>: every shader stage handed to nvnProgramSetShaders, whole, as <dir>/s<program>_<i>.mxd for the
 * offline harness (mxtest.exe). File: "MXD1", u32 program serial, u32 index in the call, u32 control bytes, u32 data bytes, then the
 * control section (at least up to +0x720, plus the assembly text if any) and the code section (header + code + constants). */
static void dump_stage_all(int serial, int index, uint64_t control, uint64_t data) {
    static const char *dir;
    static volatile LONG init;
    if (!init) { dir = getenv("SSBU_SHADERDUMP_ALL"); InterlockedExchange(&init, 1); }
    if (!dir || !control) return;
    uint32_t text_off = rd32(control + 0x14), text_len = rd32(control + 0x18), ctl_len = 0x720, data_len = 0;
    if (text_len && text_len < (1u << 20) && text_off < (1u << 20) && text_off + text_len > ctl_len) ctl_len = text_off + text_len;
    { uint32_t tail = rd32(control + 0x20); if (tail && tail < (1u << 20) && tail + 0x400 > ctl_len) ctl_len = tail + 0x400; }      /* the block after the text */
    if (data) {
        uint32_t code_len = rd32(control + 0x6f8), c_size = rd32(control + 0x6fc), c_off = rd32(control + 0x700), total = rd32(control + 0x704);
        if (code_len < (4u << 20)) data_len = 0x30 + code_len;
        if (c_off < (4u << 20) && c_size < (1u << 20) && c_off + c_size > data_len) data_len = c_off + c_size;
        if (total > data_len && total < (8u << 20)) data_len = total;
    }
    char path[300];
    snprintf(path, sizeof path, "%s/s%05d_%d.mxd", dir, serial, index);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t head[5] = {0x3144584du, (uint32_t)serial, (uint32_t)index, ctl_len, data_len};
    fwrite(head, 4, 5, f);
    fwrite(P(control), 1, ctl_len, f);
    if (data_len) fwrite(P(data), 1, data_len, f);
    fclose(f);
}

/* nvnMemoryPoolInitialize (nvn_stub.c), guest thread. The game lays a pool over the code section of each "BNSH" shader
 * container when it sets the container up (loading), long before it makes the first program of it: the Vulkan back end
 * scans the container then (nvn_vk_m4.c). */
void nvn_front_pool_made(uint64_t memory, uint64_t size) { if (g_rb == &rb_vk && memory && size >= 0x40 && (memory >> 28) != 0x710 && rd32(memory) == 0x12345678u) nvn_vk_container_seen(memory, memory); }      /* such a pool begins with the code section of a stage (its magic); a search below every other pool cost 0.2 s each (measured) */
/* nvnProgramSetShaders(program, count, const NVNshaderData *): { u64 data (GPU address), const void *control }.
 * control: magic 0x98761234, offset of the NV_gpu_program5 text at +0x14, its length at +0x18. */
__declspec(thread) long long nvn_setsh_part[8];
static void prog_set_shaders_(Ctx *c);
static void prog_set_shaders(Ctx *c) {             /* session 8: the call timed part by part; a line for every call over 2 ms (SSBU_VK_SETSH_LOG=<ms> changes the limit, 0 = no lines) */
    static int lim = -1; static LONG nl; static LONG64 fq;
    if (lim < 0) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); fq = f.QuadPart; lim = getenv("SSBU_VK_SETSH_LOG") ? atoi(getenv("SSBU_VK_SETSH_LOG")) : 2; }
    if (!lim || g_rb != &rb_vk) { prog_set_shaders_(c); return; }
    memset(nvn_setsh_part, 0, sizeof nvn_setsh_part);
    uint64_t guest = A0, ra = c->x[30];
    LONG64 t0 = pf_now();
    prog_set_shaders_(c);
    LONG64 t = pf_now() - t0;
    if (t * 1000 > (LONG64)lim * fq && (InterlockedIncrement(&nl) <= 400 || (t * 1000 > 20 * fq && nl <= 4000))) {      /* session 10: a call over 20 ms is logged after the first 400 lines too (Intel: the lines were used up at loading) */
        long long *q = nvn_setsh_part;
#define SETSH_MS(x) ((double)(x) * 1000.0 / (double)fq)
        printf("nvn_setsh: frame %lld, program object %llx (called from %llx): %.1f ms | barrier %.1f | container %.1f (search %.1f, copy or walk %.1f) | program_set %.1f (hash + job copy %.1f, lock %.1f, HELD %.1f) | rest %.1f\n",
               nvn_front_frame_next(), (unsigned long long)guest, (unsigned long long)ra, SETSH_MS(t), SETSH_MS(q[0]), SETSH_MS(q[1]), SETSH_MS(q[2]), SETSH_MS(q[3]), SETSH_MS(q[4]), SETSH_MS(q[5]), SETSH_MS(q[6]), SETSH_MS(q[7]), SETSH_MS(t - q[0] - q[1] - q[4]));
        fflush(stdout);
    }
}
static void prog_set_shaders_(Ctx *c) {
    static int nlog, nlog2;
    { LONG64 tb = pf_now(); gl_barrier(); nvn_setsh_part[0] += pf_now() - tb; }
    Prog *p = hobj(A0, K_PROG);
    int count = (int)A1;
    uint64_t stride = strstr(hostfn_current_name(), "Ext") ? 24 : 16;      /* the Ext variant has a third pointer per shader */
    InterlockedIncrement(&g_setshaders_calls);
    if (!p) { p = hnew(A0, K_PROG, sizeof(Prog)); p->serial = (int)InterlockedIncrement(&g_prog_serial); p->guest = A0; }
    for (int s = 0; s < 6; s++) if (p->st[s].state == 1 || p->st[s].state == 4) { free(p->st[s].text); p->st[s].text = NULL; p->st[s].state = 0; }
    for (int i = 0; i < count && i < 6 && A2; i++) {
        uint64_t control = rd64(A2 + (uint64_t)i * stride + 8);
        if (!control || rd32(control) != 0x98761234u) { logf_limited(&nlog, 10, "nvn_gl: %s: shader %d of program #%d has no recognisable control section\n", hostfn_current_name(), i, p->serial); continue; }
        uint64_t data = rd64(A2 + (uint64_t)i * stride);
        dump_stage_all(p->serial, i, control, data);
        /* V1 session 6: a program made by FUN_71000382a0 (13.0.4; the caller's return address lies in it) comes from a variation
         * of a "BNSH" container; that function keeps its third argument, the variation's program inside the file, in x20 */
        if (g_rb == &rb_vk && i == 0 && c->x[30] - GA(0x71000382a0ull) < 2884) { LONG64 tb = pf_now(); nvn_vk_container_seen(control, c->x[20]); nvn_setsh_part[1] += pf_now() - tb; }
        {   static int diag = -1; static LONG nd;      /* SSBU_SHADER_SRC_LOG=<n>: where the first n heap-held stages come from (guest return addresses, the memory around the control section) */
            if (diag < 0) diag = getenv("SSBU_SHADER_SRC_LOG") ? atoi(getenv("SSBU_SHADER_SRC_LOG")) : 0;
            if (diag && (control >> 28) != 0x710 && InterlockedIncrement(&nd) <= diag) {
                uint64_t fp = c->x[29], ra1 = fp ? rd64(fp + 8) : 0, fp2 = fp ? rd64(fp) : 0, ra2 = fp2 ? rd64(fp2 + 8) : 0, fp3 = fp2 ? rd64(fp2) : 0, ra3 = fp3 ? rd64(fp3 + 8) : 0, pg = control & ~0xffffull;
                printf("shader-src: frame %lld program #%d stage %d of %d: control %llx data %llx (third pointer %llx) | called from %llx <- %llx <- %llx <- %llx | 64K base: %08x %08x %08x %08x %08x %08x %08x %08x | before control: %016llx %016llx %016llx %016llx | data: %08x %08x %08x %08x\n", nvn_front_frame_next(), p->serial, i, count,
                       (unsigned long long)control, (unsigned long long)data, (unsigned long long)(stride == 24 ? rd64(A2 + (uint64_t)i * stride + 16) : 0), (unsigned long long)c->x[30], (unsigned long long)ra1, (unsigned long long)ra2, (unsigned long long)ra3,
                       rd32(pg), rd32(pg + 4), rd32(pg + 8), rd32(pg + 12), rd32(pg + 16), rd32(pg + 20), rd32(pg + 24), rd32(pg + 28),
                       (unsigned long long)rd64(control - 32), (unsigned long long)rd64(control - 24), (unsigned long long)rd64(control - 16), (unsigned long long)rd64(control - 8), data ? rd32(data) : 0, data ? rd32(data + 4) : 0, data ? rd32(data + 8) : 0, data ? rd32(data + 12) : 0);
                fflush(stdout);
            }
        }
        static const int8_t stage_of_type[16] = {-1, 0, 3, 4, 2, 1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
        if (rd32(control + 0x18)) {
            InterlockedIncrement(&g_with_text);
            prog_add_stage(p, P(control + rd32(control + 0x14)), (int)rd32(control + 0x18), p->serial);
            if (data && rd32(data) == 0x12345678u) {                  /* the machine code is there as well */
                int s2 = stage_of_type[(rd32(data + 0x30) >> 10) & 15], len2 = (int)rd32(control + 0x6f8);
                if (s2 >= 0 && s2 < 2 && p->st[s2].state == 1 && len2 > 0 && len2 < (4 << 20)) { p->st[s2].code = P(data + 0x30); p->st[s2].code_len = len2; p->st[s2].glsl_state = 0; memcpy(p->st[s2].ctl, P(control + 0x6f0), 32); }
            }
            /* V1 M3: a compute program with assembly text (the five built into the exe) also carries its machine code; the
             * Vulkan back end runs that (no NV assembly there), OpenGL only with SSBU_COMPUTE_MC=1 (translator check). */
            if (rd32(control + 0x714) == 5 && data && p->st[5].state == 1) {
                static int ncl;
                int len = (int)rd32(control + 0x6f8);
                logf_limited(&ncl, 8, "nvn_gl: program #%d: compute stage with assembly text and %d bytes of machine code: %08x %08x %08x %08x, work group %u x %u x %u\n", p->serial, len,
                             rd32(data), rd32(data + 4), rd32(data + 8), rd32(data + 12), rd32(control + 0x748), rd32(control + 0x74c), rd32(control + 0x750));
                for (int k = 0; k < 3; k++) p->st[5].local[k] = rd32(control + 0x748 + (uint64_t)k * 4);
                p->st[5].smem_bytes = rd32(control + 0x754); p->st[5].lmem_bytes = rd32(control + 0x758);
                if (len > 0 && len < (4 << 20)) { p->st[5].code = P(data); p->st[5].code_len = len; p->st[5].glsl_state = 0; memcpy(p->st[5].ctl, P(control + 0x6f0), 32); }
            }
            continue;
        }
        /* No assembly text: only Maxwell machine code. data: magic 0x12345678, program header (0x50 bytes) at +0x30, then 64-bit
         * instruction words; control +0x6f8 = size of the instructions in bytes. Header word 0 bits 10..13 = stage (1 vertex,
         * 2 tess control, 3 tess evaluation, 4 geometry, 5 fragment). */
        InterlockedIncrement(&g_without_text);
        if (rd32(control + 0x714) == 5 && data) {                     /* compute stage (control +0x714 = NVN shader stage) */
            static int ncl;
            int len = (int)rd32(control + 0x6f8);
            logf_limited(&ncl, 40, "nvn_gl: program #%d: compute stage as machine code, %d bytes, data %llx: %08x %08x %08x %08x | +0x30: %08x %08x %08x %08x\n", p->serial, len, (unsigned long long)data,
                         rd32(data), rd32(data + 4), rd32(data + 8), rd32(data + 12), rd32(data + 0x30), rd32(data + 0x34), rd32(data + 0x38), rd32(data + 0x3c));
            for (int k = 0; k < 3; k++) p->st[5].local[k] = rd32(control + 0x748 + (uint64_t)k * 4);      /* work group size */
            p->st[5].smem_bytes = rd32(control + 0x754); p->st[5].lmem_bytes = rd32(control + 0x758);
            if (len > 0 && len < (4 << 20)) { p->st[5].code = P(data); p->st[5].code_len = len; p->st[5].state = 4; p->st[5].dumped = 0; p->st[5].glsl_state = 0; memcpy(p->st[5].ctl, P(control + 0x6f0), 32); }
            continue;
        }
        if (!data || rd32(data) != 0x12345678u) { logf_limited(&nlog2, 10, "nvn_gl: program #%d shader %d: no text and no recognisable code section\n", p->serial, i); continue; }
        int s = stage_of_type[(rd32(data + 0x30) >> 10) & 15], len = (int)rd32(control + 0x6f8);
        if (s < 0 || len <= 0 || len > (4 << 20)) { logf_limited(&nlog2, 10, "nvn_gl: program #%d shader %d: unexpected program header %08x, code size %d\n", p->serial, i, rd32(data + 0x30), len); continue; }
        p->st[s].code = P(data + 0x30); p->st[s].code_len = len; p->st[s].state = 4; p->st[s].dumped = 0; p->st[s].glsl_state = 0; memcpy(p->st[s].ctl, P(control + 0x6f0), 32);
    }
    { LONG64 tb = pf_now(); g_rb->program_set(p); nvn_setsh_part[4] += pf_now() - tb; }      /* the stages go to the back end's workers at once (V1 M4: nvn_vk_m4.c; OpenGL: nvn_gl.c) */
    RET(1);
}

/* ---- samplers */
static void smpb_defaults(Ctx *c) {
    SamplerParams p;
    memset(&p, 0, sizeof p);
    p.min = 1; p.mag = 1; p.ws = p.wt = p.wr = 7; p.lodmax = 1000.0f; p.aniso = 1.0f; p.cfunc = 2;
    memcpy(P(A0), &p, sizeof p);
}
#define SMP ((SamplerParams *)P(A0))
static void smpb_minmag(Ctx *c) { SMP->min = (uint8_t)A1; SMP->mag = (uint8_t)A2; }
static void smpb_wrap(Ctx *c) { SMP->ws = (uint8_t)A1; SMP->wt = (uint8_t)A2; SMP->wr = (uint8_t)A3; }
static void smpb_lodclamp(Ctx *c) { SMP->lodmin = F0; SMP->lodmax = F1; }
static void smpb_lodbias(Ctx *c) { SMP->bias = F0; }
static void smpb_compare(Ctx *c) { SMP->cmode = (uint8_t)A1; SMP->cfunc = (uint8_t)A2; }
static void smpb_border(Ctx *c) { if (A1) memcpy(SMP->border, P(A1), 16); }
static void smpb_aniso(Ctx *c) { SMP->aniso = F0; }
static void smpb_reduction(Ctx *c) { SMP->reduction = (uint8_t)A1; }
static void smp_initialize(Ctx *c) { memcpy(P(A0), P(A1), sizeof(SamplerParams)); RET(1); }
static void spool_initialize(Ctx *c) {                 /* (pool, memoryPool, offset, numDescriptors) */
    SamplerPool *sp = hnew(A0, K_SPOOL, sizeof *sp);
    sp->count = (int)A3 > 0 ? (int)A3 : 0; sp->mempool = A1; sp->offset = A2;
    sp->e = calloc((size_t)sp->count + 1, sizeof *sp->e);
    sp->used = calloc((size_t)sp->count + 1, 1);
    RET(1);
}
static void spool_register(Ctx *c) {                   /* (pool, id, sampler) or (pool, id, samplerBuilder): same contents */
    gl_barrier();
    SamplerPool *sp = hobj(A0, K_SPOOL);
    if (!sp || (uint32_t)A1 >= (uint32_t)sp->count || !A2) return;
    AcquireSRWLockExclusive(&g_pool_lock);
    memcpy(&sp->e[(uint32_t)A1], P(A2), sizeof(SamplerParams));
    sp->used[(uint32_t)A1] = 1;
    ReleaseSRWLockExclusive(&g_pool_lock);
}

/* ---- textures */
#define TB ((TexBuilder *)P(A0))
static void texb_defaults(Ctx *c) {
    TexBuilder b;
    memset(&b, 0, sizeof b);
    b.target = 1; b.levels = 1;                        /* [sdk 0x427610]: target 1, size 0 x 0 x 0, 1 level, format 0, 0 samples, swizzle 2 3 4 5 */
    b.swz[0] = 2; b.swz[1] = 3; b.swz[2] = 4; b.swz[3] = 5;
    memcpy(P(A0), &b, sizeof b);
}
static void texb_flags(Ctx *c) { TB->flags = (int)A1; }
static void texb_target(Ctx *c) { TB->target = (int)A1; }
static void texb_format(Ctx *c) { TB->format = (int)A1; }
static void texb_width(Ctx *c) { TB->width = (int)A1; }
static void texb_height(Ctx *c) { TB->height = (int)A1; }
static void texb_depth(Ctx *c) { TB->depth = (int)A1; }
static void texb_size1d(Ctx *c) { TB->width = (int)A1; TB->height = 1; TB->depth = 1; }     /* [sdk 0x4276a0] */
static void texb_size2d(Ctx *c) { TB->width = (int)A1; TB->height = (int)A2; TB->depth = 1; }     /* [sdk 0x4276b0] */
static void texb_size3d(Ctx *c) { TB->width = (int)A1; TB->height = (int)A2; TB->depth = (int)A3; }
static void texb_levels(Ctx *c) { TB->levels = (int)A1; }
static void texb_samples(Ctx *c) { TB->samples = (int)A1; }
static void texb_swizzle(Ctx *c) { TB->swz[0] = (uint8_t)A1; TB->swz[1] = (uint8_t)A2; TB->swz[2] = (uint8_t)A3; TB->swz[3] = (uint8_t)A4; }
static void texb_dsmode(Ctx *c) { TB->dsmode = (int)A1; }
static void texb_storage(Ctx *c) { TB->pool = A1; TB->offset = A2; }
static void texb_packaged(Ctx *c) { TB->packaged = A1; }
static void texb_stride(Ctx *c) { TB->stride = (int64_t)A1; }
static void texb_layout(Ctx *c) {
    static int nlog;
    if (!A1) return;
    memcpy(TB->layout, P(A1), 8);
    TB->has_layout = 1;
    Fmt f;
    int hb = fmt_get(TB->format, &f) ? (TB->height + f.bh - 1) / f.bh : TB->height;
    logf_limited(&nlog, 40, "nvn_gl: packaged texture layout %02x %02x %02x %02x %02x %02x %02x %02x for %dx%dx%d format 0x%x target %d levels %d (block height by rule: %d)\n", TB->layout[0], TB->layout[1],
                 TB->layout[2], TB->layout[3], TB->layout[4], TB->layout[5], TB->layout[6], TB->layout[7], TB->width, TB->height, TB->depth, TB->format, TB->target, TB->levels, 1 << tex_rule_block_height_log2(hb));
}
/* Storage size and alignment are the SDK's own code (nvn_sdk.c: [sdk 0x428760] and [sdk 0x4278b0] lifted, with the GPU
 * memory-kind choice they end in). The size this file's own layout needs (tex_storage_size) is only compared with it. */
static void texb_sdk(const TexBuilder *b, NvnSdkTexBuilder *r) {
    memset(r, 0, sizeof *r);
    r->flags = (uint32_t)b->flags; r->target = (uint32_t)b->target; r->width = (uint32_t)b->width; r->height = (uint32_t)b->height; r->depth = (uint32_t)b->depth;
    r->levels = (uint32_t)b->levels; r->format = (uint32_t)b->format; r->samples = (uint32_t)b->samples; r->stride = (uint32_t)b->stride; r->glname = b->glname;
    r->has_layout = (uint32_t)b->has_layout; memcpy(&r->layout, b->layout, 4);
}
static void texb_storage_size(Ctx *c) {
    static int nlog;
    TexBuilder b = *TB;
    NvnSdkTexBuilder r;
    texb_sdk(&b, &r);
    uint64_t real = nvn_sdk_texture_storage_size(c, &r);
    size_t own = nvn_sdk_format_known(r.format) && !b.glname ? tex_storage_size(&b) : 0;
    if (own > real) logf_limited(&nlog, 40, "nvn_gl: texture layout of this file needs 0x%llx bytes, the SDK's storage size is 0x%llx (%dx%dx%d format 0x%x target %d levels %d samples %d flags 0x%x)\n",
                                 (unsigned long long)own, (unsigned long long)real, b.width, b.height, b.depth, b.format, b.target, b.levels, b.samples, b.flags);
    RET(real);
}
static void texb_storage_align(Ctx *c) {
    NvnSdkTexBuilder r;
    texb_sdk(TB, &r);
    RET(nvn_sdk_texture_storage_alignment(c, &r));
}
static void texb_get_flags(Ctx *c) { RET(TB->flags); }
static void texb_get_target(Ctx *c) { RET(TB->target); }
static void texb_get_format(Ctx *c) { RET(TB->format); }
static void texb_get_width(Ctx *c) { RET(TB->width); }
static void texb_get_height(Ctx *c) { RET(TB->height); }
static void texb_get_depth(Ctx *c) { RET(TB->depth); }
static void texb_get_levels(Ctx *c) { RET(TB->levels); }
static void texb_get_samples(Ctx *c) { RET(TB->samples); }

static void tex_initialize(Ctx *c) {                   /* (texture, builder) */
    Tex *t = hnew(A0, K_TEX, sizeof *t);
    uint64_t mem = 0, size = 0;
    memcpy(&t->b, P(A1), sizeof t->b);
    if (t->b.pool && nvn_pool_range(t->b.pool, &mem, &size) && mem) { t->mem = (uint8_t *)P(mem) + t->b.offset; t->limit = (uint8_t *)P(mem) + size; }
    t->storage = tex_storage_size2(&t->b, &t->data_size);
    if (!tex_capture_packaged(t)) {                /* PR #41, changed: no copy = the old behaviour (the game's own storage is read) */
        static int said;
        if (said++ < 5) printf("nvn: packaged texture %dx%d format 0x%x: its source %llx + %llu could not be copied: read in place as before\n",
                               t->b.width, t->b.height, t->b.format, (unsigned long long)t->b.packaged, (unsigned long long)t->data_size);
    }
    t->guest = A0;
    tex_range_add(t);
    do { t->all_next = g_all_tex; } while (InterlockedCompareExchangePointer((void *volatile *)&g_all_tex, t, t->all_next) != t->all_next);
    RET(1);
}
static void tex_finalize(Ctx *c) {
    gl_barrier();
    Tex *t = hobj(A0, K_TEX);
    if (!t) return;
    if (!t->dead) tex_range_remove(t);
    InterlockedExchange(&t->dead, 1);
    wr32(A0 + 8, 0);
    nvn_label_drop(A0);
    defer_free(K_TEX, t);
}
#define TEXGET(name, expr) static void name(Ctx *c) { Tex *t = hobj(A0, K_TEX); RET(t ? (expr) : 0); }
TEXGET(tex_get_flags, t->b.flags)
TEXGET(tex_get_target, t->b.target)
TEXGET(tex_get_format, t->b.format)
TEXGET(tex_get_width, t->b.width)
TEXGET(tex_get_height, t->b.height)
TEXGET(tex_get_depth, t->b.depth)
TEXGET(tex_get_levels, t->b.levels)
TEXGET(tex_get_samples, t->b.samples)
TEXGET(tex_get_stride, t->b.stride)
static void tex_get_storage_size(Ctx *c);       /* below: the SDK's value [sdk 0x429720], not this file's own layout size (t->storage) */
TEXGET(tex_get_memory_offset, t->b.offset)
TEXGET(tex_get_address, (uint64_t)(uintptr_t)t->mem)
TEXGET(tex_get_dsmode, t->b.dsmode)
/* Storage class (raw, and 0x100 | GPU memory kind) and sparse tile layout: the SDK's own code, lifted (nvn_sdk.c: [sdk 0x427a10],
 * [0x427b30], [0x428c00], [0x429870], [0x4290b0], [0x429150]). NVNtextureSparseTileLayout = the 16 bytes [sdk 0x411090] writes.
 * The texture forms run on the NVNtexture that nvnTextureInitialize [sdk 0x4287d0] would have built from this texture's builder. */
static void texb_storage_class(Ctx *c) { NvnSdkTexBuilder r; texb_sdk(TB, &r); RET(nvn_sdk_texture_storage_class(c, &r)); }
static void texb_raw_storage_class(Ctx *c) { NvnSdkTexBuilder r; texb_sdk(TB, &r); RET(nvn_sdk_texture_raw_storage_class(c, &r)); }
static void texb_sparse_layout(Ctx *c) { uint64_t out = A1; uint32_t v[4]; NvnSdkTexBuilder r; texb_sdk(TB, &r); nvn_sdk_texture_sparse_layout(c, &r, v); if (out) memcpy(P(out), v, 16); }
static int tex_sdk(Ctx *c, NvnSdkTexBuilder *r, const char *fn) {
    Tex *t = hobj(A0, K_TEX);
    if (t && !t->b.glname) { texb_sdk(&t->b, r); return 1; }
    printf("nvn: stopping: %s on %s: the SDK object cannot be modelled\n", fn, t ? "a texture made from an OpenGL texture name" : "a texture that was never initialized");
    fflush(stdout);
    report_and_exit(6);
    return 0;
}
static void tex_storage_class(Ctx *c) { NvnSdkTexBuilder r; uint32_t v = 0; if (tex_sdk(c, &r, "nvnTextureGetStorageClass")) nvn_sdk_tex_storage_class(c, &r, &v); RET(v); }
static void tex_raw_storage_class(Ctx *c) { NvnSdkTexBuilder r; uint32_t v = 0; if (tex_sdk(c, &r, "nvnTextureGetRawStorageClass")) nvn_sdk_tex_raw_storage_class(c, &r, &v); RET(v); }
/* nvnTextureGetStorageSize [sdk 0x429720] = the 32-bit word +0x78 of the NVNtexture = the SDK layout's size (nvn_sdk.c). Until close5
 * this answered t->storage, the size of this file's own layout, which is not rounded to the GPU page as the SDK's is. */
static void tex_get_storage_size(Ctx *c) { NvnSdkTexBuilder r; uint32_t v = 0; if (tex_sdk(c, &r, "nvnTextureGetStorageSize")) nvn_sdk_tex_storage_size(c, &r, &v); RET(v); }
static void tex_sparse_layout(Ctx *c) { uint64_t out = A1; uint32_t v[4] = {0, 0, 0, 0}; NvnSdkTexBuilder r; if (tex_sdk(c, &r, "nvnTextureGetSparseTileLayout")) nvn_sdk_tex_sparse_layout(c, &r, v); if (out) memcpy(P(out), v, 16); }

static void tex_write_texels(Ctx *c) {                 /* (texture, view, region, data) [, rowStride, imageStride] */
    Tex *t = hobj(A0, K_TEX);
    WriteJob j;
    if (!t || !A2 || !A3) return;
    memset(&j, 0, sizeof j);
    j.tex = t; j.data = P(A3);
    if (A1) { memcpy(&j.view, P(A1), sizeof j.view); j.has_view = 1; }
    memcpy(j.region, P(A2), 24);
    if (strstr(hostfn_current_name(), "Strided")) j.row_stride = (int64_t)A4;
    gl_run(g_rb->write_texels, &j);
}
static void tex_flush_texels(Ctx *c) { Tex *t = hobj(A0, K_TEX); if (t) t->dirty = 1; }

#define TV ((View *)P(A0))
static void view_defaults(Ctx *c) { memset(P(A0), 0, sizeof(View)); TV->set = 0x80; }
static void view_levels(Ctx *c) { TV->lvl0 = (int16_t)A1; TV->nlvl = (int16_t)A2; TV->set |= 0x81; }
static void view_layers(Ctx *c) { TV->lay0 = (int16_t)A1; TV->nlay = (int16_t)A2; TV->set |= 0x82; }
static void view_format(Ctx *c) { TV->format = (uint16_t)A1; TV->set |= 0x84; }
static void view_swizzle(Ctx *c) { TV->swz[0] = (uint8_t)A1; TV->swz[1] = (uint8_t)A2; TV->swz[2] = (uint8_t)A3; TV->swz[3] = (uint8_t)A4; TV->set |= 0x88; }
static void view_dsmode(Ctx *c) { TV->dsmode = (uint8_t)A1; TV->set |= 0x90; }
static void view_target(Ctx *c) { TV->target = (uint8_t)A1; TV->set |= 0xa0; }

static void tpool_initialize(Ctx *c) {                 /* (pool, memoryPool, offset, numDescriptors) */
    TexPool *tp = hnew(A0, K_TPOOL, sizeof *tp);
    tp->count = (int)A3 > 0 ? (int)A3 : 0; tp->mempool = A1; tp->offset = A2;
    tp->e = calloc((size_t)tp->count + 1, sizeof *tp->e);
    RET(1);
}
static void tpool_register(Ctx *c) {                   /* (pool, id, texture, view) */
    gl_barrier();
    TexPool *tp = hobj(A0, K_TPOOL);
    TexPoolEnt e;
    if (!tp || (uint32_t)A1 >= (uint32_t)tp->count) return;
    memset(&e, 0, sizeof e);
    e.tex = hobj(A2, K_TEX);
    if (A3) memcpy(&e.view, P(A3), sizeof e.view);
    AcquireSRWLockExclusive(&g_pool_lock);
    tp->e[(uint32_t)A1] = e;
    ReleaseSRWLockExclusive(&g_pool_lock);
}

/* ---- small state objects: contents live in the guest object */
static void blend_func_set(Ctx *c) { wr8(A0 + 1, (uint8_t)A1); wr8(A0 + 2, (uint8_t)A2); wr8(A0 + 3, (uint8_t)A3); wr8(A0 + 4, (uint8_t)A4); }
static void blend_equation(Ctx *c) { wr8(A0 + 5, (uint8_t)A1); wr8(A0 + 6, (uint8_t)A2); }
static void color_defaults(Ctx *c) { static const uint8_t d[4] = {0, 3, 8, 0}; memcpy(P(A0), d, 4); }
static void color_blend_enable(Ctx *c) { uint8_t v = rd8(A0), bit = (uint8_t)(1u << (A1 & 7)); wr8(A0, (uint8_t)((A2 & 1) ? v | bit : v & ~bit)); }
static void color_logic_op(Ctx *c) { wr8(A0 + 1, (uint8_t)A1); }
static void color_alpha_test(Ctx *c) { wr8(A0 + 2, (uint8_t)A1); }
static void chanmask_defaults(Ctx *c) { wr32(A0, 0xffffffffu); }
static void chanmask_set(Ctx *c) {                     /* (state, index, r, g, b, a) */
    uint32_t v = rd32(A0), sh = ((uint32_t)A1 & 7) * 4;
    v = (v & ~(15u << sh)) | ((uint32_t)((A2 & 1) | (A3 & 1) << 1 | (A4 & 1) << 2 | (A5 & 1) << 3) << sh);
    wr32(A0, v);
}
static void poly_defaults(Ctx *c) { static const uint8_t d[4] = {0, 1, 2, 0}; memcpy(P(A0), d, 4); }
static void poly_cull(Ctx *c) { wr8(A0, (uint8_t)A1); }
static void poly_front(Ctx *c) { wr8(A0 + 1, (uint8_t)A1); }
static void poly_mode(Ctx *c) { wr8(A0 + 2, (uint8_t)A1); }
static void poly_offset(Ctx *c) { wr8(A0 + 3, (uint8_t)A1); }
static void poly_get_cull(Ctx *c) { RET(rd8(A0)); }
static void poly_get_front(Ctx *c) { RET(rd8(A0 + 1)); }
static void poly_get_mode(Ctx *c) { RET(rd8(A0 + 2)); }
static void poly_get_offset(Ctx *c) { RET(rd8(A0 + 3)); }
/* depth-stencil: bit0 depth test, bit1 depth write, bit2 stencil test, 4..7 depth func, 8..11 / 12..15 stencil func front / back,
 * 16..27 stencil ops front (stencil fail, depth fail, depth pass), 28..39 the same for back */
static void ds_defaults(Ctx *c) { wr64(A0, 0x1111118822ULL); }
static void ds_bits(uint64_t a, int shift, uint64_t mask, uint64_t v) { wr64(a, (rd64(a) & ~(mask << shift)) | ((v & mask) << shift)); }
static void ds_depth_test(Ctx *c) { ds_bits(A0, 0, 1, A1); }
static void ds_depth_write(Ctx *c) { ds_bits(A0, 1, 1, A1); }
static void ds_stencil_test(Ctx *c) { ds_bits(A0, 2, 1, A1); }
static void ds_depth_func(Ctx *c) { ds_bits(A0, 4, 15, A1); }
static void ds_stencil_func(Ctx *c) { if (A1 & 1) ds_bits(A0, 8, 15, A2); if (A1 & 2) ds_bits(A0, 12, 15, A2); }
static void ds_stencil_op(Ctx *c) {                    /* (state, faces, stencilFail, depthFail, depthPass) */
    uint64_t v = (A2 & 15) | (A3 & 15) << 4 | (A4 & 15) << 8;
    if (A1 & 1) ds_bits(A0, 16, 0xfff, v);
    if (A1 & 2) ds_bits(A0, 28, 0xfff, v);
}
static void ds_get_depth_test(Ctx *c) { RET(rd64(A0) & 1); }
static void ds_get_depth_write(Ctx *c) { RET((rd64(A0) >> 1) & 1); }
static void ds_get_stencil_test(Ctx *c) { RET((rd64(A0) >> 2) & 1); }
static void ds_get_depth_func(Ctx *c) { RET((rd64(A0) >> 4) & 15); }
/* vertex attribute: bits 0..7 format (0 = disabled), 8..12 stream, 14..31 offset */
static void vattr_defaults(Ctx *c) { wr32(A0, 0); }
static void vattr_format(Ctx *c) { wr32(A0, (rd32(A0) & 0x1f00u) | ((uint32_t)A1 & 0xff) | ((uint32_t)A2 << 14)); }
static void vattr_stream(Ctx *c) { wr32(A0, (rd32(A0) & ~0x1f00u) | (((uint32_t)A1 & 31) << 8)); }
static void vstream_defaults(Ctx *c) { wr64(A0, 0); }
static void vstream_stride(Ctx *c) { wr32(A0, (uint32_t)A1); }
static void vstream_divisor(Ctx *c) { wr32(A0 + 4, (uint32_t)A1); }

/* ---- command buffers: recording */
static void cmd_initialize(Ctx *c) { cb_new(A0); RET(1); }
static void cmd_finalize(Ctx *c) { CmdBuf *cb = hobj(A0, K_CMD); if (cb) { cb_forget(cb); free(cb->data); free(cb); wr32(A0 + 8, 0); } }
static void cmd_add_command_memory(Ctx *c) { CmdBuf *cb = cb_get(A0); cb->cmd_size = A3; cb->cmd_used = 0; }       /* (cmdbuf, pool, offset, size): a new block replaces the current one */
static void cmd_add_control_memory(Ctx *c) { CmdBuf *cb = cb_get(A0); cb->ctrl_size = A2; cb->ctrl_used = 0; }     /* (cmdbuf, memory, size) */
static void cmd_get_command_size(Ctx *c) { RET(cb_get(A0)->cmd_size); }
static void cmd_get_control_size(Ctx *c) { RET(cb_get(A0)->ctrl_size); }
static void cmd_is_recording(Ctx *c) { RET(cb_get(A0)->recording); }
/* L1 round 2, TEST ONLY (SSBU_L1_TRACE=1 with SSBU_L1=1): for the 5 presents after the first press, every command-buffer
 * recording, submission, fence, sync wait, window acquire and present, with the guest thread name and the guest return address. */
#ifdef _WIN32
int display_l1_trace(void); double display_l1_ms(void); const char *guest_thread_name_of(unsigned long tid);
#else
int display_l1_trace(void); double display_l1_ms(void); const char *guest_thread_name_of(DWORD tid);
#endif
#define L1T(...) do { if (display_l1_trace()) { char l1b_[240]; snprintf(l1b_, sizeof l1b_, __VA_ARGS__); printf("l1-trace: %+8.2f %-26s lr %09llx %s%c", display_l1_ms(), guest_thread_name_of(GetCurrentThreadId()) ? guest_thread_name_of(GetCurrentThreadId()) : "?", (unsigned long long)c->x[30], l1b_, 10); } } while (0)
static void cmd_begin(Ctx *c) { L1T("begin recording cb %llx", (unsigned long long)A0); CmdBuf *cb = cb_get(A0); cb->len = 0; cb->recording = 1; cb->rec_cmd = cb->rec_ctrl = 0; cb->dispatches = 0; }
static void cmd_end(Ctx *c) {
    CmdBuf *cb = cb_get(A0);
    cb->ctrl_used += 32; cb->rec_ctrl += 32;
    uint64_t h = list_register(cb->data, cb->len, (uint32_t)cb->rec_cmd, (uint32_t)cb->rec_ctrl, (uint32_t)cb->dispatches);
    L1T("end recording cb %llx -> list %llx (%u bytes, %u dispatches)", (unsigned long long)A0, (unsigned long long)h, (unsigned)cb->len, (unsigned)cb->dispatches);
    cb->data = NULL; cb->len = cb->cap = 0; cb->recording = 0;
    RET(h);
}
static void cmd_call(Ctx *c) { uint64_t src = A2; int n = (int)A1; Cmd *m = rec(c, OP_CALL, (size_t)n * 8); memcpy(BLOB(m), P(src), (size_t)n * 8); }
static void cmd_texpool(Ctx *c) { rec(c, OP_TEXPOOL, 0)->a[0] = (uint64_t)(uintptr_t)hobj(A1, K_TPOOL); }
static void cmd_smppool(Ctx *c) { rec(c, OP_SMPPOOL, 0)->a[0] = (uint64_t)(uintptr_t)hobj(A1, K_SPOOL); }
static void cmd_program(Ctx *c) { rec(c, OP_PROGRAM, 0)->a[0] = (uint64_t)(uintptr_t)hobj(A1, K_PROG); }
static void rec_ranges(Ctx *c, uint32_t op, int single) {      /* (cmdbuf, stage, index, address, size) or (cmdbuf, stage, first, count, ranges) */
    int n = single ? 1 : (int)A3;
    uint64_t src = A4, addr = A3, size = A4;
    Cmd *m = rec(c, op, (size_t)n * 16);
    if (single) { m->a[2] = 1; memcpy(BLOB(m), &addr, 8); memcpy(BLOB(m) + 8, &size, 8); }
    else if (src) memcpy(BLOB(m), P(src), (size_t)n * 16);
    else memset(BLOB(m), 0, (size_t)n * 16);
}
static void cmd_ubo(Ctx *c) { rec_ranges(c, OP_UBO, 1); }
static void cmd_ubos(Ctx *c) { rec_ranges(c, OP_UBO, 0); }
static void cmd_ssbo(Ctx *c) { rec_ranges(c, OP_SSBO, 1); }
static void cmd_ssbos(Ctx *c) { rec_ranges(c, OP_SSBO, 0); }
static void cmd_texture(Ctx *c) { uint64_t h = A3; Cmd *m = rec(c, OP_TEXTURES, 8); m->a[2] = 1; memcpy(BLOB(m), &h, 8); }   /* (cmdbuf, stage, index, handle) */
static void cmd_textures(Ctx *c) {                      /* (cmdbuf, stage, first, count, handles) */
    int n = (int)A3;
    uint64_t src = A4;
    Cmd *m = rec(c, OP_TEXTURES, (size_t)n * 8);
    if (src) memcpy(BLOB(m), P(src), (size_t)n * 8); else memset(BLOB(m), 0, (size_t)n * 8);
}
static void cmd_vbuf(Ctx *c) { uint64_t addr = A2, size = A3; Cmd *m = rec(c, OP_VBUF, 16); m->a[1] = 1; memcpy(BLOB(m), &addr, 8); memcpy(BLOB(m) + 8, &size, 8); }   /* (cmdbuf, index, address, size) */
static void cmd_vbufs(Ctx *c) {                         /* (cmdbuf, first, count, ranges) */
    int n = (int)A2;
    uint64_t src = A3;
    Cmd *m = rec(c, OP_VBUF, (size_t)n * 16);
    if (src) memcpy(BLOB(m), P(src), (size_t)n * 16); else memset(BLOB(m), 0, (size_t)n * 16);
}
static void cmd_vattr(Ctx *c) { int n = (int)A1; uint64_t src = A2; Cmd *m = rec(c, OP_VATTR, (size_t)n * 4); if (n) memcpy(BLOB(m), P(src), (size_t)n * 4); }       /* (cmdbuf, count, states) */
static void cmd_vstream(Ctx *c) { int n = (int)A1; uint64_t src = A2; Cmd *m = rec(c, OP_VSTREAM, (size_t)n * 8); if (n) memcpy(BLOB(m), P(src), (size_t)n * 8); }   /* (cmdbuf, count, states) */
static void rec_state(Ctx *c, uint32_t op, size_t size) { uint64_t src = A1; Cmd *m = rec(c, op, 0); m->a[0] = m->a[1] = m->a[2] = 0; if (src) memcpy(m->a, P(src), size); }
static void cmd_blend(Ctx *c) { rec_state(c, OP_BLEND, 8); }
static void cmd_color(Ctx *c) { rec_state(c, OP_COLOR, 4); }
static void cmd_chanmask(Ctx *c) { rec_state(c, OP_CHANMASK, 4); }
static void cmd_multisample(Ctx *c) { rec_state(c, OP_MULTISAMPLE, 0x18); }
static void cmd_polygon(Ctx *c) { rec_state(c, OP_POLYGON, 4); }
static void cmd_depthstencil(Ctx *c) { rec_state(c, OP_DEPTHSTENCIL, 8); }
static void cmd_viewport(Ctx *c) {                      /* (cmdbuf, x, y, w, h) */
    float v[4] = {(float)(int)A1, (float)(int)A2, (float)(int)A3, (float)(int)A4};
    Cmd *m = rec(c, OP_VIEWPORTS, 16);
    m->a[0] = 0; m->a[1] = 1; memcpy(BLOB(m), v, 16);
}
static void cmd_viewports(Ctx *c) { int n = (int)A2; uint64_t src = A3; Cmd *m = rec(c, OP_VIEWPORTS, (size_t)n * 16); memcpy(BLOB(m), P(src), (size_t)n * 16); }   /* (cmdbuf, first, count, float[4] each) */
static void cmd_scissor(Ctx *c) { int v[4] = {(int)A1, (int)A2, (int)A3, (int)A4}; Cmd *m = rec(c, OP_SCISSORS, 16); m->a[0] = 0; m->a[1] = 1; memcpy(BLOB(m), v, 16); }
static void cmd_scissors(Ctx *c) { int n = (int)A2; uint64_t src = A3; Cmd *m = rec(c, OP_SCISSORS, (size_t)n * 16); memcpy(BLOB(m), P(src), (size_t)n * 16); }
static void cmd_depth_range(Ctx *c) { float v[2] = {F0, F1}; Cmd *m = rec(c, OP_DEPTHRANGES, 8); m->a[0] = 0; m->a[1] = 1; memcpy(BLOB(m), v, 8); }
static void cmd_depth_ranges(Ctx *c) { int n = (int)A2; uint64_t src = A3; Cmd *m = rec(c, OP_DEPTHRANGES, (size_t)n * 8); memcpy(BLOB(m), P(src), (size_t)n * 8); }
static void cmd_targets(Ctx *c) {                       /* (cmdbuf, count, textures[], views[], depth, depthView) */
    int n = (int)A1 < 8 ? (int)A1 : 8;
    uint64_t texs = A2, views = A3, depth = A4, dview = A5;
    TargetRef r[9];
    memset(r, 0, sizeof r);
    for (int i = 0; i < n; i++) {
        uint64_t t = texs ? rd64(texs + (uint64_t)i * 8) : 0, v = views ? rd64(views + (uint64_t)i * 8) : 0;
        r[i].tex = hobj(t, K_TEX);
        if (v) memcpy(&r[i].view, P(v), sizeof(View));
    }
    r[8].tex = hobj(depth, K_TEX);
    if (dview) memcpy(&r[8].view, P(dview), sizeof(View));
    Cmd *m = rec(c, OP_TARGETS, sizeof r);
    m->a[0] = (uint64_t)n;
    memcpy(BLOB(m), r, sizeof r);
}
static void cmd_stencil_ref(Ctx *c) { rec(c, OP_STENCIL_REF, 0); }
static void cmd_stencil_vmask(Ctx *c) { rec(c, OP_STENCIL_VMASK, 0); }
static void cmd_stencil_mask(Ctx *c) { rec(c, OP_STENCIL_MASK, 0); }
static void cmd_blend_color(Ctx *c) { uint64_t src = A1; Cmd *m = rec(c, OP_BLENDCOLOR, 16); memcpy(BLOB(m), P(src), 16); }
static void rec_float(Ctx *c, uint32_t op) { float v[3] = {F0, F1, F2}; Cmd *m = rec(c, op, 0); memcpy(m->a, v, 12); }
static void cmd_alpha_ref(Ctx *c) { rec_float(c, OP_ALPHAREF); }
static void cmd_point_size(Ctx *c) { rec_float(c, OP_POINTSIZE); }
static void cmd_line_width(Ctx *c) { rec_float(c, OP_LINEWIDTH); }
static void cmd_poly_offset(Ctx *c) { rec_float(c, OP_POLYOFFSET); }
static void cmd_rast_discard(Ctx *c) { rec(c, OP_RASTDISCARD, 0); }
static void cmd_depth_clamp(Ctx *c) { rec(c, OP_DEPTHCLAMP, 0); }
static void cmd_prim_restart(Ctx *c) { rec(c, OP_PRIMRESTART, 0); }
static void cmd_patch_size(Ctx *c) { rec(c, OP_PATCHSIZE, 0); }
static void cmd_clear_color(Ctx *c) { uint64_t src = A2; Cmd *m = rec(c, OP_CLEARCOLOR, 16); if (src) memcpy(BLOB(m), P(src), 16); else memset(BLOB(m), 0, 16); }   /* (cmdbuf, index, rgba, mask) */
static void cmd_clear_ds(Ctx *c) { float d = F0; Cmd *m = rec(c, OP_CLEARDS, 0); m->a[3] = 0; memcpy(&m->a[3], &d, 4); }   /* (cmdbuf, depth [v0], depthMask, stencilValue, stencilMask) */
static void cmd_draw_arrays(Ctx *c) { Cmd *m = rec(c, OP_DRAW_ARRAYS, 0); m->a[3] = 0; m->a[4] = 1; }              /* (cmdbuf, mode, first, count) */
static void cmd_draw_arrays_inst(Ctx *c) { rec(c, OP_DRAW_ARRAYS, 0); }                                            /* (cmdbuf, mode, first, count, baseInstance, instanceCount) */
static void cmd_draw_elements(Ctx *c) { Cmd *m = rec(c, OP_DRAW_ELEMENTS, 0); m->a[4] = 0; m->a[5] = 0; m->a[6] = 1; }   /* (cmdbuf, mode, type, count, indexBuffer) */
static void cmd_draw_elements_bv(Ctx *c) { Cmd *m = rec(c, OP_DRAW_ELEMENTS, 0); m->a[5] = 0; m->a[6] = 1; }             /* (.., baseVertex) */
static void cmd_draw_elements_inst(Ctx *c) {            /* (cmdbuf, mode, type, count, indexBuffer, baseVertex, baseInstance, instanceCount) */
    static int nlog;
    logf_limited(&nlog, 6, "nvn_gl: DrawElementsInstanced mode %d type %d count %d index %llx x5=%d x6=%d x7=%d\n", (int)A1, (int)A2, (int)A3, (unsigned long long)A4, (int)A5, (int)A6, (int)A7);
    rec(c, OP_DRAW_ELEMENTS, 0);
}
static void cmd_copy_buf_tex(Ctx *c) {                  /* (cmdbuf, buffer, texture, view, region, flags) */
    uint64_t view = A3, region = A4;
    Tex *t = hobj(A2, K_TEX);
    Cmd *m = rec(c, OP_COPY_BUF_TEX, 48);
    CmdBuf *cb = cb_get(A0);
    /* PR #14 (xtlg511): the copy takes the strides this command buffer has now (SetCopyRowStride / SetCopyImageStride), as
     * NVN does: at replay the running stride state may come from another command buffer's copies (a 320x320 upload read with
     * the 832-texel rows of a stage preview read past its buffer: the crash after a random stage choice) */
    m->a[1] = (uint64_t)(uintptr_t)t; m->a[5] = (uint64_t)cb->row_stride; m->a[6] = (uint64_t)cb->image_stride;
    memset(BLOB(m), 0, 48);
    if (view) memcpy(BLOB(m), P(view), sizeof(View));
    if (region) memcpy(BLOB(m) + 24, P(region), 24);
}
static void cmd_copy_tex_tex(Ctx *c) {                  /* (cmdbuf, src, srcView, srcRegion, dst, dstView, dstRegion, flags) */
    uint64_t sv = A2, sr = A3, dv = A5, dr = A6;
    Tex *s = hobj(A1, K_TEX), *d = hobj(A4, K_TEX);
    Cmd *m = rec(c, OP_COPY_TEX_TEX, 96);
    m->a[0] = (uint64_t)(uintptr_t)s; m->a[3] = (uint64_t)(uintptr_t)d;
    memset(BLOB(m), 0, 96);
    if (sv) memcpy(BLOB(m), P(sv), sizeof(View));
    if (sr) memcpy(BLOB(m) + 24, P(sr), 24);
    if (dv) memcpy(BLOB(m) + 48, P(dv), sizeof(View));
    if (dr) memcpy(BLOB(m) + 72, P(dr), 24);
}
static void cmd_copy_tex_buf(Ctx *c) {                  /* (cmdbuf, texture, view, region, buffer, flags) */
    uint64_t view = A2, region = A3;
    Tex *t = hobj(A1, K_TEX);
    Cmd *m = rec(c, OP_COPY_TEX_BUF, 48);
    CmdBuf *cb = cb_get(A0);
    m->a[0] = (uint64_t)(uintptr_t)t; m->a[3] = c->x[4]; m->a[5] = (uint64_t)cb->row_stride; m->a[6] = (uint64_t)cb->image_stride;   /* PR #14 (xtlg511): as cmd_copy_buf_tex */
    memset(BLOB(m), 0, 48);
    if (view) memcpy(BLOB(m), P(view), sizeof(View));
    if (region) memcpy(BLOB(m) + 24, P(region), 24);
}
static void cmd_sample_mask(Ctx *c) { rec(c, OP_SAMPLEMASK, 0); }                     /* (cmdbuf, mask) */
static void cmd_downsample(Ctx *c) { Tex *s = hobj(A1, K_TEX), *d = hobj(A2, K_TEX); Cmd *m = rec(c, OP_DOWNSAMPLE, 0); m->a[0] = (uint64_t)(uintptr_t)s; m->a[1] = (uint64_t)(uintptr_t)d; }
static void cmd_copy_buf_buf(Ctx *c) { rec(c, OP_COPY_BUF_BUF, 0); }                  /* (cmdbuf, src, dst, size, flags) */
static void cmd_update_ubo(Ctx *c) {                    /* (cmdbuf, buffer, bufferSize, offset, size, data) */
    uint64_t size = A4, src = A5;
    Cmd *m = rec(c, OP_UPDATE_UBO, (size_t)size);
    if (src) memcpy(BLOB(m), P(src), (size_t)size); else m->a[3] = 0;
}
static void cmd_clear_buffer(Ctx *c) { rec(c, OP_CLEAR_BUF, 0); }                     /* (cmdbuf, buffer, size, value) */
static void cmd_row_stride(Ctx *c) { cb_get(A0)->row_stride = (int64_t)A1; rec(c, OP_ROWSTRIDE, 0); }
static void cmd_dispatch(Ctx *c) { cb_get(A0)->dispatches++; rec(c, OP_DISPATCH, 0); }                          /* (cmdbuf, groupsX, groupsY, groupsZ) */
static void cmd_image(Ctx *c) { uint64_t h = A3; Cmd *m = rec(c, OP_IMAGES, 8); m->a[2] = 1; memcpy(BLOB(m), &h, 8); }   /* (cmdbuf, stage, index, imageHandle) */
static void cmd_images(Ctx *c) {                        /* (cmdbuf, stage, first, count, handles) */
    int n = (int)A3;
    uint64_t src = A4;
    Cmd *m = rec(c, OP_IMAGES, (size_t)n * 8);
    if (src) memcpy(BLOB(m), P(src), (size_t)n * 8); else memset(BLOB(m), 0, (size_t)n * 8);
}

/* Round 18, TEST ONLY (SSBU_RACECHK=1): does guest memory that submitted commands NAME (uniform / storage / vertex
 * ranges) change between nvnQueueSubmitCommands on the game's thread and the replay on the render thread? The submit
 * walks the lists and keeps a hash of every range (and the first 512 bytes of a uniform range); the back end calls
 * nvn_front_racechk at the start (phase 1) and at the end (phase 2) of the replay of the same batch, which walks them
 * again and compares. Lines "racechk:". Nothing runs without the variable. */
typedef struct RaceRec { struct RaceRec *next; int n, cap, ncs, capcs; LONG64 frame; uint64_t *h; uint8_t *copy; Range *cs; } RaceRec;      /* cs: every storage range of the batch (what a dispatch may write) */
static int race_is_storage(const RaceRec *r, const Range *g) { for (int i = 0; i < r->ncs; i++) if (g->addr < r->cs[i].addr + r->cs[i].size && r->cs[i].addr < g->addr + g->size) return 1; return 0; }
static RaceRec *g_race_head, *g_race_tail;
static SRWLOCK g_race_lock = SRWLOCK_INIT;
static LONG64 g_race_stat[3][3], g_race_batches, g_race_bad_batches[3];      /* [uniform, storage, vertex][ranges seen, changed before the replay, changed during it] */
static int racechk_env(void) { static int on = -1; if (on < 0) on = getenv("SSBU_RACECHK") != NULL; return on; }
static uint64_t race_fnv(const uint8_t *b, size_t n) { uint64_t h = 0xcbf29ce484222325ULL; for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ULL; return h; }
static void race_walk(uint64_t handle, int depth, RaceRec *r, int phase, int *pos, int *bad) {
    size_t len = 0;
    uint8_t *data = list_lookup(handle, &len);
    if (!data) return;
    for (size_t off = 0; off + sizeof(Cmd) <= len;) {
        const Cmd *m = (const Cmd *)(data + off);
        if (m->size < sizeof(Cmd)) break;
        off += m->size;
        if (m->op == OP_CALL) { for (int i = 0; i < (int)m->a[0]; i++) { uint64_t h; memcpy(&h, BLOB(m) + i * 8, 8); if (depth < 8) race_walk(h, depth + 1, r, phase, pos, bad); } continue; }
        if (m->op != OP_UBO && m->op != OP_SSBO && m->op != OP_VBUF) continue;
        int kind = m->op == OP_UBO ? 0 : m->op == OP_SSBO ? 1 : 2, count = m->op == OP_VBUF ? (int)m->a[1] : (int)m->a[2];
        size_t cap = kind == 0 ? 65536 : kind == 1 ? 16384 : 4096;
        for (int i = 0; i < count; i++) {
            Range g;
            memcpy(&g, BLOB(m) + i * 16, 16);
            if (!g.addr || !g.size || g.addr < 0x1000) continue;
            size_t n = g.size > cap ? cap : (size_t)g.size, nc = n > 512 ? 512 : n;
            const uint8_t *p = P(g.addr);
            uint64_t h = race_fnv(p, n);
            if (phase == 0) {
                if (r->n == r->cap) { r->cap = r->cap ? r->cap * 2 : 1024; r->h = realloc(r->h, (size_t)r->cap * 8); r->copy = realloc(r->copy, (size_t)r->cap * 512); }
                r->h[r->n] = h;
                if (kind == 0) memcpy(r->copy + (size_t)r->n * 512, p, nc);
                r->n++;
                if (kind == 1) { if (r->ncs == r->capcs) { r->capcs = r->capcs ? r->capcs * 2 : 256; r->cs = realloc(r->cs, (size_t)r->capcs * sizeof(Range)); } r->cs[r->ncs++] = g; }
                continue;
            }
            int k = (*pos)++;
            if (k >= r->n) continue;
            if (phase == 1) g_race_stat[kind][0]++;
            if (r->h[k] == h) continue;
            r->h[k] = h;                                 /* phase 2 then reports only what changed during the replay */
            g_race_stat[kind][phase]++; bad[kind]++;
            static int shown, shown_cs;
            static LONG64 plain[3];
            int is_cs = kind != 0 && race_is_storage(r, &g);      /* a storage range of this batch: a dispatch's target, written by our own copy-back */
            if (kind == 2 && !is_cs) { plain[phase]++; if (plain[phase] % 200 == 1) printf("racechk: vertex ranges that are NOT a storage range of their batch and changed: %lld before the replay, %lld during it (so far)\n", (long long)plain[1], (long long)plain[2]); }
            if (is_cs ? shown_cs++ < 12 : shown < 400) {
                if (!is_cs) shown++;
                if (is_cs) printf("racechk: (a storage range of this batch) ");
                printf("racechk: frame %lld %s range %d of the batch (%s, stage/first %d, slot %d) guest %llx+%llu CHANGED %s", (long long)g_st.frames, kind == 0 ? "UNIFORM" : kind == 1 ? "storage" : "vertex", k,
                       k_op_name[m->op], (int)m->a[0], (int)(m->op == OP_VBUF ? m->a[0] : m->a[1]) + i, (unsigned long long)g.addr, (unsigned long long)g.size, phase == 1 ? "between the submit and the start of its replay" : "DURING its replay");
                if (kind == 0) {
                    const uint8_t *o = r->copy + (size_t)k * 512;
                    size_t d = 0;
                    while (d < nc && o[d] == p[d]) d++;
                    if (d < nc) { d &= ~(size_t)3; printf("; first difference at +%llu: at submit %08x %08x %08x %08x, now %08x %08x %08x %08x", (unsigned long long)d, *(const uint32_t *)(o + d), d + 8 <= nc ? *(const uint32_t *)(o + d + 4) : 0, d + 12 <= nc ? *(const uint32_t *)(o + d + 8) : 0, d + 16 <= nc ? *(const uint32_t *)(o + d + 12) : 0,
                                                       *(const uint32_t *)(p + d), d + 8 <= nc ? *(const uint32_t *)(p + d + 4) : 0, d + 12 <= nc ? *(const uint32_t *)(p + d + 8) : 0, d + 16 <= nc ? *(const uint32_t *)(p + d + 12) : 0); }
                    else printf("; the difference is after the first 512 bytes");
                    memcpy(r->copy + (size_t)k * 512, p, nc);
                }
                printf("\n");
            }
        }
    }
}
static void racechk_submit(const uint64_t *handles, int count) {      /* the game's thread */
    RaceRec *r = calloc(1, sizeof *r);
    int pos = 0, bad[3] = {0, 0, 0};
    r->frame = g_st.frames;
    for (int i = 0; i < count; i++) race_walk(handles[i], 0, r, 0, &pos, bad);
    AcquireSRWLockExclusive(&g_race_lock);
    if (g_race_tail) g_race_tail->next = r; else g_race_head = r;
    g_race_tail = r;
    ReleaseSRWLockExclusive(&g_race_lock);
}
void nvn_front_racechk(const uint64_t *handles, int count, int phase) {      /* render thread: 1 = before the replay of the oldest recorded batch, 2 = after it */
    if (!racechk_env()) return;
    AcquireSRWLockExclusive(&g_race_lock);
    RaceRec *r = g_race_head;
    if (r && phase == 2) { g_race_head = r->next; if (!g_race_head) g_race_tail = NULL; }
    ReleaseSRWLockExclusive(&g_race_lock);
    if (!r) return;
    int pos = 0, bad[3] = {0, 0, 0};
    for (int i = 0; i < count; i++) race_walk(handles[i], 0, r, phase, &pos, bad);
    for (int k = 0; k < 3; k++) if (bad[k]) g_race_bad_batches[k]++;
    if (phase == 1) g_race_batches++;
    if (phase == 2) {
        static LONG64 last;
        if (g_st.frames - last >= 300) {
            last = g_st.frames;
            printf("racechk: totals at frame %lld: %lld batches | uniform ranges %lld, changed before the replay %lld, during %lld | storage %lld / %lld / %lld | vertex (first 4 KB) %lld / %lld / %lld | batches with a changed uniform / storage / vertex range: %lld / %lld / %lld\n",
                   (long long)g_st.frames, (long long)g_race_batches, (long long)g_race_stat[0][0], (long long)g_race_stat[0][1], (long long)g_race_stat[0][2], (long long)g_race_stat[1][0], (long long)g_race_stat[1][1], (long long)g_race_stat[1][2],
                   (long long)g_race_stat[2][0], (long long)g_race_stat[2][1], (long long)g_race_stat[2][2], (long long)g_race_bad_batches[0], (long long)g_race_bad_batches[1], (long long)g_race_bad_batches[2]);
        }
        free(r->h); free(r->copy); free(r->cs); free(r);
    }
}

/* ---- queue and window */
static void l1_pipe_after_submit(void); static int l1_pipe_wait(void);
static void queue_submit(Ctx *c) {                      /* (queue, count, handles) */
    SubmitJob j = {(int)A1, P(A2)};
    L1T("SUBMIT %d lists: %llx %llx %llx %llx", (int)A1, A2 && A1 > 0 ? (unsigned long long)rd64(A2) : 0ull, A2 && A1 > 1 ? (unsigned long long)rd64(A2 + 8) : 0ull, A2 && A1 > 2 ? (unsigned long long)rd64(A2 + 16) : 0ull, A2 && A1 > 3 ? (unsigned long long)rd64(A2 + 24) : 0ull);
    { void display_l1_submit(void); display_l1_submit(); }      /* L1: submits between the game's wait for the window texture and its present */
    if (j.count > 0 && A2 && racechk_env()) racechk_submit(j.handles, j.count);
    { int r1_ra_phase(void), r1_on(void); int ph = r1_ra_phase();      /* R1 round 5 (diagnosis): a submitted handle the host no longer knows, by run-ahead phase */
      if (r1_on()) { static LONG64 bad[3], ok[3]; size_t ln; for (int i = 0; i < j.count; i++) { if (list_lookup(j.handles[i], &ln)) ok[ph]++; else { bad[ph]++; if (bad[ph] <= 5 || !(bad[ph] & (bad[ph] - 1))) { uint32_t si = (uint32_t)j.handles[i] - 1; int have = si < (uint32_t)g_list_count; printf("r1-ra: submitted handle %llx unknown (phase %d: %lld bad / %lld good so far; slot gen %u data %d finalized %d created frame %lld used %lld, frame now %lld)\n", (unsigned long long)j.handles[i], ph, (long long)bad[ph], (long long)ok[ph],
                have ? g_lists[si].gen : 0, have && g_lists[si].data != NULL, have ? g_lists[si].finalized : -1, have ? (long long)g_lists[si].created : -1, have ? (long long)g_lists[si].used : -1, (long long)g_st.frames); } } } } }
    QueueObj *q = hobj(A0, K_QUEUE);
    if (q && A2) {                              /* what the submitted commands took of the queue's memory (model: see rec; a dispatch's launch descriptor is 0x100 bytes) */
        AcquireSRWLockShared(&g_list_lock);
        for (int i = 0; i < j.count; i++) {
            uint64_t h = j.handles[i];
            uint32_t k = (uint32_t)h - 1;
            if (h && k < (uint32_t)g_list_count && g_lists[k].data) { q->cmd_used += g_lists[k].model_cmd; q->control_used += g_lists[k].model_ctrl; q->compute_used += (LONG64)g_lists[k].dispatches * 0x100; }
        }
        ReleaseSRWLockShared(&g_list_lock);
    }
    if (j.count > 0 && A2 && drawtag_env()) {      /* round 17, TEST (SSBU_DRAWTAG): the frame SSBU_SHOT_FIGHT will save: its commands are tagged */
        /* the game polls the pad for frame n AFTER it has submitted the commands whose picture is saved as fight_<n>
         * (submit, next frame's logic and poll, present): the tag is armed by the present that saved fight_<n-1>, so
         * SSBU_SHOT_FIGHT must name n-1 as well (e.g. 699,700 tags the commands of 700). First form (the pending
         * shot at the submit) tagged nothing: run p17_zvk. */
        extern volatile LONG g_drawtag_next;
        int n = (int)InterlockedExchange(&g_drawtag_next, 0);
        if (n) { if (gl_async_on()) gl_post(job_drawtag, &n, sizeof n); else gl_run(job_drawtag, &n); }
    }
    int r1_ra_quiet(void);
    if (j.count > 0 && A2 && r1_ra_quiet()) { static LONG64 n; n++; }      /* R1 round 5, run-ahead: the real pass's submission = the lists the speculative pass already ran on the GPU: not run again */
    else if (j.count > 0 && A2) {
        if (gl_async_on() && j.count <= 4096) {
            size_t size = sizeof j + (size_t)j.count * 8;
            SubmitJob *copy = malloc(size);
            *copy = j;
            memcpy(copy + 1, j.handles, (size_t)j.count * 8);
            gl_post(g_rb->submit_async, copy, size);
            free(copy);
        } else gl_run(g_rb->submit, &j);
        l1_pipe_after_submit();      /* L1 round 2 (SSBU_L1_PIPE=1): the picture is complete after this submission: present it now, hold the game here */
    }
}
static void wb_set_textures(Ctx *c) {                   /* (builder, count, textures[]): kept in the builder (0x40 bytes) */
    int n = (int)A1 < 6 ? (int)A1 : 6;
    wr8(A0, (uint8_t)n);
    for (int i = 0; i < n; i++) wr64(A0 + 8 + (uint64_t)i * 8, rd64(A2 + (uint64_t)i * 8));
}
/* Track V/O session 9: the textures of every window the game made (for the Vulkan back end's resolution scale: nvn_vk_draw.c wintex_is). */
static Tex *g_window_tex[16];
static volatile int g_window_tex_n;
int nvn_front_tex_is_window(const Tex *t) { for (int i = 0; i < g_window_tex_n; i++) if (g_window_tex[i] == t) return 1; return 0; }
static void win_initialize(Ctx *c) {                    /* (window, builder) */
    Window *w = hnew(A0, K_WIN, sizeof *w);
    w->count = (int)(rd32(A1) & 0xff) <= 6 ? (int)(rd32(A1) & 0xff) : 0;
    w->interval = rd8(A1 + 4); w->nactive = rd8(A1 + 5); w->native = rd64(A1 + 56);
    display_set_interval(w->interval);
    for (int i = 0; i < w->count; i++) w->tex[i] = hobj(rd64(A1 + 8 + (uint64_t)i * 8), K_TEX);
    for (int i = 0; i < w->count; i++) if (g_window_tex_n < 16) g_window_tex[g_window_tex_n++] = w->tex[i];
    printf("nvn_gl: window with %d textures", w->count);
    if (w->count && w->tex[0]) printf(" (%dx%d format 0x%x)", w->tex[0]->b.width, w->tex[0]->b.height, w->tex[0]->b.format);
    printf("\n");
    RET(1);
}
static void win_acquire(Ctx *c) {                       /* (window, sync, int *textureIndex) -> 0 = success */
    static volatile LONG next;
    Window *w = hobj(A0, K_WIN);
    L1T("window acquire (index %d)", (int)((next + 1) % 3));
    if (A2) wr32(A2, (uint32_t)(InterlockedIncrement(&next) % (w && w->count ? (w->nactive ? w->nactive : w->count) : 2)));
    InterlockedExchangePointer(&g_window_sync, A1 ? hobj(A1, K_SYNC) : NULL);        /* P3 round 3: nvnSyncWait on it = wait for the refresh that frees the texture */
    if (w && A2 && A1) { extern Window *g_l1_acq_win; extern volatile LONG g_l1_acq_idx, g_l1_acq_new; g_l1_acq_win = w; g_l1_acq_idx = (LONG)rd32(A2); InterlockedExchange(&g_l1_acq_new, 1); }      /* L1: early present */
    display_clock_sample(A2);
    RET(0);
}
static void queue_acquire(Ctx *c) { c->x[0] = c->x[1]; c->x[1] = 0; win_acquire(c); }      /* nvnQueueAcquireTexture(queue, window, int *textureIndex) */
static void win_set_crop(Ctx *c) { Window *w = hobj(A0, K_WIN); if (w) { w->crop[0] = (int)A1; w->crop[1] = (int)A2; w->crop[2] = (int)A3; w->crop[3] = (int)A4; } }
/* close4: a window built on the movie maker's native window (nn::album::GetMovieMakerNativeWindow, nnalbum.c) is not the screen:
 * what the game presents to it is one picture of the video being recorded. The picture is read back and handed to the encoder;
 * nothing is shown and the display is not paced by it. */
uint64_t album_movie_native_window(void);
/* Session 13, TEST ONLY: SSBU_SHOT_FIGHT=<n>[,<n>...] (nnhid.c counts the fight frames at the pad poll): the present the game
 * queues next after the poll that reached fight frame n is saved as fight_<n>.bmp in SSBU_SHOT_DIR by whichever back end runs. */
static volatile LONG g_fight_shot;
void nvn_front_shot_fight(int n) { InterlockedExchange(&g_fight_shot, n); }
int nvn_front_shot_pending(void) { return (int)g_fight_shot; }      /* round 17 (SSBU_DRAWTAG): the fight frame the next queued present will be saved as, 0 = none */
unsigned long nvn_front_present_tid(void) { return g_pf_main_tid; }      /* the game thread that queues the presents (the stall log of nvn_vk_wd.c) */
/* Session 14 (display.c, the vsync thread at a tick): the present the Vulkan back end kept for this tick, as a job of the
 * render thread (in order with the other jobs; nobody waits for it). */
void nvn_vk_present_flush_job(void *arg);              /* nvn_vk_draw.c */
void nvn_front_post_present_flush(void) { if (g_vk) gl_post(nvn_vk_present_flush_job, NULL, 0); }
/* osk.c, the on-screen keyboard: drawn by the Vulkan renderer only (the OpenGL path keeps the old text dialog) */
void nvn_vk_osk_redraw_job(void *arg);                 /* nvn_vk_draw.c */
long long nvn_vk_presents(void);                       /* nvn_vk_draw.c */
int nvn_front_osk_ready(void) { return g_vk != 0; }
void nvn_front_osk_redraw(void) { if (g_vk) gl_post(nvn_vk_osk_redraw_job, NULL, 0); }
long long nvn_front_presents(void) { return g_vk ? nvn_vk_presents() : 0; }
/* ---- Track L1 (input latency), early present. The game submits the commands of a frame's picture BEFORE it waits for the
 * window texture (nvnSyncWait on the acquire sync = display_wait_refresh), then reads the pad, runs that frame's logic and
 * only then calls nvnQueuePresentTexture for the picture it had finished before the wait (measured: tests\logs\vk13_l1t_*,
 * `l1-submit:` 0 submits between the wait and the present in every fight frame). With SSBU_L1_EARLY=1 (graphics.ini
 * latency = early | lowest) that picture is handed to the window when the wait BEGINS, for the tick the game waits for (the
 * hold of session 14 keeps it until then), instead of one tick later; the game's own present of the same texture then only
 * does the pacing bookkeeping. If the game does not present that texture next, the early present is counted as a miss. */
Window *g_l1_acq_win;
volatile LONG g_l1_acq_idx, g_l1_acq_new;
static Window *volatile g_l1_early_win;
static volatile LONG g_l1_early_idx;
static LONG64 g_l1_early_n, g_l1_early_miss;
static void l1_early_present(void) {
    int display_l1_early(void);
    if (!g_vk || !display_l1_early() || !InterlockedExchange(&g_l1_acq_new, 0)) return;
    Window *w = g_l1_acq_win;
    if (!w || (w->native && w->native == album_movie_native_window())) return;
    if (g_l1_early_win) { g_l1_early_win = NULL; g_l1_early_miss++; }
    PresentJob j = {w, (int)g_l1_acq_idx};
    /* the picture of THIS acquire is the one rendered since the last present: the index the game acquired at the end of its
     * previous frame was the target of the commands it submitted since; present it */
    g_l1_early_win = w; g_l1_early_idx = j.index;
    g_l1_early_n++;
    gl_post(g_rb->present, &j, sizeof j);
}
/* L1 round 2, PIPE (SSBU_L1_PIPE=1): measured (SSBU_L1_TRACE, tests\logs\vk13_l1t_tr1.log): after its present the game
 * acquires the next window texture, waits for it (nvnSyncWait on the acquire sync, 0x710386fce0) and for the previous
 * frame's fence (0x710386fcf0), then ends the command buffers its "render dispatch" thread recorded during the previous
 * frame plus one of its own and submits them in ONE call (0x71037f0b40, list of 6), then runs the next frame (task workers,
 * pad poll, logic) and only then presents that picture (0x710386fc84). The picture is complete after that submission
 * (l1-submit: no other submission before the present). Pipe: the window wait returns at once; right after that submission
 * the picture goes to the render thread for the next tick and the GAME is held there until that tick (display_wait_refresh),
 * i.e. before its next pad poll as before; the game's later present of the same texture only does the bookkeeping.
 * Saves one whole frame of the game's two. */
static int g_l1_pipe = -1;
static volatile LONG g_l1_pipe_pending;
static int l1_pipe_on(void) { if (g_l1_pipe < 0) { const char *e = getenv("SSBU_L1_PIPE"); int display_l1_pipe_default(void); g_l1_pipe = e ? atoi(e) != 0 : display_l1_pipe_default(); printf("nvn: pipe (picture presented right after the game's frame submission, game held there) %s%c", g_l1_pipe ? "ON" : "off", 10); } return g_l1_pipe && g_vk; }
static volatile LONG g_l1_sh_pending;
static int l1_pipe_wait(void) {
    InterlockedExchange(&g_l1_sh_pending, 1);
    if (!l1_pipe_on() || !g_l1_acq_win) return 0;
    InterlockedExchange(&g_l1_pipe_pending, 1);
    return 1;
}
static void l1_nop_job(void *arg) { (void)arg; }
static void l1_pipe_after_submit(void) {
    { static int sh = -1; if (sh < 0) sh = getenv("SSBU_L1_SUBMIT_HOLD") && atoi(getenv("SSBU_L1_SUBMIT_HOLD")); if (sh && !g_l1_pipe && InterlockedExchange(&g_l1_sh_pending, 0)) gl_run(l1_nop_job, NULL); }      /* TEST: old path, but the game waits until the render thread has replayed this submission (is the old picture a race with the game's next frame?) */
    if (!InterlockedExchange(&g_l1_pipe_pending, 0)) return;
    { int r1_ra_quiet(void); if (r1_ra_quiet()) return; }      /* R1 round 5, run-ahead: the real pass's submission repeats the picture the speculative pass presented: not presented, the game not held */
    Window *w = g_l1_acq_win;
    if (!w || (w->native && w->native == album_movie_native_window())) { display_wait_refresh(); return; }
    if (g_l1_early_win) { g_l1_early_win = NULL; g_l1_early_miss++; }
    PresentJob j = {w, (int)g_l1_acq_idx};
    g_l1_early_win = w; g_l1_early_idx = j.index;
    g_l1_early_n++;
    { void display_present_plan(int vulkan); display_present_plan(g_vk); }
    gl_post(g_rb->present, &j, sizeof j);
    display_wait_refresh();
}
void nvn_l1_report(FILE *out) { if (g_l1_early_n || g_l1_early_miss) fprintf(out, "l1: early presents %lld, game presents that did not match an early present %lld%c", (long long)g_l1_early_n, (long long)g_l1_early_miss, 10); }
static void queue_present(Ctx *c) {                     /* (queue, window, textureIndex) */
    PresentJob j = {hobj(A1, K_WIN), (int)A2};
    L1T("PRESENT texture %d", (int)A2);
    if (j.win && j.win->native && j.win->native == album_movie_native_window()) { gl_run(g_rb->present_movie, &j); return; }
    g_pf_main_tid = GetCurrentThreadId();
    int ra_mode; { int r1_ra_present_mode(void); ra_mode = g_l1_pipe > 0 ? r1_ra_present_mode() : 0; }      /* R1 round 5, run-ahead: 2 = the real pass's present (nothing), 1 = presented at the speculative submission already */
    if (ra_mode == 2) return;
    if (ra_mode == 1 && g_l1_early_win == j.win) j.index = (int)g_l1_early_idx;      /* (the restore rolled the game's texture index back, the host's acquire count is not: the texture presented is the host's) */
    if (g_l1_early_win && g_l1_early_win == j.win && g_l1_early_idx == j.index) {      /* L1: this picture was presented at the game's wait already: only the pacing bookkeeping */
        g_l1_early_win = NULL;
        if (g_l1_pipe) {      /* round 2: planned and held at the submission already; a picture wanted by SSBU_SHOT_FIGHT is saved by presenting the same texture again (test only) */
            if (g_fight_shot) { void nvn_vk_shot_job(void *arg); j.shot_fight = (int)InterlockedExchange(&g_fight_shot, 0); gl_run(nvn_vk_shot_job, &j); }
        } else {
        display_before_present();
        { void display_present_plan(int vulkan); display_present_plan(g_vk); }
        }
        { void display_l1_early_skip(void); display_l1_early_skip(); }
        display_pace();
        return;
    }
    if (g_l1_early_win) { g_l1_early_win = NULL; g_l1_early_miss++; }
    if (g_fight_shot) j.shot_fight = (int)InterlockedExchange(&g_fight_shot, 0);      /* session 13 (test, SSBU_SHOT_FIGHT): bound here, on the game's thread, so that both renderers save the same game frame */
    if (j.shot_fight && drawtag_env()) InterlockedExchange(&g_drawtag_next, j.shot_fight + 1);      /* round 17 (SSBU_DRAWTAG): the next submission is the next fight frame's */
    display_before_present();
    { void display_present_plan(int vulkan); display_present_plan(g_vk); }      /* session 14: the tick this frame is shown at, decided before the present (display.c) */
    gl_run(g_rb->present, &j);
    display_pace();
}

/* ================================================================ the rest of the NVN API (plan\phases\G1_nvn_audit.md)
 * Signatures and enum values: the public nvn.h (open-ead/nnheaders, MIT). Every entry point the game obtains has a handler
 * here or in nvn_stub.c; the ones that have no effect on this platform by the API's own meaning say why in nvn_audit_notes.tsv. */

/* ---- debug labels: kept for every object (nvn_label_get), and given to the OpenGL object where there is one */
typedef struct LabelEnt { uint64_t key; char text[96]; struct LabelEnt *next; } LabelEnt;
static LabelEnt *g_labels[1024];
static SRWLOCK g_label_lock = SRWLOCK_INIT;
void nvn_label_set(uint64_t obj, const char *text) {
    unsigned h = (unsigned)((obj >> 4) * 2654435761u) & 1023;
    AcquireSRWLockExclusive(&g_label_lock);
    LabelEnt *e = g_labels[h];
    while (e && e->key != obj) e = e->next;
    if (!e) { e = calloc(1, sizeof *e); e->key = obj; e->next = g_labels[h]; g_labels[h] = e; }
    snprintf(e->text, sizeof e->text, "%s", text ? text : "");
    ReleaseSRWLockExclusive(&g_label_lock);
}
const char *nvn_label_get(uint64_t obj) {
    unsigned h = (unsigned)((obj >> 4) * 2654435761u) & 1023;
    const char *r = "";
    AcquireSRWLockShared(&g_label_lock);
    for (LabelEnt *e = g_labels[h]; e; e = e->next) if (e->key == obj) r = e->text;
    ReleaseSRWLockShared(&g_label_lock);
    return r;
}
void nvn_label_drop(uint64_t obj) { if (nvn_label_get(obj)[0]) nvn_label_set(obj, ""); }
static void any_set_label(Ctx *c) {                     /* nvn<Object>SetDebugLabel(object, const char *label) */
    nvn_label_set(A0, A1 ? (const char *)P(A1) : "");
    Tex *t = hobj(A0, K_TEX);
    Prog *p = hobj(A0, K_PROG);
    if (t) t->label_dirty = 1;                          /* applied by tex_sync on the render thread */
    if (p) p->label_dirty = 1;                          /* applied when its GLSL programs are next used */
}
static void any_debug_id(Ctx *c) { RET(A0); }           /* nvn<Object>GetDebugID: a number unique to the object: its address */

/* ---- device */
static uint64_t g_nvn_device;
static int g_device_flags;
static LARGE_INTEGER g_qpf;
uint64_t now_ns(void) {
    LARGE_INTEGER t;
    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&t);
    return (uint64_t)(t.QuadPart / g_qpf.QuadPart) * 1000000000ull + (uint64_t)(t.QuadPart % g_qpf.QuadPart) * 1000000000ull / (uint64_t)g_qpf.QuadPart;
}
static void devb_defaults(Ctx *c) { memset(P(A0), 0, 0x40); }
static void devb_set_flags(Ctx *c) { wr32(A0, (uint32_t)A1); }
static void devb_get_flags(Ctx *c) { RET(rd32(A0)); }
static void dev_initialize(Ctx *c) { g_nvn_device = A0; g_device_flags = A1 ? (int)rd32(A1) : 0; RET(1); }      /* (device, builder) */
static void dev_finalize(Ctx *c) { if (g_nvn_device == A0) g_nvn_device = 0; }
static void dev_get_origin_mode(Ctx *c) { RET(g_origin_mode); }
static void dev_get_depth_mode(Ctx *c) { RET(g_depth_mode); }
static void dev_timestamp_now(Ctx *c) { RET(now_ns()); }
static void dev_timestamp_of(Ctx *c) { RET(A1 ? rd64(A1 + 8) : 0); }      /* (device, const NVNcounterData *): counter timestamps are written in nanoseconds */
static void dev_debug_domain(Ctx *c) { static volatile LONG next; RET(InterlockedIncrement(&next)); }
static void dev_separate_handle(Ctx *c) { RET((uint64_t)(int64_t)(int32_t)(uint32_t)A1 | 0x100000000ull); }   /* nvnDeviceGetSeparateTextureHandle [sdk 0x41bc60] */
static void dev_separate_sampler_handle(Ctx *c) { RET((uint64_t)((int64_t)(int32_t)(uint32_t)A1 << 20)); }                 /* nvnDeviceGetSeparateSamplerHandle [sdk 0x41bc50]: the ID sign-extended from 32 bits, then << 20 */
static void dev_debugger_attached(Ctx *c) { RET(0); }   /* no external NVN debugger exists for this process: the true answer */
/* nvnDeviceInstallDebugCallback: in this SDK build the function is a bare `ret` [sdk 0x41b890] (the entry the name resolves to in
 * the table nvnDeviceGetProcAddress reads, [0x41d6a0] -> table [0xc28398]): nothing is stored, nothing can ever be reported.
 * Bound to n_nop in the table below. */
static void dev_apply_deferred(Ctx *c) { gl_run(g_rb->housekeeping, NULL); }      /* (device, age): objects finalized so far are released now */
static void any_get_device(Ctx *c) { RET(g_nvn_device); }                       /* nvn<Builder>GetDevice: there is one device */
static void any_set_device(Ctx *c) { if (A1) g_nvn_device = A1; }               /* nvn<Builder>SetDevice(builder, device) */

/* ---- sync objects: OpenGL fences */
static void sync_initialize(Ctx *c) { hnew(A0, K_SYNC, sizeof(SyncObj)); RET(1); }
static void sync_finalize(Ctx *c) {                     /* the small host record stays: a recorded FenceSync / WaitSync may still name it */
    SyncObj *o = hobj(A0, K_SYNC);
    if (!o) return;
    InterlockedExchange(&o->dead, 1);
    wr32(A0 + 8, 0);
    gl_run(g_rb->sync_drop, o);
}
static void queue_fence_sync(Ctx *c) {                  /* (queue, sync, condition, flags) */
    L1T("fence sync %llx", (unsigned long long)A1);
    SyncObj *o = hobj(A1, K_SYNC);
    SyncJob j = {o, (int)A3, 0, 0};
    if (o) { if (gl_async_on()) gl_post(g_rb->fence, &j, sizeof j); else gl_run(g_rb->fence, &j); }
}
static void sync_wait(Ctx *c) {                         /* (sync, timeoutNs) -> NVNsyncWaitResult */
    L1T("sync wait %llx%s", (unsigned long long)A0, hobj(A0, K_SYNC) == g_window_sync ? " (WINDOW texture)" : "");
    SyncObj *o = hobj(A0, K_SYNC);
    SyncJob j = {o, 0, A1, 3};
    if (o) gl_run(g_rb->sync_wait, &j);
    if (o && o == g_window_sync && A1 && j.result == 3) {      /* P3 round 3: the window texture's sync (never fenced by us): it is free at the refresh that shows the queued frame (display.c) */
        if (l1_pipe_wait()) j.result = 1; else {      /* L1 round 2: pipe mode does not hold here */
        l1_early_present();                                    /* L1 (latency = early / SSBU_L1_EARLY=1): the picture the game finished before this wait goes to the window now (at its tick) */
        if (display_wait_refresh()) j.result = 1;             /* NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED; otherwise as before */
        }
    }
    RET(j.result);
}
static void queue_wait_sync(Ctx *c) {                   /* (queue, sync) -> NVNboolean */
    SyncObj *o = hobj(A1, K_SYNC);
    SyncJob j = {o, 0, 0, 0};
    if (o) gl_run(g_rb->server_wait, &j);
    RET(j.result);
}
static void sync_no_gl_interop(Ctx *c) { RET(0); }

/* ---- events: a 32-bit value in memory pool storage that the CPU and queued commands signal and wait for.
 * NVNeventBuilder: +0 pool, +8 offset. NVNevent: +0 pool, +8 offset, +16 address of the value. */
static void evb_defaults(Ctx *c) { memset(P(A0), 0, 0x20); }
static void evb_storage(Ctx *c) { wr64(A0, A1); wr64(A0 + 8, A2); }
static void evb_get_storage(Ctx *c) { if (A1) wr64(A1, rd64(A0 + 8)); RET(rd64(A0)); }
static void ev_get_pool(Ctx *c) { RET(rd64(A0)); }
static void ev_get_offset(Ctx *c) { RET(rd64(A0 + 8)); }
static void ev_initialize(Ctx *c) {                     /* (event, builder) */
    uint64_t mem = 0, size = 0, pool = rd64(A1), off = rd64(A1 + 8);
    memset(P(A0), 0, 0x40);
    wr64(A0, pool); wr64(A0 + 8, off);
    if (!pool || !nvn_pool_range(pool, &mem, &size) || !mem || off + 4 > size) { RET(0); return; }
    wr64(A0 + 16, mem + off);
    RET(1);
}
static void ev_finalize(Ctx *c) { wr64(A0 + 16, 0); }
static void ev_get_value(Ctx *c) { uint64_t a = rd64(A0 + 16); RET(a ? rd32(a) : 0); }
void event_signal_at(uint64_t a, int mode, uint32_t value) {
    if (!a) return;
    if (mode == 1) InterlockedExchangeAdd((volatile LONG *)P(a), (LONG)value); else InterlockedExchange((volatile LONG *)P(a), (LONG)value);
}
static void ev_signal(Ctx *c) { event_signal_at(rd64(A0 + 16), (int)A1, (uint32_t)A2); }      /* (event, mode, value) */

/* ---- queue. NVNqueueBuilder (0x40): +0 queue memory, +8 its size, +0x10 command, +0x18 compute, +0x20 control memory size,
 * +0x28 command flush threshold, +0x30 flags, +0x34 priority. NVNqueue: +0 host record. */
static void qb_defaults(Ctx *c) {                       /* sizes: the defaults of the device (nvnDeviceGetInteger 0x3a / 0x3d / 0x3b in this table), priority normal */
    memset(P(A0), 0, 0x40);
    wr64(A0 + 0x10, 65536); wr64(A0 + 0x18, 262144); wr64(A0 + 0x20, 16384); wr64(A0 + 0x28, 0); wr32(A0 + 0x34, 100);
}
static void qb_set_flags(Ctx *c) { wr32(A0 + 0x30, (uint32_t)A1); }
static void qb_set_cmd(Ctx *c) { wr64(A0 + 0x10, A1); }
static void qb_set_compute(Ctx *c) { wr64(A0 + 0x18, A1); }
static void qb_set_control(Ctx *c) { wr64(A0 + 0x20, A1); }
static void qb_set_threshold(Ctx *c) { wr64(A0 + 0x28, A1); }
static void qb_set_priority(Ctx *c) { wr32(A0 + 0x34, (uint32_t)A1); }
static void qb_set_memory(Ctx *c) { wr64(A0, A1); wr64(A0 + 8, A2); }
static void qb_get_flags(Ctx *c) { RET(rd32(A0 + 0x30)); }
static void qb_get_cmd(Ctx *c) { RET(rd64(A0 + 0x10)); }
static void qb_get_compute(Ctx *c) { RET(rd64(A0 + 0x18)); }
static void qb_get_control(Ctx *c) { RET(rd64(A0 + 0x20)); }
static void qb_get_threshold(Ctx *c) { RET(rd64(A0 + 0x28)); }
static void qb_get_priority(Ctx *c) { RET(rd32(A0 + 0x34)); }
static void qb_get_memory(Ctx *c) { RET(rd64(A0)); }
static void qb_get_memory_size(Ctx *c) { RET(rd64(A0 + 8)); }
static void qb_get_queue_memory_size(Ctx *c) { RET((rd64(A0 + 0x10) + rd64(A0 + 0x18) + rd64(A0 + 0x20) + 0xfff) & ~0xfffull); }   /* what a queue with these sizes needs */
static void queue_initialize(Ctx *c) {                  /* (queue, builder) */
    QueueObj *q = hnew(A0, K_QUEUE, sizeof *q);
    q->cmd_size = rd64(A1 + 0x10); q->compute_size = rd64(A1 + 0x18); q->control_size = rd64(A1 + 0x20); q->flush_threshold = rd64(A1 + 0x28);
    q->flags = (int)rd32(A1 + 0x30); q->priority = (int)rd32(A1 + 0x34);
    RET(1);
}
static void queue_finalize(Ctx *c) { gl_barrier(); QueueObj *q = hobj(A0, K_QUEUE); if (q) { wr32(A0 + 8, 0); free(q); } nvn_label_drop(A0); }
static void queue_total_cmd(Ctx *c) { QueueObj *q = hobj(A0, K_QUEUE); RET(q ? q->cmd_used : 0); }
static void queue_total_control(Ctx *c) { QueueObj *q = hobj(A0, K_QUEUE); RET(q ? q->control_used : 0); }
static void queue_total_compute(Ctx *c) { QueueObj *q = hobj(A0, K_QUEUE); RET(q ? q->compute_used : 0); }
static void queue_reset_usage(Ctx *c) { QueueObj *q = hobj(A0, K_QUEUE); if (q) q->cmd_used = q->control_used = q->compute_used = 0; }
static void queue_flush(Ctx *c) { (void)c; if (gl_async_on()) gl_post(g_rb->flush, NULL, 0); else gl_run(g_rb->flush, NULL); }
static void queue_finish(Ctx *c) { (void)c; gl_run(g_rb->finish, NULL); }
static void queue_get_error(Ctx *c) {                   /* (queue, NVNqueueErrorInfo *) -> NVNqueueGetErrorResult: the state of the real GPU context */
    int st = 0;
    gl_run(g_rb->reset_status, &st);
    RET(st ? 1 : 0);                                    /* GPU_NO_ERROR, or GPU_ERROR_UNKNOWN after a context reset */
}

/* ---- multisample state (0x18 bytes): +0 multisample enable, +1 alpha to coverage, +2 its dither, +3 bit 0 coverage to colour,
 * bit 1 sample locations, bit 2 sample location grid, bits 3..4 coverage modulation mode, bits 5..7 coverage-to-colour output;
 * +4 samples, +5 raster samples; +8..+23 sixteen sample locations, x in the low and y in the high four bits (sixteenths). */
static void ms_defaults(Ctx *c) { memset(P(A0), 0, 0x18); wr8(A0, 1); wr8(A0 + 2, 1); memset(P(A0 + 8), 0x88, 16); }
static void ms_enable(Ctx *c) { wr8(A0, (uint8_t)(A1 & 1)); }
static void ms_a2c(Ctx *c) { wr8(A0 + 1, (uint8_t)(A1 & 1)); }
static void ms_a2c_dither(Ctx *c) { wr8(A0 + 2, (uint8_t)(A1 & 1)); }
static void ms_samples(Ctx *c) { wr8(A0 + 4, (uint8_t)A1); }
static void ms_raster_samples(Ctx *c) { wr8(A0 + 5, (uint8_t)A1); }
static void ms_flag(uint64_t a, unsigned shift, unsigned mask, uint64_t v) { wr8(a + 3, (uint8_t)((rd8(a + 3) & ~(mask << shift)) | ((v & mask) << shift))); }
static void ms_c2c(Ctx *c) { ms_flag(A0, 0, 1, A1); }
static void ms_locations_on(Ctx *c) { ms_flag(A0, 1, 1, A1); }
static void ms_grid_on(Ctx *c) { ms_flag(A0, 2, 1, A1); }
static void ms_covmod(Ctx *c) { ms_flag(A0, 3, 3, A1); }
static void ms_c2c_output(Ctx *c) { ms_flag(A0, 5, 7, A1); }
static void ms_set_locations(Ctx *c) {                  /* (state, start, count, const float *xy pairs in [0, 1)) */
    for (int i = 0; i < (int)A2 && (int)A1 + i < 16; i++) {
        float x, y;
        memcpy(&x, P(A3 + (uint64_t)i * 8), 4); memcpy(&y, P(A3 + (uint64_t)i * 8 + 4), 4);
        int xi = (int)(x * 16.0f), yi = (int)(y * 16.0f);
        wr8(A0 + 8 + A1 + (uint64_t)i, (uint8_t)((xi < 0 ? 0 : xi > 15 ? 15 : xi) | (yi < 0 ? 0 : yi > 15 ? 15 : yi) << 4));
    }
}
static void ms_get_enable(Ctx *c) { RET(rd8(A0)); }
static void ms_get_a2c(Ctx *c) { RET(rd8(A0 + 1)); }
static void ms_get_a2c_dither(Ctx *c) { RET(rd8(A0 + 2)); }
static void ms_get_samples(Ctx *c) { RET(rd8(A0 + 4)); }
static void ms_get_raster_samples(Ctx *c) { RET(rd8(A0 + 5)); }
static void ms_get_c2c(Ctx *c) { RET(rd8(A0 + 3) & 1); }
static void ms_get_locations_on(Ctx *c) { RET((rd8(A0 + 3) >> 1) & 1); }
static void ms_get_grid_on(Ctx *c) { RET((rd8(A0 + 3) >> 2) & 1); }
static void ms_get_covmod(Ctx *c) { RET((rd8(A0 + 3) >> 3) & 3); }
static void ms_get_c2c_output(Ctx *c) { RET(rd8(A0 + 3) >> 5); }
static void ms_get_grid(Ctx *c) {                       /* (state, int *width, int *height): the grid this GPU repeats sample locations over */
    int wh[2] = {1, 1};
    if ((rd8(A0 + 3) >> 2) & 1) gl_run(g_rb->sample_grid, wh);
    if (A1) wr32(A1, (uint32_t)wh[0]);
    if (A2) wr32(A2, (uint32_t)wh[1]);
}

/* ---- the other small state objects: getters of what the setters stored */
/* blend state (8 bytes): +0 bits 0..2 target, 3..4 advanced overlap, 5 premultiplied source, 6 normalised destination;
 * +1..+4 functions, +5 +6 equations, +7 advanced mode */
static void blend_defaults(Ctx *c) { static const uint8_t d[8] = {0x20, 2, 1, 2, 1, 1, 1, 0}; memcpy(P(A0), d, 8); }
static void blend_target(Ctx *c) { wr8(A0, (uint8_t)((rd8(A0) & ~7) | (A1 & 7))); }
static void blend_adv_mode(Ctx *c) { wr8(A0 + 7, (uint8_t)A1); }
static void blend_adv_overlap(Ctx *c) { wr8(A0, (uint8_t)((rd8(A0) & ~0x18) | ((A1 & 3) << 3))); }
static void blend_adv_premult(Ctx *c) { wr8(A0, (uint8_t)((rd8(A0) & ~0x20) | ((A1 & 1) << 5))); }
static void blend_adv_normdst(Ctx *c) { wr8(A0, (uint8_t)((rd8(A0) & ~0x40) | ((A1 & 1) << 6))); }
static void blend_get_target(Ctx *c) { RET(rd8(A0) & 7); }
static void blend_get_func(Ctx *c) { for (int i = 0; i < 4; i++) if (c->x[1 + i]) wr32(c->x[1 + i], rd8(A0 + 1 + (uint64_t)i)); }
static void blend_get_equation(Ctx *c) { if (A1) wr32(A1, rd8(A0 + 5)); if (A2) wr32(A2, rd8(A0 + 6)); }
static void blend_get_adv_mode(Ctx *c) { RET(rd8(A0 + 7)); }
static void blend_get_adv_overlap(Ctx *c) { RET((rd8(A0) >> 3) & 3); }
static void blend_get_adv_premult(Ctx *c) { RET((rd8(A0) >> 5) & 1); }
static void blend_get_adv_normdst(Ctx *c) { RET((rd8(A0) >> 6) & 1); }
static void color_get_blend_enable(Ctx *c) { RET((rd8(A0) >> (A1 & 7)) & 1); }
static void color_get_logic_op(Ctx *c) { RET(rd8(A0 + 1)); }
static void color_get_alpha_test(Ctx *c) { RET(rd8(A0 + 2)); }
static void chanmask_get(Ctx *c) {                      /* (state, index, NVNboolean *r, *g, *b, *a) */
    uint32_t m = rd32(A0) >> (((uint32_t)A1 & 7) * 4);
    for (int i = 0; i < 4; i++) if (c->x[2 + i]) wr8(c->x[2 + i], (uint8_t)((m >> i) & 1));
}
static void ds_get_stencil_func(Ctx *c) { RET((rd64(A0) >> (A1 & 2 && !(A1 & 1) ? 12 : 8)) & 15); }       /* (state, face) */
static void ds_get_stencil_op(Ctx *c) {                 /* (state, face, *stencilFail, *depthFail, *depthPass) */
    uint64_t v = rd64(A0) >> (A1 & 2 && !(A1 & 1) ? 28 : 16);
    for (int i = 0; i < 3; i++) if (c->x[2 + i]) wr32(c->x[2 + i], (uint32_t)((v >> (i * 4)) & 15));
}
static void vattr_get_format(Ctx *c) { if (A1) wr32(A1, rd32(A0) & 0xff); if (A2) wr64(A2, rd32(A0) >> 14); }
static void vattr_get_stream(Ctx *c) { RET((rd32(A0) >> 8) & 31); }
static void vstream_get_stride(Ctx *c) { RET(rd32(A0)); }
static void vstream_get_divisor(Ctx *c) { RET(rd32(A0 + 4)); }

/* ---- samplers: NVNsamplerBuilder and NVNsampler both hold a SamplerParams */
static void smpb_border_i(Ctx *c) { if (A1) { memcpy(SMP->border, P(A1), 16); SMP->border_kind = strstr(hostfn_current_name(), "ui") ? 1 : 2; } }
static void smpb_border_f(Ctx *c) { if (A1) { memcpy(SMP->border, P(A1), 16); SMP->border_kind = 0; } }
static void smpb_lod_snap(Ctx *c) { SMP->lod_snap = F0; }
static void smp_get_minmag(Ctx *c) { if (A1) wr32(A1, SMP->min); if (A2) wr32(A2, SMP->mag); }
static void smp_get_wrap(Ctx *c) { if (A1) wr32(A1, SMP->ws); if (A2) wr32(A2, SMP->wt); if (A3) wr32(A3, SMP->wr); }
static void smp_get_lodclamp(Ctx *c) { if (A1) memcpy(P(A1), &SMP->lodmin, 4); if (A2) memcpy(P(A2), &SMP->lodmax, 4); }
static void smp_get_lodbias(Ctx *c) { F0 = SMP->bias; }
static void smp_get_compare(Ctx *c) { if (A1) wr32(A1, SMP->cmode); if (A2) wr32(A2, SMP->cfunc); }
static void smp_get_border(Ctx *c) { if (A1) memcpy(P(A1), SMP->border, 16); }
static void smp_get_aniso(Ctx *c) { F0 = SMP->aniso; }
static void smp_get_reduction(Ctx *c) { RET(SMP->reduction); }
static void smp_get_lod_snap(Ctx *c) { F0 = SMP->lod_snap; }
static void smp_compare(Ctx *c) { RET(A0 && A1 && !memcmp(P(A0), P(A1), sizeof(SamplerParams))); }
static void smp_finalize(Ctx *c) { nvn_label_drop(A0); }      /* the parameters live in the guest object; OpenGL sampler objects are shared by value */
static void spool_finalize(Ctx *c) {
    gl_barrier();
    SamplerPool *sp = hobj(A0, K_SPOOL);
    if (!sp) return;
    AcquireSRWLockExclusive(&g_pool_lock);
    sp->count = 0;                                      /* the record itself stays: recorded SetSamplerPool commands may still name it */
    ReleaseSRWLockExclusive(&g_pool_lock);
    wr32(A0 + 8, 0);
    nvn_label_drop(A0);
}
static void spool_get_pool(Ctx *c) { SamplerPool *sp = hobj(A0, K_SPOOL); RET(sp ? sp->mempool : 0); }
static void spool_get_offset(Ctx *c) { SamplerPool *sp = hobj(A0, K_SPOOL); RET(sp ? sp->offset : 0); }
static void spool_get_size(Ctx *c) { SamplerPool *sp = hobj(A0, K_SPOOL); RET(sp ? sp->count : 0); }

/* ---- textures */
static void texb_get_swizzle(Ctx *c) { for (int i = 0; i < 4; i++) if (c->x[1 + i]) wr32(c->x[1 + i], TB->swz[i]); }
static void texb_get_dsmode(Ctx *c) { RET(TB->dsmode); }
static void texb_get_pool(Ctx *c) { RET(TB->pool); }
static void texb_get_offset(Ctx *c) { RET(TB->offset); }
static void texb_get_packaged(Ctx *c) { RET(TB->packaged); }
static void texb_get_layout(Ctx *c) { if (A1) memcpy(P(A1), TB->layout, 8); }
static void texb_get_stride(Ctx *c) { RET(TB->stride); }
static void texb_set_glname(Ctx *c) { TB->glname = (uint32_t)A1; }
static void texb_get_glname(Ctx *c) { RET(TB->glname); }
static void tex_get_swizzle(Ctx *c) { Tex *t = hobj(A0, K_TEX); for (int i = 0; t && i < 4; i++) if (c->x[1 + i]) wr32(c->x[1 + i], t->b.swz[i]); }
TEXGET(tex_get_pool, t->b.pool)
static void tex_compare(Ctx *c) {                       /* two textures are the same when they describe the same storage the same way */
    Tex *a = hobj(A0, K_TEX), *b = hobj(A1, K_TEX);
    RET(a && b && (a == b || !memcmp(&a->b, &b->b, sizeof a->b)));
}
static void tex_view_offset(Ctx *c) {                   /* (texture, view): where the view's first level and layer start in the texture's storage */
    Tex *t = hobj(A0, K_TEX);
    View v;
    TexLayout L;
    if (!t || !A1 || (t->b.flags & 0x10)) { RET(0); return; }
    memcpy(&v, P(A1), sizeof v);
    tex_lay(&t->b, &L);
    int level = (v.set & 0x81) == 0x81 ? v.lvl0 : 0, layer = (v.set & 0x82) == 0x82 ? v.lay0 : 0;
    if (level < 0 || level >= L.levels || layer < 0 || layer >= L.layers) { RET(0); return; }
    RET((uint64_t)(tex_level_ptr(&L, (const uint8_t *)0x1000, level, layer) - (const uint8_t *)0x1000));
}
/* ReadJob: nvn_front.h (V1 session 5: the Vulkan back end reads texels too) */
/* nvnTextureReadTexels: texels of a region as tightly packed rows (or with the given strides). nvnTextureInvalidateTexels: what
 * the GPU rendered or copied into the texture becomes readable in its pool memory, in the texture's own (block-linear) layout. */
static void tex_read_texels(Ctx *c) {                   /* (texture, view, region, out) [, rowStride, imageStride] */
    ReadJob j;
    memset(&j, 0, sizeof j);
    j.tex = hobj(A0, K_TEX); j.out = P(A3);
    if (!j.tex || !A2 || !A3) return;
    if (A1) { memcpy(&j.view, P(A1), sizeof j.view); j.has_view = 1; }
    memcpy(j.region, P(A2), 24);
    if (strstr(hostfn_current_name(), "Strided")) { j.row_stride = (int64_t)A4; j.image_stride = (int64_t)A5; }
    gl_run(g_rb->read_texels, &j);
}
static void tex_invalidate_texels(Ctx *c) {             /* (texture, view, region) */
    ReadJob j;
    memset(&j, 0, sizeof j);
    j.tex = hobj(A0, K_TEX); j.to_pool = 1;
    if (!j.tex) return;
    if (A1) { memcpy(&j.view, P(A1), sizeof j.view); j.has_view = 1; }
    gl_run(g_rb->read_texels, &j);
}
static void view_get_levels(Ctx *c) { if (A1) wr32(A1, (uint32_t)TV->lvl0); if (A2) wr32(A2, (uint32_t)TV->nlvl); RET((TV->set & 0x81) == 0x81); }
static void view_get_layers(Ctx *c) { if (A1) wr32(A1, (uint32_t)TV->lay0); if (A2) wr32(A2, (uint32_t)TV->nlay); RET((TV->set & 0x82) == 0x82); }
static void view_get_format(Ctx *c) { if (A1) wr32(A1, TV->format); RET((TV->set & 0x84) == 0x84); }
static void view_get_swizzle(Ctx *c) { for (int i = 0; i < 4; i++) if (c->x[1 + i]) wr32(c->x[1 + i], TV->swz[i]); RET((TV->set & 0x88) == 0x88); }
static void view_get_dsmode(Ctx *c) { if (A1) wr32(A1, TV->dsmode); RET((TV->set & 0x90) == 0x90); }
static void view_get_target(Ctx *c) { if (A1) wr32(A1, TV->target); RET((TV->set & 0xa0) == 0xa0); }
static void view_compare(Ctx *c) { RET(A0 && A1 && !memcmp(P(A0), P(A1), sizeof(View))); }
static void tpool_finalize(Ctx *c) {
    gl_barrier();
    TexPool *tp = hobj(A0, K_TPOOL);
    if (!tp) return;
    AcquireSRWLockExclusive(&g_pool_lock);
    tp->count = 0;                                      /* the record itself stays: recorded SetTexturePool commands may still name it */
    ReleaseSRWLockExclusive(&g_pool_lock);
    wr32(A0 + 8, 0);
    nvn_label_drop(A0);
}
static void tpool_get_pool(Ctx *c) { TexPool *tp = hobj(A0, K_TPOOL); RET(tp ? tp->mempool : 0); }
static void tpool_get_offset(Ctx *c) { TexPool *tp = hobj(A0, K_TPOOL); RET(tp ? tp->offset : 0); }
static void tpool_get_size(Ctx *c) { TexPool *tp = hobj(A0, K_TPOOL); RET(tp ? tp->count : 0); }

/* ---- programs */
static void prog_sample_shading(Ctx *c) { Prog *p = hobj(A0, K_PROG); if (p) p->sample_shading = (int)A1; }    /* 0 as the shader says, 1 forced on, 2 forced off */

/* ---- window */
static void wb_defaults(Ctx *c) { memset(P(A0), 0, 0x40); wr8(A0 + 4, 1); }
static void wb_native(Ctx *c) { wr64(A0 + 56, A1); }
static void wb_interval(Ctx *c) { wr8(A0 + 4, (uint8_t)A1); }
static void wb_num_active(Ctx *c) { wr8(A0 + 5, (uint8_t)A1); }
static void wb_get_native(Ctx *c) { RET(rd64(A0 + 56)); }
static void wb_get_interval(Ctx *c) { RET(rd8(A0 + 4)); }
static void wb_get_num_textures(Ctx *c) { RET(rd32(A0) & 0xff); }
static void wb_get_num_active(Ctx *c) { RET(rd8(A0 + 5) ? rd8(A0 + 5) : (rd32(A0) & 0xff)); }
static void wb_get_texture(Ctx *c) { RET((uint32_t)A1 < 6 ? rd64(A0 + 8 + (A1 & 7) * 8) : 0); }
static void win_finalize(Ctx *c) { gl_barrier(); Window *w = hobj(A0, K_WIN); if (w) { wr32(A0 + 8, 0); w->count = 0; } nvn_label_drop(A0); }
static void win_set_interval(Ctx *c) { Window *w = hobj(A0, K_WIN); if (w) { w->interval = (int)A1; display_set_interval((int)A1); } }
static void win_get_interval(Ctx *c) { Window *w = hobj(A0, K_WIN); RET(w ? w->interval : 1); }
static void win_get_native(Ctx *c) { Window *w = hobj(A0, K_WIN); RET(w ? w->native : 0); }
static void win_get_num_textures(Ctx *c) { Window *w = hobj(A0, K_WIN); RET(w ? w->count : 0); }
static void win_get_num_active(Ctx *c) { Window *w = hobj(A0, K_WIN); RET(w ? (w->nactive ? w->nactive : w->count) : 0); }
static void win_set_num_active(Ctx *c) { Window *w = hobj(A0, K_WIN); if (w && (int)A1 >= 1 && (int)A1 <= w->count) w->nactive = (int)A1; }
static void win_get_crop(Ctx *c) {                      /* (window, NVNrectangle *): the crop set, or the whole texture */
    Window *w = hobj(A0, K_WIN);
    if (!w || !A1) return;
    if (w->crop[2] > 0 && w->crop[3] > 0) memcpy(P(A1), w->crop, 16);
    else { int r[4] = {0, 0, w->count && w->tex[0] ? w->tex[0]->b.width : 0, w->count && w->tex[0] ? w->tex[0]->b.height : 0}; memcpy(P(A1), r, 16); }
}

/* ---- command buffers: state kept while recording */
static void cmd_set_callback(Ctx *c) { cb_get(A0)->callback = A1; }
static void cmd_set_callback_data(Ctx *c) { cb_get(A0)->callback_data = A1; }
static void cmd_get_callback(Ctx *c) { RET(cb_get(A0)->callback); }
static void cmd_get_callback_data(Ctx *c) { RET(cb_get(A0)->callback_data); }
static void cmd_callback_enabled(Ctx *c) { cb_get(A0)->callback_off = !(A1 & 1); }
static void cmd_get_command_used(Ctx *c) { RET(cb_get(A0)->cmd_used); }
static void cmd_get_control_used(Ctx *c) { RET(cb_get(A0)->ctrl_used); }
static void cmd_get_command_free(Ctx *c) { CmdBuf *cb = cb_get(A0); RET(cb->cmd_size > cb->cmd_used ? cb->cmd_size - cb->cmd_used : 0); }
static void cmd_get_control_free(Ctx *c) { CmdBuf *cb = cb_get(A0); RET(cb->ctrl_size > cb->ctrl_used ? cb->ctrl_size - cb->ctrl_used : 0); }
static void cmd_get_row_stride(Ctx *c) { RET(cb_get(A0)->row_stride); }
static void cmd_get_image_stride(Ctx *c) { RET(cb_get(A0)->image_stride); }
static void cmd_image_stride(Ctx *c) { cb_get(A0)->image_stride = (int64_t)A1; rec(c, OP_IMAGESTRIDE, 0); }
static void cmd_depth_bounds(Ctx *c) { float v[2] = {F0, F1}; Cmd *m = rec(c, OP_DEPTHBOUNDS, 0); memcpy(&m->a[1], v, 8); }       /* (cmdbuf, enable, near [v0], far [v1]) */
static void cmd_cons_raster(Ctx *c) { rec(c, OP_CONSRASTER, 0); }
static void cmd_cons_dilate(Ctx *c) { float v = F0; Cmd *m = rec(c, OP_CONSDILATE, 0); memcpy(&m->a[0], &v, 4); }
static void cmd_subpixel_bias(Ctx *c) { rec(c, OP_SUBPIXEL, 0); }
static void cmd_viewport_swizzles(Ctx *c) { int n = (int)A2; uint64_t src = A3; Cmd *m = rec(c, OP_VPSWIZZLE, (size_t)n * 16); if (n > 0 && src) memcpy(BLOB(m), P(src), (size_t)n * 16); }
static void cmd_tess_levels(Ctx *c) {                   /* (cmdbuf, const float *levels): 2 inner or 4 outer */
    int outer = strstr(hostfn_current_name(), "Outer") != NULL;
    uint64_t src = A1;
    Cmd *m = rec(c, OP_TESSLEVELS, 16);
    m->a[0] = (uint64_t)outer;
    memset(BLOB(m), 0, 16);
    if (src) memcpy(BLOB(m), P(src), outer ? 16 : 8);
}
static void cmd_barrier(Ctx *c) { rec(c, OP_BARRIER, 0); }
static void cmd_discard_color(Ctx *c) { rec(c, OP_DISCARD, 0); }
static void cmd_discard_ds(Ctx *c) { rec(c, OP_DISCARD, 0)->a[0] = ~0ull; }
static void cmd_clear_color_typed(Ctx *c) {             /* ClearColori / ClearColorui (cmdbuf, index, const int *, mask) */
    uint64_t src = A2;
    int kind = strstr(hostfn_current_name(), "ui") ? 2 : 1;
    Cmd *m = rec(c, OP_CLEARCOLOR, 16);
    m->a[5] = (uint64_t)kind;
    if (src) memcpy(BLOB(m), P(src), 16); else memset(BLOB(m), 0, 16);
}
static void cmd_clear_texture(Ctx *c) {                 /* (cmdbuf, texture, view, region, color, mask); the i / ui variants pass integers */
    uint64_t view = A2, region = A3, color = A4, mask = A5;
    const char *name = hostfn_current_name();
    int kind = strstr(name, "Textureui") ? 2 : strstr(name, "Texturei") ? 1 : 0;
    Tex *t = hobj(A1, K_TEX);
    Cmd *m = rec(c, OP_CLEARTEX, 64);
    m->a[0] = (uint64_t)(uintptr_t)t; m->a[4] = mask; m->a[5] = (uint64_t)kind;
    memset(BLOB(m), 0, 64);
    if (view) memcpy(BLOB(m), P(view), sizeof(View));
    if (region) memcpy(BLOB(m) + 24, P(region), 24); else m->a[6] = 1;
    if (color) memcpy(BLOB(m) + 48, P(color), 16);
}
static void cmd_downsample_view(Ctx *c) {               /* (cmdbuf, src, srcView, dst, dstView) */
    uint64_t sv = A2, dv = A4;
    Tex *s = hobj(A1, K_TEX), *d = hobj(A3, K_TEX);
    Cmd *m = rec(c, OP_DOWNSAMPLE_VIEW, 48);
    m->a[0] = (uint64_t)(uintptr_t)s; m->a[1] = (uint64_t)(uintptr_t)d;
    memset(BLOB(m), 0, 48);
    if (sv) memcpy(BLOB(m), P(sv), sizeof(View));
    if (dv) memcpy(BLOB(m) + 24, P(dv), sizeof(View));
}
static void cmd_draw_arrays_indirect(Ctx *c) { Cmd *m = rec(c, OP_DRAW_INDIRECT, 0); m->a[6] = 0; m->a[4] = 1; m->a[5] = 0; m->a[3] = m->a[1]; m->a[1] = 0; m->a[2] = 0; }      /* (cmdbuf, mode, indirect) */
static void cmd_draw_elements_indirect(Ctx *c) { Cmd *m = rec(c, OP_DRAW_INDIRECT, 0); m->a[6] = 1; m->a[4] = 1; m->a[5] = 0; }   /* (cmdbuf, mode, type, indexBuffer, indirect) */
static void cmd_multi_draw_arrays_indirect(Ctx *c) {    /* (cmdbuf, mode, indirect, parameter, maxDrawCount, stride) */
    uint64_t indirect = A2, param = A3, maxn = A4, stride = A5;
    Cmd *m = rec(c, OP_DRAW_INDIRECT, 8);
    m->a[1] = 0; m->a[2] = 0; m->a[3] = indirect; m->a[4] = maxn; m->a[5] = stride; m->a[6] = 2;
    memcpy(BLOB(m), &param, 8);
}
static void cmd_multi_draw_elements_indirect(Ctx *c) {  /* (cmdbuf, mode, type, indexBuffer, indirect, parameter, maxDrawCount, stride) */
    uint64_t indirect = A4, param = A5, maxn = A6, stride = A7, index = A3;
    Cmd *m = rec(c, OP_DRAW_INDIRECT, 8);
    m->a[2] = index; m->a[3] = indirect; m->a[4] = maxn; m->a[5] = stride; m->a[6] = 3;
    memcpy(BLOB(m), &param, 8);
}
static void cmd_dispatch_indirect(Ctx *c) { cb_get(A0)->dispatches++; rec(c, OP_DISPATCH_INDIRECT, 0); }       /* (cmdbuf, address of three group counts) */
static void cmd_render_enable(Ctx *c) { Cmd *m = rec(c, OP_RENDER_ENABLE, 0); m->a[1] = 0; m->a[2] = 0; }                      /* (cmdbuf, enable) */
static void cmd_render_enable_cond(Ctx *c) { Cmd *m = rec(c, OP_RENDER_ENABLE, 0); m->a[2] = m->a[1]; m->a[1] = m->a[0] + 1; }   /* (cmdbuf, mode, counter address) */
static void cmd_reset_counter(Ctx *c) { rec(c, OP_COUNTER_RESET, 0); }
static void cmd_report_counter(Ctx *c) { rec(c, OP_COUNTER_REPORT, 0); }
static void cmd_report_value(Ctx *c) { rec(c, OP_REPORT_VALUE, 0); }
static void cmd_fence_sync(Ctx *c) { SyncObj *o = hobj(A1, K_SYNC); rec(c, OP_FENCE, 0)->a[0] = (uint64_t)(uintptr_t)o; }      /* (cmdbuf, sync, condition, flags) */
static void cmd_wait_sync(Ctx *c) { SyncObj *o = hobj(A1, K_SYNC); rec(c, OP_WAITSYNC, 0)->a[0] = (uint64_t)(uintptr_t)o; }
static void cmd_signal_event(Ctx *c) { uint64_t a = A1 ? rd64(A1 + 16) : 0; rec(c, OP_EVENT_SIGNAL, 0)->a[0] = a; }            /* (cmdbuf, event, mode, location, flags, value) */
static void cmd_wait_event(Ctx *c) { uint64_t a = A1 ? rd64(A1 + 16) : 0; rec(c, OP_EVENT_WAIT, 0)->a[0] = a; }                /* (cmdbuf, event, waitMode, value) */
/* nvnCommandBufferDrawTexture(cmdbuf, textureHandle, dstRegion, srcRegion) [sdk 0x438210]: see nvn_c4.c. The handle is the one
 * nvnDeviceGetTextureHandle gives (texture ID | sampler ID << 20, bit 32 set). An empty destination records nothing, as the
 * real encoder writes nothing [0x43827c]. */
static void cmd_draw_texture(Ctx *c) {
    uint64_t h = A1, dst = A2, src = A3;
    float d[4], s[4];
    NvnC4DrawTexture w;
    memcpy(d, P(dst), 16); memcpy(s, P(src), 16);
    if (!nvn_c4_draw_texture_words(d, s, &w)) return;
    w.src_texture = (uint32_t)h & 0xfffff; w.src_sampler = (uint32_t)h >> 20;   /* [sdk 0x4382f0, 0x4382f4] */
    Cmd *m = rec(c, OP_DRAW_TEXTURE, sizeof w);
    m->a[0] = h;
    memcpy(BLOB(m), &w, sizeof w);
}
static void cmd_covmod_table(Ctx *c) { uint64_t src = A1; Cmd *m = rec(c, OP_COVMODTABLE, 64); m->a[0] = src != 0; if (src) memcpy(BLOB(m), P(src), 64); }
static void rec_debug(Ctx *c, int kind, uint64_t id, uint64_t text) {
    size_t n = text ? strnlen(P(text), 255) : 0;
    Cmd *m = rec(c, OP_DEBUG, n + 1);
    m->a[0] = (uint64_t)kind; m->a[1] = id;
    if (n) memcpy(BLOB(m), P(text), n);
    BLOB(m)[n] = 0;
}
static void cmd_push_group(Ctx *c) { rec_debug(c, 0, A1, A2); }               /* (cmdbuf, domain, description) */
static void cmd_pop_group(Ctx *c) { rec_debug(c, 1, 0, 0); }
static void cmd_marker(Ctx *c) { rec_debug(c, 2, 0, A1); }                    /* (cmdbuf, description) */
static void cmd_marker_id(Ctx *c) { rec_debug(c, 2, A1, A2); }                /* (cmdbuf, domain, description) */
static void cmd_set_label(Ctx *c) { cb_get(A0); any_set_label(c); }

static void job_finalize_handle(void *arg) {
    uint64_t h = *(uint64_t *)arg;
    uint32_t i = (uint32_t)h - 1;
    AcquireSRWLockExclusive(&g_list_lock);
    if (h && i < (uint32_t)g_list_count && g_lists[i].data && (g_lists[i].gen & 0x7fffffff) == (uint32_t)(h >> 32)) g_lists[i].finalized = 1;
    ReleaseSRWLockExclusive(&g_list_lock);
}
static void dev_finalize_command_handle(Ctx *c) {       /* (device, handle): the recorded commands of that handle are released (by the render thread, at the next present) */
    uint64_t h = A1;
    { int r1_ra_phase(void); if (r1_ra_phase() == 1) return; }      /* R1 round 5, run-ahead: a speculative pass's release is undone by the restore (the real pass asks again); its own lists expire unused */
    if (gl_async_on() && g_async_pending) gl_post(job_finalize_handle, &h, sizeof h);     /* after the submits already queued, which may still use it */
    else job_finalize_handle(&h);
}
/* ================================================================ V1 step S0: the render thread's jobs in Vulkan mode
 * (plan\phases\V1_vulkan.md, section 1). Every job the entry points above hand to the render thread arrives here when
 * SSBU_RENDERER=vk. A job nvn_vk.c implements is done there; one it does not implement yet stops the game with the name
 * of the NVN operation (nvn_vk_missing; SSBU_VK_MISSING=warn: reported once, counted, skipped). A job that is not one
 * of this file's (nvn_vk.c's own, through nvn_render_run) runs as it is. Step S2 replaces this table by the RBackend
 * interface. */
void nvn_render_run(void (*fn)(void *), void *arg) { gl_run(fn, arg); }
/* PR #41: pool / buffer finalization may be followed at once by a decommit or reuse of the guest's storage. A queue barrier
 * alone is not enough on Vulkan: kept compute buffers and asynchronous landings outlive the jobs that used them. */
void nvn_front_memory_retire(uint64_t addr, uint64_t size) {
    if (!addr || !size || !g_render_tid) return;
    if (g_rb == &rb_vk) { Range range = {addr, size}; gl_run(nvn_vk_memory_retire, &range); }
    else gl_barrier();
}

/* ---- front-end services for the Vulkan back end (nvn_front.h): thin doors to the statics above. Render thread only,
 * except where the callee has its own lock. */
int nvn_front_fmt(int nvn_format, Fmt *f) { return fmt_get(nvn_format, f); }
uint8_t *nvn_front_list(uint64_t handle, size_t *len) { return list_lookup(handle, len); }
/* Track V session 4, step-aligned comparison of the two renderers: with SSBU_SHOT_STEPS=1 the scripted input (nnhid.c)
 * asks for a screenshot when a key step of SSBU_SEQ begins; the next presented frame is saved as step_<n>.bmp in
 * SSBU_SHOT_DIR by whichever back end runs. Counted in input polls, so both renderers save the same game frame. */
static volatile LONG g_step_shot;
void nvn_front_shot_step(int step) { static int on = -1; if (on < 0) on = getenv("SSBU_SHOT_STEPS") != NULL; if (on) InterlockedExchange(&g_step_shot, step + 1); }
int nvn_front_take_step_shot(void) { return g_step_shot ? (int)InterlockedExchange(&g_step_shot, 0) : 0; }
void nvn_front_batch_begin(void) { g_serial++; }
void nvn_front_barrier_texmem(void) { g_serial++; }
int nvn_front_tex_layers(const Tex *t) { return tex_layers(&t->b); }
int nvn_front_tex_levels(const Tex *t) {
    const TexBuilder *b = &t->b;
    int w = b->width > 0 ? b->width : 1, h = b->height > 0 ? b->height : 1, d = b->depth > 0 ? b->depth : 1, levels = b->levels > 0 ? b->levels : 1, maxdim = w > h ? w : h, maxl = 1;
    if (b->target == 2 && d > maxdim) maxdim = d;
    while ((maxdim >> maxl) > 0) maxl++;
    return b->samples > 1 ? 1 : levels > maxl ? maxl : levels;
}
/* The decision of tex_sync without its OpenGL calls (the same rule, the same fields of Tex). */
int nvn_front_tex_changed(Tex *t) {
    Fmt f;
    if (t->checked == g_serial) return 0;
    t->checked = g_serial;
    const uint8_t *src = tex_source(t);
    if (t->gpu_written || !src || t->b.samples > 1 || !fmt_get(t->b.format, &f) || (f.kind >= 1 && f.kind <= 3)) return 0;
    if (!t->b.packaged && t->limit && src + t->data_size > t->limit) return 0;
    if (t->b.packaged && t->uploaded) return 0;
    LONG64 pf0 = pf_now();
    uint64_t newest = ww_newest(src, t->data_size);
    if (newest) {
        if (t->uploaded && !t->dirty && t->ww_hashed && newest <= t->ww_seen) { g_ww_clean++; pf_add(PF_TEXHASH, pf0); return 0; }
        t->ww_seen = newest;
        g_ww_dirty++;
    } else g_ww_unwatched++;
    uint64_t h = mem_hash(src, t->data_size, newest != 0 || (t->b.flags & 0x10) != 0);
    int same = t->uploaded && !t->dirty && h == t->hash && (!newest || t->ww_hashed);
    t->ww_hashed = newest != 0;
    pf_add(PF_TEXHASH, pf0); g_pf_hash_bytes += (LONG64)t->data_size;
    if (same) return 0;
    t->hash = h; t->uploaded = 1; t->dirty = 0;
    g_st.tex_uploads++;
    return 1;
}
/* tex_upload_memory with the texels handed to the caller instead of glTexSubImage. */
int nvn_front_tex_read(Tex *t, int decode, nvn_front_put_fn *put) {
    static uint8_t *tmp;
    static size_t tmp_cap;
    Fmt f;
    TexBuilder *b = &t->b;
    const uint8_t *src = tex_source(t);
    if (!src || !fmt_get(b->format, &f)) return 0;
    if (b->flags & 0x10) {                         /* linear: plain rows with a stride */
        int64_t stride = b->stride ? b->stride : (int64_t)b->width * f.bpb;
        if (f.kind == 4) return 0;
        put(t, 0, 0, b->width, b->height, 1, src, (size_t)stride * (size_t)b->height, 0, stride);
        g_st.tex_bytes += stride * b->height;
        tex_release_packaged(t);
        return 1;
    }
    TexLayout L;
    tex_lay(b, &L);
    int levels = nvn_front_tex_levels(t);
    for (int layer = 0; layer < L.layers; layer++) {
        for (int l = 0; l < levels && l < L.levels; l++) {
            const uint8_t *p = tex_level_ptr(&L, src, l, layer);
            size_t lin = L.lvl_linear_size[l], need = lin + (decode ? (size_t)L.lvl_w[l] * L.lvl_h[l] * 4 : 0);
            if (!b->packaged && t->limit && p + L.lvl_size[l] > t->limit) return 0;
            if (need > tmp_cap) { tmp_cap = need * 2; tmp = realloc(tmp, tmp_cap); }
            tex_deswizzle_level(&L, l, p, tmp);
            if (decode) {
                tex_decode_rgba8(b->format, tmp, L.lvl_w[l], L.lvl_h[l], tmp + lin);
                put(t, l, layer, L.lvl_w[l], L.lvl_h[l], 1, tmp + lin, (size_t)L.lvl_w[l] * L.lvl_h[l] * 4, 1, 0);
            } else put(t, l, layer, L.lvl_w[l], L.lvl_h[l], L.lvl_d[l], tmp, lin, 0, 0);
            g_st.tex_bytes += (LONG64)lin;
        }
    }
    tex_release_packaged(t);
    return 1;
}
int nvn_front_handle(TexPool *tp, SamplerPool *sp, uint64_t nh, Tex **t, View *v, SamplerParams *p) {
    uint32_t tid = (uint32_t)nh & 0xfffff, sid = (uint32_t)(nh >> 20) & 0xfff;
    int r = 0;
    *t = NULL;
    if (!nh || !tp) return 0;
    AcquireSRWLockShared(&g_pool_lock);
    if (tid < (uint32_t)tp->count) { *t = tp->e[tid].tex; *v = tp->e[tid].view; }
    if (sp && sid < (uint32_t)sp->count && sp->used[sid]) { *p = sp->e[sid]; r |= 2; }
    ReleaseSRWLockShared(&g_pool_lock);
    if (*t && !(*t)->dead) r |= 1; else *t = NULL;
    return r;
}
long long nvn_front_frame_next(void) { return (long long)g_st.frames + 1; }
/* V1 session 5: compute outputs of the two back ends compared OFFLINE (tests/vk5_ccmp.py). SSBU_COMPUTE_CMP=<folder>: every
 * dispatch is named by a hash of what it reads from guest memory (machine code, group counts, every bound uniform buffer,
 * every bound storage buffer it does not write; of the written ones the size); when what it wrote has landed in guest memory
 * the written ranges go to <folder>/<hash>_<storage index>.bin (the first SSBU_COMPUTE_CMP_N distinct hashes, default 600).
 * Two runs (vk, gl) that met the same inputs wrote files of the same name. Render thread only. */
static struct { uint64_t h, addr, size; int n; } g_ccmp_pend[512];
static int g_ccmp_npend, g_ccmp_on = -1, g_ccmp_left;
static uint64_t g_ccmp_seen[4096];
static const char *g_ccmp_dir;
static uint64_t ccmp_hash(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, b + i, 8); h = (h ^ w) * 0x9E3779B97F4A7C15ULL; h ^= h >> 29; }
    for (; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ULL;
    return h;
}
void nvn_front_ccmp_dispatch(const Range *ubo, const Range *ssbo, uint32_t written, const uint8_t *code, int code_len, unsigned gx, unsigned gy, unsigned gz) {
    if (g_ccmp_on < 0) { g_ccmp_dir = getenv("SSBU_COMPUTE_CMP"); g_ccmp_on = g_ccmp_dir != NULL; g_ccmp_left = getenv("SSBU_COMPUTE_CMP_N") ? atoi(getenv("SSBU_COMPUTE_CMP_N")) : 600; if (g_ccmp_on) CreateDirectoryA(g_ccmp_dir, NULL); }
    if (!g_ccmp_on || g_ccmp_left <= 0 || !code || code_len <= 0) return;
    uint32_t g[3] = {gx, gy, gz};
    uint64_t h = ccmp_hash(0xcbf29ce484222325ULL, code, (size_t)code_len);
    h = ccmp_hash(h, g, sizeof g);
    for (int n = 0; n < 16; n++) if (ubo[n].addr && ubo[n].size) { h = ccmp_hash(h ^ (uint64_t)(n + 1), P(ubo[n].addr), (size_t)ubo[n].size); }
    for (int n = 0; n < 32; n++) {
        if (!ssbo[n].addr || !ssbo[n].size) continue;
        /* SSBU_COMPUTE_CMP_MODE=1: storage buffers enter by SIZE only, written or not. Needed for skinning program #16: its
         * assembly text (OpenGL) declares storage 2 written, the translated machine code (Vulkan) 0, 1 and 2, so the
         * default naming never matches between the two. A false match (same sizes, other contents) shows as a difference. */
        static int mode = -1;
        if (mode < 0) mode = getenv("SSBU_COMPUTE_CMP_MODE") ? atoi(getenv("SSBU_COMPUTE_CMP_MODE")) : 0;
        if (mode == 1 || (written >> n & 1)) h = ccmp_hash(h ^ (uint64_t)(n + 101), &ssbo[n].size, 8);
        else h = ccmp_hash(h ^ (uint64_t)(n + 201), P(ssbo[n].addr), (size_t)ssbo[n].size);
    }
    unsigned slot = (unsigned)(h >> 17) & 4095;
    if (g_ccmp_seen[slot] == h) return;
    g_ccmp_seen[slot] = h;
    g_ccmp_left--;
    for (int n = 0; n < 32; n++) if ((written >> n & 1) && ssbo[n].addr && ssbo[n].size && g_ccmp_npend < 512) { g_ccmp_pend[g_ccmp_npend].h = h; g_ccmp_pend[g_ccmp_npend].addr = ssbo[n].addr; g_ccmp_pend[g_ccmp_npend].size = ssbo[n].size; g_ccmp_pend[g_ccmp_npend++].n = n; }
}
void nvn_front_ccmp_landed(void) {
    for (int i = 0; i < g_ccmp_npend; i++) {
        char path[400];
        snprintf(path, sizeof path, "%s/%016llx_%d.bin", g_ccmp_dir, (unsigned long long)g_ccmp_pend[i].h, g_ccmp_pend[i].n);
        FILE *fp = fopen(path, "wb");
        if (fp) { fwrite(P(g_ccmp_pend[i].addr), 1, (size_t)g_ccmp_pend[i].size, fp); fclose(fp); }
    }
    g_ccmp_npend = 0;
}
long long nvn_front_frame_end(long long draws, long long skipped, long long clears) {
    LONG64 frame = g_st.frames + 1;
    pf_frame_mark(frame);
    g_joblog_cur = frame;
    g_st.frames = frame;
    if (frame <= 3 || frame % 600 == 0 || draws != 0 && g_logn < 40 && (g_logn++, 1))
        printf("nvn_vk: frame %lld: %lld draws (%lld skipped), %lld clears | totals: %lld lists, %lld textures, %lld uploads (%lld MB)\n", (long long)frame, draws, skipped, clears,
               (long long)g_st.lists, (long long)g_st.tex_created, (long long)g_st.tex_uploads, (long long)(g_st.tex_bytes >> 20));
    {   static ULONGLONG t0;
        static LONG64 d0;
        if (frame % 300 == 0) {
            ULONGLONG now = GetTickCount64();
            if (t0 && now > t0) printf("nvn_vk: frames %lld..%lld: %.1f fps, %lld draws per frame\n", (long long)frame - 300, (long long)frame, 300000.0 / (double)(now - t0), (long long)(g_st.draws - d0) / 300);
            t0 = now; d0 = g_st.draws;
        }
    }
    fflush(stdout);
    g_prev_frame_draws = draws;
    input_frame_draws((int)draws);
    list_collect();
    return (long long)frame;
}
void nvn_front_take_dead(void (*drop)(int kind, void *host)) {
    if (!g_dead_n) return;
    EnterCriticalSection(&g_dead_lock);
    for (int i = 0; i < g_dead_n; i++) {
        drop(g_dead[i].kind, g_dead[i].p);
        if (g_dead[i].kind == K_TEX) tex_release_packaged(g_dead[i].p);
        if (g_dead[i].kind == K_PROG) { Prog *p = g_dead[i].p; for (int s = 0; s < 6; s++) { free(p->st[s].text); memset(&p->st[s], 0, sizeof p->st[s]); } }
    }
    g_dead_n = 0;
    LeaveCriticalSection(&g_dead_lock);
}
void nvn_front_count(int what, long long n) {
    switch (what) { case 0: g_st.draws += n; break; case 1: g_st.draws_skipped += n; break; case 2: g_st.clears += n; break; case 3: g_st.submits += n; break; case 5: g_st.tex_created += n; break; }
}
int nvn_front_clip_modes(void) { return g_origin_mode | g_depth_mode << 8; }
const char *nvn_front_op_name(unsigned op) { return k_op_name[op < OP_COUNT ? op : 0]; }
void *nvn_front_hwnd(void) { return g_hwnd; }
static void job_vk_shutdown(void *arg) { (void)arg; nvn_vk_shutdown(); }
/* At exit (nvn_gl_report): the render thread destroys the Vulkan objects. It is given 3 s: a render thread that is held
 * in a job at that moment is left alone (the process is ending) and that is said. */
static void vk_shutdown_at_exit(void) {
    static Job j;
    if (GetCurrentThreadId() == g_render_tid) { nvn_vk_shutdown(); return; }
    j.fn = job_vk_shutdown; j.arg = NULL; j.next = NULL; j.done = CreateEventA(NULL, FALSE, FALSE, NULL);
    EnterCriticalSection(&g_job_lock);
    if (g_job_tail) g_job_tail->next = &j; else g_job_head = &j;
    g_job_tail = &j;
    LeaveCriticalSection(&g_job_lock);
    SetEvent(g_job_event);
    /* Session 13: the event is set when the job has FINISHED, not when it is taken, and the job writes the pipeline cache
     * (0.3-0.7 s read-out + 1-2 s file for 0.9 GB, more on a busy PC): 3 s was too short and the old line ("did not take
     * the job") was printed while the shutdown was running; the process then left with the cache half written. Now: 60 s. */
    if (WaitForSingleObject(j.done, 3000) != WAIT_OBJECT_0) {
        LONG64 t0 = GetTickCount64();
        printf("nvn_vk: shutdown still running after 3 s (the pipeline cache is being written); waiting for it, at most 60 s\n"); fflush(stdout);
        if (WaitForSingleObject(j.done, 60000) != WAIT_OBJECT_0) printf("nvn_vk: shutdown NOT finished after 63 s: the render thread is held or never took the job\n");
        else printf("nvn_vk: shutdown finished %.1f s after the first 3 s\n", (double)(GetTickCount64() - t0) / 1000.0);
        fflush(stdout);
    }
}

/* nvnBufferFlushMappedRange / nvnMemoryPoolFlushMappedRange (nvn_stub.c): the CPU wrote this memory. Textures stored there are read again. */
Tex *nvn_front_tex_first(void) { return g_all_tex; }
void nvn_gl_memory_written(uint64_t addr, uint64_t size) {
    if (!size) return;
    uint64_t end = tex_range_end(addr, size);
    AcquireSRWLockShared(&g_tex_range_lock);
    if (g_tex_range_fallback) {
        for (Tex *t = g_all_tex; t; t = t->all_next)
            if (!t->dead && t->mem && (uint64_t)(uintptr_t)t->mem < end && addr < tex_range_end((uint64_t)(uintptr_t)t->mem, t->storage)) t->dirty = 1;
    } else {
        size_t lo = 0, hi = g_tex_range_count;
        while (lo < hi) { size_t mid = lo + (hi - lo) / 2; if (g_tex_ranges[mid].prefix_end <= addr) lo = mid + 1; else hi = mid; }
        for (size_t i = lo; i < g_tex_range_count && g_tex_ranges[i].begin < end; i++)
            if (g_tex_ranges[i].end > addr) g_tex_ranges[i].tex->dirty = 1;
    }
    ReleaseSRWLockShared(&g_tex_range_lock);
}

static const struct { const char *name; ImportFn fn; } k_nvn[] = {
    {"nvnDeviceSetWindowOriginMode", dev_origin_mode}, {"nvnDeviceSetDepthMode", dev_depth_mode}, {"nvnDeviceGetTextureHandle", dev_texture_handle},
    {"nvnDeviceGetTexelFetchHandle", dev_texel_fetch_handle}, {"nvnDeviceGetImageHandle", dev_image_handle},
    {"nvnProgramInitialize", prog_initialize}, {"nvnProgramFinalize", prog_finalize}, {"nvnProgramSetShaders", prog_set_shaders}, {"nvnProgramSetShadersExt", prog_set_shaders},
    {"nvnSamplerBuilderSetDefaults", smpb_defaults}, {"nvnSamplerBuilderSetMinMagFilter", smpb_minmag}, {"nvnSamplerBuilderSetWrapMode", smpb_wrap},
    {"nvnSamplerBuilderSetLodClamp", smpb_lodclamp}, {"nvnSamplerBuilderSetLodBias", smpb_lodbias}, {"nvnSamplerBuilderSetCompare", smpb_compare},
    {"nvnSamplerBuilderSetMaxAnisotropy", smpb_aniso}, {"nvnSamplerBuilderSetReductionFilter", smpb_reduction},
    {"nvnSamplerInitialize", smp_initialize},     {"nvnSamplerPoolInitialize", spool_initialize}, {"nvnSamplerPoolRegisterSampler", spool_register}, {"nvnSamplerPoolRegisterSamplerBuilder", spool_register},
        {"nvnTextureBuilderSetDefaults", texb_defaults}, {"nvnTextureBuilderSetFlags", texb_flags}, {"nvnTextureBuilderSetTarget", texb_target},
    {"nvnTextureBuilderSetFormat", texb_format}, {"nvnTextureBuilderSetWidth", texb_width}, {"nvnTextureBuilderSetHeight", texb_height}, {"nvnTextureBuilderSetDepth", texb_depth},
    {"nvnTextureBuilderSetSize1D", texb_size1d}, {"nvnTextureBuilderSetSize2D", texb_size2d}, {"nvnTextureBuilderSetSize3D", texb_size3d}, {"nvnTextureBuilderSetLevels", texb_levels},
    {"nvnTextureBuilderSetSamples", texb_samples}, {"nvnTextureBuilderSetSwizzle", texb_swizzle}, {"nvnTextureBuilderSetDepthStencilMode", texb_dsmode},
    {"nvnTextureBuilderSetStorage", texb_storage}, {"nvnTextureBuilderSetPackagedTextureData", texb_packaged}, {"nvnTextureBuilderSetPackagedTextureLayout", texb_layout},
    {"nvnTextureBuilderSetStride", texb_stride}, {"nvnTextureBuilderGetStorageSize", texb_storage_size}, {"nvnTextureBuilderGetStorageAlignment", texb_storage_align},
    {"nvnTextureBuilderGetFlags", texb_get_flags}, {"nvnTextureBuilderGetTarget", texb_get_target}, {"nvnTextureBuilderGetFormat", texb_get_format},
    {"nvnTextureBuilderGetWidth", texb_get_width}, {"nvnTextureBuilderGetHeight", texb_get_height}, {"nvnTextureBuilderGetDepth", texb_get_depth},
    {"nvnTextureBuilderGetLevels", texb_get_levels}, {"nvnTextureBuilderGetSamples", texb_get_samples},
    {"nvnTextureInitialize", tex_initialize}, {"nvnTextureFinalize", tex_finalize}, {"nvnTextureGetFlags", tex_get_flags},
    {"nvnTextureGetTarget", tex_get_target}, {"nvnTextureGetFormat", tex_get_format}, {"nvnTextureGetWidth", tex_get_width}, {"nvnTextureGetHeight", tex_get_height},
    {"nvnTextureGetDepth", tex_get_depth}, {"nvnTextureGetLevels", tex_get_levels}, {"nvnTextureGetSamples", tex_get_samples}, {"nvnTextureGetStride", tex_get_stride},
    {"nvnTextureGetStorageSize", tex_get_storage_size}, {"nvnTextureGetMemoryOffset", tex_get_memory_offset}, {"nvnTextureGetTextureAddress", tex_get_address},
    {"nvnTextureBuilderGetStorageClass", texb_storage_class}, {"nvnTextureBuilderGetRawStorageClass", texb_raw_storage_class}, {"nvnTextureBuilderGetSparseTileLayout", texb_sparse_layout},
    {"nvnTextureGetStorageClass", tex_storage_class}, {"nvnTextureGetRawStorageClass", tex_raw_storage_class}, {"nvnTextureGetSparseTileLayout", tex_sparse_layout},
    {"nvnTextureGetDepthStencilMode", tex_get_dsmode},     {"nvnTextureWriteTexels", tex_write_texels}, {"nvnTextureWriteTexelsStrided", tex_write_texels}, {"nvnTextureFlushTexels", tex_flush_texels},
    {"nvnTextureViewSetDefaults", view_defaults}, {"nvnTextureViewSetLevels", view_levels}, {"nvnTextureViewSetLayers", view_layers}, {"nvnTextureViewSetFormat", view_format},
    {"nvnTextureViewSetSwizzle", view_swizzle}, {"nvnTextureViewSetDepthStencilMode", view_dsmode}, {"nvnTextureViewSetTarget", view_target},
    {"nvnTexturePoolInitialize", tpool_initialize}, {"nvnTexturePoolRegisterTexture", tpool_register},     {"nvnBlendStateSetBlendFunc", blend_func_set}, {"nvnBlendStateSetBlendEquation", blend_equation},
    {"nvnColorStateSetDefaults", color_defaults}, {"nvnColorStateSetBlendEnable", color_blend_enable}, {"nvnColorStateSetLogicOp", color_logic_op}, {"nvnColorStateSetAlphaTest", color_alpha_test},
    {"nvnChannelMaskStateSetDefaults", chanmask_defaults}, {"nvnChannelMaskStateSetChannelMask", chanmask_set},
                        {"nvnPolygonStateSetDefaults", poly_defaults}, {"nvnPolygonStateSetCullFace", poly_cull}, {"nvnPolygonStateSetFrontFace", poly_front}, {"nvnPolygonStateSetPolygonMode", poly_mode},
    {"nvnPolygonStateSetPolygonOffsetEnables", poly_offset}, {"nvnPolygonStateGetCullFace", poly_get_cull}, {"nvnPolygonStateGetFrontFace", poly_get_front},
    {"nvnPolygonStateGetPolygonMode", poly_get_mode}, {"nvnPolygonStateGetPolygonOffsetEnables", poly_get_offset},
    {"nvnDepthStencilStateSetDefaults", ds_defaults}, {"nvnDepthStencilStateSetDepthTestEnable", ds_depth_test}, {"nvnDepthStencilStateSetDepthWriteEnable", ds_depth_write},
    {"nvnDepthStencilStateSetDepthFunc", ds_depth_func}, {"nvnDepthStencilStateSetStencilTestEnable", ds_stencil_test}, {"nvnDepthStencilStateSetStencilFunc", ds_stencil_func},
    {"nvnDepthStencilStateSetStencilOp", ds_stencil_op}, {"nvnDepthStencilStateGetDepthTestEnable", ds_get_depth_test}, {"nvnDepthStencilStateGetDepthWriteEnable", ds_get_depth_write},
    {"nvnDepthStencilStateGetStencilTestEnable", ds_get_stencil_test}, {"nvnDepthStencilStateGetDepthFunc", ds_get_depth_func},
    {"nvnVertexAttribStateSetDefaults", vattr_defaults}, {"nvnVertexAttribStateSetFormat", vattr_format}, {"nvnVertexAttribStateSetStreamIndex", vattr_stream},
    {"nvnVertexStreamStateSetDefaults", vstream_defaults}, {"nvnVertexStreamStateSetStride", vstream_stride}, {"nvnVertexStreamStateSetDivisor", vstream_divisor},
    {"nvnCommandBufferInitialize", cmd_initialize}, {"nvnCommandBufferFinalize", cmd_finalize}, {"nvnCommandBufferAddCommandMemory", cmd_add_command_memory},
    {"nvnCommandBufferAddControlMemory", cmd_add_control_memory}, {"nvnCommandBufferGetCommandMemorySize", cmd_get_command_size}, {"nvnCommandBufferGetControlMemorySize", cmd_get_control_size},
        {"nvnCommandBufferIsRecording", cmd_is_recording},         {"nvnCommandBufferBeginRecording", cmd_begin}, {"nvnCommandBufferEndRecording", cmd_end}, {"nvnCommandBufferCallCommands", cmd_call}, {"nvnCommandBufferCopyCommands", cmd_call},
    {"nvnCommandBufferSetTexturePool", cmd_texpool}, {"nvnCommandBufferSetSamplerPool", cmd_smppool}, {"nvnCommandBufferBindProgram", cmd_program},
    {"nvnCommandBufferBindUniformBuffer", cmd_ubo}, {"nvnCommandBufferBindUniformBuffers", cmd_ubos}, {"nvnCommandBufferBindStorageBuffer", cmd_ssbo},
    {"nvnCommandBufferBindStorageBuffers", cmd_ssbos}, {"nvnCommandBufferBindTexture", cmd_texture}, {"nvnCommandBufferBindTextures", cmd_textures},
    {"nvnCommandBufferBindVertexBuffer", cmd_vbuf}, {"nvnCommandBufferBindVertexBuffers", cmd_vbufs}, {"nvnCommandBufferBindVertexAttribState", cmd_vattr},
    {"nvnCommandBufferBindVertexStreamState", cmd_vstream}, {"nvnCommandBufferBindBlendState", cmd_blend}, {"nvnCommandBufferBindColorState", cmd_color},
    {"nvnCommandBufferBindChannelMaskState", cmd_chanmask}, {"nvnCommandBufferBindMultisampleState", cmd_multisample}, {"nvnCommandBufferBindPolygonState", cmd_polygon},
    {"nvnCommandBufferBindDepthStencilState", cmd_depthstencil}, {"nvnCommandBufferSetViewport", cmd_viewport}, {"nvnCommandBufferSetViewports", cmd_viewports},
    {"nvnCommandBufferSetScissor", cmd_scissor}, {"nvnCommandBufferSetScissors", cmd_scissors}, {"nvnCommandBufferSetDepthRange", cmd_depth_range},
    {"nvnCommandBufferSetDepthRanges", cmd_depth_ranges}, {"nvnCommandBufferSetRenderTargets", cmd_targets}, {"nvnCommandBufferSetStencilRef", cmd_stencil_ref},
    {"nvnCommandBufferSetStencilValueMask", cmd_stencil_vmask}, {"nvnCommandBufferSetStencilMask", cmd_stencil_mask}, {"nvnCommandBufferSetBlendColor", cmd_blend_color},
    {"nvnCommandBufferSetAlphaRef", cmd_alpha_ref}, {"nvnCommandBufferSetPointSize", cmd_point_size}, {"nvnCommandBufferSetLineWidth", cmd_line_width},
    {"nvnCommandBufferSetPolygonOffsetClamp", cmd_poly_offset}, {"nvnCommandBufferSetRasterizerDiscard", cmd_rast_discard}, {"nvnCommandBufferSetDepthClamp", cmd_depth_clamp},
    {"nvnCommandBufferSetPrimitiveRestart", cmd_prim_restart}, {"nvnCommandBufferSetPatchSize", cmd_patch_size}, {"nvnCommandBufferClearColor", cmd_clear_color},
    {"nvnCommandBufferClearDepthStencil", cmd_clear_ds}, {"nvnCommandBufferDrawArrays", cmd_draw_arrays}, {"nvnCommandBufferDrawArraysInstanced", cmd_draw_arrays_inst},
    {"nvnCommandBufferDrawElements", cmd_draw_elements}, {"nvnCommandBufferDrawElementsBaseVertex", cmd_draw_elements_bv}, {"nvnCommandBufferDrawElementsInstanced", cmd_draw_elements_inst},
    {"nvnCommandBufferCopyBufferToTexture", cmd_copy_buf_tex}, {"nvnCommandBufferCopyTextureToTexture", cmd_copy_tex_tex}, {"nvnCommandBufferCopyTextureToBuffer", cmd_copy_tex_buf},
    {"nvnCommandBufferSetSampleMask", cmd_sample_mask}, {"nvnCommandBufferDownsample", cmd_downsample},
    {"nvnCommandBufferCopyBufferToBuffer", cmd_copy_buf_buf}, {"nvnCommandBufferUpdateUniformBuffer", cmd_update_ubo}, {"nvnCommandBufferClearBuffer", cmd_clear_buffer},
    {"nvnCommandBufferSetCopyRowStride", cmd_row_stride},
                {"nvnQueueSubmitCommands", queue_submit}, {"nvnQueuePresentTexture", queue_present},     {"nvnCommandBufferBindImages", cmd_images}, {"nvnCommandBufferBindImage", cmd_image}, {"nvnCommandBufferDispatchCompute", cmd_dispatch},
    {"nvnWindowBuilderSetTextures", wb_set_textures}, {"nvnWindowInitialize", win_initialize}, {"nvnWindowAcquireTexture", win_acquire}, {"nvnWindowSetCrop", win_set_crop},
    /* the rest of the API (see the block above the table) */
    {"nvnDeviceBuilderSetDefaults", devb_defaults}, {"nvnDeviceBuilderSetFlags", devb_set_flags}, {"nvnDeviceBuilderGetFlags", devb_get_flags}, {"nvnDeviceInitialize", dev_initialize},
    {"nvnDeviceFinalize", dev_finalize}, {"nvnDeviceGetWindowOriginMode", dev_get_origin_mode}, {"nvnDeviceGetDepthMode", dev_get_depth_mode},
    {"nvnDeviceGetCurrentTimestampInNanoseconds", dev_timestamp_now}, {"nvnDeviceGetTimestampInNanoseconds", dev_timestamp_of}, {"nvnDeviceGenerateDebugDomainId", dev_debug_domain},
    {"nvnDeviceGetSeparateTextureHandle", dev_separate_handle}, {"nvnDeviceGetSeparateSamplerHandle", dev_separate_sampler_handle}, {"nvnDeviceIsExternalDebuggerAttached", dev_debugger_attached},
    {"nvnDeviceInstallDebugCallback", n_nop}, {"nvnDeviceApplyDeferredFinalizes", dev_apply_deferred}, {"nvnDeviceSetDebugLabel", any_set_label},
    {"nvnDeviceFinalizeCommandHandle", dev_finalize_command_handle}, {"nvnQueueBuilderSetDevice", any_set_device}, {"nvnQueueBuilderGetDevice", any_get_device},
    {"nvnWindowBuilderSetDevice", any_set_device}, {"nvnWindowBuilderGetDevice", any_get_device}, {"nvnTextureBuilderSetDevice", any_set_device},
    {"nvnTextureBuilderGetDevice", any_get_device}, {"nvnSamplerBuilderSetDevice", any_set_device}, {"nvnSamplerBuilderGetDevice", any_get_device}, {"nvnSyncInitialize", sync_initialize},
    {"nvnSyncFinalize", sync_finalize}, {"nvnSyncSetDebugLabel", any_set_label}, {"nvnSyncWait", sync_wait}, {"nvnQueueFenceSync", queue_fence_sync}, {"nvnQueueWaitSync", queue_wait_sync},
    {"nvnSyncCreateGLSync", sync_no_gl_interop}, {"nvnSyncInitializeFromFencedGLSync", sync_no_gl_interop}, {"nvnEventBuilderSetDefaults", evb_defaults},
    {"nvnEventBuilderSetStorage", evb_storage}, {"nvnEventBuilderGetStorage", evb_get_storage}, {"nvnEventBuilderGetMemoryPool", ev_get_pool},
    {"nvnEventBuilderGetMemoryOffset", ev_get_offset}, {"nvnEventInitialize", ev_initialize}, {"nvnEventFinalize", ev_finalize}, {"nvnEventGetValue", ev_get_value},
    {"nvnEventSignal", ev_signal}, {"nvnEventGetMemoryPool", ev_get_pool}, {"nvnEventGetMemoryOffset", ev_get_offset}, {"nvnQueueBuilderSetDefaults", qb_defaults},
    {"nvnQueueBuilderSetFlags", qb_set_flags}, {"nvnQueueBuilderSetCommandMemorySize", qb_set_cmd}, {"nvnQueueBuilderSetComputeMemorySize", qb_set_compute},
    {"nvnQueueBuilderSetControlMemorySize", qb_set_control}, {"nvnQueueBuilderSetCommandFlushThreshold", qb_set_threshold}, {"nvnQueueBuilderSetQueuePriority", qb_set_priority},
    {"nvnQueueBuilderSetQueueMemory", qb_set_memory}, {"nvnQueueBuilderGetFlags", qb_get_flags}, {"nvnQueueBuilderGetCommandMemorySize", qb_get_cmd},
    {"nvnQueueBuilderGetComputeMemorySize", qb_get_compute}, {"nvnQueueBuilderGetControlMemorySize", qb_get_control}, {"nvnQueueBuilderGetCommandFlushThreshold", qb_get_threshold},
    {"nvnQueueBuilderGetQueuePriority", qb_get_priority}, {"nvnQueueBuilderGetMemory", qb_get_memory}, {"nvnQueueBuilderGetMemorySize", qb_get_memory_size},
    {"nvnQueueBuilderGetQueueMemorySize", qb_get_queue_memory_size}, {"nvnQueueInitialize", queue_initialize}, {"nvnQueueFinalize", queue_finalize}, {"nvnQueueSetDebugLabel", any_set_label},
    {"nvnQueueGetTotalCommandMemoryUsed", queue_total_cmd}, {"nvnQueueGetTotalControlMemoryUsed", queue_total_control}, {"nvnQueueGetTotalComputeMemoryUsed", queue_total_compute},
    {"nvnQueueResetMemoryUsageCounts", queue_reset_usage}, {"nvnQueueFlush", queue_flush}, {"nvnQueueFinish", queue_finish}, {"nvnQueueGetError", queue_get_error},
    {"nvnQueueAcquireTexture", queue_acquire}, {"nvnMultisampleStateSetDefaults", ms_defaults}, {"nvnMultisampleStateSetMultisampleEnable", ms_enable},
    {"nvnMultisampleStateSetAlphaToCoverageEnable", ms_a2c}, {"nvnMultisampleStateSetAlphaToCoverageDither", ms_a2c_dither}, {"nvnMultisampleStateSetSamples", ms_samples},
    {"nvnMultisampleStateSetRasterSamples", ms_raster_samples}, {"nvnMultisampleStateSetCoverageToColorEnable", ms_c2c}, {"nvnMultisampleStateSetCoverageToColorOutput", ms_c2c_output},
    {"nvnMultisampleStateSetCoverageModulationMode", ms_covmod}, {"nvnMultisampleStateSetSampleLocationsEnable", ms_locations_on},
    {"nvnMultisampleStateSetSampleLocationsGridEnable", ms_grid_on}, {"nvnMultisampleStateSetSampleLocations", ms_set_locations}, {"nvnMultisampleStateGetMultisampleEnable", ms_get_enable},
    {"nvnMultisampleStateGetAlphaToCoverageEnable", ms_get_a2c}, {"nvnMultisampleStateGetAlphaToCoverageDither", ms_get_a2c_dither}, {"nvnMultisampleStateGetSamples", ms_get_samples},
    {"nvnMultisampleStateGetRasterSamples", ms_get_raster_samples}, {"nvnMultisampleStateGetCoverageToColorEnable", ms_get_c2c},
    {"nvnMultisampleStateGetCoverageToColorOutput", ms_get_c2c_output}, {"nvnMultisampleStateGetCoverageModulationMode", ms_get_covmod},
    {"nvnMultisampleStateGetSampleLocationsEnable", ms_get_locations_on}, {"nvnMultisampleStateGetSampleLocationsGridEnable", ms_get_grid_on},
    {"nvnMultisampleStateGetSampleLocationsGrid", ms_get_grid}, {"nvnBlendStateSetDefaults", blend_defaults}, {"nvnBlendStateSetBlendTarget", blend_target},
    {"nvnBlendStateSetAdvancedMode", blend_adv_mode}, {"nvnBlendStateSetAdvancedOverlap", blend_adv_overlap}, {"nvnBlendStateSetAdvancedPremultipliedSrc", blend_adv_premult},
    {"nvnBlendStateSetAdvancedNormalizedDst", blend_adv_normdst}, {"nvnBlendStateGetBlendTarget", blend_get_target}, {"nvnBlendStateGetBlendFunc", blend_get_func},
    {"nvnBlendStateGetBlendEquation", blend_get_equation}, {"nvnBlendStateGetAdvancedMode", blend_get_adv_mode}, {"nvnBlendStateGetAdvancedOverlap", blend_get_adv_overlap},
    {"nvnBlendStateGetAdvancedPremultipliedSrc", blend_get_adv_premult}, {"nvnBlendStateGetAdvancedNormalizedDst", blend_get_adv_normdst},
    {"nvnColorStateGetBlendEnable", color_get_blend_enable}, {"nvnColorStateGetLogicOp", color_get_logic_op}, {"nvnColorStateGetAlphaTest", color_get_alpha_test},
    {"nvnChannelMaskStateGetChannelMask", chanmask_get}, {"nvnDepthStencilStateGetStencilFunc", ds_get_stencil_func}, {"nvnDepthStencilStateGetStencilOp", ds_get_stencil_op},
    {"nvnVertexAttribStateGetFormat", vattr_get_format}, {"nvnVertexAttribStateGetStreamIndex", vattr_get_stream}, {"nvnVertexStreamStateGetStride", vstream_get_stride},
    {"nvnVertexStreamStateGetDivisor", vstream_get_divisor}, {"nvnSamplerBuilderSetBorderColor", smpb_border_f}, {"nvnSamplerBuilderSetBorderColori", smpb_border_i},
    {"nvnSamplerBuilderSetBorderColorui", smpb_border_i}, {"nvnSamplerBuilderSetLodSnap", smpb_lod_snap}, {"nvnSamplerBuilderGetMinMagFilter", smp_get_minmag},
    {"nvnSamplerBuilderGetWrapMode", smp_get_wrap}, {"nvnSamplerBuilderGetLodClamp", smp_get_lodclamp}, {"nvnSamplerBuilderGetLodBias", smp_get_lodbias},
    {"nvnSamplerBuilderGetCompare", smp_get_compare}, {"nvnSamplerBuilderGetBorderColor", smp_get_border}, {"nvnSamplerBuilderGetBorderColori", smp_get_border},
    {"nvnSamplerBuilderGetBorderColorui", smp_get_border}, {"nvnSamplerBuilderGetMaxAnisotropy", smp_get_aniso}, {"nvnSamplerBuilderGetReductionFilter", smp_get_reduction},
    {"nvnSamplerBuilderGetLodSnap", smp_get_lod_snap}, {"nvnSamplerGetMinMagFilter", smp_get_minmag}, {"nvnSamplerGetWrapMode", smp_get_wrap}, {"nvnSamplerGetLodClamp", smp_get_lodclamp},
    {"nvnSamplerGetLodBias", smp_get_lodbias}, {"nvnSamplerGetCompare", smp_get_compare}, {"nvnSamplerGetBorderColor", smp_get_border}, {"nvnSamplerGetBorderColori", smp_get_border},
    {"nvnSamplerGetBorderColorui", smp_get_border}, {"nvnSamplerGetMaxAnisotropy", smp_get_aniso}, {"nvnSamplerGetReductionFilter", smp_get_reduction}, {"nvnSamplerCompare", smp_compare},
    {"nvnSamplerGetDebugID", any_debug_id}, {"nvnSamplerFinalize", smp_finalize}, {"nvnSamplerSetDebugLabel", any_set_label}, {"nvnSamplerPoolFinalize", spool_finalize},
    {"nvnSamplerPoolGetMemoryPool", spool_get_pool}, {"nvnSamplerPoolGetMemoryOffset", spool_get_offset}, {"nvnSamplerPoolGetSize", spool_get_size},
    {"nvnSamplerPoolSetDebugLabel", any_set_label}, {"nvnTextureBuilderGetSwizzle", texb_get_swizzle}, {"nvnTextureBuilderGetDepthStencilMode", texb_get_dsmode},
    {"nvnTextureBuilderGetMemoryPool", texb_get_pool}, {"nvnTextureBuilderGetMemoryOffset", texb_get_offset}, {"nvnTextureBuilderGetPackagedTextureData", texb_get_packaged},
    {"nvnTextureBuilderGetPackagedTextureLayout", texb_get_layout}, {"nvnTextureBuilderGetStride", texb_get_stride}, {"nvnTextureBuilderSetGLTextureName", texb_set_glname},
    {"nvnTextureBuilderGetGLTextureName", texb_get_glname}, {"nvnTextureGetSwizzle", tex_get_swizzle}, {"nvnTextureGetMemoryPool", tex_get_pool}, {"nvnTextureCompare", tex_compare},
    {"nvnTextureGetDebugID", any_debug_id}, {"nvnTextureGetViewOffset", tex_view_offset}, {"nvnTextureReadTexels", tex_read_texels}, {"nvnTextureReadTexelsStrided", tex_read_texels},
    {"nvnTextureInvalidateTexels", tex_invalidate_texels}, {"nvnTextureSetDebugLabel", any_set_label}, {"nvnTextureViewGetLevels", view_get_levels},
    {"nvnTextureViewGetLayers", view_get_layers}, {"nvnTextureViewGetFormat", view_get_format}, {"nvnTextureViewGetSwizzle", view_get_swizzle},
    {"nvnTextureViewGetDepthStencilMode", view_get_dsmode}, {"nvnTextureViewGetTarget", view_get_target}, {"nvnTextureViewCompare", view_compare}, {"nvnTexturePoolFinalize", tpool_finalize},
    {"nvnTexturePoolGetMemoryPool", tpool_get_pool}, {"nvnTexturePoolGetMemoryOffset", tpool_get_offset}, {"nvnTexturePoolGetSize", tpool_get_size},
    {"nvnTexturePoolRegisterImage", tpool_register}, {"nvnTexturePoolSetDebugLabel", any_set_label}, {"nvnProgramSetDebugLabel", any_set_label},
    {"nvnProgramSetSampleShading", prog_sample_shading}, {"nvnWindowBuilderSetDefaults", wb_defaults}, {"nvnWindowBuilderSetNativeWindow", wb_native},
    {"nvnWindowBuilderSetPresentInterval", wb_interval}, {"nvnWindowBuilderSetNumActiveTextures", wb_num_active}, {"nvnWindowBuilderGetNativeWindow", wb_get_native},
    {"nvnWindowBuilderGetPresentInterval", wb_get_interval}, {"nvnWindowBuilderGetNumTextures", wb_get_num_textures}, {"nvnWindowBuilderGetNumActiveTextures", wb_get_num_active},
    {"nvnWindowBuilderGetTexture", wb_get_texture}, {"nvnWindowFinalize", win_finalize}, {"nvnWindowSetDebugLabel", any_set_label}, {"nvnWindowSetPresentInterval", win_set_interval},
    {"nvnWindowGetPresentInterval", win_get_interval}, {"nvnWindowGetNativeWindow", win_get_native}, {"nvnWindowGetNumTextures", win_get_num_textures},
    {"nvnWindowGetNumActiveTextures", win_get_num_active}, {"nvnWindowSetNumActiveTextures", win_set_num_active}, {"nvnWindowGetCrop", win_get_crop},
    {"nvnCommandBufferSetMemoryCallback", cmd_set_callback}, {"nvnCommandBufferSetMemoryCallbackData", cmd_set_callback_data}, {"nvnCommandBufferGetMemoryCallback", cmd_get_callback},
    {"nvnCommandBufferGetMemoryCallbackData", cmd_get_callback_data}, {"nvnCommandBufferSetCommandMemoryCallbackEnabled", cmd_callback_enabled},
    {"nvnCommandBufferGetCommandMemoryUsed", cmd_get_command_used}, {"nvnCommandBufferGetControlMemoryUsed", cmd_get_control_used},
    {"nvnCommandBufferGetCommandMemoryFree", cmd_get_command_free}, {"nvnCommandBufferGetControlMemoryFree", cmd_get_control_free}, {"nvnCommandBufferGetCopyRowStride", cmd_get_row_stride},
    {"nvnCommandBufferGetCopyImageStride", cmd_get_image_stride}, {"nvnCommandBufferSetCopyImageStride", cmd_image_stride}, {"nvnCommandBufferSetDepthBounds", cmd_depth_bounds},
    {"nvnCommandBufferSetConservativeRasterEnable", cmd_cons_raster}, {"nvnCommandBufferSetConservativeRasterDilate", cmd_cons_dilate},
    {"nvnCommandBufferSetSubpixelPrecisionBias", cmd_subpixel_bias}, {"nvnCommandBufferSetViewportSwizzles", cmd_viewport_swizzles},
    {"nvnCommandBufferSetInnerTessellationLevels", cmd_tess_levels}, {"nvnCommandBufferSetOuterTessellationLevels", cmd_tess_levels}, {"nvnCommandBufferBarrier", cmd_barrier},
    {"nvnCommandBufferDiscardColor", cmd_discard_color}, {"nvnCommandBufferDiscardDepthStencil", cmd_discard_ds}, {"nvnCommandBufferClearColori", cmd_clear_color_typed},
    {"nvnCommandBufferClearColorui", cmd_clear_color_typed}, {"nvnCommandBufferClearTexture", cmd_clear_texture}, {"nvnCommandBufferClearTexturei", cmd_clear_texture},
    {"nvnCommandBufferClearTextureui", cmd_clear_texture}, {"nvnCommandBufferDownsampleTextureView", cmd_downsample_view}, {"nvnCommandBufferTiledDownsample", cmd_downsample},
    {"nvnCommandBufferTiledDownsampleTextureView", cmd_downsample_view}, {"nvnCommandBufferDrawArraysIndirect", cmd_draw_arrays_indirect},
    {"nvnCommandBufferDrawElementsIndirect", cmd_draw_elements_indirect}, {"nvnCommandBufferMultiDrawArraysIndirectCount", cmd_multi_draw_arrays_indirect},
    {"nvnCommandBufferMultiDrawElementsIndirectCount", cmd_multi_draw_elements_indirect}, {"nvnCommandBufferDispatchComputeIndirect", cmd_dispatch_indirect},
    {"nvnCommandBufferSetRenderEnable", cmd_render_enable}, {"nvnCommandBufferSetRenderEnableConditional", cmd_render_enable_cond}, {"nvnCommandBufferResetCounter", cmd_reset_counter},
    {"nvnCommandBufferReportCounter", cmd_report_counter}, {"nvnCommandBufferReportValue", cmd_report_value}, {"nvnCommandBufferFenceSync", cmd_fence_sync},
    {"nvnCommandBufferWaitSync", cmd_wait_sync}, {"nvnCommandBufferSignalEvent", cmd_signal_event}, {"nvnCommandBufferWaitEvent", cmd_wait_event},
    {"nvnCommandBufferDrawTexture", cmd_draw_texture},
    {"nvnCommandBufferBindCoverageModulationTable", cmd_covmod_table},{"nvnCommandBufferPushDebugGroup", cmd_push_group}, {"nvnCommandBufferPushDebugGroupStatic", cmd_push_group},
    {"nvnCommandBufferPushDebugGroupDynamic", cmd_push_group}, {"nvnCommandBufferPopDebugGroup", cmd_pop_group}, {"nvnCommandBufferPopDebugGroupId", cmd_pop_group},
    {"nvnCommandBufferInsertDebugMarker", cmd_marker}, {"nvnCommandBufferInsertDebugMarkerStatic", cmd_marker_id}, {"nvnCommandBufferInsertDebugMarkerDynamic", cmd_marker_id},
    {"nvnCommandBufferSetDebugLabel", cmd_set_label},
    /* no effect on this platform by the meaning of the call itself: each one is explained in nvn_audit_notes.tsv */
    {"nvnCommandBufferSetShaderScratchMemory", n_nop}, {"nvnSetExternalDebuggerEnabledNVX", n_nop}, {"nvnDeviceSetIntermediateShaderCache", n_nop}, {"nvnDeviceWalkDebugDatabase", n_nop},
    {"nvnCommandBufferSetTiledCacheAction", n_nop}, {"nvnCommandBufferSetTiledCacheTileSize", n_nop}, {"nvnCommandBufferSaveZCullData", n_nop},
    {"nvnCommandBufferRestoreZCullData", n_nop}, {"nvnCommandBufferSetStencilCullCriteria", n_nop}, {"nvnCommandBufferResolveDepthBuffer", n_nop},
    {"nvnCommandBufferSetColorReductionEnable", n_nop}, {"nvnCommandBufferSetColorReductionThresholds", n_nop}, {"nvnBufferInvalidateMappedRange", n_nop},
    {"nvnMemoryPoolInvalidateMappedRange", n_nop}, {"nvnDeviceRegisterFastClearColor", n_true}, {"nvnDeviceRegisterFastClearColori", n_true},
    {"nvnDeviceRegisterFastClearColorui", n_true}, {"nvnDeviceRegisterFastClearDepth", n_true}, {"nvnTextureBuilderGetZCullStorageSize", n_zero},
    {"nvnTextureGetZCullStorageSize", n_zero},
};

/* The host function for an NVN entry point, or NULL when this backend does not implement it. */
ImportFn nvn_gl_pick(const char *name) {
    for (size_t i = 0; i < sizeof k_nvn / sizeof k_nvn[0]; i++) if (strcmp(name, k_nvn[i].name) == 0) return k_nvn[i].fn;
    return NULL;
}
