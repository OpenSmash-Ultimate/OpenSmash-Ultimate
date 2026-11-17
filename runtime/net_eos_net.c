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
#define _CRT_RAND_S
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include "net.h"
#include "net_backend.h"
#include "net_eos.h"
#include "net_path.h"
#include "net_results.h"

#define HEAD 20
#define PIECE (NET_EOS_MAX - HEAD)
#define BIGGEST (32 * PIECE)   /* (32 pieces: about 36 KB, more than any datagram of the game) */
enum { K_DATA, K_LDN, K_PING, K_PONG, K_CAND };   /* (K_PING.. K_CAND: the paths' own, below) */

#ifdef _WIN32
#include <windows.h>
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ws2_32.lib")

#define DHEAD 12   /* a direct packet's own header: 'E' 'D' type 0 | the sender's token (8) */
static void direct_raw(const struct sockaddr_in *to, const u8 *p, u32 n);
static int path_direct_for(const char *id, int rollback, struct sockaddr_in *at, u8 head[DHEAD]);
static void path_control(const char *from, const u8 *p, u32 n, const struct sockaddr_in *direct_from);
static void path_start(void);

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
static int talking(void);
static void start(void) {
    static LONG once;
    if (InterlockedExchange(&once, 1)) return;
    sim_on();
    started_at = GetTickCount64();
    net_eos_names(room_code, sizeof room_code, player, sizeof player);
    char bucket[128];
    snprintf(bucket, sizeof bucket, "ssbu1:%s", room_code);   /* (the build and mods join it in phase 2's gatherings) */
    eos_start();
    eos_hub_set_busy(talking);
    eos_hub_join(bucket);
    path_start();
    online_results_begin();   /* (hle_online.c / net_results.c: the player's results to the public lobby, theirs back) */
    fprintf(stderr, "[eos] online through Epic Online Services alone: lobby code %s, as %s\n", room_code, player);
}

/* ---- SSBU_NET_SIM ---- */
#define SIMQ 8192
#define SIM_MAX (NET_EOS_MAX + 16)   /* (a direct packet: the EOS one behind a header of its own) */
static struct { char to[NET_EOS_ID_LEN + 1]; u32 len; u64 due_us; int direct; struct sockaddr_in at; u8 data[SIM_MAX]; } *simq;
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
            u8 data[SIM_MAX];
            u32 len = simq[k].len;
            int direct = simq[k].direct;
            struct sockaddr_in at = simq[k].at;
            memcpy(to, simq[k].to, sizeof to);
            memcpy(data, simq[k].data, len);
            sim_head++;
            ReleaseSRWLockExclusive(&sim_lock);
            if (direct) direct_raw(&at, data, len);
            else eos_send(to, data, len);
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
static void sim_send(const char *id, const struct sockaddr_in *at, const u8 *pkt, u32 len, u64 due) {
    if (len > SIM_MAX) return;
    AcquireSRWLockExclusive(&sim_lock);
    if (sim_tail - sim_head >= SIMQ) { ReleaseSRWLockExclusive(&sim_lock); InterlockedIncrement(&sim_full); return; }
    u32 k = sim_tail++ % SIMQ;
    memcpy(simq[k].to, id, NET_EOS_ID_LEN + 1);
    simq[k].direct = at != NULL;
    if (at) simq[k].at = *at;
    simq[k].len = len;
    simq[k].due_us = due;
    memcpy(simq[k].data, pkt, len);
    ReleaseSRWLockExclusive(&sim_lock);
    SetEvent(sim_wake);
}

static void send_to(const char *id, int kind, unsigned type, u32 src, u32 dst, u32 sport, u32 dport, const void *data, u32 len);

/* ---- the pair's path: our own direct UDP, else Epic's P2P (net_path.h) ------------------------------------------
 * The owner's rule (2026-11-16): Epic's services for everyone, but a pair's packets go straight between the two games
 * where they can; Epic's relays only where no direct path forms, or where Epic's path is clearly quicker; the EU
 * lobby's relay only when EOS itself is not there (the whole game then plays through the lobby: net_relay.c).
 * EOS P2P punches through routers itself, but chooses between its direct path and its relays alone and says nothing
 * of round trips; so each game also opens a UDP socket of its own, tells each member it talks to its addresses (this
 * PC's, and the one a STUN server sees) and a token over EOS (K_CAND: another for each member, which only that member
 * learns, so a direct packet carrying it is that member's), and pings each of them on both paths every PING_MS: direct pings to every address it was told until one answers (that is the
 * punch), EOS pings (K_PING / K_PONG) through Epic. net_path.c picks a pair's path from the answers and logs it.
 * A direct packet: 'E' 'D' type 0 | the sender's token | ...; D_DATA carries an EOS packet ('E' 'B' ...) as it is.
 * SSBU_EOS_DIRECT=0 (or SSBU_EOS_RELAY=force, tests) leaves the direct path out: Epic's alone. */
#define PING_MS 250
#define CAND_MS 2000
#define CANDS_MAX 6
#define PATH_PEERS 64
#define FIGHT_MS 3000   /* rollback's packets to a member this recently: a fight is on (no move but off a dead path) */
#define PAIR_MS 30000   /* a member this game sent to or heard from (not broadcasts) this recently is a pair: pinged */
enum { D_DATA, D_PING, D_PONG };
typedef struct PathPeer {
    char id[NET_EOS_ID_LEN + 1];
    u64 rx;                                      /* the token this game gave them (their direct packets carry it) */
    u64 tx;                                      /* the token they gave this game (K_CAND); 0 not told yet */
    struct sockaddr_in cand[CANDS_MAX]; int ncand;
    struct sockaddr_in at; int at_ok;            /* the address their direct pongs come from */
    PathPick pick;
    u64 rb_at, said_at, used_at;                 /* rollback's last packet to them; the last log line; the last
                                                    packet to or from them alone */
    int said_cur, said_moves;
} PathPeer;
static SRWLOCK path_lock = SRWLOCK_INIT;
static PathPeer path_peers[PATH_PEERS];
static int npath;
static SOCKET dsock = INVALID_SOCKET;
static struct sockaddr_in my_cand[CANDS_MAX];
static volatile LONG my_ncand;
static u64 now_ms_(void) { return GetTickCount64(); }

static void direct_raw(const struct sockaddr_in *to, const u8 *p, u32 n) {
    if (dsock != INVALID_SOCKET) sendto(dsock, (const char *)p, (int)n, 0, (const struct sockaddr *)to, sizeof *to);
}
static u64 token_new(void) { unsigned a, b; rand_s(&a); rand_s(&b); return ((u64)a << 32 | b) | 1; }
static PathPeer *path_peer(const char *id, int make) {   /* (path_lock held exclusively when make) */
    for (int i = 0; i < npath; i++) if (!strcmp(path_peers[i].id, id)) return &path_peers[i];
    if (!make) return NULL;
    PathPeer *q = NULL;
    if (npath < PATH_PEERS) q = &path_peers[npath++];
    else {   /* (full: the one quiet longest, if quiet for long) */
        u64 t = now_ms_();
        for (int i = 0; i < npath; i++) if (t - path_peers[i].used_at > 4 * PAIR_MS && (!q || path_peers[i].used_at < q->used_at)) q = &path_peers[i];
        if (!q) return NULL;
    }
    memset(q, 0, sizeof *q);
    snprintf(q->id, sizeof q->id, "%s", id);
    q->rx = token_new();
    path_init(&q->pick);
    q->said_cur = -1;
    return q;
}
static void put64(u8 *p, u64 v) { for (int i = 7; i >= 0; i--, v >>= 8) p[i] = (u8)v; }
static u64 get64(const u8 *p) { u64 v = 0; for (int i = 0; i < 8; i++) v = v << 8 | p[i]; return v; }
static void dhead(u8 *h, int type, u64 token) { h[0] = 'E'; h[1] = 'D'; h[2] = (u8)type; h[3] = 0; put64(h + 4, token); }
static int sim_on(void);
static void direct_out(const char *id, const struct sockaddr_in *to, const u8 *p, u32 n, int may_lose) {   /* (SSBU_NET_SIM: held as data is) */
    if (!sim_on()) { direct_raw(to, p, n); return; }
    AcquireSRWLockExclusive(&sim_lock);
    int lost = may_lose && sim_loss > 0 && sim_rand() % 10000 < (unsigned)(sim_loss * 100);
    u64 due = now_us() + (u64)(sim_ms * 1000.0);
    ReleaseSRWLockExclusive(&sim_lock);
    if (!lost) sim_send(id, to, p, n, due);
}

/* a member this game talks to (a packet to or from it alone, or its addresses): a pair, pinged on both paths */
static void path_used(const char *id) {
    AcquireSRWLockExclusive(&path_lock);
    PathPeer *q = path_peer(id, 1);
    if (q) q->used_at = now_ms_();
    ReleaseSRWLockExclusive(&path_lock);
}

/* (net_eos.c, the EOS thread) 1 while this game talks to a member (a packet to or from it alone in the last 5 s: a
 * match, a room): its hub then stays the one it is in */
static int talking(void) {
    u64 t = now_ms_();
    int on = 0;
    AcquireSRWLockShared(&path_lock);
    for (int i = 0; i < npath && !on; i++) on = path_peers[i].used_at && t - path_peers[i].used_at < 5000;
    ReleaseSRWLockShared(&path_lock);
    return on;
}

/* (send_to) 1 and the address when the pair's packets go direct now; rollback's mark a fight on */
static int path_direct_for(const char *id, int rollback, struct sockaddr_in *at, u8 head[DHEAD]) {
    if (dsock == INVALID_SOCKET) return 0;
    int direct = 0;
    u64 tx = 0;
    AcquireSRWLockExclusive(&path_lock);
    PathPeer *q = path_peer(id, 0);
    if (q) {
        u64 t = now_ms_();
        if (rollback) q->rb_at = t;
        if (q->at_ok && q->tx && path_update(&q->pick, t, t - q->rb_at < FIGHT_MS) == PATH_DIRECT) { *at = q->at; tx = q->tx; direct = 1; }
    }
    ReleaseSRWLockExclusive(&path_lock);
    if (direct) dhead(head, D_DATA, tx);
    return direct;
}

static void path_say(PathPeer *q, u64 t) {   /* (path_lock held) the path a pair uses and both round trips: at each move,
                                                and every 10 s with SSBU_NET_LOG */
    if (q->said_cur == q->pick.cur && q->said_moves == q->pick.moves && (!getenv("SSBU_NET_LOG") || t - q->said_at < 10000)) return;
    int moved = q->said_moves != q->pick.moves;
    q->said_cur = q->pick.cur; q->said_moves = q->pick.moves; q->said_at = t;
    char d[24], e[24];
    if (path_alive(&q->pick, PATH_DIRECT, t)) snprintf(d, sizeof d, "%.0f ms", q->pick.rtt[PATH_DIRECT]);
    else snprintf(d, sizeof d, "%s", dsock == INVALID_SOCKET ? "off" : "none");
    if (path_alive(&q->pick, PATH_EOS, t)) snprintf(e, sizeof e, "%.0f ms", q->pick.rtt[PATH_EOS]);
    else snprintf(e, sizeof e, "none");
    fprintf(stderr, "[eos] path to %.8s: %s (direct %s, Epic's P2P %s)%s%s\n", q->id, path_name(q->pick.cur), d, e,
            moved ? ": " : "", moved ? path_why(q->pick.why) : "");
}

/* (the path thread) pings on both paths, our addresses to every member, the choices kept up to date */
static void path_tick(void) {
    static u64 cand_at;
    static char ids[64][NET_EOS_ID_LEN + 1];
    u32 ips[64];
    int n = eos_hub_members(ids, ips, 64);
    const char *mine = eos_my_id();
    u64 t = now_ms_();
    int send_cand = t - cand_at >= CAND_MS && dsock != INVALID_SOCKET;
    if (send_cand) cand_at = t;
    u8 cand[9 + CANDS_MAX * 6];   /* (the token this game gave that member, then its addresses) */
    int nc = (int)my_ncand, clen = 9 + nc * 6;
    cand[8] = (u8)nc;
    for (int i = 0; i < nc; i++) { memcpy(cand + 9 + i * 6, &my_cand[i].sin_addr, 4); memcpy(cand + 13 + i * 6, &my_cand[i].sin_port, 2); }
    u8 ping[8];
    put64(ping, now_us());
    for (int i = 0; i < n; i++) {
        if (!ips[i] || (mine && !strcmp(ids[i], mine))) continue;
        AcquireSRWLockShared(&path_lock);
        PathPeer *u = path_peer(ids[i], 0);
        int pair = u && u->used_at && t - u->used_at < PAIR_MS;
        u64 rx = u ? u->rx : 0;
        ReleaseSRWLockShared(&path_lock);
        if (!pair) continue;   /* (only the members it talks to: a hub has up to 64) */
        send_to(ids[i], K_PING, 0, eos_hub_ip(), ips[i], 0, 0, ping, sizeof ping);
        if (send_cand) { put64(cand, rx); send_to(ids[i], K_CAND, 0, eos_hub_ip(), ips[i], 0, 0, cand, (u32)clen); }
        struct sockaddr_in to[CANDS_MAX];
        int nto = 0;
        u64 tx = 0;
        AcquireSRWLockExclusive(&path_lock);
        PathPeer *q = path_peer(ids[i], 1);
        if (q && q->tx && dsock != INVALID_SOCKET) {
            tx = q->tx;
            if (q->at_ok && path_alive(&q->pick, PATH_DIRECT, t)) to[nto++] = q->at;   /* (the one that answers) */
            else for (int c = 0; c < q->ncand; c++) to[nto++] = q->cand[c];       /* (every one: the punch) */
        }
        if (q) { path_update(&q->pick, t, t - q->rb_at < FIGHT_MS); path_say(q, t); }
        ReleaseSRWLockExclusive(&path_lock);
        u8 dp[DHEAD + 8];
        dhead(dp, D_PING, tx);
        put64(dp + DHEAD, now_us());
        for (int c = 0; c < nto; c++) direct_out(ids[i], &to[c], dp, sizeof dp, 1);
    }
}

/* a K_PING / K_PONG / K_CAND through EOS, or (direct_from) a direct ping / pong (p: past its header) */
static void path_control(const char *from, const u8 *p, u32 n, const struct sockaddr_in *direct_from) {
    u64 t = now_ms_();
    if (direct_from) {
        int type = p[-DHEAD + 2];
        if (n < 8) return;
        if (type == D_PING) {
            AcquireSRWLockShared(&path_lock);
            PathPeer *k = path_peer(from, 0);
            u64 tx = k ? k->tx : 0;
            ReleaseSRWLockShared(&path_lock);
            if (!tx) return;   /* (its token not told yet: it gets our pongs once it is) */
            u8 dp[DHEAD + 8];
            dhead(dp, D_PONG, tx);
            memcpy(dp + DHEAD, p, 8);
            direct_out(from, direct_from, dp, sizeof dp, 0);
            return;
        }
        if (type != D_PONG) return;
        double rtt = (double)(now_us() - get64(p)) / 1000.0;
        int first = 0;
        char a[24] = "";
        AcquireSRWLockExclusive(&path_lock);
        PathPeer *q = path_peer(from, 1);
        if (q && rtt >= 0 && rtt < 5000) {
            first = !q->at_ok || memcmp(&q->at.sin_addr, &direct_from->sin_addr, 4) || q->at.sin_port != direct_from->sin_port;
            q->at = *direct_from; q->at_ok = 1;
            path_answer(&q->pick, PATH_DIRECT, rtt, t);
        }
        ReleaseSRWLockExclusive(&path_lock);
        if (first) {
            inet_ntop(AF_INET, &direct_from->sin_addr, a, sizeof a);
            fprintf(stderr, "[eos] path to %.8s: a direct path answers at %s:%u (%.0f ms)\n", from, a, ntohs(direct_from->sin_port), rtt);
        }
        return;
    }
    if (n < HEAD) return;
    const u8 *b = p + HEAD;
    u32 len = n - HEAD;
    if (p[2] == K_PING && len >= 8) {
        send_to(from, K_PONG, 0, eos_hub_ip(), get32(p + 4), 0, 0, b, 8);
    } else if (p[2] == K_PONG && len >= 8) {
        double rtt = (double)(now_us() - get64(b)) / 1000.0;
        AcquireSRWLockExclusive(&path_lock);
        PathPeer *q = path_peer(from, 1);
        if (q && rtt >= 0 && rtt < 10000) path_answer(&q->pick, PATH_EOS, rtt, t);
        ReleaseSRWLockExclusive(&path_lock);
    } else if (p[2] == K_CAND && len >= 9) {
        int nc = b[8];
        if (nc > CANDS_MAX || len < 9u + (u32)nc * 6) return;
        AcquireSRWLockExclusive(&path_lock);
        PathPeer *q = path_peer(from, 1);
        if (q) {
            q->used_at = t;   /* (it talks to us: a pair) */
            q->tx = get64(b);
            q->ncand = nc;
            for (int i = 0; i < nc; i++) {
                memset(&q->cand[i], 0, sizeof q->cand[i]);
                q->cand[i].sin_family = AF_INET;
                memcpy(&q->cand[i].sin_addr, b + 9 + i * 6, 4);
                memcpy(&q->cand[i].sin_port, b + 13 + i * 6, 2);
            }
        }
        ReleaseSRWLockExclusive(&path_lock);
    }
}

/* STUN (RFC 5389): one Binding request from the direct socket gives the address a router shows outside for it
 * (stun.l.google.com:19302; SSBU_ONLINE_STUN=<host:port>, or off) */
static u8 stun_tid[12];
static int stun_answer(const u8 *p, int n, struct sockaddr_in *out) {
    if (n < 20 || p[0] != 0x01 || p[1] != 0x01 || memcmp(p + 8, stun_tid, 12)) return 0;
    for (int at = 20; at + 4 <= n;) {
        int type = p[at] << 8 | p[at + 1], len = p[at + 2] << 8 | p[at + 3];
        if (at + 4 + len > n) break;
        if (type == 0x0020 && len >= 8 && p[at + 5] == 0x01) {   /* (XOR-MAPPED-ADDRESS, IPv4) */
            memset(out, 0, sizeof *out);
            out->sin_family = AF_INET;
            u16 port = (u16)((p[at + 6] << 8 | p[at + 7]) ^ 0x2112);
            u32 ip = ((u32)p[at + 8] << 24 | (u32)p[at + 9] << 16 | (u32)p[at + 10] << 8 | p[at + 11]) ^ 0x2112A442u;
            out->sin_port = htons(port);
            out->sin_addr.s_addr = htonl(ip);
            return 1;
        }
        at += 4 + ((len + 3) & ~3);
    }
    return 0;
}
static void stun_ask(void) {
    const char *e = getenv("SSBU_ONLINE_STUN");
    if (e && !_stricmp(e, "off")) return;
    char server[128];
    snprintf(server, sizeof server, "%s", e && *e ? e : "stun.l.google.com:19302");
    char *colon = strrchr(server, ':');
    const char *port = colon ? colon + 1 : "3478";
    if (colon) *colon = 0;
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(server, port, &hints, &res) || !res) return;
    u8 req[20] = {0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42};
    for (int i = 0; i < 12; i++) { unsigned r; rand_s(&r); stun_tid[i] = (u8)r; }
    memcpy(req + 8, stun_tid, 12);
    sendto(dsock, (const char *)req, sizeof req, 0, res->ai_addr, (int)res->ai_addrlen);
    freeaddrinfo(res);
}
static void cand_add(const struct sockaddr_in *a) {
    int n = (int)my_ncand;
    for (int i = 0; i < n; i++) if (!memcmp(&my_cand[i].sin_addr, &a->sin_addr, 4) && my_cand[i].sin_port == a->sin_port) return;
    if (n >= CANDS_MAX) return;
    my_cand[n] = *a;
    InterlockedExchange(&my_ncand, n + 1);
}

static DWORD WINAPI path_thread(LPVOID arg) {
    (void)arg;
    u64 next = 0, stun_at = 0;
    int stun_tries = 0;
    for (;;) {
        u64 t = now_ms_();
        if (dsock != INVALID_SOCKET && stun_tries < 5 && t >= stun_at) { stun_tries++; stun_at = t + 1500; stun_ask(); }
        if (t >= next) { next = t + PING_MS; if (eos_hub_ip()) path_tick(); }
        if (dsock == INVALID_SOCKET) { Sleep(20); continue; }
        fd_set r;
        FD_ZERO(&r);
        FD_SET(dsock, &r);
        struct timeval tv = {0, 20000};
        if (select(0, &r, NULL, NULL, &tv) <= 0) continue;
        u8 buf[2048];
        struct sockaddr_in from;
        int fl = sizeof from;
        int got = recvfrom(dsock, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (got <= 0) continue;
        struct sockaddr_in mapped;
        if (stun_answer(buf, got, &mapped)) {
            cand_add(&mapped);
            stun_tries = 5;
            char a[24];
            inet_ntop(AF_INET, &mapped.sin_addr, a, sizeof a);
            if (getenv("SSBU_NET_LOG")) fprintf(stderr, "[eos] direct path: this game is %s:%u outside its router (STUN)\n", a, ntohs(mapped.sin_port));
            continue;
        }
        if (got < DHEAD || buf[0] != 'E' || buf[1] != 'D') continue;
        u64 token = get64(buf + 4);
        char id[NET_EOS_ID_LEN + 1] = "";
        AcquireSRWLockShared(&path_lock);
        for (int i = 0; i < npath && !id[0]; i++) if (token && path_peers[i].rx == token) snprintf(id, sizeof id, "%s", path_peers[i].id);
        ReleaseSRWLockShared(&path_lock);
        if (!id[0]) continue;   /* (only the member this game gave that token to, over EOS) */
        if (buf[2] == D_DATA) net_eos_net_packet(id, buf + DHEAD, (u32)(got - DHEAD));
        else path_control(id, buf + DHEAD, (u32)(got - DHEAD), &from);
    }
}

static void path_start(void) {
    const char *off = getenv("SSBU_EOS_DIRECT"), *relay = getenv("SSBU_EOS_RELAY");
    if ((off && *off == '0') || (relay && !_stricmp(relay, "force"))) {
        fprintf(stderr, "[eos] direct path off (%s): Epic's P2P alone\n", off && *off == '0' ? "SSBU_EOS_DIRECT=0" : "SSBU_EOS_RELAY=force");
    } else {
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        struct sockaddr_in a = {0};
        a.sin_family = AF_INET;
        int len = sizeof a;
        if (s != INVALID_SOCKET && !bind(s, (struct sockaddr *)&a, sizeof a) && !getsockname(s, (struct sockaddr *)&a, &len)) {
            dsock = s;
            struct sockaddr_in c = {0};
            c.sin_family = AF_INET;
            c.sin_port = a.sin_port;
            char host[256];
            struct addrinfo hints = {0}, *res = NULL;
            hints.ai_family = AF_INET;
            if (!gethostname(host, sizeof host) && !getaddrinfo(host, NULL, &hints, &res)) {   /* (this PC's addresses) */
                for (struct addrinfo *i = res; i; i = i->ai_next) { c.sin_addr = ((struct sockaddr_in *)i->ai_addr)->sin_addr; cand_add(&c); }
                freeaddrinfo(res);
            }
            c.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* (two games on one PC) */
            cand_add(&c);
            fprintf(stderr, "[eos] direct path: UDP port %u, %ld address(es) of this PC told to the others\n", ntohs(a.sin_port), (long)my_ncand);
        } else {
            fprintf(stderr, "[eos] direct path: no UDP socket (%d): Epic's P2P alone\n", WSAGetLastError());
            if (s != INVALID_SOCKET) closesocket(s);
        }
    }
    HANDLE t = CreateThread(NULL, 0, path_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

/* one datagram (or Local Wireless message) to one member, in pieces when it needs them */
static void send_to(const char *id, int kind, unsigned type, u32 src, u32 dst, u32 sport, u32 dport, const void *data, u32 len) {
    u8 pkt[DHEAD + NET_EOS_MAX], *eb = pkt + DHEAD;
    struct sockaddr_in at;
    int direct = kind < K_PING && path_direct_for(id, dport == NET_RB_PORT, &at, pkt);   /* (the paths' own pings stay on theirs) */
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
        u32 off = i * PIECE, n = len - off < PIECE ? len - off : PIECE;
        eb[0] = 'E'; eb[1] = 'B'; eb[2] = (u8)kind; eb[3] = (u8)type;
        put32(eb + 4, src); put32(eb + 8, dst); put16(eb + 12, sport); put16(eb + 14, dport);
        put16(eb + 16, msg); eb[18] = (u8)i; eb[19] = (u8)pieces;
        if (n) memcpy(eb + HEAD, (const u8 *)data + off, n);
        if (direct) {   /* (our own direct path: the EOS packet behind the pair's header) */
            if (sim) sim_send(id, &at, pkt, DHEAD + HEAD + n, due);
            else direct_raw(&at, pkt, DHEAD + HEAD + n);
        } else if (sim) sim_send(id, NULL, eb, HEAD + n, due);
        else eos_send(id, eb, HEAD + n);
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
    if (eos_hub_member_id(dst, id)) { path_used(id); send_to(id, kind, type, src, dst, sport, dport, data, len); }
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

/* (the EOS thread, and the path thread's direct packets) pieces put back together, under slot_lock: one message at a
 * time per sender is enough for UDP's order */
static SRWLOCK slot_lock = SRWLOCK_INIT;
#define SLOTS 16
static struct { char id[NET_EOS_ID_LEN + 1]; u32 msg, pieces, got, len; u64 at; u8 head[HEAD]; u8 data[BIGGEST]; } slot[SLOTS];
static void deliver(const char *from, const u8 *h, const u8 *data, u32 len) {
    u32 src = get32(h + 4), dst = get32(h + 8), sport = get16(h + 12), dport = get16(h + 14), ip = 0, me = eos_hub_ip();
    if (!eos_hub_member_ip(from, &ip) || ip != src || !me) { InterlockedIncrement(&not_member); return; }   /* (only a member, with its own address) */
    if (dst != me && dst != 0xffffffffu && dst != NET_BROADCAST) { InterlockedIncrement(&not_for_us); return; }
    if (dst == me) path_used(from);
    if (h[2] == K_LDN) InterlockedIncrement(&in_ldn); else if (dport == NET_RB_PORT) InterlockedIncrement(&in_rb); else InterlockedIncrement(&in_data);
    if (h[2] == K_LDN) { if (net_backend_ldn_sink) net_backend_ldn_sink(src, h[3], data, len); return; }
    if (dport == NET_RB_PORT && net_rb_deliver(src, data, len)) return;
    vsock_deliver(src, (u16)sport, dst == me ? me : net_broadcast_ip(), (u16)dport, data, len);
}
void net_eos_net_packet(const char *from, const u8 *p, u32 n) {
    if (n < HEAD || p[0] != 'E' || p[1] != 'B') return;
    if (p[2] >= K_PING) {   /* (the paths' own, from a member) */
        u32 ip = 0;
        if (eos_hub_member_ip(from, &ip) && ip == get32(p + 4)) path_control(from, p, n, NULL);
        return;
    }
    u32 msg = get16(p + 16), i = p[18], pieces = p[19], len = n - HEAD;
    if (pieces <= 1) { deliver(from, p, p + HEAD, len); return; }
    if (i >= pieces || len > PIECE) return;
    u64 t = GetTickCount64();
    AcquireSRWLockExclusive(&slot_lock);
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
    if (slot[k].pieces != pieces || i * PIECE + len > BIGGEST) { ReleaseSRWLockExclusive(&slot_lock); return; }
    memcpy(slot[k].data + i * PIECE, p + HEAD, len);
    slot[k].got |= 1u << (i & 31);
    if (i == pieces - 1) slot[k].len = i * PIECE + len;
    u8 *whole = NULL, head[HEAD];
    u32 wlen = 0;
    if (pieces <= 32 && slot[k].got == (pieces == 32 ? 0xffffffffu : (1u << pieces) - 1) && slot[k].len) {
        wlen = slot[k].len;
        if ((whole = malloc(wlen))) { memcpy(whole, slot[k].data, wlen); memcpy(head, slot[k].head, HEAD); }
        slot[k].pieces = 0;
    }
    ReleaseSRWLockExclusive(&slot_lock);
    if (whole) { deliver(from, head, whole, wlen); free(whole); }   /* (outside the lock: the game's own work) */
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
