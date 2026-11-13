/* Rollback build, M5: the netcode's own part, which knows nothing of the game (hle_rb_net.c). Pads by
 * logic frame (the game's counter at 0x71052c5184, as the input tape): the local player's are
 * kept `delay` frames ahead, the others' come from the network, and a frame run before a pad
 * arrived runs on a guess (the player's last pad, held). A guess the real pad contradicts is a
 * mismatch: the caller goes back to before that frame and runs it again. Plain C, no Windows:
 * patches\rollback\tests\rb_net_test.c runs it alone, two peers over a bad link. */
#ifndef HLE_RB_NET_H
#define HLE_RB_NET_H
#include <stdint.h>
#include <stddef.h>

#define RBN_PLAYERS 8
#define RBN_RING 256          /* frames kept a player (a power of two) */
#define RBN_MAX_BACK 7        /* the furthest a rollback goes: a game waits rather than run past it */
#define RBN_REST 1600         /* a stick within this of the middle is the middle: noise is no reason to roll back */
#define RBN_MAX_PADS 64       /* pads in one message (the unacknowledged ones, newest last) */

typedef struct RbnPad { uint64_t buttons; int16_t lx, ly, rx, ry, lt, rt; } RbnPad;

/* ---- the input queues ---- */
void rbn_reset(int nplayers, int local, int delay);   /* a match starts: frames from 1 */
int rbn_delay(void);
RbnPad rbn_rest(RbnPad p);                             /* sticks and triggers as both sides store them */
int rbn_same(const RbnPad *a, const RbnPad *b);
/* the local pad read at `frame`: kept for frame + delay; returns the one for `frame` (none in the first frames) */
RbnPad rbn_local(uint32_t frame, RbnPad now);
int rbn_local_get(uint32_t frame, RbnPad *out);
uint32_t rbn_local_newest(void);
/* a pad of player p for `frame` from the network (one already known: ignored) */
void rbn_remote(int p, uint32_t frame, RbnPad pad);
extern long long rbn_miss[5];   /* (wrong guesses by kind: buttons, left stick, right stick, triggers; sticks under 2048 apart) */
uint32_t rbn_remote_newest(int p);                     /* the newest frame such that every one up to it arrived */
/* the pad player p uses at `frame`: the real one, else a guess (*guessed = 1), checked when the real one comes */
RbnPad rbn_get(int p, uint32_t frame, int *guessed);
uint32_t rbn_peek_mismatch(void);                      /* (the pending one, left where it is) */
uint32_t rbn_take_mismatch(void);                      /* the earliest frame run on a wrong guess since the last call, or 0 */
void rbn_put_mismatch(uint32_t frame);                 /* (a rollback that could not be done now: again next time) */
uint32_t rbn_confirmed(void);                          /* the newest frame every player's real pad is known for */
/* may the game run `frame` now? Not when it would put a guess further back than RBN_MAX_BACK */
int rbn_can_run(uint32_t frame);
/* (v0.9.2) player p is gone from the fight after frame `after` (the same cutoff on every game: its LEFT, or the host's
 * word): its pads past it are none, nothing waits for them, and frames already run past it with other pads run again */
void rbn_gone(int p, uint32_t after, uint32_t ran);   /* (ran: the newest frame this game has run) */
uint32_t rbn_gone_after(int p);                        /* 0: not gone */

/* ---- the wire (little-endian, lengths checked) ----
 * PADS: type 1 | player u8 | count u8 | runs u8 (bit 7: behind, version 14) | first u32 | frame u32 | check_frame u32 | check u32 |
 *       ack u32 x RBN_PLAYERS | heard u32 x RBN_PLAYERS | runs x (frames u8 | pad: 20 bytes: buttons u64, lx ly rx ry lt rt i16)
 * first: the frame of the first pad; count: the pads in all (the runs' frames add up to it), each run one pad held
 * for that many frames in a row; frame: the frame the sender is running; check: its fight hash at check_frame;
 * ack[p]: the newest frame of player p's pads the sender has all of; heard[p]: the newest frame player p said it was
 * running, as the sender heard it (both 0 for the sender itself and for players not in the match).
 * behind (version 14): the sender could not keep its own pace - the mean of its last 60 timed frames over its
 * own schedule (RBN_PACE_BEHIND_MS), frames after a rollback, a wait for pads or a hold not counted (rbn_pace below). Runs are at most
 * RBN_MAX_PADS (64), so bit 7 of that byte was free.
 * (M7, version 5, 3+ players: an ack and a frame heard per player, not one peer's. A game resends its pads from the
 * oldest any other player lacks; each game's frame advantage over each other is told from heard: see rbn_sync.)
 * (M6, version 3: runs. Every pad not yet acknowledged goes in every message, so a longer trip made longer
 * messages - up to 64 pads, 1308 bytes, 60 a second - which a weak uplink could not carry: its queue grew, the trip
 * with it, and the messages again (inet1: the ack round trip 120 -> 1570 ms in the first 7 s). Most frames hold
 * the pad of the one before: in runs a message stays a few runs long however long the trip.) */
enum { RBN_MSG_PADS = 1 };
#define RBN_HEAD (20 + 8 * RBN_PLAYERS)
#define RBN_PAD_BYTES 20
#define RBN_RUN_BYTES (1 + RBN_PAD_BYTES)
#define RBN_MSG_MAX (RBN_HEAD + RBN_MAX_PADS * RBN_RUN_BYTES)   /* (the longest PADS: no two pads in a row alike) */
typedef struct RbnMsg {
    int type, player, count;
    uint32_t first, frame, check_frame, check;
    uint32_t ack[RBN_PLAYERS], heard[RBN_PLAYERS];
    int behind;
    RbnPad pads[RBN_MAX_PADS];
} RbnMsg;
size_t rbn_build(uint8_t *buf, size_t cap, const RbnMsg *m);    /* bytes written, 0 when it doesn't fit */
int rbn_parse(const uint8_t *buf, size_t len, RbnMsg *m);       /* 1: a valid message */

/* HELLO, the handshake before a match: type 2 | player u8 | version u8 | agreed u8 | delay u8 | players u8 | assign u8 | 0 u8 |
 * seed u64 | t u64 | echo u64 | hold u32 | 0 u32 (40 bytes).
 * Player 1's game proposes the match seed and the input delay; player 2's takes both and answers with the
 * seed (agreed 1); a seed of 0 is "here, nothing yet". M6 (version 2), the round trip: t is the sender's
 * clock (us) as it sent; echo the newest t it heard from the other, and hold how long ago (us) it heard
 * it. Round trip = now - echo - hold. input_delay (byte 4): player 1's input delay + 1 (0: not chosen yet; player 1 sends
 * no seed until it has chosen, so player 2 takes both at once). */
enum { RBN_MSG_HELLO = 2 };
#define RBN_HELLO_BYTES 40
#define RBN_VERSION 14  /* (3: PADS in runs; 4: a frame hash final once its own pads are real; M6. 5: an ack and a frame heard per player;
                            * 6: Host/Join, a joiner's number from the host; M7. 7: RULES. 8: READY. 9: MENU. 10: the name tag in CHOICE.
                            * 11: no + in fights (no pause menu online): every game must take it out alike.
                            * 12: a GameCube controller in CHOICE: its slot one on every PC, analog triggers and its controls.
                            * 13: a player without a tag sends its no-name controls as a tag: every game must, or they differ.
                            * 14: PADS carry the sender's behind bit: an older game would read it as 128 more runs) */
/* (M7: players, byte 5: how many play, from the host; 0 from the others.
 * Host/Join, version 6: a joining game has no number yet: player -1 (0xFF on the wire), and its seed field carries a
 * random join id instead, so the host tells two joiners from one address apart and knows one that comes back. The
 * host gives each joiner the lowest free player and says which in every hello to it: assign, byte 6, its player + 1.) */
typedef struct RbnHello { int player, version, agreed, input_delay; uint64_t seed, t, echo; uint32_t hold; int players, assign; } RbnHello;
size_t rbn_hello_build(uint8_t *buf, size_t cap, const RbnHello *h);
/* The input delay for a round trip (ms), when it is left to the handshake (RB_DELAY=auto): 1 up to 50 ms,
 * one more for each 50 ms, at most 5. The rest of the trip the rollback covers (RBN_MAX_BACK frames). */
int rbn_delay_for_rtt(double rtt_ms);
/* (SSBU_RB_NET=online) Less delay, more rollback: the one-way trip in frames, less one (from 100 ms one way, not less one), from 2 to 9
 * (1 under 15 ms: a home network). Measured on the
 * game (docs/MULTIPLAYER.md): at ~110 ms round trip delay 2 rolls back 2-3 frames, re-run in 10-11 ms (a frame shown
 * stays under 16.7 ms), where delay 1 rolls back 3-5 in 17-20 ms (frames late) and the general rule's 4 adds 67 ms. */
int rbn_online_delay_for_rtt(double rtt_ms);
int rbn_hello_parse(const uint8_t *buf, size_t len, RbnHello *h);

/* CHOICE, the match's set-up: a player's own fighter and colour, from the game that plays it; player 1's
 * game adds the stage. type 3 | player u8 | colour u8 | flags u8 (1: stage, 2: answer, 4: name tag, 8: a GameCube controller) | hazards u8 | 0 u8 x3 |
 * chara u64 (ui_chara hash) | stage 0x20 bytes (the stage choice entry) | tag 0x60 bytes (the head of the player's
 * name-tag slot: used, name hash, length, the name, its controls; zeros without one) (144 bytes). */
enum { RBN_MSG_CHOICE = 3 };
#define RBN_TAG_BYTES 0x60
#define RBN_CHOICE_BYTES (48 + RBN_TAG_BYTES)
typedef struct RbnChoice { int player, colour, has_stage, hazards; uint64_t chara; uint8_t stage[0x20]; int ask;   /* ask (flags 2): answer with yours */
                           int has_tag; uint8_t tag[RBN_TAG_BYTES]; int gc; } RbnChoice;   /* gc (flags 8): the player plays a GameCube controller */
size_t rbn_choice_build(uint8_t *buf, size_t cap, const RbnChoice *c);
/* RULES (streamlining, item 2): the host's rules for the match - the game's rule header, the stage set-up's inputs, the
 * rule kind and stocks - a blob of `total` bytes, sent in chunks of RBN_RULES_CHUNK (one UDP packet each, the whole
 * blob being over one). type 4 | player u8 | flags u8 (1: ask, no data: the host answers with every chunk) | 0 u8 |
 * id u32 (the blob's hash: chunks of two set-ups never mix) | total u16 | off u16 | len u16 | 0 u16 | len bytes. */
enum { RBN_MSG_RULES = 4 };
#define RBN_RULES_HEAD 16
#define RBN_RULES_CHUNK 0x300
#define RBN_RULES_MAX 0x1000
typedef struct RbnRules { int player, ask; uint32_t id; int total, off, len; uint8_t data[RBN_RULES_CHUNK]; } RbnRules;
size_t rbn_rules_build(uint8_t *buf, size_t cap, const RbnRules *r);
/* READY (the GO! freeze): a game whose loading is done says so to the host; the host, once every player has, tells them
 * all to go on (all 1), so every game starts the fighters' entrances together. type 5 | player u8 | all u8 | 0 u8. */
enum { RBN_MSG_READY = 5 };
#define RBN_READY_BYTES 4
typedef struct RbnReady { int player, all; } RbnReady;
size_t rbn_ready_build(uint8_t *buf, size_t cap, const RbnReady *r);
/* MENU (the results screen): a player's A and B as they are now, from its game, every frame while the results show; the
 * other games press them for that player, so its card shows ready on every PC. type 6 | player u8 | buttons u8 (1 A,
 * 2 B) | 0 u8. */
enum { RBN_MSG_MENU = 6 };
#define RBN_MENU_BYTES 4
typedef struct RbnMenu { int player, buttons; } RbnMenu;
size_t rbn_menu_build(uint8_t *buf, size_t cap, const RbnMenu *m);
int rbn_menu_parse(const uint8_t *buf, size_t len, RbnMenu *m);
/* LEFT (v0.9): a game's fight is over (or it gave up on it): its last logic frame, so the others stop waiting for pads
 * past it - a desync had one PC's fight end while the other's went on, waiting 10 min for pads that never came.
 * type 7 | player u8 | 0 u8 x2 | frame u32. (Older games drop it: no PADS.) */
enum { RBN_MSG_LEFT = 7 };
#define RBN_LEFT_BYTES 8
typedef struct RbnLeft { int player; uint32_t frame; } RbnLeft;
size_t rbn_left_build(uint8_t *buf, size_t cap, const RbnLeft *l);
int rbn_left_parse(const uint8_t *buf, size_t len, RbnLeft *l);
/* PRE (v0.9.4): how many frames a game has run since the match set-up, every frame until the fight: a game ahead of the
 * others waits for them there, in the loading screen, so they all come to GO! together instead of one waiting at GO!.
 * type 8 | player u8 | 0 u8 x2 | frames u32. (Older games drop it, and a game never heard from this way isn't waited for.) */
enum { RBN_MSG_PRE = 8 };
#define RBN_PRE_BYTES 8
typedef struct RbnPre { int player; uint32_t frames; } RbnPre;
size_t rbn_pre_build(uint8_t *buf, size_t cap, const RbnPre *p);
int rbn_pre_parse(const uint8_t *buf, size_t len, RbnPre *p);
int rbn_ready_parse(const uint8_t *buf, size_t len, RbnReady *r);
int rbn_rules_parse(const uint8_t *buf, size_t len, RbnRules *r);
int rbn_choice_parse(const uint8_t *buf, size_t len, RbnChoice *c);

/* ---- keeping the two games' frames together (frame advantage, as GGPO does it) ----
 * Each game's advantage: its frame minus the peer's last frame heard of (+ 1). Both are late by the
 * trip, alike on both sides, so the difference of the two advantages is twice the real gap. The game
 * further ahead waits half the difference (at most 8 frames in 32), one frame at a time spread out, so
 * both sides end up rolling back about as much. In the game a wait leaves a refresh out (ipc_vi.c).
 * (M7, 3+ players: samples are kept per player, and a game waits for the one it is furthest ahead of.) */
void rbn_sync_reset(void);
void rbn_sync_sample(int p, int32_t local_adv, int32_t remote_adv);   /* one per message received from player p */
int rbn_sync_wait(uint32_t frame);                              /* frames to wait before this one (0 or 1) */
int rbn_players(void);
int rbn_local_player(void);

/* ---- pace: which PC could not keep full speed ----
 * A game's frames are timed against its own schedule: from the end of one frame's net work to the end of the
 * next's save, the game's own time, and late past RBN_PACE_LATE_MS. A frame after a rollback, a wait for pads or a hold
 * is not counted: those are the other PC's or the link's doing. behind: the last RBN_PACE_SPAN
 * counted frames, the mean of their times over RBN_PACE_BEHIND_MS (at least half of them counted; one frame counts at
 * most RBN_PACE_CLAMP_MS, so a single stall does not keep a PC behind for a span). The mean, not the share of late
 * frames: a PC that keeps the frame period exactly has about half its frames past any margin, by jitter. Every PADS message carries it, so each game's log can say
 * which PC fell behind: a slow peer and a slow link look alike in the frame advantage, not in this bit.
 * (Background, not measured here: the P+ Brawl rollback notes at brawlonline.net describe the same trouble with
 * following the gap alone. The thresholds below are untested on two PCs: only simulated slow PCs on one.) */
#define RBN_PACE_FRAME_MS (1000.0 / 60)
#define RBN_PACE_LATE_MS (RBN_PACE_FRAME_MS + 2.0)
#define RBN_PACE_BEHIND_MS (RBN_PACE_FRAME_MS + 1.5)
#define RBN_PACE_CLAMP_MS (RBN_PACE_FRAME_MS * 3)
#define RBN_PACE_SPAN 60
typedef struct RbnPace { uint8_t late[RBN_PACE_SPAN]; double ms[RBN_PACE_SPAN]; int n, next, nlate; } RbnPace;
void rbn_pace_reset(RbnPace *p);
int rbn_pace_frame(RbnPace *p, double game_ms);   /* one counted frame, its own time; 1: late (past RBN_PACE_LATE_MS; a count for the log) */
int rbn_pace_behind(const RbnPace *p);
/* The fight's verdict from the share of this game's counted frames it was behind at, the share of the peer's messages
 * that said behind, the fight's speed (logic frames / (seconds * 60)) and catch_share (below). Speed first: at
 * RBN_VERDICT_SPEED or faster the verdict is FULL, whatever the shares. Slower: BOTH, OURS or PEER by the shares
 * (RBN_VERDICT_BEHIND, a quarter, or more is behind), then CATCH when catch_share is a quarter or more, else LINK. */
enum { RBN_VERDICT_FULL, RBN_VERDICT_OURS, RBN_VERDICT_PEER, RBN_VERDICT_BOTH, RBN_VERDICT_LINK, RBN_VERDICT_CATCH };
#define RBN_VERDICT_BEHIND 0.25
#define RBN_VERDICT_SPEED 0.95
#define RBN_VERDICT_CATCH_SHARE 0.25
/* catch_share: the share of the fight's time this game spent running frames again after a rollback. Those frames are
 * not timed (see above), so a PC slowed by its own rollbacks is not "behind"; when the fight ran slow, nobody was
 * behind and this share is a quarter or more, the verdict says so (RBN_VERDICT_CATCH). */
int rbn_pace_verdict(double own_behind, double peer_behind, double speed, double catch_share);

void *rbn_state(size_t *size);   /* (the whole session's state: the test swaps two) */

#endif
