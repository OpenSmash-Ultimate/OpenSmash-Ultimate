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
 * SSBU_EOS_NEWID=1 (tests: a second player on the same PC) makes a new Device ID first. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "net_eos.h"

#ifdef _WIN32
#include <windows.h>
#pragma comment(lib, "winmm.lib")

typedef int32_t EOS_Bool;
typedef int32_t EOS_EResult;
typedef struct EOS_PlatformHandle *EOS_HPlatform;
typedef struct EOS_ConnectHandle *EOS_HConnect;
typedef struct EOS_P2PHandle *EOS_HP2P;
typedef struct EOS_ProductUserIdDetails *EOS_ProductUserId;
typedef struct EOS_ContinuanceTokenDetails *EOS_ContinuanceToken;
typedef uint64_t EOS_NotificationId;
#define EOS_CALL __stdcall
enum { EOS_SUCCESS = 0, EOS_INVALID_USER = 3, EOS_ECT_DEVICEID_ACCESS_TOKEN = 10, EOS_PR_UNRELIABLE_UNORDERED = 0 };
#define EOS_PF_DISABLE_OVERLAY 0x2ull

typedef struct { int32_t ApiVersion; void *Alloc, *Realloc, *Release; const char *ProductName, *ProductVersion; void *Reserved, *SystemInitializeOptions, *OverrideThreadAffinity; } EosInitializeOptions;   /* (API 5) */
typedef struct { const char *ClientId, *ClientSecret; } EosClientCredentials;
typedef struct {   /* (API 15) */
    int32_t ApiVersion; void *Reserved; const char *ProductId, *SandboxId; EosClientCredentials ClientCredentials; EOS_Bool bIsServer;
    const char *EncryptionKey, *OverrideCountryCode, *OverrideLocaleCode, *DeploymentId; uint64_t Flags; const char *CacheDirectory;
    uint32_t TickBudgetInMilliseconds; const void *RTCOptions; void *IntegratedPlatformOptionsContainerHandle; const void *SystemSpecificOptions; double *TaskNetworkTimeoutSeconds;
} EosPlatformOptions;
typedef struct { int32_t ApiVersion; const char *Token; int32_t Type; } EosConnectCredentials;                 /* (API 1) */
typedef struct { int32_t ApiVersion; const char *DisplayName, *NsaIdToken; } EosUserLoginInfo;                   /* (API 2) */
typedef struct { int32_t ApiVersion; const EosConnectCredentials *Credentials; const EosUserLoginInfo *UserLoginInfo; } EosLoginOptions;   /* (API 2) */
typedef struct { EOS_EResult ResultCode; void *ClientData; EOS_ProductUserId LocalUserId; EOS_ContinuanceToken ContinuanceToken; } EosLoginInfo;
typedef struct { int32_t ApiVersion; EOS_ContinuanceToken ContinuanceToken; } EosCreateUserOptions;              /* (API 1) */
typedef struct { EOS_EResult ResultCode; void *ClientData; EOS_ProductUserId LocalUserId; } EosCreateUserInfo;
typedef struct { int32_t ApiVersion; const char *DeviceModel; } EosCreateDeviceIdOptions;                      /* (API 1) */
typedef struct { int32_t ApiVersion; } EosDeleteDeviceIdOptions;                                               /* (API 1) */
typedef struct { EOS_EResult ResultCode; void *ClientData; } EosDeviceIdInfo;                                  /* (create and delete) */
typedef struct { int32_t ApiVersion; char SocketName[33]; } EosSocketId;                                       /* (API 1) */
typedef struct { int32_t ApiVersion; EOS_ProductUserId LocalUserId, RemoteUserId; const EosSocketId *SocketId; uint8_t Channel; uint32_t DataLengthBytes;
                 const void *Data; EOS_Bool bAllowDelayedDelivery; int32_t Reliability; EOS_Bool bDisableAutoAcceptConnection; } EosSendPacketOptions;   /* (API 3) */
typedef struct { int32_t ApiVersion; EOS_ProductUserId LocalUserId; uint32_t MaxDataSizeBytes; const uint8_t *RequestedChannel; } EosReceivePacketOptions;   /* (API 2) */
typedef struct { int32_t ApiVersion; EOS_ProductUserId LocalUserId, RemoteUserId; const EosSocketId *SocketId; } EosAcceptOptions;   /* (API 1) */
typedef struct { int32_t ApiVersion; EOS_ProductUserId LocalUserId; const EosSocketId *SocketId; } EosNotifyRequestOptions;            /* (API 1) */
typedef struct { void *ClientData; EOS_ProductUserId LocalUserId, RemoteUserId; const EosSocketId *SocketId; } EosRequestInfo;

/* Lobbies (eos_lobby_types.h) */
typedef struct EOS_LobbyHandle *EOS_HLobby;
typedef struct EOS_LobbySearchHandle *EOS_HLobbySearch;
typedef struct EOS_LobbyDetailsHandle *EOS_HLobbyDetails;
typedef struct EOS_LobbyModificationHandle *EOS_HLobbyModification;
enum { EOS_AT_INT64 = 1, EOS_AT_STRING = 3, EOS_LAT_PUBLIC = 0, EOS_CO_EQUAL = 0, EOS_CO_GREATERTHANOREQUAL = 3, EOS_LPL_PUBLICADVERTISED = 0,
       EOS_LMS_JOINED = 0, EOS_LMS_LEFT = 1, EOS_LMS_DISCONNECTED = 2, EOS_LMS_KICKED = 3, EOS_LMS_PROMOTED = 4, EOS_LMS_CLOSED = 5 };
typedef struct { int32_t ApiVersion; const char *Key; union { int64_t AsInt64; double AsDouble; EOS_Bool AsBool; const char *AsUtf8; } Value; int32_t ValueType; } EosAttrData;   /* (API 1) */
typedef struct { int32_t ApiVersion; EosAttrData *Data; int32_t Visibility; } EosAttr;
typedef struct { int32_t ApiVersion; EOS_ProductUserId LocalUserId; uint32_t MaxLobbyMembers; int32_t PermissionLevel; EOS_Bool bPresenceEnabled, bAllowInvites;
                 const char *BucketId; EOS_Bool bDisableHostMigration, bEnableRTCRoom; const void *LocalRTCOptions; const char *LobbyId; EOS_Bool bEnableJoinById,
                 bRejoinAfterKickRequiresInvite; const uint32_t *AllowedPlatformIds; uint32_t AllowedPlatformIdsCount; EOS_Bool bCrossplayOptOut; int32_t RTCRoomJoinActionType; } EosCreateLobbyOptions;   /* (API 10) */
typedef struct { EOS_EResult ResultCode; void *ClientData; const char *LobbyId; } EosLobbyResult;   /* (create, join, update, leave) */
typedef struct { int32_t ApiVersion; uint32_t MaxResults; } EosCreateSearchOptions;
typedef struct { int32_t ApiVersion; const EosAttrData *Parameter; int32_t ComparisonOp; } EosSetParameterOptions;
typedef struct { int32_t ApiVersion; EOS_ProductUserId LocalUserId; } EosFindOptions;
typedef struct { EOS_EResult ResultCode; void *ClientData; } EosFindInfo;
typedef struct { int32_t ApiVersion; } EosVersionOnly;
typedef struct { int32_t ApiVersion; uint32_t Index; } EosIndexOptions;
typedef struct { int32_t ApiVersion; EOS_HLobbyDetails LobbyDetailsHandle; EOS_ProductUserId LocalUserId; EOS_Bool bPresenceEnabled; const void *LocalRTCOptions;
                 EOS_Bool bCrossplayOptOut; int32_t RTCRoomJoinActionType; } EosJoinLobbyOptions;   /* (API 5) */
typedef struct { int32_t ApiVersion; EOS_ProductUserId LocalUserId; const char *LobbyId; } EosLobbyUserOptions;   /* (update modification, leave: API 1) */
typedef struct { int32_t ApiVersion; const EosAttrData *Attribute; int32_t Visibility; } EosAddAttrOptions;   /* (API 2) */
typedef struct { int32_t ApiVersion; EOS_HLobbyModification LobbyModificationHandle; } EosUpdateLobbyOptions;
typedef struct { int32_t ApiVersion; const char *LobbyId; EOS_ProductUserId LocalUserId; } EosCopyDetailsOptions;
typedef struct { int32_t ApiVersion; EOS_ProductUserId TargetUserId; const char *AttrKey; } EosCopyMemberAttrOptions;
typedef struct { void *ClientData; const char *LobbyId; EOS_ProductUserId TargetUserId; int32_t CurrentStatus; } EosMemberStatusInfo;
typedef struct { void *ClientData; const char *LobbyId; EOS_ProductUserId TargetUserId; } EosMemberUpdateInfo;
typedef void (EOS_CALL *LobbyCb)(const EosLobbyResult *);
typedef void (EOS_CALL *FindCb)(const EosFindInfo *);
typedef void (EOS_CALL *MemberStatusCb)(const EosMemberStatusInfo *);
typedef void (EOS_CALL *MemberUpdateCb)(const EosMemberUpdateInfo *);

typedef void (EOS_CALL *LoginCb)(const EosLoginInfo *);
typedef void (EOS_CALL *CreateUserCb)(const EosCreateUserInfo *);
typedef void (EOS_CALL *DeviceIdCb)(const EosDeviceIdInfo *);
typedef void (EOS_CALL *RequestCb)(const EosRequestInfo *);

#define EOS_FUNCS(X) \
    X(EOS_EResult, EOS_Initialize, (const EosInitializeOptions *)) \
    X(EOS_HPlatform, EOS_Platform_Create, (const EosPlatformOptions *)) \
    X(void, EOS_Platform_Tick, (EOS_HPlatform)) \
    X(EOS_HConnect, EOS_Platform_GetConnectInterface, (EOS_HPlatform)) \
    X(EOS_HP2P, EOS_Platform_GetP2PInterface, (EOS_HPlatform)) \
    X(void, EOS_Connect_Login, (EOS_HConnect, const EosLoginOptions *, void *, LoginCb)) \
    X(void, EOS_Connect_CreateUser, (EOS_HConnect, const EosCreateUserOptions *, void *, CreateUserCb)) \
    X(void, EOS_Connect_CreateDeviceId, (EOS_HConnect, const EosCreateDeviceIdOptions *, void *, DeviceIdCb)) \
    X(void, EOS_Connect_DeleteDeviceId, (EOS_HConnect, const EosDeleteDeviceIdOptions *, void *, DeviceIdCb)) \
    X(EOS_EResult, EOS_P2P_SendPacket, (EOS_HP2P, const EosSendPacketOptions *)) \
    X(EOS_EResult, EOS_P2P_ReceivePacket, (EOS_HP2P, const EosReceivePacketOptions *, EOS_ProductUserId *, EosSocketId *, uint8_t *, void *, uint32_t *)) \
    X(EOS_EResult, EOS_P2P_AcceptConnection, (EOS_HP2P, const EosAcceptOptions *)) \
    X(EOS_NotificationId, EOS_P2P_AddNotifyPeerConnectionRequest, (EOS_HP2P, const EosNotifyRequestOptions *, void *, RequestCb)) \
    X(EOS_EResult, EOS_ProductUserId_ToString, (EOS_ProductUserId, char *, int32_t *)) \
    X(EOS_ProductUserId, EOS_ProductUserId_FromString, (const char *)) \
    X(const char *, EOS_EResult_ToString, (EOS_EResult)) \
    X(EOS_HLobby, EOS_Platform_GetLobbyInterface, (EOS_HPlatform)) \
    X(void, EOS_Lobby_CreateLobby, (EOS_HLobby, const EosCreateLobbyOptions *, void *, LobbyCb)) \
    X(void, EOS_Lobby_JoinLobby, (EOS_HLobby, const EosJoinLobbyOptions *, void *, LobbyCb)) \
    X(void, EOS_Lobby_LeaveLobby, (EOS_HLobby, const EosLobbyUserOptions *, void *, LobbyCb)) \
    X(EOS_EResult, EOS_Lobby_UpdateLobbyModification, (EOS_HLobby, const EosLobbyUserOptions *, EOS_HLobbyModification *)) \
    X(void, EOS_Lobby_UpdateLobby, (EOS_HLobby, const EosUpdateLobbyOptions *, void *, LobbyCb)) \
    X(EOS_EResult, EOS_Lobby_CreateLobbySearch, (EOS_HLobby, const EosCreateSearchOptions *, EOS_HLobbySearch *)) \
    X(EOS_EResult, EOS_Lobby_CopyLobbyDetailsHandle, (EOS_HLobby, const EosCopyDetailsOptions *, EOS_HLobbyDetails *)) \
    X(EOS_NotificationId, EOS_Lobby_AddNotifyLobbyMemberStatusReceived, (EOS_HLobby, const EosVersionOnly *, void *, MemberStatusCb)) \
    X(EOS_NotificationId, EOS_Lobby_AddNotifyLobbyMemberUpdateReceived, (EOS_HLobby, const EosVersionOnly *, void *, MemberUpdateCb)) \
    X(EOS_EResult, EOS_LobbyModification_AddMemberAttribute, (EOS_HLobbyModification, const EosAddAttrOptions *)) \
    X(EOS_EResult, EOS_LobbyModification_AddAttribute, (EOS_HLobbyModification, const EosAddAttrOptions *)) \
    X(void, EOS_LobbyModification_Release, (EOS_HLobbyModification)) \
    X(EOS_EResult, EOS_LobbySearch_SetParameter, (EOS_HLobbySearch, const EosSetParameterOptions *)) \
    X(void, EOS_LobbySearch_Find, (EOS_HLobbySearch, const EosFindOptions *, void *, FindCb)) \
    X(uint32_t, EOS_LobbySearch_GetSearchResultCount, (EOS_HLobbySearch, const EosVersionOnly *)) \
    X(EOS_EResult, EOS_LobbySearch_CopySearchResultByIndex, (EOS_HLobbySearch, const EosIndexOptions *, EOS_HLobbyDetails *)) \
    X(void, EOS_LobbySearch_Release, (EOS_HLobbySearch)) \
    X(uint32_t, EOS_LobbyDetails_GetMemberCount, (EOS_HLobbyDetails, const EosVersionOnly *)) \
    X(EOS_ProductUserId, EOS_LobbyDetails_GetMemberByIndex, (EOS_HLobbyDetails, const EosIndexOptions *)) \
    X(EOS_EResult, EOS_LobbyDetails_CopyMemberAttributeByKey, (EOS_HLobbyDetails, const EosCopyMemberAttrOptions *, EosAttr **)) \
    X(void, EOS_LobbyDetails_Release, (EOS_HLobbyDetails)) \
    X(EOS_ProductUserId, EOS_LobbyDetails_GetLobbyOwner, (EOS_HLobbyDetails, const EosVersionOnly *)) \
    X(void, EOS_Lobby_Attribute_Release, (EosAttr *))
#define DECLARE(ret, name, args) static ret (EOS_CALL *p_##name) args;
EOS_FUNCS(DECLARE)

static EOS_HPlatform eos_plat;
static EOS_HConnect eos_con;
static EOS_HP2P eos_p2p;
static EOS_ProductUserId eos_me;
static char eos_my_text[NET_EOS_ID_LEN + 8];
static volatile LONG eos_phase;   /* 0 off, 1 logging in, 2 ready, -1 failed */
static EosSocketId eos_sock = {1, "opensmash"};
static HANDLE eos_wake;

/* eos_send's eos_queue: copies, eos_sent on the EOS thread */
#define QUEUE 256
static struct { char to[NET_EOS_ID_LEN + 1]; u32 len; u8 data[NET_EOS_MAX]; } eos_queue[QUEUE];
static u32 eos_q_head, eos_q_tail;
static SRWLOCK eos_q_lock = SRWLOCK_INIT;
static volatile LONG64 eos_sent, eos_received, eos_dropped;

static void EOS_CALL eos_created(const EosCreateUserInfo *d) {
    if (d->ResultCode == EOS_SUCCESS) { eos_me = d->LocalUserId; InterlockedExchange(&eos_phase, 2); }
    else { fprintf(stderr, "[eos] new user: %s\n", p_EOS_EResult_ToString(d->ResultCode)); InterlockedExchange(&eos_phase, -1); }
}
static void EOS_CALL eos_logged_in(const EosLoginInfo *d) {
    if (d->ResultCode == EOS_SUCCESS) { eos_me = d->LocalUserId; InterlockedExchange(&eos_phase, 2); return; }
    if (d->ResultCode == EOS_INVALID_USER) {   /* (this Device ID's first eos_login: a user of the product is made) */
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
static void EOS_CALL eos_device_id_gone(const EosDeviceIdInfo *d) {
    (void)d;
    EosCreateDeviceIdOptions o = {1, "PC"};
    p_EOS_Connect_CreateDeviceId(eos_con, &o, NULL, eos_device_id);
}
static void EOS_CALL eos_connection_request(const EosRequestInfo *d) {   /* (any player may open one: what it sends must still carry the pair's secret, net_relay.c) */
    EosAcceptOptions o = {1, eos_me, d->RemoteUserId, &eos_sock};
    p_EOS_P2P_AcceptConnection(eos_p2p, &o);
}

static char eos_cred[5][128];
static int eos_read_credentials(const char *dir) {
    static const char *const KEYS[5] = {"ProductId", "SandboxId", "DeploymentId", "ClientId", "ClientSecret"};
    char path[MAX_PATH + 16], line[256];
    snprintf(path, sizeof path, "%s\\eos.txt", dir);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        char *e = strchr(line, '=');
        if (line[0] == '#' || !e) continue;
        *e++ = 0;
        e[strcspn(e, "\r\n")] = 0;
        for (int k = 0; k < 5; k++) if (!strcmp(line, KEYS[k])) snprintf(eos_cred[k], sizeof eos_cred[k], "%s", e);
    }
    fclose(f);
    for (int k = 0; k < 5; k++) if (!eos_cred[k][0]) { fprintf(stderr, "[eos] eos.txt has no %s: off\n", KEYS[k]); return 0; }
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
        net_eos_packet(id, buf, n);
    }
    for (;;) {
        AcquireSRWLockExclusive(&eos_q_lock);
        if (eos_q_head == eos_q_tail) { ReleaseSRWLockExclusive(&eos_q_lock); break; }
        u32 k = eos_q_head++ % QUEUE;
        char to[NET_EOS_ID_LEN + 1];
        u8 data[NET_EOS_MAX];
        u32 len = eos_queue[k].len;
        memcpy(to, eos_queue[k].to, sizeof to);
        memcpy(data, eos_queue[k].data, len);
        ReleaseSRWLockExclusive(&eos_q_lock);
        EOS_ProductUserId peer = p_EOS_ProductUserId_FromString(to);
        EosSendPacketOptions o = {3, eos_me, peer, &eos_sock, 0, len, data, 0, EOS_PR_UNRELIABLE_UNORDERED, 0};
        if (p_EOS_P2P_SendPacket(eos_p2p, &o) == EOS_SUCCESS) InterlockedIncrement64(&eos_sent);
        else InterlockedIncrement64(&eos_dropped);
    }
}

/* ---- the hub: one EOS lobby per lobby code and build (more than 64 players: another one) -------------------
 * Every game of a lobby code joins a hub, an EOS lobby found by its bucket ("ssbu1:<build>:<code>") or made when
 * none has room, and gives itself a virtual address (10.13.x.y, from its EOS id) as a member attribute "IP". Two
 * members with the same address: the one whose id sorts later picks another. The hub's members and their
 * addresses are what net_eos_net.c sends to - no lobby server of ours (plan/EOS_MIGRATION.md, phase 1). */
static EOS_HLobby eos_lobby;
static char hub_bucket[128], hub_id[128];
static volatile LONG hub_state;   /* 0 none asked, 1 searching, 2 joining/creating, 3 in, -1 failed */
static u32 hub_salt, hub_mine, hub_set;   /* (the address asked for, the one the hub has for us) */
static volatile LONG hub_ip;      /* ours once the hub agrees (no other member has it) */
static u64 hub_refresh_at, hub_retry_at, hub_merge_at;
static int hub_merging;   /* (alone in our hub: another of the same bucket, whose owner sorts first, is joined instead) */
static EOS_HLobbySearch hub_search;
static SRWLOCK hub_lock = SRWLOCK_INIT;
static struct { char id[NET_EOS_ID_LEN + 1]; u32 ip; } hub_members[64];
static int hub_count;

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
    hub_set = hub_mine;
}
static void hub_refresh(void) {   /* (EOS thread) the members and their addresses, from the lobby's details */
    EosCopyDetailsOptions o = {1, hub_id, eos_me};
    EOS_HLobbyDetails det = NULL;
    if (p_EOS_Lobby_CopyLobbyDetailsHandle(eos_lobby, &o, &det) != EOS_SUCCESS) return;
    EosVersionOnly v = {1};
    u32 n = p_EOS_LobbyDetails_GetMemberCount(det, &v), clash = 0;
    struct { char id[NET_EOS_ID_LEN + 1]; u32 ip; } got[64];
    int k = 0;
    for (u32 i = 0; i < n && k < 64; i++) {
        EosIndexOptions io = {1, i};
        EOS_ProductUserId who = p_EOS_LobbyDetails_GetMemberByIndex(det, &io);
        char id[NET_EOS_ID_LEN + 8];
        int32_t idn = sizeof id;
        if (!who || p_EOS_ProductUserId_ToString(who, id, &idn) != EOS_SUCCESS) continue;
        EosCopyMemberAttrOptions ao = {1, who, "IP"};
        EosAttr *at = NULL;
        u32 ip = 0;
        if (p_EOS_LobbyDetails_CopyMemberAttributeByKey(det, &ao, &at) == EOS_SUCCESS && at) {
            if (at->Data && at->Data->ValueType == EOS_AT_INT64) ip = (u32)at->Data->Value.AsInt64;
            p_EOS_Lobby_Attribute_Release(at);
        }
        if (strcmp(id, eos_my_text) && ip && ip == hub_set && strcmp(id, eos_my_text) < 0) clash = 1;   /* (theirs first: we move) */
        snprintf(got[k].id, sizeof got[k].id, "%s", id);
        got[k++].ip = ip;
    }
    p_EOS_LobbyDetails_Release(det);
    AcquireSRWLockExclusive(&hub_lock);
    memcpy(hub_members, got, sizeof got[0] * (size_t)k);
    hub_count = k;
    ReleaseSRWLockExclusive(&hub_lock);
    int confirmed = 0;
    for (int i = 0; i < k; i++) if (!strcmp(got[i].id, eos_my_text) && got[i].ip == hub_set && hub_set) confirmed = 1;
    if (clash) { hub_salt++; InterlockedExchange(&hub_ip, 0); hub_claim(); return; }
    if (confirmed && (u32)hub_ip != hub_set) {
        InterlockedExchange(&hub_ip, (LONG)hub_set);
        fprintf(stderr, "[eos] in hub %s as 10.13.%u.%u (%d players there)\n", hub_bucket, hub_set >> 8 & 255, hub_set & 255, k);
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
    hub_claim();
    hub_refresh_at = 0;
}
static void hub_create(void) {
    EosCreateLobbyOptions o;
    memset(&o, 0, sizeof o);
    o.ApiVersion = 10; o.LocalUserId = eos_me; o.MaxLobbyMembers = 64; o.PermissionLevel = EOS_LPL_PUBLICADVERTISED;
    o.BucketId = hub_bucket; o.bAllowInvites = 1; o.bCrossplayOptOut = 0;
    p_EOS_Lobby_CreateLobby(eos_lobby, &o, NULL, hub_entered);
}
static void EOS_CALL hub_left(const EosLobbyResult *d) { (void)d; }
static void EOS_CALL hub_found(const EosFindInfo *d) {
    EosVersionOnly v = {1};
    u32 n = d->ResultCode == EOS_SUCCESS ? p_EOS_LobbySearch_GetSearchResultCount(hub_search, &v) : 0;
    if (hub_merging) {   /* (two games made a hub each at the same moment: the one whose owner sorts later moves) */
        hub_merging = 0;
        for (u32 i = 0; i < n; i++) {
            EosIndexOptions io = {1, i};
            EOS_HLobbyDetails det = NULL;
            if (p_EOS_LobbySearch_CopySearchResultByIndex(hub_search, &io, &det) != EOS_SUCCESS) continue;
            EOS_ProductUserId owner = p_EOS_LobbyDetails_GetLobbyOwner(det, &v);
            char id[NET_EOS_ID_LEN + 8];
            int32_t idn = sizeof id;
            if (owner && p_EOS_ProductUserId_ToString(owner, id, &idn) == EOS_SUCCESS && strcmp(id, eos_my_text) < 0) {
                fprintf(stderr, "[eos] hub: another one of this lobby code (made at the same moment): moving there\n");
                EosLobbyUserOptions lo = {1, eos_me, hub_id};
                p_EOS_Lobby_LeaveLobby(eos_lobby, &lo, NULL, hub_left);
                InterlockedExchange(&hub_ip, 0);
                hub_set = 0;
                AcquireSRWLockExclusive(&hub_lock); hub_count = 0; ReleaseSRWLockExclusive(&hub_lock);
                InterlockedExchange(&hub_state, 2);
                EosJoinLobbyOptions j = {5, det, eos_me, 0, NULL, 0, 0};
                p_EOS_Lobby_JoinLobby(eos_lobby, &j, NULL, hub_entered);
                p_EOS_LobbyDetails_Release(det);
                break;
            }
            p_EOS_LobbyDetails_Release(det);
        }
        p_EOS_LobbySearch_Release(hub_search);
        hub_search = NULL;
        return;
    }
    InterlockedExchange(&hub_state, 2);
    if (n) {
        EosIndexOptions io = {1, 0};
        EOS_HLobbyDetails det = NULL;
        if (p_EOS_LobbySearch_CopySearchResultByIndex(hub_search, &io, &det) == EOS_SUCCESS) {
            EosJoinLobbyOptions j = {5, det, eos_me, 0, NULL, 0, 0};
            p_EOS_Lobby_JoinLobby(eos_lobby, &j, NULL, hub_entered);
            p_EOS_LobbyDetails_Release(det);
            p_EOS_LobbySearch_Release(hub_search);
            hub_search = NULL;
            return;
        }
    }
    p_EOS_LobbySearch_Release(hub_search);
    hub_search = NULL;
    hub_create();   /* (none with room: a new one) */
}
static void hub_find(void) {
    EosCreateSearchOptions so = {1, 10};
    if (p_EOS_Lobby_CreateLobbySearch(eos_lobby, &so, &hub_search) != EOS_SUCCESS) { hub_retry_at = GetTickCount64() + 2000; return; }
    EosAttrData b = {1, "bucket", {0}, EOS_AT_STRING};
    b.Value.AsUtf8 = hub_bucket;
    EosSetParameterOptions p = {1, &b, EOS_CO_EQUAL};
    p_EOS_LobbySearch_SetParameter(hub_search, &p);
    EosAttrData s = {1, "minslotsavailable", {0}, EOS_AT_INT64};
    s.Value.AsInt64 = 1;
    EosSetParameterOptions ps = {1, &s, EOS_CO_GREATERTHANOREQUAL};
    p_EOS_LobbySearch_SetParameter(hub_search, &ps);
    EosFindOptions f = {1, eos_me};
    if (!hub_merging) InterlockedExchange(&hub_state, 1);
    p_EOS_LobbySearch_Find(hub_search, &f, NULL, hub_found);
}
static void EOS_CALL hub_member_status(const EosMemberStatusInfo *d) { (void)d; hub_refresh_at = 0; }
static void EOS_CALL hub_member_update(const EosMemberUpdateInfo *d) { (void)d; hub_refresh_at = 0; }
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
    }
    if (hub_state == 0 && t >= hub_retry_at) hub_find();
    if (hub_state == 3 && t >= hub_refresh_at) { hub_refresh_at = t + 2000; hub_refresh(); }
    if (hub_state == 3 && hub_count <= 1 && !hub_search && t >= hub_merge_at) { hub_merge_at = t + 5000; hub_merging = 1; hub_find(); }
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
    if (nid && *nid == '1') { EosDeleteDeviceIdOptions o = {1}; p_EOS_Connect_DeleteDeviceId(eos_con, &o, NULL, eos_device_id_gone); }
    else { EosCreateDeviceIdOptions o = {1, "PC"}; p_EOS_Connect_CreateDeviceId(eos_con, &o, NULL, eos_device_id); }
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
            fprintf(stderr, "[eos] ready in %llu ms: the relay of last resort is Epic's (player %s)\n", (unsigned long long)(GetTickCount64() - t0), eos_my_text);
            told = 1;
        }
        if (ph < 0) { fprintf(stderr, "[eos] off: the lobby's relay stays the last resort\n"); return 0; }
        if (ph == 2) { eos_pump(); hub_tick(); }
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
    if (!dll) { fprintf(stderr, "[eos] no EOSSDK-Win64-Shipping.dll beside the program: off\n"); return; }
    eos_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    InterlockedExchange(&eos_phase, 1);
    HANDLE t = CreateThread(NULL, 0, eos_thread_, dll, 0, NULL);
    if (t) CloseHandle(t);
}

int eos_ready(void) { return eos_phase == 2 && eos_my_text[0]; }
const char *eos_my_id(void) { return eos_ready() ? eos_my_text : NULL; }

int eos_send(const char *to, const void *data, u32 len) {
    if (!eos_ready() || len > NET_EOS_MAX || !to || strlen(to) != NET_EOS_ID_LEN) return 0;
    AcquireSRWLockExclusive(&eos_q_lock);
    if (eos_q_tail - eos_q_head >= QUEUE) { ReleaseSRWLockExclusive(&eos_q_lock); InterlockedIncrement64(&eos_dropped); return 0; }
    u32 k = eos_q_tail++ % QUEUE;
    memcpy(eos_queue[k].to, to, NET_EOS_ID_LEN + 1);
    eos_queue[k].len = len;
    memcpy(eos_queue[k].data, data, len);
    ReleaseSRWLockExclusive(&eos_q_lock);
    SetEvent(eos_wake);
    return 1;
}

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
int eos_hub_member_id(u32 ip, char id[NET_EOS_ID_LEN + 1]) {   /* 1: the member at that address */
    int found = 0;
    AcquireSRWLockShared(&hub_lock);
    for (int i = 0; i < hub_count && !found; i++)
        if (hub_members[i].ip == ip && strcmp(hub_members[i].id, eos_my_text)) { memcpy(id, hub_members[i].id, NET_EOS_ID_LEN + 1); found = 1; }
    ReleaseSRWLockShared(&hub_lock);
    return found;
}

#else   /* (Linux, macOS: not yet) */
void eos_hub_join(const char *bucket) { (void)bucket; }
u32 eos_hub_ip(void) { return 0; }
int eos_hub_failed(void) { return 1; }
int eos_hub_members(char ids[][NET_EOS_ID_LEN + 1], u32 *ips, int max) { (void)ids; (void)ips; (void)max; return 0; }
int eos_hub_member_ip(const char *id, u32 *ip) { (void)id; (void)ip; return 0; }
int eos_hub_member_id(u32 ip, char id[NET_EOS_ID_LEN + 1]) { (void)ip; (void)id; return 0; }
void eos_start(void) {}
int eos_ready(void) { return 0; }
const char *eos_my_id(void) { return NULL; }
int eos_send(const char *to, const void *data, u32 len) { (void)to; (void)data; (void)len; return 0; }
void eos_counts(u64 *a, u64 *b, u64 *c) { *a = *b = *c = 0; }
#endif
