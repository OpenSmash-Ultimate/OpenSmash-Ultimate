/* The character test bench (tests): SSBU_BENCH="<fighter>[:<costume>][,<fighter>[:<costume>]]"
 * starts a match with that fighter and costume for player 1 (and the second for the CPU,
 * mario:0 when not given), whatever the select screen picked, plays it for SSBU_BENCH_SECONDS
 * (20) and exits with a result line in the log:
 *
 *   [bench] RESULT PASS|MENUS|FREEZE-LOAD|FREEZE-MATCH <details>
 *
 * The menus are driven here (no SSBU_INPUT needed, and no pad or keyboard plays: a controller
 * can't take player 1 over): A through the title and menus as the user's save needs them, then
 * the select screen's pick, start and the stage select, again until the game sets the fighters
 * up. Which tile is picked doesn't matter: the fighter set-up (main+0x66dcf0, hle_skyline.c)
 * gets the bench's fighter and costume (bench_fighter_setup). In the match player 1 goes
 * through jabs, tilts, smashes, specials, aerials and a grab, so a moveset's scripts run.
 *
 * Failures are told apart by what was reached: MENUS (no fighter set up in SSBU_BENCH_MENU_TIMEOUT,
 * 180 s), FREEZE-LOAD (fighters set up, the match never ran in 90 s), FREEZE-MATCH (it ran, then
 * the fighters' frames stopped for 10 s). A crash ends the process with the usual report. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"

int skyline_fighter_kind(const char *name);   /* (hle_skyline.c) */

#define LOG(...) do { fprintf(stderr, "[bench] t=%.1f ", now_s()); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } while (0)

static double now_s(void) { return rt_now_ns() / 1e9; }

static int on = -1;
static int play = 0;   /* SSBU_BENCH_PLAY=1: the player's own pads play the match (the menus stay the bench's) */
static struct { char name[32]; int kind, costume; } want[2] = {{"", -1, 0}, {"mario", -1, 0}};
static double seconds = 20, menu_timeout = 180;
static volatile double setup_at = -1, match_at = -1, last_frame = -1;
static volatile long frames;
static char seen[8][48];   /* the fighters the match runs ("palutena c120") */
static u32 seen_n;

static DWORD WINAPI bench_watch(void *unused);

int bench_on(void) {
    if (on >= 0) return on;
    const char *s = getenv("SSBU_BENCH");
    on = s && *s;
    if (!on) return 0;
    for (int k = 0; k < 2 && *s; k++) {
        int n = 0;
        char name[32] = {0};
        if (sscanf(s, "%31[a-z_0-9]%n", name, &n) < 1) break;
        snprintf(want[k].name, sizeof want[k].name, "%s", name);
        s += n;
        if (*s == ':') { want[k].costume = atoi(s + 1); s++; while (*s >= '0' && *s <= '9') s++; }
        if (*s == ',') s++;
    }
    if (getenv("SSBU_BENCH_SECONDS")) seconds = atof(getenv("SSBU_BENCH_SECONDS"));
    play = getenv("SSBU_BENCH_PLAY") && atoi(getenv("SSBU_BENCH_PLAY"));
    if (getenv("SSBU_BENCH_MENU_TIMEOUT")) menu_timeout = atof(getenv("SSBU_BENCH_MENU_TIMEOUT"));
    fprintf(stderr, "[bench] %s c%02d vs %s c%02d, %.0f s of play\n", want[0].name, want[0].costume, want[1].name, want[1].costume, seconds);
    CloseHandle(CreateThread(NULL, 0, bench_watch, NULL, 0, NULL));
    return 1;
}

static void result(const char *what, const char *why) {
    char list[400] = "";
    for (u32 i = 0; i < seen_n; i++) snprintf(list + strlen(list), sizeof list - strlen(list), "%s%s", i ? ", " : "", seen[i]);
    double ran = match_at >= 0 ? last_frame - match_at : 0;
    LOG("RESULT %s %s; fighters: %s; %ld fighter frames in %.1f s (%.0f/s)", what, why, *list ? list : "(none)", frames, ran,
        ran > 0 ? frames / ran : 0.0);
    ExitProcess(0);
}

static DWORD WINAPI bench_watch(void *unused) {
    (void)unused;
    SetThreadDescription(GetCurrentThread(), L"bench");
    for (;;) {
        Sleep(250);
        double t = now_s();
        if (setup_at < 0 && t > menu_timeout) result("MENUS", "no fighter was set up: the menus weren't got through");
        if (setup_at >= 0 && match_at < 0 && t - setup_at > 90) result("FREEZE-LOAD", "the fighters were set up but the match never ran");
        if (match_at >= 0 && t - last_frame > 10) result("FREEZE-MATCH", "the fighters' frames stopped");
        if (match_at >= 0 && t - match_at >= seconds) result("PASS", "played");
    }
}

/* A fighter record the game makes a fighter from (kind at +8, costume at +0x19, entry at +0xa7):
 * the bench's fighters instead. */
void bench_fighter_setup(u64 rec) {
    int entry = *P(u8, rec + 0xa7);
    if (!bench_on() || entry > 1 || !want[entry].name[0]) return;
    if (want[entry].kind < 0) {
        want[entry].kind = skyline_fighter_kind(want[entry].name);
        if (want[entry].kind < 0) { LOG("no fighter called %s", want[entry].name); result("MENUS", "unknown fighter name"); }
    }
    LOG("set-up of entry %d: kind %d c%02u -> %s (%d) c%02d", entry, *P(i8, rec + 8), *P(u8, rec + 0x19), want[entry].name, want[entry].kind,
        want[entry].costume);
    *P(i8, rec + 8) = (i8)want[entry].kind;
    *P(u8, rec + 0x19) = (u8)want[entry].costume;
    if (setup_at < 0) setup_at = now_s();
}

/* A fighter's frame (its status line ran: hle_skyline.c), name and costume slot. */
void bench_fighter_frame(const char *name, int slot) {
    if (!bench_on()) return;
    double t = now_s();
    if (match_at < 0) { match_at = t; LOG("the match runs"); }
    last_frame = t;
    frames++;
    char d[48];
    snprintf(d, sizeof d, "%s c%02d", name, slot);
    for (u32 i = 0; i < seen_n; i++) if (!strcmp(seen[i], d)) return;
    if (seen_n < 8) { snprintf(seen[seen_n++], sizeof *seen, "%s", d); LOG("fighter %s plays", d); }
}

void bench_note(const char *event) {
    static char names[16][32];
    static int counts[16];
    if (!bench_on()) return;
    for (int i = 0; i < 16; i++) {
        if (!names[i][0]) snprintf(names[i], sizeof names[i], "%s", event);
        if (strcmp(names[i], event)) continue;
        if (counts[i]++ < 3) LOG("%s", event);
        return;
    }
}

/* ---- player 1's buttons ---- */
enum { B_A = 0, B_B, B_X, B_Y, B_L = 6, B_R, B_ZL, B_ZR, B_PLUS };
typedef struct { double at, hold; u64 buttons; int lx, ly; } Step;
#define S_UP 1
#define S_DOWN 2
#define S_LEFT 3
#define S_RIGHT 4

static void stick(int dir, i32 *lx, i32 *ly) {
    if (dir == S_UP) *ly = 32767;
    if (dir == S_DOWN) *ly = -32767;
    if (dir == S_LEFT) *lx = -32767;
    if (dir == S_RIGHT) *lx = 32767;
}

/* The menus (the user's save: title, news, Smash, the rules screen), from the start. */
static const struct { double at; int button; } MENUS[] = {
    {8, B_A}, {10, B_A}, {13, B_B}, {15, B_A}, {18, B_A}, {22, B_A}, {24.5, B_A}, {26, B_A}, {27.5, B_A}, {29, B_A}, {30.5, B_A},
};
/* One try at the select screen and the stage select, from its start: the stick up onto the
 * tiles (the first try only; far enough for a 91+ grid's smaller tiles too), pick, start, the stage. */
static const struct { double at, hold; int button, dir; } CSS[] = {
    {0, 0.55, -1, S_UP}, {1, 0.15, B_A, 0}, {2.5, 0.15, B_A, 0}, {4, 0.15, B_PLUS, 0}, {5.5, 0.15, B_PLUS, 0},
    {8, 0.15, B_A, 0}, {10, 0.15, B_A, 0}, {12, 0.15, B_A, 0},
};
#define CSS_FIRST 40.0
#define CSS_EVERY 15.0
/* In the match, over and over: (button, stick, hold) each 0.7 s. */
static const struct { int button, dir; double hold; } MOVES[] = {
    {B_A, 0, 0.1}, {B_A, 0, 0.1}, {B_A, S_RIGHT, 0.1}, {B_B, 0, 0.1}, {B_B, S_LEFT, 0.1}, {B_B, S_DOWN, 0.1},
    {B_X, 0, 0.1}, {B_A, 0, 0.1}, {B_X, 0, 0.1}, {B_A, S_DOWN, 0.1}, {B_A, S_UP, 0.1}, {-1, S_RIGHT, 0.5},
    {B_R, 0, 0.1}, {-1, S_UP, 0.2}, {B_A, S_LEFT, 0.1}, {B_B, S_UP, 0.1}, {-1, S_LEFT, 0.4}, {B_ZR, 0, 0.3},
};

/* Whether the pads play now (hle_hid.c): in play mode, once the match runs. */
int bench_pads(void) { return play && match_at >= 0; }

u64 bench_buttons(i32 *lx, i32 *ly) {
    double t = now_s();
    u64 buttons = 0;
    if (match_at >= 0) {
        double m = t - match_at;
        if (m < 1) return 0;
        int n = sizeof MOVES / sizeof *MOVES, k = (int)((m - 1) / 0.7);
        double into = (m - 1) - k * 0.7;
        k %= n;
        if (into < MOVES[k].hold) {
            if (MOVES[k].button >= 0) buttons |= 1ull << MOVES[k].button;
            stick(MOVES[k].dir, lx, ly);
        }
        return buttons;
    }
    if (setup_at >= 0) return 0;   /* (loading) */
    for (u32 i = 0; i < sizeof MENUS / sizeof *MENUS; i++)
        if (t >= MENUS[i].at && t < MENUS[i].at + 0.25) buttons |= 1ull << MENUS[i].button;
    if (t >= CSS_FIRST) {
        int attempt = (int)((t - CSS_FIRST) / CSS_EVERY);
        double into = t - CSS_FIRST - attempt * CSS_EVERY;
        for (u32 i = 0; i < sizeof CSS / sizeof *CSS; i++) {
            if (into < CSS[i].at || into >= CSS[i].at + CSS[i].hold) continue;
            if (CSS[i].button >= 0) buttons |= 1ull << CSS[i].button;
            if (CSS[i].dir && attempt == 0) stick(CSS[i].dir, lx, ly);
        }
    }
    return buttons;
}
