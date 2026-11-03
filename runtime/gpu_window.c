/* The game's window, whichever back end behind gpu_backend.h draws into it (gpu_window.h). Moved
 * here from gpu_d3d11.c unchanged. (macOS: macos/window.m; this file is left out of the Mac
 * program.)
 *
 * Windows: a Win32 window on a thread of its own (window_thread), which reads the debug keys and
 * the settings menu's keys; display.ini's size, mode and pointer are gpu_display.c's
 * (display_message). Linux: the window is SDL's, made and run by the main thread
 * (linux/window.c), which passes the debug keys on here; DXVK takes the SDL_Window as the swap
 * chain's HWND. */
#ifndef __APPLE__
#include <stdio.h>
#include <stdlib.h>

#include "display.h"
#include "gpu_backend.h"   /* (gfx_closing, gfx_window_handle) */
#include "gpu_menu.h"
#include "gpu_window.h"

static HWND window;
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;

/* Debug keys, taken at the next present: F1 the overlay, F2 its details, F3 a screenshot,
 * F4 a log of the next frame's draws and its render targets, F5 depth sampling on/off, F6 a
 * 10-second sampling profile. */
static volatile LONG hotkeys;
long gfx_take_hotkeys(void) { return InterlockedExchange(&hotkeys, 0); }

#ifdef _WIN32
/* The settings menu's keys (gpu_menu.c): Esc always, the others while it is open. */
static int menu_keys(UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_CHAR) {
        if (!menu_is_open()) return 0;
        menu_char((unsigned)w);
        return 1;
    }
    if (msg != WM_KEYDOWN) return 0;
    int repeat = (l & (1 << 30)) != 0, key = 0;
    switch (w) {
    case VK_ESCAPE: if (!repeat) key = MENU_KEY_ESC; break;
    case VK_UP: key = MENU_KEY_UP; break;
    case VK_DOWN: key = MENU_KEY_DOWN; break;
    case VK_LEFT: key = MENU_KEY_LEFT; break;
    case VK_RIGHT: key = MENU_KEY_RIGHT; break;
    case VK_RETURN: if (!repeat) key = MENU_KEY_ENTER; break;
    case VK_BACK: key = MENU_KEY_BACKSPACE; break;
    case VK_DELETE: key = MENU_KEY_DELETE; break;
    case VK_TAB: if (!repeat) key = GetKeyState(VK_SHIFT) & 0x8000 ? MENU_KEY_SHIFT_TAB : MENU_KEY_TAB; break;
    }
    if (!key || (key != MENU_KEY_ESC && !menu_is_open())) return 0;
    menu_key(key);
    return 1;
}

/* (rollback netplay, hle_rb_core.c: what a match's set-up waits for, after the window's own title;
 * "" puts the title back. With a timeout: the window's thread never blocks the caller) */
void gfx_set_status(const char *text) {
    static char base[128];
    if (!window) return;
    if (!base[0]) GetWindowTextA(window, base, sizeof base);
    char t[320];
    if (text && *text) snprintf(t, sizeof t, "%s  -  %s", base, text); else snprintf(t, sizeof t, "%s", base);
    SendMessageTimeoutA(window, WM_SETTEXT, 0, (LPARAM)t, SMTO_ABORTIFHUNG, 200, NULL);
}

void rbl_upnp_close(void);   /* (rollback netplay, hle_rb_link.c: the router's port mapping, if SSBU_RB_UPNP=1 made one; ExitProcess runs no atexit) */
static LRESULT CALLBACK window_proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    LRESULT result;
    if (display_message(h, msg, w, l, &result)) return result;   /* size, full screen, pointer (gpu_display.c) */
    if (msg == WM_CLOSE) { fprintf(stderr, "[gfx] window closed\n"); gfx_closing(); rbl_upnp_close(); fflush(stderr); ExitProcess(0); }
    if (msg == WM_KEYDOWN && w >= VK_F1 && w <= VK_F6 && !(l & (1 << 30))) InterlockedOr(&hotkeys, 1 << (w - VK_F1));
    if (menu_keys(msg, w, l)) return 0;
    return DefWindowProcW(h, msg, w, l);
}

static DWORD WINAPI window_thread(LPVOID done) {
    SetThreadDescription(GetCurrentThread(), L"window");
    /* The launcher's icon, embedded in this exe (tools/cc.py). A missing one leaves the class without an icon. */
    HICON icon = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    HICON small_icon = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, 0);
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hIcon = icon;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"ssbu";
    wc.hIconSm = small_icon ? small_icon : icon;
    RegisterClassExW(&wc);
    window = display_create_window(wc.hInstance, L"ssbu", L"Super Smash Bros. Ultimate (native)");
    /* Keep the screen saver and display sleep away while the game runs, as a game or a video player
     * does (#101). ES_CONTINUOUS holds for this thread's life, which is the window's: set once, no
     * per-frame cost. (Linux/macOS: SDL turns the screen saver off when it makes the window.) */
    if (window && !getenv("SSBU_SKYLINE_PROBE")) SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED);
    SetEvent((HANDLE)done);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    return 0;
}

static BOOL CALLBACK open_window(PINIT_ONCE o, PVOID p, PVOID *c) {
    (void)o; (void)p; (void)c;
    HANDLE done = CreateEventW(NULL, TRUE, FALSE, NULL);
    CreateThread(NULL, 0, window_thread, done, 0, NULL);
    WaitForSingleObject(done, INFINITE);
    return TRUE;
}
#else
void *rt_window(void);
void gfx_hotkey(int n) { InterlockedOr(&hotkeys, 1 << n); }

static BOOL CALLBACK open_window(PINIT_ONCE o, PVOID p, PVOID *c) {
    (void)o; (void)p; (void)c;
    window = (HWND)rt_window();
    return TRUE;
}
#endif

void *gfx_window_open(void) {
    InitOnceExecuteOnce(&once, open_window, NULL, NULL);
    return window;
}

int gfx_window_focused(void) { return window && GetForegroundWindow() == window; }
void *gfx_window_handle(void) { return window; }
#endif
