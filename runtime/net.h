/* Netplay: the game's own Local Wireless mode, on a virtual LAN carried through a relay.
 *
 * Each player's game gets an address in 10.13.0.0/16 from the relay; local wireless
 * (runtime/hle_net.c emulates nn::ldn) and the game's sockets live in that network, and their
 * datagrams - broadcasts included - travel to the other players through the relay
 * (runtime/net_relay.c), or straight to them once the online lobby has introduced the players
 * of a room to each other and their games have punched through their routers. Clients only
 * send to the relay (and those players), so no player needs port forwarding. The relay is
 * either the online lobby (tools/ssbu_relay.py on a server: it lists every player's Local
 * Wireless rooms to the others and carries each room's traffic between its players only) or a
 * private room's relay inside the host player's game (its port opened with UPnP).
 *
 * Configured by the launchers (Play SSBU.bat, launchers\Host/Join SSBU Netplay.bat) through the environment:
 *   SSBU_NET_HOST=1         run the relay here and join it (port SSBU_NET_PORT, 39420)
 *   SSBU_NET_RELAY=addr     join the lobby or relay at addr[:port]
 *   SSBU_NET_ROOM           the lobby code (a relay's room); smash, the default, is the public lobby
 *   SSBU_NET_NAME           player name
 *   SSBU_NET_DIRECT=0       no direct connections: all of a room's traffic through the lobby, and the
 *                           lobby gives this player's internet address to nobody
 *   SSBU_NET_LOG=1          log every socket/network call (debugging)
 *   SSBU_NET_PROTOCOL=3     (being tried out) join the lobby in protocol version 3, which has a room browser
 *                           and matchmaking; a lobby or a private room's relay of version 2 says no, and the
 *                           game joins it in version 2 at once. 2, the default: version 2 only, as before.
 *                           Without it, online_protocol.txt says the same (3 or 2; the settings menu's Online
 *                           > Netplay version writes it), whatever names the lobby; hosting is version 2.
 * With neither set the game stays offline, as before. */
#pragma once
#include "recomp.h"

#define NET_SUBNET      0x0A0D0000u   /* 10.13.0.0 */
#define NET_MASK        0xFFFF0000u
#define NET_GATEWAY     0x0A0D0001u   /* 10.13.0.1: the relay's own address */
#define NET_BROADCAST   0x0A0DFFFFu
#define NET_DEFAULT_PORT 39420
#define NET_PROTOCOL    2             /* a private room's relay speaks it, and a game falls back to it */
#define NET_PROTOCOL_V3 3             /* the online lobby's version 3 (tools/ssbu_relay.py): version 2 and more */
#define NET_PROTOCOL_DEFAULT NET_PROTOCOL   /* what a game asks for without SSBU_NET_PROTOCOL (3 once played with) */
#define NET_LDN_PORT    65511         /* local wireless's own messages (hle_net.c): always through the relay */

int net_configured(void);            /* netplay asked for (host or join) */
int net_log(void);                   /* SSBU_NET_LOG */
u32 net_local_ip(void);              /* this player's virtual address (host order), 0 until the relay gives one */
const char *net_player_name(void);
/* The lobby or relay this game joins (host[:port]): SSBU_NET_RELAY's, else online_server.txt's
 * with --online; NULL when it hosts a private room or plays offline. */
const char *net_relay_address(void);
int net_unreachable(void);           /* asked for, but the relay has not answered for a while: offline */

/* The online lobby: rooms are listed by the lobby, not found by their broadcasts. */
int net_lobby(void);                 /* joined to a lobby (not a private room's relay) */
void net_lobby_list(u32 seq);        /* ask for the rooms; they come back through ldn_listing, then ldn_list_end */
/* (hle_net.c) One room of the list asked for as seq (owner: its host's address), and the list's end. */
void ldn_listing(u32 seq, u32 owner, const u8 *info);
void ldn_list_end(u32 seq);

/* A datagram from one of this player's sockets to another player (or the room: NET_BROADCAST). */
void net_send(u32 src_ip, u16 src_port, u32 dst_ip, u16 dst_port, const void *data, u32 len);
/* (hle_net.c) A datagram for this player's sockets, from the relay, another player directly, or looped back. */
void vsock_deliver(u32 src_ip, u16 src_port, u32 dst_ip, u16 dst_port, const void *data, u32 len);
/* (hle_net.c) Whether this player is in a local wireless network now, the player at ip one of its
 * nodes: only then do that player's direct packets reach the game. */
int ldn_in_network(u32 ip);

/* One line about the connection for the overlay (empty when netplay is off). */
void net_status(char *out, size_t n);

/* ---- version 3: the room browser and matchmaking, for a menu page ---------------------------
 * A lobby of version 3 has them when its WELCOME says so; with a lobby of version 2 or a private
 * room's relay, both say NET_*_OFF. Nothing here blocks: the client thread asks the lobby and
 * reads its answers. */

/* The netplay version this game asks for first (SSBU_NET_PROTOCOL, else online_protocol.txt; 2 when
 * it hosts), and the one it speaks now: the lobby's answer once joined (2 after a REJECT of version
 * 2's), 0 before. */
int net_protocol_wanted(void);
int net_protocol_spoken(void);

/* The room browser: every room of this lobby code, whatever its game's build, simulation version
 * (sim_version.h) or mods, the rooms of this game's own key (those three) first; a room of another
 * key comes with why this game cannot join it (the game's own Rooms List, in Local Wireless, keeps
 * to the ones it can). net_rooms_ask asks the lobby again (answered within a moment); net_rooms_get
 * copies the last answer, at most max rooms (asking itself the first time), and says how it stands. */
enum { NET_ROOMS_OFF, NET_ROOMS_ASKING, NET_ROOMS_NO_ANSWER, NET_ROOMS_LISTED };
enum { NET_WHY_NONE, NET_WHY_BUILD, NET_WHY_SIM, NET_WHY_MODS };   /* the first of the three that differs */
typedef struct NetRoom {
    u32 host;                      /* its host's address (10.13.x.y) */
    char name[33], build[41];      /* its host's name; its game's build id */
    u32 sim;                       /* its game's simulation version */
    int players, most, open, mine; /* mine: this game's own room */
    int joinable;                  /* of this game's build, simulation version and mods: it can join */
    int nmods;
    char mods[256];                /* its mods' names, ", " between them ("" for none) */
    char why[256];                 /* not joinable: why, in one line or more */
    int why_kind;                  /* not joinable: NET_WHY_* (another build, simulation version or mods) */
} NetRoom;
void net_rooms_ask(void);
int net_rooms_get(NetRoom *out, int max, int *state);   /* the rooms copied; state: NET_ROOMS_* */

/* Matchmaking: the lobby pairs two players of the same build, simulation version, mods and lobby
 * code who search, and says which one hosts. The host makes the room (Local Wireless > Create
 * Room, or keeps its open one); the other finds it first in its Rooms List. net_match_find starts
 * a search (a new one in place of the last), net_match_cancel ends it or leaves the match (the
 * other player is told), net_match_get says how it stands. */
enum { NET_MATCH_IDLE, NET_MATCH_SEARCHING, NET_MATCH_MATCHED,   /* (the lobby's states, then how a search ended:) */
       NET_MATCH_TIMEOUT, NET_MATCH_LEFT, NET_MATCH_TOGETHER, NET_MATCH_EXPIRED,   /* no opponent in time, the other
                                    cancelled or left, both in one room, matched but not in one room in time */
       NET_MATCH_ASKING, NET_MATCH_OFF };   /* asked, no answer yet; not joined, or the lobby has no matchmaking */
enum { NET_MATCH_HOST = 1, NET_MATCH_JOIN = 2 };
typedef struct NetMatch {
    int state, role;               /* NET_MATCH_*; matched (and how it ended): NET_MATCH_HOST or NET_MATCH_JOIN */
    int room_open;                 /* matched: the host's room is open, and listed */
    u32 peer;                      /* matched: the other player's address, and its name */
    char peer_name[33];
    int waited_s;                  /* seconds searching, or since matched */
    int players, searching;        /* of this game's key and lobby code: players, and of them searching (-1: not known) */
    int code_players;              /* players of this lobby code, of every key (-1: not known) */
} NetMatch;
void net_match_find(void);
void net_match_cancel(void);
void net_match_get(NetMatch *out);
