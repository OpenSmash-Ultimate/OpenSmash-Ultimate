/* The game's Online mode (runtime/hle_online.c): NEX's stream sockets.
 *
 * The virtual LAN of runtime/net.h carries datagrams. NEX connects to its game server with TCP; a
 * stream socket the game connects to the online server (10.13.0.1) is carried by a real TCP
 * connection to the lobby. hle_net.c hands every stream socket's calls here (errors are the
 * guest's errno values: 0 or a positive E...). */
#pragma once
#include "recomp.h"

int online_stream_connect(i32 fd, u32 ip, u16 port, u32 my_ip, u16 my_port, int nonblock);   /* 0, or an errno */
i64 online_stream_send(i32 fd, const void *data, u64 len, int nonblock);   /* bytes, or -errno */
i64 online_stream_recv(i32 fd, void *buf, u64 len, int peek, int nonblock);   /* bytes (0: closed), or -errno */
u16 online_stream_revents(i32 fd, u16 events);   /* poll: POLLIN / POLLOUT / POLLERR / POLLHUP */
int online_stream_error(i32 fd);                 /* SO_ERROR: the connect's outcome, once */
void online_stream_close(i32 fd);

/* (rollback, SSBU_RB: hle_net.c's net_rb_save / net_rb_load) Each stream's place in what the game read and
 * sent at a save; a load puts back what was read since (read again) and leaves out what is sent again. */
#define ONLINE_RB_STREAMS 16
typedef struct OnlineRb { struct { i32 fd; u32 gen; u64 taken, sent; } s[ONLINE_RB_STREAMS]; } OnlineRb;
void online_rb_save(OnlineRb *o);
void online_rb_load(const OnlineRb *o);
void online_rb_counts(long long *back, long long *left_out, long long *differ);

/* (hle_net.c) Wake poll() and blocking calls: something changed on a stream. */
void net_wake(void);
