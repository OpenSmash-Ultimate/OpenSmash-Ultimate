/* The in-game NEX server (runtime/net_nex*.c) driven by commands on stdin, one a line, for tests/test_nex_c.py: what it
 * sends goes to stdout, each command's output ending with a "." line.
 *
 *   init <seed hex> <datetime> <next round> <verbose>   a server: test randomness (MD5 of seed + counter), a fixed clock
 *   now <seconds>                                         the monotonic clock
 *   part <ip> <text | ->                                  who may meet whom, for players at ip ("-": nobody's)
 *   open <ip> <port> <server port>                        a stream: "ret 1" (0: one is open already)
 *   pkt <ip> <port> <server port> <hex>                   one PRUDP Lite packet from a stream
 *   bytes <ip> <port> <server port> <hex>                 a stream's bytes (raw Lite or WebSocket): "ret 1" (0: close it)
 *   close <ip> <port> <server port>                       the stream closed
 *   dgram <ip> <port> <dport> <to ip> <hex>               a datagram to the server's address or nncs2's
 *   tick <seconds>
 *   pid <key>                                             nex_pid_of
 *
 * Out: "s <ip> <port> <server port> <hex>" (to a stream), "d <ip> <port> <from ip> <from port> <hex>" (a datagram). */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../runtime/net_nex.h"
#include "../../runtime/net_nex_int.h"

static uint8_t seed[64];
static size_t seed_n;
static uint64_t counter;
static uint8_t pool[16];
static size_t pool_left;
static uint64_t fixed_datetime;
static struct { uint32_t ip; char *part; } parts[256];
static int nparts;

static void rng(void *ctx, uint8_t *out, size_t n) {
    (void)ctx;
    for (size_t i = 0; i < n; i++) {
        if (!pool_left) {
            uint8_t buf[72];
            memcpy(buf, seed, seed_n);
            for (int k = 0; k < 8; k++) buf[seed_n + k] = (uint8_t)(counter >> (8 * k));
            nex_md5(buf, seed_n + 8, pool);
            counter++;
            pool_left = 16;
        }
        out[i] = pool[16 - pool_left--];
    }
}
static uint64_t clock_now(void *ctx) { (void)ctx; return fixed_datetime; }
static void ip_text(uint32_t ip, char out[16]) { snprintf(out, 16, "%u.%u.%u.%u", ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255); }
static void hex_out(const uint8_t *d, size_t n) {
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { putchar(H[d[i] >> 4]); putchar(H[d[i] & 15]); }
}
static void stream_out(void *ctx, uint32_t ip, uint16_t port, uint16_t sport, const uint8_t *d, size_t n) {
    (void)ctx;
    char t[16];
    ip_text(ip, t);
    printf("s %s %u %u ", t, port, sport);
    hex_out(d, n);
    putchar('\n');
}
static void datagram_out(void *ctx, uint32_t ip, uint16_t port, uint32_t src, uint16_t sport, const uint8_t *d, size_t n) {
    (void)ctx;
    char t[16], f[16];
    ip_text(ip, t);
    ip_text(src, f);
    printf("d %s %u %s %u ", t, port, f, sport);
    hex_out(d, n);
    putchar('\n');
}
static const char *part_of(void *ctx, uint32_t ip) {
    (void)ctx;
    for (int i = 0; i < nparts; i++) if (parts[i].ip == ip) return parts[i].part;
    return NULL;
}
static void log_line(void *ctx, int level, const char *line) { (void)ctx; fprintf(stderr, "[nex%d] %s\n", level, line); }

static uint32_t parse_ip(const char *t) {
    unsigned a, b, c, d;
    return sscanf(t, "%u.%u.%u.%u", &a, &b, &c, &d) == 4 ? a << 24 | b << 16 | c << 8 | d : 0;
}
static size_t unhex(const char *t, uint8_t *out) {
    size_t n = 0;
    for (; t[0] && t[1]; t += 2) {
        unsigned v;
        if (sscanf(t, "%2x", &v) != 1) break;
        out[n++] = (uint8_t)v;
    }
    return n;
}
static char *read_line(void) {
    static char *buf;
    static size_t cap;
    size_t n = 0;
    int ch;
    while ((ch = getchar()) != EOF && ch != '\n') {
        if (n + 2 > cap) { cap = cap ? cap * 2 : 4096; buf = realloc(buf, cap); if (!buf) abort(); }
        if (ch != '\r') buf[n++] = (char)ch;
    }
    if (ch == EOF && !n) return NULL;
    if (!buf) { cap = 16; buf = malloc(cap); }
    buf[n] = 0;
    return buf;
}

int main(void) {
    NexServer *s = NULL;
    setvbuf(stdout, NULL, _IOFBF, 1 << 20);
    char *line;
    while ((line = read_line())) {
        char cmd[16] = "", a[64] = "", b[64] = "", c[64] = "", d[64] = "";
        int off = 0;
        sscanf(line, "%15s%n", cmd, &off);
        const char *rest = line + off;
        uint8_t *data = malloc(strlen(line) / 2 + 1);
        if (!data) abort();
        if (!strcmp(cmd, "init")) {
            char seedhex[130];
            unsigned long long dt, round;
            int verbose;
            sscanf(rest, "%129s %llu %llu %d", seedhex, &dt, &round, &verbose);
            seed_n = unhex(seedhex, seed);
            counter = 0; pool_left = 0;
            fixed_datetime = dt;
            if (s) nex_server_free(s);
            NexIo io = {NULL, stream_out, datagram_out, part_of, log_line, rng, clock_now};
            s = nex_server_new(&io, nex_store_local(), verbose);
            nex_server_set_round_base(s, round);
        } else if (!strcmp(cmd, "now")) {
            nex_server_set_now(s, atof(rest));
        } else if (!strcmp(cmd, "part")) {
            sscanf(rest, "%63s %63s", a, b);
            uint32_t ip = parse_ip(a);
            int i = 0;
            while (i < nparts && parts[i].ip != ip) i++;
            if (i == nparts && nparts < 256) nparts++;
            free(parts[i].part);
            parts[i].ip = ip;
            parts[i].part = strcmp(b, "-") ? nex_strdup(b) : NULL;
        } else if (!strcmp(cmd, "open")) {
            sscanf(rest, "%63s %63s %63s", a, b, c);
            printf("ret %d\n", nex_stream_open(s, parse_ip(a), (uint16_t)atoi(b), (uint16_t)atoi(c)));
        } else if (!strcmp(cmd, "pkt") || !strcmp(cmd, "bytes")) {
            char *hex = malloc(strlen(rest) + 1);
            sscanf(rest, "%63s %63s %63s %s", a, b, c, hex);
            size_t n = unhex(hex, data);
            free(hex);
            if (cmd[0] == 'p') nex_stream_packet(s, parse_ip(a), (uint16_t)atoi(b), (uint16_t)atoi(c), data, n);
            else printf("ret %d\n", nex_stream_bytes(s, parse_ip(a), (uint16_t)atoi(b), (uint16_t)atoi(c), data, n));
        } else if (!strcmp(cmd, "close")) {
            sscanf(rest, "%63s %63s %63s", a, b, c);
            nex_stream_closed(s, parse_ip(a), (uint16_t)atoi(b), (uint16_t)atoi(c));
        } else if (!strcmp(cmd, "dgram")) {
            char *hex = malloc(strlen(rest) + 1);
            hex[0] = 0;
            sscanf(rest, "%63s %63s %63s %63s %s", a, b, c, d, hex);
            size_t n = unhex(hex, data);
            free(hex);
            nex_datagram(s, parse_ip(a), (uint16_t)atoi(b), (uint16_t)atoi(c), parse_ip(d), data, n);
        } else if (!strcmp(cmd, "tick")) {
            nex_server_tick(s, atof(rest));
        } else if (!strcmp(cmd, "pid")) {
            sscanf(rest, "%63s", a);
            printf("ret %llu\n", (unsigned long long)nex_pid_of(a));
        } else if (cmd[0]) {
            printf("error unknown command %s\n", cmd);
        }
        free(data);
        printf(".\n");
        fflush(stdout);
    }
    nex_server_free(s);
    return 0;
}
