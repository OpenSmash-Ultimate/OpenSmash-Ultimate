/* The in-game NEX server's gatherings in EOS Lobbies (plan/EOS_MIGRATION.md, phase 2 part B): the NexStore that
 * makes Quickplay, rooms and Battle Arenas shared between games with no server of ours.
 *
 * One EOS lobby a gathering: its id "os1-<gid>" (the gid is drawn by the game that makes it: unique among games
 * with all but certainty, and the lobby id refuses a second one), its bucket "ssbu1g:<part>" (game build, lobby code,
 * simulation version, mods: net_part_key), its members the games whose players are in it. Public attributes: what a
 * search needs - GID, MODE, OPEN, ARENA (the Battle Arena ID, "-" none), RATING (the participants' average), NP,
 * MINP, MAXP, A0..A15 (the NEX attributes) - and the whole gathering, B0..Bn (BN pieces of base64, 60 characters
 * each: an attribute holds 64), which the server needs to answer a join. Only the lobby's owner may write them.
 *
 * Every game keeps its own copy of the gatherings its player is in (the server reads and changes them as it did
 * nex_store_local's) and the last search's results. A change by this game's server goes at once, reliable and in
 * order (P2P channel 1), to the games of the other participants, and to the lobby (if this game owns it; else the
 * owner publishes it when the change reaches it). Each copy has a version; an older one is not taken.
 *
 * The server's notifications to another game's player (NexStore.call) go to that game the same way, so they arrive
 * after the change they are about; that game hands them to its server (nex_server_remote_call). A game that leaves a
 * lobby without its server saying so (it closed, lost its connection) is taken out of the copies by the others, who
 * tell their players as the server would: 3002 (left) or 3007 (disconnected) to the owner, 4000 to all if it was the
 * owner. Players' station URLs and ratings are member attributes of the hub (net_eos.c), so every game of the lobby
 * code finds them by PID. Ratings go to EOS Stats too (RATING_C<category>, self-reported: the owner's choice).
 *
 * Searching (Quickplay, an arena ID, the arenas list) and entering a gathering another game made wait for EOS (a
 * few hundred ms; NexStore.refresh, claim): entering is a lobby join, so two games racing for the last place cannot
 * both have it - the second is told the gathering is full and the server looks again.
 *
 * Threads: the store's functions run under the server's lock (hle_online.c), every EOS call on net_eos.c's thread
 * (eos_post). What the EOS thread learns waits in a queue until nex_store_eos_sync. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "net.h"
#include "net_eos.h"
#include "net_eos_sdk.h"
#include "net_eos_lobby.h"

#ifdef _WIN32
#include <windows.h>

#define MAXG 128
#define PIECE_CHARS 60
#define MAX_PIECES 37
#define MSG_PIECE 1100
#define N_PARTICIPANT_LEFT 3002
#define N_PARTICIPANT_DISCONNECTED 3007
#define N_OWNER_CHANGED 4000

static int lg(void) { return net_log(); }
static double mono_now(void) { return (double)GetTickCount64() / 1000.0; }
static int64_t unix_ms(void) {
    FILETIME f;
    GetSystemTimeAsFileTime(&f);
    return (int64_t)((((uint64_t)f.dwHighDateTime << 32 | f.dwLowDateTime) - 116444736000000000ull) / 10000);
}
static void lobby_id_of(uint32_t gid, char out[16]) { snprintf(out, 16, "os1-%08x", gid); }
static uint32_t gid_of_lobby(const char *id) {
    unsigned v;
    return id && !strncmp(id, "os1-", 4) && sscanf(id + 4, "%8x", &v) == 1 ? v : 0;
}

/* ==== a gathering as bytes (the blob) ======================================================================= */
typedef struct Buf { uint8_t *p; size_t n, cap; } Buf;
static void b_put(Buf *b, const void *d, size_t n) {
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 512;
        while (cap < b->n + n) cap *= 2;
        b->p = realloc(b->p, cap);
        if (!b->p) abort();
        b->cap = cap;
    }
    if (n) memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void b_u8(Buf *b, uint32_t v) { uint8_t x = (uint8_t)v; b_put(b, &x, 1); }
static void b_u16(Buf *b, uint32_t v) { uint8_t x[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; b_put(b, x, 2); }
static void b_u32(Buf *b, uint32_t v) { for (int i = 0; i < 4; i++) b_u8(b, v >> (8 * i)); }
static void b_u64(Buf *b, uint64_t v) { for (int i = 0; i < 8; i++) b_u8(b, (uint32_t)(v >> (8 * i))); }
static void b_bytes(Buf *b, const NexBytes *x) { b_u32(b, (uint32_t)x->n); b_put(b, x->p, x->n); }
static void b_str(Buf *b, const char *s) {
    if (!s) { b_u16(b, 0xFFFF); return; }
    size_t n = strlen(s);
    if (n > 0xFFFE) n = 0xFFFE;
    b_u16(b, (uint32_t)n);
    b_put(b, s, n);
}
typedef struct Rd { const uint8_t *p; size_t n, at; int err; } Rd;
static const uint8_t *r_take(Rd *r, size_t n) {
    if (r->err || r->n - r->at < n) { r->err = 1; return NULL; }
    const uint8_t *p = r->p + r->at;
    r->at += n;
    return p;
}
static uint32_t r_u8(Rd *r) { const uint8_t *p = r_take(r, 1); return p ? p[0] : 0; }
static uint32_t r_u16(Rd *r) { const uint8_t *p = r_take(r, 2); return p ? (uint32_t)p[0] | (uint32_t)p[1] << 8 : 0; }
static uint32_t r_u32(Rd *r) { uint32_t v = 0; for (int i = 0; i < 4; i++) v |= r_u8(r) << (8 * i); return v; }
static uint64_t r_u64(Rd *r) { uint64_t v = 0; for (int i = 0; i < 8; i++) v |= (uint64_t)r_u8(r) << (8 * i); return v; }
static void r_bytes(Rd *r, NexBytes *x) {
    uint32_t n = r_u32(r);
    const uint8_t *p = r_take(r, n);
    free(x->p);
    x->p = malloc(n ? n : 1);
    if (!x->p) abort();
    if (p && n) memcpy(x->p, p, n);
    x->n = p ? n : 0;
}
static char *r_str(Rd *r) {
    uint32_t n = r_u16(r);
    if (n == 0xFFFF) return NULL;
    const uint8_t *p = r_take(r, n);
    char *s = malloc((size_t)n + 1);
    if (!s) abort();
    if (p && n) memcpy(s, p, n);
    s[p ? n : 0] = 0;
    return s;
}

#define BLOB_VERSION 1
static void blob_of(Buf *b, const NexSession *ms, uint64_t version, int64_t t_unix) {
    b_u8(b, BLOB_VERSION);
    b_u64(b, version);
    b_u32(b, ms->gid); b_u64(b, ms->owner); b_u64(b, ms->host);
    b_u16(b, ms->min_participants); b_u16(b, ms->max_participants);
    b_u32(b, ms->policy); b_u32(b, ms->policy_arg); b_u32(b, ms->flags); b_u32(b, ms->state);
    b_str(b, ms->description);
    b_u32(b, ms->game_mode);
    b_u8(b, ms->nattribs);
    for (uint32_t i = 0; i < ms->nattribs; i++) b_u32(b, ms->attribs[i]);
    b_u8(b, ms->open ? 1 : 0);
    b_u32(b, ms->system_type);
    b_bytes(b, &ms->app_buffer);
    b_u8(b, ms->progress);
    b_bytes(b, &ms->session_key);
    b_u32(b, ms->option0);
    b_bytes(b, &ms->mm_param);
    b_u64(b, ms->started);
    b_str(b, ms->user_password);
    b_u32(b, ms->refer_gid);
    b_u8(b, ms->user_password_enabled ? 1 : 0); b_u8(b, ms->system_password_enabled ? 1 : 0);
    b_str(b, ms->codeword);
    b_u8(b, ms->versions[0]); b_u8(b, ms->versions[1]);
    b_u32(b, (uint32_t)ms->data_level);
    b_u8(b, ms->nparticipants);
    for (uint32_t i = 0; i < ms->nparticipants; i++) b_u64(b, ms->participants[i]);
    b_str(b, ms->part);
    b_u64(b, (uint64_t)t_unix);
    b_bytes(b, &ms->extra);
    b_str(b, ms->system_password);
    b_str(b, ms->arena_id);
}
/* a new session from a blob (NULL: not one); its version and creation time (unix ms) */
static NexSession *blob_read(const uint8_t *p, size_t n, uint64_t *version, int64_t *t_unix) {
    Rd r = {p, n, 0, 0};
    if (r_u8(&r) != BLOB_VERSION) return NULL;
    NexSession *ms = calloc(1, sizeof *ms);
    if (!ms) abort();
    *version = r_u64(&r);
    ms->gid = r_u32(&r); ms->owner = r_u64(&r); ms->host = r_u64(&r);
    ms->min_participants = (uint16_t)r_u16(&r); ms->max_participants = (uint16_t)r_u16(&r);
    ms->policy = r_u32(&r); ms->policy_arg = r_u32(&r); ms->flags = r_u32(&r); ms->state = r_u32(&r);
    ms->description = r_str(&r);
    ms->game_mode = r_u32(&r);
    ms->nattribs = r_u8(&r);
    if (ms->nattribs > NEX_MAX_ATTRIBS) r.err = 1;
    for (uint32_t i = 0; i < ms->nattribs && !r.err; i++) ms->attribs[i] = r_u32(&r);
    ms->open = r_u8(&r) != 0;
    ms->system_type = r_u32(&r);
    r_bytes(&r, &ms->app_buffer);
    ms->progress = (uint8_t)r_u8(&r);
    r_bytes(&r, &ms->session_key);
    ms->option0 = r_u32(&r);
    r_bytes(&r, &ms->mm_param);
    ms->started = r_u64(&r);
    ms->user_password = r_str(&r);
    ms->refer_gid = r_u32(&r);
    ms->user_password_enabled = r_u8(&r) != 0; ms->system_password_enabled = r_u8(&r) != 0;
    ms->codeword = r_str(&r);
    ms->versions[0] = (uint8_t)r_u8(&r); ms->versions[1] = (uint8_t)r_u8(&r);
    ms->data_level = (int)r_u32(&r);
    ms->nparticipants = r_u8(&r);
    if (ms->nparticipants > NEX_MAX_PARTICIPANTS + 1) r.err = 1;
    for (uint32_t i = 0; i < ms->nparticipants && !r.err; i++) ms->participants[i] = r_u64(&r);
    ms->part = r_str(&r);
    *t_unix = (int64_t)r_u64(&r);
    r_bytes(&r, &ms->extra);
    ms->system_password = r_str(&r);
    char *arena = r_str(&r);
    if (arena) { snprintf(ms->arena_id, sizeof ms->arena_id, "%s", arena); free(arena); }
    if (!ms->description) ms->description = calloc(1, 1);
    if (!ms->user_password) ms->user_password = calloc(1, 1);
    if (!ms->codeword) ms->codeword = calloc(1, 1);
    if (!ms->system_password) ms->system_password = calloc(1, 1);
    if (r.err || !ms->description || !ms->user_password || !ms->codeword || !ms->system_password) { nex_session_free(ms); return NULL; }
    ms->created = mono_now() - (double)(unix_ms() - *t_unix) / 1000.0;
    return ms;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static char *b64(const uint8_t *p, size_t n) {
    char *o = malloc(n / 3 * 4 + 8), *w = o;
    if (!o) abort();
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? (uint32_t)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        *w++ = B64[v >> 18 & 63]; *w++ = B64[v >> 12 & 63];
        *w++ = i + 1 < n ? B64[v >> 6 & 63] : '=';
        *w++ = i + 2 < n ? B64[v & 63] : '=';
    }
    *w = 0;
    return o;
}
static uint8_t *unb64(const char *s, size_t *out_n) {
    size_t n = strlen(s);
    uint8_t *o = malloc(n / 4 * 3 + 3);
    if (!o) abort();
    size_t k = 0;
    uint32_t v = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        const char *c = s[i] == '=' ? NULL : strchr(B64, s[i]);
        if (!c || !s[i]) break;
        v = v << 6 | (uint32_t)(c - B64);
        bits += 6;
        if (bits >= 8) { bits -= 8; o[k++] = (uint8_t)(v >> bits); }
    }
    *out_n = k;
    return o;
}

/* ==== what is published: a gathering's attributes ======================================================== */
typedef struct Pub {
    uint32_t gid, max_members;
    char bucket[96], arena[8];
    int64_t mode, open, rating, np, minp, maxp;
    uint32_t attribs[NEX_MAX_ATTRIBS], nattribs;
    char *pieces;              /* the blob in base64 */
} Pub;
static void pub_free(Pub *p) { if (p) { free(p->pieces); free(p); } }

/* ==== the game thread's side: the copies (under the server's lock) ======================================= */
typedef struct G { NexSession *ms; int joined; uint64_t version; int64_t t_unix; } G;
static G gs[MAXG];
static int ng;
static NexSession *grave[MAXG * 2];   /* (removed: freed at the next sync, when no handler holds them) */
static int ngrave;
static uint32_t gone[16];   /* (gatherings this game left: a search may still list them for a while) */
static int ngone;
static uint64_t my_pid;
static uint32_t my_rating = 1500;

static G *entry(uint32_t gid) { for (int i = 0; i < ng; i++) if (gs[i].ms->gid == gid) return &gs[i]; return NULL; }
static void bury(G *e) {
    if (ngrave < (int)(sizeof grave / sizeof grave[0])) grave[ngrave++] = e->ms;
    else nex_session_free(e->ms);
    int i = (int)(e - gs);
    memmove(&gs[i], &gs[i + 1], (size_t)(ng - i - 1) * sizeof gs[0]);
    ng--;
}
static void insert(NexSession *ms, int joined, uint64_t version, int64_t t_unix) {   /* (in creation order) */
    if (ng == MAXG) { nex_session_free(ms); return; }
    int at = ng;
    while (at > 0 && gs[at - 1].t_unix > t_unix) at--;
    memmove(&gs[at + 1], &gs[at], (size_t)(ng - at) * sizeof gs[0]);
    gs[at].ms = ms; gs[at].joined = joined; gs[at].version = version; gs[at].t_unix = t_unix;
    ng++;
}
/* ms's fields to e's session (the pointer the server may hold stays) */
static void take(G *e, NexSession *ms) {
    NexSession *old = e->ms, tmp = *old;
    *old = *ms;
    *ms = tmp;
    nex_session_free(ms);
}
static int has(const NexSession *ms, uint64_t pid) { for (uint32_t i = 0; i < ms->nparticipants; i++) if (ms->participants[i] == pid) return 1; return 0; }
static uint32_t room_of(const NexSession *ms) { return ms->max_participants < NEX_MAX_PARTICIPANTS ? ms->max_participants : NEX_MAX_PARTICIPANTS; }

static uint32_t avg_rating(const NexSession *ms) {
    double sum = 0;
    for (uint32_t i = 0; i < ms->nparticipants; i++) {
        uint32_t r = 0;
        if (ms->participants[i] == my_pid) r = my_rating;
        else if (!eos_hub_player(ms->participants[i], NULL, NULL, 0, &r) || !r) r = 1500;
        sum += r;
    }
    return ms->nparticipants ? (uint32_t)(sum / ms->nparticipants) : 1500;
}
static Pub *pub_of(const G *e) {
    const NexSession *ms = e->ms;
    Pub *p = calloc(1, sizeof *p);
    if (!p) abort();
    p->gid = ms->gid;
    p->max_members = room_of(ms) < 2 ? 2 : room_of(ms);
    snprintf(p->bucket, sizeof p->bucket, "ssbu1g:%s", ms->part ? ms->part : "-");
    snprintf(p->arena, sizeof p->arena, "%s", ms->arena_id[0] ? ms->arena_id : "-");
    p->mode = ms->game_mode; p->open = ms->open ? 1 : 0; p->rating = avg_rating(ms); p->np = ms->nparticipants;
    p->minp = ms->min_participants; p->maxp = ms->max_participants;
    p->nattribs = ms->nattribs;
    memcpy(p->attribs, ms->attribs, sizeof p->attribs);
    Buf b = {0};
    blob_of(&b, ms, e->version, e->t_unix);
    p->pieces = b64(b.p, b.n);
    free(b.p);
    return p;
}

/* ---- messages to other games (P2P channel 1): a flag byte (1: more pieces follow), then the message ---- */
static void send_msg(const char *to, const uint8_t *m, size_t n) {
    uint8_t pkt[MSG_PIECE + 1];
    size_t at = 0;
    do {
        size_t k = n - at < MSG_PIECE ? n - at : MSG_PIECE;
        pkt[0] = at + k < n;
        memcpy(pkt + 1, m + at, k);
        if (!eos_send_reliable(to, pkt, (u32)(k + 1)) && lg()) fprintf(stderr, "[eos] gatherings: a message to %s was not sent\n", to);
        at += k;
    } while (at < n);
}
/* the game of pid (a hub member), waiting up to ms milliseconds for the hub to know it */
static int game_of(uint64_t pid, char id[NET_EOS_ID_LEN + 1], int wait_ms) {
    for (int t = 0;; t += 50) {
        if (eos_hub_player(pid, id, NULL, 0, NULL)) return 1;
        if (t >= wait_ms) return 0;
        eos_hub_refresh_soon();
        Sleep(50);
    }
}
static void send_blob(G *e) {   /* (to the other participants' games) */
    Buf b = {0};
    b_u8(&b, 'B');
    blob_of(&b, e->ms, e->version, e->t_unix);
    for (uint32_t i = 0; i < e->ms->nparticipants; i++) {
        char id[NET_EOS_ID_LEN + 1];
        if (e->ms->participants[i] == my_pid) continue;
        if (game_of(e->ms->participants[i], id, 500)) send_msg(id, b.p, b.n);
        else fprintf(stderr, "[eos] gatherings: no game for player %llu (gathering %u)\n", (unsigned long long)e->ms->participants[i], e->ms->gid);
    }
    free(b.p);
}

/* ==== the EOS thread's side: the lobbies this game is in ==================================================== */
typedef struct L { uint32_t gid; char id[16]; int state; /* 1 being made, 2 in */ int leave_after, busy; Pub *pub; int dirty; uint64_t at; } L;
enum { LEAVE = 1, CLOSE = 2 };   /* (do_leave: CLOSE the lobby if ours - the gathering is gone - else only LEAVE it: EOS makes another member
                                  * its owner, and the gathering goes on without this game's player) */
static L ls[MAXG];
static int nl;
static L *lobby(uint32_t gid) { for (int i = 0; i < nl; i++) if (ls[i].gid == gid) return &ls[i]; return NULL; }
static void lobby_drop(L *l) { pub_free(l->pub); int i = (int)(l - ls); memmove(&ls[i], &ls[i + 1], (size_t)(nl - i - 1) * sizeof ls[0]); nl--; }
static int i_own(const char *lobby_id) {
    EosCopyDetailsOptions o = {1, lobby_id, eos_me};
    EOS_HLobbyDetails det = NULL;
    if (p_EOS_Lobby_CopyLobbyDetailsHandle(eos_lobby_interface(), &o, &det) != EOS_SUCCESS) return 0;
    EosVersionOnly v = {1};
    int mine = p_EOS_LobbyDetails_GetLobbyOwner(det, &v) == eos_me;
    p_EOS_LobbyDetails_Release(det);
    return mine;
}

/* the blob in a lobby's details (search result or joined lobby): malloc'd, NULL none */
static uint8_t *details_blob(EOS_HLobbyDetails det, size_t *n) {
    EosKeyOptions k = {1, "BN"};
    EosAttr *a = NULL;
    int pieces = 0;
    if (p_EOS_LobbyDetails_CopyAttributeByKey(det, &k, &a) == EOS_SUCCESS && a) {
        if (a->Data && a->Data->ValueType == EOS_AT_INT64) pieces = (int)a->Data->Value.AsInt64;
        p_EOS_Lobby_Attribute_Release(a);
    }
    if (pieces <= 0 || pieces > MAX_PIECES) return NULL;
    char *text = calloc((size_t)pieces * (PIECE_CHARS + 1) + 1, 1);
    if (!text) abort();
    for (int i = 0; i < pieces; i++) {
        char key[8];
        snprintf(key, sizeof key, "B%d", i);
        EosKeyOptions o = {1, key};
        a = NULL;
        if (p_EOS_LobbyDetails_CopyAttributeByKey(det, &o, &a) != EOS_SUCCESS || !a) { free(text); return NULL; }
        if (a->Data && a->Data->ValueType == EOS_AT_STRING && a->Data->Value.AsUtf8) strcat(text, a->Data->Value.AsUtf8);
        p_EOS_Lobby_Attribute_Release(a);
    }
    uint8_t *blob = unb64(text, n);
    free(text);
    return blob;
}

/* ---- the queue to the game thread ---- */
enum { EV_CALL, EV_BLOB, EV_MEMBER, EV_CLOSED };
typedef struct Ev { int kind, status; uint32_t gid; uint8_t *data; size_t n; char id[NET_EOS_ID_LEN + 1]; struct Ev *next; } Ev;
static Ev *ev_head, *ev_tail;
static SRWLOCK ev_lock = SRWLOCK_INIT;
static volatile LONG ev_waiting;
static void ev_push(int kind, uint32_t gid, const char *id, int status, const uint8_t *d, size_t n) {
    Ev *e = calloc(1, sizeof *e);
    if (!e) abort();
    e->kind = kind; e->gid = gid; e->status = status;
    if (id) snprintf(e->id, sizeof e->id, "%s", id);
    if (n) { e->data = malloc(n); if (!e->data) abort(); memcpy(e->data, d, n); e->n = n; }
    AcquireSRWLockExclusive(&ev_lock);
    if (ev_tail) ev_tail->next = e; else ev_head = e;
    ev_tail = e;
    ReleaseSRWLockExclusive(&ev_lock);
    InterlockedExchange(&ev_waiting, 1);
}

/* ---- publishing ---- */
static void EOS_CALL published(const EosLobbyResult *d) {
    L *l = lobby((uint32_t)(uintptr_t)d->ClientData);
    if (l) l->busy = 0;
    if (d->ResultCode != EOS_SUCCESS) {
        fprintf(stderr, "[eos] gatherings: gathering %u not published (%s)\n", (unsigned)(uintptr_t)d->ClientData, p_EOS_EResult_ToString(d->ResultCode));
        if (l) l->dirty = 1;
    }
}
static void add_int(EOS_HLobbyModification m, const char *key, int64_t v) {
    EosAttrData a = {1, key, {0}, EOS_AT_INT64};
    a.Value.AsInt64 = v;
    EosAddAttrOptions o = {2, &a, EOS_LAT_PUBLIC};
    p_EOS_LobbyModification_AddAttribute(m, &o);
}
static void add_str(EOS_HLobbyModification m, const char *key, const char *v) {
    EosAttrData a = {1, key, {0}, EOS_AT_STRING};
    a.Value.AsUtf8 = v;
    EosAddAttrOptions o = {2, &a, EOS_LAT_PUBLIC};
    p_EOS_LobbyModification_AddAttribute(m, &o);
}
static void publish(L *l) {   /* (EOS thread) its attributes, if this game owns the lobby */
    if (l->state != 2 || !l->pub || l->busy || !i_own(l->id)) { if (l->state == 2 && l->pub && !l->busy) l->dirty = 0; return; }
    Pub *p = l->pub;
    EosLobbyUserOptions o = {1, eos_me, l->id};
    EOS_HLobbyModification m = NULL;
    if (p_EOS_Lobby_UpdateLobbyModification(eos_lobby_interface(), &o, &m) != EOS_SUCCESS) return;
    add_int(m, "GID", p->gid); add_int(m, "MODE", p->mode); add_int(m, "OPEN", p->open); add_str(m, "ARENA", p->arena);
    add_int(m, "RATING", p->rating); add_int(m, "NP", p->np); add_int(m, "MINP", p->minp); add_int(m, "MAXP", p->maxp);
    for (uint32_t i = 0; i < p->nattribs; i++) { char k[8]; snprintf(k, sizeof k, "A%u", i); add_int(m, k, p->attribs[i]); }
    size_t len = strlen(p->pieces);
    int pieces = (int)((len + PIECE_CHARS - 1) / PIECE_CHARS);
    if (pieces > MAX_PIECES) {
        fprintf(stderr, "[eos] gatherings: gathering %u is too big for a lobby (%zu characters): others cannot join it\n", p->gid, len);
        pieces = 0;
    }
    for (int i = 0; i < pieces; i++) {
        char k[8], v[PIECE_CHARS + 1];
        size_t at = (size_t)i * PIECE_CHARS, n = len - at < PIECE_CHARS ? len - at : PIECE_CHARS;
        memcpy(v, p->pieces + at, n);
        v[n] = 0;
        snprintf(k, sizeof k, "B%d", i);
        add_str(m, k, v);
    }
    add_int(m, "BN", pieces);
    EosUpdateLobbyOptions u = {1, m};
    l->busy = 1;
    l->dirty = 0;
    l->at = GetTickCount64();
    p_EOS_Lobby_UpdateLobby(eos_lobby_interface(), &u, (void *)(uintptr_t)l->gid, published);
    p_EOS_LobbyModification_Release(m);
    if (lg()) fprintf(stderr, "[eos] gatherings: gathering %u published (%lld players, open %lld, %d pieces)\n", p->gid, (long long)p->np, (long long)p->open, pieces);
}
static void EOS_CALL member_set(const EosLobbyResult *d) {
    if (d->ResultCode != EOS_SUCCESS) fprintf(stderr, "[eos] gatherings: our member attribute in %s not set (%s)\n", d->LobbyId ? d->LobbyId : "?", p_EOS_EResult_ToString(d->ResultCode));
}
static void set_member(L *l) {   /* our player's PID as our member attribute */
    EosLobbyUserOptions o = {1, eos_me, l->id};
    EOS_HLobbyModification m = NULL;
    if (p_EOS_Lobby_UpdateLobbyModification(eos_lobby_interface(), &o, &m) != EOS_SUCCESS) return;
    EosAttrData a = {1, "PID", {0}, EOS_AT_INT64};
    a.Value.AsInt64 = (int64_t)my_pid;
    EosAddAttrOptions ao = {2, &a, EOS_LAT_PUBLIC};
    p_EOS_LobbyModification_AddMemberAttribute(m, &ao);
    EosUpdateLobbyOptions u = {1, m};
    p_EOS_Lobby_UpdateLobby(eos_lobby_interface(), &u, NULL, member_set);
    p_EOS_LobbyModification_Release(m);
}
static void EOS_CALL left(const EosLobbyResult *d) {
    if (d->ResultCode != EOS_SUCCESS && lg()) fprintf(stderr, "[eos] gatherings: leaving %s: %s\n", d->LobbyId ? d->LobbyId : "?", p_EOS_EResult_ToString(d->ResultCode));
}
static void leave_lobby(L *l, int how) {
    EosLobbyUserOptions o = {1, eos_me, l->id};
    if (how == CLOSE && i_own(l->id)) {   /* (ours: everyone else still in it is told it closed) */
        p_EOS_Lobby_DestroyLobby(eos_lobby_interface(), &o, NULL, left);
        fprintf(stderr, "[eos] gatherings: lobby of gathering %u closed\n", l->gid);
    } else {
        p_EOS_Lobby_LeaveLobby(eos_lobby_interface(), &o, NULL, left);
        fprintf(stderr, "[eos] gatherings: left the lobby of gathering %u\n", l->gid);
    }
    lobby_drop(l);
}
static void EOS_CALL created(const EosLobbyResult *d) {
    L *l = lobby((uint32_t)(uintptr_t)d->ClientData);
    if (!l) return;
    if (d->ResultCode != EOS_SUCCESS) {
        fprintf(stderr, "[eos] gatherings: lobby of gathering %u not made (%s)\n", l->gid, p_EOS_EResult_ToString(d->ResultCode));
        lobby_drop(l);
        return;
    }
    l->state = 2;
    if (d->LobbyId && strcmp(d->LobbyId, l->id)) fprintf(stderr, "[eos] gatherings: lobby id %s, not %s\n", d->LobbyId, l->id);
    fprintf(stderr, "[eos] gatherings: lobby %s made for gathering %u\n", l->id, l->gid);
    if (l->leave_after) { leave_lobby(l, l->leave_after); return; }
    set_member(l);
    l->dirty = 1;
}
/* ---- work posted by the game thread ---- */
static void do_create(void *arg) {
    Pub *p = arg;
    if (nl == MAXG || lobby(p->gid)) { pub_free(p); return; }
    L *l = &ls[nl++];
    memset(l, 0, sizeof *l);
    l->gid = p->gid;
    lobby_id_of(p->gid, l->id);
    l->state = 1;
    l->pub = p;
    EosCreateLobbyOptions o;
    memset(&o, 0, sizeof o);
    o.ApiVersion = 10; o.LocalUserId = eos_me; o.MaxLobbyMembers = p->max_members; o.PermissionLevel = EOS_LPL_PUBLICADVERTISED;
    o.BucketId = p->bucket; o.bAllowInvites = 1; o.LobbyId = l->id; o.bEnableJoinById = 1;
    p_EOS_Lobby_CreateLobby(eos_lobby_interface(), &o, (void *)(uintptr_t)p->gid, created);
}
static void do_publish(void *arg) {
    Pub *p = arg;
    L *l = lobby(p->gid);
    if (!l) { pub_free(p); return; }
    pub_free(l->pub);
    l->pub = p;
    l->dirty = 1;
}
static void do_leave(void *arg) {   /* (arg: the gid, how << 32) */
    uint32_t gid = (uint32_t)(uintptr_t)arg;
    int how = (int)((uint64_t)(uintptr_t)arg >> 32);
    L *l = lobby(gid);
    if (!l) return;
    if (l->state == 1) { l->leave_after = how; return; }
    leave_lobby(l, how);
}
static void *leave_arg(uint32_t gid, int how) { return (void *)(uintptr_t)((uint64_t)how << 32 | gid); }
typedef struct Stat { uint32_t category, rating; } Stat;
static void EOS_CALL ingested(const EosIngestInfo *d) {
    static int said;
    if (d->ResultCode != EOS_SUCCESS && !said++) fprintf(stderr, "[eos] stats: rating not saved (%s): the product needs stats RATING_C<category> (Developer Portal)\n", p_EOS_EResult_ToString(d->ResultCode));
    else if (d->ResultCode == EOS_SUCCESS && lg()) fprintf(stderr, "[eos] stats: rating saved\n");
}
static void do_stat(void *arg) {
    Stat *st = arg;
    static EOS_HStats stats;
    if (!stats) stats = p_EOS_Platform_GetStatsInterface(eos_plat);
    char name[32];
    snprintf(name, sizeof name, "RATING_C%u", st->category);
    EosIngestData d = {1, name, (int32_t)st->rating};
    EosIngestOptions o = {3, eos_me, &d, 1, eos_me};
    if (stats) p_EOS_Stats_IngestStat(stats, &o, NULL, ingested);
    free(st);
}
/* The player's GSP for the leaderboards (plan/EOS_MIGRATION.md "Account, rank e classifiche"; the site's workflow reads
 * them): GSP_F<fighter kind> the fighter's, GSP_TOP the best of the player's fighters (both LATEST in the portal; the
 * leaderboard GSP_TOP ranks by the second). */
typedef struct Gsp { uint32_t fighter, gsp, top; } Gsp;
static void EOS_CALL gsp_ingested(const EosIngestInfo *d) {
    static int said;
    if (d->ResultCode != EOS_SUCCESS && !said++) fprintf(stderr, "[eos] stats: GSP not saved (%s): the product needs stats GSP_TOP and GSP_F0..GSP_F93 (Developer Portal)\n", p_EOS_EResult_ToString(d->ResultCode));
}
static void do_gsp(void *arg) {
    Gsp *g = arg;
    EOS_HStats stats = p_EOS_Platform_GetStatsInterface(eos_plat);
    char name[16];
    snprintf(name, sizeof name, "GSP_F%u", g->fighter);
    EosIngestData d[2] = {{1, name, (int32_t)(g->gsp > 0x7fffffff ? 0x7fffffff : g->gsp)}, {1, "GSP_TOP", (int32_t)(g->top > 0x7fffffff ? 0x7fffffff : g->top)}};
    EosIngestOptions o = {3, eos_me, d, 2, eos_me};
    if (stats && eos_me) p_EOS_Stats_IngestStat(stats, &o, NULL, gsp_ingested);
    free(g);
}
void nexeos_post_gsp(uint32_t fighter, uint32_t gsp, uint32_t top) {
    if (!eos_ready() || fighter > 255) return;
    Gsp *g = malloc(sizeof *g);
    if (!g) return;
    g->fighter = fighter; g->gsp = gsp; g->top = top;
    if (!eos_post(do_gsp, g)) free(g);
}

/* ---- waiting for EOS (a search, a join): the game thread waits, the EOS thread answers ---- */
typedef struct Found { uint8_t *blob; size_t n; } Found;
typedef struct Req {
    volatile LONG refs;
    HANDLE done;
    int ok;
    uint32_t gid, rating;
    char part[96], arena[16];
    EOS_HLobbySearch search;
    Found *found; int nfound;
    uint8_t *blob; size_t nblob;
} Req;
static void req_drop(Req *r) {
    if (InterlockedDecrement(&r->refs)) return;
    for (int i = 0; i < r->nfound; i++) free(r->found[i].blob);
    free(r->found);
    free(r->blob);
    CloseHandle(r->done);
    free(r);
}
static Req *req_new(void) {
    Req *r = calloc(1, sizeof *r);
    if (!r) abort();
    r->refs = 2;
    r->done = CreateEventA(NULL, TRUE, FALSE, NULL);
    return r;
}
static void EOS_CALL search_done(const EosFindInfo *d) {
    Req *r = d->ClientData;
    EosVersionOnly v = {1};
    uint32_t n = d->ResultCode == EOS_SUCCESS ? p_EOS_LobbySearch_GetSearchResultCount(r->search, &v) : 0;
    if (d->ResultCode != EOS_SUCCESS && d->ResultCode != 13 /* EOS_NotFound: none */) fprintf(stderr, "[eos] gatherings: search: %s\n", p_EOS_EResult_ToString(d->ResultCode));
    r->found = calloc(n + 1, sizeof *r->found);
    if (!r->found) abort();
    for (uint32_t i = 0; i < n; i++) {
        EosIndexOptions io = {1, i};
        EOS_HLobbyDetails det = NULL;
        if (p_EOS_LobbySearch_CopySearchResultByIndex(r->search, &io, &det) != EOS_SUCCESS) continue;
        size_t bn = 0;
        uint8_t *blob = details_blob(det, &bn);
        p_EOS_LobbyDetails_Release(det);
        if (blob) { r->found[r->nfound].blob = blob; r->found[r->nfound++].n = bn; }
    }
    p_EOS_LobbySearch_Release(r->search);
    r->search = NULL;
    r->ok = d->ResultCode == EOS_SUCCESS || d->ResultCode == 13;
    SetEvent(r->done);
    req_drop(r);
}
static void do_search(void *arg) {
    Req *r = arg;
    EosCreateSearchOptions so = {1, 50};
    if (p_EOS_Lobby_CreateLobbySearch(eos_lobby_interface(), &so, &r->search) != EOS_SUCCESS) { SetEvent(r->done); req_drop(r); return; }
    char bucket[128];
    snprintf(bucket, sizeof bucket, "ssbu1g:%s", r->part);
    EosAttrData b = {1, "bucket", {0}, EOS_AT_STRING};
    b.Value.AsUtf8 = bucket;
    EosSetParameterOptions pb = {1, &b, EOS_CO_EQUAL};
    p_EOS_LobbySearch_SetParameter(r->search, &pb);
    if (r->arena[0]) {
        EosAttrData a = {1, "ARENA", {0}, EOS_AT_STRING};
        a.Value.AsUtf8 = r->arena;
        EosSetParameterOptions pa = {1, &a, EOS_CO_EQUAL};
        p_EOS_LobbySearch_SetParameter(r->search, &pa);
    } else {
        EosAttrData o = {1, "OPEN", {0}, EOS_AT_INT64};
        o.Value.AsInt64 = 1;
        EosSetParameterOptions po = {1, &o, EOS_CO_EQUAL};
        p_EOS_LobbySearch_SetParameter(r->search, &po);
        EosAttrData s = {1, "minslotsavailable", {0}, EOS_AT_INT64};
        s.Value.AsInt64 = 1;
        EosSetParameterOptions ps = {1, &s, EOS_CO_GREATERTHANOREQUAL};
        p_EOS_LobbySearch_SetParameter(r->search, &ps);
        EosAttrData q = {1, "RATING", {0}, EOS_AT_INT64};
        q.Value.AsInt64 = r->rating;
        EosSetParameterOptions pq = {1, &q, EOS_CO_DISTANCE};
        p_EOS_LobbySearch_SetParameter(r->search, &pq);
    }
    EosFindOptions f = {1, eos_me};
    p_EOS_LobbySearch_Find(r->search, &f, r, search_done);
}
static void EOS_CALL joined(const EosLobbyResult *d) {
    Req *r = d->ClientData;
    if (d->ResultCode == EOS_SUCCESS) {
        L *l = lobby(r->gid);
        if (!l && nl < MAXG) { l = &ls[nl++]; memset(l, 0, sizeof *l); l->gid = r->gid; lobby_id_of(r->gid, l->id); }
        if (l) {
            l->state = 2;
            set_member(l);
            EosCopyDetailsOptions o = {1, l->id, eos_me};
            EOS_HLobbyDetails det = NULL;
            if (p_EOS_Lobby_CopyLobbyDetailsHandle(eos_lobby_interface(), &o, &det) == EOS_SUCCESS) {
                r->blob = details_blob(det, &r->nblob);
                p_EOS_LobbyDetails_Release(det);
            }
        }
        r->ok = 1;
        fprintf(stderr, "[eos] gatherings: joined the lobby of gathering %u\n", r->gid);
    } else fprintf(stderr, "[eos] gatherings: lobby of gathering %u not joined (%s)\n", r->gid, p_EOS_EResult_ToString(d->ResultCode));
    SetEvent(r->done);
    req_drop(r);
}
static void do_join(void *arg) {
    Req *r = arg;
    char id[16];
    lobby_id_of(r->gid, id);
    EosJoinByIdOptions o = {3, id, eos_me, 0, NULL, 0, 0};
    p_EOS_Lobby_JoinLobbyById(eos_lobby_interface(), &o, r, joined);
}
static int wait_for(Req *r, DWORD ms) {   /* 1: answered */
    int got = WaitForSingleObject(r->done, ms) == WAIT_OBJECT_0;
    if (!got) fprintf(stderr, "[eos] gatherings: no answer from EOS in %lu ms\n", (unsigned long)ms);
    return got;
}

/* ---- what EOS tells (EOS thread) ---- */
static void EOS_CALL member_status(const EosMemberStatusInfo *d) {
    uint32_t gid = gid_of_lobby(d->LobbyId);
    if (!gid) return;
    char id[NET_EOS_ID_LEN + 8];
    int32_t idn = sizeof id;
    if (!d->TargetUserId || p_EOS_ProductUserId_ToString(d->TargetUserId, id, &idn) != EOS_SUCCESS) return;
    int me = d->TargetUserId == eos_me;
    if (lg()) fprintf(stderr, "[eos] gatherings: lobby of gathering %u: %s%s status %d\n", gid, me ? "we (" : "", id, d->CurrentStatus);
    L *l = lobby(gid);
    if (d->CurrentStatus == EOS_LMS_PROMOTED) { if (me && l) l->dirty = 1; return; }   /* (the owner now: our copy is the lobby's) */
    if (d->CurrentStatus == EOS_LMS_CLOSED || (me && d->CurrentStatus != EOS_LMS_JOINED)) {
        if (l) lobby_drop(l);
        ev_push(EV_CLOSED, gid, id, d->CurrentStatus, NULL, 0);
        return;
    }
    if (!me && (d->CurrentStatus == EOS_LMS_LEFT || d->CurrentStatus == EOS_LMS_DISCONNECTED || d->CurrentStatus == EOS_LMS_KICKED))
        ev_push(EV_MEMBER, gid, id, d->CurrentStatus, NULL, 0);
}
static void EOS_CALL lobby_update(const EosLobbyUpdateInfo *d) {   /* (attributes changed: the owner published) */
    uint32_t gid = gid_of_lobby(d->LobbyId);
    if (!gid || !lobby(gid)) return;
    EosCopyDetailsOptions o = {1, d->LobbyId, eos_me};
    EOS_HLobbyDetails det = NULL;
    if (p_EOS_Lobby_CopyLobbyDetailsHandle(eos_lobby_interface(), &o, &det) != EOS_SUCCESS) return;
    size_t n = 0;
    uint8_t *blob = details_blob(det, &n);
    p_EOS_LobbyDetails_Release(det);
    if (blob) ev_push(EV_BLOB, gid, NULL, 0, blob, n);
    free(blob);
}
/* P2P channel 1 */
static struct { char id[NET_EOS_ID_LEN + 1]; Buf b; } inbox[16];
void nexeos_packet(const char *from, const u8 *data, u32 len) {
    if (len < 1) return;
    int k = -1, free_k = -1;
    for (int i = 0; i < 16; i++) {
        if (!strcmp(inbox[i].id, from)) { k = i; break; }
        if (!inbox[i].id[0] && free_k < 0) free_k = i;
    }
    if (k < 0) {
        if (free_k < 0) free_k = 0;
        k = free_k;
        snprintf(inbox[k].id, sizeof inbox[k].id, "%s", from);
        inbox[k].b.n = 0;
    }
    Buf *b = &inbox[k].b;
    b_put(b, data + 1, len - 1);
    if (data[0]) return;   /* (more pieces to come) */
    const uint8_t *m = b->p;
    size_t n = b->n;
    if (n >= 1 + 8 + 8 && m[0] == 'N') ev_push(EV_CALL, 0, from, 0, m + 1, n - 1);
    else if (n >= 1 + 1 + 8 + 4 && m[0] == 'B') {
        Rd r = {m + 1, n - 1, 0, 0};
        r_u8(&r); r_u64(&r);
        ev_push(EV_BLOB, r_u32(&r), from, 0, m + 1, n - 1);
    }
    b->n = 0;
    inbox[k].id[0] = 0;
}
void nexeos_tick(void) {
    static int notified;
    if (!notified) {
        EosVersionOnly v = {1};
        p_EOS_Lobby_AddNotifyLobbyMemberStatusReceived(eos_lobby_interface(), &v, NULL, member_status);
        p_EOS_Lobby_AddNotifyLobbyUpdateReceived(eos_lobby_interface(), &v, NULL, lobby_update);
        notified = 1;
    }
    u64 t = GetTickCount64();
    for (int i = 0; i < nl; i++) if (ls[i].dirty && !ls[i].busy && t - ls[i].at >= 250) publish(&ls[i]);
    if (ev_waiting) online_nex_deliver();
}

/* ==== the store (the server's lock held) ============================================================== */
static int st_count(void *ctx) { (void)ctx; return ng; }
static NexSession *st_at(void *ctx, int i) { (void)ctx; return i >= 0 && i < ng ? gs[i].ms : NULL; }
static NexSession *st_get(void *ctx, uint32_t gid) { (void)ctx; G *e = entry(gid); return e ? e->ms : NULL; }
static uint32_t new_gid(void) {
    static uint64_t x;
    if (!x) {
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        x = (uint64_t)q.QuadPart ^ nex_pid_of(eos_my_id() ? eos_my_id() : "-") << 20 ^ (uint64_t)unix_ms();
    }
    for (;;) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        uint32_t g = (uint32_t)(x >> 16) & 0x7FFFFFFF;
        if (g >= 0x10000 && !entry(g)) return g;
    }
}
static NexSession *st_add(void *ctx, NexSession *ms) {
    (void)ctx;
    ms->gid = new_gid();
    insert(ms, 1, 1, unix_ms());
    G *e = entry(ms->gid);
    if (!e) return ms;   /* (no room: never with MAXG gatherings a game) */
    if (!eos_post(do_create, pub_of(e))) fprintf(stderr, "[eos] gatherings: EOS is off: gathering %u stays in this game\n", ms->gid);
    fprintf(stderr, "[eos] gatherings: gathering %u made here (mode %u, %u players at most)\n", ms->gid, ms->game_mode, ms->max_participants);
    return ms;
}
static void st_remove(void *ctx, uint32_t gid) {
    (void)ctx;
    G *e = entry(gid);
    if (!e) return;
    if (e->joined) { eos_post(do_leave, leave_arg(gid, CLOSE)); gone[ngone++ % 16] = gid; }
    bury(e);
}
static void st_changed(void *ctx, NexSession *ms) {
    (void)ctx;
    G *e = entry(ms->gid);
    if (!e || !e->joined) return;
    e->version++;
    send_blob(e);
    if (!has(ms, my_pid)) {   /* (our player left it: so does this game) */
        eos_post(do_leave, leave_arg(ms->gid, LEAVE));
        gone[ngone++ % 16] = ms->gid;
        bury(e);
        return;
    }
    eos_post(do_publish, pub_of(e));
}
static int st_station_url(void *ctx, uint64_t pid, char *out, size_t cap) {
    (void)ctx;
    for (int t = 0;; t += 50) {
        if (eos_hub_player(pid, NULL, out, cap, NULL) && out[0]) return 1;
        if (t >= 2500) break;
        eos_hub_refresh_soon();
        Sleep(50);
    }
    fprintf(stderr, "[eos] gatherings: no station URL for player %llu\n", (unsigned long long)pid);
    return 0;
}
static int st_rating(void *ctx, uint64_t pid, uint32_t category, int has_category, uint32_t *out) {
    (void)ctx; (void)category; (void)has_category;
    uint32_t r = 0;
    if (!eos_hub_player(pid, NULL, NULL, 0, &r) || !r) return 0;
    *out = r;
    return 1;
}
static void st_call(void *ctx, uint64_t pid, uint32_t protocol, uint32_t method, const uint8_t *body, size_t n) {
    (void)ctx;
    char id[NET_EOS_ID_LEN + 1];
    if (!game_of(pid, id, 1000)) { fprintf(stderr, "[eos] gatherings: no game for player %llu: call %u/%u lost\n", (unsigned long long)pid, protocol, method); return; }
    Buf b = {0};
    b_u8(&b, 'N'); b_u64(&b, pid); b_u32(&b, protocol); b_u32(&b, method);
    b_put(&b, body, n);
    send_msg(id, b.p, b.n);
    free(b.p);
    if (lg()) fprintf(stderr, "[eos] gatherings: call %u/%u for player %llu sent to its game\n", protocol, method, (unsigned long long)pid);
}
static void st_stats_changed(void *ctx, const NexStats *e) {
    (void)ctx;
    if (e->pid != my_pid) return;
    Stat *s = malloc(sizeof *s);
    if (!s) abort();
    s->category = e->category; s->rating = e->rating;
    if (!eos_post(do_stat, s)) free(s);
}
static void st_refresh(void *ctx, const char *part, uint32_t rating, const char *arena_id) {
    (void)ctx;
    Req *r = req_new();
    snprintf(r->part, sizeof r->part, "%s", part ? part : "-");
    snprintf(r->arena, sizeof r->arena, "%s", arena_id ? arena_id : "");
    r->rating = rating;
    if (!eos_post(do_search, r)) { req_drop(r); req_drop(r); return; }
    u64 t0 = GetTickCount64();
    if (wait_for(r, 5000) && r->ok) {
        for (int i = ng - 1; i >= 0; i--) if (!gs[i].joined) bury(&gs[i]);   /* (the last search's: replaced) */
        int added = 0;
        for (int i = 0; i < r->nfound; i++) {
            uint64_t v;
            int64_t t;
            NexSession *ms = blob_read(r->found[i].blob, r->found[i].n, &v, &t);
            if (!ms) continue;
            int old = has(ms, my_pid);   /* (one this game left: the search's index is a moment behind) */
            for (int k = 0; k < 16 && !old; k++) old = gone[k] == ms->gid;
            if (old || entry(ms->gid)) { nex_session_free(ms); continue; }
            insert(ms, 0, v, t);
            added++;
        }
        fprintf(stderr, "[eos] gatherings: search%s%s: %d found in %llu ms\n", arena_id ? " for arena " : "", arena_id ? arena_id : "", added,
                (unsigned long long)(GetTickCount64() - t0));
    }
    req_drop(r);
}
static int st_claim(void *ctx, NexSession *ms) {
    (void)ctx;
    G *e = entry(ms->gid);
    if (!e) return 0;
    if (e->joined) return 1;
    Req *r = req_new();
    r->gid = ms->gid;
    if (!eos_post(do_join, r)) { req_drop(r); req_drop(r); return 0; }
    int ok = wait_for(r, 6000) && r->ok;
    if (ok && r->blob) {   /* (the lobby's latest: someone may have joined since the search) */
        uint64_t v;
        int64_t t;
        NexSession *latest = blob_read(r->blob, r->nblob, &v, &t);
        if (latest && latest->gid == e->ms->gid) { take(e, latest); e->version = v > e->version ? v : e->version; }
        else nex_session_free(latest);
    }
    req_drop(r);
    if (ok && (!e->ms->open || e->ms->nparticipants + 1 > room_of(e->ms))) {
        fprintf(stderr, "[eos] gatherings: gathering %u is full or closed now\n", ms->gid);
        eos_post(do_leave, leave_arg(ms->gid, LEAVE));
        ok = 0;
    }
    if (!ok) { bury(e); return 0; }
    e->joined = 1;
    return 1;
}
static void st_destroy(void *ctx) { (void)ctx; }

NexStore nex_store_eos(void) {
    NexStore st = {NULL, st_count, st_at, st_get, st_add, st_remove, st_changed, st_station_url, st_rating, st_call, st_stats_changed, st_destroy,
                   st_refresh, st_claim};
    return st;
}

/* a game left gid's lobby without its server saying so: as the server's leave() */
static void member_gone(NexServer *s, G *e, const char *id, int status) {
    NexSession *ms = e->ms;
    uint64_t pid = 0;
    for (uint32_t i = 0; i < ms->nparticipants && !pid; i++) {
        char gid_id[NET_EOS_ID_LEN + 1];
        if (eos_hub_player(ms->participants[i], gid_id, NULL, 0, NULL) ? !strcmp(gid_id, id) : nex_pid_of(id) == ms->participants[i]) pid = ms->participants[i];
    }
    if (!pid || pid == my_pid) return;
    uint32_t i = 0;
    while (ms->participants[i] != pid) i++;
    memmove(&ms->participants[i], &ms->participants[i + 1], (ms->nparticipants - i - 1) * sizeof ms->participants[0]);
    ms->nparticipants--;
    fprintf(stderr, "[eos] gatherings: player %llu's game left gathering %u (status %d)\n", (unsigned long long)pid, ms->gid, status);
    if (!ms->nparticipants) { bury(e); return; }
    uint32_t kind = status == EOS_LMS_LEFT ? N_PARTICIPANT_LEFT : N_PARTICIPANT_DISCONNECTED;
    if (ms->owner != pid) nex_server_notify(s, ms->owner, kind, pid, ms->gid, pid);
    else {
        uint64_t nw = ms->participants[0];
        for (uint32_t k = 0; k < ms->nparticipants; k++) nex_server_notify(s, ms->participants[k], N_OWNER_CHANGED, pid, ms->gid, nw);
        ms->owner = nw;
        if (ms->host == pid) ms->host = nw;
    }
    e->version++;
    eos_post(do_publish, pub_of(e));
}

void nex_store_eos_sync(NexServer *s) {
    for (int i = 0; i < ngrave; i++) nex_session_free(grave[i]);
    ngrave = 0;
    uint64_t pid;
    char url[512];
    uint32_t rating;
    if (nex_server_player(s, &pid, url, sizeof url, &rating)) {
        my_pid = pid;
        my_rating = rating;
        eos_hub_set_player(pid, url, rating);
    }
    if (!ev_waiting) return;
    InterlockedExchange(&ev_waiting, 0);
    AcquireSRWLockExclusive(&ev_lock);
    Ev *e = ev_head;
    ev_head = ev_tail = NULL;
    ReleaseSRWLockExclusive(&ev_lock);
    while (e) {
        Ev *next = e->next;
        G *g = e->gid ? entry(e->gid) : NULL;
        if (e->kind == EV_CALL) {
            Rd r = {e->data, e->n, 0, 0};
            uint64_t to = r_u64(&r);
            uint32_t protocol = r_u32(&r), method = r_u32(&r);
            if (!r.err) {
                if (lg()) fprintf(stderr, "[eos] gatherings: call %u/%u for player %llu from %s\n", protocol, method, (unsigned long long)to, e->id);
                nex_server_remote_call(s, to, protocol, method, e->data + r.at, e->n - r.at);
            }
        } else if (e->kind == EV_BLOB && g) {
            uint64_t v;
            int64_t t;
            NexSession *ms = blob_read(e->data, e->n, &v, &t);
            if (ms && ms->gid == g->ms->gid && v > g->version) {
                if (lg()) fprintf(stderr, "[eos] gatherings: gathering %u changed elsewhere (version %llu, %u players)\n", ms->gid, (unsigned long long)v, ms->nparticipants);
                take(g, ms);
                g->version = v;
                if (g->joined) eos_post(do_publish, pub_of(g));
            } else nex_session_free(ms);
        } else if (e->kind == EV_MEMBER && g && g->joined) member_gone(s, g, e->id, e->status);
        else if (e->kind == EV_CLOSED && g && g->joined) {
            fprintf(stderr, "[eos] gatherings: lobby of gathering %u gone (status %d)\n", e->gid, e->status);
            bury(g);
        }
        free(e->data);
        free(e);
        e = next;
    }
}

#else   /* (Linux, macOS: their EOS libraries come in phase 5) */
NexStore nex_store_eos(void) { return nex_store_local(); }
void nex_store_eos_sync(NexServer *s) { (void)s; }
void nexeos_post_gsp(uint32_t fighter, uint32_t gsp, uint32_t top) { (void)fighter; (void)gsp; (void)top; }
#endif
