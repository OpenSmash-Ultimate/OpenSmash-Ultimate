/* Unit test of the rollback netcode's game-free parts (rb.h): a scripted peer sends its pads over a lossy, late,
 * reordering link with a fixed seed (each message repeats every pad not yet confirmed); this side predicts them, finds
 * its mispredictions and "rolls back" (recomputes a toy simulation from the mismatch frame). The toy state must end
 * equal to a run with every pad known at once. Two links: the first as it always was (the peer knows our acknowledgement
 * at once), the second harsher (more lost, later, copies sent twice or cut short, our acknowledgement over a lossy link
 * back). Also: the wire format byte for byte, its round trips, malformed and mutated messages, the input queues (delay,
 * guesses, the earliest wrong one, the ring's age limit, the finalized frame), the stick clamp, time sync numbers and
 * the handshake record's size and layout (rb_hs.h).
 *   clang -O1 -I<runtime> tests/rb_test.c <runtime>/rb_input.c <runtime>/rb_wire.c <runtime>/rb_timesync.c -o rb_test.exe
 *   (<runtime> = the folder with rb.h: runtime in the public tree; exit 0 = all passed. The repository root's
 *   tests/test_rollback_netcode.py builds and runs it on Windows, Linux and macOS.)
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rb.h"
#include "rb_hs.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

/* the "game": a state that depends on every pad of every frame, in order */
static uint64_t step(uint64_t s, const RbPad *a, const RbPad *b) {
    s = s * 6364136223846793005ull + a->buttons * 31 + (uint64_t)(a->lx + 40000) * 7 + b->buttons * 131 + (uint64_t)(b->ly + 40000) * 3;
    return s ^ (s >> 29);
}
static RbPad human(int player, int frame) {      /* a pad that changes now and then, like a real player */
    RbPad p = {0, 0, 0, 0, 0};
    int phase = (frame / (7 + player * 5)) % 4;
    p.buttons = phase == 1 ? 1 : phase == 3 ? 2 | 8 : 0;
    p.lx = (int16_t)(phase == 2 ? 30000 : phase == 3 ? -20000 : 900);      /* 900: noise at rest, clamped to 0 */
    return p;
}
static RbPad padb(uint64_t buttons) { RbPad p = {buttons, 0, 0, 0, 0}; return p; }

/* a link: packets with a delivery time; some lost, some late (so also reordered), some sent twice, some cut short */
typedef struct { int loss, late_min, late_spread, dup, cut; } Profile;     /* percentages; frames */
typedef struct { uint8_t b[2048]; size_t n; int at; } Pkt;
#define LQ 4096
typedef struct { Pkt q[LQ]; int n; const Profile *pr; } Link;
static void link_put(Link *l, const uint8_t *b, size_t n, int now) {
    if (l->n >= LQ) return;
    Pkt *p = &l->q[l->n++];
    memcpy(p->b, b, n); p->n = n; p->at = now + l->pr->late_min + (int)(rnd() % (uint32_t)l->pr->late_spread);
}
static void link_send(Link *l, const uint8_t *b, size_t n, int now) {
    if (rnd() % 100 < (uint32_t)l->pr->loss) return;
    link_put(l, b, n, now);
    if (l->pr->dup && rnd() % 100 < (uint32_t)l->pr->dup) link_put(l, b, n, now);                       /* a second copy, at its own time */
    if (l->pr->cut && n > 1 && rnd() % 100 < (uint32_t)l->pr->cut) link_put(l, b, 1 + rnd() % (n - 1), now);   /* a copy cut short */
}
static int link_recv(Link *l, int now, uint8_t *b, size_t *n) {
    for (int i = 0; i < l->n; i++) if (l->q[i].at <= now) { memcpy(b, l->q[i].b, l->q[i].n); *n = l->q[i].n; l->q[i] = l->q[--l->n]; return 1; }
    return 0;
}

static void wire_tests(void) {
    RbMsg m, r;
    uint8_t buf[2048];
    memset(&m, 0, sizeof m);
    m.type = RB_MSG_PADS; m.player = 1; m.count = 3; m.newest = 100; m.ack = 97; m.check_frame = 90; m.check = 0xdeadbeef;
    for (int i = 0; i < 3; i++) { m.pads[i].buttons = 0x123456789abcull + (uint64_t)i; m.pads[i].lx = (int16_t)(-32767 + i); m.pads[i].ry = (int16_t)(32767 - i); }
    size_t n = rb_msg_build(buf, sizeof buf, &m);
    CHECK(n == RB_PADS_HEAD + 3 * RB_PAD_WIRE, "pads size %zu", n);
    CHECK(rb_msg_parse(buf, n, &r) == 1, "pads parse");
    CHECK(r.player == 1 && r.count == 3 && r.newest == 100 && r.ack == 97 && r.check_frame == 90 && r.check == 0xdeadbeef, "pads header");
    for (int i = 0; i < 3; i++) CHECK(rb_pad_equal(&m.pads[i], &r.pads[i]), "pad %d round trip", i);
    CHECK(rb_msg_parse(buf, n - 1, &r) == 0, "short pads refused");
    CHECK(rb_msg_parse(buf, n + 1, &r) == 0, "long pads refused");
    for (size_t k = 0; k < n; k++) CHECK(rb_msg_parse(buf, k, &r) == 0, "pads cut to %zu bytes refused", k);
    buf[1] = 200; CHECK(rb_msg_parse(buf, n, &r) == 0, "bad player refused");
    memset(&m, 0, sizeof m); m.type = RB_MSG_PONG; m.frame = 7; m.time_us = 111; m.echo_us = 222;
    n = rb_msg_build(buf, sizeof buf, &m);
    CHECK(rb_msg_parse(buf, n, &r) == 1 && r.frame == 7 && r.time_us == 111 && r.echo_us == 222, "pong round trip");
    memset(&m, 0, sizeof m); m.type = RB_MSG_ACK; m.frame = -3;
    n = rb_msg_build(buf, sizeof buf, &m);
    CHECK(n == 5 && rb_msg_parse(buf, n, &r) == 1 && r.type == RB_MSG_ACK && r.frame == -3, "ack round trip");
    CHECK(rb_msg_parse((const uint8_t *)"\x09", 1, &r) == 0, "unknown type refused");
    CHECK(rb_msg_parse(buf, 0, &r) == 0, "empty message refused");
    RbPad p = {5, 1600, -1600, 1601, -1601};
    p = rb_pad_rest(p);
    CHECK(p.lx == 0 && p.ly == 0 && p.rx == 1601 && p.ry == -1601, "stick clamp");

    /* byte for byte, as rb_wire.c's comment says: games built on different systems must read each other */
    memset(&m, 0, sizeof m);
    m.type = RB_MSG_PADS; m.player = 1; m.count = 1; m.newest = 5; m.ack = 4; m.check_frame = 3; m.check = 0x11223344u;
    m.pads[0].buttons = 0x0102030405060708ull; m.pads[0].lx = -2; m.pads[0].ly = 0x1234; m.pads[0].ry = -32768;
    static const uint8_t pads_bytes[] = { 1, 1, 1, 0,  5, 0, 0, 0,  4, 0, 0, 0,  3, 0, 0, 0,  0x44, 0x33, 0x22, 0x11,
                                          8, 7, 6, 5, 4, 3, 2, 1,  0xfe, 0xff,  0x34, 0x12,  0, 0,  0, 0x80 };
    n = rb_msg_build(buf, sizeof buf, &m);
    CHECK(n == sizeof pads_bytes && !memcmp(buf, pads_bytes, n), "pads bytes");
    memset(&m, 0, sizeof m); m.type = RB_MSG_PING; m.frame = 0x01020304; m.time_us = 0x1122334455667788ull;
    static const uint8_t ping_bytes[] = { 3,  4, 3, 2, 1,  0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11 };
    n = rb_msg_build(buf, sizeof buf, &m);
    CHECK(n == sizeof ping_bytes && !memcmp(buf, ping_bytes, n), "ping bytes");
    memset(&m, 0, sizeof m); m.type = RB_MSG_PONG; m.frame = 1; m.time_us = 2; m.echo_us = 0x8000000000000001ull;
    static const uint8_t pong_bytes[] = { 4,  1, 0, 0, 0,  2, 0, 0, 0, 0, 0, 0, 0,  1, 0, 0, 0, 0, 0, 0, 0x80 };
    n = rb_msg_build(buf, sizeof buf, &m);
    CHECK(n == sizeof pong_bytes && !memcmp(buf, pong_bytes, n), "pong bytes");

    /* what the builder refuses */
    memset(&m, 0, sizeof m); m.type = RB_MSG_PADS; m.newest = 100; m.count = RB_MAX_PADS_PER_MSG + 1;
    CHECK(rb_msg_build(buf, sizeof buf, &m) == 0, "more than %d pads not built", RB_MAX_PADS_PER_MSG);
    m.count = -1; CHECK(rb_msg_build(buf, sizeof buf, &m) == 0, "a negative count not built");
    m.count = 2; CHECK(rb_msg_build(buf, RB_PADS_HEAD + 2 * RB_PAD_WIRE - 1, &m) == 0, "pads: a buffer one byte short refused");
    m.type = RB_MSG_ACK; CHECK(rb_msg_build(buf, 4, &m) == 0, "ack: a buffer too short refused");
    m.type = RB_MSG_PING; CHECK(rb_msg_build(buf, 12, &m) == 0, "ping: a buffer too short refused");
    m.type = RB_MSG_PONG; CHECK(rb_msg_build(buf, 20, &m) == 0, "pong: a buffer too short refused");
    m.type = 9; CHECK(rb_msg_build(buf, sizeof buf, &m) == 0, "unknown type not built");

    /* what the parser refuses although the length fits: frames count from 1, at most 64 pads */
    memset(&m, 0, sizeof m); m.type = RB_MSG_PADS; m.player = 1; m.count = 3; m.newest = 3;
    n = rb_msg_build(buf, sizeof buf, &m);
    CHECK(rb_msg_parse(buf, n, &r) == 1, "pads of frames 1..3 taken");
    buf[4] = 2; CHECK(rb_msg_parse(buf, n, &r) == 0, "pads of frames 0..2 refused");
    buf[4] = 0xff; buf[5] = 0xff; buf[6] = 0xff; buf[7] = 0xff; CHECK(rb_msg_parse(buf, n, &r) == 0, "a negative newest frame refused");
    m.count = 0; m.newest = 0; n = rb_msg_build(buf, sizeof buf, &m);
    CHECK(n == RB_PADS_HEAD && rb_msg_parse(buf, n, &r) == 1 && r.count == 0, "no pads (an acknowledgement only) taken");
    {   uint8_t big[RB_PADS_HEAD + (RB_MAX_PADS_PER_MSG + 1) * RB_PAD_WIRE];
        memset(big, 0, sizeof big); big[0] = RB_MSG_PADS; big[2] = RB_MAX_PADS_PER_MSG + 1; big[4] = 200;
        CHECK(rb_msg_parse(big, sizeof big, &r) == 0, "65 pads refused"); }
}

/* mutated messages: the parser never reads past the end (the exact-size copy is for the sanitizer builds) and a
 * message it takes is exactly the bytes rb_msg_build makes of it again (one meaning per message) */
static uint32_t g_fz = 0x2545f491u;
static uint32_t fz(void) { g_fz ^= g_fz << 13; g_fz ^= g_fz >> 17; g_fz ^= g_fz << 5; return g_fz; }
static uint64_t fz64(void) { uint64_t hi = fz(); return hi << 32 | fz(); }
static void fuzz_tests(void) {
    int taken = 0, refused = 0;
    for (int it = 0; it < 20000; it++) {
        RbMsg m; memset(&m, 0, sizeof m);
        m.type = 1 + (int)(fz() % 4); m.frame = (int32_t)fz(); m.time_us = fz64(); m.echo_us = fz64();
        m.player = (int)(fz() % RB_MAX_PLAYERS); m.count = (int)(fz() % (RB_MAX_PADS_PER_MSG + 1)); m.newest = m.count + (int32_t)(fz() % 1000);
        m.ack = (int32_t)fz(); m.check_frame = (int32_t)fz(); m.check = fz();
        for (int i = 0; i < m.count; i++) {
            m.pads[i].buttons = fz64();
            m.pads[i].lx = (int16_t)fz(); m.pads[i].ly = (int16_t)fz(); m.pads[i].rx = (int16_t)fz(); m.pads[i].ry = (int16_t)fz();
        }
        uint8_t buf[RB_PADS_HEAD + RB_MAX_PADS_PER_MSG * RB_PAD_WIRE + 4], again[sizeof buf];
        size_t n = rb_msg_build(buf, sizeof buf - 4, &m);
        CHECK(n > 0, "a valid message of type %d not built", m.type);
        switch (fz() % 4) {                                       /* one change, or none */
        case 0: break;
        case 1: if (n) buf[fz() % n] ^= (uint8_t)(1 + fz() % 255); break;      /* a byte changed */
        case 2: n = fz() % (n + 1); break;                                      /* cut short */
        case 3: { size_t k = 1 + fz() % 4; for (size_t i = 0; i < k; i++) buf[n + i] = (uint8_t)fz(); n += k; break; }   /* bytes after it */
        }
        uint8_t *exact = malloc(n ? n : 1);
        if (!exact) { CHECK(0, "out of memory"); return; }
        memcpy(exact, buf, n);
        RbMsg r;
        if (rb_msg_parse(exact, n, &r)) {
            taken++;
            size_t k = rb_msg_build(again, sizeof again, &r);
            CHECK(k == n && !memcmp(again, exact, n), "message %d taken but built again differently (%zu bytes, then %zu)", it, n, k);
        } else refused++;
        free(exact);
    }
    CHECK(taken > 1000 && refused > 1000, "mutations: %d taken, %d refused", taken, refused);
}

static void input_tests(void) {
    RbPad got, out;
    int pr;
    /* the local delay: the pad read at frame f applies at f + delay; nothing is pressed in the first frames */
    rb_input_reset(2, 0, 3);
    CHECK(rb_input_delay() == 3, "delay %d", rb_input_delay());
    got = rb_input_local(1, padb(1));
    CHECK(got.buttons == 0 && rb_input_local_latest() == 4, "frame 1: %llu, newest local %d", (unsigned long long)got.buttons, rb_input_local_latest());
    rb_input_local(2, padb(2)); rb_input_local(3, padb(3));
    got = rb_input_local(4, padb(4));
    CHECK(got.buttons == 1, "frame 4 plays frame 1's pad: %llu", (unsigned long long)got.buttons);
    CHECK(rb_input_local_get(6, &out) && out.buttons == 3, "frame 6's pad, to send again");
    CHECK(!rb_input_local_get(8, &out) && !rb_input_local_get(0, &out), "no local pad after the newest or before frame 1");
    got = rb_input_get(0, 5, &pr);
    CHECK(got.buttons == 2 && !pr, "the local player's own pad is never a guess");
    rb_input_reset(2, 0, 99); CHECK(rb_input_delay() == 15, "delay 99 -> %d", rb_input_delay());
    rb_input_reset(2, 0, -1); CHECK(rb_input_delay() == 0, "delay -1 -> %d", rb_input_delay());

    /* remote pads: in any order; a gap holds the newest consecutive frame; a second copy changes nothing */
    rb_input_reset(2, 0, 0);
    rb_input_remote_put(1, 1, padb(1)); rb_input_remote_put(1, 2, padb(2)); rb_input_remote_put(1, 4, padb(4));
    CHECK(rb_input_remote_latest(1) == 2, "gap at 3: newest %d", rb_input_remote_latest(1));
    rb_input_remote_put(1, 2, padb(9));
    got = rb_input_get(1, 2, &pr);
    CHECK(got.buttons == 2 && !pr, "a second copy of frame 2 changed it: %llu", (unsigned long long)got.buttons);
    rb_input_remote_put(1, 3, padb(3));
    CHECK(rb_input_remote_latest(1) == 4, "gap filled: newest %d", rb_input_remote_latest(1));

    /* frames 5..8 run on a guess (frame 4's pad repeated); a right guess is no rollback, the earliest wrong one is */
    for (int f = 5; f <= 8; f++) { got = rb_input_get(1, f, &pr); CHECK(pr && got.buttons == 4, "guess of frame %d: %d %llu", f, pr, (unsigned long long)got.buttons); }
    rb_input_remote_put(1, 5, padb(4));
    CHECK(rb_input_take_mismatch() == 0, "a right guess is no rollback");
    rb_input_remote_put(1, 8, padb(7)); rb_input_remote_put(1, 6, padb(6)); rb_input_remote_put(1, 7, padb(5));
    int mis = rb_input_take_mismatch();
    CHECK(mis == 6, "wrong guesses at 8, 6 and 7: rollback to 6, not %d", mis);
    CHECK(rb_input_take_mismatch() == 0, "the mismatch is taken once");
    got = rb_input_get(1, 9, &pr);                                /* a guess: frame 8's pad, sticks at rest */
    RbPad noisy = padb(7); noisy.lx = 1200; noisy.ry = -1600;
    rb_input_remote_put(1, 9, noisy);
    mis = rb_input_take_mismatch(); got = rb_input_get(1, 9, &pr);
    CHECK(mis == 0 && got.lx == 0 && got.ry == 0 && !pr, "stick noise at rest: rollback to %d", mis);
    rb_input_remote_put(1, 10, padb(1));
    CHECK(rb_input_take_mismatch() == 0, "no rollback for a frame that never ran on a guess");

    /* pads not taken: the local player's, a player not in the match, frame 0 and before */
    rb_input_remote_put(0, 11, padb(1)); rb_input_remote_put(2, 11, padb(1)); rb_input_remote_put(-1, 11, padb(1));
    rb_input_remote_put(1, 0, padb(1)); rb_input_remote_put(1, -5, padb(1));
    CHECK(rb_input_remote_latest(1) == 10 && rb_input_remote_latest(2) == 0, "pads not for this side ignored: newest %d, %d", rb_input_remote_latest(1), rb_input_remote_latest(2));
    got = rb_input_get(5, 3, &pr);
    CHECK(got.buttons == 0 && !pr, "a player not in the match has no pad");

    /* the finalized frame: the newest one every player's real pad is known for */
    for (int f = 1; f <= 12; f++) rb_input_local(f, padb(0));
    CHECK(rb_input_finalized() == 10, "finalized %d (local 12, remote 10)", rb_input_finalized());
    rb_input_remote_put(1, 11, padb(0)); rb_input_remote_put(1, 12, padb(0)); rb_input_remote_put(1, 13, padb(0));
    CHECK(rb_input_finalized() == 12, "finalized %d (local 12, remote 13)", rb_input_finalized());

    /* a frame older than the ring is not taken: it would land in the slot of the frame RB_RING later */
    rb_input_reset(2, 0, 0);
    for (int f = 1; f <= 300; f++) rb_input_remote_put(1, f, padb((uint64_t)f));
    rb_input_remote_put(1, 300 - RB_RING, padb(999));
    got = rb_input_get(1, 300, &pr);
    CHECK(got.buttons == 300 && !pr, "frame %d overwrote frame 300: %llu", 300 - RB_RING, (unsigned long long)got.buttons);
}

static void ts_tests(void) {
    rb_ts_reset(16683);
    for (int i = 0; i < 30; i++) rb_ts_sample(i < 5 ? 900000 : 12000);      /* 5 outliers must not count */
    RbSyncAction a = rb_ts_decide(60);
    CHECK(rb_ts_offset() == 12000 && a.speed < 1.0 && !a.skip, "ahead: offset %d speed %.3f", rb_ts_offset(), a.speed);
    rb_ts_reset(16683);
    for (int i = 0; i < 30; i++) rb_ts_sample(-40000);
    a = rb_ts_decide(300);
    CHECK(a.speed > 1.0 && a.advance == 2, "behind: speed %.3f advance %d", a.speed, a.advance);
    a = rb_ts_decide(301);
    CHECK(a.speed == 1.0 && !a.advance, "decides only every 30 frames");
    /* a whole frame ahead: one frame skipped, in the first 120 frames only */
    rb_ts_reset(16683);
    for (int i = 0; i < 30; i++) rb_ts_sample(20000);
    a = rb_ts_decide(90);
    CHECK(a.skip == 1 && a.speed < 1.0, "a frame ahead at 90: skip %d speed %.3f", a.skip, a.speed);
    a = rb_ts_decide(150);
    CHECK(!a.skip && a.speed < 1.0, "a frame ahead at 150: skip %d speed %.3f", a.skip, a.speed);
    /* far behind: at most 3 extra frames */
    rb_ts_reset(16683);
    for (int i = 0; i < 30; i++) rb_ts_sample(-200000);
    a = rb_ts_decide(30);
    CHECK(a.advance == 3 && a.speed > 1.0, "far behind: advance %d", a.advance);
    /* the 8 ms band: 7999 us either way is left alone, 8000 is not */
    rb_ts_reset(16683);
    for (int i = 0; i < 30; i++) rb_ts_sample(7999);
    a = rb_ts_decide(30);
    CHECK(a.speed == 1.0 && !a.skip && !a.advance, "7999 us ahead: speed %.3f", a.speed);
    rb_ts_reset(16683);
    for (int i = 0; i < 30; i++) rb_ts_sample(-7999);
    a = rb_ts_decide(30);
    CHECK(a.speed == 1.0 && !a.skip && !a.advance, "7999 us behind: speed %.3f", a.speed);
    rb_ts_reset(16683);
    for (int i = 0; i < 30; i++) rb_ts_sample(8000);
    a = rb_ts_decide(30);
    CHECK(a.speed < 1.0 && !a.skip, "8000 us ahead: speed %.3f", a.speed);
    /* fewer than 3 samples: nothing; then only the newest 30 count */
    rb_ts_reset(16683);
    rb_ts_sample(-50000); rb_ts_sample(-50000);
    a = rb_ts_decide(30);
    CHECK(a.speed == 1.0 && !a.advance && rb_ts_offset() == 0, "2 samples: speed %.3f offset %d", a.speed, rb_ts_offset());
    for (int i = 0; i < 30; i++) rb_ts_sample(12000);
    a = rb_ts_decide(60);
    CHECK(rb_ts_offset() == 12000, "only the newest 30 samples count: offset %d", rb_ts_offset());
}

/* the handshake's match record goes on the wire as it is (rb_net.c RbhMsg, packed): the same size and layout with every
 * system and compiler. A change here is a protocol change (RBH_PROTO in rb_net.c). */
static void hs_tests(void) {
    CHECK(sizeof(RbhMatch) == 388, "RbhMatch is %zu bytes", sizeof(RbhMatch));
    CHECK(offsetof(RbhMatch, colour) == 16 && offsetof(RbhMatch, layout) == 20 && offsetof(RbhMatch, stage) == 64 && offsetof(RbhMatch, hazards) == 96,
          "RbhMatch: colour at %zu, layout %zu, stage %zu, hazards %zu", offsetof(RbhMatch, colour), offsetof(RbhMatch, layout), offsetof(RbhMatch, stage), offsetof(RbhMatch, hazards));
    CHECK(offsetof(RbhMatch, items) == 100 && offsetof(RbhMatch, seed) == 104 && offsetof(RbhMatch, ctx_seed) == 112 && offsetof(RbhMatch, ctx_hist) == 116 && offsetof(RbhMatch, ctx_stage) == 212,
          "RbhMatch: items at %zu, seed %zu, ctx_seed %zu, ctx_hist %zu, ctx_stage %zu", offsetof(RbhMatch, items), offsetof(RbhMatch, seed), offsetof(RbhMatch, ctx_seed), offsetof(RbhMatch, ctx_hist), offsetof(RbhMatch, ctx_stage));
}

#define SIDE_FRAMES 3000
typedef struct {
    uint64_t state[SIDE_FRAMES + 2];               /* toy state after each frame (index = frame) */
    int rollbacks, rolled_frames, max_back;
} Side;

static void sim_from(Side *s, int from, int to) {  /* recompute frames from..to with the pads known or guessed now */
    for (int f = from; f <= to; f++) {
        int pr;
        RbPad a = rb_input_get(0, f, &pr), b = rb_input_get(1, f, &pr);
        s->state[f] = step(s->state[f - 1], &a, &b);
    }
}
/* the remote's true pad of frame g (its input delay is 2 frames, like ours) */
static RbPad remote_pad(int g) { static const RbPad zero; return g <= 2 ? zero : rb_pad_rest(human(1, g - 2)); }

/* the remote: sends its pads from (the newest of its pads it knows we have + 1) to its newest, newest first, at most 64 */
static void remote_send(Link *l, int peer_ack, int f) {
    RbMsg m; memset(&m, 0, sizeof m);
    m.type = RB_MSG_PADS; m.player = 1;
    int newest = (f < SIDE_FRAMES ? f : SIDE_FRAMES) + 2, oldest = peer_ack + 1;
    if (newest - oldest + 1 > RB_MAX_PADS_PER_MSG) oldest = newest - RB_MAX_PADS_PER_MSG + 1;
    m.newest = newest; m.count = newest - oldest + 1;
    if (m.count < 0) m.count = 0;
    for (int i = 0; i < m.count; i++) m.pads[i] = remote_pad(newest - i);
    uint8_t b[2048]; size_t n = rb_msg_build(b, sizeof b, &m);
    CHECK(n > 0, "remote message not built");
    if (n) link_send(l, b, n, f);
}
/* we take what has arrived (a message cut short is refused and dropped) */
static int g_refused;
static void local_recv(Link *l, int f) {
    uint8_t b[2048]; size_t n;
    while (link_recv(l, f, b, &n)) {
        RbMsg m;
        if (!rb_msg_parse(b, n, &m) || m.type != RB_MSG_PADS) { g_refused++; continue; }
        for (int i = 0; i < m.count; i++) rb_input_remote_put(m.player, m.newest - i, m.pads[i]);
    }
}
/* we send our newest pads with our acknowledgement of theirs (as rb_net.c send_pads); the remote reads only the ack */
static void local_send(Link *l, int f) {
    RbMsg m; memset(&m, 0, sizeof m);
    m.type = RB_MSG_PADS; m.player = 0; m.newest = rb_input_local_latest(); m.ack = rb_input_remote_latest(1);
    for (int g = m.newest; g >= 1 && m.count < 8; g--) if (!rb_input_local_get(g, &m.pads[m.count++])) { m.count--; break; }
    uint8_t b[2048]; size_t n = rb_msg_build(b, sizeof b, &m);
    CHECK(n > 0, "local message not built");
    if (n) link_send(l, b, n, f);
}
static int remote_recv_ack(Link *l, int f, int ack) {
    uint8_t b[2048]; size_t n;
    while (link_recv(l, f, b, &n)) { RbMsg m; if (rb_msg_parse(b, n, &m) && m.type == RB_MSG_PADS && m.ack > ack) ack = m.ack; }
    return ack;
}

/* one match: player 1's pads come over `to_me`; our acknowledgement goes back over `back` (NULL: the remote knows it at once) */
static void link_run(const char *name, const Profile *to_me_pr, const Profile *back_pr) {
    /* a single process can hold one rb_input instance: run side 0 against a scripted remote that delivers player 1's
     * true pads late and lossy (with redundancy), then check the state equals the perfect-input state. */
    rb_input_reset(2, 0, 2);
    static Side s;
    memset(&s, 0, sizeof s);
    static Link to_me, back;
    memset(&to_me, 0, sizeof to_me); to_me.pr = to_me_pr;
    memset(&back, 0, sizeof back); back.pr = back_pr;
    g_refused = 0;
    int peer_ack_of_theirs = 0;                    /* newest of THEIR pads the remote knows we have (it resends everything after it) */
    for (int f = 1; f <= SIDE_FRAMES; f++) {
        remote_send(&to_me, peer_ack_of_theirs, f);
        local_recv(&to_me, f);
        rb_input_local(f, human(0, f));            /* our own pad for this frame */
        if (back_pr) { local_send(&back, f); peer_ack_of_theirs = remote_recv_ack(&back, f, peer_ack_of_theirs); }
        else peer_ack_of_theirs = rb_input_remote_latest(1);
        /* a misprediction since the last frame: roll back to it and simulate again up to the frame before */
        int mis = rb_input_take_mismatch();
        if (mis && mis < f) {
            CHECK(f - mis <= 64, "%s: rollback too deep: %d", name, f - mis);
            if (f - mis > s.max_back) s.max_back = f - mis;
            s.rollbacks++; s.rolled_frames += f - mis;
            sim_from(&s, mis, f - 1);
        }
        sim_from(&s, f, f);
    }
    /* let the late packets arrive (the remote sends its last pads again until they are confirmed), then roll back over the end */
    for (int f = SIDE_FRAMES + 1; f <= SIDE_FRAMES + 60; f++) {
        if (peer_ack_of_theirs < SIDE_FRAMES + 2) remote_send(&to_me, peer_ack_of_theirs, f);
        local_recv(&to_me, f);
        if (back_pr) { local_send(&back, f); peer_ack_of_theirs = remote_recv_ack(&back, f, peer_ack_of_theirs); }
        else peer_ack_of_theirs = rb_input_remote_latest(1);
    }
    int mis = rb_input_take_mismatch();
    if (mis) sim_from(&s, mis, SIDE_FRAMES);
    /* the reference: every pad known at once (the remote's pad of frame f is human(1, f - 2), ours human(0, f - 2)) */
    uint64_t ref = 0;
    for (int f = 1; f <= SIDE_FRAMES; f++) {
        RbPad a = f <= 2 ? (RbPad){0} : rb_pad_rest(human(0, f - 2)), bb = remote_pad(f);
        ref = step(ref, &a, &bb);
    }
    CHECK(rb_input_remote_latest(1) >= SIDE_FRAMES, "%s: remote pads all received: %d", name, rb_input_remote_latest(1));
    CHECK(rb_input_finalized() >= SIDE_FRAMES, "%s: finalized %d", name, rb_input_finalized());
    CHECK(s.state[SIDE_FRAMES] == ref, "%s: final state %016llx != perfect-input state %016llx", name, (unsigned long long)s.state[SIDE_FRAMES], (unsigned long long)ref);
    if (to_me_pr->cut) CHECK(g_refused > 0, "%s: no message cut short arrived", name);
    printf("rb_test: %s: %d rollbacks, %d frames simulated again, deepest %d frames, %d messages refused\n", name, s.rollbacks, s.rolled_frames, s.max_back, g_refused);
}

int main(void) {
    wire_tests();
    fuzz_tests();
    input_tests();
    ts_tests();
    hs_tests();
    static const Profile late = {15, 2, 6, 0, 0};          /* 15 % lost, 2..7 frames late */
    static const Profile harsh = {35, 1, 15, 20, 10};      /* 35 % lost, 1..15 frames late, 20 % sent twice, 10 % also cut short */
    link_run("15% lost, 2..7 frames late", &late, NULL);
    link_run("35% lost both ways, 1..15 frames late, copies twice and cut short", &harsh, &harsh);
    printf("rb_test: %s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
