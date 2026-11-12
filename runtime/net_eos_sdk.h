/* The few declarations of Epic Online Services' C SDK (version 1.19) that the runtime uses, written out so that the
 * build needs no SDK (net_eos.c loads the library and every function below by name), and what net_eos.c shares with
 * net_eos_lobby.c. Windows only for now. */
#ifndef NET_EOS_SDK_H
#define NET_EOS_SDK_H
#ifdef _WIN32
#include <stdint.h>
#include "host.h"
#include "net_eos.h"

typedef int32_t EOS_Bool;
typedef int32_t EOS_EResult;
typedef struct EOS_PlatformHandle *EOS_HPlatform;
typedef struct EOS_ConnectHandle *EOS_HConnect;
typedef struct EOS_P2PHandle *EOS_HP2P;
typedef struct EOS_ProductUserIdDetails *EOS_ProductUserId;
typedef struct EOS_ContinuanceTokenDetails *EOS_ContinuanceToken;
typedef uint64_t EOS_NotificationId;
#define EOS_CALL __stdcall
enum { EOS_SUCCESS = 0, EOS_INVALID_USER = 3, EOS_ECT_DEVICEID_ACCESS_TOKEN = 10, EOS_ECT_DISCORD_ACCESS_TOKEN = 4, EOS_PR_UNRELIABLE_UNORDERED = 0, EOS_PR_RELIABLE_ORDERED = 2 };
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
typedef struct { int32_t ApiVersion; EOS_ProductUserId LocalUserId; EOS_ContinuanceToken ContinuanceToken; } EosLinkAccountOptions;   /* (API 1) */
typedef struct { EOS_EResult ResultCode; void *ClientData; EOS_ProductUserId LocalUserId; } EosLinkAccountInfo;
typedef struct { int32_t ApiVersion; EOS_ProductUserId PrimaryLocalUserId, LocalDeviceUserId, ProductUserIdToPreserve; } EosTransferDeviceIdOptions;   /* (API 1) */
typedef struct { EOS_EResult ResultCode; void *ClientData; EOS_ProductUserId LocalUserId; } EosTransferDeviceIdInfo;
typedef struct { int32_t ApiVersion; const char *DeviceModel; } EosCreateDeviceIdOptions;                     /* (API 1) */
typedef struct { int32_t ApiVersion; } EosDeleteDeviceIdOptions;                                               /* (API 1) */
typedef struct { EOS_EResult ResultCode; void *ClientData; } EosDeviceIdInfo;                                  /* (create and delete) */
typedef struct { int32_t ApiVersion; char SocketName[33]; } EosSocketId;                                       /* (API 1) */
typedef struct { int32_t ApiVersion; EOS_ProductUserId LocalUserId, RemoteUserId; const EosSocketId *SocketId; uint8_t Channel; uint32_t DataLengthBytes;
                 const void *Data; EOS_Bool bAllowDelayedDelivery; int32_t Reliability; EOS_Bool bDisableAutoAcceptConnection; } EosSendPacketOptions;   /* (API 3) */
typedef struct { int32_t ApiVersion; EOS_ProductUserId LocalUserId; uint32_t MaxDataSizeBytes; const uint8_t *RequestedChannel; } EosReceivePacketOptions;   /* (API 2) */
typedef struct { int32_t ApiVersion; EOS_ProductUserId LocalUserId, RemoteUserId; const EosSocketId *SocketId; } EosAcceptOptions;   /* (API 1) */
typedef struct { int32_t ApiVersion; EOS_ProductUserId LocalUserId; const EosSocketId *SocketId; } EosNotifyRequestOptions;            /* (API 1) */
typedef struct { void *ClientData; EOS_ProductUserId LocalUserId, RemoteUserId; const EosSocketId *SocketId; } EosRequestInfo;
typedef struct { int32_t ApiVersion; int32_t RelayControl; } EosRelayControlOptions;                           /* (API 1) */
enum { EOS_RC_FORCE_RELAYS = 2 };

/* Lobbies (eos_lobby_types.h) */
typedef struct EOS_LobbyHandle *EOS_HLobby;
typedef struct EOS_LobbySearchHandle *EOS_HLobbySearch;
typedef struct EOS_LobbyDetailsHandle *EOS_HLobbyDetails;
typedef struct EOS_LobbyModificationHandle *EOS_HLobbyModification;
enum { EOS_AT_INT64 = 1, EOS_AT_STRING = 3, EOS_LAT_PUBLIC = 0, EOS_CO_EQUAL = 0, EOS_CO_GREATERTHANOREQUAL = 3, EOS_CO_DISTANCE = 6, EOS_LPL_PUBLICADVERTISED = 0,
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
typedef struct { int32_t ApiVersion; const char *LobbyId; EOS_ProductUserId LocalUserId; EOS_Bool bPresenceEnabled; const void *LocalRTCOptions;
                 EOS_Bool bCrossplayOptOut; int32_t RTCRoomJoinActionType; } EosJoinByIdOptions;   /* (API 3) */
typedef struct { int32_t ApiVersion; const char *AttrKey; } EosKeyOptions;                                     /* (API 1) */
typedef struct { void *ClientData; const char *LobbyId; } EosLobbyUpdateInfo;
typedef void (EOS_CALL *LobbyCb)(const EosLobbyResult *);
typedef void (EOS_CALL *LobbyUpdateCb)(const EosLobbyUpdateInfo *);
/* Stats (eos_stats_types.h) */
typedef struct EOS_StatsHandle *EOS_HStats;
typedef struct { int32_t ApiVersion; const char *StatName; int32_t IngestAmount; } EosIngestData;              /* (API 1) */
typedef struct { int32_t ApiVersion; EOS_ProductUserId LocalUserId; const EosIngestData *Stats; uint32_t StatsCount; EOS_ProductUserId TargetUserId; } EosIngestOptions;   /* (API 3) */
typedef struct { EOS_EResult ResultCode; void *ClientData; EOS_ProductUserId LocalUserId, TargetUserId; } EosIngestInfo;
typedef void (EOS_CALL *IngestCb)(const EosIngestInfo *);
typedef void (EOS_CALL *FindCb)(const EosFindInfo *);
typedef void (EOS_CALL *MemberStatusCb)(const EosMemberStatusInfo *);
typedef void (EOS_CALL *MemberUpdateCb)(const EosMemberUpdateInfo *);

typedef void (EOS_CALL *LoginCb)(const EosLoginInfo *);
typedef void (EOS_CALL *CreateUserCb)(const EosCreateUserInfo *);
typedef void (EOS_CALL *DeviceIdCb)(const EosDeviceIdInfo *);
typedef void (EOS_CALL *LinkAccountCb)(const EosLinkAccountInfo *);
typedef void (EOS_CALL *TransferDeviceIdCb)(const EosTransferDeviceIdInfo *);
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
    X(void, EOS_Connect_LinkAccount, (EOS_HConnect, const EosLinkAccountOptions *, void *, LinkAccountCb)) \
    X(void, EOS_Connect_TransferDeviceIdAccount, (EOS_HConnect, const EosTransferDeviceIdOptions *, void *, TransferDeviceIdCb)) \
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
    X(void, EOS_Lobby_Attribute_Release, (EosAttr *)) \
    X(void, EOS_Lobby_DestroyLobby, (EOS_HLobby, const EosLobbyUserOptions *, void *, LobbyCb)) \
    X(void, EOS_Lobby_JoinLobbyById, (EOS_HLobby, const EosJoinByIdOptions *, void *, LobbyCb)) \
    X(EOS_EResult, EOS_LobbyDetails_CopyAttributeByKey, (EOS_HLobbyDetails, const EosKeyOptions *, EosAttr **)) \
    X(EOS_NotificationId, EOS_Lobby_AddNotifyLobbyUpdateReceived, (EOS_HLobby, const EosVersionOnly *, void *, LobbyUpdateCb)) \
    X(EOS_HStats, EOS_Platform_GetStatsInterface, (EOS_HPlatform)) \
    X(void, EOS_Stats_IngestStat, (EOS_HStats, const EosIngestOptions *, void *, IngestCb)) \
    X(EOS_EResult, EOS_P2P_SetRelayControl, (EOS_HP2P, const EosRelayControlOptions *))
#define EOS_DECLARE_EXTERN(ret, name, args) extern ret (EOS_CALL *p_##name) args;
EOS_FUNCS(EOS_DECLARE_EXTERN)

/* (net_eos.c) the platform and this player, once eos_ready() */
extern EOS_HPlatform eos_plat;
extern EOS_ProductUserId eos_me;
EOS_HLobby eos_lobby_interface(void);
/* (net_eos.c, the EOS thread) net_eos_lobby.c's share of every tick, and its P2P messages (channel 1) */
void nexeos_tick(void);
void nexeos_packet(const char *from, const u8 *data, u32 len);
#endif
#endif
