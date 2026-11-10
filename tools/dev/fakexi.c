#define _CRT_SECURE_NO_WARNINGS
/* Test stand-in for xinput1_4.dll, put beside a test exe (never a player's): remap tests without a pad. Build:
 *   clang-cl /O1 /LD tools/dev/fakexi.c /Fexinput1_4.dll
 * Run a debug match with p2=human and no SSBU_SEQ2: the pad plays player 2 (hle_fight_test.c). Measured 2026-11-10: DK
 * as player 2, south pressed: Giant Punch statuses (B, positional) with no remap; jab with [device XInput pad 2] A = south.
 * (2026-11-10): one pad at user index 1 (player 2).
 * Nothing is pressed until the file named by FAKEXI_GO exists; then FAKEXI_MASK (XInput button bits, hex, default
 * 0x1000 = A / south) is held for 50 ms of every 500. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } Pad;
typedef struct { DWORD packet; Pad pad; } State;

__declspec(dllexport) DWORD WINAPI XInputGetState(DWORD user, State *s) {
    static ULONGLONG t0;
    static int armed;
    if (user != 1) return ERROR_DEVICE_NOT_CONNECTED;
    ZeroMemory(s, sizeof *s);
    const char *go = getenv("FAKEXI_GO");
    if (!armed && go && GetFileAttributesA(go) != INVALID_FILE_ATTRIBUTES) { armed = 1; t0 = GetTickCount64(); }
    if (armed) {
        ULONGLONG ms = GetTickCount64() - t0;
        const char *m = getenv("FAKEXI_MASK");
        if (ms % 500 < 50) s->pad.buttons = (WORD)(m ? strtoul(m, NULL, 16) : 0x1000);
    }
    s->packet = (DWORD)(GetTickCount64() / 16);
    return ERROR_SUCCESS;
}

__declspec(dllexport) DWORD WINAPI XInputSetState(DWORD user, void *v) { (void)v; return user == 1 ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED; }
