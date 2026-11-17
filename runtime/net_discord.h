/* Discord's Social SDK in the game (net_discord.c): sign-in with Discord and Rich Presence with "Ask to Join" / "Join".
 * Off without discord_partner_sdk.dll and discord.txt beside the program, with SSBU_DISCORD=0, or in headless test runs. */
#pragma once
#include <stdint.h>
#include "net_discord_core.h"

void discord_start(void);        /* (once, at the start) */
int discord_on(void);            /* the SDK runs */
int discord_signed_in(void);     /* a Discord account is connected */

/* (hle_net.c, every second) the player's Local Wireless room: in one, its players and most, its host's address */
void discord_note_room(int in_room, int players, int max_players, uint32_t host_ip);
