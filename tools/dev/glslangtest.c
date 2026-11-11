/* The Vulkan back end's glslang path checked on the game's programs, without the game: every vertex and
 * pixel program of tools/shader_aot.py's pack (dump/aot/programs.pack) translated to the HLSL of
 * gpu_shader.c's SHADER_HLSL_GLSLANG dialect and turned into SPIR-V by ssbu_glslang.dll
 * (runtime/gpu_spirv.c's spirv_from_hlsl), as gpu_vk.c does at a first draw.
 *
 * Usage: glslangtest <pack> [-every N] [-out <dir>] [-val <spirv-val.exe>] [-v]
 *   prints how many programs, how many made SPIR-V, the time each took (mean, longest), and the
 *   first failures (-v: all, with glslang's messages); -out keeps the .spv files; -val runs SPIRV-Tools'
 *   validator on each (for Vulkan 1.1). A pixel program is translated with the outputs of the first
 *   vertex program packed before it (its pair), as the renderer pairs them; with none, all 32.
 *   Exit status 0 when every program translated made SPIR-V (and passed -val).
 *
 * Build (Windows, beside ssbu_glslang.dll, which it loads from its own folder: tools/glslang_lib.py):
 *   clang-cl /O2 <cc.CFLAGS> tools/dev/glslangtest.c runtime/gpu_shader.c /Fe<folder>\glslangtest.exe */
#define _CRT_SECURE_NO_WARNINGS
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "../../runtime/gpu_shader_cache.h"
#include "../../runtime/gpu_spirv.c"

u32 shader_size(const u8 *program, u32 max);

static jmp_buf on_fatal;
_Noreturn void rt_fatal(const char *fmt, ...) { (void)fmt; longjmp(on_fatal, 1); }
static u32 zero_cbuf(void *user, u32 bank, u32 offset) { (void)user; (void)bank; (void)offset; return 0; }

static char *translate(const u8 *p, u32 size, int stage, u32 vs_outputs, ShaderInfo *info) {
    if (setjmp(on_fatal)) return NULL;
    return shader_translate(SHADER_HLSL_GLSLANG, p, size, stage, vs_outputs, NULL, 0, info, zero_cbuf, NULL);
}

static double now_ms(void) {
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: glslangtest <pack> [-every N] [-out <dir>] [-val <spirv-val.exe>] [-v]\n"); return 2; }
    const char *out_dir = NULL, *val = NULL;
    unsigned every = 1;
    int verbose = 0;
    for (int a = 2; a < argc; a++) {
        if (!strcmp(argv[a], "-every") && a + 1 < argc) every = (unsigned)atoi(argv[++a]);
        else if (!strcmp(argv[a], "-out") && a + 1 < argc) out_dir = argv[++a];
        else if (!strcmp(argv[a], "-val") && a + 1 < argc) val = argv[++a];
        else if (!strcmp(argv[a], "-v")) verbose = 1;
        else { fprintf(stderr, "unknown argument %s\n", argv[a]); return 2; }
    }
    if (!every) every = 1;
    char why[300];
    if (!spirv_have_glslang(why, sizeof why)) { fprintf(stderr, "no glslang: %s\n", why); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    _fseeki64(f, 0, SEEK_END);
    long long n = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    u8 *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n || n < 8 || memcmp(buf, "SAOT", 4)) { fprintf(stderr, "not a pack\n"); return 1; }
    fclose(f);
    u32 count = *(u32 *)(buf + 4);
    u32 *outputs = calloc(count, sizeof *outputs);   /* a vertex program's outputs, + 1 (0: none) */
    u64 *seen = calloc(1u << 20, sizeof *seen);
    const u8 *p = buf + 8, *end = buf + n;
    unsigned programs = 0, made = 0, failed = 0, untranslated = 0, invalid = 0, shown = 0;
    double total = 0, longest = 0;
    for (u32 i = 0; i < count && p + 12 <= end; i++) {
        u32 stage = ((const u32 *)p)[0], len = ((const u32 *)p)[1], np = ((const u32 *)p)[2];
        const u32 *pairs = (const u32 *)(p + 12);
        const u8 *bytes = p + 12 + 4 * (size_t)np;
        p = bytes + len;
        if (len <= 0x50 || p > end) continue;
        u32 size = shader_size(bytes, len);
        if (size <= 0x50 || size >= 0x40000) continue;
        u32 vs_outputs = 0xFFFFFFFFu;
        if (stage == SHADER_PIXEL)
            for (u32 k = 0; k < np; k++)
                if (pairs[k] < count && outputs[pairs[k]]) { vs_outputs = outputs[pairs[k]] - 1; break; }
        ShaderInfo info;
        int sample = i % every == 0;
        if (stage == SHADER_VERTEX || sample) {
            u64 key = shader_program_hash(bytes, size) * 3 + vs_outputs * 7 + stage;
            u32 slot = (u32)(key >> 44);
            while (seen[slot] && seen[slot] != key) slot = (slot + 1) & ((1u << 20) - 1);
            int dup = seen[slot] == key;
            seen[slot] = key;
            char *hlsl = translate(bytes, size, (int)stage, stage == SHADER_PIXEL ? vs_outputs : 0, &info);
            if (stage == SHADER_VERTEX && hlsl) outputs[i] = info.outputs + 1;
            if (!sample || dup) { free(hlsl); continue; }
            programs++;
            if (!hlsl) { untranslated++; continue; }
            SpirvCode spv;
            char err[2000];
            double t0 = now_ms();
            int ok = spirv_from_hlsl(hlsl, (int)stage, &spv, err, sizeof err);
            double ms = now_ms() - t0;
            total += ms;
            if (ms > longest) longest = ms;
            if (!ok) {
                failed++;
                if (verbose || shown++ < 5) fprintf(stderr, "%s %u: %.600s\n", stage ? "ps" : "vs", i, err);
                free(hlsl);
                continue;
            }
            made++;
            char path[1024] = "";
            if (out_dir || val) {
                snprintf(path, sizeof path, "%s/%s_%05u.spv", out_dir ? out_dir : ".", stage ? "ps" : "vs", i);
                FILE *o = fopen(path, "wb");
                if (o) { fwrite(spv.code, 1, spv.size, o); fclose(o); }
            }
            if (val) {
                char cmd[2300];
                snprintf(cmd, sizeof cmd, "\"\"%s\" --target-env vulkan1.1 \"%s\"\"", val, path);
                if (system(cmd) != 0) { invalid++; fprintf(stderr, "  (%s %u: spirv-val refused it)\n", stage ? "ps" : "vs", i); }
                if (!out_dir) DeleteFileA(path);
            }
            spirv_free(&spv);
            free(hlsl);
        }
    }
    printf("%u programs: %u made SPIR-V, %u failed, %u not translated, %u refused by spirv-val; %.1f ms each, the longest %.0f ms\n",
           programs, made, failed, untranslated, invalid, made + failed ? total / (made + failed) : 0.0, longest);
    return failed || invalid ? 1 : 0;
}
