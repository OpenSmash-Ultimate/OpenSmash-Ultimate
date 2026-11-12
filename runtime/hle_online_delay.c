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

/* Rollback's Online matches (hle_rb_core.c online_setup): every player is a local one on both games, its pads read
 * from the controller of its number (rb_pad, by logic frame). The game's own netcode still ran beside it: the pad
 * frame controller (main+0x52c50f0, the one the main loop main+0x135f570 asks each frame) stayed in its online mode
 * (+0x8 = 1), whose handler main+0x17547f0 gives a logic frame only when every player of its mask (+0xd) has a pad in
 * its queue (+0x308 + 0x48 * player), the queues this step fills from what the other game's step sends over Pia, with
 * its own input delay (8). Under loss that lockstep stood still for seconds and ran in bursts (18 logic frames at a
 * time, 60 ms one way and 1 % lost), rollback holding the other game meanwhile ("frame advantage ours -10"); its
 * frame count (+0x14) climbed while it stood, and a Battle Arena match was cancelled ("INCONTRO ANNULLATO", the step's
 * state 4 -> 6) after 20-36 s. And the fight read the other player's pads from that queue, not from rollback: a pad
 * the two games' queues held differently (a rollback's load put a queue back, the game's own netcode had moved on)
 * made the games apart with every rollback pad alike (Quickplay, 60 ms + 1 %: a desync at a KO, 6599).
 * So while such a match is rollback's, the controller is put in the local mode a local melee uses (+0x8 = 0, handler
 * main+0x1754650: each player's pad from its controller, +0x298 + 4 * player) and every player mapped to the
 * controller of its number, as rollback reads it; put back as the match ends (its results run on the game's own
 * set-up). The step itself still runs: the Pia session, the results and the next search need it.
 * SSBU_RB_FC_LOCAL=0: left in the online mode, as before. */
#define PAD_CTRL 0x71052C50F0ull
/* (after: 0 before the step runs, 1 after it. The local mode and the mapping are in place only from the step's end to
 * the next step's start - across the main loop's pad handler, which runs between main+0x16d5e50 and this step - and
 * the step sees the game's own: with the players mapped here while the step ran, the step's pads and its checks told
 * the other game's step a station's players were not its own, and a Quickplay match was cut short, code 2, a few
 * frames after its state 4) */
static void online_pads_local(u64 self, int after) {
    int rb_online_match_now(void);   /* (hle_rb_core.c: the players of rollback's Online match now, else 0) */
    static int on = -1, switched;
    static u64 fc_was;
    static u32 map_was[8], mode_was;
    if (on < 0) { const char *e = getenv("SSBU_RB_FC_LOCAL"); on = !(e && *e == '0'); }
    if (!on) return;
    u64 fc = ld64(PAD_CTRL);
    int n = rb_online_match_now();
    if (switched && fc == fc_was && fc) {   /* (the game's own, for the step - and for good once the match is over) */
        st32(fc + 8, mode_was);
        for (u32 i = 0; i < 8; i++) st32(fc + 0x298 + 4 * i, map_was[i]);
    }
    if (switched && (n <= 0 || fc != fc_was)) {
        switched = 0;
        fprintf(stderr, "[net] rollback's Online match over: the game's pad lockstep back (online mode %u)\n", mode_was);
        return;
    }
    if (!after || n <= 0 || !fc || ld8(self + 0x62) != 4) return;   /* (from the step's match state on, after the step) */
    if (!switched) {
        if (ld32(fc + 8) != 1) return;   /* (not the online mode: nothing to do) */
        fc_was = fc; mode_was = ld32(fc + 8);
        char m[120]; size_t k = 0;
        for (u32 i = 0; i < 8; i++) { map_was[i] = ld32(fc + 0x298 + 4 * i); k += snprintf(m + k, sizeof m - k, " %d", (int)map_was[i]); }
        switched = 1;
        fprintf(stderr, "[net] rollback's Online match: the game's pad lockstep off (online mode 1 -> local 0, mask %02x), "
                        "players 1-%d on controllers 1-%d (were%s)\n", ld8(fc + 0xD), n, n, m);
    }
    st32(fc + 8, 0);
    for (int i = 0; i < n && i < 8; i++) st32(fc + 0x298 + 4ull * i, (u32)i);
}

/* FUN 0x71016d7730: the per-frame step of the same object (a state machine at this+0x62, 0..5;
 * it calls 0x71016cc940 itself in one state) that reads the delay at this+0x41 to decide how far
 * the local inputs run ahead of the remote ones. With our servers the setter was never seen to
 * run, so a number chosen by hand is also applied here before every step, and the game's value
 * and state are logged when they change (SSBU_NET_DELAY_LOG=1: every 600 calls too). */
/* signature: 16 words, sha256 7e149cf38a92e47e */
HLE_WRAP(71016d7730) {
    u64 self = c->x[0];
    static int logged_state = -1, logged_delay = -1, verbose = -1;
    static long calls;
    if (verbose < 0) { const char *v = getenv("SSBU_NET_DELAY_LOG"); verbose = v && v[0] == '1'; }
    u8 state = ld8(self + 0x62), game = ld8(self + 0x41);
    int want = online_delay_setting();
    if (want >= 0 && want != game) st8(self + 0x41, (u8)want);
    calls++;
    if (state != logged_state || game != logged_delay || (verbose && calls % 600 == 0)) {
        logged_state = state; logged_delay = game;
        fprintf(stderr, "[net] online step: state %u, delay %u (base %u, flag %u), setting %d, call %ld; relay role %u (mode %u, flag %u), "
                        "station %u, result %u, end %u/%u\n",
                state, game, ld8(self + 0x43), ld8(self + 0x3C), want, calls, ld8(self), ld8(0x71052C51B0ull), ld8(0x71052C51B6ull),
                ld8(self + 1), ld8(self + 0x5E), ld8(self + 0x63), ld16(self + 0x64));
    }
    online_pads_local(self, 0);
    {   /* (rollback's Online match: the step's own ends of the match. Its end asked is +0x63 - 1 over, main+0x16d5e50 when
         * main+0x16acd30 says so; 2 cut short, main+0x16ce7c0 - counted down at +0x66 to state 6, the step moving on to
         * state 5 at once. Those come from the game's own netcode, each game on its own and at its own logic frame, not
         * from the fight: a Quickplay match ended on one game at its logic frame 3065 and went on to 3466 on the other,
         * the games apart from there (the desync of a Quickplay match at 6599 before, too); the step's check that the
         * stations' games agree (main+0x16cf1d0, +0x5f: with rollback a frame runs first on a guessed pad) cut one short
         * at its logic frame 37, and the game left the gathering. So the step is not run while rollback plays the match
         * (below), and what main+0x16d5e50 asks meanwhile is taken back. Once the fight is over - the same logic frame on
         * both games - the step runs again with "over" asked, the game's own end of a match: its state 5, then 6.
         * "Over" asked, 30 frames on main+0x16cced0 calls the step's +0x68 (main+0x156b860 -> the Online melee scene's
         * main+0x15695a0: its +0x53, the "match over" main+0x156a570 waits for) and the step goes to state 6; the results
         * follow, result mode on 118-137 frames after the fight, and the games pass them (Quickplay and Battle Arena, EOS
         * relay forced, 60 ms + 1 % loss). Run through the fight instead (SSBU_RB_STEP_RUN=1), the step ends rollback's
         * fight at its first frame. SSBU_RB_OVER_AFTER=<frames>: asked that many frames after the fight.)
         * SSBU_RB_STEP_END=1: left as the game does it.) */
        int rb_online_match_now(void);
        static int keep = -1, played, after = -1, since;
        static u64 played_self;
        if (after < 0) { const char *e = getenv("SSBU_RB_OVER_AFTER"); after = e && *e ? atoi(e) : 0; }
        static long undone;
        if (keep < 0) { const char *e = getenv("SSBU_RB_STEP_END"); keep = e && *e == '1'; }
        u8 asked = ld8(self + 0x63);
        if (!keep && rb_online_match_now() > 0 && ld8(self + 0x62) == 4) {
            played = 1; played_self = self; since = 0;
            if (asked) {
                if (++undone <= 5 || undone % 600 == 0)
                    fprintf(stderr, "[net] rollback's Online match: the game's own end of the match taken back (kind %u: %s, code %u%s; logic frame %u; %ld so far)\n",
                            asked, asked == 1 ? "over" : "cut short", ld16(self + 0x64), ld8(self + 0x5F) & 1 ? ", stations apart" : "",
                            ld32(0x71052c5184ull), undone);
                if (asked == 2) { st8(self + 0x5F, ld8(self + 0x5F) & ~1); st8(self + 0x3B, 0); }
                st8(self + 0x63, 0); st16(self + 0x64, 0); st16(self + 0x66, 0);
            }
        } else if (played && rb_online_match_now() <= 0 && self == played_self && ld8(self + 0x62) == 4 && asked != 1 && ++since < after) {
            /* (meanwhile a cut short - the step's checks on what it did not run - is taken back too) */
            if (asked == 2) {
                fprintf(stderr, "[net] online step: a cut short taken back after rollback's fight (code %u, %d frames after)\n", ld16(self + 0x64), since);
                st8(self + 0x5F, ld8(self + 0x5F) & ~1); st8(self + 0x3B, 0); st8(self + 0x63, 0); st16(self + 0x64, 0); st16(self + 0x66, 0);
            }
        } else if (played && rb_online_match_now() <= 0) {
            if (self == played_self && ld8(self + 0x62) == 4) {
                if (asked != 1) { st8(self + 0x5F, ld8(self + 0x5F) & ~1); st16(self + 0x64, 0); st16(self + 0x66, 0x1E); st8(self + 0x63, 1); }
                fprintf(stderr, "[net] online step: rollback's fight is over: the game's own \"over\" asked (was %u; logic frame %u)\n", asked, ld32(0x71052c5184ull));
            }
            played = 0;
        }
    }
    {   /* (the match's end asked: +0x63 the kind - 1 the game over, as from main+0x16d5e50; 2 cut short, main+0x16ce7c0 -
         * +0x64 its code, +0x66 the frames left before the step ends it, state 6. Said once each time it is set) */
        static u8 end_was;
        u8 end = ld8(self + 0x63);
        if (end != end_was) {
            end_was = end;
            if (end) fprintf(stderr, "[net] online step: the match's end asked (kind %u: %s, code %u, in %u frames; state %u, logic frame %u; "
                             "relay role %u, heard %u, quiet check %u/%u, last heard %llu, stations apart %u)\n",
                             end, end == 1 ? "over" : end == 2 ? "cut short" : "?", ld16(self + 0x64), ld16(self + 0x66), state, ld32(0x71052c5184ull),
                             ld8(self), ld16(self + 2), ld8(self + 0x3E), ld8(self + 0x3F), (unsigned long long)ld64(self + 0x98), ld8(self + 0x5F));
        }
        static long heard_log;
        if (verbose && ++heard_log % 600 == 0)
            fprintf(stderr, "[net] online step: relay role %u, heard %u, quiet check %u/%u, last heard %llu, keep-alive sent %llu\n", ld8(self), ld16(self + 2),
                    ld8(self + 0x3E), ld8(self + 0x3F), (unsigned long long)ld64(self + 0x98), (unsigned long long)ld64(self + 0x80));
    }
    {   /* (rollback's Online match: the 31 s quiet check, main+0x16cf320 -> main+0x16cf7c4: +0x98 not renewed for 31 s
         * and +0x3e armed: the match cut short, code 2, on both games - "INCONTRO ANNULLATO" - at logic frame 1700 of a
         * Battle Arena match, every time. +0x98 is renewed by main+0x16d5e50 (main+0x16d75dc) each time a lockstep frame
         * has every station's pads, on a game whose +0x0 is 0; with the step left out (below) no station sends its pads,
         * so it is never renewed. +0x0 is not the arena's host: it is a relay role (a station that plays no pads of its
         * own and passes the others' on, main+0x16cf3e0, its keep-alive once a second, main+0x16d8c48), set when the
         * object is made (main+0x16d5b10 from main+0x134c09c) from the game's mode, main+0x52c51b0 (20 Battle Arena, 14
         * Quickplay: 0 on both games, as it should) and main+0x52c51b6; the gathering's owner and host in the NEX store
         * are the arena's maker, as the game expects. While
         * rollback plays the fight its link hears the other game every frame and says when it is gone (net_gone_check),
         * so the check is marked as already said (+0x3f), through the results to the step's own end (state 5 or 6:
         * "over" is asked in the results, above), and put back then. SSBU_RB_QUIET_CHECK=1: left on.) */
        int rb_online_match_now(void);
        static int keep = -1, marked;
        static u64 marked_self;
        static u8 was;
        if (keep < 0) { const char *e = getenv("SSBU_RB_QUIET_CHECK"); keep = e && *e == '1'; }
        int now = rb_online_match_now() > 0;
        if (marked && (state > 4 || self != marked_self || (state < 4 && !now))) {   /* (kept through the results: "over" is asked there, below the fight) */
            if (self == marked_self) st8(self + 0x3F, was);
            marked = 0;
            fprintf(stderr, "[net] online step: the game's quiet check back (state %u)\n", state);
        }
        if (!keep && now && !ld8(self) && ld8(self + 0x63) == 0 && (state == 4 || (marked && state < 4))) {
            if (!marked) {
                was = ld8(self + 0x3F); marked = 1; marked_self = self;
                fprintf(stderr, "[net] rollback's Online match: the game's 31 s quiet check off (armed %u, relay role %u)\n", ld8(self + 0x3E), ld8(self));
            }
            if (!ld8(self + 0x3F)) st8(self + 0x3F, 1);
        }
    }
    {   /* (rollback's Online match, from the step's match state on to the fight's end: the step itself is not run. It is
         * the game's own lockstep - each frame its station's pads to the other's step, their checks, its own ends of
         * the match - and with the pads rollback's (above) the fight needs none of it, while each of its decisions,
         * taken on each game apart, could stop one game's fight and not the other's. What it asked meanwhile (taken back
         * above) and the results run on it once the fight is over. The Pia session and the NEX connection live on
         * their own. SSBU_RB_STEP_RUN=1: the step runs all the same.) */
        int rb_online_match_now(void);
        static int run = -1;
        static long skipped;
        if (run < 0) { const char *e = getenv("SSBU_RB_STEP_RUN"); run = e && *e == '1'; }
        if (!run && rb_online_match_now() > 0 && ld8(self + 0x62) == 4) {
            if (!skipped++) fprintf(stderr, "[net] rollback's Online match: the game's own step left out while rollback plays the match (logic frame %u)\n",
                                    ld32(0x71052c5184ull));
            online_pads_local(self, 1);
            return;
        }
        if (skipped) { fprintf(stderr, "[net] online step: run again (left out %ld times; state %u)\n", skipped, ld8(self + 0x62)); skipped = 0; }
    }
    f_71016d7730_orig(c);
    online_pads_local(self, 1);
}
