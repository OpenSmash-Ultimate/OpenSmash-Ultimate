/* Offline checks of the shared fonts (runtime/hle_pl.c): the font area laid out from a folder of
 * firmware fonts and a folder of open ones, as the game's first nn::pl call lays it out, then
 * what nn::pl tells the game of each font type. No game files: tests/test_pl_fonts.py makes up
 * the firmware's, and the open ones are made up or third_party/fonts_open's.
 *
 *   Windows: clang-cl /nologo /O1 tools\dev\pltest.c /Febuild\pltest.exe   (with cc.BASE_CFLAGS)
 *   Linux and macOS: clang -O1 -I runtime/macos/include -I runtime/linux/include -I runtime tools/dev/pltest.c -o build/pltest
 *
 *   pltest <firmware folder> <open folder> <area file> [area size]
 *       writes the area, up to the end of its last font, to <area file>, and prints for each
 *       type, and one past them: "type <n> state <s> offset <o> size <z> from <none|firmware|open>",
 *       <o> from the area's start (0 for no font), after the runtime's own lines
 *
 * Exit code 0 when it ran. */
#include <stdarg.h>
#include "../../runtime/hle_pl.c"

const char *rt_data_dir = ".";
void kernel_add_region(u64 base, u64 size, u32 state, u32 perm) { (void)base; (void)size; (void)state; (void)perm; }
_Noreturn void rt_fatal(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    printf("FATAL: ");
    vprintf(fmt, a);
    va_end(a);
    printf("\n");
    exit(3);
}

#ifndef _WIN32
/* (the platform layer's, which this leaves out: the area is not mapped at SHARED_BASE here) */
LPVOID VirtualAlloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect) { (void)address; (void)size; (void)type; (void)protect; return NULL; }
BOOL InitOnceExecuteOnce(PINIT_ONCE o, PINIT_ONCE_FN fn, PVOID param, LPVOID *context) {
    if (!o->done) { fn(o, param, context); o->done = 1; }
    return TRUE;
}
#endif

static BOOL CALLBACK laid_out(INIT_ONCE *o, void *p, void **ctx) { (void)o; (void)p; (void)ctx; return TRUE; }

int main(int argc, char **argv) {
    if (argc < 4) { printf("usage: pltest <firmware folder> <open folder> <area file> [area size]\n"); return 2; }
    size_t room = argc > 4 ? (size_t)strtoull(argv[4], NULL, 0) : AREA_SIZE;
    u8 *area = calloc(1, room + 1);
    if (!area) { printf("no memory for the area\n"); return 2; }
    pack_fonts(area, SHARED_BASE, room, argv[1], argv[2]);
    fflush(stderr);
    InitOnceExecuteOnce(&once, laid_out, NULL, NULL);   /* (so the calls below find it laid out) */
    static const char *const from[] = {"none", "firmware", "open"};
    u64 end = 0;
    for (u32 t = 0; t <= FONTS; t++) {
        Cpu c = {0};
        c.x[0] = t;
        hle__ZN2nn2pl21RequestSharedFontLoadENS0_14SharedFontTypeE(&c);
        c.x[0] = t;
        hle__ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE(&c);
        u64 state = c.x[0];
        c.x[0] = t;
        hle__ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE(&c);
        u64 address = c.x[0];
        c.x[0] = t;
        hle__ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE(&c);
        u64 size = c.x[0];
        u64 offset = address ? address - SHARED_BASE : 0;
        if (address && offset + size > end) end = offset + size;
        printf("type %u state %llu offset %llu size %llu from %s\n", t, (unsigned long long)state, (unsigned long long)offset,
               (unsigned long long)size, t < FONTS ? from[font[t].from] : "-");
    }
    FILE *f = fopen(argv[3], "wb");
    if (!f || fwrite(area, 1, (size_t)end, f) != (size_t)end) { printf("cannot write %s\n", argv[3]); return 2; }
    fclose(f);
    free(area);
    return 0;
}
