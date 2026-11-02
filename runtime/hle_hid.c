/* nn::hid, natively: controllers.
 *
 * Players 1-8 are Pro Controllers ("full key" style) driven by the pads of hle_pad.c (any pad
 * SDL3 knows, or XInput pads without it); the console layout is positional by default, so the
 * Xbox A (bottom) is the console's B, and so on. A GameCube controller plays as one (the Gc style,
 * with its analog triggers and its motor), so the game uses its GameCube controls. The keyboard
 * adds to one player (player 1 unless controllers.ini says otherwise). Motion sensors report a
 * controller at rest; the touch screen reports no touches; rumble goes to the player's pad. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>

#include "pad.h"
#include "input_seq.h"

enum { STYLE_FULLKEY = 1u << 0, STYLE_GC = 1u << 5 };
enum { NPAD_NO1 = 0, NPAD_HANDHELD = 0x20 };
enum { ATTR_CONNECTED = 1u << 0, ATTR_WIRED = 1u << 1 };
enum {   /* nn::hid::NpadButton */
    B_A = 0, B_B, B_X, B_Y, B_STICK_L, B_STICK_R, B_L, B_R, B_ZL, B_ZR, B_PLUS, B_MINUS,
    B_LEFT, B_UP, B_RIGHT, B_DOWN, B_SL_LEFT, B_SL_UP, B_SL_RIGHT, B_SL_DOWN, B_SR_LEFT, B_SR_UP, B_SR_RIGHT, B_SR_DOWN
};

static volatile LONG64 sampling;

/* SSBU_SEQ (input_seq.h): player 1 is the sequence's - its pad is not read - and the keyboard is
 * nobody's unless SSBU_SEQ_KEYBOARD=1, so that a run goes the same way whatever is plugged in or
 * typed. Players 2-8 keep their pads. */
static Seq seq;
static int seq_on, seq_keyboard, seq_log_every;
static SeqStepsFn *volatile seq_steps;
static volatile long seq_draws = -1;   /* draw calls of the last frame shown (-1: none yet) */
enum { SEQ_MARKS = 512 };   /* (a mark per module the game loads, and a few more) */
static SRWLOCK seq_marks_lock = SRWLOCK_INIT;
static char seq_marks[SEQ_MARKS][SEQ_TEXT];
static int seq_mark_count;

static BOOL CALLBACK seq_start(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once; (void)param; (void)ctx;
    const char *text = getenv("SSBU_SEQ"), *v;
    if (!text) return TRUE;
    char why[200];
    int n = seq_parse(&seq, text, why, sizeof why);
    if (n < 0) {   /* (at start-up: main asks first, through movie_test_start) */
        fprintf(stderr, "[seq] SSBU_SEQ: %s\n", why);
        fflush(stderr);
        exit(2);
    }
    if (n == 0) {   /* (as an empty SSBU_SPEED: e.g. SSBU_SEQ="$(cat run.seq)" of a missing file) */
        fprintf(stderr, "[seq] SSBU_SEQ has no steps: no sequence, player 1 plays as usual\n");
        return TRUE;
    }
    seq_keyboard = (v = getenv("SSBU_SEQ_KEYBOARD")) && atoi(v) != 0;
    seq_log_every = (v = getenv("SSBU_SEQ_LOG")) ? atoi(v) : 0;
    fprintf(stderr, "[seq] %d steps, counted in player 1's polls; player 1's pad is not read, the keyboard is %s\n", n,
            seq_keyboard ? "read (SSBU_SEQ_KEYBOARD)" : "nobody's");
    seq_on = 1;
    return TRUE;
}

int input_seq_on(void) {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, seq_start, NULL, NULL);
    return seq_on;
}

static int seq_seen(const char *mark, void *unused) {
    (void)unused;
    int found = 0;
    AcquireSRWLockShared(&seq_marks_lock);
    for (int i = 0; i < seq_mark_count && !found; i++) found = !strcmp(seq_marks[i], mark);
    ReleaseSRWLockShared(&seq_marks_lock);
    return found;
}

void input_seq_mark(const char *what) {
    if (!input_seq_on()) return;
    int known = 0, kept = 0;
    AcquireSRWLockExclusive(&seq_marks_lock);
    for (int i = 0; i < seq_mark_count && !known; i++) known = !strcmp(seq_marks[i], what);
    if (!known && (kept = seq_mark_count < SEQ_MARKS)) snprintf(seq_marks[seq_mark_count++], sizeof seq_marks[0], "%s", what);
    ReleaseSRWLockExclusive(&seq_marks_lock);
    if (kept) fprintf(stderr, "[seq] mark %s\n", what);
    else if (!known) fprintf(stderr, "[seq] mark %s not kept: %d marks already, so a step waiting for it waits for ever\n", what, SEQ_MARKS);
}

void input_seq_set_steps(SeqStepsFn *steps_per_poll) { seq_steps = steps_per_poll; }

void input_seq_presented(u64 draws_total) {   /* (the vsync thread only) */
    static u64 last;
    seq_draws = (long)(draws_total - last);
    last = draws_total;
}

static long long seq_polls;
static void seq_log(const char *line, void *unused) { (void)unused; fprintf(stderr, "[seq] poll %lld: %s\n", seq_polls, line); }

/* One poll of player 1: the sequence moves on, and gives this poll's keys. */
static void seq_player_poll(u64 *buttons, i32 *lx, i32 *ly) {
    static SRWLOCK lock = SRWLOCK_INIT;
    static int ended;
    AcquireSRWLockExclusive(&lock);
    seq_polls++;
    long draws = seq_draws;
    seq_poll(&seq, seq_steps, draws, seq_seen, seq_log, NULL);
    long long polls = seq_polls;
    if (!ended && seq_finished(&seq)) { ended = 1; fprintf(stderr, "[seq] poll %lld: the sequence has ended\n", polls); }
    if (seq_log_every > 0 && polls % seq_log_every == 0) fprintf(stderr, "[seq] poll %lld: %ld draw calls in the last frame shown\n", polls, draws);
    *buttons |= seq.buttons;
    if (seq.lx) *lx = seq.lx;
    if (seq.ly) *ly = seq.ly;
    ReleaseSRWLockExclusive(&lock);
}

static int seq_player(u32 id) { return id == NPAD_NO1 && input_seq_on(); }
static int keyboard_slot(void) { return input_seq_on() && !seq_keyboard ? -1 : pad_keyboard_slot(); }
int bench_on(void);                         /* (hle_bench.c: a test run drives player 1 alone) */
int bench_pads(void);                       /* (... unless its play mode hands the match to the pads) */
u64 bench_buttons(i32 *lx, i32 *ly);

/* A player is connected while a pad drives it, while it is the keyboard's, and (player 1) during
 * a scripted run. */
static int pad_connected(u32 id) {
    static int scripted = -1;
    if (scripted < 0) scripted = getenv("SSBU_INPUT") != NULL || input_seq_on();
    if (id >= PAD_SLOTS) return 0;
    if (bench_on() && !bench_pads()) return id == NPAD_NO1;
    PadState p;
    return (!seq_player(id) && pad_get(id, &p)) || (int)id == keyboard_slot() || (id == NPAD_NO1 && scripted);
}

/* The player's style: a GameCube controller while its pad plays as one, else a Pro Controller. */
static u32 pad_style(u32 id) {
    if (!pad_connected(id)) return 0;
    if (bench_on() && !bench_pads()) return STYLE_FULLKEY;
    PadState p;
    return !seq_player(id) && pad_get(id, &p) && p.gc ? STYLE_GC : STYLE_FULLKEY;
}

static const char *const BUTTON_NAMES[] = {"A", "B", "X", "Y", "LSTICK", "RSTICK", "L", "R", "ZL", "ZR", "PLUS", "MINUS",
                                           "LEFT", "UP", "RIGHT", "DOWN"};

/* SSBU_INPUT="<seconds>:<button>[/<hold seconds>],...": press each button at that time, for a
 * quarter second unless a hold is given (for automated runs). SLEFT, SUP, SRIGHT and SDOWN
 * push the left stick. */
static u64 scripted_buttons(i32 *lx, i32 *ly) {
    static struct { double at, hold; int button; } presses[128];
    static int ready = -1;   /* the count, published once the list is complete (for every thread) */
    static const char *const STICK[] = {"SLEFT", "SUP", "SRIGHT", "SDOWN"};
    int count = __atomic_load_n(&ready, __ATOMIC_ACQUIRE);
    if (count < 0) {
        count = 0;
        const char *s = getenv("SSBU_INPUT");
        while (s && *s && count < 128) {
            char name[16] = {0};
            double at, hold = 0.25;
            int n = 0;
            if (sscanf(s, "%lf:%15[A-Z]%n", &at, name, &n) < 2) break;
            s += n;
            if (*s == '/') { n = 0; sscanf(s + 1, "%lf%n", &hold, &n); s += 1 + n; }
            for (int b = 0; b < (int)(sizeof BUTTON_NAMES / sizeof *BUTTON_NAMES); b++)
                if (!strcmp(name, BUTTON_NAMES[b])) { presses[count].at = at; presses[count].hold = hold; presses[count++].button = b; }
            for (int k = 0; k < 4; k++)
                if (!strcmp(name, STICK[k])) { presses[count].at = at; presses[count].hold = hold; presses[count++].button = 100 + k; }
            if (*s == ',') s++;
        }
        __atomic_store_n(&ready, count, __ATOMIC_RELEASE);
    }
    double now = rt_now_ns() / 1e9;
    u64 buttons = 0;
    for (int i = 0; i < count; i++) {
        if (now < presses[i].at || now >= presses[i].at + presses[i].hold) continue;
        switch (presses[i].button) {
        case 100: *lx = -32767; break;
        case 101: *ly = 32767; break;
        case 102: *lx = 32767; break;
        case 103: *ly = -32767; break;
        default: buttons |= 1ull << presses[i].button;
        }
    }
    return buttons;
}

/* The keyboard, while the game's window has focus: arrows move, Z/X/C/V are A/B/X/Y,
 * A/S are L/R, Q/W are ZL/ZR, Enter is + (not with Alt: Alt+Enter is the full-screen switch),
 * Backspace is -, I/J/K/L are the right stick. */
int gfx_window_focused(void);
static u64 keyboard_buttons(i32 *lx, i32 *ly, i32 *rx, i32 *ry) {
    if (!gfx_window_focused()) return 0;
    static const struct { int vk, n; } keys[] = {
        {'Z', B_A}, {'X', B_B}, {'C', B_X}, {'V', B_Y}, {'A', B_L}, {'S', B_R}, {'Q', B_ZL}, {'W', B_ZR},
        {VK_RETURN, B_PLUS}, {VK_BACK, B_MINUS},
    };
    u64 buttons = 0;
    int alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    for (u32 i = 0; i < sizeof keys / sizeof *keys; i++)
        if (GetAsyncKeyState(keys[i].vk) & 0x8000 && !(alt && keys[i].vk == VK_RETURN)) buttons |= 1ull << keys[i].n;
    if (GetAsyncKeyState(VK_LEFT) & 0x8000) *lx = -32767;
    if (GetAsyncKeyState(VK_RIGHT) & 0x8000) *lx = 32767;
    if (GetAsyncKeyState(VK_UP) & 0x8000) *ly = 32767;
    if (GetAsyncKeyState(VK_DOWN) & 0x8000) *ly = -32767;
    if (GetAsyncKeyState('J') & 0x8000) *rx = -32767;
    if (GetAsyncKeyState('L') & 0x8000) *rx = 32767;
    if (GetAsyncKeyState('I') & 0x8000) *ry = 32767;
    if (GetAsyncKeyState('K') & 0x8000) *ry = -32767;
    return buttons;
}

/* State shared by every style: { s64 sampling; u64 buttons; s32 lx, ly; s32 rx, ry; u32 attributes; u32 reserved }.
 * The attributes say connected only in the player's own style. A GameCube state goes on with
 * { u32 trigger l, trigger r (0..32767); u32 reserved }: the game presses L and R itself when a
 * trigger is past 0x27FF, whatever the buttons say, so the keyboard's L and R pull them fully. */
static void npad_state(u64 out, u32 id, u32 style) {
    u64 buttons = 0;
    i32 lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
    u32 attributes = 0;
    if (style && pad_style(id) == style) {
        attributes = ATTR_CONNECTED | ATTR_WIRED;
        PadState p;
        if (bench_on() && !bench_pads()) {
            if (id == NPAD_NO1) buttons = bench_buttons(&lx, &ly);
        } else if (seq_player(id)) seq_player_poll(&buttons, &lx, &ly);
        else if (pad_get(id, &p) && (pad_background() || gfx_window_focused())) {
            buttons = p.buttons;
            lx = p.lx; ly = p.ly; rx = p.rx; ry = p.ry;
            lt = p.lt; rt = p.rt;
        }
        if (id == NPAD_NO1 && !bench_on()) buttons |= scripted_buttons(&lx, &ly);
        if ((int)id == keyboard_slot() && (!bench_on() || bench_pads())) buttons |= keyboard_buttons(&lx, &ly, &rx, &ry);
        if (menu_blocks_input()) { buttons = 0; lx = ly = rx = ry = 0; }   /* the settings menu has them (still connected) */
        {
            /* the stick directions also exist as buttons */
            if (lx < -16384) buttons |= 1ull << B_SL_LEFT;
            if (lx > 16384) buttons |= 1ull << B_SL_RIGHT;
            if (ly > 16384) buttons |= 1ull << B_SL_UP;
            if (ly < -16384) buttons |= 1ull << B_SL_DOWN;
            if (rx < -16384) buttons |= 1ull << B_SR_LEFT;
            if (rx > 16384) buttons |= 1ull << B_SR_RIGHT;
            if (ry > 16384) buttons |= 1ull << B_SR_UP;
            if (ry < -16384) buttons |= 1ull << B_SR_DOWN;
        }
    }
    st64(out, (u64)InterlockedIncrement64(&sampling));
    st64(out + 8, buttons);
    st32(out + 16, (u32)lx); st32(out + 20, (u32)ly);
    st32(out + 24, (u32)rx); st32(out + 28, (u32)ry);
    st32(out + 32, attributes);
    st32(out + 36, 0);
    if (style == STYLE_GC) {
        if (buttons & 1ull << B_L) lt = 32767;
        if (buttons & 1ull << B_R) rt = 32767;
        st32(out + 40, (u32)lt); st32(out + 44, (u32)rt);
        st32(out + 48, 0);
    }
}

HLE(_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj) { npad_state(c->x[0], ld32(c->x[1]), STYLE_FULLKEY); }
HLE(_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj) { npad_state(c->x[0], ld32(c->x[1]), 0); }
HLE(_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj) { npad_state(c->x[0], ld32(c->x[1]), 0); }
HLE(_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj) { npad_state(c->x[0], ld32(c->x[1]), 0); }
HLE(_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj) { npad_state(c->x[0], ld32(c->x[1]), 0); }
HLE(_ZN2nn3hid12GetNpadStateEPNS0_11NpadGcStateERKj) { npad_state(c->x[0], ld32(c->x[1]), STYLE_GC); }

HLE(_ZN2nn3hid15GetNpadStyleSetERKj) { c->x[0] = pad_style(ld32(c->x[0])); }
HLE(_ZN2nn3hid19GetPlayerLedPatternERKj) {   /* the console's lights per player: 1, 12, 123, 1234, 14, 13, 134, 23 */
    static const u8 LEDS[8] = {0x1, 0x3, 0x7, 0xF, 0x9, 0x5, 0xD, 0x6};
    u32 id = ld32(c->x[0]);
    c->x[0] = id < 8 ? LEDS[id] : 0;
}
HLE(_ZN2nn3hid20GetNpadJoyAssignmentERKj) { c->x[0] = 0; }   /* dual */
HLE(_ZN2nn3hid22GetNpadControllerColorEPNS0_19NpadControllerColorERKj) {   /* { u32 main, sub } RGBA */
    st32(c->x[0], 0xFF2D2D2D); st32(c->x[0] + 4, 0xFFE6E6E6);
    c->x[0] = pad_connected(ld32(c->x[1])) ? 0 : RESULT(202, 604);
}
HLE(_ZN2nn3hid22GetNpadControllerColorEPNS0_19NpadControllerColorES2_RKj) {
    st32(c->x[0], 0xFF2D2D2D); st32(c->x[0] + 4, 0xFFE6E6E6);
    st32(c->x[1], 0xFF2D2D2D); st32(c->x[1] + 4, 0xFFE6E6E6);
    c->x[0] = pad_connected(ld32(c->x[2])) ? 0 : RESULT(202, 604);
}

/* Configuration the game declares, and assignment changes: nothing to do. */
HLE(_ZN2nn3hid14InitializeNpadEv) { (void)c; }
HLE(_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE) { (void)c; }
HLE(_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm) { (void)c; }
HLE(_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE) { (void)c; }
HLE(_ZN2nn3hid14DisconnectNpadERKj) { (void)c; }
HLE(_ZN2nn3hid18SwapNpadAssignmentERKjS2_) { (void)c; }
HLE(_ZN2nn3hid23MergeSingleJoyAsDualJoyERKjS2_) { c->x[0] = 0; }
HLE(_ZN2nn3hid28SetNpadJoyAssignmentModeDualERKj) { (void)c; }
HLE(_ZN2nn3hid30SetNpadJoyAssignmentModeSingleERKj) { (void)c; }
HLE(_ZN2nn3hid30SetNpadJoyAssignmentModeSingleERKjNS0_17NpadJoyDeviceTypeE) { (void)c; }
HLE(_ZN2nn3hid21StartLrAssignmentModeEv) { (void)c; }
HLE(_ZN2nn3hid20StopLrAssignmentModeEv) { (void)c; }

/* ---- motion: a controller lying still -------------------------------------------- */
HLE(_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE) {
    i32 n = (i32)c->x[1] < 1 ? (i32)c->x[1] : 1;
    for (i32 i = 0; i < n; i++) st32(c->x[0] + 4ull * i, ld32(c->x[2]) << 8 | 1);
    c->x[0] = (u32)n;
}
HLE(_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE) { (void)c; }
HLE(_ZN2nn3hid21IsSixAxisSensorAtRestERKNS0_19SixAxisSensorHandleE) { c->x[0] = 1; }
/* SixAxisSensorState, 0x60 bytes: deltas, sampling, acceleration, angular velocity, angle,
 * then a 3x3 orientation (identity at rest), attributes. */
HLE(_ZN2nn3hid21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE) {
    u64 s = c->x[0];
    memset(P(void, s), 0, 0x60);
    st64(s, 1);
    st64(s + 8, (u64)InterlockedIncrement64(&sampling));
    f32 one = 1.0f, down = -1.0f;
    memcpy(P(void, s + 0x18), &down, 4);   /* gravity along -z */
    memcpy(P(void, s + 0x34), &one, 4);    /* orientation: identity */
    memcpy(P(void, s + 0x44), &one, 4);
    memcpy(P(void, s + 0x54), &one, 4);
    st32(s + 0x58, 1);
}
HLE(_ZN2nn3hid28InitializeSevenSixAxisSensorEPvm) { c->x[0] = RESULT(202, 100); }
HLE(_ZN2nn3hid23StartSevenSixAxisSensorEv) { c->x[0] = 0; }
HLE(_ZN2nn3hid22StopSevenSixAxisSensorEv) { c->x[0] = 0; }
HLE(_ZN2nn3hid26IsSevenSixAxisSensorAtRestEv) { c->x[0] = 1; }
HLE(_ZN2nn3hid27GetSevenSixAxisSensorStatesEPNS0_23SevenSixAxisSensorStateEi) { c->x[0] = 0; }

/* ---- touch screen: docked, so never touched -------------------------------------- */
HLE(_ZN2nn3hid21InitializeTouchScreenEv) { (void)c; }
HLE(_ZN2nn3hid19GetTouchScreenStateILm16EEEvPNS0_16TouchScreenStateIXT_EEE) {   /* { s64 sampling; s32 count; ... } */
    st64(c->x[0], (u64)InterlockedIncrement64(&sampling));
    st32(c->x[0] + 8, 0);
}

/* ---- rumble ----------------------------------------------------------------------
 * A handle is npad id << 8 | device index; a Pro Controller's devices 0 and 1 are its left and
 * right actuators (VibrationDevicePosition 1 and 2). A GameCube controller has one device, its
 * motor (index VIB_GC_MOTOR), which the game drives with on/off commands. */
enum { VIB_GC_MOTOR = 2 };
static u8 vibration_sent[PAD_SLOTS][2][16];   /* the last VibrationValue per actuator */

HLE(_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE) {
    u32 id = ld32(c->x[2]), style = (u32)c->x[3];
    if (style & STYLE_GC && !(style & STYLE_FULLKEY)) {
        if ((i32)c->x[1] >= 1) st32(c->x[0], id << 8 | VIB_GC_MOTOR);
        c->x[0] = (i32)c->x[1] >= 1;
        return;
    }
    i32 n = (i32)c->x[1] < 2 ? (i32)c->x[1] : 2;
    for (i32 i = 0; i < n; i++) st32(c->x[0] + 4ull * i, id << 8 | (u32)i);
    c->x[0] = (u32)n;
}
HLE(_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE) { (void)c; }
HLE(_ZN2nn3hid22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE) {
    u32 handle = ld32(c->x[1]);
    if ((handle & 0xFF) == VIB_GC_MOTOR) {
        st32(c->x[0], 2);                         /* a GameCube controller's motor */
        st32(c->x[0] + 4, 0);                     /* (no side) */
        return;
    }
    st32(c->x[0], 1);                             /* linear resonant actuator */
    st32(c->x[0] + 4, (handle & 1) + 1);          /* left / right */
}
/* VibrationValue { f32 amplitude low, frequency low, amplitude high, frequency high } */
HLE(_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE) {
    u32 handle = ld32(c->x[0]), id = handle >> 8 & 0xFF, side = handle & 1;
    if (id >= PAD_SLOTS) return;
    memcpy(vibration_sent[id][side], P(void, c->x[1]), 16);
    f32 low, high;
    memcpy(&low, P(void, c->x[1]), 4);
    memcpy(&high, P(void, c->x[1] + 8), 4);
    pad_rumble(id, (int)side + 1, low > high ? low : high);
}
HLE(_ZN2nn3hid23GetActualVibrationValueEPNS0_14VibrationValueERKNS0_21VibrationDeviceHandleE) {
    u32 handle = ld32(c->x[1]), id = handle >> 8 & 0xFF;
    if (id < PAD_SLOTS) memcpy(P(void, c->x[0]), vibration_sent[id][handle & 1], 16);
    else memset(P(void, c->x[0]), 0, 16);
}
/* A GameCube pad's motor: VibrationGcErmCommand 0 stop, 1 start, 2 stop hard. */
static u32 gc_erm_sent[PAD_SLOTS];
HLE(_ZN2nn3hid25SendVibrationGcErmCommandERKNS0_21VibrationDeviceHandleERKNS0_21VibrationGcErmCommandE) {
    u32 id = ld32(c->x[0]) >> 8 & 0xFF, command = ld32(c->x[1]);
    if (id >= PAD_SLOTS) return;
    gc_erm_sent[id] = command;
    pad_rumble(id, 1, command == 1 ? 1.0f : 0.0f);
    pad_rumble(id, 2, command == 1 ? 1.0f : 0.0f);
}
HLE(_ZN2nn3hid30GetActualVibrationGcErmCommandEPNS0_21VibrationGcErmCommandERKNS0_21VibrationDeviceHandleE) {
    u32 id = ld32(c->x[1]) >> 8 & 0xFF;
    st32(c->x[0], id < PAD_SLOTS ? gc_erm_sent[id] : 0);
}
