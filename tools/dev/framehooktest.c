/* The frame hooks (runtime/frame_hooks.h, runtime/hle_each_frame.c) with made-up handlers and a
 * made-up game function behind each wrap: what runs with nothing registered, the order handlers
 * nest in, the arguments and results each one sees, the two points kept apart, the limits, a
 * registration made while a call runs, and registrations from several threads while the wraps run.
 *
 *   Windows: clang-cl /nologo /O1 tools\dev\framehooktest.c /Febuild\framehooktest.exe   (with cc.BASE_CFLAGS)
 *   Linux, macOS: clang -O1 -Wall -Werror -pthread -I runtime/macos/include -I runtime/linux/include \
 *                 -I runtime tools/dev/framehooktest.c -o framehooktest
 *
 * Prints "<n> checks, 0 failed" and exits 0 when every check passes (tests/test_frame_hooks.py). */
#include <stdio.h>
#include "../../runtime/hle_each_frame.c"
#ifndef _WIN32
#include <pthread.h>
#endif

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* What ran, in order: a handler's name and < (before) or > (after); F the frame end, L the line. */
static char trace[512];
static void say(const char *what) { size_t n = strlen(trace); snprintf(trace + n, sizeof trace - n, "%s%s", n ? " " : "", what); }
static void forget(void) {
    trace[0] = 0;
    memset(lists, 0, sizeof lists);
    memset((void *)in_use, 0, sizeof in_use);
}

/* The game's functions behind the wraps: each sets a result of its own and clobbers a register. */
static u64 game_saw[8];
void f_71038701e0_orig(Cpu *c) { memcpy(game_saw, c->x, sizeof game_saw); say("F"); c->x[0] = 0xF0044007u; c->x[9] = 0x99; }
static void line_orig(Cpu *c) { memcpy(game_saw, c->x, sizeof game_saw); say("L"); c->x[0] = 0x1111; c->x[9] = 0x98; }
void hle_wrap__ZN7lua2cpp16L2CFighterCommon31sys_line_system_control_fighterEv(Cpu *c, GuestFn *orig);
void hle_wrap_71038701e0(Cpu *c);

static Cpu cpu;
static void called_with(u64 base) {
    memset(&cpu, 0, sizeof cpu);
    for (int i = 0; i < 31; i++) cpu.x[i] = base + (u64)i;
    cpu.sp = 0x7700100000ull;
}
static void frame_end(void) { called_with(0x100); hle_wrap_71038701e0(&cpu); }
static void frame_line(void) { called_with(0x200); hle_wrap__ZN7lua2cpp16L2CFighterCommon31sys_line_system_control_fighterEv(&cpu, line_orig); }

/* A made-up handler: its name (the context) in the trace. */
static void before(Cpu *c, const u64 args[8], void *ctx) { (void)c; (void)args; char t[16]; snprintf(t, sizeof t, "%s<", (const char *)ctx); say(t); }
static void after(Cpu *c, const u64 args[8], void *ctx) { (void)c; (void)args; char t[16]; snprintf(t, sizeof t, "%s>", (const char *)ctx); say(t); }

/* Nothing registered: the game's function once, as called, and nothing else. */
static void nothing(void) {
    forget();
    frame_end();
    CHECK(!strcmp(trace, "F"), "the frame end: only the game's (%s)", trace);
    CHECK(game_saw[0] == 0x100 && game_saw[2] == 0x102 && game_saw[7] == 0x107 && cpu.x[0] == 0xF0044007u && cpu.x[9] == 0x99 &&
          cpu.x[10] == 0x10a && cpu.sp == 0x7700100000ull, "the frame end: called with the registers as they were, its results left");
    trace[0] = 0;
    frame_line();
    CHECK(!strcmp(trace, "L") && game_saw[0] == 0x200 && cpu.x[0] == 0x1111, "the frame line: only the game's (%s), through the original it was given", trace);
    CHECK(frame_hook_count(FRAME_HOOK_END) == 0 && frame_hook_count(FRAME_HOOK_LINE) == 0, "no registrations");
}

/* Lower orders outside; one order: the first registered outside. */
static void order(void) {
    forget();
    CHECK(frame_hook_add(FRAME_HOOK_END, 20, before, after, "B"), "B");
    CHECK(frame_hook_add(FRAME_HOOK_END, 10, before, after, "A"), "A");
    CHECK(frame_hook_add(FRAME_HOOK_END, 20, NULL, after, "C"), "C (after only)");
    CHECK(frame_hook_add(FRAME_HOOK_END, 30, before, NULL, "D"), "D (before only)");
    CHECK(frame_hook_add(FRAME_HOOK_END, 10, before, after, "E"), "E");
    CHECK(frame_hook_add(FRAME_HOOK_END, -5, before, after, "Z"), "Z (an order below 0)");
    frame_end();
    CHECK(!strcmp(trace, "Z< A< E< B< D< F C> B> E> A> Z>"), "nested by order, then by registration: %s", trace);
    CHECK(frame_hook_count(FRAME_HOOK_END) == 6, "6 registered (%d)", frame_hook_count(FRAME_HOOK_END));
    trace[0] = 0;
    frame_line();
    CHECK(!strcmp(trace, "L"), "the frame line's point has none of them: %s", trace);
    trace[0] = 0;
    CHECK(frame_hook_add(FRAME_HOOK_LINE, 1, before, after, "Q"), "Q at the frame line");
    frame_line();
    CHECK(!strcmp(trace, "Q< L Q>"), "the frame line's own: %s", trace);
    trace[0] = 0;
    frame_end();
    CHECK(!strcmp(trace, "Z< A< E< B< D< F C> B> E> A> Z>"), "the frame end's unchanged: %s", trace);
}

/* The arguments and results each handler sees. */
static u64 seen_before[8], seen_before_x0, seen_after[8], seen_after_x0, seen_after_x9;
static void look_before(Cpu *c, const u64 args[8], void *ctx) { (void)ctx; memcpy(seen_before, args, sizeof seen_before); seen_before_x0 = c->x[0]; }
static void look_after(Cpu *c, const u64 args[8], void *ctx) {
    (void)ctx;
    memcpy(seen_after, args, sizeof seen_after);
    seen_after_x0 = c->x[0];
    seen_after_x9 = c->x[9];
}
static int ctx_ok;
static void check_ctx(Cpu *c, const u64 args[8], void *ctx) { (void)c; (void)args; ctx_ok += ctx == (void *)&ctx_ok; }
static void arguments(void) {
    forget();
    frame_hook_add(FRAME_HOOK_END, 5, look_before, look_after, NULL);
    frame_hook_add(FRAME_HOOK_END, 6, check_ctx, check_ctx, &ctx_ok);
    frame_end();
    CHECK(seen_before[0] == 0x100 && seen_before[2] == 0x102 && seen_before[7] == 0x107 && seen_before_x0 == 0x100, "before: the arguments, in args and in the context");
    CHECK(seen_after[0] == 0x100 && seen_after[2] == 0x102 && seen_after[7] == 0x107, "after: the arguments the game was called with");
    CHECK(seen_after_x0 == 0xF0044007u && seen_after_x9 == 0x99, "after: the game's results in the context");
    int same = 1;
    for (int i = 0; i < 8; i++) same &= game_saw[i] == 0x100 + (u64)i && seen_before[i] == 0x100 + (u64)i && seen_after[i] == 0x100 + (u64)i;
    CHECK(same, "the game's function called with x0-x7 as they were; both handlers given them");
    CHECK(ctx_ok == 2, "each handler given its own pointer (%d of 2)", ctx_ok);
}

/* What is refused. */
static void limits(void) {
    forget();
    for (int i = 0; i < FRAME_HOOKS_MAX; i++) CHECK(frame_hook_add(FRAME_HOOK_LINE, i, before, after, "x"), "registration %d of %d", i + 1, FRAME_HOOKS_MAX);
    CHECK(!frame_hook_add(FRAME_HOOK_LINE, 0, before, after, "x"), "one more: refused");
    CHECK(frame_hook_count(FRAME_HOOK_LINE) == FRAME_HOOKS_MAX, "the point holds %d", FRAME_HOOKS_MAX);
    CHECK(frame_hook_add(FRAME_HOOK_END, 0, before, NULL, "y"), "the other point has room");
    CHECK(!frame_hook_add(FRAME_HOOK_POINTS, 0, before, after, "x") && !frame_hook_add(-1, 0, before, after, "x"), "no such point");
    CHECK(!frame_hook_add(FRAME_HOOK_END, 0, NULL, NULL, "x"), "nothing to run");
    CHECK(frame_hook_count(FRAME_HOOK_POINTS) == 0 && frame_hook_count(FRAME_HOOK_END) == 1, "the counts");
}

/* A handler that registers another: this call keeps its list, the next has the new one. */
static void adds_one(Cpu *c, const u64 args[8], void *ctx) {
    (void)c; (void)args; (void)ctx;
    say("R<");
    frame_hook_add(FRAME_HOOK_END, 0, before, after, "N");
}
static void during(void) {
    forget();
    frame_hook_add(FRAME_HOOK_END, 10, adds_one, after, "R");
    frame_end();
    CHECK(!strcmp(trace, "R< F R>"), "the call that registered it: without it (%s)", trace);
    trace[0] = 0;
    frame_end();
    CHECK(!strcmp(trace, "N< R< F R> N>"), "the next call: with it, outside (order 0) (%s); and again from this one", trace);
    CHECK(frame_hook_count(FRAME_HOOK_END) == 3, "three registered by now (%d)", frame_hook_count(FRAME_HOOK_END));
}

/* Registrations from four threads while this one calls the frame line: every call runs a whole
 * list (as many afters as befores), and in the end all eight are there. */
static volatile long go, befores_now;
static void count_before(Cpu *c, const u64 args[8], void *ctx) { (void)c; (void)args; (void)ctx; __atomic_add_fetch(&befores_now, 1, __ATOMIC_RELAXED); }
static void count_after(Cpu *c, const u64 args[8], void *ctx) { (void)c; (void)args; (void)ctx; __atomic_sub_fetch(&befores_now, 1, __ATOMIC_RELAXED); }
static void line_quiet(Cpu *c) { (void)c; }
static void registrar(void) {
    while (!__atomic_load_n(&go, __ATOMIC_ACQUIRE)) {}
    for (int i = 0; i < 2; i++) frame_hook_add(FRAME_HOOK_LINE, i, count_before, count_after, NULL);
}
#ifdef _WIN32
static DWORD WINAPI registrar_main(LPVOID unused) { (void)unused; registrar(); return 0; }
#else
static void *registrar_main(void *unused) { (void)unused; registrar(); return NULL; }
#endif
static void threads(void) {
    forget();
#ifdef _WIN32
    HANDLE t[4];
    for (int i = 0; i < 4; i++) t[i] = CreateThread(NULL, 0, registrar_main, NULL, 0, NULL);
#else
    pthread_t t[4];
    for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, registrar_main, NULL);
#endif
    __atomic_store_n(&go, 1, __ATOMIC_RELEASE);
    long torn = 0;
    for (int k = 0; k < 200000 && (k < 1000 || frame_hook_count(FRAME_HOOK_LINE) < 8); k++) {
        called_with(0x300);
        hle_wrap__ZN7lua2cpp16L2CFighterCommon31sys_line_system_control_fighterEv(&cpu, line_quiet);
        if (befores_now) torn++;
        befores_now = 0;
    }
#ifdef _WIN32
    WaitForMultipleObjects(4, t, TRUE, INFINITE);
#else
    for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
#endif
    CHECK(!torn, "every call ran as many afters as befores (%ld did not)", torn);
    CHECK(frame_hook_count(FRAME_HOOK_LINE) == 8, "all eight registered (%d)", frame_hook_count(FRAME_HOOK_LINE));
}

int main(void) {
    nothing();
    order();
    arguments();
    limits();
    during();
    threads();
    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
