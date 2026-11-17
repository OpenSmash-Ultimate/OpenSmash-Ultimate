/* The players' results to the public lobby (net_results.h). The owner's design (2026-11-16): Epic's services for
 * everyone, the one public lobby (Europe) the collector of results - every player's GSP per fighter and ranked
 * statistics, its /leaderboard.json and /status.json - and the fallback when EOS is not there.
 *
 * Over EOS the game server is the game's own (hle_online.c), so the lobby never sees its player log in. This thread
 * sends the lobby what the player's game server keeps of its player (POST /gsp/sync on the lobby's TCP port,
 * tools/nex/board.py, signed with the install's NSA id as GET /gsp/me is; the first time with the NSA id itself, which
 * makes the player known there - as the NEX login does on the lobby's own game server), each time it changes, and at
 * the start takes the player's records back (GET /gsp/records): the GSP a player earned on another PC is the one this
 * game shows. Records still to be sent when the game closed (the marker file) go first, before anything is taken.
 * The Discord account a player signed in with (config/discord_token.txt) is linked on the lobby (POST /gsp/link, its
 * access token, which the lobby checks with Discord once): GSP and place then belong to that account, merged from every
 * PC it plays on (each fighter's best), and only linked accounts are on the leaderboard (config/discord_leaderboard.txt
 * "off" hides one); everyone else plays all the same, unranked.
 *
 * SSBU_RESULTS_LOBBY=<host:port> sends them to another lobby (tests: a local one); =off sends nothing. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "net_results.h"
#include "net_discord_core.h"

static int hexval(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
int results_parse_records(const char *json, uint8_t *out, int size, int max) {
    const char *p = json ? strstr(json, "\"records\"") : NULL;
    if (!p || !(p = strchr(p, '['))) return 0;
    int n = 0;
    for (p++; *p && *p != ']' && n < max;) {
        if (*p != '"') { p++; continue; }
        const char *q = ++p;
        while (*q && *q != '"') q++;
        if (!*q) break;
        if (q - p == size * 2) {
            int ok = 1;
            for (int i = 0; i < size && ok; i++) {
                int a = hexval(p[2 * i]), b = hexval(p[2 * i + 1]);
                if (a < 0 || b < 0) ok = 0;
                else out[n * size + i] = (uint8_t)(a << 4 | b);
            }
            if (ok) n++;
        }
        p = q + 1;
    }
    return n;
}

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include "net_nex.h"

int account_nsa_hex(char out[17]);   /* (hle_env.c) */
#define LOBBY "opensmash-lobby.duckdns.org:39421"   /* (the one public lobby: net_relay.c) */
#define RETRY_MS (30 * 1000)
#define PROFILE_EVERY_MS (30 * 60 * 1000)

static char marker_path[1024];
static HANDLE wake;
static volatile LONG dirty;
static wchar_t lobby_host[256];
static INTERNET_PORT lobby_port;

static char *http(const wchar_t *verb, const wchar_t *path, const char *headers, const char *body, DWORD *status) {
    HINTERNET s = WinHttpOpen(L"OpenSmash", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0), c = NULL, r = NULL;
    char *buf = NULL;
    size_t got = 0, cap = 256 * 1024;
    *status = 0;
    if (!s) return NULL;
    wchar_t wh[1024] = L"";
    if (headers) MultiByteToWideChar(CP_UTF8, 0, headers, -1, wh, 1024);
    WinHttpSetTimeouts(s, 5000, 5000, 8000, 10000);
    DWORD blen = body ? (DWORD)strlen(body) : 0;
    if ((c = WinHttpConnect(s, lobby_host, lobby_port, 0)) &&
        (r = WinHttpOpenRequest(c, verb, path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0)) &&
        WinHttpSendRequest(r, headers ? wh : WINHTTP_NO_ADDITIONAL_HEADERS, headers ? (DWORD)-1L : 0, (void *)body, blen, blen, 0) &&
        WinHttpReceiveResponse(r, NULL)) {
        DWORD sl = sizeof *status;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, status, &sl, WINHTTP_NO_HEADER_INDEX);
        if ((buf = malloc(cap + 1))) {
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

static void json_string(char *out, size_t cap, const char *s) {   /* a JSON string, quotes included */
    size_t n = 0;
    if (cap < 3) { if (cap) out[0] = 0; return; }
    out[n++] = '"';
    for (; *s && n + 8 < cap; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') { out[n++] = '\\'; out[n++] = (char)ch; }
        else if (ch < 0x20) n += (size_t)snprintf(out + n, cap - n, "\\u%04x", ch);
        else out[n++] = (char)ch;
    }
    out[n++] = '"';
    out[n] = 0;
}

/* POST /gsp/sync: 1 sent (or nothing to send), 0 not now */
static int send_results(const char *nsa, const char *player) {
    static char data[48 * 1024], body[100 * 1024];
    size_t n = online_results_snapshot(data, sizeof data);
    if (!n) return 1;
    for (int with_key = 0; with_key < 2; with_key++) {
        long long t = (long long)time(NULL);
        char sig[65], key[40] = "";
        discord_sign(nsa, "sync", player, t, data, sig);
        if (with_key) snprintf(key, sizeof key, ",\"key\":\"%s\"", nsa);
        char *at = body;
        at += snprintf(body, sizeof body, "{\"player\":\"%s\",\"t\":%lld,\"sig\":\"%s\"%s,\"data\":", player, t, sig, key);
        json_string(at, sizeof body - (size_t)(at - body) - 2, data);
        strcat(body, "}");
        DWORD status;
        char *answer = http(L"POST", L"/gsp/sync", "Content-Type: application/json\r\n", body, &status);
        int unknown = status == 403 && answer && strstr(answer, "unknown player");
        if (status == 200) fprintf(stderr, "[results] sent to the lobby: %s\n", answer ? answer : "");
        else if (!unknown) fprintf(stderr, "[results] the lobby did not take them (HTTP %lu%s%s)\n", status, answer ? ": " : "", answer ? answer : "");
        free(answer);
        if (status == 200) return 1;
        if (!unknown) return 0;   /* (unknown: once more, with the NSA id that makes the player known there) */
    }
    return 0;
}

/* GET /gsp/records: the player's records as the lobby keeps them, to the game server; -1 not now */
static int fetch_records(const char *nsa, const char *player) {
    long long t = (long long)time(NULL);
    char sig[65], head[512];
    discord_sign(nsa, "records", player, t, "", sig);
    snprintf(head, sizeof head, "X-OpenSmash-Player: %s\r\nX-OpenSmash-Time: %lld\r\nX-OpenSmash-Sig: %s\r\n", player, t, sig);
    DWORD status;
    char *answer = http(L"GET", L"/gsp/records", head, NULL, &status);
    int n = -1;
    if (status == 200 && answer) {
        static uint8_t recs[NEX_POWER_RECORD * 256];
        n = results_parse_records(answer, recs, NEX_POWER_RECORD, 256);
        online_results_fetched(recs, n);
        fprintf(stderr, "[results] %d fighter record(s) of this player from the lobby\n", n);
    } else if (status == 403) n = 0;   /* (not known there yet: nothing to take) */
    else fprintf(stderr, "[results] the lobby's records not read (HTTP %lu)\n", status);
    free(answer);
    return n;
}

/* POST /gsp/link: the Discord account the player signed in with (config/discord_token.txt), proven to the lobby by its
 * access token (the lobby asks Discord who it is, once): the player's GSP and place belong to that account on every PC.
 * show: on the leaderboard unless config/discord_leaderboard.txt says "off". 1 linked (or nothing to link), 0 retry. */
static int send_link(const char *nsa, const char *player) {
    DiscordToken tok;
    if (!discord_token_load("config\\" DISCORD_TOKEN_FILE, &tok) || !tok.user_id[0]) return 1;   /* (not signed in) */
    if (!discord_token_fresh(&tok, (int64_t)time(NULL), 60)) return 0;   /* (net_discord.c refreshes it: later) */
    char choice[16] = "";
    FILE *f = fopen("config\discord_leaderboard.txt", "rb");
    if (f) { size_t k = fread(choice, 1, sizeof choice - 1, f); choice[k] = 0; fclose(f); }
    int show = _strnicmp(choice, "off", 3) != 0;
    static char payload[600], body[1400];
    char sig[65], jt[600];
    snprintf(payload, sizeof payload, "%s\n%d", tok.access, show);
    json_string(jt, sizeof jt, tok.access);
    DWORD status = 0;
    for (int with_key = 0; with_key < 2; with_key++) {   /* (the NSA id only for a lobby that does not know the player) */
        long long t = (long long)time(NULL);
        char key[40] = "";
        discord_sign(nsa, "link", player, t, payload, sig);
        if (with_key) snprintf(key, sizeof key, ",\"key\":\"%s\"", nsa);
        snprintf(body, sizeof body, "{\"player\":\"%s\",\"t\":%lld,\"token\":%s,\"show\":%s,\"sig\":\"%s\"%s}", player, t, jt, show ? "true" : "false", sig, key);
        char *answer = http(L"POST", L"/gsp/link", "Content-Type: application/json\r\n", body, &status);
        int unknown = status == 403 && answer && strstr(answer, "unknown player");
        free(answer);
        if (!unknown) break;
    }
    fprintf(stderr, "[results] Discord account linked on the lobby: %s\n", status == 200 ? (show ? "yes, on the leaderboard" : "yes, hidden") : "not now");
    return status == 200 || status == 403;   /* (403: a token Discord refused - not again until the next period) */
}

static DWORD WINAPI results_thread(LPVOID arg) {
    (void)arg;
    char nsa[17], player[17];
    while (!account_nsa_hex(nsa)) Sleep(2000);
    discord_player_id(nsa, player);
    int fetched = 0;
    uint64_t profile_at = 0, retry_at = 0;
    if (GetFileAttributesA(marker_path) != INVALID_FILE_ATTRIBUTES) InterlockedExchange(&dirty, 1);   /* (left from the last start) */
    for (;;) {
        uint64_t t = GetTickCount64();
        if (dirty && t >= retry_at) {
            InterlockedExchange(&dirty, 0);
            DeleteFileA(marker_path);   /* (before: a change while sending makes it again) */
            if (!send_results(nsa, player)) {
                FILE *f = fopen(marker_path, "wb");
                if (f) fclose(f);
                InterlockedExchange(&dirty, 1);
                retry_at = t + RETRY_MS;
            }
        }
        if (!dirty && t >= profile_at)   /* (before the records: a linked account's GSP is the account's, from every PC) */
            profile_at = t + (send_link(nsa, player) ? PROFILE_EVERY_MS : RETRY_MS);
        if (!fetched && !dirty && t >= retry_at) {   /* (the lobby's records once ours are all there) */
            if (fetch_records(nsa, player) >= 0) fetched = 1;
            else retry_at = t + RETRY_MS;
        }
        WaitForSingleObject(wake, dirty || !fetched ? 2000 : 60000);
    }
}

void results_start(const char *marker) {
    static LONG once;
    if (InterlockedExchange(&once, 1)) return;
    const char *v = getenv("SSBU_RESULTS_LOBBY");
    char where[256];
    snprintf(where, sizeof where, "%s", v && *v ? v : LOBBY);
    if (!_stricmp(where, "off")) return;
    char *colon = strrchr(where, ':');
    int port = colon ? atoi(colon + 1) : 0;
    if (!colon || port <= 0 || port > 65535) { fprintf(stderr, "[results] SSBU_RESULTS_LOBBY=%s: not host:port\n", where); return; }
    *colon = 0;
    MultiByteToWideChar(CP_UTF8, 0, where, -1, lobby_host, 256);
    lobby_port = (INTERNET_PORT)port;
    snprintf(marker_path, sizeof marker_path, "%s", marker);
    wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    HANDLE th = CreateThread(NULL, 0, results_thread, NULL, 0, NULL);
    if (th) CloseHandle(th);
    fprintf(stderr, "[results] the player's results go to the lobby %s:%d\n", where, port);
}

void results_changed(void) {
    if (!wake) return;
    if (!InterlockedExchange(&dirty, 1)) {
        FILE *f = fopen(marker_path, "wb");   /* (sent at the next start if not now) */
        if (f) fclose(f);
    }
    SetEvent(wake);
}
#else
void results_start(const char *marker) { (void)marker; }
void results_changed(void) {}
#endif
