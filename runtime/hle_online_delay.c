/* The online input delay of the game's own netcode, chosen by hand (the settings menu's Online >
 * Input delay, config/online_delay.txt: "auto" or 0-25 frames). Idea from saad-script's
 * ssbu-online-deluxe "latency slider" (AGPL-3.0); reimplemented here from its description, no
 * code taken.
 *
 * FUN 0x71016cc940 sets a match's online delay: from the connection's quality it works out a
 * number of frames, stores it at this+0x41 and caps it at 25 (0x19), the store at 0x71016ccab8
 * that the mod's inline hook rewrites. Wrapped here: after the game's own call the byte holds the
 * game's choice ("auto"); a number chosen by hand replaces it. The game reads it from there for
 * the match, so a change counts from the next match set-up. Rollback netplay (SSBU_RB) has its own
 * delay and does not use this one. */
#include <stdio.h>
#include <stdlib.h>

#include "host.h"
#include "config_folder.h"
#include "settings.h"

#define DELAY_MAX 25

static volatile LONG last_auto = -1;   /* the game's own choice at the last set-up, for the menu */

/* -1: auto (the game's), else 0..25 */
int online_delay_setting(void) {
    char line[32] = "";
    if (!settings_line_read(config_path(CONFIG_ONLINE_DELAY), line, sizeof line) || !line[0]) return -1;
    if (line[0] < '0' || line[0] > '9') return -1;
    int v = atoi(line);
    return v < 0 ? -1 : v > DELAY_MAX ? DELAY_MAX : v;
}

int online_delay_last_auto(void) { return (int)last_auto; }

/* signature: 16 words, sha256 c50efee7642ad370 */
HLE_WRAP(71016cc940) {
    u64 self = c->x[0];
    f_71016cc940_orig(c);
    u8 game = ld8(self + 0x41);
    InterlockedExchange(&last_auto, game);
    int want = online_delay_setting();
    if (want >= 0 && want != game) st8(self + 0x41, (u8)want);
    /* (a line when the choice changes, not at every call) */
    static int logged_game = -1, logged_want = -2;
    static long calls;
    calls++;
    if (game != logged_game || want != logged_want) {
        logged_game = game; logged_want = want;
        if (want < 0) fprintf(stderr, "[net] online input delay: %u frame%s, the game's choice (auto; call %ld)\n", game, game == 1 ? "" : "s", calls);
        else fprintf(stderr, "[net] online input delay: %d frame%s, the setting (the game chose %u; call %ld)\n", want, want == 1 ? "" : "s", game, calls);
    }
}
