/* Discord's Social SDK in the game: sign-in with Discord, and Rich Presence - where the player is (the menus, Quickplay,
 * a Battle Arena, a Local Wireless room), with which fighter and GSP, their position in the GSP leaderboard, the room's
 * free places - with "Ask to Join" / "Join" for Battle Arenas and Local Wireless rooms.
 *
 * Nothing of Discord is part of the source: its library (discord_partner_sdk.dll, from the SDK) and discord.txt (the
 * Discord application's id) come beside the program from a release (tools/discord_fetch.py). Without them, with
 * SSBU_DISCORD=0, or in a headless test run, this is off and nothing changes. The few declarations below are the SDK's C
 * interface (cdiscord.h, version 1.10), written out so that the build needs no SDK. Every SDK call is made on this
 * file's own thread, which runs the SDK's callbacks (Discord_RunCallbacks) every few milliseconds; other threads only
 * note what the game does.
 *
 * Sign-in. The token lives in config/discord_token.txt (net_discord_core.h), shared with OpenSmash.exe and the web
 * launcher, which are where a player normally signs in. At the start a token still good is used as it is, an expired
 * one is refreshed (and written back); with none, the game signs in itself only when asked (SSBU_DISCORD_SIGNIN=1):
 * the SDK's authorization with PKCE (the app is a Public Client, redirect http://127.0.0.1/callback), through the
 * Discord app when it runs, else the browser. Rich Presence works without signing in while the Discord app runs.
 *
 * Where the player is and their GSP. The game's Online mode talks to the lobby's own game server (tools/nex), which
 * keeps each player's GSP (DataStore 82 / 84) and their gatherings: GET /gsp/me on the lobby (tools/nex/board.py),
 * signed with the install's NSA id, says where the player plays and with which fighter, every so often. The position
 * is the one public lobby's leaderboard.json (every GSP result goes through its game server), found by the player's id. Local Wireless rooms are this
 * game's own (hle_net.c notes them).
 *
 * Joining. The join secret is the lobby, its lobby code and the arena's ID or the room host's address
 * (net_discord_core.c). A friend's accepted "Join" on the same lobby lists the room first in Local Wireless's Rooms
 * List (net_rooms_pick) or shows the arena's ID; on another lobby it is kept in config/discord_join.txt, and the next
 * start joins that lobby (net_relay.c's discord_join_at_start). */
#define _CRT_SECURE_NO_WARNINGS
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "net_discord.h"

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include "gpu_menu.h"
#include "net.h"

int account_nsa_hex(char out[17]);   /* (hle_env.c) */

#define REDIRECT "http://127.0.0.1/callback"
#define TOKEN_PATH "config\\" DISCORD_TOKEN_FILE
#define JOIN_PATH "config\\discord_join.txt"
#define BOARD_HOST L"opensmash-lobby.duckdns.org"   /* (the one public lobby's game server: every GSP result is there) */
#define BOARD_PORT 39421
#define BOARD_PATH L"/leaderboard.json"
#define ME_EVERY_MS (45 * 1000)
#define BOARD_EVERY_MS (10 * 60 * 1000)
#define ME_FRESH_MS (3 * 60 * 1000)

/* ---- the SDK's C interface (cdiscord.h), as much as is used ---------------------------------------------- */
typedef struct { uint8_t *ptr; size_t size; } DStr;      /* Discord_String */
typedef struct { void *opaque; } DObj;                    /* Discord_Client, Discord_Activity, ... */
typedef void (*DFree)(void *);
typedef void (*StatusCb)(int status, int error, int32_t detail, void *ud);
typedef void (*ResultCb)(DObj *result, void *ud);
typedef void (*TokenCb)(DObj *result, DStr access, DStr refresh, int type, int32_t expires_in, DStr scopes, void *ud);
typedef void (*AuthCb)(DObj *result, DStr code, DStr redirect, void *ud);
typedef void (*JoinCb)(DStr secret, void *ud);
typedef void (*VoidCb)(void *ud);
enum { D_TOKEN_BEARER = 1, D_STATUS_READY = 3, D_ACTIVITY_PLAYING = 0, D_PARTY_PUBLIC = 1, D_PLATFORM_DESKTOP = 1 };

#define DISCORD_FUNCS(X) \
    X(void, Discord_RunCallbacks, (void)) \
    X(void, Discord_Free, (void *)) \
    X(void, Discord_Client_Init, (DObj *)) \
    X(void, Discord_Client_SetApplicationId, (DObj *, uint64_t)) \
    X(void, Discord_Client_SetStatusChangedCallback, (DObj *, StatusCb, DFree, void *)) \
    X(void, Discord_Client_Connect, (DObj *)) \
    X(void, Discord_Client_UpdateToken, (DObj *, int, DStr, ResultCb, DFree, void *)) \
    X(void, Discord_Client_RefreshToken, (DObj *, uint64_t, DStr, TokenCb, DFree, void *)) \
    X(void, Discord_Client_GetToken, (DObj *, uint64_t, DStr, DStr, DStr, TokenCb, DFree, void *)) \
    X(void, Discord_Client_Authorize, (DObj *, DObj *, AuthCb, DFree, void *)) \
    X(void, Discord_Client_CreateAuthorizationCodeVerifier, (DObj *, DObj *)) \
    X(void, Discord_AuthorizationCodeVerifier_Challenge, (DObj *, DObj *)) \
    X(void, Discord_AuthorizationCodeVerifier_Verifier, (DObj *, DStr *)) \
    X(void, Discord_AuthorizationCodeVerifier_Drop, (DObj *)) \
    X(void, Discord_AuthorizationCodeChallenge_Drop, (DObj *)) \
    X(void, Discord_AuthorizationArgs_Init, (DObj *)) \
    X(void, Discord_AuthorizationArgs_Drop, (DObj *)) \
    X(void, Discord_AuthorizationArgs_SetClientId, (DObj *, uint64_t)) \
    X(void, Discord_AuthorizationArgs_SetScopes, (DObj *, DStr)) \
    X(void, Discord_AuthorizationArgs_SetCodeChallenge, (DObj *, DObj *)) \
    X(void, Discord_Client_GetDefaultPresenceScopes, (DStr *)) \
    X(void, Discord_Client_SetTokenExpirationCallback, (DObj *, VoidCb, DFree, void *)) \
    X(void, Discord_Client_SetActivityJoinCallback, (DObj *, JoinCb, DFree, void *)) \
    X(bool, Discord_Client_RegisterLaunchCommand, (DObj *, uint64_t, DStr)) \
    X(void, Discord_Client_UpdateRichPresence, (DObj *, DObj *, ResultCb, DFree, void *)) \
    X(void, Discord_Activity_Init, (DObj *)) \
    X(void, Discord_Activity_Drop, (DObj *)) \
    X(void, Discord_Activity_SetType, (DObj *, int)) \
    X(void, Discord_Activity_SetDetails, (DObj *, DStr *)) \
    X(void, Discord_Activity_SetState, (DObj *, DStr *)) \
    X(void, Discord_Activity_SetParty, (DObj *, DObj *)) \
    X(void, Discord_Activity_SetSecrets, (DObj *, DObj *)) \
    X(void, Discord_Activity_SetTimestamps, (DObj *, DObj *)) \
    X(void, Discord_Activity_SetAssets, (DObj *, DObj *)) \
    X(void, Discord_Activity_SetSupportedPlatforms, (DObj *, int)) \
    X(void, Discord_ActivityParty_Init, (DObj *)) \
    X(void, Discord_ActivityParty_Drop, (DObj *)) \
    X(void, Discord_ActivityParty_SetId, (DObj *, DStr)) \
    X(void, Discord_ActivityParty_SetCurrentSize, (DObj *, int32_t)) \
    X(void, Discord_ActivityParty_SetMaxSize, (DObj *, int32_t)) \
    X(void, Discord_ActivityParty_SetPrivacy, (DObj *, int)) \
    X(void, Discord_ActivitySecrets_Init, (DObj *)) \
    X(void, Discord_ActivitySecrets_Drop, (DObj *)) \
    X(void, Discord_ActivitySecrets_SetJoin, (DObj *, DStr)) \
    X(void, Discord_ActivityTimestamps_Init, (DObj *)) \
    X(void, Discord_ActivityTimestamps_Drop, (DObj *)) \
    X(void, Discord_ActivityTimestamps_SetStart, (DObj *, uint64_t)) \
    X(void, Discord_ActivityAssets_Init, (DObj *)) \
    X(void, Discord_ActivityAssets_Drop, (DObj *)) \
    X(void, Discord_ActivityAssets_SetLargeImage, (DObj *, DStr *)) \
    X(void, Discord_ActivityAssets_SetLargeText, (DObj *, DStr *)) \
    X(bool, Discord_ClientResult_Successful, (DObj *)) \
    X(void, Discord_ClientResult_Error, (DObj *, DStr *)) \
    X(void, Discord_ClientResult_Drop, (DObj *)) \
    X(void, Discord_Client_GetCurrentUser, (DObj *, DObj *)) \
    X(uint64_t, Discord_UserHandle_Id, (DObj *)) \
    X(void, Discord_UserHandle_Username, (DObj *, DStr *)) \
    X(void, Discord_UserHandle_DisplayName, (DObj *, DStr *)) \
    X(bool, Discord_UserHandle_Avatar, (DObj *, DStr *)) \
    X(void, Discord_UserHandle_Drop, (DObj *))
#define DECLARE(ret, name, args) static ret (*p_##name) args;
DISCORD_FUNCS(DECLARE)

/* ---- state --------------------------------------------------------------------------------------------- */
static SRWLOCK lock = SRWLOCK_INIT;            /* (what other threads note) */
static volatile LONG running, connected;
static uint64_t app_id;
static DObj client;
static DiscordToken tok;                        /* (Discord thread) */
static char verifier[256];
static int64_t started;
/* noted by the game and the lobby (under lock) */
static struct { int in, players, max; uint32_t host; } room;
static DiscordMe me;
static uint64_t me_at;                          /* GetTickCount64 of the last answer, 0 none */
static volatile LONG rank;                      /* the global position (the site's leaderboard.json), 0 none */

static DStr S(const char *s) { DStr d = {(uint8_t *)s, strlen(s)}; return d; }
static void copy_dstr(char *out, size_t cap, DStr s) { size_t n = s.size < cap - 1 ? s.size : cap - 1; memcpy(out, s.ptr, n); out[n] = 0; }
static int64_t now_s(void) { return (int64_t)time(NULL); }
static void say_result(const char *what, DObj *r) {
    DStr e = {0};
    p_Discord_ClientResult_Error(r, &e);
    fprintf(stderr, "[discord] %s: %.*s\n", what, (int)e.size, e.ptr ? (const char *)e.ptr : "");
    if (e.ptr) p_Discord_Free(e.ptr);
}

/* ---- the token ----------------------------------------------------------------------------------------- */
static void on_status(int status, int error, int32_t detail, void *ud) {
    (void)ud;
    if (status == D_STATUS_READY) {
        InterlockedExchange(&connected, 1);
        DObj u = {0};
        p_Discord_Client_GetCurrentUser(&client, &u);
        if (u.opaque) {
            DStr name = {0}, shown = {0}, avatar = {0};
            snprintf(tok.user_id, sizeof tok.user_id, "%llu", (unsigned long long)p_Discord_UserHandle_Id(&u));
            p_Discord_UserHandle_Username(&u, &name);
            p_Discord_UserHandle_DisplayName(&u, &shown);
            if (name.ptr) { copy_dstr(tok.username, sizeof tok.username, name); p_Discord_Free(name.ptr); }
            if (shown.ptr) { copy_dstr(tok.display_name, sizeof tok.display_name, shown); p_Discord_Free(shown.ptr); }
            if (p_Discord_UserHandle_Avatar(&u, &avatar) && avatar.ptr) { copy_dstr(tok.avatar, sizeof tok.avatar, avatar); p_Discord_Free(avatar.ptr); }
            p_Discord_UserHandle_Drop(&u);
            if (tok.access[0] || tok.refresh[0]) discord_token_save(TOKEN_PATH, &tok);
        }
        fprintf(stderr, "[discord] connected%s%s\n", tok.display_name[0] || tok.username[0] ? " as " : "",
                tok.display_name[0] ? tok.display_name : tok.username);
    } else if (error) {
        InterlockedExchange(&connected, 0);
        fprintf(stderr, "[discord] connection: status %d, error %d (%d)\n", status, error, (int)detail);
    }
}
static void on_token_set(DObj *r, void *ud) {
    (void)ud;
    if (p_Discord_ClientResult_Successful(r)) p_Discord_Client_Connect(&client);
    else say_result("the token was refused", r);
    p_Discord_ClientResult_Drop(r);
}
static void use_token(void) { p_Discord_Client_UpdateToken(&client, D_TOKEN_BEARER, S(tok.access), on_token_set, NULL, NULL); }
static void on_token(DObj *r, DStr access, DStr refresh, int type, int32_t expires_in, DStr scopes, void *ud) {
    (void)type; (void)ud;
    if (p_Discord_ClientResult_Successful(r) && access.size) {
        copy_dstr(tok.access, sizeof tok.access, access);
        if (refresh.size) copy_dstr(tok.refresh, sizeof tok.refresh, refresh);
        if (scopes.size) copy_dstr(tok.scope, sizeof tok.scope, scopes);
        tok.expires_at = now_s() + (expires_in > 0 ? expires_in : 3600);
        if (!discord_token_save(TOKEN_PATH, &tok)) fprintf(stderr, "[discord] %s could not be written\n", TOKEN_PATH);
        use_token();
    } else {
        say_result("no token (sign in again from OpenSmash.exe)", r);
        p_Discord_Client_Connect(&client);   /* (Rich Presence through the Discord app still works) */
    }
    p_Discord_ClientResult_Drop(r);
    if (access.ptr) p_Discord_Free(access.ptr);
    if (refresh.ptr) p_Discord_Free(refresh.ptr);
    if (scopes.ptr) p_Discord_Free(scopes.ptr);
}
static void refresh_token(void) {
    DiscordToken f;   /* (a launcher may have refreshed it already: Discord gives each refresh token once) */
    if (discord_token_load(TOKEN_PATH, &f) && discord_token_fresh(&f, now_s(), 120) && strcmp(f.access, tok.access)) { tok = f; use_token(); return; }
    if (f.refresh[0]) snprintf(tok.refresh, sizeof tok.refresh, "%s", f.refresh);
    p_Discord_Client_RefreshToken(&client, app_id, S(tok.refresh), on_token, NULL, NULL);
}
static void on_expiring(void *ud) { (void)ud; if (tok.refresh[0]) refresh_token(); }
static void on_authorized(DObj *r, DStr c, DStr redirect, void *ud) {
    (void)ud;
    if (p_Discord_ClientResult_Successful(r) && c.size) {
        char cs[512], rs[256];
        copy_dstr(cs, sizeof cs, c);
        copy_dstr(rs, sizeof rs, redirect.size ? redirect : S(REDIRECT));
        p_Discord_Client_GetToken(&client, app_id, S(cs), S(verifier), S(rs), on_token, NULL, NULL);
    } else {
        say_result("sign-in not done", r);
        p_Discord_Client_Connect(&client);
    }
    p_Discord_ClientResult_Drop(r);
    if (c.ptr) p_Discord_Free(c.ptr);
    if (redirect.ptr) p_Discord_Free(redirect.ptr);
}
static void sign_in(void) {   /* the SDK's authorization, PKCE (a Public Client: no secret) */
    DObj ver = {0}, chal = {0}, args = {0};
    DStr v = {0}, sc = {0};
    p_Discord_Client_CreateAuthorizationCodeVerifier(&client, &ver);
    p_Discord_AuthorizationCodeVerifier_Challenge(&ver, &chal);
    p_Discord_AuthorizationCodeVerifier_Verifier(&ver, &v);
    copy_dstr(verifier, sizeof verifier, v);
    if (v.ptr) p_Discord_Free(v.ptr);
    char scopes[256] = "";
    p_Discord_Client_GetDefaultPresenceScopes(&sc);
    if (sc.ptr) { copy_dstr(scopes, sizeof scopes, sc); p_Discord_Free(sc.ptr); }
    if (!strstr(scopes, "identify")) strncat(scopes, scopes[0] ? " identify" : "identify", sizeof scopes - strlen(scopes) - 1);
    p_Discord_AuthorizationArgs_Init(&args);
    p_Discord_AuthorizationArgs_SetClientId(&args, app_id);
    p_Discord_AuthorizationArgs_SetScopes(&args, S(scopes));
    p_Discord_AuthorizationArgs_SetCodeChallenge(&args, &chal);
    fprintf(stderr, "[discord] signing in: confirm in Discord or in the browser\n");
    p_Discord_Client_Authorize(&client, &args, on_authorized, NULL, NULL);
    p_Discord_AuthorizationArgs_Drop(&args);
    p_Discord_AuthorizationCodeChallenge_Drop(&chal);
    p_Discord_AuthorizationCodeVerifier_Drop(&ver);
}

/* ---- joining -------------------------------------------------------------------------------------------- */
static int same_lobby(const DiscordJoin *j) {
    const char *relay = net_relay_address(), *code = net_lobby_code();
    return relay && !_stricmp(relay, j->relay) && code && !strcmp(code, j->code);
}
static void on_join(DStr secret, void *ud) {
    (void)ud;
    char s[160], line[200];
    copy_dstr(s, sizeof s, secret);
    if (secret.ptr) p_Discord_Free(secret.ptr);
    DiscordJoin j;
    if (!discord_join_parse(s, &j)) { fprintf(stderr, "[discord] a join that is not ours: ignored\n"); return; }
    fprintf(stderr, "[discord] join: %s %s on %s (lobby code \"%s\")\n", j.where == DP_ARENA ? "arena" : "the room of", j.target, j.relay, j.code);
    if (same_lobby(&j)) {
        unsigned a, b, c, d;
        if (j.where == DP_ROOM && sscanf(j.target, "%u.%u.%u.%u", &a, &b, &c, &d) == 4 && a < 256 && b < 256 && c < 256 && d < 256) {
            net_rooms_pick(a << 24 | b << 16 | c << 8 | d);
            menu_toast("Discord: your friend's room is first in Local Wireless > Join a Room");
        } else if (j.where == DP_ARENA) {
            snprintf(line, sizeof line, "Discord: Battle Arena %s - Online > Battle Arenas > Find an Arena > by ID", j.target);
            menu_toast(line);
        }
        return;
    }
    FILE *f = fopen(JOIN_PATH, "wb");   /* (another lobby: taken at the next start, net_relay.c) */
    if (f) { fprintf(f, "%c %s %s %s %lld\n", j.where == DP_ROOM ? 'r' : 'a', j.relay, j.code[0] ? j.code : "-", j.target, (long long)now_s()); fclose(f); }
    menu_toast("Discord: your friend plays on another lobby - restart the game to join them");
}
static void register_launch(void) {   /* (what Discord starts for a "Join" while the game is not running) */
    char cmd[MAX_PATH * 2 + 8], dir[MAX_PATH];
    DWORD n = GetCurrentDirectoryA(sizeof dir, dir);
    if (n && n < sizeof dir) {
        snprintf(cmd, sizeof cmd, "%s\\OpenSmash.exe", dir);
        if (GetFileAttributesA(cmd) != INVALID_FILE_ATTRIBUTES) { snprintf(cmd, sizeof cmd, "\"%s\\OpenSmash.exe\"", dir); p_Discord_Client_RegisterLaunchCommand(&client, app_id, S(cmd)); return; }
    }
    snprintf(cmd, sizeof cmd, "%s", GetCommandLineA());
    p_Discord_Client_RegisterLaunchCommand(&client, app_id, S(cmd));
}

/* ---- presence ------------------------------------------------------------------------------------------- */
static void on_presence(DObj *r, void *ud) {
    static int said;
    (void)ud;
    if (!p_Discord_ClientResult_Successful(r) && !said++) say_result("Rich Presence not shown (is the Discord app running, or the player signed in?)", r);
    p_Discord_ClientResult_Drop(r);
}
static void presence_now(DiscordPresence *p) {
    memset(p, 0, sizeof *p);
    p->fighter = p->stage = -1;
    const char *relay = net_lobby() ? net_relay_address() : NULL, *code = net_lobby_code();
    if (relay) snprintf(p->relay, sizeof p->relay, "%s", relay);
    if (code) snprintf(p->code, sizeof p->code, "%s", code);
    AcquireSRWLockShared(&lock);
    int fresh = me_at && GetTickCount64() - me_at < ME_FRESH_MS;
    if (fresh && me.where != DP_MENUS) {
        p->where = me.where; p->players = me.players; p->max_players = me.max_players; p->in_match = !me.open;
        snprintf(p->target, sizeof p->target, "%s", me.arena);
        if (me.arena[0]) snprintf(p->party, sizeof p->party, "os-a-%s-%.40s", me.arena, p->relay);
    } else if (room.in) {
        p->where = DP_ROOM; p->players = room.players; p->max_players = room.max;
        snprintf(p->target, sizeof p->target, "%u.%u.%u.%u", room.host >> 24, room.host >> 16 & 255, room.host >> 8 & 255, room.host & 255);
        snprintf(p->party, sizeof p->party, "os-r-%s-%.40s", p->target, p->relay);
    }
    if (fresh) { p->fighter = me.fighter; p->gsp = me.gsp; }
    ReleaseSRWLockShared(&lock);
    p->rank = (int)rank;
}
static void presence_push(void) {
    static char last[512];
    DiscordPresence p;
    presence_now(&p);
    char details[160], state[128], secret[160], key[512];
    discord_presence_text(&p, details, sizeof details, state, sizeof state);
    discord_join_secret(&p, secret, sizeof secret);
    snprintf(key, sizeof key, "%s|%s|%s|%s|%d|%d", details, state, secret, p.party, p.players, p.max_players);
    if (!strcmp(key, last)) return;
    snprintf(last, sizeof last, "%s", key);
    DObj a = {0}, ts = {0}, assets = {0}, party = {0}, sec = {0};
    p_Discord_Activity_Init(&a);
    p_Discord_Activity_SetType(&a, D_ACTIVITY_PLAYING);
    p_Discord_Activity_SetSupportedPlatforms(&a, D_PLATFORM_DESKTOP);
    DStr d = S(details), s = S(state), img = S("opensmash"), txt = S("OpenSmash");
    p_Discord_Activity_SetDetails(&a, &d);
    if (state[0]) p_Discord_Activity_SetState(&a, &s);
    p_Discord_ActivityTimestamps_Init(&ts);
    p_Discord_ActivityTimestamps_SetStart(&ts, (uint64_t)started * 1000);
    p_Discord_Activity_SetTimestamps(&a, &ts);
    p_Discord_ActivityAssets_Init(&assets);   /* (the art "opensmash" uploaded in the Developer Portal: Rich Presence > Art Assets) */
    p_Discord_ActivityAssets_SetLargeImage(&assets, &img);
    p_Discord_ActivityAssets_SetLargeText(&assets, &txt);
    p_Discord_Activity_SetAssets(&a, &assets);
    if (p.party[0] && p.max_players > 0) {
        p_Discord_ActivityParty_Init(&party);
        p_Discord_ActivityParty_SetId(&party, S(p.party));
        p_Discord_ActivityParty_SetCurrentSize(&party, p.players > 0 ? p.players : 1);
        p_Discord_ActivityParty_SetMaxSize(&party, p.max_players);
        p_Discord_ActivityParty_SetPrivacy(&party, D_PARTY_PUBLIC);
        p_Discord_Activity_SetParty(&a, &party);
        if (secret[0]) {
            p_Discord_ActivitySecrets_Init(&sec);
            p_Discord_ActivitySecrets_SetJoin(&sec, S(secret));
            p_Discord_Activity_SetSecrets(&a, &sec);
        }
    }
    p_Discord_Client_UpdateRichPresence(&client, &a, on_presence, NULL, NULL);
    if (sec.opaque) p_Discord_ActivitySecrets_Drop(&sec);
    if (party.opaque) p_Discord_ActivityParty_Drop(&party);
    p_Discord_ActivityAssets_Drop(&assets);
    p_Discord_ActivityTimestamps_Drop(&ts);
    p_Discord_Activity_Drop(&a);
}

/* ---- the lobby's GSP pages and the site's leaderboard ------------------------------------------------------- */
static char *http_get(const wchar_t *host, INTERNET_PORT port, int secure, const wchar_t *path, const wchar_t *headers, size_t cap) {
    HINTERNET s = WinHttpOpen(L"OpenSmash", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0), c = NULL, r = NULL;
    char *buf = NULL;
    size_t got = 0;
    if (!s) return NULL;
    WinHttpSetTimeouts(s, 5000, 5000, 5000, 10000);
    if ((c = WinHttpConnect(s, host, port, 0)) &&
        (r = WinHttpOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0)) &&
        WinHttpSendRequest(r, headers ? headers : WINHTTP_NO_ADDITIONAL_HEADERS, headers ? (DWORD)-1L : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(r, NULL)) {
        DWORD status = 0, sl = sizeof status;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sl, WINHTTP_NO_HEADER_INDEX);
        if (status == 200 && (buf = malloc(cap + 1))) {
            DWORD n = 0;
            while (got < cap && WinHttpReadData(r, buf + got, (DWORD)(cap - got), &n) && n) got += n;
            buf[got] = 0;
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return buf;
}
/* GET /gsp/me on the lobby the game joined (its game server's port answers HTTP: tools/ssbu_online_lobby.py) */
static int ask_me(const char *nsa, const char *player) {
    const char *relay = net_lobby() ? net_relay_address() : NULL;
    if (!relay) {   /* (online over EOS: the public lobby keeps the player's results - net_results.c) */
        const char *r = getenv("SSBU_RESULTS_LOBBY");
        relay = r && *r && _stricmp(r, "off") ? r : "opensmash-lobby.duckdns.org:39421";
    }
    char host[256];
    snprintf(host, sizeof host, "%s", relay);
    char *colon = strrchr(host, ':');
    int port = colon ? atoi(colon + 1) : 0;
    if (!colon || port <= 0 || port > 65535) return 0;
    *colon = 0;
    int64_t t = now_s();
    char sig[65], head[512];
    discord_sign(nsa, "me", player, t, "", sig);
    snprintf(head, sizeof head, "X-OpenSmash-Player: %s\r\nX-OpenSmash-Time: %lld\r\nX-OpenSmash-Sig: %s\r\n", player, (long long)t, sig);
    wchar_t whost[256], whead[512];
    MultiByteToWideChar(CP_UTF8, 0, host, -1, whost, 256);
    MultiByteToWideChar(CP_UTF8, 0, head, -1, whead, 512);
    char *json = http_get(whost, (INTERNET_PORT)port, 0, L"/gsp/me", whead, 64 * 1024);
    if (!json) return 0;
    DiscordMe m;
    int ok = discord_me_parse(json, &m);
    free(json);
    if (ok) {
        AcquireSRWLockExclusive(&lock);
        me = m;
        me_at = GetTickCount64();
        ReleaseSRWLockExclusive(&lock);
    }
    return ok;
}
static DWORD WINAPI web_thread(LPVOID unused) {
    (void)unused;
    uint64_t next_board = 0;
    for (;;) {
        char nsa[17], player[17];
        if (net_configured() && account_nsa_hex(nsa)) {
            discord_player_id(nsa, player);
            ask_me(nsa, player);
            if (GetTickCount64() >= next_board) {
                char *json = http_get(BOARD_HOST, BOARD_PORT, 0, BOARD_PATH, NULL, 16u << 20);
                DiscordToken t;   /* (the leaderboard's rows are Discord accounts: tools/nex/board.py) */
                char who[17];
                if (discord_token_load(TOKEN_PATH, &t) && t.user_id[0]) discord_account_hash(t.user_id, who);
                else snprintf(who, sizeof who, "%s", player);
                if (json) { InterlockedExchange(&rank, discord_rank_in_leaderboard(json, who)); free(json); next_board = GetTickCount64() + BOARD_EVERY_MS; }
                else next_board = GetTickCount64() + 60 * 1000;
            }
        }
        Sleep(ME_EVERY_MS);
    }
}

/* ---- the thread ----------------------------------------------------------------------------------------- */
static DWORD WINAPI discord_thread(LPVOID dll) {
#define LOAD(ret, name, args) if (!(p_##name = (ret (*) args)(void *)GetProcAddress((HMODULE)dll, #name))) { fprintf(stderr, "[discord] the SDK has no %s: off\n", #name); return 0; }
    DISCORD_FUNCS(LOAD)
    started = now_s();
    p_Discord_Client_Init(&client);
    p_Discord_Client_SetApplicationId(&client, app_id);
    p_Discord_Client_SetStatusChangedCallback(&client, on_status, NULL, NULL);
    p_Discord_Client_SetActivityJoinCallback(&client, on_join, NULL, NULL);
    p_Discord_Client_SetTokenExpirationCallback(&client, on_expiring, NULL, NULL);
    register_launch();
    CreateDirectoryA("config", NULL);
    const char *si = getenv("SSBU_DISCORD_SIGNIN");
    if (discord_token_load(TOKEN_PATH, &tok) && discord_token_fresh(&tok, now_s(), 120)) use_token();
    else if (tok.refresh[0]) refresh_token();
    else if (si && *si == '1') sign_in();
    else p_Discord_Client_Connect(&client);   /* (no account: Rich Presence through the Discord app) */
    InterlockedExchange(&running, 1);
    HANDLE t = CreateThread(NULL, 0, web_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    uint64_t next_presence = 0;
    for (;;) {
        p_Discord_RunCallbacks();
        uint64_t now = GetTickCount64();
        if (now >= next_presence) { next_presence = now + 1500; presence_push(); }
        Sleep(16);
    }
}

void discord_start(void) {
    static LONG once;
    if (InterlockedExchange(&once, 1)) return;
    const char *e = getenv("SSBU_DISCORD");
    if (e && *e == '0') return;
    const char *headless = getenv("SSBU_HEADLESS");   /* (test runs never show up on the owner's Discord) */
    if (headless && *headless == '1' && !(e && *e == '1')) return;
    char dir[MAX_PATH], path[MAX_PATH + 40];
    DWORD n = GetModuleFileNameA(NULL, dir, sizeof dir);
    if (!n || n >= sizeof dir) return;
    char *slash = strrchr(dir, '\\');
    if (slash) *slash = 0;
    snprintf(path, sizeof path, "%s\\discord_partner_sdk.dll", dir);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) { fprintf(stderr, "[discord] no discord_partner_sdk.dll beside the program: off\n"); return; }
    snprintf(path, sizeof path, "%s\\discord.txt", dir);
    char text[512] = "";
    FILE *f = fopen(path, "rb");
    if (f) { size_t got = fread(text, 1, sizeof text - 1, f); text[got] = 0; fclose(f); }
    if (!(app_id = discord_app_id_parse(text))) { fprintf(stderr, "[discord] no application id (discord.txt) beside the program: off\n"); return; }
    snprintf(path, sizeof path, "%s\\discord_partner_sdk.dll", dir);
    HMODULE dll = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll) { fprintf(stderr, "[discord] discord_partner_sdk.dll did not load (%lu): off\n", GetLastError()); return; }
    HANDLE t = CreateThread(NULL, 0, discord_thread, dll, 0, NULL);
    if (t) CloseHandle(t);
}
int discord_on(void) { return running != 0; }
int discord_signed_in(void) { return connected != 0; }

void discord_note_room(int in_room, int players, int max_players, uint32_t host_ip) {
    AcquireSRWLockExclusive(&lock);
    room.in = in_room; room.players = players; room.max = max_players; room.host = host_ip;
    ReleaseSRWLockExclusive(&lock);
}

#else   /* (Linux, macOS: the SDK has their libraries; loading them is for later) */
void discord_start(void) {}
int discord_on(void) { return 0; }
int discord_signed_in(void) { return 0; }
void discord_note_room(int in_room, int players, int max_players, uint32_t host_ip) { (void)in_room; (void)players; (void)max_players; (void)host_ip; }
#endif
