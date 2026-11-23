/* Offline checks of a match's report for a lobby's count of matches played (runtime/net_results.c: its id and the
 * data tools/nex/matches.py parses, with the "mastery" object of OpenSmash mastery points). tests/test_net_results.py
 * builds and runs it; exit code 0 when every check passes. "resultstest.exe off" (run with SSBU_MASTERY_REPORT=0): the
 * switch reads off. (Before OpenSmash 0.5.0, as pathtest.c, it also checked Epic Online Services' path choice, net_path.c, and
 * the /gsp/records answer of the EOS results upload: gone with EOS.) */
#include <stdio.h>
#include <string.h>

#include "../../runtime/net_discord_core.h"
#include "../../runtime/net_results.h"

/* (what net_results.c reaches outside itself; never called here) */
int account_nsa_hex(char out[17]) { out[0] = 0; return 0; }
const char *net_relay_address(void) { return NULL; }
int net_means_official_lobby(const char *host) { (void)host; return 0; }
const char *net_official_lobby(void) { return NULL; }

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "off")) {   /* (SSBU_MASTERY_REPORT=0) */
        CHECK(!results_mastery_on(), "SSBU_MASTERY_REPORT=0: off");
        printf("%d checks, %d failed\n", checks, failures);
        return failures != 0;
    }
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

    /* OpenSmash mastery points: the fighters' keys from their ui_chara ids (0xc1 << 56 | hash40("ui_chara_<name>"):
     * ui_chara_mario's as the game's record holds it, the fight test's log line) */
    char key[24];
    CHECK(results_fighter_key(0xc100000edaf3c863ull, key, sizeof key) && !strcmp(key, "mario"), "Mario's ui_chara: %s", key);
    CHECK(results_fighter_key(0x000000000edaf3c863ull, key, sizeof key) && !strcmp(key, "mario"), "the hash40 alone");
    CHECK(!results_fighter_key(0xc1ffff0000000000ull, key, sizeof key) && !key[0], "no player: no key");
    CHECK(!results_fighter_key(0xc100000edaf3c864ull, key, sizeof key) && !key[0], "another id: no key");
    int every = 1;
    for (int i = 0; discord_fighter_key_at(i); i++) {   /* (each key's own id gives it back: the table has no two alike) */
        const char *k = discord_fighter_key_at(i);
        char name[48]; snprintf(name, sizeof name, "ui_chara_%s", k);
        uint32_t c = 0xffffffffu; size_t len = strlen(name);
        for (size_t j = 0; j < len; j++) { c ^= (uint8_t)name[j]; for (int b = 0; b < 8; b++) c = c >> 1 ^ (0xedb88320u & (0u - (c & 1))); }
        uint64_t id = 0xc100000000000000ull | (uint64_t)len << 32 | (uint32_t)~c;
        if (!results_fighter_key(id, key, sizeof key) || strcmp(key, k)) { every = 0; printf("  %s -> %s\n", k, key); }
    }
    CHECK(every, "every key from its own id");

    /* the "mastery" object (both games of a match send the same fighters and places; the lobby computes the points) */
    MatchPlayed q = {0x00000000000000abull, 1, 14, 2, 95, 1};
    q.mastery.have = 1; q.mastery.me = 1; q.mastery.end = MASTERY_END_NORMAL; q.mastery.left = -1; q.mastery.stock_start = 3; q.mastery.n = 2;
    strcpy(q.mastery.p[0].fighter, "mario"); q.mastery.p[0].stocks = 2; q.mastery.p[0].place = 1;
    strcpy(q.mastery.p[1].fighter, "pzenigame"); q.mastery.p[1].stocks = 0; q.mastery.p[1].place = 2;
    static char big[2048];
    n = results_match_data(&q, big, sizeof big);
    CHECK(n && !strcmp(big, "{\"id\":\"00000000000000ab\",\"mode\":\"quickplay\",\"players\":2,\"seconds\":95,\"ranked\":true,"
                            "\"net\":\"rollback\",\"via\":\"lobby\",\"exact\":true,\"mastery\":{\"v\":1,\"me\":1,\"end\":\"normal\",\"left\":-1,"
                            "\"stock_start\":3,\"players\":[{\"slot\":0,\"fighter\":\"mario\",\"stocks\":2,\"place\":1},"
                            "{\"slot\":1,\"fighter\":\"pzenigame\",\"stocks\":0,\"place\":2}]}}"), "with mastery: %s", big);
    q.mastery.have = 0;
    results_match_data(&q, big, sizeof big);
    results_match_data(&m, data, sizeof data);
    m.mode = 14;
    results_match_data(&m, data, sizeof data);
    CHECK(!strcmp(big, data), "no mastery (SSBU_MASTERY_REPORT=0, an offline game): the data as before: %s", big);
    q.mastery.have = 1; q.mastery.end = MASTERY_END_TIME; q.mastery.stock_start = 0; q.mastery.p[0].stocks = 0; q.mastery.p[0].place = 0; q.mastery.p[1].place = 0;
    results_match_data(&q, big, sizeof big);
    CHECK(strstr(big, "\"end\":\"time\",\"left\":-1,\"stock_start\":0,") && strstr(big, "\"stocks\":0,\"place\":0}]"), "a time match, no places: %s", big);
    q.mastery.end = MASTERY_END_LEFT; q.mastery.left = 0;
    results_match_data(&q, big, sizeof big);
    CHECK(strstr(big, "\"end\":\"left\",\"left\":0,"), "a player left: %s", big);
    q.mastery.end = 7; q.mastery.left = 5; q.mastery.stock_start = -2; q.mastery.p[0].place = 9; q.mastery.p[0].stocks = -1;
    strcpy(q.mastery.p[1].fighter, "Bad\"key");
    results_match_data(&q, big, sizeof big);
    CHECK(strstr(big, "\"end\":\"left\",\"left\":-1,\"stock_start\":0,") && strstr(big, "{\"slot\":0,\"fighter\":\"mario\",\"stocks\":0,\"place\":0}")
          && strstr(big, "\"fighter\":\"\","), "values out of range, a key not a key: %s", big);
    q.mastery.n = 8; q.players = 8; q.mastery.end = MASTERY_END_NORMAL;
    for (int i = 0; i < 8; i++) { strcpy(q.mastery.p[i].fighter, "miiswordsman"); q.mastery.p[i].stocks = 99; q.mastery.p[i].place = i + 1; }
    n = results_match_data(&q, big, 1536);   /* (net_results.c's buffer) */
    CHECK(n > 0 && strstr(big, "{\"slot\":7,\"fighter\":\"miiswordsman\",\"stocks\":99,\"place\":8}]}}"), "8 players fit: %zu", n);
    CHECK(!results_match_data(&q, big, 300), "too small with the mastery: nothing");
    /* (the report's signature covers the data, mastery and all: its longest form still signed) */
    char sig[65];
    discord_sign("00000000000000a1", "match", "0123456789abcdef", 1763300000, big, sig);
    results_match_data(&q, big, 1536);
    discord_sign("00000000000000a1", "match", "0123456789abcdef", 1763300000, big, sig);
    CHECK(strlen(sig) == 64, "a long report is signed");
    CHECK(results_mastery_on(), "on by default");

    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
