/* The server region (net.h): which of OpenSmash's official lobbies this game plays on.
 *
 * config/server_region.txt says "auto" (the default) or a region's id. Auto asks every region's lobby at once, a few
 * times each, and takes the one that answers soonest (the lowest median); the pick is kept in
 * config/server_region_auto.txt, so the next start does not wait: it plays on the last pick and measures again in the
 * background, for the start after it. With no pick kept, the first start measures before it joins (under a second).
 * No answer from any region: the first of the table. An override (SSBU_OFFICIAL_LOBBY, config/official_lobby.txt)
 * is that server: no regions, nothing measured.
 *
 * The question is a JOIN of netplay version 2 with no lobby ticket: an official lobby lets nobody in without one and
 * says so at once (REJECT), which is all that is timed; nothing of the player is in it (the name is "ping"). A lobby
 * that does let it in (a test lobby) is left at once. A lobby answers a stranger's address a few times a second at
 * most: 3 questions a start are far under it. */
#define _CRT_SECURE_NO_WARNINGS
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/select.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define closesocket close
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "host.h"
#include "net.h"
#include "settings.h"

const NetRegion NET_REGIONS[NET_REGION_COUNT] = {NET_REGION_TABLE};

#define REGION_SAMPLES 3
#define REGION_SAMPLE_WAIT_MS 350   /* one question's answer */
#define REGION_FIRST_WAIT_MS 800    /* a start with no pick kept: all of the measuring */

/* ---- the table, the choice, the pick (no network) ---------------------------------------------------------- */
int net_region_index(const char *id) {
    for (int i = 0; id && i < NET_REGION_COUNT; i++)
        if (!_stricmp(id, NET_REGIONS[i].id)) return i;
    return -1;
}

int net_region_of_lobby(const char *lobby) {
    for (int i = 0; lobby && *lobby && i < NET_REGION_COUNT; i++) {
        const char *l = NET_REGIONS[i].lobby, *colon = strrchr(l, ':');
        size_t host = colon ? (size_t)(colon - l) : strlen(l);
        if (!_stricmp(lobby, l) || (strlen(lobby) == host && !_strnicmp(lobby, l, host))) return i;
    }
    return -1;
}

int net_region_median(const int *ms, int n) {
    int v[16], k = 0;
    for (int i = 0; i < n && k < 16; i++)
        if (ms[i] >= 0) {
            int j = k++;
            for (; j > 0 && v[j - 1] > ms[i]; j--) v[j] = v[j - 1];
            v[j] = ms[i];
        }
    return !k ? -1 : k & 1 ? v[k / 2] : (v[k / 2 - 1] + v[k / 2]) / 2;
}

int net_region_pick(const int *ms, int n, int kept) {
    int best = -1;
    for (int i = 0; i < n; i++)
        if (ms[i] >= 0 && (best < 0 || ms[i] < ms[best])) best = i;   /* (the same time: the earlier row) */
    if (best >= 0 && kept >= 0 && kept < n && ms[kept] >= 0 && ms[kept] - ms[best] < NET_REGION_KEEP_MS) return kept;
    return best;
}

int net_region_auto_parse(const char *line, int *ms) {
    char id[32] = "";
    int t = -1;
    if (!line || sscanf(line, "%31s %d", id, &t) < 1) return -1;
    if (ms) *ms = t >= 0 && t < 60000 ? t : -1;
    return net_region_index(id);
}

/* (a clock of its own: the tests build this file without the runtime) */
static u64 region_now_ns(void) {
#ifdef _WIN32
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (u64)(c.QuadPart / f.QuadPart) * 1000000000ull + (u64)(c.QuadPart % f.QuadPart) * 1000000000ull / (u64)f.QuadPart;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (u64)t.tv_sec * 1000000000ull + (u64)t.tv_nsec;
#endif
}

/* ---- measuring --------------------------------------------------------------------------------------------- */
typedef struct RegionBatch { volatile LONG ms[NET_REGION_COUNT], done[NET_REGION_COUNT], refs; } RegionBatch;
typedef struct RegionAsk { RegionBatch *b; int region; } RegionAsk;

static int region_test_pings(int ms[NET_REGION_COUNT]) {   /* SSBU_REGION_TEST_PINGS: 1 when set */
    const char *e = getenv("SSBU_REGION_TEST_PINGS");
    if (!e || !*e) return 0;
    for (int i = 0; i < NET_REGION_COUNT; i++) {
        ms[i] = -1;
        size_t n = strlen(NET_REGIONS[i].id);
        for (const char *p = e; p && *p; p = strchr(p, ',') ? strchr(p, ',') + 1 : NULL)
            if (!_strnicmp(p, NET_REGIONS[i].id, n) && p[n] == '=') ms[i] = atoi(p + n + 1);
    }
    return 1;
}

static void region_batch_release(RegionBatch *b) {
    if (!InterlockedDecrement(&b->refs)) free(b);
}

static DWORD WINAPI region_ask_thread(LPVOID arg) {
    RegionAsk *a = arg;
    RegionBatch *b = a->b;
    int region = a->region, got[REGION_SAMPLES], n = 0;
    free(a);
    char host[256];
    snprintf(host, sizeof host, "%s", NET_REGIONS[region].lobby);
    char *colon = strrchr(host, ':');
    int port = colon ? atoi(colon + 1) : 0;
    if (colon) *colon = 0;
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    SOCKET s = INVALID_SOCKET;
    if (port > 0 && port < 65536 && !getaddrinfo(host, NULL, &hints, &res) && res) {
        struct sockaddr_in at = *(struct sockaddr_in *)res->ai_addr;
        at.sin_port = htons((unsigned short)port);
        s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        u8 join[4 + 8 + 32 + 32 + 40] = {'S', 'N', NET_PROTOCOL, 1 /* JOIN */};
        u64 nonce = region_now_ns() * 0x9E3779B97F4A7C15ull ^ (u64)(uintptr_t)&at ^ (u64)GetTickCount64() << 20 ^ (u64)region << 60;
        memcpy(join + 4, &nonce, 8);
        memcpy(join + 12, "smash", 5);
        memcpy(join + 12 + 32, "ping", 4);
        for (int i = 0; s != INVALID_SOCKET && i < REGION_SAMPLES; i++) {
            u64 sent = region_now_ns(), until = sent + REGION_SAMPLE_WAIT_MS * 1000000ull;
            int answered = 0;
            if (sendto(s, (const char *)join, sizeof join, 0, (struct sockaddr *)&at, sizeof at) < 0) break;
            while (!answered) {
                u64 now = region_now_ns();
                if (now >= until) break;
                fd_set r;
                FD_ZERO(&r);
                FD_SET(s, &r);
                struct timeval tv = {0, (long)((until - now) / 1000)};
                if (select((int)s + 1, &r, NULL, NULL, &tv) <= 0) break;
                u8 p[512];
                struct sockaddr_in from;
                socklen_t fl = sizeof from;
                int len = (int)recvfrom(s, (char *)p, sizeof p, 0, (struct sockaddr *)&from, &fl);
                if (len < 0) break;   /* (Windows: "port unreachable" came back: nothing listens there) */
                if (len < 12 || from.sin_addr.s_addr != at.sin_addr.s_addr || p[0] != 'S' || p[1] != 'N' || memcmp(p + 4, &nonce, 8)) continue;
                if (p[3] != 2 /* WELCOME */ && p[3] != 3 /* REJECT */) continue;
                answered = 1;
                if (p[3] == 2) {   /* (a lobby that lets anyone in: leave it) */
                    u8 leave[12] = {'S', 'N', NET_PROTOCOL, 8 /* LEAVE */};
                    memcpy(leave + 4, &nonce, 8);
                    sendto(s, (const char *)leave, sizeof leave, 0, (struct sockaddr *)&at, sizeof at);
                    i = REGION_SAMPLES;   /* (one time is enough there) */
                }
            }
            if (!answered) { if (!n && i) break; continue; }   /* (nothing twice: not there) */
            got[n++] = (int)((region_now_ns() - sent + 500000) / 1000000);
            InterlockedExchange(&b->ms[region], net_region_median(got, n));
        }
    }
    if (res) freeaddrinfo(res);
    if (s != INVALID_SOCKET) closesocket(s);
    InterlockedExchange(&b->done[region], 1);
    region_batch_release(b);
    return 0;
}

void net_region_measure(int ms[NET_REGION_COUNT], int wait_ms) {
    for (int i = 0; i < NET_REGION_COUNT; i++) ms[i] = -1;
    if (region_test_pings(ms)) return;
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) return;
#endif
    RegionBatch *b = calloc(1, sizeof *b);
    if (!b) return;
    b->refs = 1;
    for (int i = 0; i < NET_REGION_COUNT; i++) {
        b->ms[i] = -1;
        RegionAsk *a = malloc(sizeof *a);
        HANDLE t = NULL;
        if (a) {
            a->b = b;
            a->region = i;
            InterlockedIncrement(&b->refs);
            t = CreateThread(NULL, 0, region_ask_thread, a, 0, NULL);
        }
        if (t) CloseHandle(t);
        else {
            if (a) { free(a); InterlockedDecrement(&b->refs); }
            b->done[i] = 1;
        }
    }
    /* (a name that takes long to look up, a lobby that is slow: not waited for past wait_ms; its thread ends alone) */
    for (u64 until = region_now_ns() + (u64)(wait_ms > 0 ? wait_ms : 0) * 1000000ull;;) {
        int all = 1;
        for (int i = 0; i < NET_REGION_COUNT; i++) all &= b->done[i] != 0;
        if (all || region_now_ns() >= until) break;
        Sleep(2);
    }
    for (int i = 0; i < NET_REGION_COUNT; i++) ms[i] = (int)b->ms[i];
    region_batch_release(b);
}

/* ---- this start's lobby ------------------------------------------------------------------------------------ */
static SRWLOCK region_lock = SRWLOCK_INIT;
static char region_lobby_now[256];
static int region_overridden, region_auto = -1, region_auto_ms = -1;
static volatile LONG region_remeasuring;

static void region_auto_keep(int region, int ms) {   /* NET_REGION_AUTO_FILE (the folder is the launcher's or hle_config.c's: not made here) */
    FILE *f = fopen(NET_REGION_AUTO_FILE, "wb");   /* (one short line; a line cut short reads as no pick: measured again) */
    if (!f) return;
    fprintf(f, "%s %d %lld\n", NET_REGIONS[region].id, ms, (long long)time(NULL));
    fclose(f);
}

static DWORD WINAPI region_remeasure_thread(LPVOID unused) {
    (void)unused;
    int ms[NET_REGION_COUNT];
    net_region_measure(ms, REGION_SAMPLES * REGION_SAMPLE_WAIT_MS + 3000);
    AcquireSRWLockShared(&region_lock);
    int kept = region_auto;
    ReleaseSRWLockShared(&region_lock);
    int pick = net_region_pick(ms, NET_REGION_COUNT, kept);
    if (pick >= 0) {
        region_auto_keep(pick, ms[pick]);
        AcquireSRWLockExclusive(&region_lock);
        int was = region_auto;
        if (region_auto < 0 || region_auto == pick) { region_auto = pick; region_auto_ms = ms[pick]; }   /* (this start stays where it is) */
        ReleaseSRWLockExclusive(&region_lock);
        if (was >= 0 && was != pick)
            fprintf(stderr, "[net] server region: %s answers sooner now (%d ms): the next start plays there\n", NET_REGIONS[pick].label, ms[pick]);
    }
    InterlockedExchange(&region_remeasuring, 0);
    return 0;
}

static void region_remeasure(void) {
    if (InterlockedExchange(&region_remeasuring, 1)) return;
    HANDLE t = CreateThread(NULL, 0, region_remeasure_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    else InterlockedExchange(&region_remeasuring, 0);
}

int net_region_override(char *out, int cap) {
    const char *e = getenv("SSBU_OFFICIAL_LOBBY");
    char line[256], host[256] = "";
    if (e && *e) snprintf(host, sizeof host, "%s", e);
    else if (settings_line_read("config/official_lobby.txt", line, sizeof line)) sscanf(line, "%255s", host);
    if (host[0]) snprintf(out, (size_t)cap, "%s", host);
    return host[0] != 0;
}

static int region_setting_read(void) {
    char line[64], word[32] = "";
    if (settings_line_read(NET_REGION_FILE, line, sizeof line)) sscanf(line, "%31s", word);
    return net_region_index(word);   /* ("auto", nothing, a word that is no region: auto) */
}

const char *net_region_lobby(void) {
    AcquireSRWLockExclusive(&region_lock);
    if (!region_lobby_now[0]) {
        int region = -1;
        if (net_region_override(region_lobby_now, sizeof region_lobby_now)) region_overridden = 1;
        else if ((region = region_setting_read()) >= 0)
            fprintf(stderr, "[net] server region: %s (%s)\n", NET_REGIONS[region].label, NET_REGIONS[region].lobby);
        else {
            char line[64];
            int kept = settings_line_read(NET_REGION_AUTO_FILE, line, sizeof line) ? net_region_auto_parse(line, &region_auto_ms) : -1;
            if (kept >= 0) {
                region = region_auto = kept;
                fprintf(stderr, "[net] server region: Auto (%s, as the last time; measuring again for the next start)\n", NET_REGIONS[region].label);
                region_remeasure();
            } else {
                int ms[NET_REGION_COUNT];
                net_region_measure(ms, REGION_FIRST_WAIT_MS);
                region = net_region_pick(ms, NET_REGION_COUNT, -1);
                if (region >= 0) {
                    region_auto = region;
                    region_auto_ms = ms[region];
                    region_auto_keep(region, ms[region]);
                    fprintf(stderr, "[net] server region: Auto (%s, %d ms)\n", NET_REGIONS[region].label, ms[region]);
                } else {
                    region = 0;
                    region_auto_ms = -1;
                    fprintf(stderr, "[net] server region: Auto: no lobby answered, %s\n", NET_REGIONS[0].label);
                }
            }
        }
        if (!region_lobby_now[0]) snprintf(region_lobby_now, sizeof region_lobby_now, "%s", NET_REGIONS[region].lobby);
    }
    ReleaseSRWLockExclusive(&region_lock);
    return region_lobby_now;
}

void net_region_use(int region) {
    if (region < 0 || region >= NET_REGION_COUNT) return;
    net_region_lobby();
    AcquireSRWLockExclusive(&region_lock);
    if (!region_overridden) snprintf(region_lobby_now, sizeof region_lobby_now, "%s", NET_REGIONS[region].lobby);
    ReleaseSRWLockExclusive(&region_lock);
}

int net_region_setting(int *override) {
    char o[256];
    if (override) *override = net_region_override(o, sizeof o);
    return region_setting_read();
}

int net_region_auto(int *ms) {
    AcquireSRWLockShared(&region_lock);
    int region = region_auto, t = region_auto_ms;
    ReleaseSRWLockShared(&region_lock);
    if (region < 0) {   /* (a region was chosen at the start, or nothing answered: ask now, shown when it is known) */
        char line[64];
        if (settings_line_read(NET_REGION_AUTO_FILE, line, sizeof line)) region = net_region_auto_parse(line, &t);
        if (region < 0) region_remeasure();
    }
    if (ms) *ms = region >= 0 ? t : -1;
    return region;
}

int net_region_save(int region) {
    return settings_line_write(NET_REGION_FILE, region >= 0 && region < NET_REGION_COUNT ? NET_REGIONS[region].id : "auto");
}
