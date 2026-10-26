/* runtime/hle_fight_test.c on a made-up game (docs/TESTING-RUNS.md): its wraps and hooks called
 * as the game would call them (the frame line's and the frame end's through runtime/hle_each_frame.c,
 * the frame hooks' wraps), with the runtime's calls stubbed here and made-up guest memory
 * mapped at the game addresses it reads (game_addr.h), as the runtime maps the game. The getters
 * answer from made-up fighters. macOS and Linux (the runtime's POSIX headers); not Windows.
 *
 *   clang -pthread -I runtime/macos/include -I runtime/linux/include -I runtime tools/dev/fightruntest.c \
 *         runtime/hle_fight_test.c runtime/hle_each_frame.c -o fightruntest && ./fightruntest unset
 *
 * One scenario a run, its switches set by the caller (tests/test_fight_test.py): unset, hash,
 * stdout, det, off, match, list, seed, race, end. Prints "<n> checks, 0 failed" (and the numbers the test works
 * out on its own) and exits 0; "end" exits through SSBU_DEBUG_END's own exit. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "host.h"
#include "game_addr.h"
#include "fight_test.h"
#include "input_seq.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- made-up guest memory: the game's data around the addresses read, then made-up objects -- */
#define GUEST 0x71052c0000ull
#define GUEST_SIZE 0x100000ull
#define OBJ (GUEST + 0x80000)
#define AGENT(i) (OBJ + 0x1000ull * (i))          /* 0, 1 fighters; 2 a weapon */
#define BOMA(i) (OBJ + 0x10000ull + 0x1000ull * (i))
#define RNG (OBJ + 0x20000)
#define RNG_P1 (OBJ + 0x21000)
#define RNG_P2 (OBJ + 0x21010)
#define MANAGER (OBJ + 0x22000)                   /* the FighterManager singleton's instance_ */
#define MANAGER_IMPL (OBJ + 0x22100)
#define REPORT_RESULT (OBJ + 0x23000)
#define REPORT_COUNT (OBJ + 0x23010)
#define STATE(i) (OBJ + 0x24000 + 0x400ull * (i))
#define RECORD(i) (OBJ + 0x26000 + 0x100ull * (i))
#define BUFFER (OBJ + 0x27000)
#define STACK_TOP (OBJ + 0x30000)
/* a fighter's made-up module accessor: what the getters answer */
enum { B_STATUS = 0x100, B_MOTION = 0x108, B_FRAME = 0x110, B_X = 0x114, B_Y = 0x118, B_DAMAGE = 0x11c, B_ENTRY = 0x120 };

static void map_guest(void) {
    void *want = RT_HOST(GUEST);
    void *got = mmap(want, GUEST_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (got != want) { printf("cannot map the made-up guest memory at %p (got %p)\n", want, got); exit(3); }
}
static void stf(u64 a, float f) { memcpy(RT_HOST(a), &f, 4); }

static void set_ready_go(int on) { st8(MANAGER_IMPL + 0xd2, (u8)on); }

static void make_game(void) {
    st64(GAME_ADDR(SCRIPT_RNG_ROOT), RNG_P1);
    st64(RNG_P1, RNG_P2);
    st64(RNG_P2, RNG);
    for (int i = 0; i < FIGHT_RNG_WORDS; i++) st32(RNG + 4ull * i, 0x01010101u * (u32)i + 7);
    st64(MANAGER, OBJ + 0x22010);
    st64(OBJ + 0x22010, MANAGER_IMPL);
    for (int i = 0; i < 3; i++) {
        st64(AGENT(i) + 0x40, BOMA(i));
        st32(BOMA(i) + 8, i == 2 ? 0x10000005u : 0x00000003u);   /* category in the top 4 bits: 0 a fighter */
        st32(BOMA(i) + B_ENTRY, (u32)(i == 1 ? 1 : 0));
    }
}
/* the made-up fighter i's state at fight frame f */
static void fighter_frame(int i, int f) {
    u64 b = BOMA(i);
    st32(b + B_STATUS, (u32)(0x10 * i + f));
    st64(b + B_MOTION, 0x1000000000ull * (u64)(i + 1) + (u64)f);
    stf(b + B_FRAME, 0.5f * (float)f);
    stf(b + B_X, -10.0f + 1.25f * (float)(f + i));
    stf(b + B_Y, 0.75f * (float)i);
    stf(b + B_DAMAGE, 3.5f * (float)f);
    st32(RNG + 4ull * (5 * i + 4), (u32)(100 * i + f));   /* generator i's call count */
}

/* ---- the runtime, stubbed ------------------------------------------------------------------ */
#define FD(s) "_ZN3app8lua_bind" s "EPNS_26BattleObjectModuleAccessorE"
static const struct { const char *name; u64 addr; } SYMBOLS[] = {
    {FD("30StatusModule__status_kind_impl"), NATIVE_BASE + 0x10},
    {FD("30MotionModule__motion_kind_impl"), NATIVE_BASE + 0x20},
    {FD("24MotionModule__frame_impl"), NATIVE_BASE + 0x30},
    {FD("25PostureModule__pos_x_impl"), NATIVE_BASE + 0x40},
    {FD("25PostureModule__pos_y_impl"), NATIVE_BASE + 0x50},
    {FD("25DamageModule__damage_impl") "i", NATIVE_BASE + 0x60},
    {FD("24WorkModule__get_int_impl") "i", NATIVE_BASE + 0x70},
    {"_ZN3lib18lua_bind_get_valueIiEEbmRT_", NATIVE_BASE + 0x80},
    {"_ZN3lib9SingletonIN3app14FighterManagerEE9instance_E", MANAGER},
};
u64 rt_symbol(const char *name) {
    for (unsigned i = 0; i < sizeof SYMBOLS / sizeof *SYMBOLS; i++)
        if (!strcmp(SYMBOLS[i].name, name)) return SYMBOLS[i].addr;
    return 0;
}
#define ENTRY_CONSTANT 0x1234
static int calls_made, key_asked;
static u64 key_seen;
void rt_call(Cpu *c, u64 a) {
    u64 b = c->x[0];
    calls_made++;
    c->x[19] = 0xdead;   /* (what a call may leave in the registers: the wrapper puts them back) */
    c->x[30] = 0;
    switch (a - NATIVE_BASE) {
    case 0x10: c->x[0] = ld32(b + B_STATUS); break;
    case 0x20: c->x[0] = ld64(b + B_MOTION); break;
    case 0x30: c->v[0].s[0] = ld32(b + B_FRAME); break;
    case 0x40: c->v[0].s[0] = ld32(b + B_X); break;
    case 0x50: c->v[0].s[0] = ld32(b + B_Y); break;
    case 0x60: c->v[0].s[0] = c->x[1] == 0 ? ld32(b + B_DAMAGE) : 0; break;
    case 0x70: c->x[0] = c->x[1] == ENTRY_CONSTANT ? ld32(b + B_ENTRY) : 0xbad; break;
    case 0x80:   /* lib::lua_bind_get_value<int>(key, int &): the out slot on the guest's stack */
        key_asked++;
        key_seen = c->x[0];
        CHECK(c->x[1] < STACK_TOP && c->x[1] >= c->sp && (c->sp & 15) == 0, "the out slot is below the caller's stack, above the call's");
        st32(c->x[1], ENTRY_CONSTANT);
        c->x[0] = 1;
        break;
    default: printf("FAIL: a call to %llx\n", (unsigned long long)a); exit(4);
    }
}
int kernel_read64(u64 addr, u64 *out) {
    if (addr < GUEST || addr + 8 > GUEST + GUEST_SIZE) return 0;
    *out = ld64(addr);
    return 1;
}
static char marks[8][SEQ_TEXT];
static int mark_count;
void input_seq_mark(const char *what) { if (mark_count < 8) snprintf(marks[mark_count++], SEQ_TEXT, "%s", what); }
static SeqStepsFn *steps_fn;
static int steps_set;
void input_seq_set_steps(SeqStepsFn *fn) { steps_fn = fn; steps_set++; }
static LPTHREAD_START_ROUTINE thread_fn;
static int threads;
HANDLE CreateThread(void *security, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID param, DWORD flags, DWORD *id) {
    (void)security; (void)stack; (void)param; (void)flags; (void)id;
    thread_fn = fn;
    threads++;
    return (HANDLE)(uintptr_t)1;
}
BOOL CloseHandle(HANDLE h) { (void)h; return 1; }
static void (*sleep_tick)(void);
void Sleep(DWORD ms) { (void)ms; if (sleep_tick) sleep_tick(); }
void rt_upnp_remove(void) { fprintf(stderr, "(the relay's port mapping removed)\n"); }
void ExitProcess(UINT code) { fflush(NULL); _exit((int)code); }

/* the game's own functions behind the wraps */
static int orig_calls;
static u64 orig_x0, orig_x19, orig_x30;
static void orig_saw(Cpu *c) { orig_calls++; orig_x0 = c->x[0]; orig_x19 = c->x[19]; orig_x30 = c->x[30]; }
static void frame_line_orig(Cpu *c) { orig_saw(c); }
void f_71038701e0_orig(Cpu *c) { orig_saw(c); }   /* the frame end leaves a report in its arguments */
void f_71017880c0_orig(Cpu *c) {                    /* the record builder: copies a few fields */
    orig_saw(c);
    u64 st = c->x[0], rec = c->x[3];
    st64(rec, ld64(st + 0x88));
    st8(rec + 8, (u8)c->x[1]);
    st8(rec + 0xa, (u8)ld32(st + 0x78));
    st8(rec + 0x19, ld8(st + 0x90));
    st8(rec + 0x79, ld8(st + 0x91));
    c->x[30] = 0x55;
}
static void tick_orig(Cpu *c) { orig_saw(c); c->x[0] = 777; }
static void random_orig(Cpu *c) { orig_saw(c); memset(RT_HOST(c->x[0]), 0xab, c->x[1]); }

void test_fight_start(void);
void test_fight_presented(void);
int test_fixed_clock(u64 lr);
int test_fixed_random(u64 buffer, u64 size, const char *who, u64 caller);
void hle_wrap__ZN7lua2cpp16L2CFighterCommon31sys_line_system_control_fighterEv(Cpu *c, GuestFn *orig);
void hle_wrap__ZN2nn2os13GetSystemTickEv(Cpu *c, GuestFn *orig);
void hle_wrap_nnosGetSystemTick(Cpu *c, GuestFn *orig);
void hle_wrap__ZN2nn2os19GenerateRandomBytesEPvm(Cpu *c, GuestFn *orig);
void hle_wrap_71038701e0(Cpu *c);
void hle_wrap_71017880c0(Cpu *c);

static Cpu cpu;
/* The game runs agent i's frame line: what the game's function then saw. */
static void frame_line(int i) {
    memset(&cpu, 0, sizeof cpu);
    cpu.x[0] = AGENT(i);
    cpu.x[19] = 19;
    cpu.x[30] = 0x7100001234ull;
    cpu.sp = STACK_TOP;
    int before = orig_calls;
    hle_wrap__ZN7lua2cpp16L2CFighterCommon31sys_line_system_control_fighterEv(&cpu, frame_line_orig);
    CHECK(orig_calls == before + 1 && orig_x0 == AGENT(i) && orig_x19 == 19 && orig_x30 == 0x7100001234ull && cpu.sp == STACK_TOP,
          "agent %d: the game's frame line ran once, with the registers it was called with", i);
}
/* The frame end, its report: count dropped refreshes, result code. */
static void frame_end(u64 count, u32 result) {
    st64(REPORT_COUNT, count);
    st32(REPORT_RESULT, result);
    memset(&cpu, 0, sizeof cpu);
    cpu.x[0] = REPORT_RESULT;
    cpu.x[2] = REPORT_COUNT;
    hle_wrap_71038701e0(&cpu);
}
static void record(int idx) {
    memset(&cpu, 0, sizeof cpu);
    cpu.x[0] = STATE(idx);
    cpu.x[1] = (u64)idx;
    cpu.x[3] = RECORD(idx);
    cpu.x[30] = 0x7100004321ull;
    hle_wrap_71017880c0(&cpu);
    CHECK(orig_x0 == STATE(idx) && orig_x30 == 0x7100004321ull && cpu.x[30] == 0x55, "player %d: the record builder ran on the state, as called", idx);
}
static u32 rng_words_hash(void) {
    u32 h = 0;
    for (int i = 0; i < FIGHT_RNG_WORDS; i++) h = h * 31 + ld32(RNG + 4ull * i);
    return h;
}
static void make_states(void) {
    for (int i = 0; i < 4; i++) {
        memset(RT_HOST(STATE(i)), 0, DM_STATE);
        st8(STATE(i) + 5, 1);
        st8(STATE(i) + 0xa, 3);
        st64(STATE(i) + 0x88, 0xc1000000000000aaull + (u64)i);
        st32(STATE(i) + 0x78, i == 0 ? 0 : i == 1 ? 1 : 3);
    }
    st32(GAME_ADDR(STAGE_CHOICE_INDEX), 1);
    st64(GAME_ADDR(STAGE_CHOICES) + 0x20, 0x69000000000000bbull);
    st32(GAME_ADDR(STAGE_CHOICES) + 0x28, 0);
    st8(GAME_ADDR(STAGE_HAZARDS), 1);
    st32(GAME_ADDR(MATCH_BLOCK) + 0x54, 7);
}
static void print_state(int i) {
    printf("state %d chara %016llx type %u colour %u level %u stocks %u x224 %08x\n", i, (unsigned long long)ld64(STATE(i) + 0x88),
           ld32(STATE(i) + 0x78), ld8(STATE(i) + 0x90), ld8(STATE(i) + 0x91), ld8(STATE(i) + 0xa), ld32(STATE(i) + 0x224));
}
static void print_stage(void) {
    u64 sel = GAME_ADDR(STAGE_CHOICES) + 0x20;
    printf("stage %016llx form %u hazards %u items %u\n", (unsigned long long)ld64(sel), ld32(sel + 8), ld8(GAME_ADDR(STAGE_HAZARDS)),
           ld32(GAME_ADDR(MATCH_BLOCK) + 0x54));
}

/* ---- the scenarios ------------------------------------------------------------------------- */
/* Nothing set: every wrap only calls the game's function, and nothing else is touched. */
static void unset(void) {
    test_fight_start();
    CHECK(!threads && !steps_set, "no thread, no steps hook");
    for (int i = 0; i < 2; i++) fighter_frame(i, 1);
    u32 rng = rng_words_hash();
    for (int f = 1; f <= 3; f++) for (int i = 0; i < 3; i++) frame_line(i);
    CHECK(calls_made == 0 && mark_count == 0 && rng_words_hash() == rng, "the frame line: no call into the game, no mark, the generators left");
    frame_end(3, 0xF0044007u);
    CHECK(ld64(REPORT_COUNT) == 3 && ld32(REPORT_RESULT) == 0xF0044007u, "the frame end: the report left");
    make_states();
    u8 before[DM_STATE];
    memcpy(before, RT_HOST(STATE(0)), DM_STATE);
    record(0);
    CHECK(!memcmp(before, RT_HOST(STATE(0)), DM_STATE) && ld64(GAME_ADDR(STAGE_CHOICES) + 0x20) == 0x69000000000000bbull &&
          ld8(GAME_ADDR(STAGE_HAZARDS)) == 1 && ld32(GAME_ADDR(MATCH_BLOCK) + 0x54) == 7, "the record builder: no change");
    memset(&cpu, 0, sizeof cpu);
    cpu.x[30] = GAME_ADDR(CLOCK_READ_1);
    hle_wrap__ZN2nn2os13GetSystemTickEv(&cpu, tick_orig);
    CHECK(cpu.x[0] == 777, "GetSystemTick: the SDK's own, even at a seed's clock read");
    cpu.x[30] = GAME_ADDR(CLOCK_READ_2);
    hle_wrap_nnosGetSystemTick(&cpu, tick_orig);
    CHECK(cpu.x[0] == 777, "nnosGetSystemTick: the same");
    memset(&cpu, 0, sizeof cpu);
    cpu.x[0] = BUFFER;
    cpu.x[1] = 24;
    hle_wrap__ZN2nn2os19GenerateRandomBytesEPvm(&cpu, random_orig);
    CHECK(ld64(BUFFER) == 0xababababababababull && ld64(BUFFER + 16) == 0xababababababababull, "GenerateRandomBytes: the SDK's own");
    CHECK(!test_fixed_clock(GAME_ADDR(CLOCK_READ_3)) && !test_fixed_random(BUFFER, 8, "test", 0), "the clock and random hooks: off");
    set_ready_go(1);
    for (int i = 0; i < 5; i++) test_fight_presented();
    CHECK(threads == 0 && mark_count == 0, "frames shown: nothing");
}

/* SSBU_FIGHT_HASH=<file>: the lines (the test works them out), the mark, the generators left. */
static void hash(void) {
    test_fight_start();
    CHECK(!steps_set, "no steps hook without SSBU_DETERMINISTIC");
    for (int f = 1; f <= 3; f++) {
        fighter_frame(0, f);
        fighter_frame(1, f);
        frame_line(0);
        frame_line(2);   /* a weapon's: not a fighter */
        frame_line(1);
    }
    CHECK(mark_count == 1 && !strcmp(marks[0], "fight"), "the mark \"fight\", once");
    CHECK(ld32(RNG) == 7 && ld32(RNG + 4) == 0x01010108u && ld32(RNG + 0x14) == 0x0505050cu, "the generators' state left as it was");
    CHECK(key_asked == 1, "the entry constant asked for once: %d", key_asked);
    CHECK(!threads, "no thread");
    printf("key %016llx\n", (unsigned long long)key_seen);
    for (int i = 0; i < FIGHT_RNG_WORDS; i++) printf("%s%08x", i ? " " : "rng ", ld32(RNG + 4ull * i));
    printf("\n");
    fflush(NULL);
}

/* SSBU_FIGHT_HASH=1 with the standard output fully buffered, as Windows has it written to a file
 * (here a buffer shorter than a line, so that each line overflows it), and a line of the log on
 * the standard error after each hash line (the caller sends both to one pipe, as 2>&1): each hash
 * line is out whole before the log's next line. */
static void hash_stdout(void) {
    static char buffer[64];
    setvbuf(stdout, buffer, _IOFBF, sizeof buffer);
    test_fight_start();
    for (int f = 1; f <= 3; f++) {
        fighter_frame(0, f);
        fighter_frame(1, f);
        for (int i = 0; i < 2; i++) {
            frame_line(i);
            fprintf(stderr, "[display] speed: a line of the log after a hash line\n");
        }
    }
}

/* SSBU_DETERMINISTIC=1: the generators' fixed state, the report cleared, the steps. */
static void det(void) {
    test_fight_start();
    CHECK(steps_set == 1 && steps_fn, "the steps hook");
    CHECK(steps_fn() == 1 && steps_fn() == 1, "no fighter yet: one tick a poll");
    fighter_frame(0, 1);
    u32 rng = rng_words_hash();
    frame_line(2);
    CHECK(rng_words_hash() == rng && !mark_count, "a weapon's frame line: not a battle frame");
    frame_line(0);
    u32 want[FIGHT_RNG_WORDS];
    fight_rng_fixed(want);
    int same = 1;
    for (int i = 0; i < FIGHT_RNG_WORDS; i++) same &= ld32(RNG + 4ull * i) == want[i];
    CHECK(same, "the first battle frame: the generators' fixed state");
    CHECK(mark_count == 1 && !strcmp(marks[0], "fight"), "the mark \"fight\"");
    CHECK(calls_made == 2, "the entry id asked for (the constant and WorkModule::get_int), no getter without the hash: %d calls", calls_made);
    for (int i = 0; i < FIGHT_RNG_WORDS; i++) st32(RNG + 4ull * i, 5);
    frame_line(1);
    CHECK(ld32(RNG) == 5, "a second fighter's first frame: no second reset");
    /* the steps across a fight: the first fighter's frames, the ready-go flag */
    CHECK(steps_fn() == 1, "the first battle frame, before go: 1 tick");
    frame_line(0); frame_line(0);
    set_ready_go(1);
    CHECK(steps_fn() == 2, "go, two fight frames since: 2 ticks");
    CHECK(steps_fn() == 0, "no fight frame: no tick");
    frame_line(0);
    CHECK(steps_fn() == 1, "one: one");
    set_ready_go(0);
    frame_line(0); frame_line(0); frame_line(0);
    CHECK(steps_fn() == 1 && steps_fn() == 1, "the fight over: one a poll again");
    set_ready_go(1);
    frame_line(0); frame_line(0);
    CHECK(steps_fn() == 2, "the next fight: fight frames again");
    /* the report of dropped refreshes */
    frame_end(3, 0xF0044007u);
    CHECK(ld64(REPORT_COUNT) == 0 && ld32(REPORT_RESULT) == 0, "3 dropped refreshes: cleared, and the result");
    frame_end(1, 0xF0044005u);
    CHECK(ld64(REPORT_COUNT) == 0 && ld32(REPORT_RESULT) == 0, "the other result code");
    frame_end(2, 0x77u);
    CHECK(ld64(REPORT_COUNT) == 0 && ld32(REPORT_RESULT) == 0x77u, "another result: the count cleared, the result left");
    frame_end(0, 0xF0044007u);
    CHECK(ld32(REPORT_RESULT) == 0xF0044007u, "none dropped: left");
}

/* SSBU_DETERMINISTIC=1 SSBU_DET_OFF=7: none of the three. */
static void off(void) {
    test_fight_start();
    CHECK(!steps_set, "SSBU_DET_OFF=2: polls are counted, no steps hook");
    fighter_frame(0, 1);
    u32 rng = rng_words_hash();
    frame_line(0);
    CHECK(rng_words_hash() == rng && mark_count == 1, "SSBU_DET_OFF=1: the generators left; the mark still raised");
    frame_end(3, 0xF0044007u);
    CHECK(ld64(REPORT_COUNT) == 3 && ld32(REPORT_RESULT) == 0xF0044007u, "SSBU_DET_OFF=4: the report left");
}

/* SSBU_DEBUG_MATCH: the panel states and the stage choice as the test works them out. */
static void match(void) {
    test_fight_start();
    make_states();
    record(0);
    print_stage();
    record(1);
    record(2);
    record(3);
    for (int i = 0; i < 4; i++) print_state(i);
    CHECK(ld8(RECORD(2) + 0xa) == 1 && ld64(RECORD(2)) == ld64(STATE(2) + 0x88), "the game's function built player 3's record from the state as changed");
    set_ready_go(0);
    test_fight_presented();
    set_ready_go(1);
    test_fight_presented();
    test_fight_presented();
    CHECK(!threads, "no thread without SSBU_DEBUG_END");
}

/* SSBU_DEBUG_MATCH_LIST: a match set-up (player 1's record) a line. */
static void list(void) {
    test_fight_start();
    for (int setup = 0; setup < 4; setup++) {
        make_states();
        record(0);
        print_state(0);
        print_stage();
    }
}

/* SSBU_FIXED_SEED: the seed's clock reads and random bytes. */
static void seed(void) {
    test_fight_start();
    CHECK(test_fixed_clock(GAME_ADDR(CLOCK_READ_1)) && test_fixed_clock(GAME_ADDR(CLOCK_READ_2)) &&
          test_fixed_clock(GAME_ADDR(CLOCK_READ_3)) && test_fixed_clock(GAME_ADDR(CLOCK_READ_4)), "the four clock reads");
    CHECK(!test_fixed_clock(GAME_ADDR(CLOCK_READ_1) + 4) && !test_fixed_clock(0), "no other");
    memset(&cpu, 0, sizeof cpu);
    int before = orig_calls;
    cpu.x[30] = GAME_ADDR(CLOCK_READ_4);
    hle_wrap__ZN2nn2os13GetSystemTickEv(&cpu, tick_orig);
    CHECK(cpu.x[0] == FIXED_TICK && orig_calls == before, "GetSystemTick at a seed's clock read: the constant");
    cpu.x[30] = GAME_ADDR(CLOCK_READ_2);
    hle_wrap_nnosGetSystemTick(&cpu, tick_orig);
    CHECK(cpu.x[0] == FIXED_TICK && orig_calls == before, "nnosGetSystemTick too");
    cpu.x[30] = 0x7100000010ull;
    hle_wrap__ZN2nn2os13GetSystemTickEv(&cpu, tick_orig);
    CHECK(cpu.x[0] == 777 && orig_calls == before + 1, "elsewhere: the clock");
    memset(&cpu, 0, sizeof cpu);
    cpu.x[0] = BUFFER;
    cpu.x[1] = 21;
    cpu.x[30] = 0x7100000020ull;
    hle_wrap__ZN2nn2os19GenerateRandomBytesEPvm(&cpu, random_orig);
    CHECK(orig_calls == before + 1, "GenerateRandomBytes: the seed's stream, not the SDK's");
    CHECK(test_fixed_random(BUFFER + 21, 3, "test", 0), "the stream goes on (nn::crypto's)");
    printf("bytes ");
    for (int i = 0; i < 24; i++) printf("%02x", ld8(BUFFER + (u64)i));
    printf("\n");
}

/* SSBU_FIXED_SEED: the four clock reads from many threads at once, as the game reads the tick:
 * the log has each one's first time once. */
enum { RACERS = 16 };
static volatile int racers_ready, race_go;
static void *racer(void *unused) {
    (void)unused;
    __atomic_add_fetch(&racers_ready, 1, __ATOMIC_SEQ_CST);
    while (!__atomic_load_n(&race_go, __ATOMIC_ACQUIRE)) {}
    int said = 1;
    for (int k = 0; k < 4; k++) said &= test_fixed_clock(k == 0 ? GAME_ADDR(CLOCK_READ_1) : k == 1 ? GAME_ADDR(CLOCK_READ_2) :
                                                         k == 2 ? GAME_ADDR(CLOCK_READ_3) : GAME_ADDR(CLOCK_READ_4));
    return said ? racer : NULL;
}
static void race(void) {
    test_fight_start();
    pthread_t t[RACERS];
    for (int i = 0; i < RACERS; i++) CHECK(!pthread_create(&t[i], NULL, racer, NULL), "thread %d", i);
    while (__atomic_load_n(&racers_ready, __ATOMIC_ACQUIRE) < RACERS) {}
    __atomic_store_n(&race_go, 1, __ATOMIC_RELEASE);
    int all = 1;
    for (int i = 0; i < RACERS; i++) {
        void *r = NULL;
        pthread_join(t[i], &r);
        all &= r == racer;
    }
    CHECK(all, "every thread's four clock reads: the constant");
}

/* SSBU_DEBUG_END: frames shown come from the exit's own waits; it ends the run. */
static int shown_now;
static void end_tick(void) {
    shown_now++;
    /* frames 1-2 before the fight; a fight from 3 to 52; 53-55 between; the next from 56 on */
    set_ready_go((shown_now >= 3 && shown_now <= 52) || shown_now >= 56);
    if (shown_now < 20) { fighter_frame(0, shown_now); frame_line(0); }   /* (a few hash lines, short of a flush) */
    test_fight_presented();
    if (shown_now > 1000) { printf("FAIL: no end after 1000 frames shown\n"); exit(5); }
}
/* (the end is the window close's, ExitProcess: no atexit handler runs under the game's threads) */
static void at_exit(void) { printf("FAIL: an atexit handler ran at the end\n"); }
static void end_run(void) {
    atexit(at_exit);
    test_fight_start();
    CHECK(threads == 1 && thread_fn, "the exit's thread");
    printf("%d checks, %d failed\n", checks, failures);
    fflush(stdout);
    if (failures) exit(1);
    sleep_tick = end_tick;
    thread_fn(NULL);
    printf("FAIL: the run went on\n");
    exit(6);
}

int main(int argc, char **argv) {
    static const struct { const char *name; void (*run)(void); } SCENARIOS[] = {
        {"unset", unset}, {"hash", hash}, {"stdout", hash_stdout}, {"det", det}, {"off", off}, {"match", match}, {"list", list},
        {"seed", seed}, {"race", race}, {"end", end_run},
    };
    map_guest();
    make_game();
    for (unsigned i = 0; i < sizeof SCENARIOS / sizeof *SCENARIOS; i++)
        if (argc > 1 && !strcmp(argv[1], SCENARIOS[i].name)) {
            SCENARIOS[i].run();
            printf("%d checks, %d failed\n", checks, failures);
            return failures != 0;
        }
    printf("usage: fightruntest unset|hash|stdout|det|off|match|list|seed|race|end\n");
    return 2;
}
