/* A game's netplay client (runtime/net_relay.c) with no game around it, for tests/test_net_client_v3.py:
 * it joins the lobby or relay the environment names, as the game does (SSBU_NET_RELAY, SSBU_NET_HOST,
 * SSBU_NET_ROOM, SSBU_NET_NAME, SSBU_NET_PROTOCOL, ...: net.h), then does what each line of its input
 * says and answers it on its output. Made-up data only: the build id it says is NETV3TEST_BUILD's
 * (none without it), the datagrams are the test's.
 *
 *   Windows: built as tools/dev/netcfgtest.c is (tests/test_settings.py), with cc.CFLAGS
 *   Linux:   clang -O1 -I runtime/macos/include -I runtime/linux/include -I runtime tools/dev/netv3test.c runtime/linux/win32.c -lpthread -o build/netv3test
 *   macOS:   the same with runtime/macos/memory.c and runtime/macos/platform.c in place of -lpthread
 *
 * Commands (a line each; numbers decimal, addresses as numbers, bytes in hex), and their answers:
 *   join SECS             start the client; wait until joined: "joined VERSION ADDRESS FLAGS FELL_BACK MEMBERS"
 *                         (VERSION spoken, WELCOME's FLAGS, FELL_BACK 1 after a REJECT of version 2's), or
 *                         "notjoined"
 *   rooms SECS            the room browser: net_rooms_ask, then net_rooms_get until listed: "rooms STATE N",
 *                         then N lines "room HOST PLAYERS MOST OPEN MINE JOINABLE SIM NMODS|NAME|BUILD|MODS|WHY"
 *                         (a WHY of several lines on one, " / " between them)
 *   find, cancel          net_match_find, net_match_cancel: "ok"
 *   match STATE SECS      net_match_get until in that state (-1: at once): "match STATE ROLE OPEN PEER WAITED
 *                         PLAYERS SEARCHING CODE_PLAYERS |NAME"
 *   list SEQ SECS         the game's Rooms List (net_lobby_list): "list N HOST ..." when LIST_END came, or
 *                         "list -1" when it did not in time
 *   send DST SPORT DPORT HEX   a datagram from this game's address (net_send): "ok"
 *   recv                  "recv N", then N lines "SRC SPORT DST DPORT HEX": the datagrams that came since
 *   peers SECS N          wait until N players of the room are reached directly: "peers DIRECT PEERS", then a
 *                         line "peer ADDRESS STATE IN_DIRECT IN_RELAYED OUT_DIRECT OUT_RELAYED" each
 *   status                "status TEXT" (net_status, its lines on one)
 *   quit                  ends (net_relay.c's exit handler sends LEAVE)
 * Anything else: "error". */
#include "../../runtime/net_relay.c"

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#endif

/* ---- what net_relay.c reaches outside itself ------------------------------------------------- */
static char build_text[BUILD_LEN + 1];
const char *rt_data_dir = ".";
const RtModule rt_module_main = {.build_id = build_text};
int settings_online(void) { return 0; }   /* (no --online: the files are not read) */
int settings_line_read(const char *path, char *out, int size) { (void)path; (void)out; (void)size; return 0; }
int settings_line_write(const char *path, const char *line) { (void)path; (void)line; return 0; }
const char *config_path(enum ConfigFile file) { (void)file; return "none.txt"; }   /* (online_protocol.txt: none, as read above) */
#ifndef _WIN32
u64 rt_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (u64)t.tv_sec * 1000000000ull + (u64)t.tv_nsec; }
int rt_upnp_add(u16 port_, const char *lan, const char *what, char *pub, size_t n) { (void)port_; (void)lan; (void)what; (void)pub; (void)n; return 2; }
void rt_upnp_remove(void) {}
#endif

/* hle_net.c's side: the datagrams that come are kept for "recv"; the Rooms List's answers for "list".
 * This "game" is in every network the lobby puts it in (the lobby is the judge here). */
typedef struct Got { u32 src, dst; u16 sport, dport; u32 len; u8 data[1600]; } Got;
static SRWLOCK got_lock = SRWLOCK_INIT;
static Got got[64];
static int ngot;
static u32 listed[32], nlisted, list_seq;
static volatile LONG list_done;

void vsock_deliver(u32 src_ip, u16 src_port, u32 dst_ip, u16 dst_port, const void *data, u32 len) {
    AcquireSRWLockExclusive(&got_lock);
    if (ngot < (int)(sizeof got / sizeof *got) && len <= sizeof got[0].data) {
        Got *g = &got[ngot++];
        g->src = src_ip; g->dst = dst_ip; g->sport = src_port; g->dport = dst_port; g->len = len;
        memcpy(g->data, data, len);
    }
    ReleaseSRWLockExclusive(&got_lock);
}
int ldn_in_network(u32 ip) { (void)ip; return 1; }
void ldn_listing(u32 seq, u32 owner, const u8 *info) {
    (void)info;
    AcquireSRWLockExclusive(&got_lock);
    if (seq == list_seq && nlisted < sizeof listed / sizeof *listed) listed[nlisted++] = owner;
    ReleaseSRWLockExclusive(&got_lock);
}
void ldn_list_end(u32 seq) { if (seq == list_seq) InterlockedExchange(&list_done, 1); }

/* ---- the commands ----------------------------------------------------------------------------- */
static void say(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
    putchar('\n');
    fflush(stdout);
}

static u64 deadline(double secs) { return now_ms() + (u64)(secs * 1000); }

static void one_line(char *s) {   /* (newlines as " / ", and no '|' inside a field) */
    char out[600];
    size_t k = 0;
    for (const char *p = s; *p && k + 4 < sizeof out; p++) {
        if (*p == '\n') { memcpy(out + k, " / ", 3); k += 3; }
        else out[k++] = *p == '|' ? '/' : *p;
    }
    out[k] = 0;
    strcpy(s, out);
}

static void cmd_rooms(double secs) {
    static NetRoom r[ROOMS_MAX];
    int state = NET_ROOMS_OFF, n = 0;
    u64 end = deadline(secs);
    u32 before = *(volatile u32 *)&rooms_seq;   /* (the answer wanted: to a ROOMS sent after this) */
    net_rooms_ask();
    do {
        n = net_rooms_get(r, ROOMS_MAX, &state);
        if (state == NET_ROOMS_OFF || (state == NET_ROOMS_LISTED && *(volatile u32 *)&rooms_seq != before)) break;
        Sleep(20);
    } while (now_ms() < end);
    say("rooms %d %d", state, n);
    for (int i = 0; i < n; i++) {
        char mods[256], why[256];
        snprintf(mods, sizeof mods, "%s", r[i].mods);
        snprintf(why, sizeof why, "%s", r[i].why);
        one_line(mods);
        one_line(why);
        say("room %u %d %d %d %d %d %u %d|%s|%s|%s|%s", r[i].host, r[i].players, r[i].most, r[i].open, r[i].mine, r[i].joinable,
            (unsigned)r[i].sim, r[i].nmods, r[i].name, r[i].build, mods, why);
    }
}

static void cmd_match(int want, double secs) {
    NetMatch m;
    u64 end = deadline(secs);
    for (;;) {
        net_match_get(&m);
        if (want < 0 || m.state == want || now_ms() >= end) break;
        Sleep(20);
    }
    say("match %d %d %d %u %d %d %d %d |%s", m.state, m.role, m.room_open, m.peer, m.waited_s, m.players, m.searching, m.code_players, m.peer_name);
}

static void cmd_list(u32 seq, double secs) {
    AcquireSRWLockExclusive(&got_lock);
    list_seq = seq;
    nlisted = 0;
    ReleaseSRWLockExclusive(&got_lock);
    InterlockedExchange(&list_done, 0);
    net_lobby_list(seq);
    u64 end = deadline(secs);
    while (!list_done && now_ms() < end) Sleep(20);
    if (!list_done) { say("list -1"); return; }
    char out[400] = "list";
    AcquireSRWLockExclusive(&got_lock);
    size_t k = strlen(out);
    k += (size_t)snprintf(out + k, sizeof out - k, " %u", nlisted);
    for (u32 i = 0; i < nlisted && k < sizeof out; i++) k += (size_t)snprintf(out + k, sizeof out - k, " %u", listed[i]);
    ReleaseSRWLockExclusive(&got_lock);
    say("%s", out);
}

static void cmd_send(u32 dst, unsigned sport, unsigned dport, const char *hex) {
    u8 data[1600];
    u32 n = 0;
    for (unsigned b; n < sizeof data && sscanf(hex + 2 * n, "%2x", &b) == 1; n++) data[n] = (u8)b;
    net_send(net_local_ip(), (u16)sport, dst, (u16)dport, data, n);
    say("ok");
}

static void cmd_recv(void) {
    static Got copy[64];
    AcquireSRWLockExclusive(&got_lock);
    int n = ngot;
    memcpy(copy, got, sizeof copy[0] * (size_t)n);
    ngot = 0;
    ReleaseSRWLockExclusive(&got_lock);
    say("recv %d", n);
    for (int i = 0; i < n; i++) {
        char hex[2 * sizeof copy[0].data + 1];
        for (u32 k = 0; k < copy[i].len; k++) snprintf(hex + 2 * k, 3, "%02x", copy[i].data[k]);
        hex[2 * copy[i].len] = 0;
        say("%u %u %u %u %s", copy[i].src, copy[i].sport, copy[i].dst, copy[i].dport, hex);
    }
}

static void cmd_peers(double secs, int want) {
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

int main(void) {
    const char *b = getenv("NETV3TEST_BUILD");
    snprintf(build_text, sizeof build_text, "%s", b ? b : "");
    char line[4096];
    while (fgets(line, sizeof line, stdin)) {
        char what[16] = "", hex[3300] = "";
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
        } else if (!strcmp(what, "rooms") && sscanf(line, "rooms %lf", &secs) == 1) cmd_rooms(secs);
        else if (!strcmp(what, "find")) { net_match_find(); say("ok"); }
        else if (!strcmp(what, "cancel")) { net_match_cancel(); say("ok"); }
        else if (!strcmp(what, "match") && sscanf(line, "match %d %lf", &a, &secs) == 2) cmd_match(a, secs);
        else if (!strcmp(what, "list") && sscanf(line, "list %u %lf", &u1, &secs) == 2) cmd_list(u1, secs);
        else if (!strcmp(what, "send") && sscanf(line, "send %u %u %u %3299s", &u1, &u2, &u3, hex) == 4) cmd_send(u1, u2, u3, hex);
        else if (!strcmp(what, "recv")) cmd_recv();
        else if (!strcmp(what, "peers") && sscanf(line, "peers %lf %d", &secs, &a) == 2) cmd_peers(secs, a);
        else if (!strcmp(what, "status")) {
            char s[600];
            net_status(s, sizeof s);
            one_line(s);
            say("status %s", s);
        } else if (!strcmp(what, "quit")) break;
        else say("error");
    }
    fflush(stdout);
    exit(0);   /* (net_relay.c's exit handler: LEAVE) */
}
