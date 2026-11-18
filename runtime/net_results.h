/* Each match this game plays online, reported to a lobby's count of matches played (net_results.c). Before
 * OpenSmash 0.5.0 the same thread also sent the player's fighter records and ranked statistics to the public lobby
 * for a game whose game server was its own (Online over Epic Online Services): gone with EOS. */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* A match this game played online, for the public lobby's count of matches played (POST /match/played,
 * tools/nex/matches.py): every game of the match sends it, with the same id, and the lobby counts it once. */
typedef struct {
    uint64_t id;     /* the same on every game of the match */
    int exact;       /* 1: id from the rollback handshake's match seed (that match alone); 0: from the players' addresses
                      * and the mode (the same for those players' next match: the lobby takes reports a minute apart) */
    int mode;        /* the match block's melee mode: 14 Quickplay, 20 Battle Arena, any other a Local Wireless room's */
    int players, seconds, rollback;
} MatchPlayed;
/* (game thread) queued, sent by the results' thread (started here if it is not yet) to the official lobby the game
 * plays on, or to SSBU_RESULTS_LOBBY's; nothing with SSBU_RESULTS_LOBBY=off, nor on a server of the player's own */
void results_match_played(const MatchPlayed *m);
/* (tests) the report's data string (JSON); its length, 0 when it does not fit */
size_t results_match_data(const MatchPlayed *m, char *out, size_t cap);
uint64_t results_match_id_seed(uint64_t seed);   /* the exact id of a rollback match's seed */
/* the id of a match without a seed: its players' virtual addresses (any order; this game's among them) and mode */
uint64_t results_match_id_addresses(const uint32_t *ips, int n, int mode);
