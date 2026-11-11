/* The game's own Online mode (Quickplay, Battle Arenas, ...) against the NEX game server of
 * tools/nex, which runs inside the online lobby (tools/ssbu_online_lobby.py) at the lobby's
 * virtual address, 10.13.0.1.
 *
 * Online logs in with the network service account's id token (hle_env.c) to the game server
 * "https://g23380901-lp1.s.n.srv.nintendo.net/": NEX's PRUDP Lite, over a WebSocket, over TLS.
 * NEX has a second way in that no retail code uses: a direct server address in the NgsFacade
 * (host at +0x240, an nn::nex::String, and port at +0x268). Set, NEX opens the same WebSocket
 * without TLS to that address - a stream socket, which the stream module below carries to the
 * lobby over TCP. Pia's NAT check servers (nncs1, nncs2) resolve to the server too, and Pia's
 * matches run between the players' virtual addresses, as Local Wireless's do.
 *
 * SSBU_ONLINE_SERVER=host:port overrides the server (default 10.13.0.1:60000); "off" leaves
 * the game's own address (and Online fails, as offline).
 *
 * Online over Epic Online Services (online_server.txt "eos", plan/EOS_MIGRATION.md phase 2) has no server of ours:
 * the game server is in the game (net_nex*.c, tools/nex in C). Its streams to 10.13.0.1 go to that server instead of
 * a TCP connection, and the datagrams of Pia's NAT check to 10.13.0.1 / 10.13.255.254 are answered here (every player
 * "open", with its own virtual address and port: EOS does the real traversal). SSBU_NEX_LOCAL=1 does the same over
 * any network (tests), =0 never. */
#define _CRT_SECURE_NO_WARNINGS
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define closesocket close
#define SD_BOTH SHUT_RDWR
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "net.h"
#include "net_backend.h"
#include "net_eos.h"
#include "net_nex.h"
#include "net_eos_lobby.h"
#include "online.h"

#define DEFAULT_HOST "10.13.0.1"
#define DEFAULT_PORT 60000

/* The game server in the game: over EOS (no server of ours), or SSBU_NEX_LOCAL=1 */
static int nex_local(void) {
    const char *v = getenv("SSBU_NEX_LOCAL");
    if (v && *v) return atoi(v) != 0;
    return net_configured() && net_backend == &net_eos_backend;
}

/* The server to log in to: 0 when online play is off (it needs the online lobby: a private
 * room's relay in the host's game has no game server - or the one in the game, over EOS). */
static int online_server(char *host, size_t n, u16 *port) {
    const char *v = getenv("SSBU_ONLINE_SERVER");
    if (v && !strcmp(v, "off")) return 0;
    if (nex_local()) {   /* (the server in the game answers as 10.13.0.1 and nncs2 as 10.13.255.254, whatever was asked) */
        if (!net_configured()) return 0;
        snprintf(host, n, "%s", DEFAULT_HOST);
        *port = DEFAULT_PORT;
        return 1;
    }
    if (!v || !*v) {
        if (!net_configured() || !net_relay_address()) return 0;
        snprintf(host, n, "%s", DEFAULT_HOST);
        *port = DEFAULT_PORT;
        return 1;
    }
    const char *colon = strrchr(v, ':');
    size_t len = colon ? (size_t)(colon - v) : strlen(v);
    if (!len || len >= n) return 0;
    memcpy(host, v, len);
    host[len] = 0;
    *port = colon && atoi(colon + 1) > 0 && atoi(colon + 1) < 65536 ? (u16)atoi(colon + 1) : DEFAULT_PORT;
    return 1;
}

/* The server's IPv4 address (host order), 0 if it is not a dotted address. */
static u32 online_ip(void) {
    char host[64];
    u16 port;
    unsigned a, b, d, e;
    if (!online_server(host, sizeof host, &port) || sscanf(host, "%u.%u.%u.%u", &a, &b, &d, &e) != 4) return 0;
    return a << 24 | b << 16 | d << 8 | e;
}

/* Names of Nintendo's servers (the game server, the NAT check servers) are ours while online. */
static int nintendo_name(const char *name) { return name && strstr(name, "nintendo.net") != NULL; }

/* A dotted IPv4 address (host order), 0 if the name is not one. Station URLs carry addresses
 * this way, and the SDK's resolver (GetAddrInfo) does not work here. */
static u32 dotted_ip(const char *name) {
    unsigned a, b, d, e;
    char end;
    if (!name || sscanf(name, "%u.%u.%u.%u%c", &a, &b, &d, &e, &end) != 4 || (a | b | d | e) > 255) return 0;
    return a << 24 | b << 16 | d << 8 | e;
}

/* NEX's login job, the step that works out the game server's address (main+0x249190;
 * the job is x0, its NgsFacade at +360). Give the facade the direct address first. */
/* signature: 16 words, sha256 c9c626b15efbb3b4 */
HLE_WRAP(7100249190) {
    u64 facade = ld64(c->x[0] + 360);
    char host[64];
    u16 port;
    if (facade && online_server(host, sizeof host, &port)) {
        u64 str = facade + 576;   /* nn::nex::String { vtable; char local[16]; char *data; u64 capacity } */
        u64 data = ld64(str + 24), capacity = ld64(str + 32);
        if (data && capacity > strlen(host)) {
            memcpy(P(char, data), host, strlen(host) + 1);
            st16(facade + 616, port);
            if (net_log()) fprintf(stderr, "[online] NEX login to %s:%u\n", host, port);
        } else if (net_log()) {
            fprintf(stderr, "[online] NEX server address left as the game's: no room for \"%s\"\n", host);
        }
    }
    f_7100249190_orig(c);
}

/* NEX's InetAddress from a name (main+0x1becd0; x0 the address, x1 the name -> bool):
 * the address is at +12, in network order. */
/* signature: 16 words, sha256 324bea721ad32d31 */
HLE_WRAP(71001becd0) {
    const char *name = c->x[1] ? P(const char, c->x[1]) : NULL;
    int ours = nintendo_name(name);
    u32 ip = ours ? online_ip() : dotted_ip(name);
    if (net_log() && ours) fprintf(stderr, "[online] NEX resolves \"%s\"%s\n", name, ip ? " -> the online server" : "");
    if (ip) {
        st32(c->x[0] + 12, _byteswap_ulong(ip));
        c->x[0] = 1;
        return;
    }
    f_71001becd0_orig(c);
}

/* Pia's name resolution (main+0xba850; x0 the name, x4 a Pia InetAddress, whose IPv4
 * address is at +8 in host order -> bool): the NAT check servers are the online server too.
 * Pia's NAT check needs its two servers at different addresses: nncs2 gets the last address of
 * the server's /16 (10.13.255.254), which the online lobby keeps for it (tools/nex/server.py). */
/* signature: 16 words, sha256 ea3c274364ccadf2 */
HLE_WRAP(71000ba850) {
    const char *name = c->x[0] ? P(const char, c->x[0]) : NULL;
    int ours = nintendo_name(name);
    u32 ip = ours ? online_ip() : dotted_ip(name);
    if (ip && ours && !strncmp(name, "nncs2", 5)) ip = (ip & 0xFFFF0000) | 0xFFFE;
    if (net_log() && ours) fprintf(stderr, "[online] Pia resolves \"%s\"%s\n", name, ip ? " -> the online server" : "");
    if (ip) {
        if (c->x[4]) {
            memset(P(u8, c->x[4] + 8), 0, 16);
            st32(c->x[4] + 8, ip);
        }
        c->x[0] = 1;
        return;
    }
    f_71000ba850_orig(c);
}

/* ---- nn::crypto: SHA-1 ------------------------------------------------------------
 * The SDK's uses the CPU's SHA-1 instructions, which the recompiler does not translate; NEX's
 * WebSocket checks the server's Sec-WebSocket-Accept with it. */
static u32 rol(u32 x, int n) { return x << n | x >> (32 - n); }

static void sha1_block(u32 h[5], const u8 *p) {
    u32 w[80];
    for (int i = 0; i < 16; i++) w[i] = (u32)p[4 * i] << 24 | (u32)p[4 * i + 1] << 16 | (u32)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        u32 f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        u32 t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

static void sha1(u8 out[20], const u8 *data, u64 len) {
    u32 h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    u64 i = 0;
    for (; i + 64 <= len; i += 64) sha1_block(h, data + i);
    u8 tail[128] = {0};
    u64 rest = len - i;
    memcpy(tail, data + i, (size_t)rest);
    tail[rest] = 0x80;
    u64 n = rest + 9 <= 64 ? 64 : 128, bits = len * 8;
    for (int k = 0; k < 8; k++) tail[n - 1 - k] = (u8)(bits >> (8 * k));
    sha1_block(h, tail);
    if (n == 128) sha1_block(h, tail + 64);
    for (int k = 0; k < 5; k++) { out[4 * k] = (u8)(h[k] >> 24); out[4 * k + 1] = (u8)(h[k] >> 16); out[4 * k + 2] = (u8)(h[k] >> 8); out[4 * k + 3] = (u8)h[k]; }
}

HLE(_ZN2nn6crypto16GenerateSha1HashEPvmPKvm) {   /* (void *hash, u64 hash size, const void *data, u64 size) */
    u8 h[20];
    sha1(h, P(u8, c->x[2]), c->x[3]);
    memcpy(P(void, c->x[0]), h, c->x[1] < 20 ? (size_t)c->x[1] : 20);
}

/* ---- NEX's stream sockets ---------------------------------------------------------
 * A stream socket connected to the online server is a TCP connection to the lobby
 * (SSBU_NET_RELAY's host, same port number as its UDP), which starts with "SSBUNEX1", this
 * player's virtual address and port and the server port asked for (big-endian u32, u16, u16).
 * A thread per stream connects and then reads into the stream's buffer. */
enum { E_AGAIN_ = 11, E_MFILE_ = 24, E_PIPE_ = 32, E_CONNRESET_ = 104, E_ISCONN_ = 106, E_NOTCONN_ = 107,
       E_CONNREFUSED_ = 111, E_ALREADY_ = 114, E_INPROGRESS_ = 115 };
enum { POLLIN_ = 0x1, POLLOUT_ = 0x4, POLLERR_ = 0x8, POLLHUP_ = 0x10, POLLRDNORM_ = 0x40 };
enum { S_CONNECTING = 1, S_OPEN, S_FAILED };
#define CONNECT_MS 5000

typedef struct Stream {
    i32 fd;                  /* 0: free */
    u32 gen;                 /* (tells a reused slot from the one a thread was started for) */
    int state, err, eof, reported;
    SOCKET s;
    u8 *buf;
    u32 len, cap;
    u32 vip;
    u16 vport, dport;
    /* (rollback, online_rb_*) what the game read and sent, for a savestate's load: the last HIST bytes of each */
    u64 taken, sent, resend;   /* (resend < sent after a load: the game sends again what it had sent; left out while it matches) */
    u8 *hist, *shist;
    int local;               /* to the game server in the game (nex_local) */
} Stream;
#define STREAMS 16
static Stream streams[STREAMS];
static SRWLOCK stream_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE stream_change = CONDITION_VARIABLE_INIT;
static u32 stream_gen;

/* ---- rollback (SSBU_RB): a stream through a savestate's load ---------------------------------------
 * A load puts the game's memory back as it was at a save, NEX's state with it: what it read from a stream
 * since is forgotten, and what it sent since it will send again. The stream itself can't go back, so it is
 * the stream's ends that are made to agree: the bytes the game read after the save are put back in front of
 * what is waiting (it reads them again), and the bytes it sends again are left out as long as they are the
 * ones it had sent (once they differ, the rest goes: the lobby's NEX server then sees a packet twice, which
 * it takes as a repeat). Kept only once savestates are taken (online_rb_save), HIST bytes a stream. */
#define HIST (256u << 10)
static int rb_keep;
static long long rb_back_bytes, rb_resent_left_out, rb_resend_differ;
static void hist_put(u8 **ring, u64 at, const u8 *data, u64 n) {
    if (!*ring && !(*ring = malloc(HIST))) return;
    for (u64 i = 0; i < n; i++) (*ring)[(at + i) % HIST] = data[i];
}

static Stream *stream_of(i32 fd) {
    for (int i = 0; i < STREAMS; i++)
        if (streams[i].fd == fd) return &streams[i];
    return NULL;
}

static void changed(void) {
    WakeAllConditionVariable(&stream_change);
    net_wake();
}

/* ---- the game server in the game (net_nex.h) ----------------------------------------------------------
 * One server, its calls under nex_lock (taken before stream_lock, never inside it). What it sends to a stream goes
 * into the stream's buffer, as the lobby's bytes would; its datagrams to this player's sockets. */
static SRWLOCK nex_lock = SRWLOCK_INIT;
static NexServer *nex_srv;
static int nex_woke;
static void nex_stream_out(void *ctx, u32 ip, u16 port, u16 sport, const u8 *data, size_t n) {
    (void)ctx;
    AcquireSRWLockExclusive(&stream_lock);
    for (int i = 0; i < STREAMS; i++) {
        Stream *st = &streams[i];
        if (!st->fd || !st->local || st->vip != ip || st->vport != port || st->dport != sport || st->eof) continue;
        if (st->len + n > (16u << 20)) { st->eof = 1; nex_woke = 1; break; }   /* (a game that does not read: the stream ends) */
        if (st->len + n > st->cap) {
            u32 cap = st->cap ? st->cap : 65536;
            while (cap < st->len + n) cap *= 2;
            u8 *b = realloc(st->buf, cap);
            if (!b) { st->eof = 1; break; }
            st->buf = b;
            st->cap = cap;
        }
        memcpy(st->buf + st->len, data, n);
        st->len += (u32)n;
        nex_woke = 1;
        break;
    }
    ReleaseSRWLockExclusive(&stream_lock);
}
static void nex_datagram_out(void *ctx, u32 ip, u16 port, u32 src_ip, u16 sport, const u8 *data, size_t n) {
    (void)ctx;
    vsock_deliver(src_ip, sport, ip, port, data, (u32)n);
}
static const char *nex_part_of(void *ctx, u32 ip) {   /* (who may meet: game build, lobby code, simulation version, mods) */
    static char part[64];
    (void)ctx; (void)ip;
    if (!part[0]) net_part_key(part, sizeof part);
    return part;
}
static int nex_on_eos;   /* (its gatherings in EOS Lobbies: net_eos_lobby.c) */
/* The players' fighter records (their GSP, DataStore 82 / 84) in nex_powers.bin beside the save folder, as
 * netplay_id.txt: the game asks for them at each login and shows the GSP they hold. */
static void nex_powers_path(char *path, size_t n) {
    const char *root = getenv("SSBU_DATA_ROOT");
    if (root) snprintf(path, n, "%s/nex_powers.bin", root);
    else snprintf(path, n, "%s/../../nex_powers.bin", rt_data_dir);
}
static void nex_powers_load(NexServer *ns) {
    char path[1024];
    nex_powers_path(path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    static u8 buf[NEX_POWER_ENTRY * 4096];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    nex_server_set_powers(ns, buf, n);
}
static void nex_powers_changed(void *ctx) {   /* (under nex_lock) */
    (void)ctx;
    size_t n;
    const u8 *d = nex_server_powers(nex_srv, &n);
    char path[1024], tmp[1040];
    nex_powers_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    int ok = fwrite(d, 1, n, f) == n;
    ok &= fclose(f) == 0;
    if (ok) MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
}
static void nex_log_line(void *ctx, int level, const char *line) { (void)ctx; (void)level; if (net_log()) fprintf(stderr, "[nex] %s\n", line); }
/* the server, under nex_lock, its clock set (NULL: none could be made) */
static NexServer *nex_enter(void) {
    AcquireSRWLockExclusive(&nex_lock);
    if (!nex_srv) {
        NexIo io = {NULL, nex_stream_out, nex_datagram_out, nex_part_of, nex_log_line, NULL, NULL, nex_powers_changed};
        nex_on_eos = net_backend == &net_eos_backend;
        nex_srv = nex_server_new(&io, nex_on_eos ? nex_store_eos() : nex_store_local(), net_log());
        /* No idle timeout: the only connections are this game's own, which end when it closes its stream
         * (online_stream_close). The game paces its keep-alive pings on its own clock, which in a rollback fight is
         * the fixed clock (kernel.c rt_ticks: one 60 Hz frame a logic frame, set back by every load): with a slow
         * link it falls far behind the wall clock (2026-11-11, SSBU_NET_SIM=130,1,15 at 24 fps shown: 431 logic
         * frames, 7 s of the game's time, in 40 s), and tools/nex's 40 s without a packet closed a live connection
         * in the middle of a fight (error 2306-0332). */
        if (nex_srv) nex_server_set_idle_timeout(nex_srv, 0);
        if (nex_srv) nex_powers_load(nex_srv);
        if (net_log()) fprintf(stderr, "[online] the game server is in the game (Online over %s)\n", net_backend == &net_eos_backend ? "EOS" : "SSBU_NEX_LOCAL");
    }
    if (nex_srv) {
        static u64 ticked, identity_set;
        u64 now = GetTickCount64();
        nex_server_set_now(nex_srv, (double)now / 1000.0);
        if (now - ticked >= 1000) { ticked = now; nex_server_tick(nex_srv, (double)now / 1000.0); }   /* (idle connections, old rounds) */
        const char *id = !identity_set && net_backend == &net_eos_backend ? eos_my_id() : NULL;   /* (the PID: from the EOS id, else the token's) */
        if (id) { nex_server_set_identity(nex_srv, id); identity_set = 1; }
        if (nex_on_eos) nex_store_eos_sync(nex_srv);
    }
    nex_woke = 0;
    return nex_srv;
}
static void nex_leave(void) {
    int woke = nex_woke;
    nex_woke = 0;
    ReleaseSRWLockExclusive(&nex_lock);
    if (woke) changed();
}
/* (net_eos_lobby.c, on the EOS thread) what EOS brought, to the server now if nothing holds it */
int online_nex_deliver(void) {
    if (!TryAcquireSRWLockExclusive(&nex_lock)) return 0;
    nex_woke = 0;
    if (nex_srv && nex_on_eos) {
        nex_server_set_now(nex_srv, (double)GetTickCount64() / 1000.0);
        nex_store_eos_sync(nex_srv);
    }
    nex_leave();
    return 1;
}

/* (hle_net.c's sendto) A datagram for the game server's addresses while it is in the game: answered here. */
int online_nex_datagram(u32 src_ip, u16 sport, u32 dst, u16 dport, const void *data, u32 len) {
    if ((dst & 0xFFFF) != 0x0001 && (dst & 0xFFFF) != 0xFFFE) return 0;   /* (every datagram of the game comes here: the rest at once) */
    u32 server = online_ip();
    if (!server || (dst != server && dst != ((server & 0xFFFF0000u) | 0xFFFE)) || !nex_local()) return 0;
    NexServer *s = nex_enter();
    if (s) nex_datagram(s, src_ip, sport, dport, dst == server ? NEX_HOST : NEX_NAT_SECOND, data, len);
    nex_leave();
    return 1;
}

/* The lobby's real address: host[:port], from SSBU_NET_RELAY or online_server.txt (net_relay.c). */
static int lobby_address(struct sockaddr_in *out) {
    const char *v = net_relay_address();
    char host[256];
    if (!v || !*v) return 0;
    snprintf(host, sizeof host, "%s", v);
    u16 port = NET_DEFAULT_PORT;
    char *colon = strrchr(host, ':');
    if (colon) { port = (u16)atoi(colon + 1); *colon = 0; }
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) || !res) return 0;
    *out = *(struct sockaddr_in *)res->ai_addr;
    out->sin_port = htons(port);
    freeaddrinfo(res);
    return 1;
}

/* Connects s to the lobby within CONNECT_MS (a lobby that does not answer fails the connect, not
 * hangs it), and leaves it blocking. */
#ifdef _WIN32
static int connect_within(SOCKET s, const struct sockaddr_in *at) {
    u_long nb = 1;
    int ok = ioctlsocket(s, FIONBIO, &nb) == 0;
    if (ok && connect(s, (const struct sockaddr *)at, sizeof *at) != 0) {
        fd_set w, e;
        FD_ZERO(&w); FD_ZERO(&e);
        FD_SET(s, &w); FD_SET(s, &e);
        struct timeval tv = {CONNECT_MS / 1000, CONNECT_MS % 1000 * 1000};
        ok = WSAGetLastError() == WSAEWOULDBLOCK && select(0, NULL, &w, &e, &tv) == 1 && FD_ISSET(s, &w);
    }
    nb = 0;
    return ok && ioctlsocket(s, FIONBIO, &nb) == 0;
}
#else
static int connect_within(SOCKET s, const struct sockaddr_in *at) {
    int flags = fcntl(s, F_GETFL, 0);
    int ok = flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
    if (ok && connect(s, (const struct sockaddr *)at, sizeof *at) != 0) {   /* (a failed one polls as writable, with SO_ERROR set) */
        struct pollfd p = {s, POLLOUT, 0};
        int err = 0;
        socklen_t n = sizeof err;
        ok = errno == EINPROGRESS && poll(&p, 1, CONNECT_MS) == 1 && getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &n) == 0 && !err;
    }
    return ok && fcntl(s, F_SETFL, flags) == 0;
}
#endif

static DWORD WINAPI stream_thread(void *arg) {
    Stream *st = arg;
    AcquireSRWLockExclusive(&stream_lock);
    i32 fd = st->fd;
    u32 gen = st->gen, vip = st->vip;
    u16 vport = st->vport, dport = st->dport;
    ReleaseSRWLockExclusive(&stream_lock);
    struct sockaddr_in at;
    SOCKET s = INVALID_SOCKET;
    int ok = lobby_address(&at);
    if (ok) {
        s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        ok = s != INVALID_SOCKET && connect_within(s, &at);
    }
    if (ok) {
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
        u8 head[16] = {'S', 'S', 'B', 'U', 'N', 'E', 'X', '1', (u8)(vip >> 24), (u8)(vip >> 16), (u8)(vip >> 8), (u8)vip,
                       (u8)(vport >> 8), (u8)vport, (u8)(dport >> 8), (u8)dport};
        ok = send(s, (const char *)head, sizeof head, 0) == (int)sizeof head;
    }
    AcquireSRWLockExclusive(&stream_lock);
    if (st->fd != fd || st->gen != gen) {   /* (closed meanwhile) */
        ReleaseSRWLockExclusive(&stream_lock);
        if (s != INVALID_SOCKET) closesocket(s);
        return 0;
    }
    if (ok) { st->s = s; st->state = S_OPEN; }
    else { st->state = S_FAILED; st->err = E_CONNREFUSED_; if (s != INVALID_SOCKET) closesocket(s); s = INVALID_SOCKET; }
    ReleaseSRWLockExclusive(&stream_lock);
    if (net_log()) fprintf(stderr, "[online] stream %d to port %u: %s\n", fd, dport, ok ? "connected to the lobby" : "the lobby did not answer");
    changed();
    if (!ok) return 0;
    char chunk[16384];
    for (;;) {
        int n = recv(s, chunk, sizeof chunk, 0);
        AcquireSRWLockExclusive(&stream_lock);
        if (st->fd != fd || st->gen != gen) { ReleaseSRWLockExclusive(&stream_lock); break; }
        if (n <= 0) {
            st->eof = 1;
            ReleaseSRWLockExclusive(&stream_lock);
            changed();
            break;
        }
        if (st->len + (u32)n > st->cap) {
            u32 cap = st->cap ? st->cap : 65536;
            while (cap < st->len + (u32)n) cap *= 2;
            u8 *b = realloc(st->buf, cap);
            if (!b) { st->eof = 1; ReleaseSRWLockExclusive(&stream_lock); changed(); break; }
            st->buf = b;
            st->cap = cap;
        }
        memcpy(st->buf + st->len, chunk, (size_t)n);
        st->len += (u32)n;
        ReleaseSRWLockExclusive(&stream_lock);
        changed();
    }
    return 0;
}

int online_stream_connect(i32 fd, u32 ip, u16 port, u32 my_ip, u16 my_port, int nonblock) {
    u32 server = online_ip();
    if (!server || ip != server) return E_CONNREFUSED_;   /* (no other stream reaches anything) */
    if (net_unreachable() || !my_ip) return E_CONNREFUSED_;   /* (the lobby is gone: Online is too) */
    int local = nex_local();
    NexServer *ns = local ? nex_enter() : NULL;   /* (before stream_lock: the lock order) */
    if (local && !ns) { nex_leave(); return E_CONNREFUSED_; }
    AcquireSRWLockExclusive(&stream_lock);
    Stream *st = stream_of(fd);
    if (st) {
        int e = st->state == S_OPEN ? E_ISCONN_ : E_ALREADY_;
        ReleaseSRWLockExclusive(&stream_lock);
        if (local) nex_leave();
        return e;
    }
    for (int i = 0; i < STREAMS && !st; i++)
        if (!streams[i].fd) st = &streams[i];
    if (!st) { ReleaseSRWLockExclusive(&stream_lock); if (local) nex_leave(); return E_MFILE_; }
    memset(st, 0, sizeof *st);
    st->fd = fd;
    st->gen = ++stream_gen;
    st->state = S_CONNECTING;
    st->s = INVALID_SOCKET;
    st->vip = my_ip;
    st->vport = my_port;
    st->dport = port;
    if (local) {   /* (connected at once, to the server in the game; as with a socket that connects quickly, a
                      non-blocking connect still says "in progress" and polls writable) */
        st->local = 1;
        st->state = S_OPEN;
        ReleaseSRWLockExclusive(&stream_lock);
        int ok = nex_stream_open(ns, my_ip, my_port, port);
        nex_leave();
        if (!ok) {   /* (one the server holds already: this slot goes, the other stays) */
            AcquireSRWLockExclusive(&stream_lock);
            if ((st = stream_of(fd)) != NULL) st->local = 0;
            ReleaseSRWLockExclusive(&stream_lock);
            online_stream_close(fd);
            return E_CONNREFUSED_;
        }
        if (net_log()) fprintf(stderr, "[online] stream %d to port %u: the game server in the game\n", fd, port);
        changed();
        return nonblock ? E_INPROGRESS_ : 0;
    }
    ReleaseSRWLockExclusive(&stream_lock);
    HANDLE t = CreateThread(NULL, 0, stream_thread, st, 0, NULL);
    if (!t) { online_stream_close(fd); return E_CONNREFUSED_; }
    CloseHandle(t);
    if (nonblock) return E_INPROGRESS_;
    AcquireSRWLockExclusive(&stream_lock);
    while (st->fd == fd && st->state == S_CONNECTING) SleepConditionVariableSRW(&stream_change, &stream_lock, INFINITE, 0);
    int err = st->fd == fd && st->state == S_FAILED ? st->err : 0;
    if (err) st->reported = 1;
    ReleaseSRWLockExclusive(&stream_lock);
    return err;
}

i64 online_stream_send(i32 fd, const void *data, u64 len, int nonblock) {
    AcquireSRWLockExclusive(&stream_lock);
    Stream *st = stream_of(fd);
    while (st && st->state == S_CONNECTING && !nonblock) {
        SleepConditionVariableSRW(&stream_change, &stream_lock, INFINITE, 0);
        st = stream_of(fd);
    }
    i64 r = !st ? -E_NOTCONN_ : st->state == S_CONNECTING ? -E_AGAIN_ : st->state != S_OPEN || st->eof ? -E_PIPE_ : 0;
    SOCKET s = st && !r ? st->s : INVALID_SOCKET;
    int local = st && !r && st->local;
    u32 vip = st ? st->vip : 0;
    u16 vport = st ? st->vport : 0, dport = st ? st->dport : 0;
    const char *p = data;
    u64 left = len;
    if (!r && rb_keep) {   /* (after a load: what the game sends again) */
        while (left && st->resend < st->sent) {
            if (st->shist && (u8)*p == st->shist[st->resend % HIST]) { p++; left--; st->resend++; rb_resent_left_out++; continue; }
            rb_resend_differ++;
            if (rb_resend_differ <= 5) fprintf(stderr, "[online] stream %d: after a load the game sends other bytes than before (%llu of %llu again matched): the rest goes\n",
                                              fd, (unsigned long long)(st->resend), (unsigned long long)st->sent);
            st->resend = st->sent;
            if (st->local) { p = data; left = len; }   /* (the server in the game takes this call's bytes whole again - a
                                                          WebSocket frame, whose mask differs at once: a repeat it ignores,
                                                          not the rest of a frame it cannot read) */
        }
        if (left) { hist_put(&st->shist, st->sent, (const u8 *)p, left); st->sent += left; st->resend = st->sent; }
    }
    ReleaseSRWLockExclusive(&stream_lock);
    if (r) return r;
    if (!left) return (i64)len;
    if (local) {   /* (to the game server in the game: its answers are in the stream's buffer when this returns) */
        NexServer *ns = nex_enter();
        int ok = ns && nex_stream_bytes(ns, vip, vport, dport, (const u8 *)p, left);
        if (!ok && ns) nex_stream_closed(ns, vip, vport, dport);   /* (not NEX: the lobby would close it too) */
        nex_leave();
        if (!ok) {
            AcquireSRWLockExclusive(&stream_lock);
            if ((st = stream_of(fd)) != NULL) st->eof = 1;
            ReleaseSRWLockExclusive(&stream_lock);
            changed();
        }
        if (net_log() > 1) fprintf(stderr, "[online] stream %d: sent %llu bytes to the game server in the game\n", fd, (unsigned long long)len);
        return (i64)len;
    }
    while (left) {   /* (the host socket blocks: everything goes, in order) */
        int n = send(s, p, left > 0x10000 ? 0x10000 : (int)left, 0);
        if (n <= 0) return len - left ? (i64)(len - left) : -E_CONNRESET_;
        p += n;
        left -= (u64)n;
    }
    if (net_log() > 1) fprintf(stderr, "[online] stream %d: sent %llu bytes\n", fd, (unsigned long long)len);
    return (i64)len;
}

i64 online_stream_recv(i32 fd, void *buf, u64 len, int peek, int nonblock) {
    AcquireSRWLockExclusive(&stream_lock);
    Stream *st;
    for (;;) {
        st = stream_of(fd);
        if (!st || st->len || st->eof || st->state == S_FAILED || nonblock) break;
        SleepConditionVariableSRW(&stream_change, &stream_lock, INFINITE, 0);
    }
    i64 r;
    if (!st) r = -E_NOTCONN_;
    else if (st->len) {
        u32 n = st->len < len ? st->len : (u32)len;
        memcpy(buf, st->buf, n);
        if (!peek) {
            if (rb_keep) { hist_put(&st->hist, st->taken, st->buf, n); st->taken += n; }
            memmove(st->buf, st->buf + n, st->len - n); st->len -= n;
        }
        r = n;
    } else if (st->eof) r = 0;
    else if (st->state == S_FAILED) r = -st->err;
    else r = -E_AGAIN_;
    ReleaseSRWLockExclusive(&stream_lock);
    if (net_log() > 1 && r > 0) fprintf(stderr, "[online] stream %d: received %lld bytes\n", fd, (long long)r);
    return r;
}

u16 online_stream_revents(i32 fd, u16 events) {
    AcquireSRWLockShared(&stream_lock);
    Stream *st = stream_of(fd);
    u16 r = 0;
    if (!st) r = POLLHUP_;
    else {
        if (st->state == S_OPEN && !st->eof) r |= events & POLLOUT_;
        if (st->len || st->eof) r |= events & (POLLIN_ | POLLRDNORM_);
        if (st->eof) r |= POLLHUP_;
        if (st->state == S_FAILED) r |= POLLERR_ | (events & POLLOUT_);
    }
    ReleaseSRWLockShared(&stream_lock);
    return r;
}

int online_stream_error(i32 fd) {
    AcquireSRWLockExclusive(&stream_lock);
    Stream *st = stream_of(fd);
    int err = 0;
    if (st && st->state == S_FAILED && !st->reported) { err = st->err; st->reported = 1; }
    ReleaseSRWLockExclusive(&stream_lock);
    return err;
}

void online_stream_close(i32 fd) {
    AcquireSRWLockExclusive(&stream_lock);
    Stream *st = stream_of(fd);
    SOCKET s = INVALID_SOCKET;
    int local = st && st->local;
    u32 vip = st ? st->vip : 0;
    u16 vport = st ? st->vport : 0, dport = st ? st->dport : 0;
    if (st) {
        s = st->s;
        free(st->buf); free(st->hist); free(st->shist);
        memset(st, 0, sizeof *st);   /* (its thread sees its fd gone and stops) */
    }
    ReleaseSRWLockExclusive(&stream_lock);
    if (s != INVALID_SOCKET) { shutdown(s, SD_BOTH); closesocket(s); }
    if (local) {   /* (the server's connections from it go: the others of its gathering are told) */
        NexServer *ns = nex_enter();
        if (ns) nex_stream_closed(ns, vip, vport, dport);
        nex_leave();
    }
    if (st) changed();
}

/* (hle_net.c's net_rb_save / net_rb_load, rollback) Each stream's place in what it read and sent. */
void online_rb_save(OnlineRb *o) {
    AcquireSRWLockExclusive(&stream_lock);
    rb_keep = 1;
    for (int i = 0; i < STREAMS && i < ONLINE_RB_STREAMS; i++) {
        o->s[i].fd = streams[i].fd; o->s[i].gen = streams[i].gen;
        o->s[i].taken = streams[i].taken; o->s[i].sent = streams[i].sent;
    }
    ReleaseSRWLockExclusive(&stream_lock);
}
void online_rb_load(const OnlineRb *o) {
    int any = 0;
    AcquireSRWLockExclusive(&stream_lock);
    for (int i = 0; i < STREAMS && i < ONLINE_RB_STREAMS; i++) {
        Stream *st = &streams[i];
        if (!o->s[i].fd || st->fd != o->s[i].fd || st->gen != o->s[i].gen) continue;
        u64 back = st->taken - o->s[i].taken;
        if (back && back <= HIST && st->hist) {   /* (read since the save: in front of what is waiting, to be read again) */
            if (st->len + back > st->cap) {
                u32 cap = st->cap ? st->cap : 65536;
                while (cap < st->len + back) cap *= 2;
                u8 *b = realloc(st->buf, cap);
                if (!b) continue;
                st->buf = b; st->cap = cap;
            }
            memmove(st->buf + back, st->buf, st->len);
            for (u64 k = 0; k < back; k++) st->buf[k] = st->hist[(o->s[i].taken + k) % HIST];
            st->len += (u32)back; st->taken = o->s[i].taken;
            rb_back_bytes += (long long)back; any = 1;
        } else if (back) fprintf(stderr, "[online] stream %d: a load %llu bytes back: more than kept, not read again\n", st->fd, (unsigned long long)back);
        if (o->s[i].sent < st->sent && st->sent - o->s[i].sent <= HIST) st->resend = o->s[i].sent;
    }
    ReleaseSRWLockExclusive(&stream_lock);
    if (any) changed();
}
void online_rb_counts(long long *back, long long *left_out, long long *differ) { *back = rb_back_bytes; *left_out = rb_resent_left_out; *differ = rb_resend_differ; }
