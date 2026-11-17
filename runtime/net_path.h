/* Which path a pair of players' packets take (net_path.c): our own direct UDP path, or Epic's P2P (EOS's own punch
 * through routers, else its relays). Both are measured all the time with pings; the choice moves with hysteresis.
 *
 * - Direct first: while the direct path answers it is used, unless Epic's path is clearly quicker - its round trip
 *   at least PATH_BETTER_PCT % and PATH_BETTER_MS ms below the direct one's, for PATH_HOLD_MS on end.
 * - Back the same way: once Epic's path is no longer clearly quicker (for PATH_HOLD_MS), direct again.
 * - Never a move in the middle of a fight, unless the path in use has died (no answer for PATH_DEAD_MS).
 * - Neither path alive: Epic's (it may come back, and it is where packets go before anything answered).
 * Pure logic, no sockets: the times are the caller's (ms), the round trips are its pings'. */
#ifndef NET_PATH_H
#define NET_PATH_H
#include <stdint.h>

enum { PATH_EOS, PATH_DIRECT, PATH_COUNT };
#define PATH_DEAD_MS 1500     /* no answer this long: the path is dead */
#define PATH_HOLD_MS 3000     /* one path clearly better this long before a move */
#define PATH_BETTER_PCT 15    /* ...by this share of the other's round trip */
#define PATH_BETTER_MS 5      /* ...and by this much at least */

typedef struct PathPick {
    int cur;                         /* PATH_EOS or PATH_DIRECT */
    double rtt[PATH_COUNT];          /* smoothed round trip (ms), 0 none yet */
    uint64_t last[PATH_COUNT];       /* the last answer (ms), 0 never */
    uint64_t want_since;             /* since when the other path has been the one wanted (0: it is not) */
    int moves;                       /* moves so far */
    int why;                         /* the last move's reason: PATH_WHY_* */
} PathPick;
enum { PATH_WHY_NONE, PATH_WHY_DEAD, PATH_WHY_UP, PATH_WHY_QUICKER, PATH_WHY_NOT_QUICKER };

void path_init(PathPick *p);
void path_answer(PathPick *p, int path, double rtt_ms, uint64_t now);   /* a pong on that path */
int path_alive(const PathPick *p, int path, uint64_t now);
/* the path to use now (moves p->cur when it should); in_fight: a fight is on between the pair */
int path_update(PathPick *p, uint64_t now, int in_fight);
const char *path_name(int path);
const char *path_why(int why);
#endif
