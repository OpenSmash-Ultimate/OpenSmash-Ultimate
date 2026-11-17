/* The players' results to the public lobby (net_results.c): a game whose game server is its own (Online over EOS:
 * hle_online.c's NEX in the game) sends the lobby its player's fighter records (the GSP, DataStore 82) and ranked
 * statistics after each change, and takes their records back from it at the start, so a player's GSP is the same on
 * every PC and the lobby's leaderboard (tools/nex/board.py) has every player. */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* (once) the thread; marker: the file that says results are still to be sent (kept across starts) */
void results_start(const char *marker);
void results_changed(void);   /* the player's records or statistics changed: sent soon */

void online_results_begin(void);   /* (hle_online.c) results_start with its marker beside nex_powers.bin */
/* (hle_online.c) what is sent: the JSON object {"records": [...], "ranked": [...]} of the player's own; 0: nothing yet */
size_t online_results_snapshot(char *out, size_t cap);
/* (hle_online.c) the player's records as the lobby keeps them (NEX_POWER_RECORD bytes each), fetched at the start */
void online_results_fetched(const uint8_t *records, int n);

/* (tests) a /gsp/records answer's records: hex strings of `size` bytes, at most max; the count */
int results_parse_records(const char *json, uint8_t *out, int size, int max);

/* A match this game played online, for the public lobby's count of matches played (POST /match/played,
 * tools/nex/matches.py): every game of the match sends it, with the same id, and the lobby counts it once. */
typedef struct {
    uint64_t id;     /* the same on every game of the match */
    int exact;       /* 1: id from the rollback handshake's match seed (that match alone); 0: from the players' addresses
                      * and the mode (the same for those players' next match: the lobby takes reports a minute apart) */
    int mode;        /* the match block's melee mode: 14 Quickplay, 20 Battle Arena, any other a Local Wireless room's */
    int players, seconds, rollback, eos;
} MatchPlayed;
/* (game thread) queued, sent by the results' thread (started here if it is not yet: over the public lobby too);
 * nothing with SSBU_RESULTS_LOBBY=off */
void results_match_played(const MatchPlayed *m);
/* (tests) the report's data string (JSON); its length, 0 when it does not fit */
size_t results_match_data(const MatchPlayed *m, char *out, size_t cap);
uint64_t results_match_id_seed(uint64_t seed);   /* the exact id of a rollback match's seed */
/* the id of a match without a seed: its players' virtual addresses (any order; this game's among them) and mode */
uint64_t results_match_id_addresses(const uint32_t *ips, int n, int mode);
