/* Switch 2 controllers over Bluetooth LE on Windows (hle_padble.c): the Pro Controller 2 and the Joy-Con 2s, which
 * neither SDL nor Windows' own drivers read over Bluetooth. Nothing is paired in Windows' settings: hold the
 * controller's Sync button until its lights sweep and the game connects and pairs it itself; from then on any button
 * press wakes it toward this PC. Two Joy-Con 2s are one player (as a Pro Controller); SL + SR held 3 s on one of a pair
 * makes each its own player, held sideways; L on a left one and R on a right one held 3 s join them again.
 *
 * The input thread (hle_pad.c) makes players of its ports as it does of the USB GameCube ports (padusb.h). A port's
 * state is in SDL's terms as the others': sticks -32767..32767 with +y down; the buttons as PB_* below, the face
 * buttons by their place (A the right one, as on the console), a sideways Joy-Con's already turned to how it is held.
 * Rumble is the game's own: the HD values it sends (amplitude and frequency of each band, each motor) played as the
 * console plays them (switch2_proto.h). */
#pragma once
#include "host.h"

enum { PADBLE_PORTS = 8 };
enum {   /* PadBleState.buttons */
    PB_A = 1 << 0, PB_B = 1 << 1, PB_X = 1 << 2, PB_Y = 1 << 3, PB_LSTICK = 1 << 4, PB_RSTICK = 1 << 5, PB_L = 1 << 6,
    PB_R = 1 << 7, PB_ZL = 1 << 8, PB_ZR = 1 << 9, PB_PLUS = 1 << 10, PB_MINUS = 1 << 11, PB_LEFT = 1 << 12,
    PB_UP = 1 << 13, PB_RIGHT = 1 << 14, PB_DOWN = 1 << 15,   /* (the console's NpadButton bits 0-15) */
    PB_HOME = 1 << 16, PB_CAPTURE = 1 << 17, PB_C = 1 << 18, PB_GL = 1 << 19, PB_GR = 1 << 20,
};
enum { PADBLE_PRO = 1, PADBLE_PAIR, PADBLE_LEFT, PADBLE_RIGHT };   /* PadBleState.kind */

typedef struct PadBleState {
    int connected;
    int kind;                 /* PADBLE_*: a Pro Controller 2, two Joy-Con 2s as one, or one held sideways */
    unsigned gen;             /* changes when the port's pad becomes another (a pair split or joined) */
    u32 buttons;              /* PB_* */
    i32 lx, ly, rx, ry;       /* SDL's terms: +y down */
    char name[48];            /* "Pro Controller 2 (Bluetooth)" */
    char id[24];              /* for controllers.ini: "switch2pro", "joycon2pair", "joycon2l", "joycon2r" */
    char serial[32];          /* the controller's own serial number (a pair: the left one's) */
    char path[32];            /* its Bluetooth address, "ble:XXXXXXXXXXXX" */
} PadBleState;

/* On the input thread, once: the Bluetooth LE thread started (1), or why not in the log (0). trace = 1: each
 * advertising device is logged once (controllers.ini switch2_bluetooth = 2, SSBU_SWITCH2=2). */
int padble_start(int trace);
void padble_poll(PadBleState out[PADBLE_PORTS]);   /* every port, as last reported */
/* The player a port plays as now (0-7, -1 none): its player lights. */
void padble_player(int port, int slot);
/* The game's vibration for a port's player: per motor (0 left, 1 right) amplitude low, frequency low, amplitude high,
 * frequency high (nn::hid::VibrationValue), already scaled by the pad's strength. Played from the next packet on. */
void padble_vibrate(int port, const float motors[2][4]);
