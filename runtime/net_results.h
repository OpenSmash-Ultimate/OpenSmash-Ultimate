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
