/* Rollback netplay's wraps of game functions (SSBU_RB, hle_rb_core.c), apart so that they build
 * on every system: each is in the generated C of every build, wherever rollback runs. With
 * SSBU_RB unset each only calls the game's function (rb_lite_skip says no; the others call it
 * at once); on systems rollback is not written for (not Windows) hle_rb_core.c's stand-ins do the
 * same. The 13.0.5 addresses and what each function is are in hle_rb_core.c, by its function.
 *
 * The frame end and the player record builder are not here: they are the frame hooks' wraps
 * (hle_each_frame.c), which hle_rb_core.c registers on. */
#include "host.h"

int rb_lite_skip(Cpu *c, int group);                    /* a re-run frame leaves this presentation group out */
void rb_job_kick(Cpu *c, GuestFn *orig);                /* the task workers' kick: the safe point */
void rb_tree_node(Cpu *c, GuestFn *orig);               /* the task tree's walk (SSBU_RB_TREE) */
void rb_rand_draw(Cpu *c, GuestFn *orig, int which);    /* the script random generators (0 rand, 1 randf) */

/* FUN 0x7103549720: kicks the task workers. Right after the main loop's call to it returns is
 * the safe point where states are saved and loaded. */
/* signature: 16 words, sha256 d1a578dd945f0fbd */
HLE_WRAP(7103549720) { rb_job_kick(c, f_7103549720_orig); }

/* The main loop's presentation callees, by group (SSBU_RB_LITE): each returns `ret` when a
 * re-run frame leaves it out. 1 camera, 2 UI, 4 lighting, 8 render layers, 16 recorder,
 * 32 dynamic resolution, 64 the effect emitters' tick. */
#define LITE(a, group, ret) do { if (rb_lite_skip(c, group)) { c->x[0] = (ret); return; } f_##a##_orig(c); } while (0)
/* signature: 16 words, sha256 f789212bc42da8c1 */
HLE_WRAP(710361d5f0) { LITE(710361d5f0, 1, 0); }
/* signature: 16 words, sha256 e34905d68bcf5199 */
HLE_WRAP(7103618c80) { LITE(7103618c80, 1, 0); }
/* signature: 16 words, sha256 37f50293dde03309 */
HLE_WRAP(7103864cb0) { LITE(7103864cb0, 1, 0); }
/* signature: 16 words, sha256 6b0f9737340e8575 */
HLE_WRAP(7103619630) { LITE(7103619630, 1, 0); }
/* signature: 16 words, sha256 5464bb21a5206c20 */
HLE_WRAP(7103619130) { LITE(7103619130, 1, 0); }
/* signature: 16 words, sha256 9299926a178c8cb0 */
HLE_WRAP(7103632e00) { LITE(7103632e00, 2, 0); }
/* signature: 16 words, sha256 f920f7346c89bda2 */
HLE_WRAP(710361ed90) { LITE(710361ed90, 2, 0); }
/* signature: 16 words, sha256 41c1d5580f8021e4 */
HLE_WRAP(71035c1980) { LITE(71035c1980, 2, 0); }
/* signature: 16 words, sha256 4452cf3d65125bf1 */
HLE_WRAP(7103576550) { LITE(7103576550, 4, 0); }
/* signature: 16 words, sha256 988bdeb13724c499 */
HLE_WRAP(7103576210) { LITE(7103576210, 4, 0); }
/* signature: 16 words, sha256 d53da968ca375a14 */
HLE_WRAP(7103576970) { LITE(7103576970, 4, 0); }
/* signature: 16 words, sha256 2438a775b8d86c9d */
HLE_WRAP(71035941f0) { LITE(71035941f0, 4, 0); }
/* signature: 16 words, sha256 04abe7790f27c281 */
HLE_WRAP(7103785da0) { LITE(7103785da0, 8, 0); }
/* signature: 16 words, sha256 8e36aaa2c026b144 */
HLE_WRAP(71036f31b0) { LITE(71036f31b0, 16, 0); }
/* signature: 16 words, sha256 71bbcba75fe77864 */
HLE_WRAP(71036f32f0) { LITE(71036f32f0, 16, 0); }
/* signature: 16 words, sha256 4828d5e9f7258960 */
HLE_WRAP(71036e73b0) { LITE(71036e73b0, 16, 0); }
/* signature: 16 words, sha256 42f46d5f264cd9c6 */
HLE_WRAP(71036f79c0) { LITE(71036f79c0, 16, 1); }
/* signature: 16 words, sha256 1c1d0f746c9a475b */
HLE_WRAP(71036194a0) { LITE(71036194a0, 32, 0); }
/* signature: 16 words, sha256 3f29e611e32ef3b3 */
HLE_WRAP(710356dd50) { LITE(710356dd50, 64, 0); }

/* FUN 0x7103725030: walks the game's task tree once per logic step (SSBU_RB_TREE=1 times it). */
/* signature: 16 words, sha256 de9e1982b7c954d4 */
HLE_WRAP(7103725030) { rb_tree_node(c, f_7103725030_orig); }

/* app::sv_math::rand(Hash40, int) and randf(Hash40, float): who draws at a fight's start (the log). */
/* signature: 16 words, sha256 c552e251ac052f88 */
HLE_WRAP(7102275320) { rb_rand_draw(c, f_7102275320_orig, 0); }
/* signature: 16 words, sha256 c552e251ac052f88 */
HLE_WRAP(71022756c0) { rb_rand_draw(c, f_71022756c0_orig, 1); }
