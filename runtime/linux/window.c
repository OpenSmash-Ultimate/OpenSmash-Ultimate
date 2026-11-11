/* The game's window on Linux: SDL2, on the process's main thread.
 *
 * On Windows the renderer makes a Win32 window on a thread of its own and reads the keyboard
 * with GetAsyncKeyState. Here the main thread creates an SDL window at start-up (DXVK presents
 * into it: its HWND is the SDL_Window) and then runs the event loop for good, keeping what the
 * rest of the runtime asks for up to date: the keyboard (behind GetAsyncKeyState), focus, the
 * debug keys F1-F6 and the window's close button. Game controllers are hle_pad.c's: its input
 * thread reads them (SDL starts them here, with their events off).
 *
 * display.ini (gpu_display.c) sizes the window and picks its mode: windowed, or SDL's desktop
 * full screen for borderless and fullscreen alike (the monitor keeps its mode). F11 or Alt+Enter
 * switches between the two, as on Windows. The window's inside in pixels goes to the renderer,
 * whose swap chain follows it (the picture then 16:9 within); the pointer hides after 2 s
 * without moving (hide_cursor). */
#define _GNU_SOURCE 1
#include <SDL.h>
#include <SDL_vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

#include "../display.h"
#include "../gpu_menu.h"

static SDL_Window *window;
static volatile int focused;
static const Uint8 *keys;
static int nkeys;
static int mode;                    /* the window's mode now (DISPLAY_*) */
static int cursor_hidden;
static Uint64 mouse_moved_at;

void gfx_hotkey(int n);   /* (gpu_d3d11.c) F1 + n was pressed */

/* ---- display.ini's window ----------------------------------------------------------------- */
static void note_size(void) {   /* the window's inside, in pixels, for the swap chain */
    int w = 0, h = 0;
    SDL_Vulkan_GetDrawableSize(window, &w, &h);
    if (w > 0 && h > 0) display_note_size((unsigned)w, (unsigned)h);
}

static void note_refresh(void) {   /* the refresh rate of the window's monitor */
    SDL_DisplayMode dm;
    int i = SDL_GetWindowDisplayIndex(window);
    if (i >= 0 && !SDL_GetCurrentDisplayMode(i, &dm)) display_note_refresh(dm.refresh_rate);
}

static const char *mode_name(int m) { return m == DISPLAY_WINDOWED ? "windowed" : m == DISPLAY_BORDERLESS ? "borderless" : "fullscreen"; }

static void set_mode(int m) {
    if (m == mode) return;
    mode = m;
    display_note_mode(m);
    if (SDL_SetWindowFullscreen(window, m == DISPLAY_WINDOWED ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP))
        fprintf(stderr, "[display] cannot switch to %s: %s\n", mode_name(m), SDL_GetError());
    else fprintf(stderr, "[display] %s\n", mode_name(m));
}

/* (every turn of the event loop) the pointer hides over the focused window after 2 s still */
static void idle_pointer(Uint64 now) {
    if (display_hide_cursor() && !cursor_hidden && focused && SDL_GetMouseFocus() == window && now - mouse_moved_at > 2000) {
        SDL_ShowCursor(SDL_DISABLE);
        cursor_hidden = 1;
    }
}

/* ---- the keyboard ------------------------------------------------------------------- */
static SDL_Scancode scancode_of(int vk) {
    if (vk >= 'A' && vk <= 'Z') return (SDL_Scancode)(SDL_SCANCODE_A + (vk - 'A'));
    if (vk >= '1' && vk <= '9') return (SDL_Scancode)(SDL_SCANCODE_1 + (vk - '1'));
    if (vk == '0') return SDL_SCANCODE_0;
    if (vk >= VK_F1 && vk <= VK_F1 + 11) return (SDL_Scancode)(SDL_SCANCODE_F1 + (vk - VK_F1));
    if (vk == 0x60) return SDL_SCANCODE_KP_0;   /* (VK_NUMPAD0..9: controllers.ini's Num0..Num9) */
    if (vk > 0x60 && vk <= 0x69) return (SDL_Scancode)(SDL_SCANCODE_KP_1 + (vk - 0x61));
    switch (vk) {
    case VK_RETURN: return SDL_SCANCODE_RETURN;
    case VK_BACK: return SDL_SCANCODE_BACKSPACE;
    case VK_TAB: return SDL_SCANCODE_TAB;
    case VK_ESCAPE: return SDL_SCANCODE_ESCAPE;
    case VK_SPACE: return SDL_SCANCODE_SPACE;
    case VK_LEFT: return SDL_SCANCODE_LEFT;
    case VK_RIGHT: return SDL_SCANCODE_RIGHT;
    case VK_UP: return SDL_SCANCODE_UP;
    case VK_DOWN: return SDL_SCANCODE_DOWN;
    case VK_SHIFT: return SDL_SCANCODE_LSHIFT;
    case VK_CONTROL: return SDL_SCANCODE_LCTRL;
    case VK_MENU: return SDL_SCANCODE_LALT;
    }
    return SDL_SCANCODE_UNKNOWN;
}

SHORT GetAsyncKeyState(int vk) {
    if (!keys) return 0;
    SDL_Scancode s = scancode_of(vk);
    int down = s != SDL_SCANCODE_UNKNOWN && (int)s < nkeys && keys[s];
    if (vk == VK_SHIFT) down |= keys[SDL_SCANCODE_RSHIFT];
    if (vk == VK_CONTROL) down |= keys[SDL_SCANCODE_RCTRL];
    if (vk == VK_MENU) down |= keys[SDL_SCANCODE_RALT];
    return down ? (SHORT)0x8000 : 0;
}

HWND GetForegroundWindow(void) { return focused ? (HWND)window : NULL; }
void *rt_window(void) { return window; }

/* The settings menu's keys (gpu_menu.c): Esc always, the others while it is open. */
static void menu_key_of(const SDL_KeyboardEvent *k) {
    int key = 0;
    switch (k->keysym.scancode) {
    case SDL_SCANCODE_ESCAPE: if (!k->repeat) key = MENU_KEY_ESC; break;
    case SDL_SCANCODE_UP: key = MENU_KEY_UP; break;
    case SDL_SCANCODE_DOWN: key = MENU_KEY_DOWN; break;
    case SDL_SCANCODE_LEFT: key = MENU_KEY_LEFT; break;
    case SDL_SCANCODE_RIGHT: key = MENU_KEY_RIGHT; break;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: if (!k->repeat && !(k->keysym.mod & KMOD_ALT)) key = MENU_KEY_ENTER; break;
    case SDL_SCANCODE_BACKSPACE: key = MENU_KEY_BACKSPACE; break;
    case SDL_SCANCODE_DELETE: key = MENU_KEY_DELETE; break;
    case SDL_SCANCODE_TAB: if (!k->repeat) key = k->keysym.mod & KMOD_SHIFT ? MENU_KEY_SHIFT_TAB : MENU_KEY_TAB; break;
    case SDL_SCANCODE_PAGEUP: if (!k->repeat) key = MENU_KEY_PAGE_UP; break;   /* (the menu's pages, as L and R) */
    case SDL_SCANCODE_PAGEDOWN: if (!k->repeat) key = MENU_KEY_PAGE_DOWN; break;
    default: break;
    }
    if (key && (key == MENU_KEY_ESC || menu_is_open())) menu_key(key);
}

/* ---- start-up and the event loop ---------------------------------------------------- */
static void video_hints(void) {
    setenv("DXVK_WSI_DRIVER", "SDL2", 0);   /* (DXVK's native build asks which window system) */
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
    SDL_SetHint(SDL_HINT_APP_NAME, "Super Smash Bros. Ultimate");
}

/* --warm-shaders (gpu_d3d11.c): DXVK's native build presents through SDL, so SDL's video system
 * must be started before the first device, even though the warm pass makes no window (it draws
 * into 1x1 targets of its own). 1: started. */
int rt_video_init(void) {
    video_hints();
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        fprintf(stderr, "[warm] SDL could not start the video system: %s\n", SDL_GetError());
        return 0;
    }
    return 1;
}

int rt_window_init(void) {
    video_hints();
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");   /* positional, as hle_pad.c expects */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");   /* (controllers.ini's background = ... decides) */
    SDL_SetHint(SDL_HINT_AUTO_UPDATE_JOYSTICKS, "0");              /* hle_pad.c's input thread updates the pads */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS)) {
        fprintf(stderr, "[gfx] SDL could not start the video system: %s\n", SDL_GetError());
        return 0;
    }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) fprintf(stderr, "[audio] SDL could not start audio: %s\n", SDL_GetError());
    SDL_GameControllerEventState(SDL_IGNORE);   /* (the pads' state is read, not their events) */
    SDL_JoystickEventState(SDL_IGNORE);
    int width, height;
    display_window_size(&width, &height);
    mode = display_window_mode();
    window = SDL_CreateWindow("Super Smash Bros. Ultimate (native)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height,
                              SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                              (mode == DISPLAY_WINDOWED ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP));
    if (!window) {
        fprintf(stderr, "[gfx] cannot create the window: %s\n", SDL_GetError());
        return 0;
    }
    SDL_SetWindowMinimumSize(window, 640, 360);
    if (mode != DISPLAY_WINDOWED) fprintf(stderr, "[display] %s\n", mode_name(mode));
    mouse_moved_at = SDL_GetTicks64();
    note_size();
    note_refresh();
    keys = SDL_GetKeyboardState(&nkeys);
    focused = 1;
    fprintf(stderr, "[gfx] window ready (SDL %s video driver)\n", SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?");
    return 1;
}

/* Runs on the main thread for good. `status` is called every `every` seconds (0: never). */
void rt_window_loop(int every, void (*status)(void)) {
    Uint64 last = SDL_GetTicks64();
    for (;;) {
        SDL_Event e;
        int got = window ? SDL_WaitEventTimeout(&e, 8) : (SDL_Delay(50), 0);
        while (got) {
            switch (e.type) {
            case SDL_QUIT:
                fprintf(stderr, "[gfx] window closed\n");
                ExitProcess(0);
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) focused = 1;
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) focused = 0;
                if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) note_size();
                if (e.window.event == SDL_WINDOWEVENT_MOVED || e.window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED) note_refresh();
                break;
            case SDL_MOUSEMOTION:
                mouse_moved_at = SDL_GetTicks64();
                if (cursor_hidden) { SDL_ShowCursor(SDL_ENABLE); cursor_hidden = 0; }
                {   int w, h;
                    SDL_GetWindowSize(window, &w, &h);
                    menu_mouse_move(e.motion.x, e.motion.y, w, h); }
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (menu_is_open() && (e.button.button == SDL_BUTTON_LEFT || e.button.button == SDL_BUTTON_RIGHT)) {
                    int w, h;
                    SDL_GetWindowSize(window, &w, &h);
                    menu_mouse_move(e.button.x, e.button.y, w, h);
                    menu_key(e.button.button == SDL_BUTTON_LEFT ? MENU_MOUSE_LEFT : MENU_MOUSE_RIGHT);
                }
                break;
            case SDL_MOUSEWHEEL:
                if (menu_is_open()) {
                    int y = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y;
                    for (; y > 0; y--) menu_key(MENU_WHEEL_UP);
                    for (; y < 0; y++) menu_key(MENU_WHEEL_DOWN);
                }
                break;
            case SDL_TEXTINPUT:   /* (typed letters, for the settings menu) */
                if (menu_is_open())
                    for (const char *c = e.text.text; *c; c++) menu_char((unsigned char)*c);
                break;
            case SDL_KEYDOWN:
                menu_key_of(&e.key);
                if (e.key.repeat) break;
                if (e.key.keysym.scancode >= SDL_SCANCODE_F1 && e.key.keysym.scancode <= SDL_SCANCODE_F6)
                    gfx_hotkey(e.key.keysym.scancode - SDL_SCANCODE_F1);
                if (e.key.keysym.scancode == SDL_SCANCODE_F11 ||
                    (e.key.keysym.scancode == SDL_SCANCODE_RETURN && (e.key.keysym.mod & KMOD_ALT))) {
                    int full = display_window_mode() == DISPLAY_FULLSCREEN ? DISPLAY_FULLSCREEN : DISPLAY_BORDERLESS;
                    set_mode(mode == DISPLAY_WINDOWED ? full : DISPLAY_WINDOWED);
                }
                break;
            }
            got = SDL_PollEvent(&e);
        }
        Uint64 now = SDL_GetTicks64();
        int asked = display_take_mode();   /* (the settings menu) */
        if (window && asked >= 0) set_mode(asked);
        if (window) idle_pointer(now);
        if (every > 0 && status && now - last >= (Uint64)every * 1000) {
            last = now;
            status();
        }
    }
}
