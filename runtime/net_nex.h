/* The NEX game server inside the game (plan/EOS_MIGRATION.md, phase 2): a C port of tools/nex - PRUDP Lite over
 * streams (raw or in WebSocket frames), RMC, logging in (Ticket Granting 10/6, 10/3; Secure Connection 11/1,3,4,6,7,8,
 * the Kerberos-style tickets with RC4 / HMAC-MD5), Utility / DataStore / Screening, Pia's NAT check (every player
 * "open": EOS does the real traversal), Matchmake Extension / Match Making / NAT Traversal, Battle Arenas and
 * Matchmake Referee (ranked, the same Elo as tools/nex/ranked.py).
 *
 * It answers byte for byte what tools/nex answers for the same requests (tests/test_nex_c.py drives both through
 * tools/dev/nextest.c). Differences, by design: no server database - a player's PID comes from its EOS product user
 * id or its account token (nex_pid_of); PRUDP V1 (UDP) is not served (the game's NEX talks over a stream); nobody is
 * gated (Python's may_talk is the lobby's; EOS carries every packet).
 *
 * The gatherings (matchmake sessions) and the players this server does not hold a connection of live behind NexStore,
 * so that phase 2 part B can keep them in EOS Lobbies; nex_store_local() keeps them in this process.
 *
 * Nothing here locks: the caller serializes every call on one NexServer (runtime/hle_online.c holds a lock). The
 * store's and the io's callbacks must not call back into the server (part B queues what it learns and hands it over
 * with nex_server_remote_call outside them). */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define NEX_HOST 0x0A0D0001u            /* 10.13.0.1: the game server, nncs1 */
#define NEX_NAT_SECOND 0x0A0DFFFEu      /* 10.13.255.254: nncs2 */
#define NEX_AUTH_PORT 60000
#define NEX_SECURE_PORT 60001
#define NEX_NAT_PORT1 10025
#define NEX_NAT_PORT2 10125
#define NEX_PIA_KEEPALIVE_PORT 33335
#define NEX_SERVER_PID 2
#define NEX_MAX_ATTRIBS 16
#define NEX_MAX_PARTICIPANTS 16

typedef struct NexBytes { uint8_t *p; size_t n; } NexBytes;

/* A MatchmakeSession (Gathering + MatchmakeSession fields, NEX 4), as tools/nex/matchmaking.py's Session. Strings are
 * UTF-8 with no NUL, owned by the session (malloc). */
typedef struct NexSession {
    uint32_t gid;
    uint64_t owner, host;
    uint16_t min_participants, max_participants;
    uint32_t policy, policy_arg, flags, state;
    char *description;
    uint32_t game_mode;
    uint32_t attribs[NEX_MAX_ATTRIBS];
    uint32_t nattribs;
    int open;
    uint32_t system_type;
    NexBytes app_buffer;
    uint8_t progress;
    NexBytes session_key;
    uint32_t option0;
    NexBytes mm_param;                  /* MatchmakeParam, as the client sent it */
    uint64_t started;                   /* NEX DateTime */
    char *user_password;
    uint32_t refer_gid;
    int user_password_enabled, system_password_enabled;
    char *codeword;
    uint8_t versions[2];                /* the Gathering and MatchmakeSession levels' version bytes */
    int data_level;                     /* the nn::nex::Data level's version, -1: none */
    uint64_t participants[NEX_MAX_PARTICIPANTS + 1];
    uint32_t nparticipants;
    char *part;                         /* who may meet: game build and lobby code (NULL: none) */
    double created;                     /* (monotonic seconds) */
    NexBytes extra;                     /* MatchmakeSession fields after the codeword, sent back as they came */
    char *system_password;
    char arena_id[8];                   /* (Battle Arenas) its arena ID, "" until asked for */
} NexSession;

/* A player's statistics for one category (Matchmake Referee). */
#define NEX_STAT_FIELDS 12
typedef struct NexStats {
    uint64_t unique_id, pid;
    uint32_t category;
    uint32_t f[NEX_STAT_FIELDS];        /* recentDisconnection ... totalDraw, as ranked.py's STAT_FIELDS */
    uint32_t rating;
} NexStats;

/* Where gatherings and other games' players live. Part A: nex_store_local(), this process alone. Part B backs it
 * with EOS Lobbies: one lobby a gathering (its fields as attributes), its members the participants; every other
 * game's player is a lobby member with its PID, station URL and rating as member attributes.
 *
 * The server owns no gathering: it asks the store, changes the NexSession it is given and says so with changed().
 * Whatever the store learns from elsewhere (another game joined, left, became the owner...) it passes to
 * nex_server_remote_event() so the local players are told, as the server tells them of its own changes. */
typedef struct NexStore {
    void *ctx;
    int (*count)(void *ctx);                                   /* gatherings known, in creation (gid) order... */
    NexSession *(*at)(void *ctx, int i);                       /* ...the i-th of them */
    NexSession *(*get)(void *ctx, uint32_t gid);               /* NULL: none */
    NexSession *(*add)(void *ctx, NexSession *ms);             /* a new gathering (the store takes it and gives
                                                                  it its gid, unique among all games) */
    void (*remove)(void *ctx, uint32_t gid);                   /* gone (the store frees it: nex_session_free) */
    void (*changed)(void *ctx, NexSession *ms);                /* changed by this server: publish it */
    /* players with no connection to this server */
    int (*station_url)(void *ctx, uint64_t pid, char *out, size_t cap);   /* its public station URL, 0: unknown */
    int (*rating)(void *ctx, uint64_t pid, uint32_t category, int has_category, uint32_t *out);   /* 0: unknown */
    void (*call)(void *ctx, uint64_t pid, uint32_t protocol, uint32_t method, const uint8_t *body, size_t n);
                                                               /* a NEX request (notification, probe) for it */
    void (*stats_changed)(void *ctx, const NexStats *e);       /* a local player's statistics changed (EOS Stats) */
    void (*destroy)(void *ctx);
    /* (optional: a store whose gatherings live elsewhere) before a search for gatherings (Quickplay, an arena ID, a
     * browse): bring count/at up to date for players of part, rating the searcher's, arena_id the ID asked for (NULL:
     * any); it may block a moment. claim: before a player of this server enters ms - 0: it cannot (full, gone). */
    void (*refresh)(void *ctx, const char *part, uint32_t rating, const char *arena_id);
    int (*claim)(void *ctx, NexSession *ms);
} NexStore;
NexStore nex_store_local(void);
void nex_session_free(NexSession *ms);

typedef struct NexServer NexServer;
/* out(port, ip, data): bytes for the stream from (ip, port) to server port sport; dgram: a datagram to (ip, port)
 * from (src_ip, sport). */
typedef struct NexIo {
    void *ctx;
    void (*stream_out)(void *ctx, uint32_t ip, uint16_t port, uint16_t sport, const uint8_t *data, size_t n);
    void (*datagram_out)(void *ctx, uint32_t ip, uint16_t port, uint32_t src_ip, uint16_t sport, const uint8_t *data, size_t n);
    const char *(*part_of)(void *ctx, uint32_t ip);            /* who may meet whom (NULL: none) */
    void (*log)(void *ctx, int level, const char *line);       /* (never an account's NSA id or token) */
    void (*random)(void *ctx, uint8_t *out, size_t n);         /* NULL: the system's */
    uint64_t (*datetime_now)(void *ctx);                       /* NULL: the system clock, as NEX's DateTime */
} NexIo;

NexServer *nex_server_new(const NexIo *io, NexStore store, int verbose);
void nex_server_free(NexServer *s);
void nex_server_set_now(NexServer *s, double now);             /* monotonic seconds (ratings' window, rounds) */
void nex_server_set_round_base(NexServer *s, uint64_t next_round);   /* (tests: ranked.py's int(time.time()) << 16) */
/* PIDs: by default from the token's NSA id; identity, when set (the EOS product user id), takes its place */
void nex_server_set_identity(NexServer *s, const char *identity);
uint64_t nex_pid_of(const char *key);

/* streams: what the stream layer of tools/ssbu_online_lobby.py does (raw PRUDP Lite or a WebSocket) */
int nex_stream_open(NexServer *s, uint32_t ip, uint16_t port, uint16_t sport);   /* 0: one is open already */
int nex_stream_bytes(NexServer *s, uint32_t ip, uint16_t port, uint16_t sport, const uint8_t *data, size_t n);
                                                               /* 0: the stream is to be closed (not NEX) */
void nex_stream_closed(NexServer *s, uint32_t ip, uint16_t port, uint16_t sport);
/* one PRUDP Lite packet already cut from a stream (NexServer.stream_packet) */
void nex_stream_packet(NexServer *s, uint32_t ip, uint16_t port, uint16_t sport, const uint8_t *data, size_t n);
/* a datagram to the server's address (to_ip: NEX_HOST) or nncs2's (NEX_NAT_SECOND) */
void nex_datagram(NexServer *s, uint32_t ip, uint16_t port, uint16_t dport, uint32_t to_ip, const uint8_t *data, size_t n);
void nex_server_tick(NexServer *s, double now);
/* (part B) something that happened elsewhere, for local players: a NEX request to pid */
void nex_server_remote_call(NexServer *s, uint64_t pid, uint32_t protocol, uint32_t method, const uint8_t *body, size_t n);
/* (part B) a matchmaking notification (14/1) for pid, if it is a player of this server */
void nex_server_notify(NexServer *s, uint64_t pid, uint32_t kind, uint64_t source, uint64_t p1, uint64_t p2);
/* (part B) this server's player (the first logged in to the secure server): 1 and its PID, station URL ("" before it
 * registered one) and rating; 0: none */
int nex_server_player(NexServer *s, uint64_t *pid, char *url, size_t cap, uint32_t *rating);
