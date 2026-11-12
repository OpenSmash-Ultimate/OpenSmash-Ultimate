/* Discord's Social SDK in the game (net_discord.c): sign-in with Discord (and with it EOS Connect, net_eos.c), and
 * Rich Presence with "Ask to Join" / "Join". Off without discord_partner_sdk.dll beside the program, or SSBU_DISCORD=0. */
#pragma once
#include <stdint.h>
#include "net_discord_core.h"

void discord_start(void);        /* (once, at the start) */
int discord_on(void);            /* the SDK runs */
int discord_signed_in(void);     /* a Discord account is connected */

/* What the game is doing, from anywhere (the Discord thread shows it within a second or two). */
void discord_note_online(int where, int players, int max_players, int in_match, const char *arena_id, const char *party);   /* DP_MENUS: none */
void discord_note_room(int in_room, int players, int max_players, int hosting, const char *host_id);   /* Local Wireless */
void discord_note_fighter(int kind, uint32_t gsp);
void discord_note_lobby_code(const char *code);
/* A friend's "Join" accepted in Discord, not yet acted on: 1 and *j (taken: the next call says 0). */
int discord_take_join(DiscordJoin *j);
