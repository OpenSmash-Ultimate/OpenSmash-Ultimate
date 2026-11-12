/* Offline checks of Discord's sign-in and Rich Presence without the SDK or an account (runtime/net_discord_core.c,
 * and runtime/net_discord.c with no discord_partner_sdk.dll beside the program): the shared token file, the presence's
 * text, the join secret, fighter names, the position read from leaderboard.json, and that the game is untouched
 * without the SDK. tests/test_discord.py builds and runs it; exit code 0 when every check passes.
 *   discordtest.exe <a scratch folder> */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../runtime/net_discord.h"
#include "../../runtime/net_eos.h"

/* (net_eos.c's, which this program leaves out) */
int (*eos_external_token)(char *out, size_t n);
const char *eos_my_id(void) { return NULL; }

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
    snprintf(path, sizeof path, "%s\\%s", dir, DISCORD_TOKEN_FILE);
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
    CHECK(!discord_token_load("Z:\\no\\such\\file.txt", &u), "a missing file");
    DiscordToken f = {0};
    strcpy(f.access, "a"); f.expires_at = 1000;
    CHECK(discord_token_fresh(&f, 800, 120) && !discord_token_fresh(&f, 900, 120) && !discord_token_fresh(&f, 2000, 0), "fresh until the margin");
    f.expires_at = 0;
    CHECK(!discord_token_fresh(&f, 0, 0), "an unknown expiry is not fresh");
}

static void presence(void) {
    DiscordPresence p;
    char d[160], s[128], secret[160];
    memset(&p, 0, sizeof p); p.fighter = -1; p.stage = -1;
    discord_presence_text(&p, d, sizeof d, s, sizeof s);
    CHECK(!strcmp(d, "In the menus") && !s[0], "menus: \"%s\" / \"%s\"", d, s);
    CHECK(!discord_join_secret(&p, secret, sizeof secret) && !secret[0], "menus: nothing to join");
    p.where = DP_QUICKPLAY; p.fighter = 0; p.gsp = 8120000; p.rank = 12; p.players = 2; p.max_players = 2; p.in_match = 1;
    discord_presence_text(&p, d, sizeof d, s, sizeof s);
    CHECK(!strcmp(d, "Quickplay - Mario - GSP 8,120,000 - #12") && !strcmp(s, "In a match"), "quickplay: \"%s\" / \"%s\"", d, s);
    strcpy(p.target, "x");
    CHECK(!discord_join_secret(&p, secret, sizeof secret), "quickplay cannot be joined");
    p.where = DP_ARENA; p.fighter = 43; p.gsp = 0; p.rank = 0; p.in_match = 0; p.players = 3; p.max_players = 8;
    strcpy(p.code, "abc"); strcpy(p.target, "K7Q2M");
    discord_presence_text(&p, d, sizeof d, s, sizeof s);
    CHECK(!strcmp(d, "Battle Arena - Olimar") && !strcmp(s, "Arena K7Q2M"), "arena: \"%s\" / \"%s\"", d, s);
    CHECK(discord_join_secret(&p, secret, sizeof secret) && !strcmp(secret, "os1|a|abc|K7Q2M"), "arena secret: %s", secret);
    p.players = 8;
    CHECK(!discord_join_secret(&p, secret, sizeof secret), "a full arena cannot be joined");
    p.where = DP_ROOM; p.players = 2; p.fighter = 200; p.code[0] = 0; strcpy(p.target, "0002a2b3c4d5e6f708192a3b4c5d6e7f");
    discord_presence_text(&p, d, sizeof d, s, sizeof s);
    CHECK(!strcmp(d, "Local Wireless room") && !strcmp(s, "Waiting in the room"), "room, unknown fighter: \"%s\" / \"%s\"", d, s);
    CHECK(discord_join_secret(&p, secret, sizeof secret) && !strcmp(secret, "os1|r||0002a2b3c4d5e6f708192a3b4c5d6e7f"), "room secret: %s", secret);
    strcpy(p.target, "a|b");
    CHECK(!discord_join_secret(&p, secret, sizeof secret), "a target with the separator is refused");
    char tiny[8];
    strcpy(p.target, "K7Q2M");
    CHECK(!discord_join_secret(&p, tiny, sizeof tiny) && !tiny[0], "a secret that does not fit");

    DiscordJoin j;
    CHECK(discord_join_parse("os1|a|abc|K7Q2M", &j) && j.where == DP_ARENA && !strcmp(j.code, "abc") && !strcmp(j.target, "K7Q2M"), "parse arena");
    CHECK(discord_join_parse("os1|r||0002a2b3", &j) && j.where == DP_ROOM && !j.code[0] && !strcmp(j.target, "0002a2b3"), "parse room, no code");
    CHECK(!discord_join_parse("os1|q|abc|x", &j), "quickplay is not joinable");
    CHECK(!discord_join_parse("os2|a|abc|x", &j) && !discord_join_parse("os1|a|abc|", &j) && !discord_join_parse("os1|a|abc", &j), "malformed");
    CHECK(!discord_join_parse("os1|a|abc|x|y", &j) && !discord_join_parse("os1|a|ab c|x", &j) && !discord_join_parse(NULL, &j), "extra separator, spaces, NULL");
    CHECK(!discord_join_parse("os1|a|abcdefghijklmnopqrstuvwxyz|x", &j), "a code too long");
}

static void names(void) {
    char g[24];
    CHECK(!strcmp(discord_fighter_name(0), "Mario") && !strcmp(discord_fighter_name(43), "Olimar") && !strcmp(discord_fighter_name(93), "Sora"), "names");
    CHECK(!strcmp(discord_fighter_name(75), "Ice Climbers") && !strcmp(discord_fighter_name(114), "Pokemon Trainer"), "names (2)");
    CHECK(!discord_fighter_name(-1) && !discord_fighter_name(76) && !discord_fighter_name(94) && !discord_fighter_name(500), "not playable");
    discord_group_digits(0, g, sizeof g); CHECK(!strcmp(g, "0"), "0");
    discord_group_digits(999, g, sizeof g); CHECK(!strcmp(g, "999"), "999");
    discord_group_digits(1000, g, sizeof g); CHECK(!strcmp(g, "1,000"), "1,000: %s", g);
    discord_group_digits(4294967295u, g, sizeof g); CHECK(!strcmp(g, "4,294,967,295"), "max: %s", g);
    CHECK(!discord_group_digits(1000000, g, 5) && !g[0], "too tiny a buffer");
}

static void leaderboard(void) {
    const char *json =
        "{\"updated\": \"2026-11-12T10:00:00Z\", \"players\": [\n"
        " {\"rank\": 1, \"puid\": \"000aaa\", \"discord_id\": \"111\", \"name\": \"A \\\"}{ [rank\\\"\", \"gsp\": 9000000,\n"
        "  \"fighters\": {\"43\": {\"gsp\": 9000000, \"rank\": 1}}},\n"
        " {\"name\": \"B\", \"fighters\": {\"0\": {\"rank\": 7, \"gsp\": 1}}, \"rank\": 2, \"puid\": \"000bbb\", \"discord_id\": null},\n"
        " {\"rank\": 3, \"puid\": \"000ccc\", \"discord_id\": \"333\"}\n"
        "], \"fighters\": {\"43\": {\"name\": \"Olimar\", \"top\": [{\"rank\": 1, \"puid\": \"000zzz\"}]}}}";
    CHECK(discord_rank_in_leaderboard(json, "000aaa", NULL) == 1, "by puid");
    CHECK(discord_rank_in_leaderboard(json, "000bbb", "") == 2, "the player's rank, not a fighter's (keys in any order)");
    CHECK(discord_rank_in_leaderboard(json, NULL, "333") == 3, "by Discord id");
    CHECK(discord_rank_in_leaderboard(json, "nobody", "999") == 0, "not there");
    CHECK(discord_rank_in_leaderboard(json, "000zzz", NULL) == 0, "only \"players\" counts, not a fighter's table");
    CHECK(discord_rank_in_leaderboard("{\"updated\": null, \"players\": [], \"fighters\": {}}", "000aaa", "111") == 0, "empty");
    CHECK(discord_rank_in_leaderboard("{\"players\": [{\"rank\": 1, \"puid\": \"000a", "000a", NULL) == 0, "cut short");
    CHECK(discord_rank_in_leaderboard("garbage", "x", "y") == 0 && discord_rank_in_leaderboard(NULL, "x", "y") == 0, "not JSON");
}

static void without_the_sdk(void) {
    discord_start();   /* (no discord_partner_sdk.dll beside this program) */
    discord_start();
    CHECK(!discord_on() && !discord_signed_in(), "off without the SDK");
    CHECK(eos_external_token == NULL, "EOS is not asked to wait for a Discord token");
    discord_note_online(DP_ARENA, 2, 8, 0, "K7Q2M", "os-aK7Q2M");
    discord_note_room(1, 2, 8, 1, "id");
    discord_note_fighter(1, 5);
    discord_note_lobby_code("abc");
    DiscordJoin j;
    CHECK(!discord_take_join(&j), "no join");
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    token_file(dir);
    presence();
    names();
    leaderboard();
    without_the_sdk();
    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
