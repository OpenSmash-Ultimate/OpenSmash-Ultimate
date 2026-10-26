/* What the NVN front end (nvn_front.c) and the OpenGL back end (nvn_gl.c) share beyond nvn_front.h: the front end's
 * state the OpenGL code reads or counts into, and the calls each makes into the other. Moved out of nvn_gl.c without
 * renames (seam step S3). The Vulkan back end does not include it: it uses the nvn_front_* services of nvn_front.h. */
#ifndef NVN_FRONT_GL_H
#define NVN_FRONT_GL_H
#include <stdio.h>
#include <stdint.h>
#include <windows.h>
#include "nvn_front.h"
#include "tex_decode.h"

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define A4 (c->x[4])
#define A5 (c->x[5])
#define A6 (c->x[6])
#define A7 (c->x[7])
#define F0 (c->v[0].f[0])
#define F1 (c->v[1].f[0])
#define F2 (c->v[2].f[0])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define P(a) ((void *)(uintptr_t)(a))

int nvn_pool_range(uint64_t pool, uint64_t *mem, uint64_t *size);     /* nvn_stub.c */
void input_frame_draws(int draws);                                    /* nnhid.c */
void display_pace(void);                                              /* display.c */
int display_wait_refresh(void);                                       /* display.c, P3 round 3: one refresh clock */
void display_before_present(void);
void display_clock_sample(uint64_t index_ptr);                        /* SSBU_CLOCKSTAT */
void display_set_interval(int n);


/* ---- the render thread (nvn_front.c) */
extern DWORD g_render_tid;
extern HWND g_hwnd;
extern LONG64 g_joblog_cur;

/* Hitch log (hitch.c, SSBU_HITCH=<ms>): what the render thread is doing right now, for the watchdog. Two stores per scope. */
extern int g_hitch_ms;
extern volatile const char *g_hitch_rt;
extern volatile uint64_t g_hitch_rt_arg;
void hitch_wait_render(int on); void hitch_frame(long long frame, double ms, int battle); void hitch_report(FILE *out); void hitch_set_render(void);
#define HS_IN(name, arg) const char *hs_p_ = (const char *)g_hitch_rt; uint64_t hs_a_ = g_hitch_rt_arg; g_hitch_rt_arg = (uint64_t)(arg); g_hitch_rt = (name)
#define HS_OUT() (g_hitch_rt = hs_p_, g_hitch_rt_arg = hs_a_)

/* ---- statistics (nvn_front.c) */
extern struct NvnStats {
    LONG64 frames, submits, lists, draws, draws_skipped, clears, stale, tex_created, tex_uploads, tex_bytes, views, handles,
           prog_ok, prog_fail, binary_used, glsl_ok, glsl_fail, glsl_draws, fbo_made, fbo_bad, gl_errors, copies, unknown_fmt, arena_bytes,
           dispatches, dispatch_run, dispatch_skipped, compute_bytes, draws_from_compute;
} g_st;
extern int g_logn;
extern volatile LONG g_setshaders_calls, g_with_text, g_without_text;

/* ---- timing scopes (plan\phases\P3_performance.md). Always on: two QueryPerformanceCounter calls per scope.
 * Printed with the frame-rate line every 300 frames as milliseconds per frame. Scopes nest (PREP contains ARENA and
 * TEXSYNC; SUBMIT contains everything the render thread does for a batch); WAIT_* are measured on the calling guest
 * thread (how long it was blocked in gl_run), PACE is the sleep of display_pace. */
enum { PF_SUBMIT, PF_PREP, PF_DRAWCALL, PF_ARENA, PF_TEXHASH, PF_TEXUP, PF_HANDLE, PF_DISPATCH, PF_CFLUSH, PF_COPY, PF_PRESENT, PF_SWAP,
       PF_OTHERJOB, PF_WAIT_SUBMIT, PF_WAIT_PRESENT, PF_WAIT_OTHER, PF_PACE, PF_XLATE, PF_GLCOMPILE, PF_N };
extern volatile LONG64 g_pf[PF_N], g_pfn[PF_N];
extern LONG64 g_pf_arena_bytes, g_pf_cflush_bytes, g_pf_hash_bytes;
static inline LONG64 pf_now(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
static inline void pf_add(int k, LONG64 t0) { InterlockedExchangeAdd64(&g_pf[k], pf_now() - t0); InterlockedIncrement64(&g_pfn[k]); }
void pf_frame_mark(LONG64 frame);
void logf_limited(int *counter, int limit, const char *fmt, ...);
extern LONG64 g_prev_frame_draws, g_frame_dispatch;

/* ---- formats, guest objects, texture memory (nvn_front.c) */
int fmt_get(int nf, Fmt *f);
extern SRWLOCK g_pool_lock;
void nvn_label_set(uint64_t obj, const char *text);
const char *nvn_label_get(uint64_t obj);
void nvn_label_drop(uint64_t obj);
extern int g_origin_mode, g_depth_mode;
void tex_lay(const TexBuilder *b, TexLayout *L);
int tex_layers(const TexBuilder *b);
int lvl(int v, int l);
extern uint32_t g_serial;
const uint8_t *tex_source(const Tex *t);
void tex_release_packaged(Tex *t);
uint64_t mem_hash(const uint8_t *p, size_t n, int full);
extern LONG64 g_ww_clean, g_ww_dirty, g_ww_unwatched;
uint64_t ww_newest(const uint8_t *p, size_t n);
extern CRITICAL_SECTION g_dead_lock;
extern struct NvnDead { int kind; void *p; } *g_dead;
extern int g_dead_n;

/* ---- command lists, sync objects, events (nvn_front.c) */
extern const char *const k_op_name[OP_COUNT];
uint8_t *list_lookup(uint64_t h, size_t *len);
void list_collect(void);
uint64_t now_ns(void);
void event_signal_at(uint64_t a, int mode, uint32_t value);
extern LONG g_bufhist[6][32], g_storhist[32];     /* program.buffer[n] / program.storage[n] uses in NV assembly text (prog_add_stage) */
extern const char *const k_stage_name[6];        /* "vertex", "fragment", "geometry", "tess-control", "tess-eval", "compute" */
extern int g_draw_tag, g_dt_idx;                 /* render thread (nvn_dt.h, SSBU_DRAWTAG) */
void nvn_front_draw_tag_clear(void);

#endif
