/* The in-game NEX server's gatherings in EOS Lobbies (net_eos_lobby.c): plan/EOS_MIGRATION.md, phase 2 part B. */
#pragma once
#include "net_nex.h"

NexStore nex_store_eos(void);
/* (under the server's lock, no handler running) what the EOS thread learnt: other games' notifications for this
 * game's player, gatherings changed elsewhere, games gone from a gathering; and this game's player for the hub */
void nex_store_eos_sync(NexServer *s);
/* (runtime/hle_online.c, called on the EOS thread) take the server's lock if it is free and sync; 0: busy, later */
int online_nex_deliver(void);
