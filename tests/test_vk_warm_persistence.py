from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import cc


class WarmPersistenceTests(unittest.TestCase):
    def test_rewrite_flag_survives_valid_records(self):
        source = (ROOT / 'runtime/gpu_vk.c').read_text()
        start = source.index('    int rewrite = !data || joined')
        end = source.index('    if (rewrite) {', start)
        block = source[start:end]
        harness = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned int u32;
typedef struct { unsigned int value; } WarmRecord;
static WarmRecord *warm_records;
static u32 warm_nrecords;
static int warm_record_ok(WarmRecord *r) { return r->value != 0; }
static int warm_see(WarmRecord *r) { return 1; }
static int check(int joined, int recovered_header, int present, int valid) {
    unsigned int head = 42;
    unsigned int bytes[] = {42, valid};
    unsigned char *data = present ? (unsigned char *)bytes : NULL;
    size_t size = present ? sizeof bytes : 0;
''' + block + r'''
    return rewrite;
}
int main(void) {
    if (!check(1, 0, 1, 1)) return 1;
    if (!check(0, 1, 1, 1)) return 2;
    if (!check(0, 0, 0, 1)) return 3;
    if (check(0, 0, 1, 1)) return 4;
    if (!check(0, 0, 1, 0)) return 5;
    puts("ok"); return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            src, exe = path / 'warm.c', path / 'warm.exe'
            src.write_text(harness)
            result = subprocess.run(
                [cc.CLANG, '/nologo', *cc.LINK_FLAGS, str(src),
                 '/Fo' + directory + os.sep, '/Fe' + str(exe),
                 '/link', *cc.LINK_OPTIONS] if os.name == 'nt' else
                ['cc', '-std=c11', str(src), '-o', str(exe)],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_headerless_recovery_keeps_backup(self):
        source = (ROOT / 'runtime/gpu_vk.c').read_text()
        start = source.index('    int recovered_header = 0;')
        end = source.index('    /* The shipped list', start)
        block = source[start:end]
        harness = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned char u8;
typedef struct { unsigned int value, checksum; } WarmRecord;
static int warm_record_ok(WarmRecord *r) { return r->checksum == (r->value ^ 123); }
static int result;
static void recover(void) {
    const char *path = "list.warm";
    struct { char magic[8]; unsigned int version; } head = {"OSVKWRM", 1};
    size_t size = 2 * sizeof(WarmRecord);
    u8 *data = malloc(size);
    WarmRecord records[] = {{1, 1 ^ 123}, {2, 2 ^ 123}};
    memcpy(data, records, size);
    FILE *original = fopen(path, "wb");
    fwrite(data, 1, size, original); fclose(original);
''' + block + r'''
    if (!recovered_header || size != sizeof head + sizeof records ||
        memcmp(data, &head, sizeof head) || memcmp(data + sizeof head, records, sizeof records)) return;
    FILE *backup = fopen("list.warm.headerless.bak", "rb");
    WarmRecord saved[2];
    if (!backup) return;
    size_t got = fread(saved, 1, sizeof saved, backup); fclose(backup);
    if (got != sizeof saved || memcmp(saved, records, sizeof records)) return;
    free(data); result = 1;
}
int main(void) { recover(); return result ? 0 : 1; }
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            src, exe = path / 'recover.c', path / 'recover.exe'
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
            self.assertEqual((path / 'list.warm').read_bytes(),
                             (path / 'list.warm.headerless.bak').read_bytes())
