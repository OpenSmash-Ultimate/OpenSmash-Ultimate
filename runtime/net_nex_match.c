/* The in-game NEX server's matchmaking (net_nex.h): Matchmake Extension (109), Match Making (21), Match Making Ext
 * (50), NAT Traversal (3) and Battle Arenas - tools/nex/matchmaking.py and arenas.py in C, the gatherings behind
 * NexStore (nex_store_local: this process). */
#define _CRT_SECURE_NO_WARNINGS
#include <math.h>

#include "net_nex_int.h"

#define N_PARTICIPANT_JOINED 3001
#define N_PARTICIPANT_LEFT 3002
#define N_PARTICIPANT_DISCONNECTED 3007
#define N_PARTICIPATION_ENDED 3008
#define N_OWNER_CHANGED 4000
#define N_GATHERING_UNREGISTERED 109000
#define N_HOST_CHANGED 110000
#define RATING_WINDOW 150
#define RATING_WIDENING 25
#define MAX_CRITERIA 16
#define MAX_JOIN_MESSAGE 256
#define MAX_URL 512
#define ID_LENGTH 5
#define MAX_ID_TEXT 32
#define MAX_FOUND 50
static const char ID_LETTERS[] = "0123456789ABCDEFGHJKLMNPQRSTUVWXY";

/* ==== gatherings ================================================================================== */
static void bytes_set(NexBytes *b, const uint8_t *p, size_t n) {
    free(b->p);
    b->p = malloc(n ? n : 1);
    if (!b->p) abort();
    if (n) memcpy(b->p, p, n);
    b->n = n;
}
static void str_set(char **s, const char *v) { free(*s); *s = nex_strdup(v); }
void nex_session_free(NexSession *ms) {
    if (!ms) return;
    free(ms->description); free(ms->app_buffer.p); free(ms->session_key.p); free(ms->mm_param.p); free(ms->user_password);
    free(ms->codeword); free(ms->part); free(ms->extra.p); free(ms->system_password);
    free(ms);
}
static NexSession *session_new(void) {
    NexSession *ms = calloc(1, sizeof *ms);
    if (!ms) abort();
    ms->min_participants = 1; ms->max_participants = 2;
    ms->open = 1;
    ms->data_level = -1;
    ms->description = nex_strdup(""); ms->user_password = nex_strdup(""); ms->codeword = nex_strdup(""); ms->system_password = nex_strdup("");
    static const uint8_t param[] = {0, 4, 0, 0, 0, 0, 0, 0, 0};
    bytes_set(&ms->mm_param, param, sizeof param);
    bytes_set(&ms->app_buffer, NULL, 0); bytes_set(&ms->session_key, NULL, 0); bytes_set(&ms->extra, NULL, 0);
    return ms;
}

/* Session.parse: from a stream at a MatchmakeSession - the Data level (if any), Gathering, MatchmakeSession. NULL:
 * not one (every way it fails is CORE_INVALID_ARGUMENT, as in tools/nex) */
static NexSession *session_parse(NexIn *s) {
    uint32_t v0, gv, mv;
    NexIn first = ni_struct(s, &v0);
    if (s->err) return NULL;
    int data_level = -1;
    if (first.end == first.p) { data_level = (int)v0; first = ni_struct(s, &gv); if (s->err) return NULL; }
    else gv = v0;
    NexIn m = ni_struct(s, &mv);
    if (s->err) return NULL;
    NexIn g = first;
    NexSession *ms = session_new();
    ms->data_level = data_level;
    ms->versions[0] = (uint8_t)gv; ms->versions[1] = (uint8_t)mv;
    ms->gid = ni_u32(&g); ms->owner = ni_u64(&g); ms->host = ni_u64(&g);
    ms->min_participants = (uint16_t)ni_u16(&g); ms->max_participants = (uint16_t)ni_u16(&g);
    ms->policy = ni_u32(&g); ms->policy_arg = ni_u32(&g); ms->flags = ni_u32(&g); ms->state = ni_u32(&g);
    str_set(&ms->description, ni_str(&g));
    if (g.err) goto bad;
    ms->game_mode = ni_u32(&m);
    uint32_t na = ni_list(&m);
    for (uint32_t i = 0; i < na && !m.err; i++) { uint32_t v = ni_u32(&m); if (i < NEX_MAX_ATTRIBS) ms->attribs[i] = v; }
    if (m.err || na > NEX_MAX_ATTRIBS) goto bad;
    ms->nattribs = na;
    ms->open = ni_u8(&m) != 0;
    ms->system_type = ni_u32(&m);
    size_t n;
    const uint8_t *b = ni_buf(&m, &n);
    bytes_set(&ms->app_buffer, b, n);
    ni_u32(&m);
    ms->progress = (uint8_t)ni_u8(&m);
    b = ni_buf(&m, &n);
    bytes_set(&ms->session_key, b, n);
    ms->option0 = ni_u32(&m);
    size_t start = m.p;
    ni_struct(&m, NULL);
    if (m.err) goto bad;
    bytes_set(&ms->mm_param, m.d + start, m.p - start);
    ms->started = ni_u64(&m);
    str_set(&ms->user_password, ni_str(&m));
    ms->refer_gid = ni_u32(&m);
    ms->user_password_enabled = ni_u8(&m) != 0;
    ms->system_password_enabled = ni_u8(&m) != 0;
    if (m.err) goto bad;
    if (!ni_eof(&m)) str_set(&ms->codeword, ni_str(&m));
    if (m.err) goto bad;
    bytes_set(&ms->extra, m.d + m.p, m.end - m.p);
    return ms;
bad:
    nex_session_free(ms);
    s->err = 1;
    return NULL;
}

static void session_encode(NexOut *o, const NexSession *ms, uint64_t now, int private_) {
    if (ms->data_level >= 0) { size_t at = no_struct_begin(o, (uint32_t)ms->data_level); no_struct_end(o, at); }
    size_t at = no_struct_begin(o, ms->versions[0]);
    no_u32(o, ms->gid); no_u64(o, ms->owner); no_u64(o, ms->host);
    no_u16(o, ms->min_participants); no_u16(o, ms->max_participants);
    no_u32(o, ms->policy); no_u32(o, ms->policy_arg); no_u32(o, ms->flags); no_u32(o, ms->state);
    no_str(o, ms->description);
    no_struct_end(o, at);
    at = no_struct_begin(o, ms->versions[1]);
    no_u32(o, ms->game_mode);
    no_u32(o, ms->nattribs);
    for (uint32_t i = 0; i < ms->nattribs; i++) no_u32(o, ms->attribs[i]);
    no_u8(o, ms->open ? 1 : 0);
    no_u32(o, ms->system_type);
    no_buf(o, ms->app_buffer.p, ms->app_buffer.n);
    no_u32(o, ms->nparticipants);
    no_u8(o, ms->progress);
    if (private_) no_buf(o, ms->session_key.p, ms->session_key.n); else no_buf(o, NULL, 0);
    no_u32(o, ms->option0);
    no_put(o, ms->mm_param.p, ms->mm_param.n);
    no_u64(o, ms->started ? ms->started : now);
    no_str(o, private_ ? ms->user_password : "");
    no_u32(o, ms->refer_gid);
    no_u8(o, ms->user_password_enabled ? 1 : 0);
    no_u8(o, ms->system_password_enabled ? 1 : 0);
    no_str(o, ms->codeword);
    no_put(o, ms->extra.p, ms->extra.n);
    no_struct_end(o, at);
}
static void encode_session(NexServer *s, NexOut *o, const NexSession *ms, int private_) { session_encode(o, ms, nex_datetime_now(s), private_); }

static uint32_t room(const NexSession *ms) { return ms->max_participants < NEX_MAX_PARTICIPANTS ? ms->max_participants : NEX_MAX_PARTICIPANTS; }
static int has_pid(const NexSession *ms, uint64_t pid) {
    for (uint32_t i = 0; i < ms->nparticipants; i++) if (ms->participants[i] == pid) return 1;
    return 0;
}
static int same_part(const char *a, const char *b) { return (!a && !b) || (a && b && !strcmp(a, b)); }

/* ---- the store ---- */
static NexSession *get(NexServer *s, uint32_t gid) { return s->store.get(s->store.ctx, gid); }
static void changed(NexServer *s, NexSession *ms) { if (s->store.changed) s->store.changed(s->store.ctx, ms); }
static NexSession *of_pid(NexServer *s, uint64_t pid) {
    int n = s->store.count(s->store.ctx);
    for (int i = 0; i < n; i++) {
        NexSession *ms = s->store.at(s->store.ctx, i);
        if (has_pid(ms, pid)) return ms;
    }
    return NULL;
}

/* ---- search criteria ---- */
typedef struct Rule { int any; long long lo, hi; } Rule;
typedef struct Criteria {
    Rule attribs[NEX_MAX_ATTRIBS]; uint32_t nattribs;
    Rule game_mode, min_participants, max_participants, system_type;
    uint32_t vacant;
} Criteria;

/* Python's int() of a filter's part: blanks around, a sign, digits with single underscores between */
static int py_int(const char *t, size_t n, long long *out) {
    size_t i = 0;
    while (i < n && (t[i] == ' ' || (t[i] >= 9 && t[i] <= 13) || (t[i] >= 28 && t[i] <= 31))) i++;
    while (n > i && (t[n - 1] == ' ' || (t[n - 1] >= 9 && t[n - 1] <= 13) || (t[n - 1] >= 28 && t[n - 1] <= 31))) n--;
    int neg = 0;
    if (i < n && (t[i] == '+' || t[i] == '-')) neg = t[i++] == '-';
    if (i >= n) return 0;
    long long v = 0;
    int digits = 0, last_us = 0;
    for (; i < n; i++) {
        if (t[i] == '_') { if (!digits || last_us) return 0; last_us = 1; continue; }
        if (t[i] < '0' || t[i] > '9') return 0;
        if (v < (1ll << 60)) v = v * 10 + (t[i] - '0');
        digits++; last_us = 0;
    }
    if (!digits || last_us) return 0;
    *out = neg ? -v : v;
    return 1;
}
static Rule rule_of(const char *text) {
    Rule r = {1, 0, 0};
    if (!*text || u8_chars(text) > 40) return r;
    const char *comma = strchr(text, ',');
    size_t ln = comma ? (size_t)(comma - text) : strlen(text);
    const char *hi = comma ? comma + 1 : "";
    long long lo, h;
    if (!py_int(text, ln, &lo)) return r;
    if (*hi) { if (!py_int(hi, strlen(hi), &h)) return r; }
    else h = lo;
    r.any = 0; r.lo = lo; r.hi = h;
    return r;
}
static int fits(const Rule *r, long long v) { return r->any || (r->lo <= v && v <= r->hi); }
static int criteria_parse(NexIn *s, Criteria *c) {
    memset(c, 0, sizeof *c);
    uint32_t n = ni_list(s);
    for (uint32_t i = 0; i < n && !s->err; i++) {
        char *t = ni_str(s);
        if (i < NEX_MAX_ATTRIBS) c->attribs[c->nattribs++] = rule_of(t);
    }
    c->game_mode = rule_of(ni_str(s));
    c->min_participants = rule_of(ni_str(s));
    c->max_participants = rule_of(ni_str(s));
    c->system_type = rule_of(ni_str(s));
    ni_u8(s); ni_u8(s); ni_u8(s); ni_u32(s);
    if (s->err) return 0;
    c->vacant = ni_eof(s) ? 1 : ni_u16(s);
    return !s->err;
}
static int criteria_matches(const Criteria *c, const NexSession *ms, uint32_t joining, int attribs) {
    if (!fits(&c->game_mode, ms->game_mode) || !fits(&c->system_type, ms->system_type)) return 0;
    if (!fits(&c->min_participants, ms->min_participants) || !fits(&c->max_participants, ms->max_participants)) return 0;
    if (attribs)
        for (uint32_t i = 0; i < c->nattribs && i < ms->nattribs; i++)
            if (!fits(&c->attribs[i], ms->attribs[i])) return 0;
    return ms->nparticipants + (joining > c->vacant ? joining : c->vacant) <= room(ms);
}

/* ==== notifications, joining and leaving ===================================================== */
void nex_notify(NexServer *s, const uint64_t *pids, uint32_t n, uint32_t kind, uint64_t source, uint64_t p1, uint64_t p2, const char *text, uint64_t p3) {
    NexOut body = {0};
    size_t at = no_struct_begin(&body, 0);
    no_u64(&body, source); no_u32(&body, kind); no_u64(&body, p1); no_u64(&body, p2); no_str(&body, text); no_u64(&body, p3);
    no_struct_end(&body, at);
    for (uint32_t i = 0; i < n; i++) nex_call(s, pids[i], 14, 1, body.p, body.n);
    no_free(&body);
}
static void notify1(NexServer *s, uint64_t pid, uint32_t kind, uint64_t source, uint64_t p1, uint64_t p2) { nex_notify(s, &pid, 1, kind, source, p1, p2, "", 0); }

/* pid out of ms (gone with its last player) */
static void leave(NexServer *s, uint64_t pid, NexSession *ms, uint32_t kind) {
    uint32_t i = 0;
    while (i < ms->nparticipants && ms->participants[i] != pid) i++;
    if (i == ms->nparticipants) return;
    memmove(&ms->participants[i], &ms->participants[i + 1], (ms->nparticipants - i - 1) * sizeof ms->participants[0]);
    ms->nparticipants--;
    if (!ms->nparticipants) {
        nex_log(s, 1, "gathering %u closed", ms->gid);
        s->store.remove(s->store.ctx, ms->gid);
        return;
    }
    if (ms->owner != pid) notify1(s, ms->owner, kind, pid, ms->gid, pid);
    else {
        uint64_t nw = ms->participants[0];
        nex_notify(s, ms->participants, ms->nparticipants, N_OWNER_CHANGED, pid, ms->gid, nw, "", 0);
        ms->owner = nw;
        if (ms->host == pid) ms->host = nw;
    }
    changed(s, ms);
}
static void leave_others(NexServer *s, NexConn *c, NexSession *keep) {
    NexSession *ms = of_pid(s, c->pid);
    if (ms && ms != keep) leave(s, c->pid, ms, N_PARTICIPANT_LEFT);
}
void nex_match_gone(NexServer *s, NexConn *c) {
    NexSession *ms = of_pid(s, c->pid);
    if (ms) leave(s, c->pid, ms, N_PARTICIPANT_DISCONNECTED);
}

static NexSession *new_session(NexServer *s, NexConn *c, NexSession *ms) {
    ms->owner = ms->host = c->pid;
    ms->nparticipants = 0;
    uint8_t key[32];
    nex_random(s, key, 32);
    bytes_set(&ms->session_key, key, 32);
    ms->started = nex_datetime_now(s);
    ms->created = s->now;
    free(ms->part);
    ms->part = c->has_part ? nex_strdup(c->part) : NULL;
    ms->arena_id[0] = 0;
    return s->store.add(s->store.ctx, ms);
}

/* c into ms (and out of any other): everyone in hears of the join - the joiner too - and the joiner of everyone
 * already in (Pia connects its mesh to the stations these notifications name) */
static void join(NexServer *s, NexConn *c, NexSession *ms, const char *message) {
    leave_others(s, c, ms);
    if (!has_pid(ms, c->pid) && ms->nparticipants <= NEX_MAX_PARTICIPANTS) ms->participants[ms->nparticipants++] = c->pid;
    char msg[MAX_JOIN_MESSAGE * 4 + 1];
    size_t cut = u8_prefix(message, MAX_JOIN_MESSAGE);
    memcpy(msg, message, cut);
    msg[cut] = 0;
    uint32_t count = ms->nparticipants;
    uint64_t parts[NEX_MAX_PARTICIPANTS + 1];
    memcpy(parts, ms->participants, count * sizeof parts[0]);
    changed(s, ms);
    nex_notify(s, parts, count, N_PARTICIPANT_JOINED, c->pid, ms->gid, c->pid, msg, count);
    for (uint32_t i = 0; i < count; i++)
        if (parts[i] != c->pid) nex_notify(s, &c->pid, 1, N_PARTICIPANT_JOINED, parts[i], ms->gid, parts[i], "", count);
    nex_log(s, 1, "pid %llu joined gathering %u (%u players)", (unsigned long long)c->pid, ms->gid, count);
}

static uint32_t session_of(NexServer *s, uint32_t gid, NexSession **out) {
    *out = get(s, gid);
    return *out ? 0 : NEX_NO_GATHERING;
}
static uint32_t member(NexServer *s, NexConn *c, uint32_t gid, NexSession **out) {
    uint32_t e = session_of(s, gid, out);
    if (e) return e;
    return has_pid(*out, c->pid) ? 0 : NEX_RV_NOT_PARTICIPATED;
}
static uint32_t managed(NexServer *s, NexConn *c, uint32_t gid, NexSession **out) {
    uint32_t e = member(s, c, gid, out);
    if (e) return e;
    return c->pid == (*out)->owner || c->pid == (*out)->host ? 0 : NEX_RV_PERMISSION_DENIED;
}

/* ==== Matchmake Extension (109) ================================================================ */
typedef struct ParamHead { NexSession *ms; uint32_t nextra, count; char *message; NexIn rest; } ParamHead;
/* the common start of Auto/CreateMatchmakeSessionParam */
static int param_head(NexIn *s, ParamHead *h) {
    NexIn p = ni_struct(s, NULL);
    if (s->err) return 0;
    h->ms = session_parse(&p);
    if (!h->ms) return 0;
    h->nextra = ni_list(&p);
    for (uint32_t i = 0; i < h->nextra && !p.err; i++) ni_u64(&p);
    ni_u32(&p); ni_u32(&p);
    h->message = ni_str(&p);
    h->count = ni_u16(&p);
    h->rest = p;
    if (p.err) { nex_session_free(h->ms); h->ms = NULL; return 0; }
    return 1;
}

static uint32_t mm_auto(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    ParamHead h;
    if (!param_head(in, &h)) return NEX_EINVAL;
    NexIn *p = &h.rest;
    uint32_t ncrit = ni_list(p);
    Criteria *crit = nex_tmp(&s->arena, sizeof *crit * (ncrit < MAX_CRITERIA ? ncrit : MAX_CRITERIA) + 1);
    uint32_t kept = 0;
    for (uint32_t i = 0; i < ncrit && !p->err; i++) {
        NexIn q = ni_struct(p, NULL);
        if (p->err) break;
        Criteria tmp;
        if (!criteria_parse(&q, &tmp)) { p->err = 1; break; }
        if (kept < MAX_CRITERIA) crit[kept++] = tmp;
    }
    uint32_t ntargets = ni_list(p);
    uint32_t *targets = nex_tmp(&s->arena, sizeof *targets * (ntargets < MAX_CRITERIA ? ntargets : MAX_CRITERIA) + 1);
    for (uint32_t i = 0; i < ntargets && !p->err; i++) { uint32_t v = ni_u32(p); if (i < MAX_CRITERIA) targets[i] = v; }
    if (p->err) { nex_session_free(h.ms); return NEX_EINVAL; }
    if (ncrit > MAX_CRITERIA || ntargets > MAX_CRITERIA) { nex_session_free(h.ms); return NEX_CORE_INVALID_ARGUMENT; }
    NexSession *source = h.ms;
    uint32_t joining = h.count > 1 + h.nextra ? h.count : 1 + h.nextra;
    const char *part = c->has_part ? c->part : NULL;
    double mine = nex_rating_of(s, c->pid);
    if (s->store.refresh) s->store.refresh(s->store.ctx, part, (uint32_t)mine, NULL);
    uint32_t refused[8], nrefused = 0;   /* (gatherings the store could not enter: another game filled them first) */
again:;
    NexSession *best = NULL, *best_fb = NULL;
    double best_d = 0, best_fb_d = 0;
    int n = s->store.count(s->store.ctx);
    for (int i = 0; i < n; i++) {
        NexSession *ms = s->store.at(s->store.ctx, i);
        if (!same_part(ms->part, part) || !ms->open || has_pid(ms, c->pid)) continue;
        int no = 0;
        for (uint32_t k = 0; k < nrefused; k++) if (refused[k] == ms->gid) no = 1;
        if (no) continue;
        if (ntargets) {
            int t = 0;
            for (uint32_t k = 0; k < ntargets; k++) if (targets[k] == ms->gid) t = 1;
            if (!t) continue;
        }
        if (ms->game_mode != source->game_mode && !ncrit) continue;
        if (!ncrit && ms->nparticipants + joining > room(ms)) continue;
        double sum = 0;
        for (uint32_t k = 0; k < ms->nparticipants; k++) sum += nex_rating_of(s, ms->participants[k]);
        double rating = ms->nparticipants ? sum / ms->nparticipants : 1500;
        double diff = fabs(rating - mine);
        int fallback = 0;
        if (ncrit) {
            int any = 0, loose = 0;
            for (uint32_t k = 0; k < ncrit; k++) if (criteria_matches(&crit[k], ms, joining, 1)) any = 1;
            if (!any) {
                for (uint32_t k = 0; k < ncrit; k++) if (criteria_matches(&crit[k], ms, joining, 0)) loose = 1;
                if (!loose) continue;
                fallback = 1;
            }
        }
        if (!fallback && diff > RATING_WINDOW + RATING_WIDENING * (s->now - ms->created)) fallback = 1;
        /* (the closest; a tie: the oldest gathering - gids are in order) */
        if (fallback) { if (!best_fb || diff < best_fb_d) { best_fb = ms; best_fb_d = diff; } }
        else if (!best || diff < best_d) { best = ms; best_d = diff; }
    }
    NexSession *found = best ? best : best_fb;
    if (found && s->store.claim && !s->store.claim(s->store.ctx, found)) {
        nex_log(s, 1, "gathering %u could not be entered: looking again", found->gid);
        if (nrefused < 8) { refused[nrefused++] = found->gid; goto again; }
        found = NULL;
    }
    uint32_t found_gid = found ? found->gid : 0;
    leave_others(s, c, found);
    if (found_gid) found = get(s, found_gid);   /* (still there: c was not in it) */
    if (!found) {
        found = new_session(s, c, source);
        source = NULL;
        nex_log(s, 1, "pid %llu opened gathering %u", (unsigned long long)c->pid, found->gid);
    }
    nex_session_free(source);
    join(s, c, found, h.message);
    encode_session(s, out, found, 1);
    return 0;
}

static uint32_t mm_create(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    ParamHead h;
    if (!param_head(in, &h)) return NEX_EINVAL;
    leave_others(s, c, NULL);
    NexSession *ms = new_session(s, c, h.ms);
    nex_log(s, 1, "pid %llu created gathering %u", (unsigned long long)c->pid, ms->gid);
    join(s, c, ms, h.message);
    encode_session(s, out, ms, 1);
    return 0;
}

static uint32_t mm_join(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    NexIn p = ni_struct(in, NULL);
    if (in->err) return NEX_EINVAL;
    uint32_t gid = ni_u32(&p);
    uint32_t n = ni_list(&p);
    for (uint32_t i = 0; i < n && !p.err; i++) ni_u64(&p);
    ni_u32(&p); ni_u32(&p); ni_u8(&p);
    char *user_password = ni_str(&p), *system_password = ni_str(&p), *message = ni_str(&p);
    if (p.err) return NEX_EINVAL;
    NexSession *ms;
    uint32_t e = session_of(s, gid, &ms);
    if (e) return e;
    if (!has_pid(ms, c->pid)) {
        if (!same_part(ms->part, c->has_part ? c->part : NULL)) return NEX_NO_GATHERING;
        if (!ms->open) return NEX_RV_SESSION_CLOSED;
        if (ms->nparticipants + 1 > room(ms)) return NEX_RV_SESSION_FULL;
        if (s->store.claim && !s->store.claim(s->store.ctx, ms)) return NEX_RV_SESSION_FULL;
        if (!(ms = get(s, gid))) return NEX_NO_GATHERING;
        if (ms->nparticipants + 1 > room(ms)) return NEX_RV_SESSION_FULL;   /* (the store's latest) */
    }
    if (*ms->system_password && strcmp(system_password, ms->system_password)) return NEX_WRONG_PASSWORD;
    if (ms->user_password_enabled && strcmp(user_password, ms->user_password)) return NEX_WRONG_PASSWORD;
    join(s, c, ms, message);
    encode_session(s, out, ms, 1);
    return 0;
}

#define MANAGED_GID(var) \
    uint32_t gid_ = ni_u32(in); \
    if (in->err) return NEX_EINVAL; \
    NexSession *var; \
    uint32_t e_ = managed(s, c, gid_, &var); \
    if (e_) return e_;

static uint32_t mm_close(NexServer *s, NexConn *c, NexIn *in, NexOut *out) { (void)out; MANAGED_GID(ms) ms->open = 0; changed(s, ms); return 0; }
static uint32_t mm_open(NexServer *s, NexConn *c, NexIn *in, NexOut *out) { (void)out; MANAGED_GID(ms) ms->open = 1; changed(s, ms); return 0; }
static uint32_t mm_modify_attribute(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    (void)out;
    MANAGED_GID(ms)
    uint32_t index = ni_u32(in), value = ni_u32(in);
    if (in->err) return NEX_EINVAL;
    if (index >= NEX_MAX_ATTRIBS) return NEX_CORE_INVALID_ARGUMENT;
    while (ms->nattribs <= index) ms->attribs[ms->nattribs++] = 0;
    ms->attribs[index] = value;
    changed(s, ms);
    return 0;
}
static uint32_t mm_update_app_buffer(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    (void)out;
    MANAGED_GID(ms)
    size_t n;
    const uint8_t *b = ni_buf(in, &n);
    if (in->err) return NEX_EINVAL;
    bytes_set(&ms->app_buffer, b, n);
    changed(s, ms);
    return 0;
}
static uint32_t mm_update_attributes(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    (void)out;
    MANAGED_GID(ms)
    uint32_t n = ni_list(in), v[NEX_MAX_ATTRIBS];
    for (uint32_t i = 0; i < n && !in->err; i++) { uint32_t x = ni_u32(in); if (i < NEX_MAX_ATTRIBS) v[i] = x; }
    if (in->err) return NEX_EINVAL;
    if (n > NEX_MAX_ATTRIBS) return NEX_CORE_INVALID_ARGUMENT;
    memcpy(ms->attribs, v, n * sizeof v[0]);
    ms->nattribs = n;
    changed(s, ms);
    return 0;
}
static uint32_t mm_update_progress(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    (void)out;
    uint32_t gid = ni_u32(in);
    if (in->err) return NEX_EINVAL;
    NexSession *ms;
    uint32_t e = member(s, c, gid, &ms);
    if (e) return e;
    uint32_t v = ni_u8(in);
    if (in->err) return NEX_EINVAL;
    ms->progress = (uint8_t)v;
    changed(s, ms);
    return 0;
}
static uint32_t mm_generate_password(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    MANAGED_GID(ms)
    uint8_t r[4];
    nex_random(s, r, 4);   /* (randbelow(10 ** 8): drawn as tests/test_nex_c.py's stand-in for secrets draws it) */
    uint32_t v = ((uint32_t)r[0] | (uint32_t)r[1] << 8 | (uint32_t)r[2] << 16 | (uint32_t)r[3] << 24) % 100000000u;
    char pw[16];
    snprintf(pw, sizeof pw, "%08u", v);
    str_set(&ms->system_password, pw);
    ms->system_password_enabled = 1;
    changed(s, ms);
    no_str(out, pw);
    return 0;
}
static uint32_t mm_clear_password(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    (void)out;
    MANAGED_GID(ms)
    str_set(&ms->system_password, "");
    ms->system_password_enabled = 0;
    changed(s, ms);
    return 0;
}
static uint32_t mm_find_by_gid(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    uint32_t gid = ni_u32(in);
    if (in->err) return NEX_EINVAL;
    NexSession *ms;
    uint32_t e = session_of(s, gid, &ms);
    if (e) return e;
    encode_session(s, out, ms, has_pid(ms, c->pid));
    return 0;
}
static uint32_t mm_update_part(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    (void)out;
    NexIn p = ni_struct(in, NULL);
    if (in->err) return NEX_EINVAL;
    uint32_t gid = ni_u32(&p);
    if (p.err) return NEX_EINVAL;
    NexSession *ms;
    uint32_t e = managed(s, c, gid, &ms);
    if (e) return e;
    uint32_t flags = ni_u32(&p), n = ni_list(&p), v[NEX_MAX_ATTRIBS];
    for (uint32_t i = 0; i < n && !p.err; i++) { uint32_t x = ni_u32(&p); if (i < NEX_MAX_ATTRIBS) v[i] = x; }
    int open = ni_u8(&p) != 0;
    size_t bn;
    const uint8_t *b = ni_buf(&p, &bn);
    uint32_t progress = ni_u8(&p);
    if (p.err) return NEX_EINVAL;
    if ((flags & 1) && n > NEX_MAX_ATTRIBS) return NEX_CORE_INVALID_ARGUMENT;
    if (flags & 1) { memcpy(ms->attribs, v, n * sizeof v[0]); ms->nattribs = n; }
    if (flags & 2) ms->open = open;
    if (flags & 4) bytes_set(&ms->app_buffer, b, bn);
    if (flags & 8) ms->progress = (uint8_t)progress;
    changed(s, ms);
    return 0;
}
static uint32_t mm_58(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    (void)s; (void)c;
    ni_u32(in);
    if (in->err) return NEX_EINVAL;
    no_u32(out, 0);
    return 0;
}

/* ---- Battle Arenas (tools/nex/arenas.py) ---- */
static NexSession *arena_named(NexServer *s, const char *code) {
    int n = s->store.count(s->store.ctx);
    for (int i = 0; i < n; i++) {
        NexSession *ms = s->store.at(s->store.ctx, i);
        if (ms->arena_id[0] && !strcmp(ms->arena_id, code)) return ms;
    }
    return NULL;
}
static uint32_t ar_arena_id(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    uint32_t gid = ni_u32(in);
    if (in->err) return NEX_EINVAL;
    NexSession *ms;
    uint32_t e = member(s, c, gid, &ms);
    if (e) return e;
    if (!ms->arena_id[0]) {
        char code[ID_LENGTH + 1];
        for (int tries = 0; tries < 1000; tries++) {
            for (int i = 0; i < ID_LENGTH; i++) {
                uint8_t b;
                nex_random(s, &b, 1);
                code[i] = ID_LETTERS[b % (sizeof ID_LETTERS - 1)];
            }
            code[ID_LENGTH] = 0;
            if (!arena_named(s, code)) break;
        }
        memcpy(ms->arena_id, code, ID_LENGTH + 1);
        changed(s, ms);
    }
    nex_log(s, 1, "arena ID of gathering %u: %s", gid, ms->arena_id);
    size_t at = no_struct_begin(out, 0);
    no_str(out, ms->arena_id);
    no_u64(out, nex_datetime_now(s));
    no_struct_end(out, at);
    return 0;
}
/* 56 (SSBU's own): an arena ID -> its gathering */
static uint32_t ar_56(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    NexIn t = ni_make(in->d + in->p, in->end - in->p, in->a);
    in->p = in->end;
    char *text = ni_str(&t);
    uint32_t gid = 0;
    if (!t.err) {
        char code[MAX_ID_TEXT * 4 + 1];
        size_t cut = u8_prefix(text, MAX_ID_TEXT);
        const char *b = text, *e = text + cut;
        while (b < e && (*b == ' ' || (*b >= 9 && *b <= 13) || (*b >= 28 && *b <= 31))) b++;
        while (e > b && (e[-1] == ' ' || (e[-1] >= 9 && e[-1] <= 13) || (e[-1] >= 28 && e[-1] <= 31))) e--;
        size_t n = (size_t)(e - b);
        for (size_t i = 0; i < n; i++) code[i] = b[i] >= 'a' && b[i] <= 'z' ? (char)(b[i] - 32) : b[i];
        code[n] = 0;
        if (s->store.refresh) s->store.refresh(s->store.ctx, c->has_part ? c->part : NULL, nex_rating_of(s, c->pid), code);
        NexSession *ms = arena_named(s, code);
        if (ms && same_part(ms->part, c->has_part ? c->part : NULL)) gid = ms->gid;
        if (!gid) return NEX_NO_GATHERING;
    }
    no_u32(out, gid);
    return 0;
}
static int browse_order(const void *a, const void *b) {
    const NexSession *x = *(NexSession *const *)a, *y = *(NexSession *const *)b;
    if (x->nparticipants != y->nparticipants) return x->nparticipants > y->nparticipants ? -1 : 1;
    return x->gid < y->gid ? -1 : x->gid > y->gid;
}
static uint32_t ar_browse(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    NexIn p = ni_struct(in, NULL);
    if (in->err) return NEX_EINVAL;
    Criteria cr;
    if (!criteria_parse(&p, &cr)) return NEX_EINVAL;
    if (s->store.refresh) s->store.refresh(s->store.ctx, c->has_part ? c->part : NULL, nex_rating_of(s, c->pid), NULL);
    int n = s->store.count(s->store.ctx), k = 0;
    NexSession **found = nex_tmp(&s->arena, sizeof *found * (size_t)(n + 1));
    for (int i = 0; i < n; i++) {
        NexSession *ms = s->store.at(s->store.ctx, i);
        if (same_part(ms->part, c->has_part ? c->part : NULL) && ms->open && !has_pid(ms, c->pid) && criteria_matches(&cr, ms, 1, 1)) found[k++] = ms;
    }
    qsort(found, (size_t)k, sizeof *found, browse_order);
    if (k > MAX_FOUND) k = MAX_FOUND;
    no_u32(out, (uint32_t)k);
    for (int i = 0; i < k; i++) encode_session(s, out, found[i], 0);
    return 0;
}
/* 51 FindMatchmakeSessionByParticipant (Battle Arenas > friends' arenas; request main+0x2503c0, answer parser main+0x253ae0 reads
 * List<the session in a wrapper>): no friends here, so an empty list; its parameters are not read */
static uint32_t ar_friends(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    (void)s; (void)c;
    in->p = in->end;
    no_u32(out, 0);
    return 0;
}

/* ==== Match Making (21), Match Making Ext (50) ========================================================= */
static uint32_t mk_session_urls(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    (void)c;
    uint32_t gid = ni_u32(in);
    if (in->err) return NEX_EINVAL;
    NexSession *ms;
    uint32_t e = session_of(s, gid, &ms);
    if (e) return e;
    char url[1024];
    int have = nex_url_of_pid(s, ms->host, url, sizeof url);
    no_u32(out, have ? 1 : 0);
    if (have) no_str(out, url);
    return 0;
}
static uint32_t mk_update_host(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    (void)out;
    uint32_t gid = ni_u32(in);
    if (in->err) return NEX_EINVAL;
    NexSession *ms;
    uint32_t e = member(s, c, gid, &ms);
    if (e) return e;
    int migrate = ni_u8(in) != 0;
    if (in->err) return NEX_EINVAL;
    ms->host = c->pid;
    if (migrate) ms->owner = c->pid;
    changed(s, ms);
    uint64_t others[NEX_MAX_PARTICIPANTS + 1];
    uint32_t k = 0;
    for (uint32_t i = 0; i < ms->nparticipants; i++) if (ms->participants[i] != c->pid) others[k++] = ms->participants[i];
    nex_notify(s, others, k, N_HOST_CHANGED, c->pid, ms->gid, 0, "", 0);
    return 0;
}
static uint32_t mk_migrate(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    (void)out;
    MANAGED_GID(ms)
    uint32_t n = ni_list(in);
    uint64_t nw = 0;
    int have = 0;
    for (uint32_t i = 0; i < n && !in->err; i++) {
        uint64_t p = ni_u64(in);
        if (!in->err && !have && has_pid(ms, p)) { nw = p; have = 1; }
    }
    if (in->err) return NEX_EINVAL;
    if (have && nw != ms->owner) {
        uint64_t old = ms->owner;
        ms->owner = nw;
        changed(s, ms);
        nex_notify(s, ms->participants, ms->nparticipants, N_OWNER_CHANGED, old, ms->gid, nw, "", 0);
    }
    return 0;
}
static uint32_t mk_unregister(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    uint32_t gid = ni_u32(in);
    if (in->err) return NEX_EINVAL;
    if (get(s, gid)) {
        NexSession *ms;
        uint32_t e = managed(s, c, gid, &ms);
        if (e) return e;
        uint64_t others[NEX_MAX_PARTICIPANTS + 1];
        uint32_t k = 0;
        for (uint32_t i = 0; i < ms->nparticipants; i++) if (ms->participants[i] != c->pid) others[k++] = ms->participants[i];
        s->store.remove(s->store.ctx, gid);
        nex_notify(s, others, k, N_GATHERING_UNREGISTERED, c->pid, gid, 0, "", 0);
    }
    no_u8(out, 1);
    return 0;
}
static uint32_t mx_end_participation(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    uint32_t gid = ni_u32(in);
    ni_str(in);
    if (in->err) return NEX_EINVAL;
    NexSession *ms = get(s, gid);
    if (ms) leave(s, c->pid, ms, N_PARTICIPATION_ENDED);
    no_u8(out, ms ? 1 : 0);
    return 0;
}
static uint32_t mx_participants(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    (void)c;
    uint32_t gid = ni_u32(in);
    if (in->err) return NEX_EINVAL;
    NexSession *ms;
    uint32_t e = session_of(s, gid, &ms);
    if (e) return e;
    ni_u8(in);
    if (in->err) return NEX_EINVAL;
    no_u32(out, ms->nparticipants);
    for (uint32_t i = 0; i < ms->nparticipants; i++) no_u64(out, ms->participants[i]);
    return 0;
}

/* ==== NAT Traversal (3) =========================================================================== */
/* a station URL's address field ("prudp:/address=...;port=..."): 1 and its value, or 0 */
static int url_address(const char *url, char *out, size_t cap) {
    const char *sep = strstr(url, ":/");
    const char *f = sep ? sep + 2 : url + strlen(url);
    for (;;) {
        const char *end = strchr(f, ';');
        size_t len = end ? (size_t)(end - f) : strlen(f);
        const char *eq = memchr(f, '=', len);
        size_t kn = eq ? (size_t)(eq - f) : len;
        if (kn == 7 && !strncmp(f, "address", 7)) {
            size_t vn = eq ? len - kn - 1 : 0;
            if (vn >= cap) vn = cap - 1;
            if (vn) memcpy(out, eq + 1, vn);
            out[vn] = 0;
            return 1;
        }
        if (!end) return 0;
        f = end + 1;
    }
}
/* whether a station (its virtual address and URLs) has one of these addresses (none_too: also a URL without one) */
static int station_has(NexConn *o, char addrs[][64], int n, int none_too) {
    char ipt[16], a[64];
    snprintf(ipt, sizeof ipt, "%u.%u.%u.%u", o->ip >> 24, o->ip >> 16 & 255, o->ip >> 8 & 255, o->ip & 255);
    for (int i = 0; i < n; i++) if (!strcmp(addrs[i], ipt)) return 1;
    for (int u = 0; u < o->nurls; u++) {
        int has = url_address(o->urls[u], a, sizeof a);
        if (!has) { if (none_too) return 1; continue; }
        for (int i = 0; i < n; i++) if (!strcmp(addrs[i], a)) return 1;
    }
    return 0;
}
static uint32_t nt_probe_ext(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    (void)out;
    uint32_t n = ni_list(in);
    char (*wanted)[64] = nex_tmp(&s->arena, 64 * (size_t)(n < 64 ? n : 64) + 64);
    int nw = 0;
    for (uint32_t i = 0; i < n && !in->err; i++) {
        char *t = ni_str(in), a[64];
        if (!in->err && nw < 64 && url_address(t, a, sizeof a)) memcpy(wanted[nw++], a, 64);
    }
    char *source = ni_str(in);
    if (in->err) return NEX_EINVAL;
    if (n > NEX_MAX_PARTICIPANTS || u8_chars(source) > MAX_URL) return NEX_CORE_INVALID_ARGUMENT;
    char src[1][64];
    int src_has = url_address(source, src[0], sizeof src[0]);
    /* the source must be the caller's own station: its address, or one of its URLs' */
    if (!station_has(c, src, src_has, !src_has)) return NEX_RV_PERMISSION_DENIED;
    NexSession *ms = of_pid(s, c->pid);
    if (!ms) return 0;
    NexOut body = {0};
    no_str(&body, source);
    uint64_t parts[NEX_MAX_PARTICIPANTS + 1];
    uint32_t np = ms->nparticipants;
    memcpy(parts, ms->participants, np * sizeof parts[0]);
    for (uint32_t i = 0; i < np; i++) {
        NexConn *o = nex_by_pid(s, parts[i]);
        if (o == c) continue;
        if (o) { if (station_has(o, wanted, nw, 0)) nex_call(s, parts[i], 3, 2, body.p, body.n); }
        else {   /* (another game's player: its station URL from the store) */
            char url[1024], a[64];
            if (s->store.station_url && s->store.station_url(s->store.ctx, parts[i], url, sizeof url) && url_address(url, a, sizeof a))
                for (int k = 0; k < nw; k++) if (!strcmp(wanted[k], a)) { nex_call(s, parts[i], 3, 2, body.p, body.n); break; }
        }
    }
    no_free(&body);
    return 0;
}
static uint32_t nt_report_result(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    (void)s; (void)c; (void)out;
    ni_u32(in); ni_u8(in); ni_u32(in);
    return in->err ? NEX_EINVAL : 0;
}
static uint32_t nt_report_detail(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    (void)s; (void)c; (void)out;
    ni_u32(in); ni_u8(in); ni_u32(in); ni_u32(in);
    return in->err ? NEX_EINVAL : 0;
}
static uint32_t nt_report_properties(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    (void)s; (void)out;
    uint32_t m = ni_u32(in), f = ni_u32(in);
    ni_u32(in);
    if (in->err) return NEX_EINVAL;
    c->natm = m; c->natf = f;
    return 0;
}
static uint32_t nt_relay_key(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    (void)c; (void)in;
    no_u32(out, 0);
    no_u64(out, nex_datetime_now(s));
    no_str(out, "");
    no_u16(out, 0);
    no_u32(out, 0);
    no_u32(out, 0x23380901);
    return 0;
}

NexHandler nex_match_handler(uint32_t protocol, uint32_t method) {
    switch (protocol) {
    case 109:
        switch (method) {
        case 40: return mm_auto;
        case 38: return mm_create;
        case 39: return mm_join;
        case 1: return mm_close;
        case 2: return mm_open;
        case 8: return mm_modify_attribute;
        case 11: return mm_update_app_buffer;
        case 12: return mm_update_attributes;
        case 34: return mm_update_progress;
        case 36: return mm_generate_password;
        case 37: return mm_clear_password;
        case 41: case 49: return mm_find_by_gid;
        case 44: return mm_update_part;
        case 58: return mm_58;
        case 55: return ar_arena_id;
        case 56: return ar_56;
        case 52: return ar_browse;
        case 51: return ar_friends;
        }
        return NULL;
    case 21:
        switch (method) {
        case 41: return mk_session_urls;
        case 42: return mk_update_host;
        case 44: return mk_migrate;
        case 2: return mk_unregister;
        }
        return NULL;
    case 50:
        if (method == 1) return mx_end_participation;
        if (method == 2) return mx_participants;
        return NULL;
    case 3:
        switch (method) {
        case 3: return nt_probe_ext;
        case 4: return nt_report_result;
        case 5: return nt_report_properties;
        case 6: return nt_relay_key;
        case 7: return nt_report_detail;
        }
        return NULL;
    }
    return NULL;
}

void nex_server_notify(NexServer *s, uint64_t pid, uint32_t kind, uint64_t source, uint64_t p1, uint64_t p2) {
    if (nex_by_pid(s, pid)) nex_notify(s, &pid, 1, kind, source, p1, p2, "", 0);
}

/* ==== the local store: this process's gatherings ======================================================= */
typedef struct LocalStore { NexSession **s; int n, cap; uint32_t next_gid; } LocalStore;
static int ls_count(void *ctx) { return ((LocalStore *)ctx)->n; }
static NexSession *ls_at(void *ctx, int i) { return ((LocalStore *)ctx)->s[i]; }
static NexSession *ls_get(void *ctx, uint32_t gid) {
    LocalStore *l = ctx;
    for (int i = 0; i < l->n; i++) if (l->s[i]->gid == gid) return l->s[i];
    return NULL;
}
static NexSession *ls_add(void *ctx, NexSession *ms) {
    LocalStore *l = ctx;
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->s = realloc(l->s, (size_t)l->cap * sizeof *l->s);
        if (!l->s) abort();
    }
    ms->gid = l->next_gid++;
    l->s[l->n++] = ms;
    return ms;
}
static void ls_remove(void *ctx, uint32_t gid) {
    LocalStore *l = ctx;
    for (int i = 0; i < l->n; i++)
        if (l->s[i]->gid == gid) {
            nex_session_free(l->s[i]);
            memmove(&l->s[i], &l->s[i + 1], (size_t)(l->n - i - 1) * sizeof *l->s);
            l->n--;
            return;
        }
}
static void ls_destroy(void *ctx) {
    LocalStore *l = ctx;
    for (int i = 0; i < l->n; i++) nex_session_free(l->s[i]);
    free(l->s);
    free(l);
}
NexStore nex_store_local(void) {
    LocalStore *l = calloc(1, sizeof *l);
    if (!l) abort();
    l->next_gid = 0x10000;
    NexStore st = {l, ls_count, ls_at, ls_get, ls_add, ls_remove, NULL, NULL, NULL, NULL, NULL, ls_destroy, NULL, NULL};
    return st;
}
