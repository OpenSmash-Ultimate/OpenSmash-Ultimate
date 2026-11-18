/* Offline checks of a match's report for a lobby's count of matches played (runtime/net_results.c: its id and the
 * data tools/nex/matches.py parses). tests/test_net_results.py builds and runs it; exit code 0 when every check
 * passes. (Before OpenSmash 0.5.0, as pathtest.c, it also checked Epic Online Services' path choice, net_path.c, and
 * the /gsp/records answer of the EOS results upload: gone with EOS.) */
#include <stdio.h>
#include <string.h>

#include "../../runtime/net_results.h"

/* (what net_results.c reaches outside itself; never called here) */
int account_nsa_hex(char out[17]) { out[0] = 0; return 0; }
const char *net_relay_address(void) { return NULL; }
int net_means_official_lobby(const char *host) { (void)host; return 0; }
const char *net_official_lobby(void) { return NULL; }

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    /* a match played, for the lobby's count (tools/nex/matches.py parses what results_match_data writes) */
    uint32_t one[3] = {0x0a0d0005u, 0x0a0d0002u, 0x0a0d0009u}, other[3] = {0x0a0d0009u, 0x0a0d0005u, 0x0a0d0002u};
    CHECK(results_match_id_addresses(one, 3, 20) == results_match_id_addresses(other, 3, 20), "the same players in any order: one id");
    CHECK(results_match_id_addresses(one, 3, 20) != results_match_id_addresses(one, 3, 14), "another mode: another id");
    CHECK(results_match_id_addresses(one, 2, 20) != results_match_id_addresses(one, 3, 20), "other players: another id");
    CHECK(results_match_id_seed(0x1234) == results_match_id_seed(0x1234) && results_match_id_seed(0x1234) != results_match_id_seed(0x1235),
          "the seed's id");
    char data[512];
    MatchPlayed m = {0x00000000000000abull, 1, 14, 2, 95, 1};
    size_t n = results_match_data(&m, data, sizeof data);
    CHECK(n && !strcmp(data, "{\"id\":\"00000000000000ab\",\"mode\":\"quickplay\",\"players\":2,\"seconds\":95,\"ranked\":true,"
                             "\"net\":\"rollback\",\"via\":\"lobby\",\"exact\":true}"), "a Quickplay match's data: %s", data);
    MatchPlayed r = {0xffffffffffffffffull, 0, 1, 1, -5, 0};
    results_match_data(&r, data, sizeof data);
    CHECK(!strcmp(data, "{\"id\":\"ffffffffffffffff\",\"mode\":\"room\",\"players\":2,\"seconds\":0,\"ranked\":false,"
                        "\"net\":\"game\",\"via\":\"lobby\",\"exact\":false}"), "a room's, clamped: %s", data);
    m.mode = 20;
    results_match_data(&m, data, sizeof data);
    CHECK(strstr(data, "\"mode\":\"arena\"") && strstr(data, "\"ranked\":false"), "an arena's: %s", data);
    CHECK(results_match_data(&m, data, 20) == 0, "too small: nothing");

    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
