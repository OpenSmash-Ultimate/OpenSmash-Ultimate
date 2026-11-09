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
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "config_folder.h"
#include "display.h"
#include "gpu_menu.h"
#include "audio.h"
#include "gpu_overlay.h"
#include "language.h"
#include "mod_switch.h"
#include "net.h"
#include "pad.h"
#include "padusb.h"
#include "settings.h"
#include "sim_version.h"

int gfx_window_focused(void);
void gfx_closing(void); /* (gpu_d3d11.c) the game is about to end */
#ifdef _WIN32
int gfx_adapter_names(char names[][64], int max); /* (gpu_d3d11.c) the graphics cards, fastest first */
#endif

#define PUBLIC_LOBBY "smash.recomps.wtf:39421" /* (the online_server.txt the port comes with) */

static volatile int open_, releasing;
int menu_is_open(void) { return open_; }
int menu_blocks_input(void) { return open_ || releasing; }

/* ---- messages ------------------------------------------------------------------------- */
static SRWLOCK toast_lock = SRWLOCK_INIT;
static char toast[160];
static ULONGLONG toast_until; /* 1: not shown yet (its time starts at the first draw) */

void menu_toast(const char *text)
{
    AcquireSRWLockExclusive(&toast_lock);
    snprintf(toast, sizeof toast, "%s", text);
    toast_until = 1;
    ReleaseSRWLockExclusive(&toast_lock);
}

int menu_visible(void) { return open_ || (toast_until && (toast_until == 1 || GetTickCount64() < toast_until)); }

/* ---- keys from the window's thread ---------------------------------------------------- */
static volatile LONG q[64], q_head, q_tail;

static void push(LONG v)
{
    LONG h = q_head;
    if (h - q_tail >= 64)
        return;
    q[h & 63] = v;
    InterlockedExchange(&q_head, h + 1);
}

void menu_key(int key) { push(key); }
void menu_char(unsigned ch)
{
    if (ch >= 32 && ch < 127)
        push((LONG)(0x10000 | ch));
}

/* ---- what can be pressed ---------------------------------------------------------------- */
enum
{ /* (from keys, pads or the test script) */
    EV_UP = 1,
    EV_DOWN,
    EV_LEFT,
    EV_RIGHT,
    EV_ENTER,
    EV_A,
    EV_BKSP,
    EV_B,
    EV_DEL,
    EV_NEXT,
    EV_PREV,
    EV_DONE,
    EV_ESC,
    EV_CHAR
};

/* ---- the rows of a page ------------------------------------------------------------------ */
enum
{
    P_DISPLAY,
    P_PADS,
    P_ONLINE,
    P_GAME,
    P_LOBBY,
    P_MODS,
    P_REMAP,
    P_KEYS,
    P_STICKS,
    PAGES = P_REMAP
}; /* (the pad's buttons, the keyboard's keys, the pad's sticks: no key of their own) */
#define SUB_PAGE(p) ((p) == P_REMAP || (p) == P_KEYS || (p) == P_STICKS) /* (a page of the Controllers page's) */
static const char *const PAGE_NAMES[PAGES] = {"Display", "Controllers", "Online", "Game", "Lobby", "Mods"};
enum { K_INFO, K_CHOICE, K_ACTION, K_TEXT };
enum { W_NONE, W_NOW, W_RESTART };
enum {
    R_NONE, R_WINDOW, R_PACING, R_LATENCY, R_CURSOR, R_OVERLAY, R_SIZE, R_SCALE, R_GPU,
    R_PAD, R_PAD_PLAYER, R_PAD_LAYOUT, R_PAD_DEADZONE, R_PAD_RUMBLE, R_PAD_TRIGGER, R_PAD_REMAP, R_PAD_TEST, R_PAD_RESET,
    R_PAD_STICKS, R_PAD_RECAL, R_STICKS_BACK,
    R_RUMBLE, R_KEYBOARD, R_BACKGROUND, R_PADS_FILE,
    R_NET_STATUS, R_NET_MODE, R_NET_CODE, R_NET_SERVER, R_NET_NAME, R_NET_DIRECT, R_NET_PROTOCOL,
    R_LOBBY, R_MATCH_FIND, R_MATCH_CANCEL, R_ROOMS,
    R_LANGUAGE, R_VOLUME, R_RESTART, R_QUIT, R_REMAP_BACK, R_MODS_STATE, R_MOD_CODE, R_MODS_OFF, R_LAN_ROOMS,
    R_KEYS, R_KEYS_BACK, R_KEYS_RESET, R_RENDERER, R_ROOMS_PREV, R_ROOMS_NEXT, R_ROOMS_GAME_PAGE, R_RUNAHEAD, R_NET_ROLLBACK, R_NET_DELAY,
    R_ROOM_FIRST = 60,    /* + the room's place on the page of rooms shown (ROOM_PAGE at most) */
    R_REMAP_FIRST = 100,  /* + console button */
    R_MOD_FIRST = 200,    /* + the mod's place in mods.json (MENU_MODS at most) */
    R_KEY_FIRST = 400,    /* + the keyboard binding (PAD_KEYS) */
    R_OSK = 500           /* the game's own text box (menu_osk_run) */
};
#define IS_KEY(id) ((id) >= R_KEY_FIRST && (id) < R_KEY_FIRST + PAD_KEYS)
#define IS_REMAP(id) ((id) >= R_REMAP_FIRST && (id) < R_REMAP_FIRST + 16)

typedef struct Row
{
    int id, kind, when;
    char label[48], value[112], note[64];
    const char *help;
    int warn;
} Row;
enum
{
    MENU_ROWS = 180,
    MENU_MODS = 150
}; /* (the Mods page: a row a mod, MENU_MODS at most) */
static Row rows[MENU_ROWS];
static int nrows;

static Row *add(int id, int kind, int when, const char *label, const char *help)
{
    Row *r = &rows[nrows < MENU_ROWS - 1 ? nrows++ : MENU_ROWS - 1];
    memset(r, 0, sizeof *r);
    r->id = id;
    r->kind = kind;
    r->when = when;
    r->help = help;
    snprintf(r->label, sizeof r->label, "%s", label);
    return r;
}

/* ---- state ---------------------------------------------------------------------------- */
static int page, sel[P_STICKS + 1], top[P_STICKS + 1];
static int pad_slot = -1;           /* the pad the Controllers page sets up */
static int confirm;                 /* a row asking yes or no */
static int editing, grid_r, grid_c; /* a text row being typed */
static char edit_buf[64];
static int capture_button = -1; /* the console button a new control is wanted for */
static u32 capture_held;
static ULONGLONG capture_until;
static int capture_key = -1;     /* the keyboard binding a new key is wanted for */
static int capture_key_held = 1; /* (keys still held from before: they must go first) */
static ULONGLONG capture_quiet;  /* until then the window's queued keys are passed over (the key just bound) */
static int restart_pending;
static int *overlay_on;           /* the frame-rate overlay's switch (gpu_d3d11.c) */
static char pins[PAD_SLOTS][128]; /* controllers.ini's [players], read at the menu's opening and changes */
static int rumble_slot = -1;
static ULONGLONG rumble_stop;
static char version[64];
static char gpus[8][64];
static int ngpus;
static int built_page = -1; /* the page build() made last (-1: none since the menu opened) */
/* The game's software keyboard (nn::swkbd, hle_applet.c) typed on the text grid: asked from the game's
 * thread (menu_osk_run), opened and answered here. 0 none, 1 asked, 2 open, 3 done, 4 cancelled. */
static volatile LONG osk_state;
static char osk_title[96], osk_text[64];
static int osk_max = 32;

static struct { int mode; char custom[128], code[40], name[40]; int direct, protocol, rollback; } net;
enum { NET_PUBLIC, NET_PRIVATE, NET_CUSTOM, NET_OFFLINE, NET_LAN, NET_MODE_COUNT };
static const char *const NET_MODES[NET_MODE_COUNT] = {"public lobby", "private lobby (a code)", "your own server", "offline",
                                                      "this network (LAN)"};

static void net_load(void)
{
    char line[256] = "", host[128] = "", code[40] = "";
    settings_line_read(config_path(CONFIG_ONLINE_SERVER), line, sizeof line);
    sscanf(line, "%127s %39s", host, code);
    memset(&net, 0, sizeof net);
    snprintf(net.code, sizeof net.code, "%s", code);
    if (!host[0] || !_stricmp(host, "off"))
        net.mode = NET_OFFLINE;
    else if (!_stricmp(host, "lan"))
        net.mode = NET_LAN; /* (net_relay.c's LAN discovery) */
    else if (!_stricmp(host, PUBLIC_LOBBY))
        net.mode = code[0] ? NET_PRIVATE : NET_PUBLIC;
    else
    {
        net.mode = NET_CUSTOM;
        snprintf(net.custom, sizeof net.custom, "%s", host);
    }
    settings_line_read(config_path(CONFIG_ONLINE_NAME), net.name, sizeof net.name);
    char direct[16] = "";
    net.direct = !(settings_line_read(config_path(CONFIG_ONLINE_DIRECT), direct, sizeof direct) && !_strnicmp(direct, "off", 3));
    char rollback[16] = "";   /* (net_relay.c's net_rollback_default reads it the same way) */
    net.rollback = settings_line_read(config_path(CONFIG_ONLINE_ROLLBACK), rollback, sizeof rollback) && !_strnicmp(rollback, "on", 2);   /* (off by default: net_relay.c) */
    char protocol[16] = "";   /* (net_relay.c reads it the same way) */
    int v = settings_line_read(config_path(CONFIG_ONLINE_PROTOCOL), protocol, sizeof protocol) ? atoi(protocol) : 0;
    net.protocol = v == NET_PROTOCOL || v == NET_PROTOCOL_V3 ? v : NET_PROTOCOL_DEFAULT;
}

static void net_save_server(void)
{
    char line[256];
    if (net.mode == NET_OFFLINE)
        snprintf(line, sizeof line, "off");
    else if (net.mode == NET_LAN)
        snprintf(line, sizeof line, "lan");
    else if (net.mode == NET_PUBLIC)
        snprintf(line, sizeof line, "%s", PUBLIC_LOBBY);
    else if (net.mode == NET_PRIVATE)
    {
        if (!net.code[0])
            snprintf(net.code, sizeof net.code, "friends");
        snprintf(line, sizeof line, "%s %s", PUBLIC_LOBBY, net.code);
    }
    else
        snprintf(line, sizeof line, "%s%s%s", net.custom[0] ? net.custom : "127.0.0.1:39420", net.code[0] ? " " : "", net.code);
    settings_line_write(config_path(CONFIG_ONLINE_SERVER), line);
}

static void load_version(void)
{
    char path[600];
    snprintf(path, sizeof path, "%s/../../version.txt", rt_data_dir ? rt_data_dir : ".");
    version[0] = 0;
    FILE *f = fopen(path, "r");
    if (f)
    {
        if (fgets(version, sizeof version, f))
            version[strcspn(version, "\r\n")] = 0;
        fclose(f);
    }
}

static void load_pins(void);

static void open_menu(void)
{
    if (open_)
        return;
    net_load();
    load_pins();
    load_version();
#ifdef _WIN32
    ngpus = gfx_adapter_names(gpus, 8);
#endif
    confirm = editing = 0;
    capture_button = capture_key = -1;
    if (SUB_PAGE(page))
        page = P_PADS;
    open_ = 1;
    fprintf(stderr, "[menu] open\n");
}

static void close_menu(void)
{
    if (!open_)
        return;
    if (osk_state == 2)
        InterlockedExchange(&osk_state, 4); /* (the game's text box closed with the menu: cancelled) */
    confirm = editing = 0;
    capture_button = capture_key = -1;
    releasing = 1; /* (the game gets input back once nothing is held) */
    open_ = 0;
    built_page = -1; /* (the Lobby page lists the rooms again when it shows next) */
    fprintf(stderr, "[menu] closed\n");
}

/* ---- saving --------------------------------------------------------------------------- */
static void changed(int when)
{
    if (when == W_RESTART && !restart_pending)
    {
        restart_pending = 1;
        menu_toast("Saved: takes effect when the game starts again (Game > Restart)");
    }
}

static void set_display(const char *key, const char *value) { settings_ini_set(display_ini_path(), NULL, key, value); }

/* The [device ...] a pad's settings go in: the one it takes them from now, else its name's. */
static void pad_section(const PadInfo *p, char *out, int size) { snprintf(out, (size_t)size, "device %s", p->section[0] ? p->section : p->name); }

static void set_pad(const PadInfo *p, const char *key, const char *value)
{
    char section[128];
    pad_section(p, section, sizeof section);
    settings_ini_set(pad_ini_path(), section, key, value);
    pad_reload();
}

static int pad_matches(const PadInfo *p, const char *what)
{
    return *what && (!_stricmp(what, p->guid) || !_stricmp(what, p->name) || !_stricmp(what, p->type));
}

static void load_pins(void)
{
    for (int k = 1; k <= PAD_SLOTS; k++)
    {
        char key[4];
        snprintf(key, sizeof key, "%d", k);
        if (!settings_ini_get(pad_ini_path(), "players", key, pins[k - 1], sizeof pins[k - 1]))
            pins[k - 1][0] = 0;
    }
}

/* The player controllers.ini keeps for this pad ([players]), 0 none. */
static int pad_pin(const PadInfo *p)
{
    for (int k = 1; k <= PAD_SLOTS; k++)
        if (pad_matches(p, pins[k - 1]))
            return k;
    return 0;
}

/* ---- the pages -------------------------------------------------------------------------- */
static PadInfo pads[PAD_SLOTS];
static int npads_connected;

static const char *on_off(int v) { return v ? "on" : "off"; }

static void note_env(Row *r, const char *var)
{
    const char *v = getenv(var);
    if (v && *v)
        snprintf(r->note, sizeof r->note, "%s set", var);
}

static const int SIZES[][2] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
static const int REMAP_ORDER[16] = {0, 1, 2, 3, 6, 7, 8, 9, 10, 11, 4, 5, 13, 15, 12, 14}; /* A B X Y L R ZL ZR + - sticks, d-pad */

/* ---- the Lobby page: the online lobby's rooms and matchmaking (net.h, netplay version 3) ----
 * Joining stays the game's own: a room this game can join is in Local Wireless > Rooms List, and
 * matchmaking's host creates a room there, which comes first in the other player's list. The page
 * says what to do, and asks the lobby for the rooms again each time it shows (and on A). */
enum
{
    LOBBY_ROOMS = 512,
    ROOM_PAGE = 30,
    GAME_LIST = 24
}; /* (GAME_LIST: the game's Rooms List, hle_net.c) */
static NetRoom lobby_rooms[LOBBY_ROOMS];
static int nlobby_rooms, rooms_state, room_view, game_pages = 1; /* room_view: the first room of the page shown; game_pages: of the game's list */
#define ROOM_AT(id) (&lobby_rooms[room_view + (id) - R_ROOM_FIRST])
#define IS_ROOM(id) ((id) >= R_ROOM_FIRST && (id) < R_ROOM_FIRST + ROOM_PAGE && room_view + (id) - R_ROOM_FIRST < nlobby_rooms)
static NetMatch match;
static ULONGLONG match_started;                         /* when the menu's search started (the time it shows) */
static int match_followed, match_said, match_said_open; /* that search, followed while the menu is closed too */
static char helps[MENU_ROWS][900];                      /* help made for a row (help_set) */

static void help_set(Row *r, const char *fmt, ...)
{
    char *h = helps[r - rows];
    va_list a;
    va_start(a, fmt);
    vsnprintf(h, sizeof helps[0], fmt, a);
    va_end(a);
    r->help = h;
}

static const char *who(const char *name) { return name[0] ? name : "the other player"; }

/* This game looks for its private room on the local network (Play online in > this network: net.h's LAN discovery). */
static int net_on_lan(void)
{
    int st = net_lan(NULL, NULL);
    return st == NET_LAN_LOOKING || st == NET_LAN_NONE || st == NET_LAN_FOUND;
}

/* The LAN discovery's row: the rooms heard on this network (LAN mode), or whether this game's own
 * private room is announced there (hosting). */
static void lan_row(int looking)
{ /* (looking: LAN mode; else hosting) */
    int ours = 0, others = 0, lan = net_lan(&ours, &others);
    Row *r = add(R_LAN_ROOMS, K_ACTION, W_NONE, looking ? "Rooms on this network" : "On this network", NULL);
    if (looking)
    {
        snprintf(r->value, sizeof r->value, "%d", ours);
        if (others)
            snprintf(r->value + strlen(r->value), sizeof r->value - strlen(r->value), " (and %d of another version)", others);
        help_set(r, "Private rooms hosted on this network by games of your version, heard now. The game joins the first one by itself "
                    "(Online > Now says which), and the next one if that host leaves.%s",
                 others ? "\nThe others are of another game or WindowSmash version, or of another room code: they can't be joined." : "");
    }
    else if (lan == NET_LAN_ANNOUNCING)
    {
        snprintf(r->value, sizeof r->value, "announced");
        r->help = "Games on this network set to Play online in > this network find your room by themselves, no address or code needed.";
    }
    else
    {
        snprintf(r->value, sizeof r->value, "not announced");
        r->help = "Your room isn't announced on this network, as Direct connections are off (or SSBU_NET_LAN=0): players here join "
                  "with the room code or your address.";
    }
}

static const char *const PROTOCOL_HELP = "3: the online lobby's room browser and matchmaking (the Lobby page), being tried out. A "
                                         "lobby or private room of version 2 still works: the game joins it in 2. 2: as before.";

static void add_protocol_row(void)
{
    Row *r = add(R_NET_PROTOCOL, K_CHOICE, W_RESTART, "Netplay version", PROTOCOL_HELP);
    snprintf(r->value, sizeof r->value, "%s", net.protocol == NET_PROTOCOL_V3 ? "3 (being tried out)" : "2");
    if (net_on_lan())
        snprintf(r->note, sizeof r->note, "this network: 2");
    else if (net_configured() && !net_relay_address())
        snprintf(r->note, sizeof r->note, "hosting: 2");
    note_env(r, "SSBU_NET_PROTOCOL");
}

enum
{
    LB_OFFLINE,
    LB_HOSTING,
    LB_NEEDS_V3,
    LB_CONNECTING,
    LB_PRIVATE,
    LB_V2_LOBBY,
    LB_NONE,
    LB_ON,
    LB_LAN
};
static int lobby_state(void)
{
    if (!net_configured())
        return LB_OFFLINE;
    if (net_on_lan())
        return LB_LAN;
    if (!net_relay_address())
        return LB_HOSTING;
    if (net_protocol_wanted() != NET_PROTOCOL_V3)
        return LB_NEEDS_V3;
    if (!net_local_ip())
        return LB_CONNECTING;
    if (!net_lobby())
        return LB_PRIVATE;
    if (net_protocol_spoken() != NET_PROTOCOL_V3)
        return LB_V2_LOBBY;
    if (rooms_state == NET_ROOMS_OFF && match.state == NET_MATCH_OFF)
        return LB_NONE;
    return LB_ON;
}

static void match_rows(void)
{
    const NetMatch *m = &match;
    const char *other = who(m->peer_name);
    ULONGLONG now = GetTickCount64();
    int waited = match_started && now >= match_started ? (int)((now - match_started) / 1000) : m->waited_s;
    Row *r = add(R_MATCH_FIND, K_ACTION, W_NONE, "Find a match", NULL);
    switch (m->state)
    {
    case NET_MATCH_ASKING:
    case NET_MATCH_SEARCHING:
        if (m->state == NET_MATCH_ASKING)
            snprintf(r->value, sizeof r->value, "asking the lobby...");
        else
            snprintf(r->value, sizeof r->value, "searching... %d:%02d", waited / 60, waited % 60);
        help_set(r, "Searching for another player of your game, WindowSmash and mods, with your lobby code. You can close this menu "
                    "and play meanwhile: a message says when a match is found.");
        break;
    case NET_MATCH_MATCHED:
        if (m->role == NET_MATCH_HOST)
        {
            snprintf(r->value, sizeof r->value, "with %.32s: you host", other);
            help_set(r, "You host: close this menu, then Local Wireless > Create a Room (or keep yours open). %s finds it first in "
                        "their Rooms List.",
                     other);
        }
        else if (m->room_open)
        {
            snprintf(r->value, sizeof r->value, "with %.32s: their room is open", other);
            help_set(r, "%s hosts, and their room is open: close this menu, then Local Wireless > Rooms List. Their room comes first.", other);
        }
        else
        {
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
    if (m->state == NET_MATCH_ASKING || m->state == NET_MATCH_SEARCHING || m->state == NET_MATCH_MATCHED)
    {
        r = add(R_MATCH_CANCEL, K_ACTION, W_NONE, m->state == NET_MATCH_MATCHED ? "Leave the match" : "Stop searching", NULL);
        if (m->state == NET_MATCH_MATCHED)
            help_set(r, "Ends the match: %s is told.", other);
        else
            help_set(r, "Stops the search.");
    }
}

/* ---- the Mods page: every mod's switch (mods/mods.json, mod_switch.h) -------------------- */
static ModSwitch menu_mods[MENU_MODS];
static int nmenu_mods = -1, menu_mods_code = -1; /* (-1: read again when the page shows) */

static void mods_rows(int fresh)
{
    if (fresh || nmenu_mods < 0)
        nmenu_mods = mods_switch_list(menu_mods, MENU_MODS, &menu_mods_code);
    if (nmenu_mods < 0)
        nmenu_mods = 0;
    int on = 0, running = mods_running_count();
    for (int i = 0; i < nmenu_mods; i++)
        on += menu_mods[i].on;
    char msg[200];
    int st = mods_switch_state(msg, sizeof msg);
    Row *r = add(R_MODS_STATE, K_INFO, W_NONE, "Mods",
                 "Every mod's switch: the same as the launcher's and the Mods app's (mods/mods.json). A change counts when the game "
                 "starts again (Restart now), which applies the mods first. Online, players meet when they run the same mods.");
    if (st == MODS_SWITCH_WORKING || st == MODS_SWITCH_FAILED)
        snprintf(r->value, sizeof r->value, "%s", msg);
    else
        snprintf(r->value, sizeof r->value, "%d installed, %d on; %d running now", nmenu_mods, on, running);
    int code = mod_switch_code_patches(menu_mods_code, running);
    r = add(R_MOD_CODE, K_CHOICE, W_RESTART, MOD_SWITCH_CODE_TITLE,
            "ARCropolis' fixes to the game's code for costume slots a mod adds (c08 and up). Off: the game's own code, as with no "
            "mod. Auto: on while a mod runs. Players online need the same.");
    snprintf(r->value, sizeof r->value, "%s%s", code ? "on" : "off", menu_mods_code < 0 ? " (auto)" : "");
    if (rt_code_patches_on() != code)
        snprintf(r->note, sizeof r->note, "now %s", rt_code_patches_on() ? "on" : "off");
    if (on)
        add(R_MODS_OFF, K_ACTION, W_RESTART, "Turn every mod off", "Every mod off, for the game as it is (online: the vanilla game's players).");
    if (restart_pending)
        add(R_RESTART, K_ACTION, W_NONE, "Restart now", "Starts the game again, with the changes. A match in progress is lost.");
    for (int i = 0; i < nmenu_mods; i++)
    {
        const ModSwitch *m = &menu_mods[i];
        r = add(R_MOD_FIRST + i, K_CHOICE, W_RESTART, "", NULL);
        snprintf(r->label, sizeof r->label, "%.46s", m->title);
        snprintf(r->value, sizeof r->value, "%s", m->on ? "on" : "off");
        help_set(r, "%s\nFolder: mods/%s. A or Left/Right: on or off (counts when the game starts again).%s", m->title, m->name,
                 mod_switch_name_ok(m->name) ? "" : "\nIts folder's name can't be passed on from here: use the launcher.");
    }
    if (nmenu_mods == MENU_MODS)
        add(R_NONE, K_INFO, W_NONE, "More mods", "The first ones only: the launcher lists them all.");
    else if (!nmenu_mods)
        add(R_NONE, K_INFO, W_NONE, "No mod installed", "Mods are installed with the launcher (OpenSmash.exe) or the Mods app.");
}

/* A mod's (or the code patches') switch turned (dir: either way: it flips). */
static void mod_flip(const Row *row)
{
    int ok;
    if (row->id == R_MOD_CODE)
    {
        int code = mod_switch_code_patches(menu_mods_code, mods_running_count());
        ok = mods_switch_set(MOD_SWITCH_CODE, !code);
        if (ok)
            menu_mods_code = !code;
    }
    else
    {
        ModSwitch *m = &menu_mods[row->id - R_MOD_FIRST];
        ok = mods_switch_set(m->name, !m->on);
        if (ok)
            m->on = !m->on;
    }
    if (!ok)
    {
        menu_toast(mods_switch_state(NULL, 0) == MODS_SWITCH_WORKING ? "Still changing the last one: a moment" : "Can't change it from here: use the launcher");
        return;
    }
    changed(W_RESTART);
}

/* Why a room can't be joined, in a few words (the help has the whole of it). */
static const char *room_short(const NetRoom *x)
{
    if (x->why_kind == NET_WHY_BUILD)
        return "another game version";
    if (x->why_kind == NET_WHY_SIM)
        return x->sim > SIM_VERSION ? "update your WindowSmash" : "their WindowSmash is older";
    return "other mods";
}

static void room_rows(void)
{
    int can = 0;
    for (int i = 0; i < nlobby_rooms; i++)
        can += lobby_rooms[i].joinable && !lobby_rooms[i].mine;
    Row *r = add(R_ROOMS, K_ACTION, W_NONE, "Rooms", "Every room of your lobby code, of any game version or mods. A: list them again. "
                                                     "The ones you can join are in Local Wireless > Rooms List too.");
    if (rooms_state == NET_ROOMS_ASKING)
        snprintf(r->value, sizeof r->value, "asking the lobby...");
    else if (rooms_state == NET_ROOMS_NO_ANSWER)
        snprintf(r->value, sizeof r->value, "no answer from the lobby (A: ask again)");
    else if (!nlobby_rooms)
        snprintf(r->value, sizeof r->value, "none");
    else
        snprintf(r->value, sizeof r->value, "%d, %d you can join", nlobby_rooms, can);
    if (room_view >= nlobby_rooms)
        room_view = 0;
    int end = room_view + ROOM_PAGE < nlobby_rooms ? room_view + ROOM_PAGE : nlobby_rooms;
    if (room_view > 0)
    {
        r = add(R_ROOMS_PREV, K_ACTION, W_NONE, "Earlier rooms", "A: the rooms before these.");
        snprintf(r->value, sizeof r->value, "%d-%d", room_view - ROOM_PAGE + 1 > 1 ? room_view - ROOM_PAGE + 1 : 1, room_view);
    }
    u32 picked = net_rooms_picked();
    for (int i = room_view; i < end; i++)
    {
        const NetRoom *x = &lobby_rooms[i];
        const char *host = x->name[0] ? x->name : "(no name)";
        r = add(R_ROOM_FIRST + i - room_view, K_ACTION, W_NONE, "", NULL);
        snprintf(r->label, sizeof r->label, "%.30s%s", host, x->mine ? " (you)" : "");
        /* (the help, with the cursor on it: why not, then its game and mods against yours: net_relay.c room_details) */
        if (!x->joinable)
        {
            snprintf(r->value, sizeof r->value, "can't join: %s", room_short(x));
            help_set(r, "%s%s\n%s", x->why, x->why_kind != NET_WHY_SIM ? "" : x->sim > SIM_VERSION ? "\nYours is the older one: update WindowSmash to join."
                                                                                                   : "\nTheirs is the older one: the host needs to update.",
                     x->details);
            continue;
        }
        snprintf(r->value, sizeof r->value, "%d/%d players, %s%s", x->players, x->most, x->open ? "open" : "full or closed",
                 picked && x->host == picked && !x->mine ? " - picked: first in your Rooms List" : x->open && !x->mine ? " - A: join"
                                                                                                                       : "");
        if (x->mine)
            help_set(r, "Your own room, %d of %d players: other players of your game find it in their Rooms List.\n%s", x->players, x->most,
                     x->details);
        else if (x->open)
            help_set(r, "%s's room, %d of %d players. A: put it first in Local Wireless > Rooms List, then close this menu "
                        "and pick it there.\n%s",
                     host, x->players, x->most, x->details);
        else
            help_set(r, "%s's room is full or closed for now (%d of %d players). Once it opens, it is in Local Wireless > Rooms List.\n%s",
                     host, x->players, x->most, x->details);
    }
    if (end < nlobby_rooms)
    {
        r = add(R_ROOMS_NEXT, K_ACTION, W_NONE, "More rooms", "A: the rooms after these.");
        snprintf(r->value, sizeof r->value, "%d-%d of %d", end + 1, end + ROOM_PAGE < nlobby_rooms ? end + ROOM_PAGE : nlobby_rooms, nlobby_rooms);
    }
    int others = can - (picked != 0); /* (the game's Rooms List: the picked room, then a page of GAME_LIST of the others) */
    if (others > GAME_LIST || net_rooms_page_get() > 0)
    {
        int pages = others > 0 ? (others + GAME_LIST - 1) / GAME_LIST : 1, pg = net_rooms_page_get() < pages ? net_rooms_page_get() : 0;
        game_pages = pages;
        r = add(R_ROOMS_GAME_PAGE, K_ACTION, W_NONE, "Game's Rooms List shows", "The game's own Rooms List (Local Wireless) holds 24 rooms: "
                                                                                "A shows the next 24 of the ones you can join there (the room you picked always comes first).");
        snprintf(r->value, sizeof r->value, "rooms %d-%d of %d (A: next)", pg * GAME_LIST + 1, pg * GAME_LIST + GAME_LIST < others ? pg * GAME_LIST + GAME_LIST : others, others);
    }
}

/* The Lobby page's rows (fresh: the page shows anew, so the rooms are asked for again). */
static void build_lobby(int fresh)
{
    nlobby_rooms = net_rooms_get(lobby_rooms, LOBBY_ROOMS, &rooms_state);
    net_match_get(&match);
    if (fresh && (rooms_state == NET_ROOMS_LISTED || rooms_state == NET_ROOMS_NO_ANSWER))
        net_rooms_ask(); /* (asking: once is enough) */
    int st = lobby_state();
    Row *r = add(R_LOBBY, K_INFO, W_NONE, "Lobby", NULL);
    switch (st)
    {
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
    case LB_LAN:
    {
        int lan = net_lan(NULL, NULL);
        if (lan == NET_LAN_FOUND && net_local_ip())
            snprintf(r->value, sizeof r->value, "in a room on this network");
        else if (lan == NET_LAN_FOUND)
            snprintf(r->value, sizeof r->value, "joining a room on this network...");
        else if (lan == NET_LAN_NONE)
            snprintf(r->value, sizeof r->value, "no room on this network yet");
        else
            snprintf(r->value, sizeof r->value, "looking on this network...");
        r->help = "Play online in > this network: the game joins a private room hosted on your local network by itself, and keeps "
                  "looking until one starts. Its players find each other's rooms in Local Wireless > Rooms List. The room browser and "
                  "matchmaking are the online lobby's.";
        break;
    }
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
        else
            snprintf(r->value, sizeof r->value, "version 3");
        r->help = "The players of your lobby code who can play with you (the same game, WindowSmash and mods), and how many of them "
                  "are searching for a match.";
    }
    if (st == LB_ON)
    {
        if (match.state != NET_MATCH_OFF)
            match_rows();
        if (rooms_state != NET_ROOMS_OFF)
            room_rows();
    }
    else if (st == LB_OFFLINE || st == LB_NEEDS_V3)
        add_protocol_row();
    else if (st == LB_LAN || st == LB_HOSTING)
        lan_row(st == LB_LAN);
    if (restart_pending)
        add(R_RESTART, K_ACTION, W_NONE, "Restart now", "Starts the game again, with the changes. A match in progress is lost.");
}

/* A search the menu started, followed while the menu is closed too: a message when it is matched,
 * when the host's room opens, and when it ends. */
static void match_follow(void)
{
    if (!match_followed)
        return;
    NetMatch m;
    net_match_get(&m);
    const char *other = who(m.peer_name);
    char t[160] = "";
    if (m.state != match_said)
    {
        switch (m.state)
        {
        case NET_MATCH_MATCHED:
            if (m.role == NET_MATCH_HOST)
                snprintf(t, sizeof t, "Matched with %.32s: you host (Local Wireless > Create a Room)", other);
            else if (m.room_open)
                snprintf(t, sizeof t, "Matched with %.32s: their room is open (Local Wireless > Rooms List)", other);
            else
                snprintf(t, sizeof t, "Matched with %.32s: they host (their room comes first in Rooms List)", other);
            break;
        case NET_MATCH_TIMEOUT:
            snprintf(t, sizeof t, "Matchmaking: no opponent found in time");
            break;
        case NET_MATCH_LEFT:
            snprintf(t, sizeof t, "Matchmaking: %.32s cancelled or left", other);
            break;
        case NET_MATCH_TOGETHER:
            snprintf(t, sizeof t, "Matchmaking: you and %.32s are in one room", other);
            break;
        case NET_MATCH_EXPIRED:
            snprintf(t, sizeof t, "Matchmaking: the match expired (not in one room in time)");
            break;
        }
        match_said = m.state;
    }
    else if (m.state == NET_MATCH_MATCHED && m.role == NET_MATCH_JOIN && m.room_open && !match_said_open)
        snprintf(t, sizeof t, "%.32s's room is open (Local Wireless > Rooms List)", other);
    if (t[0])
        menu_toast(t);
    match_said_open = m.state == NET_MATCH_MATCHED && m.room_open;
    if (m.state == NET_MATCH_IDLE || (m.state >= NET_MATCH_TIMEOUT && m.state <= NET_MATCH_EXPIRED))
        match_followed = 0;
}

static void build(void)
{
    /* (the Lobby page: the row under the cursor, by what it is, so the cursor stays on it when the
     * rooms or the search change around it) */
    int same = built_page == page, was = same && sel[page] < nrows ? rows[sel[page]].id : R_NONE;
    u32 was_host = IS_ROOM(was) ? ROOM_AT(was)->host : 0;
    built_page = page;
    nrows = 0;
    Row *r;
    DisplaySettings ds;
    display_settings(&ds);
    PadInfo *pad = pad_slot >= 0 && pads[pad_slot].connected ? &pads[pad_slot] : NULL;
    switch (page)
    {
    case P_DISPLAY:
#ifdef __APPLE__
        r = add(R_WINDOW, K_CHOICE, W_NOW, "Window", "Windowed: a normal window. Borderless and full screen: macOS's full screen, in a space of its own. Option+Return switches too.");
#else
        r = add(R_WINDOW, K_CHOICE, W_NOW, "Window", "Windowed: a normal window. Borderless: covering the monitor. Full screen: borderless and always on top. F11 switches too.");
#endif
        snprintf(r->value, sizeof r->value, "%s", ds.mode == DISPLAY_BORDERLESS ? "borderless" : ds.mode == DISPLAY_FULLSCREEN ? "full screen"
                                                                                                                               : "windowed");
        note_env(r, "SSBU_WINDOW");
        r = add(R_PACING, K_CHOICE, W_NOW, "Frame pacing",
                "Clock: a steady 60 frames a second. VRR: for G-Sync / FreeSync monitors, smoothest with borderless (tears without VRR). Display: follow a 60/120/240 Hz monitor's own refresh.");
        snprintf(r->value, sizeof r->value, "%s", ds.pacing == PACING_VRR ? "vrr" : ds.pacing == PACING_DISPLAY ? "display"
                                                                                                                : "clock");
        note_env(r, "SSBU_PACING");
        r = add(R_LATENCY, K_CHOICE, W_NOW, "Frames queued ahead", "How many frames the graphics card may work ahead: 1 is the most responsive, 3 the smoothest on a busy PC.");
        snprintf(r->value, sizeof r->value, "%d", ds.latency);
#ifdef _WIN32
        { /* (run-ahead: hle_rb_core.c. Its savestates are set up when the game starts: a change from off takes a restart) */
            int kernel_rb_switch(void);
            int n = display_runahead();
            r = add(R_RUNAHEAD, K_CHOICE, kernel_rb_switch() ? W_NOW : W_RESTART, "Run-ahead (experimental)",
                    "Less input delay in fights: each frame is shown 1 or 2 frames ahead. 1 shows the real frame a frame sooner; "
                    "2 also guesses one frame on the controls held now. Each frame ahead costs a frame of the game's work more: off on a slow PC.");
            if (n)
                snprintf(r->value, sizeof r->value, "%d frame%s", n, n > 1 ? "s" : "");
            else
                snprintf(r->value, sizeof r->value, "off");
            note_env(r, "SSBU_RUNAHEAD");
        }
#endif
        r = add(R_CURSOR, K_CHOICE, W_NOW, "Hide the mouse pointer", "Hides the pointer over the game after 2 seconds without moving it.");
        snprintf(r->value, sizeof r->value, "%s", on_off(ds.hide_cursor));
        r = add(R_OVERLAY, K_CHOICE, W_NOW, "Frame rate in the corner", "The frame rate and a graph of frame times (F1 shows or hides it while playing; F2 adds details).");
        snprintf(r->value, sizeof r->value, "%s", on_off(overlay_on ? *overlay_on : ds.overlay));
        note_env(r, "SSBU_OVERLAY");
#ifdef __APPLE__
        r = add(R_SIZE, K_CHOICE, W_RESTART, "Window size", "The size of the game's window when it is not full screen, in points (on a Retina display, a point is 2 x 2 pixels).");
#else
        r = add(R_SIZE, K_CHOICE, W_RESTART, "Window size", "The size of the game's window when it is not full screen (at 100% Windows scaling).");
#endif
        snprintf(r->value, sizeof r->value, "%d x %d", ds.width, ds.height);
        note_env(r, "SSBU_WINDOW_SIZE");
        r = add(R_SCALE, K_CHOICE, W_RESTART, "Render scale",
                "How many times 1920x1080 the game draws at: sharper on a big screen, more work for the graphics card. Auto: as many pixels as the window has.");
        if (ds.render_scale <= 0)
            snprintf(r->value, sizeof r->value, "auto");
        else
            snprintf(r->value, sizeof r->value, "%g", ds.render_scale);
        note_env(r, "SSBU_RENDER_SCALE");
#ifdef _WIN32
        r = add(R_GPU, K_CHOICE, W_RESTART, "Graphics card", "Auto picks the fastest card (on a laptop, the NVIDIA or AMD one). Pick another to use it instead.");
        snprintf(r->value, sizeof r->value, "%s", ds.gpu);
        note_env(r, "SSBU_GPU");
#endif
#ifndef __APPLE__
        r = add(R_RENDERER, K_CHOICE, W_RESTART, "Renderer",
#ifdef _WIN32
                "Direct3D 11: the renderer the game is played and tested with. Vulkan: in development; until it can draw the game, "
#else
                "DXVK: Direct3D 11 on Vulkan, the renderer the game is played and tested with. Vulkan: in development; until it can draw the game, "
#endif
                "the game checks Vulkan at its start, says what it found in its log, and draws as before.");
        snprintf(r->value, sizeof r->value, "%s", renderer_name(ds.renderer));
        note_env(r, "SSBU_RENDERER");
#endif
        break;
    case P_PADS:
        if (!npads_connected)
        {
            r = add(R_NONE, K_INFO, W_NONE, "Controllers", "Plug in or pair a controller: it shows here as it connects. The keyboard works too (see below).");
            snprintf(r->value, sizeof r->value, "none connected");
        }
        else
        {
            r = add(R_PAD, K_CHOICE, W_NONE, "Controller", "Which controller to set up (left / right to pick another).");
            if (pad && pad->slot >= 0)
                snprintf(r->value, sizeof r->value, "player %d: %.80s", pad->slot + 1, pad->name);
            else if (pad)
                snprintf(r->value, sizeof r->value, "no player: %.80s", pad->name);
            if (pad)
            {
                int pin = pad_pin(pad);
                r = add(R_PAD_PLAYER, K_CHOICE, W_NOW, "Player", "Auto: the lowest free player. A number keeps that player for this controller, every time it connects.");
                if (pin)
                    snprintf(r->value, sizeof r->value, "%d (kept)", pin);
                else
                    snprintf(r->value, sizeof r->value, "auto (now %d)", pad->slot + 1);
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
                add(R_PAD_STICKS, K_ACTION, W_NOW, "Sticks...", "The sticks' numbers live: their centre, how far they reach, and the adapter's speed.");
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
                "The keyboard adds to this player. Its keys: Keyboard keys... (by default arrows move, Z X C V are A B X Y, A S are L R, Q W are ZL ZR, Enter is +, I J K L the right stick).");
        if (pad_keyboard_slot() < 0)
            snprintf(r->value, sizeof r->value, "nobody");
        else
            snprintf(r->value, sizeof r->value, "player %d", pad_keyboard_slot() + 1);
        add(R_KEYS, K_ACTION, W_NONE, "Keyboard keys...", "Which key is each of the game's buttons and stick directions ([keyboard] in controllers.ini).");
        r = add(R_BACKGROUND, K_CHOICE, W_NOW, "Pads when not focused", "On: controllers work while another window is in front. Off: only while the game's window is.");
        snprintf(r->value, sizeof r->value, "%s", on_off(pad_background()));
        r = add(R_PADS_FILE, K_ACTION, W_NONE, "Open controllers.ini", "Everything else, by hand: settings per kind of pad, more remaps. Changes there count at the next start.");
        snprintf(r->value, sizeof r->value, "%s", pad_ini_path());
        break;
    case P_REMAP:
        r = add(R_REMAP_BACK, K_ACTION, W_NONE, "< Back", "Back to the controller's settings.");
        if (pad)
            snprintf(r->value, sizeof r->value, "%.80s", pad->name);
        for (int i = 0; pad && i < 16; i++)
        {
            int b = REMAP_ORDER[i];
            char label[24];
            snprintf(label, sizeof label, "%s", pad_button_name(b));
            r = add(R_REMAP_FIRST + b, K_ACTION, W_NOW, label, "A or Enter: then press the control for this button. Y or Delete: its default again.");
            snprintf(r->value, sizeof r->value, "%s", pad->map[b]);
        }
        break;
    case P_KEYS:
        r = add(R_KEYS_BACK, K_ACTION, W_NONE, "< Back", "Back to the controllers' settings.");
        for (int i = 0; i < PAD_KEYS; i++)
        {
            int b = i < 16 ? REMAP_ORDER[i] : i;
            r = add(R_KEY_FIRST + b, K_ACTION, W_NOW, pad_key_label(b), "A or Enter: then press the key for this. Y or Delete: its default again.");
            pad_key_names(b, r->value, sizeof r->value);
        }
        add(R_KEYS_RESET, K_ACTION, W_NOW, "Back to the defaults", "Every key as it was: arrows, Z X C V, A S, Q W, Enter, Backspace, I J K L, Shift to walk.");
        break;
    case P_STICKS:
    {
        r = add(R_STICKS_BACK, K_ACTION, W_NONE, "< Back", "Back to the controller's settings.");
        if (pad)
            snprintf(r->value, sizeof r->value, "%.80s", pad->name);
        if (pad && pad->slot >= 0)
        {
            PadState st;
            pad_get((u32)pad->slot, &st);
            static const char *const STICKS[2] = {"Left stick", "C stick"};
            for (int k = 0; k < 2; k++)
            {
                i32 x = k ? st.rx : st.lx, y = k ? st.ry : st.ly;
                int mag = (int)(hypot((double)x, (double)y) * 100 / 32767);
                r = add(R_NONE, K_INFO, W_NONE, STICKS[k], "The stick as the game gets it, after the calibration and the dead zone.");
                snprintf(r->value, sizeof r->value, "x %d, y %d (%d%%)", x, y, mag > 100 ? 100 : mag);
                for (int a = 0; a < 2; a++)
                {
                    int i = k * 2 + a; /* (PadInfo's axis order: lx ly rx ry lt rt) */
                    char label[24];
                    snprintf(label, sizeof label, "%s %c", STICKS[k], a ? 'y' : 'x');
                    r = add(R_NONE, K_INFO, W_NONE, label,
                            "The axis as this pad reports it, and the least and most seen since it was plugged in (or the recalibration below).");
                    snprintf(r->value, sizeof r->value, "%d (seen %d to %d)", pad->axis[i], pad->axis_lo[i], pad->axis_hi[i]);
                }
            }
            for (int t = 0; t < 2; t++)
            {
                char label[24];
                snprintf(label, sizeof label, "%c trigger", t ? 'R' : 'L');
                r = add(R_NONE, K_INFO, W_NONE, label,
                        "The trigger as this pad reports it, and the least and most seen since it was plugged in (or the recalibration below).");
                snprintf(r->value, sizeof r->value, "%d (seen %d to %d)", pad->axis[4 + t], pad->axis_lo[4 + t], pad->axis_hi[4 + t]);
            }
            PadUsbAxes ua;
            if (pad->usb_port >= 0 && padusb_axes(pad->usb_port, &ua))
            {
                int hz = padusb_rate(pad->usb_port);
                int adapter = !strncmp(pad->guid, "gcadapter", 9); /* (the Wii U adapter: 125 Hz is its full speed) */
                int slow = adapter && hz > 0 && hz < 125;
                r = add(R_NONE, K_INFO, W_NONE, "Adapter speed",
                        slow ? "How many times a second the adapter reports: every report is a chance for an input, so less than 125 Hz "
                               "is added delay. It can reach 125 Hz: plug it into a USB port on the machine itself (not a hub), "
                               "and keep the switch on its Wii U side."
                             : "How many times a second the adapter reports (in its Wii U mode, as read here): 125 Hz is its full speed.");
                if (hz)
                    snprintf(r->value, sizeof r->value, "%d Hz%s", hz, slow ? " (can be 125, plug it into a faster USB port)" : "");
                else
                    snprintf(r->value, sizeof r->value, "measuring...");
                r->warn = slow;
                static const char *const AXES[4] = {"Adapter stick x", "Adapter stick y", "Adapter C-stick x", "Adapter C-stick y"};
                for (int a = 0; a < 4; a++)
                {
                    r = add(R_NONE, K_INFO, W_NONE, AXES[a],
                            "The adapter's own number for this axis (0-255), the centre measured for it, and how far each way reads as fully pushed.");
                    snprintf(r->value, sizeof r->value, "%d (centre %d, -%d/+%d)", ua.raw[a], ua.centre[a], ua.lo[a], ua.hi[a]);
                }
                add(R_PAD_RECAL, K_ACTION, W_NOW, "Recalibrate sticks",
                    "Measures the sticks' centre and reach again: let go of the sticks and triggers, then A or Enter. "
                    "(They are measured as the controller is plugged in, too.)");
            }
            else
            {
                add(R_NONE, K_INFO, W_NONE, "Its calibration",
                    "This pad's own driver's: it measures the sticks as the pad is plugged in (replug it to measure them again).");
            }
            add(R_NONE, K_INFO, W_NONE, "A dash", "A dash needs a flick past about 80% strength: a stick that cannot reach it dashes late, or only walks.");
        }
        break;
    }
    case P_ONLINE:
    {
        const char *launcher = getenv("SSBU_NET_RELAY") && *getenv("SSBU_NET_RELAY") ? "SSBU_NET_RELAY set" : getenv("SSBU_NET_HOST") && *getenv("SSBU_NET_HOST") ? "hosting (launcher)"
                                                                                                                                                                  : NULL;
        r = add(R_NET_STATUS, K_INFO, W_NONE, "Now", "What the game is connected to.");
        char status[512];
        net_status(status, sizeof status);
        char *nl = strchr(status, '\n');
        if (nl)
        {
            *nl = 0;
            help_set(r, "What the game is connected to.\n%s", nl + 1);
        } /* (a refusal's what-differs, the room's players) */
        snprintf(r->value, sizeof r->value, "%.110s", status[0] ? status : net_configured() ? "connecting..."
                                                                                            : "offline");
        r = add(R_NET_MODE, K_CHOICE, W_RESTART, "Play online in",
                "Public lobby: everyone's rooms in Local Wireless. Private: only players with the same code. Your own server: host:port. "
                "Offline: no netplay. This network: a private room hosted on your local network, found by itself.");
        snprintf(r->value, sizeof r->value, "%s", NET_MODES[net.mode]);
        if (net.mode == NET_PRIVATE || net.mode == NET_CUSTOM)
        {
            r = add(R_NET_CODE, K_TEXT, W_RESTART, "Lobby code", "Players with the same code see each other's rooms (letters and digits, no spaces).");
            snprintf(r->value, sizeof r->value, "%s", net.code[0] ? net.code : (net.mode == NET_PRIVATE ? "friends" : "(none)"));
        }
        if (net.mode == NET_CUSTOM)
        {
            r = add(R_NET_SERVER, K_TEXT, W_RESTART, "Server", "The lobby server's address and port, e.g. 192.168.1.20:39420.");
            snprintf(r->value, sizeof r->value, "%s", net.custom[0] ? net.custom : "(none)");
        }
        r = add(R_NET_NAME, K_TEXT, W_RESTART, "Your name", "What other players see (up to 32 letters).");
        snprintf(r->value, sizeof r->value, "%s", net.name[0] ? net.name : "(picked at the first online start)");
        r = add(R_NET_DIRECT, K_CHOICE, W_RESTART, "Direct connections",
                "On: rooms connect players directly when they can (less lag). Off: everything goes through the lobby, so other players never learn your internet address.");
        snprintf(r->value, sizeof r->value, "%s", on_off(net.direct));
        r = add(R_NET_DELAY, K_CHOICE, W_NOW, "Input delay",
                "The frames the game's own netcode waits before your inputs count online (Auto: its own choice, from the connection). Fewer frames answer sooner but stutter on a slow connection. Counts from the next match.");
        {
            int d = online_delay_setting(), a = online_delay_last_auto();
            if (d >= 0)
                snprintf(r->value, sizeof r->value, "%d frame%s", d, d == 1 ? "" : "s");
            else if (a >= 0)
                snprintf(r->value, sizeof r->value, "Auto (%d last match)", a);
            else
                snprintf(r->value, sizeof r->value, "Auto");
        }
#ifdef _WIN32   /* (rollback netplay is Windows' only: hle_rb_core.c) */
        r = add(R_NET_ROLLBACK, K_CHOICE, W_RESTART, "Rollback netcode",
                "On: Quickplay and Local Wireless room matches are played with rollback when the other player has it on too (your inputs answer at once). Off: the game's own netcode.");
        snprintf(r->value, sizeof r->value, "%s", on_off(net.rollback));
#endif
        for (int i = 1; i < nrows; i++)
        {
            if (launcher)
                snprintf(rows[i].note, sizeof rows[i].note, "%s", launcher);
            else if (!settings_online())
                snprintf(rows[i].note, sizeof rows[i].note, "for Play SSBU");
        }
        add_protocol_row(); /* (read with any lobby: no launcher's note) */
        if (restart_pending)
            add(R_RESTART, K_ACTION, W_NONE, "Restart now", "Starts the game again, with the changes. A match in progress is lost.");
        break;
    }
    case P_MODS:
        mods_rows(!same);
        break;
    case P_LOBBY:
        build_lobby(!same);
        if (was == R_NONE)
            break;
        for (int i = 0; i < nrows; i++)
            if (was_host ? IS_ROOM(rows[i].id) && ROOM_AT(rows[i].id)->host == was_host : rows[i].id == was)
            {
                sel[page] = i;
                break;
            }
        break;
    case P_GAME:
    {
        char lang[32] = "";
        r = add(R_LANGUAGE, K_CHOICE, W_RESTART, "Game language",
                "Auto: the language Windows shows its menus in. Chinese, Japanese and Korean text needs your console's firmware fonts.");
        if (!settings_ini_get(display_ini_path(), NULL, "language", lang, sizeof lang) || !lang[0])
            snprintf(lang, sizeof lang, "auto");
        snprintf(r->value, sizeof r->value, "%s", _stricmp(lang, "auto") ? language_pick(lang) : "auto");
        note_env(r, "SSBU_LANGUAGE");
        r = add(R_VOLUME, K_CHOICE, W_NOW, "Volume", "The game's whole sound, over the game's own sound settings; 0% is silent.");
        snprintf(r->value, sizeof r->value, "%d%%", audio_master_volume());
        note_env(r, "SSBU_VOLUME");
    }
        add(R_RESTART, K_ACTION, W_NONE, "Restart the game", "Starts the game again (for the settings marked 'next start'). A match in progress is lost.");
        add(R_QUIT, K_ACTION, W_NONE, "Quit the game", "Closes the game. A match in progress is lost.");
#ifdef __APPLE__
        static const char *const KEYS[][2] = {{"Esc / Home button", "this menu (or hold - and + for a second)"}, {"F1, Cmd+1", "frame rate on / off"}, {"F2, Cmd+2", "details"}, {"F3, Cmd+3", "screenshot (screenshots folder)"}, {"Option+Return", "full screen (or Ctrl+Cmd+F)"}};
#else
        static const char *const KEYS[][2] = {{"Esc / Home button", "this menu (or hold - and + for a second)"}, {"F1", "frame rate on / off"}, {"F2", "details"}, {"F3", "screenshot (screenshots folder)"}, {"F11, Alt+Enter", "full screen"}};
#endif
        for (u32 i = 0; i < sizeof KEYS / sizeof *KEYS; i++)
        {
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

static void start_edit(int id)
{
    editing = id;
    grid_r = grid_c = 0;
    const char *v = id == R_NET_CODE ? net.code : id == R_NET_SERVER ? net.custom
                                                                     : net.name;
    snprintf(edit_buf, sizeof edit_buf, "%s", v);
}

static void finish_edit(void)
{
    if (editing == R_OSK)
    { /* the game's text box: as typed */
        snprintf(osk_text, sizeof osk_text, "%s", edit_buf);
        editing = 0;
        InterlockedExchange(&osk_state, 3);
        close_menu();
        return;
    }
    char *s = edit_buf;
    while (*s == ' ')
        s++;
    size_t n = strlen(s);
    while (n && s[n - 1] == ' ')
        s[--n] = 0;
    if (editing == R_NET_NAME)
    {
        if (*s)
        {
            snprintf(net.name, sizeof net.name, "%.32s", s);
            settings_line_write(config_path(CONFIG_ONLINE_NAME), net.name);
            changed(W_RESTART);
        }
    }
    else if (!strchr(s, ' '))
    {
        if (editing == R_NET_CODE)
            snprintf(net.code, sizeof net.code, "%s", s);
        else if (editing == R_NET_SERVER && *s)
            snprintf(net.custom, sizeof net.custom, "%s", s);
        net_save_server();
        changed(W_RESTART);
    }
    else
        menu_toast("No spaces in a lobby code or a server's address");
    editing = 0;
}

static void change(const Row *row, int dir)
{
    if (row->id == R_MOD_CODE || (row->id >= R_MOD_FIRST && row->id < R_MOD_FIRST + nmenu_mods))
    {
        (void)dir;
        mod_flip(row);
        return;
    }
    DisplaySettings ds;
    display_settings(&ds);
    PadInfo *pad = pad_slot >= 0 && pads[pad_slot].connected ? &pads[pad_slot] : NULL;
    char v[64];
    switch (row->id)
    {
    case R_WINDOW:
    {
        int m = cycle(ds.mode, 3, dir);
        display_set_mode(m);
        set_display("window", m == DISPLAY_BORDERLESS ? "borderless" : m == DISPLAY_FULLSCREEN ? "fullscreen"
                                                                                               : "windowed");
        break;
    }
    case R_PACING:
    {
        Pacing p = (Pacing)cycle(ds.pacing, 3, dir);
        display_set_pacing(p);
        set_display("pacing", p == PACING_VRR ? "vrr" : p == PACING_DISPLAY ? "display"
                                                                            : "clock");
        break;
    }
    case R_LATENCY:
        snprintf(v, sizeof v, "%d", cycle(ds.latency - 1, 3, dir) + 1);
        display_set_latency(atoi(v));
        set_display("latency", v);
        break;
    case R_RUNAHEAD: /* off, 1, 2 (3 only by hand: display.ini or SSBU_RUNAHEAD) */
        snprintf(v, sizeof v, "%d", cycle(display_runahead() > 2 ? 2 : display_runahead(), 3, dir));
        display_set_runahead(atoi(v));
        set_display("runahead", v);
        break;
    case R_VOLUME:
    { /* 0 to 100 in steps of 10 */
        int vol = (audio_master_volume() + 5) / 10;
        vol = cycle(vol, 11, dir) * 10;
        audio_set_master_volume(vol);
        snprintf(v, sizeof v, "%d", vol);
        set_display("volume", v);
        break;
    }
    case R_LANGUAGE:
    { /* auto, then the game's codes */
        char lang[32] = "";
        int i = -1;
        if (settings_ini_get(display_ini_path(), NULL, "language", lang, sizeof lang) && lang[0] && _stricmp(lang, "auto"))
            for (int k = 0; k < LANGUAGE_COUNT; k++)
                if (!strcmp(language_pick(lang), LANGUAGE_CODES[k]))
                    i = k;
        i = cycle(i + 1, LANGUAGE_COUNT + 1, dir) - 1;
        set_display("language", i < 0 ? "auto" : LANGUAGE_CODES[i]);
        break;
    }
    case R_CURSOR:
        display_set_hide_cursor(!ds.hide_cursor);
        set_display("hide_cursor", ds.hide_cursor ? "0" : "1");
        break;
    case R_OVERLAY:
        if (overlay_on)
            *overlay_on = !*overlay_on;
        set_display("overlay", overlay_on && *overlay_on ? "1" : "0");
        break;
    case R_SIZE:
    {
        int n = sizeof SIZES / sizeof *SIZES, i = -1;
        for (int k = 0; k < n; k++)
            if (SIZES[k][0] == ds.width && SIZES[k][1] == ds.height)
                i = k;
        if (i >= 0)
            i = cycle(i, n, dir);
        else
        { /* (a size typed by hand: the next one up or down) */
            i = 0;
            while (i < n && SIZES[i][0] * SIZES[i][1] < ds.width * ds.height)
                i++;
            i = cycle(dir < 0 ? i - 1 : i, n, 0);
        }
        snprintf(v, sizeof v, "%d", SIZES[i][0]);
        set_display("width", v);
        snprintf(v, sizeof v, "%d", SIZES[i][1]);
        set_display("height", v);
        break;
    }
    case R_SCALE:
    {
        static const char *const SCALES[] = {"1", "auto", "1.5", "2", "3"};
        static const float VALUES[] = {1, 0, 1.5f, 2, 3};
        int n = sizeof SCALES / sizeof *SCALES, i = -1;
        for (int k = 0; k < n; k++)
            if (VALUES[k] == ds.render_scale)
                i = k;
        i = i < 0 ? (dir > 0 ? 0 : n - 1) : cycle(i, n, dir);
        set_display("render_scale", SCALES[i]);
        display_set_render_scale(VALUES[i]); /* (shown now; the renderer takes it at the next start) */
        break;
    }
    case R_RENDERER:
    { /* the system's own, then vulkan */
        int next = cycle(ds.renderer == RENDERER_VULKAN, 2, dir) ? RENDERER_VULKAN : RENDERER_DEFAULT;
        set_display("renderer", renderer_name(next));
        display_set_renderer(next); /* (shown now; the renderer is chosen at the next start) */
        break;
    }
#ifdef _WIN32
    case R_GPU:
    {
        int i = -1; /* auto, then the cards */
        for (int k = 0; k < ngpus; k++)
            if (!_stricmp(ds.gpu, gpus[k]))
                i = k;
        if (i < 0 && _stricmp(ds.gpu, "auto"))
            i = ngpus; /* (something typed by hand: next is auto) */
        i = cycle(i + 1, ngpus + 1, dir) - 1;
        set_display("gpu", i < 0 ? "auto" : gpus[i]);
        break;
    }
#endif
    case R_PAD:
    {
        int start = pad_slot < 0 ? 0 : pad_slot;
        for (int k = 1; k <= PAD_SLOTS; k++)
        {
            int s = cycle(start, PAD_SLOTS, dir * k);
            if (pads[s].connected)
            {
                pad_slot = s;
                break;
            }
        }
        return;
    }
    case R_PAD_PLAYER:
    {
        if (!pad)
            return;
        int pin = cycle(pad_pin(pad), PAD_SLOTS + 1, dir);
        for (int k = 1; k <= PAD_SLOTS; k++)
        { /* any old pin of this pad goes */
            snprintf(v, sizeof v, "%d", k);
            if (pad_matches(pad, pins[k - 1]))
                settings_ini_set(pad_ini_path(), "players", v, NULL);
        }
        if (pin)
        {
            snprintf(v, sizeof v, "%d", pin);
            settings_ini_set(pad_ini_path(), "players", v, pad->guid);
        }
        load_pins();
        pad_reload();
        break;
    }
    case R_PAD_LAYOUT:
        if (pad)
            set_pad(pad, "layout", pad->layout ? "positional" : "labels");
        break;
    case R_PAD_DEADZONE:
        if (pad)
        {
            snprintf(v, sizeof v, "%d", cycle(pad->deadzone / 5, 11, dir) * 5);
            set_pad(pad, "deadzone", v);
        }
        break;
    case R_PAD_RUMBLE:
        if (pad)
        {
            snprintf(v, sizeof v, "%d", cycle(pad->rumble / 10, 11, dir) * 10);
            set_pad(pad, "rumble", v);
        }
        break;
    case R_PAD_TRIGGER:
        if (pad)
        {
            snprintf(v, sizeof v, "%d", cycle(pad->trigger / 5 - 1, 19, dir) * 5 + 5);
            set_pad(pad, "trigger_threshold", v);
        }
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
        net.mode = cycle(net.mode, NET_MODE_COUNT, dir);
        net_save_server();
        break;
    case R_NET_DIRECT:
        net.direct = !net.direct;
        settings_line_write(config_path(CONFIG_ONLINE_DIRECT), net.direct ? "on" : "off");
        break;
    case R_NET_DELAY: /* Auto, 0 .. 25, round */
    {
        int d = online_delay_setting() + dir;
        if (d < -1)
            d = 25;
        if (d > 25)
            d = -1;
        char v[8];
        snprintf(v, sizeof v, "%d", d);
        settings_line_write(config_path(CONFIG_ONLINE_DELAY), d < 0 ? "auto" : v);
        break;
    }
    case R_NET_ROLLBACK:
        net.rollback = !net.rollback;
        settings_line_write(config_path(CONFIG_ONLINE_ROLLBACK), net.rollback ? "on" : "off");
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

static void action(const Row *row)
{
    PadInfo *pad = pad_slot >= 0 && pads[pad_slot].connected ? &pads[pad_slot] : NULL;
    switch (row->kind)
    {
    case K_CHOICE:
        change(row, 1);
        return;
    case K_TEXT:
        start_edit(row->id);
        return;
    }
    if (IS_KEY(row->id))
    {
        capture_key = row->id - R_KEY_FIRST;
        capture_key_held = 1; /* (Enter, or whatever chose the row, goes first) */
        capture_until = GetTickCount64() + 6000;
        return;
    }
    if (IS_REMAP(row->id) && pad)
    {
        capture_button = row->id - R_REMAP_FIRST;
        capture_held = ~0u; /* (whatever is held now must go first) */
        capture_until = GetTickCount64() + 6000;
        return;
    }
    if (IS_ROOM(row->id))
    { /* a room: put it first in the game's own Rooms List, where it is joined */
        const NetRoom *x = ROOM_AT(row->id);
        const char *host = x->name[0] ? x->name : "(no name)";
        char t[160];
        if (x->mine)
            snprintf(t, sizeof t, "This is your own room");
        else if (!x->joinable)
            snprintf(t, sizeof t, "Can't join %.32s's room: %s", host, room_short(x));
        else if (!x->open)
            snprintf(t, sizeof t, "%.32s's room is full or closed for now", host);
        else
        {
            net_rooms_pick(x->host);
            snprintf(t, sizeof t, "%.32s's room comes first now: close this menu, then Local Wireless > Rooms List", host);
            changed(W_NONE);
        }
        menu_toast(t);
        return;
    }
    switch (row->id)
    {
    case R_MATCH_FIND:
        if (match.state == NET_MATCH_ASKING || match.state == NET_MATCH_SEARCHING)
            menu_toast("Searching already");
        else if (match.state == NET_MATCH_MATCHED)
        {
            char t[160];
            snprintf(t, sizeof t, match.role == NET_MATCH_HOST ? "You host: Local Wireless > Create a Room (%.32s joins it)" : "%.32s hosts: their room comes first in Local Wireless > Rooms List", who(match.peer_name));
            menu_toast(t);
        }
        else
        {
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
    case R_ROOMS:
        net_rooms_ask();
        break;
    case R_ROOMS_PREV:
        room_view = room_view > ROOM_PAGE ? room_view - ROOM_PAGE : 0;
        changed(W_NONE);
        break;
    case R_ROOMS_NEXT:
        if (room_view + ROOM_PAGE < nlobby_rooms)
            room_view += ROOM_PAGE;
        changed(W_NONE);
        break;
    case R_ROOMS_GAME_PAGE:
        net_rooms_page((net_rooms_page_get() + 1) % game_pages);
        changed(W_NONE);
        break;
    case R_LAN_ROOMS:
    {
        int ours = 0, lan = net_lan(&ours, NULL);
        if (lan == NET_LAN_ANNOUNCING)
            menu_toast("Your room is announced on this network");
        else if (lan == NET_LAN_QUIET)
            menu_toast("Not announced: Direct connections are off (Online page)");
        else if (lan == NET_LAN_FOUND && net_local_ip())
            menu_toast("In a room on this network: Local Wireless > Rooms List");
        else if (ours)
            menu_toast("Joining the room on this network...");
        else
            menu_toast("Still looking: a room shows here once a game on this network hosts one");
        break;
    }
    case R_PAD_REMAP:
        if (pad)
        {
            page = P_REMAP;
            sel[P_REMAP] = 1;
            top[P_REMAP] = 0;
        }
        break;
    case R_PAD_STICKS:
        if (pad)
        {
            page = P_STICKS;
            sel[P_STICKS] = 0;
            top[P_STICKS] = 0;
        }
        break;
    case R_PAD_RECAL:
        if (pad && pad->usb_port >= 0)
        {
            padusb_recalibrate(pad->usb_port);
            pad_reset_axes((u32)pad->slot);
            menu_toast("Recalibrating: keep the sticks and triggers still for a moment");
        }
        break;
    case R_REMAP_BACK:
    case R_KEYS_BACK:
    case R_STICKS_BACK:
        page = P_PADS;
        break;
    case R_KEYS:
        page = P_KEYS;
        sel[P_KEYS] = 1;
        top[P_KEYS] = 0;
        break;
    case R_KEYS_RESET:
        for (int i = 0; i < PAD_KEYS; i++)
            settings_ini_set(pad_ini_path(), "keyboard", pad_key_label(i), NULL);
        pad_reload();
        menu_toast("The keyboard's keys are back to the defaults");
        break;
    case R_PAD_TEST:
        if (pad && pad->slot >= 0)
        {
            pad_rumble((u32)pad->slot, 1, 0.8f);
            pad_rumble((u32)pad->slot, 2, 0.8f);
            rumble_slot = pad->slot;
            rumble_stop = GetTickCount64() + 500;
        }
        break;
    case R_PAD_RESET:
        if (pad)
        {
            static const char *const KEYS[] = {"layout", "deadzone", "rumble", "trigger_threshold"};
            char section[128];
            pad_section(pad, section, sizeof section);
            for (u32 i = 0; i < sizeof KEYS / sizeof *KEYS; i++)
                settings_ini_set(pad_ini_path(), section, KEYS[i], NULL);
            for (int b = 0; b < 16; b++)
                settings_ini_set(pad_ini_path(), section, pad_button_name(b), NULL);
            pad_reload();
            menu_toast("This controller is back to the defaults");
        }
        break;
    case R_PADS_FILE:
    {
#ifdef _WIN32
        char cmd[MAX_PATH + 32];
        snprintf(cmd, sizeof cmd, "notepad.exe \"%s\"", pad_ini_path());
        STARTUPINFOA si = {sizeof si};
        PROCESS_INFORMATION pi;
        if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
#else
        char cmd[MAX_PATH + 64];
#ifdef __APPLE__
        snprintf(cmd, sizeof cmd, "open -t '%s' >/dev/null 2>&1 &", pad_ini_path()); /* (the default text editor) */
#else
        snprintf(cmd, sizeof cmd, "xdg-open '%s' >/dev/null 2>&1 &", pad_ini_path());
#endif
        if (system(cmd))
        {
        }
#endif
        menu_toast("controllers.ini opened in another window (changes there count at the next start)");
        break;
    }
    case R_RESTART:
    case R_QUIT:
        confirm = row->id;
        break;
    case R_MODS_OFF:
        if (!mods_switch_set("--all", 0))
        {
            menu_toast("Still changing the last one: a moment");
            break;
        }
        for (int i = 0; i < nmenu_mods; i++)
            menu_mods[i].on = 0;
        changed(W_RESTART);
        break;
    }
}

/* ---- pads and keys -------------------------------------------------------------------- */
static u64 nav_held;
static int nav_wait_release; /* after a control was taken for a button: until everything is let go */
static ULONGLONG nav_next;
static ULONGLONG combo_since;
static int combo_armed = 1;
static u32 guide_held;

enum
{
    NB_UP = 1,
    NB_DOWN = 2,
    NB_LEFT = 4,
    NB_RIGHT = 8,
    NB_A = 16,
    NB_B = 32,
    NB_L = 64,
    NB_R = 128,
    NB_Y = 256,
    NB_PLUS = 512
};

/* Every pad's buttons as the menu's: the d-pad or left stick, A, B, L/ZL, R/ZR, Y, +. */
static u64 pad_nav(int *combo, u32 *guide)
{
    u64 nb = 0;
    *combo = 0;
    *guide = 0;
    for (u32 s = 0; s < PAD_SLOTS; s++)
    {
        PadState p;
        if (!pad_get(s, &p))
            continue;
        u64 b = p.buttons;
        if (b & 1ull << 13 || p.ly > 16384)
            nb |= NB_UP;
        if (b & 1ull << 15 || p.ly < -16384)
            nb |= NB_DOWN;
        if (b & 1ull << 12 || p.lx < -16384)
            nb |= NB_LEFT;
        if (b & 1ull << 14 || p.lx > 16384)
            nb |= NB_RIGHT;
        if (b & 1ull << 0)
            nb |= NB_A;
        if (b & 1ull << 1)
            nb |= NB_B;
        if (b & (1ull << 6 | 1ull << 8))
            nb |= NB_L;
        if (b & (1ull << 7 | 1ull << 9))
            nb |= NB_R;
        if (b & 1ull << 3)
            nb |= NB_Y;
        if (b & 1ull << 10)
            nb |= NB_PLUS;
        if ((b & 1ull << 10) && (b & 1ull << 11))
            *combo = 1;
        if (pads[s].connected && pads[s].raw & 1u << PAD_RAW_GUIDE_BIT)
            *guide = 1;
    }
    return nb;
}

static int keys_held(void)
{
    static const int VK[] = {VK_RETURN, VK_ESCAPE, VK_BACK, VK_SPACE, VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, 'Z', 'X', 'C', 'V'};
    for (u32 i = 0; i < sizeof VK / sizeof *VK; i++)
        if (GetAsyncKeyState(VK[i]) & 0x8000)
            return 1;
    return 0;
}

/* A key the keyboard page can bind, held now (the lowest code), or 0. Not Esc (it cancels) nor the
 * F keys (the debug and rollback keys). */
static int menu_key_pressed(void)
{
    static const int OTHER[] = {VK_LEFT, VK_UP, VK_RIGHT, VK_DOWN, VK_RETURN, VK_BACK, VK_SPACE, VK_TAB, VK_SHIFT, VK_CONTROL, VK_MENU};
    for (u32 i = 0; i < sizeof OTHER / sizeof *OTHER; i++)
        if (GetAsyncKeyState(OTHER[i]) & 0x8000)
            return OTHER[i];
    for (int vk = '0'; vk <= 'Z'; vk++)
        if ((vk <= '9' || vk >= 'A') && GetAsyncKeyState(vk) & 0x8000)
            return vk;
    for (int vk = 0x60; vk <= 0x69; vk++) /* (the number pad's digits) */
        if (GetAsyncKeyState(vk) & 0x8000)
            return vk;
    return 0;
}

/* ---- the text grid (for pads) ------------------------------------------------------------- */
static const char *const GRID[] = {"ABCDEFGHIJKLM", "NOPQRSTUVWXYZ", "abcdefghijklm", "nopqrstuvwxyz", "0123456789-_.", ": "};
enum
{
    GRID_ROWS = 6,
    GRID_COLS = 13
};
static const char *const GRID_KEYS[] = {"space", "delete", "done", "cancel"}; /* the last row, after ':' */

static int grid_cols(int r) { return r == GRID_ROWS - 1 ? 1 + 4 : GRID_COLS; }

static void edit_event(int ev, unsigned ch)
{
    size_t n = strlen(edit_buf);
    size_t max = editing == R_NET_NAME ? 32 : editing == R_NET_SERVER ? 60
                                          : editing == R_OSK          ? (size_t)osk_max
                                                                      : 32;
    switch (ev)
    {
    case EV_CHAR:
        if (n < max)
        {
            edit_buf[n] = (char)ch;
            edit_buf[n + 1] = 0;
        }
        break;
    case EV_BKSP:
    case EV_B:
    case EV_DEL:
        if (n)
            edit_buf[n - 1] = 0;
        break;
    case EV_ENTER:
    case EV_DONE:
        finish_edit();
        break;
    case EV_ESC:
    case EV_PREV:
        editing = 0;
        break;
    case EV_UP:
        grid_r = cycle(grid_r, GRID_ROWS, -1);
        if (grid_c >= grid_cols(grid_r))
            grid_c = grid_cols(grid_r) - 1;
        break;
    case EV_DOWN:
        grid_r = cycle(grid_r, GRID_ROWS, 1);
        if (grid_c >= grid_cols(grid_r))
            grid_c = grid_cols(grid_r) - 1;
        break;
    case EV_LEFT:
        grid_c = cycle(grid_c, grid_cols(grid_r), -1);
        break;
    case EV_RIGHT:
        grid_c = cycle(grid_c, grid_cols(grid_r), 1);
        break;
    case EV_A:
        if (grid_r < GRID_ROWS - 1)
            edit_event(EV_CHAR, (unsigned char)GRID[grid_r][grid_c]);
        else if (grid_c == 0)
            edit_event(EV_CHAR, ':');
        else if (grid_c == 1)
            edit_event(EV_CHAR, ' ');
        else if (grid_c == 2)
            edit_event(EV_DEL, 0);
        else if (grid_c == 3)
            finish_edit();
        else
            editing = 0;
        break;
    }
}

static void move_sel(int dir)
{
    int *s = &sel[page];
    for (int k = 0; k < nrows; k++)
    {
        int i = cycle(*s, nrows, dir * (k + 1));
        if (selectable(&rows[i]))
        {
            *s = i;
            return;
        }
    }
}

static void event(int ev, unsigned ch)
{
    if (confirm)
    {
        if (ev == EV_A || ev == EV_ENTER)
        {
            int what = confirm;
            confirm = 0;
            if (what == R_RESTART)
                settings_restart();
            else
            {
                fprintf(stderr, "[menu] quit\n");
                gfx_closing();
                fflush(stderr);
                ExitProcess(0);
            }
        }
        else if (ev == EV_B || ev == EV_BKSP || ev == EV_ESC)
            confirm = 0;
        return;
    }
    if (editing)
    {
        int was = editing;
        edit_event(ev, ch);
        if (was == R_OSK && !editing && osk_state == 2)
            close_menu(); /* (cancelled: close_menu says so) */
        return;
    }
    if (capture_button >= 0)
    {
        if (ev == EV_ESC)
            capture_button = -1;
        return;
    }
    if (capture_key >= 0)
    {
        if (ev == EV_ESC)
            capture_key = -1;
        return;
    }
    Row *row = sel[page] < nrows ? &rows[sel[page]] : NULL;
    switch (ev)
    {
    case EV_UP:
        move_sel(-1);
        break;
    case EV_DOWN:
        move_sel(1);
        break;
    case EV_LEFT:
    case EV_RIGHT:
        if (row && row->kind == K_CHOICE)
            change(row, ev == EV_RIGHT ? 1 : -1);
        break;
    case EV_A:
    case EV_ENTER:
        if (row && selectable(row))
            action(row);
        break;
    case EV_DEL:
        if (page == P_REMAP && row && IS_REMAP(row->id) && pad_slot >= 0 && pads[pad_slot].connected)
        {
            char section[128];
            pad_section(&pads[pad_slot], section, sizeof section);
            settings_ini_set(pad_ini_path(), section, pad_button_name(row->id - R_REMAP_FIRST), NULL);
            pad_reload();
        }
        if (page == P_KEYS && row && IS_KEY(row->id))
        {
            settings_ini_set(pad_ini_path(), "keyboard", pad_key_label(row->id - R_KEY_FIRST), NULL);
            pad_reload();
        }
        break;
    case EV_B:
    case EV_BKSP:
        if (SUB_PAGE(page))
            page = P_PADS;
        else
            close_menu();
        break;
    case EV_ESC:
        close_menu();
        break;
    case EV_NEXT:
    case EV_PREV:
        page = SUB_PAGE(page) ? P_PADS : cycle(page, PAGES, ev == EV_NEXT ? 1 : -1);
        break;
    case EV_CHAR:
        if (ch >= '1' && ch < '1' + PAGES)
            page = (int)(ch - '1');
        else if (ch == ' ' && row && selectable(row))
            action(row);
        break;
    }
}

/* SSBU_MENU_AT / SSBU_MENU_KEYS (tests) */
static int script_events(void)
{
    static struct
    {
        double at;
        int ev;
        char text[48];
        int done;
    } steps[64];
    static int nsteps = -1, opened;
    static double open_at = -1;
    int shot = 0;
    if (nsteps < 0)
    {
        nsteps = 0;
        const char *a = getenv("SSBU_MENU_AT");
        open_at = a && *a ? atof(a) : -1;
        const char *s = getenv("SSBU_MENU_KEYS");
        static const struct
        {
            const char *name;
            int ev;
        } NAMES[] = {{"UP", EV_UP}, {"DOWN", EV_DOWN}, {"LEFT", EV_LEFT}, {"RIGHT", EV_RIGHT}, {"ENTER", EV_ENTER}, {"ESC", EV_ESC}, {"TAB", EV_NEXT}, {"BKSP", EV_BKSP}, {"DEL", EV_DEL}, {"SHOT", -1}};
        while (s && *s && nsteps < 64)
        {
            char tok[64];
            int n = 0;
            if (sscanf(s, "%lf:%63[^,]%n", &steps[nsteps].at, tok, &n) < 2)
                break;
            s += n;
            if (*s == ',')
                s++;
            if (tok[0] == '=')
            {
                steps[nsteps].ev = EV_CHAR;
                snprintf(steps[nsteps].text, sizeof steps[nsteps].text, "%s", tok + 1);
                nsteps++;
                continue;
            }
            for (u32 i = 0; i < sizeof NAMES / sizeof *NAMES; i++)
                if (!strcmp(tok, NAMES[i].name))
                {
                    steps[nsteps++].ev = NAMES[i].ev;
                    break;
                }
        }
    }
    double now = rt_now_ns() / 1e9;
    if (open_at >= 0 && !opened && now >= open_at)
    {
        opened = 1;
        open_menu();
    }
    for (int i = 0; i < nsteps; i++)
    {
        if (steps[i].done || now < steps[i].at)
            continue;
        steps[i].done = 1;
        if (steps[i].ev < 0)
            shot = 1;
        else if (steps[i].ev == EV_CHAR)
            for (const char *c = steps[i].text; *c; c++)
                event(EV_CHAR, (unsigned char)*c);
        else if (steps[i].ev == EV_ESC && !open_)
            open_menu();
        else
            event(steps[i].ev, 0);
    }
    return shot;
}

int menu_frame(int *overlay, int f3)
{
    ULONGLONG now = GetTickCount64();
    overlay_on = overlay;
    for (u32 s = 0; s < PAD_SLOTS; s++)
        pad_info(s, &pads[s]);
    npads_connected = 0;
    for (u32 s = 0; s < PAD_SLOTS; s++)
        npads_connected += pads[s].connected;
    if ((pad_slot < 0 || !pads[pad_slot].connected) && npads_connected)
        for (int s = 0; s < PAD_SLOTS; s++)
            if (pads[s].connected)
            {
                pad_slot = s;
                break;
            }
    if (rumble_slot >= 0 && now >= rumble_stop)
    {
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
    if (combo && combo_armed && (focused || open_))
    {
        if (!combo_since)
            combo_since = now;
        if (now - combo_since >= 1000)
        {
            toggle = 1;
            combo_armed = 0;
        }
    }
    else if (!combo)
    {
        combo_since = 0;
        combo_armed = 1;
    }
    int shot = script_events() || (f3 && open_);
    if (osk_state == 1)
    { /* the game asked for text: the menu opens on the grid */
        if (!open_)
            open_menu();
        editing = R_OSK;
        grid_r = grid_c = 0;
        snprintf(edit_buf, sizeof edit_buf, "%s", osk_text);
        nav_held = nb;
        InterlockedExchange(&osk_state, 2);
    }
    if (open_)
        build();
    while (q_tail != q_head)
    {
        LONG v = q[q_tail & 63];
        InterlockedIncrement(&q_tail);
        if (now < capture_quiet)
            continue; /* (the key just bound: not also a press in the menu) */
        if (v & 0x10000)
        {
            if (open_)
                event(EV_CHAR, (unsigned)(v & 0xFF));
            continue;
        }
        static const int EV_OF[] = {0, EV_ESC, EV_UP, EV_DOWN, EV_LEFT, EV_RIGHT, EV_ENTER, EV_BKSP, EV_NEXT, EV_PREV, EV_DEL};
        if (v == MENU_KEY_ESC && !open_)
        {
            open_menu();
            continue;
        }
        if (open_ && v > 0 && v < (LONG)(sizeof EV_OF / sizeof *EV_OF))
            event(EV_OF[v], 0);
        if (open_)
            build();
    }
    if (toggle && !open_)
        open_menu();
    else if (toggle)
        close_menu();
    if (toggle)
    {
        nav_held = nb; /* (what opened it is not also a press in it) */
        if (open_)
            build();
    }
    if (open_ && capture_key >= 0)
    { /* a new key for a binding: the first one pressed after all are let go */
        int vk = menu_key_pressed();
        if (now > capture_until)
            capture_key = -1;
        else if (capture_key_held)
        {
            if (!vk)
                capture_key_held = 0;
        }
        else if (vk)
        {
            char name[16];
            pad_key_name(vk, name, sizeof name);
            settings_ini_set(pad_ini_path(), "keyboard", pad_key_label(capture_key), name);
            pad_reload();
            fprintf(stderr, "[menu] keyboard: %s = %s\n", pad_key_label(capture_key), name);
            capture_key = -1;
            capture_quiet = now + 300;
            nav_wait_release = 1;
        }
        nav_held = nb;
    }
    else if (open_ && capture_button >= 0)
    { /* a new control for a button: the first one pressed after all are let go */
        PadInfo *pad = pad_slot >= 0 && pads[pad_slot].connected ? &pads[pad_slot] : NULL;
        u32 raw = pad ? pad->raw : 0;
        if (!pad || now > capture_until)
            capture_button = -1;
        else if (capture_held == ~0u)
        {
            if (!raw)
                capture_held = 0;
        }
        else if (raw)
        {
            int bit = 0;
            while (!(raw >> bit & 1))
                bit++;
            char section[128];
            pad_section(pad, section, sizeof section);
            settings_ini_set(pad_ini_path(), section, pad_button_name(capture_button), pad_raw_name(bit));
            pad_reload();
            capture_button = -1;
            nav_wait_release = 1;
        }
        nav_held = nb;
    }
    else if (open_ && nav_wait_release)
    {
        if (!nb)
            nav_wait_release = 0;
        nav_held = nb;
    }
    else if (open_)
    {
        const u64 DIRS = NB_UP | NB_DOWN | NB_LEFT | NB_RIGHT;
        u64 pressed = nb & ~nav_held, dirs = nb & DIRS;
        if (pressed & DIRS)
            nav_next = now + 400; /* held: repeats after 0.4 s, every 80 ms */
        else if (dirs && now >= nav_next)
        {
            pressed |= dirs;
            nav_next = now + 80;
        }
        nav_held = nb;
        static const struct
        {
            u64 bit;
            int ev;
        } MAP[] = {{NB_UP, EV_UP}, {NB_DOWN, EV_DOWN}, {NB_LEFT, EV_LEFT}, {NB_RIGHT, EV_RIGHT}, {NB_A, EV_A}, {NB_B, EV_B}, {NB_L, EV_PREV}, {NB_R, EV_NEXT}, {NB_Y, EV_DEL}, {NB_PLUS, EV_DONE}};
        for (u32 i = 0; i < sizeof MAP / sizeof *MAP && open_; i++)
            if (pressed & MAP[i].bit)
            {
                event(MAP[i].ev, 0);
                build();
            }
    }
    if (!open_)
        nav_held = nb;
    if (releasing && !open_ && !nb && !keys_held())
        releasing = 0;
    match_follow();
    if (open_)
    {
        build();
        if (sel[page] >= nrows)
            sel[page] = nrows - 1;
        if (sel[page] < 0)
            sel[page] = 0;
        if (nrows && !selectable(&rows[sel[page]]))
            move_sel(1);
    }
    return shot ? MENU_SHOT : 0;
}

/* ---- drawing ------------------------------------------------------------------------- */
static float text_w(const char *s, float size) { return (float)strlen(s) * size / 2; }

/* s wrapped to lines of at most `cols` characters, into out (a line s starts itself wrapped on its own). */
static void wrap(const char *s, int cols, char *out, size_t size)
{
    size_t o = 0;
    while (*s && o + 2 < size)
    {
        int n = (int)strcspn(s, "\n");
        if (n <= cols)
        {
            o += (size_t)snprintf(out + o, size - o, "%.*s%s", n, s, s[n] ? "\n" : "");
            s += n + (s[n] != 0);
            continue;
        }
        int cut = cols;
        while (cut > 0 && s[cut] != ' ')
            cut--;
        if (!cut)
            cut = cols;
        o += (size_t)snprintf(out + o, size - o, "%.*s\n", cut, s);
        s += cut;
        while (*s == ' ')
            s++;
    }
    out[o < size ? o : size - 1] = 0;
}

static void box(float x, float y, float w, float h, float s)
{
    ovl_rect(x, y, w, h, OVL_RGBA(14, 16, 24, 245));
    ovl_rect(x, y, w, 3 * s, OVL_RGBA(225, 60, 60, 255));
}

/* The message showing now (its time starting at the first look), or 0. */
static int toast_now(char *t, size_t size)
{
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&toast_lock);
    if (toast_until == 1)
        toast_until = now + 4000;
    int show = toast_until && now < toast_until;
    snprintf(t, size, "%s", toast);
    ReleaseSRWLockExclusive(&toast_lock);
    return show;
}

/* A question or an editor over the menu: the menu dimmed, the box lighter and solid. */
static void modal_box(float w, float h, float x, float y, float bw, float bh, float s)
{
    ovl_rect(0, 0, w, h, OVL_RGBA(0, 0, 0, 120));
    ovl_rect(x, y, bw, bh, OVL_RGBA(30, 34, 48, 255));
    ovl_rect(x, y, bw, 3 * s, OVL_RGBA(225, 60, 60, 255));
}

static void draw_toast(float w, float h, float s)
{
    char t[160];
    if (!toast_now(t, sizeof t))
        return;
    float size = 28 * s, tw = text_w(t, size) + 48 * s, th = size + 28 * s;
    float x = (w - tw) / 2, y = h - th - 40 * s;
    box(x, y, tw, th, s);
    ovl_text(x + 24 * s, y + 14 * s, size, OVL_RGBA(240, 240, 240, 255), t);
}

void menu_draw(float w, float h)
{
    float s = h / 1080.0f;
    if (!open_)
    {
        draw_toast(w, h, s);
        return;
    }
    const unsigned WHITE = OVL_RGBA(240, 240, 245, 255), GREY = OVL_RGBA(150, 155, 170, 255), DIM = OVL_RGBA(110, 115, 130, 255),
                   GREEN = OVL_RGBA(120, 220, 140, 255), AMBER = OVL_RGBA(245, 190, 90, 255), ACCENT = OVL_RGBA(225, 60, 60, 255);
    ovl_rect(0, 0, w, h, OVL_RGBA(0, 0, 0, 150));
    float px = w * 0.06f, py = h * 0.06f, pw = w * 0.88f, ph = h * 0.88f;
    box(px, py, pw, ph, s);
    ovl_text(px + 32 * s, py + 22 * s, 44 * s, WHITE, "Settings");
    char banner[200] = "";
    char status[512];
    net_status(status, sizeof status);
    if (strstr(status, "Room:"))
        snprintf(banner, sizeof banner, "You are in a netplay room: the game keeps running while this menu is open.");
    else if (!strncmp(status, "Netplay: ", 9) && strchr(status, '\n'))
    { /* (refused, with what differs: its first line) */
        const char *nl = strchr(status, '\n') + 1;
        snprintf(banner, sizeof banner, "Can't join: %.*s (Online > Now)", (int)strcspn(nl, "\n"), nl);
    }
    else if (restart_pending)
        snprintf(banner, sizeof banner, "Some changes take effect when the game starts again: Game > Restart the game.");
    char t[160];
    if (toast_now(t, sizeof t))
        ovl_text(px + 300 * s, py + 34 * s, 24 * s, WHITE, t); /* (a message, in the menu: here) */
    else if (banner[0])
        ovl_text(px + 300 * s, py + 34 * s, 24 * s, strstr(status, "Room:") ? ACCENT : AMBER, banner);
    /* the pages */
    float ty = py + 110 * s;
    for (int i = 0; i < PAGES; i++)
    {
        int on = page == i || (SUB_PAGE(page) && i == P_PADS);
        if (on)
            ovl_rect(px + 20 * s, ty - 6 * s, 250 * s, 50 * s, OVL_RGBA(55, 62, 90, 255));
        char t[32];
        snprintf(t, sizeof t, "%d %s", i + 1, PAGE_NAMES[i]);
        ovl_text(px + 36 * s, ty, 32 * s, on ? WHITE : GREY, t);
        ty += 60 * s;
    }
    /* the rows */
    float rx = px + 300 * s, ry = py + 110 * s, rw = px + pw - 28 * s - rx, rh = 50 * s, size = 29 * s;
    float help_y = py + ph - 150 * s;
    int fit = (int)((help_y - 16 * s - ry) / rh);
    if (fit < 1)
        fit = 1;
    int *t0 = &top[page];
    if (sel[page] < *t0)
        *t0 = sel[page];
    if (sel[page] >= *t0 + fit)
        *t0 = sel[page] - fit + 1;
    if (*t0 > nrows - fit)
        *t0 = nrows - fit > 0 ? nrows - fit : 0;
    float vx = rx + rw * 0.40f;
    for (int i = *t0; i < nrows && i < *t0 + fit; i++)
    {
        const Row *r = &rows[i];
        float y = ry + (float)(i - *t0) * rh;
        int on = i == sel[page];
        if (on)
            ovl_rect(rx, y - 6 * s, rw, rh - 4 * s, OVL_RGBA(55, 62, 90, 255));
        ovl_text(rx + 16 * s, y, size, r->kind == K_INFO ? GREY : WHITE, r->label);
        char v[160];
        if (r->kind == K_CHOICE && on)
            snprintf(v, sizeof v, "< %s >", r->value);
        else
            snprintf(v, sizeof v, "%s", r->value);
        const char *tag = r->note[0] ? r->note : r->when == W_NOW   ? "now"
                                             : r->when == W_RESTART ? "next start"
                                                                    : "";
        float tag_size = 22 * s, tag_w = text_w(tag, tag_size);
        int room = (int)((rx + rw - 24 * s - tag_w - vx) / (size / 2)) - 1;
        if (room > 0 && (int)strlen(v) > room)
        {
            v[room > 3 ? room - 3 : 0] = 0;
            strcat(v, "...");
        }
        ovl_text(vx, y, size, r->warn ? ACCENT : r->kind == K_INFO ? WHITE
                                             : on                  ? WHITE
                                                                   : OVL_RGBA(200, 205, 220, 255),
                 v);
        ovl_text(rx + rw - 16 * s - tag_w, y + 5 * s, tag_size, r->note[0] ? DIM : r->when == W_NOW ? GREEN
                                                                                                    : AMBER,
                 tag);
    }
    if (*t0 > 0)
        ovl_text(rx + rw - 40 * s, ry - 40 * s, 26 * s, GREY, "^");
    if (*t0 + fit < nrows)
        ovl_text(rx + rw - 40 * s, help_y - 34 * s, 26 * s, GREY, "v");
    /* the selected row's help, and the keys */
    ovl_rect(px + 20 * s, help_y - 8 * s, pw - 40 * s, 2 * s, OVL_RGBA(60, 66, 90, 255));
    if (sel[page] < nrows && rows[sel[page]].help)
    {
        char wrapped[600];
        wrap(rows[sel[page]].help, (int)((pw - 80 * s) / (13 * s)), wrapped, sizeof wrapped);
        ovl_text(px + 36 * s, help_y + 4 * s, 26 * s, OVL_RGBA(200, 210, 230, 255), wrapped);
    }
    ovl_text(px + 36 * s, py + ph - 44 * s, 22 * s, DIM,
             page == P_REMAP    ? "Up/Down: choose   A/Enter: press a new control   Y/Delete: default   B/Backspace: back   Esc: close"
             : page == P_KEYS   ? "Up/Down: choose   A/Enter: press a new key   Y/Delete: default   B/Backspace: back   Esc: close"
             : page == P_STICKS ? "The numbers update live   B/Backspace: back   Esc: close"
                                : "Up/Down: choose   Left/Right: change   A/Enter: select   L/R or Tab, 1-6: page   B/Esc: close");
    /* on top: a question, the text being typed, or a control being waited for */
    if (confirm)
    {
        float bw = 820 * s, bh = 200 * s, bx = (w - bw) / 2, by = (h - bh) / 2;
        modal_box(w, h, bx, by, bw, bh, s);
        ovl_text(bx + 36 * s, by + 34 * s, 34 * s, WHITE, confirm == R_RESTART ? "Start the game again?" : "Quit the game?");
        ovl_text(bx + 36 * s, by + 84 * s, 26 * s, GREY, "A match in progress is lost.");
        ovl_text(bx + 36 * s, by + 140 * s, 26 * s, AMBER, "A / Enter: yes      B / Esc: no");
    }
    else if (editing)
    {
        float bw = 900 * s, bh = 600 * s, bx = (w - bw) / 2, by = (h - bh) / 2, cs = 56 * s;
        modal_box(w, h, bx, by, bw, bh, s);
        ovl_text(bx + 36 * s, by + 28 * s, 30 * s, GREY, editing == R_OSK ? osk_title : editing == R_NET_NAME ? "Your name"
                                                                                    : editing == R_NET_CODE   ? "Lobby code"
                                                                                                              : "Server (host:port)");
        char t[80];
        snprintf(t, sizeof t, "%s_", edit_buf);
        ovl_rect(bx + 30 * s, by + 72 * s, bw - 60 * s, 56 * s, OVL_RGBA(16, 18, 26, 255));
        ovl_text(bx + 44 * s, by + 82 * s, 34 * s, WHITE, t);
        float gx = bx + (bw - GRID_COLS * cs) / 2, gy = by + 160 * s;
        for (int r = 0; r < GRID_ROWS; r++)
            for (int c = 0; c < grid_cols(r); c++)
            {
                char cell[16];
                float cx = gx + c * cs, cw = cs - 6 * s;
                if (r < GRID_ROWS - 1)
                    snprintf(cell, sizeof cell, "%c", GRID[r][c]);
                else if (c == 0)
                    snprintf(cell, sizeof cell, ":");
                else
                {
                    snprintf(cell, sizeof cell, "%s", GRID_KEYS[c - 1]);
                    cx = gx + cs + (c - 1) * 2.75f * cs;
                    cw = 2.75f * cs - 6 * s;
                }
                int on = r == grid_r && c == grid_c;
                ovl_rect(cx, gy + r * cs, cw, cs - 6 * s, on ? OVL_RGBA(225, 60, 60, 255) : OVL_RGBA(40, 45, 62, 255));
                ovl_text(cx + (cw - text_w(cell, 30 * s)) / 2, gy + r * cs + 10 * s, 30 * s, WHITE, cell);
            }
        ovl_text(bx + 36 * s, by + bh - 52 * s, 22 * s, DIM, "Type, then Enter (Esc: cancel). Pad: A types, B deletes, + done, L cancels.");
    }
    else if (capture_key >= 0)
    {
        float bw = 900 * s, bh = 200 * s, bx = (w - bw) / 2, by = (h - bh) / 2;
        modal_box(w, h, bx, by, bw, bh, s);
        char t[160];
        snprintf(t, sizeof t, "Press the key for %s", pad_key_label(capture_key));
        ovl_text(bx + 36 * s, by + 34 * s, 34 * s, WHITE, t);
        ovl_text(bx + 36 * s, by + 84 * s, 26 * s, GREY, "a letter, digit, arrow, Enter, Space, Shift, Ctrl, Alt, Tab or the number pad");
        ULONGLONG now = GetTickCount64();
        snprintf(t, sizeof t, "Esc: cancel   (%d s)", capture_until > now ? (int)((capture_until - now + 999) / 1000) : 0);
        ovl_text(bx + 36 * s, by + 140 * s, 26 * s, AMBER, t);
    }
    else if (capture_button >= 0)
    {
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

/* (hle_applet.c, on the game's thread) the game's software keyboard on the menu's text grid, the last frame
 * shown again under it while the game waits. 1 typed (out, ASCII), 0 cancelled, -1 not here (the renderer
 * can't show a frame again: the caller uses its dialog). */
int gfx_present_again(void); /* (the renderer) the last frame once more, with the overlay and the menu */
int menu_osk_run(const char *title, const char *initial, int max_len, char *out, size_t cap)
{
    if (!gfx_present_again())
        return -1;
    snprintf(osk_title, sizeof osk_title, "%s", title && *title ? title : "Enter text");
    snprintf(osk_text, sizeof osk_text, "%s", initial ? initial : "");
    osk_max = max_len > 0 && max_len < (int)sizeof osk_text ? max_len : (int)sizeof osk_text - 1;
    InterlockedExchange(&osk_state, 1);
    while (osk_state == 1 || osk_state == 2)
    {
        gfx_present_again();
        Sleep(16);
    }
    int ok = osk_state == 3;
    if (ok)
        snprintf(out, cap, "%s", osk_text);
    InterlockedExchange(&osk_state, 0);
    return ok;
}
