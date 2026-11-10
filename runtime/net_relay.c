/* Netplay's relay (see net.h): the client every player's game runs, and the relay of a private
 * room, which the host player's game runs. tools/ssbu_relay.py is the online lobby: the same
 * protocol, plus room lists.
 *
 * Protocol 2, UDP, every field big-endian; each packet starts 'S' 'N' version type:
 *   1 JOIN     client: nonce u64, room[32], name[32], build[40]  (repeated until answered; build:
 *                      the game's main module build id - players of different versions never meet)
 *   2 WELCOME  relay:  nonce u64, address u32, cookie u64, flags u8 (1: a lobby), then the member
 *                      list as in MEMBERS (empty from a lobby)
 *   3 REJECT   relay:  nonce u64, reason u8, text[64]       (sent with the JOIN's version)
 *                      then, from a relay that knows the room's game: 'W' 'D', the room host's
 *                      build[40], sim u32, flags u8 (1: sim and mods known), mod set (flag 1) -
 *                      what differs, said by the refused game (refusal_detail); reasons 1 full,
 *                      2 another netplay version, 3 another game build, 6 another WindowSmash
 *                      (simulation version) or other mods. Older games read the text only.
 *   4 PING     client: nonce u64, time u64, cookie u64, ping u16   (every second; keeps NAT open)
 *   5 PONG     relay:  time u64, players u16, rooms u16, flags u8
 *   6 DATA     both:   source u32, destination u32, source port u16, destination port u16, payload
 *   7 MEMBERS  relay:  count u8, count x (address u32, name[32])
 *   8 LEAVE    client: nonce u64
 *   9 LIST     client: cookie u64, seq u32                 (a lobby's rooms, for nn::ldn's Scan)
 *  10 LISTING  lobby:  seq u32, host address u32, NetworkInfo[0x480]   (up to 24, best first)
 *  11 LIST_END lobby:  seq u32, count u8
 *  12 PEERS    lobby:  cookie u64, version u32, count u8, count x (address u32, name[32], flags u8,
 *                      public address u32, port u16, LAN address u32, port u16, secret u64)
 *                      (the other players of this player's room; flags 1: introduced, 2: that player
 *                      turned direct connections off, 4: its game cannot make them)
 *  13 BCAST    client: count u8, count x address u32, then as DATA   (a broadcast for the room's
 *                      players not listed: the ones this player reaches directly)
 *  14 PUNCH    player: secret u64, from u32, to u32, ack u8, nonce u64   (straight to another player)
 *  15 DIRECT   player: secret u64, then as DATA                         (the same)
 * A JOIN of version 2 from this game ends with 'W' 'S', its simulation version u32 and its mod set
 * (as version 3's, below): a private room's relay then refuses a game whose WindowSmash or mods
 * differ from the host's, saying which (REJECT's detail); relays and lobbies that don't know it
 * read the JOIN as before.
 * A game that can connect directly adds to its PINGs: peers version u32 (of the last PEERS it got),
 * LAN address u32, port u16, flags u8 (1: direct connections allowed), direct peers u8; a lobby
 * that introduces players says so with WELCOME flag 2. Older games and lobbies ignore both.
 * Routers that change ports (see "direct connections"): a game adds to its PINGs 'N' 'P' and the
 * outside port its router maps to its own (UPnP, NAT-PMP or PCP; 0 none); a lobby that knows them adds
 * to its PONGs (after version 3's counts, 4 bytes of zeros before them in version 2's) 'N' 'A', the
 * port it sees the game at, the port it saw the game's PROBE at (0 none yet) and its PROBE port (0
 * none), and to its PEERS, after the entries, 'N' '2' and each entry's PROBE port (0 unknown); its
 * PEERS give a player behind another router a LAN address of 0 and, as port, that player's own (or
 * mapped) port. Older games ignore all of these.
 *  30 PROBE    client: cookie u64   (to the lobby's PROBE port: a second port of the router's to compare)
 * A relay gives each member of a room an address in 10.13.0.0/16 and forwards DATA by
 * destination (the room's broadcast address: every other member), checking the source; a
 * lobby gives every player an address of its own and carries a Local Wireless network's
 * traffic between that network's players only. A member that is silent for 10 s is gone; a
 * JOIN with its nonce from a new place takes it back. Up to 8 members a relay's room.
 *
 * Direct connections: the lobby introduces the players of a room to each other, and their games
 * then send the room's traffic straight to each other where their routers let them (UDP hole
 * punching), through the lobby where not. See "direct connections" below.
 *
 * Protocol 3 (SSBU_NET_PROTOCOL=3 or online_protocol.txt, net.h), the online lobby's: version 2
 * and more, on the same port; tools/ssbu_relay.py's comment has every message. The client's JOIN
 * adds this game's simulation version (sim_version.h) and mod set; WELCOME's flags say what the
 * lobby has (4 the room browser, 8 matchmaking, 16 rollback); PONG adds two counts. Every packet
 * of the client is then of version 3, to the lobby and to the other players alike (two games of
 * version 3 talk in version 3: a lobby of version 3 puts players of version 2 and 3 in rooms of
 * their own). A lobby or relay of version 2 answers a JOIN of version 3 with REJECT reason 2: the
 * client then joins it in version 2, as a game of version 2 does. A private room's relay (below)
 * speaks version 2 only, so the host's own game joins it in version 2.
 *
 * Rollback netplay's messages (SSBU_RB_NET=relay, hle_rb_relay.c; net.h's NET_RB_PORT) are DATA (or
 * DIRECT) between the virtual port 7701 of two players, and, to a player of the lobby code outside this
 * player's room, a lobby of version 3's RB messages: "rollback messages" below. */
#define _CRT_SECURE_NO_WARNINGS
#define _CRT_RAND_S
#define COBJMACROS
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <natupnp.h>
#include <oleauto.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")   /* (GetBestRoute: the gateway, for PCP and NAT-PMP) */
#else
#include <sys/select.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <dirent.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define closesocket close
#endif
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config_folder.h"
#include "host.h"
#include "mod_diff.h"
#include "net.h"
#include "room_code.h"
#include "settings.h"
#include "sim_version.h"

#define MAGIC0 'S'
#define MAGIC1 'N'
enum { T_JOIN = 1, T_WELCOME, T_REJECT, T_PING, T_PONG, T_DATA, T_MEMBERS, T_LEAVE, T_LIST, T_LISTING, T_LIST_END,
       T_PEERS, T_BCAST, T_PUNCH, T_DIRECT };
#define T_PROBE 30
enum { W_LOBBY = 1, W_INTRODUCES = 2, W_ROOMS = 4, W_MATCH = 8, W_RB = 16 };   /* WELCOME's flags (4, 8, 16: version 3's) */
#define R_VERSION 2   /* REJECT's reason: another netplay version */
#define NAME_LEN 32
#define BUILD_LEN 40
#define JOIN_SIZE (4 + 8 + 2 * NAME_LEN + BUILD_LEN)
#define MODS_MAX 32                                  /* (a mod set's entries, at most; MOD_NAME: mod_diff.h) */
#define MOD_SET_MAX (1 + MODS_MAX * (5 + MOD_NAME))
#define REJECT_SIZE (4 + 8 + 1 + 64)
#define R_OTHER 6     /* REJECT's reason: another WindowSmash (simulation version) or other mods */
#define ROOM_MEMBERS 8
#define TIMEOUT_MS 10000
#define UNREACHABLE_MS 5000   /* unanswered this long: the game plays offline (and keeps asking) */
#define NETWORK_INFO 0x480
#define PING_EXT_SIZE (4 + 8 + 8 + 8 + 2 + 4 + 4 + 2 + 1 + 1)

/* (rt_module_main, host.h: its build id tells game versions apart) */

static u16 get16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }
static u32 get32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static u64 get64(const u8 *p) { return (u64)get32(p) << 32 | get32(p + 4); }
static void put16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void put32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
static void put64(u8 *p, u64 v) { put32(p, (u32)(v >> 32)); put32(p + 4, (u32)v); }
static u64 now_ms(void) { return GetTickCount64(); }
static volatile LONG proto = NET_PROTOCOL;   /* the version the client speaks now (configure; a REJECT of version 2's) */
static void header(u8 *p, int type) { p[0] = MAGIC0; p[1] = MAGIC1; p[2] = (u8)proto; p[3] = (u8)type; }
static void relay_header(u8 *p, int type) { header(p, type); p[2] = NET_PROTOCOL; }   /* (a private room's relay: version 2) */

/* Every packet goes out here. macOS refuses packets to the local network (EHOSTUNREACH, or
 * EPERM) until the user lets the game reach it - macOS asks the first time; System Settings >
 * Privacy & Security > Local Network changes it later: the first refusal is told, with where. */
#ifdef __APPLE__
static int local_network(u32 ip) {   /* (host order) private, link-local, multicast, broadcast */
    return (ip >> 24) == 10 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8 || (ip >> 16) == 0xA9FE || (ip >> 28) == 0xE || ip == 0xFFFFFFFFu;
}
#endif
static int test_nat;   /* (below: SSBU_NET_TEST_NAT) */
static SOCKET sim_out(SOCKET s, const struct sockaddr_in *to);
static void send_udp(SOCKET s, const void *p, int n, const struct sockaddr_in *to) {
    if (test_nat) s = sim_out(s, to);   /* (tests: through the router simulated below) */
    if (sendto(s, (const char *)p, n, 0, (const struct sockaddr *)to, sizeof *to) >= 0) return;
#ifdef __APPLE__
    static volatile LONG told;
    int err = errno;
    if ((err == EHOSTUNREACH || err == EPERM) && local_network(ntohl(to->sin_addr.s_addr)) && !InterlockedExchange(&told, 1)) {
        char ip[16];
        inet_ntop(AF_INET, &to->sin_addr, ip, sizeof ip);
        fprintf(stderr, "[net] macOS keeps the game off your local network (%s:%u: %s): allow WindowSmash in System Settings > "
                        "Privacy & Security > Local Network\n", ip, ntohs(to->sin_port), strerror(err));
    }
#endif
}

/* (POSIX) room in a socket's send buffer for the biggest datagram the game sends (net_send's
 * 65000 bytes and a header), as a bigger one is refused: macOS's buffer holds 9216 bytes to start
 * with (Linux's far more already; Windows' is left as it is). */
static void big_sends(SOCKET s) {
#ifndef _WIN32
    int size = 0;
    socklen_t len = sizeof size;
    if (getsockopt(s, SOL_SOCKET, SO_SNDBUF, &size, &len) || size < 65536) {
        size = 65536;
        setsockopt(s, SOL_SOCKET, SO_SNDBUF, &size, sizeof size);
    }
#else
    (void)s;
#endif
}

/* ---- configuration ------------------------------------------------------------------ */
static int configured = -1, hosting, logging;
static char relay_text[256], room[NAME_LEN + 1] = "smash", name[NAME_LEN + 1] = "Player";
static u16 port = NET_DEFAULT_PORT;
static int direct_ok = 1, test_nat;   /* SSBU_NET_DIRECT; SSBU_NET_TEST_NAT (tests) */
static int learn_ok = 1;              /* SSBU_NET_TEST_LEARN=0 (tests): as before, no address learnt from a hello */
static volatile LONG sim_upnp;        /* SSBU_NET_TEST_UPNP=1 (tests), or a port mapping made: the simulated router forwards the game's port */
static int proto_wanted = NET_PROTOCOL;   /* SSBU_NET_PROTOCOL: the version asked for first (proto: spoken now) */
static u64 test_block_at = ~0ull;     /* SSBU_NET_TEST_BLOCK (tests): no direct packets from then on */
static int lan_mode, lan_test, lan_announce = 1, room_named;   /* relay "lan"; SSBU_NET_LAN_TEST; SSBU_NET_LAN; a room asked for */
static u16 lan_port = NET_LAN_PORT;                            /* SSBU_NET_LAN_PORT (tests) */
static volatile LONG lan_state;                                /* NET_LAN_* (LAN discovery, below) */

#ifdef _WIN32
static u64 process_age_ms(void) {
    FILETIME created, x, y, z, now;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &x, &y, &z)) return 0;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a, b;
    a.LowPart = created.dwLowDateTime; a.HighPart = created.dwHighDateTime;
    b.LowPart = now.dwLowDateTime; b.HighPart = now.dwHighDateTime;
    return b.QuadPart > a.QuadPart ? (b.QuadPart - a.QuadPart) / 10000 : 0;
}
#else
/* (Linux: the runtime's own clock, which starts with the process) */
static u64 process_age_ms(void) { return rt_now_ns() / 1000000; }
#endif

/* Started with --online (Play SSBU.bat; settings.h): the lobby, name and direct connections from
 * the files the settings menu and the launcher write, where no SSBU_NET_* variable says them.
 * online_server.txt: "host:port [lobby code]" or "off"; online_name.txt: the name (one is picked
 * and kept there when it is missing); online_direct.txt: "off" for no direct connections. */
/* online_server.txt "auto": the public lobby this PC reaches fastest (a TCP connect to each, timed; their game
 * servers answer TCP on the same port). Lobbies are separate until federation (TODO): players on different ones
 * don't see each other, so "auto" is a choice in the settings, not the default. */
static const struct { const char *where, *host; } PUBLIC_LOBBIES[] = {
    {"North America", "smash.recomps.wtf:39421"},
    {"Europe", "opensmash-lobby.duckdns.org:39421"},
};
static double connect_ms(const char *hostport) {
    char host[256], *colon;
    snprintf(host, sizeof host, "%s", hostport);
    if (!(colon = strrchr(host, ':'))) return -1;
    *colon = 0;
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, colon + 1, &hints, &res) || !res) return -1;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    double ms = -1;
    if (s != INVALID_SOCKET) {
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        connect(s, res->ai_addr, (int)res->ai_addrlen);
        fd_set w, e;
        FD_ZERO(&w); FD_ZERO(&e); FD_SET(s, &w); FD_SET(s, &e);
        struct timeval tv = {1, 500000};
        if (select(0, NULL, &w, &e, &tv) == 1 && FD_ISSET(s, &w)) {
            QueryPerformanceCounter(&t1);
            ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
        }
        closesocket(s);
    }
    freeaddrinfo(res);
    return ms;
}
static void nearest_lobby(char *out, size_t n) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    double best = -1;
    char said[512] = "";
    size_t l = 0;
    for (size_t i = 0; i < sizeof PUBLIC_LOBBIES / sizeof *PUBLIC_LOBBIES; i++) {
        double ms = connect_ms(PUBLIC_LOBBIES[i].host);
        int w = ms >= 0 ? snprintf(said + l, sizeof said - l, "%s%s %.0f ms", l ? ", " : "", PUBLIC_LOBBIES[i].where, ms)
                        : snprintf(said + l, sizeof said - l, "%s%s no answer", l ? ", " : "", PUBLIC_LOBBIES[i].where);
        if (w > 0 && (size_t)w < sizeof said - l) l += (size_t)w;
        if (ms >= 0 && (best < 0 || ms < best)) { best = ms; snprintf(out, n, "%s", PUBLIC_LOBBIES[i].host); }
    }
    if (best < 0) snprintf(out, n, "%s", PUBLIC_LOBBIES[0].host);
    fprintf(stderr, "[net] the nearest public lobby: %s (%s)\n", out, said);
}

static void configure_from_files(void) {
    char line[256], host[256] = "", code[NAME_LEN + 1] = "";
    if (settings_line_read(config_path(CONFIG_ONLINE_SERVER), line, sizeof line)) sscanf(line, "%255s %32s", host, code);
    if (!host[0] || !_stricmp(host, "off")) return;
    if (!_stricmp(host, "auto")) nearest_lobby(host, sizeof host);
    snprintf(relay_text, sizeof relay_text, "%s", host);
    if (code[0] && !getenv("SSBU_NET_ROOM")) { snprintf(room, sizeof room, "%s", code); room_named = 1; }
    if (!getenv("SSBU_NET_NAME")) {
        if (!settings_line_read(config_path(CONFIG_ONLINE_NAME), line, sizeof line) || !line[0]) {
            snprintf(line, sizeof line, "Player%04u", 1000 + (unsigned)(GetTickCount64() * 2654435761u >> 7) % 9000);
            settings_line_write(config_path(CONFIG_ONLINE_NAME), line);
            fprintf(stderr, "[net] online as %s (change it in the settings: Esc)\n", line);
        }
        snprintf(name, sizeof name, "%s", line);
    }
    if (!getenv("SSBU_NET_DIRECT") && settings_line_read(config_path(CONFIG_ONLINE_DIRECT), line, sizeof line) && !_strnicmp(line, "off", 3)) direct_ok = 0;
}

int net_rollback_default(void) {
    static int v = -1;
    if (v >= 0) return v;
    char line[256], host[256] = "";
    const char *h = getenv("SSBU_NET_HOST"), *r = getenv("SSBU_NET_RELAY");
    v = 0;
    /* (off unless asked for: online_rollback.txt "on", the settings menu's Online > Rollback netcode. The game's
     * own netcode is the default until rollback passes two-PC tests; alpha.3's players found it unplayable) */
    if (!(settings_line_read(config_path(CONFIG_ONLINE_ROLLBACK), line, sizeof line) && !_strnicmp(line, "on", 2))) return v;
    /* (a private room started by Host/Join SSBU Netplay.bat: SSBU_NET_HOST or SSBU_NET_RELAY, no --online) */
    if (h && *h && strcmp(h, "0")) return v = 1;
    if (r && *r) return v = _stricmp(r, "off") != 0;
    if (!settings_online()) return v;
    if (settings_line_read(config_path(CONFIG_ONLINE_SERVER), line, sizeof line)) sscanf(line, "%255s", host);
    return v = host[0] && _stricmp(host, "off") != 0;
}

/* online_protocol.txt (the settings menu's Online > Netplay version): 3 or 2, the version to ask for
 * first; anything else, or no file: otherwise. Read with any lobby, as SSBU_NET_PROTOCOL is: the
 * macOS and Linux launchers name the lobby in SSBU_NET_RELAY. */
static int protocol_from_file(int otherwise) {
    char line[16];
    if (!settings_line_read(config_path(CONFIG_ONLINE_PROTOCOL), line, sizeof line)) return otherwise;
    int v = atoi(line);
    return v == NET_PROTOCOL || v == NET_PROTOCOL_V3 ? v : otherwise;
}

static void configure(void) {
    if (configured >= 0) return;
    const char *h = getenv("SSBU_NET_HOST"), *r = getenv("SSBU_NET_RELAY"), *v;
    hosting = h && *h && strcmp(h, "0");
    if ((v = getenv("SSBU_NET_PORT")) && atoi(v) > 0 && atoi(v) < 65536) port = (u16)atoi(v);
    if ((v = getenv("SSBU_NET_ROOM")) && *v) { snprintf(room, sizeof room, "%s", v); room_named = 1; }
    if ((v = getenv("SSBU_NET_NAME")) && *v) snprintf(name, sizeof name, "%s", v);
    if ((v = getenv("SSBU_NET_LOG"))) logging = atoi(v) > 0 ? atoi(v) : 1;
    if ((v = getenv("SSBU_NET_DIRECT")) && !strcmp(v, "0")) direct_ok = 0;
    /* (tests) SSBU_NET_TEST_NAT=1..4: a router of one kind between this game and the others (sim_out
     * below); SSBU_NET_TEST_BLOCK=<seconds after the game started>: drop every direct packet
     * from then on, as if a firewall closed the path (0: from the start) */
    if ((v = getenv("SSBU_NET_TEST_NAT"))) test_nat = atoi(v) > 0 && atoi(v) <= 4 ? atoi(v) : 0;
    if ((v = getenv("SSBU_NET_TEST_UPNP"))) sim_upnp = atoi(v) > 0;
    if ((v = getenv("SSBU_NET_TEST_LEARN"))) learn_ok = atoi(v) != 0;
    if ((v = getenv("SSBU_NET_TEST_BLOCK")) && *v) {
        u64 age = process_age_ms(), at = (u64)(atof(v) * 1000);
        test_block_at = now_ms() + (at > age ? at - age : 0);
    }
    /* LAN discovery (below): SSBU_NET_LAN=0, no beacons when hosting; (tests) SSBU_NET_LAN_PORT, the beacons' port, and
     * SSBU_NET_LAN_TEST=1, beacons to 127.0.0.1 only (unicast, where multicast loopback is not to be counted on) */
    if ((v = getenv("SSBU_NET_LAN")) && !strcmp(v, "0")) lan_announce = 0;
    if ((v = getenv("SSBU_NET_LAN_PORT")) && atoi(v) > 0 && atoi(v) < 65536) lan_port = (u16)atoi(v);
    if ((v = getenv("SSBU_NET_LAN_TEST"))) lan_test = atoi(v) > 0;
    if (hosting) snprintf(relay_text, sizeof relay_text, "127.0.0.1:%u", port);
    else if (r && *r) { if (_stricmp(r, "off")) snprintf(relay_text, sizeof relay_text, "%s", r); }
    else if (settings_online()) configure_from_files();
    lan_mode = !hosting && !_stricmp(relay_text, "lan");
    configured = hosting || relay_text[0];
    proto_wanted = NET_PROTOCOL_DEFAULT;   /* SSBU_NET_PROTOCOL=2|3, else online_protocol.txt's; hosting: its own relay's, 2 */
    if ((v = getenv("SSBU_NET_PROTOCOL")) && (atoi(v) == NET_PROTOCOL || atoi(v) == NET_PROTOCOL_V3)) proto_wanted = atoi(v);
    else if (configured && !hosting) proto_wanted = protocol_from_file(proto_wanted);
    if (hosting || lan_mode) proto_wanted = NET_PROTOCOL;   /* (a private room's relay: version 2) */
    proto = proto_wanted;
}

static void start(void);
int net_configured(void) { start(); return configured; }
int net_log(void) { configure(); return logging; }
const char *net_player_name(void) { configure(); return name; }
const char *net_relay_address(void) { configure(); return !hosting && !lan_mode && relay_text[0] ? relay_text : NULL; }

/* ---- the relay (host mode) ------------------------------------------------------------ */
typedef struct Member { struct sockaddr_in at; u64 nonce, seen; u32 ip; char name[NAME_LEN + 1]; } Member;
typedef struct Room {
    char name[NAME_LEN + 1];
    u8 build[BUILD_LEN];
    int tail;                 /* its host's JOIN had the tail (sim, mods): games with one are held to them */
    u32 sim;
    u8 mods[MOD_SET_MAX];
    int mods_len;
    Member members[ROOM_MEMBERS];
    int count;
} Room;

/* A mod set's length at p (n bytes there), or -1 when it is not one. */
static int mod_set_size(const u8 *p, int n) {
    if (n < 1 || p[0] > MODS_MAX) return -1;
    int o = 1;
    for (int i = 0; i < p[0]; i++) {
        if (o + 5 > n || p[o + 4] > MOD_NAME || o + 5 + p[o + 4] > n) return -1;
        o += 5 + p[o + 4];
    }
    return o;
}
static int u32_cmp(const void *a, const void *b) { u32 x = *(const u32 *)a, y = *(const u32 *)b; return x < y ? -1 : x > y; }
/* Whether two mod sets are the same mods: their hashes, in any order (the lobby's mods_key). */
static int mod_sets_same(const u8 *a, const u8 *b) {
    if (a[0] != b[0]) return 0;
    u32 ha[MODS_MAX], hb[MODS_MAX];
    int oa = 1, ob = 1;
    for (int i = 0; i < a[0]; i++) {
        ha[i] = get32(a + oa); oa += 5 + a[oa + 4];
        hb[i] = get32(b + ob); ob += 5 + b[ob + 4];
    }
    qsort(ha, a[0], sizeof *ha, u32_cmp);
    qsort(hb, b[0], sizeof *hb, u32_cmp);
    return !memcmp(ha, hb, sizeof *ha * a[0]);
}
/* A JOIN's tail of version 2 ('W' 'S', sim, mod set: send_join): the mod set's length, 0 when none. */
static int join_tail(const u8 *p, int n, u32 *sim, const u8 **mods) {
    if (n < JOIN_SIZE + 2 + 4 + 1 || p[JOIN_SIZE] != 'W' || p[JOIN_SIZE + 1] != 'S') return 0;
    int l = mod_set_size(p + JOIN_SIZE + 6, n - JOIN_SIZE - 6);
    if (l <= 0) return 0;
    *sim = get32(p + JOIN_SIZE + 2);
    *mods = p + JOIN_SIZE + 6;
    return l;
}
#define ROOMS 64
static Room rooms[ROOMS];
static SOCKET relay_sock = INVALID_SOCKET;

static int same_addr(const struct sockaddr_in *a, const struct sockaddr_in *b) {
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static int member_list(const Room *r, u8 *p) {
    p[0] = (u8)r->count;
    for (int i = 0; i < r->count; i++) {
        put32(p + 1 + i * (4 + NAME_LEN), r->members[i].ip);
        memcpy(p + 5 + i * (4 + NAME_LEN), r->members[i].name, NAME_LEN);
    }
    return 1 + r->count * (4 + NAME_LEN);
}

static void tell_members(const Room *r) {
    u8 p[4 + 1 + ROOM_MEMBERS * (4 + NAME_LEN)];
    relay_header(p, T_MEMBERS);
    int n = 4 + member_list(r, p + 4);
    for (int i = 0; i < r->count; i++) send_udp(relay_sock, p, n, &r->members[i].at);
}

static void drop_member(Room *r, int i) {
    if (logging) fprintf(stderr, "[net] relay: %s left room %s\n", r->members[i].name, r->name);
    r->members[i] = r->members[--r->count];
    tell_members(r);
}

static void relay_packet(const u8 *p, int n, const struct sockaddr_in *from) {
    if (n < 4 || p[0] != MAGIC0 || p[1] != MAGIC1) return;
    u8 out[4 + 8 + 4 + 8 + 1 + 1 + ROOM_MEMBERS * (4 + NAME_LEN)];
    if (p[2] != NET_PROTOCOL) {   /* another version of the game's netplay: say so */
        if (n >= 4 + 8 + 2 * NAME_LEN && p[3] == T_JOIN) {   /* (only to a whole JOIN: no small packet gets a bigger answer) */
            memset(out, 0, 4 + 8 + 1 + 64);
            relay_header(out, T_REJECT);
            memcpy(out + 4, p + 4, 8);
            out[12] = 2;
            snprintf((char *)out + 13, 64, "different netplay version (relay %d, you %d)", NET_PROTOCOL, p[2]);
            send_udp(relay_sock, out, 4 + 8 + 1 + 64, from);
        }
        return;
    }
    /* who sent it */
    Room *r = NULL;
    int m = -1;
    for (int i = 0; i < ROOMS && m < 0; i++)
        for (int j = 0; j < rooms[i].count; j++)
            if (same_addr(&rooms[i].members[j].at, from)) { r = &rooms[i]; m = j; break; }
    switch (p[3]) {
    case T_JOIN: {
        if (n < JOIN_SIZE) return;
        u64 nonce = get64(p + 4);
        const u8 *build = p + 12 + 2 * NAME_LEN;
        char want[NAME_LEN + 1] = {0}, who[NAME_LEN + 1] = {0};
        memcpy(want, p + 12, NAME_LEN);
        memcpy(who, p + 12 + NAME_LEN, NAME_LEN);
        if (m >= 0 && r->members[m].nonce != nonce) { drop_member(r, m); r = NULL; m = -1; }   /* (a new game from the same place) */
        if (m < 0) {   /* a member coming back from a new place, or a new one */
            for (int i = 0; i < ROOMS && m < 0; i++)
                for (int j = 0; j < rooms[i].count; j++)
                    if (rooms[i].members[j].nonce == nonce && !strcmp(rooms[i].name, want)) { r = &rooms[i]; m = j; r->members[j].at = *from; break; }
        }
        if (m < 0) {
            r = NULL;
            for (int i = 0; i < ROOMS && !r; i++) if (rooms[i].count && !strcmp(rooms[i].name, want)) r = &rooms[i];
            for (int i = 0; i < ROOMS && !r; i++) if (!rooms[i].count) { r = &rooms[i]; snprintf(r->name, sizeof r->name, "%s", want); }
            u32 jsim = 0;
            const u8 *jmods = NULL;
            int jlen = join_tail(p, n, &jsim, &jmods), other_build = r && r->count && memcmp(r->build, build, BUILD_LEN);
            int other_sim = r && r->count && r->tail && jlen && jsim != r->sim;
            int other_mods = r && r->count && r->tail && jlen && !other_sim && !mod_sets_same(r->mods, jmods);
            const char *why = !r ? "the relay is full" : r->count == ROOM_MEMBERS ? "the room is full (8 players)"
                            : other_build ? "the host has another game version" : other_sim ? "the host has another WindowSmash version"
                            : other_mods ? "the host runs other mods" : NULL;
            if (why) {
                u8 rej[REJECT_SIZE + 2 + BUILD_LEN + 5 + MOD_SET_MAX];
                int len = REJECT_SIZE;
                memset(rej, 0, REJECT_SIZE);
                relay_header(rej, T_REJECT);
                put64(rej + 4, nonce);
                rej[12] = !r || r->count == ROOM_MEMBERS ? 1 : other_build ? 3 : R_OTHER;
                snprintf((char *)rej + 13, 64, "%s", why);
                if (r && r->count && r->count < ROOM_MEMBERS) {   /* (the detail: the host's game, for the refused one to compare) */
                    rej[len] = 'W';
                    rej[len + 1] = 'D';
                    memcpy(rej + len + 2, r->build, BUILD_LEN);
                    put32(rej + len + 2 + BUILD_LEN, r->tail ? r->sim : 0);
                    rej[len + 2 + BUILD_LEN + 4] = r->tail ? 1 : 0;
                    len += 2 + BUILD_LEN + 5;
                    if (r->tail) { memcpy(rej + len, r->mods, (size_t)r->mods_len); len += r->mods_len; }
                }
                if (logging) fprintf(stderr, "[net] relay: refused %s in room %s: %s\n", who, want, why);
                send_udp(relay_sock, rej, len, from);
                return;
            }
            if (!r->count) {   /* the host (the first): its game's, which the others' are held to */
                memcpy(r->build, build, BUILD_LEN);
                r->tail = jlen > 0;
                r->sim = jsim;
                r->mods_len = jlen;
                if (jlen) memcpy(r->mods, jmods, (size_t)jlen);
            }
            u32 ip = 0;   /* the lowest free address from 10.13.0.2 */
            for (u32 cand = NET_GATEWAY + 1; !ip; cand++) {
                int used = 0;
                for (int j = 0; j < r->count; j++) used |= r->members[j].ip == cand;
                if (!used) ip = cand;
            }
            m = r->count++;
            memset(&r->members[m], 0, sizeof r->members[m]);
            r->members[m].at = *from;
            r->members[m].nonce = nonce;
            r->members[m].ip = ip;
            snprintf(r->members[m].name, sizeof r->members[m].name, "%s", who);
            if (logging) fprintf(stderr, "[net] relay: %s joined room %s as 10.13.%u.%u\n", who, r->name, ip >> 8 & 255, ip & 255);
            tell_members(r);
        }
        r->members[m].seen = now_ms();
        relay_header(out, T_WELCOME);
        put64(out + 4, nonce);
        put32(out + 12, r->members[m].ip);
        put64(out + 16, 0);   /* (cookie: a private room's relay checks none) */
        out[24] = 0;          /* (flags: not a lobby) */
        int len = 25 + member_list(r, out + 25);
        send_udp(relay_sock, out, len, from);
        return;
    }
    case T_PING:
        if (m < 0 || n < 20) return;
        r->members[m].seen = now_ms();
        relay_header(out, T_PONG);
        memcpy(out + 4, p + 12, 8);
        put16(out + 12, (u16)r->count);
        put16(out + 14, 0);
        out[16] = 0;
        send_udp(relay_sock, out, 17, from);
        return;
    case T_LEAVE:
        if (m >= 0) drop_member(r, m);
        return;
    case T_DATA: {
        if (m < 0 || n < 16) return;
        Member *src = &r->members[m];
        src->seen = now_ms();
        if (get32(p + 4) != src->ip) return;   /* (only from its own address) */
        u32 dst = get32(p + 8);
        for (int j = 0; j < r->count; j++)
            if (j != m && (dst == NET_BROADCAST || dst == 0xFFFFFFFFu || r->members[j].ip == dst))
                send_udp(relay_sock, p, n, &r->members[j].at);
        return;
    }
    }
}

/* ---- STUN: the relay's address outside the router, when UPnP opened nothing --------------
 * One Binding request (RFC 5389) from the relay's own socket to a public STUN server
 * (stun.l.google.com:19302; SSBU_ONLINE_STUN=<host:port>, or off) gives the address and port the
 * router shows for that socket (ssbu-recomp's net_relay.c). A router that keeps that mapping for
 * everyone (most home routers) then lets friends reach the room through its code; one that does
 * not (a "symmetric" NAT) does not, and the share file says it may not work. */
static volatile LONG stun_got;
static char stun_ip[64];
static u16 stun_port;
static u8 stun_id[12];

static int stun_answer(const u8 *p, int n) {   /* (relay thread) a Binding success for our request: 1 */
    if (n < 20 || p[0] != 0x01 || p[1] != 0x01 || p[4] != 0x21 || p[5] != 0x12 || p[6] != 0xA4 || p[7] != 0x42 || memcmp(p + 8, stun_id, 12)) return 0;
    int len = p[2] << 8 | p[3];
    for (int at = 20; at + 4 <= 20 + len && at + 4 <= n;) {
        int type = p[at] << 8 | p[at + 1], alen = p[at + 2] << 8 | p[at + 3];
        if ((type == 0x0020 || type == 0x0001) && alen >= 8 && at + 12 <= n && p[at + 5] == 1) {   /* (XOR-)MAPPED-ADDRESS, IPv4 */
            u16 port_ = (u16)(p[at + 6] << 8 | p[at + 7]);
            u32 ip = (u32)p[at + 8] << 24 | (u32)p[at + 9] << 16 | (u32)p[at + 10] << 8 | p[at + 11];
            if (type == 0x0020) { port_ ^= 0x2112; ip ^= 0x2112A442u; }
            snprintf(stun_ip, sizeof stun_ip, "%u.%u.%u.%u", ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255);
            stun_port = port_;
            InterlockedExchange(&stun_got, 1);
            return 1;
        }
        at += 4 + ((alen + 3) & ~3);
    }
    return 1;   /* (ours, without an address we read: dropped all the same) */
}

static int stun_query(void) {   /* (upnp_thread) 1: stun_ip / stun_port are the relay's outside address */
    const char *e = getenv("SSBU_ONLINE_STUN");
    char server[128];
    snprintf(server, sizeof server, "%s", e && *e ? e : "stun.l.google.com:19302");
    if (!_stricmp(server, "off") || !strcmp(server, "0") || relay_sock == INVALID_SOCKET) return 0;
    u16 sport = 3478;
    char *colon = strrchr(server, ':');
    if (colon) { *colon = 0; if (atoi(colon + 1) > 0) sport = (u16)atoi(colon + 1); }
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(server, NULL, &hints, &res) || !res) return 0;
    struct sockaddr_in to = *(struct sockaddr_in *)res->ai_addr;
    freeaddrinfo(res);
    to.sin_port = htons(sport);
    u8 req[20] = {0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42};
    for (int i = 0; i < 12; i++) { unsigned r; rand_s(&r); stun_id[i] = (u8)r; }
    memcpy(req + 8, stun_id, 12);
    for (int tries = 0; tries < 3 && !stun_got; tries++) {
        send_udp(relay_sock, req, sizeof req, &to);
        for (int w = 0; w < 50 && !stun_got; w++) Sleep(20);
    }
    return stun_got != 0;
}

static DWORD WINAPI relay_thread(LPVOID unused) {
    (void)unused;
    SetThreadDescription(GetCurrentThread(), L"netplay relay");
    static u8 buf[65536];
    u64 last_sweep = now_ms();
    for (;;) {
        struct sockaddr_in from;
        socklen_t flen = sizeof from;
        int n = recvfrom(relay_sock, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &flen);
        if (n >= 20 && stun_answer(buf, n)) continue;   /* (the STUN server's answer: upnp_thread asked) */
        if (n > 0) relay_packet(buf, n, &from);
        if (now_ms() - last_sweep >= 1000) {   /* members gone silent */
            last_sweep = now_ms();
            for (int i = 0; i < ROOMS; i++)
                for (int j = 0; j < rooms[i].count;)
                    if (now_ms() - rooms[i].members[j].seen > TIMEOUT_MS) drop_member(&rooms[i], j); else j++;
        }
    }
}

/* ---- UPnP: open the relay's port on the router ---------------------------------------- */
#ifdef _WIN32
static const CLSID CLSID_UPnPNAT_ = {0xAE1E00AA, 0x3FD5, 0x403C, {0x8A, 0x27, 0x2B, 0xBD, 0xC3, 0x0C, 0xD0, 0xE1}};
static const IID IID_IUPnPNAT_ = {0xB171C812, 0xCC76, 0x485A, {0x94, 0xD8, 0xB6, 0xB3, 0xA2, 0x79, 0x4E, 0x99}};
static IStaticPortMappingCollection *upnp_mappings;
#endif
static volatile LONG upnp_state;   /* 0 trying, 1 port open, 2 no UPnP router, 3 refused, 4 open but the router is behind another NAT, 5 not open but known through STUN */
static char lan_ip[64], public_ip[64];

static u32 outbound_address(void) {   /* the address this PC goes out on, host order (no packet is sent) */
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &a.sin_addr);
    socklen_t len = sizeof a;
    u32 ip = 0;
    if (s != INVALID_SOCKET && !connect(s, (struct sockaddr *)&a, sizeof a) && !getsockname(s, (struct sockaddr *)&a, &len))
        ip = ntohl(a.sin_addr.s_addr);
    if (s != INVALID_SOCKET) closesocket(s);
    return ip;
}

static void lan_address(void) {
    u32 ip = outbound_address();
    if (ip) snprintf(lan_ip, sizeof lan_ip, "%u.%u.%u.%u", ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255);
}

static u16 upnp_port;   /* the port mapped (one at most: a hosted room's relay, or the client's in a lobby) */
#ifdef _WIN32
static void upnp_close(void) {
    if (!upnp_mappings) return;
    BSTR udp = SysAllocString(L"UDP");
    IStaticPortMappingCollection_Remove(upnp_mappings, upnp_port, udp);
    SysFreeString(udp);
}
#else
/* (linux/upnp.c: the router's port mapping through SSDP and SOAP, as Windows' UPnP NAT API does) */
int rt_upnp_add(u16 port, const char *lan_ip, const char *what, char *public_ip, size_t n);
void rt_upnp_remove(void);
static void upnp_close(void) { rt_upnp_remove(); }
#endif

/* UDP port p of this PC (lan_ip) the same port outside, through the router's UPnP: 1 open (pub: the
 * router's outside address), 2 no UPnP router, 3 refused. Taken back at exit. */
static LONG upnp_add(u16 p, const char *what, char *pub, size_t n) {
#ifndef _WIN32
    LONG state = rt_upnp_add(p, lan_ip, what, pub, n);
    if (state == 1) { upnp_port = p; atexit(upnp_close); }
    return state;
#else
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IUPnPNAT *nat = NULL;
    LONG state = 2;
    if (SUCCEEDED(CoCreateInstance(&CLSID_UPnPNAT_, NULL, CLSCTX_ALL, &IID_IUPnPNAT_, (void **)&nat)) && nat) {
        IStaticPortMappingCollection *maps = NULL;
        if (SUCCEEDED(IUPnPNAT_get_StaticPortMappingCollection(nat, &maps)) && maps) {   /* (NULL: no UPnP router) */
            WCHAR client[64], desc[64];
            MultiByteToWideChar(CP_UTF8, 0, lan_ip, -1, client, 64);
            MultiByteToWideChar(CP_UTF8, 0, what, -1, desc, 64);
            BSTR udp = SysAllocString(L"UDP"), who = SysAllocString(client), named = SysAllocString(desc);
            IStaticPortMapping *map = NULL;
            IStaticPortMappingCollection_Remove(maps, p, udp);   /* (a stale one from an earlier run) */
            if (SUCCEEDED(IStaticPortMappingCollection_Add(maps, p, udp, p, who, VARIANT_TRUE, named, &map)) && map) {
                BSTR ext = NULL;
                if (SUCCEEDED(IStaticPortMapping_get_ExternalIPAddress(map, &ext)) && ext) {
                    WideCharToMultiByte(CP_UTF8, 0, ext, -1, pub, (int)n, NULL, NULL);
                    SysFreeString(ext);
                }
                IStaticPortMapping_Release(map);
                upnp_mappings = maps;
                upnp_port = p;
                atexit(upnp_close);
                state = 1;
            } else {
                state = 3;
                IStaticPortMappingCollection_Release(maps);
            }
            SysFreeString(udp); SysFreeString(who); SysFreeString(named);
        }
        IUPnPNAT_Release(nat);
    }
    return state;
#endif
}

/* Whether an address is one the internet cannot reach: private, or carrier-grade NAT's 100.64/10 -
 * a router reporting one of these as its own outside address sits behind another NAT. */
static int unreachable_address(const char *text) {
    unsigned a, b, cc, d;
    if (sscanf(text, "%u.%u.%u.%u", &a, &b, &cc, &d) != 4) return 0;
    return a == 10 || (a == 172 && (b & 0xF0) == 16) || (a == 192 && b == 168) || (a == 100 && (b & 0xC0) == 64) || a == 0;
}

static void write_share_file(void) {
    char path[600];
    snprintf(path, sizeof path, "%s/../../netplay_address.txt", rt_data_dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    unsigned a, b, cc, d;
    char code[12] = "", lan_code[12] = "";
    if (public_ip[0] && sscanf(public_ip, "%u.%u.%u.%u", &a, &b, &cc, &d) == 4) room_code_of(a << 24 | b << 16 | cc << 8 | d, port, code);
    if (sscanf(lan_ip, "%u.%u.%u.%u", &a, &b, &cc, &d) == 4) room_code_of(a << 24 | b << 16 | cc << 8 | d, port, lan_code);
    char stun_code[12] = "";
    if (upnp_state == 5 && sscanf(stun_ip, "%u.%u.%u.%u", &a, &b, &cc, &d) == 4) room_code_of(a << 24 | b << 16 | cc << 8 | d, stun_port, stun_code);
    if (upnp_state == 5 && stun_code[0]) {
        fprintf(f, "Your router did not open the port, but it shows the relay outside as %s:%u (STUN).\n"
                   "Give your friends this room code: %s. It works if your router keeps that mapping for everyone\n"
                   "(most home routers do); if friends can't join, open UDP port %u on the router or see docs/MULTIPLAYER.md.\n",
                stun_ip, stun_port, stun_code, port);
        fprintf(stderr, "[net] room code for friends (through STUN): %s\n", stun_code);
    } else if (upnp_state == 1 && code[0]) {
        fprintf(f, "Give your friends this room code to join: %s (or the address %s:%u)\n", code, public_ip, port);
        fprintf(stderr, "[net] room code for friends: %s (on this network: %s)\n", code, lan_code);
    } else if (upnp_state == 1) fprintf(f, "Give your friends this address to join: %s:%u\n", public_ip[0] ? public_ip : "(your public IP)", port);
    else if (upnp_state == 4) fprintf(f, "Your router opened the port, but its own internet address (%s) is not reachable from outside:\n"
                                         "it is behind another router or your provider's NAT. Friends on the same network can join %s:%u.\n"
                                         "For friends elsewhere, use a VPN or tools/ssbu_relay.py on a reachable machine (see docs/MULTIPLAYER.md).\n",
                                      public_ip, lan_ip, port);
    else fprintf(f, "Your router did not open the port (UPnP %s).\nFriends on the same network can join %s:%u.\n"
                    "For friends elsewhere, run tools/ssbu_relay.py on a reachable machine (see docs/MULTIPLAYER.md).\n",
                 upnp_state == 2 ? "not available" : "refused", lan_ip, port);
    if (lan_code[0]) fprintf(f, "Friends on the same network can also use the room code %s.\n", lan_code);
    if (lan_state == NET_LAN_ANNOUNCING)
        fprintf(f, "Friends on the same network can also just look for it: the settings menu's Online > Play online in > this network\n"
                   "(or SSBU_NET_RELAY=lan).\n");
    fprintf(f, "Room: %s\n", room);
    fclose(f);
}

static DWORD WINAPI upnp_thread(LPVOID unused) {
    (void)unused;
    lan_address();
    LONG state = upnp_add(port, "SSBU netplay relay", public_ip, sizeof public_ip);
    if (state == 1 && unreachable_address(public_ip)) state = 4;
    if (state != 1 && stun_query() && !unreachable_address(stun_ip)) {
        fprintf(stderr, "[net] STUN: the relay is %s:%u outside the router (port %u here)\n", stun_ip, stun_port, port);
        state = 5;
    }
    InterlockedExchange(&upnp_state, state);
    if (state == 1) fprintf(stderr, "[net] UPnP: port %u opened; friends join %s:%u (LAN address %s)\n", port, public_ip, port, lan_ip);
    else if (state == 4) fprintf(stderr, "[net] UPnP: port %u opened, but the router's outside address %s is behind another NAT; LAN address %s\n", port, public_ip, lan_ip);
    else if (state != 5) fprintf(stderr, "[net] UPnP: %s; LAN address %s, port %u\n", state == 2 ? "no UPnP router found" : "the router refused the port", lan_ip, port);
    write_share_file();
    return 0;
}

/* ---- the client ---------------------------------------------------------------------- */
static SOCKET client_sock = INVALID_SOCKET;
static struct sockaddr_in relay_at;
static volatile LONG joined;           /* WELCOME seen and not timed out */
static volatile LONG lobby;            /* ...from a lobby */
static volatile u32 my_ip;
static u64 nonce, cookie, last_pong, unjoined_since;
static volatile LONG members, rooms_open, ping_ms = -1;
static volatile LONG lobby_flags;                         /* the last WELCOME's (W_*) */
static volatile LONG key_searching = -1, code_players = -1;   /* version 3's PONG: players of this key searching; of the code */
static int fell_back;                                     /* a relay of version 2 said no to version 3: version 2 since */
static char reject_text[80], member_names[ROOM_MEMBERS][NAME_LEN + 1];
static char reject_detail[600];   /* what differs, from a REJECT's detail (refusal_detail): "" for none */
static u32 member_ips[ROOM_MEMBERS];      /* (a private room's: its MEMBERS, this player included; members_lock) */
static int member_count;
static SRWLOCK members_lock = SRWLOCK_INIT;

u32 net_local_ip(void) { start(); return joined ? my_ip : 0; }
int net_protocol_wanted(void) { configure(); return proto_wanted; }
int net_protocol_spoken(void) { start(); return joined ? (int)proto : 0; }
int net_unreachable(void) { start(); return configured && !joined && now_ms() - unjoined_since > UNREACHABLE_MS; }
int net_lobby(void) { start(); return joined && lobby; }

/* ---- rollback messages (net.h's NET_RB_PORT; SSBU_RB_NET=relay, hle_rb_relay.c) --------------
 * DATA between the virtual port 7701 of two players, as any game datagram: straight to a player of
 * this player's Local Wireless room while a direct path works (net_send), through the relay
 * otherwise. A private room's relay carries DATA between its members whatever they play; the lobby
 * only within a Local Wireless room, so to a player of the lobby code outside it (no room shared:
 * the lobby introduces nobody) the message goes as an RB of version 3 (cookie u64, destination u32,
 * message: the lobby carries it to a player of the same key and lobby code, or, destination 0 on a
 * private code, to all of them). What arrives goes to the sink, never to the game's sockets; with no
 * sink the port's datagrams are the game's as before, and RBs are dropped. (net_rb_send and
 * net_room_members are below net_send, which they use.) */
static volatile NetRbSink rb_sink;
static volatile LONG64 rb_out_data, rb_out_lobby, rb_in_direct, rb_in_relayed, rb_in_lobby;
void net_rb_sink(NetRbSink sink) { rb_sink = sink; }
static int rb_take(u32 src, u16 dport, const u8 *data, u32 len, volatile LONG64 *count) {   /* 1: the sink's */
    NetRbSink s = rb_sink;
    if (!s || dport != NET_RB_PORT) return 0;
    InterlockedIncrement64(count);
    if (len) s(src, data, len);
    return 1;
}
void net_rb_counts(NetRbCounts *out) {
    out->out_data = rb_out_data; out->out_lobby = rb_out_lobby;
    out->in_direct = rb_in_direct; out->in_relayed = rb_in_relayed; out->in_lobby = rb_in_lobby;
}

/* ---- direct connections (the online lobby's rooms) -------------------------------------
 * The lobby introduces the players of a room to each other (PEERS): each one's internet address
 * as the lobby sees it (and its address on its own network, when two are behind the same router)
 * and a secret of the pair's. Both then punch through their routers: PUNCH hellos to each address
 * the other may be at, every 200 ms for 5 s, each answered by an ack that echoes its nonce - an
 * ack proves the path works both ways. While acks keep coming (a hello every 400 ms) the room's
 * game traffic to that player goes straight there (DIRECT); otherwise through the lobby, as without
 * direct connections, with a hello now and then in case the path opens. A direct packet reaches
 * the game only with the pair's secret, the sender's own virtual address, ports of the game's
 * (never local wireless's, which always goes through the lobby, the judge of who is in which room)
 * and the sender a node of this player's network.
 *
 * Routers that change ports. The lobby sees a player at the port its router gives the lobby; a router
 * that gives every destination a port of its own (a "symmetric" NAT: some ISP routers, mobile networks,
 * carrier-grade NAT) sends the hellos to the other player from another port, which the lobby never
 * saw. So, besides the lobby's addresses, a player is tried at:
 *   - the address its hellos really come from (learnt from a hello with the pair's secret, the
 *     sender's and this player's virtual addresses: the other side then answers there - this alone
 *     joins a router that changes ports with one that lets in what comes from an address sent to);
 *   - its public address with its game's own port, or the port its router maps to it (PCP, NAT-PMP,
 *     UPnP: map_thread), which the lobby gives as a LAN address of 0;
 *   - where its router hands out ports in turn (the lobby saw its PROBE at another port, close to the
 *     first): the next GUESSES ports in that step, during the first punching only and GUESS_BUDGET
 *     hellos at most per player.
 * A path that still does not open goes through the lobby, and the log says why: which router changes
 * ports, and what turns it on (UPnP / NAT-PMP in the router). The lobby's relay then carries it: the
 * delay is this player -> the lobby -> the other, both legs. */
#define PEERS_MAX (ROOM_MEMBERS - 1)
#define PEER_SIZE (4 + NAME_LEN + 1 + 4 + 2 + 4 + 2 + 8)
#define PUNCH_SIZE (4 + 8 + 4 + 4 + 1 + 8)
#define DIRECT_HEAD (4 + 8)
#define PUNCH_MS 5000     /* punching this long without an answer: through the lobby */
#define USABLE_MS 1200    /* a direct path is used while its last ack is younger than this */
#define GUESSES 16        /* ports guessed when a router hands them out in turn: those between its two, then its next ones */
#define GUESS_SPAN 64     /* ...two ports this close (the lobby's and the PROBE's) say it does */
#define GUESS_BUDGET 160  /* hellos to guessed ports, at most, per player and punching */
#define CANDS (3 + 1 + GUESSES)
enum { P_PUNCHING, P_DIRECT, P_RELAYED };
enum { C_PUBLIC, C_LAN, C_PORT, C_LEARNT, C_GUESS };   /* where a candidate comes from */
typedef struct Candidate {   /* an address a player may be reached at */
    struct sockaddr_in at;
    int kind;
    u64 nonce, asked, prev, prev_asked;   /* our last two hellos there, and when they went */
} Candidate;
typedef struct Peer {
    u32 ip;
    char name[NAME_LEN + 1];
    int flags, introduced, state, via, ncand, nlob, acks, rounds, guessed, why, ever_direct, given_up;   /* via: the candidate that answers; nlob: the lobby's */
    u16 port2;                                        /* (the port the lobby saw its PROBE at; 0 unknown) */
    u64 secret, t0, last_ack, next_hello, acks_at, learnt_at, next_retry;
    LONG rtt;
    Candidate cand[CANDS];                            /* the lobby's, its guessed ports, then the one learnt */
    volatile LONG in_direct, in_relayed, out_direct, out_relayed, dropped;
} Peer;
static SRWLOCK peers_lock = SRWLOCK_INIT;   /* (never held while calling hle_net, or printing) */
static Peer peers[PEERS_MAX];
static int npeers;
static u32 peers_version;
static volatile LONG direct_not_in_room;    /* direct packets from a player not in this player's network */
static int bound_loopback;
static u32 local_ip;                        /* this player's own address and port (told to the lobby) */
static u16 local_port;
static volatile LONG seen_port, seen_probe, probe_port;   /* the lobby's PONG 'N' 'A': where it sees this game, and its PROBE port */
static volatile LONG map_state;   /* the client's port mapping: 0 not asked, 1 asking, 2 mapped, 3 none */
static volatile LONG map_port;    /* ...the router's outside port for it (2) */

/* How a router hands out ports: from the game's own port, the port the lobby sees and the one its
 * PROBE port saw. */
enum { NAT_UNKNOWN, NAT_KEEPS, NAT_ONE_PORT, NAT_IN_TURN, NAT_CHANGES };
static int nat_kind(u16 own, u16 seen, u16 second) {
    if (!seen) return NAT_UNKNOWN;
    if (second && second != seen) { int d = (int)second - (int)seen; return d >= -GUESS_SPAN && d <= GUESS_SPAN ? NAT_IN_TURN : NAT_CHANGES; }
    if (own && own == seen) return NAT_KEEPS;
    return second ? NAT_ONE_PORT : NAT_UNKNOWN;
}
static int my_nat(void) { return nat_kind(local_port, (u16)seen_port, (u16)seen_probe); }
static int changes_ports(int kind) { return kind == NAT_IN_TURN || kind == NAT_CHANGES; }
enum { WHY_NONE, WHY_MINE, WHY_THEIRS, WHY_BOTH, WHY_BLOCKED };
#define RETRY_MS 30000   /* relayed: its guessed ports once more this often (the path may open mid-match) */

/* How often this session ends up relayed: players introduced, reached directly (at some point), given
 * up on (by why: WHY_*) - said at exit and every 10 minutes with players, and in F1. */
static volatile LONG stat_introduced, stat_direct, stat_relayed[WHY_BLOCKED + 1];
static void stats_line(char *out, size_t n) {
    snprintf(out, n, "direct paths this session: %ld of %ld players introduced; relayed %ld (your router changes ports %ld, theirs %ld, both %ld, "
                     "blocked %ld)", (long)stat_direct, (long)stat_introduced,
             (long)(stat_relayed[WHY_MINE] + stat_relayed[WHY_THEIRS] + stat_relayed[WHY_BOTH] + stat_relayed[WHY_BLOCKED]),
             (long)stat_relayed[WHY_MINE], (long)stat_relayed[WHY_THEIRS], (long)stat_relayed[WHY_BOTH], (long)stat_relayed[WHY_BLOCKED]);
}

typedef struct Notes { int n; char line[2 * PEERS_MAX + 1][320]; } Notes;   /* (lines printed once the lock is let go) */
static void note(Notes *ns, const char *fmt, ...) {
    if (ns->n >= (int)(sizeof ns->line / sizeof ns->line[0])) return;
    va_list a;
    va_start(a, fmt);
    vsnprintf(ns->line[ns->n++], sizeof ns->line[0], fmt, a);
    va_end(a);
}
static void print_notes(const Notes *ns) { for (int i = 0; i < ns->n; i++) fprintf(stderr, "[net] %s\n", ns->line[i]); }

static const char *addr_text(const struct sockaddr_in *a, char *buf) {   /* (buf: 24 bytes) */
    char ip[16];
    inet_ntop(AF_INET, &a->sin_addr, ip, sizeof ip);
    snprintf(buf, 24, "%s:%u", ip, ntohs(a->sin_port));
    return buf;
}
static u64 random64(void) { unsigned int a, b; rand_s(&a); rand_s(&b); return (u64)a << 32 | b; }

static int usable(const Peer *q, u64 t) { return q->state == P_DIRECT && t - q->last_ack < USABLE_MS && t < test_block_at; }

static const char *peer_state(const Peer *q, u64 t, char *buf) {   /* (buf: 48 bytes) */
    if (usable(q, t)) { snprintf(buf, 48, "direct %ld ms", (long)q->rtt); return buf; }
    if (q->introduced && q->state == P_PUNCHING) return "relayed (trying direct)";
    if (q->introduced)
        return q->why == WHY_MINE ? "relayed (your router changes ports)" : q->why == WHY_THEIRS ? "relayed (their router changes ports)"
             : q->why == WHY_BOTH ? "relayed (both routers change ports)" : "relayed";
    return !direct_ok ? "relayed (direct off here)" : q->flags & 2 ? "relayed (direct off)" : q->flags & 4 ? "relayed (older game)" : "relayed";
}

/* Why no direct path formed with q (WHY_*), and the words for the log. */
static int why_relayed(const Peer *q, char *out, size_t n) {
    u16 own = 0;
    for (int c = 0; c < q->nlob; c++) if (q->cand[c].kind == C_PORT || q->cand[c].kind == C_LAN) own = ntohs(q->cand[c].at.sin_port);
    int mine = changes_ports(my_nat()) && map_state != 2, theirs = changes_ports(nat_kind(own, ntohs(q->cand[0].at.sin_port), q->port2));
    if (mine && theirs) {
        snprintf(out, n, "both routers change ports (symmetric NAT): UPnP or NAT-PMP on in either router lets it through");
        return WHY_BOTH;
    }
    if (mine) {
        snprintf(out, n, "your router changes ports (symmetric NAT, outside port %ld for the lobby, %ld for another): turn on UPnP or NAT-PMP "
                         "in your router (or forward a UDP port to this PC)", (long)seen_port, (long)seen_probe);
        return WHY_MINE;
    }
    if (theirs) {
        snprintf(out, n, "%s's router changes ports (symmetric NAT) and yours lets in only what it sent to: UPnP or NAT-PMP on in %s's router lets it through",
                 q->name, q->name);
        return WHY_THEIRS;
    }
    snprintf(out, n, "a router or firewall blocks it");
    return WHY_BLOCKED;
}

static void send_punch(Peer *q, int c, int ack, u64 word, u64 t) {   /* (word: a hello's nonce, or the one an ack echoes) */   /* (peers_lock held exclusively) */
    if (t >= test_block_at) return;
    u8 p[PUNCH_SIZE];
    header(p, T_PUNCH);
    put64(p + 4, q->secret);
    put32(p + 12, my_ip);
    put32(p + 16, q->ip);
    p[20] = (u8)ack;
    put64(p + 21, word);
    send_udp(client_sock, p, sizeof p, &q->cand[c].at);
}
static void hello(Peer *q, int c, u64 t) {
    Candidate *k = &q->cand[c];
    k->prev = k->nonce;
    k->prev_asked = k->asked;
    k->nonce = random64() | 1;
    k->asked = t;
    send_punch(q, c, 0, k->nonce, t);
}

/* (peers_lock held) The introduced player whose secret a direct packet carries, if it came from
 * one of that player's addresses (which one: *cand); else NULL. */
static Peer *peer_from(u64 secret, const struct sockaddr_in *from, int *cand) {
    for (int i = 0; i < npeers; i++) {
        Peer *q = &peers[i];
        if (!q->introduced || q->secret != secret) continue;
        for (int c = 0; c < q->ncand; c++)
            if (same_addr(&q->cand[c].at, from)) { *cand = c; return q; }
        return NULL;
    }
    return NULL;
}

/* (peers_lock held) A PUNCH of player ip with the pair's secret, from an address of none of its
 * candidates: its router gives this player another port than the lobby's. That address becomes (or
 * replaces) its learnt candidate - unless the direct path runs over the learnt one, or it was learnt
 * just now (no flapping). */
static Peer *peer_learn(u64 secret, u32 ip, const struct sockaddr_in *from, u64 t, int *cand, Notes *ns) {
    if (!learn_ok) return NULL;
    for (int i = 0; i < npeers; i++) {
        Peer *q = &peers[i];
        if (!q->introduced || q->secret != secret || q->ip != ip) continue;
        int c = q->ncand;
        for (int k = 0; k < q->ncand; k++) if (q->cand[k].kind == C_LEARNT) c = k;
        if (c < q->ncand && ((q->state == P_DIRECT && q->via == c) || t - q->learnt_at < 2000)) return NULL;
        if (c >= CANDS) return NULL;
        char at[24];
        memset(&q->cand[c], 0, sizeof q->cand[c]);
        q->cand[c].at = *from;
        q->cand[c].kind = C_LEARNT;
        if (c == q->ncand) q->ncand++;
        q->learnt_at = t;
        *cand = c;
        note(ns, "%s's hellos come from %s, not the address the lobby saw (its router changes ports): answering there", q->name, addr_text(from, at));
        return q;
    }
    return NULL;
}

static void clear_peers(void) {
    AcquireSRWLockExclusive(&peers_lock);
    npeers = 0;
    peers_version = 0;
    ReleaseSRWLockExclusive(&peers_lock);
}

static void add_cand(Peer *q, u32 ip, u16 port_, int kind) {   /* (host order) once, never a port of 0 */
    if (!ip || !port_ || q->ncand >= CANDS) return;
    for (int c = 0; c < q->ncand; c++)
        if (q->cand[c].at.sin_addr.s_addr == htonl(ip) && q->cand[c].at.sin_port == htons(port_)) return;
    Candidate *k = &q->cand[q->ncand++];
    memset(k, 0, sizeof *k);
    k->at.sin_family = AF_INET;
    k->at.sin_addr.s_addr = htonl(ip);
    k->at.sin_port = htons(port_);
    k->kind = kind;
}

/* A PEERS from the lobby: the room's players now. One already known (same addresses, same secret)
 * keeps its path; a new one, or one at a new address, is punched to. */
static void read_peers(const u8 *p, int n) {
    if (n < 5) return;
    u32 version = get32(p);
    int count = p[4];
    if ((i32)(version - peers_version) <= 0 || count > PEERS_MAX || 5 + count * PEER_SIZE > n) return;
    const u8 *tail = p + 5 + count * PEER_SIZE;   /* ('N' '2': each player's PROBE port) */
    int has2 = n >= 5 + count * PEER_SIZE + 2 + 2 * count && tail[0] == 'N' && tail[1] == '2';
    u64 t = now_ms();
    Peer next[PEERS_MAX];
    Notes ns = {0};
    memset(next, 0, sizeof next);
    AcquireSRWLockExclusive(&peers_lock);
    for (int i = 0; i < count; i++) {
        const u8 *e = p + 5 + i * PEER_SIZE, *a = e + 4 + NAME_LEN + 1;
        Peer *q = &next[i];
        q->ip = get32(e);
        for (int k = 0; k < NAME_LEN; k++) q->name[k] = e[4 + k] && e[4 + k] < 0x20 ? '?' : (char)e[4 + k];
        q->flags = e[4 + NAME_LEN];
        q->secret = get64(a + 12);
        q->introduced = (q->flags & 1) && q->secret && get32(a) && get16(a + 4) && direct_ok;
        if (q->introduced) {
            u32 pub = get32(a), lan = get32(a + 6);
            u16 pport = get16(a + 4), lport = get16(a + 10);
            q->port2 = has2 ? get16(tail + 2 + 2 * i) : 0;
            add_cand(q, pub, pport, C_PUBLIC);
            if (lan) add_cand(q, lan, lport, C_LAN);            /* (behind this player's router too) */
            else add_cand(q, pub, lport, C_PORT);               /* (its own port, or its router's mapping of it) */
            q->nlob = q->ncand;
            int d = (int)q->port2 - (int)pport, step = d < 0 ? -1 : 1;
            if (q->port2 && d && d >= -GUESS_SPAN && d <= GUESS_SPAN) {   /* (its router hands out ports in turn) */
                for (int g = (int)pport + step; g != (int)q->port2 && q->ncand < q->nlob + GUESSES / 4; g += step)
                    add_cand(q, pub, (u16)g, C_GUESS);   /* (its port for this player may have come between the two) */
                for (int g = 1; q->ncand < q->nlob + GUESSES; g++) {   /* (or after them: in the router's step) */
                    int guess = (int)q->port2 + d * g;
                    if (guess <= 0 || guess >= 65536) break;
                    add_cand(q, pub, (u16)guess, C_GUESS);
                }
            }
        }
        const Peer *old = NULL;
        for (int j = 0; j < npeers && !old; j++)
            if (peers[j].ip == q->ip && peers[j].secret == q->secret && peers[j].introduced == q->introduced) old = &peers[j];
        int same = old && old->nlob == q->nlob, same2 = same && old->port2 == q->port2;
        for (int c = 0; same && c < q->nlob; c++) same = same_addr(&old->cand[c].at, &q->cand[c].at);
        same2 &= same;
        if (old) {
            q->in_direct = old->in_direct; q->in_relayed = old->in_relayed; q->dropped = old->dropped;
            q->out_direct = old->out_direct; q->out_relayed = old->out_relayed; q->rtt = old->rtt;
        }
        if (same) {   /* (the same player at the same addresses: its path kept; a new PROBE port only changes the guesses) */
            q->state = old->state; q->via = old->via; q->t0 = old->t0; q->last_ack = old->last_ack;
            q->next_hello = old->next_hello; q->acks_at = old->acks_at; q->acks = old->acks;
            q->rounds = old->rounds; q->guessed = old->guessed; q->why = old->why; q->learnt_at = old->learnt_at;
            q->ever_direct = old->ever_direct; q->given_up = old->given_up; q->next_retry = old->next_retry;
            if (same2) {
                q->ncand = old->ncand;
                memcpy(q->cand, old->cand, sizeof q->cand);
            } else {
                memcpy(q->cand, old->cand, sizeof q->cand[0] * (size_t)q->nlob);   /* (its hellos' nonces kept) */
                for (int c = q->nlob; c < old->ncand; c++)                         /* (and the address learnt, if any) */
                    if (old->cand[c].kind == C_LEARNT && q->ncand < CANDS) q->cand[q->ncand++] = old->cand[c];
                if (q->state != P_DIRECT) {   /* (more to try: punching again, the guesses' budget anew) */
                    q->state = P_PUNCHING;
                    q->t0 = t;
                    q->guessed = 0;
                }
                if (q->ncand > q->nlob) note(&ns, "%s's router hands out ports in turn: trying its next ones too", q->name);
            }
        } else if (q->introduced) {
            char at[24];
            q->state = P_PUNCHING;
            q->t0 = q->next_hello = t;
            InterlockedIncrement(&stat_introduced);
            note(&ns, "%s: introduced, at %s%s%s; punching through", q->name, addr_text(&q->cand[0].at, at),
                 q->ncand > 1 && q->cand[1].kind == C_LAN ? " (or on this network)" : "",
                 q->ncand > q->nlob ? " (its router hands out ports in turn: its next ones too)" : "");
        } else q->state = P_RELAYED;
    }
    memcpy(peers, next, sizeof next);
    npeers = count;
    peers_version = version;
    ReleaseSRWLockExclusive(&peers_lock);
    print_notes(&ns);
}

/* (client thread, every 50 ms) Hellos due, paths gone quiet, punching given up. */
static void peers_tick(u64 t) {
    static u64 last_report;
    Notes ns = {0};
    AcquireSRWLockExclusive(&peers_lock);
    for (int i = 0; i < npeers; i++) {
        Peer *q = &peers[i];
        if (!q->introduced) continue;
        if (q->state == P_DIRECT && t - q->last_ack >= USABLE_MS) {
            q->state = P_RELAYED;
            q->t0 = t;   /* (and hellos every 400 ms for a while: it may come back) */
            note(&ns, "direct path to %s went quiet; using the relay", q->name);
        } else if (q->state == P_PUNCHING && t - q->t0 >= PUNCH_MS) {
            char why[240];
            q->state = P_RELAYED;
            q->why = why_relayed(q, why, sizeof why);
            q->next_retry = t + RETRY_MS;
            if (!q->ever_direct && !q->given_up) { q->given_up = 1; InterlockedIncrement(&stat_relayed[q->why]); }
            note(&ns, "no direct path to %s: %s; through the lobby's relay (you -> the lobby -> %s: a longer ping)", q->name, why, q->name);
            if (q->guessed) note(&ns, "(%d hellos to %s's guessed ports)", q->guessed, q->name);
        }
        if (t >= q->next_hello) {
            q->next_hello = t + (q->state == P_PUNCHING ? 200 : q->state == P_DIRECT || t - q->t0 < PUNCH_MS ? 400 : 5000);
            q->rounds++;
            int retry = q->state == P_RELAYED && t >= q->next_retry;
            if (q->state == P_DIRECT) hello(q, q->via, t);
            else
                for (int c = 0; c < q->ncand; c++) {
                    if (q->cand[c].kind == C_GUESS) {   /* (while punching, every other round, within the budget; relayed, a round now and then) */
                        if (!retry && (q->state != P_PUNCHING || (q->rounds & 1) || q->guessed >= GUESS_BUDGET)) continue;
                        q->guessed++;
                    }
                    hello(q, c, t);
                }
            if (retry) q->next_retry = t + RETRY_MS;
        }
    }
    if (logging && npeers && t - last_report >= 10000) {   /* (debugging: which way each player's traffic goes) */
        last_report = t;
        for (int i = 0; i < npeers; i++) {
            char st[48];
            const Peer *q = &peers[i];
            note(&ns, "peer %s: %s; in %ld direct + %ld relayed, out %ld direct + %ld relayed, %ld dropped", q->name,
                 peer_state(q, t, st), (long)q->in_direct, (long)q->in_relayed, (long)q->out_direct, (long)q->out_relayed, (long)q->dropped);
        }
    }
    ReleaseSRWLockExclusive(&peers_lock);
    print_notes(&ns);
}

/* A packet from another player, straight (not from the relay). */
static void direct_packet(const u8 *p, int n, const struct sockaddr_in *from) {
    u64 t = now_ms();
    int c = 0;
    if (t >= test_block_at || !lobby || n < DIRECT_HEAD) return;
    u64 secret = get64(p + 4);
    if (p[3] == T_DIRECT && n >= DIRECT_HEAD + 12) {
        const u8 *b = p + DIRECT_HEAD;
        u32 src = get32(b), dst = get32(b + 4);
        u16 sport = get16(b + 8), dport = get16(b + 10);
        int ok = 0;
        AcquireSRWLockShared(&peers_lock);
        Peer *q = peer_from(secret, from, &c);
        if (q) {
            ok = q->ip == src && (dst == my_ip || dst == NET_BROADCAST || dst == 0xFFFFFFFFu) &&
                 sport != NET_LDN_PORT && dport != NET_LDN_PORT;
            InterlockedIncrement(ok ? &q->in_direct : &q->dropped);
        }
        ReleaseSRWLockShared(&peers_lock);
        if (ok && rb_take(src, dport, b + 12, (u32)(n - DIRECT_HEAD - 12), &rb_in_direct)) return;   /* (rollback's: the pair's secret is the proof) */
        if (ok && ldn_in_network(src)) vsock_deliver(src, sport, dst, dport, b + 12, (u32)(n - DIRECT_HEAD - 12));
        else if (ok) InterlockedIncrement(&direct_not_in_room);
        return;
    }
    if (p[3] != T_PUNCH || n < PUNCH_SIZE) return;
    u32 from_ip = get32(p + 12), to_ip = get32(p + 16);
    int ack = p[20];
    u64 echo = get64(p + 21);
    Notes ns = {0};
    AcquireSRWLockExclusive(&peers_lock);
    Peer *q = peer_from(secret, from, &c);
    if (!q && !ack && to_ip == my_ip) q = peer_learn(secret, from_ip, from, t, &c, &ns);   /* (a hello from a port the lobby never saw) */
    if (q && q->ip == from_ip && to_ip == my_ip) {
        Candidate *k = &q->cand[c];
        if (ack) {   /* (an answer to one of our latest hellos there, from there: the path works both ways) */
            u64 asked = echo == k->nonce ? k->asked : echo == k->prev ? k->prev_asked : 0;
            if (echo && asked && t - asked < 2000) {
                if (echo == k->nonce) k->nonce = 0; else k->prev = 0;   /* (each answer once) */
                q->rtt = (LONG)(t - asked);
                q->last_ack = t;
                if (q->state != P_DIRECT || (k->kind == C_LAN && q->cand[q->via].kind != C_LAN)) {   /* (its own network's address rather than through the router) */
                    char at[24];
                    static const char *const kinds[] = {"public", "LAN", "its own port at", "learnt", "guessed"};
                    if (!q->ever_direct) {
                        q->ever_direct = 1;
                        InterlockedIncrement(&stat_direct);
                        if (q->given_up) InterlockedDecrement(&stat_relayed[q->why]);   /* (it opened later: direct after all) */
                    }
                    if (q->state == P_RELAYED) note(&ns, "%s: the direct path opened (mid-match): off the relay", q->name);
                    q->state = P_DIRECT;
                    q->via = c;
                    q->why = WHY_NONE;
                    note(&ns, "%s: direct, via %s %s (%ld ms)", q->name, kinds[k->kind], addr_text(&k->at, at), (long)q->rtt);
                }
            }
        } else {
            if (t - q->acks_at >= 1000) { q->acks_at = t; q->acks = 0; }
            if (q->acks++ < 10) {
                send_punch(q, c, 1, echo, t);
                if (q->state != P_DIRECT) hello(q, c, t);   /* (theirs got here: ours may get there now) */
            }
        }
    } else if (q) InterlockedIncrement(&q->dropped);
    ReleaseSRWLockExclusive(&peers_lock);
    print_notes(&ns);
}

static void count_relayed_in(u32 src) {
    AcquireSRWLockShared(&peers_lock);
    for (int i = 0; i < npeers; i++) if (peers[i].ip == src) InterlockedIncrement(&peers[i].in_relayed);
    ReleaseSRWLockShared(&peers_lock);
}

static void local_address(void) {   /* this player's own address and port, for players behind the same router */
    struct sockaddr_in a;
    socklen_t len = sizeof a;
    if (getsockname(client_sock, (struct sockaddr *)&a, &len) || !a.sin_port) return;
    local_ip = bound_loopback ? INADDR_LOOPBACK : outbound_address();
    local_port = ntohs(a.sin_port);
}

/* To one player (straight there while a direct path works, else through the lobby), or to the
 * room: straight to each player reached directly, through the lobby to the rest. */
void net_send(u32 src_ip, u16 src_port, u32 dst_ip, u16 dst_port, const void *data, u32 len) {
    if (!joined || client_sock == INVALID_SOCKET || len > 65000) return;
    enum { ROOM = 4 + 1 + 4 * PEERS_MAX };   /* (before the DATA body: room for the longest header, BCAST's) */
    u8 stack[1600], *buf = ROOM + 12 + len <= sizeof stack ? stack : malloc(ROOM + 12 + len);
    if (!buf) return;
    u8 *body = buf + ROOM, *h;
    put32(body, src_ip);
    put32(body + 4, dst_ip);
    put16(body + 8, src_port);
    put16(body + 10, dst_port);
    memcpy(body + 12, data, len);
    int n = 12 + (int)len, broadcast = dst_ip == NET_BROADCAST || dst_ip == 0xFFFFFFFFu, nd = 0, relayed = 0;
    struct { struct sockaddr_in at; u64 secret; u32 ip; } direct[PEERS_MAX];
    if (src_port != NET_LDN_PORT && dst_port != NET_LDN_PORT) {   /* (local wireless's own: always through the lobby) */
        u64 t = now_ms();
        AcquireSRWLockShared(&peers_lock);
        for (int i = 0; i < npeers; i++) {
            Peer *q = &peers[i];
            if (!broadcast && q->ip != dst_ip) continue;
            if (usable(q, t)) {
                direct[nd].at = q->cand[q->via].at;
                direct[nd].secret = q->secret;
                direct[nd++].ip = q->ip;
                InterlockedIncrement(&q->out_direct);
            } else {
                relayed++;
                InterlockedIncrement(&q->out_relayed);
            }
        }
        ReleaseSRWLockShared(&peers_lock);
    }
    for (int i = 0; i < nd; i++) {
        h = body - DIRECT_HEAD;
        header(h, T_DIRECT);
        put64(h + 4, direct[i].secret);
        send_udp(client_sock, h, n + DIRECT_HEAD, &direct[i].at);
    }
    if (!nd || (broadcast && relayed)) {
        if (nd) {   /* (a broadcast some players got directly: the lobby leaves those out) */
            h = body - (5 + 4 * nd);
            header(h, T_BCAST);
            h[4] = (u8)nd;
            for (int i = 0; i < nd; i++) put32(h + 5 + 4 * i, direct[i].ip);
        } else {
            h = body - 4;
            header(h, T_DATA);
        }
        send_udp(client_sock, h, (int)(body - h) + n, &relay_at);
    }
    if (buf != stack) free(buf);
}

/* Rollback's messages (above, "rollback messages"). */
static int is_room_peer(u32 ip) {   /* (the lobby: a player of this player's Local Wireless room) */
    int yes = 0;
    AcquireSRWLockShared(&peers_lock);
    for (int i = 0; i < npeers && !yes; i++) yes = peers[i].ip == ip;
    ReleaseSRWLockShared(&peers_lock);
    return yes;
}
static void rb_lobby_send(u32 dst, const void *data, u32 len);
static int lobby_has_rb(void);
void net_rb_send(u32 dst, const void *data, u32 len) {
    if (!joined || client_sock == INVALID_SOCKET || !len) return;
    u32 to = dst ? dst : NET_BROADCAST;
    if (!lobby) {   /* (a private room's relay: DATA to its members) */
        net_send(my_ip, NET_RB_PORT, to, NET_RB_PORT, data, len);
        InterlockedIncrement64(&rb_out_data);
        return;
    }
    int rb = lobby_has_rb(), in_room = dst ? is_room_peer(dst) : 1;
    if (in_room || !rb) {   /* (straight there, or the lobby's DATA within the room) */
        net_send(my_ip, NET_RB_PORT, to, NET_RB_PORT, data, len);
        InterlockedIncrement64(&rb_out_data);
    }
    if (rb && !(dst && in_room)) rb_lobby_send(dst, data, len);   /* (to every player of the code: the room's get it twice) */
}
static int u32_order(const void *a, const void *b) { u32 x = *(const u32 *)a, y = *(const u32 *)b; return x < y ? -1 : x > y; }

/* (net.h) The players the game's sockets sent to lately: the Online mode's match players. */
#define NOTED_MAX 8
#define NAT_CHECK_SECOND 0x0A0DFFFEu   /* (10.13.255.254: Pia's second NAT check server, the lobby's: hle_online.c) */
static struct { u32 ip; u64 at, order; } noted[NOTED_MAX];
static u64 noted_order;
static SRWLOCK noted_lock = SRWLOCK_INIT;
void net_note_peer(u32 ip) {
    if ((ip & NET_MASK) != NET_SUBNET || ip == NET_GATEWAY || ip == NAT_CHECK_SECOND || ip == NET_BROADCAST || ip == my_ip) return;
    u64 t = now_ms();
    AcquireSRWLockExclusive(&noted_lock);
    int k = 0;
    for (int i = 0; i < NOTED_MAX; i++) {
        if (noted[i].ip == ip) { k = i; break; }
        if (noted[i].at < noted[k].at) k = i;   /* (else the oldest makes room) */
    }
    noted[k].ip = ip; noted[k].at = t; noted[k].order = ++noted_order;
    ReleaseSRWLockExclusive(&noted_lock);
}
int net_recent_peers(u32 *out, int max, u32 within_ms) {
    u64 t = now_ms();
    int n = 0;
    AcquireSRWLockShared(&noted_lock);
    for (int i = 0; i < NOTED_MAX && n < max; i++)
        if (noted[i].ip && t - noted[i].at <= within_ms) out[n++] = noted[i].ip;
    u64 at[NOTED_MAX];
    for (int i = 0; i < n; i++) for (int j = 0; j < NOTED_MAX; j++) if (noted[j].ip == out[i]) at[i] = noted[j].order;
    ReleaseSRWLockShared(&noted_lock);
    for (int i = 1; i < n; i++)   /* (most recent first) */
        for (int j = i; j > 0 && at[j] > at[j - 1]; j--) { u32 x = out[j]; out[j] = out[j - 1]; out[j - 1] = x; u64 y = at[j]; at[j] = at[j - 1]; at[j - 1] = y; }
    return n;
}
int net_room_members(u32 *out, int max) {
    start();
    if (!joined || max <= 0) return 0;
    u32 all[ROOM_MEMBERS + PEERS_MAX];
    int n = 0;
    if (lobby) {
        all[n++] = my_ip;
        AcquireSRWLockShared(&peers_lock);
        for (int i = 0; i < npeers; i++) all[n++] = peers[i].ip;
        ReleaseSRWLockShared(&peers_lock);
    } else {
        AcquireSRWLockShared(&members_lock);
        for (int i = 0; i < member_count; i++) all[n++] = member_ips[i];
        ReleaseSRWLockShared(&members_lock);
    }
    qsort(all, (size_t)n, sizeof all[0], u32_order);
    if (n > max) n = max;
    memcpy(out, all, sizeof all[0] * (size_t)n);
    return n;
}

/* (ICMP "port unreachable" would otherwise make a UDP socket's next receive fail; Linux reports
 * that only for connected sockets) */
#ifdef _WIN32
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
static void no_connreset(SOCKET s) {
    BOOL off = FALSE;
    DWORD bytes = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &bytes, NULL, NULL);
}
#else
static void no_connreset(SOCKET s) { (void)s; }
#endif

static int resolve_relay(void) {
    char hostpart[256];
    snprintf(hostpart, sizeof hostpart, "%s", relay_text);
    u16 rport = port;
    char *colon = strrchr(hostpart, ':');
    if (colon) { *colon = 0; if (atoi(colon + 1) > 0) rport = (u16)atoi(colon + 1); }
    u32 code_ip;
    u16 code_port;
    if (room_code_read(relay_text, &code_ip, &code_port)) {   /* a room code (room_code.h): the host's address in it */
        memset(&relay_at, 0, sizeof relay_at);
        relay_at.sin_family = AF_INET;
        relay_at.sin_addr.s_addr = htonl(code_ip);
        relay_at.sin_port = htons(code_port);
    } else {
        struct addrinfo hints = {0}, *res = NULL;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(hostpart, NULL, &hints, &res) || !res) return 0;
        relay_at = *(struct sockaddr_in *)res->ai_addr;
        relay_at.sin_port = htons(rport);
        freeaddrinfo(res);
    }
    if (relay_at.sin_addr.s_addr == htonl(INADDR_LOOPBACK)) {   /* (the relay is here: listen on loopback only) */
        struct sockaddr_in a = {0};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(client_sock, (struct sockaddr *)&a, sizeof a);
        bound_loopback = 1;
    }
    return 1;
}

/* ---- (tests) a router in this process: SSBU_NET_TEST_NAT ---------------------------------
 * Every packet of the client's socket goes through it (send_udp, net_recv), the lobby's and the players':
 *   1  a "cone" router: one outside port for every destination (the game's own), and in only what comes
 *      from an address and port sent to in the last 30 s (port-restricted), as most home routers;
 *   2  a "symmetric" one: each destination gets an outside port of its own (here a socket of its own on
 *      a random port), which lets in that destination only - the lobby sees one port, a player another,
 *      as some ISP routers, mobile networks and carrier-grade NATs do;
 *   3  the same with consecutive ports (a sequential symmetric NAT: its next port can be guessed);
 *   4  a cone router that lets in anything from an address sent to (address-restricted: on this PC,
 *      every port of 127.0.0.1).
 * With 2 or 3 and a port mapping (SSBU_NET_TEST_UPNP=1, or one the game made: sim_upnp) the game's own
 * port is forwarded: anything sent to it comes in, and the answers to who sent there go out from it. */
#define SIM_MAPS 64
typedef struct SimMap { struct sockaddr_in to; SOCKET s; u64 sent; } SimMap;
static SimMap sim_maps[SIM_MAPS];
static int nsim_maps;
static u16 sim_next_port;
static SRWLOCK sim_lock = SRWLOCK_INIT;

static SimMap *sim_find(const struct sockaddr_in *to) {   /* (sim_lock held) */
    for (int i = 0; i < nsim_maps; i++) if (same_addr(&sim_maps[i].to, to)) return &sim_maps[i];
    return NULL;
}
static SOCKET sim_socket(void) {   /* (sim_lock held) a new outside port: random (2) or the next one (3) */
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return s;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(bound_loopback ? INADDR_LOOPBACK : INADDR_ANY);
    for (int tries = 0; tries < 200; tries++) {
        unsigned r;
        rand_s(&r);
        if (test_nat == 3) { if (!sim_next_port) sim_next_port = (u16)(20000 + r % 20000); a.sin_port = htons(sim_next_port++); }
        else a.sin_port = htons((u16)(20000 + r % 40000));   /* (anywhere: no guess finds it) */
        if (!bind(s, (struct sockaddr *)&a, sizeof a)) { no_connreset(s); return s; }
    }
    closesocket(s);
    return INVALID_SOCKET;
}
static SOCKET sim_out(SOCKET s, const struct sockaddr_in *to) {
    if (s != client_sock || client_sock == INVALID_SOCKET) return s;
    AcquireSRWLockExclusive(&sim_lock);
    SimMap *m = sim_find(to);
    if (!m && nsim_maps < SIM_MAPS) {
        m = &sim_maps[nsim_maps];
        m->to = *to;
        m->s = test_nat == 2 || test_nat == 3 ? sim_socket() : client_sock;
        if (m->s == INVALID_SOCKET) m = NULL; else nsim_maps++;
    }
    if (m) { m->sent = now_ms(); s = m->s; }
    ReleaseSRWLockExclusive(&sim_lock);
    return s;
}
static int sim_lets_in(SOCKET on, const struct sockaddr_in *from) {   /* 1: the simulated router lets this in */
    int ok = 0;
    u64 t = now_ms();
    AcquireSRWLockExclusive(&sim_lock);
    if (test_nat == 1 || test_nat == 4) {
        for (int i = 0; i < nsim_maps && !ok; i++)
            ok = t - sim_maps[i].sent < 30000 && (test_nat == 4 ? sim_maps[i].to.sin_addr.s_addr == from->sin_addr.s_addr : same_addr(&sim_maps[i].to, from));
    } else if (on == client_sock) {   /* (the game's own port: open only when forwarded; answers then go out from it) */
        if ((ok = sim_upnp != 0)) {
            SimMap *m = sim_find(from);
            if (!m && nsim_maps < SIM_MAPS) { m = &sim_maps[nsim_maps++]; m->to = *from; }
            if (m) { m->s = client_sock; m->sent = t; }
        }
    } else {
        for (int i = 0; i < nsim_maps && !ok; i++) ok = sim_maps[i].s == on && same_addr(&sim_maps[i].to, from);
    }
    ReleaseSRWLockExclusive(&sim_lock);
    return ok;
}

/* The client's next packet (0: none in 100 ms, or one the simulated router keeps out). */
static int net_recv(u8 *buf, int size, struct sockaddr_in *from) {
    socklen_t flen = sizeof *from;
    if (test_nat == 2 || test_nat == 3) {
        SOCKET socks[SIM_MAPS + 1];
        int ns = 0;
        socks[ns++] = client_sock;
        AcquireSRWLockShared(&sim_lock);
        for (int i = 0; i < nsim_maps; i++) if (sim_maps[i].s != client_sock && sim_maps[i].s != INVALID_SOCKET) socks[ns++] = sim_maps[i].s;
        ReleaseSRWLockShared(&sim_lock);
        fd_set set;
        FD_ZERO(&set);
        SOCKET top = 0;
        for (int i = 0; i < ns; i++) { FD_SET(socks[i], &set); if (socks[i] > top) top = socks[i]; }
        struct timeval tv = {0, 100000};
        if (select((int)top + 1, &set, NULL, NULL, &tv) <= 0) return 0;
        for (int i = 0; i < ns; i++) {
            if (!FD_ISSET(socks[i], &set)) continue;
            int n = recvfrom(socks[i], (char *)buf, size, 0, (struct sockaddr *)from, &flen);
            return n > 0 && sim_lets_in(socks[i], from) ? n : 0;
        }
        return 0;
    }
    int n = recvfrom(client_sock, (char *)buf, size, 0, (struct sockaddr *)from, &flen);
    if (n > 0 && test_nat && !sim_lets_in(client_sock, from)) return 0;
    return n;
}

/* ---- the router's own mapping of the client's port (lobby mode) --------------------------
 * A router that changes ports (above, test 2) keeps direct paths shut unless it forwards the game's
 * port: asked for here once the game is in a lobby - PCP, then NAT-PMP (RFC 6887, 6886: one UDP
 * question to the gateway's port 5351), then UPnP (the hosting code's upnp_add) - for the port the
 * client's socket has, the outside port the same if the router agrees. The outside port goes to the
 * lobby in the PINGs ('N' 'P'), which tells the other players; PCP's and NAT-PMP's mapping is renewed
 * while the game runs, and every mapping is taken back at exit. SSBU_NET_UPNP=0: the router is left
 * alone; a lobby on this PC or this network needs none. (tests) SSBU_NET_TEST_GATEWAY=<ip:port>: the
 * PCP / NAT-PMP gateway, and no UPnP. */
static char map_how[8];
static struct sockaddr_in map_gw;
static int map_pcp;               /* (the gateway speaks PCP) */
static u8 map_nonce[12];

static u32 gateway_address(void) {   /* host order; 0 unknown */
    u32 gw = 0;
#ifdef _WIN32
    MIB_IPFORWARDROW row;
    if (GetBestRoute(htonl(0x08080808u), 0, &row) == NO_ERROR) gw = ntohl(row.dwForwardNextHop);
#elif defined(__linux__)
    FILE *f = fopen("/proc/net/route", "r");
    char line[256], ifname[64];
    unsigned dest, g, flags;
    if (f) {
        while (!gw && fgets(line, sizeof line, f))
            if (sscanf(line, "%63s %x %x %x", ifname, &dest, &g, &flags) == 4 && dest == 0 && (flags & 2)) gw = ntohl(g);
        fclose(f);
    }
#endif
    if (!gw) { u32 me = outbound_address(); if (me) gw = (me & 0xFFFFFF00u) | 1; }   /* (most home routers: .1 of the PC's network) */
    return gw;
}

/* One PCP MAP or NAT-PMP request for UDP port lp (life 0: take it back): the answer's outside port, or 0. */
static u16 pmp_ask(SOCKET s, u16 lp, u16 want, u32 life, int pcp) {
    u8 q[60], a[1100];
    int n;
    memset(q, 0, sizeof q);
    if (pcp) {   /* (RFC 6887: the header, then MAP's: nonce, protocol 17, internal port, the outside port and address wanted) */
        u32 me = outbound_address();
        q[0] = 2; q[1] = 1;
        put32(q + 4, life);
        q[18] = q[19] = 0xFF;
        put32(q + 20, me);
        memcpy(q + 24, map_nonce, 12);
        q[36] = 17;
        put16(q + 40, lp);
        put16(q + 42, want);
        q[54] = q[55] = 0xFF;
        n = 60;
    } else {     /* (RFC 6886: version 0, opcode 1 UDP, internal port, outside port wanted, lifetime) */
        q[1] = 1;
        put16(q + 4, lp);
        put16(q + 6, want);
        put32(q + 8, life);
        n = 12;
    }
    for (int tries = 0, wait = 250; tries < 3; tries++, wait *= 2) {
        sendto(s, (const char *)q, n, 0, (const struct sockaddr *)&map_gw, sizeof map_gw);
        if (!life) return 0;
        u64 end = now_ms() + (u64)wait;
        while (now_ms() < end) {
            struct sockaddr_in from;
            socklen_t flen = sizeof from;
            int got = recvfrom(s, (char *)a, sizeof a, 0, (struct sockaddr *)&from, &flen);
            if (got <= 0 || from.sin_addr.s_addr != map_gw.sin_addr.s_addr) continue;
            if (pcp && got >= 60 && a[0] == 2 && a[1] == 0x81 && !memcmp(a + 24, map_nonce, 12))
                return a[3] == 0 ? get16(a + 42) : 0;
            if (pcp && got >= 2 && a[0] == 0) return 0;   /* (a NAT-PMP gateway: PCP unsupported) */
            if (!pcp && got >= 16 && a[0] == 0 && a[1] == 129 && get16(a + 2) == 0 && get16(a + 8) == lp) return get16(a + 10);
            if (!pcp && got >= 4 && a[0] == 0 && a[1] == 129) return 0;   /* (refused) */
        }
    }
    return 0;
}
static void map_close(void) {
    if (map_state != 2) return;
    if (!strcmp(map_how, "UPnP")) { upnp_close(); return; }
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return;
    pmp_ask(s, local_port, 0, 0, map_pcp);
    closesocket(s);
}
static DWORD WINAPI map_thread(LPVOID unused) {
    (void)unused;
    SetThreadDescription(GetCurrentThread(), L"netplay port mapping");
    const char *test_gw = getenv("SSBU_NET_TEST_GATEWAY");
    int testing = test_gw && *test_gw;
    u16 lp = local_port, ext = 0;
    memset(&map_gw, 0, sizeof map_gw);
    map_gw.sin_family = AF_INET;
    map_gw.sin_port = htons(5351);
    if (testing) {
        char ip[64];
        snprintf(ip, sizeof ip, "%s", test_gw);
        char *colon = strrchr(ip, ':');
        if (colon) { *colon = 0; map_gw.sin_port = htons((u16)atoi(colon + 1)); }
        inet_pton(AF_INET, ip, &map_gw.sin_addr);
    } else map_gw.sin_addr.s_addr = htonl(gateway_address());
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
    DWORD timeout = 100;
#else
    struct timeval timeout = {0, 100000};
#endif
    if (s != INVALID_SOCKET) {
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
        no_connreset(s);
        for (int i = 0; i < 12; i++) { unsigned r; rand_s(&r); map_nonce[i] = (u8)r; }
        if (map_gw.sin_addr.s_addr && (ext = pmp_ask(s, lp, lp, 7200, 1))) { map_pcp = 1; snprintf(map_how, sizeof map_how, "PCP"); }
        else if (map_gw.sin_addr.s_addr && (ext = pmp_ask(s, lp, lp, 7200, 0))) snprintf(map_how, sizeof map_how, "NAT-PMP");
    }
    if (!ext && !testing) {   /* (UPnP: as for a hosted room) */
        char pub[64] = "";
        lan_address();
        if (upnp_add(lp, "SSBU netplay (online lobby)", pub, sizeof pub) == 1) { ext = lp; snprintf(map_how, sizeof map_how, "UPnP"); }
    }
    if (ext) {
        InterlockedExchange(&map_port, ext);
        InterlockedExchange(&map_state, 2);
        InterlockedExchange(&sim_upnp, 1);   /* (tests: the simulated router forwards the port now) */
        atexit(map_close);
        fprintf(stderr, "[net] %s: the router forwards outside UDP port %u to this game's port %u: direct paths even where it changes ports\n",
                map_how, ext, lp);
    } else {
        InterlockedExchange(&map_state, 3);
        fprintf(stderr, "[net] port mapping: the router answered neither PCP, NAT-PMP nor UPnP (off in the router?): direct paths only where it keeps ports\n");
    }
    while (ext && map_how[0] != 'U' && s != INVALID_SOCKET) {   /* (PCP / NAT-PMP: renewed at half its life) */
        Sleep(3600 * 1000);
        u16 again = pmp_ask(s, lp, ext, 7200, map_pcp);
        if (again && again != ext) { InterlockedExchange(&map_port, again); ext = again; }
    }
    if (s != INVALID_SOCKET) closesocket(s);
    return 0;
}
static void map_start(void) {   /* (client thread: joined a lobby, local_port known) */
    if (map_state || !local_port) return;
    const char *u = getenv("SSBU_NET_UPNP"), *gw = getenv("SSBU_NET_TEST_GATEWAY");
    u32 lobby_ip = ntohl(relay_at.sin_addr.s_addr);
    int testing = gw && *gw;
    int nearby = bound_loopback || (lobby_ip >> 24) == 127 || (lobby_ip >> 24) == 10 || (lobby_ip >> 20) == 0xAC1 || (lobby_ip >> 16) == 0xC0A8;
    if (!direct_ok || (!testing && ((u && !strcmp(u, "0")) || nearby))) { map_state = 3; return; }
    map_state = 1;
    CloseHandle(CreateThread(NULL, 0, map_thread, NULL, 0, NULL));
}

/* Version 3's JOIN ends with this game's mod set: count u8 (32 at most), count x (hash u32, name
 * length u8 (31 at most), name). The lobby parts players by the hashes (in any order) and shows the
 * names. A mod's hash must come out the same on every OS, or players of one mod on Windows, Linux
 * and macOS never meet: ssbu-recomp's rule (its runtime/net_relay.c tree_hash: over each file of the
 * mod, FNV-1a 64 of its path in lower case with '/' and then of its size, summed and folded to 32
 * bits), over the files that are the same on every OS only - never a mod's native code (a .dll, .so
 * or .dylib), which is built for each, nor a mod's notes for the mod manager (info.toml, its preview
 * and readme), which the game never reads.
 *
 * The mods are tools/mods.py's: the turned-on ones of the last apply (dump/mods/applied.json,
 * "mods"), in the port's mods/ folder - apply makes them the game's data (romfs.cfg), so two games
 * with different mods applied are parted (their matches would not stay the same). All of them, by
 * name (case not counted), whatever order a folder listing has: a skin pack has hundreds, and
 * ssbu-recomp's first 32 in the listing's order gave two copies of one pack different sets (the order
 * differs between filesystems and machines). More than 32: the first 31, then "and <n> more", hashed
 * over the rest's names and hashes. None applied: none, as before. */
static u8 mod_set[MOD_SET_MAX] = {0};   /* (count 0: none) */
static int mod_set_len = 1;
#define JOIN_V3_MAX (JOIN_SIZE + 4 + (int)sizeof mod_set)

#define FNV0 0xcbf29ce484222325ull
static u64 fnv64(u64 h, const void *p, size_t n) {
    const u8 *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}
static int starts_ci(const char *s, const char *prefix) {   /* (case not counted) */
    for (; *prefix; s++, prefix++) if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return 0;
    return 1;
}
static int mod_file_hashed(const char *name) {
    static const char *const native[] = {".dll", ".so", ".dylib"};
    size_t n = strlen(name);
    if (name[0] == '.' || !_stricmp(name, "info.toml") || starts_ci(name, "preview.") || starts_ci(name, "readme.")) return 0;
    for (size_t k = 0; k < sizeof native / sizeof *native; k++) {
        size_t l = strlen(native[k]);
        if (n > l && !_stricmp(name + n - l, native[k])) return 0;
    }
    return 1;
}
/* The sum over the files below dir (rel: their path in the mod, '/' between) of FNV-1a 64 of the path
 * in lower case, then of the size (8 bytes, little-endian): ssbu-recomp's tree_hash. */
static u64 mod_tree_hash(const char *dir, const char *rel, int depth, int *files) {
    u64 sum = 0;
    char sub[1024], r[1024];
#ifdef _WIN32
    char pat[1024];
    WIN32_FIND_DATAA fd;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        const char *nm = fd.cFileName;
        int isdir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        u64 size = (u64)fd.nFileSizeHigh << 32 | fd.nFileSizeLow;
        snprintf(sub, sizeof sub, "%s/%s", dir, nm);
#else
    DIR *dp = opendir(dir);
    if (!dp) return 0;
    for (struct dirent *de; (de = readdir(dp));) {
        const char *nm = de->d_name;
        struct stat st;
        snprintf(sub, sizeof sub, "%s/%s", dir, nm);
        if (stat(sub, &st)) continue;   /* (stat: a mod that is a link is read through it) */
        int isdir = S_ISDIR(st.st_mode);
        u64 size = (u64)st.st_size;
#endif
        if (strcmp(nm, ".") && strcmp(nm, "..")) {
            snprintf(r, sizeof r, "%s%s%s", rel, *rel ? "/" : "", nm);
            if (isdir) {
                if (depth < 16) sum += mod_tree_hash(sub, r, depth + 1, files);
            } else if (mod_file_hashed(nm)) {
                u8 le[8];
                for (char *s = r; *s; s++) *s = (char)tolower((unsigned char)*s);
                for (int k = 0; k < 8; k++) le[k] = (u8)(size >> (8 * k));
                sum += fnv64(fnv64(FNV0, r, strlen(r)), le, 8);
                (*files)++;
            }
        }
#ifdef _WIN32
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    }
    closedir(dp);
#endif
    return sum;
}
static u32 hex4(const char *p) {
    char t[5] = {p[0], p[0] ? p[1] : 0, 0, 0, 0};
    if (t[1]) { t[2] = p[2]; if (t[2]) t[3] = p[3]; }
    return (u32)strtoul(t, NULL, 16);
}
/* The names of applied.json's "mods": [...], decoded (json.dump writes \uXXXX for what isn't ASCII:
 * made UTF-8, a folder's name on disk) */
static int applied_mods(const char *js, char (*out)[256], int max) {
    const char *p = strstr(js, "\"mods\"");
    if (!p || !(p = strchr(p, '['))) return 0;
    int n = 0;
    for (p++; *p && *p != ']' && n < max;) {
        if (*p != '"') { p++; continue; }
        char *o = out[n];
        size_t k = 0;
        for (p++; *p && *p != '"'; p++) {
            u32 c = (u8)*p;
            if (c == '\\' && p[1]) {
                p++;
                if (*p == 'u') {
                    c = hex4(p + 1);
                    p += strnlen(p + 1, 4);
                    if (c >= 0xD800 && c < 0xDC00 && p[1] == '\\' && p[2] == 'u') {
                        u32 lo = hex4(p + 3);
                        if (lo >= 0xDC00 && lo < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00); p += 6; }
                    }
                } else c = *p == 'n' ? '\n' : *p == 't' ? '\t' : (u8)*p;
            }
            u8 e[4];
            int l = 0;
            if (c < 0x80) e[l++] = (u8)c;
            else if (c < 0x800) { e[l++] = (u8)(0xC0 | c >> 6); e[l++] = (u8)(0x80 | (c & 0x3F)); }
            else if (c < 0x10000) { e[l++] = (u8)(0xE0 | c >> 12); e[l++] = (u8)(0x80 | (c >> 6 & 0x3F)); e[l++] = (u8)(0x80 | (c & 0x3F)); }
            else { e[l++] = (u8)(0xF0 | c >> 18); e[l++] = (u8)(0x80 | (c >> 12 & 0x3F)); e[l++] = (u8)(0x80 | (c >> 6 & 0x3F)); e[l++] = (u8)(0x80 | (c & 0x3F)); }
            for (int q = 0; q < l && k + 1 < 256; q++) o[k++] = (char)e[q];
        }
        o[k] = 0;
        if (*p) p++;
        n++;
    }
    return n;
}
typedef struct ModSum { char name[MOD_NAME + 1]; u32 hash; } ModSum;
static int modsum_cmp(const void *a, const void *b) { return _stricmp(((const ModSum *)a)->name, ((const ModSum *)b)->name); }
/* mod_set from the last apply's mods, once (a game started before an apply keeps the set it started with) */
static void mod_set_make(void) {
    static int done;
    if (done) return;
    done = 1;
    const char *data = rt_data_dir ? rt_data_dir : "dump/modules";
    char path[1024];
    static char js[1 << 20];
    static char names[4096][256];
    int count = 0, code = rt_code_patches_on();
    snprintf(path, sizeof path, "%s/../mods/applied.json", data);
    FILE *f = fopen(path, "rb");
    js[0] = 0;
    if (f) {
        size_t n = fread(js, 1, sizeof js - 1, f);
        fclose(f);
        js[n] = 0;
        /* Only what the game reads (mods.py's Tree.is_modded): romfs.cfg the modded layer map, or the real
         * ARCropolis loading the listed mods. "mods.py vanilla" keeps applied.json's list as it was. */
        char cfg[4096] = "";
        snprintf(path, sizeof path, "%s/romfs.cfg", data);
        if ((f = fopen(path, "rb"))) { cfg[fread(cfg, 1, sizeof cfg - 1, f)] = 0; fclose(f); }
        if (strstr(cfg, "romfs_mods.layers") || strstr(js, "\"arcropolis\": true")) count = applied_mods(js, names, 4096);
    }
    /* ARCropolis' code patches, on (rt_code_patches_on): an entry of their own, so a game with them and
     * one without are parted, and the room browser says so (mod_diff.h) */
    ModSum *m = count + code ? calloc((size_t)(count + code), sizeof *m) : NULL;
    if (!m) return;
    if (code) {
        snprintf(m[count].name, sizeof m[count].name, "%s", MOD_CODE_PATCHES);
        u64 h = fnv64(FNV0, "WindowSmash: ARCropolis code patches, 13.0.5", 45);
        m[count].hash = (u32)(h ^ h >> 32);
    }
    int applied = count;
    count += code;
    for (int i = 0; i < applied; i++) {
        char dir[1024];
        int files = 0;
        snprintf(dir, sizeof dir, "%s/../../mods/%s", data, names[i]);
        u64 h = mod_tree_hash(dir, "", 0, &files);
        size_t k = 0;
        for (const char *s = names[i]; *s && k < MOD_NAME; s++) m[i].name[k++] = (u8)*s >= 0x20 && *s != ',' ? *s : ' ';
        m[i].name[k] = 0;
        m[i].hash = (u32)(h ^ h >> 32);
    }
    qsort(m, (size_t)count, sizeof *m, modsum_cmp);
    int listed = count <= MODS_MAX ? count : MODS_MAX - 1;
    u8 *w = mod_set;
    *w++ = (u8)(count <= MODS_MAX ? count : MODS_MAX);
    for (int i = 0; i < listed; i++) {
        size_t l = strlen(m[i].name);
        put32(w, m[i].hash);
        w[4] = (u8)l;
        memcpy(w + 5, m[i].name, l);
        w += 5 + l;
    }
    if (count > MODS_MAX) {   /* the rest, by name and hash, as one entry */
        u64 h = FNV0;
        for (int i = listed; i < count; i++) {
            u8 le[4];
            put32(le, m[i].hash);
            h = fnv64(fnv64(h, m[i].name, strlen(m[i].name) + 1), le, 4);
        }
        char rest[MOD_NAME + 1];
        snprintf(rest, sizeof rest, "and %d more", count - listed);
        size_t l = strlen(rest);
        put32(w, (u32)(h ^ h >> 32));
        w[4] = (u8)l;
        memcpy(w + 5, rest, l);
        w += 5 + l;
    }
    mod_set_len = (int)(w - mod_set);
    fprintf(stderr, "[online] mods: %d applied (dump/mods/applied.json)%s: a mod set of %d entr%s\n", applied, code ? ", code patches on" : "",
           mod_set[0], mod_set[0] == 1 ? "y" : "ies");
    free(m);
}

static void send_join(void) {
    u8 p[JOIN_V3_MAX + 2] = {0};
    mod_set_make();
    header(p, T_JOIN);
    put64(p + 4, nonce);
    memcpy(p + 12, room, strlen(room));
    memcpy(p + 12 + NAME_LEN, name, strlen(name));
    const char *build = rt_module_main.build_id ? rt_module_main.build_id : "";
    size_t build_len = strlen(build);
    memcpy(p + 12 + 2 * NAME_LEN, build, build_len < BUILD_LEN ? build_len : BUILD_LEN);
    int len = JOIN_SIZE;
    if (proto == NET_PROTOCOL_V3) {   /* (then the simulation version and the mod set) */
        put32(p + JOIN_SIZE, SIM_VERSION);
        memcpy(p + JOIN_SIZE + 4, mod_set, (size_t)mod_set_len);
        len += 4 + mod_set_len;
    } else {   /* version 2's tail: 'W' 'S', the same two (a private room's relay compares them) */
        p[JOIN_SIZE] = 'W';
        p[JOIN_SIZE + 1] = 'S';
        put32(p + JOIN_SIZE + 2, SIM_VERSION);
        memcpy(p + JOIN_SIZE + 6, mod_set, (size_t)mod_set_len);
        len += 6 + mod_set_len;
    }
    send_udp(client_sock, p, len, &relay_at);
}

/* The game's Rooms List shows 24 rooms (hle_net.c): the room picked in F1's room browser comes
 * first (LIST flag 2), and the rest a page of 24 at a time (flag 4), so that any room can be
 * reached however many the lobby has. A lobby without them reads the first 16 bytes. */
static volatile LONG list_first, list_page;
void net_rooms_pick(u32 host) { InterlockedExchange(&list_first, (LONG)host); }
u32 net_rooms_picked(void) { return (u32)list_first; }
void net_rooms_page(int page) { InterlockedExchange(&list_page, page < 0 ? 0 : page); }
int net_rooms_page_get(void) { return (int)list_page; }

void net_lobby_list(u32 seq) {
    if (!net_lobby()) return;
    u8 p[4 + 8 + 4 + 1 + 4 + 2];
    header(p, T_LIST);
    put64(p + 4, cookie);
    put32(p + 12, seq);
    u32 first = (u32)list_first;
    int page = (int)list_page, n = 16;
    if (first || page) {
        p[16] = (u8)((first ? 2 : 0) | (page ? 4 : 0));
        put32(p + 17, first);
        p[21] = (u8)(page >> 8);
        p[22] = (u8)page;
        n = sizeof p;
    }
    send_udp(client_sock, p, n, &relay_at);
}

/* ---- version 3: the room browser and matchmaking (net.h) --------------------------------
 * ssbu-recomp's (its runtime/net_relay.c: rooms_ask, rooms_info, rooms_end, net_rooms_get,
 * room_why and mods_read; mm_find, mm_send, mm_tick, mm_state and net_match_*), in version 3's
 * header and layouts (a room's simulation version after its build), with WELCOME's flags in place
 * of its guesses, and no popups: a menu page will show what they say. The lobby says which rooms
 * this game can join (ROOM_INFO's flag 8: the room's key is this game's); room_why says why not,
 * from what the room's game is. Rollback messages (RB): "rollback messages" above. */
enum { T_RB = 16, T_ROOMS, T_ROOM_INFO, T_ROOMS_END, T_MATCH, T_MATCH_STATE };
#define ROOMS_MAX 512   /* (the browser's list: the lobby sends up to 1000) */
#define ROOM_INFO_MIN (4 + 4 + 3 + NAME_LEN + BUILD_LEN + 4 + 1)   /* (its body, with a mod set of none) */
#define MATCH_STATE_SIZE (4 + 3 + 4 + NAME_LEN + 2 + 2 + 2)
#define ROOMS_WAIT_MS 3000   /* a ROOMS unanswered this long: NET_ROOMS_NO_ANSWER */

static int lobby_has(int flag) { return joined && lobby && proto == NET_PROTOCOL_V3 && (lobby_flags & flag); }
static int lobby_has_rb(void) { return lobby_has(W_RB); }
static void rb_lobby_send(u32 dst, const void *data, u32 len) {   /* (an RB: the lobby's bounds, 16 < length <= 1500) */
    u8 p[1500];
    if (16 + len > sizeof p) return;
    header(p, T_RB);
    put64(p + 4, cookie);
    put32(p + 12, dst);
    memcpy(p + 16, data, len);
    send_udp(client_sock, p, 16 + (int)len, &relay_at);
    InterlockedIncrement64(&rb_out_lobby);
}
static void printable(char *out, const u8 *p, int n) {   /* (out: n + 1 bytes) */
    for (int k = 0; k < n; k++) out[k] = p[k] && p[k] < 0x20 ? '?' : (char)p[k];
    out[n] = 0;
}
static void names_add(char *out, size_t n, const char *what) {   /* (", " between) */
    size_t t = strlen(out);
    snprintf(out + t, n - t, "%s%s", t ? ", " : "", what);
}

/* A mod set (count u8, count x (hash u32, name length u8, name)) into out (max at most): how many
 * mods it has, or -1 when it is not one. */
static int mods_read(const u8 *p, int n, Mod *out, int max) {
    if (n < 1 || p[0] > MODS_MAX) return -1;
    int count = p[0], k = 0, o = 1;
    for (int i = 0; i < count; i++) {
        if (o + 5 > n || p[o + 4] > MOD_NAME || o + 5 + p[o + 4] > n) return -1;
        if (k < max) {
            int l = p[o + 4];
            out[k].hash = get32(p + o);
            for (int c = 0; c < l; c++) out[k].name[c] = p[o + 5 + c] >= 0x20 && p[o + 5 + c] < 0x7F ? (char)p[o + 5 + c] : '?';
            out[k].name[l] = 0;
            k++;
        }
        o += 5 + p[o + 4];
    }
    return k;
}

/* Why this game cannot join a room whose game is of build b, simulation version sim and mods m:
 * the first that differs of the build, the simulation version and the mods (by name); which one
 * in *kind (NET_WHY_*). */
static void room_why(const char *b, u32 sim, const Mod *m, int nm, char *out, size_t n, int *kind) {
    const char *mine = rt_module_main.build_id ? rt_module_main.build_id : "";
    out[0] = 0;
    *kind = _stricmp(b, mine) ? NET_WHY_BUILD : sim != SIM_VERSION ? NET_WHY_SIM : NET_WHY_MODS;
    if (_stricmp(b, mine)) { snprintf(out, n, "Another version of the game: this room's is build %.8s, yours %.8s", b, mine); return; }
    if (sim != SIM_VERSION) {
        snprintf(out, n, "Another version of WindowSmash: this room's simulation version is %u, yours %u (the older one needs updating)",
                 (unsigned)sim, (unsigned)SIM_VERSION);
        return;
    }
    Mod own[MODS_MAX];
    mod_set_make();
    int no = mods_read(mod_set, mod_set_len, own, MODS_MAX);
    mods_diff_text(m, nm, own, no < 0 ? 0 : no, "this room", out, n);
    if (!out[0]) snprintf(out, n, "Other mods, of the same names");
}

/* A room's game and mods, for the room browser's details: its build, WindowSmash (simulation
 * version) and netplay version, each said against this game's, and its mods, each marked as this
 * game has it. */
static void room_details(const NetRoom *r, const Mod *m, int nm, char *out, size_t n) {
    const char *mine = rt_module_main.build_id ? rt_module_main.build_id : "";
    Mod own[MODS_MAX];
    char mods[400];
    mod_set_make();
    int no = mods_read(mod_set, mod_set_len, own, MODS_MAX);
    mods_list_marked(m, nm, own, no < 0 ? 0 : no, mods, sizeof mods);
    char build[48], sim[48];
    if (!_stricmp(r->build, mine)) snprintf(build, sizeof build, "%.8s (yours too)", r->build);
    else snprintf(build, sizeof build, "%.8s (yours %.8s)", r->build, mine);
    if (r->sim == SIM_VERSION) snprintf(sim, sizeof sim, "%u (yours too)", (unsigned)r->sim);
    else snprintf(sim, sizeof sim, "%u (yours %u)", (unsigned)r->sim, (unsigned)SIM_VERSION);
    snprintf(out, n, "Game build %s, WindowSmash simulation version %s, netplay version 3\nMods: %s", build, sim, nm ? mods : "none");
}

/* What differs between this game and the room's host's, from a REJECT's detail (build[40], sim u32,
 * flags u8, mod set): the build, else the simulation version, else the mods (mod_diff.h). */
static void refusal_detail(const u8 *d, int n, char *out, size_t size) {
    const char *mine = rt_module_main.build_id ? rt_module_main.build_id : "";
    char build[BUILD_LEN + 1];
    out[0] = 0;
    if (n < BUILD_LEN + 5) return;
    printable(build, d, BUILD_LEN);
    u32 sim = get32(d + BUILD_LEN);
    int known = d[BUILD_LEN + 4] & 1;
    if (_stricmp(build, mine)) { snprintf(out, size, "Game build differs: the room's (or lobby's) is %.8s, yours %.8s", build, mine); return; }
    if (!known) return;
    if (sim != SIM_VERSION) {
        snprintf(out, size, "WindowSmash differs: simulation version %u in the room, %u in yours (the older one needs updating)", (unsigned)sim,
                 (unsigned)SIM_VERSION);
        return;
    }
    Mod theirs[MODS_MAX], own[MODS_MAX];
    int nt = mods_read(d + BUILD_LEN + 5, n - BUILD_LEN - 5, theirs, MODS_MAX);
    mod_set_make();
    int no = mods_read(mod_set, mod_set_len, own, MODS_MAX);
    if (nt >= 0) mods_diff_text(theirs, nt, own, no < 0 ? 0 : no, "the room", out, size);
}

/* The room browser. room_next is the client thread's, filled by ROOM_INFOs; ROOMS_END makes it
 * room_list, which net_rooms_get reads. */
static SRWLOCK rooms_lock = SRWLOCK_INIT;
static NetRoom room_list[ROOMS_MAX], room_next[ROOMS_MAX];
static int nroom_list, nroom_next;
static u32 rooms_seq;
static volatile LONG rooms_wanted, rooms_waiting, rooms_listed;   /* net_rooms_ask's; a ROOMS unanswered; a list since joined */
static volatile u64 rooms_sent;

static void rooms_ask(u64 t) {   /* (client thread) */
    u8 p[4 + 8 + 4];
    header(p, T_ROOMS);
    put64(p + 4, cookie);
    put32(p + 12, ++rooms_seq);
    nroom_next = 0;
    rooms_sent = t;
    InterlockedExchange(&rooms_waiting, 1);
    send_udp(client_sock, p, sizeof p, &relay_at);
}

static void rooms_info(const u8 *p, int n) {   /* (client thread) a ROOM_INFO's body */
    if (n < ROOM_INFO_MIN || get32(p) != rooms_seq || !rooms_waiting || nroom_next >= ROOMS_MAX) return;
    NetRoom *r = &room_next[nroom_next++];
    Mod m[MODS_MAX];
    memset(r, 0, sizeof *r);
    r->host = get32(p + 4);
    r->players = p[8];
    r->most = p[9];
    r->open = p[10] & 1;
    r->mine = (p[10] & 2) != 0;
    r->joinable = (p[10] & 8) != 0;
    printable(r->name, p + 11, NAME_LEN);
    printable(r->build, p + 11 + NAME_LEN, BUILD_LEN);
    r->sim = get32(p + 11 + NAME_LEN + BUILD_LEN);
    int at = 11 + NAME_LEN + BUILD_LEN + 4, nm = mods_read(p + at, n - at, m, MODS_MAX);
    if (nm < 0) snprintf(r->mods, sizeof r->mods, "(unreadable)");
    r->nmods = nm < 0 ? 0 : nm;
    for (int i = 0; i < r->nmods; i++) names_add(r->mods, sizeof r->mods, m[i].name);
    if (!r->joinable) room_why(r->build, r->sim, m, r->nmods, r->why, sizeof r->why, &r->why_kind);
    room_details(r, m, r->nmods, r->details, sizeof r->details);
}

static void rooms_end(const u8 *p, int n) {   /* (client thread) a ROOMS_END's body */
    if (n < 5 || get32(p) != rooms_seq || !rooms_waiting) return;
    static int said = -1;
    int can = 0;
    AcquireSRWLockExclusive(&rooms_lock);
    memcpy(room_list, room_next, sizeof room_list[0] * (size_t)nroom_next);
    nroom_list = nroom_next;
    for (int i = 0; i < nroom_list; i++) can += room_list[i].joinable && !room_list[i].mine;
    ReleaseSRWLockExclusive(&rooms_lock);
    InterlockedExchange(&rooms_listed, 1);
    InterlockedExchange(&rooms_waiting, 0);
    if (nroom_next != said) fprintf(stderr, "[net] rooms of lobby code %s: %d (%d this game can join)\n", room, nroom_next, can);
    said = nroom_next;
}

void net_rooms_ask(void) { InterlockedExchange(&rooms_wanted, 1); }

int net_rooms_get(NetRoom *out, int max, int *state) {
    int st = NET_ROOMS_OFF, n = 0;
    if (lobby_has(W_ROOMS)) {
        if (!rooms_listed && !rooms_waiting) net_rooms_ask();   /* (the first time: ask) */
        st = rooms_waiting ? (now_ms() - rooms_sent > ROOMS_WAIT_MS ? NET_ROOMS_NO_ANSWER : NET_ROOMS_ASKING)
           : rooms_listed ? NET_ROOMS_LISTED : NET_ROOMS_ASKING;
        AcquireSRWLockShared(&rooms_lock);
        n = nroom_list < max ? nroom_list : max;
        if (n > 0) memcpy(out, room_list, sizeof *out * (size_t)n);
        ReleaseSRWLockShared(&rooms_lock);
    }
    if (state) *state = st;
    return n;
}

/* Matchmaking: while this game searches or is matched, MATCH (op 1, the search's id) goes every
 * 2 s, each answered with a MATCH_STATE (the lobby sends one itself when something changes); a
 * cancel is MATCH op 0, sent until the lobby says idle (3 times at most). */
static SRWLOCK match_lock = SRWLOCK_INIT;
static NetMatch match_now;   /* (match_lock, as match_id; zero: NET_MATCH_IDLE) */
static u32 match_id;
static volatile LONG match_want, match_cancels, match_soon;   /* searching or matched; cancels left to send; send at once */
static u64 match_sent;                                         /* (client thread) */

static void match_send(int op) {   /* (client thread) */
    u8 p[4 + 8 + 1 + 4];
    header(p, T_MATCH);
    put64(p + 4, cookie);
    p[12] = (u8)op;
    AcquireSRWLockShared(&match_lock);
    put32(p + 13, match_id);
    ReleaseSRWLockShared(&match_lock);
    send_udp(client_sock, p, sizeof p, &relay_at);
}

static void match_tick(u64 t) {   /* (client thread, joined) */
    if (match_cancels > 0) {
        if (InterlockedExchange(&match_soon, 0) || t - match_sent >= 300) { match_send(0); match_sent = t; InterlockedDecrement(&match_cancels); }
        return;
    }
    if (!match_want || !(InterlockedExchange(&match_soon, 0) || t - match_sent >= 2000)) return;
    match_send(1);
    match_sent = t;
}

static void match_state(const u8 *p) {   /* (client thread) a MATCH_STATE's body */
    static const char *const ended[] = {"no opponent found in time", "the other player cancelled or left",
                                        "both in one room", "the match expired (not in one room in time)"};
    int st = p[4], role = p[5], flags = p[6];
    char other[NAME_LEN + 1];
    printable(other, p + 11, NAME_LEN);
    AcquireSRWLockExclusive(&match_lock);
    if (get32(p) != match_id || st > NET_MATCH_EXPIRED) { ReleaseSRWLockExclusive(&match_lock); return; }
    if (!match_want) {   /* (cancelled, or over: the lobby's idle ends the cancel; the rest came late) */
        if (st == NET_MATCH_IDLE) InterlockedExchange(&match_cancels, 0);
        ReleaseSRWLockExclusive(&match_lock);
        return;
    }
    int was = match_now.state, was_open = match_now.room_open;
    match_now.state = st;
    match_now.role = role;
    match_now.room_open = flags & 1;
    match_now.peer = get32(p + 7);
    memcpy(match_now.peer_name, other, sizeof match_now.peer_name);
    match_now.searching = get16(p + 43);
    match_now.players = get16(p + 45);
    match_now.waited_s = get16(p + 47);
    ReleaseSRWLockExclusive(&match_lock);
    if (st >= NET_MATCH_TIMEOUT) InterlockedExchange(&match_want, 0);   /* (over: nothing more to ask) */
    if (st == NET_MATCH_MATCHED && was != NET_MATCH_MATCHED)
        fprintf(stderr, "[net] match: matched with %s: %s\n", other, role == NET_MATCH_HOST ? "this game hosts (Local Wireless > Create Room)"
                        : "this game joins their room (Local Wireless > Rooms List: theirs comes first)");
    if (st == NET_MATCH_MATCHED && role == NET_MATCH_JOIN && (flags & 1) && !(was == NET_MATCH_MATCHED && was_open))
        fprintf(stderr, "[net] match: %s's room is open\n", other);
    if (st >= NET_MATCH_TIMEOUT && st != was)
        fprintf(stderr, "[net] match: %s%s%s\n", ended[st - NET_MATCH_TIMEOUT], st == NET_MATCH_TIMEOUT ? "" : ": ", st == NET_MATCH_TIMEOUT ? "" : other);
}

void net_match_find(void) {
    if (!lobby_has(W_MATCH)) return;   /* (net_match_get says NET_MATCH_OFF) */
    AcquireSRWLockExclusive(&match_lock);
    match_id = (u32)random64() | 1;
    memset(&match_now, 0, sizeof match_now);
    match_now.state = NET_MATCH_ASKING;
    ReleaseSRWLockExclusive(&match_lock);
    InterlockedExchange(&match_cancels, 0);
    InterlockedExchange(&match_want, 1);
    InterlockedExchange(&match_soon, 1);
    fprintf(stderr, "[net] match: searching for an opponent (lobby code %s)\n", room);
}

void net_match_cancel(void) {
    AcquireSRWLockExclusive(&match_lock);
    int was = match_now.state;
    if (was == NET_MATCH_SEARCHING || was == NET_MATCH_MATCHED || was == NET_MATCH_ASKING) match_now.state = NET_MATCH_IDLE;
    ReleaseSRWLockExclusive(&match_lock);
    if (InterlockedExchange(&match_want, 0)) {
        InterlockedExchange(&match_cancels, 3);
        InterlockedExchange(&match_soon, 1);
        fprintf(stderr, "[net] match: cancelled\n");
    }
}

void net_match_get(NetMatch *out) {
    AcquireSRWLockShared(&match_lock);
    *out = match_now;
    ReleaseSRWLockShared(&match_lock);
    if (!lobby_has(W_MATCH)) {
        memset(out, 0, sizeof *out);
        out->state = NET_MATCH_OFF;
        out->players = out->searching = out->code_players = -1;
        return;
    }
    if (out->state != NET_MATCH_SEARCHING && out->state != NET_MATCH_MATCHED) {   /* (between searches: the PONG's counts) */
        out->players = (int)members;
        out->searching = (int)key_searching;
    }
    out->code_players = (int)code_players;
}

/* (client thread) A new start with the lobby: the last one's rooms and search went with it; a
 * search goes on as a new one. */
static void v3_new_start(void) {
    AcquireSRWLockExclusive(&rooms_lock);
    nroom_list = 0;
    ReleaseSRWLockExclusive(&rooms_lock);
    InterlockedExchange(&rooms_listed, 0);
    InterlockedExchange(&rooms_waiting, 0);
    InterlockedExchange(&key_searching, -1);
    InterlockedExchange(&code_players, -1);
    InterlockedExchange(&match_cancels, 0);
    AcquireSRWLockExclusive(&match_lock);
    if (match_want) match_now.state = NET_MATCH_ASKING;
    else if (match_now.state != NET_MATCH_IDLE) memset(&match_now, 0, sizeof match_now);
    ReleaseSRWLockExclusive(&match_lock);
    InterlockedExchange(&match_soon, 1);
}

static void v3_tick(u64 t) {   /* (client thread, joined) what the room browser and matchmaking have to send */
    if (InterlockedExchange(&rooms_wanted, 0) && lobby_has(W_ROOMS)) rooms_ask(t);
    if (lobby_has(W_MATCH)) match_tick(t);
}

static void read_members(const u8 *p, int n) {
    int count = n >= 1 ? p[0] : 0, got = 0;
    if (count > ROOM_MEMBERS) count = ROOM_MEMBERS;
    AcquireSRWLockExclusive(&members_lock);
    for (int i = 0; i < count && 1 + (i + 1) * (4 + NAME_LEN) <= n; i++, got++) {
        member_ips[i] = get32(p + 1 + i * (4 + NAME_LEN));
        memcpy(member_names[i], p + 5 + i * (4 + NAME_LEN), NAME_LEN);
        member_names[i][NAME_LEN] = 0;
    }
    member_count = got;
    ReleaseSRWLockExclusive(&members_lock);
    InterlockedExchange(&members, count);
}

/* ---- LAN discovery: a private room found on the local network --------------------------------
 * A game hosting a private room (SSBU_NET_HOST) says so every second to the multicast group
 * 239.255.13.13 and to the broadcast address, UDP port NET_LAN_PORT (39501), with a TTL of 1 (no
 * router passes it on):
 *   BEACON 'S' 'N' 2 32, 'L' 'B', id u64 (this run's), relay port u16, sim u32, players u8,
 *          build[40], room[32], host name[32]
 * Not sent while direct connections are off (online_direct.txt, SSBU_NET_DIRECT=0: the player keeps
 * its address to itself) or with SSBU_NET_LAN=0. A game whose relay is "lan" (SSBU_NET_RELAY=lan,
 * online_server.txt "lan [room]", the settings menu's Play online in > this network) listens on that
 * port: after 1.5 s it joins the first host it heard of its own build and simulation version (and of
 * its room, when it names one: SSBU_NET_ROOM, or the file's code) at the beacon's source address, in
 * that host's room; none: it says so and keeps listening, so a host that starts later is found, and
 * after losing its host it takes the next one. The relay itself refuses other mods (REJECT), as for
 * any private room. Two games of one PC find each other too (multicast comes back to the PC's own
 * listeners). (tests) SSBU_NET_LAN_TEST=1: beacons to 127.0.0.1 only, the listener on 127.0.0.1. */
#define T_LAN_BEACON 32
#define LAN_GROUP "239.255.13.13"
#define LAN_BEACON_SIZE (4 + 2 + 8 + 2 + 4 + 1 + BUILD_LEN + 2 * NAME_LEN)
#define LAN_HOSTS 16
#define LAN_FRESH_MS 3500    /* a host not heard for this long is gone */
#define LAN_LISTEN_MS 1500   /* listened for before the first pick */
typedef struct LanHost {
    u64 id, first, seen;
    struct sockaddr_in at;   /* its relay */
    u32 sim;
    int players;
    char build[BUILD_LEN + 1], room[NAME_LEN + 1], name[NAME_LEN + 1];
} LanHost;
static SRWLOCK lan_lock = SRWLOCK_INIT;
static LanHost lan_hosts[LAN_HOSTS];
static int nlan_hosts;
static u64 lan_started;            /* (client thread) the listener's start */

static void lan_options(SOCKET s) {
#ifdef _WIN32
    int ttl = 1, loop = 1;
#else
    unsigned char ttl = 1, loop = 1;   /* (macOS takes a byte for both; Linux either) */
#endif
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&yes, sizeof yes);
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (const char *)&ttl, sizeof ttl);
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, (const char *)&loop, sizeof loop);
    u32 out = outbound_address();   /* (the network the PC goes out on, not a VPN's or a virtual machine's adapter) */
    if (out && !lan_test) {
        struct in_addr a;
        a.s_addr = htonl(out);
        setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, (const char *)&a, sizeof a);
    }
}

static void lan_beacon_make(u8 *p, u64 id) {
    memset(p, 0, LAN_BEACON_SIZE);
    relay_header(p, T_LAN_BEACON);
    p[4] = 'L';
    p[5] = 'B';
    put64(p + 6, id);
    put16(p + 14, port);
    put32(p + 16, SIM_VERSION);
    int players = 0;
    for (int i = 0; i < ROOMS; i++)   /* (the relay thread's: a moment old at most) */
        if (rooms[i].count && !strcmp(rooms[i].name, room)) players = rooms[i].count;
    p[20] = (u8)players;
    const char *build = rt_module_main.build_id ? rt_module_main.build_id : "";
    size_t n = strlen(build);
    memcpy(p + 21, build, n < BUILD_LEN ? n : BUILD_LEN);
    memcpy(p + 21 + BUILD_LEN, room, strlen(room));
    memcpy(p + 21 + BUILD_LEN + NAME_LEN, name, strlen(name));
}

static DWORD WINAPI lan_beacon_thread(LPVOID unused) {   /* (hosting) */
    (void)unused;
    SetThreadDescription(GetCurrentThread(), L"netplay lan beacon");
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 0;
    lan_options(s);
    struct sockaddr_in to[2];
    memset(to, 0, sizeof to);
    int nto = lan_test ? 1 : 2;
    for (int i = 0; i < 2; i++) { to[i].sin_family = AF_INET; to[i].sin_port = htons(lan_port); }
    if (lan_test) to[0].sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    else { inet_pton(AF_INET, LAN_GROUP, &to[0].sin_addr); to[1].sin_addr.s_addr = htonl(INADDR_BROADCAST); }
    u64 id = random64() | 1;
    u8 p[LAN_BEACON_SIZE];
    for (;;) {
        lan_beacon_make(p, id);
        for (int i = 0; i < nto; i++) send_udp(s, p, sizeof p, &to[i]);
        Sleep(1000);
    }
}

static void lan_announce_start(void) {   /* (hosting, the relay up) */
    if (!lan_announce || !direct_ok) {
        InterlockedExchange(&lan_state, NET_LAN_QUIET);
        fprintf(stderr, "[net] lan: room %s not announced on this network (%s): players here join with the room code or address\n", room,
                !lan_announce ? "SSBU_NET_LAN=0" : "direct connections off");
        return;
    }
    InterlockedExchange(&lan_state, NET_LAN_ANNOUNCING);
    CloseHandle(CreateThread(NULL, 0, lan_beacon_thread, NULL, 0, NULL));
    fprintf(stderr, "[net] lan: announcing room %s on this network (UDP port %u%s)\n", room, lan_port, lan_test ? ", to 127.0.0.1 only" : "");
}

static int lan_beacon_read(const u8 *p, int n, LanHost *h) {
    if (n < LAN_BEACON_SIZE || p[0] != MAGIC0 || p[1] != MAGIC1 || p[2] != NET_PROTOCOL || p[3] != T_LAN_BEACON || p[4] != 'L' || p[5] != 'B')
        return 0;
    memset(h, 0, sizeof *h);
    h->id = get64(p + 6);
    h->at.sin_family = AF_INET;
    h->at.sin_port = htons(get16(p + 14));
    h->sim = get32(p + 16);
    h->players = p[20];
    printable(h->build, p + 21, BUILD_LEN);
    printable(h->room, p + 21 + BUILD_LEN, NAME_LEN);
    printable(h->name, p + 21 + BUILD_LEN + NAME_LEN, NAME_LEN);
    return h->id && get16(p + 14) && h->room[0];
}

static int lan_ours(const LanHost *h) {   /* of this game's build and simulation version, and of its room when it names one */
    char mine[BUILD_LEN + 1];
    snprintf(mine, sizeof mine, "%.40s", rt_module_main.build_id ? rt_module_main.build_id : "");
    return !strcmp(h->build, mine) && h->sim == SIM_VERSION && (!room_named || !strcmp(h->room, room));
}

static DWORD WINAPI lan_listen_thread(LPVOID sock) {   /* (LAN mode) every beacon heard, kept in lan_hosts */
    SOCKET s = (SOCKET)(uintptr_t)sock;
    SetThreadDescription(GetCurrentThread(), L"netplay lan listener");
    static u8 buf[2048];
    for (;;) {
        struct sockaddr_in from;
        socklen_t flen = sizeof from;
        int n = recvfrom(s, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &flen);
        LanHost h;
        if (n <= 0 || !lan_beacon_read(buf, n, &h)) continue;
        h.at.sin_addr = from.sin_addr;
        u64 t = now_ms();
        AcquireSRWLockExclusive(&lan_lock);
        int k = 0;
        while (k < nlan_hosts && lan_hosts[k].id != h.id) k++;
        if (k == nlan_hosts) {   /* a new one: in a free place, else in the stalest one's */
            if (nlan_hosts < LAN_HOSTS) nlan_hosts++;
            else {
                k = 0;
                for (int i = 1; i < LAN_HOSTS; i++) if (lan_hosts[i].seen < lan_hosts[k].seen) k = i;
            }
            h.first = t;
            if (logging) {
                char ip[16];
                inet_ntop(AF_INET, &h.at.sin_addr, ip, sizeof ip);
                fprintf(stderr, "[net] lan: heard %s's room %s (%d players) at %s:%u\n", h.name, h.room, h.players, ip, ntohs(h.at.sin_port));
            }
        } else h.first = lan_hosts[k].first;
        h.seen = t;
        lan_hosts[k] = h;
        ReleaseSRWLockExclusive(&lan_lock);
    }
}

static void lan_count(int *rooms_out, int *others) {   /* the hosts heard lately: of this game's, and the others */
    int ours = 0, other = 0;
    u64 t = now_ms();
    AcquireSRWLockShared(&lan_lock);
    for (int i = 0; i < nlan_hosts; i++)
        if (t - lan_hosts[i].seen < LAN_FRESH_MS) { if (lan_ours(&lan_hosts[i])) ours++; else other++; }
    ReleaseSRWLockShared(&lan_lock);
    if (rooms_out) *rooms_out = ours;
    if (others) *others = other;
}

int net_lan(int *rooms_out, int *others) {
    start();
    lan_count(rooms_out, others);
    return lan_state;
}

static void lan_listen(void) {   /* (client thread, LAN mode) */
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    lan_started = now_ms();
    if (s == INVALID_SOCKET) return;
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof yes);   /* (other games of this PC listen too) */
#ifdef SO_REUSEPORT
    setsockopt(s, SOL_SOCKET, SO_REUSEPORT, (const char *)&yes, sizeof yes);
#endif
#ifdef _WIN32
    DWORD timeout = 200;
#else
    struct timeval timeout = {0, 200000};
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
    no_connreset(s);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(lan_port);
    a.sin_addr.s_addr = lan_test ? htonl(INADDR_LOOPBACK) : htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&a, sizeof a)) {
        fprintf(stderr, "[net] lan: cannot listen on UDP port %u: no room on this network can be found\n", lan_port);
        closesocket(s);
        return;
    }
    if (!lan_test) {   /* (the group, on the network the PC goes out on and on the default one) */
        struct ip_mreq m;
        memset(&m, 0, sizeof m);
        inet_pton(AF_INET, LAN_GROUP, &m.imr_multiaddr);
        u32 out = outbound_address();
        if (out) { m.imr_interface.s_addr = htonl(out); setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *)&m, sizeof m); }
        m.imr_interface.s_addr = htonl(INADDR_ANY);
        setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *)&m, sizeof m);
    }
    CloseHandle(CreateThread(NULL, 0, lan_listen_thread, (LPVOID)(uintptr_t)s, 0, NULL));
}

/* (client thread, LAN mode) Waits until a host of this game's is heard (listened for 1.5 s first),
 * then makes it the relay: its address, and its room. */
static void lan_wait(void) {
    int said = 0;
    InterlockedExchange(&lan_state, NET_LAN_LOOKING);
    for (;;) {
        u64 t = now_ms();
        LanHost best;
        int found = 0;
        if (t - lan_started >= LAN_LISTEN_MS) {
            AcquireSRWLockShared(&lan_lock);
            for (int i = 0; i < nlan_hosts; i++) {
                const LanHost *h = &lan_hosts[i];
                if (t - h->seen < LAN_FRESH_MS && lan_ours(h) && (!found || h->first < best.first)) { best = *h; found = 1; }
            }
            ReleaseSRWLockShared(&lan_lock);
        }
        if (found) {
            char ip[16];
            inet_ntop(AF_INET, &best.at.sin_addr, ip, sizeof ip);
            snprintf(room, sizeof room, "%s", best.room);
            relay_at = best.at;
            snprintf(relay_text, sizeof relay_text, "%s:%u", ip, ntohs(best.at.sin_port));
            if (relay_at.sin_addr.s_addr == htonl(INADDR_LOOPBACK) && !bound_loopback) {   /* (as resolve_relay: the relay is here) */
                struct sockaddr_in a = {0};
                a.sin_family = AF_INET;
                a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                bind(client_sock, (struct sockaddr *)&a, sizeof a);
                bound_loopback = 1;
            }
            reject_text[0] = 0;
            InterlockedExchange(&lan_state, NET_LAN_FOUND);
            fprintf(stderr, "[net] lan: found %s's room %s on this network at %s; joining\n", best.name[0] ? best.name : "a host", best.room,
                    relay_text);
            return;
        }
        if (!said && t - lan_started >= LAN_LISTEN_MS) {
            said = 1;
            int others = 0;
            lan_count(NULL, &others);   /* (not net_lan: start() may not be done yet) */
            InterlockedExchange(&lan_state, NET_LAN_NONE);
            snprintf(reject_text, sizeof reject_text, "no room on this network yet (still looking)");
            if (others) fprintf(stderr, "[net] lan: %d room%s on this network, of another game version or room only; still looking\n", others,
                                others == 1 ? "" : "s");
            else fprintf(stderr, "[net] lan: no room on this network yet; still looking\n");
        }
        Sleep(100);
    }
}

static DWORD WINAPI client_thread(LPVOID unused) {
    (void)unused;
    SetThreadDescription(GetCurrentThread(), L"netplay client");
    static u8 buf[65536];
    if (lan_mode) { lan_listen(); lan_wait(); }   /* (LAN discovery: the relay is the host heard) */
    else
        while (!resolve_relay()) {
            snprintf(reject_text, sizeof reject_text, "cannot find the relay %s", relay_text);
            Sleep(3000);
        }
    reject_text[0] = 0;
    u64 last_join = 0, last_ping = 0, last_tick = 0, last_probe = 0, last_stats = now_ms();
    if (!direct_ok) fprintf(stderr, "[net] direct connections off (SSBU_NET_DIRECT=0): all through the relay\n");
    for (;;) {
        u64 t = now_ms();
        if (!joined && t - last_join >= 500) { send_join(); last_join = t; }
        if (joined && t - last_ping >= 1000) {
            u8 p[PING_EXT_SIZE + 4];
            header(p, T_PING);
            put64(p + 4, nonce);
            put64(p + 12, t);
            put64(p + 20, cookie);
            put16(p + 28, ping_ms < 0 || ping_ms > 0xFFFF ? 0xFFFF : (u16)ping_ms);
            if (!local_port) local_address();
            int direct_now = 0;
            AcquireSRWLockShared(&peers_lock);
            for (int i = 0; i < npeers; i++) direct_now += usable(&peers[i], t);
            put32(p + 30, peers_version);
            ReleaseSRWLockShared(&peers_lock);
            put32(p + 34, local_ip);
            put16(p + 38, local_port);
            p[40] = (u8)direct_ok;
            p[41] = (u8)direct_now;
            p[42] = 'N'; p[43] = 'P';   /* (the router's outside port for this game's: map_thread) */
            put16(p + 44, map_state == 2 ? (u16)map_port : 0);
            send_udp(client_sock, p, sizeof p, &relay_at);
            last_ping = t;
            if (lobby && local_port) map_start();
            if (lobby && probe_port && t - last_probe >= (seen_probe ? 20000u : 2000u)) {   /* (the lobby's PROBE port: a second port of the router's) */
                u8 q[12];
                struct sockaddr_in at = relay_at;
                header(q, T_PROBE);
                put64(q + 4, cookie);
                at.sin_port = htons((u16)probe_port);
                send_udp(client_sock, q, sizeof q, &at);
                last_probe = t;
            }
            if (lobby && t - last_stats >= 600000) {
                char line[256];
                last_stats = t;
                stats_line(line, sizeof line);
                if (stat_introduced) fprintf(stderr, "[net] %s\n", line);
            }
        }
        if (joined && t - last_pong > TIMEOUT_MS) {
            unjoined_since = t;
            InterlockedExchange(&joined, 0);   /* the relay is gone: keep asking to join again */
            InterlockedExchange(&ping_ms, -1);
            clear_peers();
            InterlockedExchange(&lobby_flags, 0);
            InterlockedExchange(&proto, proto_wanted);   /* (asked in its version again: it may be another lobby now) */
            fell_back = 0;
            fprintf(stderr, "[net] lost the relay %s; reconnecting\n", relay_text);
            if (lan_mode) { lan_wait(); last_join = 0; }   /* (the next host heard on this network: the lost one is heard no more) */
        }
        if (joined && t - last_tick >= 50) { peers_tick(t); last_tick = t; }
        if (joined) v3_tick(t);
        struct sockaddr_in from;
        int n = net_recv(buf, sizeof buf, &from);
        if (n < 4 || buf[0] != MAGIC0 || buf[1] != MAGIC1) continue;
        if (!same_addr(&from, &relay_at)) {   /* (another player, straight) */
            if (buf[2] == proto && joined) direct_packet(buf, n, &from);
            continue;
        }
        if (buf[2] != proto && buf[3] != T_REJECT) continue;   /* (a REJECT comes in the JOIN's version, or a relay's own) */
        switch (buf[3]) {
        case T_DATA:
            if (n >= 16 && joined) {
                if (get16(buf + 12) != NET_LDN_PORT) count_relayed_in(get32(buf + 4));
                if (!rb_take(get32(buf + 4), get16(buf + 14), buf + 16, (u32)(n - 16), &rb_in_relayed))
                    vsock_deliver(get32(buf + 4), get16(buf + 12), get32(buf + 8), get16(buf + 14), buf + 16, (u32)(n - 16));
            }
            break;
        case T_WELCOME:
            if (n < 26 || get64(buf + 4) != nonce) break;
            if (!joined) { clear_peers(); v3_new_start(); }   /* (a new start: the lobby counts PEERS from 1 again) */
            my_ip = get32(buf + 12);
            cookie = get64(buf + 16);
            InterlockedExchange(&lobby, buf[24] & W_LOBBY);
            InterlockedExchange(&lobby_flags, buf[24]);
            read_members(buf + 25, n - 25);
            last_pong = now_ms();
            last_ping = 0;   /* (a ping at once: a lobby answers nothing else until it has seen the cookie) */
            reject_text[0] = 0;
            if (!joined && lobby) fprintf(stderr, "[net] joined the lobby on %s as 10.13.%u.%u (lobby code %s)%s\n", relay_text, my_ip >> 8 & 255, my_ip & 255, room,
                                          proto == NET_PROTOCOL_V3 ? ", protocol 3" : "");
            else if (!joined) fprintf(stderr, "[net] joined room %s on %s as 10.13.%u.%u (%ld players)\n", room, relay_text, my_ip >> 8 & 255, my_ip & 255, (long)members);
            InterlockedExchange(&joined, 1);
            break;
        case T_REJECT:
            if (n >= 13 + 1 && get64(buf + 4) == nonce) {
                if (fell_back && buf[2] == NET_PROTOCOL_V3) break;   /* (the answer to a JOIN of version 3, sent before) */
                if (proto == NET_PROTOCOL_V3 && buf[12] == R_VERSION) {   /* a lobby or relay of version 2: join it in version 2 */
                    InterlockedExchange(&proto, NET_PROTOCOL);
                    fell_back = 1;
                    last_join = 0;   /* (at once) */
                    fprintf(stderr, "[net] %s speaks netplay version 2: joining it in version 2 (no room browser or matchmaking there)\n", relay_text);
                    break;
                }
                snprintf(reject_text, sizeof reject_text, "%.64s", (const char *)buf + 13);   /* (also version 3's reason 5: a mod set it can't read) */
                reject_detail[0] = 0;
                if (n >= REJECT_SIZE + 2 + BUILD_LEN + 5 && buf[REJECT_SIZE] == 'W' && buf[REJECT_SIZE + 1] == 'D')
                    refusal_detail(buf + REJECT_SIZE + 2, n - REJECT_SIZE - 2, reject_detail, sizeof reject_detail);
                fprintf(stderr, "[net] the relay refused: %s%s%s\n", reject_text, reject_detail[0] ? "\n[net]   " : "", reject_detail);
                Sleep(2000);
            }
            break;
        case T_PONG:
            if (n >= 17) {
                last_pong = now_ms();
                InterlockedExchange(&ping_ms, (LONG)(last_pong - get64(buf + 4)));
                if (lobby) { InterlockedExchange(&members, get16(buf + 12)); InterlockedExchange(&rooms_open, get16(buf + 14)); }
                if (proto == NET_PROTOCOL_V3 && n >= 21) {   /* (version 3's: players of this key searching, players of the code) */
                    InterlockedExchange(&key_searching, get16(buf + 17));
                    InterlockedExchange(&code_players, get16(buf + 19));
                }
                if (n >= 29 && buf[21] == 'N' && buf[22] == 'A') {   /* (where the lobby sees this game; its PROBE port) */
                    int was = my_nat();
                    InterlockedExchange(&seen_port, get16(buf + 23));
                    InterlockedExchange(&seen_probe, get16(buf + 25));
                    InterlockedExchange(&probe_port, get16(buf + 27));
                    int now_kind = my_nat();
                    if (now_kind != was && now_kind != NAT_UNKNOWN && lobby) {
                        static const char *const said[] = {"", "keeps this game's port", "maps this game to one outside port for every destination",
                                                           "changes ports for each destination, in turn (sequential symmetric NAT: the other player guesses the next)",
                                                           "changes ports for each destination (symmetric NAT: direct paths need UPnP or NAT-PMP on in the router)"};
                        fprintf(stderr, "[net] your router %s (UDP port %u here, %ld outside%s)\n", said[now_kind], local_port, (long)seen_port,
                                seen_probe && seen_probe != seen_port ? ", another for a second destination" : "");
                    }
                }
            }
            break;
        case T_MEMBERS:
            read_members(buf + 4, n - 4);
            break;
        case T_LISTING:
            if (n >= 12 + NETWORK_INFO && lobby) ldn_listing(get32(buf + 4), get32(buf + 8), buf + 12);
            break;
        case T_LIST_END:
            if (n >= 9 && lobby) ldn_list_end(get32(buf + 4));
            break;
        case T_PEERS:
            if (n >= 4 + 8 + 5 && lobby && get64(buf + 4) == cookie) read_peers(buf + 12, n - 12);
            break;
        case T_ROOM_INFO:
            if (lobby_has(W_ROOMS)) rooms_info(buf + 4, n - 4);
            break;
        case T_ROOMS_END:
            if (lobby_has(W_ROOMS)) rooms_end(buf + 4, n - 4);
            break;
        case T_MATCH_STATE:
            if (n >= 4 + MATCH_STATE_SIZE && lobby_has(W_MATCH)) match_state(buf + 4);
            break;
        case T_RB:   /* (source u32, message: from a player of this lobby code, through the lobby) */
            if (n > 8 && lobby_has(W_RB)) rb_take(get32(buf + 4), NET_RB_PORT, buf + 8, (u32)(n - 8), &rb_in_lobby);
            break;
        }
    }
}

static void leave(void) {
    if (stat_introduced) {
        char line[256];
        stats_line(line, sizeof line);
        fprintf(stderr, "[net] %s\n", line);
    }
    if (!joined) return;
    u8 p[12];
    header(p, T_LEAVE);
    put64(p + 4, nonce);
    send_udp(client_sock, p, sizeof p, &relay_at);
}

/* Started on first use (the overlay asks every frame), once Winsock is up. */
static void start(void) {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    BOOL pending;
    InitOnceBeginInitialize(&once, 0, &pending, NULL);
    if (!pending) return;
    configure();
    if (configured) {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        DWORD timeout = 100;
#else
        struct timeval timeout = {0, 100000};
#endif
        unsigned int r1, r2;
        rand_s(&r1); rand_s(&r2);
        nonce = (u64)r1 << 32 | r2;
        unjoined_since = now_ms();
        if (hosting) {
            relay_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            struct sockaddr_in a = {0};
            a.sin_family = AF_INET;
            a.sin_port = htons(port);
            const char *only = getenv("SSBU_NET_BIND");   /* (an address to listen on alone, e.g. 127.0.0.1 for tests) */
            if (only && *only) inet_pton(AF_INET, only, &a.sin_addr);
            if (ntohl(a.sin_addr.s_addr) >> 24 == 127) lan_test = 1;   /* (a relay on loopback: announced to this PC's loopback only) */
            if (relay_sock == INVALID_SOCKET || bind(relay_sock, (struct sockaddr *)&a, sizeof a)) {
                snprintf(reject_text, sizeof reject_text, "cannot use port %u for the relay (in use?)", port);
                fprintf(stderr, "[net] %s\n", reject_text);
            } else {
                setsockopt(relay_sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
                no_connreset(relay_sock);
                big_sends(relay_sock);
                CloseHandle(CreateThread(NULL, 0, relay_thread, NULL, 0, NULL));
                fprintf(stderr, "[net] hosting the relay on UDP port %u, room %s\n", port, room);
                lan_announce_start();   /* (before UPnP's share file, which says whether the room is announced) */
                const char *upnp = getenv("SSBU_NET_UPNP");   /* 0: leave the router alone */
                if (upnp && !strcmp(upnp, "0")) { lan_address(); upnp_state = 3; }
                else CloseHandle(CreateThread(NULL, 0, upnp_thread, NULL, 0, NULL));
            }
        }
        client_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (client_sock != INVALID_SOCKET) {
            setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
            no_connreset(client_sock);
            int big = 1 << 20;
            setsockopt(client_sock, SOL_SOCKET, SO_RCVBUF, (const char *)&big, sizeof big);
            big_sends(client_sock);
            CloseHandle(CreateThread(NULL, 0, client_thread, NULL, 0, NULL));
            atexit(leave);
        }
    }
    InitOnceComplete(&once, 0, NULL);
}

int net_peer_quality(NetPeerQ *out, int max) {
    if (!configured || !joined) return 0;
    u64 t = now_ms();
    int n = 0;
    AcquireSRWLockShared(&peers_lock);
    for (int i = 0; i < npeers && n < max; i++) {
        const Peer *q = &peers[i];
        NetPeerQ *o = &out[n++];
        snprintf(o->name, sizeof o->name, "%s", q->name);
        int direct = usable(q, t);
        o->relayed = !direct;
        o->rtt_ms = direct ? (int)q->rtt : -1;
        /* (good: a direct path under 60 ms; so-so: up to 120 ms, or through the lobby's relay; poor: slower, or
         * nothing heard from them for over 2 s) */
        int quiet = q->last_ack && t - q->last_ack > 2000 && direct;
        o->quality = quiet ? 2 : !direct ? 1 : q->rtt <= 60 ? 0 : q->rtt <= 120 ? 1 : 2;
    }
    ReleaseSRWLockShared(&peers_lock);
    return n;
}

void net_status(char *out, size_t n) {
    start();
    if (!configured) { if (n) out[0] = 0; return; }
    if (reject_text[0] && !joined) { snprintf(out, n, "Netplay: %s%s%s", reject_text, reject_detail[0] ? "\n" : "", reject_detail); return; }
    if (lan_mode && !joined && lan_state != NET_LAN_FOUND) { snprintf(out, n, "Netplay: looking for a room on this network..."); return; }
    if (!joined && net_unreachable()) { snprintf(out, n, "Netplay: no answer from %s - playing offline (still trying)", relay_text); return; }
    if (!joined) { snprintf(out, n, "Netplay: connecting to %s...", relay_text); return; }
    if (lobby) {   /* (smash, the default code, is the public lobby) */
        char code[NAME_LEN + 16] = "";
        if (strcmp(room, "smash")) snprintf(code, sizeof code, " (lobby %s)", room);
        snprintf(out, n, "Online%s: %ld player%s, %ld room%s - you are %s, ping %ld ms", code, (long)members, members == 1 ? "" : "s",
                 (long)rooms_open, rooms_open == 1 ? "" : "s", name, (long)ping_ms);
        size_t l = strlen(out);   /* the players of this player's room: direct, or through the lobby */
        u64 t = now_ms();
        AcquireSRWLockShared(&peers_lock);
        int relayed_mine = 0;
        for (int i = 0; i < npeers && l + 1 < n; i++) {
            char st[48];
            relayed_mine |= peers[i].why == WHY_MINE || peers[i].why == WHY_BOTH;
            int w = snprintf(out + l, n - l, "%s%s %s", i ? ", " : "\nRoom: ", peers[i].name, peer_state(&peers[i], t, st));
            l = w < 0 ? n - 1 : l + (size_t)w < n ? l + (size_t)w : n - 1;
        }
        ReleaseSRWLockShared(&peers_lock);
        if (relayed_mine && l + 1 < n)   /* (what turns direct paths on) */
            snprintf(out + l, n - l, "\nYour router changes ports (symmetric NAT): relayed through the lobby. Turn on UPnP or NAT-PMP in the router for direct paths");
        return;
    }
    char share[192] = "";
    if (hosting) {
        LONG u = upnp_state;
        if (u == 0) snprintf(share, sizeof share, "  | opening port %u on the router...", port);
        else if (u == 1) snprintf(share, sizeof share, "  | friends join %s:%u", public_ip[0] ? public_ip : "(your public IP)", port);
        else if (u == 4) snprintf(share, sizeof share, "  | router behind another NAT: only same-network friends can join (%s:%u)", lan_ip, port);
        else if (u == 5) snprintf(share, sizeof share, "  | friends join %s:%u (STUN: if the router keeps the mapping)", stun_ip, stun_port);
        else snprintf(share, sizeof share, "  | UPnP %s: only same-network friends can join (%s:%u)", u == 2 ? "unavailable" : "refused", lan_ip, port);
        if (lan_state == NET_LAN_ANNOUNCING) snprintf(share + strlen(share), sizeof share - strlen(share), "  | announced on this network");
    }
    snprintf(out, n, "Netplay: room %s, %ld player%s, %s 10.13.%u.%u, ping %ld ms%s", room, (long)members, members == 1 ? "" : "s",
             name, my_ip >> 8 & 255, my_ip & 255, (long)ping_ms, share);
}
