/* Offline checks of Discord's sign-in and Rich Presence without the SDK or an account (runtime/net_discord_core.c,
 * and runtime/net_discord.c with no discord_partner_sdk.dll beside the program): the shared token file, the application
 * id file, the presence's text, the join secret, fighter names, SHA-256 / HMAC and the signed requests to the lobby's
 * GSP pages, reading GET /gsp/me's answer and the position in leaderboard.json, and that the game is untouched without
 * the SDK. tests/test_discord.py builds and runs it (and checks the "sign" lines it prints against tools/nex/board.py);
 * exit code 0 when every check passes.
 *   discordtest.exe <a scratch folder> */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../runtime/net_discord.h"

/* (the game's, which this program leaves out) */
int net_lobby(void) { return 0; }
int net_configured(void) { return 0; }
const char *net_relay_address(void) { return NULL; }
const char *net_lobby_code(void) { return "smash"; }
void net_rooms_pick(unsigned host) { (void)host; }
void menu_toast(const char *text) { (void)text; }
int account_nsa_hex(char out[17]) { out[0] = 0; return 0; }

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void token_file(const char *dir) {
    DiscordToken t, u;
    memset(&t, 0, sizeof t);
    strcpy(t.access, "acc.123"); strcpy(t.refresh, "ref=456"); strcpy(t.scope, "openid sdk.social_layer_presence identify");
    t.expires_at = 1900000000; strcpy(t.user_id, "80351110224678912"); strcpy(t.username, "nelly"); strcpy(t.display_name, "Nelly 'N' Smash"); strcpy(t.avatar, "8342729096ea3675442027381ff50dfe");
    char text[4096];
    CHECK(discord_token_format(&t, text, sizeof text) > 0, "format");
    CHECK(discord_token_parse(text, &u) && !memcmp(&t, &u, sizeof t), "the file reads back as written:\n%s", text);
    CHECK(strstr(text, "refresh_token=ref=456\n") != NULL, "a value with '=' is kept whole");
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, DISCORD_TOKEN_FILE);
    CHECK(discord_token_save(path, &t), "save");
    CHECK(discord_token_load(path, &u) && !strcmp(u.access, "acc.123") && u.expires_at == 1900000000, "load");
    strcpy(t.access, "acc2");
    CHECK(discord_token_save(path, &t) && discord_token_load(path, &u) && !strcmp(u.access, "acc2"), "saved again over the old file");
    strcpy(t.display_name, "two\nlines");
    CHECK(!discord_token_format(&t, text, sizeof text), "a value with a line break is refused (it would make another key)");
    CHECK(!discord_token_save(path, &t) && discord_token_load(path, &u) && !strcmp(u.display_name, "Nelly 'N' Smash"), "and the file stays as it was");
    CHECK(discord_token_parse("# comment\r\n  access_token = abc \r\nexpires_at=12\r\nunknown=1\r\n", &u) && !strcmp(u.access, "abc") && u.expires_at == 12, "CRLF, spaces, comments, unknown keys");
    CHECK(!discord_token_parse("# signed out\n", &u), "no token: not usable");
    CHECK(discord_token_parse("refresh_token=r\n", &u) && !u.access[0], "a refresh token alone is usable (refreshed at the start)");
    CHECK(!discord_token_load("Z:/no/such/file.txt", &u), "a missing file");
    DiscordToken f = {0};
    strcpy(f.access, "a"); f.expires_at = 1000;
    CHECK(discord_token_fresh(&f, 800, 120) && !discord_token_fresh(&f, 900, 120) && !discord_token_fresh(&f, 2000, 0), "fresh until the margin");
    f.expires_at = 0;
    CHECK(!discord_token_fresh(&f, 0, 0), "an unknown expiry is not fresh");
}

static void app_id(void) {
    CHECK(discord_app_id_parse("# Discord\r\nApplicationId=123456789012345678\r\n") == 123456789012345678ull, "ApplicationId=");
    CHECK(discord_app_id_parse("  ApplicationId = 123456789012345678  \n") == 123456789012345678ull, "spaces");
    CHECK(discord_app_id_parse("123456789012345678") == 123456789012345678ull, "the digits alone");
    CHECK(!discord_app_id_parse("") && !discord_app_id_parse(NULL) && !discord_app_id_parse("ApplicationId=\n"), "none");
    CHECK(!discord_app_id_parse("ApplicationId=12345") && !discord_app_id_parse("ApplicationId=1234567890123456789x"), "not an id");
    CHECK(!discord_app_id_parse("ClientSecret=123456789012345678"), "another key is not the id");
}

static void presence(void) {
    DiscordPresence p;
    char d[160], s[128], secret[160];
    memset(&p, 0, sizeof p); p.fighter = -1; p.stage = -1;
    discord_presence_text(&p, d, sizeof d, s, sizeof s);
    CHECK(!strcmp(d, "In the menus") && !s[0], "menus: \"%s\" / \"%s\"", d, s);
    CHECK(!discord_join_secret(&p, secret, sizeof secret) && !secret[0], "menus: nothing to join");
    p.where = DP_QUICKPLAY; p.fighter = 0; p.gsp = 8120000; p.rank = 12; p.players = 2; p.max_players = 2; p.in_match = 1;
    strcpy(p.relay, "opensmash-lobby.duckdns.org:39421"); strcpy(p.code, "smash");
    discord_presence_text(&p, d, sizeof d, s, sizeof s);
    CHECK(!strcmp(d, "Quickplay - Mario - GSP 8,120,000 - #12") && !strcmp(s, "In a match"), "quickplay: \"%s\" / \"%s\"", d, s);
    strcpy(p.target, "x");
    CHECK(!discord_join_secret(&p, secret, sizeof secret), "quickplay cannot be joined");
    p.where = DP_ARENA; p.fighter = 42; p.gsp = 0; p.rank = 0; p.in_match = 0; p.players = 3; p.max_players = 8;
    strcpy(p.code, "abc"); strcpy(p.target, "K7Q2M");
    discord_presence_text(&p, d, sizeof d, s, sizeof s);
    CHECK(!strcmp(d, "Battle Arena - Olimar") && !strcmp(s, "Arena K7Q2M - 5 free"), "arena: \"%s\" / \"%s\"", d, s);
    CHECK(discord_join_secret(&p, secret, sizeof secret) && !strcmp(secret, "os2|a|opensmash-lobby.duckdns.org:39421|abc|K7Q2M"), "arena secret: %s", secret);
    p.players = 8;
    CHECK(!discord_join_secret(&p, secret, sizeof secret), "a full arena cannot be joined");
    p.where = DP_ROOM; p.players = 2; p.fighter = 200; strcpy(p.code, "smash"); strcpy(p.target, "10.13.0.7");
    discord_presence_text(&p, d, sizeof d, s, sizeof s);
    CHECK(!strcmp(d, "Local Wireless room") && !strcmp(s, "Waiting in the room - 6 free"), "room, unknown fighter: \"%s\" / \"%s\"", d, s);
    CHECK(discord_join_secret(&p, secret, sizeof secret) && !strcmp(secret, "os2|r|opensmash-lobby.duckdns.org:39421|smash|10.13.0.7"), "room secret: %s", secret);
    p.relay[0] = 0;
    CHECK(!discord_join_secret(&p, secret, sizeof secret), "no lobby (a private room's relay): nothing to join");
    strcpy(p.relay, "opensmash-lobby.duckdns.org:39421");
    strcpy(p.target, "a|b");
    CHECK(!discord_join_secret(&p, secret, sizeof secret), "a target with the separator is refused");
    char tiny[8];
    strcpy(p.target, "K7Q2M");
    CHECK(!discord_join_secret(&p, tiny, sizeof tiny) && !tiny[0], "a secret that does not fit");

    DiscordJoin j;
    CHECK(discord_join_parse("os2|a|opensmash-lobby.duckdns.org:39421|abc|K7Q2M", &j) && j.where == DP_ARENA && !strcmp(j.relay, "opensmash-lobby.duckdns.org:39421")
          && !strcmp(j.code, "abc") && !strcmp(j.target, "K7Q2M"), "parse arena");
    CHECK(discord_join_parse("os2|r|1.2.3.4:39421||10.13.0.7", &j) && j.where == DP_ROOM && !j.code[0] && !strcmp(j.target, "10.13.0.7"), "parse room, no code");
    CHECK(!discord_join_parse("os2|q|h:1|abc|x", &j), "quickplay is not joinable");
    CHECK(!discord_join_parse("os1|a|abc|x", &j) && !discord_join_parse("os2|a|h:1|abc|", &j) && !discord_join_parse("os2|a|h:1|abc", &j), "malformed");
    CHECK(!discord_join_parse("os2|a||abc|x", &j), "no lobby");
    CHECK(!discord_join_parse("os2|a|h:1|abc|x|y", &j) && !discord_join_parse("os2|a|h:1|ab c|x", &j) && !discord_join_parse(NULL, &j), "extra separator, spaces, NULL");
    CHECK(!discord_join_parse("os2|a|h:1|abcdefghijklmnopqrstuvwxyzabcdefghijklmnop|x", &j) && !j.where, "a code too long");
}

static void names(void) {
    char g[24];
    CHECK(!strcmp(discord_fighter_name(0), "Mario") && !strcmp(discord_fighter_name(42), "Olimar") && !strcmp(discord_fighter_name(85), "Sora"), "names");
    CHECK(!strcmp(discord_fighter_name(16), "Ice Climbers") && !strcmp(discord_fighter_name(37), "Pokemon Trainer"), "names (2)");
    CHECK(!strcmp(discord_fighter_name(29), "Mr. Game & Watch") && !strcmp(discord_fighter_name(30), "Meta Knight"), "names (GSP numbers: #389)");
    CHECK(!discord_fighter_name(-1) && !discord_fighter_name(86) && !discord_fighter_name(114) && !discord_fighter_name(500), "not playable");
    discord_group_digits(0, g, sizeof g); CHECK(!strcmp(g, "0"), "0");
    discord_group_digits(999, g, sizeof g); CHECK(!strcmp(g, "999"), "999");
    discord_group_digits(1000, g, sizeof g); CHECK(!strcmp(g, "1,000"), "1,000: %s", g);
    discord_group_digits(4294967295u, g, sizeof g); CHECK(!strcmp(g, "4,294,967,295"), "max: %s", g);
    CHECK(!discord_group_digits(1000000, g, 5) && !g[0], "too tiny a buffer");
}

static void tohex(const uint8_t *b, size_t n, char *out) { for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]); }

static void hashing(void) {
    uint8_t h[32];
    char x[65];
    discord_sha256("", 0, h); tohex(h, 32, x);
    CHECK(!strcmp(x, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "SHA-256 of nothing: %s", x);
    discord_sha256("abc", 3, h); tohex(h, 32, x);
    CHECK(!strcmp(x, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "SHA-256 abc: %s", x);
    const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";   /* (two blocks with the padding) */
    discord_sha256(two, strlen(two), h); tohex(h, 32, x);
    CHECK(!strcmp(x, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"), "SHA-256, 56 bytes: %s", x);
    discord_hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, h); tohex(h, 32, x);
    CHECK(!strcmp(x, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"), "HMAC (RFC 4231, case 2): %s", x);
    uint8_t key[131];
    memset(key, 0xaa, sizeof key);
    const char *big = "Test Using Larger Than Block-Size Key - Hash Key First";
    discord_hmac_sha256(key, sizeof key, big, strlen(big), h); tohex(h, 32, x);
    CHECK(!strcmp(x, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"), "HMAC, a long key (case 6): %s", x);
    /* (printed for tests/test_discord.py, which checks them against tools/nex/board.py) */
    char player[17], sig[65], acct[17];
    discord_player_id("00000000000000a1", player);
    discord_sign("00000000000000a1", "profile", player, 1763300000, "Nelly\nab12", sig);
    discord_account_hash("80351110224678912", acct);
    printf("sign %s %s %s\n", player, sig, acct);
}

static void me(void) {
    DiscordMe m;
    const char *json = "{\"id\":\"3f9a0c1d2e4b5a69\",\"rank\":2,\"ranked\":7,\"fighters\":[{\"fighter\":43,\"gsp\":1500,\"rank\":2},"
                       "{\"fighter\":0,\"gsp\":900,\"rank\":1}],\"fighter\":0,\"gsp\":900,\"name\":\"N \\\"q\\\" {[\",\"where\":"
                       "{\"mode\":\"arena\",\"arena\":\"K7Q2M\",\"players\":3,\"max\":8,\"open\":true}}";
    CHECK(discord_me_parse(json, &m) && m.rank == 2 && m.fighter == 0 && m.gsp == 900 && m.where == DP_ARENA && !strcmp(m.arena, "K7Q2M")
          && m.players == 3 && m.max_players == 8 && m.open, "me: rank %d fighter %d gsp %u where %d arena %s %d/%d open %d",
          m.rank, m.fighter, m.gsp, m.where, m.arena, m.players, m.max_players, m.open);
    CHECK(discord_me_parse("{\"rank\":0,\"fighters\":[],\"fighter\":-1,\"gsp\":0,\"where\":null}", &m) && m.where == DP_MENUS && m.fighter == -1, "no gathering");
    CHECK(discord_me_parse("{\"where\":{\"mode\":\"quickplay\",\"arena\":\"\",\"players\":2,\"max\":2,\"open\":false}}", &m)
          && m.where == DP_QUICKPLAY && !m.open && m.max_players == 2, "quickplay");
    CHECK(discord_me_parse("{\"where\":{\"mode\":\"arena\",\"arena\":\"a|b\"}}", &m) && !m.arena[0], "an arena id with the separator is dropped");
    CHECK(!discord_me_parse("{\"rank\":2,", &m) && !m.rank && !discord_me_parse("[1]", &m) && !discord_me_parse(NULL, &m), "malformed");
}

static void leaderboard(void) {
    const char *json =
        "{\"updated\": \"2026-11-12T10:00:00Z\", \"ranked\": 3, \"players\": [\n"
        " {\"rank\": 1, \"id\": \"aaaa\", \"name\": \"A \\\"}{ [rank\\\"\", \"gsp\": 9000000,\n"
        "  \"fighters\": {\"43\": {\"gsp\": 9000000, \"rank\": 1}}},\n"
        " {\"name\": \"B\", \"fighters\": {\"0\": {\"rank\": 7, \"gsp\": 1}}, \"rank\": 2, \"id\": \"bbbb\", \"discord\": null},\n"
        " {\"rank\": 3, \"id\": \"cccc\"}\n"
        "], \"fighters\": {\"43\": {\"name\": \"Olimar\", \"top\": [{\"rank\": 1, \"id\": \"zzzz\"}]}}}";
    CHECK(discord_rank_in_leaderboard(json, "aaaa") == 1, "by id");
    CHECK(discord_rank_in_leaderboard(json, "bbbb") == 2, "the player's rank, not a fighter's (keys in any order)");
    CHECK(discord_rank_in_leaderboard(json, "cccc") == 3, "the last");
    CHECK(discord_rank_in_leaderboard(json, "nobody") == 0, "not there");
    CHECK(discord_rank_in_leaderboard(json, "zzzz") == 0, "only \"players\" counts, not a fighter's table");
    CHECK(discord_rank_in_leaderboard("{\"updated\": null, \"players\": [], \"fighters\": {}}", "aaaa") == 0, "empty");
    CHECK(discord_rank_in_leaderboard("{\"players\": [{\"rank\": 1, \"id\": \"aaaa\"}, {\"rank\": 2, \"id\": \"bb", "aaaa") == 1, "cut short after it");
    CHECK(discord_rank_in_leaderboard("{\"players\": [{\"rank\": 1, \"id\": \"aa", "aa") == 0, "cut short in it");
    CHECK(discord_rank_in_leaderboard("garbage", "x") == 0 && discord_rank_in_leaderboard(NULL, "x") == 0 && discord_rank_in_leaderboard(json, "") == 0, "not JSON");
}

static void without_the_sdk(void) {
    discord_start();   /* (no discord_partner_sdk.dll beside this program) */
    discord_start();
    CHECK(!discord_on() && !discord_signed_in(), "off without the SDK");
    discord_note_room(1, 2, 8, 0x0A0D0007);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    token_file(dir);
    app_id();
    presence();
    names();
    hashing();
    me();
    leaderboard();
    without_the_sdk();
    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
