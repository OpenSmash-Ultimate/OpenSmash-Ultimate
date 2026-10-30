/* Patch one compiled function so the calls the recompiler baked in reach a hook.
 *
 * Indirect calls already go through the dispatch table (rt_hook_function swaps that slot).
 * A direct call is a jump to this function's first bytes. Those bytes are replaced with a
 * jump to the hook, and the replaced bytes are kept in a trampoline that still runs the
 * function as it was. A later hook on the same function retargets that one jump, so the
 * new hook runs first and its original is the previous hook. Every other function is left
 * as it was. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdint.h>
#include <string.h>

#ifdef DETOUR_TEST
#include <stdint.h>
#include <stdio.h>
#include <windows.h>
typedef struct Cpu Cpu;
typedef void GuestFn(Cpu *c);
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t i32;
typedef int64_t i64;
#else
#include "host.h"
#endif

#include "hde/hde64.c"

#define PATCH_LEN 5

typedef struct Detour {
    GuestFn *body;         /* the function whose first bytes were overwritten */
    GuestFn *trampoline;   /* those bytes, then a jump back to the rest */
    u8 *stub;              /* a jump to `current`, within reach of `body` */
    GuestFn *current;      /* the hook direct calls reach now */
    struct Detour *next;
} Detour;

static Detour *detours;
static u8 *cave_base;
static u32 cave_off;

static int falls_through(const hde64s *hs) {
    if (hs->opcode == 0xC3 || hs->opcode == 0xC2 || hs->opcode == 0xCC ||
        hs->opcode == 0xE9 || hs->opcode == 0xEB)
        return 0;
    if (hs->opcode == 0xFF && (hs->flags & F_MODRM) && hs->modrm_reg == 4)
        return 0;
    return 1;
}

static int is_rip(const hde64s *hs) {
    return (hs->flags & F_MODRM) && !(hs->flags & F_SIB) && hs->modrm_mod == 0 && hs->modrm_rm == 5;
}

static u32 imm_size(const hde64s *hs) {
    if (hs->flags & F_IMM64) return 8;
    if (hs->flags & F_IMM32) return 4;
    if (hs->flags & F_IMM16) return 2;
    if (hs->flags & F_IMM8) return 1;
    return 0;
}

static int rel_target(const u8 *ip, const hde64s *hs, u64 *to) {
    i64 disp;
    if (!(hs->flags & F_RELATIVE)) return 0;
    if (hs->flags & F_IMM32) disp = (i32)hs->imm.imm32;
    else if (hs->flags & F_IMM8) disp = (i32)(int8_t)hs->imm.imm8;
    else return 0;
    *to = (u64)(ip + hs->len) + disp;
    return 1;
}

static u8 *emit_abs_jmp(u8 *p, u8 *end, u64 target) {
    if ((u32)(end - p) < 14) return NULL;
    *p++ = 0xFF; *p++ = 0x25;
    *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 0;
    memcpy(p, &target, 8);
    return p + 8;
}

static u8 *emit_abs_call(u8 *p, u8 *end, u64 target) {
    if ((u32)(end - p) < 16) return NULL;
    *p++ = 0xFF; *p++ = 0x15;
    *p++ = 2; *p++ = 0; *p++ = 0; *p++ = 0;
    *p++ = 0xEB; *p++ = 8;
    memcpy(p, &target, 8);
    return p + 8;
}

static u8 *emit_jcc(u8 *p, u8 *end, u8 inverted_near, u64 target) {
    if ((u32)(end - p) < 20) return NULL;
    *p++ = 0x0F;
    *p++ = inverted_near;
    *(u32 *)p = 14;
    p += 4;
    return emit_abs_jmp(p, end, target);
}

static u8 *emit_copy(u8 *p, u8 *end, const u8 *ip, const hde64s *hs) {
    u32 isz, at;
    i32 old, nd32;
    i64 nd;
    u64 abs;
    if ((u32)(end - p) < hs->len) return NULL;
    memcpy(p, ip, hs->len);
    if (!is_rip(hs)) return p + hs->len;
    isz = imm_size(hs);
    if (hs->len < isz + 4) return NULL;
    at = hs->len - isz - 4;
    memcpy(&old, ip + at, 4);
    abs = (u64)(ip + hs->len) + (i64)old;
    nd = (i64)abs - (i64)(u64)(p + hs->len);
    if (nd > 2147483647LL || nd < -2147483647LL - 1) return NULL;
    nd32 = (i32)nd;
    memcpy(p + at, &nd32, 4);
    return p + hs->len;
}

/* Copy whole instructions until 12 bytes are covered. 0 if a copy would split an instruction,
 * run into the next function, or couldn't be moved. */
static int build_tramp(const u8 *src, u8 *out, u32 cap, u32 *stolen, u32 *out_len) {
    u32 n = 0;
    int closed = 0;
    u8 *p = out, *end = out + cap;
    while (n < PATCH_LEN) {
        hde64s hs;
        int fall;
        hde64_disasm(src + n, &hs);
        if ((hs.flags & F_ERROR) || hs.len == 0 || n + hs.len > 48) return 0;
        fall = falls_through(&hs);
        if (!fall && n + hs.len < PATCH_LEN) return 0;
        if (hs.flags & F_RELATIVE) {
            u64 to;
            if (!rel_target(src + n, &hs, &to)) return 0;
            if (hs.opcode == 0xE8)
                p = emit_abs_call(p, end, to);
            else if (hs.opcode == 0xE9 || hs.opcode == 0xEB)
                p = emit_abs_jmp(p, end, to);
            else if (hs.opcode == 0x0F && hs.opcode2 >= 0x80 && hs.opcode2 <= 0x8F)
                p = emit_jcc(p, end, (u8)(hs.opcode2 ^ 1), to);
            else if (hs.opcode >= 0x70 && hs.opcode <= 0x7F)
                p = emit_jcc(p, end, (u8)(((hs.opcode & 0x0F) ^ 1) | 0x80), to);
            else
                return 0;
        } else
            p = emit_copy(p, end, src + n, &hs);
        if (!p) return 0;
        n += hs.len;
        if (!fall) { closed = 1; break; }
    }
    if (n < PATCH_LEN) return 0;
    if (!closed) {
        p = emit_abs_jmp(p, end, (u64)(src + n));
        if (!p) return 0;
    }
    *stolen = n;
    *out_len = (u32)(p - out);
    return 1;
}

static u8 *take_cave(void *near_to, u32 n) {
    SYSTEM_INFO si;
    u64 step, dist;
    u8 *origin;
    n = (n + 15u) & ~15u;
    if (cave_base) {
        i64 d = (i64)((u8 *)near_to - cave_base);
        if (d < 0) d = -d;
        if (d < 0x70000000LL && cave_off + n <= 4096) {
            u8 *p = cave_base + cave_off;
            cave_off += n;
            return p;
        }
    }
    GetSystemInfo(&si);
    step = si.dwAllocationGranularity ? si.dwAllocationGranularity : 65536;
    origin = (u8 *)((u64)near_to & ~(step - 1));
    for (dist = 0; dist < 0x40000000; dist += step) {
        int s;
        for (s = 0; s < 2; s++) {
            u8 *want, *got;
            i64 d;
            if (dist == 0 && s == 1) continue;
            want = origin + (s ? -(i64)dist : (i64)dist);
            got = (u8 *)VirtualAlloc(want, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (!got) continue;
            d = (i64)((u8 *)near_to - got);
            if (d < 0) d = -d;
            if (d >= 0x70000000LL) { VirtualFree(got, 0, MEM_RELEASE); continue; }
            cave_base = got;
            cave_off = n;
            return got;
        }
    }
    return NULL;
}

/* `stub` is `jmp [rip+0]; dq hook` (14 bytes). `fn`'s first `stolen` bytes (at least 5) become
 * a jump to that stub, so a hook anywhere in memory is reached without a 12-byte patch. */
static void write_stub(u8 *stub, GuestFn *hook) {
    stub[0] = 0xFF;
    stub[1] = 0x25;
    stub[2] = stub[3] = stub[4] = stub[5] = 0;
    memcpy(stub + 6, &hook, 8);
    FlushInstructionCache(GetCurrentProcess(), stub, 14);
}

static int write_near(void *fn, u32 stolen, u8 *stub) {
    u8 patch[48];
    DWORD old = 0;
    i64 rel;
    if (stolen < PATCH_LEN || stolen > sizeof patch) return 0;
    rel = (i64)(stub - ((u8 *)fn + 5));
    if (rel > 2147483647LL || rel < -2147483647LL - 1) return 0;
    patch[0] = 0xE9;
    *(i32 *)(patch + 1) = (i32)rel;
    if (stolen > 5) memset(patch + 5, 0x90, stolen - 5);
    VirtualProtect(fn, stolen, PAGE_EXECUTE_READWRITE, &old);
    memcpy(fn, patch, stolen);
    VirtualProtect(fn, stolen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, stolen);
    return 1;
}

/* Make direct calls to `body` run `hook`. Returns what still reaches the function as it was
 * (the trampoline, or the previous hook when one was already patched in). NULL if the first
 * bytes can't be moved safely: the dispatch-table hook still stands, direct calls do not. */
/* A function shorter than 5 bytes (a bare ret) cannot hold a normal jump. The bytes after it,
 * up to the next function, are padding. A 2-byte jump lands in a stub written there. `room` is
 * how many bytes belong to this function before the next one. */
static int detour_tiny(u8 *body, GuestFn *hook, u32 room, GuestFn **tramp_out) {
    hde64s hs;
    u32 n = 0;
    u8 saved[16], *mem;
    DWORD old = 0;
    if (room < 16) return 0;
    while (n < 5 && n < room) {
        int fall;
        hde64_disasm(body + n, &hs);
        if ((hs.flags & F_ERROR) || hs.len == 0 || n + hs.len > room || n + hs.len > sizeof saved) return 0;
        if (hs.len >= 5 && n == 0) return 0;
        fall = falls_through(&hs);
        n += hs.len;
        if (!fall) break;
    }
    if (n == 0 || n >= 5 || (u32)2 + 14 > room) return 0;
    mem = take_cave(body, n);
    if (!mem) return 0;
    memcpy(saved, body, n);
    memcpy(mem, saved, n);
    FlushInstructionCache(GetCurrentProcess(), mem, n);
    VirtualProtect(body, 16, PAGE_EXECUTE_READWRITE, &old);
    write_stub(body + 2, hook);
    body[0] = 0xEB;
    body[1] = 0;
    VirtualProtect(body, 16, old, &old);
    FlushInstructionCache(GetCurrentProcess(), body, 16);
    *tramp_out = (GuestFn *)mem;
    return 1;
}

GuestFn *rt_detour_entry(GuestFn *body, GuestFn *hook, int allow_new, u32 room) {
    Detour *d;
    u32 stolen = 0, len = 0;
    u8 *mem, *stub;
    GuestFn *tiny_tramp = NULL;
    if (!body || !hook || body == hook) return NULL;
    for (d = detours; d; d = d->next) {
        if (d->current == body || d->body == body) {
            GuestFn *prev = d->current;
            write_stub(d->stub, hook);
            d->current = hook;
            return prev;
        }
    }
    if (!allow_new) return NULL;
    if (detour_tiny((u8 *)body, hook, room, &tiny_tramp)) {
        d = (Detour *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *d);
        if (d) {
            d->body = body;
            d->trampoline = tiny_tramp;
            d->stub = (u8 *)body + 2;
            d->current = hook;
            d->next = detours;
            detours = d;
        }
        return tiny_tramp;
    }
    /* Built in the cave, not a temporary buffer: a rip-relative instruction is corrected
     * for the address it actually runs at. */
    mem = take_cave(body, 320);
    stub = mem ? take_cave(body, 14) : NULL;
    if (!mem || !stub) return NULL;
    if (!build_tramp((const u8 *)body, mem, 320, &stolen, &len)) return NULL;
    FlushInstructionCache(GetCurrentProcess(), mem, len);
    write_stub(stub, hook);
    d = (Detour *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *d);
    if (!d) return NULL;
    d->body = body;
    d->trampoline = (GuestFn *)mem;
    d->stub = stub;
    d->current = hook;
    if (!write_near(body, stolen, stub)) { HeapFree(GetProcessHeap(), 0, d); return NULL; }
    d->next = detours;
    detours = d;
    return d->trampoline;
}

#ifdef DETOUR_TEST
static int hits;

static void add_hit(void) { hits += 1; }

/* Six pushes and a stack adjustment (12 bytes), then a relative call, then the matching pops. */
static u8 fn_bytes[40];
static void (*fn)(Cpu *);

static void hook_a(Cpu *c) { hits += 10; }
static GuestFn *orig_a;
static void hook_a_run(Cpu *c) { hits += 10; orig_a(c); }

static GuestFn *orig_b;
static void hook_b(Cpu *c) { hits += 100; orig_b(c); }

static int glob = 3;
__declspec(noinline) static void uses_glob(Cpu *c) { (void)c; hits += glob; }

static u8 tiny_bytes[32];
static GuestFn *tiny_fn;

static void build_fn(void) {
    u8 *p = fn_bytes;
    u64 target = (u64)(void *)add_hit;
    i32 rel;
    *p++ = 0x53; *p++ = 0x55; *p++ = 0x56; *p++ = 0x57;
    *p++ = 0x41; *p++ = 0x54; *p++ = 0x41; *p++ = 0x55;
    *p++ = 0x48; *p++ = 0x83; *p++ = 0xEC; *p++ = 0x28;
    *p++ = 0xE8;
    rel = (i32)(target - (u64)(p + 4));
    memcpy(p, &rel, 4);
    p += 4;
    *p++ = 0x48; *p++ = 0x83; *p++ = 0xC4; *p++ = 0x28;
    *p++ = 0x41; *p++ = 0x5D; *p++ = 0x41; *p++ = 0x5C;
    *p++ = 0x5F; *p++ = 0x5E; *p++ = 0x5D; *p++ = 0x5B;
    *p++ = 0xC3;
    fn = (void (*)(Cpu *))fn_bytes;
    DWORD old;
    VirtualProtect(fn_bytes, sizeof fn_bytes, PAGE_EXECUTE_READWRITE, &old);
}

int main(void) {
    GuestFn *back;
    build_fn();
    hits = 0;
    fn(NULL);
    if (hits != 1) { printf("plain %d\n", hits); return 1; }

    orig_a = rt_detour_entry(fn, hook_a_run, 1, 40);
    if (!orig_a) { printf("detour failed\n"); return 2; }
    hits = 0;
    fn(NULL);
    if (hits != 11) { printf("hooked %d\n", hits); return 3; }

    orig_b = rt_detour_entry(hook_a_run, hook_b, 0, 0);
    if (orig_b != hook_a_run) { printf("chain original is not the first hook\n"); return 4; }
    hits = 0;
    fn(NULL);
    if (hits != 111) { printf("chained %d\n", hits); return 5; }

    back = rt_detour_entry(uses_glob, hook_a, 1, 64);
    if (!back) { printf("rip detour failed\n"); return 6; }
    hits = 0;
    uses_glob(NULL);
    if (hits != 10) { printf("rip hooked %d\n", hits); return 7; }
    hits = 0;
    back(NULL);
    if (hits != 3) { printf("rip original %d\n", hits); return 8; }

    memset(tiny_bytes, 0xCC, sizeof tiny_bytes);
    tiny_bytes[0] = 0xC3;
    { DWORD oldp; VirtualProtect(tiny_bytes, sizeof tiny_bytes, PAGE_EXECUTE_READWRITE, &oldp); }
    tiny_fn = (GuestFn *)tiny_bytes;
    if (rt_detour_entry(tiny_fn, hook_a, 1, 4) != NULL) { printf("tiny patched without room\n"); return 9; }
    back = rt_detour_entry(tiny_fn, hook_a, 1, 32);
    if (!back) { printf("tiny detour failed\n"); return 10; }
    hits = 0;
    tiny_fn(NULL);
    if (hits != 10) { printf("tiny hooked %d\n", hits); return 11; }
    hits = 0;
    back(NULL);
    if (hits != 0) { printf("tiny original %d\n", hits); return 12; }
    printf("ok\n");
    return 0;
}
#endif
