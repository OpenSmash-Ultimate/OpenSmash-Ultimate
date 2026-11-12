/* (net_nex_*.c) What the parts of the in-game NEX server share. */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net_nex.h"

#define NEX_SUCCESS 0x00010001u
#define NEX_CORE_NOT_IMPLEMENTED 0x80010002u
#define NEX_CORE_ACCESS_DENIED 0x80010006u
#define NEX_CORE_INVALID_ARGUMENT 0x8001000Au
#define NEX_CORE_BUFFER_OVERFLOW 0x8001000Fu
#define NEX_RV_INVALID_USERNAME 0x80030064u
#define NEX_RV_SESSION_FULL 0x800300C8u
#define NEX_RV_NOT_PARTICIPATED 0x800300D4u
#define NEX_RV_PERMISSION_DENIED 0x800300D9u
#define NEX_RV_SESSION_CLOSED 0x800300DBu
#define NEX_NO_GATHERING 0x8006000Au
#define NEX_WRONG_PASSWORD 0x8006000Bu
#define NEX_DATASTORE_NOT_FOUND 0x80690004u
#define NEX_NO_ROUND 0x80690002u

/* ---- encoding (tools/nex/streams.py) ---- */
typedef struct NexOut { uint8_t *p; size_t n, cap; } NexOut;
void no_put(NexOut *o, const void *d, size_t n);
void no_u8(NexOut *o, uint32_t v);
void no_u16(NexOut *o, uint32_t v);
void no_u32(NexOut *o, uint32_t v);
void no_u64(NexOut *o, uint64_t v);
void no_str(NexOut *o, const char *s);
void no_buf(NexOut *o, const uint8_t *d, size_t n);
void no_qbuf(NexOut *o, const uint8_t *d, size_t n);
size_t no_struct_begin(NexOut *o, uint32_t version);
void no_struct_end(NexOut *o, size_t at);
void no_free(NexOut *o);

typedef struct NexArena NexArena;
typedef struct NexIn { const uint8_t *d; size_t p, end; int err; NexArena *a; } NexIn;
NexIn ni_make(const uint8_t *d, size_t n, NexArena *a);
uint32_t ni_u8(NexIn *s);
uint32_t ni_u16(NexIn *s);
uint32_t ni_u32(NexIn *s);
uint64_t ni_u64(NexIn *s);
const uint8_t *ni_read(NexIn *s, size_t n);
char *ni_str(NexIn *s);                                  /* (decoded as Python's utf-8 "replace"; arena memory) */
const uint8_t *ni_buf(NexIn *s, size_t *n);
const uint8_t *ni_qbuf(NexIn *s, size_t *n);
uint32_t ni_list(NexIn *s);                              /* a list's count (error over 100000) */
NexIn ni_struct(NexIn *s, uint32_t *version);
int ni_eof(const NexIn *s);
void *nex_tmp(NexArena *a, size_t n);
char *nex_tmpstr(NexArena *a, const char *s);

/* UTF-8 text as Python's str sees it */
size_t u8_chars(const char *s);
size_t u8_prefix(const char *s, size_t chars);          /* bytes of the first `chars` characters */
char *nex_strdup(const char *s);

/* ---- crypto (tools/nex/crypto.py) ---- */
void nex_md5(const uint8_t *d, size_t n, uint8_t out[16]);
void nex_hmac_md5(const uint8_t *key, size_t kn, const uint8_t *d, size_t n, uint8_t out[16]);
void nex_sha1(const uint8_t *d, size_t n, uint8_t out[20]);

/* ---- the server ---- */
typedef struct NexHeld { int used; uint16_t seq; uint8_t frag; uint8_t *p; size_t n; } NexHeld;
#define NEX_HOLD_WINDOW 64
#define NEX_MAX_URLS 8

typedef struct NexConn {
    int secure;                         /* the secure server's (else authentication's) */
    uint32_t ip; uint16_t port;         /* the client's stream (addr) */
    uint8_t vport, local_vport, streams;
    int state;                          /* 0 syn, 1 open, 2 closed */
    int has_connect_ack;
    NexOut connect_ack;
    uint8_t support[256]; size_t support_n;
    uint8_t server_sig[16];
    uint8_t client_sig[256]; size_t client_sig_n;
    uint8_t session_key[32];
    uint16_t in_seq, out_seq;
    NexHeld held[NEX_HOLD_WINDOW];
    size_t held_bytes;
    NexOut frags;
    double seen;
    uint64_t pid;
    int in_by_pid;
    /* the service's */
    char *urls[NEX_MAX_URLS]; int nurls, has_urls, registered;
    uint32_t natm, natf;
    char *part; int has_part;
    uint32_t category; int has_category;
} NexConn;

typedef struct NexStream {
    uint32_t ip; uint16_t port, sport;
    int mode;                           /* 0 not known yet, 1 raw PRUDP Lite, 2 HTTP request, 3 WebSocket */
    NexOut in;                          /* bytes not taken yet (the HTTP request, WebSocket frames) */
    NexOut lite;                        /* PRUDP Lite bytes not a whole packet yet */
} NexStream;

typedef struct NexRound {
    uint64_t rid; uint32_t gid, category;
    uint64_t *pids; uint32_t npids;
    int ended, has_outcome;
    uint64_t by, reporter;
    uint64_t *opids; uint32_t *owl; uint32_t nout;   /* the first report's outcome: pid -> win/loss */
    double at;
} NexRound;

typedef struct NexArena { void **p; size_t n, cap; } NexArenaImpl;

struct NexServer {
    NexIo io;
    NexStore store;
    int verbose;
    double now;
    double idle_timeout;                /* (seconds without a packet that close a connection; 0: never) */
    uint8_t server_key[16];
    uint8_t sig_key[2][16];             /* the Lite servers': authentication, secure */
    NexConn **conns[2]; int nconns[2], capconns[2];
    NexConn **dead; int ndead, capdead;
    NexStream *streams; int nstreams, capstreams;
    uint32_t call_id;
    struct { uint32_t ip; uint16_t port; } *pia; int npia, cappia;
    char *identity;
    NexArena arena;
    /* ranked */
    NexStats **stats; int nstats, capstats;    /* in the order made */
    NexRound *rounds; int nrounds, caprounds;
    uint64_t next_round;
    /* the players' fighter records (DataStore 82 / 84): NEX_POWER_ENTRY bytes each, pid then the record */
    uint8_t *powers; int npowers, cappowers;
    uint8_t last_power[NEX_POWER_RECORD]; uint64_t last_power_pid; int last_power_new, has_last_power;   /* (nex_server_take_power) */
};

/* rng: the system's, or the test's (io.random) */
void nex_random(NexServer *s, uint8_t *out, size_t n);
uint64_t nex_datetime_now(NexServer *s);
void nex_log(NexServer *s, int level, const char *fmt, ...);
NexConn *nex_by_pid(NexServer *s, uint64_t pid);
void nex_call(NexServer *s, uint64_t pid, uint32_t protocol, uint32_t method, const uint8_t *body, size_t n);   /* by_pid's or the store's */
int nex_station_url(NexServer *s, NexConn *c, char *out, size_t cap);
int nex_url_of_pid(NexServer *s, uint64_t pid, char *out, size_t cap);

typedef uint32_t (*NexHandler)(NexServer *s, NexConn *c, NexIn *in, NexOut *out);
#define NEX_EINVAL NEX_CORE_INVALID_ARGUMENT
NexHandler nex_match_handler(uint32_t protocol, uint32_t method);
NexHandler nex_ranked_handler(uint32_t protocol, uint32_t method);
void nex_match_gone(NexServer *s, NexConn *c);
uint32_t nex_rating_of(NexServer *s, uint64_t pid);
void nex_ranked_free(NexServer *s);
void nex_ranked_tick(NexServer *s, double now);
void nex_notify(NexServer *s, const uint64_t *pids, uint32_t n, uint32_t kind, uint64_t source, uint64_t p1, uint64_t p2, const char *text, uint64_t p3);
