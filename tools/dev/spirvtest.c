/* runtime/gpu_spirv.c's DXBC to SPIR-V, checked (tests/test_spirv.py).
 *
 * Usage: spirvtest -self
 *          a vertex, a pixel and a compute program with every kind of register gpu_shader.c's HLSL
 *          uses, compiled with fxc and translated; prints "<stage> set <s> binding <b>" for each
 *          resource in the SPIR-V (the binding map, gpu_spirv.h), then "self ok" or what failed
 *        spirvtest <shader cache dir> [-v] [-out <dir>] [-val <spirv-val.exe>] [-max N]
 *          every program of the disk cache (gpu_shader_cache.h's .dxbc files) translated: how many,
 *          how long, which failed (-v: vkd3d-shader's messages); -out keeps the .spv files; -val
 *          has SPIRV-Tools' validator check each one (for Vulkan 1.1). Built as the runtime is
 *          (tools/cc.py's flags: with SSBU_HAVE_VKD3D when deps/vkd3d has the library). */
#define _CRT_SECURE_NO_WARNINGS
#define COBJMACROS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")

#include "../../runtime/gpu_spirv.c"

static const char *const STAGES[] = {"vs", "ps", "cs"};

/* The (set, binding) of each decorated variable: OpDecorate DescriptorSet / Binding. */
static int bindings_of(const SpirvCode *c, unsigned out[][2], int max) {
    const unsigned *w = c->code;
    size_t n = c->size / 4;
    if (n < 5 || w[0] != 0x07230203) return -1;
    static unsigned set_of[1 << 16], binding_of[1 << 16];
    static unsigned char has[1 << 16];
    memset(has, 0, sizeof has);
    for (size_t i = 5; i < n;) {
        unsigned op = w[i] & 0xFFFF, len = w[i] >> 16;
        if (!len || i + len > n) return -1;
        if (op == 71 && len >= 4 && w[i + 1] < (1u << 16)) {   /* OpDecorate target decoration value */
            if (w[i + 2] == 34) { set_of[w[i + 1]] = w[i + 3]; has[w[i + 1]] |= 1; }
            if (w[i + 2] == 33) { binding_of[w[i + 1]] = w[i + 3]; has[w[i + 1]] |= 2; }
        }
        i += len;
    }
    int k = 0;
    for (unsigned id = 0; id < (1u << 16) && k < max; id++)
        if (has[id] == 3) { out[k][0] = set_of[id]; out[k][1] = binding_of[id]; k++; }
    return k;
}

static int self_test(void) {
    static const char *const SRC[] = {
        "cbuffer Params : register(b0) { float4 a; };\n"
        "ByteAddressBuffer cb3 : register(t3); ByteAddressBuffer vb0 : register(t18); ByteAddressBuffer gm1 : register(t97);\n"
        "float4 main(uint id : SV_VertexID) : SV_Position {\n"
        "  return a + asfloat(cb3.Load4(id * 16)) + asfloat(vb0.Load(id * 4)).xxxx + asfloat(gm1.Load(4)).xxxx; }\n",
        "cbuffer Params : register(b0) { float4 a; };\n"
        "Texture2D tex0 : register(t64); SamplerState smp0 : register(s0); ByteAddressBuffer cb1 : register(t1);\n"
        "float4 main(float4 p : SV_Position) : SV_Target { return tex0.Sample(smp0, p.xy) * a + asfloat(cb1.Load(0)).xxxx; }\n",
        "RWByteAddressBuffer gm0 : register(u0); RWStructuredBuffer<uint> gm2 : register(u2); ByteAddressBuffer cb0 : register(t0);\n"
        "[numthreads(64, 1, 1)] void main(uint3 i : SV_DispatchThreadID) { gm0.Store(i.x * 4, gm2[i.x] + cb0.Load(0)); }\n",
    };
    static const char *const PROFILE[] = {"vs_5_0", "ps_5_0", "cs_5_0"};
    int failed = 0;
    for (int s = 0; s < 3; s++) {
        ID3DBlob *code = NULL, *errors = NULL;
        if (FAILED(D3DCompile(SRC[s], strlen(SRC[s]), NULL, NULL, NULL, "main", PROFILE[s], 0, 0, &code, &errors))) {
            printf("%s: fxc failed: %s\n", STAGES[s], errors ? (char *)ID3D10Blob_GetBufferPointer(errors) : "?");
            failed = 1;
            continue;
        }
        SpirvCode spv;
        char err[4096];
        if (!spirv_from_dxbc(ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), s, &spv, err, sizeof err)) {
            printf("%s: not translated: %s\n", STAGES[s], err);
            failed = 1;
        } else {
            unsigned b[64][2];
            int n = bindings_of(&spv, b, 64);
            if (n < 0) { printf("%s: not SPIR-V\n", STAGES[s]); failed = 1; }
            for (int i = 0; i < n; i++) printf("%s set %u binding %u\n", STAGES[s], b[i][0], b[i][1]);
            spirv_free(&spv);
        }
        ID3D10Blob_Release(code);
        if (errors) ID3D10Blob_Release(errors);
    }
    printf(failed ? "self failed\n" : "self ok\n");
    return failed;
}

static double now_ms(void) {
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);   /* (each line out before a fault) */
    if (argc < 2) { fprintf(stderr, "usage: spirvtest -self | <shader cache dir> [-v] [-out dir] [-val spirv-val.exe] [-max N]\n"); return 2; }
    if (!spirv_available()) { printf("no vkd3d-shader in this build\n"); return 3; }
    if (!strcmp(argv[1], "-self")) return self_test();
    const char *out = NULL, *val = NULL;
    int verbose = 0, max = 1 << 30;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-val") && i + 1 < argc) val = argv[++i];
        else if (!strcmp(argv[i], "-max") && i + 1 < argc) max = atoi(argv[++i]);
    }
    if (out) CreateDirectoryA(out, NULL);
    char pattern[1100], tmp[MAX_PATH] = "";
    snprintf(pattern, sizeof pattern, "%s\\v*_*.dxbc", argv[1]);
    if (val && !out) {   /* (the validator reads a file) */
        GetTempPathA(MAX_PATH, tmp);
        strcat(tmp, "spirvtest.spv");
    }
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) { printf("no .dxbc files in %s\n", argv[1]); return 1; }
    int total[3] = {0}, ok[3] = {0}, invalid = 0;
    double ms = 0, worst = 0;
    size_t bytes_in = 0, bytes_out = 0;
    char worst_name[300] = "";
    static unsigned char buf[4 << 20];
    do {
        const char *u = strchr(fd.cFileName, '_');
        int s = !u ? -1 : !strncmp(u + 1, "vs_", 3) ? 0 : !strncmp(u + 1, "ps_", 3) ? 1 : !strncmp(u + 1, "cs_", 3) ? 2 : -1;
        if (s < 0 || total[0] + total[1] + total[2] >= max) continue;
        char path[1400];
        snprintf(path, sizeof path, "%s\\%s", argv[1], fd.cFileName);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        size_t n = fread(buf, 1, sizeof buf, f);
        fclose(f);
        total[s]++;
        bytes_in += n;
        SpirvCode spv;
        char err[8192];
        double t0 = now_ms();
        int done = spirv_from_dxbc(buf, n, s, &spv, err, sizeof err);
        double t = now_ms() - t0;
        ms += t;
        if (t > worst) { worst = t; snprintf(worst_name, sizeof worst_name, "%s", fd.cFileName); }
        if (!done) {
            printf("FAILED %s%s%s", fd.cFileName, verbose ? ": " : "\n", verbose ? err : "");
            if (verbose) printf("\n");
            continue;
        }
        ok[s]++;
        bytes_out += spv.size;
        char spv_path[1400] = "";
        if (out) snprintf(spv_path, sizeof spv_path, "%s\\%.*s.spv", out, (int)(strlen(fd.cFileName) - 5), fd.cFileName);
        else if (val) snprintf(spv_path, sizeof spv_path, "%s", tmp);
        if (spv_path[0]) {
            FILE *o = fopen(spv_path, "wb");
            if (o) { fwrite(spv.code, 1, spv.size, o); fclose(o); }
        }
        if (val && spv_path[0]) {
            char cmd[3000];
            snprintf(cmd, sizeof cmd, "\"\"%s\" --target-env vulkan1.1 \"%s\" 2>&1\"", val, spv_path);
            FILE *p = _popen(cmd, "r");
            char line[1024], said[4096] = "";
            while (p && fgets(line, sizeof line, p)) if (strlen(said) + strlen(line) < sizeof said) strcat(said, line);
            int rc = p ? _pclose(p) : -1;
            if (rc) { invalid++; printf("INVALID %s: %s", fd.cFileName, said); }
        }
        spirv_free(&spv);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    int all = total[0] + total[1] + total[2], good = ok[0] + ok[1] + ok[2];
    printf("translated %d of %d (vs %d/%d, ps %d/%d, cs %d/%d) in %.0f ms, longest %.1f ms (%s); DXBC %.1f MB -> SPIR-V %.1f MB",
           good, all, ok[0], total[0], ok[1], total[1], ok[2], total[2], ms, worst, worst_name, bytes_in / 1048576.0, bytes_out / 1048576.0);
    if (val) printf("; %d invalid", invalid);
    printf("\n");
    return good == all && !invalid ? 0 : 1;
}
