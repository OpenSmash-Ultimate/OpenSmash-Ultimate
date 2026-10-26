/* Native side of the differential tester: runs lifted single instructions / short
 * snippets from given CPU states and records the resulting state for comparison with a
 * reference AArch64 emulator. Usage: difftest <image> <scratch.bin> <in.bin> <out.bin>
 *
 * The image and the scratch window sit at their guest addresses plus RT_MEMBASE, where the
 * lifted code's memory helpers look for them (recomp.h). A test that faults (an access
 * outside them, rt_trap) is recorded as a fault and the run goes on: structured exception
 * handling on Windows, signal handlers and siglongjmp on Linux and macOS. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "recomp.h"

#define BASE 0x7100000000ull
#define SCRATCH 0x7200000000ull
#define SCRATCH_SIZE 0x20000

typedef struct TestIO { u32 n, z, c, v; u64 pc; } TestIO;
typedef void TestFn(Cpu *, TestIO *);
extern TestFn *const t_funcs[];
extern const u32 t_count;

struct In { u32 idx, pad; u64 x[31], sp, nzcv, v[64]; };
struct Out { u64 x[31], sp, nzcv, v[64], pc, dirty_off, dirty_len; u8 dirty[64]; u64 fault; };

#ifdef _WIN32
_Noreturn void rt_trap(Cpu *c, u64 pc, const char *why) {
    (void)c; (void)pc; (void)why;
    RaiseException(0xE0000001, 0, 0, NULL);
    abort();
}

static int run(TestFn *fn, Cpu *cpu, TestIO *io) {
    __try {
        fn(cpu, io);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
    return 0;
}
#define fseek64 _fseeki64
#define ftell64 _ftelli64
#else
static sigjmp_buf fault_jb;

_Noreturn void rt_trap(Cpu *c, u64 pc, const char *why) {
    (void)c; (void)pc; (void)why;
    siglongjmp(fault_jb, 1);
}

static void on_fault(int sig) { (void)sig; siglongjmp(fault_jb, 1); }

static int run(TestFn *fn, Cpu *cpu, TestIO *io) {
    if (sigsetjmp(fault_jb, 1))   /* (1: the signal mask comes back too, unblocking the signal) */
        return 1;
    fn(cpu, io);
    return 0;
}

/* Fresh memory at a guest address (host RT_HOST(guest)), never over a mapping already there. */
static u8 *map_at(u64 guest, size_t size) {
    void *want = RT_HOST(guest);
    int flags = MAP_PRIVATE | MAP_ANON;
#ifdef MAP_FIXED_NOREPLACE
    flags |= MAP_FIXED_NOREPLACE;   /* (Linux 4.17 on; an older kernel takes the address as a hint: checked below) */
#endif
    void *p = mmap(want, size, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (p != want) {
        fprintf(stderr, "cannot map %#zx bytes at %p (guest %#llx): %s\n", size, want, (unsigned long long)guest,
                p == MAP_FAILED ? strerror(errno) : "the address is taken");
        exit(2);
    }
    return p;
}
#define fseek64 fseeko
#define ftell64 ftello
#endif

GuestFn *rt_lookup(u64 addr) { rt_trap(NULL, addr, "indirect call inside a test"); }
GuestFn *rt_lookup_cached(u64 addr, RtCache *cache) { (void)cache; return rt_lookup(addr); }

static void *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek64(f, 0, SEEK_END);
    *size = (size_t)ftell64(f);
    fseek64(f, 0, SEEK_SET);
    void *p = malloc(*size ? *size : 1);
    if (fread(p, 1, *size, f) != *size) { fprintf(stderr, "short read: %s\n", path); exit(2); }
    fclose(f);
    return p;
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: difftest <image> <scratch.bin> <in.bin> <out.bin>\n"); return 2; }

    size_t n;
#ifdef _WIN32
    HANDLE file = CreateFileA(argv[1], GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    HANDLE map = file == INVALID_HANDLE_VALUE ? NULL : CreateFileMappingA(file, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!map || MapViewOfFileEx(map, FILE_MAP_READ, 0, 0, 0, (void *)BASE) != (void *)BASE) {
        fprintf(stderr, "cannot map %s at %#llx\n", argv[1], BASE);
        return 2;
    }
    u8 *scratch = VirtualAlloc((void *)SCRATCH, SCRATCH_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    u8 *image = read_file(argv[1], &n);
    size_t mapped = (n + page - 1) & ~(page - 1);
    u8 *at = map_at(BASE, mapped ? mapped : page);
    memcpy(at, image, n);
    if (mprotect(at, mapped ? mapped : page, PROT_READ)) { perror("mprotect"); return 2; }
    free(image);
    u8 *scratch = map_at(SCRATCH, SCRATCH_SIZE);
    static const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE};
    for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_fault;
        sigemptyset(&sa.sa_mask);
        if (sigaction(sigs[i], &sa, NULL)) { perror("sigaction"); return 2; }
    }
#endif
    u8 *pristine = read_file(argv[2], &n);
    if (scratch != (u8 *)RT_HOST(SCRATCH) || n != SCRATCH_SIZE) { fprintf(stderr, "scratch setup failed\n"); return 2; }
    memcpy(scratch, pristine, SCRATCH_SIZE);

    struct In *in = read_file(argv[3], &n);
    size_t count = n / sizeof *in;
    struct Out *out = calloc(count ? count : 1, sizeof *out);
    for (size_t t = 0; t < count; t++) {
        Cpu cpu = {0};
        TestIO io = {(u32)(in[t].nzcv >> 3) & 1, (u32)(in[t].nzcv >> 2) & 1, (u32)(in[t].nzcv >> 1) & 1,
                     (u32)in[t].nzcv & 1, 0};
        memcpy(cpu.x, in[t].x, sizeof cpu.x);
        cpu.sp = in[t].sp;
        memcpy(cpu.v, in[t].v, sizeof cpu.v);
        struct Out *o = &out[t];
        o->fault = in[t].idx >= t_count ? 2 : run(t_funcs[in[t].idx], &cpu, &io);
        memcpy(o->x, cpu.x, sizeof cpu.x);
        o->sp = cpu.sp;
        o->nzcv = (u64)io.n << 3 | io.z << 2 | io.c << 1 | io.v;
        memcpy(o->v, cpu.v, sizeof cpu.v);
        o->pc = io.pc;
        o->dirty_off = ~0ull;
        if (memcmp(scratch, pristine, SCRATCH_SIZE)) {
            size_t lo = 0, hi = SCRATCH_SIZE;
            while (scratch[lo] == pristine[lo]) lo++;
            while (scratch[hi - 1] == pristine[hi - 1]) hi--;
            o->dirty_off = lo;
            o->dirty_len = hi - lo;
            memcpy(o->dirty, scratch + lo, hi - lo < 64 ? hi - lo : 64);
            memcpy(scratch + lo, pristine + lo, hi - lo);
        }
    }
    FILE *f = fopen(argv[4], "wb");
    if (!f || fwrite(out, sizeof *out, count, f) != count) { perror(argv[4]); return 2; }
    fclose(f);
    return 0;
}
