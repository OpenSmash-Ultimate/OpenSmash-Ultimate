/* Discord's sign-in and Rich Presence (runtime/net_discord.c): the parts that need no SDK and no game - the token
 * file the game and the launchers share, the presence's text, the join secret and the fighters' names (net_discord_core.c).
 * tests/test_discord.py checks them through tools/dev/discordtest.c. */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* config/discord_token.txt (beside the other settings; the game, OpenSmash.exe and the web launcher read and write it):
 * key=value lines, '#' comments. Delete it to sign out. */
#define DISCORD_TOKEN_FILE "discord_token.txt"
typedef struct DiscordToken {
    char access[512], refresh[512], scope[256];
    int64_t expires_at;                  /* unix seconds, 0: unknown */
    char user_id[24], username[64], display_name[96], avatar[64];
} DiscordToken;
int discord_token_parse(const char *text, DiscordToken *t);              /* 1: it has an access or a refresh token */
size_t discord_token_format(const DiscordToken *t, char *out, size_t cap);   /* the file's text (0: did not fit) */
int discord_token_load(const char *path, DiscordToken *t);               /* 1: read and usable */
int discord_token_save(const char *path, const DiscordToken *t);         /* written whole (a .tmp file, then renamed) */
/* 1: the access token is good for at least `margin` more seconds (0 when expires_at is unknown) */
int discord_token_fresh(const DiscordToken *t, int64_t now, int64_t margin);

/* What the player is doing, as the game knows it (net_discord.c's discord_presence_*). */
enum { DP_MENUS, DP_QUICKPLAY, DP_ARENA, DP_ROOM, DP_LOCAL };
typedef struct DiscordPresence {
    int where;                   /* DP_* */
    int fighter;                 /* fighter kind, -1: not known */
    int stage;                   /* -1: not known (the runtime has no stage yet) */
    int players, max_players;    /* 0: not in a gathering / room */
    int in_match;                /* the match is on (else waiting / looking) */
    uint32_t gsp;                /* that fighter's GSP as the game posted it, 0: unknown */
    int rank;                    /* the player's position in the leaderboard (leaderboard.json), 0: none */
    char code[16];               /* the lobby code (online_server.txt "eos <code>"), "" none */
    char target[48];             /* an arena's ID, or the Local Wireless room host's EOS id; "" none */
    char party[64];              /* the same for everyone in the gathering ("" none) */
} DiscordPresence;
/* The two lines Discord shows: details ("Quickplay - Mario - GSP 1,460,000 - #12") and state ("In a match"). */
void discord_presence_text(const DiscordPresence *p, char *details, size_t dn, char *state, size_t sn);
/* The join secret friends' games get from "Ask to Join" / "Join": "os1|<q|a|r>|<code>|<target>"; "" when the
 * player's place cannot be joined (menus, a full gathering, Quickplay). */
size_t discord_join_secret(const DiscordPresence *p, char *out, size_t cap);
typedef struct DiscordJoin { int where; char code[16], target[48]; } DiscordJoin;
int discord_join_parse(const char *secret, DiscordJoin *j);             /* 1: one of ours, in *j */

const char *discord_fighter_name(int kind);                               /* "Mario"; NULL: not a playable kind */
size_t discord_group_digits(uint32_t v, char *out, size_t cap);           /* 1460000 -> "1,460,000" */
/* The player's position in leaderboard.json (the site's, tools: Denkishi/opensmash-site): the "rank" of the entry of
 * "players" whose "puid" or "discord_id" is the one given; 0: not there. A tolerant reader, not a JSON parser. */
int discord_rank_in_leaderboard(const char *json, const char *puid, const char *discord_id);
