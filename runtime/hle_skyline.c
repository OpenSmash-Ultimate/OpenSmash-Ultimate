/* Skyline plugins (docs/SKYLINE.md): the code GameBanana's moveset and new-character mods ship
 * (plugin.nro, Switch code built with skyline-rs), recompiled like the game's own NROs
 * (tools/recomp.py) and run here as Skyline runs them on the console.
 *
 *   loading  once the game mounts its RomFS (nn::fs::MountRom, as Skyline does), each module
 *            named in <data dir>/../skyline/plugins.txt is loaded (rt_load_nro_named) and its
 *            `main` run
 *   Skyline  the C API plugins import from Skyline itself: A64HookFunction (through the
 *            dispatch tables, rt_hook_function), getRegionAddress, sky_memcpy, the logger,
 *            get_program_id, get_plugin_addresses
 *   nn::ro   LookupSymbol answers from the runtime's symbols, which have the native plugins'
 *            names too: that is how plugins find Smashline
 *   Smashline 2, natively: the scripts plugins install (smashline_install_*), put into each
 *            fighter's agents as the game creates them (the create_agent functions, hooked)
 *
 * SSBU_SKYLINE_LOG=0 quiets the log of each call. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"

u64 rt_wrap_symbol(const char *name, GuestFn *fn);   /* (host.c) */
void fs_mount_sd(void);                              /* (hle_fs.c) */

#define MAIN_BASE 0x7100000000ull
#define PROGRAM_ID 0x01006A800016E000ull
#define R_NOT_FOUND RESULT(22, 1100)

static int quiet;
#define LOG(...) do { if (!quiet) { fprintf(stderr, "[skyline] " __VA_ARGS__); fputc('\n', stderr); } } while (0)

static const char *guest_str(u64 a) { return a ? P(const char, a) : "(null)"; }

/* ---- what the console's Skyline provides ------------------------------------------- */
/* (region: 0 text, 1 rodata, 2 data, 3 bss, 4 heap) - main's segments, from its MOD0 */
HLE(getRegionAddress) {
    u32 region = (u32)c->x[0] & 0xFF;
    u64 mod0 = MAIN_BASE + *P(u32, MAIN_BASE + 4);
    u64 addr = 0;
    if (region == 0) addr = MAIN_BASE;
    else if (region == 1) addr = MAIN_BASE + 0x39c9000;   /* (main 13.0.5's rodata and data: dump/exefs/main's segments) */
    else if (region == 2) addr = MAIN_BASE + 0x4f17000;
    else if (region == 3) addr = mod0 + *P(i32, mod0 + 8);
    else LOG("getRegionAddress(%u): not known here, 0", region);
    c->x[0] = addr;
}

/* Every hook target, "<module> <offset>" a line in <data dir>/../skyline/hooks.txt: the next build
 * makes the ones the game also calls directly reach their hooks too (tools/recomp.py hookable). */
static void note_hook_target(u64 target) {
    char where[96], line[128], path[1200];
    rt_describe(target, where, sizeof where);
    char *plus = strchr(where, '+');
    if (!plus || strncmp(plus, "+0x", 3)) return;
    *plus = 0;
    snprintf(line, sizeof line, "%s %s\n", where, plus + 3);
    snprintf(path, sizeof path, "%s/../skyline/hooks.txt", rt_data_dir);
    FILE *f = fopen(path, "r");
    char have[128];
    while (f && fgets(have, sizeof have, f))
        if (!strcmp(have, line)) { fclose(f); return; }
    if (f) fclose(f);
    if ((f = fopen(path, "a"))) { fputs(line, f); fclose(f); }
}

static void note_inline_point(u64 at) {
    char where[96], line[128], path[1200], have[128];
    rt_describe(at, where, sizeof where);
    char *plus = strchr(where, '+');
    if (!plus || strncmp(plus, "+0x", 3)) return;
    *plus = 0;
    snprintf(line, sizeof line, "%s %s inline\n", where, plus + 3);
    snprintf(path, sizeof path, "%s/../skyline/hooks.txt", rt_data_dir);
    FILE *f = fopen(path, "r");
    while (f && fgets(have, sizeof have, f))
        if (!strcmp(have, line)) { fclose(f); return; }
    if (f) fclose(f);
    if ((f = fopen(path, "a"))) { fputs(line, f); fclose(f); }
}

/* Plugins built for game 13.0.4 (a moveset's lib_paramconfig or Smashline code, say): main's code
 * moved by these amounts in 13.0.5 (address pairs of plugins released for both: Smashline's 13.0.5
 * commit, One Slot Effects' two builds, HewDraw Remix's 13.0.5 offsets; tools/ports/ has the CSK
 * Collection's, ported whole). A function hook that misses a function start moves by its region's
 * amount when that lands on one; nothing else is moved (an inline hook or a patch can't be checked). */
static const struct { u64 from, to; i64 by; } MOVED_IN_13_0_5[] = {
    {0x16b0000, 0x1792000, -0x1a0}, {0x1792000, 0x17d0000, -0xe0}, {0x17d0000, 0x17f1000, -0x40},
    {0x2310000, 0x2335000, 0x1bc}, {0x2335000, 0x2630000, 0x450}, {0x297d000, 0x2990000, 0x490},
    {0x3097000, 0x39c9000, 0x5b0},
};
int rt_is_function(u64 addr);   /* (host.c) */
u64 rt_moved_in_13_0_5(u64 at) {
    for (u32 i = 0; i < sizeof MOVED_IN_13_0_5 / sizeof *MOVED_IN_13_0_5; i++)
        if (at >= MAIN_BASE + MOVED_IN_13_0_5[i].from && at < MAIN_BASE + MOVED_IN_13_0_5[i].to &&
            !rt_is_function(at) && rt_is_function(at + MOVED_IN_13_0_5[i].by))
            return at + MOVED_IN_13_0_5[i].by;
    return at;
}

/* A64HookFunction(symbol, replace, result): calls to `symbol` go to `replace`; *result reaches
 * the function as it was. */
HLE(A64HookFunction) {
    u64 asked = c->x[0], target = rt_moved_in_13_0_5(asked), replace = c->x[1], result = c->x[2];
    char a[96], b[96], m[96];
    note_hook_target(target);
    u64 original = rt_hook_function(target, replace);
    if (target != asked) LOG("A64HookFunction at %s: a 13.0.4 address, moved", rt_describe(asked, m, sizeof m));
    LOG("A64HookFunction %s -> %s%s", rt_describe(target, a, sizeof a), rt_describe(replace, b, sizeof b),
        original ? "" : ": NOT a function entry, left alone");
    if (result) *P(u64, result) = original ? original : target;
}

/* A64InlineHook(address, callback): callback(InlineCtx *) runs before the instruction at address,
 * with the registers to read and change. Recompiled code has that call only where the build put it:
 * the address goes into hooks.txt as an inline point (tools/recomp.py inline_points), and the next
 * build calls rt_inline_hook there. */
typedef void InlineNative(Cpu *c, u64 arg);
static struct { u64 at, callback; InlineNative *native; u64 arg; } inline_hooks[512];
static u32 inline_n;
static void note_inline_point(u64 at);
static u64 cave_site(u64 at);

static void add_inline_hook(u64 at, u64 callback, InlineNative *native, u64 arg) {
    if (inline_n < sizeof inline_hooks / sizeof *inline_hooks) {
        inline_hooks[inline_n].at = at; inline_hooks[inline_n].callback = callback;
        inline_hooks[inline_n].native = native; inline_hooks[inline_n].arg = arg;
        __atomic_store_n(&inline_n, inline_n + 1, __ATOMIC_RELEASE);
    }
    note_inline_point(at);
}

HLE(A64InlineHook) {
    char a[96], b[96], s[96];
    u64 at = c->x[0], site = cave_site(at);
    LOG("A64InlineHook at %s -> %s%s%s", rt_describe(at, a, sizeof a), rt_describe(c->x[1], b, sizeof b),
        site ? ": a code cave, so at the call that branches to it, " : "", site ? rt_describe(site, s, sizeof s) : "");
    add_inline_hook(site ? site : at, c->x[1], NULL, 0);
}

/* (debugging) SSBU_DEBUG_AT=<module>+0x<offset>,...: the registers and 0x40 bytes of the stack at
 * those points, each also an inline point in hooks.txt ("<module> <offset> inline") so the build
 * stops there. */
static void debug_at(Cpu *c, u64 at) {
    static char points[16][96];
    static int count = -1;
    if (count < 0) {
        const char *e = getenv("SSBU_DEBUG_AT");
        count = 0;
        while (e && *e && count < 16) {
            size_t len = strcspn(e, ",");
            snprintf(points[count++], sizeof points[0], "%.*s", (int)len, e);
            e += len + (e[len] == ',');
        }
    }
    if (!count) return;
    char where[96];
    rt_describe(at, where, sizeof where);
    for (int i = 0; i < count; i++) {
        if (strcmp(where, points[i])) continue;
        fprintf(stderr, "[debug] %s:", where);
        for (int r = 0; r < 31; r++) fprintf(stderr, " x%d=%llx", r, (unsigned long long)c->x[r]);
        fprintf(stderr, " sp=%llx [sp]:", (unsigned long long)c->sp);
        for (int k = 0; k < 8; k++) fprintf(stderr, " %llx", (unsigned long long)*P(u64, c->sp + 8ull * k));
        fputc('\n', stderr);
    }
}

/* ---- when plugins hand the CSK Collection their characters, and when it reads them ---------- */
/* One line each, with the time and thread: a character added after the hook that builds the
 * select screen's list is missing from it (a friend's PC lost several; a Mac one now and then). */
static u64 csk_add_original;
static void csk_adding(void);
static int env_int(const char *name, int otherwise);
static void csk_record_added(Cpu *c);   /* (the entry's color_start_index: the match's costume) */
static void csk_add_traced(Cpu *c) {
    char who[96];
    static int delay = -1;
    if (delay < 0) delay = env_int("SSBU_CSK_ADD_DELAY", 0);
    if (delay) Sleep((DWORD)delay);
    csk_adding();
    fprintf(stderr, "[skyline] t=%.3f thread %lu: a character added to the CSK Collection by %s\n", rt_now_ns() / 1e9,
            (unsigned long)GetCurrentThreadId(), rt_describe(c->x[30] - 4, who, sizeof who));
    csk_record_added(c);   /* (the real function keeps the entry; this notes which costume the match must use) */
    rt_call(c, csk_add_original);
}
static void csk_trace_hook(u64 at, u64 callback) {   /* (rt_inline_hook: each CSK hook's first run) */
    static u64 seen[64];
    static int seen_n;
    for (int i = 0; i < seen_n; i++) if (seen[i] == at) return;
    if (seen_n < 64) seen[seen_n++] = at;
    char where[96], who[96];
    fprintf(stderr, "[skyline] t=%.3f thread %lu: first run of the CSK Collection's hook at %s (%s)\n", rt_now_ns() / 1e9,
            (unsigned long)GetCurrentThreadId(), rt_describe(at, where, sizeof where), rt_describe(callback, who, sizeof who));
}

/* ---- the game waits for ARCropolis' "mods mounted" callbacks ---------------------------- */
/* ARCropolis queues ModFilesystemMounted as it mounts mods: (its initial_loading hook), and a
 * thread of its own runs the plugins' callbacks for it a moment later (api/event.rs: a round of
 * callbacks, then a 20 ms sleep), the game not waiting. Plugins add their characters to the CSK
 * Collection there. On a Mac they are all in within half a second, some 2 s before the CSK
 * Collection's hooks read them; on a friend's slower PC several came after, and were left off the
 * select screen. So from the mount on (hle_fs.c), the CSK Collection's hooks wait for that round
 * to end: the thread that added the first character sleeping (3 s for one to come at all, 20 s
 * at most). */
static volatile int mods_gate;          /* 1: mounted, the callbacks' round not over; 2: over */
static volatile unsigned long csk_thread;   /* the thread adding characters (the callbacks') */
static u64 mount_ns;
void skyline_mods_mounted(void) {
    fprintf(stderr, "[skyline] t=%.3f thread %lu: ARCropolis mounted mods:\n", rt_now_ns() / 1e9, (unsigned long)GetCurrentThreadId());
    if (mods_gate) return;
    mount_ns = rt_now_ns();
    __atomic_store_n(&mods_gate, 1, __ATOMIC_RELEASE);
}
static void csk_adding(void) {   /* (csk_add_traced: a character, on the callbacks' thread) */
    if (__atomic_load_n(&mods_gate, __ATOMIC_ACQUIRE) == 1 && !csk_thread) csk_thread = GetCurrentThreadId();
}
void skyline_note_sleep(void) {   /* (kernel.c: SleepThread) - the callbacks' round over */
    if (csk_thread && csk_thread == GetCurrentThreadId() && __atomic_load_n(&mods_gate, __ATOMIC_ACQUIRE) == 1) {
        fprintf(stderr, "[skyline] t=%.3f: the plugins' mods-mounted callbacks are done\n", rt_now_ns() / 1e9);
        __atomic_store_n(&mods_gate, 2, __ATOMIC_RELEASE);
    }
}
/* (for testing: SSBU_CSK_ADD_DELAY=<ms> makes each character that much slower to add - a slow
 * PC - and SSBU_MODS_GATE=0 leaves the wait out) */
static int env_int(const char *name, int otherwise) { const char *v = getenv(name); return v && *v ? atoi(v) : otherwise; }
void skyline_mods_gate(const char *what) {
    static int on = -1;
    if (on < 0) on = env_int("SSBU_MODS_GATE", 1);
    if (!on || __atomic_load_n(&mods_gate, __ATOMIC_ACQUIRE) != 1 || GetCurrentThreadId() == csk_thread) return;
    u64 start = rt_now_ns();
    while (__atomic_load_n(&mods_gate, __ATOMIC_ACQUIRE) == 1) {
        u64 now = rt_now_ns();
        if (!csk_thread && now - mount_ns > 3000000000ull) break;   /* (no plugin adds characters) */
        if (now - start > 20000000000ull) break;
        Sleep(2);
    }
    int done = __atomic_load_n(&mods_gate, __ATOMIC_ACQUIRE) == 2;
    if (__atomic_exchange_n(&mods_gate, 2, __ATOMIC_ACQ_REL) == 1 || done)
        LOG("%s waited %.0f ms for the plugins' mods-mounted callbacks%s", what, (rt_now_ns() - start) / 1e6,
            done ? "" : csk_thread ? " (gave up after 20 s)" : " (none added characters)");
}

/* (generated code, at an inline point) skyline-rs' InlineCtx: x0-x30, sp, then q0-q31. */
void rt_inline_hook(Cpu *c, u64 at) {
    debug_at(c, at);
    u32 n = __atomic_load_n(&inline_n, __ATOMIC_ACQUIRE);
    for (u32 i = 0; i < n; i++) {
        if (inline_hooks[i].at != at) continue;
        if (inline_hooks[i].native) { inline_hooks[i].native(c, inline_hooks[i].arg); continue; }
        {
            char who[96];
            if (!strncmp(rt_describe(inline_hooks[i].callback, who, sizeof who), "the_csk_collection+", 19)) {
                csk_trace_hook(at, inline_hooks[i].callback);
                if (__atomic_load_n(&mods_gate, __ATOMIC_ACQUIRE) == 1) skyline_mods_gate("a CSK Collection hook");
            }
        }
        Cpu saved = *c;
        u64 ctx = (c->sp - 0x400) & ~15ull;
        for (int r = 0; r < 31; r++) *P(u64, ctx + 8ull * r) = c->x[r];
        *P(u64, ctx + 0xF8) = c->sp;
        memcpy(P(void, ctx + 0x100), c->v, 0x200);
        c->sp = ctx - 0x100;
        c->x[0] = ctx;
        rt_call(c, inline_hooks[i].callback);
        *c = saved;
        for (int r = 0; r < 31; r++) c->x[r] = *P(u64, ctx + 8ull * r);
        memcpy(c->v, P(void, ctx + 0x100), 0x200);
    }
}

/* sky_memcpy(dest, src, n): plugins patch code and read-only data with it. Data changes work at
 * once. A change to code means nothing to the recompiled function until the next build lifts the
 * new instructions: each changed word goes into <data dir>/../skyline/patches.txt, "<module>
 * <offset> <old word> <new word>" (tools/recomp.py code_patches, tools/skyline.py stale_hooks). */
const char *rt_code_offset(u64 addr, u64 *off);   /* (host.c) */
/* Smashline's inline hooks in lua2cpp_common: it writes a `bl` at the hooked call to a code cave
 * (the original branch, as a `b`, after some nops) and inline-hooks the cave. Recompiled code runs
 * none of that, so the hook goes before the hooked call itself (the cave's address -> the call's);
 * tools/recomp.py leaves those patches out (code_patches). */
static struct { u64 cave, site; } caves[256];
static u32 cave_n;
static void note_cave(u64 at, u32 word) {
    if ((word & 0xFC000000) != 0x94000000 || cave_n == 256) return;   /* bl */
    i64 imm = (i64)(word & 0x3FFFFFF) << 38 >> 36;
    caves[cave_n].cave = at + (u64)imm; caves[cave_n++].site = at;
}
static u64 cave_site(u64 at) {
    for (u32 i = 0; i < cave_n; i++) if (caves[i].cave == at) return caves[i].site;
    return 0;
}

static void note_code_patch(u64 dest, u64 src, u64 n) {
    char path[1200], line[160], have[160];
    snprintf(path, sizeof path, "%s/../skyline/patches.txt", rt_data_dir);
    for (u64 w = dest & ~3ull; w < dest + n; w += 4) {
        u64 off;
        const char *mod = rt_code_offset(w, &off);
        if (!mod) continue;
        u32 old = *P(u32, w), now = old;
        for (int b = 0; b < 4; b++)
            if (w + b >= dest && w + b < dest + n) now = (now & ~(0xFFu << 8 * b)) | (u32)*P(u8, src + (w + b - dest)) << 8 * b;
        if (now == old) continue;
        note_cave(w, now);
        snprintf(line, sizeof line, "%s %llx %08x %08x\n", mod, (unsigned long long)off, old, now);
        FILE *f = fopen(path, "r");
        int seen = 0;
        while (f && !seen && fgets(have, sizeof have, f)) seen = !strcmp(have, line);
        if (f) fclose(f);
        if (!seen && (f = fopen(path, "a"))) { fputs(line, f); fclose(f); }
    }
}
HLE(sky_memcpy) {
    char a[96];
    u64 to = c->x[0];
    LOG("sky_memcpy %#llx bytes to %s", (unsigned long long)c->x[2], rt_describe(to, a, sizeof a));
    note_code_patch(to, c->x[1], c->x[2]);
    memcpy(G2H(to), G2H(c->x[1]), c->x[2]);
    c->x[0] = 0;
}

HLE(skyline_tcp_send_raw) {   /* (data, size): the plugin's println! */
    fwrite(G2H(c->x[0]), 1, c->x[1], stderr);
}

HLE(get_program_id) { c->x[0] = PROGRAM_ID; }

/* nn::oe::GetDisplayVersion(DisplayVersion *): the game's version as the console shows it (char[16]).
 * The game doesn't ask; plugins check it (ARCropolis). The SDK's own asks a system service that
 * isn't here, so its dispatch entry gets this (skyline_load_plugins). */
static void display_version(Cpu *c) {
    memset(G2H(c->x[0]), 0, 16);
    memcpy(G2H(c->x[0]), "13.0.5", 6);
}

/* The SDK functions plugins call that the game never does, whose SDK code would ask system services
 * that aren't here: their dispatch entries get these when plugins load (plugin_shims). */
static void show_application_error(Cpu *c) {   /* (ApplicationErrorArg: the dialog's text at 0x10, the full screen's at 0x810) */
    u64 arg = c->x[0];   /* (on the console the player closes it and the game goes on: a panic aborts by itself after) */
    fprintf(stderr, "[skyline] a plugin showed an error: %.1000s\n  %.1500s\n", P(char, arg + 0x10), P(char, arg + 0x810));
    fflush(stderr);
}
static void exit_application(Cpu *c) { (void)c; fprintf(stderr, "[skyline] a plugin exited the game\n"); fflush(stderr); ExitProcess(0); }
static void nothing(Cpu *c) { c->x[0] = 0; }

/* nn::crypto::GenerateSha256Hash(out, out size, data, size): the SDK's uses the CPU's SHA-256
 * instructions, which the recompiler doesn't translate (ARCropolis hashes the plugins it loads). */
static void sha256(const u8 *data, u64 n, u8 out[32]) {
    static const u32 K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
        0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
        0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    u32 h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
#define ROR(x, r) ((x) >> (r) | (x) << (32 - (r)))
    u64 blocks = (n + 9 + 63) / 64;
    for (u64 b = 0; b < blocks; b++) {
        u8 block[64];
        for (int i = 0; i < 64; i++) {
            u64 at = b * 64 + (u64)i;
            u64 len_at = blocks * 64 - 8;
            block[i] = at < n ? data[at] : at == n ? 0x80 : at >= len_at ? (u8)((n * 8) >> (8 * (7 - (at - len_at)))) : 0;
        }
        u32 w[64], a = h[0], bb = h[1], cc = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 16; i++) w[i] = (u32)block[4 * i] << 24 | (u32)block[4 * i + 1] << 16 | (u32)block[4 * i + 2] << 8 | block[4 * i + 3];
        for (int i = 16; i < 64; i++) {
            u32 s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        for (int i = 0; i < 64; i++) {
            u32 t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            u32 t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & bb) ^ (a & cc) ^ (bb & cc));
            hh = g; g = f; f = e; e = d + t1; d = cc; cc = bb; bb = a; a = t1 + t2;
        }
        h[0] += a; h[1] += bb; h[2] += cc; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
#undef ROR
    for (int i = 0; i < 32; i++) out[i] = (u8)(h[i / 4] >> (24 - 8 * (i % 4)));
}
static void sha256_hash(Cpu *c) {   /* (out, out size, data, size) */
    u8 digest[32];
    sha256(P(u8, c->x[2]), c->x[3], digest);
    memcpy(G2H(c->x[0]), digest, c->x[1] < 32 ? c->x[1] : 32);
}
static void unavailable(Cpu *c) { c->x[0] = RESULT(123, 1); }   /* (nn::ssl: no SSL here - update checks give up) */
static void plugin_shims(void) {
    static const struct { const char *name; GuestFn *fn; } SHIMS[] = {
        {"_ZN2nn2oe17GetDisplayVersionEPNS0_14DisplayVersionE", display_version},
        {"_ZN2nn3err20ShowApplicationErrorERKNS0_19ApplicationErrorArgE", show_application_error},
        {"_ZN2nn2oe15ExitApplicationEv", exit_application},
        {"_ZN2nn3hid6detail22SetSupportedNpadIdTypeEPKji", nothing},
        {"_ZN2nn3ssl10InitializeEv", unavailable},
        {"_ZN2nn6crypto18GenerateSha256HashEPvmPKvm", sha256_hash},
    };
    for (u32 i = 0; i < sizeof SHIMS / sizeof *SHIMS; i++) {
        u64 at = rt_symbol(SHIMS[i].name);
        if (at) rt_hook_function(at, rt_native(SHIMS[i].fn, SHIMS[i].name));
    }
    void fs_plugin_shims(void), rtld_plugin_shims(void);   /* (hle_fs.c, hle_rtld.c) */
    fs_plugin_shims();
    rtld_plugin_shims();
}

/* get_plugin_addresses(addr, *start, *end): the plugin's module around an address */
HLE(get_plugin_addresses) {
    char a[96];
    LOG("get_plugin_addresses(%s): not known here", rt_describe(c->x[0], a, sizeof a));
    if (c->x[1]) *P(u64, c->x[1]) = 0;
    if (c->x[2]) *P(u64, c->x[2]) = 0;
}

/* nn::ro::LookupSymbol(*address, name) */
static void lookup_symbol(Cpu *c) {
    const char *name = guest_str(c->x[1]);
    u64 addr = rt_resolve_weak(name);
    LOG("LookupSymbol %s: %#llx", name, (unsigned long long)addr);
    if (!addr) { c->x[0] = R_NOT_FOUND; return; }
    *P(u64, c->x[0]) = addr;
    c->x[0] = 0;
}

/* Plugins check that the plugins they need are installed (rom:/skyline/plugins/<lib>.nro); the
 * ones the runtime provides natively are. (hle_fs.c asks for a RomFS path with no "rom:/".) */
int skyline_provides(const char *path) {
    static const char *const natives[] = {"skyline/plugins/libsmashline_plugin.nro", "skyline/plugins/libnro_hook.nro",
                                          "skyline/plugins/libarcropolis.nro",
                                          /* (linked into each plugin that uses it) */
                                          "skyline/plugins/libparam_config.nro",
                                          /* (loaded from plugins.txt; Vegeta and Gogeta stop unless this path exists) */
                                          "skyline/plugins/libone_slot_eff.nro",
                                          /* (not yet: plugins check for it, Sonic Re-Imagined without using it) */
                                          "skyline/plugins/libthe_csk_collection.nro", 0};
    while (*path == '/') path++;
    for (int i = 0; natives[i]; i++)
        if (!strcmp(path, natives[i])) return 1;
    return 0;
}

/* ---- Smashline 2 ------------------------------------------------------------------- */
/* Hash40: the CRC-32 of the name, its length in bits 32-39 */
static u64 hash40(const char *s, size_t n) {
    u32 crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= (u8)s[i];
        for (int k = 0; k < 8; k++) crc = crc >> 1 ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return (u64)~crc | (u64)(n & 0xFF) << 32;
}

/* A Smashline costume list ({const usize *data; usize len}), copied: the slots a script is for. */
typedef struct { u32 n; u32 *slots; } Costume;
typedef struct { u64 agent, script, function; int category; Costume costume; } AcmdScript;
typedef struct { u64 agent, function; int status, line; Costume costume; } StatusScript;
typedef struct { u64 agent, function; int line; u32 ran; Costume costume; } LineCallback;
static AcmdScript *acmd;
static StatusScript *statuses;
static LineCallback *lines;
static u32 acmd_n, status_n, line_n, acmd_cap, status_cap, line_cap;
static SRWLOCK scripts_lock = SRWLOCK_INIT;
static const char *const CATEGORY[] = {"game", "effect", "sound", "expression"};
enum { LINE_MAIN = 1, SET_STATUS_SCRIPTS = 9 };   /* (smashline StatusLine; L2CFighterBase's virtual function) */
static u64 FIGHTER_HASH, WEAPON_HASH;             /* Hash40("fighter"), ("weapon"): scripts for every one */

#define PUSH(arr, n, cap, ...) do { \
    AcquireSRWLockExclusive(&scripts_lock); \
    if (n == cap) arr = realloc(arr, sizeof *arr * (cap = cap * 2 + 64)); \
    arr[n++] = (__VA_ARGS__); \
    ReleaseSRWLockExclusive(&scripts_lock); \
} while (0)

static Costume costume_of(u64 data, u64 len) {
    Costume k = {0, NULL};
    if (!data || !len || len > 4096) return k;
    k.slots = malloc(sizeof *k.slots * len);
    for (u64 i = 0; i < len; i++) k.slots[i] = (u32)*P(u64, data + 8 * i);
    k.n = (u32)len;
    return k;
}
static int has_slot(Costume k, int slot) {
    for (u32 i = 0; i < k.n; i++) if ((int)k.slots[i] == slot) return 1;
    return 0;
}
/* Smashline's rule: a slot some plugin claimed for this agent (a "new character" on extra
 * costumes) gets only the scripts for that slot; any other slot gets the scripts with no list. */
static int claimed(u64 agent, int slot) {
    for (u32 i = 0; i < acmd_n; i++) if (acmd[i].agent == agent && has_slot(acmd[i].costume, slot)) return 1;
    for (u32 i = 0; i < status_n; i++) if (statuses[i].agent == agent && has_slot(statuses[i].costume, slot)) return 1;
    for (u32 i = 0; i < line_n; i++) if (lines[i].agent == agent && has_slot(lines[i].costume, slot)) return 1;
    return 0;
}
static int for_slot(Costume k, int is_claimed, int slot) { return is_claimed ? has_slot(k, slot) : !k.n; }

static const char *slots_text(Costume k) {
    static __declspec(thread) char text[48];
    if (!k.n) return "";
    if (k.n == 1) snprintf(text, sizeof text, " (costume c%02u)", k.slots[0]);
    else snprintf(text, sizeof text, " (%u costumes, c%02u...c%02u)", k.n, k.slots[0], k.slots[k.n - 1]);
    return text;
}

static void add_acmd(u64 agent, Costume k, u64 script, int category, u64 function) {
    PUSH(acmd, acmd_n, acmd_cap, (AcmdScript){agent, script, function, category, k});
    LOG("acmd %#llx %s script %#llx%s -> %#llx", (unsigned long long)agent, CATEGORY[category & 3], (unsigned long long)script,
        slots_text(k), (unsigned long long)function);
}
static void add_status(u64 agent, Costume k, int status, int line, u64 function) {
    if (!agent) agent = hash40("common", 6);
    PUSH(statuses, status_n, status_cap, (StatusScript){agent, function, status, line, k});
    LOG("status %#llx kind %d line %d%s -> %#llx", (unsigned long long)agent, status, line, slots_text(k), (unsigned long long)function);
}
static void add_line(u64 agent, Costume k, int line, u64 function) {
    PUSH(lines, line_n, line_cap, (LineCallback){agent, function, line, 0, k});
    LOG("line callback %#llx line %d%s -> %#llx%s", (unsigned long long)agent, line, slots_text(k), (unsigned long long)function,
        line == LINE_MAIN ? "" : " (only the Main line's are run yet)");
}

/* (agent, script, category, priority, function) and (agent, costume, script, category, priority, function) */
HLE(smashline_install_acmd_script) { add_acmd(c->x[0], (Costume){0}, c->x[1], (int)c->x[2], c->x[4]); }
HLE(smashline_install_acmd_script_costume) { add_acmd(c->x[0], costume_of(c->x[1], c->x[2]), c->x[3], (int)c->x[4], c->x[6]); }
/* (agent or 0: common, status, line, function) */
HLE(smashline_install_status_script) { add_status(c->x[0], (Costume){0}, (int)c->x[1], (int)c->x[2], c->x[3]); }
HLE(smashline_install_status_script_costume) { add_status(c->x[0], costume_of(c->x[1], c->x[2]), (int)c->x[3], (int)c->x[4], c->x[5]); }
/* (agent or 0, line, function) */
HLE(smashline_install_line_callback) { add_line(c->x[0], (Costume){0}, (int)c->x[1], c->x[2]); }
HLE(smashline_install_line_callback_costume) { add_line(c->x[0], costume_of(c->x[1], c->x[2]), (int)c->x[3], c->x[4]); }
/* (agent or 0, event, function): object events need hooks inside main's object code - not yet */
HLE(smashline_install_state_callback) {
    LOG("state callback %#llx event %d -> %#llx: NOT SUPPORTED yet, left out", (unsigned long long)c->x[0], (int)c->x[1],
        (unsigned long long)c->x[2]);
}
HLE(smashline_install_state_callback_costume) {
    LOG("state callback %#llx event %d (costumes) -> %#llx: NOT SUPPORTED yet, left out", (unsigned long long)c->x[0], (int)c->x[3],
        (unsigned long long)c->x[4]);
}

/* What the game had before a plugin's script went in: (agent, script hash or status/line) ->
 * function, for smashline_get_original_acmd and _status. Agents come and go at the same
 * addresses: an entry is overwritten when its agent is made again. */
#define ORIGINALS 65536
static struct { u64 agent, key, function; } originals[ORIGINALS];
static SRWLOCK originals_lock = SRWLOCK_INIT;
static u32 original_slot(u64 agent, u64 key) { return (u32)((agent * 0x9E3779B97F4A7C15ull ^ key * 0xC2B2AE3D27D4EB4Full) >> 48); }
static void remember(u64 agent, u64 key, u64 function) {
    AcquireSRWLockExclusive(&originals_lock);
    u32 i = original_slot(agent, key);
    for (u32 k = 0; k < 64; k++, i = (i + 1) & (ORIGINALS - 1))
        if (!originals[i].agent || (originals[i].agent == agent && originals[i].key == key)) {
            originals[i].agent = agent; originals[i].key = key; originals[i].function = function;
            break;
        }
    ReleaseSRWLockExclusive(&originals_lock);
}
static u64 recall(u64 agent, u64 key) {
    u64 f = 0;
    AcquireSRWLockShared(&originals_lock);
    u32 i = original_slot(agent, key);
    for (u32 k = 0; k < 64 && originals[i].agent; k++, i = (i + 1) & (ORIGINALS - 1))
        if (originals[i].agent == agent && originals[i].key == key) { f = originals[i].function; break; }
    ReleaseSRWLockShared(&originals_lock);
    return f;
}
#define STATUS_KEY(line, status) ((u64)(u32)(line) << 32 | (u32)(status) | 1ull << 63)
HLE(smashline_get_original_acmd) { c->x[0] = recall(c->x[0], c->x[1]); }                       /* (agent, hash) */
HLE(smashline_get_original_status) { c->x[0] = recall(c->x[0], STATUS_KEY(c->x[1], c->x[2])); }   /* (agent, line, kind) */
/* (module name {ptr, len}, offset): an address in another module's code */
HLE(smashline_get_target_function) {
    char name[96] = "";
    u64 len = c->x[1] < sizeof name ? c->x[1] : sizeof name - 1;
    if (c->x[0]) memcpy(name, P(char, c->x[0]), len);
    name[len] = 0;
    u64 base = strcmp(name, "main") ? rt_load_nro_named(c, name) : MAIN_BASE;
    LOG("get_target_function %s+%#llx: %s", name, (unsigned long long)c->x[2], base ? "found" : "NOT loaded");
    c->x[0] = base ? base + c->x[2] : 0;
}

/* ---- which agent: name, costume slot ---- */
#define FIGHTER_NAMES (MAIN_BASE + 0x4f81e20)       /* const char *[118]: fighter names, by kind */
#define FIGHTER_COUNT 118
#define WEAPON_NAMES (MAIN_BASE + 0x5186bd0)        /* const char *[0x267]: weapon names, by kind */
#define WEAPON_OWNER_NAMES (MAIN_BASE + 0x5189240)  /* ... and their owners' */
#define WEAPON_COUNT 0x267
#define ENTRY_TABLE (MAIN_BASE + 0x5324680)         /* (smashline utils.rs: an entry's costume) */

static const char *guest_name(u64 table, int i, int count) {
    if (i < 0 || i >= count) return NULL;
    u64 ptr = *P(u64, table + 8ull * i);
    return ptr >= MAIN_BASE && ptr < MAIN_BASE + 0x8000000 ? P(const char, ptr) : NULL;
}

/* A fighter's kind by its name ("palutena"): -1 if none (the bench, hle_bench.c). */
int skyline_fighter_kind(const char *name) {
    for (int k = 0; k < FIGHTER_COUNT; k++) {
        const char *n = guest_name(FIGHTER_NAMES, k, FIGHTER_COUNT);
        if (n && !strcmp(n, name)) return k;
    }
    return -1;
}

typedef struct { u64 hash; int slot, weapon; char name[64]; } Agent;

/* The agent a battle object is: a fighter ("sonic") or a weapon ("sonic_gimmickjump"), and the
 * costume slot of the player it belongs to (-1: none). 0 hash: neither. */
static Agent agent_of(u64 object, int kind) {
    Agent a = {0, -1, 0, ""};
    if (!object) return a;
    u32 category = *P(u32, object + 8) >> 28;
    if (kind < 0) kind = *P(i32, object + 0xC);
    if (category == 0) {
        const char *n = guest_name(FIGHTER_NAMES, kind, FIGHTER_COUNT);
        if (n) snprintf(a.name, sizeof a.name, "%s", n);
    } else if (category == 1) {
        const char *o = guest_name(WEAPON_OWNER_NAMES, kind, WEAPON_COUNT), *n = guest_name(WEAPON_NAMES, kind, WEAPON_COUNT);
        if (o && n) snprintf(a.name, sizeof a.name, "%s_%s", o, n);
        a.weapon = 1;
    }
    if (!*a.name) return a;
    a.hash = hash40(a.name, strlen(a.name));
    int entry = *P(i32, object + 0x10);
    u64 vec = *P(u64, ENTRY_TABLE);
    if (entry >= 0 && entry <= 7 && vec) {
        u64 info = *P(u64, vec + 8ull * entry + 0xE8);
        if (info) a.slot = *P(i32, info + 100);
    }
    return a;
}

/* ---- ACMD: the agent factories ---- */
/* The game's agent factories (main, 13.0.5; smashline's create_agent.rs): (object, module
 * accessor, lua state) -> agent, one per script category; the "share" ones, for fighters that
 * share another's code, take the fighter kind first. */
static const struct { u64 offset; int category, share; } FACTORIES[] = {
    {0x64c310, 0, 0}, {0x64c930, 1, 0}, {0x64cf50, 3, 0}, {0x64d570, 2, 0},
    {0x64db90, 0, 1}, {0x64e2a0, 1, 1}, {0x64e8b0, 3, 1}, {0x64eec0, 2, 1},
    {0x33acde0 + 0x5B0, 0, 0}, {0x33add40 + 0x5B0, 1, 0}, {0x33aeca0 + 0x5B0, 2, 0},   /* weapons */
};
#define NFACTORIES (sizeof FACTORIES / sizeof *FACTORIES)
static u64 factory_original[NFACTORIES];

/* lib::L2CAgent's function_map (a libc++ unordered_map<Hash40, fn> at +0x10): each node
 * {next, hash, key, function}, the list from +0x20 */
static u64 agent_function(u64 agent, u64 script) {
    u64 node = *P(u64, agent + 0x20);
    for (int i = 0; node && i < 100000; i++, node = *P(u64, node))
        if (*P(u64, node + 16) == script) return *P(u64, node + 24);
    return 0;
}

static void created(Cpu *c, int which) {
    int category = FACTORIES[which].category, share = FACTORIES[which].share;
    u64 object = c->x[share ? 1 : 0];
    int kind = share ? (int)c->x[0] : -1;
    rt_call(c, factory_original[which]);
    u64 agent = c->x[0];
    Agent a = agent && object ? agent_of(object, kind) : (Agent){0};
    if (!a.hash) return;
    u32 put = 0;
    AcquireSRWLockShared(&scripts_lock);
    u32 n = acmd_n;
    int is_claimed = claimed(a.hash, a.slot);
    ReleaseSRWLockShared(&scripts_lock);
    for (u32 i = 0; i < n; i++)
        if (acmd[i].agent == a.hash && acmd[i].category == category && for_slot(acmd[i].costume, is_claimed, a.slot)) {
            remember(agent, acmd[i].script, agent_function(agent, acmd[i].script));
            rt_callsym(c, "_ZN3lib8L2CAgent20sv_set_function_hashEPvN3phx6Hash40E", 3, agent, acmd[i].function, acmd[i].script);
            put++;
        }
    if (put) LOG("%s (c%02d) %s agent %#llx: %u acmd scripts from plugins", a.name, a.slot, CATEGORY[category], (unsigned long long)agent, put);
    c->x[0] = agent;
}
#define FACTORY(k) static void factory_##k(Cpu *c) { created(c, k); }
FACTORY(0) FACTORY(1) FACTORY(2) FACTORY(3) FACTORY(4) FACTORY(5) FACTORY(6) FACTORY(7) FACTORY(8) FACTORY(9) FACTORY(10)
static GuestFn *const factory_hooks[NFACTORIES] = {factory_0, factory_1, factory_2, factory_3, factory_4, factory_5,
                                                   factory_6, factory_7, factory_8, factory_9, factory_10};

/* ---- weapons plugins clone (smashline_clone_weapon): a fighter gets another fighter's article ---- */
/* The game asks for a fighter's static data (main+0x64b730: kind -> StaticFighterData, its article
 * table at +8: {ArticleDescriptor *descriptors (32 bytes each, weapon id first); count}); the
 * fighters with new articles get a copy with them appended, max counts changed as plugins say
 * (smashline cloning/weapons.rs). */
#define STATIC_FIGHTER_DATA 0x64b730
static u64 static_data_original;
static struct { int owner, original_owner, weapon; } new_articles[128];
static u32 new_article_n;
static struct { int weapon, count; } weapon_counts[128];
static u32 weapon_count_n;
static u64 static_cache[FIGHTER_COUNT];
static struct { int weapon, old_owner, owner; u64 owner_name, new_name; } new_agents[128];
static u32 new_agent_n;
static volatile int current_owner = -1;   /* (the fighter being set up: main+0x6079d0) */
static SRWLOCK weapons_lock = SRWLOCK_INIT;

static int fighter_index(u64 str, u64 len) {
    char name[64];
    if (!str || len >= sizeof name) return -1;
    memcpy(name, P(char, str), len);
    name[len] = 0;
    for (int k = 0; k < FIGHTER_COUNT; k++) {
        const char *n = guest_name(FIGHTER_NAMES, k, FIGHTER_COUNT);
        if (n && !strcmp(n, name)) return k;
    }
    LOG("smashline: no fighter called %s", name);
    return -1;
}

/* (original owner {ptr, len}, original article id, new owner {ptr, len}, new name {ptr, len}, use original code) -> id */
HLE(smashline_clone_weapon) {
    int from = fighter_index(c->x[0], c->x[1]), weapon = (int)c->x[2], to = fighter_index(c->x[3], c->x[4]);
    char name[64] = "";
    if (c->x[5] && c->x[6] < sizeof name) { memcpy(name, P(char, c->x[5]), c->x[6]); name[c->x[6]] = 0; }
    AcquireSRWLockExclusive(&weapons_lock);
    int id = 0;
    for (u32 i = 0; i < new_article_n; i++)
        if (new_articles[i].owner == to) {
            if (new_articles[i].original_owner == from && new_articles[i].weapon == weapon) { ReleaseSRWLockExclusive(&weapons_lock); c->x[0] = (u64)id; return; }
            id++;
        }
    if (from >= 0 && to >= 0 && new_article_n < sizeof new_articles / sizeof *new_articles) {
        new_articles[new_article_n].owner = to; new_articles[new_article_n].original_owner = from;
        new_articles[new_article_n++].weapon = weapon;
        memset(static_cache, 0, sizeof static_cache);
        if (new_agent_n < sizeof new_agents / sizeof *new_agents) {   /* (the names its data goes by: the new owner's) */
            char owner[64] = "";
            const char *n = guest_name(FIGHTER_NAMES, to, FIGHTER_COUNT);
            if (n) snprintf(owner, sizeof owner, "%s", n);
            new_agents[new_agent_n].weapon = weapon; new_agents[new_agent_n].old_owner = from; new_agents[new_agent_n].owner = to;
            new_agents[new_agent_n].owner_name = rt_guest_copy(owner, strlen(owner) + 1);
            new_agents[new_agent_n].new_name = rt_guest_copy(name, strlen(name) + 1);
            new_agent_n++;
        }
    }
    ReleaseSRWLockExclusive(&weapons_lock);
    LOG("smashline: weapon %d of fighter %d cloned to fighter %d as %s (article %d)", weapon, from, to, name, id);
    c->x[0] = (u64)id;
}
HLE(smashline_update_weapon_count) {   /* (article id, new count) */
    AcquireSRWLockExclusive(&weapons_lock);
    if (weapon_count_n < sizeof weapon_counts / sizeof *weapon_counts) {
        weapon_counts[weapon_count_n].weapon = (int)c->x[0];
        weapon_counts[weapon_count_n++].count = (int)c->x[1];
        memset(static_cache, 0, sizeof static_cache);
    }
    ReleaseSRWLockExclusive(&weapons_lock);
}
HLE(smashline_whitelist_kirby_copy_article) {
    LOG("smashline: Kirby copy article %d of fighter %d: not supported yet (Kirby's copy keeps its own)", (int)c->x[1], (int)c->x[0]);
}
HLE(smashline_add_param_object) {   /* (fighter {ptr, len}, name {ptr, len}) */
    LOG("smashline: param object for fighter %d: not supported yet, left out", fighter_index(c->x[0], c->x[1]));
}
HLE(smashline_get_action_registry) { c->x[0] = 0; LOG("smashline: action registry (script files): not supported, none"); }
HLE(smashline_reload_script) { (void)c; }
HLE(smashline_remove_by_plugin_range) { (void)c; }

/* While a fighter is set up (main+0x6079d0: (object, id, kind, entry, hash)), the game looks up its
 * articles' data by owner and name: for a cloned one, the new owner's and name (inline hooks at
 * smashline's places: the value a register is about to be used as, replaced). */
#define FIGHTER_INIT 0x6079d0
static u64 fighter_init_original;
static void fighter_init(Cpu *c) {
    int was = current_owner;
    current_owner = (int)c->x[2];
    rt_call(c, fighter_init_original);
    current_owner = was;
}
enum { SWAP_OWNER, SWAP_OWNER_NAME, SWAP_NAME };
static void swap_weapon(Cpu *c, u64 arg) {   /* (arg: kind << 16 | source register << 8 | destination register) */
    int kind = (int)(arg >> 16), src = (int)(arg >> 8 & 0xFF), dst = (int)(arg & 0xFF), owner = current_owner;
    if (owner < 0) return;
    for (u32 i = 0; i < new_agent_n; i++)
        if (new_agents[i].weapon == (int)c->x[src] && new_agents[i].owner == owner) {
            c->x[dst] = kind == SWAP_OWNER ? (u64)new_agents[i].owner : kind == SWAP_OWNER_NAME ? new_agents[i].owner_name : new_agents[i].new_name;
            return;
        }
}
static const struct { u64 offset; int kind, src, dst; } WEAPON_SWAPS[] = {
    {0x33b6628 + 0x5B0, SWAP_OWNER, 21, 26}, {0x33acf78 + 0x5B0, SWAP_OWNER, 22, 8}, {0x33aee38 + 0x5B0, SWAP_OWNER, 22, 8},
    {0x33aded8 + 0x5B0, SWAP_OWNER, 22, 8}, {0x33ac040 + 0x5B0, SWAP_OWNER, 22, 8},
    {0x17e0a4c - 0x40, SWAP_OWNER_NAME, 26, 25}, {0x33ace7c + 0x5B0, SWAP_OWNER_NAME, 8, 2}, {0x33aed3c + 0x5B0, SWAP_OWNER_NAME, 8, 2},
    {0x33adddc + 0x5B0, SWAP_OWNER_NAME, 8, 2}, {0x33abf54 + 0x5B0, SWAP_OWNER_NAME, 8, 2},
    {0x17e098c - 0x40, SWAP_NAME, 23, 22}, {0x33b6830 + 0x5B0, SWAP_NAME, 21, 27}, {0x33b69f0 + 0x5B0, SWAP_NAME, 21, 2},
    {0x33b6d14 + 0x5B0, SWAP_NAME, 21, 2}, {0x33ace8c + 0x5B0, SWAP_NAME, 8, 3}, {0x33aed4c + 0x5B0, SWAP_NAME, 8, 3},
    {0x33addec + 0x5B0, SWAP_NAME, 8, 3}, {0x33abf64 + 0x5B0, SWAP_NAME, 8, 3},
};

static void static_data(Cpu *c) {
    int kind = (int)c->x[0];
    rt_call(c, static_data_original);
    u64 data = c->x[0];
    if (!data || kind < 0 || kind >= FIGHTER_COUNT || (!new_article_n && !weapon_count_n)) return;
    if (static_cache[kind]) { c->x[0] = static_cache[kind]; return; }
    u64 info = *P(u64, data + 8);
    u64 descs = info ? *P(u64, info) : 0, count = info ? *P(u64, info + 8) : 0;
    u32 extra = 0;
    for (u32 i = 0; i < new_article_n; i++) if (new_articles[i].owner == kind) extra++;
    int counts_change = 0;
    for (u64 k = 0; k < count && !counts_change; k++)
        for (u32 j = 0; j < weapon_count_n; j++) if (weapon_counts[j].weapon == *P(i32, descs + 32 * k)) counts_change = 1;
    if (!extra && !counts_change) return;
    u64 table = rt_callsym(c, "malloc", 1, (count + extra) * 32 + 16 + 88);
    if (!table) { c->x[0] = data; return; }
    memcpy(P(void, table), P(void, descs), count * 32);
    u64 n = count;
    for (u32 i = 0; i < new_article_n; i++) {
        if (new_articles[i].owner != kind) continue;
        c->x[0] = (u64)new_articles[i].original_owner;
        rt_call(c, static_data_original);
        u64 src = c->x[0], sinfo = src ? *P(u64, src + 8) : 0;
        u64 sd = sinfo ? *P(u64, sinfo) : 0, sc = sinfo ? *P(u64, sinfo + 8) : 0;
        for (u64 k = 0; k < sc; k++)
            if (*P(i32, sd + 32 * k) == new_articles[i].weapon) { memcpy(P(void, table + 32 * n), P(void, sd + 32 * k), 32); n++; break; }
    }
    for (u64 k = 0; k < n; k++)
        for (u32 j = 0; j < weapon_count_n; j++)
            if (weapon_counts[j].weapon == *P(i32, table + 32 * k)) *P(i32, table + 32 * k + 4) = weapon_counts[j].count;
    u64 new_info = table + 32 * (count + extra), new_data = new_info + 16;
    *P(u64, new_info) = table;
    *P(u64, new_info + 8) = n;
    memcpy(P(void, new_data), P(void, data), 88);
    *P(u64, new_data + 8) = new_info;
    static_cache[kind] = new_data;
    LOG("fighter %d: %llu articles, %llu of them added by plugins", kind, (unsigned long long)n, (unsigned long long)(n - count));
    c->x[0] = new_data;
}

/* ---- status scripts ---- */
/* The status agent (L2CFighterBase, or L2CWeaponCommon) has its status functions put in by its
 * virtual set_status_scripts; the agents the factories make get a copy of their class's vtable
 * whose set_status_scripts runs the original and then puts in the plugins' (as smashline does),
 * with sv_set_status_func(status, line, function); new status kinds raise the agent's status
 * count (global table [0xC]). */
static const u64 STATUS_FACTORIES[] = {0x64bbd0, 0x33abee0 + 0x5B0};   /* fighters, weapons */
static u64 status_factory_original[2];
static struct { u64 original, copy; } vtables[256];
static u32 vtable_n;
static SRWLOCK vtable_lock = SRWLOCK_INIT;

/* A lib::L2CValue holding an integer, on the guest stack at `at` */
static void l2c_int(Cpu *c, u64 at, int value) { rt_callsym(c, "_ZN3lib8L2CValueC1Ei", 2, at, (u64)(u32)value); }

static void set_status_scripts(Cpu *c) {
    u64 agent = c->x[0], vtable = *P(u64, agent), original = 0;
    AcquireSRWLockShared(&vtable_lock);
    for (u32 i = 0; i < vtable_n; i++)
        if (vtables[i].copy == vtable) original = *P(u64, vtables[i].original + 8 * SET_STATUS_SCRIPTS);
    ReleaseSRWLockShared(&vtable_lock);
    if (original) rt_call(c, original);
    Agent a = agent_of(*P(u64, agent + 0x38), -1);
    if (!a.hash) return;
    u64 common = a.weapon ? WEAPON_HASH : FIGHTER_HASH;
    AcquireSRWLockShared(&scripts_lock);
    u32 n = status_n;
    int is_claimed = claimed(a.hash, a.slot);
    ReleaseSRWLockShared(&scripts_lock);
    u32 put = 0;
    int most = -1;
    u64 sp = c->sp;
    c->sp = (sp - 0x40) & ~15ull;
    u64 id = c->sp, line = c->sp + 0x10, value = c->sp + 0x20;
    for (int pass = 0; pass < 2; pass++)   /* (every agent's first, then this one's: these win) */
        for (u32 i = 0; i < n; i++) {
            StatusScript s = statuses[i];
            if (s.agent != (pass ? a.hash : common) || s.line < 0 || !for_slot(s.costume, is_claimed, a.slot)) continue;
            l2c_int(c, id, s.status);
            l2c_int(c, line, s.line);
            c->x[8] = value;   /* (the returned L2CValue) */
            rt_callsym(c, "_ZN7lua2cpp12L2CAgentBase18sv_get_status_funcERKN3lib8L2CValueES4_", 3, agent, id, line);
            remember(agent, STATUS_KEY(s.line, s.status), *P(u32, value) ? *P(u64, value + 8) : 0);
            rt_callsym(c, "_ZN3lib8L2CValueD1Ev", 1, value);
            rt_callsym(c, "_ZN7lua2cpp12L2CAgentBase18sv_set_status_funcERKN3lib8L2CValueES4_Pv", 4, agent, id, line, s.function);
            rt_callsym(c, "_ZN3lib8L2CValueD1Ev", 1, line);
            rt_callsym(c, "_ZN3lib8L2CValueD1Ev", 1, id);
            if (s.status > most) most = s.status;
            put++;
        }
    if (most >= 0) {   /* the status count: L2CFighterBase's global_table (after the 0xC8-byte L2CAgentBase), entry 0xC */
        u64 entry = rt_callsym(c, "_ZNK3lib8L2CValueixEi", 2, agent + 0xC8, 0xCull);
        if (entry && (*P(u32, entry) == 2 || *P(u32, entry) == 0) && *P(i64, entry + 8) < most + 1) {
            LOG("%s: status count %lld -> %d (new status kinds)", a.name, (long long)*P(i64, entry + 8), most + 1);
            *P(u32, entry) = 2;
            *P(i64, entry + 8) = most + 1;
        }
    }
    c->sp = sp;
    if (put) LOG("%s (c%02d) status agent %#llx: %u status scripts from plugins", a.name, a.slot, (unsigned long long)agent, put);
}

static void status_created(Cpu *c, int which) {
    rt_call(c, status_factory_original[which]);
    u64 agent = c->x[0];
    if (!agent || !status_n) return;
    u64 vtable = *P(u64, agent), copy = 0;
    AcquireSRWLockExclusive(&vtable_lock);
    for (u32 i = 0; i < vtable_n && !copy; i++)
        if (vtables[i].original == vtable || vtables[i].copy == vtable) copy = vtables[i].copy;
    if (!copy && vtable_n < sizeof vtables / sizeof *vtables) {
        u64 table[64];
        memcpy(table, P(void, vtable), sizeof table);   /* (more than the class has: never read past its own) */
        table[SET_STATUS_SCRIPTS] = rt_native(set_status_scripts, "smashline set_status_scripts");
        copy = rt_guest_copy(table, sizeof table);
        vtables[vtable_n].original = vtable;
        vtables[vtable_n++].copy = copy;
    }
    ReleaseSRWLockExclusive(&vtable_lock);
    if (copy) *P(u64, agent) = copy;
    c->x[0] = agent;
}
static void status_created_fighter(Cpu *c) { status_created(c, 0); }
static void status_created_weapon(Cpu *c) { status_created(c, 1); }

/* ---- line callbacks ---- */
/* Status Main line callbacks (once per frame, after the status's main function: "opff"-style
 * code): after the agent's status line runs (lua2cpp_common+0x3250, (agent, variadic, string,
 * va_list): smashline's call_line_status_hook), each plugin callback for that agent, or for every
 * fighter or weapon. The other lines' callbacks are inline hooks in Smashline: not yet. */
#define LINE_STATUS_OFFSET 0x3250
int bench_on(void);                                      /* (hle_bench.c: the character test bench) */
void bench_fighter_setup(u64 rec);
void bench_fighter_frame(const char *name, int slot);
void bench_note(const char *event);
static u64 line_status_original;

static void line_status(Cpu *c) {
    u64 fighter = c->x[0];
    rt_call(c, line_status_original);
    u64 result = c->x[0];
    Agent a = agent_of(*P(u64, fighter + 0x38), -1);
    if (!a.hash) return;
    if (!a.weapon) bench_fighter_frame(a.name, a.slot);
    u64 common = a.weapon ? WEAPON_HASH : FIGHTER_HASH;
    u32 n = line_n;
    int is_claimed = -1;
    for (u32 i = 0; i < n; i++) {
        if (lines[i].line != LINE_MAIN || (lines[i].agent != a.hash && lines[i].agent != common)) continue;
        if (lines[i].agent == a.hash) {
            if (is_claimed < 0) is_claimed = claimed(a.hash, a.slot);
            if (!for_slot(lines[i].costume, is_claimed, a.slot)) continue;
        }
        c->x[0] = fighter;
        rt_call(c, lines[i].function);
        if (!lines[i].ran++) {
            char d[96];
            LOG("line callback %s ran for the first time (%s, c%02d)", rt_describe(lines[i].function, d, sizeof d), a.name, a.slot);
        }
    }
    c->x[0] = result;
}

/* A module the game loaded (host.c): the hooks that go into it. */
void skyline_module_loaded(const char *name, u64 base) {
    if ((!line_n && !bench_on()) || strcmp(name, "lua2cpp_common") || line_status_original) return;
    line_status_original = rt_hook_function(base + LINE_STATUS_OFFSET, rt_native(line_status, "smashline line callbacks"));
    LOG("lua2cpp_common loaded: Main line callbacks %s", line_status_original ? "hooked" : "NOT hooked (no function there)");
}

/* ---- ARCropolis' API (what plugins ask of it) ------------------------------------- */
/* The mods' files are already in the game's data (tools/mods.py), so the file system is
 * "mounted" from the start: the event callbacks run once the plugins' mains have. */
static u64 event_callbacks[64][2];   /* (event, function) */
static u32 event_n;
HLE(arcrop_register_event_callback) {   /* (event, fn(event)) */
    char a[96];
    LOG("arcrop event callback for event %u: %s", (u32)c->x[0] & 0xFF, rt_describe(c->x[1], a, sizeof a));
    if (event_n < 64) { event_callbacks[event_n][0] = (u32)c->x[0] & 0xFF; event_callbacks[event_n++][1] = c->x[1]; }
}
HLE(arcrop_require_api_version) { (void)c; }
HLE(arcrop_api_version) { static u64 version; if (!version) { u32 v[2] = {1, 8}; version = rt_guest_copy(v, sizeof v); } c->x[0] = version; }
HLE(arcrop_is_mod_enabled) { c->x[0] = 1; }    /* (hash of a mod's folder): the turned-on mods are the ones applied */
HLE(arcrop_is_file_loaded) { c->x[0] = 1; }
/* File callbacks: arcrop_register_callback(hash, size, fn(hash, buffer, size, *out size) -> bool). On
 * the console ARCropolis runs one as the game loads that file. Here they run once the plugins' mains
 * have (run_file_callbacks), on the file as the mods are applied: <data dir>/../skyline/arc/<hash>.in,
 * which tools/mods.py writes for each hash listed in skyline/callbacks.txt; what they make goes to
 * <hash>.out, which the next apply puts in the game's data (tools/skyline.py probe applies again when
 * that changed). arcrop_load_file and arcrop_get_decompressed_size read the same .in (a later
 * callback for the same file: what the one before made). */
static struct { u64 hash, size, fn; } file_callbacks[256];
static u32 file_callback_n;
static struct { u64 hash; u8 *data; size_t size; } arc_now[64];   /* (while the callbacks run: each file as it is now) */
static u32 arc_now_n;
HLE(arcrop_register_callback) {
    char a[96];
    LOG("arcrop file callback for %#llx (up to %#llx bytes): %s", (unsigned long long)c->x[0], (unsigned long long)c->x[1],
        rt_describe(c->x[2], a, sizeof a));
    if (file_callback_n < 256) {
        file_callbacks[file_callback_n].hash = c->x[0] & 0xFFFFFFFFFFull;
        file_callbacks[file_callback_n].size = c->x[1];
        file_callbacks[file_callback_n++].fn = c->x[2];
    }
}
static int arc_file(u64 hash, u8 **data, size_t *size) {
    hash &= 0xFFFFFFFFFFull;
    for (u32 i = 0; i < arc_now_n; i++)
        if (arc_now[i].hash == hash) { *data = arc_now[i].data; *size = arc_now[i].size; return 1; }
    char path[1200];
    snprintf(path, sizeof path, "%s/../skyline/arc/%010llx.in", rt_data_dir, (unsigned long long)hash);
    FILE *f = fopen(path, "rb");
    if (!f || arc_now_n == 64) { if (f) fclose(f); return 0; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *buf = malloc(n > 0 ? (size_t)n : 1);
    if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) n = 0;
    fclose(f);
    arc_now[arc_now_n].hash = hash; arc_now[arc_now_n].data = buf; arc_now[arc_now_n++].size = n > 0 ? (size_t)n : 0;
    *data = buf; *size = n > 0 ? (size_t)n : 0;
    return 1;
}
HLE(arcrop_get_decompressed_size) {   /* (hash, *size) -> bool */
    u8 *d; size_t n;
    int ok = arc_file(c->x[0], &d, &n);
    if (ok && c->x[1]) *P(u64, c->x[1]) = n;
    c->x[0] = (u64)ok;
}
HLE(arcrop_load_file) {   /* (hash, buffer, length, *out size) -> bool */
    u8 *d; size_t n;
    int ok = arc_file(c->x[0], &d, &n) && n <= c->x[2];
    if (ok) memcpy(G2H(c->x[1]), d, n);
    if (ok && c->x[3]) *P(u64, c->x[3]) = n;
    c->x[0] = (u64)ok;
}
static void run_file_callbacks(Cpu *c) {
    char path[1200];
    snprintf(path, sizeof path, "%s/../skyline/callbacks.txt", rt_data_dir);
    FILE *list = fopen(path, "w");
    for (u32 i = 0; i < file_callback_n; i++) {
        u64 h = file_callbacks[i].hash;
        if (list) fprintf(list, "%010llx %llx\n", (unsigned long long)h, (unsigned long long)file_callbacks[i].size);
        u8 *d; size_t n;
        if (!arc_file(h, &d, &n)) { LOG("file callback for %#llx: no skyline/arc/%010llx.in yet (the next apply writes it)", (unsigned long long)h, (unsigned long long)h); continue; }
        size_t room = file_callbacks[i].size > n ? file_callbacks[i].size : n;
        u64 buf = rt_callsym(c, "malloc", 1, (u64)room), out = rt_callsym(c, "malloc", 1, (u64)8);
        if (!buf || !out) continue;
        memcpy(G2H(buf), d, n);
        *P(u64, out) = n;
        c->x[0] = h; c->x[1] = buf; c->x[2] = room; c->x[3] = out;
        rt_call(c, file_callbacks[i].fn);
        int ok = (c->x[0] & 1) && *P(u64, out) <= room;
        size_t got = ok ? *P(u64, out) : 0;
        LOG("file callback for %#llx: %s (%zu -> %zu bytes)", (unsigned long long)h, ok ? "changed it" : "left it", n, got);
        snprintf(path, sizeof path, "%s/../skyline/arc/%010llx.out", rt_data_dir, (unsigned long long)h);
        if (!ok) remove(path);
        if (ok) {   /* (the next callback for this file, and the .out, get what this one made) */
            u8 *now = malloc(got ? got : 1);
            memcpy(now, G2H(buf), got);
            for (u32 k = 0; k < arc_now_n; k++) if (arc_now[k].hash == h) { free(arc_now[k].data); arc_now[k].data = now; arc_now[k].size = got; }
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(now, 1, got, f); fclose(f); }
        }
        rt_callsym(c, "free", 1, buf);
        rt_callsym(c, "free", 1, out);
    }
    if (list) fclose(list);
}
HLE(arcrop_register_callback_with_path) {
    LOG("arcrop_register_callback_with_path for %#llx: NOT SUPPORTED yet, left out", (unsigned long long)c->x[0]);
}
HLE(arcrop_register_extension_callback) { LOG("arcrop_register_extension_callback: NOT SUPPORTED yet, left out"); }

/* ---- the CSK Collection's API: new characters on the select screen --------------------- */
/* Plugins add character entries (ui_chara_db, ui_layout_db) as Rust structs (the_csk_collection_api:
 * each field an enum, Overwrite(value) or Optional(Option<value>)). They are written to
 * <data dir>/../skyline/csk.json, and tools/mods.py puts them into the game's databases when it
 * applies the mods (a new entry is a copy of the one it is cloned from, with these fields set). */
enum { F_STR, F_H40, F_I16, F_I32, F_I8, F_U8, F_BOOL, F_F32, F_U8MAP, F_H40MAP };
static const struct { u32 size, payload; const char *prc; } FTYPE[] = {
    {24, 8, "string"}, {24, 8, "hash40"}, {8, 4, "short"}, {12, 4, "int"}, {8, 4, "sbyte"}, {8, 4, "byte"},
    {8, 4, "bool"}, {12, 4, "float"}, {56, 8, "byte"}, {56, 8, "hash40"},
};
typedef struct { const char *name; int type; } Field;
static const Field CHARA_FIELDS[] = {
    {"name_id", F_STR}, {"fighter_kind", F_H40}, {"fighter_kind_corps", F_H40}, {"ui_series_id", F_H40},
    {"fighter_type", F_H40}, {"alt_chara_id", F_H40}, {"exhibit_year", F_I16}, {"exhibit_day_order", F_I32},
    {"ext_skill_page_num", F_I8}, {"is_img_ext_skill_page0", F_BOOL}, {"is_img_ext_skill_page1", F_BOOL},
    {"is_img_ext_skill_page2", F_BOOL}, {"skill_list_order", F_I8}, {"disp_order", F_I8}, {"save_no", F_I8},
    {"chara_count", F_I8}, {"can_select", F_BOOL}, {"is_usable_soundtest", F_BOOL}, {"is_called_pokemon", F_BOOL},
    {"is_mii", F_BOOL}, {"is_boss", F_BOOL}, {"is_hidden_boss", F_BOOL}, {"is_dlc", F_BOOL}, {"is_patch", F_BOOL},
    {"is_plural_message", F_BOOL}, {"is_plural_narration", F_BOOL}, {"is_article", F_BOOL}, {"extra_flags", F_I32},
    {"has_multiple_face", F_BOOL}, {"result_pf0", F_BOOL}, {"result_pf1", F_BOOL}, {"result_pf2", F_BOOL},
    {"color_num", F_U8}, {"extra_index_maps", F_U8MAP}, {"extra_hash_maps", F_H40MAP}, {"shop_item_tag", F_H40}, {0, 0}};
/* ui_layout_db: ui_chara_id, chara_color, then eye flashes and the many offsets/scales (floats) */
static Field LAYOUT_FIELDS[160];
static void layout_fields(void) {
    static char names[160][40];
    int n = 0;
#define ADD(t, ...) do { snprintf(names[n], sizeof names[n], __VA_ARGS__); LAYOUT_FIELDS[n].name = names[n]; LAYOUT_FIELDS[n++].type = t; } while (0)
    ADD(F_H40, "ui_chara_id"); ADD(F_U8, "chara_color");
    for (int e = 0; e < 3; e++) {
        ADD(F_U8, "eye_%d_flash_count", e);
        for (int k = 0; k < 5; k++) { ADD(F_F32, "eye_%d_flash%d_pos_x", e, k); ADD(F_F32, "eye_%d_flash%d_pos_y", e, k); }
    }
    ADD(F_F32, "eye_flash_info_pos_x"); ADD(F_F32, "eye_flash_info_pos_y"); ADD(F_BOOL, "spirits_eye_visible");
    static const char *const sets[] = {"chara_1", "chara_1_1", "chara_1_2", "chara_1_3", "chara_1_4", "chara_1_5", "chara_3_0",
                                       "chara_3_1", "chara_3_2", "chara_3_3", "chara_3_4", "chara_3_5", "chara_3_6", "chara_3_7",
                                       "chara_5", "chara_select_icon_list", "chara_7_0", "chara_7_1", "chara_0", 0};
    for (int k = 0; sets[k]; k++) { ADD(F_F32, "%s_offset_x", sets[k]); ADD(F_F32, "%s_offset_y", sets[k]); ADD(F_F32, "%s_scale", sets[k]); }
    LAYOUT_FIELDS[n].name = 0;
#undef ADD
}

static FILE *csk;   /* (the json being written: <data dir>/../skyline/csk.json.tmp, open from the plugins' loading on) */
static char csk_path[1200], csk_tmp[1220];
static int csk_loaded;   /* (the plugins' mains have run: each entry from now on is published at once) */
/* csk.json: what is written so far, closed as a list - if it changed (then the mods need applying again) */
static SRWLOCK csk_lock = SRWLOCK_INIT;
static void csk_publish(void) {
    if (!csk) return;
    AcquireSRWLockExclusive(&csk_lock);
    fflush(csk);
    FILE *in = fopen(csk_tmp, "rb");
    if (!in) { ReleaseSRWLockExclusive(&csk_lock); return; }
    fseek(in, 0, SEEK_END);
    long n = ftell(in);
    fseek(in, 0, SEEK_SET);
    char *text = malloc((size_t)n + 8);
    size_t got = fread(text, 1, (size_t)n, in);
    fclose(in);
    memcpy(text + got, "\n]\n", 3);
    got += 3;
    FILE *old_f = fopen(csk_path, "rb");
    int same = 0;
    if (old_f) {
        char *old = malloc(got + 1);
        same = fread(old, 1, got + 1, old_f) == got && !memcmp(old, text, got);
        free(old);
        fclose(old_f);
    }
    if (!same) {
        FILE *out = fopen(csk_path, "wb");
        if (out) { fwrite(text, 1, got, out); fclose(out); }
        fprintf(stderr, "[skyline] the plugins' character entries changed (%s): the mods need applying again for them to show\n", csk_path);
    }
    free(text);
    ReleaseSRWLockExclusive(&csk_lock);
}
static DWORD WINAPI probe_timer(void *unused) {   /* (SSBU_SKYLINE_PROBE with the real ARCropolis: its chainload is done by then) */
    (void)unused;
    Sleep(60000);
    csk_publish();
    fprintf(stderr, "[skyline] probe done (60 s after the plugins loaded)\n");
    fflush(stderr);
    ExitProcess(0);
}
static u64 csk_current;   /* (the ui_chara_id of the entry being written) */
typedef struct { u64 ui_chara, base; int start; } Redirect;   /* (base: the fighter's own ui_chara, found when needed) */
static Redirect redirects[64];
static u32 redirect_n;
static int csk_items;

static void json_str(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) { if (*s == '"' || *s == '\\') fputc('\\', f); if ((u8)*s >= 0x20) fputc(*s, f); }
    fputc('"', f);
}

/* One value of a field type at `at` (an Overwrite's or a Some's), as JSON. */
static void json_value(FILE *f, int type, u64 at) {
    switch (type) {
    case F_STR: { u64 p = *P(u64, at); json_str(f, p ? P(const char, p) : ""); break; }
    case F_H40: fprintf(f, "\"%#llx\"", (unsigned long long)*P(u64, at)); break;
    case F_I16: fprintf(f, "%d", *P(i16, at)); break;
    case F_I32: fprintf(f, "%d", *P(i32, at)); break;
    case F_I8: fprintf(f, "%d", *P(i8, at)); break;
    case F_U8: fprintf(f, "%u", *P(u8, at)); break;
    case F_BOOL: fprintf(f, "%s", *P(u8, at) ? "true" : "false"); break;
    case F_F32: fprintf(f, "%.9g", (double)*P(f32, at)); break;
    }
}

/* Where an Optional's value is (Rust's Option layout: the tag first), or 0 for None. */
static u64 option_value(int type, u64 at) {
    switch (type) {
    case F_STR: case F_H40: return *P(u8, at) == 1 ? at + 8 : 0;
    case F_I16: return *P(u16, at) == 1 ? at + 2 : 0;
    case F_I32: case F_F32: return *P(u32, at) == 1 ? at + 4 : 0;
    case F_I8: case F_U8: return *P(u8, at) == 1 ? at + 1 : 0;
    case F_BOOL: return *P(u8, at) <= 1 ? at : 0;   /* (bool's niche: 2 is None) */
    }
    return 0;
}

/* A std HashMap<u64, V> (hashbrown): its table {ctrl, bucket_mask, growth_left, items} and the hash
 * keys {k0, k1} - in that order as built for skyline-rs (the other order, keys first and
 * bucket_mask before ctrl, is taken too); the entries (key, V) lie below ctrl, a byte of ctrl
 * each (top bit clear: in use). */
static int guest_pointer(u64 v) { return v >= 0x1000000000ull && v < 0x8000000000ull && !(v & 7); }
static void json_map(FILE *f, int type, u64 map, int *first) {
    u64 q[6];
    for (int i = 0; i < 6; i++) q[i] = *P(u64, map + 8 * i);
    u64 ctrl, mask, items;
    if (guest_pointer(q[0])) ctrl = q[0], mask = q[1], items = q[3];
    else ctrl = q[3], mask = q[2], items = q[5];
    if (!guest_pointer(ctrl) || mask & (mask + 1)) return;
    if (!ctrl || mask > 4095 || items > mask + 1) return;
    int vtype = type == F_U8MAP ? F_U8 : F_H40;
    u64 size = 8 + (vtype == F_U8 ? 8 : 24);
    for (u64 i = 0; i <= mask; i++) {
        if (*P(u8, ctrl + i) & 0x80) continue;
        u64 e = ctrl - (i + 1) * size, key = *P(u64, e), v = e + 8;
        u32 tag = *P(u32, v);
        u64 val = tag == 0 ? v + FTYPE[vtype].payload : tag == 1 ? option_value(vtype, v + FTYPE[vtype].payload) : 0;
        if (!val) continue;
        if (key == 0x11895f00fcull && redirect_n < 64 && csk_current)   /* (color_start_index: csk redirect) */
            redirects[redirect_n++] = (Redirect){csk_current, 0, *P(u8, val)};
        fprintf(f, "%s\"%#llx\": [\"%s\", ", *first ? "" : ", ", (unsigned long long)key, FTYPE[vtype].prc);
        json_value(f, vtype, val);
        fputc(']', f);
        *first = 0;
    }
}

/* An entry struct: id (u64), clone-from id (Option<u64>), then the fields, repr(C)-aligned. */
static u64 csk_kinds[64][2];   /* (ui_chara, its fighter_kind hash) */
static u32 csk_kind_n;
/* The plugin module that called (its return address), for csk.json: tools/mods.py keeps the entries
 * of turned-on mods' plugins only. */
static const char *caller_module(Cpu *c, char *buf, size_t n) {
    rt_describe(c->x[30], buf, n);
    char *plus = strchr(buf, '+');
    if (plus) *plus = 0;
    return buf;
}
static void json_entry(Cpu *c, const char *kind, const char *id_key, const Field *fields, u64 at) {
    if (!csk) return;
    char who[96];
    csk_current = !strcmp(kind, "ui_chara_db") ? *P(u64, at) : 0;
    fprintf(csk, "%s  {\"kind\": \"%s\", \"plugin\": \"%s\", \"%s\": \"%#llx\", \"clone_from\": ", csk_items++ ? ",\n" : "", kind,
            caller_module(c, who, sizeof who), id_key, (unsigned long long)*P(u64, at));
    if (*P(u8, at + 8) == 1) fprintf(csk, "\"%#llx\"", (unsigned long long)*P(u64, at + 16));
    else fprintf(csk, "null");
    fprintf(csk, ", \"set\": {");
    u64 o = 24;
    int first = 1;
    for (const Field *fl = fields; fl->name; fl++) {
        u32 align = FTYPE[fl->type].payload;
        o = (o + align - 1) & ~(u64)(align - 1);
        u64 v = at + o, p = v + FTYPE[fl->type].payload;
        u32 tag = *P(u32, v);
        if (fl->type == F_U8MAP || fl->type == F_H40MAP) {
            if (tag <= 1) json_map(csk, fl->type, p, &first);
        } else {
            u64 val = tag == 0 ? p : tag == 1 ? option_value(fl->type, p) : 0;
            if (val && csk_current && !strcmp(fl->name, "fighter_kind") && csk_kind_n < 64)
                csk_kinds[csk_kind_n][0] = csk_current, csk_kinds[csk_kind_n++][1] = *P(u64, val) & 0xFFFFFFFFFFull;
            if (val) {
                fprintf(csk, "%s\"%s\": [\"%s\", ", first ? "" : ", ", fl->name, FTYPE[fl->type].prc);
                json_value(csk, fl->type, val);
                fputc(']', csk);
                first = 0;
            }
        }
        o += FTYPE[fl->type].size;
    }
    fprintf(csk, "}}");
}

/* The mod plugins call the CSK Collection's own add, not the native one, so the costume a new
 * character stands in for (color_start_index, c120 for a Wolf slot) has to be read here. The
 * match otherwise keeps costume 0 and the fighter it was cloned from comes out unchanged. */
static void csk_record_added(Cpu *c) {
    AcquireSRWLockExclusive(&csk_lock);
    json_entry(c, "ui_chara_db", "ui_chara_id", CHARA_FIELDS, c->x[0]);
    ReleaseSRWLockExclusive(&csk_lock);
    if (csk_loaded) csk_publish();
    u64 at = c->x[0], id = *P(u64, at) & 0xFFFFFFFFFFull;
    if (!id) return;
    for (u32 r = 0; r < redirect_n; r++) if ((redirects[r].ui_chara & 0xFFFFFFFFFFull) == id) return;
    for (u64 o = 0; o + 16 <= 0x600; o += 8) {
        if ((*P(u64, at + o) & 0xFFFFFFFFFFull) != 0x11895f00fcull) continue;   /* hash40("color_start_index") */
        int start = *P(u8, at + o + 12);   /* Overwrite's byte sits 4 bytes after the tag */
        if (start < 8 || start > 200) {
            start = 0;
            for (int k = 8; k <= 20; k++) {
                int b = *P(u8, at + o + k);
                if (b >= 8 && b <= 200) { start = b; break; }
            }
        }
        if (!start || redirect_n >= 64) return;
        redirects[redirect_n++] = (Redirect){id, 0, start};
        LOG("redirect recorded: character %#llx uses costumes from c%02d", (unsigned long long)id, start);
        return;
    }
}

HLE(add_chara_db_entry_info) {
    LOG("CSK: character entry %#llx", (unsigned long long)*P(u64, c->x[0]));
    AcquireSRWLockExclusive(&csk_lock);
    json_entry(c, "ui_chara_db", "ui_chara_id", CHARA_FIELDS, c->x[0]);
    ReleaseSRWLockExclusive(&csk_lock);
    if (csk_loaded) csk_publish();
}
HLE(add_chara_layout_db_entry_info) {
    LOG("CSK: character layout entry %#llx", (unsigned long long)*P(u64, c->x[0]));
    if (!LAYOUT_FIELDS[0].name) layout_fields();
    AcquireSRWLockExclusive(&csk_lock);
    json_entry(c, "ui_layout_db", "ui_layout_id", LAYOUT_FIELDS, c->x[0]);
    ReleaseSRWLockExclusive(&csk_lock);
    if (csk_loaded) csk_publish();
}
HLE(add_narration_characall_entry) {   /* (C string) -> bool */
    AcquireSRWLockExclusive(&csk_lock);
    char who[96];
    if (csk) {
        fprintf(csk, "%s  {\"kind\": \"narration\", \"plugin\": \"%s\", \"label\": ", csk_items++ ? ",\n" : "", caller_module(c, who, sizeof who));
        json_str(csk, guest_str(c->x[0]));
        fprintf(csk, "}");
    }
    ReleaseSRWLockExclusive(&csk_lock);
    if (csk_loaded) csk_publish();
    c->x[0] = 1;
}
HLE(set_fighter_jingle) { LOG("CSK: fighter jingle for %#llx: %s (left out)", (unsigned long long)c->x[0], guest_str(c->x[1])); }
HLE(allow_ui_chara_hash_online) { (void)c; }
HLE(disable_ui_chara_hash_online) { (void)c; }
HLE(play_bgm) { (void)c; }
HLE(is_online) { c->x[0] = 0; }
HLE(csk_collection_version) { static u64 v; if (!v) { u32 ver[3] = {5, 0, 6}; v = rt_guest_copy(ver, sizeof ver); } c->x[0] = v; }
/* (the rest of the CSK Collection's tables: not yet - each left out, said once) */
static void csk_unsupported(const char *name) { LOG("CSK: %s is NOT SUPPORTED yet, left out", name); }
HLE(add_series_db_entry_info) { (void)c; csk_unsupported("add_series_db_entry_info"); }
HLE(add_bgm_db_entry_info) { (void)c; csk_unsupported("add_bgm_db_entry_info"); }
HLE(add_stream_set_entry_info) { (void)c; csk_unsupported("add_stream_set_entry_info"); }
HLE(add_assigned_info_entry_info) { (void)c; csk_unsupported("add_assigned_info_entry_info"); }
HLE(add_stream_property_entry_info) { (void)c; csk_unsupported("add_stream_property_entry_info"); }
HLE(add_new_sli_entry) { (void)c; csk_unsupported("add_new_sli_entry"); }
HLE(add_new_bgm_property_entry) { (void)c; csk_unsupported("add_new_bgm_property_entry"); }
HLE(add_tracks_to_playlist) { (void)c; csk_unsupported("add_tracks_to_playlist"); }
HLE(add_stage_db_entry) { (void)c; csk_unsupported("add_stage_db_entry"); }
HLE(add_ui_stage_db_resources_entry) { (void)c; csk_unsupported("add_ui_stage_db_resources_entry"); }
HLE(add_gametitle_db_entry_info) { (void)c; csk_unsupported("add_gametitle_db_entry_info"); }
HLE(add_tips_db_entry_info) { (void)c; csk_unsupported("add_tips_db_entry_info"); }
HLE(add_amiibo_db_entry_info) { (void)c; csk_unsupported("add_amiibo_db_entry_info"); }
HLE(add_mii_body_db_entry_info) { (void)c; csk_unsupported("add_mii_body_db_entry_info"); }
HLE(add_mii_hat_db_entry_info) { (void)c; csk_unsupported("add_mii_hat_db_entry_info"); }
HLE(load_ui_file) { (void)c; csk_unsupported("load_ui_file"); }
HLE(change_entry_chara_ui) { (void)c; csk_unsupported("change_entry_chara_ui"); }
HLE(get_color_from_entry_id) { c->x[0] = 0; }
HLE(get_ui_chara_from_entry_id) { c->x[0] = 0; }
HLE(get_victor_color) { c->x[0] = 0; }

/* ---- the CSK Collection's select-screen redirect: a new character's colors are its fighter's
 * costumes from color_start_index on (c120...). The game makes each player's fighter from a
 * record (main+0x66dcf0: kind at +8, costume at +0x19, entry at +0xa7); before that, its
 * pre-battle set-up (main+0x141b360) has each player's choice. */
static u64 entry_chara[8];   /* the ui_chara each entry chose (0: not known) */
#define PRE_BATTLE 0x141b360
#define FIGHTER_SETUP 0x66dcf0
#define COLOR_START_KEY 0x11895f00fcull   /* hash40("color_start_index") */
static u64 pre_battle_original, fighter_setup_original;

static void scan_for(const char *what, u64 at, u64 size) {
    for (u64 o = 0; o + 8 <= size; o += 8) {
        u64 v = *P(u64, at + o) & 0xFFFFFFFFFFull;
        for (u32 r = 0; r < redirect_n; r++)
            if (v == redirects[r].ui_chara) LOG("redirect probe: %s+%#llx holds %#llx", what, (unsigned long long)o, (unsigned long long)v);
    }
}

static void pre_battle(Cpu *c) {
    u64 setup = *P(u64, c->x[0] + 0x28);
    bench_note("pre-battle set-up");
    rt_call(c, pre_battle_original);
    if (!redirect_n || !setup) return;
    u64 list = *P(u64, setup + 0x1010);
    int n = list ? *P(i32, list) : 0;
    if (getenv("SSBU_REDIRECT_PROBE")) {
        scan_for("setup", setup, 0x1100);
        if (list) scan_for("list", list, 8 * 0x1d8 + 0x100);
    }
    LOG("pre-battle: %d players", n);
}

/* The redirect a record names, if any: a new character's ui_chara hash among its first 0x400 bytes. */
static Redirect *redirect_in(u64 rec, u64 *where) {
    for (u64 o = 0; o + 8 <= 0x400; o += 8) {
        u64 v = *P(u64, rec + o) & 0xFFFFFFFFFFull;
        for (u32 r = 0; r < redirect_n; r++)
            if (v == redirects[r].ui_chara) { *where = o; return &redirects[r]; }
    }
    return NULL;
}

/* The game's portrait paths (main+0x3238a70: (tagged ui_chara hash, color, image kind)): a
 * redirected character's costume (c120...) is its own color again (0...), with its own
 * portraits - wherever the game asks by the costume, as the battle's HUD does. */
#define UI_PATH 0x3238a70
static u64 ui_path_original;

static void ui_path(Cpu *c) {
    u64 h = c->x[0] & 0xFFFFFFFFFFull;
    int color = (int)c->x[1];
    bench_note("portrait path asked");
    for (u32 i = 0; i < redirect_n; i++) {
        Redirect *r = &redirects[i];
        if (color < r->start || color >= r->start + 64) continue;
        if (h == r->ui_chara || (r->base && h == r->base)) {
            c->x[0] = (c->x[0] & ~0xFFFFFFFFFFull) | r->ui_chara;
            c->x[1] = (u64)(color - r->start);
            break;
        }
    }
    rt_call(c, ui_path_original);
}

static void fighter_setup(Cpu *c) {
    u64 rec = c->x[0], where = 0;
    int entry = *P(u8, rec + 0xa7);
    if (bench_on()) {   /* (the bench's fighter and costume; a new character's portraits by its costume) */
        const char *was = guest_name(FIGHTER_NAMES, *P(i8, rec + 8), FIGHTER_COUNT);
        char from[64];
        snprintf(from, sizeof from, "ui_chara_%s", was ? was : "");
        bench_fighter_setup(rec);
        const char *n = guest_name(FIGHTER_NAMES, *P(i8, rec + 8), FIGHTER_COUNT);
        if (was && n) {   /* (the select screen's pick, where the record names it: the bench's fighter, for the HUD) */
            char to[64];
            snprintf(to, sizeof to, "ui_chara_%s", n);
            u64 hf = hash40(from, strlen(from)), ht = hash40(to, strlen(to));
            for (u64 o = 0; o + 8 <= 0x200; o += 8)
                if ((*P(u64, rec + o) & 0xFFFFFFFFFFull) == hf) *P(u64, rec + o) = (*P(u64, rec + o) & ~0xFFFFFFFFFFull) | ht;
        }
        int k = *P(u8, rec + 0x19);
        char fk[64], ui[64];
        if (n) snprintf(fk, sizeof fk, "fighter_kind_%s", n), snprintf(ui, sizeof ui, "ui_chara_%s", n);
        for (u32 i = 0; n && i < redirect_n; i++)
            for (u32 j = 0; j < csk_kind_n; j++)
                if (csk_kinds[j][0] == redirects[i].ui_chara && csk_kinds[j][1] == hash40(fk, strlen(fk)) &&
                    k >= redirects[i].start && k < redirects[i].start + 64)
                    redirects[i].base = hash40(ui, strlen(ui));
        rt_call(c, fighter_setup_original);
        return;
    }
    Redirect *r = redirect_n ? redirect_in(rec, &where) : NULL;
    if (!r && entry < 8 && entry_chara[entry])
        for (u32 i = 0; i < redirect_n && !r; i++)
            if (redirects[i].ui_chara == entry_chara[entry]) r = &redirects[i];
    if (r && !r->base) {   /* (the fighter's own ui_chara: ui_chara_<its name>) */
        const char *n = guest_name(FIGHTER_NAMES, *P(i8, rec + 8), FIGHTER_COUNT);
        char ui[64];
        if (n) { snprintf(ui, sizeof ui, "ui_chara_%s", n); r->base = hash40(ui, strlen(ui)); }
    }
    if (r && *P(u8, rec + 0x19) < r->start) {
        LOG("redirect: entry %d (%#llx at +%#llx): costume c%02u -> c%02u", entry, (unsigned long long)r->ui_chara,
            (unsigned long long)where, *P(u8, rec + 0x19), *P(u8, rec + 0x19) + r->start);
        *P(u8, rec + 0x19) += (u8)r->start;
    } else if (getenv("SSBU_REDIRECT_PROBE")) {
        LOG("fighter set-up: entry %d kind %d costume %u: no redirect", entry, *P(i8, rec + 8), *P(u8, rec + 0x19));
    }
    rt_call(c, fighter_setup_original);
}

/* ---- loading ----------------------------------------------------------------------- */
void skyline_load_plugins(Cpu *c) {
    static int done;
    if (done++) return;
    char path[1200], line[256];
    snprintf(path, sizeof path, "%s/../skyline/plugins.txt", rt_data_dir);
    FILE *f = fopen(path, "r");
    if (!f && bench_on()) {   /* (no plugins: the bench's own hooks) */
        fighter_setup_original = rt_hook_function(MAIN_BASE + FIGHTER_SETUP, rt_native(fighter_setup, "bench fighter set-up"));
        pre_battle_original = rt_hook_function(MAIN_BASE + PRE_BATTLE, rt_native(pre_battle, "csk pre-battle"));
        ui_path_original = rt_hook_function(MAIN_BASE + UI_PATH, rt_native(ui_path, "csk portrait paths"));
        return;
    }
    if (!f) return;
    const char *q = getenv("SSBU_SKYLINE_LOG");
    quiet = q && *q == '0';
    u64 saved[31];
    memcpy(saved, c->x, sizeof saved);
    /* (plugins find Smashline with nn::ro::LookupSymbol: the native one, from now on) */
    rt_wrap_symbol("_ZN2nn2ro12LookupSymbolEPmPKc", lookup_symbol);
    fs_mount_sd();
    void rtld_init(void);   /* (hle_rtld.c: nn::ro's module lists, LoadModuleInternal) */
    rtld_init();
    plugin_shims();
    /* The real Smashline among the plugins (a standard plugin, tools/skyline.py): it hooks what the
     * native one would, so the native one stays out */
    int real_smashline = 0;
    while (fgets(line, sizeof line, f)) { line[strcspn(line, "\r\n#")] = 0; if (!strcmp(line, "smashline")) real_smashline = 1; }
    rewind(f);
    if (real_smashline) LOG("the real Smashline is listed: the native one is left out");
    for (u32 k = 0; k < NFACTORIES && !real_smashline; k++) {
        factory_original[k] = rt_hook_function(MAIN_BASE + FACTORIES[k].offset, rt_native(factory_hooks[k], "smashline create_agent"));
        if (!factory_original[k]) LOG("create_agent at main+%#llx is not a function: Smashline's acmd scripts will not reach it",
                                      (unsigned long long)FACTORIES[k].offset);
    }
    FIGHTER_HASH = hash40("fighter", 7);
    WEAPON_HASH = hash40("weapon", 6);
    GuestFn *status_hooks[2] = {status_created_fighter, status_created_weapon};
    for (int k = 0; k < 2 && !real_smashline; k++) {
        status_factory_original[k] = rt_hook_function(MAIN_BASE + STATUS_FACTORIES[k], rt_native(status_hooks[k], "smashline create_agent_status"));
        if (!status_factory_original[k]) LOG("create_agent_status at main+%#llx is not a function: status scripts will not reach it",
                                             (unsigned long long)STATUS_FACTORIES[k]);
    }
    snprintf(csk_path, sizeof csk_path, "%s/../skyline/csk.json", rt_data_dir);
    snprintf(csk_tmp, sizeof csk_tmp, "%s.tmp", csk_path);
    csk = fopen(csk_tmp, "w");
    if (csk) fprintf(csk, "[\n");
    if (!real_smashline) {
    static_data_original = rt_hook_function(MAIN_BASE + STATIC_FIGHTER_DATA, rt_native(static_data, "smashline fighter articles"));
    fighter_init_original = rt_hook_function(MAIN_BASE + FIGHTER_INIT, rt_native(fighter_init, "smashline fighter set-up"));
    note_hook_target(MAIN_BASE + FIGHTER_INIT);
    for (u32 k = 0; k < sizeof WEAPON_SWAPS / sizeof *WEAPON_SWAPS; k++)
        add_inline_hook(MAIN_BASE + WEAPON_SWAPS[k].offset, 0, swap_weapon,
                        (u64)WEAPON_SWAPS[k].kind << 16 | (u64)WEAPON_SWAPS[k].src << 8 | (u64)WEAPON_SWAPS[k].dst);
    note_hook_target(MAIN_BASE + STATIC_FIGHTER_DATA);
    }
    pre_battle_original = rt_hook_function(MAIN_BASE + PRE_BATTLE, rt_native(pre_battle, "csk pre-battle"));
    fighter_setup_original = rt_hook_function(MAIN_BASE + FIGHTER_SETUP, rt_native(fighter_setup, "csk fighter set-up"));
    note_hook_target(MAIN_BASE + FIGHTER_SETUP);   /* (called directly too: hookable from the next build on) */
    ui_path_original = rt_hook_function(MAIN_BASE + UI_PATH, rt_native(ui_path, "csk portrait paths"));
    note_hook_target(MAIN_BASE + UI_PATH);
    /* every plugin loaded (its exports there for the others), then each main run, as Skyline does:
     * a plugin whose main has another load the mods' plugins (ARCropolis' chainload) finds Smashline's
     * exports whatever the order */
    static char names[64][128];
    static u64 bases[64];
    int count = 0;
    while (fgets(line, sizeof line, f) && count < 64) {
        line[strcspn(line, "\r\n#")] = 0;
        if (!*line) continue;
        u64 base = rt_load_nro_named(c, line);
        if (!base) { fprintf(stderr, "[skyline] plugin %s is not recompiled into this build: left out\n", line); continue; }
        snprintf(names[count], sizeof names[count], "%s", line);
        bases[count++] = base;
    }
    fclose(f);
    for (int i = 0; i < count; i++) {
        u64 entry = rt_module_export(bases[i], "main");
        fprintf(stderr, "[skyline] plugin %s at %#llx: running main\n", names[i], (unsigned long long)bases[i]);
        if (entry) rt_call(c, entry);
        fprintf(stderr, "[skyline] plugin %s: main returned\n", names[i]);
    }
    for (int ev = 0; ev < 2; ev++)   /* ArcFilesystemMounted, ModFilesystemMounted */
        for (u32 i = 0; i < event_n; i++)
            if (event_callbacks[i][0] == (u64)ev) {
                char a[96];
                LOG("running arcrop event %d callback %s", ev, rt_describe(event_callbacks[i][1], a, sizeof a));
                c->x[0] = (u64)ev;
                rt_call(c, event_callbacks[i][1]);
            }
    if (file_callback_n) run_file_callbacks(c);
    csk_publish();   /* (a change means the mods need applying again: tools/skyline.py probe does) */
    csk_loaded = 1;
    if (getenv("SSBU_SKYLINE_PROBE")) {
        if (rt_symbol("arcrop_register_callback") && rt_module_export(rt_symbol("arcrop_register_callback") & ~0xFFFFFFull, "main"))
            CloseHandle(CreateThread(NULL, 0, probe_timer, NULL, 0, NULL));   /* (the real ARCropolis: the mods' plugins run later, as it mounts the mods) */
        else { fprintf(stderr, "[skyline] probe done\n"); fflush(stderr); ExitProcess(0); }
    }
    fprintf(stderr, "[skyline] plugins installed %u acmd scripts, %u status scripts, %u line callbacks\n", acmd_n, status_n, line_n);
    u64 csk_add = rt_symbol("add_chara_db_entry_info");
    if (csk_add && !csk_add_original) csk_add_original = rt_hook_function(csk_add, rt_native(csk_add_traced, "csk add_chara_db_entry_info"));
    memcpy(c->x, saved, sizeof saved);
}
