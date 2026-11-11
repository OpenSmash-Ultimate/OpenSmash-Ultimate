/* Epic Online Services' P2P, the relay of last resort (net_eos.c). */
#ifndef NET_EOS_H
#define NET_EOS_H
#include "host.h"

#define NET_EOS_ID_LEN 32   /* a product user id, as text */
#define NET_EOS_MAX 1170    /* the most one EOS packet carries (EOS_P2P_MAX_PACKET_SIZE) */

void eos_start(void);                                     /* (once) off without the SDK beside the program, eos.txt or with SSBU_EOS=0 */
int eos_ready(void);                                      /* logged in */
const char *eos_my_id(void);                              /* this player's id, NULL until ready */
int eos_send(const char *to, const void *data, u32 len);  /* queued; 0: not sent (not ready, too long, queue full) */
void eos_counts(u64 *sent, u64 *received, u64 *dropped);

/* (net_relay.c) a packet from another player through EOS, on the EOS thread */
void net_eos_packet(const char *from, const u8 *data, u32 len);
#endif
