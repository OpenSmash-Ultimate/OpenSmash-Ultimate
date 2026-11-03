/* Online play: nn::ldn (Local Wireless) and nn::socket on a virtual network carried by our lobby (net_relay.h).
 * Adapted from SNO2207/WindowSmash (runtime/hle_net.c: ldn state machine, NetworkInfo handling, virtual UDP sockets),
 * used with the author's permission; our own implementation, on this runtime's import tables.
 *
 * OFF unless net_online(): nnldn_register() then returns at once, and every import stays what nnnet_c4a.inc made it
 * (an OFFLINE console: ldn::Initialize = 0x2ecb, loopback-only Winsock sockets). Nothing here runs offline.
 *
 * ON: nnldn_register() replaces the nn::ldn imports and the data calls of nn::socket (Socket, Bind, SendTo, RecvFrom,
 * Poll, ...; Initialize / Finalize / errno / the resolver stay nnnet_c4a.inc's).
 *
 * nn::ldn as modelled. [sdk 0x2bcd20] AttachStateChangeEvent attaches the service's event to the game's SystemEvent with
 * EventClearMode AutoClear (w3 = 1 for AttachReadableHandleToSystemEvent); the event is signalled on every state change and
 * whenever the network's node list changes. States 0 None, 1 Initialized, 2 AccessPoint, 3 AccessPointCreated, 4 Station,
 * 5 StationConnected. A host's network is its NetworkInfo (0x480 bytes, layout below, the one WindowSmash's code uses; the
 * game reads it back through GetNetworkInfo, Scan / ScanPrivate): the host broadcasts it to its room every second (and
 * at once on any change) as an ADVERTISE message on the reserved port 65511; the lobby keeps the latest one per host
 * and answers a scan with the open rooms of the same build and lobby code. A station asks the host (JOIN with session
 * id, user name, version), the host adds a node and answers ACCEPT with the network; stations send KEEP every second;
 * 6 s of silence either way ends the connection (DisconnectReason 6 SignalLost).
 *   NetworkInfo: +0 local communication id u64, +0xa scene id u16, +0x10 session id[16], +0x20 BSSID[6],
 *   +0x26 SSID length, +0x27 SSID[33], +0x48 channel u16, +0x4a link level, +0x4b network type (2 = ldn),
 *   +0x50 security parameter data[16], +0x60 security mode u16, +0x62 accept policy, +0x66 node count max,
 *   +0x67 node count, +0x68 eight nodes of 0x40 (+0 ipv4 u32, +4 MAC[6], +0xa node id, +0xb connected, +0xc user
 *   name[33], +0x2e local communication version s16), +0x26a advertise data size u16, +0x26c advertise data[0x180].
 *   NOT VERIFIED against a console: the Result codes for "no answer" (0x86cb: 203-67, chosen from how the game handles it)
 *   and wrong state (0x40cb: 203-32), the MAC and SSID contents (made up from the address and the session id), link level 3, channel 6.
 *
 * nn::socket as modelled while online: up to 128 UDP sockets (TCP: socket only; connect refused, accept waits for ever) on
 * the virtual network: this player's address, 10.13.255.255 / 255.255.255.255 broadcasts and 127.0.0.1. Numbers as
 * nnnet_c4a.inc's (BSD sockaddr_in with length byte, SOL_SOCKET 0xffff, O_NONBLOCK 0x800, MSG_PEEK 2, MSG_DONTWAIT 0x80,
 * the SDK's errno values through __errno_location). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hostrt.h"
#include "net_relay.h"

void os_init_event(uint64_t key, int autoclear);          /* os_threads.c */
void os_signal_event(uint64_t key);
void input_mark(const char *what);                         /* nnhid.c: lets an input sequence wait for "@ldn:..." */

#define RET(v) (c->x[0] = (uint64_t)(v))
#define RETI(v) (c->x[0] = (uint64_t)(int64_t)(v))
#define PTR(a) ((uint8_t *)(uintptr_t)(a))
static uint32_t ld32(uint64_t a) { uint32_t v; memcpy(&v, PTR(a), 4); return v; }
static uint16_t ld16(uint64_t a) { uint16_t v; memcpy(&v, PTR(a), 2); return v; }
static void st32(uint64_t a, uint32_t v) { memcpy(PTR(a), &v, 4); }
static void st16(uint64_t a, uint16_t v) { memcpy(PTR(a), &v, 2); }
static uint64_t ms_now(void) { return GetTickCount64(); }
static int LOGL(int n) { return net_log_level() >= n; }
static void ip_str(uint32_t ip, char *b) { snprintf(b, 16, "%u.%u.%u.%u", ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255); }

/* ================================================================== nn::ldn */
#define R_LDN_STATE   0x40cbu        /* 203-32: the call does not fit the state */
#define R_LDN_NONET   0x86cbu        /* 203-67: no answer from the room. The game's Pia layer (ConvertLdnResult 0x710016b2f0) maps
                                         0x86cb to its own 0x6838, the only ldn result after which it calls Connect once more
                                         (0x710016f21c); 0x80cb would fall into its generic error branch [binary, 13.0.4] */
#define R_LDN_OFF     0x2ecbu        /* 203-23: wireless off (the offline answer, nnnet_c4a.inc), also when the lobby is gone */
enum { LS_NONE, LS_INIT, LS_AP, LS_AP_CREATED, LS_STA, LS_STA_CONNECTED };
enum { M_ADVERTISE = 1, M_JOIN, M_ACCEPT, M_REJECT, M_KEEP, M_LEAVE, M_GONE, M_KICK };
enum { DR_NONE, DR_USER, DR_SYSTEM, DR_DESTROYED_BY_USER, DR_DESTROYED_BY_SYSTEM, DR_REJECTED, DR_SIGNAL_LOST };
#define NI_SIZE 0x480
#define NI_LCID 0x00
#define NI_SCENE 0x0a
#define NI_SESSION 0x10
#define NI_BSSID 0x20
#define NI_SSID_LEN 0x26
#define NI_SSID 0x27
#define NI_CHANNEL 0x48
#define NI_LINK 0x4a
#define NI_TYPE 0x4b
#define NI_SECPARAM 0x50
#define NI_SECMODE 0x60
#define NI_POLICY 0x62
#define NI_NODE_MAX 0x66
#define NI_NODE_COUNT 0x67
#define NI_NODES 0x68
#define NI_ADV_SIZE 0x26a
#define NI_ADV 0x26c
#define NODE(i) (NI_NODES + 0x40 * (i))
#define SEEN_MAX 24
#define SILENT_MS 6000

typedef struct { uint8_t info[NI_SIZE]; uint32_t from; uint64_t at; } Seen;
static struct {
    SRWLOCK lock;
    CONDITION_VARIABLE cv;
    int state, reason, answer, agent;
    uint64_t event;                       /* the game's SystemEventType (0: none attached) */
    uint8_t net[NI_SIZE];                 /* the network: our own (host) or the one joined (station) */
    uint64_t heard[8], host_heard;
    uint32_t join_host;
    uint8_t join_session[16];
    Seen seen[SEEN_MAX];
    uint32_t list_asked, list_filling, list_done;
    int list_next;
} L = {SRWLOCK_INIT, CONDITION_VARIABLE_INIT};

static uint32_t u32at(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void mac_of(uint8_t *mac, uint32_t ip) { mac[0] = 0x02; mac[1] = 0x53; mac[2] = (uint8_t)(ip >> 24); mac[3] = (uint8_t)(ip >> 16); mac[4] = (uint8_t)(ip >> 8); mac[5] = (uint8_t)ip; }
static void signal_game(void) { if (L.event) os_signal_event(L.event); }
static void set_state(int s) {            /* (lock held) */
    if (L.state != s && LOGL(1)) printf("[online] ldn: state %d -> %d\n", L.state, s);
    L.state = s;
    signal_game();
}
static void ldn_msg(uint32_t to, int type, const void *body, uint32_t len) {
    uint8_t b[4 + NI_SIZE + 8];
    if (len > sizeof b - 4) return;
    b[0] = 'L'; b[1] = (uint8_t)type; b[2] = b[3] = 0;
    memcpy(b + 4, body, len);
    net_send(net_my_ip(), VNET_LDN_PORT, to, VNET_LDN_PORT, b, 4 + len);
}
static void advertise(void) { ldn_msg(VNET_BROADCAST, M_ADVERTISE, L.net, NI_SIZE); }
static int node_index(uint32_t ip) {      /* (lock held) */
    for (int i = 0; i < 8; i++) if (L.net[NODE(i) + 0xb] && u32at(L.net + NODE(i)) == ip) return i;
    return -1;
}
static void remove_node(int i) {          /* (host, lock held) */
    if (LOGL(1)) printf("[online] ldn: %.32s left the room\n", (const char *)L.net + NODE(i) + 0xc);
    memset(L.net + NODE(i), 0, 0x40);
    L.net[NI_NODE_COUNT]--;
    advertise();
    signal_game();
}
static void station_lost(int reason) {    /* (station, lock held) */
    if (LOGL(1)) printf("[online] ldn: disconnected from the room (reason %d)\n", reason);
    L.reason = reason;
    set_state(LS_STA);
}

static DWORD WINAPI ldn_agent(LPVOID unused) {
    (void)unused;
    uint64_t last = 0;
    for (;;) {
        Sleep(200);
        uint64_t t = ms_now();
        AcquireSRWLockExclusive(&L.lock);
        int tick = t - last >= 1000;
        if (tick) last = t;
        if (L.state == LS_AP_CREATED) {
            if (tick) advertise();
            for (int i = 1; i < 8; i++) if (L.net[NODE(i) + 0xb] && t - L.heard[i] > SILENT_MS) remove_node(i);
        } else if (L.state == LS_STA_CONNECTED) {
            if (tick) ldn_msg(u32at(L.net + NODE(0)), M_KEEP, L.net + NI_SESSION, 16);
            if (t - L.host_heard > SILENT_MS) station_lost(DR_SIGNAL_LOST);
        }
        ReleaseSRWLockExclusive(&L.lock);
    }
}

static void ldn_receive(uint32_t from, const uint8_t *p, uint32_t n) {
    if (n < 4 || p[0] != 'L') return;
    const uint8_t *m = p + 4;
    uint32_t len = n - 4;
    uint64_t t = ms_now();
    const char *mark = NULL;
    AcquireSRWLockExclusive(&L.lock);
    switch (p[1]) {
    case M_ADVERTISE: {
        if (len < NI_SIZE || u32at(m + NODE(0)) != from) break;
        int slot = 0;
        for (int i = 0; i < SEEN_MAX; i++) {
            if (!memcmp(L.seen[i].info + NI_SESSION, m + NI_SESSION, 16)) { slot = i; break; }
            if (L.seen[i].at < L.seen[slot].at) slot = i;
        }
        memcpy(L.seen[slot].info, m, NI_SIZE);
        L.seen[slot].from = from;
        L.seen[slot].at = t;
        if (L.state == LS_STA_CONNECTED && !memcmp(L.net + NI_SESSION, m + NI_SESSION, 16) && from == u32at(L.net + NODE(0))) {
            L.host_heard = t;
            if (memcmp(L.net, m, NI_SIZE)) {
                memcpy(L.net, m, NI_SIZE);
                if (node_index(net_my_ip()) < 0) station_lost(DR_REJECTED); else signal_game();
            }
        }
        break;
    }
    case M_JOIN: {                        /* session[16], user name[32], version s16 */
        if (len < 50 || L.state != LS_AP_CREATED || memcmp(m, L.net + NI_SESSION, 16)) break;
        int node = node_index(from);
        if (node < 0 && L.net[NI_POLICY] != 1 && L.net[NI_NODE_COUNT] < L.net[NI_NODE_MAX])
            for (int i = 1; i < 8 && node < 0; i++)
                if (!L.net[NODE(i) + 0xb]) {
                    uint8_t *nd = L.net + NODE(i);
                    node = i;
                    memset(nd, 0, 0x40);
                    put32(nd, from);
                    mac_of(nd + 4, from);
                    nd[0xa] = (uint8_t)i;
                    nd[0xb] = 1;
                    memcpy(nd + 0xc, m + 16, 32);
                    memcpy(nd + 0x2e, m + 48, 2);
                    L.net[NI_NODE_COUNT]++;
                    if (LOGL(1)) printf("[online] ldn: %.32s joined the room as node %d\n", (const char *)nd + 0xc, i);
                    mark = "ldn:station-joined";
                }
        if (node < 0) { uint8_t why = 1; ldn_msg(from, M_REJECT, &why, 1); break; }
        L.heard[node] = t;
        ldn_msg(from, M_ACCEPT, L.net, NI_SIZE);
        advertise();
        signal_game();
        break;
    }
    case M_ACCEPT:
        if (len < NI_SIZE || L.state != LS_STA || L.answer || from != L.join_host || memcmp(m + NI_SESSION, L.join_session, 16)) break;
        memcpy(L.net, m, NI_SIZE);
        L.answer = 1;
        WakeAllConditionVariable(&L.cv);
        break;
    case M_REJECT:
        if (L.state == LS_STA && !L.answer && from == L.join_host) { L.answer = -1; WakeAllConditionVariable(&L.cv); }
        break;
    case M_KEEP: {
        int node = L.state == LS_AP_CREATED && len >= 16 && !memcmp(m, L.net + NI_SESSION, 16) ? node_index(from) : -1;
        if (node > 0) L.heard[node] = t;
        break;
    }
    case M_LEAVE: {
        int node = L.state == LS_AP_CREATED && len >= 16 && !memcmp(m, L.net + NI_SESSION, 16) ? node_index(from) : -1;
        if (node > 0) remove_node(node);
        break;
    }
    case M_GONE:
    case M_KICK:
        if (L.state == LS_STA_CONNECTED && len >= 16 && !memcmp(m, L.net + NI_SESSION, 16) && from == u32at(L.net + NODE(0)))
            station_lost(p[1] == M_GONE ? DR_DESTROYED_BY_USER : DR_REJECTED);
        break;
    }
    ReleaseSRWLockExclusive(&L.lock);
    if (mark) input_mark(mark);
}

/* the lobby's room list (net_relay.c): replaces the rooms seen, in its order */
static void list_begin(uint32_t seq) {    /* (lock held) */
    if (L.list_filling == seq) return;
    memset(L.seen, 0, sizeof L.seen);
    L.list_filling = seq;
    L.list_next = 0;
}
void nnldn_listing(uint32_t seq, uint32_t owner, const uint8_t *info) {
    AcquireSRWLockExclusive(&L.lock);
    if (seq == L.list_asked && seq != L.list_done && u32at(info + NODE(0)) == owner) {
        list_begin(seq);
        if (L.list_next < SEEN_MAX) {
            Seen *s = &L.seen[L.list_next++];
            memcpy(s->info, info, NI_SIZE);
            s->from = owner;
            s->at = ms_now();
        }
    }
    ReleaseSRWLockExclusive(&L.lock);
}
void nnldn_list_end(uint32_t seq) {
    AcquireSRWLockExclusive(&L.lock);
    if (seq == L.list_asked) { list_begin(seq); L.list_done = seq; WakeAllConditionVariable(&L.cv); }
    ReleaseSRWLockExclusive(&L.lock);
}

static int wait_for_address(int ms) {     /* the lobby may still be answering when the game opens Local Wireless */
    for (int i = 0; i < ms / 50 && !net_my_ip() && !net_lobby_gone(); i++) Sleep(50);
    return net_my_ip() != 0;
}

static void ldn_Initialize(Ctx *c) {
    net_start();
    if (!wait_for_address(3000)) {
        if (LOGL(1)) printf("[online] ldn: Initialize: no answer from the lobby -> 0x2ecb (as offline)\n");
        RET(R_LDN_OFF);
        return;
    }
    AcquireSRWLockExclusive(&L.lock);
    if (!L.agent) {
        HANDLE th = CreateThread(NULL, 0, ldn_agent, NULL, 0, NULL);
        L.agent = 1;
        if (th) { SetThreadDescription(th, L"online local wireless"); CloseHandle(th); }
    }
    if (L.state == LS_NONE) set_state(LS_INIT);
    ReleaseSRWLockExclusive(&L.lock);
    if (LOGL(1)) printf("[online] ldn: Initialize -> 0\n");
    RET(0);
}
static void ldn_Finalize(Ctx *c) {
    AcquireSRWLockExclusive(&L.lock);
    if (L.state == LS_AP_CREATED) ldn_msg(VNET_BROADCAST, M_GONE, L.net + NI_SESSION, 16);
    if (L.state == LS_STA_CONNECTED) ldn_msg(u32at(L.net + NODE(0)), M_LEAVE, L.net + NI_SESSION, 16);
    L.state = LS_NONE;
    ReleaseSRWLockExclusive(&L.lock);
    if (LOGL(1)) printf("[online] ldn: Finalize\n");
    (void)c;
}
static void ldn_GetState(Ctx *c) { RET((uint32_t)L.state); }
static void ldn_AttachStateChangeEvent(Ctx *c) {
    L.event = c->x[0];
    os_init_event(L.event, 1);
    if (LOGL(2)) printf("[online] ldn: state change event %llx attached\n", (unsigned long long)L.event);
}
static void ldn_OpenStation(Ctx *c) {
    AcquireSRWLockExclusive(&L.lock);
    int ok = L.state == LS_INIT || L.state == LS_STA;
    if (ok) { L.reason = DR_NONE; set_state(LS_STA); }
    ReleaseSRWLockExclusive(&L.lock);
    RET(ok ? 0 : R_LDN_STATE);
}
static void ldn_CloseStation(Ctx *c) {
    AcquireSRWLockExclusive(&L.lock);
    if (L.state == LS_STA_CONNECTED) ldn_msg(u32at(L.net + NODE(0)), M_LEAVE, L.net + NI_SESSION, 16);
    if (L.state == LS_STA || L.state == LS_STA_CONNECTED) set_state(LS_INIT);
    ReleaseSRWLockExclusive(&L.lock);
    RET(0);
}
static void ldn_OpenAccessPoint(Ctx *c) {
    AcquireSRWLockExclusive(&L.lock);
    int ok = L.state == LS_INIT || L.state == LS_AP;
    if (ok) set_state(LS_AP);
    ReleaseSRWLockExclusive(&L.lock);
    RET(ok ? 0 : R_LDN_STATE);
}
static void ldn_CloseAccessPoint(Ctx *c) {
    AcquireSRWLockExclusive(&L.lock);
    if (L.state == LS_AP_CREATED) ldn_msg(VNET_BROADCAST, M_GONE, L.net + NI_SESSION, 16);
    if (L.state == LS_AP || L.state == LS_AP_CREATED) set_state(LS_INIT);
    ReleaseSRWLockExclusive(&L.lock);
    RET(0);
}

/* NetworkConfig: +0 intent (local communication id u64, scene id u16 @0xa), +0x10 channel s16, +0x12 node count max,
 * +0x14 local communication version s16. SecurityConfig: +0 security mode u16. SecurityParameter: data[16], session[16].
 * UserConfig: user name[33]. */
static void create_network(Ctx *c, uint64_t config, uint64_t security, uint64_t secparam, uint64_t user) {
    AcquireSRWLockExclusive(&L.lock);
    uint32_t me = net_my_ip();
    if (L.state != LS_AP || !me) { ReleaseSRWLockExclusive(&L.lock); RET(R_LDN_STATE); return; }
    uint8_t *n = L.net;
    memset(n, 0, NI_SIZE);
    memcpy(n + NI_LCID, PTR(config), 8);
    memcpy(n + NI_SCENE, PTR(config + 0xa), 2);
    if (secparam) { memcpy(n + NI_SECPARAM, PTR(secparam), 16); memcpy(n + NI_SESSION, PTR(secparam + 16), 16); }
    else {                                /* (no parameter given: a session id of our own, from the performance counter) */
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        uint64_t x = (uint64_t)q.QuadPart ^ ((uint64_t)me << 32) ^ GetCurrentProcessId();
        for (int i = 0; i < 32; i++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; n[(i < 16 ? NI_SECPARAM : NI_SESSION - 16) + i] = (uint8_t)x; }
    }
    mac_of(n + NI_BSSID, me);
    static const char hex[] = "0123456789abcdef";
    n[NI_SSID_LEN] = 32;
    for (int i = 0; i < 32; i++) n[NI_SSID + i] = (uint8_t)hex[n[NI_SESSION + i / 2] >> (i & 1 ? 0 : 4) & 15];
    int16_t ch = (int16_t)ld16(config + 0x10);
    uint16_t chu = (uint16_t)(ch > 0 ? ch : 6);
    memcpy(n + NI_CHANNEL, &chu, 2);
    n[NI_LINK] = 3;
    n[NI_TYPE] = 2;
    memcpy(n + NI_SECMODE, PTR(security), 2);
    uint8_t mx = PTR(config)[0x12];
    n[NI_NODE_MAX] = mx && mx <= 8 ? mx : 8;
    n[NI_NODE_COUNT] = 1;
    uint8_t *h = n + NODE(0);
    put32(h, me);
    mac_of(h + 4, me);
    h[0xb] = 1;
    memcpy(h + 0xc, PTR(user), 32);
    memcpy(h + 0x2e, PTR(config + 0x14), 2);
    set_state(LS_AP_CREATED);
    advertise();
    if (LOGL(1)) printf("[online] ldn: room created by %.32s (%u players at most, scene %04x)\n", (const char *)h + 0xc, n[NI_NODE_MAX], ld16(config + 0xa));
    ReleaseSRWLockExclusive(&L.lock);
    input_mark("ldn:created");
    RET(0);
}
static void ldn_CreateNetwork(Ctx *c) { create_network(c, c->x[0], c->x[1], 0, c->x[2]); }
static void ldn_CreateNetworkPrivate(Ctx *c) { create_network(c, c->x[0], c->x[1], c->x[2], c->x[3]); }
static void ldn_DestroyNetwork(Ctx *c) {
    AcquireSRWLockExclusive(&L.lock);
    if (L.state == LS_AP_CREATED) { ldn_msg(VNET_BROADCAST, M_GONE, L.net + NI_SESSION, 16); set_state(LS_AP); }
    ReleaseSRWLockExclusive(&L.lock);
    if (LOGL(1)) printf("[online] ldn: DestroyNetwork\n");
    RET(0);
}

/* ScanFilter: +0 intent (lcid u64, scene @0xa), +0x10 session id[16], +0x20 network type, +0x24 BSSID, +0x2a SSID
 * length, +0x2b SSID, +0x5c flags (1 lcid, 2 session id, 4 network type, 0x10 SSID, 0x20 scene id). */
static void scan(Ctx *c, uint64_t out, uint64_t count_p, int32_t room, uint64_t filter) {
    uint32_t flags = ld32(filter + 0x5c);
    static uint64_t scans;
    AcquireSRWLockExclusive(&L.lock);
    uint32_t seq = ++L.list_asked;
    ReleaseSRWLockExclusive(&L.lock);
    net_list_rooms(seq);
    uint64_t until = ms_now() + 600, now;
    AcquireSRWLockExclusive(&L.lock);
    while (L.list_done != seq && (now = ms_now()) < until) SleepConditionVariableSRW(&L.cv, &L.lock, (DWORD)(until - now), 0);
    int answered = L.list_done == seq;
    ReleaseSRWLockExclusive(&L.lock);
    uint64_t t = ms_now();
    int32_t found = 0;
    uint32_t first = net_match_host();    /* O2: matched to join a room: that host's room comes first in the game's list */
    int first_at = -1;
    AcquireSRWLockShared(&L.lock);
    for (int i = 0; first && i < SEEN_MAX && first_at < 0; i++)
        if (L.seen[i].at && t - L.seen[i].at <= 4000 && L.seen[i].from == first && u32at(L.seen[i].info + NODE(0)) == first) first_at = i;
    for (int k = first_at >= 0 ? -1 : 0; k < SEEN_MAX && found < room; k++) {
        int i = k < 0 ? first_at : k;
        if (k >= 0 && i == first_at) continue;
        const uint8_t *n = L.seen[i].info;
        if (!L.seen[i].at || t - L.seen[i].at > 4000) continue;
        if (L.state == LS_AP_CREATED && !memcmp(n + NI_SESSION, L.net + NI_SESSION, 16)) continue;
        if ((flags & 1) && memcmp(n + NI_LCID, PTR(filter), 8)) continue;
        if ((flags & 2) && memcmp(n + NI_SESSION, PTR(filter + 0x10), 16)) continue;
        if ((flags & 0x10) && (n[NI_SSID_LEN] != PTR(filter)[0x2a] || memcmp(n + NI_SSID, PTR(filter + 0x2b), n[NI_SSID_LEN]))) continue;
        if ((flags & 0x20) && memcmp(n + NI_SCENE, PTR(filter + 0xa), 2)) continue;
        memcpy(PTR(out + (uint64_t)NI_SIZE * found), n, NI_SIZE);
        found++;
    }
    ReleaseSRWLockShared(&L.lock);
    if (count_p) st32(count_p, (uint32_t)found);
    scans++;
    if (LOGL(2) || (LOGL(1) && (found || scans % 20 == 1)))
        printf("[online] ldn: scan %llu (filter %#x, room for %d): %d room(s)%s%s\n", (unsigned long long)scans, flags, room, found, answered ? "" : " (no list from the lobby in time)",
               first_at >= 0 ? ", the matched host's room first" : "");
    if (found) input_mark("ldn:scan-found");
    input_mark("ldn:scanned");                      /* (R3 lwr, test aid) a scan was made (SSBU_SEQ @ldn:scanned) */
    RET(0);
}
static void ldn_Scan(Ctx *c) { scan(c, c->x[0], c->x[1], (int32_t)c->x[2], c->x[3]); }

static void join(Ctx *c, const uint8_t *session, uint64_t user, uint16_t version) {
    AcquireSRWLockExclusive(&L.lock);
    uint32_t host = 0;
    for (int i = 0; i < SEEN_MAX && !host; i++)
        if (L.seen[i].at && !memcmp(L.seen[i].info + NI_SESSION, session, 16)) host = u32at(L.seen[i].info + NODE(0));
    char why[256];
    if (host && net_room_refusal(host, why, sizeof why)) {     /* O1: a room of another version or mod set: listed, never joined */
        ReleaseSRWLockExclusive(&L.lock);
        if (LOGL(1)) printf("[online] ldn: cannot join (another build: %s)\n", why);
        RET(R_LDN_NONET);
        return;
    }
    if (L.state != LS_STA || !host) {
        ReleaseSRWLockExclusive(&L.lock);
        if (LOGL(1)) printf("[online] ldn: cannot join (%s)\n", host ? "not a station" : "room not seen in a scan");
        RET(host ? R_LDN_STATE : R_LDN_NONET);
        return;
    }
    uint8_t ask[52] = {0};
    memcpy(ask, session, 16);
    memcpy(ask + 16, PTR(user), 32);
    memcpy(ask + 48, &version, 2);
    L.answer = 0;
    L.join_host = host;
    memcpy(L.join_session, session, 16);
    for (int tries = 0; tries < 5 && !L.answer; tries++) {
        ldn_msg(host, M_JOIN, ask, sizeof ask);
        uint64_t until = ms_now() + 1000, now;
        while (!L.answer && (now = ms_now()) < until) SleepConditionVariableSRW(&L.cv, &L.lock, (DWORD)(until - now), 0);
    }
    int ok = L.answer == 1;
    if (ok) { L.host_heard = ms_now(); L.reason = DR_NONE; set_state(LS_STA_CONNECTED); }
    if (LOGL(1)) printf("[online] ldn: joining %.32s's room: %s\n", (const char *)L.net + NODE(0) + 0xc, ok ? "joined" : L.answer ? "refused" : "no answer");
    L.answer = 0;
    L.join_host = 0;
    ReleaseSRWLockExclusive(&L.lock);
    if (ok) input_mark("ldn:connected");
    RET(ok ? 0 : R_LDN_NONET);
}
/* Connect(const NetworkInfo &, const SecurityConfig &, const UserConfig &, int version, ConnectOption) */
static void ldn_Connect(Ctx *c) { join(c, PTR(c->x[0] + NI_SESSION), c->x[2], (uint16_t)c->x[3]); }
/* ConnectPrivate(const NetworkConfig &, const SecurityConfig &, const SecurityParameter &, const UserConfig &, int, ConnectOption) */
static void ldn_ConnectPrivate(Ctx *c) { join(c, PTR(c->x[2] + 16), c->x[3], (uint16_t)c->x[4]); }
static void ldn_Disconnect(Ctx *c) {
    AcquireSRWLockExclusive(&L.lock);
    if (L.state == LS_STA_CONNECTED) {
        ldn_msg(u32at(L.net + NODE(0)), M_LEAVE, L.net + NI_SESSION, 16);
        L.reason = DR_USER;
        set_state(LS_STA);
    }
    ReleaseSRWLockExclusive(&L.lock);
    RET(0);
}
static int in_network(void) { return L.state == LS_AP_CREATED || L.state == LS_STA_CONNECTED; }
int nnldn_players(void) {                 /* players in this player's room, itself included; 0 = not in a room. No lock: one byte */
    int s = L.state;
    return s == LS_AP_CREATED || s == LS_STA_CONNECTED ? L.net[NI_NODE_COUNT] : 0;
}
/* (R3 pub1, testers 2026-10-20) the address of THE other player of this room (a 1v1 room: exactly one other node), 0 = none or
 * more than one: net_relay.c sends the rollback messages to it */
uint32_t nnldn_other_ip(void) {
    uint32_t me = net_my_ip(), r = 0; int n = 0;
    AcquireSRWLockShared(&L.lock);
    if (in_network()) for (int i = 0; i < 8; i++) if (L.net[NODE(i) + 0xb]) { uint32_t ip = u32at(L.net + NODE(i)); if (ip && ip != me) { r = ip; n++; } }
    ReleaseSRWLockShared(&L.lock);
    return n == 1 ? r : 0;
}
int nnldn_role(void) { int s = L.state; return s == LS_AP_CREATED ? 1 : s == LS_STA_CONNECTED ? 2 : 0; }      /* (R3 pub1) 1 = this game made the room, 2 = it joined one, 0 = no room */
int nnldn_on(void) { return L.state != LS_NONE; }      /* the game is in Local Wireless (Initialize called, no Finalize yet), in a room or not */
int nnldn_has_node(uint32_t ip) {
    AcquireSRWLockShared(&L.lock);
    int in = in_network() && node_index(ip) >= 0;
    ReleaseSRWLockShared(&L.lock);
    return in;
}
static void ldn_GetNetworkInfo(Ctx *c) {
    AcquireSRWLockShared(&L.lock);
    int ok = in_network();
    if (ok) memcpy(PTR(c->x[0]), L.net, NI_SIZE);
    ReleaseSRWLockShared(&L.lock);
    RET(ok ? 0 : R_LDN_STATE);
}
static void ldn_GetNetworkConfig(Ctx *c) {
    AcquireSRWLockShared(&L.lock);
    int ok = in_network();
    if (ok) {
        uint8_t *o = PTR(c->x[0]);
        memset(o, 0, 0x20);
        memcpy(o, L.net + NI_LCID, 8);
        memcpy(o + 0xa, L.net + NI_SCENE, 2);
        memcpy(o + 0x10, L.net + NI_CHANNEL, 2);
        o[0x12] = L.net[NI_NODE_MAX];
        memcpy(o + 0x14, L.net + NODE(0) + 0x2e, 2);
    }
    ReleaseSRWLockShared(&L.lock);
    RET(ok ? 0 : R_LDN_STATE);
}
static void ldn_GetIpv4Address(Ctx *c) {  /* (Ipv4Address *, SubnetMask *): raw host-order words */
    uint32_t me = net_my_ip();
    if (!me) { RET(R_LDN_STATE); return; }
    if (c->x[0]) st32(c->x[0], me);
    if (c->x[1]) st32(c->x[1], VNET_MASK);
    RET(0);
}
static void ldn_GetSecurityParameter(Ctx *c) {
    AcquireSRWLockShared(&L.lock);
    int ok = in_network();
    if (ok) { memcpy(PTR(c->x[0]), L.net + NI_SECPARAM, 16); memcpy(PTR(c->x[0] + 16), L.net + NI_SESSION, 16); }
    ReleaseSRWLockShared(&L.lock);
    RET(ok ? 0 : R_LDN_STATE);
}
static void ldn_GetDisconnectReason(Ctx *c) { RET((uint32_t)L.reason); }
static void ldn_SetAdvertiseData(Ctx *c) {
    uint64_t size = c->x[1] < 0x180 ? c->x[1] : 0x180;
    AcquireSRWLockExclusive(&L.lock);
    uint16_t s = (uint16_t)size;
    memcpy(L.net + NI_ADV_SIZE, &s, 2);
    memset(L.net + NI_ADV, 0, 0x180);
    if (size) memcpy(L.net + NI_ADV, PTR(c->x[0]), size);
    if (L.state == LS_AP_CREATED) advertise();
    ReleaseSRWLockExclusive(&L.lock);
    if (LOGL(2)) printf("[online] ldn: advertise data %llu bytes\n", (unsigned long long)size);
    RET(0);
}
static void ldn_SetStationAcceptPolicy(Ctx *c) {
    AcquireSRWLockExclusive(&L.lock);
    L.net[NI_POLICY] = (uint8_t)c->x[0];
    if (L.state == LS_AP_CREATED) advertise();
    ReleaseSRWLockExclusive(&L.lock);
    RET(0);
}
static void ldn_Reject(Ctx *c) {
    uint32_t ip = (uint32_t)c->x[0];
    AcquireSRWLockExclusive(&L.lock);
    int node = L.state == LS_AP_CREATED ? node_index(ip) : -1;
    if (node > 0) { ldn_msg(ip, M_KICK, L.net + NI_SESSION, 16); remove_node(node); }
    ReleaseSRWLockExclusive(&L.lock);
    RET(0);
}
static void ldn_SetWirelessControllerRestriction(Ctx *c) { RET(0); }

/* ================================================================== nn::socket on the virtual network */
#define LOOPBACK 0x7f000001u
#define ALL_ONES 0xffffffffu
enum { E_BADF = 9, E_AGAIN = 11, E_INVAL = 22, E_MFILE = 24, E_DESTADDRREQ = 89, E_MSGSIZE = 90, E_NOPROTOOPT = 92,
       E_PROTONOSUPPORT = 93, E_OPNOTSUPP = 95, E_AFNOSUPPORT = 97, E_ADDRINUSE = 98, E_ADDRNOTAVAIL = 99,
       E_NETUNREACH = 101, E_NOTCONN = 107, E_CONNREFUSED = 111 };
typedef struct Dgram { struct Dgram *next; uint32_t src, len; uint16_t sport; uint8_t data[]; } Dgram;
typedef struct {
    int open, type, flags, nonblock, reuse, shut_rd, shut_wr;
    uint32_t ip, peer_ip, rcvtimeo, rcvbuf, sndbuf;
    uint16_t port, peer_port;
    Dgram *head, *tail;
    uint32_t count, bytes;
} VSock;
#define VS_MAX 128
static VSock vs[VS_MAX];
static SRWLOCK vs_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE vs_cv = CONDITION_VARIABLE_INIT;
static uint16_t vs_next_port = 49152;
static volatile LONG64 vs_in, vs_out;

static VSock *vsock(int fd) { return fd >= 0 && fd < VS_MAX && vs[fd].open ? &vs[fd] : NULL; }
static int *errno_ptr(Ctx *c) {
    uint64_t keep = c->x[0];
    ImportFn f = import_lookup("__errno_location");
    if (!f) return NULL;
    c->x[0] = 0;
    f(c);
    int *p = (int *)(uintptr_t)c->x[0];
    c->x[0] = keep;
    return p;
}
static void fail(Ctx *c, int e) { int *p = errno_ptr(c); if (p) *p = e; RETI(-1); }

static int read_sa(uint64_t a, uint32_t len, uint32_t *ip, uint16_t *port) {
    if (!a || len < 8 || PTR(a)[1] != 2) return 0;
    uint16_t p; uint32_t i;
    memcpy(&p, PTR(a + 2), 2); memcpy(&i, PTR(a + 4), 4);
    *port = _byteswap_ushort(p);
    *ip = _byteswap_ulong(i);
    return 1;
}
static void write_sa(uint64_t a, uint64_t lenp, uint32_t ip, uint16_t port) {
    if (!a) return;
    uint8_t b[16] = {16, 2};
    uint16_t p = _byteswap_ushort(port);
    uint32_t i = _byteswap_ulong(ip);
    memcpy(b + 2, &p, 2);
    memcpy(b + 4, &i, 4);
    uint32_t cap = lenp ? ld32(lenp) : 16;
    memcpy(PTR(a), b, cap < 16 ? cap : 16);
    if (lenp) st32(lenp, 16);
}
static int port_used(const VSock *self, uint16_t port) {
    for (int i = 0; i < VS_MAX; i++)
        if (&vs[i] != self && vs[i].open && vs[i].type == self->type && vs[i].port == port && !(vs[i].reuse && self->reuse)) return 1;
    return 0;
}
static uint16_t free_port(const VSock *self) {
    for (int tries = 0; tries < 16384; tries++) {
        uint16_t p = vs_next_port++;
        if (vs_next_port < 49152) vs_next_port = 49152;
        if (!port_used(self, p)) return p;
    }
    return 0;
}
static void drop_queue(VSock *s) {
    while (s->head) { Dgram *d = s->head; s->head = d->next; free(d); }
    s->tail = NULL;
    s->count = s->bytes = 0;
}

void nnldn_deliver(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const void *data, uint32_t len) {
    if (dport == VNET_LDN_PORT) { if (sport == VNET_LDN_PORT) ldn_receive(src, data, len); return; }
    int bcast = dst == ALL_ONES || dst == VNET_BROADCAST, got = 0;
    AcquireSRWLockExclusive(&vs_lock);
    for (int i = 0; i < VS_MAX; i++) {
        VSock *s = &vs[i];
        if (!s->open || s->type != 2 || s->port != dport || s->shut_rd) continue;
        if (!bcast && s->ip && s->ip != dst && s->ip != LOOPBACK) continue;
        if (s->peer_port && (s->peer_ip != src || s->peer_port != sport)) continue;
        if (s->count >= 1024 || s->bytes + len > (1u << 20)) continue;          /* full: dropped, as a LAN would */
        Dgram *d = malloc(sizeof *d + len);
        if (!d) continue;
        d->next = NULL; d->src = src; d->sport = sport; d->len = len;
        memcpy(d->data, data, len);
        if (s->tail) s->tail->next = d; else s->head = d;
        s->tail = d;
        s->count++;
        s->bytes += len;
        got = 1;
        if (!bcast && !s->reuse) break;
    }
    ReleaseSRWLockExclusive(&vs_lock);
    if (got) { InterlockedIncrement64(&vs_in); WakeAllConditionVariable(&vs_cv); }
    if (LOGL(3)) { char a[16], b[16]; ip_str(src, a); ip_str(dst, b); printf("[online] in %u bytes %s:%u -> %s:%u%s\n", len, a, sport, b, dport, got ? "" : " (no socket)"); }
}

static void so_socket(Ctx *c) {
    int domain = (int)(c->x[0] & 0xff), type = (int)c->x[1], base = type & 0xffff;
    if (domain != 2) { fail(c, E_AFNOSUPPORT); return; }
    if (base != 1 && base != 2) { fail(c, E_PROTONOSUPPORT); return; }
    int fd = -1;
    AcquireSRWLockExclusive(&vs_lock);
    for (int i = 0; i < VS_MAX && fd < 0; i++)
        if (!vs[i].open) {
            memset(&vs[i], 0, sizeof vs[i]);
            vs[i].open = 1;
            vs[i].type = base;
            vs[i].nonblock = (type & 0x20000000) != 0;
            vs[i].rcvbuf = vs[i].sndbuf = 0x10000;
            fd = i;
        }
    ReleaseSRWLockExclusive(&vs_lock);
    if (LOGL(2)) printf("[online] socket(%d, %#x) = %d\n", domain, type, fd);
    if (fd < 0) fail(c, E_MFILE); else RETI(fd);
}
static void so_bind(Ctx *c) {
    int fd = (int)c->x[0], err;
    uint32_t ip = 0;
    uint16_t port = 0;
    AcquireSRWLockExclusive(&vs_lock);
    VSock *s = vsock(fd);
    err = !s ? E_BADF : !read_sa(c->x[1], (uint32_t)c->x[2], &ip, &port) ? E_INVAL : 0;
    if (!err && ip && ip != LOOPBACK && ip != net_my_ip() && ip != ALL_ONES && ip != VNET_BROADCAST) err = E_ADDRNOTAVAIL;
    if (!err && port && port_used(s, port)) err = E_ADDRINUSE;
    if (!err && !port && !(port = free_port(s))) err = E_ADDRINUSE;
    if (!err) { s->ip = ip; s->port = port; }
    ReleaseSRWLockExclusive(&vs_lock);
    if (LOGL(1)) { char a[16]; ip_str(ip, a); printf("[online] bind(%d, %s:%u) = %d\n", fd, a, port, err ? -err : 0); }
    if (err) fail(c, err); else RET(0);
}
static void so_connect(Ctx *c) {
    int fd = (int)c->x[0], err;
    uint32_t ip = 0;
    uint16_t port = 0;
    AcquireSRWLockExclusive(&vs_lock);
    VSock *s = vsock(fd);
    err = !s ? E_BADF : !read_sa(c->x[1], (uint32_t)c->x[2], &ip, &port) ? E_INVAL : s->type == 1 ? E_CONNREFUSED : 0;
    if (!err) { if (!s->port) s->port = free_port(s); s->peer_ip = ip; s->peer_port = port; }
    ReleaseSRWLockExclusive(&vs_lock);
    if (LOGL(1)) { char a[16]; ip_str(ip, a); printf("[online] connect(%d, %s:%u) = %d\n", fd, a, port, err ? -err : 0); }
    if (err) fail(c, err); else RET(0);
}
static void do_send(Ctx *c, int fd, uint64_t buf, uint64_t len, uint64_t addr, uint32_t alen) {
    uint32_t dst = 0;
    uint16_t dport = 0, sport = 0;
    int err = 0;
    if (!len) { RET(0); return; }
    if (!buf) { fail(c, E_INVAL); return; }
    AcquireSRWLockExclusive(&vs_lock);
    VSock *s = vsock(fd);
    if (!s) err = E_BADF;
    else if (s->type != 2 || s->shut_wr) err = E_NOTCONN;
    else if (addr && alen) { if (!read_sa(addr, alen, &dst, &dport)) err = E_INVAL; }
    else if (s->peer_port) { dst = s->peer_ip; dport = s->peer_port; }
    else err = E_DESTADDRREQ;
    if (!err && len > 65507) err = E_MSGSIZE;
    if (!err && !s->port && !(s->port = free_port(s))) err = E_ADDRINUSE;
    if (!err) sport = s->port;
    ReleaseSRWLockExclusive(&vs_lock);
    uint32_t me = net_my_ip();
    int bcast = dst == ALL_ONES || dst == VNET_BROADCAST;
    if (!err && !bcast && dst != LOOPBACK && dst != me && ((dst & VNET_MASK) != VNET_SUBNET || !me)) err = E_NETUNREACH;
    if (LOGL(3) || (LOGL(1) && err)) { char a[16]; ip_str(dst, a); printf("[online] sendto(%d, %llu bytes, %s:%u) from port %u = %d\n", fd, (unsigned long long)len, a, dport, sport, err ? -err : 0); }
    if (err) { fail(c, err); return; }
    const void *data = PTR(buf);
    if (dst == LOOPBACK) nnldn_deliver(LOOPBACK, sport, LOOPBACK, dport, data, (uint32_t)len);
    else {
        if (dst == me || bcast) nnldn_deliver(me, sport, dst, dport, data, (uint32_t)len);   /* a broadcast reaches this player too */
        if (dst != me) { net_send(me, sport, bcast ? VNET_BROADCAST : dst, dport, data, (uint32_t)len); InterlockedIncrement64(&vs_out); }
    }
    RET(len);
}
static void so_send(Ctx *c) { do_send(c, (int)c->x[0], c->x[1], c->x[2], 0, 0); }
static void so_sendto(Ctx *c) { do_send(c, (int)c->x[0], c->x[1], c->x[2], c->x[4], (uint32_t)c->x[5]); }

static int wait_vs(uint64_t deadline) {   /* (vs_lock held) 0 once the deadline has passed */
    DWORD ms = INFINITE;
    if (deadline != ~0ull) { uint64_t now = ms_now(); if (now >= deadline) return 0; ms = (DWORD)(deadline - now); }
    SleepConditionVariableSRW(&vs_cv, &vs_lock, ms, 0);
    return 1;
}
static void do_recv(Ctx *c, int fd, uint64_t buf, uint64_t len, int flags, uint64_t addr, uint64_t alenp) {
    if (!len) { RET(0); return; }
    if (!buf) { fail(c, E_INVAL); return; }
    AcquireSRWLockExclusive(&vs_lock);
    VSock *s = vsock(fd);
    uint64_t deadline = s && s->rcvtimeo ? ms_now() + s->rcvtimeo : ~0ull;
    int err = 0;
    for (;;) {
        s = vsock(fd);
        if (!s) { err = E_BADF; break; }
        if (s->head) break;
        if (s->shut_rd) { ReleaseSRWLockExclusive(&vs_lock); RET(0); return; }
        if (s->nonblock || (flags & 0x80)) { err = E_AGAIN; break; }
        if (!wait_vs(deadline)) { err = E_AGAIN; break; }
    }
    if (err) { ReleaseSRWLockExclusive(&vs_lock); fail(c, err); return; }
    Dgram *d = s->head;
    uint32_t n = d->len < len ? d->len : (uint32_t)len, ip = d->src;
    uint16_t port = d->sport;
    memcpy(PTR(buf), d->data, n);
    if (!(flags & 2)) {
        s->head = d->next;
        if (!s->head) s->tail = NULL;
        s->count--;
        s->bytes -= d->len;
        free(d);
    }
    ReleaseSRWLockExclusive(&vs_lock);
    write_sa(addr, alenp, ip, port);
    RET(n);
}
static void so_recv(Ctx *c) { do_recv(c, (int)c->x[0], c->x[1], c->x[2], (int)c->x[3], 0, 0); }
static void so_recvfrom(Ctx *c) { do_recv(c, (int)c->x[0], c->x[1], c->x[2], (int)c->x[3], c->x[4], c->x[5]); }

static void so_poll(Ctx *c) {             /* (pollfd {int fd; short events, revents} *, nfds, timeout ms) */
    uint64_t fds = c->x[0], nfds = c->x[1];
    int timeout = (int)c->x[2];
    if (!fds && nfds) { fail(c, E_INVAL); return; }
    uint64_t deadline = timeout < 0 ? ~0ull : ms_now() + (uint64_t)timeout;
    uint32_t ready;
    AcquireSRWLockExclusive(&vs_lock);
    for (;;) {
        ready = 0;
        for (uint64_t i = 0; i < nfds; i++) {
            uint64_t p = fds + 8 * i;
            int fd = (int)ld32(p);
            uint16_t ev = ld16(p + 4), re = 0;
            if (fd < 0) { st16(p + 6, 0); continue; }
            VSock *s = vsock(fd);
            if (!s) re = 0x20;
            else {
                if (s->head) re |= ev & (0x1 | 0x40);
                if (s->type == 2 && !s->shut_wr) re |= ev & 0x4;
                if (s->shut_rd && s->type == 1) re |= 0x10;
            }
            st16(p + 6, re);
            ready += re != 0;
        }
        if (ready || timeout == 0 || !wait_vs(deadline)) break;
    }
    ReleaseSRWLockExclusive(&vs_lock);
    RET(ready);
}
static void so_fcntl(Ctx *c) {            /* (fd, cmd, int): 3 F_GETFL, 4 F_SETFL */
    int fd = (int)c->x[0], cmd = (int)c->x[1], arg = (int)c->x[2], r = 0, bad = 0;
    AcquireSRWLockExclusive(&vs_lock);
    VSock *s = vsock(fd);
    if (s && cmd == 3) r = s->flags | (s->nonblock ? 0x800 : 0);
    else if (s && cmd == 4) { s->flags = arg; s->nonblock = (arg & 0x800) != 0; }
    else if (s) bad = 1;
    ReleaseSRWLockExclusive(&vs_lock);
    if (!s) fail(c, E_BADF); else if (bad) fail(c, E_INVAL); else RETI(r);
}
static void so_setsockopt(Ctx *c) {       /* (fd, level, option, const void *, len) */
    int fd = (int)c->x[0], level = (int)c->x[1], opt = (int)c->x[2];
    uint64_t val = c->x[3];
    uint32_t len = (uint32_t)c->x[4], v = val && len >= 4 ? ld32(val) : 0;
    if (!val) { fail(c, E_INVAL); return; }
    AcquireSRWLockExclusive(&vs_lock);
    VSock *s = vsock(fd);
    if (s && level == 0xffff) {
        if (opt == 0x4 || opt == 0x200) s->reuse = v != 0;
        else if (opt == 0x1001) s->sndbuf = v;
        else if (opt == 0x1002) s->rcvbuf = v;
        else if (opt == 0x1006 && len >= 16) {             /* SO_RCVTIMEO: timeval {s64 sec, s64 usec} */
            int64_t sec, usec;
            memcpy(&sec, PTR(val), 8); memcpy(&usec, PTR(val + 8), 8);
            s->rcvtimeo = (uint32_t)(sec * 1000 + usec / 1000);
            if (!s->rcvtimeo && (sec || usec)) s->rcvtimeo = 1;
        }
    }
    ReleaseSRWLockExclusive(&vs_lock);
    if (LOGL(2)) printf("[online] setsockopt(%d, %#x, %#x, %u)\n", fd, level, opt, v);
    if (!s) fail(c, E_BADF); else RET(0);
}
static void so_getsockopt(Ctx *c) {       /* (fd, level, option, void *, unsigned *len) */
    int fd = (int)c->x[0], level = (int)c->x[1], opt = (int)c->x[2], err;
    uint64_t val = c->x[3], lenp = c->x[4];
    uint32_t v = 0;
    if (!val || !lenp || !ld32(lenp)) { fail(c, E_INVAL); return; }
    AcquireSRWLockShared(&vs_lock);
    VSock *s = vsock(fd);
    err = !s ? E_BADF : 0;
    if (s && level == 0xffff) {
        if (opt == 0x1008) v = (uint32_t)s->type;
        else if (opt == 0x1001) v = s->sndbuf;
        else if (opt == 0x1002) v = s->rcvbuf;
        else if (opt == 0x4 || opt == 0x200) v = (uint32_t)s->reuse;
    } else if (s && level != 0 && level != 6) err = E_NOPROTOOPT;
    ReleaseSRWLockShared(&vs_lock);
    if (err) { fail(c, err); return; }
    if (ld32(lenp) >= 4) { st32(val, v); st32(lenp, 4); }
    RET(0);
}
static void do_name(Ctx *c, int peer) {
    int fd = (int)c->x[0];
    if (!c->x[1] || !c->x[2] || !ld32(c->x[2])) { fail(c, E_INVAL); return; }
    AcquireSRWLockShared(&vs_lock);
    VSock *s = vsock(fd);
    uint32_t ip = s ? (peer ? s->peer_ip : s->ip) : 0;
    uint16_t port = s ? (peer ? s->peer_port : s->port) : 0;
    ReleaseSRWLockShared(&vs_lock);
    if (!s) { fail(c, E_BADF); return; }
    if (peer && !port) { fail(c, E_NOTCONN); return; }
    write_sa(c->x[1], c->x[2], ip, port);
    RET(0);
}
static void so_getsockname(Ctx *c) { do_name(c, 0); }
static void so_getpeername(Ctx *c) { do_name(c, 1); }
static void so_shutdown(Ctx *c) {
    int fd = (int)c->x[0], how = (int)c->x[1];
    AcquireSRWLockExclusive(&vs_lock);
    VSock *s = vsock(fd);
    if (s) { if (how != 1) s->shut_rd = 1; if (how != 0) s->shut_wr = 1; }
    ReleaseSRWLockExclusive(&vs_lock);
    WakeAllConditionVariable(&vs_cv);
    if (!s) fail(c, E_BADF); else RET(0);
}
static void so_close(Ctx *c) {
    int fd = (int)c->x[0];
    AcquireSRWLockExclusive(&vs_lock);
    VSock *s = vsock(fd);
    if (s) { drop_queue(s); memset(s, 0, sizeof *s); }
    ReleaseSRWLockExclusive(&vs_lock);
    WakeAllConditionVariable(&vs_cv);
    if (LOGL(2)) printf("[online] close(%d)\n", fd);
    if (!s) fail(c, E_BADF); else RET(0);
}
static void so_listen(Ctx *c) {
    VSock *s = vsock((int)c->x[0]);
    if (!s) fail(c, E_BADF); else if (s->type != 1) fail(c, E_OPNOTSUPP); else RET(0);
}
static void so_accept(Ctx *c) {           /* no stream ever connects on the virtual network: wait until the socket goes */
    int fd = (int)c->x[0], err;
    AcquireSRWLockExclusive(&vs_lock);
    VSock *s = vsock(fd);
    err = !s ? E_BADF : s->nonblock ? E_AGAIN : 0;
    while (!err) { wait_vs(~0ull); if (!(s = vsock(fd)) || s->shut_rd) err = E_BADF; }
    ReleaseSRWLockExclusive(&vs_lock);
    fail(c, err);
}

/* ================================================================== registration */
#define SOK(n) "_ZN2nn6socket" n
#define LDNK(n) "_ZN2nn3ldn" n
void nnldn_register(void) {
    if (!net_online()) return;
    static const ImportDef defs[] = {
        {LDNK("10InitializeEv"), ldn_Initialize}, {LDNK("8FinalizeEv"), ldn_Finalize}, {LDNK("8GetStateEv"), ldn_GetState},
        {LDNK("22AttachStateChangeEventEPNS_2os15SystemEventTypeE"), ldn_AttachStateChangeEvent},
        {LDNK("32SetWirelessControllerRestrictionENS0_29WirelessControllerRestrictionE"), ldn_SetWirelessControllerRestriction},
        {LDNK("11OpenStationEv"), ldn_OpenStation}, {LDNK("12CloseStationEv"), ldn_CloseStation},
        {LDNK("15OpenAccessPointEv"), ldn_OpenAccessPoint}, {LDNK("16CloseAccessPointEv"), ldn_CloseAccessPoint},
        {LDNK("10DisconnectEv"), ldn_Disconnect}, {LDNK("14DestroyNetworkEv"), ldn_DestroyNetwork},
        {LDNK("14GetNetworkInfoEPNS0_11NetworkInfoE"), ldn_GetNetworkInfo}, {LDNK("16GetNetworkConfigEPNS0_13NetworkConfigE"), ldn_GetNetworkConfig},
        {LDNK("14GetIpv4AddressEPNS0_11Ipv4AddressEPNS0_10SubnetMaskE"), ldn_GetIpv4Address},
        {LDNK("20GetSecurityParameterEPNS0_17SecurityParameterE"), ldn_GetSecurityParameter},
        {LDNK("16SetAdvertiseDataEPKvm"), ldn_SetAdvertiseData}, {LDNK("22SetStationAcceptPolicyENS0_12AcceptPolicyE"), ldn_SetStationAcceptPolicy},
        {LDNK("6RejectENS0_11Ipv4AddressE"), ldn_Reject},
        {LDNK("4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi"), ldn_Scan},
        {LDNK("11ScanPrivateEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi"), ldn_Scan},
        {LDNK("13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE"), ldn_CreateNetwork},
        {LDNK("20CreateNetworkPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiPKNS0_12AddressEntryE"), ldn_CreateNetworkPrivate},
        {LDNK("7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE"), ldn_Connect},
        {LDNK("14ConnectPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiNS0_13ConnectOptionE"), ldn_ConnectPrivate},
        {LDNK("19GetDisconnectReasonEv"), ldn_GetDisconnectReason},
        {SOK("6SocketEiii"), so_socket}, {SOK("6SocketENS0_6FamilyENS0_4TypeENS0_8ProtocolE"), so_socket}, {"nnsocketSocket", so_socket},
        {SOK("5CloseEi"), so_close}, {"nnsocketClose", so_close},
        {SOK("4BindEiPK8sockaddrj"), so_bind}, {SOK("4BindEiPKNS0_8SockAddrEj"), so_bind}, {"nnsocketBind", so_bind},
        {SOK("7ConnectEiPK8sockaddrj"), so_connect}, {"nnsocketConnect", so_connect},
        {SOK("6ListenEii"), so_listen}, {SOK("8ShutdownEii"), so_shutdown}, {SOK("6AcceptEiP8sockaddrPj"), so_accept},
        {SOK("11GetSockNameEiP8sockaddrPj"), so_getsockname}, {"nnsocketGetSockName", so_getsockname}, {"nnsocketGetPeerName", so_getpeername},
        {SOK("4RecvEiPvmi"), so_recv}, {"nnsocketRecv", so_recv},
        {SOK("8RecvFromEiPvmiP8sockaddrPj"), so_recvfrom}, {SOK("8RecvFromEiPvmNS0_7MsgFlagEPNS0_8SockAddrEPj"), so_recvfrom},
        {SOK("4SendEiPKvmi"), so_send}, {"nnsocketSend", so_send},
        {SOK("6SendToEiPKvmiPK8sockaddrj"), so_sendto}, {SOK("6SendToEiPKvmNS0_7MsgFlagEPKNS0_8SockAddrEj"), so_sendto},
        {SOK("5FcntlEiiz"), so_fcntl}, {"nnsocketFcntl", so_fcntl},
        {SOK("4PollEP6pollfdmi"), so_poll}, {"nnsocketPoll", so_poll},
        {SOK("10GetSockOptEiiiPvPj"), so_getsockopt}, {"nnsocketGetSockOpt", so_getsockopt},
        {SOK("10SetSockOptEiiiPKvj"), so_setsockopt}, {SOK("10SetSockOptEiNS0_5LevelENS0_6OptionEPKvj"), so_setsockopt}, {"nnsocketSetSockOpt", so_setsockopt},
    };
    int n = (int)(sizeof defs / sizeof defs[0]), missing = 0;
    for (int i = 0; i < n; i++) if (!import_data(defs[i].name)) { printf("[online] \"%s\" is not an import of the game\n", defs[i].name); missing++; }
    register_imports(defs, n);
    printf("[online] nn::ldn and nn::socket replaced by the virtual network (%d handlers, %d not imported)\n", n, missing);
    fflush(stdout);
    net_start();                          /* join the lobby during boot: the address is there when Local Wireless opens */
}
