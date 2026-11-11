/* Epic Online Services' P2P, the relay of last resort (net_eos.c). */
#ifndef NET_EOS_H
#define NET_EOS_H
#include <stddef.h>
#include "host.h"

#define NET_EOS_ID_LEN 32   /* a product user id, as text */
#define NET_EOS_MAX 1170    /* the most one EOS packet carries (EOS_P2P_MAX_PACKET_SIZE) */

void eos_start(void);                                     /* (once) off without the SDK beside the program, eos.txt or with SSBU_EOS=0 */
int eos_ready(void);                                      /* logged in */
const char *eos_my_id(void);                              /* this player's id, NULL until ready */
int eos_send(const char *to, const void *data, u32 len);  /* queued; 0: not sent (not ready, too long, queue full) */
int eos_send_reliable(const char *to, const void *data, u32 len);   /* the same on channel 1: reliable, in order */
int eos_post(void (*fn)(void *), void *arg);              /* fn(arg) on the EOS thread, in order; 0: EOS off, queue full */
void eos_counts(u64 *sent, u64 *received, u64 *dropped);

/* The hub (phase 1 of plan/EOS_MIGRATION.md): one EOS lobby per lobby code, each member's virtual address an attribute. */
void eos_hub_join(const char *bucket);
u32 eos_hub_ip(void);                                     /* ours, 0 until the hub has it */
int eos_hub_failed(void);
int eos_hub_members(char ids[][NET_EOS_ID_LEN + 1], u32 *ips, int max);
int eos_hub_member_ip(const char *id, u32 *ip);
int eos_hub_member_id(u32 ip, char id[NET_EOS_ID_LEN + 1]);
/* every game's player in the hub (its NEX PID, station URL and rating as member attributes: net_eos_lobby.c) */
void eos_hub_set_player(u64 pid, const char *url, u32 rating);
int eos_hub_player(u64 pid, char id[NET_EOS_ID_LEN + 1], char *url, size_t cap, u32 *rating);   /* 1: a member; url "" unknown yet */
void eos_hub_refresh_soon(void);

/* The network over EOS alone (net_eos_net.c), and what net_relay.c gives it */
struct NetBackend;
extern const struct NetBackend net_eos_backend;
int net_eos_selected(void);                                  /* online_server.txt "eos [code]" or SSBU_NET_RELAY=eos */
void net_eos_names(char *code, size_t code_n, char *name, size_t name_n);
void net_part_key(char *out, size_t n);                     /* who may meet whom: build, lobby code, sim version, mods */
int net_rb_deliver(u32 src, const u8 *data, u32 len);        /* 1: rollback's sink took it */
void net_eos_net_packet(const char *from, const u8 *p, u32 n);

/* (net_relay.c) a packet from another player through EOS, on the EOS thread */
void net_eos_packet(const char *from, const u8 *data, u32 len);
#endif
