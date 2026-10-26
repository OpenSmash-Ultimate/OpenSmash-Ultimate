/* Fights in test runs (docs/TESTING-RUNS.md): SSBU_FIGHT_HASH, SSBU_DETERMINISTIC (SSBU_DET_OFF),
 * SSBU_FIXED_SEED, SSBU_DEBUG_MATCH, SSBU_DEBUG_MATCH_LIST and SSBU_DEBUG_END, ported from
 * ssbu-recomp (fighter_sdk/fighter_host.c, runtime/debug_mode.c; fight_test.h has their logic).
 * Each is off unless its variable is set, and the wraps here then only call the game's function;
 * so do the frame hooks', with nothing of this file registered on them.
 *
 * The fighters' frame line (lua2cpp::L2CFighterCommon::sys_line_system_control_fighter, which the
 * game runs for every fighter once a frame before its status scripts) and the frame end are
 * reached through the frame hooks (frame_hooks.h; hle_each_frame.c wraps both), which rollback
 * shares. The frame line's first call for a fighter is the first battle frame: SSBU_SEQ's mark
 * "fight", and the generators' fixed state. Calls since then are that fighter's fight frames,
 * which the hash log and the sequence's steps count.
 *
 * The wraps have no code signature yet (tools/recomp.py wrap-sig prints one, from the game).
 *
 * Not ported, as ssbu-recomp's research or online play: SSBU_FIGHT_HASH_WORK, the match seed of
 * its online rollback, SSBU_DEBUG_KEEP, _P34, _DUMP, _SCAN, _WATCH, _RESEARCH and the other dumps,
 * and the fighter kind numbers its log looked up in a bindings file. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>

#include "fight_test.h"
#include "frame_hooks.h"
#include "game_addr.h"
#include "host.h"
#include "input_seq.h"

#define FD(s) "_ZN3app8lua_bind" s "EPNS_26BattleObjectModuleAccessorE"
#define FIGHTER_MANAGER "_ZN3lib9SingletonIN3app14FighterManagerEE9instance_E"

/* ---- calls into the game ----------------------------------------------------------------- */
/* An exported getter of a fighter's module accessor, x1 an int argument, on this thread's
 * context, which is put back after; a float result through *f. 0 when no module exports it. */
static u64 getter(Cpu *c, const char *name, u64 boma, u64 x1, float *f) {
    u64 fn = rt_symbol(name);
    if (!fn) return 0;
    Cpu saved = *c;
    c->x[0] = boma;
    c->x[1] = x1;
    memset(&c->v[0], 0, sizeof c->v[0]);
    rt_call(c, fn);
    u64 r = c->x[0];
    if (f) memcpy(f, &c->v[0], 4);
    *c = saved;
    return r;
}

/* FIGHTER_INSTANCE_WORK_ID_INT_ENTRY_ID's value, as lua2cpp_common gets its constants:
 * lib::lua_bind_get_value<int> of the XXH64 of the name, seed ~0 (as ssbu-recomp read it off the
 * game). ssbu-recomp itself read it from the constant table by an index its build made. Not
 * found: every line's e is -1, as there without the table. */
static int entry_work_id(Cpu *c, u64 *id) {
    static int state;   /* 0 not asked yet, 1 found, -1 not */
    static u64 value;
    if (!state) {
        static const char NAME[] = "FIGHTER_INSTANCE_WORK_ID_INT_ENTRY_ID";
        u64 fn = rt_symbol("_ZN3lib18lua_bind_get_valueIiEEbmRT_");   /* bool (u64 key, int &) */
        state = -1;
        if (fn) {
            Cpu saved = *c;
            u64 sp = (c->sp - 0x40) & ~15ull, out = sp + 0x20;   /* (below the caller's frame) */
            st32(out, 0);
            c->sp = sp;
            c->x[0] = fight_xxh64(NAME, sizeof NAME - 1, ~0ull);
            c->x[1] = out;
            rt_call(c, fn);
            if (c->x[0] & 1) state = 1;
            value = (u64)(i64)(i32)ld32(out);
            *c = saved;
        }
        if (state > 0) fprintf(stderr, "[fight] %s is %lld: the fighters' entry ids\n", NAME, (long long)value);
        else fprintf(stderr, "[fight] %s not found (lib::lua_bind_get_value<int>%s): e is -1 on every line\n", NAME,
                     fn ? " said no" : " is not exported");
    }
    *id = value;
    return state > 0;
}

/* The game's FighterManager says a fight is on: its ready-go flag (impl + 0xd2). Any thread. */
static int ready_go(void) {
    static u64 inst;
    u64 mgr, impl, word;
    if (!inst) inst = rt_symbol(FIGHTER_MANAGER);
    return inst && kernel_read64(inst, &mgr) && mgr && kernel_read64(mgr, &impl) && impl &&
           kernel_read64(impl + 0xd0, &word) && (word >> 16 & 0xff);
}

/* [[[root]]]: the 9 script random generators (fight_test.h), or 0 before there are any. */
static u64 rng_base(void) {
    u64 p = ld64(GAME_ADDR(SCRIPT_RNG_ROOT));
    if (p) p = ld64(p);
    if (p) p = ld64(p);
    return p;
}

/* ---- SSBU_FIGHT_HASH, SSBU_DETERMINISTIC --------------------------------------------------- */
static int hash_on, det, det_off;
static FILE *hash_out;
static u64 hash_run = FIGHT_FNV_BASIS;
static struct { u64 boma; int entry; volatile long long frame; } fighters[8];
static volatile int fighter_count;
static FightDrops drops;
static FightSteps steps;

static void first_battle_frame(int entry) {
    u64 rb = rng_base();
    fprintf(stderr, "[fight] first battle frame (fighter entry %d); input mark \"fight\" raised; script random generators at %llx\n",
            entry, (unsigned long long)rb);
    if (rb) {
        fprintf(stderr, "[fight] generators before:");
        for (int i = 0; i < FIGHT_RNGS; i++) fprintf(stderr, " %08x.%u", ld32(rb + 0x14ull * i + 12), ld32(rb + 0x14ull * i + 16));
        fprintf(stderr, "\n");
    }
    if (det && !(det_off & DET_OFF_RNG) && rb) {
        u32 w[FIGHT_RNG_WORDS];
        fight_rng_fixed(w);
        for (int i = 0; i < FIGHT_RNG_WORDS; i++) st32(rb + 4ull * i, w[i]);
        fprintf(stderr, "[fight] SSBU_DETERMINISTIC: the 9 script random generators set to a fixed state, counters 0\n");
    }
    input_seq_mark("fight");
}

/* At the entry of a fighter's frame line (x0 = the agent; agent + 0x40 = its module accessor,
 * whose word + 8 has the category in its top 4 bits, 0 for a fighter). */
static void frame_line(Cpu *c) {
    u64 agent = c->x[0], boma = agent ? ld64(agent + 0x40) : 0;
    if (!boma || ld32(boma + 8) >> 28 != 0) return;
    int k = 0;
    while (k < fighter_count && fighters[k].boma != boma) k++;
    if (k == fighter_count) {
        if (k == 8) return;
        u64 id;
        fighters[k].boma = boma;
        fighters[k].frame = 0;
        fighters[k].entry = entry_work_id(c, &id) ? (int)getter(c, FD("24WorkModule__get_int_impl") "i", boma, id, NULL) : -1;
        fighter_count = k + 1;
        if (k == 0) first_battle_frame(fighters[k].entry);
    }
    FightFrame fr = {++fighters[k].frame, fighters[k].entry};
    if (!hash_out) return;
    fr.st = (int)getter(c, FD("30StatusModule__status_kind_impl"), boma, 0, NULL);
    fr.mot = getter(c, FD("30MotionModule__motion_kind_impl"), boma, 0, NULL);
    getter(c, FD("24MotionModule__frame_impl"), boma, 0, &fr.mf);
    getter(c, FD("25PostureModule__pos_x_impl"), boma, 0, &fr.x);
    getter(c, FD("25PostureModule__pos_y_impl"), boma, 0, &fr.y);
    getter(c, FD("25DamageModule__damage_impl") "i", boma, 0, &fr.dmg);
    u64 rb = rng_base();
    u32 w[FIGHT_RNG_WORDS];
    for (int i = 0; rb && i < FIGHT_RNG_WORDS; i++) w[i] = ld32(rb + 4ull * i);
    fight_rng_sum(rb ? w : NULL, &fr.rngcalls, &fr.rh);
    char line[512];   /* (room to spare: the longest line is FIGHT_LINE_MAX bytes) */
    int n = fight_hash_line(line, sizeof line, &hash_run, &fr);
    fwrite(line, 1, (size_t)n, hash_out);
    /* A file is written out every 64 fight frames; the standard output each line at once. The
     * log's other lines go to the same file there (2>&1), and on Windows the standard output is
     * not line-buffered: a line its buffer kept half of would get another line in its middle,
     * and fight_hash_diff would leave it out. */
    if (hash_out == stdout || (fr.f & 63) == 0) fflush(hash_out);
    if (k == 0 && fr.f % 300 == 0 && rb) {   /* which generators are drawn from */
        fprintf(stderr, "[fight] frame %lld: generator calls", fr.f);
        for (int i = 0; i < FIGHT_RNGS; i++) fprintf(stderr, " %u", w[5 * i + 4]);
        fprintf(stderr, "; polls that saw 0 / 1 / 2 / 3+ new fight frames: %lld %lld %lld %lld; frame ends %lld, of them %lld reported "
                "dropped refreshes (%lld in all)\n", steps.hist[0], steps.hist[1], steps.hist[2], steps.hist[3], drops.frame_ends,
                drops.calls, drops.frames);
    }
}

/* Before the frame line (registered with SSBU_FIGHT_HASH or SSBU_DETERMINISTIC). */
static void frame_line_hook(Cpu *c, const u64 args[8], void *unused) {
    (void)args; (void)unused;
    frame_line(c);
}

/* After the frame end (FRAME_END), which reports "dropped" refreshes through its third argument,
 * result 0xF0044007 / 0xF0044005 through its first (hle_each_frame.c), and the game then runs that
 * many extra logic frames on the same pad sample. When the counting thread ran is wall time, so
 * SSBU_DETERMINISTIC clears the report after the call (ssbu-recomp's reading of the game).
 * (Registered with SSBU_DETERMINISTIC.) */
static void frame_end_hook(Cpu *c, const u64 args[8], void *unused) {
    (void)c; (void)unused;
    u64 result = args[0], count = args[2];
    fight_frame_end(&drops, count ? P(u8, count) : NULL, result ? P(u8, result) : NULL, det_off & DET_OFF_DROPS);
}

/* SSBU_SEQ's ticks at each of player 1's polls: the fight frames of the first fighter since the
 * last poll, so that the sequence's place is a function of the fight frame alone (ssbu-recomp
 * measured the frame line at 510 and 605 calls in two runs over the same 462 polls). */
static int seq_steps(void) {
    if (!fighter_count) return 1;
    return (int)fight_steps(&steps, fighters[0].frame, ready_go());
}

/* ---- SSBU_FIXED_SEED -------------------------------------------------------------------- */
static int fixed_on;
static u64 fixed_state;
static SRWLOCK fixed_lock = SRWLOCK_INIT;
static long fixed_calls;

/* A clock read the game turns into a random seed: ssbu-recomp found these by their return
 * addresses, each followed by a generator's set-up. The time of day's too (hle_env.c). Each
 * one's first time is logged. */
int test_fixed_clock(u64 lr) {
    static volatile LONG seen;   /* (the game reads the tick on many threads: one of them logs it) */
    if (!fixed_on) return 0;
    const u64 at[4] = {GAME_ADDR(CLOCK_READ_1), GAME_ADDR(CLOCK_READ_2), GAME_ADDR(CLOCK_READ_3), GAME_ADDR(CLOCK_READ_4)};
    for (int i = 0; i < 4; i++)
        if (lr == at[i]) {
            if (!(InterlockedOr(&seen, 1 << i) & (1 << i)))
                fprintf(stderr, "[fight] fixed seed: the clock read at %llx gave the constant\n", (unsigned long long)lr);
            return 1;
        }
    return 0;
}
/* Random bytes from the seed's stream instead; 0 when off. The first 16 calls are logged. */
int test_fixed_random(u64 buffer, u64 size, const char *who, u64 caller) {
    if (!fixed_on) return 0;
    AcquireSRWLockExclusive(&fixed_lock);
    fixed_fill(&fixed_state, P(u8, buffer), size);
    long n = ++fixed_calls;
    ReleaseSRWLockExclusive(&fixed_lock);
    if (n <= 16) fprintf(stderr, "[fight] fixed seed: %s call %ld, %llu bytes, caller %llx\n", who, n, (unsigned long long)size,
                         (unsigned long long)caller);
    return 1;
}

HLE_WRAP_EXPORT(_ZN2nn2os13GetSystemTickEv) {
    if (test_fixed_clock(c->x[30])) { c->x[0] = FIXED_TICK; return; }
    orig(c);
}
HLE_WRAP_EXPORT(nnosGetSystemTick) {
    if (test_fixed_clock(c->x[30])) { c->x[0] = FIXED_TICK; return; }
    orig(c);
}
HLE_WRAP_EXPORT(_ZN2nn2os19GenerateRandomBytesEPvm) {
    if (test_fixed_random(c->x[0], c->x[1], "nn::os::GenerateRandomBytes", c->x[30] - 4)) return;
    orig(c);
}

/* ---- SSBU_DEBUG_MATCH, SSBU_DEBUG_MATCH_LIST --------------------------------------------- */
static DebugMatch match;
static int match_on, match_fight;
static FILE *match_list;
static u8 match_p2[DM_STATE];   /* player 2's state as changed: players 3 and 4 copy it */
static int match_p2_ok;

/* The current stage choice (three entries of 0x20 bytes, the index of the current one apart). */
static u64 stage_choice(void) {
    u32 cur = ld32(GAME_ADDR(STAGE_CHOICE_INDEX));
    return GAME_ADDR(STAGE_CHOICES) + 0x20ull * (cur < 3 ? cur : 0);
}

/* The record builder (PLAYER_RECORD_BUILDER): one player's choice on the select screens (a
 * panel state, x0) into that player's record of the match block (x3); x1 the player. */
HLE_WRAP(71017880c0) {
    if (!match_on) { f_71017880c0_orig(c); return; }
    u64 st = c->x[0], rec = c->x[3];
    int idx = (int)c->x[1];
    u64 was = ld64(st + 0x88);
    u32 type_was = ld32(st + 0x78);
    if (idx == 0 && dm_next(&match, match_list)) {
        match_fight++;
        fprintf(stderr, "[fight] SSBU_DEBUG_MATCH_LIST: match %d\n", match_fight);
    }
    type_was = dm_player(&match, idx, P(u8, st), type_was, match_p2_ok ? match_p2 : NULL);
    if (idx == 0) {
        u64 sel = stage_choice(), block = GAME_ADDR(MATCH_BLOCK);
        fprintf(stderr, "[fight] debug match: the screens' stage choice: ui_stage %llx form %u, hazards %u; rule: stock %u, item appear %u\n",
                (unsigned long long)ld64(sel), ld32(sel + 8), ld8(GAME_ADDR(STAGE_HAZARDS)), ld8(block + 0x4c), ld32(block + 0x54));
        dm_stage(&match, P(u8, sel), P(u8, GAME_ADDR(STAGE_HAZARDS)), P(u8, block + 0x54));
    }
    if (idx == 1) { memcpy(match_p2, P(u8, st), DM_STATE); match_p2_ok = 1; }
    f_71017880c0_orig(c);
    fprintf(stderr, "[fight] debug match: player %d: screen choice %llx type %u -> record: ui_chara %llx, fighter kind %d, type %u, "
            "colour %u, cpu level %u\n", idx, (unsigned long long)was, type_was, (unsigned long long)ld64(rec), (int)(i8)ld8(rec + 8),
            ld8(rec + 0xa), ld8(rec + 0x19), ld8(rec + 0x79));
}

/* ---- SSBU_DEBUG_END --------------------------------------------------------------------- */
static long long end_limit, shown;
static int fights;
static DebugEnd end;
static volatile long long end_fired, end_count;

#ifndef _WIN32
void rt_upnp_remove(void);   /* (linux/upnp.c: the netplay relay's port mapping, if it made one) */
#endif
/* As the window's close button ends the game (gpu_d3d11.c, linux/window.c, macos/window.m), the
 * hash log written out first: ExitProcess, which runs no atexit handlers or libraries' destructors
 * under the game's threads, still running mid-fight (on Linux the shim's is fflush and _exit). */
static DWORD WINAPI end_watch(LPVOID unused) {
    (void)unused;
    while (!end_fired) Sleep(5);
    fprintf(stderr, "[fight] SSBU_DEBUG_END: %lld fight frames at frame %lld shown: the run ends\n", end_count, end_fired);
    if (hash_out) fflush(hash_out);
    fflush(NULL);
#ifndef _WIN32
    rt_upnp_remove();
#endif
    ExitProcess(0);
    return 0;
}

/* The vsync thread, at each frame shown (ipc_vi.c): the fight frames SSBU_DEBUG_END counts, and
 * the debug match's line when a fight starts. */
void test_fight_presented(void) {
    if (!end_limit && !match_on) return;
    shown++;
    int ended;
    long long n = dm_end_frame(&end, ready_go(), &ended);
    if (ended) fprintf(stderr, "[fight] fight off (the game's ready-go cleared) at frame %lld shown, after %lld fight frames\n", shown, end.frames);
    if (n == 1) {
        u64 block = GAME_ADDR(MATCH_BLOCK), sel = stage_choice();
        fprintf(stderr, "[fight] fight %d on (the game's ready-go) at frame %lld shown: p1 kind %d, p2 kind %d type %u cpu power %u | stage id %x, "
                "ui_stage %llx form %u, hazards %u | rule: stock %u, item appear %u\n", ++fights, shown, (int)(i8)ld8(block + 0x588),
                (int)(i8)ld8(block + 0x588 + 0x260), ld8(block + 0x580 + 0x260 + 0xa), ld8(block + 0x580 + 0x260 + 0x79), ld32(block + 0x420),
                (unsigned long long)ld64(sel), ld32(sel + 8), ld8(GAME_ADDR(STAGE_HAZARDS)), ld8(block + 0x4c), ld32(block + 0x54));
    }
    if (end_limit && n >= end_limit && !end_fired) { end_count = n; end_fired = shown; }
}

/* ---- start --------------------------------------------------------------------------------- */
/* (main, before the game runs) */
void test_fight_start(void) {
    const char *h = getenv("SSBU_FIGHT_HASH"), *d = getenv("SSBU_DETERMINISTIC"), *o = getenv("SSBU_DET_OFF");
    const char *m = getenv("SSBU_DEBUG_MATCH"), *list = getenv("SSBU_DEBUG_MATCH_LIST");
    u64 n;
    if (m && !*m) m = NULL;   /* (set but empty: unset, as for the others) */
    if (list && !*list) list = NULL;
    det = d && d[0] && d[0] != '0';
    det_off = o ? atoi(o) : 0;
    if (h && h[0] && h[0] != '0') {
        hash_out = !strcmp(h, "1") ? stdout : fopen(h, "w");
        hash_on = hash_out != NULL;
        if (!hash_on) fprintf(stderr, "[fight] SSBU_FIGHT_HASH: cannot write %s\n", h);
    }
    if (hash_on || det) {
        fprintf(stderr, "[fight] SSBU_FIGHT_HASH %s, SSBU_DETERMINISTIC %d (off mask %d): the fighters' frame line is watched\n",
                hash_on ? "on" : "off", det, det_off);
        frame_hook_add(FRAME_HOOK_LINE, FRAME_ORDER_FIGHT_TEST, frame_line_hook, NULL, NULL);
    }
    if (det) frame_hook_add(FRAME_HOOK_END, FRAME_ORDER_FIGHT_TEST, NULL, frame_end_hook, NULL);
    if (det && !(det_off & DET_OFF_STEPS)) input_seq_set_steps(seq_steps);
    if (fixed_seed_parse(getenv("SSBU_FIXED_SEED"), &n)) {
        fixed_on = 1;
        fixed_state = fixed_stream(n);
        fprintf(stderr, "[fight] SSBU_FIXED_SEED %llu: the clock reads at %llx, %llx, %llx, %llx give a constant; random bytes from the seed\n",
                (unsigned long long)n, (unsigned long long)GAME_ADDR(CLOCK_READ_1), (unsigned long long)GAME_ADDR(CLOCK_READ_2),
                (unsigned long long)GAME_ADDR(CLOCK_READ_3), (unsigned long long)GAME_ADDR(CLOCK_READ_4));
    }
    dm_init(&match);
    if (list && !(match_list = fopen(list, "r"))) fprintf(stderr, "[fight] SSBU_DEBUG_MATCH_LIST: cannot open %s\n", list);
    if (m) dm_parse(&match, m);
    match_on = m || match_list;
    if (match_on) fprintf(stderr, "[fight] debug match: %s%s%s\n", m ? m : "", m && match_list ? ", then " : "", match_list ? list : "");
    if ((end_limit = dm_end_parse(getenv("SSBU_DEBUG_END")))) {
        fprintf(stderr, "[fight] SSBU_DEBUG_END: the run ends after %lld fight frames\n", end_limit);
        CloseHandle(CreateThread(NULL, 0, end_watch, NULL, 0, NULL));
    }
}
