// Example mod: counts every jump squat of every fighter and, with SSBU_MOD_NOJUMP=1, forbids ground jumps.
// Build: python fighter_sdk\tools\build_mod.py fighter_sdk\examples\mod_example.cpp mods_s3     Run: SSBU_MODS=mods_s3
#include <stdio.h>
#include <stdlib.h>
#include "hook.h"

static const HookApi *api;
static HookOrig orig_jump_squat, orig_check_jump;      // static: written when lua2cpp_common is loaded
static int jump_squats;

// L2CValue L2CFighterCommon::status_JumpSquat(): x0 = the fighter's script agent, x8 = where the result goes
static void my_jump_squat(Ctx *c, void *user) {
    char line[96];
    snprintf(line, sizeof line, "[mod] jump squat number %d (agent %llx)", ++jump_squats, (unsigned long long)c->x[0]);
    api->log(line);
    orig_jump_squat(c);                                // the game's own function (or an older hook) does the work
}

// L2CValue L2CFighterCommon::sub_transition_group_check_ground_jump(): "did the fighter start a jump?"
static void my_check_jump(Ctx *c, void *user) {
    uint64_t *result = (uint64_t *)c->x[8];
    result[0] = 1; result[1] = 0;                      // L2CValue(false): type 1 = bool, value 0; the original is not called
}

extern "C" MOD_EXPORT void mod_init(const HookApi *a) {
    api = a;
    api->install_name("_ZN7lua2cpp16L2CFighterCommon16status_JumpSquatEv", my_jump_squat, nullptr, &orig_jump_squat);
    if (getenv("SSBU_MOD_NOJUMP"))
        api->install_name("_ZN7lua2cpp16L2CFighterCommon38sub_transition_group_check_ground_jumpEv", my_check_jump, nullptr, &orig_check_jump);
}
