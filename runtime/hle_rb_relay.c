/* Rollback netplay's second link: the rollback messages (hle_rb_net.h) through the netplay client
 * (net_relay.c, net.h's NET_RB_PORT), inside the room the players already share - a private room's
 * relay hosted by one of their games, or the online lobby (tools/ssbu_relay.py) - addressed by the
 * room members' virtual addresses (10.13.x.y). net_relay.c sends each one straight to the other game
 * where the lobby introduced the two and their routers let the hole punching through, through the
 * relay otherwise; nobody opens a port for it (only a private room's host, whose relay port exists
 * already). Once SSBU_RB_NET=relay opened this link (rbl_open_relay), hle_rb_link.c hands every rbl_*
 * call here (rbl_relay, hle_rb_link.h); its direct UDP link (SSBU_RB_NET=net) is as it was.
 *
 * The same star as the UDP link: player 1's game (the host) talks to every other game, each of
 * them to the host only, and the host passes messages on (hle_rb_core.c). A game that does not know
 * the host's address yet sends to every player of the room (net_rb_send's destination 0), and takes
 * as the host the first game whose valid message says it is player 1; until then it takes nothing
 * from anyone else.
 *
 * Players' numbers (SSBU_RB_NET=relay,auto, the default): the room's members ordered by virtual
 * address, lowest first, are players 1, 2, ...; a game waits until the room has SSBU_RB_NET_PLAYERS
 * of them (2 by default; every game must say the same), then keeps its number for the session. A
 * private room's relay gives its members the lowest free address from 10.13.0.2 in the order they
 * join, so the hosting game, which joins its own relay first, is player 1. In the lobby the room is
 * the Local Wireless room both games are in (its players as the lobby introduces them, PEERS), and
 * the order is the lobby's (it hands addresses out in turn). Players of a lobby code who share no
 * room give their numbers instead (relay,0 / relay,1 ...; relay,-1: the host gives one, as the UDP
 * link's Host/Join). */
#ifndef _WIN32
typedef int hle_rb_relay_unused;   /* (rollback netplay is written for Windows: hle_rb_core.c) */
#else
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "net.h"
#include "net_eos.h"
#include "hle_rb_link.h"

#define LP 8
#define QN 1024
#define QBYTES 2048
static struct { int len; u32 from; double at; char data[QBYTES]; } q[QN];
static unsigned q_head, q_tail;   /* (q_tail - q_head messages waiting) */
static SRWLOCK q_lock = SRWLOCK_INIT;
static volatile LONG open_now;
static u32 peers[LP];
static int have[LP];
static int local = -1, is_host, link_players = 2, want_players = 2, numbered, online;
static u32 last_from;
static double last_at;
static long long sent, received, dropped, overflowed;

static double clock_ms(void) {   /* (the performance counter's clock, as hle_rb_core.c's now_ms) */
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}
static void ip_text(u32 ip, char *s, size_t n) { snprintf(s, n, "%u.%u.%u.%u", ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255); }

/* (the netplay client's thread) a message from the room, kept with when it came, as hle_rb_link.c's receive thread */
static void sink(u32 src, const void *data, u32 len) {
    if (!open_now || !len || len > QBYTES) return;
    double at = clock_ms();
    AcquireSRWLockExclusive(&q_lock);
    if (q_tail - q_head < QN) {
        unsigned k = q_tail++ % QN;
        q[k].len = (int)len; q[k].from = src; q[k].at = at; memcpy(q[k].data, data, len);
    } else overflowed++;
    ReleaseSRWLockExclusive(&q_lock);
}

/* local: this game's player (0 the host), -1 a joiner the host numbers, -2 from the room's order (rbr_number).
 * players: how many play (the room's order waits for that many). 1: ready. */
static int rbr_open(int loc, int players) {
    if (!net_configured()) {
        fprintf(stderr, "[rb] link: SSBU_RB_NET=relay needs netplay: a lobby (SSBU_NET_RELAY) or a private room (SSBU_NET_HOST=1, or SSBU_NET_RELAY to its host)\n");
        return 0;
    }
    memset(peers, 0, sizeof peers); memset(have, 0, sizeof have);
    want_players = link_players = players < 2 ? 2 : players > LP ? LP : players;
    local = loc < -1 ? -1 : loc; numbered = loc >= -1; is_host = loc == 0; online = loc == RBL_ONLINE;
    q_head = q_tail = 0;
    sent = received = dropped = overflowed = 0;
    InterlockedExchange(&open_now, 1);
    net_rb_sink(sink);
    char who[64];
    if (loc == RBL_ONLINE) snprintf(who, sizeof who, "its number from each Online match's set-up");
    else if (loc == -2) snprintf(who, sizeof who, "its number from the room's order, once %d players are in it", want_players);
    else if (loc == -1) snprintf(who, sizeof who, "its number from the host");
    else snprintf(who, sizeof who, "player %d%s", loc + 1, loc == 0 ? " (the host)" : "");
    fprintf(stderr, "[rb] link: through the netplay relay (virtual port %d, direct where the routers let it): %s\n", NET_RB_PORT, who);
    return 1;
}
static void rbr_close(void) {
    net_rb_sink(NULL);
    InterlockedExchange(&open_now, 0);
    memset(have, 0, sizeof have);
    sent = received = dropped = overflowed = 0;
    q_head = q_tail = 0;
}
/* (SSBU_RB_NET=relay,auto) this game's number: its place among the room's members by virtual address, once the room has
 * every player; -1 until then (and while it has more members than players and this game is not among the first). */
static int rbr_number(void) {
    if (numbered) return local;
    if (online) return -1;   /* (rbl_relay_online numbers it) */
    u32 m[16], me = net_local_ip();
    int n = net_room_members(m, 16);
    if (!me || n < want_players) return -1;
    int rank = -1;
    for (int i = 0; i < n; i++) if (m[i] == me) rank = i;
    static int shown;
    if (rank < 0 || rank >= want_players) {
        if (!shown++) fprintf(stderr, "[rb] link: the room has %d members, more than the %d players: only the first %d by address play, and this game is not one of them\n",
                              n, want_players, want_players);
        return -1;
    }
    char t[160] = "", a[20];
    size_t k = 0;
    for (int i = 0; i < want_players && k < sizeof t; i++) { ip_text(m[i], a, sizeof a); k += (size_t)snprintf(t + k, sizeof t - k, "%s%s", i ? ", " : "", a); }
    local = rank; is_host = rank == 0; numbered = 1;
    fprintf(stderr, "[rb] link: the room's players by address: %s: this game is player %d%s\n", t, rank + 1, rank == 0 ? " (the host)" : "");
    return local;
}
static void rbr_players(int n) { link_players = n < 2 ? 2 : n > LP ? LP : n; }
static void rbr_send_to(int p, const void *buf, int len) {
    if (!open_now || p < 0 || p >= LP || len <= 0) return;
    if (have[p]) { net_rb_send(peers[p], buf, (u32)len); sent++; return; }
    if (!is_host && p == 0 && numbered) { net_rb_send(0, buf, (u32)len); sent++; }   /* (the host not found yet: to every player of the room) */
}
static void rbr_send(const void *buf, int len) {
    if (!is_host) { rbr_send_to(0, buf, len); return; }
    for (int p = 1; p < LP; p++) if (have[p]) rbr_send_to(p, buf, len);
}
/* (a message from this address may be read: a known game's; the host's, anyone's in the room (it takes a sender in
 * only as a valid joiner, rbr_accept); a game without its host yet, anyone's too, but rbr_accept takes only player 1) */
static int welcome(u32 from) {
    if (from == net_local_ip()) return 0;
    for (int p = 0; p < LP; p++) if (have[p] && peers[p] == from) return 1;
    return is_host || !have[0];
}
/* (SSBU_RB_LINK_DELAY_MS and SSBU_NET_SIM=<ms>[,<loss %>[,<jitter ms>]], as hle_rb_link.c's: each message held that long
 * after it came, that share of them lost, up to that much more on each; tests only. Two games on one PC each holding
 * what they receive by <ms> play as over a round trip of 2 x <ms>.) */
static double link_delay_ms = -1, sim_loss, sim_jitter;
static unsigned sim_seed = 0x9E3779B9u;
static long long sim_lost;
static unsigned sim_rand(void) { sim_seed ^= sim_seed << 13; sim_seed ^= sim_seed >> 17; sim_seed ^= sim_seed << 5; return sim_seed; }
static int rbr_recv(void *buf, int cap) {
    if (!open_now) return 0;
    if (link_delay_ms < 0) {
        const char *e = getenv("SSBU_RB_LINK_DELAY_MS"), *sim = getenv("SSBU_NET_SIM");
        link_delay_ms = e && atof(e) > 0 ? atof(e) : 0;
        if (sim && *sim && net_backend == &net_eos_backend) {   /* (EOS's network holds and drops all the game's traffic itself: net_eos_net.c) */
            fprintf(stderr, "[rb] link (relay): SSBU_NET_SIM applied by the EOS network to everything sent, not again here\n");
            sim = NULL;
        }
        if (sim && *sim) {
            double ms = 0, loss = 0, jitter = 0;
            sscanf(sim, "%lf,%lf,%lf", &ms, &loss, &jitter);
            if (ms > 0) link_delay_ms = ms;
            sim_loss = loss > 0 ? (loss < 100 ? loss : 100) : 0;
            sim_jitter = jitter > 0 ? jitter : 0;
            fprintf(stderr, "[rb] link (relay): SSBU_NET_SIM: %.0f ms held, %.1f%% lost, up to %.0f ms jitter\n", link_delay_ms, sim_loss, sim_jitter);
        }
    }
    for (;;) {
        AcquireSRWLockExclusive(&q_lock);
        if (q_head == q_tail) { ReleaseSRWLockExclusive(&q_lock); return 0; }
        double hold = link_delay_ms;
        if (sim_jitter > 0) hold += (double)((q_head * 2654435761u) % 1000) / 1000.0 * sim_jitter;   /* (the same for a message however often asked) */
        if (hold > 0 && clock_ms() - q[q_head % QN].at < hold) { ReleaseSRWLockExclusive(&q_lock); return 0; }
        unsigned k = q_head++ % QN;
        if (sim_loss > 0 && sim_rand() % 10000 < (unsigned)(sim_loss * 100)) { sim_lost++; ReleaseSRWLockExclusive(&q_lock); continue; }
        u32 from = q[k].from;
        int n = q[k].len < cap ? q[k].len : cap;
        memcpy(buf, q[k].data, (size_t)n);
        double at = hold > 0 ? clock_ms() : q[k].at;   /* (a simulated hold: the message "arrives" as it is let through, so the round trips measured include it) */
        ReleaseSRWLockExclusive(&q_lock);
        if (!welcome(from)) { dropped++; continue; }
        last_from = from; last_at = at;
        received++;
        return n;
    }
}
static double rbr_last_arrival_ms(void) { return last_at; }
/* The last message read was valid and player p's (as it says): as hle_rb_link.c's rbl_accept, by virtual address. A
 * virtual address does not move (a game that comes back into the room keeps its own; one started again has a new one,
 * and the host knows it again by its join id: rbr_assign). 1: new; 0: as known; -1: refused. */
static int rbr_accept(int p) {
    int k = is_host ? p : 0;
    if (k < 0 || k >= LP || (is_host && (k == 0 || k >= link_players))) return -1;
    if (!is_host && !have[0] && p != 0) return -1;   /* (the host is the game that says it is player 1) */
    if (have[k]) return last_from == peers[k] ? 0 : -1;
    for (int o = 0; o < LP; o++) if (have[o] && peers[o] == last_from) return -1;
    char a[20]; ip_text(last_from, a, sizeof a);
    fprintf(stderr, "[rb] link: player %d found at %s%s\n", k + 1, a, k == 0 && !is_host ? " (the host)" : "");
    peers[k] = last_from; have[k] = 1;
    return 1;
}
static int rbr_has_peer(int p) { return p >= 0 && p < LP && have[p]; }
static int rbr_who(void) {
    for (int p = 1; p < LP; p++) if (have[p] && peers[p] == last_from) return p;
    return -1;
}
static void rbr_assign(int p) {
    if (!is_host || p < 1 || p >= LP) return;
    char a[20]; ip_text(last_from, a, sizeof a);
    fprintf(stderr, "[rb] link: a joining game at %s is player %d\n", a, p + 1);
    peers[p] = last_from; have[p] = 1;
}
static void rbr_stats(long long *s, long long *r, long long *e) { *s = sent; *r = received; *e = 0; }
static long long rbr_dropped(void) { return dropped + overflowed; }
/* (the fight's report) which way the messages went: the netplay client's counts since the game started */
static void rbr_report(void) {
    NetRbCounts c;
    net_rb_counts(&c);
    fprintf(stderr, "[rb] link (relay): messages out %lld as datagrams (straight or through the relay), %lld through the lobby; in %lld straight, "
            "%lld through the relay, %lld through the lobby; %lld dropped (not the room's player, or the queue full)\n",
            c.out_data, c.out_lobby, c.in_direct, c.in_relayed, c.in_lobby, dropped + overflowed);
}

static const RblOps ops = {rbr_close, rbr_players, rbr_send_to, rbr_send, rbr_recv, rbr_last_arrival_ms, rbr_accept, rbr_has_peer,
                           rbr_who, rbr_assign, rbr_stats, rbr_dropped};
int rbl_open_relay(int loc, int players) {
    if (rbl_relay) rbl_relay->close();
    rbl_relay = rbr_open(loc, players) ? &ops : NULL;
    return rbl_relay != NULL;
}
int rbl_relay_number(void) { return rbl_relay == &ops ? rbr_number() : -1; }
int rbl_relay_online(int loc, int players, const unsigned *ips) {
    if (rbl_relay != &ops || !online || loc < 0 || loc >= LP || players < 2 || players > LP) return 0;
    AcquireSRWLockExclusive(&q_lock);
    q_head = q_tail;   /* (anything queued is from before this match) */
    ReleaseSRWLockExclusive(&q_lock);
    memset(peers, 0, sizeof peers); memset(have, 0, sizeof have);
    want_players = link_players = players;
    char t[160] = "", a[20];
    size_t k = 0;
    for (int p = 0; p < players; p++) {
        if (p == loc) { k += (size_t)snprintf(t + k, sizeof t - k, "%sthis game", p ? ", " : ""); continue; }
        if (!ips[p]) return 0;
        peers[p] = ips[p]; have[p] = 1;
        ip_text(ips[p], a, sizeof a); k += (size_t)snprintf(t + k, sizeof t - k, "%s%s", p ? ", " : "", a);
    }
    local = loc; is_host = loc == 0; numbered = 1;
    fprintf(stderr, "[rb] link: an Online match's players: %s: this game is player %d%s\n", t, loc + 1, loc == 0 ? " (the host)" : "");
    return 1;
}
void rbl_relay_online_end(void) {
    if (rbl_relay != &ops || !online || !numbered) return;
    memset(peers, 0, sizeof peers); memset(have, 0, sizeof have);
    local = -1; is_host = 0; numbered = 0;
    fprintf(stderr, "[rb] link: the Online match is over: its players forgotten\n");
}
void rbl_relay_report(void) { if (rbl_relay == &ops) rbr_report(); }

#endif   /* _WIN32 */
