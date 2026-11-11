/* Offline checks of the controller layer (runtime/hle_pad.c) without a pad, SDL or the game:
 * configuration, button mapping, GameCube controllers, sticks, player slots and rumble pacing.
 *
 *   python -c "import sys; sys.path.insert(0,'tools'); import cc, subprocess; subprocess.run([cc.CLANG, '/nologo', '/O1', *cc.CFLAGS, 'tools/dev/padtest.c', '/Febuild/padtest.exe'], check=True)"
 *   build\padtest.exe
 *
 * Exit code 0 when every check passes. */
#include <stdarg.h>
#include "../../runtime/hle_pad.c"
#include "../../runtime/hle_padusb.c"

/* (what hle_pad.c reaches outside itself; the file is not read here, and nothing is converted) */
const char *config_path(enum ConfigFile file) { (void)file; return "controllers.ini"; }
int settings_ini_set(const char *path, const char *section, const char *key, const char *value) { (void)path; (void)section; (void)key; (void)value; return 0; }

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void load_text(const char *text) {
    memset(&cfg, 0, sizeof cfg);
    cfg.keyboard_slot = 0; cfg.background = 1; cfg.rumble = 1;
    cfg.def.layout = cfg.def.style = cfg.def.deadzone = cfg.def.rumble = cfg.def.trigger = cfg.def.invert = cfg.def.dpad_moves = -1;
    kb_defaults();
    char path[MAX_PATH], dir[MAX_PATH];
    GetTempPathA(sizeof dir, dir);
    GetTempFileNameA(dir, "pad", 0, path);
    FILE *f = fopen(path, "w");
    fputs(text, f);
    fclose(f);
    f = fopen(path, "r");
    config_parse(f);
    fclose(f);
    DeleteFileA(path);
}

static Dev make_dev(const char *guid, const char *serial, const char *path, const char *name, const char *type) {
    Dev d = {0};
    d.used = 1; d.xinput = -1; d.slot = -1;
    snprintf(d.guid, sizeof d.guid, "%s", guid);
    snprintf(d.serial, sizeof d.serial, "%s", serial);
    snprintf(d.path, sizeof d.path, "%s", path);
    snprintf(d.name, sizeof d.name, "%s", name);
    snprintf(d.type, sizeof d.type, "%s", type);
    return d;
}

static PadState state(Dev *d, u32 raw_buttons, i32 lt, i32 rt) {
    Raw raw = {raw_buttons, {0, 0, 0, 0, lt, rt}};
    PadState o;
    dev_state(d, &raw, &o);
    return o;
}

static u64 press(Dev *d, u32 raw_buttons, i32 lt, i32 rt) {
    Raw raw = {raw_buttons, {0, 0, 0, 0, lt, rt}};
    PadState o;
    dev_state(d, &raw, &o);
    return o.buttons;
}

/* Connect a device into devs[i] the way sdl_open does. */
static int plug(int i, Dev d) {
    devs[i] = d;
    devs[i].slot = dev_assign(&devs[i]);
    if (devs[i].slot >= 0) dev_remember(&devs[i]);
    return devs[i].slot;
}

static void test_config(void) {
    load_text("[general]\nkeyboard_player = 3\nbackground = 0\n[players]\n2 = DualSense Wireless Controller ; mine\n9 = x\n"
              "[device default]\nlayout = labels\ndeadzone = 80\nrumble = 40\ntrigger_threshold = 50\ninvert = rightx, lefty\nZR = rightshoulder, misc1\n"
              "[device ps5]\nrumble = 70\nA = south\n[device 0300abcd]\nlayout = positional\n");
    CHECK(cfg.keyboard_slot == 2, "keyboard slot %d", cfg.keyboard_slot);
    CHECK(cfg.background == 0, "background");
    CHECK(!strcmp(cfg.pin[1], "DualSense Wireless Controller"), "pin '%s' (comment after a value)", cfg.pin[1]);
    CHECK(cfg.def.layout == 1 && cfg.def.deadzone == 50 && cfg.def.rumble == 40 && cfg.def.trigger == 50, "defaults (deadzone clamped)");
    CHECK(cfg.def.invert == (4 | 2), "invert mask from a list");
    CHECK(cfg.def.has_map[N_ZR] && cfg.def.nmap[N_ZR] == 2, "ZR remap with two controls");
    CHECK(cfg.ndev == 2 && !strcmp(cfg.dev[0].match, "ps5") && cfg.dev[0].rumble == 70 && cfg.dev[0].has_map[N_A], "[device ps5]");
    load_text("keyboard_player = 0\n[general]\nkeyboard_player = 0\n");
    CHECK(cfg.keyboard_slot == -1, "no keyboard");
}

static void test_mapping(void) {
    load_text("");
    Dev d = make_dev("g", "", "", "Xbox", "xboxone");
    int xbox_labels[4] = {N_A, N_B, N_X, N_Y};
    dev_configure(&d, xbox_labels);
    CHECK(press(&d, 1u << RB_SOUTH, 0, 0) == 1ull << N_B, "positional: bottom face button is B");
    CHECK(press(&d, 1u << RB_EAST, 0, 0) == 1ull << N_A, "positional: right is A");
    CHECK(press(&d, 1u << RB_NORTH, 0, 0) == 1ull << N_X, "positional: top is X");
    CHECK(press(&d, 1u << RB_WEST, 0, 0) == 1ull << N_Y, "positional: left is Y");
    CHECK(press(&d, 1u << RB_START | 1u << RB_BACK, 0, 0) == (1ull << N_PLUS | 1ull << N_MINUS), "start/back");
    CHECK(press(&d, 1u << RB_DPAD_UP | 1u << RB_DPAD_LEFT, 0, 0) == (1ull << N_UP | 1ull << N_LEFT), "d-pad");
    CHECK(press(&d, 0, 8000, 0) == 0 && press(&d, 0, 8200, 32767) == (1ull << N_ZL | 1ull << N_ZR), "triggers at 25%%");
    CHECK(press(&d, 1u << RB_GUIDE | 1u << RB_MISC1, 0, 0) == 0, "guide and misc1 unmapped");

    load_text("[device default]\nlayout = labels\n");
    dev_configure(&d, xbox_labels);
    CHECK(press(&d, 1u << RB_SOUTH, 0, 0) == 1ull << N_A, "labels: Xbox A is A");
    CHECK(press(&d, 1u << RB_NORTH, 0, 0) == 1ull << N_Y, "labels: Xbox Y is Y");
    int ps_labels[4] = {-1, -1, -1, -1};
    dev_configure(&d, ps_labels);
    CHECK(press(&d, 1u << RB_SOUTH, 0, 0) == 1ull << N_B, "labels on a PlayStation pad stay positional");
    int nintendo_labels[4] = {N_B, N_A, N_Y, N_X};
    dev_configure(&d, nintendo_labels);
    CHECK(press(&d, 1u << RB_SOUTH, 0, 0) == 1ull << N_B, "labels on a Nintendo pad = positional");

    load_text("[device default]\nZR = rightshoulder\n[device xboxone]\nA = south\nB = none\ntrigger_threshold = 90\n");
    dev_configure(&d, xbox_labels);
    CHECK(press(&d, 1u << RB_RIGHT_SHOULDER, 0, 0) == (1ull << N_R | 1ull << N_ZR), "ZR = rightshoulder (R keeps it too)");
    CHECK(press(&d, 1u << RB_SOUTH, 0, 0) == 1ull << N_A, "per-type remap A = south");
    CHECK(press(&d, 0, 0, 32767) == 0, "ZR remapped away from the trigger");
    CHECK(press(&d, 0, 26000, 0) == 0 && press(&d, 0, 30000, 0) == 1ull << N_ZL, "type's trigger threshold 90%%");
    Dev other = make_dev("g2", "", "", "Other", "ps4");
    dev_configure(&other, ps_labels);
    CHECK(press(&other, 1u << RB_SOUTH, 0, 0) == 1ull << N_B, "another type keeps positional");
}

static void test_gamecube(void) {
    load_text("[device default]\nstyle = pro\n[device gamecube]\nstyle = gamecube\n[device x]\nstyle = sideways\n");
    CHECK(cfg.def.style == 1 && cfg.dev[0].style == 2 && cfg.dev[1].style == -1, "style values");

    load_text("");
    int sdl_labels[4] = {N_A, N_X, N_B, N_Y};   /* SDL3's labels on a GameCube pad: A bottom, X right, B left, Y top */
    Dev gc = make_dev("g", "", "", "Nintendo GameCube Controller", "gamecube");
    dev_configure(&gc, sdl_labels);
    CHECK(gc.gc, "a GameCube pad plays as one");
    CHECK(press(&gc, 1u << RB_SOUTH, 0, 0) == 1ull << N_A, "GameCube A");
    CHECK(press(&gc, 1u << RB_EAST, 0, 0) == 1ull << N_X, "GameCube X (right of A)");
    CHECK(press(&gc, 1u << RB_WEST, 0, 0) == 1ull << N_B, "GameCube B (left of A)");
    CHECK(press(&gc, 1u << RB_NORTH, 0, 0) == 1ull << N_Y, "GameCube Y");
    CHECK(press(&gc, 1u << RB_RIGHT_SHOULDER, 0, 0) == 1ull << N_ZR, "Z is ZR");
    CHECK(press(&gc, 1u << RB_START, 0, 0) == 1ull << N_PLUS, "Start is +");
    PadState o = state(&gc, 0, 12000, 32767);
    CHECK(o.gc && o.lt == 12000 && o.rt == 32767 && o.buttons == 0, "analog L/R, no L/R/ZL/ZR bits: %d %d %llx", o.lt, o.rt,
          (unsigned long long)o.buttons);
    o = state(&gc, 1u << RB_MISC3, 3000, 0);
    CHECK(o.lt == 32767 && o.rt == 0, "the click at the bottom of L pulls it fully: %d", o.lt);
    o = state(&gc, 1u << RB_MISC4, 0, 0);
    CHECK(o.rt == 32767, "and of R");
    dev_configure(&gc, NULL);
    CHECK(press(&gc, 1u << RB_EAST, 0, 0) == 1ull << N_X && press(&gc, 1u << RB_SOUTH, 0, 0) == 1ull << N_A, "letters without SDL's labels");

    Dev pcmode = make_dev("m", "", "", "Mayflash GameCube Controller Adapter", "standard");
    dev_configure(&pcmode, NULL);
    CHECK(pcmode.gc && press(&pcmode, 1u << RB_SOUTH, 0, 0) == 1ull << N_A, "a GameCube pad by name");

    Dev xbox = make_dev("x", "", "", "Xbox Controller", "xboxone");
    int xbox_labels[4] = {N_A, N_B, N_X, N_Y};
    dev_configure(&xbox, xbox_labels);
    CHECK(!xbox.gc && !state(&xbox, 0, 0, 0).gc, "an Xbox pad is a Pro Controller");

    load_text("[device xboxone]\nstyle = gamecube\n[device gamecube]\nstyle = pro\n");
    dev_configure(&xbox, xbox_labels);
    CHECK(xbox.gc, "style = gamecube");
    CHECK(press(&xbox, 1u << RB_SOUTH, 0, 0) == 1ull << N_B, "keeps its layout: bottom is B");
    CHECK(press(&xbox, 1u << RB_RIGHT_SHOULDER, 0, 0) == 1ull << N_ZR && press(&xbox, 1u << RB_LEFT_SHOULDER, 0, 0) == 1ull << N_ZR,
          "both bumpers are Z");
    o = state(&xbox, 0, 20000, 0);
    CHECK(o.lt == 20000 && o.buttons == 0, "its triggers are analog L and R");
    dev_configure(&gc, sdl_labels);
    CHECK(!gc.gc, "style = pro on a GameCube pad");
    CHECK(press(&gc, 1u << RB_EAST, 0, 0) == 1ull << N_X, "still by its letters");
    CHECK(press(&gc, 1u << RB_RIGHT_SHOULDER, 0, 0) == 1ull << N_R && press(&gc, 0, 0, 32767) == 1ull << N_ZR, "Z is R, triggers ZL/ZR");

    load_text("[device gamecube]\nL = leftshoulder\nZR = south\n");
    dev_configure(&gc, sdl_labels);
    o = state(&gc, 1u << RB_LEFT_SHOULDER | 1u << RB_SOUTH, 30000, 0);
    CHECK(o.lt == 32767 && o.buttons == (1ull << N_A | 1ull << N_ZR), "remaps: L on a button, ZR = south: %d %llx", o.lt,
          (unsigned long long)o.buttons);
    CHECK(state(&gc, 0, 30000, 0).lt == 0, "L remapped away from the trigger");

    load_text("[device default]\ninvert = rightx, righty\n");
    Dev inv = make_dev("i", "", "", "Pad", "ps5");
    dev_configure(&inv, NULL);
    Raw r = {0, {10000, -20000, 20000, -10000, 0, 0}};
    dev_state(&inv, &r, &o);
    CHECK(o.lx == 10000 && o.ly == 20000 && o.rx == -20000 && o.ry == -10000, "invert rightx/righty: %d %d %d %d", o.lx, o.ly,
          o.rx, o.ry);
    load_text("[device default]\ninvert = none\n");
    dev_configure(&inv, NULL);
    dev_state(&inv, &r, &o);
    CHECK(o.rx == 20000 && o.ry == 10000, "invert = none leaves the sticks alone");
}

static void test_sticks(void) {
    i32 x, y;
    stick(0, 100, 0, -32768, &x, &y);
    CHECK(x == 0 && y == 32767, "up is +y, clamped: %d %d", x, y);
    stick(0, 100, 0, 32767, &x, &y);
    CHECK(y == -32767, "down: %d", y);
    stick(0, 100, 32767, -32768, &x, &y);
    CHECK(x > 23000 && x < 23300 && y > 23000 && y < 23300 && (double)x * x + (double)y * y <= 32767.0 * 32767.0 + 70000,
          "corner inside the circle: %d %d", x, y);
    stick(0, 100, 1000, 0, &x, &y);
    CHECK(x == 1000 && y == 0, "no deadzone passes small values: %d", x);
    stick(10, 100, 3000, 0, &x, &y);
    CHECK(x == 0, "inside a 10%% deadzone: %d", x);
    stick(10, 100, 32767, 0, &x, &y);
    CHECK(x == 32767, "full travel with a deadzone: %d", x);
    stick(10, 100, 6554, 0, &x, &y);
    CHECK(x > 3600 && x < 3700, "rescaled past the deadzone: %d", x);
    stick(0, 90, 29490, 0, &x, &y);
    CHECK(x == 32767, "a 90%% outer zone: 90%% of the travel is full: %d", x);
    stick(0, 90, 14745, 0, &x, &y);
    CHECK(x > 16300 && x < 16500, "and half of it a bit more than half: %d", x);
    stick(10, 90, 3000, 0, &x, &y);
    CHECK(x == 0, "the deadzone still holds with an outer zone: %d", x);
    XPad xp = {0x1000 | 0x0008, 255, 0, -32768, -32768, 0, 32767};
    Raw raw;
    xinput_raw(&xp, &raw);
    CHECK(raw.buttons == (1u << RB_SOUTH | 1u << RB_DPAD_RIGHT) && raw.axis[RA_LEFTY] == 32767 && raw.axis[RA_RIGHTY] == -32767 &&
          raw.axis[RA_LEFT_TRIGGER] == 32767, "XInput report in SDL terms");
}

static void test_slots(void) {
    load_text("[players]\n4 = ps5\n");
    memset(devs, 0, sizeof devs);
    nknown = 0;
    CHECK(plug(0, make_dev("xbox", "", "\\\\?\\HID#1", "Xbox Controller", "xboxone")) == 0, "first pad: player 1");
    CHECK(plug(1, make_dev("xbox", "", "\\\\?\\HID#2", "Xbox Controller", "xboxone")) == 1, "second: player 2");
    CHECK(plug(2, make_dev("ds", "AA:BB", "\\\\?\\HID#3", "DualSense", "ps5")) == 3, "pinned type: player 4");
    CHECK(plug(3, make_dev("other", "", "\\\\?\\HID#4", "Pad", "ps4")) == 2, "next free skips the pinned player");
    devs[0].used = 0; devs[1].used = 0; devs[2].used = 0;   /* three unplugged */
    CHECK(plug(1, make_dev("xbox", "", "\\\\?\\HID#2", "Xbox Controller", "xboxone")) == 1, "same path: player 2 again");
    CHECK(plug(0, make_dev("xbox", "", "\\\\?\\HID#9", "Xbox Controller", "xboxone")) == 0, "same GUID, new path: the free one it had");
    CHECK(plug(2, make_dev("ds", "AA:BB", "\\\\?\\HID#7", "DualSense", "ps5")) == 3, "the pinned pad comes back to its player");
    CHECK(plug(4, make_dev("ds4", "S1", "\\\\?\\HID#10", "DS4", "ps4")) == 4, "player 5");
    CHECK(plug(5, make_dev("ds4", "S2", "\\\\?\\HID#11", "DS4", "ps4")) == 5, "player 6");
    devs[4].used = 0; devs[5].used = 0;
    CHECK(plug(5, make_dev("ds4", "S2", "\\\\?\\HID#12", "DS4", "ps4")) == 5, "same serial, new path: its own player, not the first free");
    CHECK(plug(4, make_dev("ds4", "S1", "\\\\?\\HID#13", "DS4", "ps4")) == 4, "and the other one");
    for (int i = 6; i < 9; i++) {
        char g[8]; snprintf(g, sizeof g, "n%d", i);
        plug(i, make_dev(g, "", "", "Pad", "ps4"));
    }
    CHECK(devs[6].slot == 6 && devs[7].slot == 7 && devs[8].slot == -1, "eight players, then none: %d %d %d", devs[6].slot, devs[7].slot, devs[8].slot);
}

static void test_rumble(void) {
    load_text("[device default]\nrumble = 50\n");
    Dev d = make_dev("g", "", "", "Pad", "ps5");
    dev_configure(&d, NULL);
    d.slot = 5;
    u16 lo, hi;
    ULONGLONG t = GetTickCount64();
    pad_rumble(5, 1, 1.0f);
    pad_rumble(5, 2, 0.25f);
    CHECK(rumble_due(&d, t, &lo, &hi) && lo == 32767 && hi == 8191, "half strength: %u %u", lo, hi);
    CHECK(!rumble_due(&d, t + 50, &lo, &hi), "no resend within 100 ms");
    CHECK(rumble_due(&d, t + 100, &lo, &hi), "refresh after 100 ms");
    pad_rumble(5, 1, 0.5f);
    CHECK(!rumble_due(&d, t + 105, &lo, &hi), "a change waits 10 ms");
    CHECK(rumble_due(&d, t + 110, &lo, &hi) && lo == 16383, "then goes out: %u", lo);
    pad_rumble(5, 1, 0); pad_rumble(5, 2, 0);
    CHECK(rumble_due(&d, t + 130, &lo, &hi) && !lo && !hi, "stop");
    CHECK(!rumble_due(&d, t + 400, &lo, &hi), "nothing while stopped");
    pad_rumble(5, 1, 1.0f);
    CHECK(rumble_due(&d, t + 500, &lo, &hi) && lo, "start again");
    CHECK(rumble_due(&d, t + 3600, &lo, &hi) && !lo, "stopped when the game goes quiet for 3 s");
    CHECK(pad_rumble_value(5, 1) == 1.0f && pad_rumble_value(9, 1) == 0, "last value kept");

    load_text("[general]\nrumble = 0\n[device default]\nrumble = 50\n");
    CHECK(!cfg.rumble, "[general] rumble = 0");
    d.sent_low = d.sent_high = 0; d.sent_at = 0;
    pad_rumble(5, 1, 1.0f); pad_rumble(5, 2, 1.0f);
    t = GetTickCount64();
    CHECK(!rumble_due(&d, t, &lo, &hi) && !lo && !hi, "no controller rumbles: %u %u", lo, hi);
    load_text("[general]\nrumble = 1\n");
    CHECK(rumble_due(&d, t + 20, &lo, &hi) && lo, "on again: %u", lo);
}

/* GameCube controllers straight from USB (hle_padusb.c): the adapter's and the Switch 2
 * controller's reports, as players see them. */
static PadState usb_player(int port) {
    static const int LETTERS[4] = {N_A, N_X, N_B, N_Y};
    Dev d = make_dev(usb_ports[port].id, "", usb_ports[port].path, usb_ports[port].name, "gamecube");
    d.usb = port + 1;
    dev_configure(&d, LETTERS);
    Raw raw;
    usb_raw(&usb_ports[port], &raw);
    PadState o;
    dev_state(&d, &raw, &o);
    return o;
}

static void test_usb(void) {
    load_text("");
    Device dev = {.kind = KIND_ADAPTER, .first = 4};
    UCHAR r[37] = {0x21};
    UCHAR *p1 = r + 1, *p3 = r + 1 + 18;
    p1[0] = 0x14;                          /* port 1: a cable, the rumble cable in */
    p1[3] = p1[4] = p1[5] = p1[6] = 128; p1[7] = 30; p1[8] = 32;
    p3[0] = 0x20;                          /* port 3: a WaveBird */
    p3[3] = 130; p3[4] = p3[5] = p3[6] = 128; p3[7] = p3[8] = 20;
    adapter_report(&dev, r, sizeof r);
    CHECK(usb_ports[4].connected && !usb_ports[5].connected && usb_ports[6].connected && !usb_ports[7].connected, "ports 1 and 3");
    p1[1] = 0x01 | 0x80; p1[2] = 0x02 | 0x01;   /* A, up; Z, Start */
    p1[3] = 208; p1[4] = 48;                   /* the stick right and down, 80 from the centre */
    p1[7] = 170; p1[8] = 32;                   /* L all the way, R at rest */
    adapter_report(&dev, r, sizeof r);
    PadState o = usb_player(4);
    CHECK(o.gc && o.buttons == (1ull << N_A | 1ull << N_UP | 1ull << N_ZR | 1ull << N_PLUS), "A, up, Z = ZR, Start = +: %llx",
          (unsigned long long)o.buttons);
    CHECK(o.lx > 23000 && o.ly < -23000, "stick right and down (+y up): %d %d", o.lx, o.ly);
    CHECK(o.lt == 32767 && o.rt == 0, "L analog, R at rest: %d %d", o.lt, o.rt);
    p1[1] = 0x02 | 0x04 | 0x08; p1[2] = 0x08; p1[7] = 30;   /* B, X, Y; the click at L's bottom */
    adapter_report(&dev, r, sizeof r);
    o = usb_player(4);
    CHECK(o.buttons == (1ull << N_B | 1ull << N_X | 1ull << N_Y) && o.lt == 32767, "B X Y, L's click: %llx %d",
          (unsigned long long)o.buttons, o.lt);
    r[0] = 0x22;
    p1[1] = 0;
    adapter_report(&dev, r, sizeof r);
    CHECK(usb_player(4).buttons, "another report id leaves the ports alone");
    r[0] = 0x21;
    p1[0] = 0;
    adapter_report(&dev, r, sizeof r);
    CHECK(!usb_ports[4].connected, "unplugged");

    Device sw = {.kind = KIND_SWITCH2, .first = 8, .trigger_zero = {30, 30}};
    UCHAR s[64] = {0x05};
    s[5] = 0x08 | 0x80; s[6] = 0x02; s[7] = 0x02 | 0x40;   /* A, Z; Start; up, L's click */
    int lx = 2048 + 1400, ly = 2048;
    s[11] = (UCHAR)lx; s[12] = (UCHAR)((lx >> 8) | (ly & 0x0F) << 4); s[13] = (UCHAR)(ly >> 4);
    s[14] = 0; s[15] = 0x08; s[16] = 0x80;               /* the C stick centred */
    s[61] = 30; s[62] = 232;
    switch2_report(&sw, s, sizeof s);
    o = usb_player(8);
    CHECK(o.buttons == (1ull << N_A | 1ull << N_ZR | 1ull << N_PLUS | 1ull << N_UP), "Switch 2: A, Z, Start, up: %llx",
          (unsigned long long)o.buttons);
    CHECK(o.lx > 32000 && o.ly == 0 && o.lt == 32767 && o.rt == 32767, "Switch 2: stick, L's click, R all the way: %d %d %d %d",
          o.lx, o.ly, o.lt, o.rt);
    s[5] = 0x01 | 0x02 | 0x04; s[6] = 0; s[7] = 0;
    switch2_report(&sw, s, sizeof s);
    CHECK(usb_player(8).buttons == (1ull << N_Y | 1ull << N_X | 1ull << N_B), "Switch 2: Y X B");
}

/* The adapter's stick calibration (hle_padusb.c's Axis): the centre measured from still reports
 * (a stick moving, or resting far off, as it is plugged in keeps the default), each direction's
 * reach grown only after a few reports in a row agree, and recalibration. */
static void test_axes(void) {
    Device dev = {.kind = KIND_ADAPTER, .first = 0};
    UCHAR r[37] = {0x21};
    UCHAR *p1 = r + 1, *p2 = r + 1 + 9;
    p1[0] = 0x10; p1[3] = p1[4] = p1[5] = p1[6] = 128; p1[7] = p1[8] = 30;
    for (int i = 0; i < 40; i++) adapter_report(&dev, r, sizeof r);   /* at rest: the centre is measured */
    PadUsbAxes ua;
    CHECK(padusb_axes(0, &ua) && ua.centre[0] == 128 && ua.lo[0] == 80 && ua.hi[0] == 80 && ua.raw[0] == 128,
          "rest: centre 128, reach 80: %d %d %d %d", ua.centre[0], ua.lo[0], ua.hi[0], ua.raw[0]);
    CHECK(usb_ports[0].lx == 0 && usb_ports[0].ly == 0, "rest: zeroed");

    p2[0] = 0x10; p2[3] = 150; p2[4] = p2[5] = p2[6] = 128; p2[7] = p2[8] = 30;
    for (int i = 0; i < 40; i++) adapter_report(&dev, r, sizeof r);
    CHECK(padusb_axes(1, &ua) && ua.centre[0] == 128, "plugged in 22 off: too far, the default centre stays: %d", ua.centre[0]);
    CHECK(usb_ports[1].lx > 8000 && usb_ports[1].lx < 10000, "and its offset shows: %d", usb_ports[1].lx);
    p2[0] = 0;

    /* each direction's own reach */
    p1[3] = 48;
    adapter_report(&dev, r, sizeof r);
    CHECK(usb_ports[0].lx == -32767, "80 left is all the way: %d", usb_ports[0].lx);
    p1[3] = 198;
    adapter_report(&dev, r, sizeof r);
    CHECK(usb_ports[0].lx == 28671, "70 right with 80 of reach: %d", usb_ports[0].lx);
    p1[3] = 128;
    adapter_report(&dev, r, sizeof r);
    CHECK(usb_ports[0].lx == 0, "back to rest");

    /* a corrupt report must not grow the reach: one spike, then four in a row */
    p1[3] = 255;
    adapter_report(&dev, r, sizeof r);
    CHECK(padusb_axes(0, &ua) && ua.hi[0] == 80 && usb_ports[0].lx == 32767, "one spike: saturated, the reach keeps 80: %d", ua.hi[0]);
    p1[3] = 128;
    adapter_report(&dev, r, sizeof r);
    p1[3] = 255;
    for (int i = 0; i < 4; i++) adapter_report(&dev, r, sizeof r);
    CHECK(padusb_axes(0, &ua) && ua.hi[0] == 100, "four spikes in a row: the reach grows, capped: %d", ua.hi[0]);

    /* Recalibrate sticks (the menu) starts it over */
    padusb_recalibrate(0);
    p1[3] = 128;
    adapter_report(&dev, r, sizeof r);
    CHECK(padusb_axes(0, &ua) && ua.centre[0] == 128 && ua.hi[0] == 80, "recalibrated: as new");

    /* a trigger's top is as careful */
    CHECK(dev.trigger_top[0][0] == 170, "trigger top rest + 140: %d", dev.trigger_top[0][0]);
    p1[7] = 255;
    adapter_report(&dev, r, sizeof r);
    CHECK(dev.trigger_top[0][0] == 170, "one trigger spike: the top keeps 170");
    for (int i = 0; i < 3; i++) adapter_report(&dev, r, sizeof r);
    CHECK(dev.trigger_top[0][0] == 255, "four in a row: the top is relearned: %d", dev.trigger_top[0][0]);
}

/* The keyboard ([keyboard]): the defaults are the keys of before, rebinding, several keys, the
 * sticks' directions, Alt+Enter, ssbu-recomp's spellings and bad names. */
static int held_keys[8], nheld;
static int fake_down(int vk) { for (int i = 0; i < nheld; i++) if (held_keys[i] == vk) return 1; return 0; }
static u64 keys(i32 *lx, i32 *ly, i32 *rx, i32 *ry, int n, ...) {
    va_list ap;
    va_start(ap, n);
    for (nheld = 0; nheld < n && nheld < 8; nheld++) held_keys[nheld] = va_arg(ap, int);
    va_end(ap);
    *lx = *ly = *rx = *ry = 0;
    return pad_keyboard_state(fake_down, lx, ly, rx, ry);
}

static void test_keyboard(void) {
    i32 lx, ly, rx, ry;
    load_text("[general]\nkeyboard_player = 1\n");
    CHECK(keys(&lx, &ly, &rx, &ry, 4, 'Z', 'X', 'C', 'V') == (1ull << N_A | 1ull << N_B | 1ull << N_X | 1ull << N_Y), "Z X C V are A B X Y");
    CHECK(keys(&lx, &ly, &rx, &ry, 4, 'A', 'S', 'Q', 'W') == (1ull << N_L | 1ull << N_R | 1ull << N_ZL | 1ull << N_ZR), "A S Q W are L R ZL ZR");
    CHECK(keys(&lx, &ly, &rx, &ry, 2, VK_RETURN, VK_BACK) == (1ull << N_PLUS | 1ull << N_MINUS), "Enter, Backspace");
    CHECK(keys(&lx, &ly, &rx, &ry, 2, VK_RETURN, VK_MENU) == 0, "Alt+Enter is not +");
    CHECK(keys(&lx, &ly, &rx, &ry, 2, VK_LEFT, VK_UP) == 0 && lx == -32767 && ly == 32767 && !rx && !ry, "arrows: the left stick");
    CHECK(keys(&lx, &ly, &rx, &ry, 2, 'L', 'K') == 0 && rx == 32767 && ry == -32767 && !lx && !ly, "L K: the right stick");
    CHECK(keys(&lx, &ly, &rx, &ry, 2, VK_LEFT, VK_RIGHT) == 0 && lx == 32767, "left and right: right, as before");
    CHECK(keys(&lx, &ly, &rx, &ry, 1, 'E') == 0, "E: nothing");

    load_text("[keyboard]\nA = Space, j\nB = x ; a comment\nLSTICK_LEFT = a\nLSTICK_RIGHT = D\nLSTICK_UP = w\nLSTICK_DOWN = s\nL = none\n"
              "UP = Num8\nDOWN = 0x62\nZR = F5\nRSTICK = shift\nnot_a_button = Z\nX = Bogus, C\n");
    CHECK(keys(&lx, &ly, &rx, &ry, 1, VK_SPACE) == 1ull << N_A && keys(&lx, &ly, &rx, &ry, 1, 'J') == 1ull << N_A, "A = Space, J");
    CHECK(keys(&lx, &ly, &rx, &ry, 1, 'Z') == 0, "Z no longer A");
    CHECK(keys(&lx, &ly, &rx, &ry, 1, 'X') == 1ull << N_B, "B = x (any case, comment after)");
    CHECK(keys(&lx, &ly, &rx, &ry, 2, 'A', 'W') == 0 && lx == -32767 && ly == 32767, "WASD movement");
    CHECK(keys(&lx, &ly, &rx, &ry, 1, VK_LEFT) == 0 && lx == 0, "the arrows no longer move");
    CHECK(keys(&lx, &ly, &rx, &ry, 1, 'J') == 1ull << N_A && rx == -32767, "a key on two bindings: both (J is A and still the right stick's left)");
    CHECK(keys(&lx, &ly, &rx, &ry, 2, 0x68, 0x62) == (1ull << N_UP | 1ull << N_DOWN), "the d-pad on Num8, 0x62");
    CHECK(keys(&lx, &ly, &rx, &ry, 1, VK_F1 + 4) == 1ull << N_ZR && keys(&lx, &ly, &rx, &ry, 1, VK_SHIFT) == 1ull << N_RSTICK, "F5, Shift");
    CHECK(keys(&lx, &ly, &rx, &ry, 1, 'C') == 1ull << N_X, "a bad key name skipped, the rest kept");
    CHECK(cfg.nkb[N_L] == 0, "L = none");
    char names[64];
    pad_key_names(N_A, names, sizeof names);
    CHECK(!strcmp(names, "Space, J"), "names '%s'", names);
    pad_key_names(N_L, names, sizeof names);
    CHECK(!strcmp(names, "none"), "none '%s'", names);
    pad_key_names(N_UP, names, sizeof names);
    CHECK(!strcmp(names, "Num8"), "Num8 '%s'", names);
    pad_key_names(N_ZR, names, sizeof names);
    CHECK(!strcmp(names, "F5"), "F5 '%s'", names);

    /* every name reads back as itself */
    for (int vk = 1; vk < 256; vk++) {
        char n[16];
        pad_key_name(vk, n, sizeof n);
        CHECK(pad_key_parse(n) == vk, "0x%02X -> %s -> %d", vk, n, pad_key_parse(n));
    }
    CHECK(pad_key_parse("ENTER") == VK_RETURN && pad_key_parse("BACKSPACE") == VK_BACK && pad_key_parse("NUM0") == 0x60 &&
          pad_key_parse("ESC") == VK_ESCAPE && pad_key_parse("CTRL") == VK_CONTROL && pad_key_parse("F12") == VK_F1 + 11,
          "ssbu-recomp's spellings");
    CHECK(pad_key_parse("F13") < 0 && pad_key_parse("?") < 0 && pad_key_parse("0x") < 0 && pad_key_parse("Num10") < 0, "no such keys");

    /* ssbu-recomp's [keyboard] (its own defaults): read as written, STICK_L as LSTICK */
    load_text("[keyboard]\nplayer = 2\nA = Z\nL = Q\nR = E\nZL = A\nZR = S\nSTICK_L = none\nSTICK_R = T\nLEFT = J\nUP = I\nRIGHT = L\nDOWN = K\n"
              "RSTICK_LEFT = none\nRSTICK_UP = none\nRSTICK_RIGHT = none\nRSTICK_DOWN = none\nmovement = ssf2\n");
    CHECK(cfg.keyboard_slot == 1, "player = 2");
    CHECK(keys(&lx, &ly, &rx, &ry, 2, 'Q', 'E') == (1ull << N_L | 1ull << N_R), "L R on Q E");
    CHECK(keys(&lx, &ly, &rx, &ry, 1, 'T') == 1ull << N_RSTICK && cfg.nkb[N_LSTICK] == 0, "STICK_R = T, STICK_L none");
    CHECK(keys(&lx, &ly, &rx, &ry, 2, 'J', 'K') == (1ull << N_LEFT | 1ull << N_DOWN) && rx == 0 && ry == 0, "IJKL the d-pad");
    CHECK(cfg.kb_ssf2 == 1, "movement = ssf2 read");

    /* movement = ssf2 (kb_ssf2): one call a poll */
    load_text("[keyboard]\nmovement = ssf2\n");
    for (int i = 0; i < 20; i++) keys(&lx, &ly, &rx, &ry, 0);   /* (time passes, nothing held) */
    keys(&lx, &ly, &rx, &ry, 1, VK_RIGHT);
    CHECK(lx == (i32)(0.60f * 32767), "ssf2: a key walks (%d)", lx);
    for (int i = 0; i < 4; i++) keys(&lx, &ly, &rx, &ry, 1, VK_RIGHT);
    CHECK(lx == 32767, "ssf2: then fully (walking speed, no dash: %d)", lx);
    keys(&lx, &ly, &rx, &ry, 0);
    keys(&lx, &ly, &rx, &ry, 1, VK_RIGHT);
    CHECK(lx == 32767, "ssf2: the key again soon: a dash (%d)", lx);
    for (int i = 0; i < 20; i++) keys(&lx, &ly, &rx, &ry, 0);
    keys(&lx, &ly, &rx, &ry, 1, VK_DOWN);
    CHECK(ly == -(i32)(0.655f * 32767), "ssf2: down crouches (%d)", ly);
    for (int i = 0; i < 20; i++) keys(&lx, &ly, &rx, &ry, 0);
    u64 b = keys(&lx, &ly, &rx, &ry, 1, VK_UP);
    CHECK(ly == 0, "ssf2: up held back (a short hop or an up smash first: %d)", ly);
    b = keys(&lx, &ly, &rx, &ry, 2, VK_UP, 'Z');
    CHECK(ly == 32767 && (b & 1ull << N_A), "ssf2: up then A: up and A together (up smash)");
    for (int i = 0; i < 20; i++) keys(&lx, &ly, &rx, &ry, 0);
    load_text("[keyboard]\nmovement = full\n");
    keys(&lx, &ly, &rx, &ry, 1, VK_RIGHT);
    CHECK(lx == 32767 && cfg.kb_ssf2 == 0, "movement = full: fully at once");
}

static void test_dpad_moves(void) {
    load_text("[device default]\ndpad_moves = on\n[device ps5]\ndpad_moves = off\n");
    CHECK(cfg.def.dpad_moves == 1 && cfg.dev[0].dpad_moves == 0, "dpad_moves parsed per section");
    load_text("");
    CHECK(cfg.def.dpad_moves == -1, "dpad_moves unset by default");
    Dev d = make_dev("g", "", "", "Xbox", "xboxone");
    int labels[4] = {N_A, N_B, N_X, N_Y};
    dev_configure(&d, labels);
    CHECK(d.dpad_moves == 0, "dpad_moves off by default");
    Raw raw = {1u << RB_DPAD_RIGHT, {0, 0, 0, 0, 0, 0}};
    PadState o;
    dev_state(&d, &raw, &o);
    CHECK(o.lx == 0 && (o.buttons >> N_RIGHT & 1), "off: the D-pad is only buttons");
    load_text("[device default]\ndpad_moves = on\n");
    Dev e = make_dev("g", "", "", "Xbox", "xboxone");
    dev_configure(&e, labels);
    CHECK(e.dpad_moves == 1, "dpad_moves on");
    dev_state(&e, &raw, &o);
    CHECK(o.lx == 32767 && o.ly == 0 && !(o.buttons >> N_RIGHT & 1), "right: full tilt, no D-pad button");
    raw.buttons = 1u << RB_DPAD_UP | 1u << RB_DPAD_LEFT;
    dev_state(&e, &raw, &o);
    CHECK(o.lx == -23170 && o.ly == 23170 && !(o.buttons >> N_UP & 1), "diagonal on the circle");
    raw.buttons = 1u << RB_DPAD_DOWN;
    dev_state(&e, &raw, &o);
    CHECK(o.ly == -32767, "down is negative y");
    raw.buttons = 1u << RB_DPAD_RIGHT;
    raw.axis[RA_LEFTX] = -20000;   /* the stick pushed less far the other way: the D-pad wins */
    dev_state(&e, &raw, &o);
    CHECK(o.lx == 32767, "larger wins: D-pad over a small stick");
    raw.buttons = 0;
    raw.axis[RA_LEFTX] = 30000;
    dev_state(&e, &raw, &o);
    CHECK(o.lx > 29000 && o.lx <= 32767, "no D-pad: the stick as it is");
}

int main(void) {
    test_keyboard();
    test_config();
    test_mapping();
    test_gamecube();
    test_usb();
    test_axes();
    test_sticks();
    test_slots();
    test_rumble();
    test_dpad_moves();
    printf("%d of %d checks passed\n", checks - failures, checks);
    return failures != 0;
}
