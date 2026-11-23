/* Each match this game plays online, for a lobby's count of matches played (net_results.h): POST /match/played on the
 * lobby's TCP port (tools/nex/matches.py), signed with the install's NSA id as GET /gsp/me is (the first time with the
 * NSA id itself, which makes the player known there) - Quickplay, Battle Arenas and Local Wireless rooms. A report the
 * lobby does not take (an older lobby: 404) is dropped; one it can not be asked now is tried again a few times.
 *
 * The lobby: the official lobby the game plays on (net_relay_address() one of the official lobby's names: its lobby or a
 * private room on its relay), at its own TCP port as GET /gsp/me (net_discord.c). The official lobby counts a Quickplay
 * match itself (DataStore 21: tools/nex/board.py meta_posted, which refuses the report of one, "via": "lobby"); Battle
 * Arenas (which post no DataStore 21) and rooms are counted from these reports alone, so without them the counter of
 * those modes stayed at 0. A game on a server of the player's own, a LAN game, or the host of a private room (no relay)
 * sends nothing: the report's NSA id key goes to an official lobby only. SSBU_RESULTS_LOBBY=<host:port> names another
 * lobby instead (tests: a local one); =off sends nothing. Before OpenSmash 0.5.0 this thread also sent a game's fighter records
 * and ranked statistics to the lobby (POST /gsp/sync, /gsp/link, GET /gsp/records) when its game server was its own,
 * Online over Epic Online Services: gone with EOS.
 *
 * OpenSmash mastery points (SSBU_MASTERY_REPORT, on by default; =0 the report as before): the data gets a "mastery"
 * object - this game's slot, how the match ended, the stocks it started with, each player's fighter (its ui_chara_db
 * name without "ui_chara_"), stocks left and place - and the request the account's lobby ticket as X-OpenSmash-Ticket
 * (as GET /gsp/me sends it: to an official lobby only). The lobby computes the points; the game never says one. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "net_results.h"
#include "net_discord_core.h"

static uint64_t mix64(uint64_t z) {
    z += 0x9e3779b97f4a7c15ull;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
uint64_t results_match_id_seed(uint64_t seed) { return mix64(seed ^ 0x6d61746368736564ull); }
uint64_t results_match_id_addresses(const uint32_t *ips, int n, int mode) {
    uint32_t s[16];
    if (n > 16) n = 16;
    for (int i = 0; i < n; i++) {   /* (sorted: every game lists them in its own order) */
        uint32_t v = ips[i];
        int j = i;
        for (; j > 0 && s[j - 1] > v; j--) s[j] = s[j - 1];
        s[j] = v;
    }
    uint64_t h = mix64(0x6164647265737365ull ^ (uint8_t)mode);
    for (int i = 0; i < n; i++)
        if (!i || s[i] != s[i - 1]) h = mix64(h ^ s[i]);
    return h;
}
int results_mastery_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("SSBU_MASTERY_REPORT"); on = !(e && *e == '0'); }
    return on;
}
static uint64_t hash40(const char *s) {   /* (the game's hash40: CRC-32 of the text, its length above) */
    uint32_t c = 0xffffffffu;
    size_t n = 0;
    for (; s[n]; n++) {
        c ^= (uint8_t)s[n];
        for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xedb88320u & (0u - (c & 1)));
    }
    return (uint64_t)n << 32 | (uint32_t)~c;
}
int results_fighter_key(uint64_t ui_chara, char *out, size_t cap) {
    if (cap) out[0] = 0;
    uint64_t want = ui_chara & 0xffffffffffull;
    for (int i = 0; discord_fighter_key_at(i); i++) {
        const char *k = discord_fighter_key_at(i);
        char name[48];
        snprintf(name, sizeof name, "ui_chara_%s", k);
        if (hash40(name) == want && strlen(k) < cap) { snprintf(out, cap, "%s", k); return 1; }
    }
    return 0;
}
/* the "mastery" object, appended at out (cap left): its length, 0 when it does not fit */
static size_t mastery_json(const MatchMastery *x, char *out, size_t cap) {
    static const char *const END[3] = {"normal", "time", "left"};
    int n = x->n < 0 ? 0 : x->n > MASTERY_PLAYERS ? MASTERY_PLAYERS : x->n;
    size_t k = 0;
    int w = snprintf(out, cap, ",\"mastery\":{\"v\":1,\"me\":%d,\"end\":\"%s\",\"left\":%d,\"stock_start\":%d,\"players\":[",
                     x->me, END[x->end >= 0 && x->end <= 2 ? x->end : 2], x->left < 0 || x->left >= n ? -1 : x->left,
                     x->stock_start < 0 ? 0 : x->stock_start > 99 ? 99 : x->stock_start);
    if (w <= 0 || (size_t)w >= cap) return 0;
    k = (size_t)w;
    for (int i = 0; i < n; i++) {
        char key[24] = "";
        for (size_t j = 0; x->p[i].fighter[j] && j + 1 < sizeof key; j++) {   /* (a ui_chara_db name's characters only) */
            char ch = x->p[i].fighter[j];
            if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) { key[0] = 0; break; }
            key[j] = ch; key[j + 1] = 0;
        }
        int st = x->p[i].stocks < 0 ? 0 : x->p[i].stocks > 99 ? 99 : x->p[i].stocks;
        int pl = x->p[i].place < 0 || x->p[i].place > n ? 0 : x->p[i].place;
        w = snprintf(out + k, cap - k, "%s{\"slot\":%d,\"fighter\":\"%s\",\"stocks\":%d,\"place\":%d}", i ? "," : "", i, key, st, pl);
        if (w <= 0 || (size_t)w >= cap - k) return 0;
        k += (size_t)w;
    }
    w = snprintf(out + k, cap - k, "]}");
    if (w <= 0 || (size_t)w >= cap - k) return 0;
    return k + (size_t)w;
}
size_t results_match_data(const MatchPlayed *m, char *out, size_t cap) {
    int players = m->players < 2 ? 2 : m->players > 8 ? 8 : m->players;
    int seconds = m->seconds < 0 ? 0 : m->seconds > 3600 ? 3600 : m->seconds;
    const char *mode = m->mode == 14 ? "quickplay" : m->mode == 20 ? "arena" : "room";
    int n = snprintf(out, cap, "{\"id\":\"%016llx\",\"mode\":\"%s\",\"players\":%d,\"seconds\":%d,\"ranked\":%s,\"net\":\"%s\","
                               "\"via\":\"%s\",\"exact\":%s",
                     (unsigned long long)m->id, mode, players, seconds, m->mode == 14 ? "true" : "false",
                     m->rollback ? "rollback" : "game", "lobby", m->exact ? "true" : "false");   /* ("via": "eos" was EOS's) */
    if (n <= 0 || (size_t)n >= cap) return 0;
    if (m->mastery.have && m->mastery.n >= 1) {
        size_t k = mastery_json(&m->mastery, out + n, cap - (size_t)n);
        if (!k) return 0;
        n += (int)k;
    }
    if ((size_t)n + 1 >= cap) return 0;
    out[n++] = '}';
    out[n] = 0;
    return (size_t)n;
}

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

int account_nsa_hex(char out[17]);   /* (hle_env.c) */
const char *net_relay_address(void);           /* (net.h, net_relay.c) */
int net_means_official_lobby(const char *host);
const char *net_official_lobby(void);
#define MATCH_QUEUE 16
#define MATCH_RETRY_MS (10 * 1000)
#define MATCH_TRIES 6

static HANDLE wake;
static wchar_t lobby_host[256];   /* (the results' thread's alone) the lobby a report goes to now */
static INTERNET_PORT lobby_port;
static int lobby_fixed;           /* SSBU_RESULTS_LOBBY named it: lobby_host:lobby_port always */
static SRWLOCK match_lock = SRWLOCK_INIT;
static MatchPlayed match_q[MATCH_QUEUE];
static int match_n;

static int lobby_official;        /* (the results' thread's alone) lobby_host is an official lobby: the ticket may go there */
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

/* POST /match/played: 1 done with it (taken, or not to be sent again), 0 not now */
static int send_match(const char *nsa, const char *player, const MatchPlayed *m) {
    char data[1536], body[4096], head[640] = "Content-Type: application/json\r\n", ticket[400];
    if (!results_match_data(m, data, sizeof data)) return 1;
    /* (mastery: the account's ticket, as GET /gsp/me sends it - to an official lobby only, it is a credential) */
    if (m->mastery.have && lobby_official && discord_lobby_ticket(ticket, sizeof ticket))
        snprintf(head, sizeof head, "Content-Type: application/json\r\nX-OpenSmash-Ticket: %s\r\n", ticket);
    for (int with_key = 0; with_key < 2; with_key++) {
        long long t = (long long)time(NULL);
        char sig[65], key[40] = "", *at = body;
        discord_sign(nsa, "match", player, t, data, sig);
        if (with_key) snprintf(key, sizeof key, ",\"key\":\"%s\"", nsa);
        at += snprintf(body, sizeof body, "{\"player\":\"%s\",\"t\":%lld,\"sig\":\"%s\"%s,\"data\":", player, t, sig, key);
        json_string(at, sizeof body - (size_t)(at - body) - 2, data);
        strcat(body, "}");
        DWORD status;
        char *answer = http(L"POST", L"/match/played", head, body, &status);
        int unknown = status == 403 && answer && strstr(answer, "unknown player");
        fprintf(stderr, "[results] match played (%s)%s: the lobby answered HTTP %lu%s%s\n", data, strstr(head, "Ticket") ? " with the account's ticket" : "",
                status, answer ? ": " : "", answer ? answer : "");
        free(answer);
        if (unknown) continue;   /* (once more, with the NSA id that makes the player known there) */
        return status != 0 && status < 500;   /* (taken, already counted, refused, or a lobby without the count: 404) */
    }
    return 1;
}

/* "host:port" (with default_port, "host" alone too) into lobby_host and lobby_port: 1 when it is one */
static int set_lobby(const char *where, int default_port) {
    char host[256];
    snprintf(host, sizeof host, "%s", where);
    char *colon = strrchr(host, ':');
    int port = colon ? atoi(colon + 1) : default_port;
    if (colon == host || !host[0] || port <= 0 || port > 65535) return 0;
    if (colon) *colon = 0;
    if (!MultiByteToWideChar(CP_UTF8, 0, host, -1, lobby_host, 256)) return 0;
    lobby_port = (INTERNET_PORT)port;
    return 1;
}
/* (no SSBU_RESULTS_LOBBY) the official lobby the game plays on now, into lobby_host and lobby_port: 1 when there is
 * one; asked here, on this thread (the official lobby's name may need the region decided: net_region.c) */
static int official_target(void) {
    const char *relay = net_relay_address();
    if (!relay || !net_means_official_lobby(relay)) {
        fprintf(stderr, "[results] a match not sent: the game plays on no official lobby (%s)\n", relay ? relay : "no relay");
        return 0;
    }
    const char *lobby = net_official_lobby();
    if (lobby && set_lobby(lobby, 39420)) { lobby_official = 1; return 1; }   /* (NET_DEFAULT_PORT: the relay's own when the name has none) */
    fprintf(stderr, "[results] a match not sent: the official lobby's name (%s) is not host:port\n", lobby ? lobby : "none");
    return 0;
}

/* the queued reports, oldest first, until one can not be sent now (tried again MATCH_RETRY_MS on, MATCH_TRIES times) */
static void send_matches(const char *nsa, const char *player, uint64_t now, uint64_t *match_at, int *tries) {
    for (;;) {
        MatchPlayed m;
        AcquireSRWLockExclusive(&match_lock);
        int have = match_n > 0;
        if (have) m = match_q[0];
        ReleaseSRWLockExclusive(&match_lock);
        if (!have || now < *match_at) return;
        if (lobby_fixed) {   /* (SSBU_RESULTS_LOBBY: the ticket only when that lobby is the official one - a test's override) */
            const char *v = getenv("SSBU_RESULTS_LOBBY");
            lobby_official = v && net_means_official_lobby(v);
        }
        int done = (!lobby_fixed && !official_target()) || send_match(nsa, player, &m) || ++*tries >= MATCH_TRIES;
        if (!done) { *match_at = now + MATCH_RETRY_MS; return; }
        *tries = 0;
        AcquireSRWLockExclusive(&match_lock);
        if (match_n > 0) memmove(match_q, match_q + 1, (size_t)--match_n * sizeof *match_q);
        ReleaseSRWLockExclusive(&match_lock);
    }
}

static DWORD WINAPI results_thread(LPVOID arg) {
    (void)arg;
    char nsa[17], player[17];
    while (!account_nsa_hex(nsa)) Sleep(2000);
    discord_player_id(nsa, player);
    int match_tries = 0;
    uint64_t match_at = 0;
    for (;;) {
        uint64_t t = GetTickCount64();
        send_matches(nsa, player, t, &match_at, &match_tries);
        AcquireSRWLockExclusive(&match_lock);
        int waiting = match_n;
        ReleaseSRWLockExclusive(&match_lock);
        WaitForSingleObject(wake, waiting ? 2000 : 60000);
    }
}

/* (once) the lobby and the thread: 1 running, 0 none (SSBU_RESULTS_LOBBY=off or not host:port) */
static int thread_start(void) {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    static int running;
    BOOL pending;
    if (!InitOnceBeginInitialize(&once, 0, &pending, NULL)) return 0;
    if (!pending) return running;
    const char *v = getenv("SSBU_RESULTS_LOBBY");
    int start = 1;   /* (unset or "auto": the official lobby the game plays on - see the top of this file) */
    if (v && *v && !_stricmp(v, "off")) start = 0;
    else if (v && *v && _stricmp(v, "auto") && !(lobby_fixed = set_lobby(v, 0))) {
        fprintf(stderr, "[results] SSBU_RESULTS_LOBBY=%s: not host:port\n", v);
        start = 0;
    }
    if (start) {
        wake = CreateEventA(NULL, FALSE, FALSE, NULL);
        HANDLE th = wake ? CreateThread(NULL, 0, results_thread, NULL, 0, NULL) : NULL;
        if (th) { CloseHandle(th); running = 1; }
        fprintf(stderr, "[results] %s%s hears of this game's matches%s\n", lobby_fixed ? "the lobby " : "the official lobby it plays on",
                lobby_fixed ? v : "", running ? "" : " (no thread: nothing sent)");
    }
    InitOnceComplete(&once, 0, NULL);
    return running;
}

void results_match_played(const MatchPlayed *m) {
    if (!thread_start()) return;   /* (on no official lobby, nothing to send it to: dropped by the thread) */
    AcquireSRWLockExclusive(&match_lock);
    if (match_n == MATCH_QUEUE) memmove(match_q, match_q + 1, (size_t)--match_n * sizeof *match_q);   /* (the oldest out) */
    match_q[match_n] = *m;
    if (!results_mastery_on()) match_q[match_n].mastery.have = 0;   /* (SSBU_MASTERY_REPORT=0: the report as before) */
    match_n++;
    ReleaseSRWLockExclusive(&match_lock);
    SetEvent(wake);
}

#else
void results_match_played(const MatchPlayed *m) { (void)m; }
#endif
