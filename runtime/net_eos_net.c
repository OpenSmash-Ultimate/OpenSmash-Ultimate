/* The online network over Epic Online Services alone (plan/EOS_MIGRATION.md, phase 1): no lobby server of ours.
 * online_server.txt "eos [lobby code]" (or SSBU_NET_RELAY=eos) picks it. The game joins its lobby code's hub
 * (net_eos.c: an EOS lobby, its members' virtual addresses as attributes) and every datagram of its virtual
 * network goes to the hub member at that address through EOS P2P - straight where EOS punches through, else
 * through Epic's relays. A broadcast goes to every member. Local Wireless's own messages (net_backend.h's YLDN_*)
 * go the same way, and rollback's (NET_RB_PORT) reach the rollback link (net_rb_deliver).
 *
 * Each EOS packet: 'E' 'B' kind (0 data, 1 Local Wireless) type | source, destination (4 each) | source port,
 * destination port (2 each) | message id (2), piece, pieces | the piece's bytes. A datagram longer than one
 * EOS packet (1170 bytes) goes in pieces, put back together on arrival (a piece missing for a second: dropped,
 * as UDP drops). A packet is taken only from the hub member whose EOS id sent it, with that member's address.
 *
 * SSBU_NET_SIM=<ms>[,<loss %>[,<jitter ms>]] (tests: two games on one PC as over the internet) holds every datagram and
 * Local Wireless message this game sends <ms> (plus up to <jitter> more), and drops that share of them, before EOS
 * gets it: a queue of its own and a thread of its own sending what is due, in order (a slow one holds the rest back,
 * as a queue in a router does). Each game holding what it sends: a round trip of 2 x <ms>. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net.h"
#include "net_backend.h"
#include "net_eos.h"
#include "net_discord.h"

#define HEAD 20
#define PIECE (NET_EOS_MAX - HEAD)
#define BIGGEST (32 * PIECE)   /* (32 pieces: about 36 KB, more than any datagram of the game) */
enum { K_DATA, K_LDN };

#ifdef _WIN32
#include <windows.h>
#pragma comment(lib, "winmm.lib")

static char room_code[64] = "smash", player[33] = "Player";
static u64 started_at;
static volatile LONG msg_ids;
static volatile LONG out_data, out_ldn, in_data, in_ldn, in_rb, not_member, not_for_us, no_member_out, sim_lost, sim_full;
static void counts_line(void) {   /* (every 10 s, with SSBU_NET_LOG) */
    static u64 last;
    u64 t = GetTickCount64();
    if (t - last < 10000 || !getenv("SSBU_NET_LOG")) return;
    last = t;
    fprintf(stderr, "[eos] net: out %ld data + %ld room, in %ld data (%ld rollback) + %ld room; dropped: %ld not a member, %ld not ours, %ld sends to no member; SSBU_NET_SIM lost %ld (%ld queue full)\n",
            (long)out_data, (long)out_ldn, (long)in_data, (long)in_rb, (long)in_ldn, (long)not_member, (long)not_for_us, (long)no_member_out, (long)sim_lost, (long)sim_full);
}

static void put16(u8 *p, u32 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void put32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
static u32 get16(const u8 *p) { return (u32)p[0] << 8 | p[1]; }
static u32 get32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

static int selected(void) { return net_eos_selected(); }
static u32 local_ip(void) { return eos_hub_ip(); }
static const char *player_name(void) { return player; }
static int unreachable(void) { return !eos_hub_ip() && (eos_hub_failed() || GetTickCount64() - started_at > 20000); }
static int has_peer(u32 ip) { char id[NET_EOS_ID_LEN + 1]; return eos_hub_member_id(ip, id); }

static int sim_on(void);
static void start(void) {
    static LONG once;
    if (InterlockedExchange(&once, 1)) return;
    sim_on();
    started_at = GetTickCount64();
    net_eos_names(room_code, sizeof room_code, player, sizeof player);
    char bucket[128];
    snprintf(bucket, sizeof bucket, "ssbu1:%s", room_code);   /* (the build and mods join it in phase 2's gatherings) */
    eos_start();
    eos_hub_join(bucket);
    discord_note_lobby_code(room_code);   /* (in Discord's join secret: a friend's game must have the same) */
    fprintf(stderr, "[eos] online through Epic Online Services alone: lobby code %s, as %s\n", room_code, player);
}

/* ---- SSBU_NET_SIM ---- */
#define SIMQ 8192
static struct { char to[NET_EOS_ID_LEN + 1]; u32 len; u64 due_us; u8 data[NET_EOS_MAX]; } *simq;
static u32 sim_head, sim_tail;
static SRWLOCK sim_lock = SRWLOCK_INIT;
static HANDLE sim_wake;
static double sim_ms, sim_loss, sim_jitter;
static u64 sim_last_due;
static unsigned sim_seed = 0x2545F491u;
static unsigned sim_rand(void) { sim_seed ^= sim_seed << 13; sim_seed ^= sim_seed >> 17; sim_seed ^= sim_seed << 5; return sim_seed; }
static u64 now_us(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (u64)((double)c.QuadPart * 1e6 / (double)f.QuadPart);
}
static DWORD WINAPI sim_thread(LPVOID arg) {   /* (sends what is due, in order) */
    (void)arg;
    timeBeginPeriod(1);
    for (;;) {
        DWORD wait = INFINITE;
        for (;;) {
            AcquireSRWLockExclusive(&sim_lock);
            if (sim_head == sim_tail) { ReleaseSRWLockExclusive(&sim_lock); wait = INFINITE; break; }
            u32 k = sim_head % SIMQ;
            u64 t = now_us();
            if (simq[k].due_us > t) { ReleaseSRWLockExclusive(&sim_lock); wait = (DWORD)((simq[k].due_us - t) / 1000); break; }
            char to[NET_EOS_ID_LEN + 1];
            u8 data[NET_EOS_MAX];
            u32 len = simq[k].len;
            memcpy(to, simq[k].to, sizeof to);
            memcpy(data, simq[k].data, len);
            sim_head++;
            ReleaseSRWLockExclusive(&sim_lock);
            eos_send(to, data, len);
        }
        WaitForSingleObject(sim_wake, wait);
    }
}
static int sim_on(void) {   /* (once: SSBU_NET_SIM read, the thread started) */
    static volatile LONG state;   /* 0 not read, 1 off, 2 on */
    if (state) return state == 2;
    const char *v = getenv("SSBU_NET_SIM");
    double ms = 0, loss = 0, jitter = 0;
    if (v && *v) sscanf(v, "%lf,%lf,%lf", &ms, &loss, &jitter);
    if (ms <= 0 && loss <= 0) { InterlockedExchange(&state, 1); return 0; }
    sim_ms = ms > 0 ? ms : 0;
    sim_loss = loss > 0 ? (loss < 100 ? loss : 100) : 0;
    sim_jitter = jitter > 0 ? jitter : 0;
    simq = calloc(SIMQ, sizeof *simq);
    sim_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    HANDLE t = simq && sim_wake ? CreateThread(NULL, 0, sim_thread, NULL, 0, NULL) : NULL;
    if (!t) { InterlockedExchange(&state, 1); return 0; }
    CloseHandle(t);
    fprintf(stderr, "[eos] net: SSBU_NET_SIM: what this game sends held %.0f ms (up to %.0f more), %.1f%% of it lost\n", sim_ms, sim_jitter, sim_loss);
    InterlockedExchange(&state, 2);
    return 1;
}
static void sim_send(const char *id, const u8 *pkt, u32 len, u64 due) {
    AcquireSRWLockExclusive(&sim_lock);
    if (sim_tail - sim_head >= SIMQ) { ReleaseSRWLockExclusive(&sim_lock); InterlockedIncrement(&sim_full); return; }
    u32 k = sim_tail++ % SIMQ;
    memcpy(simq[k].to, id, NET_EOS_ID_LEN + 1);
    simq[k].len = len;
    simq[k].due_us = due;
    memcpy(simq[k].data, pkt, len);
    ReleaseSRWLockExclusive(&sim_lock);
    SetEvent(sim_wake);
}

/* one datagram (or Local Wireless message) to one member, in pieces when it needs them */
static void send_to(const char *id, int kind, unsigned type, u32 src, u32 dst, u32 sport, u32 dport, const void *data, u32 len) {
    u8 pkt[NET_EOS_MAX];
    u32 pieces = len ? (len + PIECE - 1) / PIECE : 1, msg = (u32)InterlockedIncrement(&msg_ids) & 0xffff;
    if (pieces > 255) return;
    int sim = sim_on();
    u64 due = 0;
    if (sim) {
        AcquireSRWLockExclusive(&sim_lock);
        int lost = sim_loss > 0 && sim_rand() % 10000 < (unsigned)(sim_loss * 100);
        due = now_us() + (u64)((sim_ms + (sim_jitter > 0 ? (double)(sim_rand() % 1000) / 1000.0 * sim_jitter : 0)) * 1000.0);
        if (due < sim_last_due) due = sim_last_due;   /* (in order) */
        if (!lost) sim_last_due = due;
        ReleaseSRWLockExclusive(&sim_lock);
        if (lost) { InterlockedIncrement(&sim_lost); return; }
    }
    for (u32 i = 0; i < pieces; i++) {
        u32 at = i * PIECE, n = len - at < PIECE ? len - at : PIECE;
        pkt[0] = 'E'; pkt[1] = 'B'; pkt[2] = (u8)kind; pkt[3] = (u8)type;
        put32(pkt + 4, src); put32(pkt + 8, dst); put16(pkt + 12, sport); put16(pkt + 14, dport);
        put16(pkt + 16, msg); pkt[18] = (u8)i; pkt[19] = (u8)pieces;
        if (n) memcpy(pkt + HEAD, (const u8 *)data + at, n);
        if (sim) sim_send(id, pkt, HEAD + n, due);
        else eos_send(id, pkt, HEAD + n);
    }
}
static void send_kind(int kind, unsigned type, u32 src, u32 sport, u32 dst, u32 dport, const void *data, u32 len) {
    u32 me = eos_hub_ip();
    if (!me || len > BIGGEST) return;
    if (dst == 0xffffffffu || dst == NET_BROADCAST) {
        char ids[64][NET_EOS_ID_LEN + 1];
        u32 ips[64];
        int n = eos_hub_members(ids, ips, 64);
        const char *mine = eos_my_id();
        for (int i = 0; i < n; i++)
            if (ips[i] && (!mine || strcmp(ids[i], mine))) send_to(ids[i], kind, type, src, dst, sport, dport, data, len);
        return;
    }
    char id[NET_EOS_ID_LEN + 1];
    if (eos_hub_member_id(dst, id)) send_to(id, kind, type, src, dst, sport, dport, data, len);
    else InterlockedIncrement(&no_member_out);
}
static void send_data(u32 src, u16 sport, u32 dst, u16 dport, const void *data, u32 len) {
    if (src != eos_hub_ip()) return;
    InterlockedIncrement(&out_data);
    counts_line();
    send_kind(K_DATA, 0, src, sport, dst, dport, data, len);
}
static void send_ldn(u32 dst, unsigned type, const void *data, u32 len) { InterlockedIncrement(&out_ldn); send_kind(K_LDN, type, eos_hub_ip(), 0, dst, 0, data, len); }

static void status(char *out, size_t cap) {
    u32 ip = eos_hub_ip();
    char ids[64][NET_EOS_ID_LEN + 1];
    u32 ips[64];
    int n = eos_hub_members(ids, ips, 64);
    if (ip) snprintf(out, cap, "Epic Online Services: lobby code %s, %d players - %s (10.13.%u.%u)", room_code, n, player, ip >> 8 & 255, ip & 255);
    else snprintf(out, cap, "Epic Online Services: %s", eos_hub_failed() ? "not available" : "connecting");
}

/* (the EOS thread) pieces put back together: one message at a time per sender is enough for UDP's order */
#define SLOTS 16
static struct { char id[NET_EOS_ID_LEN + 1]; u32 msg, pieces, got, len; u64 at; u8 head[HEAD]; u8 data[BIGGEST]; } slot[SLOTS];
static void deliver(const char *from, const u8 *h, const u8 *data, u32 len) {
    u32 src = get32(h + 4), dst = get32(h + 8), sport = get16(h + 12), dport = get16(h + 14), ip = 0, me = eos_hub_ip();
    if (!eos_hub_member_ip(from, &ip) || ip != src || !me) { InterlockedIncrement(&not_member); return; }   /* (only a member, with its own address) */
    if (dst != me && dst != 0xffffffffu && dst != NET_BROADCAST) { InterlockedIncrement(&not_for_us); return; }
    if (h[2] == K_LDN) InterlockedIncrement(&in_ldn); else if (dport == NET_RB_PORT) InterlockedIncrement(&in_rb); else InterlockedIncrement(&in_data);
    if (h[2] == K_LDN) { if (net_backend_ldn_sink) net_backend_ldn_sink(src, h[3], data, len); return; }
    if (dport == NET_RB_PORT && net_rb_deliver(src, data, len)) return;
    vsock_deliver(src, (u16)sport, dst == me ? me : net_broadcast_ip(), (u16)dport, data, len);
}
void net_eos_net_packet(const char *from, const u8 *p, u32 n) {
    if (n < HEAD || p[0] != 'E' || p[1] != 'B') return;
    u32 msg = get16(p + 16), i = p[18], pieces = p[19], len = n - HEAD;
    if (pieces <= 1) { deliver(from, p, p + HEAD, len); return; }
    if (i >= pieces || len > PIECE) return;
    u64 t = GetTickCount64();
    int k = -1, old = 0;
    for (int j = 0; j < SLOTS; j++) {
        if (slot[j].pieces && !strcmp(slot[j].id, from) && slot[j].msg == msg) { k = j; break; }
        if (slot[j].at < slot[old].at) old = j;
    }
    if (k < 0) {   /* (a new message: the oldest slot, or one given up after a second) */
        k = old;
        if (slot[k].pieces && t - slot[k].at < 1000) for (int j = 0; j < SLOTS; j++) if (!slot[j].pieces) { k = j; break; }
        snprintf(slot[k].id, sizeof slot[k].id, "%s", from);
        slot[k].msg = msg; slot[k].pieces = pieces; slot[k].got = 0; slot[k].len = 0; slot[k].at = t;
        memcpy(slot[k].head, p, HEAD);
    }
    if (slot[k].pieces != pieces || i * PIECE + len > BIGGEST) return;
    memcpy(slot[k].data + i * PIECE, p + HEAD, len);
    slot[k].got |= 1u << (i & 31);
    if (i == pieces - 1) slot[k].len = i * PIECE + len;
    if (pieces <= 32 && slot[k].got == (pieces == 32 ? 0xffffffffu : (1u << pieces) - 1) && slot[k].len) {
        deliver(from, slot[k].head, slot[k].data, slot[k].len);
        slot[k].pieces = 0;
    }
}

const NetBackend net_eos_backend = {selected, start, local_ip, player_name, unreachable, send_data, send_ldn, has_peer, status};

#else   /* (macOS and Linux: their EOS libraries come in phase 5) */
static int selected(void) { return 0; }
static void start(void) {}
static u32 local_ip(void) { return 0; }
static const char *player_name(void) { return "Player"; }
static int unreachable(void) { return 1; }
static void send_data(u32 a, u16 b, u32 c, u16 d, const void *e, u32 f) { (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; }
static void send_ldn(u32 a, unsigned b, const void *c, u32 d) { (void)a; (void)b; (void)c; (void)d; }
static int has_peer(u32 a) { (void)a; return 0; }
static void status(char *s, size_t n) { snprintf(s, n, "Epic Online Services: not on this system yet"); }
void net_eos_net_packet(const char *from, const u8 *p, u32 n) { (void)from; (void)p; (void)n; }
const NetBackend net_eos_backend = {selected, start, local_ip, player_name, unreachable, send_data, send_ldn, has_peer, status};
#endif
