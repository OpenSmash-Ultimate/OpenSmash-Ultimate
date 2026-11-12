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
    X(const char *, EOS_EResult_ToString, (EOS_EResult))
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
        if (ph == 2) eos_pump();
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

#else   /* (Linux, macOS: not yet) */
void eos_start(void) {}
int eos_ready(void) { return 0; }
const char *eos_my_id(void) { return NULL; }
int eos_send(const char *to, const void *data, u32 len) { (void)to; (void)data; (void)len; return 0; }
void eos_counts(u64 *a, u64 *b, u64 *c) { *a = *b = *c = 0; }
#endif
