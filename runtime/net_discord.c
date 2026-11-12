/* Discord's Social SDK in the game (plan/EOS_MIGRATION.md, "Discord Social SDK"): sign-in with Discord, which also
 * signs the player into EOS Connect (net_eos.c), and Rich Presence - where the player is, with which fighter and GSP,
 * their position in the leaderboard - with "Ask to Join" / "Join" for Battle Arenas and Local Wireless rooms.
 *
 * Nothing of Discord is part of the source: its library (discord_partner_sdk.dll, from the SDK; a release puts it
 * beside the program, tools/discord_fetch.py) is loaded from beside the program when it is there. Without it, or with
 * SSBU_DISCORD=0, this is off and nothing changes. The few declarations below are the SDK's C interface (cdiscord.h,
 * version 1.10), written out so that the build needs no SDK. Every SDK call is made on this file's own thread, which
 * runs the SDK's callbacks (Discord_RunCallbacks) every few milliseconds; other threads only note what the game does.
 *
 * Sign-in. The token lives in config/discord_token.txt (net_discord_core.h), shared with OpenSmash.exe and the web
 * launcher, which are where a player normally signs in. At the start a token still good is used as it is, an expired
 * one is refreshed (and written back); with none, the game signs in itself only when asked (SSBU_DISCORD_SIGNIN=1):
 * the SDK's authorization with PKCE (the app is a Public Client, redirect http://127.0.0.1/callback), through the
 * Discord app when it runs, else the browser. Rich Presence works without signing in while the Discord app runs.
 *
 * Joining. The join secret is the lobby code and the arena's ID or the room host's EOS id (net_discord_core.c); a friend's
 * accepted "Join" arrives as discord_take_join() and config/discord_join.txt. What the game does with it (going
 * straight into that arena or room) is not done yet: see the PR's notes. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "net_discord.h"
#include "net_eos.h"

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#define APP_ID 1069217215540371466ull   /* the Discord application "OpenSmash" */
#define REDIRECT "http://127.0.0.1/callback"
#define TOKEN_PATH "config\\" DISCORD_TOKEN_FILE
#define JOIN_PATH "config\\discord_join.txt"
#define BOARD_HOST L"denkishi.github.io"
#define BOARD_PATH L"/opensmash-site/leaderboard.json"

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
static SRWLOCK lock = SRWLOCK_INIT;            /* (what other threads note; the token for EOS) */
static volatile LONG running, connected;
static DObj client;
static DiscordToken tok;                        /* (Discord thread) */
static char verifier[256];
static int64_t started;
enum { T_NONE, T_WORKING, T_READY };
static volatile LONG tok_state = T_NONE;        /* what EOS is told (eos_external_token) */
static char eos_token[sizeof tok.access];       /* (under lock) */
static char user_id[24];                         /* (under lock) the Discord account's, for the leaderboard */
/* noted by the game */
static struct { int where, players, max, in_match; char arena[16], party[64]; } on;
static struct { int in, players, max, hosting; char host[48]; } room;
static int fighter = -1;
static uint32_t gsp;
static char code[16];
static volatile LONG rank;
static DiscordJoin join_pending;
static int join_has;

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
static void give_eos(int state) {
    AcquireSRWLockExclusive(&lock);
    if (state == T_READY) { snprintf(eos_token, sizeof eos_token, "%s", tok.access); snprintf(user_id, sizeof user_id, "%s", tok.user_id); }
    else eos_token[0] = 0;
    ReleaseSRWLockExclusive(&lock);
    InterlockedExchange(&tok_state, state);
}
static int eos_hook(char *out, size_t n) {   /* (the EOS thread) */
    LONG s = tok_state;
    if (s == T_WORKING) return -1;
    if (s != T_READY) return 0;
    AcquireSRWLockShared(&lock);
    snprintf(out, n, "%s", eos_token);
    ReleaseSRWLockShared(&lock);
    return out[0] ? 1 : 0;
}

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
            discord_token_save(TOKEN_PATH, &tok);
            AcquireSRWLockExclusive(&lock);
            snprintf(user_id, sizeof user_id, "%s", tok.user_id);
            ReleaseSRWLockExclusive(&lock);
        }
        fprintf(stderr, "[discord] signed in as %s\n", tok.display_name[0] ? tok.display_name : tok.username);
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
static void use_token(void) {   /* tok.access is good: EOS gets it, the SDK connects */
    give_eos(T_READY);
    p_Discord_Client_UpdateToken(&client, D_TOKEN_BEARER, S(tok.access), on_token_set, NULL, NULL);
}
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
        give_eos(T_NONE);
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
    give_eos(T_WORKING);
    p_Discord_Client_RefreshToken(&client, APP_ID, S(tok.refresh), on_token, NULL, NULL);
}
static void on_expiring(void *ud) { (void)ud; if (tok.refresh[0]) refresh_token(); }
static void on_authorized(DObj *r, DStr c, DStr redirect, void *ud) {
    (void)ud;
    if (p_Discord_ClientResult_Successful(r) && c.size) {
        char cs[512], rs[256];
        copy_dstr(cs, sizeof cs, c);
        copy_dstr(rs, sizeof rs, redirect.size ? redirect : S(REDIRECT));
        p_Discord_Client_GetToken(&client, APP_ID, S(cs), S(verifier), S(rs), on_token, NULL, NULL);
    } else {
        say_result("sign-in not done", r);
        give_eos(T_NONE);
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
    if (!strstr(scopes, "identify")) strncat(scopes, scopes[0] ? " identify" : "identify", sizeof scopes - strlen(scopes) - 1);   /* (EOS reads the account with it) */
    p_Discord_AuthorizationArgs_Init(&args);
    p_Discord_AuthorizationArgs_SetClientId(&args, APP_ID);
    p_Discord_AuthorizationArgs_SetScopes(&args, S(scopes));
    p_Discord_AuthorizationArgs_SetCodeChallenge(&args, &chal);
    give_eos(T_WORKING);
    fprintf(stderr, "[discord] signing in: confirm in Discord or in the browser\n");
    p_Discord_Client_Authorize(&client, &args, on_authorized, NULL, NULL);
    p_Discord_AuthorizationArgs_Drop(&args);
    p_Discord_AuthorizationCodeChallenge_Drop(&chal);
    p_Discord_AuthorizationCodeVerifier_Drop(&ver);
}

/* ---- joining -------------------------------------------------------------------------------------------- */
static void on_join(DStr secret, void *ud) {
    (void)ud;
    char s[160];
    copy_dstr(s, sizeof s, secret);
    if (secret.ptr) p_Discord_Free(secret.ptr);
    DiscordJoin j;
    if (!discord_join_parse(s, &j)) { fprintf(stderr, "[discord] a join that is not ours: ignored\n"); return; }
    AcquireSRWLockExclusive(&lock);
    join_pending = j;
    join_has = 1;
    ReleaseSRWLockExclusive(&lock);
    FILE *f = fopen(JOIN_PATH, "wb");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
    fprintf(stderr, "[discord] join: %s %s (lobby code \"%s\")\n", j.where == DP_ARENA ? "arena" : "room", j.target, j.code);
}
static void register_launch(void) {   /* (what Discord starts for a "Join" while the game is not running) */
    char cmd[MAX_PATH * 2 + 8], dir[MAX_PATH];
    DWORD n = GetCurrentDirectoryA(sizeof dir, dir);
    if (n && n < sizeof dir) {
        snprintf(cmd, sizeof cmd, "%s\\OpenSmash.exe", dir);
        if (GetFileAttributesA(cmd) != INVALID_FILE_ATTRIBUTES) { snprintf(cmd, sizeof cmd, "\"%s\\OpenSmash.exe\"", dir); p_Discord_Client_RegisterLaunchCommand(&client, APP_ID, S(cmd)); return; }
    }
    snprintf(cmd, sizeof cmd, "%s", GetCommandLineA());
    p_Discord_Client_RegisterLaunchCommand(&client, APP_ID, S(cmd));
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
    AcquireSRWLockShared(&lock);
    if (on.where != DP_MENUS) {
        p->where = on.where; p->players = on.players; p->max_players = on.max; p->in_match = on.in_match;
        snprintf(p->target, sizeof p->target, "%s", on.arena);
        snprintf(p->party, sizeof p->party, "%s", on.party);
    } else if (room.in) {
        p->where = DP_ROOM; p->players = room.players; p->max_players = room.max;
        snprintf(p->target, sizeof p->target, "%s", room.host);
        snprintf(p->party, sizeof p->party, "os-r%s", room.host);
    }
    p->fighter = fighter; p->gsp = gsp; p->stage = -1;
    snprintf(p->code, sizeof p->code, "%s", code);
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

/* ---- the player's position: the site's leaderboard.json (no server of ours; Denkishi/opensmash-site's workflow
 * makes it from EOS's leaderboard) --------------------------------------------------------------------------- */
static char *http_get(const wchar_t *host, const wchar_t *path, size_t cap) {
    HINTERNET s = WinHttpOpen(L"OpenSmash", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0), c = NULL, r = NULL;
    char *buf = NULL;
    size_t got = 0;
    if (!s) return NULL;
    WinHttpSetTimeouts(s, 5000, 5000, 5000, 10000);
    if ((c = WinHttpConnect(s, host, INTERNET_DEFAULT_HTTPS_PORT, 0)) &&
        (r = WinHttpOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)) &&
        WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, NULL)) {
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
static DWORD WINAPI rank_thread(LPVOID unused) {
    (void)unused;
    for (;;) {
        const char *puid = eos_my_id();
        char did[24];
        AcquireSRWLockShared(&lock);
        snprintf(did, sizeof did, "%s", eos_token[0] ? user_id : "");
        ReleaseSRWLockShared(&lock);
        if (puid || did[0]) {
            char *json = http_get(BOARD_HOST, BOARD_PATH, 8u << 20);
            if (json) { InterlockedExchange(&rank, discord_rank_in_leaderboard(json, puid, did)); free(json); Sleep(10 * 60 * 1000); continue; }
        }
        Sleep(30 * 1000);
    }
}

/* ---- the thread ----------------------------------------------------------------------------------------- */
static DWORD WINAPI discord_thread(LPVOID dll) {
#define LOAD(ret, name, args) if (!(p_##name = (ret (*) args)(void *)GetProcAddress((HMODULE)dll, #name))) { fprintf(stderr, "[discord] the SDK has no %s: off\n", #name); return 0; }
    DISCORD_FUNCS(LOAD)
    started = now_s();
    p_Discord_Client_Init(&client);
    p_Discord_Client_SetApplicationId(&client, APP_ID);
    p_Discord_Client_SetStatusChangedCallback(&client, on_status, NULL, NULL);
    p_Discord_Client_SetActivityJoinCallback(&client, on_join, NULL, NULL);
    p_Discord_Client_SetTokenExpirationCallback(&client, on_expiring, NULL, NULL);
    register_launch();
    CreateDirectoryA("config", NULL);
    const char *si = getenv("SSBU_DISCORD_SIGNIN");
    if (discord_token_load(TOKEN_PATH, &tok) && discord_token_fresh(&tok, now_s(), 120)) use_token();
    else if (tok.refresh[0]) refresh_token();
    else if (si && *si == '1') sign_in();
    else give_eos(T_NONE);
    eos_external_token = eos_hook;   /* (set once the token's state is known: EOS waits only while it is T_WORKING) */
    InterlockedExchange(&running, 1);
    HANDLE t = CreateThread(NULL, 0, rank_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    u64 next_presence = 0;
    for (;;) {
        p_Discord_RunCallbacks();
        u64 now = GetTickCount64();
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
    HMODULE dll = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll) { fprintf(stderr, "[discord] discord_partner_sdk.dll did not load (%lu): off\n", GetLastError()); return; }
    HANDLE t = CreateThread(NULL, 0, discord_thread, dll, 0, NULL);
    if (t) CloseHandle(t);
}
int discord_on(void) { return running != 0; }
int discord_signed_in(void) { return connected != 0; }

void discord_note_online(int where, int players, int max_players, int in_match, const char *arena_id, const char *party) {
    AcquireSRWLockExclusive(&lock);
    on.where = where; on.players = players; on.max = max_players; on.in_match = in_match;
    snprintf(on.arena, sizeof on.arena, "%s", arena_id ? arena_id : "");
    snprintf(on.party, sizeof on.party, "%s", party ? party : "");
    ReleaseSRWLockExclusive(&lock);
}
void discord_note_room(int in_room, int players, int max_players, int hosting, const char *host_id) {
    AcquireSRWLockExclusive(&lock);
    room.in = in_room; room.players = players; room.max = max_players; room.hosting = hosting;
    snprintf(room.host, sizeof room.host, "%s", host_id ? host_id : "");
    ReleaseSRWLockExclusive(&lock);
}
void discord_note_fighter(int kind, uint32_t value) {
    AcquireSRWLockExclusive(&lock);
    fighter = kind; gsp = value;
    ReleaseSRWLockExclusive(&lock);
}
void discord_note_lobby_code(const char *c) {
    AcquireSRWLockExclusive(&lock);
    snprintf(code, sizeof code, "%s", c ? c : "");
    ReleaseSRWLockExclusive(&lock);
}
int discord_take_join(DiscordJoin *j) {
    AcquireSRWLockExclusive(&lock);
    int has = join_has;
    if (has) { *j = join_pending; join_has = 0; }
    ReleaseSRWLockExclusive(&lock);
    return has;
}

#else   /* (Linux, macOS: the SDK has their libraries; loading them comes with EOS's, plan phase 5) */
void discord_start(void) {}
int discord_on(void) { return 0; }
int discord_signed_in(void) { return 0; }
void discord_note_online(int where, int players, int max_players, int in_match, const char *arena_id, const char *party) {
    (void)where; (void)players; (void)max_players; (void)in_match; (void)arena_id; (void)party;
}
void discord_note_room(int in_room, int players, int max_players, int hosting, const char *host_id) {
    (void)in_room; (void)players; (void)max_players; (void)hosting; (void)host_id;
}
void discord_note_fighter(int kind, uint32_t value) { (void)kind; (void)value; }
void discord_note_lobby_code(const char *c) { (void)c; }
int discord_take_join(DiscordJoin *j) { (void)j; return 0; }
#endif
