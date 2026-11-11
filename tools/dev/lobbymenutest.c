/* The settings menu (runtime/gpu_menu.c) with no game around it, for tests/test_lobby_menu.py: its
 * Lobby page (the room browser and matchmaking) and Online > Netplay version, over the game's own
 * netplay client (runtime/net_relay.c) and settings files (runtime/hle_settings.c), as the game runs
 * them. The menu's keys are pressed and its rows read line by line; the client joins the lobby the
 * environment names (SSBU_NET_RELAY, SSBU_NET_HOST, SSBU_NET_PROTOCOL, ...: net.h), or, with
 * --online, the one in online_server.txt. The settings files are read and written in the folder it
 * runs in. Made-up data only: the build id it says is LOBBYMENUTEST_BUILD's (none without it).
 *
 *   Windows: clang-cl /O1 <cc.CFLAGS> tools/dev/lobbymenutest.c runtime/net_relay.c runtime/hle_settings.c
 *   Linux:   clang -O1 -I <stubs> -I runtime/macos/include -I runtime/linux/include -I runtime tools/dev/lobbymenutest.c
 *            runtime/net_relay.c runtime/hle_settings.c runtime/linux/win32.c -lpthread
 *   macOS:   the same with runtime/macos/memory.c and runtime/macos/platform.c in place of -lpthread
 *   (<stubs>: a folder with a d3d11.h that names the three types gpu_overlay.h needs; the test writes it)
 *
 * Commands (a line each), and their answers:
 *   key NAME [N]   press a key N times (ESC UP DOWN LEFT RIGHT ENTER BKSP TAB SHTAB DEL PGUP PGDN), a frame after each: "ok"
 *   pad BUTTONS [N] press pad 1's buttons (A B X Y L R PLUS UP DOWN LEFT RIGHT, joined by +) N times: held a frame,
 *                  let go a frame: "ok"
 *   type TEXT      type the characters (5: the Lobby page), then a frame: "ok"
 *   frame SECS     the menu's frames for that long, as the vsync thread runs them (0: one): "ok"
 *   rows           a frame, then "rows OPEN PAGE SEL N FOOT" (FOOT: 1 on Restart game, 2 on Quit game, else 0) and N
 *                  lines "row ID KIND|LABEL|VALUE|NOTE|HELP"
 *                  (a HELP of several lines on one, " / " between them)
 *   draw           the menu drawn at 1920 x 1080: "draw N", then N lines, the text drawn
 *   mouse X Y [W H] the pointer moved to X, Y in a window of W x H (1920 x 1080), after a draw, then a frame: "ok"
 *   click L|R      a button pressed, after a draw, then a frame: "ok"
 *   wheel U|D      a wheel notch, then a frame: "ok"
 *   toast          the last message since the last "toast" ("" for none): "toast TEXT"
 *   net SECS       wait until joined: "net WANTED SPOKEN LOBBY" (net_protocol_wanted, _spoken, net_lobby)
 *   osk MIN MAX INITIAL|TITLE|GUIDE   the game asks for text (menu_osk_run, on a thread of its own, as the game's):
 *                  frames until the keyboard is open: "ok"
 *   oskstate       a frame, then "oskstate EDITING ROW COL|TEXT" (EDITING: 1 the game's text box is open)
 *   oskresult SECS wait for the game's answer: "oskresult OK|TEXT" (OK 1 typed, 0 cancelled; "oskresult pending")
 *   quit           ends (net_relay.c's exit handler sends LEAVE)
 * Anything else: "error". */
#ifdef __APPLE__   /* (macos/platform.c has a menu_blocks_input of its own: the Mac program has no menu yet) */
#define menu_blocks_input lobbymenutest_menu_blocks_input
#endif
#include "../../runtime/gpu_menu.c"

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#else
#include <time.h>
#endif

/* ---- what the menu and net_relay.c reach outside themselves ---------------------------------- */
static char build_text[41];
const char *rt_data_dir = ".";
const RtModule rt_module_main = {.build_id = build_text};
static const char *const FILES[CONFIG_FILES] = {"display.ini", "controllers.ini", "launcher.ini", "online_server.txt",
                                                "online_name.txt", "online_direct.txt", "online_protocol.txt",
                                                "online_rollback.txt", "online_delay.txt"};
const char *config_path(enum ConfigFile file) { return (unsigned)file < CONFIG_FILES ? FILES[file] : NULL; }   /* (this folder's) */
#ifdef _WIN32
u64 rt_now_ns(void) {
    static LARGE_INTEGER freq, t0;
    LARGE_INTEGER t;
    if (!freq.QuadPart) { QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&t0); }
    QueryPerformanceCounter(&t);
    return (u64)((double)(t.QuadPart - t0.QuadPart) * 1e9 / (double)freq.QuadPart);
}
int gfx_adapter_names(char names[][64], int max) { (void)names; (void)max; return 0; }
#else
u64 rt_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (u64)t.tv_sec * 1000000000ull + (u64)t.tv_nsec; }
int rt_upnp_add(u16 port_, const char *lan, const char *what, char *pub, size_t n) { (void)port_; (void)lan; (void)what; (void)pub; (void)n; return 2; }
void rt_upnp_remove(void) {}
SHORT GetAsyncKeyState(int vk) { (void)vk; return 0; }   /* (no keys held: the window's) */
#endif
int online_delay_setting(void) { return -1; }   /* (hle_online_delay.c: auto) */
int online_delay_last_auto(void) { return -1; }
void display_fit(unsigned w, unsigned h, unsigned *x, unsigned *y, unsigned *fw, unsigned *fh) {   /* (gpu_display.c's: the picture's 16:9 place) */
    if ((unsigned long long)w * 9 > (unsigned long long)h * 16) { *fh = h; *fw = h * 16 / 9; }
    else { *fw = w; *fh = w * 9 / 16; }
    *x = (w - *fw) / 2;
    *y = (h - *fh) / 2;
}
int gfx_window_focused(void) { return 1; }
void gfx_closing(void) {}
static volatile LONG osk_frames;   /* (osk: a "renderer" that shows frames again, so the keyboard is the menu's) */
int gfx_present_again(void) { return osk_frames; }
static int renderer_saved;   /* (the Display page's Renderer row: what it saved, shown again) */
void display_settings(DisplaySettings *out) {
    memset(out, 0, sizeof *out);
    out->width = 1920; out->height = 1080; out->latency = 1; out->render_scale = 1;
    snprintf(out->gpu, sizeof out->gpu, "auto");
    out->renderer = renderer_saved;
}
const char *display_ini_path(void) { return FILES[CONFIG_DISPLAY]; }
void display_set_mode(int mode) { (void)mode; }
void display_set_pacing(Pacing pacing) { (void)pacing; }
void display_set_hide_cursor(int on) { (void)on; }
void display_set_latency(int frames) { (void)frames; }
int display_runahead(void) { return 0; }                     /* (run-ahead: gpu_display.c, hle_rb_core.c) */
void display_set_runahead(int frames) { (void)frames; }
int kernel_rb_switch(void) { return 0; }
void display_set_render_scale(float scale) { (void)scale; }
void display_set_renderer(int renderer) { renderer_saved = renderer; }
int display_renderer(void) { return RENDERER_DEFAULT; }   /* (the setting this start began with: auto, which drew with Direct3D 11) */
int gfx_renderer(void) { return RENDERER_D3D11; }
int audio_master_volume(void) { return 100; }                 /* (ipc_audio.c: the master volume) */
void audio_set_master_volume(int percent) { (void)percent; }
/* (hle_mod_switch.c: no mods here) */
int mods_switch_list(ModSwitch *out, int max, int *code) { (void)out; (void)max; *code = -1; return 0; }
int mods_switch_set(const char *name, int on) { (void)name; (void)on; return 0; }
int mods_switch_state(char *msg, size_t n) { if (msg && n) msg[0] = 0; return MODS_SWITCH_IDLE; }
int mods_running_count(void) { return 0; }
int rt_code_patches_on(void) { return 0; }
int pad_info(u32 slot, PadInfo *out) { (void)slot; memset(out, 0, sizeof *out); return 0; }   /* (no controllers) */
static PadState fake_pad;   /* (pad: player 1's pad, its buttons held for a frame) */
int pad_get(u32 slot, PadState *out) {
    memset(out, 0, sizeof *out);
    if (slot || !fake_pad.connected) return 0;
    *out = fake_pad;
    return 1;
}
void pad_reset_axes(u32 slot) { (void)slot; }
int padusb_axes(int port, PadUsbAxes *out) { (void)port; memset(out, 0, sizeof *out); return 0; }
void padusb_recalibrate(int port) { (void)port; }
int padusb_rate(int port) { (void)port; return 0; }
void pad_rumble(u32 slot, int side, float amplitude) { (void)slot; (void)side; (void)amplitude; }
void pad_reload(void) {}
const char *pad_ini_path(void) { return FILES[CONFIG_CONTROLLERS]; }
const char *pad_button_name(int npad) { (void)npad; return "A"; }
const char *pad_raw_name(int bit) { (void)bit; return "south"; }
const char *pad_key_label(int i) { (void)i; return "A"; }
void pad_key_names(int i, char *out, int size) { (void)i; snprintf(out, (size_t)size, "Z"); }
void pad_key_name(int vk, char *out, int size) { (void)vk; snprintf(out, (size_t)size, "Z"); }
int pad_rumble_on(void) { return 1; }
int pad_keyboard_slot(void) { return 0; }
int pad_background(void) { return 0; }
void vsock_deliver(u32 src_ip, u16 src_port, u32 dst_ip, u16 dst_port, const void *data, u32 len) {
    (void)src_ip; (void)src_port; (void)dst_ip; (void)dst_port; (void)data; (void)len;
}
int ldn_in_network(u32 ip) { (void)ip; return 0; }
void ldn_listing(u32 seq, u32 owner, const u8 *info) { (void)seq; (void)owner; (void)info; }
void ldn_list_end(u32 seq) { (void)seq; }

/* The overlay: the text the menu draws, kept while "draw" asks for it. */
static char drawn[64][700];
static int ndrawn, drawing;
void ovl_rect(float x, float y, float w, float h, unsigned color) { (void)x; (void)y; (void)w; (void)h; (void)color; }
float ovl_text(float x, float y, float size, unsigned color, const char *s) {
    (void)x; (void)color;
    if (drawing && ndrawn < (int)(sizeof drawn / sizeof *drawn)) snprintf(drawn[ndrawn++], sizeof drawn[0], "%s", s);
    return y + size;
}

/* ---- the commands ----------------------------------------------------------------------------- */
static void say(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
    putchar('\n');
    fflush(stdout);
}

static void one_line(const char *s, char *out, size_t n) {   /* (newlines as " / ", and no '|' inside a field) */
    size_t k = 0;
    for (; *s && k + 4 < n; s++) {
        if (*s == '\n') { memcpy(out + k, " / ", 3); k += 3; }
        else out[k++] = *s == '|' ? '/' : *s;
    }
    out[k] = 0;
}

static int overlay_switch;
static void frame(void) { menu_frame(&overlay_switch, 0); }

static void frames(double secs) {
    u64 end = GetTickCount64() + (u64)(secs * 1000);
    do { frame(); Sleep(16); } while (GetTickCount64() < end);
}

static void cmd_rows(void) {
    frame();
    say("rows %d %d %d %d %d", open_, page, sel[page], nrows, at_foot() ? foot : 0);
    for (int i = 0; i < nrows; i++) {
        char label[120], value[240], note[140], help[900];
        one_line(rows[i].label, label, sizeof label);
        one_line(rows[i].value, value, sizeof value);
        one_line(rows[i].note, note, sizeof note);
        one_line(rows[i].help ? rows[i].help : "", help, sizeof help);
        say("row %d %d|%s|%s|%s|%s", rows[i].id, rows[i].kind, label, value, note, help);
    }
}

static void cmd_draw(void) {
    frame();
    ndrawn = 0;
    drawing = 1;
    menu_draw(1920, 1080);
    drawing = 0;
    say("draw %d", ndrawn);
    for (int i = 0; i < ndrawn; i++) {
        char t[1000];
        one_line(drawn[i], t, sizeof t);
        say("%s", t);
    }
}

static void cmd_toast(void) {
    char t[160];
    AcquireSRWLockExclusive(&toast_lock);
    snprintf(t, sizeof t, "%s", toast);
    toast[0] = 0;
    ReleaseSRWLockExclusive(&toast_lock);
    say("toast %s", t);
}

/* The game's thread asking for text (hle_applet.c), as menu_osk_run is called there. */
static struct { char title[160], guide[160], initial[200], out[1024]; int min, max; volatile LONG done; int ok; } osk_ask;
static DWORD WINAPI osk_thread(LPVOID p) {
    (void)p;
    osk_ask.ok = menu_osk_run(osk_ask.title, osk_ask.guide, osk_ask.initial, osk_ask.min, osk_ask.max, osk_ask.out, sizeof osk_ask.out);
    InterlockedExchange(&osk_ask.done, 1);
    return 0;
}

static void cmd_osk(const char *line) {
    char rest[600] = "";
    int at = 0;
    memset((void *)&osk_ask, 0, sizeof osk_ask);
    if (sscanf(line, "osk %d %d %n", &osk_ask.min, &osk_ask.max, &at) < 2 || !at) { say("error"); return; }
    snprintf(rest, sizeof rest, "%s", line + at);
    char *f[3] = {rest, NULL, NULL};
    for (int i = 1; i < 3 && f[i - 1]; i++) {
        char *bar = strchr(f[i - 1], '|');
        if (bar) { *bar = 0; f[i] = bar + 1; }
    }
    snprintf(osk_ask.initial, sizeof osk_ask.initial, "%s", f[0]);
    snprintf(osk_ask.title, sizeof osk_ask.title, "%s", f[1] ? f[1] : "");
    snprintf(osk_ask.guide, sizeof osk_ask.guide, "%s", f[2] ? f[2] : "");
    osk_frames = 1;
    CloseHandle(CreateThread(NULL, 0, osk_thread, NULL, 0, NULL));
    for (int i = 0; i < 300 && !(editing == R_OSK && osk_state == 2); i++) { frame(); Sleep(5); }
    say(editing == R_OSK ? "ok" : "error");
}

static void cmd_pad(const char *names, int n) {
    static const struct { const char *name; u64 bit; } B[] = {{"A", 1ull << 0}, {"B", 1ull << 1}, {"X", 1ull << 2}, {"Y", 1ull << 3},
        {"L", 1ull << 6}, {"R", 1ull << 7}, {"PLUS", 1ull << 10}, {"MINUS", 1ull << 11}, {"LEFT", 1ull << 12}, {"UP", 1ull << 13},
        {"RIGHT", 1ull << 14}, {"DOWN", 1ull << 15}};
    u64 held = 0;
    char copy[200];
    snprintf(copy, sizeof copy, "%s", names);
    for (char *t = strtok(copy, "+"); t; t = strtok(NULL, "+")) {
        u64 bit = 0;
        for (u32 i = 0; i < sizeof B / sizeof *B; i++) if (!strcmp(t, B[i].name)) bit = B[i].bit;
        if (!bit) { say("error"); return; }
        held |= bit;
    }
    fake_pad.connected = 1;
    for (int i = 0; i < n; i++) {
        fake_pad.buttons = held;
        frame();
        fake_pad.buttons = 0;
        frame();
    }
    say("ok");
}

int main(int argc, char **argv) {
    settings_args(argc, argv);   /* (--online: the lobby from online_server.txt, as Play SSBU.bat starts the game) */
    const char *b = getenv("LOBBYMENUTEST_BUILD");
    snprintf(build_text, sizeof build_text, "%s", b ? b : "");
    static const struct { const char *name; int key; } KEYS[] = {
        {"ESC", MENU_KEY_ESC}, {"UP", MENU_KEY_UP}, {"DOWN", MENU_KEY_DOWN}, {"LEFT", MENU_KEY_LEFT}, {"RIGHT", MENU_KEY_RIGHT},
        {"ENTER", MENU_KEY_ENTER}, {"BKSP", MENU_KEY_BACKSPACE}, {"TAB", MENU_KEY_TAB}, {"SHTAB", MENU_KEY_SHIFT_TAB}, {"DEL", MENU_KEY_DELETE},
        {"PGUP", MENU_KEY_PAGE_UP}, {"PGDN", MENU_KEY_PAGE_DOWN}};
    char line[512];
    while (fgets(line, sizeof line, stdin)) {
        char what[16] = "", arg[400] = "";
        double secs = 0;
        int n = 1;
        line[strcspn(line, "\r\n")] = 0;
        sscanf(line, "%15s", what);
        if (!strcmp(what, "key") && sscanf(line, "key %399s %d", arg, &n) >= 1) {
            int key = 0;
            for (u32 i = 0; i < sizeof KEYS / sizeof *KEYS; i++) if (!strcmp(arg, KEYS[i].name)) key = KEYS[i].key;
            if (!key) { say("error"); continue; }
            for (int i = 0; i < n; i++) { menu_key(key); frame(); }
            say("ok");
        } else if (!strcmp(what, "type") && sscanf(line, "type %399s", arg) == 1) {
            for (const char *c = arg; *c; c++) menu_char((unsigned char)*c);
            frame();
            say("ok");
        } else if (!strcmp(what, "frame") && sscanf(line, "frame %lf", &secs) == 1) { frames(secs); say("ok"); }
        else if (!strcmp(what, "rows")) cmd_rows();
        else if (!strcmp(what, "draw")) cmd_draw();
        else if (!strcmp(what, "toast")) cmd_toast();
        else if (!strcmp(what, "mouse")) {
            int x = 0, y = 0, w = 1920, h = 1080;
            if (sscanf(line, "mouse %d %d %d %d", &x, &y, &w, &h) < 2) { say("error"); continue; }
            frame();
            menu_draw(1920, 1080);
            menu_mouse_move(x, y, w, h);
            frame();
            say("ok");
        }
        else if (!strcmp(what, "click") || !strcmp(what, "wheel")) {
            int key = !strcmp(line, "click L") ? MENU_MOUSE_LEFT : !strcmp(line, "click R") ? MENU_MOUSE_RIGHT
                    : !strcmp(line, "wheel U") ? MENU_WHEEL_UP : !strcmp(line, "wheel D") ? MENU_WHEEL_DOWN : 0;
            if (!key) { say("error"); continue; }
            frame();
            menu_draw(1920, 1080);
            menu_key(key);
            frame();
            say("ok");
        }
        else if (!strcmp(what, "net") && sscanf(line, "net %lf", &secs) == 1) {
            u64 end = GetTickCount64() + (u64)(secs * 1000);
            if (net_configured()) while (!net_local_ip() && GetTickCount64() < end) Sleep(20);
            say("net %d %d %d", net_protocol_wanted(), net_protocol_spoken(), net_lobby());
        } else if (!strcmp(what, "pad") && sscanf(line, "pad %399s %d", arg, &n) >= 1) cmd_pad(arg, n);
        else if (!strcmp(what, "osk")) cmd_osk(line);
        else if (!strcmp(what, "oskstate")) {
            frame();
            say("oskstate %d %d %d|%s", editing == R_OSK, grid_r, grid_c, edit_buf);
        } else if (!strcmp(what, "oskresult") && sscanf(line, "oskresult %lf", &secs) == 1) {
            u64 end = GetTickCount64() + (u64)(secs * 1000);
            while (!osk_ask.done && GetTickCount64() < end) { frame(); Sleep(5); }
            if (osk_ask.done) say("oskresult %d|%s", osk_ask.ok, osk_ask.ok == 1 ? osk_ask.out : "");
            else say("oskresult pending");
        } else if (!strcmp(what, "quit")) break;
        else say("error");
    }
    fflush(stdout);
    exit(0);   /* (net_relay.c's exit handler: LEAVE) */
}
