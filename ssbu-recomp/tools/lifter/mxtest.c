/* Offline test of the Maxwell -> GLSL translator against the real OpenGL driver.
 *
 *   mxtest <dir> [options]          <dir> holds the stages dumped with SSBU_SHADERDUMP_ALL=<dir> (s<program>_<i>.mxd, see nvn_front.c)
 *     -v0            use the first translator (mx_translate_v0.c) instead of mx_translate.c, for a before/after table
 *     -n             translate only, do not compile
 *     -o <file>      one line per unique stage: file, stage, status (OK / NOTR / REJECT), opcode or error, detail
 *     -g <dir>       write the GLSL of the first rejected stages of every distinct driver error to <dir>
 *     -s <file.mxd>  print the GLSL of one stage and the driver's verdict
 *
 * Build: clang mxtest.c mx_translate.c mx_translate_v0.c -o mxtest.exe -lopengl32 -lgdi32 -luser32
 * Each unique code section is tested once (duplicates are counted in the "uses" columns). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>
#include <GL/gl.h>
#include "mx_translate.h"

#pragma comment(lib, "opengl32.lib")
__declspec(dllexport) DWORD NvOptimusEnablement = 1;

char *mx_translate_v0(const uint8_t *code, int code_len, const uint8_t *cdata, uint32_t cdata_size, int stage, uint32_t int_attrs, uint32_t *cbufs, int *textures, uint32_t *in_attr, char *why);

typedef GLuint (APIENTRY *PFN_CSPV)(GLenum, GLsizei, const char *const *);
typedef void (APIENTRY *PFN_GPIV)(GLuint, GLenum, GLint *);
typedef void (APIENTRY *PFN_GPIL)(GLuint, GLsizei, GLsizei *, char *);
typedef void (APIENTRY *PFN_DP)(GLuint);
typedef void (APIENTRY *PFN_MSCT)(GLuint);
static PFN_CSPV glCreateShaderProgramv_;
static PFN_GPIV glGetProgramiv_;
static PFN_GPIL glGetProgramInfoLog_;
static PFN_DP glDeleteProgram_;

static int gl_init(void) {
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "mxtest";
    RegisterClassA(&wc);
    HWND w = CreateWindowA("mxtest", "mxtest", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, NULL, NULL, wc.hInstance, NULL);
    HDC dc = GetDC(w);
    PIXELFORMATDESCRIPTOR pfd;
    memset(&pfd, 0, sizeof pfd);
    pfd.nSize = sizeof pfd; pfd.nVersion = 1; pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER; pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32;
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    HGLRC rc = wglCreateContext(dc);
    if (!rc || !wglMakeCurrent(dc, rc)) { printf("no OpenGL context\n"); return 0; }
    glCreateShaderProgramv_ = (PFN_CSPV)wglGetProcAddress("glCreateShaderProgramv");
    glGetProgramiv_ = (PFN_GPIV)wglGetProcAddress("glGetProgramiv");
    glGetProgramInfoLog_ = (PFN_GPIL)wglGetProcAddress("glGetProgramInfoLog");
    glDeleteProgram_ = (PFN_DP)wglGetProcAddress("glDeleteProgram");
    PFN_MSCT msct = (PFN_MSCT)wglGetProcAddress("glMaxShaderCompilerThreadsKHR");
    if (msct) msct(0xffffffffu);
    printf("%s | %s | %s%s\n", (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION), msct ? " | parallel compile" : "");
    return glCreateShaderProgramv_ && glGetProgramiv_ && glGetProgramInfoLog_;
}

/* ---------------------------------------------------------------- dumped stages */
typedef struct {
    char name[64];
    int stage, has_code, text, uses;
    uint8_t *data; uint32_t data_len, code_len, c_off, c_size, local[3], smem, lmem;
    uint64_t hash;
    int status;                 /* 0 untested, 1 ok, 2 not translated, 3 rejected */
    char op[48], detail[240];
    char *glsl;
    GLuint prog;
} Stage;

static uint64_t fnv(const uint8_t *p, size_t n, uint64_t h) { for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ULL; return h; }
static int load(const char *path, const char *name, Stage *s) {
    FILE *f = fopen(path, "rb");
    uint32_t head[5];
    if (!f) return 0;
    if (fread(head, 4, 5, f) != 5 || head[0] != 0x3144584du) { fclose(f); return 0; }
    uint8_t *ctl = malloc(head[3] + 16);
    memset(s, 0, sizeof *s);
    snprintf(s->name, sizeof s->name, "%s", name);
    if (fread(ctl, 1, head[3], f) != head[3]) { fclose(f); free(ctl); return 0; }
    s->data_len = head[4];
    s->data = malloc(s->data_len + 16);
    if (s->data_len && fread(s->data, 1, s->data_len, f) != s->data_len) { fclose(f); free(ctl); return 0; }
    fclose(f);
    memcpy(&s->code_len, ctl + 0x6f8, 4); memcpy(&s->c_size, ctl + 0x6fc, 4); memcpy(&s->c_off, ctl + 0x700, 4);
    uint32_t tl;
    memcpy(&tl, ctl + 0x18, 4);
    s->text = tl != 0;
    uint32_t nvn_stage = 0;
    if (head[3] >= 0x760) { memcpy(&nvn_stage, ctl + 0x714, 4); memcpy(s->local, ctl + 0x748, 12); memcpy(&s->smem, ctl + 0x754, 4); memcpy(&s->lmem, ctl + 0x758, 4); }
    free(ctl);
    if (nvn_stage == 5) {                        /* compute: no program header, the code section starts with the instructions */
        s->stage = MX_COMPUTE;
        s->has_code = s->code_len >= 0x20 && s->code_len <= s->data_len;
    } else if (s->data_len >= 0x80 && s->code_len + 0x30 <= s->data_len) {
        static const int8_t st[16] = {-1, MX_VERTEX, MX_TESS_CONTROL, MX_TESS_EVAL, MX_GEOMETRY, MX_FRAGMENT, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
        uint32_t sph0;
        memcpy(&sph0, s->data + 0x30, 4);
        s->stage = st[(sph0 >> 10) & 15];
        s->has_code = s->stage >= 0;
    }
    s->hash = fnv(s->data, s->data_len, 0xcbf29ce484222325ULL ^ (uint64_t)s->stage);
    s->uses = 1;
    return 1;
}

static int g_vkmode;                             /* -vk */
static int (*vk_compile)(int stage, const char *src, uint32_t **words, size_t *count, char *log, size_t log_size);
static void (*vk_free)(uint32_t *words);
static long long g_vk_words;
static void translate(Stage *s, int v0) {
    const uint8_t *code = s->data + (s->stage == MX_COMPUTE ? 0 : 0x30), *cdata = NULL;
    uint32_t csize = 0;
    if (s->c_size && s->c_off + s->c_size <= s->data_len) { cdata = s->data + s->c_off; csize = s->c_size; }
    if (v0) {
        uint32_t cb, in;
        int tx;
        char why[200] = "";
        if (s->stage > 1) { s->status = 2; snprintf(s->op, sizeof s->op, "stage %d", s->stage); return; }
        s->glsl = mx_translate_v0(code, (int)s->code_len, cdata, csize, s->stage, 0, &cb, &tx, &in, why);
        if (!s->glsl) {
            unsigned long long w;
            s->status = 2;
            snprintf(s->detail, sizeof s->detail, "%s", why);
            if (sscanf(why, "instruction %llx", &w) == 1) snprintf(s->op, sizeof s->op, "%s", mx_opcode_name(w));
            else { snprintf(s->op, sizeof s->op, "%s", why); char *d = strpbrk(s->op, "0123456789"); if (d) *d = 0; }
        }
        return;
    }
    MxIn in;
    MxOut out;
    memset(&in, 0, sizeof in);
    in.stage = s->stage; in.code = code; in.code_len = (int)s->code_len; in.cdata = cdata; in.cdata_size = csize;
    in.binding_base = (unsigned)(s->stage == MX_FRAGMENT ? 20 : 0); in.ssbo_base = (unsigned)(s->stage == MX_FRAGMENT ? 56 : 40);
    if (s->stage == MX_COMPUTE) { memcpy(in.local_size, s->local, 12); in.smem_bytes = s->smem; in.lmem_bytes = s->lmem; in.track_writes = 1; in.wmask_binding = 90; }
    if (g_vkmode) {                              /* -vk: Vulkan GLSL with the bindings of plan\phases\V1_vulkan.md section 4 */
        unsigned b = s->stage == MX_COMPUTE ? 0u : 40u * (unsigned)s->stage;
        in.target = 1; in.binding_base = b; in.ssbo_base = b + 20; in.wmask_binding = b + 38; in.tex_base = b; in.image_base = b + 32;
    }
    if (!mx_translate(&in, &out)) {
        s->status = 2;
        snprintf(s->op, sizeof s->op, "%s", out.op);
        snprintf(s->detail, sizeof s->detail, "%s", out.why);
        return;
    }
    s->glsl = out.glsl;
}

static void first_error_line(const char *log, char *out, size_t n) {
    /* NVIDIA: "0(123) : error C1234: text" -> "C1234: text" without numbers in identifiers */
    const char *p = strstr(log, "error");
    if (!p) p = log;
    const char *c = strstr(p, ": ");
    if (c && c - p < 16) p = c + 2;
    size_t k = 0;
    for (; *p && *p != '\n' && k + 1 < n; p++) out[k++] = *p;
    out[k] = 0;
}

typedef struct { char key[160]; int uniq, uses, written; } Bucket;
static Bucket *bucket(Bucket *b, int *n, const char *key) {
    for (int i = 0; i < *n; i++) if (!strcmp(b[i].key, key)) return &b[i];
    if (*n >= 4000) return &b[3999];
    memset(&b[*n], 0, sizeof b[*n]);
    snprintf(b[*n].key, sizeof b[*n].key, "%s", key);
    return &b[(*n)++];
}
static int by_uniq(const void *a, const void *b) { return ((const Bucket *)b)->uniq - ((const Bucket *)a)->uniq; }

/* -vk: the text is compiled to SPIR-V by glslang (ssbu_glslang.dll, built by build_glslang.bat) instead of by the OpenGL driver. */
static int vk_compile_stage(Stage *s, char *log, size_t log_size) {
    uint32_t *words = NULL;
    size_t count = 0;
    int ok = vk_compile(s->stage, s->glsl, &words, &count, log, log_size);
    if (ok) { g_vk_words += (long long)count; vk_free(words); }
    return ok;
}
static void vk_error_line(const char *log, char *out, size_t n) {       /* "ERROR: 0:123: 'x' : text" -> "'x' : text" */
    const char *p = strstr(log, "ERROR: ");
    size_t k = 0;
    if (p) { p += 7; const char *c = strchr(p, ':'); if (c) c = strchr(c + 1, ':'); if (c && c - p < 16) p = c + 2; } else p = log;
    for (; *p && *p != '\n' && k + 1 < n; p++) out[k++] = *p;
    out[k] = 0;
}

int main(int argc, char **argv) {
    const char *dir = NULL, *outfile = NULL, *glsldir = NULL, *single = NULL;
    int v0 = 0, nocompile = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v0")) v0 = 1;
        else if (!strcmp(argv[i], "-vk")) g_vkmode = 1;
        else if (!strcmp(argv[i], "-n")) nocompile = 1;
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) outfile = argv[++i];
        else if (!strcmp(argv[i], "-g") && i + 1 < argc) glsldir = argv[++i];
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) single = argv[++i];
        else dir = argv[i];
    }
    if (!dir && !single) { printf("usage: mxtest <dir> [-v0] [-n] [-o results.tsv] [-g glsl_dir] | mxtest -s <file.mxd> [-v0]\n"); return 1; }
    if (!nocompile && !g_vkmode && !gl_init()) return 2;
    if (!nocompile && g_vkmode) {
        HMODULE m = LoadLibraryA("ssbu_glslang.dll");
        vk_compile = m ? (void *)GetProcAddress(m, "ssbu_glsl_to_spirv") : NULL;
        vk_free = m ? (void *)GetProcAddress(m, "ssbu_spirv_free") : NULL;
        if (!vk_compile || !vk_free) { printf("ssbu_glslang.dll cannot be loaded (build it with build_glslang.bat)\n"); return 2; }
    }

    if (single) {
        Stage s;
        if (!load(single, single, &s)) { printf("cannot read %s\n", single); return 1; }
        translate(&s, v0);
        if (!s.glsl) { printf("not translated: %s (%s)\n", s.op, s.detail); return 1; }
        printf("%s", s.glsl);
        if (!nocompile && g_vkmode) {
            char log[8192] = "";
            int ok = vk_compile_stage(&s, log, sizeof log);
            printf("\n---- glslang: %s (%lld SPIR-V words)\n%s\n", ok ? "accepted" : "REJECTED", g_vk_words, log);
        } else
        if (!nocompile) {
            GLint ok = 0;
            char log[8192] = "";
            GLuint p = glCreateShaderProgramv_(s.stage == MX_FRAGMENT ? 0x8B30 : s.stage == MX_COMPUTE ? 0x91B9 : 0x8B31, 1, (const char *const *)&s.glsl);
            glGetProgramiv_(p, 0x8B82, &ok);
            glGetProgramInfoLog_(p, sizeof log - 1, NULL, log);
            printf("\n---- driver: %s\n%s\n", ok ? "accepted" : "REJECTED", log);
        }
        return 0;
    }

    /* load and de-duplicate */
    WIN32_FIND_DATAA fd;
    char pattern[512], path[512];
    snprintf(pattern, sizeof pattern, "%s\\*.mxd", dir);
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) { printf("no .mxd files in %s\n", dir); return 1; }
    int cap = 16384, n = 0, files = 0, nocode = 0, textonly = 0;
    Stage *st = malloc(sizeof *st * (size_t)cap);
    do {
        Stage s;
        snprintf(path, sizeof path, "%s\\%s", dir, fd.cFileName);
        if (!load(path, fd.cFileName, &s)) continue;
        files++;
        if (!s.has_code) { nocode++; if (s.text) textonly++; free(s.data); continue; }
        int dup = -1;
        for (int k = n - 1; k >= 0 && dup < 0; k--) if (st[k].hash == s.hash && st[k].data_len == s.data_len) dup = k;
        if (dup >= 0) { st[dup].uses++; free(s.data); continue; }
        if (n == cap) { cap *= 2; st = realloc(st, sizeof *st * (size_t)cap); }
        st[n++] = s;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    int byst[6] = {0}, usest[6] = {0}, total_uses = 0;
    for (int k = 0; k < n; k++) { byst[st[k].stage]++; usest[st[k].stage] += st[k].uses; total_uses += st[k].uses; }
    printf("translator: %s\nfiles: %d, without machine code: %d (%d of them assembly text only)\nstages with machine code: %d (%d unique): vertex %d, fragment %d, geometry %d, tess %d+%d, compute %d\n",
           v0 ? "v0 (first version)" : "mx_translate.c", files, nocode, textonly, total_uses, n, byst[0], byst[1], byst[2], byst[3], byst[4], byst[5]);

    DWORD t0 = GetTickCount();
    int ntr = 0;
    for (int k = 0; k < n; k++) { translate(&st[k], v0); if (st[k].glsl) ntr++; }
    printf("translation: %.1f s\n", (GetTickCount() - t0) / 1000.0);

    int nok = 0, nrej = 0;
    if (!nocompile && g_vkmode) {
        t0 = GetTickCount();
        for (int k = 0; k < n; k++) {
            char log[4096] = "";
            if (!st[k].glsl) continue;
            if (vk_compile_stage(&st[k], log, sizeof log)) { st[k].status = 1; nok++; }
            else { st[k].status = 3; nrej++; vk_error_line(log, st[k].detail, sizeof st[k].detail); snprintf(st[k].op, sizeof st[k].op, "glslang"); }
            if (k % 256 == 0) fprintf(stderr, "\r  compiled %d / %d", k, n);
        }
        fprintf(stderr, "\n");
        printf("glslang compile to SPIR-V: %.1f s, %lld words\n", (GetTickCount() - t0) / 1000.0, g_vk_words);
    } else
    if (!nocompile) {
        t0 = GetTickCount();
        for (int base = 0; base < n; base += 256) {
            int end = base + 256 < n ? base + 256 : n;
            for (int k = base; k < end; k++)
                if (st[k].glsl) st[k].prog = glCreateShaderProgramv_(st[k].stage == MX_FRAGMENT ? 0x8B30 : st[k].stage == MX_COMPUTE ? 0x91B9 : 0x8B31, 1, (const char *const *)&st[k].glsl);
            for (int k = base; k < end; k++) {
                GLint ok = 0;
                if (!st[k].glsl) continue;
                glGetProgramiv_(st[k].prog, 0x8B82, &ok);
                if (ok) { st[k].status = 1; nok++; }
                else {
                    char log[4096] = "";
                    glGetProgramInfoLog_(st[k].prog, sizeof log - 1, NULL, log);
                    st[k].status = 3; nrej++;
                    first_error_line(log, st[k].detail, sizeof st[k].detail);
                    snprintf(st[k].op, sizeof st[k].op, "driver");
                }
                glDeleteProgram_(st[k].prog);
            }
            fprintf(stderr, "\r  compiled %d / %d", end, n);
        }
        fprintf(stderr, "\n");
        printf("driver compile: %.1f s\n", (GetTickCount() - t0) / 1000.0);
    }

    /* tables */
    int uok = 0, untr = 0, urej = 0;
    for (int k = 0; k < n; k++) { if (st[k].status == 1 || (nocompile && st[k].glsl)) uok += st[k].uses; else if (st[k].status == 2) untr += st[k].uses; else if (st[k].status == 3) urej += st[k].uses; }
    printf("\n                         unique   (all uses)\n");
    printf("  stages                 %6d   (%d)\n", n, total_uses);
    printf("  translated             %6d   \n", ntr);
    if (!nocompile) printf("  compiled by the driver %6d   (%d)\n  rejected by the driver %6d   (%d)\n", nok, uok, nrej, urej);
    printf("  not translatable       %6d   (%d)\n", n - ntr, untr);
    {
        static const char *const sn[6] = {"vertex", "fragment", "geometry", "tess-control", "tess-eval", "compute"};
        printf("\n  per stage        unique  translated  compiled\n");
        for (int q = 0; q < 6; q++) {
            int u = 0, t = 0, c = 0;
            for (int k = 0; k < n; k++) if (st[k].stage == q) { u++; t += st[k].glsl != NULL; c += st[k].status == 1; }
            if (u) printf("  %-14s %8d %11d %9d\n", sn[q], u, t, c);
        }
    }
    static Bucket b[4000];
    int nb = 0;
    for (int k = 0; k < n; k++) if (st[k].status == 2) { Bucket *x = bucket(b, &nb, st[k].op); x->uniq++; x->uses += st[k].uses; }
    qsort(b, (size_t)nb, sizeof b[0], by_uniq);
    if (nb) printf("\nnot translatable, by first unsupported instruction / feature (unique stages, all uses):\n");
    for (int k = 0; k < nb; k++) {
        const char *ex = "";
        for (int q = 0; q < n; q++) if (st[q].status == 2 && !strcmp(st[q].op, b[k].key)) { ex = st[q].detail; break; }
        printf("  %5d %6d  %-22s e.g. %.90s\n", b[k].uniq, b[k].uses, b[k].key, ex);
    }
    nb = 0;
    for (int k = 0; k < n; k++) if (st[k].status == 3) { Bucket *x = bucket(b, &nb, st[k].detail); x->uniq++; x->uses += st[k].uses; }
    qsort(b, (size_t)nb, sizeof b[0], by_uniq);
    if (nb) printf("\nrejected by the driver, by first error (unique stages, all uses):\n");
    for (int k = 0; k < nb; k++) printf("  %5d %6d  %.150s\n", b[k].uniq, b[k].uses, b[k].key);
    if (glsldir) {
        CreateDirectoryA(glsldir, NULL);
        for (int k = 0; k < n; k++) {
            if (st[k].status != 3) continue;
            Bucket *x = bucket(b, &nb, st[k].detail);
            if (x->written >= 3) continue;
            x->written++;
            snprintf(path, sizeof path, "%s\\%s.glsl", glsldir, st[k].name);
            FILE *f = fopen(path, "wb");
            if (f) { fprintf(f, "// %s\n%s", st[k].detail, st[k].glsl); fclose(f); }
        }
    }
    if (outfile) {
        FILE *f = fopen(outfile, "w");
        static const char *const sn[6] = {"vertex", "fragment", "geometry", "tess-control", "tess-eval", "compute"};
        static const char *const ss[4] = {"untested", "OK", "NOTR", "REJECT"};
        for (int k = 0; f && k < n; k++) fprintf(f, "%s\t%s\t%d\t%s\t%s\t%s\n", st[k].name, sn[st[k].stage], st[k].uses, ss[st[k].status], st[k].op, st[k].detail);
        if (f) fclose(f);
    }
    return 0;
}
