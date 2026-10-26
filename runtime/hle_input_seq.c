/* SSBU_SEQ's parser and step machine (input_seq.h). Nothing of the runtime: hle_hid.c gives
 * them player 1's polls, tools/dev/testruntest.c made-up sequences. */
#define _CRT_SECURE_NO_WARNINGS
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "input_seq.h"

/* The names SSBU_INPUT takes (hle_hid.c): NpadButton bits 0-15, then the left stick. */
static const char *const KEY_NAMES[] = {"A", "B", "X", "Y", "LSTICK", "RSTICK", "L", "R", "ZL", "ZR", "PLUS", "MINUS",
                                        "LEFT", "UP", "RIGHT", "DOWN"};
static const char *const STICK_NAMES[] = {"SLEFT", "SUP", "SRIGHT", "SDOWN"};

/* ---- parsing ------------------------------------------------------------------------- */
typedef struct Parse { const char *p, *end; } Parse;   /* what is left of one step's text */

static int number(Parse *t, long *out) {
    long v = 0;
    if (t->p == t->end || !isdigit((unsigned char)*t->p)) return 0;
    while (t->p < t->end && isdigit((unsigned char)*t->p)) {
        v = v * 10 + (*t->p++ - '0');
        if (v > SEQ_MAX_N) return 0;
    }
    *out = v;
    return 1;
}

static int take(Parse *t, char c) {
    if (t->p == t->end || *t->p != c) return 0;
    t->p++;
    return 1;
}

/* "<lo>-<hi>[/<n>]" */
static const char *range(Parse *t, SeqStep *st) {
    if (!number(t, &st->lo) || !take(t, '-') || !number(t, &st->hi)) return "expected <lo>-<hi> draw calls";
    if (st->lo > st->hi) return "the range's low end is above its high end";
    st->need = SEQ_RUN;
    if (take(t, '/') && (!number(t, &st->need) || st->need < 1)) return "expected a count of 1 or more after '/'";
    return NULL;
}

/* "<name>[+<name>...]", up to '/' or the end */
static const char *keys(Parse *t, SeqStep *st) {
    for (;;) {
        const char *name = t->p;
        while (t->p < t->end && *t->p != '+' && *t->p != '/') t->p++;
        size_t n = (size_t)(t->p - name);
        int found = 0;
        for (int b = 0; b < (int)(sizeof KEY_NAMES / sizeof *KEY_NAMES) && !found; b++)
            if (n == strlen(KEY_NAMES[b]) && !memcmp(name, KEY_NAMES[b], n)) { st->buttons |= 1ull << b; found = 1; }
        for (int k = 0; k < 4 && !found; k++)
            if (n == strlen(STICK_NAMES[k]) && !memcmp(name, STICK_NAMES[k], n)) {
                int *axis = k & 1 ? &st->ly : &st->lx, v = k == 0 || k == 3 ? -32767 : 32767;
                if (*axis && *axis != v) return "the stick pushed two opposite ways";
                *axis = v;
                found = 1;
            }
        if (!found) return n ? "unknown key (the names are SSBU_INPUT's: A, ZL, SLEFT, ...)" : "expected a key";
        if (!take(t, '+')) return NULL;
    }
}

static const char *hold(Parse *t, long *out) {
    if (take(t, '/') && (!number(t, out) || *out < 1)) return "expected a hold of 1 or more ticks after '/'";
    return NULL;
}

static int starts(Parse *t, const char *with) {
    size_t n = strlen(with);
    if ((size_t)(t->end - t->p) < n || memcmp(t->p, with, n)) return 0;
    t->p += n;
    return 1;
}

static const char *step(Parse *t, SeqStep *st) {
    const char *e;
    size_t n = (size_t)(t->end - t->p);
    st->hold = SEQ_HOLD;
    if (starts(t, "@untilmark:")) {
        st->kind = SEQ_UNTILMARK;
        const char *colon = NULL;   /* the mark is up to the last ':' (a mark may hold one: module:<name>) */
        for (const char *c = t->p; c < t->end; c++) if (*c == ':') colon = c;
        if (!colon || colon == t->p) return "expected @untilmark:<mark>:<keys>";
        memcpy(st->mark, t->p, (size_t)(colon - t->p));
        t->p = colon + 1;
        if ((e = keys(t, st))) return e;
        st->wait = SEQ_PERIOD;
        if (take(t, '/')) {
            if (!number(t, &st->wait)) return "expected a period in ticks after '/'";
            if ((e = hold(t, &st->hold))) return e;
        }
        if (st->wait <= st->hold) return "the period must be longer than the hold";
    } else if (starts(t, "@until:")) {
        st->kind = SEQ_UNTIL;
        if ((e = range(t, st))) return e;
        if (!take(t, ':')) return "expected ':' and the keys after the range";
        if ((e = keys(t, st))) return e;
        st->wait = SEQ_PERIOD;
        if (take(t, '/') && !number(t, &st->wait)) return "expected a period in ticks after '/'";
        if (st->wait <= st->hold) return "the period must be longer than the hold (8)";
    } else if (starts(t, "@draws:")) {
        st->kind = SEQ_DRAWS;
        if ((e = range(t, st))) return e;
    } else if (*t->p == '@') {
        st->kind = SEQ_MARK;
        memcpy(st->mark, t->p + 1, n - 1);
        if (n == 1) return "expected a mark's name after '@'";
        if (!strcmp(st->mark, "draws") || !strcmp(st->mark, "until") || !strcmp(st->mark, "untilmark"))
            return "expected ':' after it";
        t->p = t->end;
    } else {
        st->kind = SEQ_KEYS;
        st->hold = 0;
        if (!number(t, &st->wait)) return "expected a number of ticks, or @";
        if (take(t, ':')) {
            if ((e = keys(t, st))) return e;
            st->hold = SEQ_HOLD;
            if ((e = hold(t, &st->hold))) return e;
        }
    }
    return t->p == t->end ? NULL : "unexpected text at its end";
}

static void begin(Seq *s, int i) {   /* (steps of no ticks at all are passed over) */
    while (i < s->count && s->steps[i].kind == SEQ_KEYS && s->steps[i].wait + s->steps[i].hold == 0) i++;
    s->index = i;
    s->pos = s->run = s->pressed = 0;
}

int seq_parse(Seq *s, const char *text, char *why, size_t why_size) {
    memset(s, 0, sizeof *s);
    const char *p = text ? text : "";
    for (;;) {
        while (*p && (isspace((unsigned char)*p) || *p == '#'))
            if (*p++ == '#') while (*p && *p != '\n') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && !isspace((unsigned char)*p) && *p != '#') p++;
        size_t n = (size_t)(p - start);
        if (s->count == SEQ_STEPS) { snprintf(why, why_size, "more than %d steps", SEQ_STEPS); return -1; }
        SeqStep *st = &s->steps[s->count++];
        if (n >= SEQ_TEXT) {
            snprintf(why, why_size, "step %d (%.20s...): longer than %d characters", s->count, start, SEQ_TEXT - 1);
            return -1;
        }
        memcpy(st->text, start, n);
        Parse t = {start, p};
        const char *e = step(&t, st);
        if (e) { snprintf(why, why_size, "step %d (%s): %s", s->count, st->text, e); return -1; }
    }
    begin(s, 0);
    return s->count;
}

/* ---- running ------------------------------------------------------------------------- */
static void say(Seq *s, const char *fmt, ...) {
    int n = snprintf(s->said, sizeof s->said, "tick %lld, step %d (%s): ", s->ticks, s->index + 1, s->steps[s->index].text);
    va_list ap;
    va_start(ap, fmt);
    if (n > 0 && (size_t)n < sizeof s->said) vsnprintf(s->said + n, sizeof s->said - (size_t)n, fmt, ap);
    va_end(ap);
}

int seq_tick(Seq *s, long draws, SeqSeenFn *seen, void *user) {
    s->ticks++;
    s->said[0] = 0;
    s->buttons = 0;
    s->lx = s->ly = 0;
    if (s->index >= s->count) return 0;
    SeqStep *st = &s->steps[s->index];
    int done = 0, press = 0;
    long p = ++s->pos;   /* this tick, counted from the step's first (1) */
    int in_range = draws >= st->lo && draws <= st->hi && draws >= 0;
    switch (st->kind) {
    case SEQ_KEYS:
        press = p > st->wait;
        if (p == st->wait + 1 && press) say(s, "keys for %ld ticks", st->hold);
        done = p >= st->wait + st->hold;
        break;
    case SEQ_MARK:
        if ((done = seen && seen(st->mark, user))) say(s, "%s seen", st->mark);
        break;
    case SEQ_DRAWS:
        s->run = in_range ? s->run + 1 : 0;
        if ((done = s->run >= st->need)) say(s, "%ld..%ld draw calls for %ld ticks (now %ld)", st->lo, st->hi, st->need, draws);
        break;
    case SEQ_UNTIL:
    case SEQ_UNTILMARK: {
        int met;
        if (st->kind == SEQ_UNTIL) { s->run = in_range ? s->run + 1 : 0; met = s->run >= st->need; }
        else met = seen && seen(st->mark, user);
        int pressing = s->pressed && p - s->pressed < st->hold;
        if (met && !pressing) {
            done = 1;
            if (st->kind == SEQ_UNTIL) say(s, "%ld..%ld draw calls for %ld ticks (now %ld)", st->lo, st->hi, st->need, draws);
            else say(s, "%s seen", st->mark);
            break;
        }
        if (!pressing && (!s->pressed || p - s->pressed >= st->wait) && (st->kind == SEQ_UNTILMARK || s->run == 0)) {
            s->pressed = p;
            pressing = 1;
            if (st->kind == SEQ_UNTIL) say(s, "keys for %ld ticks (%ld draw calls), again every %ld until %ld..%ld", st->hold, draws, st->wait, st->lo, st->hi);
            else say(s, "keys for %ld ticks, again every %ld until %s", st->hold, st->wait, st->mark);
        }
        press = pressing;
        break;
    }
    }
    if (press) { s->buttons = st->buttons; s->lx = st->lx; s->ly = st->ly; }
    if (done) begin(s, s->index + 1);
    return s->said[0] != 0;
}

int seq_finished(const Seq *s) { return s->index >= s->count; }

long seq_poll(Seq *s, SeqStepsFn *steps, long draws, SeqSeenFn *seen, SeqLogFn *log, void *user) {
    long ticks = steps ? steps() : 1;
    if (ticks < 0) ticks = 0;
    if (ticks > SEQ_MAX_TICKS) ticks = SEQ_MAX_TICKS;
    for (long i = 0; i < ticks; i++)
        if (seq_tick(s, draws, seen, user) && log) log(s->said, user);
    return ticks;
}
