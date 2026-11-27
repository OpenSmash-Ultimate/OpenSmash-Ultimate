/* The ping overlay (SSBU_NET_PING, display.ini net_ping): in an online rollback fight, each other player's round trip
 * and the input delay the match plays with, drawn over the picture as SSBU Arena draws them (gpu_overlay.c draws it,
 * hle_rb_core.c measures). Plain C with no Windows calls: tests/test_net_ping.py checks this file alone.
 *
 * THE PING SHOWN. The rollback link measures a round trip on every PADS message a player sends (its ack of our pads,
 * against when they first went out), 60 a second, and on every HELLO before the fight. A trip measured by ack is late by
 * up to a frame of the other game's polling, so each NET_PING_WINDOW_MS window keeps its least (the handshake reads the
 * least too); the number shown is the median of the last NET_PING_SAMPLES windows (SSBU Arena's SSBU_PING_SMOOTH: the
 * median of the last 5 round trips), so one late packet is no spike. Nothing heard for NET_PING_STALE_MS: "--".
 *
 * WHAT IS DRAWN. Per player: a signal icon of four bars and the number, both in the colour of the quality (green up to
 * 60 ms, yellow up to 120, red above or nothing heard), bars lit 4 up to 40 ms, 3 up to 80, 2 up to 130, else 1; then the
 * input delay as "<n>f" and how it was chosen ("auto": from the round trip at the handshake; "fixed": set by hand), in
 * the light text. A 2-player fight: left of the other player's stock icons (SSBU Arena's place, measured on the game's
 * 2-player HUD at 1280x720: the HUD's 16:9 part, its right end at 24.3 % / 62.8 % of the width, its bottom at 97.8 % of
 * the height). 3 or 4 players: one row at the top's middle, "P2 <bars> 48   P3 <bars> 60   2f auto" (the frame-rate
 * overlay has the top left, the timer the top right). Only online rollback fights (Windows: rollback runs there alone). */
#ifndef NET_PING_H
#define NET_PING_H
#include <stdio.h>
#include <string.h>

#define NET_PING_PLAYERS 8
#define NET_PING_SAMPLES 5
#define NET_PING_WINDOW_MS 250.0
#define NET_PING_STALE_MS 3000.0

/* One player's round trips. net_ping_sample: the measuring thread only; net_ping_shown: any thread (a torn read is an
 * older window's value, never garbage: each field is one aligned word). */
typedef struct NetPingHist {
    volatile int ring[NET_PING_SAMPLES];   /* each window's least round trip, ms */
    volatile int n;                        /* windows closed so far */
    volatile double last_ms;               /* when the newest round trip came (0: none) */
    double win_t0, win_min;                /* the open window (win_t0 0: none) */
} NetPingHist;

static inline void net_ping_reset(NetPingHist *h) { memset((void *)h, 0, sizeof *h); }

static inline void net_ping_push(NetPingHist *h, double ms) {
    int v = (int)(ms + 0.5), k = h->n;
    h->ring[k % NET_PING_SAMPLES] = v < 0 ? 0 : v;
    h->n = k + 1;
}

/* A round trip of rtt_ms measured at now_ms (a clock in ms). The first one is shown at once; after it, each window's least. */
static inline void net_ping_sample(NetPingHist *h, double now_ms, double rtt_ms) {
    if (!(rtt_ms > 0) || rtt_ms > 5000) return;
    h->last_ms = now_ms > 0 ? now_ms : 1e-3;
    if (h->n == 0 && h->win_t0 <= 0) { net_ping_push(h, rtt_ms); h->win_t0 = now_ms > 0 ? now_ms : 1e-3; h->win_min = 1e9; return; }
    if (h->win_t0 <= 0) { h->win_t0 = now_ms > 0 ? now_ms : 1e-3; h->win_min = rtt_ms; return; }
    if (rtt_ms < h->win_min) h->win_min = rtt_ms;
    if (now_ms - h->win_t0 >= NET_PING_WINDOW_MS) {
        if (h->win_min < 1e9) net_ping_push(h, h->win_min);
        h->win_t0 = now_ms > 0 ? now_ms : 1e-3; h->win_min = 1e9;
    }
}

/* The number shown: the median of the last NET_PING_SAMPLES windows, ms; -1: none yet, or nothing for NET_PING_STALE_MS. */
static inline int net_ping_shown(const NetPingHist *h, double now_ms) {
    int n = h->n;
    double last = h->last_ms;
    if (n <= 0 || last <= 0 || now_ms - last > NET_PING_STALE_MS) return -1;
    int k = n < NET_PING_SAMPLES ? n : NET_PING_SAMPLES, v[NET_PING_SAMPLES];
    for (int i = 0; i < k; i++) v[i] = h->ring[(n - 1 - i) % NET_PING_SAMPLES];
    for (int i = 1; i < k; i++) for (int j = i; j > 0 && v[j] < v[j - 1]; j--) { int t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; }
    return v[k / 2];
}

/* How the match's input delay was chosen: the host's word (HELLO byte 7); 0 from a game older than the overlay. */
enum { NET_DELAY_UNKNOWN = 0, NET_DELAY_AUTO = 1, NET_DELAY_FIXED = 2 };

/* What the overlay shows now (hle_rb_core.c rb_net_ping_view): players 0 when no online rollback fight is on. */
typedef struct NetPingView {
    int players, local;              /* the match's players; this game's (0-based) */
    int ms[NET_PING_PLAYERS];        /* each other player's ping (net_ping_shown), -1 not known; -2 this game's own slot or none */
    int delay, delay_how;            /* the input delay in frames (-1 not chosen yet), NET_DELAY_* */
} NetPingView;

/* 0 green, 1 yellow, 2 red (also: nothing heard) */
static inline int net_ping_level(int ms) { return ms < 0 ? 2 : ms <= 60 ? 0 : ms <= 120 ? 1 : 2; }
static inline int net_ping_bars(int ms) { return ms < 0 ? 0 : ms <= 40 ? 4 : ms <= 80 ? 3 : ms <= 130 ? 2 : 1; }
static inline void net_ping_text(char *out, size_t cap, int ms) {
    if (ms < 0) snprintf(out, cap, "--");
    else snprintf(out, cap, "%d", ms > 999 ? 999 : ms);
}
/* "2f auto", "3f fixed", "2f" (a host older than the overlay); "" when no delay is chosen yet */
static inline void net_ping_delay_text(char *out, size_t cap, int delay, int how) {
    if (delay < 0) { if (cap) out[0] = 0; return; }
    snprintf(out, cap, "%df%s", delay > 99 ? 99 : delay, how == NET_DELAY_AUTO ? " auto" : how == NET_DELAY_FIXED ? " fixed" : "");
}

/* The sizes, from the picture's height h (SSBU Arena's: a dot is h / 540, 2 px at 1080; its letters 7 dots high). The
 * overlay's font (gpu_overlay.c): a letter of size h * 26 / 1080 has capitals about 14 px high at 1080, as SSBU Arena's. */
static inline float net_ping_dot(float h) { return h / 540.0f; }
static inline float net_ping_size(float h) { return h * 26.0f / 1080.0f; }
/* the width of one item: icon, gap, the number, and (delay_chars > 0) a gap and the delay's text */
static inline float net_ping_width(float h, int chars, int delay_chars) {
    float dot = net_ping_dot(h), gap = dot / 2 > 1 ? dot / 2 : 1, cw = net_ping_size(h) / 2;
    return 4 * dot + 3 * gap + dot * 2 + chars * cw + (delay_chars > 0 ? 3 * dot + delay_chars * cw : 0);
}
/* A 2-player fight: where player `slot`'s item ends (its right edge) and its bottom, over a w x h picture (the HUD stays
 * in the middle 16:9 part of a wider one: docs/ULTRAWIDE.md, damage meters). */
static inline void net_ping_anchor(float w, float h, int slot, float *right, float *bottom) {
    static const float fx[2] = {0.243f, 0.628f};
    float pw = w, px = 0;
    if (w * 9 > h * 16) { pw = h * 16 / 9; px = (w - pw) / 2; }
    *right = px + fx[slot ? 1 : 0] * pw;
    *bottom = 0.978f * h;
}

#endif
