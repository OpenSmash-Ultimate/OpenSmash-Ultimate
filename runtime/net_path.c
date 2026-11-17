/* The choice of a pair's path (net_path.h): direct first, Epic's P2P when direct is dead or clearly slower. */
#include "net_path.h"

void path_init(PathPick *p) {
    p->cur = PATH_EOS;
    p->rtt[PATH_EOS] = p->rtt[PATH_DIRECT] = 0;
    p->last[PATH_EOS] = p->last[PATH_DIRECT] = 0;
    p->want_since = 0;
    p->moves = 0;
    p->why = PATH_WHY_NONE;
}

void path_answer(PathPick *p, int path, double rtt_ms, uint64_t now) {
    if (path < 0 || path >= PATH_COUNT || rtt_ms < 0) return;
    p->rtt[path] = p->rtt[path] > 0 && p->last[path] && now - p->last[path] < PATH_DEAD_MS ? p->rtt[path] * 0.8 + rtt_ms * 0.2 : rtt_ms;
    p->last[path] = now ? now : 1;
}

int path_alive(const PathPick *p, int path, uint64_t now) { return p->last[path] && now - p->last[path] < PATH_DEAD_MS; }

/* Epic's path clearly quicker than the direct one (both alive) */
static int eos_quicker(const PathPick *p) {
    double d = p->rtt[PATH_DIRECT], e = p->rtt[PATH_EOS];
    return e > 0 && d > 0 && e <= d * (1.0 - PATH_BETTER_PCT / 100.0) && e <= d - PATH_BETTER_MS;
}

static int move(PathPick *p, int to, int why) {
    p->cur = to;
    p->want_since = 0;
    p->moves++;
    p->why = why;
    return to;
}

int path_update(PathPick *p, uint64_t now, int in_fight) {
    int direct = path_alive(p, PATH_DIRECT, now), eos = path_alive(p, PATH_EOS, now);
    if (!path_alive(p, p->cur, now)) {   /* (the path in use died: at once, fight or not) */
        if (p->cur == PATH_EOS && direct) return move(p, PATH_DIRECT, PATH_WHY_DEAD);
        if (p->cur == PATH_DIRECT) return move(p, PATH_EOS, PATH_WHY_DEAD);   /* (Epic's, alive or not: where packets wait) */
        return p->cur;
    }
    int want;
    if (!direct) want = PATH_EOS;
    else if (!eos) want = PATH_DIRECT;
    else want = eos_quicker(p) ? PATH_EOS : PATH_DIRECT;
    if (want == p->cur) { p->want_since = 0; return p->cur; }
    if (in_fight) { p->want_since = 0; return p->cur; }   /* (never mid-fight while the path in use lives) */
    if (want == PATH_DIRECT && !p->moves) return move(p, PATH_DIRECT, PATH_WHY_UP);   /* (the first choice: direct as soon as it answers) */
    if (!p->want_since) { p->want_since = now ? now : 1; return p->cur; }
    if (now - p->want_since < PATH_HOLD_MS) return p->cur;
    return move(p, want, want == PATH_EOS ? PATH_WHY_QUICKER : p->why == PATH_WHY_QUICKER ? PATH_WHY_NOT_QUICKER : PATH_WHY_UP);
}

const char *path_name(int path) { return path == PATH_DIRECT ? "direct" : "Epic's P2P"; }
const char *path_why(int why) {
    switch (why) {
    case PATH_WHY_DEAD: return "the path in use stopped answering";
    case PATH_WHY_UP: return "the direct path answers";
    case PATH_WHY_QUICKER: return "Epic's path is clearly quicker";
    case PATH_WHY_NOT_QUICKER: return "Epic's path is no longer clearly quicker";
    default: return "-";
    }
}
