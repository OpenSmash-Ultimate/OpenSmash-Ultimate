/* The in-game NEX server's ranked play (net_nex.h): Matchmake Referee (120) - tools/nex/ranked.py in C, with the
 * same Elo. Statistics are kept in this process (NexStore.stats_changed hands each change on: EOS Stats in part B). */
#define _CRT_SECURE_NO_WARNINGS
#include <math.h>

#include "net_nex_int.h"

#define K 32.0
#define DEFAULT_RATING 1500
#define MAX_RATING 99999
#define MAX_CATEGORIES 128
#define MAX_STATS 50000
#define MAX_TARGETS 256
#define MAX_ROUNDS 4096
#define ROUND_LIFETIME (3 * 3600.0)
enum { WIN = 1, LOSS = 2, DRAW = 3 };
enum { F_RECENT_WIN = 3, F_RECENT_LOSS, F_RECENT_DRAW, F_TOTAL_WIN = 9, F_TOTAL_LOSS, F_TOTAL_DRAW };

static uint32_t clamp_rating(long long v) { return (uint32_t)(v < 1 ? 1 : v > MAX_RATING ? MAX_RATING : v); }

static NexStats fresh(uint64_t pid, uint32_t category, uint32_t initial, int has_initial) {
    NexStats e;
    memset(&e, 0, sizeof e);
    e.unique_id = pid << 24 ^ category;
    e.category = category;
    e.pid = pid;
    e.rating = has_initial && initial ? clamp_rating(initial) : DEFAULT_RATING;
    return e;
}
static NexStats *find(NexServer *s, uint64_t pid, uint32_t category) {
    for (int i = 0; i < s->nstats; i++) if (s->stats[i]->pid == pid && s->stats[i]->category == category) return s->stats[i];
    return NULL;
}
static int count_of(NexServer *s, uint64_t pid) {
    int n = 0;
    for (int i = 0; i < s->nstats; i++) n += s->stats[i]->pid == pid;
    return n;
}
/* pid's statistics for category, made (and kept) if it has none; NULL if no more may be kept */
static NexStats *entry(NexServer *s, uint64_t pid, uint32_t category, uint32_t initial, int has_initial) {
    NexStats *e = find(s, pid, category);
    if (e) return e;
    if (count_of(s, pid) >= MAX_CATEGORIES || s->nstats >= MAX_STATS) return NULL;
    if (s->nstats == s->capstats) {
        s->capstats = s->capstats ? s->capstats * 2 : 16;
        s->stats = realloc(s->stats, (size_t)s->capstats * sizeof *s->stats);
        if (!s->stats) abort();
    }
    e = malloc(sizeof *e);
    if (!e) abort();
    *e = fresh(pid, category, initial, has_initial);
    s->stats[s->nstats++] = e;
    if (s->store.stats_changed) s->store.stats_changed(s->store.ctx, e);
    return e;
}
static NexStats look(NexServer *s, uint64_t pid, uint32_t category) {
    NexStats *e = find(s, pid, category);
    return e ? *e : fresh(pid, category, 0, 0);
}

/* a player's rating for matchmaking: the category the game asked about last, else the best */
uint32_t nex_rating_of(NexServer *s, uint64_t pid) {
    NexConn *c = nex_by_pid(s, pid);
    if (!count_of(s, pid)) {
        uint32_t r;
        if (!c && s->store.rating && s->store.rating(s->store.ctx, pid, 0, 0, &r)) return r;
        return DEFAULT_RATING;
    }
    if (c && c->has_category) {
        NexStats *e = find(s, pid, c->category);
        if (e) return e->rating;
    }
    uint32_t best = 0;
    for (int i = 0; i < s->nstats; i++) if (s->stats[i]->pid == pid && s->stats[i]->rating > best) best = s->stats[i]->rating;
    return best;
}

static void write_stats(NexOut *o, const NexStats *e) {
    size_t at = no_struct_begin(o, 0);
    no_u64(o, e->unique_id);
    no_u32(o, e->category);
    no_u64(o, e->pid);
    for (int i = 0; i < NEX_STAT_FIELDS; i++) no_u32(o, e->f[i]);
    no_u32(o, e->rating);
    no_struct_end(o, at);
}

void nex_ranked_tick(NexServer *s, double now) {
    int k = 0;
    while (k < s->nrounds && !(now - s->rounds[k].at < ROUND_LIFETIME && s->nrounds - k <= MAX_ROUNDS)) k++;
    for (int i = 0; i < k; i++) { free(s->rounds[i].pids); free(s->rounds[i].opids); free(s->rounds[i].owl); }
    if (k) { memmove(s->rounds, s->rounds + k, (size_t)(s->nrounds - k) * sizeof *s->rounds); s->nrounds -= k; }
}
void nex_ranked_free(NexServer *s) {
    nex_ranked_tick(s, 1e300);
    free(s->rounds);
    for (int i = 0; i < s->nstats; i++) free(s->stats[i]);
    free(s->stats);
}
static NexRound *round_of(NexServer *s, uint64_t rid) {
    for (int i = 0; i < s->nrounds; i++) if (s->rounds[i].rid == rid) return &s->rounds[i];
    return NULL;
}

/* ---- methods ---- */
static uint32_t rf_get_or_create(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    NexIn p = ni_struct(in, NULL);
    if (in->err) return NEX_EINVAL;
    uint32_t category = ni_u32(&p), initial = ni_u32(&p);
    if (p.err) return NEX_EINVAL;
    c->category = category;
    c->has_category = 1;
    NexStats *e = entry(s, c->pid, category, initial, 1), f;
    if (!e) { f = fresh(c->pid, category, initial, 1); e = &f; }
    nex_log(s, 1, "pid %llu: stats for category %u: rating %u", (unsigned long long)c->pid, category, e->rating);
    write_stats(out, e);
    return 0;
}
static uint32_t rf_create(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    (void)out;
    NexIn p = ni_struct(in, NULL);
    if (in->err) return NEX_EINVAL;
    uint32_t category = ni_u32(&p), initial = ni_u32(&p);
    if (p.err) return NEX_EINVAL;
    entry(s, c->pid, category, initial, 1);
    return 0;
}
static int target(NexIn *in, uint64_t *pid, uint32_t *category) {
    NexIn p = ni_struct(in, NULL);
    if (in->err) return 0;
    *pid = ni_u64(&p);
    *category = ni_u32(&p);
    return !p.err;
}
static uint32_t rf_primary(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    (void)c;
    uint64_t pid;
    uint32_t cat;
    if (!target(in, &pid, &cat)) return NEX_EINVAL;
    NexStats e = look(s, pid, cat);
    write_stats(out, &e);
    return 0;
}
static uint32_t rf_primaries(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    (void)c;
    uint32_t n = ni_u32(in);
    if (in->err) return NEX_EINVAL;
    if (n > MAX_TARGETS) return NEX_CORE_INVALID_ARGUMENT;
    uint64_t pids[MAX_TARGETS];
    uint32_t cats[MAX_TARGETS];
    for (uint32_t i = 0; i < n; i++) if (!target(in, &pids[i], &cats[i])) return NEX_EINVAL;
    no_u32(out, n);
    for (uint32_t i = 0; i < n; i++) { NexStats e = look(s, pids[i], cats[i]); write_stats(out, &e); }
    no_u32(out, n);
    for (uint32_t i = 0; i < n; i++) no_u32(out, NEX_SUCCESS);
    return 0;
}
static uint32_t rf_all(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    (void)c;
    uint64_t pid;
    uint32_t cat;
    if (!target(in, &pid, &cat)) return NEX_EINVAL;
    no_u32(out, (uint32_t)count_of(s, pid));
    for (int i = 0; i < s->nstats; i++) if (s->stats[i]->pid == pid) write_stats(out, s->stats[i]);
    return 0;
}

static uint32_t rf_start_round(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    NexIn p = ni_struct(in, NULL);
    if (in->err) return NEX_EINVAL;
    uint32_t category = ni_u32(&p), gid = ni_u32(&p), n = ni_list(&p);
    uint64_t *pids = nex_tmp(&s->arena, sizeof *pids * ((size_t)n + 1));
    for (uint32_t i = 0; i < n && !p.err; i++) pids[i] = ni_u64(&p);
    if (p.err) return NEX_EINVAL;
    NexSession *ms = s->store.get(s->store.ctx, gid);
    int in_it = 0;
    if (ms) for (uint32_t i = 0; i < ms->nparticipants; i++) in_it |= ms->participants[i] == c->pid;
    if (!in_it) return NEX_RV_NOT_PARTICIPATED;
    uint64_t *players = malloc(sizeof *players * ((size_t)n + 1));
    if (!players) abort();
    uint32_t np = 0;
    for (uint32_t i = 0; i < n; i++) {   /* (once each, in order, of the gathering's players) */
        int dup = 0, member = 0;
        for (uint32_t k = 0; k < ms->nparticipants; k++) member |= ms->participants[k] == pids[i];
        for (uint32_t k = 0; member && k < np; k++) dup |= players[k] == pids[i];
        if (!dup && member) players[np++] = pids[i];
    }
    nex_ranked_tick(s, s->now);
    if (s->nrounds == s->caprounds) {
        s->caprounds = s->caprounds ? s->caprounds * 2 : 16;
        s->rounds = realloc(s->rounds, (size_t)s->caprounds * sizeof *s->rounds);
        if (!s->rounds) abort();
    }
    NexRound *r = &s->rounds[s->nrounds++];
    memset(r, 0, sizeof *r);
    r->rid = s->next_round++;
    r->gid = gid; r->category = category; r->pids = players; r->npids = np; r->by = c->pid; r->at = s->now;
    nex_log(s, 1, "round %llu started by pid %llu: category %u, gathering %u, %u players", (unsigned long long)r->rid, (unsigned long long)c->pid, category, gid, np);
    uint64_t rid = r->rid, *others = nex_tmp(&s->arena, sizeof *others * ((size_t)np + 1));
    uint32_t no = 0;
    for (uint32_t i = 0; i < np; i++) if (players[i] != c->pid) others[no++] = players[i];
    nex_notify(s, others, no, 116000, c->pid, rid, 0, "", 0);
    no_u64(out, rid);
    return 0;
}
static uint32_t rf_start_param(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    (void)c;
    uint64_t rid = ni_u64(in);
    if (in->err) return NEX_EINVAL;
    NexRound *r = round_of(s, rid);
    if (!r) return NEX_NO_ROUND;
    size_t at = no_struct_begin(out, 0);
    no_u32(out, r->category);
    no_u32(out, r->gid);
    no_u32(out, r->npids);
    for (uint32_t i = 0; i < r->npids; i++) no_u64(out, r->pids[i]);
    no_struct_end(out, at);
    return 0;
}
static int in_round(const NexRound *r, uint64_t pid) {
    if (pid == r->by) return 1;
    for (uint32_t i = 0; i < r->npids; i++) if (r->pids[i] == pid) return 1;
    return 0;
}

typedef struct Result { uint64_t pid; uint32_t winloss; } Result;
/* new ratings from one result a player, all worked out before any is changed */
static void apply(NexServer *s, uint32_t category, const Result *results, uint32_t n) {
    static const double score[4] = {0, 1.0, 0.0, 0.5};
    Result known[MAX_TARGETS + 64];
    uint32_t nk = 0;
    for (uint32_t i = 0; i < n && nk < sizeof known / sizeof known[0]; i++)
        if (results[i].winloss >= WIN && results[i].winloss <= DRAW) known[nk++] = results[i];
    if (nk < 2) return;
    NexStats *entries[MAX_TARGETS + 64];
    int missing = 0;
    for (uint32_t i = 0; i < nk; i++) missing |= !(entries[i] = entry(s, known[i].pid, category, 0, 0));
    if (missing) { nex_log(s, 1, "ratings for category %u not kept: too many statistics", category); return; }
    uint32_t before[MAX_TARGETS + 64], after[MAX_TARGETS + 64];
    for (uint32_t i = 0; i < nk; i++) before[i] = entries[i]->rating;
    for (uint32_t i = 0; i < nk; i++) {
        double delta = 0.0;
        for (uint32_t j = 0; j < nk; j++) {
            if (j == i) continue;
            long long diff = (long long)before[j] - (long long)before[i];
            if (diff < -4000) diff = -4000;
            if (diff > 4000) diff = 4000;
            double expected = 1 / (1 + pow(10.0, (double)diff / 400));
            delta += K * (score[known[i].winloss] - expected);
        }
        after[i] = clamp_rating((long long)nearbyint((double)before[i] + delta / (nk - 1)));   /* (Python's round: half to even) */
    }
    for (uint32_t i = 0; i < nk; i++) {
        NexStats *e = entries[i];
        e->rating = after[i];
        uint32_t w = known[i].winloss;
        e->f[w == WIN ? F_TOTAL_WIN : w == LOSS ? F_TOTAL_LOSS : F_TOTAL_DRAW]++;
        e->f[w == WIN ? F_RECENT_WIN : w == LOSS ? F_RECENT_LOSS : F_RECENT_DRAW]++;
        nex_log(s, 1, "rating of pid %llu (category %u): %u -> %u", (unsigned long long)e->pid, category, before[i], e->rating);
        if (s->store.stats_changed) s->store.stats_changed(s->store.ctx, e);
    }
}

static uint32_t rf_end_round(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    (void)out;
    NexIn p = ni_struct(in, NULL);
    if (in->err) return NEX_EINVAL;
    uint64_t rid = ni_u64(&p);
    uint32_t n = ni_list(&p);
    Result *results = nex_tmp(&s->arena, sizeof *results * ((size_t)n + 1));
    for (uint32_t i = 0; i < n && !p.err; i++) {
        NexIn q = ni_struct(&p, NULL);
        if (p.err) break;
        results[i].pid = ni_u64(&q);
        ni_u32(&q);
        results[i].winloss = ni_u32(&q);
        ni_u32(&q);
        size_t bn;
        ni_qbuf(&q, &bn);
        if (q.err) { p.err = 1; break; }
    }
    if (p.err) return NEX_EINVAL;
    NexRound *r = round_of(s, rid);
    if (!r) { nex_log(s, 1, "round %llu ended by pid %llu: no such round", (unsigned long long)rid, (unsigned long long)c->pid); return 0; }
    if (!in_round(r, c->pid)) return NEX_RV_PERMISSION_DENIED;
    Result *report = nex_tmp(&s->arena, sizeof *report * ((size_t)r->npids + 1));
    uint32_t nr = 0;
    for (uint32_t i = 0; i < n; i++) {   /* (one result a player of the round: the first) */
        int ok = 0, dup = 0;
        for (uint32_t k = 0; k < r->npids; k++) ok |= r->pids[k] == results[i].pid;
        for (uint32_t k = 0; k < nr; k++) dup |= report[k].pid == results[i].pid;
        if (ok && !dup) report[nr++] = results[i];
    }
    if (r->ended) {
        if (r->has_outcome) {
            int same = r->nout == nr;
            for (uint32_t i = 0; same && i < nr; i++) {
                int found = 0;
                for (uint32_t k = 0; k < r->nout; k++) if (r->opids[k] == report[i].pid) found = r->owl[k] == report[i].winloss;
                same = found;
            }
            if (!same) nex_log(s, 1, "round %llu: MISMATCH - pid %llu reports another outcome than pid %llu (the first counts)",
                               (unsigned long long)rid, (unsigned long long)c->pid, (unsigned long long)r->reporter);
        }
        return 0;
    }
    r->ended = 1; r->has_outcome = 1; r->reporter = c->pid;
    r->opids = malloc(sizeof *r->opids * ((size_t)nr + 1));
    r->owl = malloc(sizeof *r->owl * ((size_t)nr + 1));
    if (!r->opids || !r->owl) abort();
    for (uint32_t i = 0; i < nr; i++) { r->opids[i] = report[i].pid; r->owl[i] = report[i].winloss; }
    r->nout = nr;
    apply(s, r->category, report, nr);
    return 0;
}
static uint32_t rf_end_without_report(NexServer *s, NexConn *c, NexIn *in, NexOut *out) {
    (void)out;
    uint64_t rid = ni_u64(in);
    if (in->err) return NEX_EINVAL;
    NexRound *r = round_of(s, rid);
    if (r) {
        if (!in_round(r, c->pid)) return NEX_RV_PERMISSION_DENIED;
        r->ended = 1;
    }
    return 0;
}

NexHandler nex_ranked_handler(uint32_t protocol, uint32_t method) {
    if (protocol != 120) return NULL;
    switch (method) {
    case 1: return rf_start_round;
    case 2: return rf_start_param;
    case 3: return rf_end_round;
    case 4: return rf_end_without_report;
    case 8: return rf_primary;
    case 9: return rf_primaries;
    case 10: return rf_all;
    case 11: return rf_create;
    case 12: return rf_get_or_create;
    }
    return NULL;
}
