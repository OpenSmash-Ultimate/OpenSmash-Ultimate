/* Rollback netplay's links (hle_rb_core.c calls rbl_*): the direct UDP link, hle_rb_link.c, and the
 * link through the netplay relay, hle_rb_relay.c. While the relay's is open, hle_rb_link.c hands
 * every rbl_* call to it through rbl_relay (NULL: the UDP link), so a program with the UDP link
 * alone (tools/dev/rblinktest.c) needs nothing of the relay. */
#ifndef HLE_RB_LINK_H
#define HLE_RB_LINK_H

typedef struct RblOps {
    void (*close)(void);
    void (*players)(int n);
    void (*send_to)(int p, const void *buf, int len);
    void (*send)(const void *buf, int len);
    int (*recv)(void *buf, int cap);
    double (*last_arrival_ms)(void);
    int (*accept)(int p);
    int (*has_peer)(int p);
    int (*who)(void);
    void (*assign)(int p);
    void (*stats)(long long *s, long long *r, long long *e);
    long long (*dropped)(void);
} RblOps;
extern const RblOps *rbl_relay;   /* (hle_rb_link.c) */

/* hle_rb_relay.c. local: this game's player (0 the host), -1 a joiner the host numbers, RBL_AUTO from the
 * room's order; players: how many play. 1: open (rbl_* go through the relay from now on). */
#define RBL_AUTO (-2)
int rbl_open_relay(int local, int players);
int rbl_relay_number(void);    /* (RBL_AUTO) this game's number once the room has every player, else -1 */
void rbl_relay_report(void);   /* (the fight's report) which way the messages went */

#endif
