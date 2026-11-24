/* Discord's sign-in and Rich Presence (runtime/net_discord.c): the parts that need no SDK and no game - the token file
 * the game and the launchers share, the presence's text, the join secret, the fighters' names, the signed requests to
 * the lobby's GSP pages (tools/nex/board.py) and reading their answers (net_discord_core.c). Plain C on every OS;
 * tests/test_discord.py checks it through tools/dev/discordtest.c. */
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

/* discord.txt beside the program (a release writes it from the repository's secrets: tools/discord_fetch.py): the
 * Discord application's id, "ApplicationId=<digits>" or the digits alone. 0: none (Discord stays off). */
uint64_t discord_app_id_parse(const char *text);

/* What the player is doing, as the game knows it (net_discord.c's discord_note_*). */
enum { DP_MENUS, DP_QUICKPLAY, DP_ARENA, DP_ROOM, DP_LOCAL };
typedef struct DiscordPresence {
    int where;                   /* DP_* */
    int fighter;                 /* fighter kind, -1: not known */
    int stage;                   /* -1: not known (the runtime has no stage yet) */
    int players, max_players;    /* 0: not in a gathering / room */
    int in_match;                /* the match is on (else waiting / looking) */
    uint32_t gsp;                /* that fighter's GSP as the game posted it, 0: unknown */
    int rank;                    /* the player's position in the leaderboard, 0: none */
    /* (OpenSmash mastery points) mastery_mode 1: the fighter's mastery instead of the GSP and the position
     * ("Quickplay - Mario - Mastery 1,460"); 0 (SSBU_MASTERY_PRESENCE=0): the GSP and the position as before */
    int mastery_mode;
    int has_mastery;             /* the lobbies' GET /mastery/me answered: `mastery` is that fighter's */
    uint32_t mastery;            /* points summed over the regions + the largest veteran bonus */
    char relay[80];              /* the lobby (host:port) the player is on, "" none */
    char code[40];               /* its lobby code */
    char target[48];             /* an arena's ID, or the Local Wireless room host's address (10.13.x.y); "" none */
    char party[64];              /* the same for everyone in the gathering ("" none) */
} DiscordPresence;
/* The two lines Discord shows: details ("Quickplay - Mario - GSP 1,460 - #12") and state ("In a match"). */
void discord_presence_text(const DiscordPresence *p, char *details, size_t dn, char *state, size_t sn);
/* The join secret friends' games get from "Ask to Join" / "Join": "os2|<a|r>|<lobby>|<code>|<target>"; "" when the
 * player's place cannot be joined (menus, Quickplay, a full gathering, no lobby). Discord takes 128 characters. */
size_t discord_join_secret(const DiscordPresence *p, char *out, size_t cap);
typedef struct DiscordJoin { int where; char relay[80], code[40], target[48]; } DiscordJoin;
int discord_join_parse(const char *secret, DiscordJoin *j);             /* 1: one of ours, in *j */
/* The "lobby" part of a join secret for where this game is online: the address of the lobby server it joined, else
 * NULL (offline, a private room's relay, LAN: nothing a friend can join). An older game's secret may name "eos" (Epic
 * Online Services, gone in 0.5.0): never where this game is, and the official lobby at the next start (net_relay.c). */
const char *discord_join_lobby(const char *lobby_relay);
/* 1 when j is where this game is online now (discord_join_lobby's answer, the lobby code it plays with): same network
 * and lobby code, so the room or arena can be found without a restart. */
int discord_join_same(const DiscordJoin *j, const char *my_lobby, const char *my_code);
/* A room join's host address (the Local Wireless host's virtual address, 10.13.x.y), host order; 0: not one. */
uint32_t discord_join_room_host(const DiscordJoin *j);

const char *discord_fighter_name(int kind);                               /* "Mario"; NULL: not a playable kind */
/* A fighter's key (OpenSmash mastery points): its ui_chara_db name without "ui_chara_" - "mario", "ptrainer",
 * "miifighter" - by the same number as discord_fighter_name (the GSP records' save_no); NULL: none. */
const char *discord_fighter_key(int kind);
/* Every key the game's ui_chara_db can hold for a playable fighter, i = 0, 1, ... until NULL: the select screen's
 * (discord_fighter_key's) and then the ones a fighter's other parts have (popo / nana, pzenigame / pfushigisou /
 * plizardon, eflame / elight ...). */
const char *discord_fighter_key_at(int i);
/* The fighter's number (discord_fighter_name's) a key belongs to, its other parts' too (pzenigame -> 37 Pokemon
 * Trainer); -1: none. */
int discord_fighter_of_key(const char *key);
size_t discord_group_digits(uint32_t v, char *out, size_t cap);           /* 1460000 -> "1,460,000" */

/* ---- the lobby's GSP pages (tools/nex/board.py) ------------------------------------------------------------- */
void discord_sha256(const void *data, size_t n, uint8_t out[32]);
void discord_hmac_sha256(const void *key, size_t kn, const void *msg, size_t mn, uint8_t out[32]);
/* The player's id there: the first 16 hex digits of SHA-256("opensmash-player\n" + the NSA id's 16 hex digits). */
void discord_player_id(const char *nsa_hex, char out[17]);
/* A Discord account as the leaderboard knows it: SHA-256("opensmash-discord\n" + its user id), 16 hex digits. */
void discord_account_hash(const char *user_id, char out[17]);
/* A signed request's signature: hex HMAC-SHA256, key the NSA id's hex, over
 * "opensmash1\n<what>\n<player>\n<t>\n<payload>". */
void discord_sign(const char *nsa_hex, const char *what, const char *player, int64_t t, const char *payload, char out[65]);

/* GET /gsp/me's answer: the player's position on that lobby, the fighter they posted last and its GSP, and where they
 * play now (where: DP_MENUS when in no gathering, else DP_QUICKPLAY or DP_ARENA with the arena's ID). */
typedef struct DiscordMe {
    int rank, fighter;
    uint32_t gsp;
    int where, players, max_players, open;
    char arena[16];
} DiscordMe;
int discord_me_parse(const char *json, DiscordMe *m);                     /* 1: read */
/* The player's position in leaderboard.json (the one public lobby's: tools/nex/board.py): the "rank"
 * of the entry of "players" whose "id" is the one given; 0: not there. A tolerant reader, not a JSON parser. */
int discord_rank_in_leaderboard(const char *json, const char *player_id);

/* (v0.5.0) the signed-in account's lobby ticket: SSBU_ONLINE_TICKET, else the file SSBU_ONLINE_TICKET_FILE names (the
 * launcher renews it while the game runs). 1 when there is one that can go in a header (printable ASCII, no control
 * character). A credential: it goes to an official lobby only. */
int discord_lobby_ticket(char *out, size_t cap);

/* ---- OpenSmash mastery points: GET /mastery/me on each region's lobby (with the ticket) ----------------------- */
#define DISCORD_MASTERY_MAX 160
typedef struct DiscordMastery {
    int answers;                         /* lobbies whose answer was read */
    int n;
    struct { char key[24]; long long points; int veteran; } f[DISCORD_MASTERY_MAX];
} DiscordMastery;
/* One lobby's answer, {"region", "account", "total", "fighters": [{"fighter", "points", "veteran", ...}]}, added to
 * *m: the points summed over the answers, the veteran bonus the largest of them. 1: read (m->answers counted). */
int discord_mastery_add(const char *json, DiscordMastery *m);
/* A fighter's mastery (discord_fighter_name's number; its other parts' keys too): the points summed + the largest
 * veteran bonus, into *out (0 for a fighter the answers do not list). 0: no answer read. */
int discord_mastery_of(const DiscordMastery *m, int kind, long long *out);
