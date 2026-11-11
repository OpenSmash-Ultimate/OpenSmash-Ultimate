"""RMC, NEX's remote method calls, carried in PRUDP DATA messages.

Request:  u32 size | protocol | 0x80 (0xFF, u16 protocol if >= 0x7F) | u32 call id | u32 method | parameters
Response: u32 size | protocol | u8 1 | u32 call id | u32 method | 0x8000 | data
          u32 size | protocol | u8 0 | u32 error | u32 call id"""
import struct

SUCCESS = 0x00010001
CORE_NOT_IMPLEMENTED = 0x80010002
CORE_ACCESS_DENIED = 0x80010006
CORE_INVALID_ARGUMENT = 0x8001000A
CORE_BUFFER_OVERFLOW = 0x8001000F
RENDEZVOUS_NOT_AUTHENTICATED = 0x80030002
RENDEZVOUS_INVALID_USERNAME = 0x80030064
RENDEZVOUS_ACCESS_DENIED = 0x8003006C
RENDEZVOUS_SESSION_VOID = 0x8003006D
RENDEZVOUS_SESSION_FULL = 0x800300C8
RENDEZVOUS_NOT_PARTICIPATED = 0x800300D4
RENDEZVOUS_PERMISSION_DENIED = 0x800300D9
RENDEZVOUS_SESSION_CLOSED = 0x800300DB
RENDEZVOUS_LIMIT_EXCEEDED = 0x800300DF

PROTOCOLS = {1: "RemoteLog", 3: "NATTraversal", 10: "TicketGranting", 11: "SecureConnection",
             14: "Notification", 21: "MatchMaking", 50: "MatchMakingExt", 100: "NintendoNotification", 109: "MatchmakeExtension",
             110: "Utility", 115: "DataStore", 120: "MatchmakeReferee", 124: "Screening", 201: "Tournament"}
METHODS = {
    50: {1: "EndParticipation", 2: "GetParticipants", 3: "GetDetailedParticipants", 4: "GetParticipantsURLs",
         5: "GetGatheringRelations", 6: "DeleteFromDeletions"},
    3: {1: "RequestProbeInitiation", 2: "InitiateProbe", 3: "RequestProbeInitiationExt", 4: "ReportNATTraversalResult",
        5: "ReportNATProperties", 6: "GetRelaySignatureKey", 7: "ReportNATTraversalResultDetail"},
    10: {1: "ValidateAndRequestTicket", 2: "ValidateAndRequestTicketWithCustomData", 3: "RequestTicket", 4: "GetPID",
         5: "GetName", 6: "ValidateAndRequestTicketWithParam"},
    11: {1: "Register", 2: "RequestConnectionData", 3: "RequestUrls", 4: "RegisterEx", 5: "TestConnectivity",
         6: "UpdateURLs", 7: "ReplaceURL", 8: "SendReport"},
    21: {1: "RegisterGathering", 2: "UnregisterGathering", 41: "GetSessionURLs", 42: "UpdateSessionHost",
         44: "MigrateGatheringOwnership"},
    109: {1: "CloseParticipation", 2: "OpenParticipation", 8: "ModifyCurrentGameAttribute", 11: "UpdateApplicationBuffer",
          12: "UpdateMatchmakeSessionAttribute", 17: "CreateCommunity", 18: "UpdateCommunity", 19: "JoinCommunity",
          20: "FindCommunityByGatheringId", 21: "FindOfficialCommunity", 22: "FindCommunityByParticipant",
          34: "UpdateProgressScore", 36: "GenerateMatchmakeSessionSystemPassword",
          37: "ClearMatchmakeSessionSystemPassword", 38: "CreateMatchmakeSessionWithParam",
          39: "JoinMatchmakeSessionWithParam", 40: "AutoMatchmakeWithParam_Postpone",
          41: "FindMatchmakeSessionByGatheringIdDetail", 44: "UpdateMatchmakeSessionPart",
          48: "FindMatchmakeSessionByGatheringId", 49: "FindMatchmakeSessionBySingleGatheringId",
          50: "FindMatchmakeSessionByOwner", 51: "FindMatchmakeSessionByParticipant",
          52: "BrowseMatchmakeSessionNoHolderNoResultRange", 54: "FindCommunityByOwner",
          55: "GetArenaId (SSBU)", 56: "method 56 (SSBU)", 58: "method 58 (SSBU)"},
    110: {7: "GetIntegerSettings", 8: "GetStringSettings", 11: "GetGameEvents"},
    120: {1: "StartRound", 2: "GetStartRoundParam", 3: "EndRound", 4: "EndRoundWithoutReport", 12: "GetOrCreateStats"},
}


def name_of(protocol, method=None):
    p = PROTOCOLS.get(protocol, f"protocol {protocol}")
    if method is None:
        return p
    return f"{p}.{METHODS.get(protocol, {}).get(method, f'method {method}')}"


def protocol_bytes(protocol, request):
    flag = 0x80 if request else 0
    if protocol < 0x7F:
        return bytes((protocol | flag,))
    return bytes((0x7F | flag,)) + struct.pack("<H", protocol)


class Message:
    __slots__ = ("protocol", "request", "call_id", "method", "body", "success", "error")

    def __repr__(self):
        if self.request:
            return f"<request {name_of(self.protocol, self.method)} call {self.call_id} {len(self.body)} B>"
        if self.success:
            return f"<response {name_of(self.protocol, self.method)} call {self.call_id} {len(self.body)} B>"
        return f"<error {self.error:#x} {name_of(self.protocol)} call {self.call_id}>"


def decode(data):
    if len(data) < 5:
        raise ValueError("short RMC message")
    size = struct.unpack_from("<I", data, 0)[0]
    data = data[4:4 + size]
    m = Message()
    b = data[0]
    m.request = bool(b & 0x80)
    m.protocol = b & 0x7F
    i = 1
    if m.protocol == 0x7F:
        m.protocol = struct.unpack_from("<H", data, 1)[0]
        i = 3
    m.success, m.error, m.method = True, 0, 0
    if m.request:
        m.call_id, m.method = struct.unpack_from("<II", data, i)
        m.body = data[i + 8:]
    else:
        m.success = data[i] == 1
        if m.success:
            m.call_id, m.method = struct.unpack_from("<II", data, i + 1)
            m.method &= 0x7FFF
            m.body = data[i + 9:]
        else:
            m.error, m.call_id = struct.unpack_from("<II", data, i + 1)
            m.body = b""
    return m


def _sized(b):
    return struct.pack("<I", len(b)) + b


def request(protocol, call_id, method, body):
    return _sized(protocol_bytes(protocol, True) + struct.pack("<II", call_id, method) + body)


def response(protocol, call_id, method, body):
    return _sized(protocol_bytes(protocol, False) + b"\1" + struct.pack("<II", call_id, method | 0x8000) + body)


def error(protocol, call_id, code):
    return _sized(protocol_bytes(protocol, False) + b"\0" + struct.pack("<II", code, call_id))


class NexError(Exception):
    def __init__(self, code, why=""):
        super().__init__(f"{code:#x} {why}")
        self.code = code
