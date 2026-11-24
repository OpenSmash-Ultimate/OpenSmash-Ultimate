from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import cc


class TranslationCacheTests(unittest.TestCase):
    def test_roundtrip_and_invalid_records_fall_back(self):
        source = (ROOT / 'runtime/gpu_vk.c').read_text()
        block = source[source.index('typedef struct TranslationHeader {'):source.index('/* The program\'s SPIR-V by glslang:')]
        header = (ROOT / 'runtime/gpu_shader.h').read_text()
        info = header[header.index('typedef struct ShaderTexture {'):header.index('/* NVN describes')]
        harness = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
enum { SHADER_VERTEX, SHADER_PIXEL, SHADER_COMPUTE };
enum { TEX_ARRAY_CUBE = 7 };
#define SHADER_CACHE_SPV_VERSION 123
static unsigned spv_version(void) { return SHADER_CACHE_SPV_VERSION; }   /* (gpu_vk.c's, without the AMD variant) */
''' + info + r'''
typedef struct Job { struct Job *next; void (*run)(struct Job *); } Job;
static Job *pending;
static void job_add(Job *job) { if (pending) exit(102); pending = job; }
static void drain(void) { Job *job = pending; pending = NULL; if (job) job->run(job); }
typedef struct { int stage; u64 hash; u32 vs_outputs; ShaderInfo info; } VShader;
static const char *cache_dir(void) { return "."; }
static u32 spirv_glslang_version(void) { return 1; }
static u32 desc_hash(const void *data, size_t n) {
    const u8 *p = data; u32 h = 2166136261u;
    while (n--) h = (h ^ *p++) * 16777619u;
    return h;
}
static void write_file(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb"); if (!f) exit(100);
    if (fwrite(data, 1, n, f) != n) exit(101); fclose(f);
}
''' + block + r'''
int main(void) {
    VShader s = {0}; s.hash = 42; s.info.stage = SHADER_VERTEX;
    if (translation_load(&s)) return 1;
    s.info.outputs = 77; translation_save(&s, "source");
    if (!pending || translation_load(&s)) return 9;
    drain(); s.info.outputs = 0;
    char *text = translation_load(&s);
    if (!text || strcmp(text, "source") || s.info.outputs != 77) return 2;
    free(text);
    char path[1200]; translation_path(path, sizeof path, &s);
    FILE *f = fopen(path, "r+b"); TranslationHeader head;
    fread(&head, 1, sizeof head, f); head.info.outputs ^= 1;
    rewind(f); fwrite(&head, 1, sizeof head, f); fclose(f);
    if (translation_load(&s)) return 3;
    translation_save(&s, "source"); drain();
    f = fopen(path, "ab"); fputc(1, f); fclose(f);
    if (translation_load(&s)) return 4;
    f = fopen(path, "wb"); fputc(1, f); fclose(f);
    if (translation_load(&s)) return 5;
    s.hash = 43; s.info.ntables = 1; translation_save(&s, "dependent");
    if (translation_load(&s)) return 6;
    s.info.ntables = 0; s.info.ntex = 33; translation_save(&s, "invalid");
    if (translation_load(&s)) return 7;
    s.info.ntex = 0; s.stage = SHADER_PIXEL;
    if (translation_load(&s)) return 8;
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            src, exe = path / 'cache.c', path / 'cache.exe'
            src.write_text(harness)
            run = subprocess.run(
                [cc.CLANG, '/nologo', *cc.LINK_FLAGS, str(src),
                 '/Fo' + directory + os.sep, '/Fe' + str(exe),
                 '/link', *cc.LINK_OPTIONS] if os.name == 'nt' else
                ['cc', '-std=c11', str(src), '-o', str(exe)],
                capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            run = subprocess.run([str(exe)], cwd=directory, capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
