/* runtime/gpu_nvcache.c with no game around it, for tests/test_nv_cache.py.
 *
 *   Windows: clang-cl /O1 <cc.CFLAGS> tools/dev/nvcachetest.c
 *
 * Arguments, each answered on a line of its own:
 *   trim DIR MB KEEP_MS   nv_cache_trim(DIR, MB megabytes, KEEP_MS): "trim LEFT REMOVED REMOVED_BYTES"
 *   words FOUND MB        nv_cache_setting_words: "words TEXT"
 *   setup DIR             nv_cache_vulkan_setup(DIR): "setup PROCESS|CRT", __GL_SHADER_DISK_CACHE_PATH as the
 *                         process and this program's C runtime see it after ("-": not set)
 *   check                 the driver's settings and folders, on the check's thread, waited for (its log lines on stderr) */
#include "../../runtime/gpu_nvcache.c"

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "trim") && i + 3 < argc) {
            uint32_t removed = 0;
            uint64_t bytes = 0, left = nv_cache_trim(argv[i + 1], strtoull(argv[i + 2], NULL, 10) << 20, strtoull(argv[i + 3], NULL, 10), &removed, &bytes);
            printf("trim %llu %u %llu\n", (unsigned long long)left, removed, (unsigned long long)bytes);
            i += 3;
        } else if (!strcmp(argv[i], "words") && i + 2 < argc) {
            char w[32];
            nv_cache_setting_words(atoi(argv[i + 1]), (uint32_t)strtoul(argv[i + 2], NULL, 0), w, sizeof w);
            printf("words %s\n", w);
            i += 2;
        } else if (!strcmp(argv[i], "setup") && i + 1 < argc) {
            nv_cache_vulkan_setup(argv[++i]);
            char v[MAX_PATH] = "-";
            if (!GetEnvironmentVariableA("__GL_SHADER_DISK_CACHE_PATH", v, sizeof v)) strcpy(v, "-");
            const char *c = getenv("__GL_SHADER_DISK_CACHE_PATH");
            printf("setup %s|%s\n", v, c ? c : "-");
        } else if (!strcmp(argv[i], "check")) {
            check_main((void *)(intptr_t)1);
            printf("check\n");
        } else {
            fprintf(stderr, "nvcachetest: what is %s?\n", argv[i]);
            return 2;
        }
        fflush(stdout);
    }
    return 0;
}
