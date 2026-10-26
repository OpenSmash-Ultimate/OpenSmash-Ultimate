/* Addresses in the game's main module that the test-run tools use (docs/TESTING-RUNS.md): the
 * fight hash, the deterministic mode and the debug match, ported from ssbu-recomp.
 *
 * WindowSmash builds game version 13.0.5 only (host.c game_version_ok, tools/game_version.py),
 * so 13.0.5's addresses are the ones used. Each is written beside its 13.0.4 address because it
 * comes from ssbu-recomp, which ran both versions and wrote every address as 13.0.4's: its GA()
 * gives the running build's address of a 13.0.4 one (ssbu-recomp/runtime/hostrt.c,
 * ssbu-recomp/docs/VERSIONS.md). Its rule, which game_addr_in() below follows: read-only data,
 * data and bss moved as whole segments, one page (0x1000) later in 13.0.5; code moved by
 * different amounts in different places, so a code address is known only from a table of
 * addresses checked one by one (hostrt.c's g_code_13_0_5; VERSIONS.md says how each was found
 * and checked, on ssbu-recomp's author's dump). tests/test_game_addr.py checks every row here
 * against that table and that rule, read from hostrt.c. A code address the table does not have
 * is not in this list: a guessed one would hook the wrong function.
 *
 * What each is, as ssbu-recomp found it in 13.0.4 (a hardware write watch with the guest's frame
 * chain, and Ghidra: ssbu-recomp/tests/DEBUG_MODE.md, runtime/debug_mode.c,
 * fighter_sdk/fighter_host.c, runtime/nnsdk.c):
 *   FRAME_END             the graphics module's frame end: acquires the window texture, then
 *                         compares the refreshes the game's VSync Count Thread counted (window
 *                         object + 0x138) with the count at the last frame (+ 0x140) and reports
 *                         count - (last + 1) "dropped" refreshes through its third argument
 *                         (result 0xF0044007 / 0xF0044005); the game then runs that many extra
 *                         logic frames on the same pad sample. (SSBU_DETERMINISTIC clears that.)
 *   PLAYER_RECORD_BUILDER (panel state, player index, -, record): turns one player's choice on the
 *                         select screens into that player's record in MATCH_BLOCK. State + 5 rule
 *                         kind (1 stock), + 0xa stocks, + 0x78 type (0 human, 1 CPU, 3 none),
 *                         + 0x88 ui_chara id, + 0x90 colour, + 0x91 / + 0x224 CPU level. The stage
 *                         choice is read right after the records, in the same caller.
 *   SCRIPT_RNG_ROOT       [[[it]]] = the 9 script random generators app::sv_math::rand indexes by
 *                         its Hash40 argument: 0x14 bytes each, 4 xorshift128 state words and a
 *                         call counter.
 *   MATCH_BLOCK           the global match block: the rule at + 0 (+ 0x4c stocks, + 0x54 item
 *                         appearance, + 0x420 the stage), the players' records at + 0x580, 0x260
 *                         bytes each (+ 8 fighter kind; ssbu-recomp reads 0x71052c4730 / 0x71052c4738
 *                         in 13.0.4's terms, i.e. + 0x580 / + 0x588).
 *   STAGE_CHOICES         three entries of 0x20 bytes: + 0 ui_stage id (0x69 << 56 | hash40), + 8 form
 *                         (0 normal, 1 battlefield, 2 omega), + 0xc flags, + 0x10 a second id.
 *   STAGE_CHOICE_INDEX    u32: the entry in use.
 *   STAGE_HAZARDS         u8: stage hazards on / off (ssbu-recomp wrote it; not verified there).
 *   CLOCK_READ_n          return addresses of the clock reads the game turns into random seeds
 *                         (ssbu-recomp's SSBU_FIXED_SEED answers them with a constant): compared
 *                         with a caller's link register, not called.
 * Not addresses, so not here: the FighterManager singleton (the exported symbol
 * _ZN3lib9SingletonIN3app14FighterManagerEE9instance_E; its impl + 0xd2 is the ready-go flag) and
 * the fighters' frame line (lua2cpp::L2CFighterCommon::sys_line_system_control_fighter, exported
 * by lua2cpp_common). */
#pragma once
#include <stdint.h>

#define GAME_MAIN_BASE 0x7100000000ull
#define GAME_IMAGE_SIZE 0x7500000ull    /* main with its bss: both versions end below it */
#define GAME_13_0_4_RO_DATA 0x39c8000ull   /* 13.0.4's read-only data, data and bss start here (offset in main) */
#define GAME_13_0_5_PAGE 0x1000ull      /* ... and are this much later in 13.0.5 */

/*  name                     13.0.4        13.0.5        */
#define GAME_ADDRESSES(X) \
    X(FRAME_END,             0x710386fc30, 0x71038701e0)   /* code */ \
    X(PLAYER_RECORD_BUILDER, 0x7101788260, 0x71017880c0)   /* code */ \
    X(SCRIPT_RNG_ROOT,       0x71052c25b0, 0x71052c35b0)   /* data */ \
    X(MATCH_BLOCK,           0x71052c41b0, 0x71052c51b0)   /* data */ \
    X(STAGE_CHOICES,         0x71053097c0, 0x710530a7c0)   /* data */ \
    X(STAGE_CHOICE_INDEX,    0x7105309968, 0x710530a968)   /* data */ \
    X(STAGE_HAZARDS,         0x7105309985, 0x710530a985)   /* data */ \
    X(CLOCK_READ_1,          0x7100047d20, 0x7100047d20)   /* code: a return address */ \
    X(CLOCK_READ_2,          0x7100138534, 0x7100138534)   /* code: a return address */ \
    X(CLOCK_READ_3,          0x710366d1f0, 0x710366d7a0)   /* code: a return address */ \
    X(CLOCK_READ_4,          0x710366d388, 0x710366d938)   /* code: a return address */

enum {
#define GAME_ADDR_INDEX(name, at_13_0_4, at_13_0_5) GA_##name,
    GAME_ADDRESSES(GAME_ADDR_INDEX)
#undef GAME_ADDR_INDEX
    GA_COUNT
};

typedef struct GameAddress { const char *name; uint64_t at_13_0_4, at_13_0_5; } GameAddress;
static const GameAddress game_addresses[GA_COUNT] = {
#define GAME_ADDR_ROW(name, at_13_0_4, at_13_0_5) {#name, at_13_0_4##ull, at_13_0_5##ull},
    GAME_ADDRESSES(GAME_ADDR_ROW)
#undef GAME_ADDR_ROW
};

/* The address of one of the above in the build WindowSmash runs: GAME_ADDR(FRAME_END). */
#define GAME_ADDR(name) (game_addresses[GA_##name].at_13_0_5)

enum { GAME_13_0_4, GAME_13_0_5 };
#define GAME_BUILD GAME_13_0_5   /* the one WindowSmash builds (tools/game_version.py) */

/* In build `build`, the address of what is at `a` in 13.0.4 (ssbu-recomp's GA()); 0 when that is
 * not known: a code address that is not in the list above. Outside main nothing moved. WindowSmash
 * runs 13.0.5 only; 13.0.4 maps every address to itself, should it ever take that build too. */
static inline uint64_t game_addr_in(int build, uint64_t a) {
    if (build == GAME_13_0_4 || a < GAME_MAIN_BASE || a - GAME_MAIN_BASE >= GAME_IMAGE_SIZE) return a;
    if (a - GAME_MAIN_BASE >= GAME_13_0_4_RO_DATA) return a + GAME_13_0_5_PAGE;
    for (int i = 0; i < GA_COUNT; i++)
        if (game_addresses[i].at_13_0_4 == a) return game_addresses[i].at_13_0_5;
    return 0;
}
/* The same in the build WindowSmash runs: for porting ssbu-recomp's GA(0x71...) as it is written. */
static inline uint64_t game_addr(uint64_t at_13_0_4) { return game_addr_in(GAME_BUILD, at_13_0_4); }
