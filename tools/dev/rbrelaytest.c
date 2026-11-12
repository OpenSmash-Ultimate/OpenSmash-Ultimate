/* Rollback netplay's link through the netplay relay (runtime/hle_rb_relay.c, SSBU_RB_NET=relay) with no
 * game around it, for tests/test_rb_net.py: the game's netplay client (runtime/net_relay.c) joins the lobby
 * or private room the environment names (SSBU_NET_*: net.h), and the rollback link opened on it (rbl_*, as
 * hle_rb_core.c calls them) trades real rollback messages (runtime/hle_rb_net.c's HELLO and PADS) with
 * another rbrelaytest: a handshake in which the host finds every player and every player the host, then
 * PADS both ways until each has the other's pads for every frame, acknowledged. Made-up data only.
 * Windows only (the rollback build is); built as tools/dev/netv3test.c is, with three more sources:
 *   clang-cl /nologo /O1 <cc.CFLAGS> tools\dev\rbrelaytest.c runtime\hle_rb_relay.c runtime\hle_rb_link.c runtime\hle_rb_net.c
 *            /Febuild\rbrelaytest.exe /link ws2_32.lib iphlpapi.lib advapi32.lib user32.lib ole32.lib oleaut32.lib
 *
 * Commands (a line each; numbers decimal, addresses as numbers, bytes in hex), and their answers:
 *   join SECS              start the netplay client, wait until joined: netv3test.c's "joined VERSION ADDRESS FLAGS
 *                          FELL_BACK MEMBERS", or "notjoined"
 *   send DST SPORT DPORT HEX   a datagram from this game's address (net_send; the Local Wireless set-up): "ok"
 *   peers SECS N           wait until N players of the room are reached directly: netv3test.c's "peers DIRECT PEERS"
 *                          and a "peer ..." line each
 *   open WHO PLAYERS       rbl_open_relay: WHO auto, 0-3 or -1: "open 1" (or 0)
 *   number SECS            (open auto) wait for this game's number from the room's order: "number P" (-1: none)
 *   online P PEER          SSBU_RB_NET=online: the link opened with no number, then an Online match's set-up gives it
 *                          (rbl_relay_online: this game player P, the other at address PEER): "online OK NUMBER"
 *   offline                the Online match over (rbl_relay_online_end): "offline NUMBER" (-1)
 *   note IP / recent MS    net_note_peer (as hle_net.c for each datagram sent) / net_recent_peers: "recent N IP..."
 *   hello SECS             the handshake: "hello OK PLAYER" or "hello FAIL ..."
 *   pads N SECS            PADS for frames 1..N both ways (two players): "pads OK GOT BAD" or "pads FAIL ..."
 *   stats                  "stats OUT_DATA OUT_LOBBY IN_DIRECT IN_RELAYED IN_LOBBY SENT RECEIVED DROPPED"
 *   quit                   ends (net_relay.c's exit handler sends LEAVE)
 * Anything else: "error". */
#include "../../runtime/net_relay.c"
#include "../../runtime/net_eos.c"
#include "../../runtime/hle_rb_link.h"
#include "../../runtime/hle_rb_net.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

/* ---- what net_relay.c reaches outside itself (as netv3test.c) ---------------------------------- */
static char build_text[BUILD_LEN + 1];
const char *rt_data_dir = ".";
int rt_code_patches_on(void) { const char *e = getenv("SSBU_CODE_PATCHES"); return e && *e == '1'; }   /* (hle_mod_switch.c: SSBU_CODE_PATCHES here) */
const RtModule rt_module_main = {.build_id = build_text};
/* (net_eos_net.c, net_eos_lobby.c and the NEX server's: the EOS hub alone, the game's NEX over EOS; never reached here) */
const struct NetBackend net_eos_backend;
void net_eos_net_packet(const char *from, const u8 *p, u32 n) { (void)from; (void)p; (void)n; }
void nexeos_tick(void) {}
void nexeos_packet(const char *from, const u8 *data, u32 len) { (void)from; (void)data; (void)len; }
uint64_t nex_pid_of(const char *key) { (void)key; return 0; }
int settings_online(void) { return 0; }
int settings_line_read(const char *path, char *out, int size) { (void)path; (void)out; (void)size; return 0; }
int settings_line_write(const char *path, const char *line) { (void)path; (void)line; return 0; }
const char *config_path(enum ConfigFile file) { (void)file; return "none.txt"; }
void vsock_deliver(u32 src_ip, u16 src_port, u32 dst_ip, u16 dst_port, const void *data, u32 len) {
    (void)src_ip; (void)src_port; (void)dst_ip; (void)dst_port; (void)data; (void)len;   /* (the game's sockets: none here) */
}
int ldn_in_network(u32 ip) { (void)ip; return 1; }
void ldn_listing(u32 seq, u32 owner, const u8 *info) { (void)seq; (void)owner; (void)info; }
void ldn_list_end(u32 seq) { (void)seq; }
/* (hle_rb_link.c's, as hle_rb_core.c declares them) */
void rbl_send(const void *buf, int len);
void rbl_send_to(int p, const void *buf, int len);
int rbl_recv(void *buf, int cap);
int rbl_accept(int p);
int rbl_has_peer(int p);
int rbl_who(void);
void rbl_assign(int p);
void rbl_players(int n);
void rbl_stats(long long *s, long long *r, long long *e);
long long rbl_dropped(void);

static void say(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
    putchar('\n');
    fflush(stdout);
}
static u64 deadline(double secs) { return now_ms() + (u64)(secs * 1000); }

static int me = -1, nplayers = 2, is_auto;

static void cmd_send(u32 dst, unsigned sport, unsigned dport, const char *hex) {
    u8 data[1600];
    u32 n = 0;
    for (unsigned b; n < sizeof data && sscanf(hex + 2 * n, "%2x", &b) == 1; n++) data[n] = (u8)b;
    net_send(net_local_ip(), (u16)sport, dst, (u16)dport, data, n);
    say("ok");
}
static void cmd_peers(double secs, int want) {   /* (netv3test.c's answer) */
    Peer copy[PEERS_MAX];
    int n = 0, direct = 0;
    u64 end = deadline(secs);
    for (;;) {
        u64 t = now_ms();
        AcquireSRWLockShared(&peers_lock);
        n = npeers;
        memcpy(copy, peers, sizeof copy[0] * (size_t)n);
        direct = 0;
        for (int i = 0; i < n; i++) direct += usable(&peers[i], t);
        ReleaseSRWLockShared(&peers_lock);
        if (direct >= want || t >= end) break;
        Sleep(20);
    }
    say("peers %d %d", direct, n);
    for (int i = 0; i < n; i++)
        say("peer %u %s %ld %ld %ld %ld", copy[i].ip, usable(&copy[i], now_ms()) ? "direct" : copy[i].state == P_PUNCHING ? "punching" : "relayed",
            (long)copy[i].in_direct, (long)copy[i].in_relayed, (long)copy[i].out_direct, (long)copy[i].out_relayed);
}

/* The handshake, as hle_rb_core.c's in short: hellos every 20 ms, the host's to each player found, the others' to
 * the host (to the whole room while it is not found); the host takes a player in by its hello (a joiner, -1, gets the
 * lowest free number, told in the host's hellos), the others take the host by its. Done once this game has every
 * player it talks to, and heard from each since; it goes on saying hello a while for the others. */
static void cmd_hello(double secs) {
    u64 end = deadline(secs), done_at = 0, last = 0;
    int heard[RBN_PLAYERS] = {0};
    while (now_ms() < end && (!done_at || now_ms() < done_at + 400)) {
        u64 t = now_ms();
        if (t - last >= 20) {
            last = t;
            for (int p = 0; p < nplayers; p++) {
                if (p == me || (me != 0 && p != 0)) continue;
                RbnHello h = {me, RBN_VERSION, 0, 0, me == 0 ? 0x1234ull : me < 0 ? 0x77ull + (u64)GetCurrentProcessId() : 0};
                if (me == 0) { h.players = nplayers; h.assign = p + 1; }
                u8 out[RBN_HELLO_BYTES];
                size_t k = rbn_hello_build(out, sizeof out, &h);
                if (k) rbl_send_to(p, out, (int)k);
            }
        }
        u8 buf[2048];
        int len;
        while ((len = rbl_recv(buf, sizeof buf)) > 0) {
            RbnHello h;
            if (!rbn_hello_parse(buf, (size_t)len, &h) || h.version != RBN_VERSION) continue;
            int p = h.player;
            if (me == 0 && p == -1) {   /* (a joiner: the lowest free number, or its own) */
                p = rbl_who();
                for (int q = 1; p < 0 && q < nplayers; q++) if (!rbl_has_peer(q)) { rbl_assign(q); p = q; }
                if (p < 0) continue;
            } else {
                if (p < 0 || p >= nplayers || p == me || (me != 0 && p != 0)) continue;
                if (rbl_accept(p) < 0) continue;
            }
            if (me != 0 && h.assign && me < 0) { me = h.assign - 1; fprintf(stderr, "[test] the host made this game player %d\n", me + 1); }
            heard[p] = 1;
        }
        int all = me >= 0;
        for (int p = 0; p < nplayers && all; p++) if (p != me && (me == 0 || p == 0)) all = rbl_has_peer(p) && heard[p];
        if (all && !done_at) done_at = now_ms();
        Sleep(2);
    }
    if (done_at) say("hello OK %d", me);
    else say("hello FAIL %d", me);
}

/* PADS (two players): every 5 ms one message with this game's pads from the oldest the other has not acknowledged (64 at
 * most), its acknowledgement of the other's; each pad checked against what the other's must be. A pad is held for 3
 * frames in a row (runs on the wire). */
static RbnPad pad_of(int player, u32 frame) {
    RbnPad p = {0};
    u32 v = (frame + 2) / 3;
    p.buttons = (u64)(player + 1) << 40 | v;
    p.lx = (int16_t)(v * 7 + player); p.ly = (int16_t)-(int)(v & 0x3FFF); p.rt = (int16_t)player;
    return p;
}
static void cmd_pads(u32 n, double secs) {
    int other = me == 0 ? 1 : 0;
    static u8 got[1 << 16];
    if (n >= sizeof got || me < 0 || nplayers != 2) { say("pads FAIL setup"); return; }
    memset(got, 0, sizeof got);
    u32 low = 0, acked = 0, bad = 0, msgs = 0;
    u64 end = deadline(secs), done_at = 0, last = 0;
    while (now_ms() < end && (!done_at || now_ms() < done_at + 500)) {
        u64 t = now_ms();
        if (t - last >= 5) {
            last = t;
            RbnMsg m = {0};
            m.type = RBN_MSG_PADS; m.player = me;
            m.first = acked + 1 <= n ? acked + 1 : n;
            m.count = (int)(n - m.first + 1 < RBN_MAX_PADS ? n - m.first + 1 : RBN_MAX_PADS);
            m.frame = n;
            m.ack[other] = low;
            for (int i = 0; i < m.count; i++) m.pads[i] = pad_of(me, m.first + (u32)i);
            u8 out[RBN_MSG_MAX];
            size_t k = rbn_build(out, sizeof out, &m);
            if (k) rbl_send(out, (int)k);
        }
        u8 buf[2048];
        int len;
        while ((len = rbl_recv(buf, sizeof buf)) > 0) {
            RbnMsg m;
            if (!rbn_parse(buf, (size_t)len, &m)) continue;   /* (a late hello) */
            if (m.player != other || rbl_accept(m.player) < 0) continue;
            msgs++;
            if (m.ack[me] > acked && m.ack[me] <= n) acked = m.ack[me];
            for (int i = 0; i < m.count; i++) {
                u32 f = m.first + (u32)i;
                if (!f || f > n) continue;
                RbnPad want = pad_of(other, f);
                const RbnPad *g = &m.pads[i];   /* (field by field: the struct has padding) */
                if (g->buttons != want.buttons || g->lx != want.lx || g->ly != want.ly || g->rx != want.rx || g->ry != want.ry || g->lt != want.lt || g->rt != want.rt) { bad++; continue; }
                got[f] = 1;
            }
            while (low < n && got[low + 1]) low++;
        }
        if (low == n && acked == n && !done_at) done_at = now_ms();
        Sleep(1);
    }
    if (done_at && !bad) say("pads OK %u %u %u", low, bad, msgs);
    else say("pads FAIL got %u acked %u bad %u messages %u", low, acked, bad, msgs);
}

int main(void) {
    const char *b = getenv("NETV3TEST_BUILD");
    snprintf(build_text, sizeof build_text, "%s", b ? b : "");
    char line[4096];
    while (fgets(line, sizeof line, stdin)) {
        char what[16] = "", hex[3300] = "", who[16] = "";
        double secs = 0;
        int a = 0;
        unsigned u1 = 0, u2 = 0, u3 = 0;
        line[strcspn(line, "\r\n")] = 0;
        sscanf(line, "%15s", what);
        if (!strcmp(what, "join") && sscanf(line, "join %lf", &secs) == 1) {
            u64 end = deadline(secs);
            if (net_configured()) while (!joined && now_ms() < end) Sleep(20);
            if (joined) say("joined %ld %u %ld %d %ld", (long)proto, my_ip, (long)lobby_flags, fell_back, (long)members);
            else say("notjoined");
        } else if (!strcmp(what, "send") && sscanf(line, "send %u %u %u %3299s", &u1, &u2, &u3, hex) == 4) cmd_send(u1, u2, u3, hex);
        else if (!strcmp(what, "peers") && sscanf(line, "peers %lf %d", &secs, &a) == 2) cmd_peers(secs, a);
        else if (!strcmp(what, "open") && sscanf(line, "open %15s %d", who, &a) == 2) {
            nplayers = a;
            is_auto = !strcmp(who, "auto");
            me = is_auto ? -1 : atoi(who);
            int ok = rbl_open_relay(is_auto ? RBL_AUTO : me, nplayers);
            if (ok && me == 0) rbl_players(nplayers);
            say("open %d", ok);
        } else if (!strcmp(what, "online") && sscanf(line, "online %d %u", &a, &u1) == 2) {
            /* (SSBU_RB_NET=online: the link opened with no number; an Online match's set-up gives this game's player and
             * the other's address, as hle_rb_core.c's online_setup does from the game's records and net_recent_peers) */
            nplayers = 2; is_auto = 0;
            int ok = rbl_open_relay(RBL_ONLINE, nplayers), before = rbl_relay_number();
            u32 ips[8] = {0};
            ips[1 - a] = u1;
            ok = ok && before == -1 && rbl_relay_online(a, nplayers, ips);
            if (ok) { me = a; if (a == 0) rbl_players(nplayers); }
            say("online %d %d", ok, rbl_relay_number());
        } else if (!strcmp(what, "offline")) {
            rbl_relay_online_end();
            say("offline %d", rbl_relay_number());
        } else if (!strcmp(what, "note") && sscanf(line, "note %u", &u1) == 1) { net_note_peer(u1); say("ok"); }
        else if (!strcmp(what, "recent") && sscanf(line, "recent %u", &u1) == 1) {
            u32 r[8];
            int n = net_recent_peers(r, 8, u1);
            char t[160] = "";
            size_t k = 0;
            for (int i = 0; i < n; i++) k += (size_t)snprintf(t + k, sizeof t - k, " %u", r[i]);
            say("recent %d%s", n, t);
        } else if (!strcmp(what, "number") && sscanf(line, "number %lf", &secs) == 1) {
            u64 end = deadline(secs);
            int p = -1;
            while ((p = rbl_relay_number()) < 0 && now_ms() < end) Sleep(20);
            if (p >= 0) { me = p; if (p == 0) rbl_players(nplayers); }
            say("number %d", p);
        } else if (!strcmp(what, "hello") && sscanf(line, "hello %lf", &secs) == 1) cmd_hello(secs);
        else if (!strcmp(what, "pads") && sscanf(line, "pads %u %lf", &u1, &secs) == 2) cmd_pads(u1, secs);
        else if (!strcmp(what, "stats")) {
            NetRbCounts c;
            long long s, r, e;
            net_rb_counts(&c);
            rbl_stats(&s, &r, &e);
            say("stats %lld %lld %lld %lld %lld %lld %lld %lld", c.out_data, c.out_lobby, c.in_direct, c.in_relayed, c.in_lobby, s, r, rbl_dropped());
        } else if (!strcmp(what, "quit")) break;
        else say("error");
    }
    fflush(stdout);
    exit(0);
}
