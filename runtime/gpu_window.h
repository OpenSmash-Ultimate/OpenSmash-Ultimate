/* The game's window (gpu_window.c), whichever back end draws into it: on Windows a Win32 window
 * on a thread of its own, on Linux SDL's (linux/window.c, made by the main thread). macOS has its
 * own (macos/window.m, macos/gpu_metal.m). */
#pragma once

/* (the back end's start) The window, made the first time: an HWND (Linux: the SDL_Window); NULL if
 * it cannot be made. */
void *gfx_window_open(void);
/* The debug keys pressed since the last call (bit n: F1 + n), taken at a present. */
long gfx_take_hotkeys(void);
/* (Linux: linux/window.c) F1 + n was pressed. */
void gfx_hotkey(int n);
/* Whether the game's window has the keyboard. */
int gfx_window_focused(void);
/* (rollback netplay, hle_rb_core.c) what a match's set-up waits for, after the window's title. */
void gfx_set_status(const char *text);
