/* The in-game NEX server (net_nex.h): encoding, crypto, PRUDP Lite, the stream layer, RMC, logging in, Secure
 * Connection, the stub services and Pia's NAT check - tools/nex/{streams,crypto,prudp,rmc,server,services}.py and the
 * stream part of tools/ssbu_online_lobby.py, in C. */
#define _CRT_SECURE_NO_WARNINGS
#define _CRT_RAND_S
#include <math.h>
#include <stdarg.h>
#include <time.h>

#include "net_nex_int.h"

#ifdef _WIN32
#include <stdlib.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

/* ==== encoding ===================================================================================== */
void no_put(NexOut *o, const void *d, size_t n) {
    if (o->n + n > o->cap) {
        size_t cap = o->cap ? o->cap : 64;
        while (cap < o->n + n) cap *= 2;
        uint8_t *p = realloc(o->p, cap);
        if (!p) abort();
        o->p = p;
        o->cap = cap;
    }
    if (n) memcpy(o->p + o->n, d, n);
    o->n += n;
}
void no_u8(NexOut *o, uint32_t v) { uint8_t b = (uint8_t)v; no_put(o, &b, 1); }
void no_u16(NexOut *o, uint32_t v) { uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; no_put(o, b, 2); }
void no_u32(NexOut *o, uint32_t v) { uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)}; no_put(o, b, 4); }
void no_u64(NexOut *o, uint64_t v) { no_u32(o, (uint32_t)v); no_u32(o, (uint32_t)(v >> 32)); }
void no_str(NexOut *o, const char *s) {
    size_t n = strlen(s) + 1;
    no_u16(o, (uint32_t)n);   /* (Python's u16 masks a longer one's length too) */
    no_put(o, s, n);
}
void no_buf(NexOut *o, const uint8_t *d, size_t n) { no_u32(o, (uint32_t)n); no_put(o, d, n); }
void no_qbuf(NexOut *o, const uint8_t *d, size_t n) { no_u16(o, (uint32_t)n); no_put(o, d, n); }
size_t no_struct_begin(NexOut *o, uint32_t version) { no_u8(o, version); no_u32(o, 0); return o->n; }
void no_struct_end(NexOut *o, size_t at) {
    uint32_t n = (uint32_t)(o->n - at);
    for (int i = 0; i < 4; i++) o->p[at - 4 + i] = (uint8_t)(n >> (8 * i));
}
void no_free(NexOut *o) { free(o->p); o->p = NULL; o->n = o->cap = 0; }

void *nex_tmp(NexArena *a, size_t n) {
    void *p = calloc(1, n ? n : 1);
    if (!p) abort();
    if (a->n == a->cap) {
        size_t cap = a->cap ? a->cap * 2 : 64;
        void **q = realloc(a->p, cap * sizeof *q);
        if (!q) abort();
        a->p = q;
        a->cap = cap;
    }
    a->p[a->n++] = p;
    return p;
}
char *nex_tmpstr(NexArena *a, const char *s) { size_t n = strlen(s); char *p = nex_tmp(a, n + 1); memcpy(p, s, n); return p; }
static void arena_clear(NexArena *a) {
    for (size_t i = 0; i < a->n; i++) free(a->p[i]);
    a->n = 0;
}
char *nex_strdup(const char *s) {
    size_t n = strlen(s ? s : "");
    char *p = malloc(n + 1);
    if (!p) abort();
    memcpy(p, s ? s : "", n + 1);
    return p;
}

NexIn ni_make(const uint8_t *d, size_t n, NexArena *a) { NexIn s = {d, 0, n, 0, a}; return s; }
const uint8_t *ni_read(NexIn *s, size_t n) {
    if (s->err || n > s->end - s->p) { s->err = 1; s->p = s->end; return NULL; }
    const uint8_t *p = s->d + s->p;
    s->p += n;
    return p;
}
uint32_t ni_u8(NexIn *s) { const uint8_t *p = ni_read(s, 1); return p ? p[0] : 0; }
uint32_t ni_u16(NexIn *s) { const uint8_t *p = ni_read(s, 2); return p ? (uint32_t)p[0] | (uint32_t)p[1] << 8 : 0; }
uint32_t ni_u32(NexIn *s) { const uint8_t *p = ni_read(s, 4); return p ? (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24 : 0; }
uint64_t ni_u64(NexIn *s) { uint64_t lo = ni_u32(s); return lo | (uint64_t)ni_u32(s) << 32; }
int ni_eof(const NexIn *s) { return s->p >= s->end; }

/* bytes -> UTF-8 as Python's bytes.decode("utf-8", "replace") makes it: each maximal invalid subpart is U+FFFD */
static size_t utf8_fix(const uint8_t *b, size_t n, char *out) {
    size_t o = 0, i = 0;
    while (i < n) {
        uint8_t c = b[i];
        if (c < 0x80) { out[o++] = (char)c; i++; continue; }
        int need = 0;
        uint8_t lo = 0x80, hi = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) need = 1;
        else if (c >= 0xE0 && c <= 0xEF) { need = 2; if (c == 0xE0) lo = 0xA0; if (c == 0xED) hi = 0x9F; }
        else if (c >= 0xF0 && c <= 0xF4) { need = 3; if (c == 0xF0) lo = 0x90; if (c == 0xF4) hi = 0x8F; }
        size_t k = 1;
        int ok = need > 0;
        for (int j = 0; ok && j < need; j++) {
            if (i + k >= n) { ok = 0; break; }
            uint8_t d = b[i + k];
            if (d < (j ? 0x80 : lo) || d > (j ? 0xBF : hi)) { ok = 0; break; }
            k++;
        }
        if (ok) { memcpy(out + o, b + i, k); o += k; i += k; }
        else { out[o++] = (char)0xEF; out[o++] = (char)0xBF; out[o++] = (char)0xBD; i += k; }
    }
    out[o] = 0;
    return o;
}
char *ni_str(NexIn *s) {
    uint32_t n = ni_u16(s);
    const uint8_t *p = n ? ni_read(s, n) : NULL;
    if (!p) return nex_tmpstr(s->a, "");
    const uint8_t *z = memchr(p, 0, n);
    size_t len = z ? (size_t)(z - p) : n;
    char *out = nex_tmp(s->a, len * 3 + 1);
    utf8_fix(p, len, out);
    return out;
}
const uint8_t *ni_buf(NexIn *s, size_t *n) { *n = ni_u32(s); const uint8_t *p = ni_read(s, *n); if (!p) *n = 0; return p ? p : (const uint8_t *)""; }
const uint8_t *ni_qbuf(NexIn *s, size_t *n) { *n = ni_u16(s); const uint8_t *p = ni_read(s, *n); if (!p) *n = 0; return p ? p : (const uint8_t *)""; }
uint32_t ni_list(NexIn *s) {
    uint32_t n = ni_u32(s);
    if (n > 100000) { s->err = 1; s->p = s->end; return 0; }
    return n;
}
NexIn ni_struct(NexIn *s, uint32_t *version) {
    uint32_t v = ni_u8(s), n = ni_u32(s);
    NexIn sub = {s->d, s->p, s->p, 0, s->a};
    if (version) *version = v;
    if (s->err || n > s->end - s->p) { s->err = 1; s->p = s->end; sub.err = 1; return sub; }
    sub.end = s->p + n;
    s->p += n;
    return sub;
}

size_t u8_chars(const char *s) {
    size_t n = 0;
    for (; *s; s++) n += ((uint8_t)*s & 0xC0) != 0x80;
    return n;
}
size_t u8_prefix(const char *s, size_t chars) {
    size_t i = 0, n = 0;
    for (; s[i]; i++) {
        if (((uint8_t)s[i] & 0xC0) != 0x80) {
            if (n == chars) break;
            n++;
        }
    }
    return i;
}

/* ==== crypto ======================================================================================= */
typedef struct { uint32_t h[4]; uint64_t len; uint8_t buf[64]; size_t bn; } Md5;
static uint32_t rol32(uint32_t x, int n) { return x << n | x >> (32 - n); }
static void md5_block(uint32_t h[4], const uint8_t *p) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                              4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t w[16];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] | (uint32_t)p[4 * i + 1] << 8 | (uint32_t)p[4 * i + 2] << 16 | (uint32_t)p[4 * i + 3] << 24;
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
        else { f = c ^ (b | ~d); g = (7 * i) & 15; }
        uint32_t t = d;
        d = c; c = b;
        b = b + rol32(a + f + K[i] + w[g], R[i]);
        a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
}
static void md5_init(Md5 *m) { m->h[0] = 0x67452301; m->h[1] = 0xefcdab89; m->h[2] = 0x98badcfe; m->h[3] = 0x10325476; m->len = 0; m->bn = 0; }
static void md5_update(Md5 *m, const uint8_t *d, size_t n) {
    m->len += n;
    while (n) {
        size_t k = 64 - m->bn < n ? 64 - m->bn : n;
        memcpy(m->buf + m->bn, d, k);
        m->bn += k; d += k; n -= k;
        if (m->bn == 64) { md5_block(m->h, m->buf); m->bn = 0; }
    }
}
static void md5_final(Md5 *m, uint8_t out[16]) {
    uint64_t bits = m->len * 8;
    uint8_t pad = 0x80, zero = 0;
    md5_update(m, &pad, 1);
    while (m->bn != 56) md5_update(m, &zero, 1);
    uint8_t l[8];
    for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (8 * i));
    md5_update(m, l, 8);
    for (int i = 0; i < 4; i++) for (int k = 0; k < 4; k++) out[4 * i + k] = (uint8_t)(m->h[i] >> (8 * k));
}
void nex_md5(const uint8_t *d, size_t n, uint8_t out[16]) { Md5 m; md5_init(&m); md5_update(&m, d, n); md5_final(&m, out); }
void nex_hmac_md5(const uint8_t *key, size_t kn, const uint8_t *d, size_t n, uint8_t out[16]) {
    uint8_t k[64] = {0}, ip[64], op[64], inner[16];
    if (kn > 64) nex_md5(key, kn, k); else memcpy(k, key, kn);
    for (int i = 0; i < 64; i++) { ip[i] = k[i] ^ 0x36; op[i] = k[i] ^ 0x5c; }
    Md5 m;
    md5_init(&m); md5_update(&m, ip, 64); md5_update(&m, d, n); md5_final(&m, inner);
    md5_init(&m); md5_update(&m, op, 64); md5_update(&m, inner, 16); md5_final(&m, out);
}

static void rc4(const uint8_t *key, size_t kn, const uint8_t *in, uint8_t *out, size_t n) {
    uint8_t s[256];
    for (int i = 0; i < 256; i++) s[i] = (uint8_t)i;
    for (int i = 0, j = 0; i < 256; i++) {
        j = (j + s[i] + key[i % kn]) & 0xFF;
        uint8_t t = s[i]; s[i] = s[j]; s[j] = t;
    }
    int i = 0, j = 0;
    for (size_t k = 0; k < n; k++) {
        i = (i + 1) & 0xFF;
        j = (j + s[i]) & 0xFF;
        uint8_t t = s[i]; s[i] = s[j]; s[j] = t;
        out[k] = in[k] ^ s[(s[i] + s[j]) & 0xFF];
    }
}
/* kerberos_encrypt: RC4, then HMAC-MD5 of the ciphertext */
static void kerberos_encrypt(const uint8_t *key, size_t kn, const uint8_t *d, size_t n, NexOut *out) {
    size_t at = out->n;
    no_put(out, d, n);
    rc4(key, kn, d, out->p + at, n);
    uint8_t mac[16];
    nex_hmac_md5(key, kn, out->p + at, n, mac);
    no_put(out, mac, 16);
}
/* the plaintext (n - 16 bytes into out), or 0 if the MAC does not match */
static int kerberos_decrypt(const uint8_t *key, size_t kn, const uint8_t *d, size_t n, uint8_t *out) {
    if (n < 16) return 0;
    uint8_t mac[16];
    nex_hmac_md5(key, kn, d, n - 16, mac);
    if (memcmp(mac, d + n - 16, 16)) return 0;
    rc4(key, kn, d, out, n - 16);
    return 1;
}

static void sha1_block(uint32_t h[5], const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        uint32_t t = rol32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol32(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}
void nex_sha1(const uint8_t *data, size_t len, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    size_t i = 0;
    for (; i + 64 <= len; i += 64) sha1_block(h, data + i);
    uint8_t tail[128] = {0};
    size_t rest = len - i;
    memcpy(tail, data + i, rest);
    tail[rest] = 0x80;
    size_t n = rest + 9 <= 64 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int k = 0; k < 8; k++) tail[n - 1 - k] = (uint8_t)(bits >> (8 * k));
    sha1_block(h, tail);
    if (n == 128) sha1_block(h, tail + 64);
    for (int k = 0; k < 5; k++) { out[4 * k] = (uint8_t)(h[k] >> 24); out[4 * k + 1] = (uint8_t)(h[k] >> 16); out[4 * k + 2] = (uint8_t)(h[k] >> 8); out[4 * k + 3] = (uint8_t)h[k]; }
}
static void base64(const uint8_t *d, size_t n, char *out) {
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? (uint32_t)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        out[o++] = A[v >> 18 & 63];
        out[o++] = A[v >> 12 & 63];
        out[o++] = i + 1 < n ? A[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? A[v & 63] : '=';
    }
    out[o] = 0;
}

/* ==== the server's plumbing ========================================================================= */
void nex_random(NexServer *s, uint8_t *out, size_t n) {
    if (s->io.random) { s->io.random(s->io.ctx, out, n); return; }
#ifdef _WIN32
    for (size_t i = 0; i < n; i += 4) {
        unsigned v = 0;
        rand_s(&v);
        for (size_t k = 0; k < 4 && i + k < n; k++) out[i + k] = (uint8_t)(v >> (8 * k));
    }
#else
    int fd = open("/dev/urandom", O_RDONLY);
    size_t got = 0;
    if (fd >= 0) { ssize_t r = read(fd, out, n); got = r > 0 ? (size_t)r : 0; close(fd); }
    for (; got < n; got++) out[got] = (uint8_t)rand();
#endif
}
uint64_t nex_datetime_now(NexServer *s) {
    if (s->io.datetime_now) return s->io.datetime_now(s->io.ctx);
    time_t t = time(NULL);
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    return (uint64_t)(g.tm_year + 1900) << 26 | (uint64_t)(g.tm_mon + 1) << 22 | (uint64_t)g.tm_mday << 17 | (uint64_t)g.tm_hour << 12 | (uint64_t)g.tm_min << 6 | (uint64_t)g.tm_sec;
}
void nex_log(NexServer *s, int level, const char *fmt, ...) {
    if (!s->io.log || s->verbose < level) return;
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    s->io.log(s->io.ctx, level, line);
}
static void ip_text(uint32_t ip, char out[16]) { snprintf(out, 16, "%u.%u.%u.%u", ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255); }

/* PIDs: stable, from the EOS product user id or the account's NSA id: 1000000 .. 2^31 - 1, FNV-1a 64 */
uint64_t nex_pid_of(const char *key) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (const uint8_t *p = (const uint8_t *)key; *p; p++) { h ^= *p; h *= 0x100000001b3ull; }
    return 1000000 + h % (0x7FFFFFFFull - 1000000);
}

/* ==== RMC ======================================================================================== */
typedef struct Rmc { int request, success; uint32_t protocol, call_id, method, error; const uint8_t *body; size_t n; } Rmc;
static int rmc_decode(const uint8_t *data, size_t n, Rmc *m) {
    if (n < 5) return 0;
    uint32_t size = (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
    const uint8_t *d = data + 4;
    size_t len = n - 4 < size ? n - 4 : size;
    if (len < 1) return 0;
    NexIn s = ni_make(d, len, NULL);
    uint32_t b = d[0];
    size_t i = 1;
    m->request = (b & 0x80) != 0;
    m->protocol = b & 0x7F;
    if (m->protocol == 0x7F) {
        if (len < 3) return 0;
        m->protocol = (uint32_t)d[1] | (uint32_t)d[2] << 8;
        i = 3;
    }
    m->success = 1; m->error = 0; m->method = 0;
    s.p = i;
    if (m->request) {
        m->call_id = ni_u32(&s); m->method = ni_u32(&s);
        if (s.err) return 0;
        m->body = d + i + 8; m->n = len - i - 8;
    } else {
        if (i >= len) return 0;
        m->success = d[i] == 1;
        s.p = i + 1;
        if (m->success) { m->call_id = ni_u32(&s); m->method = ni_u32(&s) & 0x7FFF; if (s.err) return 0; m->body = d + i + 9; m->n = len - i - 9; }
        else { m->error = ni_u32(&s); m->call_id = ni_u32(&s); if (s.err) return 0; m->body = d; m->n = 0; }
    }
    return 1;
}
static void rmc_head(NexOut *o, uint32_t protocol, int request) {
    uint32_t flag = request ? 0x80 : 0;
    if (protocol < 0x7F) no_u8(o, protocol | flag);
    else { no_u8(o, 0x7F | flag); no_u16(o, protocol); }
}
static void rmc_sized(NexOut *o, const NexOut *body) { no_u32(o, (uint32_t)body->n); no_put(o, body->p, body->n); }
static void rmc_request(NexOut *o, uint32_t protocol, uint32_t call_id, uint32_t method, const uint8_t *body, size_t n) {
    NexOut b = {0};
    rmc_head(&b, protocol, 1); no_u32(&b, call_id); no_u32(&b, method); no_put(&b, body, n);
    rmc_sized(o, &b); no_free(&b);
}
static void rmc_response(NexOut *o, uint32_t protocol, uint32_t call_id, uint32_t method, const uint8_t *body, size_t n) {
    NexOut b = {0};
    rmc_head(&b, protocol, 0); no_u8(&b, 1); no_u32(&b, call_id); no_u32(&b, method | 0x8000); no_put(&b, body, n);
    rmc_sized(o, &b); no_free(&b);
}
static void rmc_error(NexOut *o, uint32_t protocol, uint32_t call_id, uint32_t code) {
    NexOut b = {0};
    rmc_head(&b, protocol, 0); no_u8(&b, 0); no_u32(&b, code); no_u32(&b, call_id);
    rmc_sized(o, &b); no_free(&b);
}

/* ==== PRUDP Lite (tools/nex/prudp.py, lite=True) ==================================================== */
enum { SYN, CONNECT, DATA, DISCONNECT, PING, LITE_PING = 8 };
enum { F_ACK = 0x1, F_RELIABLE = 0x2, F_NEED_ACK = 0x4, F_HAS_SIZE = 0x8, F_MULTI_ACK = 0x200 };
enum { O_SUPPORT = 0, O_SIGNATURE = 1, O_FRAGMENT = 2 };
#define FRAGMENT 1300
#define LITE_HEADER 12
#define MAX_MESSAGE (64 * 1024)
#define MAX_HELD (64 * 1024)
#define MAX_CONNS_PER_HOST 4
#define MAX_CONNS 1024
#define IDLE_TIMEOUT 40.0
#define MAX_RESPONSE (255 * FRAGMENT - 64)
#define STREAM_BUFFER (256 * 1024)
#define STREAM_PACKET (16 * 1024)

_Static_assert(sizeof(((NexConn *)0)->support) >= 255 && sizeof(((NexConn *)0)->client_sig) >= 255, "option values are up to 255 bytes");
typedef struct Lite {
    uint32_t type, flags, src, dst, streams, frag, seq;
    const uint8_t *opt[256]; uint8_t optn[256]; uint8_t has[256];
    const uint8_t *payload; size_t n;
} Lite;

static long lite_size(const uint8_t *d, size_t n) {
    if (n < LITE_HEADER) return 0;
    if (d[0] != 0x80) return -1;
    return LITE_HEADER + d[1] + ((long)d[2] | (long)d[3] << 8);
}
static int lite_decode(const uint8_t *d, size_t n, Lite *p) {
    long size = lite_size(d, n);
    if (size <= 0 || (size_t)size > n) return 0;
    size_t optsize = d[1], plen = (size_t)d[2] | (size_t)d[3] << 8;
    uint32_t tf = (uint32_t)d[8] | (uint32_t)d[9] << 8;
    p->streams = d[4]; p->src = d[5]; p->dst = d[6]; p->frag = d[7];
    p->type = tf & 0xF; p->flags = tf >> 4;
    p->seq = (uint32_t)d[10] | (uint32_t)d[11] << 8;
    memset(p->has, 0, sizeof p->has);
    const uint8_t *o = d + LITE_HEADER;
    for (size_t i = 0; i + 2 <= optsize;) {
        uint8_t oid = o[i], m = o[i + 1];
        size_t avail = optsize - (i + 2);
        p->opt[oid] = o + i + 2;
        p->optn[oid] = (uint8_t)(m < avail ? m : avail);
        p->has[oid] = 1;
        i += 2 + (size_t)m;
    }
    p->payload = d + LITE_HEADER + optsize;
    p->n = plen;
    return 1;
}

/* ---- streams: the server's writers -------------------------------------------------------------- */
static NexStream *stream_find(NexServer *s, uint32_t ip, uint16_t port, uint16_t sport) {
    for (int i = 0; i < s->nstreams; i++)
        if (s->streams[i].ip == ip && s->streams[i].port == port && s->streams[i].sport == sport) return &s->streams[i];
    return NULL;
}
static void ws_frame(NexOut *o, uint32_t opcode, const uint8_t *d, size_t n) {
    no_u8(o, 0x80 | opcode);
    if (n < 126) no_u8(o, (uint32_t)n);
    else if (n < 65536) { no_u8(o, 126); no_u8(o, (uint32_t)(n >> 8)); no_u8(o, (uint32_t)n); }
    else { no_u8(o, 127); for (int i = 7; i >= 0; i--) no_u8(o, (uint32_t)((uint64_t)n >> (8 * i))); }
    no_put(o, d, n);
}
static void stream_write(NexServer *s, NexStream *st, const uint8_t *d, size_t n) {
    if (!s->io.stream_out) return;
    if (st->mode == 3) {
        NexOut f = {0};
        ws_frame(&f, 2, d, n);
        s->io.stream_out(s->io.ctx, st->ip, st->port, st->sport, f.p, f.n);
        no_free(&f);
    } else s->io.stream_out(s->io.ctx, st->ip, st->port, st->sport, d, n);
}
static void stream_send(NexServer *s, uint32_t ip, uint16_t port, uint16_t sport, const uint8_t *d, size_t n) {
    NexStream *st = stream_find(s, ip, port, sport);
    if (st) stream_write(s, st, d, n);
}

/* ---- connections ----------------------------------------------------------------------------------- */
static uint16_t server_port(int secure) { return secure ? NEX_SECURE_PORT : NEX_AUTH_PORT; }
static NexConn *conn_find(NexServer *s, int k, uint32_t ip, uint16_t port, uint32_t vport) {
    for (int i = 0; i < s->nconns[k]; i++) {
        NexConn *c = s->conns[k][i];
        if (c->ip == ip && c->port == port && c->vport == vport) return c;
    }
    return NULL;
}
NexConn *nex_by_pid(NexServer *s, uint64_t pid) {
    for (int i = 0; i < s->nconns[1]; i++)
        if (s->conns[1][i]->in_by_pid && s->conns[1][i]->pid == pid) return s->conns[1][i];
    return NULL;
}
static void conn_free(NexConn *c) {
    for (int i = 0; i < NEX_HOLD_WINDOW; i++) free(c->held[i].p);
    no_free(&c->frags);
    no_free(&c->connect_ack);
    for (int i = 0; i < c->nurls; i++) free(c->urls[i]);
    free(c->part);
    free(c);
}
static void server_closed(NexServer *s, NexConn *c, const char *why);
static void conn_close(NexServer *s, NexConn *c, const char *why) {
    int k = c->secure;
    for (int i = 0; i < s->nconns[k]; i++) {
        if (s->conns[k][i] != c) continue;
        memmove(&s->conns[k][i], &s->conns[k][i + 1], (size_t)(s->nconns[k] - i - 1) * sizeof c);
        s->nconns[k]--;
        c->state = 2;
        nex_log(s, 2, "%s: %u.%u.%u.%u:%u gone (%s)", k ? "secure/tcp" : "auth/tcp", c->ip >> 24, c->ip >> 16 & 255, c->ip >> 8 & 255, c->ip & 255, c->port, why);
        if (s->ndead == s->capdead) {
            s->capdead = s->capdead ? s->capdead * 2 : 16;
            s->dead = realloc(s->dead, (size_t)s->capdead * sizeof *s->dead);
            if (!s->dead) abort();
        }
        s->dead[s->ndead++] = c;
        server_closed(s, c, why);
        return;
    }
}

static void lite_send(NexServer *s, NexConn *c, uint32_t type, uint32_t flags, uint32_t seq, uint32_t frag,
                      int nopt, const uint8_t *oid, const uint8_t *const *ov, const size_t *on, const uint8_t *payload, size_t n, NexOut *keep) {
    NexOut o = {0};
    size_t optsize = 0;
    for (int i = 0; i < nopt; i++) optsize += 2 + on[i];
    if (optsize > 255 || n > 0xFFFF) { nex_log(s, 1, "a PRUDP Lite packet of %zu B of options: not sent", optsize); return; }   /* (tools/nex fails there too) */
    no_u8(&o, 0x80); no_u8(&o, (uint32_t)optsize); no_u16(&o, (uint32_t)n);
    no_u8(&o, c->streams); no_u8(&o, c->local_vport); no_u8(&o, c->vport); no_u8(&o, frag);
    no_u16(&o, type | flags << 4); no_u16(&o, seq);
    for (int i = 0; i < nopt; i++) { no_u8(&o, oid[i]); no_u8(&o, (uint32_t)on[i]); no_put(&o, ov[i], on[i]); }
    no_put(&o, payload, n);
    stream_send(s, c->ip, c->port, server_port(c->secure), o.p, o.n);
    if (keep) { no_free(keep); *keep = o; } else no_free(&o);
}
static void lite_ack(NexServer *s, NexConn *c, const Lite *p) {
    lite_send(s, c, p->type, F_ACK | (p->flags & F_HAS_SIZE), p->seq, 0, 0, NULL, NULL, NULL, NULL, 0, NULL);
}
static void send_message(NexServer *s, NexConn *c, const uint8_t *m, size_t n) {
    if (c->state != 1) return;
    size_t parts = n ? (n + FRAGMENT - 1) / FRAGMENT : 1;
    for (size_t i = 0; i < parts; i++) {
        size_t at = i * FRAGMENT, len = n - at < FRAGMENT ? n - at : FRAGMENT;
        uint32_t frag = i == parts - 1 ? 0 : (uint32_t)(i % 255 + 1);
        lite_send(s, c, DATA, F_RELIABLE | F_NEED_ACK | F_HAS_SIZE, c->out_seq, frag, 0, NULL, NULL, NULL, n ? m + at : m, n ? len : 0, NULL);
        c->out_seq = (uint16_t)(c->out_seq + 1);
        if (!c->out_seq) c->out_seq = 1;
    }
}

void nex_call(NexServer *s, uint64_t pid, uint32_t protocol, uint32_t method, const uint8_t *body, size_t n) {
    NexConn *c = nex_by_pid(s, pid);
    if (!c) {
        if (s->store.call) s->store.call(s->store.ctx, pid, protocol, method, body, n);
        return;
    }
    uint32_t cid = s->call_id;
    s->call_id = s->call_id % 0x7FFFFFFF + 1;
    NexOut o = {0};
    rmc_request(&o, protocol, cid, method, body, n);
    send_message(s, c, o.p, o.n);
    no_free(&o);
}
void nex_server_remote_call(NexServer *s, uint64_t pid, uint32_t protocol, uint32_t method, const uint8_t *body, size_t n) {
    if (nex_by_pid(s, pid)) nex_call(s, pid, protocol, method, body, n);
    arena_clear(&s->arena);
}

/* ---- the RMC of one connection ---------------------------------------------------------------------- */
static NexHandler core_handler(uint32_t protocol, uint32_t method);
static void server_message(NexServer *s, NexConn *c, const uint8_t *data, size_t n) {
    Rmc m;
    if (!rmc_decode(data, n, &m)) { nex_log(s, 1, "pid %llu: undecodable RMC message (%zu B)", (unsigned long long)c->pid, n); return; }
    if (!m.request) return;   /* (answers to our notifications: nothing waits for them) */
    NexHandler fn = core_handler(m.protocol, m.method);
    if (!fn) fn = nex_match_handler(m.protocol, m.method);
    if (!fn) fn = nex_ranked_handler(m.protocol, m.method);
    int served = c->secure ? m.protocol != 10 && c->pid && nex_by_pid(s, c->pid) == c : m.protocol == 10;
    NexOut reply = {0};
    char hex[2 * 64 + 4] = "";   /* (the start of its parameters, as tools/nex shows them; none of logging in's,
                                    which carry the account's token) */
    size_t hn = m.protocol == 10 ? 0 : m.n < 64 ? m.n : 64;
    for (size_t i = 0; i < hn; i++) snprintf(hex + 2 * i, 3, "%02x", m.body[i]);
    if (m.n > hn) strcat(hex, "..");
    if (fn && served && m.protocol == 115 && m.method != 8)   /* (DataStore: each call, as tools/nex logs them) */
        nex_log(s, 1, "DataStore method %u from pid %llu: %s", m.method, (unsigned long long)c->pid, m.n ? hex : "(nothing)");
    if (!fn || !served) {
        nex_log(s, 1, "%s protocol %u method %u from pid %llu: %s", fn ? "not served there:" : "UNHANDLED", m.protocol, m.method, (unsigned long long)c->pid, hex);
        rmc_error(&reply, m.protocol, m.call_id, NEX_CORE_NOT_IMPLEMENTED);
    } else {
        NexIn in = ni_make(m.body, m.n, &s->arena);
        NexOut body = {0};
        uint32_t err = fn(s, c, &in, &body);
        if (!err && body.n > MAX_RESPONSE) err = NEX_CORE_BUFFER_OVERFLOW;
        if (err) {
            nex_log(s, 1, "protocol %u method %u from pid %llu: error %#x", m.protocol, m.method, (unsigned long long)c->pid, err);
            rmc_error(&reply, m.protocol, m.call_id, err);
        } else rmc_response(&reply, m.protocol, m.call_id, m.method, body.p, body.n);
        no_free(&body);
    }
    send_message(s, c, reply.p, reply.n);
    no_free(&reply);
}

/* ---- Lite packets in ---------------------------------------------------------------------------------- */
static int connect_answer(NexServer *s, NexConn *c, const uint8_t *payload, size_t n, NexOut *answer);
static void on_data(NexServer *s, NexConn *c, const Lite *p) {
    if (p->flags & F_ACK) return;          /* (a stream already delivers: nothing waits for acknowledgements) */
    if (p->flags & F_MULTI_ACK) return;
    if (!(p->flags & F_RELIABLE)) return;
    int taken = c->state == 1;             /* (Lite has no substreams: always 0) */
    uint16_t ahead = (uint16_t)(p->seq - c->in_seq);
    if (taken && ahead > 0 && ahead < 0x8000 && (ahead >= NEX_HOLD_WINDOW || c->held_bytes + p->n > MAX_HELD)) return;
    if (p->flags & F_NEED_ACK) lite_ack(s, c, p);
    if (!taken || ahead >= 0x8000) return;
    NexHeld *h = &c->held[p->seq % NEX_HOLD_WINDOW];
    if (h->used && h->seq == p->seq) return;
    h->used = 1; h->seq = (uint16_t)p->seq; h->frag = (uint8_t)p->frag;
    free(h->p);
    h->p = malloc(p->n ? p->n : 1);
    if (!h->p) abort();
    memcpy(h->p, p->payload, p->n);
    h->n = p->n;
    c->held_bytes += p->n;
    for (;;) {
        NexHeld *q = &c->held[c->in_seq % NEX_HOLD_WINDOW];
        if (!q->used || q->seq != c->in_seq) break;
        q->used = 0;
        c->held_bytes -= q->n;
        c->in_seq = (uint16_t)(c->in_seq + 1);
        no_put(&c->frags, q->p, q->n);
        if (c->frags.n > MAX_MESSAGE) { conn_close(s, c, "a message of more than 65536 bytes"); return; }
        if (q->frag == 0) {
            NexOut msg = c->frags;
            c->frags = (NexOut){0};
            if (msg.n) {
                server_message(s, c, msg.p, msg.n);
                no_free(&msg);
                if (c->state == 2) return;
            } else no_free(&msg);
        }
    }
}

static void lite_in(NexServer *s, int k, uint32_t ip, uint16_t port, const uint8_t *d, size_t n) {
    Lite p;
    if (!lite_decode(d, n, &p)) { nex_log(s, 1, "not a PRUDP Lite packet (%zu B)", n); return; }
    NexConn *c = conn_find(s, k, ip, port, p.src);
    if (p.type == SYN) {
        if (c) conn_close(s, c, "new SYN");
        NexConn *oldest = NULL;
        int mine = 0;
        for (int i = 0; i < s->nconns[k]; i++)
            if (s->conns[k][i]->ip == ip) { mine++; if (!oldest || s->conns[k][i]->seen < oldest->seen) oldest = s->conns[k][i]; }
        if (mine >= MAX_CONNS_PER_HOST) conn_close(s, oldest, "too many connections from there");
        if (s->nconns[k] >= MAX_CONNS) return;
        c = calloc(1, sizeof *c);
        if (!c) abort();
        c->secure = k; c->ip = ip; c->port = port; c->vport = (uint8_t)p.src; c->seen = s->now;
        c->in_seq = 1; c->out_seq = 1;
        uint8_t session;
        nex_random(s, &session, 1);   /* (the V1 session id: unused over a stream, drawn as tools/nex draws it) */
        if (s->nconns[k] == s->capconns[k]) {
            s->capconns[k] = s->capconns[k] ? s->capconns[k] * 2 : 8;
            s->conns[k] = realloc(s->conns[k], (size_t)s->capconns[k] * sizeof *s->conns[k]);
            if (!s->conns[k]) abort();
        }
        s->conns[k][s->nconns[k]++] = c;
        c->local_vport = (uint8_t)p.dst;
        c->streams = (uint8_t)((p.streams & 0xF) << 4 | p.streams >> 4);
        /* on_syn */
        if (p.has[O_SUPPORT]) { memcpy(c->support, p.opt[O_SUPPORT], p.optn[O_SUPPORT]); c->support_n = p.optn[O_SUPPORT]; }
        else { memcpy(c->support, "\4\0\0\0", 4); c->support_n = 4; }
        char ipt[16], addr[48];
        ip_text(ip, ipt);
        snprintf(addr, sizeof addr, "('%s', %u)", ipt, port);   /* (Python's repr of the address) */
        nex_hmac_md5(s->sig_key[k], 16, (const uint8_t *)addr, strlen(addr), c->server_sig);
        const uint8_t ids[2] = {O_SUPPORT, O_SIGNATURE};
        const uint8_t *vals[2] = {c->support, c->server_sig};
        size_t lens[2] = {c->support_n, 16};
        lite_send(s, c, SYN, F_ACK | (p.flags & F_HAS_SIZE), p.seq, 0, 2, ids, vals, lens, NULL, 0, NULL);
        return;
    }
    if (!c) return;
    c->seen = s->now;
    if (p.type == CONNECT) {
        if (c->state == 1) {   /* (our CONNECT-ACK again) */
            if (c->has_connect_ack) stream_send(s, c->ip, c->port, server_port(c->secure), c->connect_ack.p, c->connect_ack.n);
            return;
        }
        c->client_sig_n = p.has[O_SIGNATURE] ? p.optn[O_SIGNATURE] : 0;
        if (c->client_sig_n) memcpy(c->client_sig, p.opt[O_SIGNATURE], c->client_sig_n);
        c->in_seq = (uint16_t)(p.seq + 1);
        NexOut answer = {0};
        if (!connect_answer(s, c, p.payload, p.n, &answer)) { no_free(&answer); conn_close(s, c, "refused"); return; }
        const uint8_t ids[1] = {O_SUPPORT};
        const uint8_t *vals[1] = {c->support};
        size_t lens[1] = {c->support_n};
        lite_send(s, c, CONNECT, F_ACK | (p.flags & F_HAS_SIZE), p.seq, 0, 1, ids, vals, lens, answer.p, answer.n, &c->connect_ack);
        c->has_connect_ack = 1;
        no_free(&answer);
        c->state = 1;
    } else if (p.type == DATA) on_data(s, c, &p);
    else if (p.type == PING || p.type == LITE_PING) {
        if (p.flags & F_NEED_ACK) lite_ack(s, c, &p);
    } else if (p.type == DISCONNECT) {
        for (int i = 0; i < 3; i++) lite_send(s, c, DISCONNECT, F_ACK | (p.flags & F_HAS_SIZE), p.seq, 0, 0, NULL, NULL, NULL, NULL, 0, NULL);
        conn_close(s, c, "client disconnected");
    }
}

static void end_call(NexServer *s) {
    for (int i = 0; i < s->ndead; i++) conn_free(s->dead[i]);
    s->ndead = 0;
    arena_clear(&s->arena);
}

/* ==== logging in ================================================================================ */
#define SECURE_URL "prudps:/address=10.13.0.1;port=60001;CID=1;PID=2;sid=1;stream=10;type=2"
#define BUILD_NAME "ssbu-port nex 1"

static void issue_ticket(NexServer *s, uint64_t source, uint64_t target, uint64_t issued, NexOut *ticket, char hex[33]) {
    uint8_t session_key[32], source_key[16], rnd[16], key[16], cat[32];
    nex_random(s, session_key, 32);
    nex_random(s, source_key, 16);
    nex_random(s, rnd, 16);
    NexOut info = {0}, inner_enc = {0}, inner = {0}, body = {0};
    no_u64(&info, issued); no_u64(&info, source); no_put(&info, session_key, 32);
    memcpy(cat, s->server_key, 16); memcpy(cat + 16, rnd, 16);
    nex_md5(cat, 32, key);
    kerberos_encrypt(key, 16, info.p, info.n, &inner_enc);
    no_u32(&inner, 16); no_put(&inner, rnd, 16); no_u32(&inner, (uint32_t)inner_enc.n); no_put(&inner, inner_enc.p, inner_enc.n);
    no_put(&body, session_key, 32); no_u64(&body, target); no_u32(&body, (uint32_t)inner.n); no_put(&body, inner.p, inner.n);
    kerberos_encrypt(source_key, 16, body.p, body.n, ticket);
    for (int i = 0; i < 16; i++) snprintf(hex + 2 * i, 3, "%02x", source_key[i]);
    no_free(&info); no_free(&inner_enc); no_free(&inner); no_free(&body);
}

/* (source PID, session key) of an inner ticket this server issued */
static int open_inner(NexServer *s, const uint8_t *inner, size_t n, uint64_t *pid, uint8_t key_out[32]) {
    if (n < 4) return 0;
    uint32_t rn = (uint32_t)inner[0] | (uint32_t)inner[1] << 8 | (uint32_t)inner[2] << 16 | (uint32_t)inner[3] << 24;
    if ((uint64_t)4 + rn + 4 > n) return 0;
    const uint8_t *rnd = inner + 4;
    const uint8_t *q = inner + 4 + rn;
    uint32_t m = (uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24;
    size_t at = 8 + (size_t)rn, en = at + m <= n ? m : n - at;
    uint8_t *cat = malloc(16 + (size_t)rn), key[16];
    if (!cat) return 0;
    memcpy(cat, s->server_key, 16); memcpy(cat + 16, rnd, rn);
    nex_md5(cat, 16 + (size_t)rn, key);
    free(cat);
    uint8_t *info = malloc(en ? en : 1);
    if (!info) return 0;
    int ok = kerberos_decrypt(key, 16, inner + at, en, info) && en - 16 >= 48;
    if (ok) {
        *pid = 0;
        for (int i = 0; i < 8; i++) *pid |= (uint64_t)info[8 + i] << (8 * i);
        memcpy(key_out, info + 16, 32);
    }
    free(info);
    return ok;
}

static int connect_answer(NexServer *s, NexConn *c, const uint8_t *payload, size_t n, NexOut *answer) {
    if (!c->secure) return 1;   /* (the authentication server: an empty answer) */
    NexIn in = ni_make(payload, n, &s->arena);
    size_t tn, rn;
    const uint8_t *ticket = ni_buf(&in, &tn), *request = ni_buf(&in, &rn);
    if (in.err) { nex_log(s, 1, "secure CONNECT: bad payload"); return 0; }
    uint64_t pid;
    uint8_t key[32];
    if (!open_inner(s, ticket, tn, &pid, key)) { nex_log(s, 1, "secure CONNECT: not one of our tickets"); return 0; }
    uint8_t *req = malloc(rn ? rn : 1);
    if (!req) return 0;
    int ok = kerberos_decrypt(key, 32, request, rn, req) && rn - 16 >= 16;
    uint64_t req_pid = 0;
    uint32_t check = 0;
    if (ok) {
        for (int i = 0; i < 8; i++) req_pid |= (uint64_t)req[i] << (8 * i);
        check = (uint32_t)req[12] | (uint32_t)req[13] << 8 | (uint32_t)req[14] << 16 | (uint32_t)req[15] << 24;
    }
    free(req);
    if (!ok) { nex_log(s, 1, "secure CONNECT: request does not decrypt"); return 0; }
    if (req_pid != pid) { nex_log(s, 1, "secure CONNECT: ticket for %llu, request from %llu", (unsigned long long)pid, (unsigned long long)req_pid); return 0; }
    NexConn *old = nex_by_pid(s, pid);
    if (old && old != c) conn_close(s, old, "logged in again");
    memcpy(c->session_key, key, 32);
    c->pid = pid;
    const char *part = s->io.part_of ? s->io.part_of(s->io.ctx, c->ip) : NULL;
    free(c->part);
    c->part = part ? nex_strdup(part) : NULL;
    c->has_part = part != NULL;
    c->in_by_pid = 1;
    nex_log(s, 1, "pid %llu connected to the secure server", (unsigned long long)pid);
    no_u32(answer, 4);
    no_u32(answer, check + 1);
    return 1;
}

static void server_closed(NexServer *s, NexConn *c, const char *why) {
    if (!c->secure || !c->in_by_pid) return;
    c->in_by_pid = 0;
    nex_log(s, 1, "pid %llu left (%s)", (unsigned long long)c->pid, why);
    nex_match_gone(s, c);
}

static int is_lower_hex16(const char *t, size_t n) {
    if (n != 16) return 0;
    for (size_t i = 0; i < n; i++) if (!((t[i] >= '0' && t[i] <= '9') || (t[i] >= 'a' && t[i] <= 'f'))) return 0;
    return 1;
}

/* 10/6 ValidateAndRequestTicketWithParam */
static uint32_t tg_validate_with_param(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    NexIn p = ni_struct(in, NULL);
    if (in->err) return NEX_EINVAL;
    uint32_t platform = ni_u32(&p);
    (void)ni_str(&p);   /* (the user name: can be the NSA id itself, so never logged) */
    char *holder = ni_str(&p);
    uint32_t total = ni_u32(&p), n = ni_u32(&p);
    if (p.err || total != n + 4u) return NEX_EINVAL;
    const uint8_t *data = ni_read(&p, n);
    if (p.err) return NEX_EINVAL;
    const char *token = "";
    if (!strcmp(holder, "AuthenticationInfo")) {
        NexIn d = ni_make(data, n, &s->arena), last = ni_make((const uint8_t *)"", 0, &s->arena);
        while (!ni_eof(&d)) {
            last = ni_struct(&d, NULL);
            if (d.err) return NEX_EINVAL;
        }
        token = ni_str(&last);
        if (last.err) return NEX_EINVAL;
    }
    /* "ssbu-port.1.<NSA id, 16 hex digits>.<name>" (token.split(".", 3)) */
    const char *f[4] = {token, NULL, NULL, NULL};
    size_t fl[4] = {0};
    int parts = 1;
    for (const char *q = token; *q && parts < 4; q++)
        if (*q == '.') f[parts++] = q + 1;
    for (int i = 0; i < parts; i++) fl[i] = i + 1 < parts ? (size_t)(f[i + 1] - 1 - f[i]) : strlen(f[i]);
    if (parts != 4 || fl[0] != 9 || strncmp(f[0], "ssbu-port", 9) || !is_lower_hex16(f[2], fl[2])) {
        nex_log(s, 1, "login refused: not a port token (%zu characters)", strlen(token));
        return NEX_RV_INVALID_USERNAME;
    }
    char nsa[17];
    memcpy(nsa, f[2], 16);
    nsa[16] = 0;
    uint64_t pid = nex_pid_of(s->identity ? s->identity : nsa);
    c->pid = pid;
    NexOut ticket = {0};
    char hex[33];
    issue_ticket(s, pid, NEX_SERVER_PID, nex_datetime_now(s), &ticket, hex);
    nex_log(s, 1, "login: platform %u -> pid %llu", platform, (unsigned long long)pid);
    size_t at = no_struct_begin(out, 0);
    no_u64(out, pid);
    no_buf(out, ticket.p, ticket.n);
    no_str(out, SECURE_URL);
    no_u64(out, nex_datetime_now(s));
    no_str(out, BUILD_NAME);
    no_str(out, hex);
    no_struct_end(out, at);
    no_free(&ticket);
    return 0;
}

/* 10/3 RequestTicket: for the PID this connection logged in as, and no other */
static uint32_t tg_request_ticket(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    uint64_t source = ni_u64(in), target = ni_u64(in);
    if (in->err) return NEX_EINVAL;
    if (!c->pid || source != c->pid) return NEX_CORE_ACCESS_DENIED;
    NexOut ticket = {0};
    char hex[33];
    issue_ticket(s, source, target, nex_datetime_now(s), &ticket, hex);
    no_u32(out, NEX_SUCCESS);
    no_buf(out, ticket.p, ticket.n);
    no_str(out, hex);
    no_free(&ticket);
    return 0;
}

/* ==== Secure Connection (11) ============================================================================ */
typedef struct Fields { int n; char *k[320]; char *v[320]; } Fields;
static void fields_set(Fields *f, NexArena *a, const char *k, size_t kn, const char *v, size_t vn) {
    char *key = nex_tmp(a, kn + 1), *val = nex_tmp(a, vn + 1);
    memcpy(key, k, kn); memcpy(val, v, vn);
    for (int i = 0; i < f->n; i++) if (!strcmp(f->k[i], key)) { f->v[i] = val; return; }
    if (f->n < 320) { f->k[f->n] = key; f->v[f->n] = val; f->n++; }
}
static void fields_put(Fields *f, NexArena *a, const char *k, const char *v) { fields_set(f, a, k, strlen(k), v, strlen(v)); }

/* A client's station as the server tells it: tools/nex/server.py's station_url */
int nex_station_url(NexServer *s, NexConn *c, char *out, size_t cap) {
    NexArena *a = &s->arena;
    Fields f = {0};
    char ipt[16], num[32];
    ip_text(c->ip, ipt);
    fields_put(&f, a, "address", ipt);
    snprintf(num, sizeof num, "%u", c->port);
    fields_put(&f, a, "port", num);
    fields_put(&f, a, "natf", "0"); fields_put(&f, a, "natm", "0"); fields_put(&f, a, "pmp", "0");
    fields_put(&f, a, "sid", "15"); fields_put(&f, a, "upnp", "0");
    const char *scheme = "prudp";
    size_t scheme_n = 5;
    if (c->nurls) {
        const char *u = c->urls[0], *sep = strstr(u, ":/");
        scheme = u;
        scheme_n = sep ? (size_t)(sep - u) : strlen(u);
        const char *rest = sep ? sep + 2 : u + strlen(u);
        while (1) {
            const char *end = strchr(rest, ';');
            size_t len = end ? (size_t)(end - rest) : strlen(rest);
            const char *eq = memchr(rest, '=', len);
            if (eq) fields_set(&f, a, rest, (size_t)(eq - rest), eq + 1, len - (size_t)(eq - rest) - 1);
            if (!end) break;
            rest = end + 1;
        }
    }
    fields_put(&f, a, "address", ipt);
    fields_put(&f, a, "type", "3");
    snprintf(num, sizeof num, "%llu", (unsigned long long)c->pid); fields_put(&f, a, "PID", num);
    snprintf(num, sizeof num, "%u", (uint32_t)c->pid); fields_put(&f, a, "RVCID", num);
    snprintf(num, sizeof num, "%u", c->natm); fields_put(&f, a, "natm", num);
    snprintf(num, sizeof num, "%u", c->natf); fields_put(&f, a, "natf", num);
    for (int i = 0; i < s->npia; i++)
        if (s->pia[i].ip == c->ip) { snprintf(num, sizeof num, "%u", s->pia[i].port); fields_put(&f, a, "port", num); }
    NexOut o = {0};
    no_put(&o, scheme, scheme_n);
    no_put(&o, ":/", 2);
    for (int i = 0; i < f.n; i++) {
        if (i) no_put(&o, ";", 1);
        no_put(&o, f.k[i], strlen(f.k[i]));
        no_put(&o, "=", 1);
        no_put(&o, f.v[i], strlen(f.v[i]));
    }
    int ok = o.n < cap;
    if (ok) { memcpy(out, o.p, o.n); out[o.n] = 0; }
    no_free(&o);
    return ok;
}
int nex_server_player(NexServer *s, uint64_t *pid, char *url, size_t cap, uint32_t *rating) {
    for (int i = 0; i < s->nconns[1]; i++) {
        NexConn *c = s->conns[1][i];
        if (!c->in_by_pid || !c->pid) continue;
        *pid = c->pid;
        if (!c->registered || !nex_station_url(s, c, url, cap)) url[0] = 0;
        *rating = nex_rating_of(s, c->pid);
        arena_clear(&s->arena);
        return 1;
    }
    return 0;
}
int nex_url_of_pid(NexServer *s, uint64_t pid, char *out, size_t cap) {
    NexConn *c = nex_by_pid(s, pid);
    if (c) return nex_station_url(s, c, out, cap);
    return s->store.station_url && s->store.station_url(s->store.ctx, pid, out, cap);
}

static uint32_t read_urls(NexIn *in, char **urls, uint32_t *n, int *too_many) {
    uint32_t count = ni_list(in);
    *n = 0; *too_many = 0;
    for (uint32_t i = 0; i < count && !in->err; i++) {
        char *u = ni_str(in);
        if (in->err) break;
        if (*n < NEX_MAX_URLS) urls[(*n)++] = u;
        else *too_many = 1;
        if (u8_chars(u) > 512) *too_many = 1;
    }
    return in->err ? NEX_EINVAL : 0;
}
static void set_urls(NexConn *c, char **urls, uint32_t n) {
    for (int i = 0; i < c->nurls; i++) free(c->urls[i]);
    for (uint32_t i = 0; i < n; i++) c->urls[i] = nex_strdup(urls[i]);
    c->nurls = (int)n;
    c->has_urls = 1;
}
static uint32_t do_register(NexServer *s, NexConn *c, char **urls, uint32_t n, NexOut *out) {
    set_urls(c, urls, n);
    c->registered = 1;
    char url[1024];
    if (!nex_station_url(s, c, url, sizeof url)) url[0] = 0;
    no_u32(out, NEX_SUCCESS);
    no_u32(out, (uint32_t)c->pid);
    no_str(out, url);
    return 0;
}
static uint32_t sc_register(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    char *urls[NEX_MAX_URLS];
    uint32_t n;
    int bad;
    if (read_urls(in, urls, &n, &bad)) return NEX_EINVAL;
    if (bad) return NEX_CORE_INVALID_ARGUMENT;
    return do_register(s, c, urls, n, out);
}
static uint32_t sc_register_ex(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    char *urls[NEX_MAX_URLS];
    uint32_t n;
    int bad;
    if (read_urls(in, urls, &n, &bad)) return NEX_EINVAL;
    (void)ni_str(in);
    uint32_t total = ni_u32(in), m = ni_u32(in);
    if (in->err || total != m + 4u) return NEX_EINVAL;
    ni_read(in, m);
    if (in->err) return NEX_EINVAL;
    if (bad) return NEX_CORE_INVALID_ARGUMENT;
    return do_register(s, c, urls, n, out);
}
static uint32_t sc_request_urls(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    (void)c;
    ni_u32(in);
    uint64_t pid = ni_u64(in);
    if (in->err) return NEX_EINVAL;
    NexConn *t = nex_by_pid(s, pid);
    char url[1024];
    int have = t ? t->registered && nex_station_url(s, t, url, sizeof url)
                 : s->store.station_url && s->store.station_url(s->store.ctx, pid, url, sizeof url);
    no_u8(out, have ? 1 : 0);
    no_u32(out, have ? 1 : 0);
    if (have) no_str(out, url);
    return 0;
}
static uint32_t sc_update_urls(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    (void)s; (void)out;
    char *urls[NEX_MAX_URLS];
    uint32_t n;
    int bad;
    if (read_urls(in, urls, &n, &bad)) return NEX_EINVAL;
    if (bad) return NEX_CORE_INVALID_ARGUMENT;
    set_urls(c, urls, n);
    return 0;
}
static uint32_t sc_replace_url(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    (void)s; (void)c; (void)out;
    ni_str(in); ni_str(in);
    return in->err ? NEX_EINVAL : 0;
}
static uint32_t empty_reply(NexServer *s, NexConn *c, NexIn *in, NexOut *out) { (void)s; (void)c; (void)in; (void)out; return 0; }
static uint32_t read_all_reply(NexServer *s, NexConn *c, NexIn *in, NexOut *out) { (void)s; (void)c; (void)out; in->p = in->end; return 0; }

/* ==== the Online menu's other services (tools/nex/services.py) ========================================= */
static uint32_t u32_zero(NexServer *s, NexConn *c, NexIn *in, NexOut *out) { (void)s; (void)c; (void)in; no_u32(out, 0); return 0; }
static uint32_t ut_integer_settings(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    (void)s; (void)c;
    ni_u32(in);
    if (in->err) return NEX_EINVAL;
    no_u32(out, 0);
    return 0;
}
static uint32_t ut_9(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    (void)s; (void)c;
    NexIn p = ni_struct(in, NULL);
    if (in->err) return NEX_EINVAL;
    ni_u32(&p); ni_u32(&p); ni_str(&p);
    if (p.err) return NEX_EINVAL;
    size_t at = no_struct_begin(out, 0);
    no_u32(out, 0); no_u32(out, 0); no_u32(out, 0);
    no_struct_end(out, at);
    return 0;
}
static uint32_t ut_10(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    (void)s; (void)c;
    in->p = in->end;
    size_t at = no_struct_begin(out, 0);
    no_u16(out, 0); no_u32(out, 0xFFFFFFFFu);
    no_struct_end(out, at);
    return 0;
}
static uint32_t ds_get_meta(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    (void)s; (void)c; (void)out;
    NexIn p = ni_struct(in, NULL);
    if (in->err) return NEX_EINVAL;
    ni_u64(&p);
    NexIn t = ni_struct(&p, NULL);
    if (p.err) return NEX_EINVAL;
    ni_u64(&t); ni_u16(&t);
    if (t.err) return NEX_EINVAL;
    return NEX_DATASTORE_NOT_FOUND;
}
/* ---- fighter records: DataStore 82 posts one, 84 gives them back (tools/nex/services.py Powers) ---- */
#define MAX_POWER_ENTRIES 4096
static uint64_t power_pid(const uint8_t *e) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | e[i]; return v; }
static int power_put(NexServer *s, uint64_t pid, const uint8_t *rec) {
    for (int i = 0; i < s->npowers; i++) {
        uint8_t *e = s->powers + (size_t)i * NEX_POWER_ENTRY;
        if (power_pid(e) == pid && e[8] == rec[0]) { memcpy(e + 8, rec, NEX_POWER_RECORD); return 1; }
    }
    if (s->npowers >= MAX_POWER_ENTRIES) return 0;
    if (s->npowers == s->cappowers) {
        s->cappowers = s->cappowers ? s->cappowers * 2 : 16;
        s->powers = realloc(s->powers, (size_t)s->cappowers * NEX_POWER_ENTRY);
        if (!s->powers) abort();
    }
    uint8_t *e = s->powers + (size_t)s->npowers++ * NEX_POWER_ENTRY;
    for (int i = 0; i < 8; i++) e[i] = (uint8_t)(pid >> (8 * i));
    memcpy(e + 8, rec, NEX_POWER_RECORD);
    return 1;
}
const uint8_t *nex_server_powers(NexServer *s, size_t *n) { *n = (size_t)s->npowers * NEX_POWER_ENTRY; return s->powers; }
void nex_server_set_powers(NexServer *s, const uint8_t *data, size_t n) {
    for (size_t at = 0; at + NEX_POWER_ENTRY <= n; at += NEX_POWER_ENTRY) power_put(s, power_pid(data + at), data + at + 8);
}
/* 115/82 (SSBU's own) {u8 fighter, u32, u32, u32, DateTime, u32} -> nothing: one of the player's fighter records
 * (its GSP), posted as a match is set up and after it; kept */
static uint32_t ds_post_power(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    (void)out;
    NexIn p = ni_struct(in, NULL);
    if (in->err) return NEX_EINVAL;
    if (p.end - p.p != NEX_POWER_RECORD) return NEX_CORE_INVALID_ARGUMENT;
    const uint8_t *r = p.d + p.p;
    nex_log(s, 1, "pid %llu: fighter %u's record: %u %u %u", (unsigned long long)c->pid, r[0],
            (unsigned)(r[1] | r[2] << 8 | r[3] << 16 | (uint32_t)r[4] << 24),
            (unsigned)(r[5] | r[6] << 8 | r[7] << 16 | (uint32_t)r[8] << 24),
            (unsigned)(r[9] | r[10] << 8 | r[11] << 16 | (uint32_t)r[12] << 24));
    memcpy(s->last_power, r, NEX_POWER_RECORD); s->last_power_pid = c->pid; s->last_power_new = s->has_last_power = 1;
    if (power_put(s, c->pid, r) && s->io.powers_changed) s->io.powers_changed(s->io.ctx);
    return 0;
}
uint32_t nex_power_gsp(const uint8_t *rec) { return rec[1] | rec[2] << 8 | rec[3] << 16 | (uint32_t)rec[4] << 24; }
static uint32_t power_top(NexServer *s, uint64_t pid) {
    uint32_t top = 0;
    for (int i = 0; i < s->npowers; i++) {
        const uint8_t *e = s->powers + (size_t)i * NEX_POWER_ENTRY;
        if (power_pid(e) == pid && nex_power_gsp(e + 8) > top) top = nex_power_gsp(e + 8);
    }
    return top;
}
int nex_server_take_power(NexServer *s, uint64_t *pid, uint8_t rec[NEX_POWER_RECORD], uint32_t *top) {
    if (!s->last_power_new) return 0;
    s->last_power_new = 0;
    *pid = s->last_power_pid;
    memcpy(rec, s->last_power, NEX_POWER_RECORD);
    *top = power_top(s, s->last_power_pid);
    return 1;
}
int nex_server_where(NexServer *s, NexWhere *w) {
    memset(w, 0, sizeof *w);
    w->fighter = -1;
    for (int i = 0; i < s->nconns[1] && !w->pid; i++) if (s->conns[1][i]->in_by_pid && s->conns[1][i]->pid) w->pid = s->conns[1][i]->pid;
    if (!w->pid) return 0;
    if (s->has_last_power && s->last_power_pid == w->pid) { w->fighter = s->last_power[0]; w->gsp = nex_power_gsp(s->last_power); }
    int n = s->store.count(s->store.ctx);
    for (int i = 0; i < n; i++) {
        NexSession *ms = s->store.at(s->store.ctx, i);
        if (!ms) continue;
        for (uint32_t k = 0; k < ms->nparticipants; k++) {
            if (ms->participants[k] != w->pid) continue;
            w->in_gathering = 1;
            w->gid = ms->gid; w->game_mode = ms->game_mode; w->state = ms->state; w->open = ms->open;
            w->players = ms->nparticipants;
            w->max_players = ms->max_participants;
            memcpy(w->arena_id, ms->arena_id, sizeof w->arena_id);
            w->arena_id[sizeof w->arena_id - 1] = 0;
            return 1;
        }
    }
    return 1;
}
/* 115/84 -> Map<u8 fighter, the records 82 posts> (by fighter), then a structure {u32, u32, u32, u64}: zeros */
static uint32_t ds_84(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    (void)in;
    const uint8_t *mine[256] = {0};
    uint32_t count = 0;
    for (int i = 0; i < s->npowers; i++) {
        const uint8_t *e = s->powers + (size_t)i * NEX_POWER_ENTRY;
        if (power_pid(e) == c->pid && !mine[e[8]]) { mine[e[8]] = e + 8; count++; }
    }
    no_u32(out, count);
    for (int f = 0; f < 256; f++) {
        if (!mine[f]) continue;
        no_u8(out, (uint32_t)f);
        size_t at = no_struct_begin(out, 0);
        no_put(out, mine[f], NEX_POWER_RECORD);
        no_struct_end(out, at);
    }
    size_t at = no_struct_begin(out, 0);
    no_u32(out, 0); no_u32(out, 0); no_u32(out, 0); no_u64(out, 0);
    no_struct_end(out, at);
    return 0;
}
/* 115/21 PostMetaBinary (DataStorePreparePostParam) -> u64 data id: nothing kept, the PID as the id */
static uint32_t ds_post_meta_binary(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    (void)s;
    ni_struct(in, NULL);
    if (in->err) return NEX_EINVAL;
    no_u64(out, c->pid);
    return 0;
}
/* 115/95 (SSBU's own, no parameters) -> structure {bool}: false */
static uint32_t ds_95(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    (void)s; (void)c; (void)in;
    size_t at = no_struct_begin(out, 0);
    no_u8(out, 0);
    no_struct_end(out, at);
    return 0;
}

static NexHandler core_handler(uint32_t protocol, uint32_t method) {
    static const uint8_t ds_no_reply[] = {3, 6, 11, 18, 22, 26, 31, 32, 37, 38, 41, 51, 56, 57, 58, 74, 76, 78, 79, 83, 86, 89, 91, 92, 94, 96, 97};
    switch (protocol) {
    case 10:
        if (method == 6) return tg_validate_with_param;
        if (method == 3) return tg_request_ticket;
        return NULL;
    case 11:
        switch (method) {
        case 1: return sc_register;
        case 4: return sc_register_ex;
        case 3: return sc_request_urls;
        case 6: return sc_update_urls;
        case 7: return sc_replace_url;
        case 8: return empty_reply;
        }
        return NULL;
    case 110:
        switch (method) {
        case 7: return ut_integer_settings;
        case 9: return ut_9;
        case 10: return ut_10;
        case 11: return u32_zero;
        }
        return NULL;
    case 115:
        if (method == 8) return ds_get_meta;
        if (method == 60 || method == 61) return u32_zero;
        if (method == 84) return ds_84;
        if (method == 82) return ds_post_power;
        if (method == 21) return ds_post_meta_binary;
        if (method == 95) return ds_95;
        for (size_t i = 0; i < sizeof ds_no_reply; i++) if (method == ds_no_reply[i]) return read_all_reply;
        return NULL;
    case 124:
        return method == 1 ? read_all_reply : NULL;
    }
    return NULL;
}

/* ==== the server ================================================================================= */
NexServer *nex_server_new(const NexIo *io, NexStore store, int verbose) {
    NexServer *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->io = *io;
    s->store = store;
    s->idle_timeout = IDLE_TIMEOUT;
    s->verbose = verbose;
    s->call_id = 1;
    s->next_round = (uint64_t)time(NULL) << 16;
    /* (drawn in tools/nex's order: the ticket key, then the four PRUDP servers' signature keys) */
    uint8_t unused[16];
    nex_random(s, s->server_key, 16);
    nex_random(s, unused, 16);   /* (UDP authentication) */
    nex_random(s, unused, 16);   /* (UDP secure) */
    nex_random(s, s->sig_key[0], 16);
    nex_random(s, s->sig_key[1], 16);
    return s;
}
void nex_server_free(NexServer *s) {
    if (!s) return;
    for (int k = 0; k < 2; k++) {
        for (int i = 0; i < s->nconns[k]; i++) conn_free(s->conns[k][i]);
        free(s->conns[k]);
    }
    end_call(s);
    free(s->dead);
    for (int i = 0; i < s->nstreams; i++) { no_free(&s->streams[i].in); no_free(&s->streams[i].lite); }
    free(s->streams);
    free(s->pia);
    free(s->identity);
    nex_ranked_free(s);
    free(s->powers);
    free(s->arena.p);
    if (s->store.destroy) s->store.destroy(s->store.ctx);
    free(s);
}
void nex_server_set_now(NexServer *s, double now) { s->now = now; }
void nex_server_set_idle_timeout(NexServer *s, double seconds) { s->idle_timeout = seconds; }
void nex_server_set_round_base(NexServer *s, uint64_t next_round) { s->next_round = next_round; }
void nex_server_set_identity(NexServer *s, const char *identity) {
    free(s->identity);
    s->identity = identity && *identity ? nex_strdup(identity) : NULL;
}

int nex_stream_open(NexServer *s, uint32_t ip, uint16_t port, uint16_t sport) {
    if (stream_find(s, ip, port, sport)) return 0;
    if (s->nstreams == s->capstreams) {
        s->capstreams = s->capstreams ? s->capstreams * 2 : 8;
        s->streams = realloc(s->streams, (size_t)s->capstreams * sizeof *s->streams);
        if (!s->streams) abort();
    }
    NexStream *st = &s->streams[s->nstreams++];
    memset(st, 0, sizeof *st);
    st->ip = ip; st->port = port; st->sport = sport;
    return 1;
}
void nex_stream_packet(NexServer *s, uint32_t ip, uint16_t port, uint16_t sport, const uint8_t *data, size_t n) {
    int k = sport == NEX_AUTH_PORT ? 0 : sport == NEX_SECURE_PORT ? 1 : -1;
    if (k >= 0) lite_in(s, k, ip, port, data, n);
    end_call(s);
}
void nex_stream_closed(NexServer *s, uint32_t ip, uint16_t port, uint16_t sport) {
    NexStream *st = stream_find(s, ip, port, sport);
    if (st) {
        no_free(&st->in); no_free(&st->lite);
        *st = s->streams[--s->nstreams];
    }
    int k = sport == NEX_AUTH_PORT ? 0 : 1;
    for (int i = 0; i < s->nconns[k];) {
        NexConn *c = s->conns[k][i];
        if (c->ip == ip && c->port == port) conn_close(s, c, "stream closed");
        else i++;
    }
    end_call(s);
}

static int ci_prefix(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char x = a[i] >= 'A' && a[i] <= 'Z' ? (char)(a[i] + 32) : a[i];
        if (x != b[i]) return 0;
    }
    return 1;
}
/* the stream's bytes as tools/ssbu_online_lobby.py takes them: raw PRUDP Lite, or a WebSocket (HTTP upgrade, then
 * frames: close ends it, a ping is answered); every whole Lite packet goes to the server */
static int lite_packets(NexServer *s, uint32_t ip, uint16_t port, uint16_t sport) {
    for (;;) {
        NexStream *st = stream_find(s, ip, port, sport);
        if (!st) return 0;
        if (st->lite.n > STREAM_BUFFER) return 0;
        long n = lite_size(st->lite.p, st->lite.n);
        if (n < 0 || n > STREAM_PACKET) return 0;
        if (n == 0 || (size_t)n > st->lite.n) return 1;
        uint8_t *pkt = malloc((size_t)n);
        if (!pkt) abort();
        memcpy(pkt, st->lite.p, (size_t)n);
        memmove(st->lite.p, st->lite.p + n, st->lite.n - (size_t)n);
        st->lite.n -= (size_t)n;
        nex_stream_packet(s, ip, port, sport, pkt, (size_t)n);
        free(pkt);
    }
}
int nex_stream_bytes(NexServer *s, uint32_t ip, uint16_t port, uint16_t sport, const uint8_t *data, size_t n) {
    NexStream *st = stream_find(s, ip, port, sport);
    if (!st) return 0;
    int ok = 1;
    if (st->mode == 0 && n) {
        if (data[0] == 'G') st->mode = 2;
        else if (data[0] == 0x80) st->mode = 1;
        else ok = 0;
    }
    if (ok && st->mode == 1) {
        no_put(&st->lite, data, n);
        ok = lite_packets(s, ip, port, sport);
    } else if (ok && st->mode >= 2) {
        no_put(&st->in, data, n);
        if (st->mode == 2) {
            size_t end = 0;
            for (size_t i = 0; i + 4 <= st->in.n; i++) if (!memcmp(st->in.p + i, "\r\n\r\n", 4)) { end = i + 4; break; }
            if (!end) ok = st->in.n <= 16384;
            else {
                char key[256] = "";
                for (size_t i = 0; i < end;) {   /* (the Sec-WebSocket-Key line, its value stripped) */
                    size_t e = i;
                    while (e + 1 < end && !(st->in.p[e] == '\r' && st->in.p[e + 1] == '\n')) e++;
                    const char *line = (const char *)st->in.p + i;
                    size_t len = e - i;
                    if (len >= 18 && ci_prefix(line, "sec-websocket-key:", 18)) {
                        const char *v = line + 18, *ve = line + len;
                        while (v < ve && (*v == ' ' || *v == '\t' || *v == '\v' || *v == '\f' || *v == '\r' || *v == '\n')) v++;
                        while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t' || ve[-1] == '\v' || ve[-1] == '\f' || ve[-1] == '\r' || ve[-1] == '\n')) ve--;
                        size_t kn = (size_t)(ve - v) < 200 ? (size_t)(ve - v) : 200;
                        memcpy(key, v, kn);
                        key[kn] = 0;
                        break;
                    }
                    i = e + 2;
                }
                char cat[300], acc[40];
                uint8_t h[20];
                snprintf(cat, sizeof cat, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
                nex_sha1((const uint8_t *)cat, strlen(cat), h);
                base64(h, 20, acc);
                char resp[512];
                int rn = snprintf(resp, sizeof resp, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                  "Sec-WebSocket-Accept: %s\r\nSec-WebSocket-Protocol: NEX\r\n\r\n", acc);
                if (s->io.stream_out) s->io.stream_out(s->io.ctx, ip, port, sport, (const uint8_t *)resp, (size_t)rn);
                memmove(st->in.p, st->in.p + end, st->in.n - end);
                st->in.n -= end;
                st->mode = 3;
            }
        }
        while (ok && st->mode == 3) {   /* (frames) */
            const uint8_t *b = st->in.p;
            size_t have = st->in.n;
            if (have < 2) break;
            uint64_t len = b[1] & 0x7F;
            size_t head = 2;
            if (len == 126) { if (have < 4) break; len = (uint64_t)b[2] << 8 | b[3]; head = 4; }
            else if (len == 127) { if (have < 10) break; len = 0; for (int i = 0; i < 8; i++) len = len << 8 | b[2 + i]; head = 10; }
            if (len > STREAM_PACKET * 4 || ((b[0] & 0x08) && len > 125)) { ok = 0; break; }
            int masked = (b[1] & 0x80) != 0;
            size_t need = head + (masked ? 4 : 0) + (size_t)len;
            if (have < need) break;
            uint32_t opcode = b[0] & 0x0F;
            uint8_t *payload = malloc((size_t)len + 1);
            if (!payload) abort();
            for (size_t i = 0; i < len; i++) payload[i] = b[head + (masked ? 4 : 0) + i] ^ (masked ? b[head + (i & 3)] : 0);
            memmove(st->in.p, st->in.p + need, st->in.n - need);
            st->in.n -= need;
            if (opcode == 8) { free(payload); ok = 0; break; }
            if (opcode == 9 && s->io.stream_out) {
                NexOut f = {0};
                ws_frame(&f, 10, payload, (size_t)len);
                s->io.stream_out(s->io.ctx, ip, port, sport, f.p, f.n);
                no_free(&f);
            }
            if (opcode == 0 || opcode == 2) {
                no_put(&st->lite, payload, (size_t)len);
                free(payload);
                ok = lite_packets(s, ip, port, sport);
                st = stream_find(s, ip, port, sport);
                if (!st) { ok = 0; break; }
            } else free(payload);
        }
    }
    end_call(s);
    return ok;
}

/* Pia's NAT check (nn::pia::nex::NatPropertyDetecter): 16-byte requests, a big-endian type, 101 and 102 to nncs1,
 * 103 to nncs2; the answer is the type, the port and address the request came from - the player's own virtual
 * ones: no NAT, EOS does the traversal - and 0, from where it went (102's from the other port) */
void nex_datagram(NexServer *s, uint32_t ip, uint16_t port, uint16_t dport, uint32_t to_ip, const uint8_t *data, size_t n) {
    int to_host = to_ip == NEX_HOST;
    if (dport == NEX_NAT_PORT1 || dport == NEX_NAT_PORT2) {
        if (n < 16) return;
        uint32_t kind = (uint32_t)data[0] << 24 | (uint32_t)data[1] << 16 | (uint32_t)data[2] << 8 | data[3];
        if (kind < 101 || kind > 103) { nex_log(s, 1, "NAT check of an unknown kind %u", kind); return; }
        uint16_t from = dport;
        if (kind == 102) from = dport == NEX_NAT_PORT1 ? NEX_NAT_PORT2 : NEX_NAT_PORT1;
        uint8_t r[16] = {(uint8_t)(kind >> 24), (uint8_t)(kind >> 16), (uint8_t)(kind >> 8), (uint8_t)kind, 0, 0, (uint8_t)(port >> 8), (uint8_t)port,
                         (uint8_t)(ip >> 24), (uint8_t)(ip >> 16), (uint8_t)(ip >> 8), (uint8_t)ip, 0, 0, 0, 0};
        if (s->io.datagram_out) s->io.datagram_out(s->io.ctx, ip, port, to_ip, from, r, 16);
    } else if (dport == NEX_PIA_KEEPALIVE_PORT && to_host && n >= 4 && !memcmp(data, "\x32\xab\x98\x64", 4)) {
        for (int i = 0; i < s->npia; i++) if (s->pia[i].ip == ip) { s->pia[i].port = port; return; }
        if (s->npia == s->cappia) {
            s->cappia = s->cappia ? s->cappia * 2 : 8;
            s->pia = realloc(s->pia, (size_t)s->cappia * sizeof *s->pia);
            if (!s->pia) abort();
        }
        s->pia[s->npia].ip = ip;
        s->pia[s->npia].port = port;
        s->npia++;
    }
    /* (PRUDP V1 over UDP, 60000/60001: not served - the game's NEX talks over a stream) */
}

void nex_server_tick(NexServer *s, double now) {
    s->now = now;
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < s->nconns[k];) {
            NexConn *c = s->conns[k][i];
            if (s->idle_timeout > 0 && now - c->seen > s->idle_timeout) conn_close(s, c, "timed out");
            else i++;
        }
    nex_ranked_tick(s, now);
    end_call(s);
}
