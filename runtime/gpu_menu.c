/* The settings menu (gpu_menu.h): display.ini, controllers.ini and the online_*.txt files,
 * changed from inside the game. Every change is saved at once (hle_settings.c keeps the files'
 * comments); the ones the game can take while running take effect at once, the others at its
 * next start, which the menu can do (Game > Restart).
 *
 * It runs on the vsync thread, inside the renderer's present: menu_frame reads the pads (their
 * console buttons, as the game would get them) and the keys the window queued, menu_draw
 * draws it over the picture with the overlay's quads. Pages: Display, Controllers (with a page
 * of a pad's buttons), Online, Game, Lobby (the online lobby's room browser and matchmaking, in
 * netplay version 3: net.h).
 *
 * Tests: SSBU_MENU_AT=<seconds> opens it then; SSBU_MENU_KEYS="<seconds>:<key>,..." presses
 * UP DOWN LEFT RIGHT ENTER ESC TAB BKSP DEL, SHOT (a screenshot with the menu), or types
 * =<text>. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "config_folder.h"
#include "display.h"
#include "gpu_menu.h"
#include "gpu_overlay.h"
#include "net.h"
#include "pad.h"
#include "settings.h"
#include "sim_version.h"

int gfx_window_focused(void);
void gfx_closing(void);   /* (gpu_d3d11.c) the game is about to end */
#ifdef _WIN32
int gfx_adapter_names(char names[][64], int max);   /* (gpu_d3d11.c) the graphics cards, fastest first */
#endif

#define PUBLIC_LOBBY "smash.recomps.wtf:39421"   /* (the online_server.txt the port comes with) */

static volatile int open_, releasing;
int menu_is_open(void) { return open_; }
int menu_blocks_input(void) { return open_ || releasing; }

/* ---- messages ------------------------------------------------------------------------- */
static SRWLOCK toast_lock = SRWLOCK_INIT;
static char toast[160];
static ULONGLONG toast_until;   /* 1: not shown yet (its time starts at the first draw) */

void menu_toast(const char *text) {
    AcquireSRWLockExclusive(&toast_lock);
    snprintf(toast, sizeof toast, "%s", text);
    toast_until = 1;
    ReleaseSRWLockExclusive(&toast_lock);
}

int menu_visible(void) { return open_ || (toast_until && (toast_until == 1 || GetTickCount64() < toast_until)); }

/* ---- keys from the window's thread ---------------------------------------------------- */
static volatile LONG q[64], q_head, q_tail;

static void push(LONG v) {
    LONG h = q_head;
    if (h - q_tail >= 64) return;
    q[h & 63] = v;
    InterlockedExchange(&q_head, h + 1);
}

void menu_key(int key) { push(key); }
void menu_char(unsigned ch) { if (ch >= 32 && ch < 127) push((LONG)(0x10000 | ch)); }

/* ---- what can be pressed ---------------------------------------------------------------- */
enum {   /* (from keys, pads or the test script) */
    EV_UP = 1, EV_DOWN, EV_LEFT, EV_RIGHT, EV_ENTER, EV_A, EV_BKSP, EV_B, EV_DEL, EV_NEXT, EV_PREV, EV_DONE, EV_ESC, EV_CHAR
};

/* ---- the rows of a page ------------------------------------------------------------------ */
enum { P_DISPLAY, P_PADS, P_ONLINE, P_GAME, P_LOBBY, P_REMAP, PAGES = P_REMAP };   /* (the pad's buttons: no key of their own) */
static const char *const PAGE_NAMES[PAGES] = {"Display", "Controllers", "Online", "Game", "Lobby"};
enum { K_INFO, K_CHOICE, K_ACTION, K_TEXT };
enum { W_NONE, W_NOW, W_RESTART };
enum {
    R_NONE, R_WINDOW, R_PACING, R_LATENCY, R_CURSOR, R_OVERLAY, R_SIZE, R_SCALE, R_GPU,
    R_PAD, R_PAD_PLAYER, R_PAD_LAYOUT, R_PAD_DEADZONE, R_PAD_RUMBLE, R_PAD_TRIGGER, R_PAD_REMAP, R_PAD_TEST, R_PAD_RESET,
    R_RUMBLE, R_KEYBOARD, R_BACKGROUND, R_PADS_FILE,
    R_NET_STATUS, R_NET_MODE, R_NET_CODE, R_NET_SERVER, R_NET_NAME, R_NET_DIRECT, R_NET_PROTOCOL,
    R_LOBBY, R_MATCH_FIND, R_MATCH_CANCEL, R_ROOMS,
    R_RESTART, R_QUIT, R_REMAP_BACK, R_ROOM_FIRST = 60,   /* + the room's place in the list */
    R_REMAP_FIRST = 100   /* + console button */
};

typedef struct Row { int id, kind, when; char label[48], value[112], note[64]; const char *help; } Row;
static Row rows[40];
static int nrows;

static Row *add(int id, int kind, int when, const char *label, const char *help) {
    Row *r = &rows[nrows < 39 ? nrows++ : 39];
    memset(r, 0, sizeof *r);
    r->id = id; r->kind = kind; r->when = when; r->help = help;
    snprintf(r->label, sizeof r->label, "%s", label);
    return r;
}

/* ---- state ---------------------------------------------------------------------------- */
static int page, sel[P_REMAP + 1], top[P_REMAP + 1];
static int pad_slot = -1;                /* the pad the Controllers page sets up */
static int confirm;                      /* a row asking yes or no */
static int editing, grid_r, grid_c;      /* a text row being typed */
static char edit_buf[64];
static int capture_button = -1;          /* the console button a new control is wanted for */
static u32 capture_held;
static ULONGLONG capture_until;
static int restart_pending;
static int *overlay_on;                  /* the frame-rate overlay's switch (gpu_d3d11.c) */
static char pins[PAD_SLOTS][128];        /* controllers.ini's [players], read at the menu's opening and changes */
static int rumble_slot = -1;
static ULONGLONG rumble_stop;
static char version[64];
static char gpus[8][64];
static int ngpus;
static int built_page = -1;              /* the page build() made last (-1: none since the menu opened) */

static struct { int mode; char custom[128], code[40], name[40]; int direct, protocol; } net;
enum { NET_PUBLIC, NET_PRIVATE, NET_CUSTOM, NET_OFFLINE };
static const char *const NET_MODES[4] = {"public lobby", "private lobby (a code)", "your own server", "offline"};

static void net_load(void) {
    char line[256] = "", host[128] = "", code[40] = "";
    settings_line_read(config_path(CONFIG_ONLINE_SERVER), line, sizeof line);
    sscanf(line, "%127s %39s", host, code);
    memset(&net, 0, sizeof net);
    snprintf(net.code, sizeof net.code, "%s", code);
    if (!host[0] || !_stricmp(host, "off")) net.mode = NET_OFFLINE;
    else if (!_stricmp(host, PUBLIC_LOBBY)) net.mode = code[0] ? NET_PRIVATE : NET_PUBLIC;
    else { net.mode = NET_CUSTOM; snprintf(net.custom, sizeof net.custom, "%s", host); }
    settings_line_read(config_path(CONFIG_ONLINE_NAME), net.name, sizeof net.name);
    char direct[16] = "";
    net.direct = !(settings_line_read(config_path(CONFIG_ONLINE_DIRECT), direct, sizeof direct) && !_strnicmp(direct, "off", 3));
    char protocol[16] = "";   /* (net_relay.c reads it the same way) */
    int v = settings_line_read(config_path(CONFIG_ONLINE_PROTOCOL), protocol, sizeof protocol) ? atoi(protocol) : 0;
    net.protocol = v == NET_PROTOCOL || v == NET_PROTOCOL_V3 ? v : NET_PROTOCOL_DEFAULT;
}

static void net_save_server(void) {
    char line[256];
    if (net.mode == NET_OFFLINE) snprintf(line, sizeof line, "off");
    else if (net.mode == NET_PUBLIC) snprintf(line, sizeof line, "%s", PUBLIC_LOBBY);
    else if (net.mode == NET_PRIVATE) {
        if (!net.code[0]) snprintf(net.code, sizeof net.code, "friends");
        snprintf(line, sizeof line, "%s %s", PUBLIC_LOBBY, net.code);
    }
    else snprintf(line, sizeof line, "%s%s%s", net.custom[0] ? net.custom : "127.0.0.1:39420", net.code[0] ? " " : "", net.code);
    settings_line_write(config_path(CONFIG_ONLINE_SERVER), line);
}

static void load_version(void) {
    char path[600];
    snprintf(path, sizeof path, "%s/../../version.txt", rt_data_dir ? rt_data_dir : ".");
    version[0] = 0;
    FILE *f = fopen(path, "r");
    if (f) {
        if (fgets(version, sizeof version, f)) version[strcspn(version, "\r\n")] = 0;
        fclose(f);
    }
}

static void load_pins(void);

static void open_menu(void) {
    if (open_) return;
    net_load();
    load_pins();
    load_version();
#ifdef _WIN32
    ngpus = gfx_adapter_names(gpus, 8);
#endif
    confirm = editing = 0;
    capture_button = -1;
    if (page == P_REMAP) page = P_PADS;
    open_ = 1;
    fprintf(stderr, "[menu] open\n");
}

static void close_menu(void) {
    if (!open_) return;
    confirm = editing = 0;
    capture_button = -1;
    releasing = 1;   /* (the game gets input back once nothing is held) */
    open_ = 0;
    built_page = -1;   /* (the Lobby page lists the rooms again when it shows next) */
    fprintf(stderr, "[menu] closed\n");
}

/* ---- saving --------------------------------------------------------------------------- */
static void changed(int when) {
    if (when == W_RESTART && !restart_pending) {
        restart_pending = 1;
        menu_toast("Saved: takes effect when the game starts again (Game > Restart)");
    }
}

static void set_display(const char *key, const char *value) { settings_ini_set(display_ini_path(), NULL, key, value); }

/* The [device ...] a pad's settings go in: the one it takes them from now, else its name's. */
static void pad_section(const PadInfo *p, char *out, int size) { snprintf(out, (size_t)size, "device %s", p->section[0] ? p->section : p->name); }

static void set_pad(const PadInfo *p, const char *key, const char *value) {
    char section[128];
    pad_section(p, section, sizeof section);
    settings_ini_set(pad_ini_path(), section, key, value);
    pad_reload();
}

static int pad_matches(const PadInfo *p, const char *what) {
    return *what && (!_stricmp(what, p->guid) || !_stricmp(what, p->name) || !_stricmp(what, p->type));
}

static void load_pins(void) {
    for (int k = 1; k <= PAD_SLOTS; k++) {
        char key[4];
        snprintf(key, sizeof key, "%d", k);
        if (!settings_ini_get(pad_ini_path(), "players", key, pins[k - 1], sizeof pins[k - 1])) pins[k - 1][0] = 0;
    }
}

/* The player controllers.ini keeps for this pad ([players]), 0 none. */
static int pad_pin(const PadInfo *p) {
    for (int k = 1; k <= PAD_SLOTS; k++)
        if (pad_matches(p, pins[k - 1])) return k;
    return 0;
}

/* ---- the pages -------------------------------------------------------------------------- */
static PadInfo pads[PAD_SLOTS];
static int npads_connected;

static const char *on_off(int v) { return v ? "on" : "off"; }

static void note_env(Row *r, const char *var) {
    const char *v = getenv(var);
    if (v && *v) snprintf(r->note, sizeof r->note, "%s set", var);
}

static const int SIZES[][2] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
static const int REMAP_ORDER[16] = {0, 1, 2, 3, 6, 7, 8, 9, 10, 11, 4, 5, 13, 15, 12, 14};   /* A B X Y L R ZL ZR + - sticks, d-pad */

/* ---- the Lobby page: the online lobby's rooms and matchmaking (net.h, netplay version 3) ----
 * Joining stays the game's own: a room this game can join is in Local Wireless > Rooms List, and
 * matchmaking's host creates a room there, which comes first in the other player's list. The page
 * says what to do, and asks the lobby for the rooms again each time it shows (and on A). */
enum { LOBBY_ROOMS = 32 };
static NetRoom lobby_rooms[LOBBY_ROOMS];
static int nlobby_rooms, rooms_state;
static NetMatch match;
static ULONGLONG match_started;          /* when the menu's search started (the time it shows) */
static int match_followed, match_said, match_said_open;   /* that search, followed while the menu is closed too */
static char helps[40][420];              /* help made for a row (help_set) */

static void help_set(Row *r, const char *fmt, ...) {
    char *h = helps[r - rows];
    va_list a;
    va_start(a, fmt);
    vsnprintf(h, sizeof helps[0], fmt, a);
    va_end(a);
    r->help = h;
}

static const char *who(const char *name) { return name[0] ? name : "the other player"; }

static const char *const PROTOCOL_HELP = "3: the online lobby's room browser and matchmaking (the Lobby page), being tried out. A "
                                         "lobby or private room of version 2 still works: the game joins it in 2. 2: as before.";

static void add_protocol_row(void) {
    Row *r = add(R_NET_PROTOCOL, K_CHOICE, W_RESTART, "Netplay version", PROTOCOL_HELP);
    snprintf(r->value, sizeof r->value, "%s", net.protocol == NET_PROTOCOL_V3 ? "3 (being tried out)" : "2");
    if (net_configured() && !net_relay_address()) snprintf(r->note, sizeof r->note, "hosting: 2");
    note_env(r, "SSBU_NET_PROTOCOL");
}

enum { LB_OFFLINE, LB_HOSTING, LB_NEEDS_V3, LB_CONNECTING, LB_PRIVATE, LB_V2_LOBBY, LB_NONE, LB_ON };
static int lobby_state(void) {
    if (!net_configured()) return LB_OFFLINE;
    if (!net_relay_address()) return LB_HOSTING;
    if (net_protocol_wanted() != NET_PROTOCOL_V3) return LB_NEEDS_V3;
    if (!net_local_ip()) return LB_CONNECTING;
    if (!net_lobby()) return LB_PRIVATE;
    if (net_protocol_spoken() != NET_PROTOCOL_V3) return LB_V2_LOBBY;
    if (rooms_state == NET_ROOMS_OFF && match.state == NET_MATCH_OFF) return LB_NONE;
    return LB_ON;
}

static void match_rows(void) {
    const NetMatch *m = &match;
    const char *other = who(m->peer_name);
    ULONGLONG now = GetTickCount64();
    int waited = match_started && now >= match_started ? (int)((now - match_started) / 1000) : m->waited_s;
    Row *r = add(R_MATCH_FIND, K_ACTION, W_NONE, "Find a match", NULL);
    switch (m->state) {
    case NET_MATCH_ASKING:
    case NET_MATCH_SEARCHING:
        if (m->state == NET_MATCH_ASKING) snprintf(r->value, sizeof r->value, "asking the lobby...");
        else snprintf(r->value, sizeof r->value, "searching... %d:%02d", waited / 60, waited % 60);
        help_set(r, "Searching for another player of your game, WindowSmash and mods, with your lobby code. You can close this menu "
                    "and play meanwhile: a message says when a match is found.");
        break;
    case NET_MATCH_MATCHED:
        if (m->role == NET_MATCH_HOST) {
            snprintf(r->value, sizeof r->value, "with %.32s: you host", other);
            help_set(r, "You host: close this menu, then Local Wireless > Create a Room (or keep yours open). %s finds it first in "
                        "their Rooms List.", other);
        } else if (m->room_open) {
            snprintf(r->value, sizeof r->value, "with %.32s: their room is open", other);
            help_set(r, "%s hosts, and their room is open: close this menu, then Local Wireless > Rooms List. Their room comes first.", other);
        } else {
            snprintf(r->value, sizeof r->value, "with %.32s: they host", other);
            help_set(r, "%s hosts: once their room is open, it comes first in Local Wireless > Rooms List. A message says when.", other);
        }
        break;
    case NET_MATCH_TIMEOUT:
        snprintf(r->value, sizeof r->value, "no opponent found in time");
        help_set(r, "Nobody of your game, WindowSmash, mods and lobby code searched in time. A: search again.");
        break;
    case NET_MATCH_LEFT:
        snprintf(r->value, sizeof r->value, "%.32s cancelled or left", other);
        help_set(r, "%s cancelled or left the match. A: search again.", other);
        break;
    case NET_MATCH_TOGETHER:
        snprintf(r->value, sizeof r->value, "in one room with %.32s", other);
        help_set(r, "You and %s are in one room: the match is made. A: search again.", other);
        break;
    case NET_MATCH_EXPIRED:
        snprintf(r->value, sizeof r->value, "expired: not in one room in time");
        help_set(r, "You and %s weren't in one room in time. A: search again.", other);
        break;
    default:
        snprintf(r->value, sizeof r->value, "not searching");
        help_set(r, "Pairs you with another player of your game, WindowSmash and mods, with your lobby code, and says who hosts. A: "
                    "search. You can close this menu meanwhile: a message says when a match is found.");
    }
    if (m->state == NET_MATCH_ASKING || m->state == NET_MATCH_SEARCHING || m->state == NET_MATCH_MATCHED) {
        r = add(R_MATCH_CANCEL, K_ACTION, W_NONE, m->state == NET_MATCH_MATCHED ? "Leave the match" : "Stop searching", NULL);
        if (m->state == NET_MATCH_MATCHED) help_set(r, "Ends the match: %s is told.", other);
        else help_set(r, "Stops the search.");
    }
}

/* Why a room can't be joined, in a few words (the help has the whole of it). */
static const char *room_short(const NetRoom *x) {
    if (x->why_kind == NET_WHY_BUILD) return "another game version";
    if (x->why_kind == NET_WHY_SIM) return x->sim > SIM_VERSION ? "update your WindowSmash" : "their WindowSmash is older";
    return "other mods";
}

static void room_rows(void) {
    int can = 0;
    for (int i = 0; i < nlobby_rooms; i++) can += lobby_rooms[i].joinable && !lobby_rooms[i].mine;
    Row *r = add(R_ROOMS, K_ACTION, W_NONE, "Rooms", "Every room of your lobby code, of any game version or mods. A: list them again. "
                                                    "The ones you can join are in Local Wireless > Rooms List too.");
    if (rooms_state == NET_ROOMS_ASKING) snprintf(r->value, sizeof r->value, "asking the lobby...");
    else if (rooms_state == NET_ROOMS_NO_ANSWER) snprintf(r->value, sizeof r->value, "no answer from the lobby (A: ask again)");
    else if (!nlobby_rooms) snprintf(r->value, sizeof r->value, "none");
    else snprintf(r->value, sizeof r->value, "%d, %d you can join", nlobby_rooms, can);
    for (int i = 0; i < nlobby_rooms; i++) {
        const NetRoom *x = &lobby_rooms[i];
        const char *host = x->name[0] ? x->name : "(no name)";
        r = add(R_ROOM_FIRST + i, K_ACTION, W_NONE, "", NULL);
        snprintf(r->label, sizeof r->label, "%.30s%s", host, x->mine ? " (you)" : "");
        if (!x->joinable) {
            snprintf(r->value, sizeof r->value, "can't join: %s", room_short(x));
            help_set(r, "%s%s", x->why, x->why_kind != NET_WHY_SIM ? "" : x->sim > SIM_VERSION ? "\nYours is the older one: update WindowSmash to join."
                                                                                               : "\nTheirs is the older one: the host needs to update.");
            continue;
        }
        snprintf(r->value, sizeof r->value, "%d/%d players, %s", x->players, x->most, x->open ? "open" : "full or closed");
        if (x->mine) help_set(r, "Your own room, %d of %d players: other players of your game find it in their Rooms List.", x->players, x->most);
        else if (x->open) help_set(r, "%s's room, %d of %d players. To join it, close this menu, then Local Wireless > Rooms List and pick %s's room.%s%s",
                                   host, x->players, x->most, host, x->nmods ? "\nMods: " : "", x->mods);
        else help_set(r, "%s's room is full or closed for now (%d of %d players). Once it opens, it is in Local Wireless > Rooms List.",
                      host, x->players, x->most);
    }
}

/* The Lobby page's rows (fresh: the page shows anew, so the rooms are asked for again). */
static void build_lobby(int fresh) {
    nlobby_rooms = net_rooms_get(lobby_rooms, LOBBY_ROOMS, &rooms_state);
    net_match_get(&match);
    if (fresh && (rooms_state == NET_ROOMS_LISTED || rooms_state == NET_ROOMS_NO_ANSWER)) net_rooms_ask();   /* (asking: once is enough) */
    int st = lobby_state();
    Row *r = add(R_LOBBY, K_INFO, W_NONE, "Lobby", NULL);
    switch (st) {
    case LB_OFFLINE:
        snprintf(r->value, sizeof r->value, "offline");
        r->help = "The online lobby's room browser and matchmaking. You're offline: choose a lobby in Online > Play online in, set "
                  "Netplay version to 3 (below), then restart the game.";
        break;
    case LB_HOSTING:
        snprintf(r->value, sizeof r->value, "hosting a private room");
        r->help = "You host a private room: its players find your room in Local Wireless > Rooms List. The room browser and "
                  "matchmaking are the online lobby's.";
        break;
    case LB_NEEDS_V3:
        snprintf(r->value, sizeof r->value, "needs netplay version 3");
        r->help = "The lobby's room browser and matchmaking need netplay version 3, which is being tried out. To turn it on, set "
                  "Netplay version to 3 (below, or on the Online page) and restart the game, or start it with SSBU_NET_PROTOCOL=3.";
        break;
    case LB_CONNECTING:
        snprintf(r->value, sizeof r->value, "%s", net_unreachable() ? "no answer from the lobby" : "connecting...");
        r->help = "Waiting for the lobby (Online > Now says more).";
        break;
    case LB_PRIVATE:
        snprintf(r->value, sizeof r->value, "in a private room");
        r->help = "You're in a host player's private room: its players find each other's rooms in Local Wireless > Rooms List. The "
                  "room browser and matchmaking are the online lobby's.";
        break;
    case LB_V2_LOBBY:
        snprintf(r->value, sizeof r->value, "this lobby speaks version 2 only");
        r->help = "This lobby speaks netplay version 2 only, so it has no room browser or matchmaking. Its rooms are in Local "
                  "Wireless > Rooms List, as before.";
        break;
    case LB_NONE:
        snprintf(r->value, sizeof r->value, "no room browser in this lobby");
        r->help = "This lobby speaks netplay version 3, but without a room browser or matchmaking.";
        break;
    default:
        if (match.players >= 0 && match.searching >= 0)
            snprintf(r->value, sizeof r->value, "%d player%s, %d searching", match.players, match.players == 1 ? "" : "s", match.searching);
        else snprintf(r->value, sizeof r->value, "version 3");
        r->help = "The players of your lobby code who can play with you (the same game, WindowSmash and mods), and how many of them "
                  "are searching for a match.";
    }
    if (st == LB_ON) {
        if (match.state != NET_MATCH_OFF) match_rows();
        if (rooms_state != NET_ROOMS_OFF) room_rows();
    } else if (st == LB_OFFLINE || st == LB_NEEDS_V3) add_protocol_row();
    if (restart_pending) add(R_RESTART, K_ACTION, W_NONE, "Restart now", "Starts the game again, with the changes. A match in progress is lost.");
}

/* A search the menu started, followed while the menu is closed too: a message when it is matched,
 * when the host's room opens, and when it ends. */
static void match_follow(void) {
    if (!match_followed) return;
    NetMatch m;
    net_match_get(&m);
    const char *other = who(m.peer_name);
    char t[160] = "";
    if (m.state != match_said) {
        switch (m.state) {
        case NET_MATCH_MATCHED:
            if (m.role == NET_MATCH_HOST) snprintf(t, sizeof t, "Matched with %.32s: you host (Local Wireless > Create a Room)", other);
            else if (m.room_open) snprintf(t, sizeof t, "Matched with %.32s: their room is open (Local Wireless > Rooms List)", other);
            else snprintf(t, sizeof t, "Matched with %.32s: they host (their room comes first in Rooms List)", other);
            break;
        case NET_MATCH_TIMEOUT: snprintf(t, sizeof t, "Matchmaking: no opponent found in time"); break;
        case NET_MATCH_LEFT: snprintf(t, sizeof t, "Matchmaking: %.32s cancelled or left", other); break;
        case NET_MATCH_TOGETHER: snprintf(t, sizeof t, "Matchmaking: you and %.32s are in one room", other); break;
        case NET_MATCH_EXPIRED: snprintf(t, sizeof t, "Matchmaking: the match expired (not in one room in time)"); break;
        }
        match_said = m.state;
    } else if (m.state == NET_MATCH_MATCHED && m.role == NET_MATCH_JOIN && m.room_open && !match_said_open)
        snprintf(t, sizeof t, "%.32s's room is open (Local Wireless > Rooms List)", other);
    if (t[0]) menu_toast(t);
    match_said_open = m.state == NET_MATCH_MATCHED && m.room_open;
    if (m.state == NET_MATCH_IDLE || (m.state >= NET_MATCH_TIMEOUT && m.state <= NET_MATCH_EXPIRED)) match_followed = 0;
}

static void build(void) {
    /* (the Lobby page: the row under the cursor, by what it is, so the cursor stays on it when the
     * rooms or the search change around it) */
    int same = built_page == page, was = same && sel[page] < nrows ? rows[sel[page]].id : R_NONE;
    u32 was_host = was >= R_ROOM_FIRST && was < R_ROOM_FIRST + nlobby_rooms ? lobby_rooms[was - R_ROOM_FIRST].host : 0;
    built_page = page;
    nrows = 0;
    Row *r;
    DisplaySettings ds;
    display_settings(&ds);
    PadInfo *pad = pad_slot >= 0 && pads[pad_slot].connected ? &pads[pad_slot] : NULL;
    switch (page) {
    case P_DISPLAY:
        r = add(R_WINDOW, K_CHOICE, W_NOW, "Window", "Windowed: a normal window. Borderless: covering the monitor. Full screen: borderless and always on top. F11 switches too.");
        snprintf(r->value, sizeof r->value, "%s", ds.mode == DISPLAY_BORDERLESS ? "borderless" : ds.mode == DISPLAY_FULLSCREEN ? "full screen" : "windowed");
        note_env(r, "SSBU_WINDOW");
        r = add(R_PACING, K_CHOICE, W_NOW, "Frame pacing",
                "Clock: a steady 60 frames a second. VRR: for G-Sync / FreeSync monitors, smoothest with borderless (tears without VRR). Display: follow a 60/120/240 Hz monitor's own refresh.");
        snprintf(r->value, sizeof r->value, "%s", ds.pacing == PACING_VRR ? "vrr" : ds.pacing == PACING_DISPLAY ? "display" : "clock");
        note_env(r, "SSBU_PACING");
        r = add(R_LATENCY, K_CHOICE, W_NOW, "Frames queued ahead", "How many frames the graphics card may work ahead: 1 is the most responsive, 3 the smoothest on a busy PC.");
        snprintf(r->value, sizeof r->value, "%d", ds.latency);
        r = add(R_CURSOR, K_CHOICE, W_NOW, "Hide the mouse pointer", "Hides the pointer over the game after 2 seconds without moving it.");
        snprintf(r->value, sizeof r->value, "%s", on_off(ds.hide_cursor));
        r = add(R_OVERLAY, K_CHOICE, W_NOW, "Frame rate in the corner", "The frame rate and a graph of frame times (F1 shows or hides it while playing; F2 adds details).");
        snprintf(r->value, sizeof r->value, "%s", on_off(overlay_on ? *overlay_on : ds.overlay));
        note_env(r, "SSBU_OVERLAY");
        r = add(R_SIZE, K_CHOICE, W_RESTART, "Window size", "The size of the game's window when it is not full screen (at 100% Windows scaling).");
        snprintf(r->value, sizeof r->value, "%d x %d", ds.width, ds.height);
        note_env(r, "SSBU_WINDOW_SIZE");
        r = add(R_SCALE, K_CHOICE, W_RESTART, "Render scale",
                "How many times 1920x1080 the game draws at: sharper on a big screen, more work for the graphics card. Auto: as many pixels as the window has.");
        if (ds.render_scale <= 0) snprintf(r->value, sizeof r->value, "auto");
        else snprintf(r->value, sizeof r->value, "%g", ds.render_scale);
        note_env(r, "SSBU_RENDER_SCALE");
#ifdef _WIN32
        r = add(R_GPU, K_CHOICE, W_RESTART, "Graphics card", "Auto picks the fastest card (on a laptop, the NVIDIA or AMD one). Pick another to use it instead.");
        snprintf(r->value, sizeof r->value, "%s", ds.gpu);
        note_env(r, "SSBU_GPU");
#endif
        break;
    case P_PADS:
        if (!npads_connected) {
            r = add(R_NONE, K_INFO, W_NONE, "Controllers", "Plug in or pair a controller: it shows here as it connects. The keyboard works too (see below).");
            snprintf(r->value, sizeof r->value, "none connected");
        } else {
            r = add(R_PAD, K_CHOICE, W_NONE, "Controller", "Which controller to set up (left / right to pick another).");
            if (pad && pad->slot >= 0) snprintf(r->value, sizeof r->value, "player %d: %.80s", pad->slot + 1, pad->name);
            else if (pad) snprintf(r->value, sizeof r->value, "no player: %.80s", pad->name);
            if (pad) {
                int pin = pad_pin(pad);
                r = add(R_PAD_PLAYER, K_CHOICE, W_NOW, "Player", "Auto: the lowest free player. A number keeps that player for this controller, every time it connects.");
                if (pin) snprintf(r->value, sizeof r->value, "%d (kept)", pin);
                else snprintf(r->value, sizeof r->value, "auto (now %d)", pad->slot + 1);
                r = add(R_PAD_LAYOUT, K_CHOICE, W_NOW, "Face buttons",
                        "By position: as on a Switch pad, the right button is A. By label: the letter printed on the pad (an Xbox pad's A is A; PlayStation pads stay by position).");
                snprintf(r->value, sizeof r->value, "%s", pad->layout ? "by label" : "by position");
                r = add(R_PAD_DEADZONE, K_CHOICE, W_NOW, "Stick dead zone", "How much of the sticks' travel around the centre is ignored: raise it if a worn stick drifts.");
                snprintf(r->value, sizeof r->value, "%d%%", pad->deadzone);
                r = add(R_PAD_RUMBLE, K_CHOICE, W_NOW, "Rumble", "The strength of the rumble; 0 is off.");
                snprintf(r->value, sizeof r->value, "%d%%", pad->rumble);
                r = add(R_PAD_TRIGGER, K_CHOICE, W_NOW, "Trigger press point", "How far a trigger is pulled before it counts as ZL / ZR.");
                snprintf(r->value, sizeof r->value, "%d%%", pad->trigger);
                add(R_PAD_REMAP, K_ACTION, W_NOW, "Buttons...", "Which control on this pad is each of the game's buttons.");
                add(R_PAD_TEST, K_ACTION, W_NONE, "Test rumble", "Rumbles this pad for half a second.");
                add(R_PAD_RESET, K_ACTION, W_NOW, "Back to the defaults", "This pad's buttons, dead zone, rumble and trigger as they were.");
                char section[128];
                pad_section(pad, section, sizeof section);
                for (int i = 0; i < nrows; i++)
                    if (rows[i].id >= R_PAD_LAYOUT && rows[i].id <= R_PAD_RESET && rows[i].id != R_PAD_TEST)
                        snprintf(rows[i].note, sizeof rows[i].note, "[%.40s]", section);
            }
        }
        r = add(R_RUMBLE, K_CHOICE, W_NOW, "Rumble", "On: controllers rumble, each as strong as its own Rumble setting. Off: no controller rumbles.");
        snprintf(r->value, sizeof r->value, "%s", on_off(pad_rumble_on()));
        r = add(R_KEYBOARD, K_CHOICE, W_NOW, "Keyboard plays as",
                "The keyboard adds to this player: arrows move, Z X C V are A B X Y, A S are L R, Q W are ZL ZR, Enter is +, I J K L the right stick.");
        if (pad_keyboard_slot() < 0) snprintf(r->value, sizeof r->value, "nobody");
        else snprintf(r->value, sizeof r->value, "player %d", pad_keyboard_slot() + 1);
        r = add(R_BACKGROUND, K_CHOICE, W_NOW, "Pads when not focused", "On: controllers work while another window is in front. Off: only while the game's window is.");
        snprintf(r->value, sizeof r->value, "%s", on_off(pad_background()));
        r = add(R_PADS_FILE, K_ACTION, W_NONE, "Open controllers.ini", "Everything else, by hand: settings per kind of pad, more remaps. Changes there count at the next start.");
        snprintf(r->value, sizeof r->value, "%s", pad_ini_path());
        break;
    case P_REMAP:
        r = add(R_REMAP_BACK, K_ACTION, W_NONE, "< Back", "Back to the controller's settings.");
        if (pad) snprintf(r->value, sizeof r->value, "%.80s", pad->name);
        for (int i = 0; pad && i < 16; i++) {
            int b = REMAP_ORDER[i];
            char label[24];
            snprintf(label, sizeof label, "%s", pad_button_name(b));
            r = add(R_REMAP_FIRST + b, K_ACTION, W_NOW, label, "A or Enter: then press the control for this button. Y or Delete: its default again.");
            snprintf(r->value, sizeof r->value, "%s", pad->map[b]);
        }
        break;
    case P_ONLINE: {
        const char *launcher = getenv("SSBU_NET_RELAY") && *getenv("SSBU_NET_RELAY") ? "SSBU_NET_RELAY set" :
                               getenv("SSBU_NET_HOST") && *getenv("SSBU_NET_HOST") ? "hosting (launcher)" : NULL;
        r = add(R_NET_STATUS, K_INFO, W_NONE, "Now", "What the game is connected to.");
        char status[512];
        net_status(status, sizeof status);
        char *nl = strchr(status, '\n');
        if (nl) *nl = 0;
        snprintf(r->value, sizeof r->value, "%.110s", status[0] ? status : net_configured() ? "connecting..." : "offline");
        r = add(R_NET_MODE, K_CHOICE, W_RESTART, "Play online in",
                "Public lobby: everyone's rooms in Local Wireless. Private: only players with the same code. Your own server: host:port. Offline: no netplay.");
        snprintf(r->value, sizeof r->value, "%s", NET_MODES[net.mode]);
        if (net.mode == NET_PRIVATE || net.mode == NET_CUSTOM) {
            r = add(R_NET_CODE, K_TEXT, W_RESTART, "Lobby code", "Players with the same code see each other's rooms (letters and digits, no spaces).");
            snprintf(r->value, sizeof r->value, "%s", net.code[0] ? net.code : (net.mode == NET_PRIVATE ? "friends" : "(none)"));
        }
        if (net.mode == NET_CUSTOM) {
            r = add(R_NET_SERVER, K_TEXT, W_RESTART, "Server", "The lobby server's address and port, e.g. 192.168.1.20:39420.");
            snprintf(r->value, sizeof r->value, "%s", net.custom[0] ? net.custom : "(none)");
        }
        r = add(R_NET_NAME, K_TEXT, W_RESTART, "Your name", "What other players see (up to 32 letters).");
        snprintf(r->value, sizeof r->value, "%s", net.name[0] ? net.name : "(picked at the first online start)");
        r = add(R_NET_DIRECT, K_CHOICE, W_RESTART, "Direct connections",
                "On: rooms connect players directly when they can (less lag). Off: everything goes through the lobby, so other players never learn your internet address.");
        snprintf(r->value, sizeof r->value, "%s", on_off(net.direct));
        for (int i = 1; i < nrows; i++) {
            if (launcher) snprintf(rows[i].note, sizeof rows[i].note, "%s", launcher);
            else if (!settings_online()) snprintf(rows[i].note, sizeof rows[i].note, "for Play SSBU");
        }
        add_protocol_row();   /* (read with any lobby: no launcher's note) */
        if (restart_pending) add(R_RESTART, K_ACTION, W_NONE, "Restart now", "Starts the game again, with the changes. A match in progress is lost.");
        break;
    }
    case P_LOBBY:
        build_lobby(!same);
        if (was == R_NONE) break;
        for (int i = 0; i < nrows; i++)
            if (was_host ? rows[i].id >= R_ROOM_FIRST && lobby_rooms[rows[i].id - R_ROOM_FIRST].host == was_host : rows[i].id == was) {
                sel[page] = i;
                break;
            }
        break;
    case P_GAME:
        add(R_RESTART, K_ACTION, W_NONE, "Restart the game", "Starts the game again (for the settings marked 'next start'). A match in progress is lost.");
        add(R_QUIT, K_ACTION, W_NONE, "Quit the game", "Closes the game. A match in progress is lost.");
        static const char *const KEYS[][2] = {{"Esc / Home button", "this menu (or hold - and + for a second)"}, {"F1", "frame rate on / off"},
                                               {"F2", "details"}, {"F3", "screenshot (screenshots folder)"}, {"F11, Alt+Enter", "full screen"}};
        for (u32 i = 0; i < sizeof KEYS / sizeof *KEYS; i++) {
            r = add(R_NONE, K_INFO, W_NONE, KEYS[i][0], "Keys that work while playing.");
            snprintf(r->value, sizeof r->value, "%s", KEYS[i][1]);
        }
        r = add(R_NONE, K_INFO, W_NONE, "Version", "The game version this folder has (version.txt).");
        snprintf(r->value, sizeof r->value, "%s", version[0] ? version : "unknown (no version.txt)");
        break;
    }
}

static int selectable(const Row *r) { return r->kind != K_INFO; }

/* ---- changing a row ----------------------------------------------------------------------- */
static int cycle(int v, int n, int dir) { return ((v + dir) % n + n) % n; }

static void start_edit(int id) {
    editing = id;
    grid_r = grid_c = 0;
    const char *v = id == R_NET_CODE ? net.code : id == R_NET_SERVER ? net.custom : net.name;
    snprintf(edit_buf, sizeof edit_buf, "%s", v);
}

static void finish_edit(void) {
    char *s = edit_buf;
    while (*s == ' ') s++;
    size_t n = strlen(s);
    while (n && s[n - 1] == ' ') s[--n] = 0;
    if (editing == R_NET_NAME) {
        if (*s) { snprintf(net.name, sizeof net.name, "%.32s", s); settings_line_write(config_path(CONFIG_ONLINE_NAME), net.name); changed(W_RESTART); }
    } else if (!strchr(s, ' ')) {
        if (editing == R_NET_CODE) snprintf(net.code, sizeof net.code, "%s", s);
        else if (editing == R_NET_SERVER && *s) snprintf(net.custom, sizeof net.custom, "%s", s);
        net_save_server();
        changed(W_RESTART);
    } else menu_toast("No spaces in a lobby code or a server's address");
    editing = 0;
}

static void change(const Row *row, int dir) {
    DisplaySettings ds;
    display_settings(&ds);
    PadInfo *pad = pad_slot >= 0 && pads[pad_slot].connected ? &pads[pad_slot] : NULL;
    char v[64];
    switch (row->id) {
    case R_WINDOW: {
        int m = cycle(ds.mode, 3, dir);
        display_set_mode(m);
        set_display("window", m == DISPLAY_BORDERLESS ? "borderless" : m == DISPLAY_FULLSCREEN ? "fullscreen" : "windowed");
        break;
    }
    case R_PACING: {
        Pacing p = (Pacing)cycle(ds.pacing, 3, dir);
        display_set_pacing(p);
        set_display("pacing", p == PACING_VRR ? "vrr" : p == PACING_DISPLAY ? "display" : "clock");
        break;
    }
    case R_LATENCY:
        snprintf(v, sizeof v, "%d", cycle(ds.latency - 1, 3, dir) + 1);
        display_set_latency(atoi(v));
        set_display("latency", v);
        break;
    case R_CURSOR:
        display_set_hide_cursor(!ds.hide_cursor);
        set_display("hide_cursor", ds.hide_cursor ? "0" : "1");
        break;
    case R_OVERLAY:
        if (overlay_on) *overlay_on = !*overlay_on;
        set_display("overlay", overlay_on && *overlay_on ? "1" : "0");
        break;
    case R_SIZE: {
        int n = sizeof SIZES / sizeof *SIZES, i = -1;
        for (int k = 0; k < n; k++) if (SIZES[k][0] == ds.width && SIZES[k][1] == ds.height) i = k;
        if (i >= 0) i = cycle(i, n, dir);
        else {   /* (a size typed by hand: the next one up or down) */
            i = 0;
            while (i < n && SIZES[i][0] * SIZES[i][1] < ds.width * ds.height) i++;
            i = cycle(dir < 0 ? i - 1 : i, n, 0);
        }
        snprintf(v, sizeof v, "%d", SIZES[i][0]);
        set_display("width", v);
        snprintf(v, sizeof v, "%d", SIZES[i][1]);
        set_display("height", v);
        break;
    }
    case R_SCALE: {
        static const char *const SCALES[] = {"1", "auto", "1.5", "2", "3"};
        static const float VALUES[] = {1, 0, 1.5f, 2, 3};
        int n = sizeof SCALES / sizeof *SCALES, i = -1;
        for (int k = 0; k < n; k++) if (VALUES[k] == ds.render_scale) i = k;
        i = i < 0 ? (dir > 0 ? 0 : n - 1) : cycle(i, n, dir);
        set_display("render_scale", SCALES[i]);
        display_set_render_scale(VALUES[i]);   /* (shown now; the renderer takes it at the next start) */
        break;
    }
#ifdef _WIN32
    case R_GPU: {
        int i = -1;   /* auto, then the cards */
        for (int k = 0; k < ngpus; k++) if (!_stricmp(ds.gpu, gpus[k])) i = k;
        if (i < 0 && _stricmp(ds.gpu, "auto")) i = ngpus;   /* (something typed by hand: next is auto) */
        i = cycle(i + 1, ngpus + 1, dir) - 1;
        set_display("gpu", i < 0 ? "auto" : gpus[i]);
        break;
    }
#endif
    case R_PAD: {
        int start = pad_slot < 0 ? 0 : pad_slot;
        for (int k = 1; k <= PAD_SLOTS; k++) {
            int s = cycle(start, PAD_SLOTS, dir * k);
            if (pads[s].connected) { pad_slot = s; break; }
        }
        return;
    }
    case R_PAD_PLAYER: {
        if (!pad) return;
        int pin = cycle(pad_pin(pad), PAD_SLOTS + 1, dir);
        for (int k = 1; k <= PAD_SLOTS; k++) {   /* any old pin of this pad goes */
            snprintf(v, sizeof v, "%d", k);
            if (pad_matches(pad, pins[k - 1])) settings_ini_set(pad_ini_path(), "players", v, NULL);
        }
        if (pin) {
            snprintf(v, sizeof v, "%d", pin);
            settings_ini_set(pad_ini_path(), "players", v, pad->guid);
        }
        load_pins();
        pad_reload();
        break;
    }
    case R_PAD_LAYOUT: if (pad) set_pad(pad, "layout", pad->layout ? "positional" : "labels"); break;
    case R_PAD_DEADZONE:
        if (pad) { snprintf(v, sizeof v, "%d", cycle(pad->deadzone / 5, 11, dir) * 5); set_pad(pad, "deadzone", v); }
        break;
    case R_PAD_RUMBLE:
        if (pad) { snprintf(v, sizeof v, "%d", cycle(pad->rumble / 10, 11, dir) * 10); set_pad(pad, "rumble", v); }
        break;
    case R_PAD_TRIGGER:
        if (pad) { snprintf(v, sizeof v, "%d", cycle(pad->trigger / 5 - 1, 19, dir) * 5 + 5); set_pad(pad, "trigger_threshold", v); }
        break;
    case R_KEYBOARD:
        snprintf(v, sizeof v, "%d", cycle(pad_keyboard_slot() + 1, PAD_SLOTS + 1, dir));
        settings_ini_set(pad_ini_path(), "general", "keyboard_player", v);
        pad_reload();
        break;
    case R_BACKGROUND:
        settings_ini_set(pad_ini_path(), "general", "background", pad_background() ? "0" : "1");
        pad_reload();
        break;
    case R_RUMBLE:
        settings_ini_set(pad_ini_path(), "general", "rumble", pad_rumble_on() ? "0" : "1");
        pad_reload();
        break;
    case R_NET_MODE:
        net.mode = cycle(net.mode, 4, dir);
        net_save_server();
        break;
    case R_NET_DIRECT:
        net.direct = !net.direct;
        settings_line_write(config_path(CONFIG_ONLINE_DIRECT), net.direct ? "on" : "off");
        break;
    case R_NET_PROTOCOL:
        net.protocol = net.protocol == NET_PROTOCOL_V3 ? NET_PROTOCOL : NET_PROTOCOL_V3;
        settings_line_write(config_path(CONFIG_ONLINE_PROTOCOL), net.protocol == NET_PROTOCOL_V3 ? "3" : "2");
        break;
    default:
        return;
    }
    changed(row->when);
}

static void action(const Row *row) {
    PadInfo *pad = pad_slot >= 0 && pads[pad_slot].connected ? &pads[pad_slot] : NULL;
    switch (row->kind) {
    case K_CHOICE: change(row, 1); return;
    case K_TEXT: start_edit(row->id); return;
    }
    if (row->id >= R_REMAP_FIRST && pad) {
        capture_button = row->id - R_REMAP_FIRST;
        capture_held = ~0u;   /* (whatever is held now must go first) */
        capture_until = GetTickCount64() + 6000;
        return;
    }
    if (row->id >= R_ROOM_FIRST && row->id < R_ROOM_FIRST + nlobby_rooms) {   /* a room: what to do, in the game's own menus */
        const NetRoom *x = &lobby_rooms[row->id - R_ROOM_FIRST];
        const char *host = x->name[0] ? x->name : "(no name)";
        char t[160];
        if (x->mine) snprintf(t, sizeof t, "This is your own room");
        else if (!x->joinable) snprintf(t, sizeof t, "Can't join %.32s's room: %s", host, room_short(x));
        else if (!x->open) snprintf(t, sizeof t, "%.32s's room is full or closed for now", host);
        else snprintf(t, sizeof t, "Close this menu, then Local Wireless > Rooms List: %.32s's room", host);
        menu_toast(t);
        return;
    }
    switch (row->id) {
    case R_MATCH_FIND:
        if (match.state == NET_MATCH_ASKING || match.state == NET_MATCH_SEARCHING) menu_toast("Searching already");
        else if (match.state == NET_MATCH_MATCHED) {
            char t[160];
            snprintf(t, sizeof t, match.role == NET_MATCH_HOST ? "You host: Local Wireless > Create a Room (%.32s joins it)"
                                                                : "%.32s hosts: their room comes first in Local Wireless > Rooms List", who(match.peer_name));
            menu_toast(t);
        } else {
            net_match_find();
            match_started = GetTickCount64();
            match_followed = 1;
            match_said = NET_MATCH_ASKING;
            match_said_open = 0;
        }
        break;
    case R_MATCH_CANCEL:
        menu_toast(match.state == NET_MATCH_MATCHED ? "You left the match" : "Search stopped");
        net_match_cancel();
        break;
    case R_ROOMS: net_rooms_ask(); break;
    case R_PAD_REMAP: if (pad) { page = P_REMAP; sel[P_REMAP] = 1; top[P_REMAP] = 0; } break;
    case R_REMAP_BACK: page = P_PADS; break;
    case R_PAD_TEST:
        if (pad && pad->slot >= 0) {
            pad_rumble((u32)pad->slot, 1, 0.8f);
            pad_rumble((u32)pad->slot, 2, 0.8f);
            rumble_slot = pad->slot;
            rumble_stop = GetTickCount64() + 500;
        }
        break;
    case R_PAD_RESET:
        if (pad) {
            static const char *const KEYS[] = {"layout", "deadzone", "rumble", "trigger_threshold"};
            char section[128];
            pad_section(pad, section, sizeof section);
            for (u32 i = 0; i < sizeof KEYS / sizeof *KEYS; i++) settings_ini_set(pad_ini_path(), section, KEYS[i], NULL);
            for (int b = 0; b < 16; b++) settings_ini_set(pad_ini_path(), section, pad_button_name(b), NULL);
            pad_reload();
            menu_toast("This controller is back to the defaults");
        }
        break;
    case R_PADS_FILE: {
#ifdef _WIN32
        char cmd[MAX_PATH + 32];
        snprintf(cmd, sizeof cmd, "notepad.exe \"%s\"", pad_ini_path());
        STARTUPINFOA si = {sizeof si};
        PROCESS_INFORMATION pi;
        if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
#else
        char cmd[MAX_PATH + 64];
        snprintf(cmd, sizeof cmd, "xdg-open '%s' >/dev/null 2>&1 &", pad_ini_path());
        if (system(cmd)) {}
#endif
        menu_toast("controllers.ini opened in another window (changes there count at the next start)");
        break;
    }
    case R_RESTART:
    case R_QUIT:
        confirm = row->id;
        break;
    }
}

/* ---- pads and keys -------------------------------------------------------------------- */
static u64 nav_held;
static int nav_wait_release;             /* after a control was taken for a button: until everything is let go */
static ULONGLONG nav_next;
static ULONGLONG combo_since;
static int combo_armed = 1;
static u32 guide_held;

enum { NB_UP = 1, NB_DOWN = 2, NB_LEFT = 4, NB_RIGHT = 8, NB_A = 16, NB_B = 32, NB_L = 64, NB_R = 128, NB_Y = 256, NB_PLUS = 512 };

/* Every pad's buttons as the menu's: the d-pad or left stick, A, B, L/ZL, R/ZR, Y, +. */
static u64 pad_nav(int *combo, u32 *guide) {
    u64 nb = 0;
    *combo = 0;
    *guide = 0;
    for (u32 s = 0; s < PAD_SLOTS; s++) {
        PadState p;
        if (!pad_get(s, &p)) continue;
        u64 b = p.buttons;
        if (b & 1ull << 13 || p.ly > 16384) nb |= NB_UP;
        if (b & 1ull << 15 || p.ly < -16384) nb |= NB_DOWN;
        if (b & 1ull << 12 || p.lx < -16384) nb |= NB_LEFT;
        if (b & 1ull << 14 || p.lx > 16384) nb |= NB_RIGHT;
        if (b & 1ull << 0) nb |= NB_A;
        if (b & 1ull << 1) nb |= NB_B;
        if (b & (1ull << 6 | 1ull << 8)) nb |= NB_L;
        if (b & (1ull << 7 | 1ull << 9)) nb |= NB_R;
        if (b & 1ull << 3) nb |= NB_Y;
        if (b & 1ull << 10) nb |= NB_PLUS;
        if ((b & 1ull << 10) && (b & 1ull << 11)) *combo = 1;
        if (pads[s].connected && pads[s].raw & 1u << PAD_RAW_GUIDE_BIT) *guide = 1;
    }
    return nb;
}

static int keys_held(void) {
    static const int VK[] = {VK_RETURN, VK_ESCAPE, VK_BACK, VK_SPACE, VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, 'Z', 'X', 'C', 'V'};
    for (u32 i = 0; i < sizeof VK / sizeof *VK; i++)
        if (GetAsyncKeyState(VK[i]) & 0x8000) return 1;
    return 0;
}

/* ---- the text grid (for pads) ------------------------------------------------------------- */
static const char *const GRID[] = {"ABCDEFGHIJKLM", "NOPQRSTUVWXYZ", "abcdefghijklm", "nopqrstuvwxyz", "0123456789-_.", ": "};
enum { GRID_ROWS = 6, GRID_COLS = 13 };
static const char *const GRID_KEYS[] = {"space", "delete", "done", "cancel"};   /* the last row, after ':' */

static int grid_cols(int r) { return r == GRID_ROWS - 1 ? 1 + 4 : GRID_COLS; }

static void edit_event(int ev, unsigned ch) {
    size_t n = strlen(edit_buf);
    size_t max = editing == R_NET_NAME ? 32 : editing == R_NET_SERVER ? 60 : 32;
    switch (ev) {
    case EV_CHAR: if (n < max) { edit_buf[n] = (char)ch; edit_buf[n + 1] = 0; } break;
    case EV_BKSP: case EV_B: case EV_DEL: if (n) edit_buf[n - 1] = 0; break;
    case EV_ENTER: case EV_DONE: finish_edit(); break;
    case EV_ESC: case EV_PREV: editing = 0; break;
    case EV_UP: grid_r = cycle(grid_r, GRID_ROWS, -1); if (grid_c >= grid_cols(grid_r)) grid_c = grid_cols(grid_r) - 1; break;
    case EV_DOWN: grid_r = cycle(grid_r, GRID_ROWS, 1); if (grid_c >= grid_cols(grid_r)) grid_c = grid_cols(grid_r) - 1; break;
    case EV_LEFT: grid_c = cycle(grid_c, grid_cols(grid_r), -1); break;
    case EV_RIGHT: grid_c = cycle(grid_c, grid_cols(grid_r), 1); break;
    case EV_A:
        if (grid_r < GRID_ROWS - 1) edit_event(EV_CHAR, (unsigned char)GRID[grid_r][grid_c]);
        else if (grid_c == 0) edit_event(EV_CHAR, ':');
        else if (grid_c == 1) edit_event(EV_CHAR, ' ');
        else if (grid_c == 2) edit_event(EV_DEL, 0);
        else if (grid_c == 3) finish_edit();
        else editing = 0;
        break;
    }
}

static void move_sel(int dir) {
    int *s = &sel[page];
    for (int k = 0; k < nrows; k++) {
        int i = cycle(*s, nrows, dir * (k + 1));
        if (selectable(&rows[i])) { *s = i; return; }
    }
}

static void event(int ev, unsigned ch) {
    if (confirm) {
        if (ev == EV_A || ev == EV_ENTER) {
            int what = confirm;
            confirm = 0;
            if (what == R_RESTART) settings_restart();
            else { fprintf(stderr, "[menu] quit\n"); gfx_closing(); fflush(stderr); ExitProcess(0); }
        } else if (ev == EV_B || ev == EV_BKSP || ev == EV_ESC) confirm = 0;
        return;
    }
    if (editing) { edit_event(ev, ch); return; }
    if (capture_button >= 0) { if (ev == EV_ESC) capture_button = -1; return; }
    Row *row = sel[page] < nrows ? &rows[sel[page]] : NULL;
    switch (ev) {
    case EV_UP: move_sel(-1); break;
    case EV_DOWN: move_sel(1); break;
    case EV_LEFT: case EV_RIGHT: if (row && row->kind == K_CHOICE) change(row, ev == EV_RIGHT ? 1 : -1); break;
    case EV_A: case EV_ENTER: if (row && selectable(row)) action(row); break;
    case EV_DEL:
        if (page == P_REMAP && row && row->id >= R_REMAP_FIRST && pad_slot >= 0 && pads[pad_slot].connected) {
            char section[128];
            pad_section(&pads[pad_slot], section, sizeof section);
            settings_ini_set(pad_ini_path(), section, pad_button_name(row->id - R_REMAP_FIRST), NULL);
            pad_reload();
        }
        break;
    case EV_B: case EV_BKSP:
        if (page == P_REMAP) page = P_PADS;
        else close_menu();
        break;
    case EV_ESC: close_menu(); break;
    case EV_NEXT: case EV_PREV:
        page = page == P_REMAP ? P_PADS : cycle(page, PAGES, ev == EV_NEXT ? 1 : -1);
        break;
    case EV_CHAR:
        if (ch >= '1' && ch < '1' + PAGES) page = (int)(ch - '1');
        else if (ch == ' ' && row && selectable(row)) action(row);
        break;
    }
}

/* SSBU_MENU_AT / SSBU_MENU_KEYS (tests) */
static int script_events(void) {
    static struct { double at; int ev; char text[48]; int done; } steps[64];
    static int nsteps = -1, opened;
    static double open_at = -1;
    int shot = 0;
    if (nsteps < 0) {
        nsteps = 0;
        const char *a = getenv("SSBU_MENU_AT");
        open_at = a && *a ? atof(a) : -1;
        const char *s = getenv("SSBU_MENU_KEYS");
        static const struct { const char *name; int ev; } NAMES[] = {{"UP", EV_UP}, {"DOWN", EV_DOWN}, {"LEFT", EV_LEFT}, {"RIGHT", EV_RIGHT},
            {"ENTER", EV_ENTER}, {"ESC", EV_ESC}, {"TAB", EV_NEXT}, {"BKSP", EV_BKSP}, {"DEL", EV_DEL}, {"SHOT", -1}};
        while (s && *s && nsteps < 64) {
            char tok[64];
            int n = 0;
            if (sscanf(s, "%lf:%63[^,]%n", &steps[nsteps].at, tok, &n) < 2) break;
            s += n;
            if (*s == ',') s++;
            if (tok[0] == '=') { steps[nsteps].ev = EV_CHAR; snprintf(steps[nsteps].text, sizeof steps[nsteps].text, "%s", tok + 1); nsteps++; continue; }
            for (u32 i = 0; i < sizeof NAMES / sizeof *NAMES; i++)
                if (!strcmp(tok, NAMES[i].name)) { steps[nsteps++].ev = NAMES[i].ev; break; }
        }
    }
    double now = rt_now_ns() / 1e9;
    if (open_at >= 0 && !opened && now >= open_at) { opened = 1; open_menu(); }
    for (int i = 0; i < nsteps; i++) {
        if (steps[i].done || now < steps[i].at) continue;
        steps[i].done = 1;
        if (steps[i].ev < 0) shot = 1;
        else if (steps[i].ev == EV_CHAR) for (const char *c = steps[i].text; *c; c++) event(EV_CHAR, (unsigned char)*c);
        else if (steps[i].ev == EV_ESC && !open_) open_menu();
        else event(steps[i].ev, 0);
    }
    return shot;
}

int menu_frame(int *overlay, int f3) {
    ULONGLONG now = GetTickCount64();
    overlay_on = overlay;
    for (u32 s = 0; s < PAD_SLOTS; s++) pad_info(s, &pads[s]);
    npads_connected = 0;
    for (u32 s = 0; s < PAD_SLOTS; s++) npads_connected += pads[s].connected;
    if ((pad_slot < 0 || !pads[pad_slot].connected) && npads_connected)
        for (int s = 0; s < PAD_SLOTS; s++) if (pads[s].connected) { pad_slot = s; break; }
    if (rumble_slot >= 0 && now >= rumble_stop) {
        pad_rumble((u32)rumble_slot, 1, 0);
        pad_rumble((u32)rumble_slot, 2, 0);
        rumble_slot = -1;
    }
    /* opening and closing: Esc (the queue), the Home button, or - and + held for a second */
    int combo;
    u32 guide;
    u64 nb = pad_nav(&combo, &guide);
    int focused = gfx_window_focused();
    int toggle = guide && !guide_held && (focused || open_);
    guide_held = guide;
    if (combo && combo_armed && (focused || open_)) {
        if (!combo_since) combo_since = now;
        if (now - combo_since >= 1000) { toggle = 1; combo_armed = 0; }
    } else if (!combo) { combo_since = 0; combo_armed = 1; }
    int shot = script_events() || (f3 && open_);
    if (open_) build();
    while (q_tail != q_head) {
        LONG v = q[q_tail & 63];
        InterlockedIncrement(&q_tail);
        if (v & 0x10000) { if (open_) event(EV_CHAR, (unsigned)(v & 0xFF)); continue; }
        static const int EV_OF[] = {0, EV_ESC, EV_UP, EV_DOWN, EV_LEFT, EV_RIGHT, EV_ENTER, EV_BKSP, EV_NEXT, EV_PREV, EV_DEL};
        if (v == MENU_KEY_ESC && !open_) { open_menu(); continue; }
        if (open_ && v > 0 && v < (LONG)(sizeof EV_OF / sizeof *EV_OF)) event(EV_OF[v], 0);
        if (open_) build();
    }
    if (toggle && !open_) open_menu();
    else if (toggle) close_menu();
    if (toggle) {
        nav_held = nb;   /* (what opened it is not also a press in it) */
        if (open_) build();
    }
    if (open_ && capture_button >= 0) {   /* a new control for a button: the first one pressed after all are let go */
        PadInfo *pad = pad_slot >= 0 && pads[pad_slot].connected ? &pads[pad_slot] : NULL;
        u32 raw = pad ? pad->raw : 0;
        if (!pad || now > capture_until) capture_button = -1;
        else if (capture_held == ~0u) { if (!raw) capture_held = 0; }
        else if (raw) {
            int bit = 0;
            while (!(raw >> bit & 1)) bit++;
            char section[128];
            pad_section(pad, section, sizeof section);
            settings_ini_set(pad_ini_path(), section, pad_button_name(capture_button), pad_raw_name(bit));
            pad_reload();
            capture_button = -1;
            nav_wait_release = 1;
        }
        nav_held = nb;
    } else if (open_ && nav_wait_release) {
        if (!nb) nav_wait_release = 0;
        nav_held = nb;
    } else if (open_) {
        const u64 DIRS = NB_UP | NB_DOWN | NB_LEFT | NB_RIGHT;
        u64 pressed = nb & ~nav_held, dirs = nb & DIRS;
        if (pressed & DIRS) nav_next = now + 400;   /* held: repeats after 0.4 s, every 80 ms */
        else if (dirs && now >= nav_next) { pressed |= dirs; nav_next = now + 80; }
        nav_held = nb;
        static const struct { u64 bit; int ev; } MAP[] = {{NB_UP, EV_UP}, {NB_DOWN, EV_DOWN}, {NB_LEFT, EV_LEFT}, {NB_RIGHT, EV_RIGHT}, {NB_A, EV_A},
                                                         {NB_B, EV_B}, {NB_L, EV_PREV}, {NB_R, EV_NEXT}, {NB_Y, EV_DEL}, {NB_PLUS, EV_DONE}};
        for (u32 i = 0; i < sizeof MAP / sizeof *MAP && open_; i++)
            if (pressed & MAP[i].bit) { event(MAP[i].ev, 0); build(); }
    }
    if (!open_) nav_held = nb;
    if (releasing && !open_ && !nb && !keys_held()) releasing = 0;
    match_follow();
    if (open_) {
        build();
        if (sel[page] >= nrows) sel[page] = nrows - 1;
        if (sel[page] < 0) sel[page] = 0;
        if (nrows && !selectable(&rows[sel[page]])) move_sel(1);
    }
    return shot ? MENU_SHOT : 0;
}

/* ---- drawing ------------------------------------------------------------------------- */
static float text_w(const char *s, float size) { return (float)strlen(s) * size / 2; }

/* s wrapped to lines of at most `cols` characters, into out (a line s starts itself wrapped on its own). */
static void wrap(const char *s, int cols, char *out, size_t size) {
    size_t o = 0;
    while (*s && o + 2 < size) {
        int n = (int)strcspn(s, "\n");
        if (n <= cols) {
            o += (size_t)snprintf(out + o, size - o, "%.*s%s", n, s, s[n] ? "\n" : "");
            s += n + (s[n] != 0);
            continue;
        }
        int cut = cols;
        while (cut > 0 && s[cut] != ' ') cut--;
        if (!cut) cut = cols;
        o += (size_t)snprintf(out + o, size - o, "%.*s\n", cut, s);
        s += cut;
        while (*s == ' ') s++;
    }
    out[o < size ? o : size - 1] = 0;
}

static void box(float x, float y, float w, float h, float s) {
    ovl_rect(x, y, w, h, OVL_RGBA(14, 16, 24, 245));
    ovl_rect(x, y, w, 3 * s, OVL_RGBA(225, 60, 60, 255));
}

/* The message showing now (its time starting at the first look), or 0. */
static int toast_now(char *t, size_t size) {
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&toast_lock);
    if (toast_until == 1) toast_until = now + 4000;
    int show = toast_until && now < toast_until;
    snprintf(t, size, "%s", toast);
    ReleaseSRWLockExclusive(&toast_lock);
    return show;
}

/* A question or an editor over the menu: the menu dimmed, the box lighter and solid. */
static void modal_box(float w, float h, float x, float y, float bw, float bh, float s) {
    ovl_rect(0, 0, w, h, OVL_RGBA(0, 0, 0, 120));
    ovl_rect(x, y, bw, bh, OVL_RGBA(30, 34, 48, 255));
    ovl_rect(x, y, bw, 3 * s, OVL_RGBA(225, 60, 60, 255));
}

static void draw_toast(float w, float h, float s) {
    char t[160];
    if (!toast_now(t, sizeof t)) return;
    float size = 28 * s, tw = text_w(t, size) + 48 * s, th = size + 28 * s;
    float x = (w - tw) / 2, y = h - th - 40 * s;
    box(x, y, tw, th, s);
    ovl_text(x + 24 * s, y + 14 * s, size, OVL_RGBA(240, 240, 240, 255), t);
}

void menu_draw(float w, float h) {
    float s = h / 1080.0f;
    if (!open_) { draw_toast(w, h, s); return; }
    const unsigned WHITE = OVL_RGBA(240, 240, 245, 255), GREY = OVL_RGBA(150, 155, 170, 255), DIM = OVL_RGBA(110, 115, 130, 255),
                   GREEN = OVL_RGBA(120, 220, 140, 255), AMBER = OVL_RGBA(245, 190, 90, 255), ACCENT = OVL_RGBA(225, 60, 60, 255);
    ovl_rect(0, 0, w, h, OVL_RGBA(0, 0, 0, 150));
    float px = w * 0.06f, py = h * 0.06f, pw = w * 0.88f, ph = h * 0.88f;
    box(px, py, pw, ph, s);
    ovl_text(px + 32 * s, py + 22 * s, 44 * s, WHITE, "Settings");
    char banner[200] = "";
    char status[512];
    net_status(status, sizeof status);
    if (strstr(status, "Room:")) snprintf(banner, sizeof banner, "You are in a netplay room: the game keeps running while this menu is open.");
    else if (restart_pending) snprintf(banner, sizeof banner, "Some changes take effect when the game starts again: Game > Restart the game.");
    char t[160];
    if (toast_now(t, sizeof t)) ovl_text(px + 300 * s, py + 34 * s, 24 * s, WHITE, t);   /* (a message, in the menu: here) */
    else if (banner[0]) ovl_text(px + 300 * s, py + 34 * s, 24 * s, strstr(status, "Room:") ? ACCENT : AMBER, banner);
    /* the pages */
    float ty = py + 110 * s;
    for (int i = 0; i < PAGES; i++) {
        int on = page == i || (page == P_REMAP && i == P_PADS);
        if (on) ovl_rect(px + 20 * s, ty - 6 * s, 250 * s, 50 * s, OVL_RGBA(55, 62, 90, 255));
        char t[32];
        snprintf(t, sizeof t, "%d %s", i + 1, PAGE_NAMES[i]);
        ovl_text(px + 36 * s, ty, 32 * s, on ? WHITE : GREY, t);
        ty += 60 * s;
    }
    /* the rows */
    float rx = px + 300 * s, ry = py + 110 * s, rw = px + pw - 28 * s - rx, rh = 50 * s, size = 29 * s;
    float help_y = py + ph - 150 * s;
    int fit = (int)((help_y - 16 * s - ry) / rh);
    if (fit < 1) fit = 1;
    int *t0 = &top[page];
    if (sel[page] < *t0) *t0 = sel[page];
    if (sel[page] >= *t0 + fit) *t0 = sel[page] - fit + 1;
    if (*t0 > nrows - fit) *t0 = nrows - fit > 0 ? nrows - fit : 0;
    float vx = rx + rw * 0.40f;
    for (int i = *t0; i < nrows && i < *t0 + fit; i++) {
        const Row *r = &rows[i];
        float y = ry + (float)(i - *t0) * rh;
        int on = i == sel[page];
        if (on) ovl_rect(rx, y - 6 * s, rw, rh - 4 * s, OVL_RGBA(55, 62, 90, 255));
        ovl_text(rx + 16 * s, y, size, r->kind == K_INFO ? GREY : WHITE, r->label);
        char v[160];
        if (r->kind == K_CHOICE && on) snprintf(v, sizeof v, "< %s >", r->value);
        else snprintf(v, sizeof v, "%s", r->value);
        const char *tag = r->note[0] ? r->note : r->when == W_NOW ? "now" : r->when == W_RESTART ? "next start" : "";
        float tag_size = 22 * s, tag_w = text_w(tag, tag_size);
        int room = (int)((rx + rw - 24 * s - tag_w - vx) / (size / 2)) - 1;
        if (room > 0 && (int)strlen(v) > room) { v[room > 3 ? room - 3 : 0] = 0; strcat(v, "..."); }
        ovl_text(vx, y, size, r->kind == K_INFO ? WHITE : on ? WHITE : OVL_RGBA(200, 205, 220, 255), v);
        ovl_text(rx + rw - 16 * s - tag_w, y + 5 * s, tag_size, r->note[0] ? DIM : r->when == W_NOW ? GREEN : AMBER, tag);
    }
    if (*t0 > 0) ovl_text(rx + rw - 40 * s, ry - 40 * s, 26 * s, GREY, "^");
    if (*t0 + fit < nrows) ovl_text(rx + rw - 40 * s, help_y - 34 * s, 26 * s, GREY, "v");
    /* the selected row's help, and the keys */
    ovl_rect(px + 20 * s, help_y - 8 * s, pw - 40 * s, 2 * s, OVL_RGBA(60, 66, 90, 255));
    if (sel[page] < nrows && rows[sel[page]].help) {
        char wrapped[600];
        wrap(rows[sel[page]].help, (int)((pw - 80 * s) / (13 * s)), wrapped, sizeof wrapped);
        ovl_text(px + 36 * s, help_y + 4 * s, 26 * s, OVL_RGBA(200, 210, 230, 255), wrapped);
    }
    ovl_text(px + 36 * s, py + ph - 44 * s, 22 * s, DIM,
             page == P_REMAP ? "Up/Down: choose   A/Enter: press a new control   Y/Delete: default   B/Backspace: back   Esc: close"
                             : "Up/Down: choose   Left/Right: change   A/Enter: select   L/R or Tab, 1-5: page   B/Esc: close");
    /* on top: a question, the text being typed, or a control being waited for */
    if (confirm) {
        float bw = 820 * s, bh = 200 * s, bx = (w - bw) / 2, by = (h - bh) / 2;
        modal_box(w, h, bx, by, bw, bh, s);
        ovl_text(bx + 36 * s, by + 34 * s, 34 * s, WHITE, confirm == R_RESTART ? "Start the game again?" : "Quit the game?");
        ovl_text(bx + 36 * s, by + 84 * s, 26 * s, GREY, "A match in progress is lost.");
        ovl_text(bx + 36 * s, by + 140 * s, 26 * s, AMBER, "A / Enter: yes      B / Esc: no");
    } else if (editing) {
        float bw = 900 * s, bh = 600 * s, bx = (w - bw) / 2, by = (h - bh) / 2, cs = 56 * s;
        modal_box(w, h, bx, by, bw, bh, s);
        ovl_text(bx + 36 * s, by + 28 * s, 30 * s, GREY, editing == R_NET_NAME ? "Your name" : editing == R_NET_CODE ? "Lobby code" : "Server (host:port)");
        char t[80];
        snprintf(t, sizeof t, "%s_", edit_buf);
        ovl_rect(bx + 30 * s, by + 72 * s, bw - 60 * s, 56 * s, OVL_RGBA(16, 18, 26, 255));
        ovl_text(bx + 44 * s, by + 82 * s, 34 * s, WHITE, t);
        float gx = bx + (bw - GRID_COLS * cs) / 2, gy = by + 160 * s;
        for (int r = 0; r < GRID_ROWS; r++)
            for (int c = 0; c < grid_cols(r); c++) {
                char cell[16];
                float cx = gx + c * cs, cw = cs - 6 * s;
                if (r < GRID_ROWS - 1) snprintf(cell, sizeof cell, "%c", GRID[r][c]);
                else if (c == 0) snprintf(cell, sizeof cell, ":");
                else { snprintf(cell, sizeof cell, "%s", GRID_KEYS[c - 1]); cx = gx + cs + (c - 1) * 2.75f * cs; cw = 2.75f * cs - 6 * s; }
                int on = r == grid_r && c == grid_c;
                ovl_rect(cx, gy + r * cs, cw, cs - 6 * s, on ? OVL_RGBA(225, 60, 60, 255) : OVL_RGBA(40, 45, 62, 255));
                ovl_text(cx + (cw - text_w(cell, 30 * s)) / 2, gy + r * cs + 10 * s, 30 * s, WHITE, cell);
            }
        ovl_text(bx + 36 * s, by + bh - 52 * s, 22 * s, DIM, "Type, then Enter (Esc: cancel). Pad: A types, B deletes, + done, L cancels.");
    } else if (capture_button >= 0) {
        float bw = 900 * s, bh = 200 * s, bx = (w - bw) / 2, by = (h - bh) / 2;
        modal_box(w, h, bx, by, bw, bh, s);
        char t[160];
        PadInfo *pad = pad_slot >= 0 ? &pads[pad_slot] : NULL;
        snprintf(t, sizeof t, "Press the control for %s", pad_button_name(capture_button));
        ovl_text(bx + 36 * s, by + 34 * s, 34 * s, WHITE, t);
        snprintf(t, sizeof t, "on %.60s", pad ? pad->name : "the controller");
        ovl_text(bx + 36 * s, by + 84 * s, 26 * s, GREY, t);
        ULONGLONG now = GetTickCount64();
        snprintf(t, sizeof t, "Esc: cancel   (%d s)", capture_until > now ? (int)((capture_until - now + 999) / 1000) : 0);
        ovl_text(bx + 36 * s, by + 140 * s, 26 * s, AMBER, t);
    }
}
