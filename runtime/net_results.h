/* Each match this game plays online, reported to a lobby's count of matches played (net_results.c). Before
 * OpenSmash 0.5.0 the same thread also sent the player's fighter records and ranked statistics to the public lobby
 * for a game whose game server was its own (Online over Epic Online Services): gone with EOS. */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* (OpenSmash mastery points) what this game saw of the match, for the lobby's points (it computes them: the game says
 * no number). Every game of the match sends it; the lobby gives full points only when they agree. Unknown values are
 * 0 (a place, stocks) or -1 (the player who left), never guessed. */
enum { MASTERY_END_NORMAL, MASTERY_END_TIME, MASTERY_END_LEFT };
#define MASTERY_PLAYERS 8
typedef struct {
    int have;          /* 0: no mastery object (the report's data as before 0.5.x: the lobby gives no points) */
    int me;            /* this game's player slot, 0-based */
    int end;           /* MASTERY_END_*: the last stock lost (normal), the time limit (time), a player left or was lost */
    int left;          /* the slot of the player who left, -1: none or not known */
    int stock_start;   /* the stocks each player started with, 0 for a time match */
    int n;             /* players */
    struct {
        char fighter[24];   /* the ui_chara_db name without "ui_chara_" ("mario", "miifighter"); "" not known */
        int stocks;         /* stocks left at the end (0 in a time match) */
        int place;          /* 1 the winner, 2 ...; 0 not known (a tie, a time match's) */
    } p[MASTERY_PLAYERS];
} MatchMastery;

/* A match this game played online, for the public lobby's count of matches played (POST /match/played,
 * tools/nex/matches.py): every game of the match sends it, with the same id, and the lobby counts it once. */
typedef struct {
    uint64_t id;     /* the same on every game of the match */
    int exact;       /* 1: id from the rollback handshake's match seed (that match alone); 0: from the players' addresses
                      * and the mode (the same for those players' next match: the lobby takes reports a minute apart) */
    int mode;        /* the match block's melee mode: 14 Quickplay, 20 Battle Arena, any other a Local Wireless room's */
    int players, seconds, rollback;
    MatchMastery mastery;   /* (its "mastery" object; none when mastery.have is 0) */
} MatchPlayed;
/* (game thread) queued, sent by the results' thread (started here if it is not yet) to the official lobby the game
 * plays on, or to SSBU_RESULTS_LOBBY's; nothing with SSBU_RESULTS_LOBBY=off, nor on a server of the player's own */
void results_match_played(const MatchPlayed *m);
/* (tests) the report's data string (JSON); its length, 0 when it does not fit */
size_t results_match_data(const MatchPlayed *m, char *out, size_t cap);
uint64_t results_match_id_seed(uint64_t seed);   /* the exact id of a rollback match's seed */
/* SSBU_MASTERY_REPORT=0: the report as before (no "mastery" object, no ticket header); on by default */
int results_mastery_on(void);
/* A fighter's key from its ui_chara id (0xc1 << 56 | hash40("ui_chara_<name>")): "<name>" into out; 0 not known */
int results_fighter_key(uint64_t ui_chara, char *out, size_t cap);
/* the id of a match without a seed: its players' virtual addresses (any order; this game's among them) and mode */
uint64_t results_match_id_addresses(const uint32_t *ips, int n, int mode);
