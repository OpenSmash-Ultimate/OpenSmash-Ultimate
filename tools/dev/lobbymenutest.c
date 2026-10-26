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
 *   key NAME [N]   press a key N times (ESC UP DOWN LEFT RIGHT ENTER BKSP TAB SHTAB DEL), a frame after each: "ok"
 *   type TEXT      type the characters (5: the Lobby page), then a frame: "ok"
 *   frame SECS     the menu's frames for that long, as the vsync thread runs them (0: one): "ok"
 *   rows           a frame, then "rows OPEN PAGE SEL N" and N lines "row ID KIND|LABEL|VALUE|NOTE|HELP"
 *                  (a HELP of several lines on one, " / " between them)
 *   draw           the menu drawn at 1920 x 1080: "draw N", then N lines, the text drawn
 *   toast          the last message since the last "toast" ("" for none): "toast TEXT"
 *   net SECS       wait until joined: "net WANTED SPOKEN LOBBY" (net_protocol_wanted, _spoken, net_lobby)
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
                                                "online_name.txt", "online_direct.txt", "online_protocol.txt"};
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
int gfx_window_focused(void) { return 1; }
void gfx_closing(void) {}
void display_settings(DisplaySettings *out) {
    memset(out, 0, sizeof *out);
    out->width = 1920; out->height = 1080; out->latency = 1; out->render_scale = 1;
    snprintf(out->gpu, sizeof out->gpu, "auto");
}
const char *display_ini_path(void) { return FILES[CONFIG_DISPLAY]; }
void display_set_mode(int mode) { (void)mode; }
void display_set_pacing(Pacing pacing) { (void)pacing; }
void display_set_hide_cursor(int on) { (void)on; }
void display_set_latency(int frames) { (void)frames; }
void display_set_render_scale(float scale) { (void)scale; }
int pad_info(u32 slot, PadInfo *out) { (void)slot; memset(out, 0, sizeof *out); return 0; }   /* (no controllers) */
int pad_get(u32 slot, PadState *out) { (void)slot; memset(out, 0, sizeof *out); return 0; }
void pad_rumble(u32 slot, int side, float amplitude) { (void)slot; (void)side; (void)amplitude; }
void pad_reload(void) {}
const char *pad_ini_path(void) { return FILES[CONFIG_CONTROLLERS]; }
const char *pad_button_name(int npad) { (void)npad; return "A"; }
const char *pad_raw_name(int bit) { (void)bit; return "south"; }
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
    say("rows %d %d %d %d", open_, page, sel[page], nrows);
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

int main(int argc, char **argv) {
    settings_args(argc, argv);   /* (--online: the lobby from online_server.txt, as Play SSBU.bat starts the game) */
    const char *b = getenv("LOBBYMENUTEST_BUILD");
    snprintf(build_text, sizeof build_text, "%s", b ? b : "");
    static const struct { const char *name; int key; } KEYS[] = {
        {"ESC", MENU_KEY_ESC}, {"UP", MENU_KEY_UP}, {"DOWN", MENU_KEY_DOWN}, {"LEFT", MENU_KEY_LEFT}, {"RIGHT", MENU_KEY_RIGHT},
        {"ENTER", MENU_KEY_ENTER}, {"BKSP", MENU_KEY_BACKSPACE}, {"TAB", MENU_KEY_TAB}, {"SHTAB", MENU_KEY_SHIFT_TAB}, {"DEL", MENU_KEY_DELETE}};
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
        else if (!strcmp(what, "net") && sscanf(line, "net %lf", &secs) == 1) {
            u64 end = GetTickCount64() + (u64)(secs * 1000);
            if (net_configured()) while (!net_local_ip() && GetTickCount64() < end) Sleep(20);
            say("net %d %d %d", net_protocol_wanted(), net_protocol_spoken(), net_lobby());
        } else if (!strcmp(what, "quit")) break;
        else say("error");
    }
    fflush(stdout);
    exit(0);   /* (net_relay.c's exit handler: LEAVE) */
}
