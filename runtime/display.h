/* Display options (display.ini): the window's mode and size, the picture's place in it, and the
 * pacing of frames (gpu_display.c). */
#pragma once
#ifndef __OBJC__   /* (macOS's window and renderer are Objective-C, whose BOOL is not the shim's: they need none of it) */
#include <windows.h>
#endif

typedef enum { PACING_CLOCK, PACING_VRR, PACING_DISPLAY } Pacing;
enum { DISPLAY_WINDOWED, DISPLAY_BORDERLESS, DISPLAY_FULLSCREEN };

void display_dpi_aware(void);           /* first thing in main: window sizes in real pixels */
#ifdef _WIN32
/* The game's window, made by the renderer's window thread with its class and procedure. */
HWND display_create_window(HINSTANCE inst, const wchar_t *cls, const wchar_t *title);
/* The window procedure offers every message here first: nonzero = handled, the answer in *result. */
int display_message(HWND h, UINT msg, WPARAM w, LPARAM l, LRESULT *result);
#else
/* Linux and macOS: linux/window.c or macos/window.m makes the window with SDL from display.ini's
 * settings and reports to it. */
int display_window_mode(void);
void display_window_size(int *width, int *height);   /* (macOS: in points) */
int display_hide_cursor(void);
void display_note_size(unsigned w, unsigned h);   /* the window's inside changed (pixels) */
void display_note_refresh(int hz);                /* the window's monitor refreshes hz times a second */
#endif
/* The renderer, at a present: the window's client size, when it changed since the last call. */
int display_take_size(unsigned *w, unsigned *h);
/* The 16:9 picture centred in a w x h back buffer. */
void display_fit(unsigned w, unsigned h, unsigned *x, unsigned *y, unsigned *fw, unsigned *fh);
int display_latency(void);              /* frames the GPU may queue ahead: 1-3 */
Pacing display_pacing(void);
const char *display_gpu(void);          /* the graphics card to use: "auto", or part of its name (SSBU_GPU) */
/* render_scale: the game's pictures are drawn this many times its 1920x1080 in each direction
 * (0.5-4; SSBU_RENDER_SCALE). 0 = auto: as many as the picture has pixels in the window at the
 * start (1440p: 1.33), at least 1. */
float display_render_scale(void);
float display_volume(void);            /* display.ini volume, 0 to 1 (100 is full) */
/* The renderer says whether the swap chain can tear (for VRR); the flags each Present gets. */
void display_set_tearing(int supported);
unsigned display_present_flags(void);
/* The vsync thread: returns when the next refresh the game sees is due. */
void display_wait_tick(void);
/* SSBU_SPEED (test runs): 1 = normal; else the vsync thread gives one refresh per frame shown,
 * n x 60 a second (display_speed_tick waits for the next one's time), 0 = uncapped. */
double display_speed(void);
void display_speed_tick(void);

/* The settings menu (gpu_menu.c), which saves display.ini itself: the settings in effect, and
 * changes that take effect now. The window's size and the graphics card count at the next start. */
typedef struct DisplaySettings { int mode, width, height, latency, hide_cursor, overlay; Pacing pacing; char gpu[64]; float render_scale; } DisplaySettings;
void display_settings(DisplaySettings *out);
const char *display_ini_path(void);
int display_overlay(void);                /* overlay = 1: the frame-rate overlay shows from the start */
float display_overlay_scale(void);        /* overlay_scale = 0.25-2: the overlay's size */
void display_set_mode(int mode);          /* DISPLAY_*: the window switches now (and F11 goes back to it) */
int display_take_mode(void);              /* (Linux, the window's loop) a mode asked for, or -1 */
void display_set_pacing(Pacing pacing);
void display_set_hide_cursor(int on);
void display_set_latency(int frames);
int display_take_latency(void);           /* the renderer, at a present: a new latency, or 0 */
void display_set_render_scale(float scale);   /* (shown by the menu; the renderer takes it at the next start) */
