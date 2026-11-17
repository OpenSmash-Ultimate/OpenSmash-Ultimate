/* Epic Online Services' P2P as the relay of last resort (runtime/net_relay.c: a player no direct path
 * reaches). EOS punches through routers itself and, where that fails, carries the packets through Epic's
 * relays, which are near both players - where the lobby's relay is one fixed server. Players need no
 * Epic account: the game logs in with a Device ID (EOS Connect), made once per Windows user.
 *
 * Nothing of EOS is part of the source: the SDK's library (EOSSDK-Win64-Shipping.dll) is loaded from
 * beside the program when it is there, and its credentials come from eos.txt beside it (ProductId=,
 * SandboxId=, DeploymentId=, ClientId=, ClientSecret=; a release packs both). Without either, or with
 * SSBU_EOS=0, this is off and net_relay.c works as before. The few declarations below are the SDK's C
 * interface (version 1.19), written out here so that the build needs no SDK.
 *
 * Every EOS call is made on this file's own thread (the SDK is not thread-safe): it ticks the SDK every
 * millisecond, sends what eos_send() queued and hands what arrives to net_eos_packet() (net_relay.c).
 * SSBU_EOS_NEWID=1 (tests: a second player on the same PC) makes a new Device ID first; SSBU_EOS_IDFILE=<file> keeps
 * one per file across runs (below). Other files have work done on
 * that thread with eos_post(); P2P channel 1 (reliable, ordered: eos_send_reliable) carries net_eos_lobby.c's messages,
 * channel 0 the game's datagrams. SSBU_EOS_RELAY=force (tests: two games on one PC as over the internet) sends every
 * P2P packet through Epic's relay servers, never straight. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "net_eos.h"
#include "net_eos_sdk.h"
#include "net_nex.h"

#ifdef _WIN32
#include <windows.h>
#pragma comment(lib, "winmm.lib")

#define DECLARE(ret, name, args) ret (EOS_CALL *p_##name) args;
EOS_FUNCS(DECLARE)

EOS_HPlatform eos_plat;
static EOS_HConnect eos_con;
static EOS_HP2P eos_p2p;
EOS_ProductUserId eos_me;
static char eos_my_text[NET_EOS_ID_LEN + 8];
static volatile LONG eos_phase;   /* 0 off, 1 logging in, 2 ready, -1 failed */
static EosSocketId eos_sock = {1, "opensmash"};
static HANDLE eos_wake;

/* eos_send's eos_queue: copies, eos_sent on the EOS thread */
#define QUEUE 256
static struct { char to[NET_EOS_ID_LEN + 1]; u32 len; u8 channel, data[NET_EOS_MAX]; } eos_queue[QUEUE];
static u32 eos_q_head, eos_q_tail;
static SRWLOCK eos_q_lock = SRWLOCK_INIT;
static volatile LONG64 eos_sent, eos_received, eos_dropped;
/* eos_post's: work for the EOS thread, in order */
#define POSTS 1024
static struct { void (*fn)(void *); void *arg; } eos_posts[POSTS];
static u32 eos_p_head, eos_p_tail;

static void EOS_CALL eos_created(const EosCreateUserInfo *d) {
    if (d->ResultCode == EOS_SUCCESS) { eos_me = d->LocalUserId; InterlockedExchange(&eos_phase, 2); }
    else { fprintf(stderr, "[eos] new user: %s\n", p_EOS_EResult_ToString(d->ResultCode)); InterlockedExchange(&eos_phase, -1); }
}
static void EOS_CALL eos_logged_in(const EosLoginInfo *d) {
    if (d->ResultCode == EOS_SUCCESS) { eos_me = d->LocalUserId; InterlockedExchange(&eos_phase, 2); return; }
    if (d->ResultCode == EOS_INVALID_USER) {   /* (this Device ID's first eos_login: a user of the product is made) */
        fprintf(stderr, "[eos] this Device ID has no player yet: a new one is made\n");
        EosCreateUserOptions o = {1, d->ContinuanceToken};
        p_EOS_Connect_CreateUser(eos_con, &o, NULL, eos_created);
        return;
    }
    fprintf(stderr, "[eos] eos_login: %s\n", p_EOS_EResult_ToString(d->ResultCode));
    InterlockedExchange(&eos_phase, -1);
}
static void eos_login(void) {
    static EosConnectCredentials c = {1, NULL, EOS_ECT_DEVICEID_ACCESS_TOKEN};
    static EosUserLoginInfo u = {2, "opensmash", NULL};
    EosLoginOptions o = {2, &c, &u};
    p_EOS_Connect_Login(eos_con, &o, NULL, eos_logged_in);
}
static void EOS_CALL eos_device_id(const EosDeviceIdInfo *d) { (void)d; eos_login(); }   /* (made, or there already) */
/* SSBU_EOS_IDFILE=<file> (tests: two players on one PC that keep who they are from one run to the next, each with its
 * own data root): the SDK keeps the Device ID's credentials per Windows user, in the Windows credential store
 * (generic credentials "eos-sdk:<ProductId>:<n>"). With the file there, its copy goes back into the store before the
 * login; without it, a new Device ID is made (as SSBU_EOS_NEWID=1) and, once logged in, copied into the file. */
#include <wincred.h>
#pragma comment(lib, "advapi32.lib")
#define ID_PIECES 8
static char eos_cred[5][128];   /* (eos.txt's, below) */
static const char *eos_id_file(void) { const char *f = getenv("SSBU_EOS_IDFILE"); return f && *f ? f : NULL; }
static void eos_id_target(wchar_t *t, size_t n, int piece) { _snwprintf(t, n, L"eos-sdk:%hs:%d", eos_cred[0], piece); t[n - 1] = 0; }
static int eos_id_restore(const char *path) {   /* (1: the file's credentials are in the store) */
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    static u8 blob[ID_PIECES][CRED_MAX_CREDENTIAL_BLOB_SIZE];
    u32 head[2];
    int n = 0;
    while (n < ID_PIECES && fread(head, sizeof head, 1, f) == 1 && head[0] >= 1 && head[0] <= ID_PIECES && head[1] <= sizeof blob[0] &&
           fread(blob[n], 1, head[1], f) == head[1]) {
        wchar_t t[96];
        eos_id_target(t, 96, (int)head[0]);
        CREDENTIALW c;
        memset(&c, 0, sizeof c);
        c.Type = CRED_TYPE_GENERIC; c.TargetName = t; c.Comment = L"Epic Online Services SDK";
        c.CredentialBlobSize = head[1]; c.CredentialBlob = blob[n]; c.Persist = CRED_PERSIST_LOCAL_MACHINE;
        if (!CredWriteW(&c, 0)) { fprintf(stderr, "[eos] SSBU_EOS_IDFILE: the store refused piece %u (%lu)\n", head[0], GetLastError()); break; }
        n++;
    }
    fclose(f);
    if (n) fprintf(stderr, "[eos] SSBU_EOS_IDFILE: the Device ID of %s (%d piece%s) put back\n", path, n, n == 1 ? "" : "s");
    return n > 0;
}
static void eos_id_save(const char *path) {   /* (after a login with a new Device ID) */
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "[eos] SSBU_EOS_IDFILE: %s cannot be written\n", path); return; }
    int n = 0;
    for (int piece = 1; piece <= ID_PIECES; piece++) {
        wchar_t t[96];
        eos_id_target(t, 96, piece);
        PCREDENTIALW c;
        if (!CredReadW(t, CRED_TYPE_GENERIC, 0, &c)) continue;
        u32 head[2] = {(u32)piece, c->CredentialBlobSize};
        fwrite(head, sizeof head, 1, f);
        fwrite(c->CredentialBlob, 1, c->CredentialBlobSize, f);
        CredFree(c);
        n++;
    }
    fclose(f);
    fprintf(stderr, "[eos] SSBU_EOS_IDFILE: the new Device ID kept in %s (%d piece%s)\n", path, n, n == 1 ? "" : "s");
}
static int eos_id_new;   /* (a new Device ID was made: eos_id_save once logged in) */
/* The store is one per Windows user, shared by every game on this PC: each game holds this lock from before it reads
 * or changes the store (SSBU_EOS_NEWID, SSBU_EOS_IDFILE) until its login is over, so two games starting together never
 * log in with each other's Device ID. Two games logged in as the same EOS player at once are not two players: EOS
 * hands each P2P connection to that player to one of them only (2026-11-17: in 3 test pairs of 11 the host had taken
 * the store's Device ID while the other test's guest that had made it was still running; the joiner's messages went
 * to that other game and it showed 2618-0521). eos_same_player_check says so in the log. */
static HANDLE eos_store_lock;
static void eos_store_take(void) {
    eos_store_lock = CreateMutexA(NULL, FALSE, "Local\\OpenSmashEosDeviceIdStore");
    if (!eos_store_lock) return;
    DWORD w = WaitForSingleObject(eos_store_lock, 20000);   /* (WAIT_ABANDONED: its holder was killed - ours now) */
    if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED) {
        fprintf(stderr, "[eos] another game of this PC kept the Device ID store 20 s (its login hangs?): logging in without waiting\n");
        CloseHandle(eos_store_lock);
        eos_store_lock = NULL;
    }
}
static void eos_store_give(void) {
    if (!eos_store_lock) return;
    ReleaseMutex(eos_store_lock);
    CloseHandle(eos_store_lock);
    eos_store_lock = NULL;
}
static void eos_same_player_check(void) {   /* (once logged in; the handle stays open while this game runs) */
    char name[96];
    snprintf(name, sizeof name, "Local\\OpenSmashEosPlayer_%s", eos_my_text);
    HANDLE h = CreateMutexA(NULL, FALSE, name);
    if (h && GetLastError() == ERROR_ALREADY_EXISTS)
        fprintf(stderr, "[eos] another game on this PC is logged in as the same EOS player (%s): EOS gives what is sent to this player to one of "
                        "the two only. Two players on one PC need a Device ID each (SSBU_EOS_NEWID=1 or SSBU_EOS_IDFILE)\n", eos_my_text);
}

static void EOS_CALL eos_connection_request(const EosRequestInfo *d) {   /* (any player may open one: what it sends must still carry the pair's secret, net_relay.c) */
    EosAcceptOptions o = {1, eos_me, d->RemoteUserId, &eos_sock};
    p_EOS_P2P_AcceptConnection(eos_p2p, &o);
}

static char eos_cred[5][128];
static volatile LONG eos_missing;   /* (eos_files_missing: the SDK or eos.txt not beside the program) */
static int eos_read_credentials(const char *dir) {
    static const char *const KEYS[5] = {"ProductId", "SandboxId", "DeploymentId", "ClientId", "ClientSecret"};
    char path[MAX_PATH + 16], line[256];
    snprintf(path, sizeof path, "%s\\eos.txt", dir);
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "[eos] no eos.txt beside the program: off\n"); InterlockedExchange(&eos_missing, 1); return 0; }
    while (fgets(line, sizeof line, f)) {
        char *e = strchr(line, '=');
        if (line[0] == '#' || !e) continue;
        *e++ = 0;
        e[strcspn(e, "\r\n")] = 0;
        for (int k = 0; k < 5; k++) if (!strcmp(line, KEYS[k])) snprintf(eos_cred[k], sizeof eos_cred[k], "%s", e);
    }
    fclose(f);
    for (int k = 0; k < 5; k++) if (!eos_cred[k][0]) { fprintf(stderr, "[eos] eos.txt has no %s: off\n", KEYS[k]); InterlockedExchange(&eos_missing, 1); return 0; }
    return 1;
}

static void eos_pump(void) {   /* (EOS thread) what arrived, then what is queued */
    u8 buf[NET_EOS_MAX];
    for (int i = 0; i < 64; i++) {
        EosReceivePacketOptions o = {2, eos_me, sizeof buf, NULL};
        EOS_ProductUserId from = NULL;
        EosSocketId s;
        uint8_t ch;
        uint32_t n = 0;
        if (p_EOS_P2P_ReceivePacket(eos_p2p, &o, &from, &s, &ch, buf, &n) != EOS_SUCCESS) break;
        char id[NET_EOS_ID_LEN + 8];
        int32_t idn = sizeof id;
        if (p_EOS_ProductUserId_ToString(from, id, &idn) != EOS_SUCCESS) continue;
        InterlockedIncrement64(&eos_received);
        if (ch == 1) nexeos_packet(id, buf, n);
        else net_eos_packet(id, buf, n);
    }
    for (;;) {   /* (work posted by other threads) */
        AcquireSRWLockExclusive(&eos_q_lock);
        if (eos_p_head == eos_p_tail) { ReleaseSRWLockExclusive(&eos_q_lock); break; }
        u32 k = eos_p_head++ % POSTS;
        void (*fn)(void *) = eos_posts[k].fn;
        void *arg = eos_posts[k].arg;
        ReleaseSRWLockExclusive(&eos_q_lock);
        fn(arg);
    }
    for (;;) {
        AcquireSRWLockExclusive(&eos_q_lock);
        if (eos_q_head == eos_q_tail) { ReleaseSRWLockExclusive(&eos_q_lock); break; }
        u32 k = eos_q_head++ % QUEUE;
        char to[NET_EOS_ID_LEN + 1];
        u8 data[NET_EOS_MAX];
        u32 len = eos_queue[k].len;
        u8 channel = eos_queue[k].channel;
        memcpy(to, eos_queue[k].to, sizeof to);
        memcpy(data, eos_queue[k].data, len);
        ReleaseSRWLockExclusive(&eos_q_lock);
        EOS_ProductUserId peer = p_EOS_ProductUserId_FromString(to);
        EosSendPacketOptions o = {3, eos_me, peer, &eos_sock, channel, len, data, channel != 0, channel ? EOS_PR_RELIABLE_ORDERED : EOS_PR_UNRELIABLE_UNORDERED, 0};
        if (p_EOS_P2P_SendPacket(eos_p2p, &o) == EOS_SUCCESS) InterlockedIncrement64(&eos_sent);
        else InterlockedIncrement64(&eos_dropped);
    }
}

/* ---- the hub: one EOS lobby per lobby code and build (more than 64 players: another one) -------------------
 * Every game of a lobby code joins a hub, an EOS lobby found by its bucket ("ssbu1:<build>:<code>") or made when
 * none has room, and gives itself a virtual address (10.13.x.y, from its EOS id) as a member attribute "IP". Two
 * members with the same address: the one whose id sorts later picks another. The hub's members and their
 * addresses are what net_eos_net.c sends to - no lobby server of ours (plan/EOS_MIGRATION.md, phase 1).
 *
 * One hub per lobby code. Two games that look at the same moment and find none each make one, and a game reaches
 * only its own hub's members, so the two halves would never meet. Every hub of a code is therefore one of a row,
 * in the order of their lobby ids, and each game belongs in the first with room: on joining it takes the lowest
 * id with room, and it looks at all of them again 3 s after entering, then every 10 s while there is more than one
 * or it is alone, else every 60 s (hub_check). A hub with a lower id and room: it joins that one first and leaves
 * its own 15 s later (hub_old), so that a game that found it there a moment ago still reaches it - but not while
 * it is talking to a member (a match, a room: net_eos_net.c's eos_hub_set_busy), which finishes first. The order
 * does not depend on who owns a hub, so a hub whose owner left (EOS makes another member the owner) changes
 * nothing. A hub holds 64 members at most (EOS_LOBBY_MAX_LOBBY_MEMBERS): more players fill the first, then the
 * next, so at most one has room. While a look finds more than one hub, gathering searches cover every hub of the
 * code (eos_hub_split, net_eos_lobby.c), and the members of the gatherings' lobbies this game is in count as
 * members (hub_refresh): each game writes its address, rating and station URL into those lobbies too, so the
 * players of one gathering reach each other whichever hub each is in.
 * Tests: SSBU_EOS_HUB_SPLIT=1 makes a hub of its own on joining (no search: two such games start split);
 * SSBU_EOS_HUB_MAX=<1..64> the most members of a hub this game makes (1: every game of the code alone in one). */
static EOS_HLobby eos_lobby;
static char hub_bucket[128], hub_id[128];
static volatile LONG hub_state;   /* 0 none asked, 1 searching, 2 joining/creating, 3 in, -1 failed */
static u32 hub_salt, hub_mine, hub_set;   /* (the address asked for, the one the hub has for us) */
static volatile LONG hub_ip;      /* ours once the hub agrees (no other member has it) */
static u64 hub_refresh_at, hub_retry_at, hub_check_at;
static int hub_checking;          /* (the search is a look at every hub of the code: hub_found) */
static int hub_moving;            /* (joining the code's first hub: hub_moved) */
static volatile LONG hub_multi;   /* the last look found more than one hub of this code */
static char hub_old[128];         /* the hub we moved from, left at hub_old_until */
static u64 hub_old_until;
static int hub_busy_said;
static int (*hub_busy)(void);     /* (net_eos_net.c) 1: talking to a member now */
static volatile LONG hub_attrs_ver;
static EOS_HLobbySearch hub_search;
static SRWLOCK hub_lock = SRWLOCK_INIT;
#define HUB_URL 256
#define EOS_NOT_FOUND 13
#define HUB_MEMBERS 128   /* (the hub's 64, the gatherings' and the old hub's) */
typedef struct HubMember { char id[NET_EOS_ID_LEN + 1]; u32 ip, rating; u64 pid; char url[HUB_URL]; } HubMember;
static HubMember hub_members[HUB_MEMBERS];
static int hub_count, hub_own_count;   /* (all of them; the hub's own) */
/* this game's player as the other games see it (net_eos_lobby.c: its NEX PID, station URL and rating, member
 * attributes PID, RT and U0..U3, the URL in pieces of 60 characters: an attribute holds 64) */
typedef struct HubMe { u64 pid; u32 rating; char url[HUB_URL]; } HubMe;
static HubMe hub_me, hub_me_set;
static int hub_me_dirty;
static u64 hub_me_at;

static int hub_env(const char *name) { const char *e = getenv(name); return e && *e ? atoi(e) : 0; }
static u32 hub_address(const char *id, u32 salt) {   /* 10.13.x.y, never .0/.255, 10.13.0.1 or 10.13.255.254 */
    u32 h = 2166136261u ^ salt * 16777619u;
    for (const char *p = id; *p; p++) h = (h ^ (u8)*p) * 16777619u;
    for (;; h = h * 16777619u + 1) {
        u32 x = h >> 8 & 255, y = h & 255;
        if (y == 0 || y == 255 || (x == 0 && y == 1) || (x == 255 && y == 254)) continue;
        return 0x0A0D0000u | x << 8 | y;
    }
}
static void EOS_CALL hub_updated(const EosLobbyResult *d) {
    if (d->ResultCode != EOS_SUCCESS) fprintf(stderr, "[eos] hub: our address not set (%s)\n", p_EOS_EResult_ToString(d->ResultCode));
    hub_refresh_at = 0;
}
static void hub_claim(void) {   /* (EOS thread) our address as our member attribute */
    hub_mine = hub_address(eos_my_text, hub_salt);
    EosLobbyUserOptions o = {1, eos_me, hub_id};
    EOS_HLobbyModification m = NULL;
    if (p_EOS_Lobby_UpdateLobbyModification(eos_lobby, &o, &m) != EOS_SUCCESS) return;
    EosAttrData a = {1, "IP", {0}, EOS_AT_INT64};
    a.Value.AsInt64 = hub_mine;
    EosAddAttrOptions ao = {2, &a, EOS_LAT_PUBLIC};
    p_EOS_LobbyModification_AddMemberAttribute(m, &ao);
    EosUpdateLobbyOptions u = {1, m};
    p_EOS_Lobby_UpdateLobby(eos_lobby, &u, NULL, hub_updated);
    p_EOS_LobbyModification_Release(m);
    if (hub_set != hub_mine) InterlockedIncrement(&hub_attrs_ver);
    hub_set = hub_mine;
}
/* (hub_refresh) the members of one lobby added to got[0..k), but those there already: the hub's own (own: clashes
 * with our address noted, *n its member count), a gathering's or the old hub's (only with an address no one there has) */
static int hub_read(const char *lobby, HubMember *got, int k, int own, u32 *clash, int *count) {
    EosCopyDetailsOptions o = {1, lobby, eos_me};
    EOS_HLobbyDetails det = NULL;
    if (p_EOS_Lobby_CopyLobbyDetailsHandle(eos_lobby, &o, &det) != EOS_SUCCESS) return k;
    EosVersionOnly v = {1};
    u32 n = p_EOS_LobbyDetails_GetMemberCount(det, &v);
    if (count) *count = (int)n;
    for (u32 i = 0; i < n && k < HUB_MEMBERS; i++) {
        EosIndexOptions io = {1, i};
        EOS_ProductUserId who = p_EOS_LobbyDetails_GetMemberByIndex(det, &io);
        char id[NET_EOS_ID_LEN + 8];
        int32_t idn = sizeof id;
        if (!who || p_EOS_ProductUserId_ToString(who, id, &idn) != EOS_SUCCESS) continue;
        int me = !strcmp(id, eos_my_text), there = 0;
        if (!own && me) continue;   /* (ours: the hub's own entry) */
        EosCopyMemberAttrOptions ao = {1, who, "IP"};
        EosAttr *at = NULL;
        u32 ip = 0;
        if (p_EOS_LobbyDetails_CopyMemberAttributeByKey(det, &ao, &at) == EOS_SUCCESS && at) {
            if (at->Data && at->Data->ValueType == EOS_AT_INT64) ip = (u32)at->Data->Value.AsInt64;
            p_EOS_Lobby_Attribute_Release(at);
        }
        if (own && !me && ip && ip == hub_set && strcmp(id, eos_my_text) < 0) *clash = 1;   /* (theirs first: we move) */
        for (int j = 0; j < k && !there; j++) there = !strcmp(got[j].id, id);
        if (there) continue;
        if (!own) {   /* (another hub's player: its address is unique in its hub only) */
            int taken = !ip || ip == hub_set || ip == (u32)hub_ip;
            for (int j = 0; j < k && !taken; j++) taken = got[j].ip == ip;
            if (taken) continue;
        }
        HubMember *g = &got[k++];
        memset(g, 0, sizeof *g);
        snprintf(g->id, sizeof g->id, "%s", id);
        g->ip = ip;
        static const char *const KEYS[6] = {"PID", "RT", "U0", "U1", "U2", "U3"};
        for (int a = 0; a < 6; a++) {
            EosCopyMemberAttrOptions ko = {1, who, KEYS[a]};
            EosAttr *val = NULL;
            if (p_EOS_LobbyDetails_CopyMemberAttributeByKey(det, &ko, &val) != EOS_SUCCESS || !val) continue;
            if (val->Data && a < 2 && val->Data->ValueType == EOS_AT_INT64) { if (a == 0) g->pid = (u64)val->Data->Value.AsInt64; else g->rating = (u32)val->Data->Value.AsInt64; }
            else if (val->Data && a >= 2 && val->Data->ValueType == EOS_AT_STRING && val->Data->Value.AsUtf8) {
                size_t len = strlen(g->url);
                snprintf(g->url + len, sizeof g->url - len, "%s", val->Data->Value.AsUtf8);
            }
            p_EOS_Lobby_Attribute_Release(val);
        }
    }
    p_EOS_LobbyDetails_Release(det);
    return k;
}
static void hub_refresh(void) {   /* (EOS thread) the members and their addresses, from the lobbies' details */
    static HubMember got[HUB_MEMBERS];
    u32 clash = 0;
    int k = 0, own = 0;
    char lob[16][16];   /* (the gatherings' players first: the ones played with stay among the first 64 the network reads) */
    int nl = nexeos_lobbies(lob, 16);
    for (int i = 0; i < nl; i++) k = hub_read(lob[i], got, k, 0, &clash, NULL);
    k = hub_read(hub_id, got, k, 1, &clash, &own);
    if (hub_old[0]) k = hub_read(hub_old, got, k, 0, &clash, NULL);
    AcquireSRWLockExclusive(&hub_lock);
    memcpy(hub_members, got, sizeof got[0] * (size_t)k);
    hub_count = k;
    hub_own_count = own;
    ReleaseSRWLockExclusive(&hub_lock);
    int confirmed = 0;
    for (int i = 0; i < k; i++) if (!strcmp(got[i].id, eos_my_text) && got[i].ip == hub_set && hub_set) confirmed = 1;
    if (clash) { hub_salt++; InterlockedExchange(&hub_ip, 0); hub_claim(); return; }
    if (confirmed && (u32)hub_ip != hub_set) {
        InterlockedExchange(&hub_ip, (LONG)hub_set);
        InterlockedIncrement(&hub_attrs_ver);
        fprintf(stderr, "[eos] in hub %s (lobby %s) as 10.13.%u.%u (%d players there)\n", hub_bucket, hub_id, hub_set >> 8 & 255, hub_set & 255, own);
    }
}
static void EOS_CALL hub_entered(const EosLobbyResult *d) {
    if (d->ResultCode != EOS_SUCCESS) {
        fprintf(stderr, "[eos] hub: %s; trying again\n", p_EOS_EResult_ToString(d->ResultCode));
        hub_retry_at = GetTickCount64() + 2000;
        InterlockedExchange(&hub_state, 0);
        return;
    }
    snprintf(hub_id, sizeof hub_id, "%s", d->LobbyId ? d->LobbyId : "");
    InterlockedExchange(&hub_state, 3);
    if (hub_me.pid) hub_me_dirty = 1;   /* (a hub joined again: our player there too) */
    hub_claim();
    hub_refresh_at = 0;
    hub_check_at = GetTickCount64() + 3000;
}
static void hub_create(void) {
    int most = hub_env("SSBU_EOS_HUB_MAX");
    EosCreateLobbyOptions o;
    memset(&o, 0, sizeof o);
    o.ApiVersion = 10; o.LocalUserId = eos_me; o.MaxLobbyMembers = most >= 1 && most <= 64 ? (u32)most : 64; o.PermissionLevel = EOS_LPL_PUBLICADVERTISED;
    o.BucketId = hub_bucket; o.bAllowInvites = 1; o.bCrossplayOptOut = 0;
    if (o.MaxLobbyMembers != 64) fprintf(stderr, "[eos] hub: SSBU_EOS_HUB_MAX: a hub of %u members at most\n", o.MaxLobbyMembers);
    p_EOS_Lobby_CreateLobby(eos_lobby, &o, NULL, hub_entered);
}
static void EOS_CALL hub_left(const EosLobbyResult *d) { (void)d; }
static void hub_leave_old(void) {
    EosLobbyUserOptions lo = {1, eos_me, hub_old};
    p_EOS_Lobby_LeaveLobby(eos_lobby, &lo, NULL, hub_left);
    fprintf(stderr, "[eos] hub: left the old hub %s\n", hub_old);
    hub_old[0] = 0;
    hub_refresh_at = 0;
}
static void EOS_CALL hub_moved(const EosLobbyResult *d) {
    hub_moving = 0;
    if (d->ResultCode != EOS_SUCCESS || !d->LobbyId) {
        fprintf(stderr, "[eos] hub: moving to the lobby code's first hub: %s; staying in %s\n", p_EOS_EResult_ToString(d->ResultCode), hub_id);
        return;
    }
    if (hub_old[0]) hub_leave_old();   /* (one move after another: the older one goes now) */
    snprintf(hub_old, sizeof hub_old, "%s", hub_id);
    hub_old_until = GetTickCount64() + 15000;
    snprintf(hub_id, sizeof hub_id, "%s", d->LobbyId);
    fprintf(stderr, "[eos] hub: moved to the lobby code's first hub %s; the old one %s is left in 15 s\n", hub_id, hub_old);
    if (hub_me.pid) hub_me_dirty = 1;
    hub_claim();   /* (our address there: the same one but for a clash; the old one works until it is confirmed) */
    hub_refresh_at = 0;
    hub_check_at = GetTickCount64() + 3000;
}
static int hub_info(EOS_HLobbyDetails det, char *id, size_t cap, u32 *slots) {   /* 1: its lobby id and free places */
    EosVersionOnly v = {1};
    EosLobbyInfo *in = NULL;
    if (p_EOS_LobbyDetails_CopyInfo(det, &v, &in) != EOS_SUCCESS || !in) return 0;
    snprintf(id, cap, "%s", in->LobbyId ? in->LobbyId : "");
    *slots = in->AvailableSlots;
    p_EOS_LobbyDetails_Info_Release(in);
    return id[0] != 0;
}
static void hub_join_result(u32 i, LobbyCb cb) {   /* (the search's result i) */
    EosIndexOptions io = {1, i};
    EOS_HLobbyDetails det = NULL;
    if (p_EOS_LobbySearch_CopySearchResultByIndex(hub_search, &io, &det) != EOS_SUCCESS) { EosLobbyResult r = {EOS_NOT_FOUND, NULL, NULL}; cb(&r); return; }
    EosJoinLobbyOptions j = {5, det, eos_me, 0, NULL, 0, 0};
    p_EOS_Lobby_JoinLobby(eos_lobby, &j, NULL, cb);
    p_EOS_LobbyDetails_Release(det);
}
static void EOS_CALL hub_found(const EosFindInfo *d) {
    EosVersionOnly v = {1};
    u32 n = d->ResultCode == EOS_SUCCESS ? p_EOS_LobbySearch_GetSearchResultCount(hub_search, &v) : 0;
    int checking = hub_checking, hubs = 0, ours_seen = 0;
    hub_checking = 0;
    char best[128] = "", id[128];
    u32 best_i = 0, slots = 0;
    for (u32 i = 0; i < n; i++) {   /* (the lowest lobby id with room, ours apart) */
        EosIndexOptions io = {1, i};
        EOS_HLobbyDetails det = NULL;
        if (p_EOS_LobbySearch_CopySearchResultByIndex(hub_search, &io, &det) != EOS_SUCCESS) continue;
        if (hub_info(det, id, sizeof id, &slots)) {
            int ours = checking && !strcmp(id, hub_id);
            hubs++;
            ours_seen |= ours;
            if (!ours && slots >= 1 && (!best[0] || strcmp(id, best) < 0)) { snprintf(best, sizeof best, "%s", id); best_i = i; }
        }
        p_EOS_LobbyDetails_Release(det);
    }
    if (checking) {
        if (d->ResultCode != EOS_SUCCESS && d->ResultCode != EOS_NOT_FOUND) {
            fprintf(stderr, "[eos] hub: looking at the lobby code's hubs: %s\n", p_EOS_EResult_ToString(d->ResultCode));
            hubs = (int)hub_multi + 1;   /* (as at the last look) */
        } else if (!ours_seen) hubs++;   /* (ours, if the search leaves it out) */
        LONG multi = hubs > 1;
        if (multi != hub_multi) {
            InterlockedExchange(&hub_multi, multi);
            if (multi) fprintf(stderr, "[eos] hub: %d hubs of this lobby code: gathering searches cover them all%s\n", hubs, best[0] && strcmp(best, hub_id) < 0 ? "" : " (none before ours with room: staying)");
            else fprintf(stderr, "[eos] hub: one hub of this lobby code again\n");
        }
        if (best[0] && strcmp(best, hub_id) < 0) {
            if (hub_busy && hub_busy()) {
                if (!hub_busy_said) fprintf(stderr, "[eos] hub: the lobby code's first hub with room is %s: moving there once no member is talked to\n", best);
                hub_busy_said = 1;
            } else {
                hub_busy_said = 0;
                hub_moving = 1;
                fprintf(stderr, "[eos] hub: moving from %s to the lobby code's first hub with room %s (%d hubs)\n", hub_id, best, hubs);
                hub_join_result(best_i, hub_moved);
            }
        }
        p_EOS_LobbySearch_Release(hub_search);
        hub_search = NULL;
        return;
    }
    InterlockedExchange(&hub_state, 2);
    if (best[0]) {
        hub_join_result(best_i, hub_entered);
        p_EOS_LobbySearch_Release(hub_search);
        hub_search = NULL;
        return;
    }
    p_EOS_LobbySearch_Release(hub_search);
    hub_search = NULL;
    hub_create();   /* (none with room: a new one) */
}
static void hub_find(void) {   /* (hub_checking: every hub of the code, full ones too) */
    EosCreateSearchOptions so = {1, hub_checking ? 100 : 20};
    if (p_EOS_Lobby_CreateLobbySearch(eos_lobby, &so, &hub_search) != EOS_SUCCESS) {
        hub_search = NULL;
        if (hub_checking) hub_checking = 0;
        else hub_retry_at = GetTickCount64() + 2000;
        return;
    }
    EosAttrData b = {1, "bucket", {0}, EOS_AT_STRING};
    b.Value.AsUtf8 = hub_bucket;
    EosSetParameterOptions p = {1, &b, EOS_CO_EQUAL};
    p_EOS_LobbySearch_SetParameter(hub_search, &p);
    if (!hub_checking) {
        EosAttrData s = {1, "minslotsavailable", {0}, EOS_AT_INT64};
        s.Value.AsInt64 = 1;
        EosSetParameterOptions ps = {1, &s, EOS_CO_GREATERTHANOREQUAL};
        p_EOS_LobbySearch_SetParameter(hub_search, &ps);
        InterlockedExchange(&hub_state, 1);
    }
    EosFindOptions f = {1, eos_me};
    p_EOS_LobbySearch_Find(hub_search, &f, NULL, hub_found);
}
static void EOS_CALL hub_member_status(const EosMemberStatusInfo *d) { (void)d; hub_refresh_at = 0; }
static void EOS_CALL hub_member_update(const EosMemberUpdateInfo *d) { (void)d; hub_refresh_at = 0; }
static void EOS_CALL hub_player_set(const EosLobbyResult *d) {
    if (d->ResultCode != EOS_SUCCESS) { fprintf(stderr, "[eos] hub: our player not set (%s)\n", p_EOS_EResult_ToString(d->ResultCode)); hub_me_dirty = 1; }
    hub_refresh_at = 0;
}
static void hub_attrs(EOS_HLobbyModification m, const HubMe *me, int pid, u32 ip) {   /* (EOS thread) our player's member attributes */
    EosAttrData a = {1, "PID", {0}, EOS_AT_INT64};
    EosAddAttrOptions ao = {2, &a, EOS_LAT_PUBLIC};
    if (pid) { a.Value.AsInt64 = (int64_t)me->pid; p_EOS_LobbyModification_AddMemberAttribute(m, &ao); }
    if (ip) { a.Key = "IP"; a.Value.AsInt64 = ip; p_EOS_LobbyModification_AddMemberAttribute(m, &ao); }
    a.Key = "RT";
    a.Value.AsInt64 = me->rating;
    p_EOS_LobbyModification_AddMemberAttribute(m, &ao);
    static const char *const U[4] = {"U0", "U1", "U2", "U3"};
    char piece[4][61];
    size_t len = strlen(me->url);
    for (int i = 0; i < 4; i++) {
        size_t at = (size_t)i * 60, n = at < len ? (len - at < 60 ? len - at : 60) : 0;
        memcpy(piece[i], me->url + (at < len ? at : len), n);
        piece[i][n] = 0;
        EosAttrData u = {1, U[i], {0}, EOS_AT_STRING};
        u.Value.AsUtf8 = piece[i];
        EosAddAttrOptions uo = {2, &u, EOS_LAT_PUBLIC};
        p_EOS_LobbyModification_AddMemberAttribute(m, &uo);
    }
}
static void hub_player(void) {   /* (EOS thread) our player's PID, rating and station URL as member attributes */
    AcquireSRWLockExclusive(&hub_lock);
    hub_me_set = hub_me;
    hub_me_dirty = 0;
    ReleaseSRWLockExclusive(&hub_lock);
    EosLobbyUserOptions o = {1, eos_me, hub_id};
    EOS_HLobbyModification m = NULL;
    if (p_EOS_Lobby_UpdateLobbyModification(eos_lobby, &o, &m) != EOS_SUCCESS) { hub_me_dirty = 1; return; }
    hub_attrs(m, &hub_me_set, 1, 0);
    EosUpdateLobbyOptions up = {1, m};
    p_EOS_Lobby_UpdateLobby(eos_lobby, &up, NULL, hub_player_set);
    p_EOS_LobbyModification_Release(m);
}
void eos_hub_member_attrs(EOS_HLobbyModification m) {   /* (a gathering's lobby: its PID is net_eos_lobby.c's) */
    HubMe me;
    AcquireSRWLockShared(&hub_lock);
    me = hub_me;
    ReleaseSRWLockShared(&hub_lock);
    hub_attrs(m, &me, 0, (u32)hub_ip);
}
void eos_hub_set_busy(int (*fn)(void)) { hub_busy = fn; }
unsigned eos_hub_attrs_version(void) { return (unsigned)hub_attrs_ver; }
int eos_hub_split(void) { return hub_multi != 0; }
static void hub_tick(void) {   /* (EOS thread) */
    static int notified;
    u64 t = GetTickCount64();
    if (!hub_bucket[0]) return;
    if (!eos_lobby) eos_lobby = p_EOS_Platform_GetLobbyInterface(eos_plat);
    if (!notified) {
        EosVersionOnly v = {1};
        p_EOS_Lobby_AddNotifyLobbyMemberStatusReceived(eos_lobby, &v, NULL, hub_member_status);
        p_EOS_Lobby_AddNotifyLobbyMemberUpdateReceived(eos_lobby, &v, NULL, hub_member_update);
        notified = 1;
        if (hub_env("SSBU_EOS_HUB_SPLIT") == 1) {   /* (tests: a hub of our own, as if another game had made its own at the same moment) */
            fprintf(stderr, "[eos] hub: SSBU_EOS_HUB_SPLIT: a hub of our own, no search\n");
            InterlockedExchange(&hub_state, 2);
            hub_create();
        }
    }
    if (hub_state == 0 && t >= hub_retry_at) hub_find();
    if (hub_state == 3 && t >= hub_refresh_at) { hub_refresh_at = t + 2000; hub_refresh(); }
    if (hub_state == 3 && hub_me_dirty && t >= hub_me_at) { hub_me_at = t + 300; hub_player(); }
    if (hub_state == 3 && hub_old[0] && t >= hub_old_until && !(hub_busy && hub_busy())) hub_leave_old();
    if (hub_state == 3 && !hub_search && !hub_moving && t >= hub_check_at) {
        hub_check_at = t + (hub_multi || hub_own_count <= 1 ? 10000 : 60000);
        hub_checking = 1;
        hub_find();
    }
}

static DWORD WINAPI eos_thread_(LPVOID dll) {
#define LOAD(ret, name, args) if (!(p_##name = (ret (EOS_CALL *) args)(void *)GetProcAddress((HMODULE)dll, #name))) { fprintf(stderr, "[eos] the SDK has no %s: off\n", #name); InterlockedExchange(&eos_phase, -1); return 0; }
    EOS_FUNCS(LOAD)
    EosInitializeOptions io = {5, NULL, NULL, NULL, "OpenSmash", "1", NULL, NULL, NULL};
    EOS_EResult r = p_EOS_Initialize(&io);
    if (r != EOS_SUCCESS) { fprintf(stderr, "[eos] start: %s\n", p_EOS_EResult_ToString(r)); InterlockedExchange(&eos_phase, -1); return 0; }
    EosPlatformOptions po;
    memset(&po, 0, sizeof po);
    po.ApiVersion = 15;
    po.ProductId = eos_cred[0]; po.SandboxId = eos_cred[1]; po.DeploymentId = eos_cred[2];
    po.ClientCredentials.ClientId = eos_cred[3]; po.ClientCredentials.ClientSecret = eos_cred[4];
    po.Flags = EOS_PF_DISABLE_OVERLAY;
    if (!(eos_plat = p_EOS_Platform_Create(&po))) { fprintf(stderr, "[eos] the platform did not start: off\n"); InterlockedExchange(&eos_phase, -1); return 0; }
    eos_con = p_EOS_Platform_GetConnectInterface(eos_plat);
    eos_p2p = p_EOS_Platform_GetP2PInterface(eos_plat);
    const char *nid = getenv("SSBU_EOS_NEWID");
    eos_store_take();
    if (eos_id_file() && !(nid && *nid == '1') && !eos_id_restore(eos_id_file())) eos_id_new = 1;
    if (eos_id_new || (nid && *nid == '1')) {   /* (only the store's copy is removed, not with EOS_Connect_DeleteDeviceId: seen, a Device ID
                                                   that call had replaced, put back from its file later, no longer led to its player - a new
                                                   one was made; and another game of this PC may still be logged in with it) */
        for (int piece = 1; piece <= ID_PIECES; piece++) { wchar_t t[96]; eos_id_target(t, 96, piece); CredDeleteW(t, CRED_TYPE_GENERIC, 0); }
        EosCreateDeviceIdOptions o = {1, "PC"}; p_EOS_Connect_CreateDeviceId(eos_con, &o, NULL, eos_device_id);
    } else { EosCreateDeviceIdOptions o = {1, "PC"}; p_EOS_Connect_CreateDeviceId(eos_con, &o, NULL, eos_device_id); }
    timeBeginPeriod(1);
    u64 t0 = GetTickCount64();
    int told = 0;
    for (;;) {
        p_EOS_Platform_Tick(eos_plat);
        LONG ph = eos_phase;
        if (ph == 2 && !told) {
            int32_t n = sizeof eos_my_text;
            p_EOS_ProductUserId_ToString(eos_me, eos_my_text, &n);
            EosNotifyRequestOptions o = {1, eos_me, &eos_sock};
            p_EOS_P2P_AddNotifyPeerConnectionRequest(eos_p2p, &o, NULL, eos_connection_request);
            const char *relay = getenv("SSBU_EOS_RELAY");
            if (relay && !_stricmp(relay, "force")) {
                EosRelayControlOptions rc = {1, EOS_RC_FORCE_RELAYS};
                EOS_EResult rr = p_EOS_P2P_SetRelayControl(eos_p2p, &rc);
                fprintf(stderr, "[eos] SSBU_EOS_RELAY=force: every P2P packet through Epic's relays (%s)\n", p_EOS_EResult_ToString(rr));
            }
            fprintf(stderr, "[eos] ready in %llu ms: the relay of last resort is Epic's (player %s)\n", (unsigned long long)(GetTickCount64() - t0), eos_my_text);
            if (eos_id_new) eos_id_save(eos_id_file());
            eos_store_give();
            eos_same_player_check();
            told = 1;
        }
        if (ph == 1 && GetTickCount64() - t0 > 15000) eos_store_give();   /* (a login that hangs keeps no other game waiting; once) */
        if (ph < 0) { eos_store_give(); fprintf(stderr, "[eos] off: the lobby's relay stays the last resort\n"); return 0; }
        if (ph == 2) { eos_pump(); hub_tick(); nexeos_tick(); }
        WaitForSingleObject(eos_wake, 1);
    }
}

void eos_start(void) {
    static LONG once;
    if (InterlockedExchange(&once, 1)) return;
    const char *e = getenv("SSBU_EOS");
    if (e && *e == '0') return;
    char dir[MAX_PATH], path[MAX_PATH + 40];
    DWORD n = GetModuleFileNameA(NULL, dir, sizeof dir);
    if (!n || n >= sizeof dir) return;
    char *slash = strrchr(dir, '\\');
    if (slash) *slash = 0;
    if (!eos_read_credentials(dir)) return;
    snprintf(path, sizeof path, "%s\\EOSSDK-Win64-Shipping.dll", dir);
    HMODULE dll = LoadLibraryA(path);
    if (!dll) { fprintf(stderr, "[eos] no EOSSDK-Win64-Shipping.dll beside the program: off\n"); InterlockedExchange(&eos_missing, 1); return; }
    eos_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    InterlockedExchange(&eos_phase, 1);
    HANDLE t = CreateThread(NULL, 0, eos_thread_, dll, 0, NULL);
    if (t) CloseHandle(t);
}

int eos_ready(void) { return eos_phase == 2 && eos_my_text[0]; }
int eos_files_missing(void) { eos_start(); return eos_missing != 0; }
/* 1 once logged in, within ms; 0 at once when EOS is off here (no SDK or eos.txt beside the program, SSBU_EOS=0) or
 * its start failed, or when the login did not come in time (Epic's services down or out of reach) */
int eos_wait_ready(unsigned ms) {
    eos_start();
    u64 until = GetTickCount64() + ms;
    while (eos_phase == 1 || (eos_phase == 2 && !eos_my_text[0])) {
        if (GetTickCount64() >= until) return 0;
        Sleep(10);
    }
    return eos_ready();
}
const char *eos_my_id(void) { return eos_ready() ? eos_my_text : NULL; }

static int eos_send_on(const char *to, const void *data, u32 len, u8 channel) {
    if (!eos_ready() || len > NET_EOS_MAX || !to || strlen(to) != NET_EOS_ID_LEN) return 0;
    AcquireSRWLockExclusive(&eos_q_lock);
    if (eos_q_tail - eos_q_head >= QUEUE) { ReleaseSRWLockExclusive(&eos_q_lock); InterlockedIncrement64(&eos_dropped); return 0; }
    u32 k = eos_q_tail++ % QUEUE;
    memcpy(eos_queue[k].to, to, NET_EOS_ID_LEN + 1);
    eos_queue[k].len = len;
    eos_queue[k].channel = channel;
    memcpy(eos_queue[k].data, data, len);
    ReleaseSRWLockExclusive(&eos_q_lock);
    SetEvent(eos_wake);
    return 1;
}

int eos_send(const char *to, const void *data, u32 len) { return eos_send_on(to, data, len, 0); }
int eos_send_reliable(const char *to, const void *data, u32 len) { return eos_send_on(to, data, len, 1); }
int eos_post(void (*fn)(void *), void *arg) {
    if (eos_phase <= 0) return 0;
    AcquireSRWLockExclusive(&eos_q_lock);
    if (eos_p_tail - eos_p_head >= POSTS) { ReleaseSRWLockExclusive(&eos_q_lock); return 0; }
    u32 k = eos_p_tail++ % POSTS;
    eos_posts[k].fn = fn;
    eos_posts[k].arg = arg;
    ReleaseSRWLockExclusive(&eos_q_lock);
    SetEvent(eos_wake);
    return 1;
}
EOS_HLobby eos_lobby_interface(void) { if (!eos_lobby) eos_lobby = p_EOS_Platform_GetLobbyInterface(eos_plat); return eos_lobby; }

void eos_counts(u64 *out_sent, u64 *out_received, u64 *out_dropped) { *out_sent = (u64)eos_sent; *out_received = (u64)eos_received; *out_dropped = (u64)eos_dropped; }

void eos_hub_join(const char *bucket) {   /* (once; the EOS thread does the rest) */
    if (hub_bucket[0] || !bucket || !*bucket) return;
    snprintf(hub_bucket, sizeof hub_bucket, "%s", bucket);
    if (eos_wake) SetEvent(eos_wake);
}
u32 eos_hub_ip(void) { return (u32)hub_ip; }
int eos_hub_failed(void) { return eos_phase < 0 || hub_state < 0; }
int eos_hub_members(char ids[][NET_EOS_ID_LEN + 1], u32 *ips, int max) {
    AcquireSRWLockShared(&hub_lock);
    int n = hub_count < max ? hub_count : max;
    for (int i = 0; i < n; i++) { memcpy(ids[i], hub_members[i].id, NET_EOS_ID_LEN + 1); ips[i] = hub_members[i].ip; }
    ReleaseSRWLockShared(&hub_lock);
    return n;
}
int eos_hub_member_ip(const char *id, u32 *ip) {   /* 1: a member, its address in *ip */
    int found = 0;
    AcquireSRWLockShared(&hub_lock);
    for (int i = 0; i < hub_count && !found; i++)
        if (!strcmp(hub_members[i].id, id)) { *ip = hub_members[i].ip; found = 1; }
    ReleaseSRWLockShared(&hub_lock);
    return found;
}
void eos_hub_set_player(u64 pid, const char *url, u32 rating) {
    AcquireSRWLockExclusive(&hub_lock);
    if (hub_me.pid != pid || hub_me.rating != rating || strncmp(hub_me.url, url ? url : "", sizeof hub_me.url - 1)) {
        hub_me.pid = pid;
        hub_me.rating = rating;
        snprintf(hub_me.url, sizeof hub_me.url, "%s", url ? url : "");
        hub_me_dirty = 1;
        InterlockedIncrement(&hub_attrs_ver);
    }
    ReleaseSRWLockExclusive(&hub_lock);
}
void eos_hub_refresh_soon(void) { hub_refresh_at = 0; }
const char *eos_hub_lobby(void) { return hub_state == 3 ? hub_id : ""; }
int eos_hub_player(u64 pid, char id[NET_EOS_ID_LEN + 1], char *url, size_t cap, u32 *rating) {
    int found = 0;
    AcquireSRWLockShared(&hub_lock);
    for (int i = 0; i < hub_count && !found; i++) {
        HubMember *m = &hub_members[i];
        if (m->pid ? m->pid != pid : nex_pid_of(m->id) != pid) continue;
        found = 1;
        if (id) memcpy(id, m->id, NET_EOS_ID_LEN + 1);
        if (url && cap) snprintf(url, cap, "%s", m->url);
        if (rating) *rating = m->rating;
    }
    ReleaseSRWLockShared(&hub_lock);
    return found;
}
int eos_hub_member_id(u32 ip, char id[NET_EOS_ID_LEN + 1]) {   /* 1: the member at that address */
    int found = 0;
    AcquireSRWLockShared(&hub_lock);
    for (int i = 0; i < hub_count && !found; i++)
        if (hub_members[i].ip == ip && strcmp(hub_members[i].id, eos_my_text)) { memcpy(id, hub_members[i].id, NET_EOS_ID_LEN + 1); found = 1; }
    ReleaseSRWLockShared(&hub_lock);
    return found;
}

#else   /* (Linux, macOS: not yet) */
void eos_hub_set_busy(int (*fn)(void)) { (void)fn; }
void eos_hub_set_player(u64 pid, const char *url, u32 rating) { (void)pid; (void)url; (void)rating; }
void eos_hub_refresh_soon(void) {}
const char *eos_hub_lobby(void) { return ""; }
int eos_hub_player(u64 pid, char id[NET_EOS_ID_LEN + 1], char *url, size_t cap, u32 *rating) { (void)pid; (void)id; (void)url; (void)cap; (void)rating; return 0; }
int eos_send_reliable(const char *to, const void *data, u32 len) { (void)to; (void)data; (void)len; return 0; }
int eos_post(void (*fn)(void *), void *arg) { (void)fn; (void)arg; return 0; }
void eos_hub_join(const char *bucket) { (void)bucket; }
u32 eos_hub_ip(void) { return 0; }
int eos_hub_failed(void) { return 1; }
int eos_hub_members(char ids[][NET_EOS_ID_LEN + 1], u32 *ips, int max) { (void)ids; (void)ips; (void)max; return 0; }
int eos_hub_member_ip(const char *id, u32 *ip) { (void)id; (void)ip; return 0; }
int eos_hub_member_id(u32 ip, char id[NET_EOS_ID_LEN + 1]) { (void)ip; (void)id; return 0; }
void eos_start(void) {}
int eos_ready(void) { return 0; }
int eos_files_missing(void) { return 0; }
int eos_wait_ready(unsigned ms) { (void)ms; return 0; }
const char *eos_my_id(void) { return NULL; }
int eos_send(const char *to, const void *data, u32 len) { (void)to; (void)data; (void)len; return 0; }
void eos_counts(u64 *a, u64 *b, u64 *c) { *a = *b = *c = 0; }
#endif
